#include "renderer/renderer.hpp"
#include "discord_rpc.hpp"
#include "settings_window.hpp"
#include <numeric>
#include <deque>
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
constexpr auto IDLE_TITLE = L"PRTXRXTPT";

struct App
{
    HWND window{}, menu{};
    POINT savedCursor{};
    WINDOWPLACEMENT savedPlacement{sizeof(WINDOWPLACEMENT)};
    bool fullscreen = true, focused = false;
    bool dirty = true, showFps = false;
    HWND overlay{};
    SettingsWindow settings;
    std::deque<double> intervals;
    Clock::time_point lastFrame{}, sample{};
    void positionOverlay() {
        POINT origin{12,12};ClientToScreen(window,&origin);
        SetWindowPos(overlay,HWND_TOP,origin.x,origin.y,305,100,SWP_NOACTIVATE);
    }
    void toggleDiagnostics() {
        showFps=!showFps;intervals.clear();lastFrame={};sample={};
        syncExclusive();positionOverlay();
        SetWindowTextW(overlay,L"  Measuring frame times...");
        ShowWindow(overlay,showFps?SW_SHOWNOACTIVATE:SW_HIDE);
        dirty=true;
    }
    void diagnostics(Clock::time_point end, double cpuMs) {
        if(!showFps) return;
        if(lastFrame.time_since_epoch().count()) {
            intervals.push_back(std::chrono::duration<double,std::milli>(end-lastFrame).count());
            if(intervals.size()>240) intervals.pop_front();
        }
        lastFrame=end;
        if(intervals.empty() || end-sample<std::chrono::milliseconds(250)) return;
        sample=end;
        std::vector<double> sorted(intervals.begin(),intervals.end());
        size_t worst=(sorted.size()+99)/100;
        std::partial_sort(sorted.begin(),sorted.begin()+worst,sorted.end(),std::greater<>());
        double avg=std::accumulate(sorted.begin(),sorted.end(),0.)/sorted.size();
        double low=std::accumulate(sorted.begin(),sorted.begin()+worst,0.)/worst;
        double gpu=renderer->gpuMilliseconds();
        auto label=std::format(L"  {:.0f} FPS     Avg {:.0f}     1% low {:.0f}\r\n  Total frame   {:.2f} ms\r\n  CPU frame    {:.2f} ms\r\n  GPU frame    {}",
            1000./intervals.back(),1000./avg,1000./low,intervals.back(),cpuMs,
            gpu>=0?std::format(L"{:.2f} ms",gpu):std::wstring(L"unavailable"));
        SetWindowTextW(overlay,label.c_str());
    }
    void toggleSettings() {
        capture(false);
        if(!IsWindowVisible(settings.window)) renderer->setFullScreen(fullscreen,false);
        settings.toggle();syncExclusive();dirty=true;
    }

    std::unique_ptr<Renderer> renderer;
    DiscordRpc discord;
    std::string mapName;
    int64_t sessionStart = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
    Clock::time_point lastPresenceCheck{};
    Camera camera;
    bool captured{}, menuRequested{}, quit{};
    bool keys[256]{};
    std::wstring draft;
    std::future<Map> pending;

    void syncPresence()
    {
        if (!discord.enabled() || !renderer)
            return;
        DiscordActivity act;
        act.details = std::format("Map: {}", mapName.empty() ? "Custom Map" : mapName);
        if (renderer->settings[Enabled])
            act.state = std::format("Path Tracing | {}% | {} SPP", renderer->settings[Scale], renderer->settings[Spp]);
        else
            act.state = "Rasterizer";
        act.largeImage = "icon";
        act.largeText = renderer->adapterName().empty()
                            ? "PRTX Map Viewer"
                            : std::format("PRTX Map Viewer ({})", renderer->adapterName());
        act.startTimestamp = sessionStart;
        discord.setActivity(std::move(act));
    }

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
        bool active = !IsIconic(window) && !IsWindowVisible(menu) && !IsWindowVisible(settings.window) && !showFps && focused;
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

    void activate(Map m)
    {
        mapName = m.name.empty() ? "Custom Map" : m.name;
        renderer->upload(m);
        camera = m.camera;
        dirty = true;
        syncPresence();
    }

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
    case WM_MOVE:
        if (app->overlay && app->showFps) app->positionOverlay();
        break;
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
    case WM_SETFOCUS:
        app->syncExclusive();
        break;
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
        if (w == VK_TAB) { app->toggleDiagnostics(); return 0; }
        if (w == VK_F2) { app->toggleSettings(); return 0; }
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
                return loadMap(source, false);
            });
            EnableWindow(GetDlgItem(h, IDOK), FALSE);
            SetTimer(h, 1, 30, nullptr);
        }
        catch (std::exception const &e) { SetDlgItemTextA(h, 202, e.what()); }
        return TRUE;
    case 204:
        app->closeMenu();app->toggleSettings();return TRUE;
    case 203:
        app->quit = true;
        return TRUE;
    }
    return FALSE;
}
} 

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR args, int show)
{
    try
    {
        winrt::init_apartment();
        SetProcessDPIAware();
        App app;
        app.fullscreen = std::wstring_view(args) != L"--windowed";

        WNDCLASSEXW wc{sizeof(WNDCLASSEXW)};
        wc.hbrBackground = GetSysColorBrush(COLOR_3DFACE);
        wc.lpfnWndProc   = windowProc;
        wc.hInstance     = instance;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon         = static_cast<HICON>(LoadImageW(
            instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
        wc.hIconSm       = static_cast<HICON>(LoadImageW(
            instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
        wc.lpszClassName = L"PrtxVoxelMap";
        RegisterClassExW(&wc);

        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromPoint({}, MONITOR_DEFAULTTOPRIMARY), &mi);
        int mw = mi.rcMonitor.right  - mi.rcMonitor.left;
        int mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
        int ww = std::min(1280, mw), wh = std::min(760, mh);
        int wx = mi.rcMonitor.left + (mw - ww) / 2, wy = mi.rcMonitor.top + (mh - wh) / 2;
        app.savedPlacement.rcNormalPosition = {wx, wy, wx + ww, wy + wh};
        app.savedPlacement.showCmd = SW_SHOWNORMAL;

        app.window = CreateWindowW(wc.lpszClassName, IDLE_TITLE,
                                   (app.fullscreen ? WS_POPUP : WS_OVERLAPPEDWINDOW) | WS_CLIPCHILDREN,
                                   app.fullscreen ? mi.rcMonitor.left : wx, app.fullscreen ? mi.rcMonitor.top : wy,
                                   app.fullscreen ? mw : ww, app.fullscreen ? mh : wh,
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

        INITCOMMONCONTROLSEX common{sizeof(common),ICC_BAR_CLASSES};
        InitCommonControlsEx(&common);
        app.settings.create(app.window,*app.renderer);
        app.overlay=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TRANSPARENT,L"STATIC",L"",
            WS_POPUP|WS_BORDER|SS_LEFT|SS_NOPREFIX,0,0,305,100,app.window,nullptr,instance,nullptr);
        SendMessage(app.overlay,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),FALSE);

        auto resource = FindResourceW(instance, MAKEINTRESOURCEW(102), MAKEINTRESOURCEW(10));
        if (resource)
        {
            auto data = static_cast<const char *>(LockResource(LoadResource(instance, resource)));
            app.activate(loadMap(winrt::to_hstring(std::string_view(data, SizeofResource(instance, resource))), false));
        }
        else
            app.activate(loadMap(LR"({"chnks":[]})", false));

        ShowWindow(app.window, show == SW_SHOWMINIMIZED ? SW_SHOWMINIMIZED : SW_SHOWNORMAL);
        app.focused = (GetForegroundWindow() == app.window);
        app.syncExclusive();
        auto last = Clock::now(), nextFrame = last;

        while (!app.quit)
        {
            auto loopStart=Clock::now();
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                if(IsWindowVisible(app.settings.window)) {
                    if(msg.message==WM_KEYDOWN && (msg.wParam==VK_F2 || msg.wParam==VK_ESCAPE)) {
                        app.toggleSettings();continue;
                    }
                    if(IsDialogMessage(app.settings.window,&msg)) continue;
                }
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
            if (now - app.lastPresenceCheck >= std::chrono::seconds(1))
            {
                app.lastPresenceCheck = now;
                app.syncPresence();
            }
            bool moving = app.captured && (app.keys['W'] != app.keys['S'] ||
                                           app.keys['A'] != app.keys['D'] ||
                                           app.keys['E'] != app.keys['Q']);
            bool active = !IsIconic(app.window) && !IsWindowVisible(app.menu);
            bool render = active && (app.dirty || moving || app.renderer->settings[Enabled] ||
                                     app.showFps || IsWindowVisible(app.settings.window));
            if(!active) app.lastFrame={};

            if (render && now >= nextFrame)
            {
                if (moving)
                    app.move(std::clamp(std::chrono::duration<float>(now - last).count(), 0.f, .05f));
                double updateMs=std::chrono::duration<double,std::milli>(Clock::now()-loopStart).count();
                bool submitted=app.renderer->draw(app.camera, app.showFps);
                app.dirty = !submitted;
                if(submitted) app.diagnostics(Clock::now(),updateMs+app.renderer->cpuMilliseconds());
                nextFrame = now + std::chrono::microseconds(submitted?16667:2000);
                last      = now;
            }
            if (!moving) last = now;

            DWORD timeout = INFINITE;
            if (render)
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
