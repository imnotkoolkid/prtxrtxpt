#include "settings_window.hpp"
#include <commctrl.h>
#include <commdlg.h>
#include <format>
#include <string>
#include <stdexcept>

namespace
{
constexpr int Group = 11, Preset = 12, List = 13, Slider = 14, Number = 15, Action = 16;
bool colorControl(Parameter p) { return p == SunTint || p == SkyTint || p == EmissionTint; }
std::wstring formatted(Parameter p, int v)
{
    if (colorControl(p))
        return std::format(L"#{:06X}", v);
    if (controls[p].high == 1)
        return v ? L"On" : L"Off";
    return std::to_wstring(v);
}
void move(HWND h, int x, int y, int w, int height) { MoveWindow(h, x, y, w, height, TRUE); }
} 
void SettingsWindow::create(HWND owner, Renderer &r)
{
    renderer = &r;
    auto instance = GetModuleHandle(nullptr);
    WNDCLASS wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_3DFACE);
    wc.lpszClassName = L"PrtxRenderSettings";
    RegisterClass(&wc);
    window = CreateWindowEx(WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT, wc.lpszClassName, L"Rendering settings",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME, CW_USEDEFAULT,
                            CW_USEDEFAULT, 438, 530, owner, nullptr, instance, this);
    if (!window)
        throw std::runtime_error("Could not create rendering settings.");
}
void SettingsWindow::toggle()
{
    if (IsWindowVisible(window))
        ShowWindow(window, SW_HIDE);
    else
    {
        ShowWindow(window, SW_SHOWNORMAL);
        SetForegroundWindow(window);
    }
}
LRESULT CALLBACK SettingsWindow::proc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    auto self = reinterpret_cast<SettingsWindow *>(GetWindowLongPtr(h, GWLP_USERDATA));
    if (msg == WM_NCCREATE)
    {
        self = static_cast<SettingsWindow *>(reinterpret_cast<CREATESTRUCT *>(l)->lpCreateParams);
        self->window = h;
        SetWindowLongPtr(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->message(msg, w, l) : DefWindowProc(h, msg, w, l);
}
void SettingsWindow::layout()
{
    RECT r{};
    GetClientRect(window, &r);
    int width = r.right, half = width / 2, bottom = r.bottom;
    move(group, 12, 12, half - 18, 200);
    move(preset, half + 3, 12, half - 15, 200);
    move(list, 12, 48, width - 24, std::max(50, bottom - 155));
    move(label, 12, bottom - 97, width - 24, 23);
    move(slider, 8, bottom - 68, width - 126, 35);
    move(number, width - 112, bottom - 68, 98, 26);
    move(action, 12, bottom - 67, 180, 28);
}
void SettingsWindow::populate()
{
    const int category = int(SendMessage(group, CB_GETCURSEL, 0, 0));
    SendMessage(list, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < ParameterCount; ++i)
        if (controls[i].group == category)
        {
            auto p = Parameter(i);
            auto s = std::format(L"{}\t{}", controls[i].label, formatted(p, renderer->settings[p]));
            auto row = SendMessage(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s.c_str()));
            SendMessage(list, LB_SETITEMDATA, row, i);
        }
    SendMessage(list, LB_SETCURSEL, 0, 0);
    refresh();
}
void SettingsWindow::refresh()
{
    refreshing = true;
    auto row = SendMessage(list, LB_GETCURSEL, 0, 0);
    if (row != LB_ERR)
        selected = Parameter(SendMessage(list, LB_GETITEMDATA, row, 0));
    const auto &c = controls[selected];
    int v = renderer->settings[selected];
    bool numeric = c.high != 1 && !colorControl(selected);
    ShowWindow(slider, numeric ? SW_SHOW : SW_HIDE);
    ShowWindow(number, numeric ? SW_SHOW : SW_HIDE);
    ShowWindow(action, !numeric ? SW_SHOW : SW_HIDE);
    SetWindowText(label, c.label);
    SendMessage(slider, TBM_SETRANGEMIN, FALSE, c.low);
    SendMessage(slider, TBM_SETRANGEMAX, FALSE, c.high);
    SendMessage(slider, TBM_SETPOS, TRUE, v);
    SetWindowText(number, std::to_wstring(v).c_str());
    SetWindowText(action, (colorControl(selected) ? L"Choose color..." : (v ? L"Turn off" : L"Turn on")));
    refreshing = false;
}
void SettingsWindow::editValue(int v)
{
    renderer->settings.set(selected, v);
    auto row = SendMessage(list, LB_GETCURSEL, 0, 0);
    auto top = SendMessage(list, LB_GETTOPINDEX, 0, 0);
    auto s =
        std::format(L"{}\t{}", controls[selected].label, formatted(selected, renderer->settings[selected]));
    SendMessage(list, LB_DELETESTRING, row, 0);
    SendMessage(list, LB_INSERTSTRING, row, reinterpret_cast<LPARAM>(s.c_str()));
    SendMessage(list, LB_SETITEMDATA, row, selected);
    SendMessage(list, LB_SETCURSEL, row, 0);
    SendMessage(list, LB_SETTOPINDEX, top, 0);
    SendMessage(preset, CB_SETCURSEL, 0, 0);
    refresh();
}
LRESULT SettingsWindow::message(UINT msg, WPARAM w, LPARAM l)
{
    if (msg == WM_CREATE)
    {
        auto control = [&](const wchar_t *cls, const wchar_t *title, DWORD style, int id)
        {
            HWND h = CreateWindowEx(0, cls, title, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, window,
                                    reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandle(nullptr), nullptr);
            SendMessage(h, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
            return h;
        };
        group = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, Group);
        for (auto s : {L"Tracing and sampling", L"Lighting"})
            SendMessage(group, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
        SendMessage(group, CB_SETCURSEL, 0, 0);
        preset = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, Preset);
        for (auto s : {L"Quality preset", L"Low", L"Medium", L"High", L"Ultra"})
            SendMessage(preset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
        SendMessage(preset, CB_SETCURSEL, 0, 0);
        list = control(
            L"LISTBOX", L"",
            LBS_NOTIFY | LBS_USETABSTOPS | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_BORDER | WS_TABSTOP, List);
        int tab = 142;
        SendMessage(list, LB_SETTABSTOPS, 1, reinterpret_cast<LPARAM>(&tab));
        label = control(L"STATIC", L"", 0, 0);
        slider = control(TRACKBAR_CLASS, L"", TBS_HORZ | WS_TABSTOP, Slider);
        number = control(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, Number);
        action = control(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, Action);
        populate();
        layout();
        return 0;
    }
    if (msg == WM_SIZE && list)
    {
        layout();
        return 0;
    }
    if (msg == WM_GETMINMAXINFO)
    {
        reinterpret_cast<MINMAXINFO *>(l)->ptMinTrackSize = {330, 390};
        return 0;
    }
    if (msg == WM_CLOSE || (msg == WM_COMMAND && LOWORD(w) == IDCANCEL))
    {
        ShowWindow(window, SW_HIDE);
        SetFocus(GetWindow(window, GW_OWNER));
        return 0;
    }
    if (msg == WM_HSCROLL && reinterpret_cast<HWND>(l) == slider)
    {
        editValue(int(SendMessage(slider, TBM_GETPOS, 0, 0)));
        return 0;
    }
    if (msg == WM_COMMAND && !refreshing)
    {
        int id = LOWORD(w), event = HIWORD(w);
        if (id == IDOK && GetFocus() == number)
            SetFocus(list);
        if (id == Group && event == CBN_SELCHANGE)
            populate();
        if (id == Preset && event == CBN_SELCHANGE)
        {
            int i = int(SendMessage(preset, CB_GETCURSEL, 0, 0));
            if (i)
            {
                renderer->settings.preset(i - 1);
                populate();
            }
        }
        if (id == List && event == LBN_SELCHANGE)
            refresh();
        if (id == Number && event == EN_KILLFOCUS && IsWindowVisible(number))
        {
            BOOL ok;
            int v = int(GetDlgItemInt(window, Number, &ok, TRUE));
            if (ok)
                editValue(v);
            else
                refresh();
        }
        if (id == Action && event == BN_CLICKED)
        {
            int v = renderer->settings[selected];
            if (colorControl(selected))
            {
                static COLORREF colors[16]{};
                CHOOSECOLOR cc{sizeof(cc)};
                cc.hwndOwner = window;
                cc.lpCustColors = colors;
                cc.rgbResult = RGB(v >> 16, v >> 8, v);
                cc.Flags = CC_RGBINIT | CC_FULLOPEN;
                if (ChooseColor(&cc))
                    editValue(GetRValue(cc.rgbResult) << 16 | GetGValue(cc.rgbResult) << 8 |
                              GetBValue(cc.rgbResult));
            }
            else
                editValue(!v);
        }
        return 0;
    }
    return DefWindowProc(window, msg, w, l);
}
