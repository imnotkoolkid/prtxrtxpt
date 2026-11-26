#include "discord_rpc.hpp"
#include "discord_config.hpp"
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <string_view>
#include <windows.h>

namespace
{
std::string trim(std::string_view sv)
{
    constexpr std::string_view whitespace = " \t\r\n";
    auto first = sv.find_first_not_of(whitespace);
    if (first == sv.npos)
        return {};
    return std::string(sv.substr(first, sv.find_last_not_of(whitespace) - first + 1));
}

std::string resolveAppId(std::string explicitId)
{
    if (auto id = trim(explicitId); !id.empty())
        return id;

    if (const char *env = std::getenv("PRTX_DISCORD_APP_ID"))
        if (auto id = trim(env); !id.empty())
            return id;

    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH))
    {
        auto cfgPath = std::filesystem::path(exePath).parent_path() / L"discord_app_id.txt";
        std::ifstream f(cfgPath);
        std::string line;
        while (std::getline(f, line))
        {
            auto id = trim(line);
            if (!id.empty() && id.front() != '#')
                return id;
        }
    }

    return trim(PRTX_DISCORD_APP_ID);
}

std::string jsonEscape(std::string_view s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20)
                out += std::format("\\u{:04x}", c);
            else
                out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

bool writeAll(HANDLE pipe, const void *data, size_t size)
{
    auto bytes = static_cast<const uint8_t *>(data);
    size_t sent = 0;
    while (sent < size)
    {
        DWORD written = 0;
        if (!WriteFile(pipe, bytes + sent, static_cast<DWORD>(size - sent), &written, nullptr) || written == 0)
            return false;
        sent += written;
    }
    return true;
}

bool readAll(HANDLE pipe, void *data, size_t size)
{
    auto bytes = static_cast<uint8_t *>(data);
    size_t got = 0;
    while (got < size)
    {
        DWORD read = 0;
        if (!ReadFile(pipe, bytes + got, static_cast<DWORD>(size - got), &read, nullptr) || read == 0)
            return false;
        got += read;
    }
    return true;
}

bool sendFrame(HANDLE pipe, uint32_t opcode, std::string_view payload)
{
    uint32_t header[2]{opcode, static_cast<uint32_t>(payload.size())};
    return writeAll(pipe, header, sizeof(header)) &&
           (payload.empty() || writeAll(pipe, payload.data(), payload.size()));
}

bool recvFrame(HANDLE pipe, uint32_t &opcode, std::string &payload)
{
    uint32_t header[2]{};
    if (!readAll(pipe, header, sizeof(header)))
        return false;
    opcode = header[0];
    if (header[1] > 1024 * 1024)
        return false;
    payload.resize(header[1]);
    return header[1] == 0 || readAll(pipe, payload.data(), payload.size());
}

bool drainIncoming(HANDLE pipe)
{
    for (;;)
    {
        DWORD available;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
            return false;
        if (available < 8)
            return true;
        uint32_t op;
        std::string msg;
        if (!recvFrame(pipe, op, msg) || op == 2)
            return false;
    }
}

HANDLE connectPipe(std::string_view clientId)
{
    for (int i = 0; i < 10; ++i)
    {
        auto name = std::format(L"\\\\.\\pipe\\discord-ipc-{}", i);
        HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE)
            continue;

        auto handshake = std::format("{{\"v\":1,\"client_id\":\"{}\"}}", jsonEscape(clientId));
        uint32_t op = 0;
        std::string reply;
        if (sendFrame(pipe, 0, handshake) && recvFrame(pipe, op, reply) && op == 1 &&
            reply.find("\"ERROR\"") == std::string::npos)
            return pipe;

        CloseHandle(pipe);
    }
    return INVALID_HANDLE_VALUE;
}

std::string buildActivityPayload(const DiscordActivity &a, DWORD pid, uint64_t nonce)
{
    std::string act = "{";
    auto append = [&](std::string field) {
        if (act.size() > 1)
            act.push_back(',');
        act += field;
    };
    if (!a.details.empty())
        append(std::format("\"details\":\"{}\"", jsonEscape(a.details)));
    if (!a.state.empty())
        append(std::format("\"state\":\"{}\"", jsonEscape(a.state)));
    if (a.startTimestamp > 0)
        append(std::format("\"timestamps\":{{\"start\":{}}}", a.startTimestamp));
    if (!a.largeImage.empty())
    {
        append(std::format("\"assets\":{{\"large_image\":\"{}\"", jsonEscape(a.largeImage)));
        if (!a.largeText.empty())
            act += std::format(",\"large_text\":\"{}\"", jsonEscape(a.largeText));
        act.push_back('}');
    }
    act.push_back('}');

    return std::format("{{\"cmd\":\"SET_ACTIVITY\",\"args\":{{\"pid\":{},\"activity\":{}}},\"nonce\":\"{}\"}}",
                       pid, act, nonce);
}
} 

DiscordRpc::DiscordRpc(std::string appId) : clientId(resolveAppId(std::move(appId)))
{
    if (!clientId.empty())
        worker = std::jthread([this](std::stop_token stop) { run(stop); });
}

void DiscordRpc::setActivity(DiscordActivity activity)
{
    if (clientId.empty())
        return;
    std::scoped_lock lock(mutex);
    if (current == activity)
        return;
    current = std::move(activity);
    hasPending = true;
}

void DiscordRpc::run(std::stop_token stop)
{
    HANDLE pipe = INVALID_HANDLE_VALUE;
    DWORD pid = GetCurrentProcessId();
    uint64_t nonce = 1;
    auto nextReconnect = std::chrono::steady_clock::time_point{};

    while (!stop.stop_requested())
    {
        auto now = std::chrono::steady_clock::now();
        if (pipe == INVALID_HANDLE_VALUE && now >= nextReconnect)
        {
            pipe = connectPipe(clientId);
            if (pipe == INVALID_HANDLE_VALUE)
                nextReconnect = now + std::chrono::seconds(5);
            else
            {
                std::scoped_lock lock(mutex);
                if (!current.details.empty() || !current.state.empty())
                    hasPending = true;
            }
        }

        if (pipe != INVALID_HANDLE_VALUE)
        {
            bool alive = drainIncoming(pipe);

            DiscordActivity snapshot;
            bool sendNow = false;
            {
                std::scoped_lock lock(mutex);
                if (hasPending)
                {
                    snapshot = current;
                    hasPending = false;
                    sendNow = true;
                }
            }

            if (alive && sendNow)
            {
                auto payload = buildActivityPayload(snapshot, pid, nonce++);
                alive = sendFrame(pipe, 1, payload) && drainIncoming(pipe);
            }

            if (!alive)
            {
                CloseHandle(pipe);
                pipe = INVALID_HANDLE_VALUE;
                nextReconnect = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    if (pipe != INVALID_HANDLE_VALUE)
    {
        auto clear = std::format("{{\"cmd\":\"SET_ACTIVITY\",\"args\":{{\"pid\":{},\"activity\":null}},\"nonce\":\"{}\"}}",
                                 pid, nonce++);
        sendFrame(pipe, 1, clear);
        sendFrame(pipe, 2, "{}");
        CloseHandle(pipe);
    }
}
