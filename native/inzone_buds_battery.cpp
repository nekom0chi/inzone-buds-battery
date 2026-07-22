#define NOMINMAX
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <string>

namespace {

constexpr wchar_t kAppName[] = L"INZONE Buds Battery";
constexpr wchar_t kWindowClass[] = L"INZONEBudsBatteryNativeWindow";
constexpr wchar_t kMutexName[] = L"Local\\INZONEBudsBattery";
constexpr wchar_t kStartupLinkName[] = L"INZONE Buds Battery.lnk";
constexpr wchar_t kLegacyStartupFileName[] = L"INZONE Buds Battery.cmd";
constexpr wchar_t kStartupRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kStartupValueName[] = L"INZONE Buds Battery";

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kRefreshMessage = WM_APP + 2;
constexpr UINT_PTR kTimerId = 1;
constexpr UINT kRefreshMs = 60'000;
constexpr ULONG kTimerToleranceMs = 15'000;

constexpr UINT kCmdRefresh = 1001;
constexpr UINT kCmdStartup = 1002;
constexpr UINT kCmdExit = 1003;

struct BatteryValue {
    std::string value = "Unknown";
    long long timestamp = 0;

    int percent() const {
        if (value.empty()) return -1;
        char* end = nullptr;
        long parsed = std::strtol(value.c_str(), &end, 10);
        if (*end != '\0' || parsed < 0 || parsed > 100) return -1;
        return static_cast<int>(parsed);
    }

    std::wstring label() const {
        int parsed = percent();
        if (parsed >= 0) return std::to_wstring(parsed) + L"%";
        if (value == "Disconnect") return L"Disconnect";
        return L"Unknown";
    }
};

struct BatteryState {
    BatteryValue left;
    BatteryValue right;
    BatteryValue caseBattery;

    int iconPercent() const {
        int leftPercent = left.percent();
        int rightPercent = right.percent();
        if (leftPercent >= 0 && rightPercent >= 0) return std::min(leftPercent, rightPercent);
        if (leftPercent >= 0) return leftPercent;
        return rightPercent;
    }

    std::wstring summary() const {
        return L"L " + left.label() + L" / R " + right.label() + L" / Case " + caseBattery.label();
    }
};

struct LogSnapshot {
    bool exists = false;
    FILETIME writeTime{};
    ULONGLONG size = 0;
};

std::wstring envPath(const wchar_t* name) {
    DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (!needed) return L"";
    std::wstring value(needed, L'\0');
    GetEnvironmentVariableW(name, value.data(), needed);
    if (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

std::wstring logPath() {
    return envPath(L"APPDATA") + L"\\Sony\\INZONE Hub\\ActionLog.log";
}

std::wstring pathJoin(const std::wstring& directory, const std::wstring& name) {
    if (directory.empty()) return name;
    wchar_t last = directory.back();
    if (last == L'\\' || last == L'/') return directory + name;
    return directory + L"\\" + name;
}

std::wstring startupDirectory() {
    return envPath(L"APPDATA") + L"\\Microsoft\\Windows\\Start Menu\\Programs\\Startup";
}

std::wstring startupPath() {
    return pathJoin(startupDirectory(), kStartupLinkName);
}

std::wstring legacyStartupPath() {
    return pathJoin(startupDirectory(), kLegacyStartupFileName);
}

std::wstring modulePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    while (size == buffer.size()) {
        buffer.resize(buffer.size() * 2);
        size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    }
    buffer.resize(size);
    return buffer;
}

void cleanupLegacyStartupFiles() {
    DeleteFileW(startupPath().c_str());
    DeleteFileW(legacyStartupPath().c_str());
}

bool isStartupEnabled() {
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kStartupRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD size = 0;
    LONG result = RegQueryValueExW(key, kStartupValueName, nullptr, &type, nullptr, &size);
    RegCloseKey(key);
    return result == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && size > 0;
}

bool setStartupEnabled(bool enable) {
    HKEY key{};
    LONG result = RegCreateKeyExW(HKEY_CURRENT_USER, kStartupRunKey, 0, nullptr, 0,
                                  KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &key, nullptr);
    if (result != ERROR_SUCCESS) return false;

    if (!enable) {
        result = RegDeleteValueW(key, kStartupValueName);
        RegCloseKey(key);
        cleanupLegacyStartupFiles();
        return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
    }

    cleanupLegacyStartupFiles();
    std::wstring command = L"\"" + modulePath() + L"\"";
    result = RegSetValueExW(key, kStartupValueName, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(command.c_str()),
                            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

std::string extractJsonString(const std::string& line, const std::string& key) {
    std::string marker = "\"" + key + "\":";
    size_t position = line.find(marker);
    if (position == std::string::npos) return "";
    position += marker.size();
    while (position < line.size() && (line[position] == ' ' || line[position] == '\t')) ++position;
    if (position >= line.size()) return "";

    if (line[position] == '"') {
        ++position;
        std::string result;
        bool escaped = false;
        for (; position < line.size(); ++position) {
            char character = line[position];
            if (escaped) {
                result.push_back(character);
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == '"') {
                break;
            } else {
                result.push_back(character);
            }
        }
        return result;
    }

    size_t end = line.find_first_of(",}", position);
    return line.substr(position, end == std::string::npos ? std::string::npos : end - position);
}

long long extractJsonInt64(const std::string& line, const std::string& key) {
    std::string value = extractJsonString(line, key);
    return value.empty() ? 0 : std::strtoll(value.c_str(), nullptr, 10);
}

void updateValue(BatteryValue& target, const std::string& value, long long timestamp) {
    if (timestamp >= target.timestamp) {
        target.value = value.empty() ? "Unknown" : value;
        target.timestamp = timestamp;
    }
}

BatteryState readBatteryState() {
    BatteryState state;
    std::ifstream file(logPath().c_str(), std::ios::binary);
    if (!file) return state;

    file.seekg(0, std::ios::end);
    std::streamoff size = file.tellg();
    if (size < 0) return state;
    constexpr std::streamoff kReadTailBytes = 1'000'000;
    std::streamoff start = size > kReadTailBytes ? size - kReadTailBytes : 0;
    file.seekg(start, std::ios::beg);
    if (start > 0) {
        std::string partialLine;
        std::getline(file, partialLine);
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.find("batteryStatus") == std::string::npos) continue;
        std::string item = extractJsonString(line, "item");
        std::string value = extractJsonString(line, "value");
        long long timestamp = extractJsonInt64(line, "timeStamp");
        if (item == "batteryStatusLeft") {
            updateValue(state.left, value, timestamp);
        } else if (item == "batteryStatusRight") {
            updateValue(state.right, value, timestamp);
        } else if (item == "batteryStatusCase") {
            updateValue(state.caseBattery, value, timestamp);
        }
    }
    return state;
}

LogSnapshot getLogSnapshot() {
    LogSnapshot snapshot;
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(logPath().c_str(), GetFileExInfoStandard, &attributes)) return snapshot;
    snapshot.exists = true;
    snapshot.writeTime = attributes.ftLastWriteTime;
    snapshot.size = (static_cast<ULONGLONG>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
    return snapshot;
}

bool sameSnapshot(const LogSnapshot& left, const LogSnapshot& right) {
    return left.exists == right.exists && left.size == right.size &&
           CompareFileTime(&left.writeTime, &right.writeTime) == 0;
}

COLORREF batteryColor(int percent) {
    if (percent < 0) return RGB(82, 91, 102);
    if (percent <= 20) return RGB(206, 67, 67);
    if (percent <= 50) return RGB(214, 142, 39);
    return RGB(43, 147, 96);
}

HICON createBatteryIcon(int percent) {
    constexpr int size = 64;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP color = CreateCompatibleBitmap(screen, size, size);
    HBITMAP previousBitmap = static_cast<HBITMAP>(SelectObject(dc, color));

    RECT bounds{0, 0, size, size};
    HBRUSH background = CreateSolidBrush(batteryColor(percent));
    FillRect(dc, &bounds, background);
    DeleteObject(background);

    HPEN border = CreatePen(PS_SOLID, 3, RGB(255, 255, 255));
    HPEN previousPen = static_cast<HPEN>(SelectObject(dc, border));
    HBRUSH previousBrush = static_cast<HBRUSH>(SelectObject(dc, GetStockObject(NULL_BRUSH)));
    RoundRect(dc, 3, 3, 61, 61, 12, 12);
    SelectObject(dc, previousBrush);
    SelectObject(dc, previousPen);
    DeleteObject(border);

    std::wstring text = percent < 0 ? L"--" : std::to_wstring(percent);
    int fontSize = text.size() <= 2 ? 31 : 25;
    HFONT font = CreateFontW(-fontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT previousFont = static_cast<HFONT>(SelectObject(dc, font));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(dc, text.c_str(), -1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, previousFont);
    DeleteObject(font);

    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmColor = color;
    info.hbmMask = mask;
    HICON icon = CreateIconIndirect(&info);

    SelectObject(dc, previousBitmap);
    DeleteObject(mask);
    DeleteObject(color);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return icon;
}

void enableEfficiencyMode() {
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));
}

class TrayApp {
public:
    int run(HINSTANCE instance) {
        instance_ = instance;
        enableEfficiencyMode();
        if (!createWindow()) return 1;

        refresh(true);
        addOrUpdateIcon(NIM_ADD);
        if (!SetCoalescableTimer(hwnd_, kTimerId, kRefreshMs, nullptr, kTimerToleranceMs)) {
            SetTimer(hwnd_, kTimerId, kRefreshMs, nullptr);
        }

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }

private:
    HINSTANCE instance_{};
    HWND hwnd_{};
    HICON icon_{};
    BatteryState state_{};
    LogSnapshot logSnapshot_{};
    std::wstring lastSummary_;
    UINT taskbarCreatedMessage_{};

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* app = reinterpret_cast<TrayApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            app = static_cast<TrayApp*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        return app ? app->handleMessage(hwnd, message, wparam, lparam)
                   : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    bool createWindow() {
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = TrayApp::windowProc;
        windowClass.hInstance = instance_;
        windowClass.lpszClassName = kWindowClass;
        if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        hwnd_ = CreateWindowExW(0, kWindowClass, kAppName, 0, 0, 0, 0, 0,
                                nullptr, nullptr, instance_, this);
        taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
        return hwnd_ != nullptr;
    }

    LRESULT handleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        if (taskbarCreatedMessage_ && message == taskbarCreatedMessage_) {
            addOrUpdateIcon(NIM_ADD);
            return 0;
        }

        switch (message) {
            case WM_TIMER:
                refresh(false);
                return 0;
            case WM_POWERBROADCAST:
                if (wparam == PBT_APMRESUMEAUTOMATIC) refresh(true);
                return TRUE;
            case WM_COMMAND:
                handleCommand(LOWORD(wparam));
                return 0;
            case kRefreshMessage:
                refresh(true);
                return 0;
            case kTrayMessage: {
                UINT event = LOWORD(lparam);
                if (event == WM_LBUTTONUP || event == NIN_SELECT || event == NIN_KEYSELECT) refresh(true);
                if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) showMenu();
                return 0;
            }
            case WM_DESTROY: {
                KillTimer(hwnd, kTimerId);
                auto data = notifyData();
                Shell_NotifyIconW(NIM_DELETE, &data);
                if (icon_) DestroyIcon(icon_);
                PostQuitMessage(0);
                return 0;
            }
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    void handleCommand(UINT command) {
        if (command == kCmdRefresh) refresh(true);
        if (command == kCmdStartup) toggleStartup();
        if (command == kCmdExit) DestroyWindow(hwnd_);
    }

    NOTIFYICONDATAW notifyData() const {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = hwnd_;
        data.uID = 1;
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        data.uCallbackMessage = kTrayMessage;
        data.hIcon = icon_;
        std::wstring tooltip = std::wstring(kAppName) + L": " + state_.summary();
        wcsncpy_s(data.szTip, tooltip.c_str(), _TRUNCATE);
        return data;
    }

    void addOrUpdateIcon(DWORD command) {
        std::wstring summary = state_.summary();
        HICON previousIcon = nullptr;
        if (!icon_ || summary != lastSummary_) {
            previousIcon = icon_;
            icon_ = createBatteryIcon(state_.iconPercent());
        }

        auto data = notifyData();
        bool registered = Shell_NotifyIconW(command, &data) != FALSE;
        if (!registered && command == NIM_MODIFY) registered = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
        if (registered) {
            data.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &data);
        }
        lastSummary_ = summary;
        if (previousIcon) DestroyIcon(previousIcon);
    }

    void refresh(bool force) {
        LogSnapshot currentSnapshot = getLogSnapshot();
        if (!force && sameSnapshot(currentSnapshot, logSnapshot_)) return;

        logSnapshot_ = currentSnapshot;
        BatteryState updated = readBatteryState();
        if (updated.summary() == state_.summary()) return;
        state_ = updated;
        addOrUpdateIcon(NIM_MODIFY);
    }

    void showMenu() {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, kCmdRefresh, L"今すぐ更新");
        bool startup = isStartupEnabled();
        AppendMenuW(menu, MF_STRING | (startup ? MF_CHECKED : 0), kCmdStartup,
                    startup ? L"スタートアップ起動: ON" : L"スタートアップ起動: OFF");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kCmdExit, L"終了");

        POINT cursor{};
        GetCursorPos(&cursor);
        SetForegroundWindow(hwnd_);
        UINT command = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                      cursor.x, cursor.y, 0, hwnd_, nullptr);
        PostMessageW(hwnd_, WM_NULL, 0, 0);
        DestroyMenu(menu);
        if (command) PostMessageW(hwnd_, WM_COMMAND, command, 0);
    }

    void toggleStartup() {
        bool enable = !isStartupEnabled();
        if (!setStartupEnabled(enable)) {
            MessageBoxW(hwnd_, L"スタートアップ設定に失敗しました。", kAppName, MB_OK | MB_ICONERROR);
        }
    }
};

}  // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    HANDLE mutex = CreateMutexW(nullptr, FALSE, kMutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(kWindowClass, kAppName);
        if (existing) PostMessageW(existing, kRefreshMessage, 0, 0);
        CloseHandle(mutex);
        return 0;
    }

    TrayApp app;
    int result = app.run(instance);
    if (mutex) CloseHandle(mutex);
    return result;
}
