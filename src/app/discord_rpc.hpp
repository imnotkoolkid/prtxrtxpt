#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

struct DiscordActivity
{
    std::string details;
    std::string state;
    std::string largeImage = "icon";
    std::string largeText = "PRTXRTXPT";
    int64_t startTimestamp = 0;

    bool operator==(const DiscordActivity &) const = default;
};

class DiscordRpc
{
  public:
    explicit DiscordRpc(std::string appId = {});

    DiscordRpc(const DiscordRpc &) = delete;
    DiscordRpc &operator=(const DiscordRpc &) = delete;

    void setActivity(DiscordActivity activity);
    bool enabled() const { return !clientId.empty(); }

  private:
    std::string clientId;
    std::mutex mutex;
    DiscordActivity current;
    bool hasPending = false;
    std::jthread worker; 

    void run(std::stop_token stop);
};
