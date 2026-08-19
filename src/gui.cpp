#include "common.h"
#include <commctrl.h>
#include <filesystem>
namespace MultiFFBJoy
{
    HWND g_mainWindow = nullptr;
    HWND g_statusWindow = nullptr;
    HWND g_logWindow = nullptr;
// ---------------------------------------------------------------------
// UTF-8 -> UTF-16
// ---------------------------------------------------------------------
    std::wstring Utf8ToWide(
        const char* text)
    {
        if (text == nullptr)
            return {};
        const int required =
        MultiByteToWideChar(
            CP_UTF8,
            0,
            text,
            -1,
            nullptr,
            0);
        if (required <= 0)
            return {};
        std::wstring result(
            static_cast<size_t>(required - 1),
            L'\0');
        MultiByteToWideChar(
            CP_UTF8,
            0,
            text,
            -1,
            result.data(),
            required);
        return result;
    }
// ---------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------
    void Log(
        const std::string& text)
    {
        auto* message =
        new std::wstring(
            Utf8ToWide(
                text.c_str()));
        if (g_mainWindow != nullptr)
        {
            PostMessageW(
                g_mainWindow,
                WM_APP_LOG,
                0,
                reinterpret_cast<LPARAM>(
                    message));
        }
        else
        {
            delete message;
        }
    }
// ---------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------
    void UpdateStatus()
    {
        if (g_statusWindow == nullptr)
            return;
        DeviceState state;
        {
            std::lock_guard<std::mutex> lock(
                g_stateMutex);
            state = g_state;
        }
        wchar_t text[4096]{};
        swprintf_s(
            text,
            L"Selected device: %ls\r\n"
            L"Axes: %lu\r\n"
            L"Force Feedback: %s\r\n"
            L"Spring effect: %s\r\n"
            L"Acquired: %s\r\n"
            L"Logical X axis: 0x%lX\r\n"
            L"Logical Y axis: 0x%lX\r\n"
            L"Spring strength: %.3f\r\n"
            L"UDP: 127.0.0.1:%d\r\n"
            L"FFB safety timeout: %lu ms",
            state.name.c_str(),
            state.axisCount,
            state.forceFeedback
            ? L"Yes"
            : L"No",
            state.springSupported
            ? L"Yes"
            : L"No",
            state.acquired
            ? L"Yes"
            : L"No",
            static_cast<unsigned long>(
                state.xAxisOffset),
            static_cast<unsigned long>(
                state.yAxisOffset),
            state.springStrength,
            UDP_PORT,
            COMMAND_TIMEOUT_MS);
        SetWindowTextW(
            g_statusWindow,
            text);
    }
// ---------------------------------------------------------------------
// GUI helpers
// ---------------------------------------------------------------------
    static HMENU MakeControlId(
        int id)
    {
        return MAKEINTRESOURCEA(id);
    }
// ---------------------------------------------------------------------
// Preset list
// ---------------------------------------------------------------------
    void PopulatePresetList()
    {
        if (g_mainWindow == nullptr)
            return;
        HWND list =
        GetDlgItem(
            g_mainWindow,
            IDC_PRESET_LIST);
        if (list == nullptr)
            return;
        SendMessageW(
            list,
            LB_RESETCONTENT,
            0,
            0);
        g_availablePresets =
        EnumerateForceFieldPresets();
        for (const auto& preset :
            g_availablePresets)
        {
            SendMessageW(
                list,
                LB_ADDSTRING,
                0,
                reinterpret_cast<LPARAM>(
                    preset.displayName.c_str()));
        }
        Logf(
            "Found %zu forcefield preset(s).",
            g_availablePresets.size());
    }
// ---------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------
    static LRESULT CALLBACK WindowProcedure(
        HWND window,
        UINT messageId,
        WPARAM wParam,
        LPARAM lParam)
    {
        switch (messageId)
        {
        case WM_CREATE:
            {
                g_statusWindow =
                CreateWindowExW(
                    WS_EX_CLIENTEDGE,
                    L"STATIC",
                    L"Initializing...",
                    WS_CHILD |
                    WS_VISIBLE |
                    SS_LEFT,
                    12,
                    12,
                    400,
                    190,
                    window,
                    nullptr,
                    GetModuleHandleW(nullptr),
                    nullptr);
                g_logWindow =
                CreateWindowExW(
                    WS_EX_CLIENTEDGE,
                    L"EDIT",
                    L"",
                    WS_CHILD |
                    WS_VISIBLE |
                    WS_VSCROLL |
                    ES_MULTILINE |
                    ES_AUTOVSCROLL |
                    ES_READONLY,
                    12,
                    230,
                    720,
                    280,
                    window,
                    nullptr,
                    GetModuleHandleW(nullptr),
                    nullptr);
                CreateWindowW(
                    L"BUTTON",
                    L"FFB Up",
                    WS_CHILD | WS_VISIBLE,
                    430,
                    12,
                    100,
                    32,
                    window,
                    MakeControlId(IDC_FFB_UP),
                    GetModuleHandleW(nullptr),
                    nullptr);
                CreateWindowW(
                    L"BUTTON",
                    L"FFB Down",
                    WS_CHILD | WS_VISIBLE,
                    540,
                    12,
                    100,
                    32,
                    window,
                    MakeControlId(IDC_FFB_DOWN),
                    GetModuleHandleW(nullptr),
                    nullptr);
                CreateWindowW(
                    L"BUTTON",
                    L"FFB Left",
                    WS_CHILD | WS_VISIBLE,
                    430,
                    52,
                    100,
                    32,
                    window,
                    MakeControlId(IDC_FFB_LEFT),
                    GetModuleHandleW(nullptr),
                    nullptr);
                CreateWindowW(
                    L"BUTTON",
                    L"FFB Right",
                    WS_CHILD | WS_VISIBLE,
                    540,
                    52,
                    100,
                    32,
                    window,
                    MakeControlId(IDC_FFB_RIGHT),
                    GetModuleHandleW(nullptr),
                    nullptr);
                CreateWindowW(
                    L"BUTTON",
                    L"Stop",
                    WS_CHILD | WS_VISIBLE,
                    430,
                    92,
                    100,
                    32,
                    window,
                    MakeControlId(IDC_FFB_STOP),
                    GetModuleHandleW(nullptr),
                    nullptr);
                CreateWindowW(
                    L"BUTTON",
                    L"Center",
                    WS_CHILD | WS_VISIBLE,
                    540,
                    92,
                    100,
                    32,
                    window,
                    MakeControlId(IDC_FFB_CENTER),
                    GetModuleHandleW(nullptr),
                    nullptr);
                CreateWindowExW(
                    0,
                    L"LISTBOX",
                    nullptr,
                    WS_CHILD |
                    WS_VISIBLE |
                    WS_BORDER |
                    LBS_NOTIFY |
                    WS_VSCROLL,
                    10,
                    135,
                    390,
                    90,
                    window,
                    MakeControlId(IDC_PRESET_LIST),
                    GetModuleHandleW(nullptr),
                    nullptr);
                CreateWindowExW(
                    0,
                    L"BUTTON",
                    L"Load",
                    WS_CHILD |
                    WS_VISIBLE |
                    BS_PUSHBUTTON,
                    410,
                    135,
                    80,
                    28,
                    window,
                    MakeControlId(IDC_PRESET_LOAD),
                    GetModuleHandleW(nullptr),
                    nullptr);
                UpdateStatus();
                return 0;
            }
        case WM_SIZE:
            {
                const int width =
                LOWORD(lParam);
                const int height =
                HIWORD(lParam);
                if (g_statusWindow != nullptr)
                {
                    MoveWindow(
                        g_statusWindow,
                        12,
                        12,
                        std::max(
                            100,
                            width - 380),
                        110,
                        TRUE);
                }
                if (g_logWindow != nullptr)
                {
                    MoveWindow(
                        g_logWindow,
                        12,
                        240,
                        std::max(
                            100,
                            width - 24),
                        std::max(
                            100,
                            height - 270),
                        TRUE);
                }
                return 0;
            }
        case WM_COMMAND:
            {
                const int controlId =
                LOWORD(wParam);
                const int notificationCode =
                HIWORD(wParam);
                if (notificationCode != BN_CLICKED)
                    break;
                switch (controlId)
                {
                case IDC_FFB_UP:
                    SendUdpCommand(
                        "TEST_FFB 0 10000");
                    return 0;
                case IDC_FFB_DOWN:
                    SendUdpCommand(
                        "TEST_FFB 0 -10000");
                    return 0;
                case IDC_FFB_LEFT:
                    SendUdpCommand(
                        "TEST_FFB 10000 0");
                    return 0;
                case IDC_FFB_RIGHT:
                    SendUdpCommand(
                        "TEST_FFB -10000 0");
                    return 0;
                case IDC_FFB_STOP:
                    {
                        Log("GUI: STOP");
                        ClearForceFieldPreset();
                        return 0;
                    }
                case IDC_FFB_CENTER:
                    SendUdpCommand("CENTER");
                    return 0;
                case IDC_PRESET_LOAD:
                    {
                        HWND list =
                        GetDlgItem(
                            window,
                            IDC_PRESET_LIST);
                        if (list == nullptr)
                            return 0;
                        const LRESULT selection =
                        SendMessageW(
                            list,
                            LB_GETCURSEL,
                            0,
                            0);
                        if (selection == LB_ERR)
                        {
                            Log(
                                "No forcefield preset selected.");
                            return 0;
                        }
                        const size_t index =
                        static_cast<size_t>(
                            selection);
                        if (index >=
                            g_availablePresets.size())
                        {
                            Log(
                                "Invalid forcefield preset selection.");
                            return 0;
                        }
                        const auto& preset =
                        g_availablePresets[index];
                        Logf(
                            "Loading forcefield preset: %ls",
                            preset.displayName.c_str());
                        if (LoadForceFieldPreset(
                            preset.path))
                        {
                            Log(
                                "Forcefield preset loaded.");
                            UpdatePresetTest();
                        }
                        return 0;
                    }
                default:
                    break;
                }
                break;
            }
        case WM_APP_LOG:
            {
                auto* logMessage =
                reinterpret_cast<std::wstring*>(
                    lParam);
                if (logMessage != nullptr)
                {
                    if (g_logWindow != nullptr)
                    {
                        const int length =
                        GetWindowTextLengthW(
                            g_logWindow);
                        SendMessageW(
                            g_logWindow,
                            EM_SETSEL,
                            static_cast<WPARAM>(
                                length),
                            static_cast<LPARAM>(
                                length));
                        const std::wstring line =
                        *logMessage + L"\r\n";
                        SendMessageW(
                            g_logWindow,
                            EM_REPLACESEL,
                            FALSE,
                            reinterpret_cast<LPARAM>(
                                line.c_str()));
                        SendMessageW(
                            g_logWindow,
                            EM_SCROLL,
                            SB_BOTTOM,
                            0);
                    }
                    delete logMessage;
                }
                return 0;
            }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(
            window,
            messageId,
            wParam,
            lParam);
    }
// ---------------------------------------------------------------------
// Main window creation
// ---------------------------------------------------------------------
    bool CreateMainWindow(
        HINSTANCE instance,
        int showCommand)
    {
        WNDCLASSW windowClass{};
        windowClass.hInstance =
        instance;
        windowClass.lpfnWndProc =
        WindowProcedure;
        windowClass.lpszClassName =
        L"MultiFFBJoyWindow";
        windowClass.hCursor =
        LoadCursorW(
            nullptr,
            IDC_ARROW);
        windowClass.hbrBackground =
        reinterpret_cast<HBRUSH>(
            COLOR_WINDOW + 1);
        if (!RegisterClassW(
            &windowClass))
        {
            Logf(
                "RegisterClassW failed: %lu",
                GetLastError());
            return false;
        }
        g_mainWindow =
        CreateWindowW(
            windowClass.lpszClassName,
            L"MultiFFBJoy - DirectInput FFB Bridge",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            760,
            560,
            nullptr,
            nullptr,
            instance,
            nullptr);
        if (g_mainWindow == nullptr)
        {
            Logf(
                "CreateWindowW failed: %lu",
                GetLastError());
            return false;
        }
        ShowWindow(
            g_mainWindow,
            showCommand);
        UpdateWindow(
            g_mainWindow);
        return true;
    }
// ---------------------------------------------------------------------
// Message loop
// ---------------------------------------------------------------------
    int RunMessageLoop()
    {
        MSG winMessage{};
        while (GetMessageW(
            &winMessage,
            nullptr,
            0,
            0) > 0)
        {
            TranslateMessage(
                &winMessage);
            DispatchMessageW(
                &winMessage);
        }
        return static_cast<int>(
            winMessage.wParam);
    }
// ---------------------------------------------------------------------
// Window destruction
// ---------------------------------------------------------------------
    void DestroyMainWindow()
    {
        if (g_mainWindow != nullptr)
        {
            DestroyWindow(
                g_mainWindow);
            g_mainWindow = nullptr;
        }
        g_statusWindow = nullptr;
        g_logWindow = nullptr;
    }
}