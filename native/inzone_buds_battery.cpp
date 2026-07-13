#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kAppName[] = L"INZONE Buds Battery";
constexpr wchar_t kWindowClass[] = L"INZONEBudsBatteryNativeWindow";
constexpr wchar_t kMutexName[] = L"Local\\INZONEBudsBattery";
constexpr wchar_t kNoticeMutexName[] = L"Local\\INZONEBudsBatteryAlreadyRunningNotice";
constexpr wchar_t kStartupLinkName[] = L"INZONE Buds Battery.lnk";
constexpr wchar_t kLegacyStartupFileName[] = L"INZONE Buds Battery.cmd";
constexpr wchar_t kStartupRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kStartupValueName[] = L"INZONE Buds Battery";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT_PTR kTimerId = 1;
constexpr UINT kRefreshMs = 15000;

constexpr UINT kCmdShow = 1001;
constexpr UINT kCmdRefresh = 1002;
constexpr UINT kCmdStartup = 1003;
constexpr UINT kCmdExit = 1004;

struct BatteryValue {
    std::string value = "Unknown";
    std::string localTime;
    long long timestamp = 0;

    int percent() const {
        if (value.empty()) return -1;
        char* end = nullptr;
        long parsed = std::strtol(value.c_str(), &end, 10);
        if (*end != '\0' || parsed < 0 || parsed > 100) return -1;
        return static_cast<int>(parsed);
    }

    std::wstring label() const {
        int p = percent();
        if (p >= 0) return std::to_wstring(p) + L"%";
        if (value == "Disconnect") return L"Disconnect";
        return L"Unknown";
    }
};

struct BatteryState {
    BatteryValue left;
    BatteryValue right;
    BatteryValue caseBattery;
    std::wstring error;

    int iconPercent() const {
        std::vector<int> values;
        int l = left.percent();
        int r = right.percent();
        if (l >= 0) values.push_back(l);
        if (r >= 0) values.push_back(r);
        if (values.empty()) return -1;
        return *std::min_element(values.begin(), values.end());
    }

    std::wstring summary() const {
        return L"L " + left.label() + L" / R " + right.label() + L" / Case " + caseBattery.label();
    }

    std::wstring details() const {
        std::wstring text = L"INZONE Buds\n";
        text += L"Left : " + left.label() + L"\n";
        text += L"Right: " + right.label() + L"\n";
        text += L"Case : " + caseBattery.label();
        std::string last = newestLocalTime();
        if (!last.empty()) text += L"\nLast : " + widen(last);
        if (!error.empty()) text += L"\nNote : " + error;
        return text;
    }

    std::string newestLocalTime() const {
        const BatteryValue* newest = &left;
        if (right.timestamp > newest->timestamp) newest = &right;
        if (caseBattery.timestamp > newest->timestamp) newest = &caseBattery;
        return newest->localTime;
    }

    static std::wstring widen(const std::string& s) {
        if (s.empty()) return L"";
        int size = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        if (size <= 0) return std::wstring(s.begin(), s.end());
        std::wstring result(size, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), result.data(), size);
        return result;
    }
};

std::wstring widen(const std::string& s) {
    return BatteryState::widen(s);
}

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

std::wstring pathJoin(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    wchar_t last = dir.back();
    if (last == L'\\' || last == L'/') return dir + name;
    return dir + L"\\" + name;
}

std::wstring startupDirectory() {
    std::wstring appData = envPath(L"APPDATA");
    if (!appData.empty()) return appData + L"\\Microsoft\\Windows\\Start Menu\\Programs\\Startup";
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

bool fileExists(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

void cleanupLegacyStartupFiles() {
    DeleteFileW(startupPath().c_str());
    DeleteFileW(legacyStartupPath().c_str());
}

std::wstring startupCommand() {
    return L"\"" + modulePath() + L"\"";
}

bool isStartupEnabledInRegistry() {
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
    std::wstring command = startupCommand();
    result = RegSetValueExW(key, kStartupValueName, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(command.c_str()),
                            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return result == ERROR_SUCCESS && isStartupEnabledInRegistry();
}

std::string extractJsonString(const std::string& line, const std::string& key) {
    std::string marker = "\"" + key + "\":";
    size_t pos = line.find(marker);
    if (pos == std::string::npos) return "";
    pos += marker.size();
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
    if (pos >= line.size()) return "";
    if (line[pos] == '"') {
        ++pos;
        std::string out;
        bool escaped = false;
        for (; pos < line.size(); ++pos) {
            char c = line[pos];
            if (escaped) {
                out.push_back(c);
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                break;
            } else {
                out.push_back(c);
            }
        }
        return out;
    }
    size_t end = line.find_first_of(",}", pos);
    return line.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

long long extractJsonInt64(const std::string& line, const std::string& key) {
    std::string value = extractJsonString(line, key);
    if (value.empty()) return 0;
    return std::strtoll(value.c_str(), nullptr, 10);
}

void updateValue(BatteryValue& target, const std::string& value, const std::string& localTime, long long timestamp) {
    if (timestamp >= target.timestamp) {
        target.value = value.empty() ? "Unknown" : value;
        target.localTime = localTime;
        target.timestamp = timestamp;
    }
}

BatteryState readBatteryState() {
    BatteryState state;
    std::wstring path = logPath();
    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file) {
        state.error = L"Log file not found.";
        return state;
    }

    file.seekg(0, std::ios::end);
    std::streamoff size = file.tellg();
    std::streamoff start = size > 2'000'000 ? size - 2'000'000 : 0;
    file.seekg(start, std::ios::beg);
    if (start > 0) {
        std::string ignored;
        std::getline(file, ignored);
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.find("batteryStatus") == std::string::npos) continue;
        std::string item = extractJsonString(line, "item");
        std::string value = extractJsonString(line, "value");
        std::string local = extractJsonString(line, "localTime");
        long long timestamp = extractJsonInt64(line, "timeStamp");
        if (item == "batteryStatusLeft") {
            updateValue(state.left, value, local, timestamp);
        } else if (item == "batteryStatusRight") {
            updateValue(state.right, value, local, timestamp);
        } else if (item == "batteryStatusCase") {
            updateValue(state.caseBattery, value, local, timestamp);
        }
    }
    return state;
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
    HBITMAP old = static_cast<HBITMAP>(SelectObject(dc, color));

    HBRUSH brush = CreateSolidBrush(batteryColor(percent));
    RECT rect{0, 0, size, size};
    FillRect(dc, &rect, brush);
    DeleteObject(brush);

    HPEN pen = CreatePen(PS_SOLID, 3, RGB(255, 255, 255));
    HPEN oldPen = static_cast<HPEN>(SelectObject(dc, pen));
    HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(dc, GetStockObject(NULL_BRUSH)));
    RoundRect(dc, 3, 3, 61, 61, 12, 12);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);

    std::wstring text = percent < 0 ? L"--" : std::to_wstring(percent);
    int fontSize = text.size() <= 2 ? 31 : 25;
    HFONT font = CreateFontW(-fontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT oldFont = static_cast<HFONT>(SelectObject(dc, font));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(dc, text.c_str(), -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldFont);
    DeleteObject(font);

    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmColor = color;
    info.hbmMask = mask;
    HICON icon = CreateIconIndirect(&info);

    SelectObject(dc, old);
    DeleteObject(mask);
    DeleteObject(color);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return icon;
}

class TrayApp {
public:
    int run(HINSTANCE instance) {
        instance_ = instance;
        state_ = readBatteryState();
        if (!createWindow()) return 1;
        taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
        addOrUpdateIcon(NIM_ADD);
        SetTimer(hwnd_, kTimerId, kRefreshMs, nullptr);

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return 0;
    }

private:
    HINSTANCE instance_{};
    HWND hwnd_{};
    HICON icon_{};
    BatteryState state_{};
    std::wstring lastSummary_;
    bool detailsOpen_{};
    UINT taskbarCreatedMessage_{};

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        auto* app = reinterpret_cast<TrayApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            app = reinterpret_cast<TrayApp*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        return app ? app->handleMessage(hwnd, msg, wparam, lparam) : DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    bool createWindow() {
        WNDCLASSW wc{};
        wc.lpfnWndProc = TrayApp::windowProc;
        wc.hInstance = instance_;
        wc.lpszClassName = kWindowClass;
        RegisterClassW(&wc);
        hwnd_ = CreateWindowExW(0, kWindowClass, kAppName, 0, 0, 0, 0, 0,
                                nullptr, nullptr, instance_, this);
        return hwnd_ != nullptr;
    }

    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        if (taskbarCreatedMessage_ != 0 && msg == taskbarCreatedMessage_) {
            addOrUpdateIcon(NIM_ADD);
            return 0;
        }
        switch (msg) {
            case WM_TIMER:
                refresh(false);
                return 0;
            case WM_COMMAND:
                handleCommand(LOWORD(wparam));
                return 0;
            case kTrayMessage: {
                UINT event = LOWORD(lparam);
                if (event == WM_LBUTTONUP) showDetails();
                if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) showMenu();
                return 0;
            }
            case WM_DESTROY:
            {
                auto data = notifyData();
                Shell_NotifyIconW(NIM_DELETE, &data);
                if (icon_) DestroyIcon(icon_);
                PostQuitMessage(0);
                return 0;
            }
        }
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    void handleCommand(UINT id) {
        if (id == kCmdShow) showDetails();
        if (id == kCmdRefresh) refresh(false);
        if (id == kCmdStartup) toggleStartup();
        if (id == kCmdExit) DestroyWindow(hwnd_);
    }

    NOTIFYICONDATAW notifyData() {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = hwnd_;
        data.uID = 1;
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        data.uCallbackMessage = kTrayMessage;
        data.hIcon = icon_;
        std::wstring tip = std::wstring(kAppName) + L": " + state_.summary();
        wcsncpy_s(data.szTip, tip.c_str(), _TRUNCATE);
        return data;
    }

    void addOrUpdateIcon(DWORD command) {
        std::wstring summary = state_.summary();
        HICON old = nullptr;
        if (!icon_ || summary != lastSummary_) {
            old = icon_;
            icon_ = createBatteryIcon(state_.iconPercent());
        }
        auto data = notifyData();
        bool registered = Shell_NotifyIconW(command, &data) != FALSE;
        if (!registered && command == NIM_MODIFY) {
            registered = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
        }
        if (registered) {
            data.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &data);
        }
        lastSummary_ = summary;
        if (old) DestroyIcon(old);
    }

    void refresh(bool show) {
        state_ = readBatteryState();
        addOrUpdateIcon(NIM_MODIFY);
        if (show) showDetails();
    }

    void showDetails() {
        if (detailsOpen_) {
            SetForegroundWindow(hwnd_);
            return;
        }
        detailsOpen_ = true;
        MessageBoxW(hwnd_, state_.details().c_str(), kAppName, MB_OK | MB_ICONINFORMATION);
        detailsOpen_ = false;
    }

    void showMenu() {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, kCmdShow, L"バッテリーを表示");
        AppendMenuW(menu, MF_STRING, kCmdRefresh, L"表示を更新");
        std::wstring startup = isStartupEnabled() ? L"スタートアップ起動: ON" : L"スタートアップ起動: OFF";
        AppendMenuW(menu, MF_STRING | (isStartupEnabled() ? MF_CHECKED : 0), kCmdStartup, startup.c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kCmdExit, L"終了");

        POINT pt{};
        GetCursorPos(&pt);
        SetForegroundWindow(hwnd_);
        UINT command = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                      pt.x, pt.y, 0, hwnd_, nullptr);
        PostMessageW(hwnd_, WM_NULL, 0, 0);
        DestroyMenu(menu);
        if (command) PostMessageW(hwnd_, WM_COMMAND, command, 0);
    }

    bool isStartupEnabled() const {
        return isStartupEnabledInRegistry();
    }

    void toggleStartup() {
        bool enable = !isStartupEnabled();
        if (!setStartupEnabled(enable)) {
            MessageBoxW(hwnd_, L"スタートアップ設定に失敗しました。", kAppName, MB_OK | MB_ICONERROR);
            return;
        }
        MessageBoxW(hwnd_, enable ? L"スタートアップをONにしました。" : L"スタートアップをOFFにしました。",
                    kAppName, MB_OK | MB_ICONINFORMATION);
    }
};

}  // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    HANDLE mutex = CreateMutexW(nullptr, FALSE, kMutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HANDLE noticeMutex = CreateMutexW(nullptr, FALSE, kNoticeMutexName);
        if (noticeMutex && GetLastError() != ERROR_ALREADY_EXISTS) {
            MessageBoxW(nullptr, L"INZONE Buds Battery はすでに起動しています。", kAppName,
                        MB_OK | MB_ICONINFORMATION);
        }
        if (noticeMutex) CloseHandle(noticeMutex);
        CloseHandle(mutex);
        return 0;
    }
    TrayApp app;
    int result = app.run(instance);
    if (mutex) CloseHandle(mutex);
    return result;
}
