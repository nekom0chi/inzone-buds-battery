#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <objidl.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kAppName[] = L"INZONE Buds Battery";
constexpr wchar_t kWindowClass[] = L"INZONEBudsBatteryNativeWindow";
constexpr wchar_t kDashboardClass[] = L"INZONEBudsBatteryDashboardWindow";
constexpr wchar_t kMutexName[] = L"Local\\INZONEBudsBattery";
constexpr wchar_t kNoticeMutexName[] = L"Local\\INZONEBudsBatteryAlreadyRunningNotice";
constexpr wchar_t kStartupLinkName[] = L"INZONE Buds Battery.lnk";
constexpr wchar_t kLegacyStartupFileName[] = L"INZONE Buds Battery.cmd";
constexpr wchar_t kStartupRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kStartupValueName[] = L"INZONE Buds Battery";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT_PTR kTimerId = 1;
constexpr UINT kRefreshMs = 15000;
constexpr int kProductImageResource = 101;
constexpr int kDashboardWidth = 620;
constexpr int kDashboardHeight = 600;

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

std::wstring historyDirectory() {
    return envPath(L"LOCALAPPDATA") + L"\\INZONE Buds Battery";
}

std::wstring historyPath() {
    return historyDirectory() + L"\\battery-history.csv";
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

struct HistorySample {
    long long timestamp = 0;
    int left = -1;
    int right = -1;
    int caseBattery = -1;
};

long long unixSeconds(long long timestamp) {
    while (timestamp > 40'000'000'000LL) timestamp /= 1000;
    return timestamp;
}

bool parseStoredSample(const std::string& line, HistorySample& sample) {
    long long timestamp = 0;
    int left = -1;
    int right = -1;
    int caseBattery = -1;
    if (std::sscanf(line.c_str(), "%lld,%d,%d,%d", &timestamp, &left, &right, &caseBattery) != 4) return false;
    sample = {timestamp, left, right, caseBattery};
    return timestamp > 0;
}

void recordBatteryHistory(const BatteryState& state) {
    static long long lastWrite = 0;
    static int lastLeft = -2;
    static int lastRight = -2;
    static int lastCase = -2;

    int left = state.left.percent();
    int right = state.right.percent();
    int caseBattery = state.caseBattery.percent();
    if (left < 0 && right < 0 && caseBattery < 0) return;

    long long now = static_cast<long long>(std::time(nullptr));
    bool changed = left != lastLeft || right != lastRight || caseBattery != lastCase;
    if (!changed && now - lastWrite < 5 * 60) return;

    CreateDirectoryW(historyDirectory().c_str(), nullptr);
    std::ofstream file(historyPath().c_str(), std::ios::app);
    if (!file) return;
    file << now << ',' << left << ',' << right << ',' << caseBattery << '\n';
    lastWrite = now;
    lastLeft = left;
    lastRight = right;
    lastCase = caseBattery;
}

void pruneStoredHistory() {
    std::ifstream input(historyPath().c_str());
    if (!input) return;
    long long cutoff = static_cast<long long>(std::time(nullptr)) - 8LL * 24 * 60 * 60;
    std::vector<HistorySample> samples;
    std::string line;
    while (std::getline(input, line)) {
        HistorySample sample;
        if (parseStoredSample(line, sample) && sample.timestamp >= cutoff) samples.push_back(sample);
    }
    input.close();

    std::wstring temporary = historyPath() + L".tmp";
    std::ofstream output(temporary.c_str(), std::ios::trunc);
    if (!output) return;
    for (const auto& sample : samples) {
        output << sample.timestamp << ',' << sample.left << ',' << sample.right << ',' << sample.caseBattery << '\n';
    }
    output.close();
    MoveFileExW(temporary.c_str(), historyPath().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

std::vector<HistorySample> readBatteryHistory() {
    std::vector<HistorySample> history;
    auto appendStoredHistory = [&history]() {
        std::ifstream stored(historyPath().c_str());
        std::string storedLine;
        while (std::getline(stored, storedLine)) {
            HistorySample sample;
            if (parseStoredSample(storedLine, sample)) history.push_back(sample);
        }
        long long cutoff = static_cast<long long>(std::time(nullptr)) - 8LL * 24 * 60 * 60;
        history.erase(std::remove_if(history.begin(), history.end(),
                                     [cutoff](const HistorySample& sample) { return sample.timestamp < cutoff; }),
                      history.end());
        std::sort(history.begin(), history.end(),
                  [](const HistorySample& a, const HistorySample& b) { return a.timestamp < b.timestamp; });
    };

    std::ifstream file(logPath().c_str(), std::ios::binary);
    if (!file) {
        appendStoredHistory();
        return history;
    }

    file.seekg(0, std::ios::end);
    std::streamoff size = file.tellg();
    constexpr std::streamoff kHistoryReadLimit = 64LL * 1024 * 1024;
    std::streamoff start = size > kHistoryReadLimit ? size - kHistoryReadLimit : 0;
    file.seekg(start, std::ios::beg);
    if (start > 0) {
        std::string ignored;
        std::getline(file, ignored);
    }

    int left = -1;
    int right = -1;
    int caseBattery = -1;
    std::string line;
    while (std::getline(file, line)) {
        if (line.find("batteryStatus") == std::string::npos) continue;
        std::string item = extractJsonString(line, "item");
        BatteryValue value;
        value.value = extractJsonString(line, "value");
        int percent = value.percent();
        long long timestamp = unixSeconds(extractJsonInt64(line, "timeStamp"));
        if (timestamp <= 0) continue;

        if (item == "batteryStatusLeft") left = percent;
        else if (item == "batteryStatusRight") right = percent;
        else if (item == "batteryStatusCase") caseBattery = percent;
        else continue;

        if (!history.empty() && history.back().timestamp == timestamp) {
            history.back() = {timestamp, left, right, caseBattery};
        } else {
            history.push_back({timestamp, left, right, caseBattery});
        }
    }
    appendStoredHistory();
    return history;
}

std::wstring formatGraphTime(long long timestamp, bool week) {
    std::time_t value = static_cast<std::time_t>(timestamp);
    std::tm local{};
    localtime_s(&local, &value);
    wchar_t buffer[32]{};
    wcsftime(buffer, std::size(buffer), week ? L"%m/%d" : L"%H:%M", &local);
    return buffer;
}

std::unique_ptr<Gdiplus::Bitmap> loadPngResource(HINSTANCE instance, int resourceId) {
    HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!resource) return nullptr;
    DWORD size = SizeofResource(instance, resource);
    HGLOBAL loaded = LoadResource(instance, resource);
    const void* source = LockResource(loaded);
    if (!source || size == 0) return nullptr;

    HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!copy) return nullptr;
    void* destination = GlobalLock(copy);
    memcpy(destination, source, size);
    GlobalUnlock(copy);

    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(copy, TRUE, &stream) != S_OK) {
        GlobalFree(copy);
        return nullptr;
    }
    std::unique_ptr<Gdiplus::Bitmap> sourceBitmap(Gdiplus::Bitmap::FromStream(stream));
    std::unique_ptr<Gdiplus::Bitmap> result;
    if (sourceBitmap && sourceBitmap->GetLastStatus() == Gdiplus::Ok) {
        result.reset(sourceBitmap->Clone(0, 0, sourceBitmap->GetWidth(), sourceBitmap->GetHeight(),
                                         PixelFormat32bppARGB));
    }
    stream->Release();
    return result;
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
    int run(HINSTANCE instance, bool openDashboard) {
        instance_ = instance;
        Gdiplus::GdiplusStartupInput startupInput;
        if (Gdiplus::GdiplusStartup(&gdiplusToken_, &startupInput, nullptr) != Gdiplus::Ok) return 1;
        productImage_ = loadPngResource(instance_, kProductImageResource);
        pruneStoredHistory();
        state_ = readBatteryState();
        recordBatteryHistory(state_);
        if (!createWindow()) {
            Gdiplus::GdiplusShutdown(gdiplusToken_);
            return 1;
        }
        taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
        addOrUpdateIcon(NIM_ADD);
        SetTimer(hwnd_, kTimerId, kRefreshMs, nullptr);
        if (openDashboard) showDetails();

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        productImage_.reset();
        Gdiplus::GdiplusShutdown(gdiplusToken_);
        return 0;
    }

private:
    HINSTANCE instance_{};
    HWND hwnd_{};
    HWND dashboard_{};
    HICON icon_{};
    BatteryState state_{};
    std::vector<HistorySample> history_;
    std::wstring lastSummary_;
    std::unique_ptr<Gdiplus::Bitmap> productImage_;
    ULONG_PTR gdiplusToken_{};
    bool showWeek_{};
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

    static LRESULT CALLBACK dashboardProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        auto* app = reinterpret_cast<TrayApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            app = reinterpret_cast<TrayApp*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        return app ? app->handleDashboardMessage(hwnd, msg, wparam, lparam)
                   : DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    bool createWindow() {
        WNDCLASSW wc{};
        wc.lpfnWndProc = TrayApp::windowProc;
        wc.hInstance = instance_;
        wc.lpszClassName = kWindowClass;
        RegisterClassW(&wc);
        hwnd_ = CreateWindowExW(0, kWindowClass, kAppName, 0, 0, 0, 0, 0,
                                nullptr, nullptr, instance_, this);
        if (!hwnd_) return false;

        WNDCLASSEXW dashboardClass{};
        dashboardClass.cbSize = sizeof(dashboardClass);
        dashboardClass.style = CS_HREDRAW | CS_VREDRAW;
        dashboardClass.lpfnWndProc = TrayApp::dashboardProc;
        dashboardClass.hInstance = instance_;
        dashboardClass.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(1));
        dashboardClass.hIconSm = dashboardClass.hIcon;
        dashboardClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        dashboardClass.hbrBackground = nullptr;
        dashboardClass.lpszClassName = kDashboardClass;
        if (!RegisterClassExW(&dashboardClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

        DWORD style = WS_POPUP;
        DWORD extendedStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
        dashboard_ = CreateWindowExW(extendedStyle, kDashboardClass, kAppName, style,
                                     0, 0, kDashboardWidth, kDashboardHeight,
                                     nullptr, nullptr, instance_, this);
        if (dashboard_) {
            HRGN region = CreateRoundRectRgn(0, 0, kDashboardWidth + 1, kDashboardHeight + 1, 18, 18);
            SetWindowRgn(dashboard_, region, FALSE);
        }
        return dashboard_ != nullptr;
    }

    static void fillRoundedRect(Gdiplus::Graphics& graphics, const Gdiplus::RectF& rect,
                                float radius, const Gdiplus::Color& color) {
        Gdiplus::GraphicsPath path;
        float diameter = radius * 2.0f;
        path.AddArc(rect.X, rect.Y, diameter, diameter, 180, 90);
        path.AddArc(rect.GetRight() - diameter, rect.Y, diameter, diameter, 270, 90);
        path.AddArc(rect.GetRight() - diameter, rect.GetBottom() - diameter, diameter, diameter, 0, 90);
        path.AddArc(rect.X, rect.GetBottom() - diameter, diameter, diameter, 90, 90);
        path.CloseFigure();
        Gdiplus::SolidBrush brush(color);
        graphics.FillPath(&brush, &path);
    }

    static void drawCenteredText(Gdiplus::Graphics& graphics, const std::wstring& text,
                                 const Gdiplus::Font& font, const Gdiplus::RectF& rect,
                                 const Gdiplus::Color& color) {
        Gdiplus::StringFormat format;
        format.SetAlignment(Gdiplus::StringAlignmentCenter);
        format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        Gdiplus::SolidBrush brush(color);
        graphics.DrawString(text.c_str(), -1, &font, rect, &format, &brush);
    }

    void drawBatteryBadge(Gdiplus::Graphics& graphics, float x, float y, const wchar_t* name,
                          const std::wstring& value, const Gdiplus::Color& accent) {
        Gdiplus::RectF badge(x, y, 96.0f, 58.0f);
        fillRoundedRect(graphics, badge, 8.0f, Gdiplus::Color(244, 255, 255, 255));
        Gdiplus::Pen border(Gdiplus::Color(34, accent.GetR(), accent.GetG(), accent.GetB()), 1.0f);
        graphics.DrawRectangle(&border, badge.X, badge.Y, badge.Width, badge.Height);

        Gdiplus::Font labelFont(L"Yu Gothic UI", 12.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::Font valueFont(L"Segoe UI", 22.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        drawCenteredText(graphics, name, labelFont, Gdiplus::RectF(x, y + 5, 96, 18),
                         Gdiplus::Color(255, 86, 92, 101));
        drawCenteredText(graphics, value, valueFont, Gdiplus::RectF(x, y + 22, 96, 30), accent);
    }

    void drawGraphLine(Gdiplus::Graphics& graphics, const Gdiplus::RectF& chart,
                       long long start, long long end, int HistorySample::*member,
                       const Gdiplus::Color& color) {
        if (end <= start) return;
        Gdiplus::Pen pen(color, 2.4f);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        bool hasPrevious = false;
        Gdiplus::PointF previous;
        for (const auto& sample : history_) {
            if (sample.timestamp < start || sample.timestamp > end) continue;
            int value = sample.*member;
            if (value < 0) {
                hasPrevious = false;
                continue;
            }
            float x = chart.X + static_cast<float>(sample.timestamp - start) /
                                  static_cast<float>(end - start) * chart.Width;
            float y = chart.GetBottom() - static_cast<float>(value) / 100.0f * chart.Height;
            Gdiplus::PointF point(x, y);
            if (hasPrevious) graphics.DrawLine(&pen, previous, point);
            previous = point;
            hasPrevious = true;
        }
    }

    void paintDashboard(HWND hwnd) {
        PAINTSTRUCT paint{};
        HDC target = BeginPaint(hwnd, &paint);
        RECT client{};
        GetClientRect(hwnd, &client);
        int width = client.right;
        int height = client.bottom;
        HDC memory = CreateCompatibleDC(target);
        HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
        HBITMAP oldBitmap = static_cast<HBITMAP>(SelectObject(memory, bitmap));

        Gdiplus::Graphics graphics(memory);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.Clear(Gdiplus::Color(255, 246, 248, 250));

        Gdiplus::Font titleFont(L"Yu Gothic UI", 24.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::Font subtitleFont(L"Yu Gothic UI", 12.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush titleBrush(Gdiplus::Color(255, 29, 33, 38));
        Gdiplus::SolidBrush mutedBrush(Gdiplus::Color(255, 102, 110, 120));
        graphics.DrawString(L"INZONE Buds", -1, &titleFont, Gdiplus::PointF(24, 18), &titleBrush);
        std::wstring updated = L"最終更新: " + widen(state_.newestLocalTime());
        if (state_.newestLocalTime().empty()) updated = L"INZONE HUBのログを待っています";
        graphics.DrawString(updated.c_str(), -1, &subtitleFont, Gdiplus::PointF(26, 52), &mutedBrush);

        if (productImage_ && productImage_->GetLastStatus() == Gdiplus::Ok) {
            const float imageWidth = std::min(650.0f, static_cast<float>(width - 64));
            float imageHeight = imageWidth * productImage_->GetHeight() / productImage_->GetWidth();
            graphics.DrawImage(productImage_.get(), Gdiplus::RectF((width - imageWidth) / 2.0f, 72.0f,
                                                                    imageWidth, imageHeight));
        }

        drawBatteryBadge(graphics, 42, 262, L"L", state_.left.label(), Gdiplus::Color(255, 39, 143, 92));
        drawBatteryBadge(graphics, width / 2.0f - 48, 262, L"CASE", state_.caseBattery.label(),
                         Gdiplus::Color(255, 73, 82, 94));
        drawBatteryBadge(graphics, width - 138.0f, 262, L"R", state_.right.label(),
                         Gdiplus::Color(255, 206, 67, 67));

        Gdiplus::RectF graphCard(20, 340, static_cast<float>(width - 40), static_cast<float>(height - 360));
        fillRoundedRect(graphics, graphCard, 8.0f, Gdiplus::Color(255, 255, 255, 255));
        Gdiplus::Font graphTitleFont(L"Yu Gothic UI", 16.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        graphics.DrawString(L"バッテリー履歴", -1, &graphTitleFont, Gdiplus::PointF(40, 360), &titleBrush);

        Gdiplus::RectF dayTab(static_cast<float>(width - 210), 352, 82, 34);
        Gdiplus::RectF weekTab(static_cast<float>(width - 120), 352, 82, 34);
        fillRoundedRect(graphics, dayTab, 6.0f, showWeek_ ? Gdiplus::Color(255, 238, 241, 244)
                                                       : Gdiplus::Color(255, 36, 42, 49));
        fillRoundedRect(graphics, weekTab, 6.0f, showWeek_ ? Gdiplus::Color(255, 36, 42, 49)
                                                         : Gdiplus::Color(255, 238, 241, 244));
        Gdiplus::Font tabFont(L"Yu Gothic UI", 12.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        drawCenteredText(graphics, L"1日", tabFont, dayTab,
                         showWeek_ ? Gdiplus::Color(255, 66, 73, 82) : Gdiplus::Color(255, 255, 255, 255));
        drawCenteredText(graphics, L"1週間", tabFont, weekTab,
                         showWeek_ ? Gdiplus::Color(255, 255, 255, 255) : Gdiplus::Color(255, 66, 73, 82));

        Gdiplus::RectF chart(72, 410, static_cast<float>(width - 110), static_cast<float>(height - 492));
        Gdiplus::Font axisFont(L"Segoe UI", 10.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::Pen gridPen(Gdiplus::Color(255, 226, 230, 234), 1.0f);
        for (int value : {0, 25, 50, 75, 100}) {
            float y = chart.GetBottom() - value / 100.0f * chart.Height;
            graphics.DrawLine(&gridPen, chart.X, y, chart.GetRight(), y);
            drawCenteredText(graphics, std::to_wstring(value), axisFont,
                             Gdiplus::RectF(34, y - 8, 32, 16), Gdiplus::Color(255, 122, 130, 139));
        }

        if (!history_.empty()) {
            long long end = history_.back().timestamp;
            long long start = end - (showWeek_ ? 7LL * 24 * 60 * 60 : 24LL * 60 * 60);
            drawGraphLine(graphics, chart, start, end, &HistorySample::left, Gdiplus::Color(255, 39, 143, 92));
            drawGraphLine(graphics, chart, start, end, &HistorySample::right, Gdiplus::Color(255, 206, 67, 67));
            drawGraphLine(graphics, chart, start, end, &HistorySample::caseBattery, Gdiplus::Color(255, 73, 82, 94));

            std::wstring startText = formatGraphTime(start, showWeek_);
            std::wstring middleText = formatGraphTime(start + (end - start) / 2, showWeek_);
            std::wstring endText = formatGraphTime(end, showWeek_);
            drawCenteredText(graphics, startText, axisFont, Gdiplus::RectF(chart.X - 22, chart.GetBottom() + 5, 60, 18),
                             Gdiplus::Color(255, 122, 130, 139));
            drawCenteredText(graphics, middleText, axisFont,
                             Gdiplus::RectF(chart.X + chart.Width / 2 - 30, chart.GetBottom() + 5, 60, 18),
                             Gdiplus::Color(255, 122, 130, 139));
            drawCenteredText(graphics, endText, axisFont,
                             Gdiplus::RectF(chart.GetRight() - 38, chart.GetBottom() + 5, 76, 18),
                             Gdiplus::Color(255, 122, 130, 139));
        } else {
            Gdiplus::Font emptyFont(L"Yu Gothic UI", 13.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            drawCenteredText(graphics, L"表示できるバッテリー履歴がありません", emptyFont, chart,
                             Gdiplus::Color(255, 122, 130, 139));
        }

        Gdiplus::Font legendFont(L"Yu Gothic UI", 11.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        float legendY = static_cast<float>(height - 34);
        const wchar_t* legendNames[] = {L"L", L"R", L"CASE"};
        Gdiplus::Color legendColors[] = {Gdiplus::Color(255, 39, 143, 92), Gdiplus::Color(255, 206, 67, 67),
                                         Gdiplus::Color(255, 73, 82, 94)};
        float legendX = width / 2.0f - 105.0f;
        for (int i = 0; i < 3; ++i) {
            Gdiplus::SolidBrush dot(legendColors[i]);
            graphics.FillEllipse(&dot, legendX, legendY + 4.0f, 8.0f, 8.0f);
            graphics.DrawString(legendNames[i], -1, &legendFont, Gdiplus::PointF(legendX + 13, legendY), &mutedBrush);
            legendX += i == 2 ? 0.0f : 72.0f;
        }

        Gdiplus::Pen popupBorder(Gdiplus::Color(255, 201, 207, 214), 1.0f);
        Gdiplus::GraphicsPath borderPath;
        Gdiplus::RectF borderRect(0.5f, 0.5f, static_cast<float>(width - 1), static_cast<float>(height - 1));
        float borderDiameter = 18.0f;
        borderPath.AddArc(borderRect.X, borderRect.Y, borderDiameter, borderDiameter, 180, 90);
        borderPath.AddArc(borderRect.GetRight() - borderDiameter, borderRect.Y, borderDiameter, borderDiameter, 270, 90);
        borderPath.AddArc(borderRect.GetRight() - borderDiameter, borderRect.GetBottom() - borderDiameter,
                          borderDiameter, borderDiameter, 0, 90);
        borderPath.AddArc(borderRect.X, borderRect.GetBottom() - borderDiameter,
                          borderDiameter, borderDiameter, 90, 90);
        borderPath.CloseFigure();
        graphics.DrawPath(&popupBorder, &borderPath);

        BitBlt(target, 0, 0, width, height, memory, 0, 0, SRCCOPY);
        SelectObject(memory, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(memory);
        EndPaint(hwnd, &paint);
    }

    LRESULT handleDashboardMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        switch (msg) {
            case WM_ACTIVATE:
                if (LOWORD(wparam) == WA_INACTIVE && IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
                return 0;
            case WM_MOUSEACTIVATE:
                return MA_ACTIVATE;
            case WM_SIZE: {
                int width = LOWORD(lparam);
                int height = HIWORD(lparam);
                if (width > 0 && height > 0) {
                    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, 18, 18);
                    SetWindowRgn(hwnd, region, TRUE);
                }
                return 0;
            }
            case WM_PAINT:
                paintDashboard(hwnd);
                return 0;
            case WM_ERASEBKGND:
                return 1;
            case WM_LBUTTONUP: {
                int x = GET_X_LPARAM(lparam);
                int y = GET_Y_LPARAM(lparam);
                RECT client{};
                GetClientRect(hwnd, &client);
                if (y >= 352 && y <= 386) {
                    if (x >= client.right - 210 && x <= client.right - 128) showWeek_ = false;
                    if (x >= client.right - 120 && x <= client.right - 38) showWeek_ = true;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            case WM_KEYDOWN:
                if (wparam == VK_ESCAPE) ShowWindow(hwnd, SW_HIDE);
                return 0;
            case WM_CLOSE:
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            case WM_DESTROY:
                dashboard_ = nullptr;
                return 0;
        }
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        if (taskbarCreatedMessage_ != 0 && msg == taskbarCreatedMessage_) {
            addOrUpdateIcon(NIM_ADD);
            return 0;
        }
        switch (msg) {
            case WM_TIMER:
                refresh(false);
                if (dashboard_ && IsWindowVisible(dashboard_)) {
                    history_ = readBatteryHistory();
                    InvalidateRect(dashboard_, nullptr, FALSE);
                }
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
                if (dashboard_) DestroyWindow(dashboard_);
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
        recordBatteryHistory(state_);
        addOrUpdateIcon(NIM_MODIFY);
        if (show) showDetails();
    }

    void showDetails() {
        if (!dashboard_) return;
        refresh(false);
        history_ = readBatteryHistory();
        InvalidateRect(dashboard_, nullptr, FALSE);
        POINT cursor{};
        GetCursorPos(&cursor);
        HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo{sizeof(monitorInfo)};
        GetMonitorInfoW(monitor, &monitorInfo);
        int x = cursor.x - kDashboardWidth + 36;
        int y = monitorInfo.rcWork.bottom - kDashboardHeight - 10;
        int left = static_cast<int>(monitorInfo.rcWork.left) + 10;
        int right = static_cast<int>(monitorInfo.rcWork.right) - kDashboardWidth - 10;
        int top = static_cast<int>(monitorInfo.rcWork.top) + 10;
        x = std::clamp(x, left, right);
        y = std::max(y, top);
        SetWindowPos(dashboard_, HWND_TOPMOST, x, y, kDashboardWidth, kDashboardHeight,
                     SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
        SetForegroundWindow(dashboard_);
        SetFocus(dashboard_);
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

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR commandLine, int) {
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
    bool openDashboard = commandLine && wcsstr(commandLine, L"--dashboard") != nullptr;
    int result = app.run(instance, openDashboard);
    if (mutex) CloseHandle(mutex);
    return result;
}
