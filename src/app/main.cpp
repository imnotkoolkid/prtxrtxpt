#include "renderer/renderer.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <commctrl.h>
#include <format>
#include <future>
#include <string>
#include <windowsx.h>
#include <winrt/base.h>

namespace
{
using Clock = std::chrono::steady_clock;
constexpr auto IDLE_TITLE = L"PRTX Map Viewer  |  Click to fly  ·  Esc for map code  ·  F11 windowed";

struct App
{
    HWND window{}, menu{};
    POINT savedCursor{};
    WINDOWPLACEMENT savedPlacement{sizeof(WINDOWPLACEMENT)};
    bool fullscreen = true, focused = false;
    bool dirty = true, showFps = false;
    unsigned frames = 0;
    Clock::time_point sample = Clock::now();

    std::unique_ptr<Renderer> renderer;
    Camera camera;
    bool captured{}, menuRequested{}, quit{};
    bool keys[256]{};
    std::wstring draft;
    std::future<Map> pending;

    void layout()
    {
        if (!menu) return;
        RECT client{}, panel{};
        GetClientRect(window, &client);
        GetWindowRect(menu, &panel);
        SetWindowPos(menu, HWND_TOP,
                     std::max(0L, (client.right  - panel.right  + panel.left) / 2),
                     std::max(0L, (client.bottom - panel.bottom + panel.top)  / 2),
                     0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    }

    void syncExclusive()
    {
        if (!renderer) return;
        bool active = !IsIconic(window) && !IsWindowVisible(menu) && focused;
        renderer->setFullScreen(fullscreen, active);
    }

    void toggleFullscreen()
    {
        capture(false);
        fullscreen = !fullscreen;
        if (fullscreen)
        {
            GetWindowPlacement(window, &savedPlacement);
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &mi);
            SetWindowLongPtrW(window, GWL_STYLE, WS_POPUP | WS_CLIPCHILDREN | WS_VISIBLE);
            SetWindowPos(window, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
        else
        {
            renderer->setFullScreen(false, false);
            SetWindowLongPtrW(window, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_VISIBLE);
            SetWindowPlacement(window, &savedPlacement);
            SetWindowPos(window, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
        syncExclusive();
        layout();
        dirty = true;
    }

    void closeMenu()
    {
        ShowWindow(menu, SW_HIDE);
        SetFocus(window);
        syncExclusive();
        dirty = true;
    }

    void activate(Map m) { renderer->upload(m); camera = m.camera; dirty = true; }

    void capture(bool enable)
    {
        if (enable == captured) return;
        captured = enable;
        std::fill_n(keys, 256, false);
        if (enable)
        {
            RECT r;
            GetClientRect(window, &r);
            MapWindowPoints(window, nullptr, reinterpret_cast<POINT *>(&r), 2);
            GetCursorPos(&savedCursor);
            LONG x = (r.left + r.right) / 2, y = (r.top + r.bottom) / 2;
            RECT anchor{x, y, x + 1, y + 1};
            if (!ClipCursor(&anchor)) { captured = false; return; }
            SetCursor(nullptr);
        }
        else
        {
            ClipCursor(nullptr);
            SetCursorPos(savedCursor.x, savedCursor.y);
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
        }
    }

    void move(float dt)
    {
        float speed = dt * (keys[VK_SPACE] ? 144.f : 18.f);
        int f = int(keys['W']) - keys['S'], r = int(keys['D']) - keys['A'];
        camera.pos.x += speed * (-std::sin(camera.yaw) * f + std::cos(camera.yaw) * r);
        camera.pos.z += speed * (-std::cos(camera.yaw) * f - std::sin(camera.yaw) * r);
        camera.pos.y += speed * (int(keys['E']) - keys['Q']);
    }
};

LRESULT CALLBACK windowProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (msg == WM_NCCREATE)
        SetWindowLongPtr(h, GWLP_USERDATA,
                         reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCT *>(l)->lpCreateParams));
    auto app = reinterpret_cast<App *>(GetWindowLongPtr(h, GWLP_USERDATA));
    if (!app) return DefWindowProc(h, msg, w, l);

    switch (msg)
    {
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(h, &paint);
        EndPaint(h, &paint);
        return 0;
    }
    case WM_SIZE:
        app->capture(false);
        app->layout();
        app->dirty = true;
        return 0;
    case WM_ACTIVATEAPP:
        app->focused = (w != 0);
        if (!w) app->capture(false);
        app->syncExclusive();
        if (w) app->dirty = true;
        break;
    case WM_CLOSE:
        app->quit = true;
        return 0;
    case WM_KILLFOCUS:
    case WM_ENTERSIZEMOVE:
        app->capture(false);
        return 0;
    case WM_LBUTTONDOWN:
        if (!IsWindowVisible(app->menu)) { SetFocus(h); app->capture(true); }
        return 0;
    case WM_SETCURSOR:
        if (app->captured) { SetCursor(nullptr); return TRUE; }
        break;
    case WM_SYSKEYDOWN:
        if (w == VK_RETURN && (l & (1L << 29)) && !(l & (1L << 30)))
            { app->toggleFullscreen(); return 0; }
        break;
    case WM_KEYDOWN:
        if (l & (1L << 30)) return 0;
        if (w == VK_F11)    { app->toggleFullscreen(); return 0; }
        if (w == VK_TAB)
        {
            app->showFps = !app->showFps;
            app->frames  = 0;
            app->sample  = Clock::now();
            SetWindowTextW(h, app->showFps ? L"PRTX Map Viewer | FPS 0" : IDLE_TITLE);
            return 0;
        }
        if (w == VK_ESCAPE) { app->capture(false); app->menuRequested = true; return 0; }
        if (app->captured && w < 256) app->keys[w] = true;
        return 0;
    case WM_KEYUP:
        if (w < 256) app->keys[w] = false;
        return 0;
    case WM_INPUT:
        if (app->captured)
        {
            RAWINPUT input{};
            UINT size = sizeof(input);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(l), RID_INPUT, &input, &size,
                                sizeof(RAWINPUTHEADER)) == size &&
                input.header.dwType == RIM_TYPEMOUSE &&
                !(input.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
            {
                constexpr float sensitivity = .077f * 3.14159265f / 180;
                app->dirty |= input.data.mouse.lLastX != 0 || input.data.mouse.lLastY != 0;
                app->camera.yaw  -= input.data.mouse.lLastX * sensitivity;
                app->camera.pitch = std::clamp(app->camera.pitch - input.data.mouse.lLastY * sensitivity,
                                               -1.57f, 1.57f);
            }
        }
        break;
    }
    return DefWindowProc(h, msg, w, l);
}

std::wstring editText(HWND h)
{
    auto edit = GetDlgItem(h, 201);
    std::wstring s(GetWindowTextLengthW(edit) + 1, L'\0');
    s.resize(GetWindowTextW(edit, s.data(), int(s.size())));
    return s;
}

LRESULT CALLBACK editProc(HWND edit, UINT msg, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR)
{
    if (msg == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000))
    {
        if (w == 'A')       { SendMessage(edit, EM_SETSEL, 0, -1); return 0; }
        if (w == VK_RETURN) { PostMessage(GetParent(edit), WM_COMMAND, IDOK, 0); return 0; }
    }
    if (msg == WM_CHAR && (w == 1 || w == 10)) return 0;
    return DefSubclassProc(edit, msg, w, l);
}

INT_PTR CALLBACK menuProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    auto app = reinterpret_cast<App *>(GetWindowLongPtr(h, DWLP_USER));
    if (msg == WM_INITDIALOG)
    {
        app = reinterpret_cast<App *>(l);
        SetWindowLongPtr(h, DWLP_USER, l);
        SetWindowSubclass(GetDlgItem(h, 201), editProc, 1, 0);
        SendDlgItemMessage(h, 201, EM_SETLIMITTEXT, 0x7ffffffe, 0);
        SetDlgItemText(h, 201, app->draft.c_str());
        return TRUE;
    }
    if (!app) return FALSE;

    if (msg == WM_TIMER && app->pending.valid() &&
        app->pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        KillTimer(h, 1);
        try
        {
            app->activate(app->pending.get());
            EnableWindow(GetDlgItem(h, IDOK), TRUE);
            SetDlgItemTextW(h, 202, L"Map loaded.");
            app->closeMenu();
        }
        catch (std::exception const &e)
        {
            SetDlgItemTextA(h, 202, e.what());
            EnableWindow(GetDlgItem(h, IDOK), TRUE);
        }
        return TRUE;
    }
    if (msg == WM_CLOSE || (msg == WM_COMMAND && LOWORD(w) == IDCANCEL))
    {
        if (!app->pending.valid()) { app->draft = editText(h); app->closeMenu(); }
        return TRUE;
    }
    if (msg != WM_COMMAND) return FALSE;

    switch (LOWORD(w))
    {
    case IDOK:
        if (app->pending.valid()) return TRUE;
        try
        {
            app->draft = editText(h);
            SetDlgItemText(h, 202, L"Reading map and building chunk meshes...");
            app->pending = std::async(std::launch::async, [source = app->draft] {
                winrt::init_apartment();
                return loadMap(source);
            });
            EnableWindow(GetDlgItem(h, IDOK), FALSE);
            SetTimer(h, 1, 30, nullptr);
        }
        catch (std::exception const &e) { SetDlgItemTextA(h, 202, e.what()); }
        return TRUE;
    case 203:
        app->quit = true;
        return TRUE;
    }
    return FALSE;
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    try
    {
        winrt::init_apartment();
        SetProcessDPIAware();
        App app;

        WNDCLASSW wc{};
        wc.hbrBackground = GetSysColorBrush(COLOR_3DFACE);
        wc.lpfnWndProc   = windowProc;
        wc.hInstance     = instance;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"PrtxVoxelMap";
        RegisterClassW(&wc);

        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromPoint({}, MONITOR_DEFAULTTOPRIMARY), &mi);
        int mw = mi.rcMonitor.right  - mi.rcMonitor.left;
        int mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
        int ww = std::min(1280, mw), wh = std::min(760, mh);
        int wx = mi.rcMonitor.left + (mw - ww) / 2, wy = mi.rcMonitor.top + (mh - wh) / 2;
        app.savedPlacement.rcNormalPosition = {wx, wy, wx + ww, wy + wh};
        app.savedPlacement.showCmd = SW_SHOWNORMAL;

        app.window = CreateWindowW(wc.lpszClassName, IDLE_TITLE,
                                   WS_POPUP | WS_CLIPCHILDREN,
                                   mi.rcMonitor.left, mi.rcMonitor.top, mw, mh,
                                   nullptr, nullptr, instance, &app);
        if (!app.window) throw std::runtime_error("Could not open the viewer window.");

        RAWINPUTDEVICE mouse{1, 2, 0, app.window};
        if (!RegisterRawInputDevices(&mouse, 1, sizeof(mouse)))
            throw std::runtime_error("Could not register mouse input.");

        app.menu = CreateDialogParamW(instance, MAKEINTRESOURCEW(200), app.window,
                                      menuProc, reinterpret_cast<LPARAM>(&app));
        if (!app.menu) throw std::runtime_error("Could not create viewer controls.");

        app.layout();
        app.renderer = std::make_unique<Renderer>(app.window);

        auto resource = FindResourceW(instance, MAKEINTRESOURCEW(102), MAKEINTRESOURCEW(10));
        if (!resource) throw std::runtime_error("Embedded default map is missing.");
        auto data = static_cast<const char *>(LockResource(LoadResource(instance, resource)));
        app.activate(loadMap(winrt::to_hstring(std::string_view(data, SizeofResource(instance, resource)))));

        ShowWindow(app.window, show == SW_SHOWMINIMIZED ? SW_SHOWMINIMIZED : SW_SHOWNORMAL);
        app.focused = (GetForegroundWindow() == app.window);
        app.syncExclusive();
        auto last = Clock::now(), nextFrame = last;

        while (!app.quit)
        {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                if (IsWindowVisible(app.menu))
                {
                    if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE)
                        { SendMessage(app.menu, WM_COMMAND, IDCANCEL, 0); continue; }
                    if (IsDialogMessageW(app.menu, &msg)) continue;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (app.quit) break;

            if (app.menuRequested)
            {
                app.menuRequested = false;
                app.renderer->setFullScreen(app.fullscreen, false);
                RedrawWindow(app.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE);
                ShowWindow(app.menu, SW_SHOW);
                SetFocus(GetDlgItem(app.menu, 201));
            }

            auto now    = Clock::now();
            bool moving = app.captured && (app.keys['W'] != app.keys['S'] ||
                                           app.keys['A'] != app.keys['D'] ||
                                           app.keys['E'] != app.keys['Q']);
            bool active = !IsIconic(app.window) && !IsWindowVisible(app.menu);
            bool render = active && (app.dirty || moving);

            if (render && now >= nextFrame)
            {
                if (moving)
                    app.move(std::clamp(std::chrono::duration<float>(now - last).count(), 0.f, .05f));
                app.renderer->draw(app.camera, app.showFps);
                app.dirty = false;
                ++app.frames;
                nextFrame = now + std::chrono::microseconds(16667);
                last      = now;
            }
            if (!moving) last = now;

            if (app.showFps && now - app.sample >= std::chrono::seconds(1))
            {
                auto fps   = std::lround(app.frames / std::chrono::duration<double>(now - app.sample).count());
                auto label = std::format(L"PRTX Map Viewer | FPS {} | {}{}",
                                         fps, std::wstring_view(winrt::to_hstring(app.renderer->adapterName())),
                                         app.renderer->isFullScreenExclusive() ? L" | FSE" : L"");
                if (app.frames && app.renderer->gpuMilliseconds() >= 0)
                    label += std::format(L" | GPU {:.2f} ms", app.renderer->gpuMilliseconds());
                SetWindowTextW(app.window, label.c_str());
                app.frames = 0;
                app.sample = now;
            }

            DWORD timeout = app.showFps ? 1000 : INFINITE;
            if (active && (app.dirty || moving))
                timeout = DWORD(std::max<int64_t>(
                    0, std::chrono::ceil<std::chrono::milliseconds>(nextFrame - Clock::now()).count()));
            MsgWaitForMultipleObjectsEx(0, nullptr, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }

        app.capture(false);
        app.renderer.reset();
        DestroyWindow(app.window);
        return 0;
    }
    catch (std::exception const &e)
    {
        ClipCursor(nullptr);
        MessageBoxA(nullptr, e.what(), "Map viewer", MB_ICONERROR);
        return 1;
    }
}
