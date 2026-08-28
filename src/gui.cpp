#include "common.h"
namespace MultiFFBJoy
{
    HWND g_mainWindow = nullptr;
    HWND g_statusWindow = nullptr;
    HWND g_logWindow = nullptr;
    std::wstring Utf8ToWide(const char* text)
    {
        if (!text)
            return {};
        const int required = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
        if (required <= 0)
            return {};
        std::wstring result(static_cast<size_t>(required), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text, -1, result.data(), required);
        result.resize(static_cast<size_t>(required - 1));
        return result;
    }
    void Log(const std::string& text)
    {
        auto* message = new std::wstring(Utf8ToWide(text.c_str()));
        HWND window = g_mainWindow;
        if (!window || !PostMessageW(window, WM_APP_LOG, 0,
            reinterpret_cast<LPARAM>(message)))
            delete message;
    }
    static void RefreshStatusWindow()
    {
        if (!g_statusWindow)
            return;
        DeviceState state;
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            state = g_state;
        }
        wchar_t text[4096]{};
        swprintf_s(text, std::size(text),
            L"Selected device: %ls\r\n"
            L"Axes: %lu\r\n"
            L"Force Feedback: %s\r\n"
            L"Spring effect: %s\r\n"
            L"Acquired: %s\r\n"
            L"Spring strength: %.3f\r\n"
            L"UDP: 127.0.0.1:%d\r\n"
            L"FFB safety timeout: %lu ms",
            state.name.c_str(), state.axisCount,
            state.forceFeedback ? L"Yes" : L"No",
            state.springSupported ? L"Yes" : L"No",
            state.acquired ? L"Yes" : L"No",
            state.springStrength, UDP_PORT, COMMAND_TIMEOUT_MS);
        SetWindowTextW(g_statusWindow, text);
    }
    void UpdateStatus()
    {
        HWND window = g_mainWindow;
        if (window)
            PostMessageW(window, WM_APP_STATUS, 0, 0);
    }
    static HMENU ControlId(int id)
    {
        return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
    }
    void PopulatePresetList()
    {
        if (!g_mainWindow)
            return;
        HWND list = GetDlgItem(g_mainWindow, IDC_PRESET_LIST);
        if (!list)
            return;
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        const auto paths = EnumerateForceFieldPresets();
        g_availablePresets.clear();
        g_availablePresets.reserve(paths.size());
        for (const auto& path : paths)
        {
            PresetInfo info{path, path.filename().wstring()};
            g_availablePresets.push_back(info);
            SendMessageW(list, LB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(info.displayName.c_str()));
        }
        Logf("Found %zu forcefield preset(s).", g_availablePresets.size());
    }
    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message,
        WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_CREATE:
            {
                g_statusWindow = CreateWindowExW(
                    WS_EX_CLIENTEDGE, L"STATIC", L"Initializing...",
                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                    12, 12, 390, 120, window, nullptr, GetModuleHandleW(nullptr), nullptr);
                g_logWindow = CreateWindowExW(
                    WS_EX_CLIENTEDGE, L"EDIT", L"",
                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
                    ES_AUTOVSCROLL | ES_READONLY,
                    12, 240, 720, 300, window, nullptr,
                    GetModuleHandleW(nullptr), nullptr);
                struct ButtonSpec { int id; const wchar_t* text; int x; int y; };
                const ButtonSpec buttons[] = {
                    {IDC_FFB_UP, L"FFB Up", 420, 12},
                    {IDC_FFB_DOWN, L"FFB Down", 530, 12},
                    {IDC_FFB_LEFT, L"FFB Left", 420, 52},
                    {IDC_FFB_RIGHT, L"FFB Right", 530, 52},
                    {IDC_FFB_STOP, L"Stop", 420, 92},
                    {IDC_FFB_CENTER, L"Center", 530, 92},
                    {IDC_PRND_TEST, L"PRND Test", 500, 135},
                    {IDC_PRND_PARK, L"PRND Park", 610, 135},
                    {IDC_PRND_REVERSE, L"PRND Reverse", 500, 170},
                    {IDC_PRND_NEUTRAL, L"PRND Neutral", 610, 170},
                    {IDC_PRND_DRIVE, L"PRND Drive", 500, 205},
                };
                for (const auto& button : buttons)
                {
                    CreateWindowW(L"BUTTON", button.text, WS_CHILD | WS_VISIBLE,
                        button.x, button.y, 100, 28, window,
                        ControlId(button.id), GetModuleHandleW(nullptr), nullptr);
                }
                CreateWindowExW(
                    0, L"LISTBOX", nullptr,
                    WS_CHILD | WS_VISIBLE | WS_BORDER | LBS_NOTIFY | WS_VSCROLL,
                    10, 135, 390, 90, window, ControlId(IDC_PRESET_LIST),
                    GetModuleHandleW(nullptr), nullptr);
                CreateWindowW(L"BUTTON", L"Load", WS_CHILD | WS_VISIBLE,
                    410, 135, 80, 28, window, ControlId(IDC_PRESET_LOAD),
                    GetModuleHandleW(nullptr), nullptr);
                RefreshStatusWindow();
                return 0;
            }
        case WM_SIZE:
            {
                const int width = LOWORD(lParam);
                const int height = HIWORD(lParam);
                if (g_statusWindow)
                    MoveWindow(g_statusWindow, 12, 12, std::max(100, width - 380), 110, TRUE);
                if (g_logWindow)
                    MoveWindow(g_logWindow, 12, 240, std::max(100, width - 24),
                        std::max(100, height - 270), TRUE);
                return 0;
            }
        case WM_COMMAND:
            {
                if (HIWORD(wParam) != BN_CLICKED)
                    break;
                switch (LOWORD(wParam))
                {
                case IDC_FFB_UP: SendUdpCommand("TEST_FFB 0 10000"); return 0;
                case IDC_FFB_DOWN: SendUdpCommand("TEST_FFB 0 -10000"); return 0;
                case IDC_FFB_LEFT: SendUdpCommand("TEST_FFB 10000 0"); return 0;
                case IDC_FFB_RIGHT: SendUdpCommand("TEST_FFB -10000 0"); return 0;
                case IDC_FFB_STOP: ClearForceFieldPreset(); return 0;
                case IDC_FFB_CENTER: SendUdpCommand("CENTER"); return 0;
                case IDC_PRND_TEST: StartHardCodedPRNDTest(); return 0;
                case IDC_PRND_PARK: ApplyHardCodedPRNDZone(0); return 0;
                case IDC_PRND_REVERSE: ApplyHardCodedPRNDZone(1); return 0;
                case IDC_PRND_NEUTRAL: ApplyHardCodedPRNDZone(2); return 0;
                case IDC_PRND_DRIVE: ApplyHardCodedPRNDZone(3); return 0;
                case IDC_PRESET_LOAD:
                    {
                        HWND list = GetDlgItem(window, IDC_PRESET_LIST);
                        if (!list) return 0;
                        const LRESULT selected = SendMessageW(list, LB_GETCURSEL, 0, 0);
                        if (selected == LB_ERR || static_cast<size_t>(selected) >= g_availablePresets.size())
                            return 0;
                        const auto preset = g_availablePresets[static_cast<size_t>(selected)];
                        if (LoadForceFieldPreset(preset.path))
                            StartPresetTest();
                        return 0;
                    }
                default: break;
                }
                break;
            }
        case WM_APP_LOG:
            {
                auto* messageText = reinterpret_cast<std::wstring*>(lParam);
                if (!messageText)
                    return 0;
                if (g_logWindow)
                {
                    const int length = GetWindowTextLengthW(g_logWindow);
                    SendMessageW(g_logWindow, EM_SETSEL, length, length);
                    const std::wstring line = *messageText + L"\r\n";
                    SendMessageW(g_logWindow, EM_REPLACESEL, FALSE,
                        reinterpret_cast<LPARAM>(line.c_str()));
                    SendMessageW(g_logWindow, EM_SCROLL, SB_BOTTOM, 0);
                }
                delete messageText;
                return 0;
            }
        case WM_APP_STATUS:
            RefreshStatusWindow();
            return 0;
        case WM_DESTROY:
            g_mainWindow = nullptr;
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }
    bool CreateMainWindow(HINSTANCE instance, int showCommand)
    {
        WNDCLASSW wc{};
        wc.hInstance = instance;
        wc.lpfnWndProc = WindowProcedure;
        wc.lpszClassName = L"MultiFFBJoyWindow";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            Logf("RegisterClassW failed: %lu", GetLastError());
            return false;
        }
        g_mainWindow = CreateWindowW(
            wc.lpszClassName, L"MultiFFBJoy - DirectInput FFB Bridge",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 760, 600,
            nullptr, nullptr, instance, nullptr);
        if (!g_mainWindow)
        {
            Logf("CreateWindowW failed: %lu", GetLastError());
            return false;
        }
        ShowWindow(g_mainWindow, showCommand);
        UpdateWindow(g_mainWindow);
        return true;
    }
    int RunMessageLoop()
    {
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }
    void DestroyMainWindow()
    {
        if (g_mainWindow)
            DestroyWindow(g_mainWindow);
        g_mainWindow = nullptr;
        g_statusWindow = nullptr;
        g_logWindow = nullptr;
    }
} // namespace MultiFFBJoy
