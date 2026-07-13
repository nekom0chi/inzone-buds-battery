#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <objidl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cstdlib>
#include <fstream>
#include <iomanip>
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
constexpr int kWhiteProductImageResource = 102;
constexpr int kPurpleProductImageResource = 103;
constexpr int kDashboardWidth = 460;
constexpr int kDashboardHeight = 680;

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
    std::string eqPreset = "Unknown";
    long long eqTimestamp = 0;
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

std::wstring settingsPath() {
    return historyDirectory() + L"\\settings.ini";
}

enum class ProductColor {
    Black,
    White,
    GlassPurple,
};

struct AppSettings {
    ProductColor productColor = ProductColor::Black;
    std::string eqPreset;
};

std::string productColorKey(ProductColor color) {
    if (color == ProductColor::White) return "white";
    if (color == ProductColor::GlassPurple) return "glass-purple";
    return "black";
}

AppSettings loadSettings() {
    AppSettings settings;
    std::ifstream file(settingsPath().c_str());
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t separator = line.find('=');
        if (separator == std::string::npos) continue;
        std::string key = line.substr(0, separator);
        std::string value = line.substr(separator + 1);
        if (key == "product_color") {
            if (value == "white") settings.productColor = ProductColor::White;
            else if (value == "glass-purple") settings.productColor = ProductColor::GlassPurple;
            else settings.productColor = ProductColor::Black;
        } else if (key == "eq_preset") {
            settings.eqPreset = value;
        }
    }
    return settings;
}

bool saveSettings(const AppSettings& settings) {
    CreateDirectoryW(historyDirectory().c_str(), nullptr);
    std::wstring temporary = settingsPath() + L".tmp";
    std::ofstream file(temporary.c_str(), std::ios::trunc);
    if (!file) return false;
    file << "product_color=" << productColorKey(settings.productColor) << '\n';
    file << "eq_preset=" << settings.eqPreset << '\n';
    file.close();
    return MoveFileExW(temporary.c_str(), settingsPath().c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

struct EqPresetDefinition {
    const char* key;
    const wchar_t* label;
    std::array<int, 10> gains;
    bool enabled;
};

const std::array<EqPresetDefinition, 6> kEqPresets = {{
    {"Flat", L"Flat", {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, false},
    {"Bass Boost", L"Bass Boost", {12, 12, 8, 0, 0, 0, 0, 0, 0, 0}, true},
    {"Music/Video", L"Music/Video", {3, 2, 2, 1, 0, -1, -2, -2, -9, -9}, true},
    {"FPS-1", L"FPS-1", {-6, -2, 2, 3, 2, 0, 2, 3, 1, -2}, true},
    {"FPS-2", L"FPS-2", {-6, -3, -1, 0, 1, 2, 3, 1, -1, -3}, true},
    {"FPS-3", L"FPS-3", {-6, -1, 1, 2, 1, 0, -1, -5, -9, -9}, true},
}};

const EqPresetDefinition* findEqPreset(const std::string& key) {
    auto found = std::find_if(kEqPresets.begin(), kEqPresets.end(),
                              [&key](const EqPresetDefinition& preset) { return key == preset.key; });
    return found == kEqPresets.end() ? nullptr : &*found;
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

std::wstring activeGameApoPath() {
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDeviceCollection* devices = nullptr;
    std::wstring result;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator)))) {
        return result;
    }
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) {
        UINT count = 0;
        devices->GetCount(&count);
        for (UINT i = 0; i < count && result.empty(); ++i) {
            IMMDevice* device = nullptr;
            IPropertyStore* properties = nullptr;
            LPWSTR endpointId = nullptr;
            PROPVARIANT friendlyName;
            PropVariantInit(&friendlyName);
            if (SUCCEEDED(devices->Item(i, &device)) &&
                SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties)) &&
                SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &friendlyName)) &&
                friendlyName.vt == VT_LPWSTR && friendlyName.pwszVal &&
                SUCCEEDED(device->GetId(&endpointId))) {
                std::wstring name(friendlyName.pwszVal);
                std::wstring id(endpointId);
                size_t guidStart = id.find_last_of(L'{');
                if (name.find(L"INZONE Buds") != std::wstring::npos &&
                    name.find(L"Game") != std::wstring::npos &&
                    guidStart != std::wstring::npos) {
                    std::wstring candidate = envPath(L"APPDATA") + L"\\Sony\\INZONE Hub\\APO\\" +
                                             id.substr(guidStart) + L".yaml";
                    if (fileExists(candidate)) result = candidate;
                }
            }
            if (endpointId) CoTaskMemFree(endpointId);
            PropVariantClear(&friendlyName);
            if (properties) properties->Release();
            if (device) device->Release();
        }
    }
    if (devices) devices->Release();
    enumerator->Release();
    return result;
}

struct EqCoefficients {
    double q = 1.0;
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
};

double qForEqGain(int gain) {
    static constexpr std::array<double, 13> values = {
        1.00, 1.35, 1.56, 1.80, 1.98, 2.19, 2.40, 2.55, 2.70, 2.85, 3.00, 3.10, 3.20,
    };
    return values[static_cast<size_t>(std::clamp(std::abs(gain), 0, 12))];
}

EqCoefficients calculateEqCoefficients(double frequency, int gain) {
    constexpr double sampleRate = 48000.0;
    constexpr double pi = 3.14159265358979323846;
    double q = qForEqGain(gain);
    double amplitude = std::pow(10.0, gain / 40.0);
    double omega = 2.0 * pi * frequency / sampleRate;
    double alpha = std::sin(omega) / (2.0 * q);
    double a0 = 1.0 + alpha / amplitude;
    EqCoefficients value;
    value.q = q;
    value.b0 = (1.0 + alpha * amplitude) / a0;
    value.b1 = (-2.0 * std::cos(omega)) / a0;
    value.b2 = (1.0 - alpha * amplitude) / a0;
    value.a1 = (-2.0 * std::cos(omega)) / a0;
    value.a2 = (1.0 - alpha / amplitude) / a0;
    return value;
}

std::string formatEqNumber(double value) {
    if (std::abs(value) < 0.0000000005) value = 0.0;
    std::ostringstream out;
    out << std::fixed << std::setprecision(8) << value;
    return out.str();
}

template <typename Getter>
std::string makeEqArray(const std::array<EqCoefficients, 10>& coefficients, Getter getter) {
    std::ostringstream out;
    out << '[';
    for (size_t i = 0; i < coefficients.size(); ++i) {
        if (i) out << ", ";
        out << formatEqNumber(getter(coefficients[i]));
    }
    out << ", 0, 0]";
    return out.str();
}

bool blockEnabled(const std::vector<std::string>& lines, const std::string& blockName) {
    bool inBlock = false;
    for (const auto& line : lines) {
        if (!line.empty() && line.front() != ' ' && line.back() == ':') {
            inBlock = line == blockName;
            continue;
        }
        if (inBlock && line.find("enable: true") != std::string::npos) return true;
        if (inBlock && line.find("enable: false") != std::string::npos) return false;
    }
    return false;
}

bool replaceYamlBlock(std::vector<std::string>& lines, const std::string& blockName,
                      const std::vector<std::string>& replacement) {
    auto start = std::find(lines.begin(), lines.end(), blockName);
    if (start == lines.end()) return false;
    auto end = start + 1;
    while (end != lines.end() && (end->empty() || end->front() == ' ' || end->front() == '\t')) ++end;
    lines.erase(start, end);
    lines.insert(start, replacement.begin(), replacement.end());
    return true;
}

std::vector<std::string> makeEqualizerYaml(const EqPresetDefinition& preset) {
    static constexpr std::array<double, 10> frequencies = {
        31.5, 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0,
    };
    std::array<EqCoefficients, 10> coefficients{};
    for (size_t i = 0; i < coefficients.size(); ++i) {
        coefficients[i] = calculateEqCoefficients(frequencies[i], preset.gains[i]);
    }
    std::string enables = preset.enabled ? "[1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0]"
                                         : "[0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]";
    std::ostringstream q;
    std::ostringstream gains;
    q << '[';
    gains << '[';
    for (size_t i = 0; i < coefficients.size(); ++i) {
        if (i) {
            q << ", ";
            gains << ", ";
        }
        q << std::fixed << std::setprecision(2) << coefficients[i].q;
        gains << preset.gains[i];
    }
    q << ", 0, 0]";
    gains << ", 0, 0]";
    return {
        "equalizer:",
        std::string("  enable: ") + (preset.enabled ? "true" : "false"),
        "",
        "  coeffs:",
        "    - enables: " + enables,
        "    - a1: " + makeEqArray(coefficients, [](const EqCoefficients& c) { return c.a1; }),
        "    - a2: " + makeEqArray(coefficients, [](const EqCoefficients& c) { return c.a2; }),
        "    - b0: " + makeEqArray(coefficients, [](const EqCoefficients& c) { return c.b0; }),
        "    - b1: " + makeEqArray(coefficients, [](const EqCoefficients& c) { return c.b1; }),
        "    - b2: " + makeEqArray(coefficients, [](const EqCoefficients& c) { return c.b2; }),
        "",
        "  params:",
        "    - enables: " + enables,
        "    - type: [2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 0, 0]",
        "    - fc: [31.5, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000, 0, 0]",
        "    - q: " + q.str(),
        "    - gain: " + gains.str(),
        "",
    };
}

bool applyEqPresetToApo(const EqPresetDefinition& preset, std::wstring& error) {
    std::wstring path = activeGameApoPath();
    if (path.empty() || !fileExists(path)) {
        error = L"INZONE Buds - Game のEQ設定が見つかりません。";
        return false;
    }
    std::ifstream input(path.c_str(), std::ios::binary);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    input.close();
    bool eqActive = preset.enabled || blockEnabled(lines, "mode_equalizer:");
    if (!replaceYamlBlock(lines, "equalizer:", makeEqualizerYaml(preset)) ||
        !replaceYamlBlock(lines, "amp1:", {"amp1:", std::string("    - enable: ") + (eqActive ? "true" : "false"), "    - gain: -18"}) ||
        !replaceYamlBlock(lines, "amp2:", {"amp2:", std::string("    - enable: ") + (eqActive ? "true" : "false"), "    - gain: 18"}) ||
        !replaceYamlBlock(lines, "alc:", {"alc:", std::string("    - enable: ") + (eqActive ? "true" : "false"),
                                          "    - threshold: -18", "    - ratio: 1000", "    - attackTime: 0.001", "    - releaseTime: 1.000"})) {
        error = L"EQ設定ファイルの形式を確認できませんでした。";
        return false;
    }
    std::wstring backup = path + L".inzone-battery.bak";
    CopyFileW(path.c_str(), backup.c_str(), TRUE);
    std::wstring temporary = path + L".tmp";
    std::ofstream output(temporary.c_str(), std::ios::binary | std::ios::trunc);
    if (!output) {
        error = L"EQ設定ファイルへ書き込めませんでした。";
        return false;
    }
    for (const auto& outputLine : lines) output << outputLine << "\r\n";
    output.close();
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        error = L"EQ設定の反映に失敗しました。";
        return false;
    }
    return true;
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

std::string readSoundProfilePreset() {
    std::wstring path = envPath(L"APPDATA") + L"\\Sony\\INZONE Hub\\SoundProfile.json";
    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file) return "";
    std::string json((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::string value = extractJsonString(json, "EQPreset");
    if (value == "FPS1") return "FPS-1";
    if (value == "FPS2") return "FPS-2";
    if (value == "FPS3") return "FPS-3";
    if (value == "BASS_BOOST" || value == "BASSBOOST") return "Bass Boost";
    if (value == "MUSIC_VIDEO" || value == "MUSICVIDEO") return "Music/Video";
    if (value == "FLAT") return "Flat";
    return "";
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
        if (line.find("batteryStatus") == std::string::npos && line.find("eqPreset") == std::string::npos) continue;
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
        } else if (item == "eqPreset" && timestamp >= state.eqTimestamp) {
            state.eqPreset = value.empty() ? "Unknown" : value;
            state.eqTimestamp = timestamp;
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

long long localDayStart(long long timestamp) {
    std::time_t value = static_cast<std::time_t>(timestamp);
    std::tm local{};
    localtime_s(&local, &value);
    local.tm_hour = 0;
    local.tm_min = 0;
    local.tm_sec = 0;
    local.tm_isdst = -1;
    return static_cast<long long>(std::mktime(&local));
}

long long addLocalDays(long long timestamp, int days) {
    std::time_t value = static_cast<std::time_t>(timestamp);
    std::tm local{};
    localtime_s(&local, &value);
    local.tm_mday += days;
    local.tm_isdst = -1;
    return static_cast<long long>(std::mktime(&local));
}

std::wstring formatLocalDate(long long timestamp, const wchar_t* format) {
    std::time_t value = static_cast<std::time_t>(timestamp);
    std::tm local{};
    localtime_s(&local, &value);
    wchar_t buffer[32]{};
    wcsftime(buffer, std::size(buffer), format, &local);
    return buffer;
}

struct BucketValues {
    int left = -1;
    int right = -1;
    int caseBattery = -1;
};

BucketValues latestValuesInBucket(const std::vector<HistorySample>& history, long long start, long long end) {
    BucketValues values;
    for (const auto& sample : history) {
        if (sample.timestamp < start || sample.timestamp >= end) continue;
        if (sample.left >= 0) values.left = sample.left;
        if (sample.right >= 0) values.right = sample.right;
        if (sample.caseBattery >= 0) values.caseBattery = sample.caseBattery;
    }
    return values;
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
        HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        comInitialized_ = SUCCEEDED(comResult);
        Gdiplus::GdiplusStartupInput startupInput;
        if (Gdiplus::GdiplusStartup(&gdiplusToken_, &startupInput, nullptr) != Gdiplus::Ok) return 1;
        productImages_[0] = loadPngResource(instance_, kProductImageResource);
        productImages_[1] = loadPngResource(instance_, kWhiteProductImageResource);
        productImages_[2] = loadPngResource(instance_, kPurpleProductImageResource);
        settings_ = loadSettings();
        pruneStoredHistory();
        state_ = readBatteryState();
        if (settings_.eqPreset.empty()) {
            std::string profilePreset = readSoundProfilePreset();
            if (findEqPreset(profilePreset)) settings_.eqPreset = profilePreset;
            else if (findEqPreset(state_.eqPreset)) settings_.eqPreset = state_.eqPreset;
        }
        if (!settings_.eqPreset.empty()) {
            saveSettings(settings_);
        }
        if (!settings_.eqPreset.empty()) state_.eqPreset = settings_.eqPreset;
        selectedDayStart_ = localDayStart(static_cast<long long>(std::time(nullptr)));
        recordBatteryHistory(state_);
        if (!createWindow()) {
            Gdiplus::GdiplusShutdown(gdiplusToken_);
            if (comInitialized_) CoUninitialize();
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
        for (auto& image : productImages_) image.reset();
        Gdiplus::GdiplusShutdown(gdiplusToken_);
        if (comInitialized_) CoUninitialize();
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
    std::array<std::unique_ptr<Gdiplus::Bitmap>, 3> productImages_;
    AppSettings settings_{};
    long long selectedDayStart_{};
    ULONG_PTR gdiplusToken_{};
    bool comInitialized_{};
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
                          const std::wstring& value, bool rightSide) {
        Gdiplus::RectF badge(x, y, 78.0f, 48.0f);
        fillRoundedRect(graphics, badge, 7.0f, Gdiplus::Color(255, 27, 32, 40));
        Gdiplus::Pen border(Gdiplus::Color(255, 47, 55, 66), 1.0f);
        graphics.DrawRectangle(&border, badge.X, badge.Y, badge.Width, badge.Height);
        Gdiplus::Font labelFont(L"Yu Gothic UI", 10.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        float valueSize = value.size() > 6 ? 10.0f : (value.size() > 4 ? 14.0f : 18.0f);
        Gdiplus::Font valueFont(L"Segoe UI", valueSize, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        drawCenteredText(graphics, name, labelFont, Gdiplus::RectF(x, y + 3, 78, 15),
                         rightSide ? Gdiplus::Color(255, 255, 91, 105) : Gdiplus::Color(255, 171, 180, 191));
        drawCenteredText(graphics, value, valueFont, Gdiplus::RectF(x, y + 17, 78, 26),
                         Gdiplus::Color(255, 247, 249, 252));
    }

    void drawRightText(Gdiplus::Graphics& graphics, const std::wstring& text, const Gdiplus::Font& font,
                       const Gdiplus::RectF& rect, const Gdiplus::Color& color) {
        Gdiplus::StringFormat format;
        format.SetAlignment(Gdiplus::StringAlignmentFar);
        format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        Gdiplus::SolidBrush brush(color);
        graphics.DrawString(text.c_str(), -1, &font, rect, &format, &brush);
    }

    void drawBarChart(Gdiplus::Graphics& graphics, const Gdiplus::RectF& chart,
                      const std::vector<BucketValues>& buckets, const std::vector<std::wstring>& labels,
                      bool week) {
        const Gdiplus::Color leftColor(255, 84, 211, 164);
        const Gdiplus::Color rightColor(255, 255, 91, 105);
        const Gdiplus::Color caseColor(255, 174, 139, 255);
        Gdiplus::Font axisFont(L"Segoe UI", 9.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::Pen gridPen(Gdiplus::Color(255, 48, 56, 67), 1.0f);
        for (int value : {0, 50, 100}) {
            float y = chart.GetBottom() - value / 100.0f * chart.Height;
            graphics.DrawLine(&gridPen, chart.X, y, chart.GetRight(), y);
            drawRightText(graphics, std::to_wstring(value), axisFont,
                          Gdiplus::RectF(14, y - 8, 24, 16), Gdiplus::Color(255, 119, 130, 143));
        }
        if (buckets.empty()) return;
        float groupWidth = chart.Width / static_cast<float>(buckets.size());
        float barWidth = week ? 8.0f : 4.5f;
        float gap = week ? 2.5f : 1.5f;
        auto drawBar = [&](float x, int value, const Gdiplus::Color& color) {
            if (value < 0) return;
            float barHeight = std::max(1.0f, chart.Height * std::clamp(value, 0, 100) / 100.0f);
            Gdiplus::SolidBrush brush(color);
            graphics.FillRectangle(&brush, x, chart.GetBottom() - barHeight, barWidth, barHeight);
        };
        for (size_t i = 0; i < buckets.size(); ++i) {
            float center = chart.X + groupWidth * (static_cast<float>(i) + 0.5f);
            float totalWidth = barWidth * 3.0f + gap * 2.0f;
            float x = center - totalWidth / 2.0f;
            drawBar(x, buckets[i].left, leftColor);
            drawBar(x + barWidth + gap, buckets[i].right, rightColor);
            drawBar(x + (barWidth + gap) * 2.0f, buckets[i].caseBattery, caseColor);
            if (i < labels.size() && (week || i % 2 == 0)) {
                drawCenteredText(graphics, labels[i], axisFont,
                                 Gdiplus::RectF(chart.X + groupWidth * static_cast<float>(i), chart.GetBottom() + 4,
                                                groupWidth, week ? 29.0f : 17.0f),
                                 Gdiplus::Color(255, 151, 161, 173));
            }
        }
        if (!week) {
            drawRightText(graphics, L"24", axisFont,
                          Gdiplus::RectF(chart.GetRight() - 18, chart.GetBottom() + 4, 22, 17),
                          Gdiplus::Color(255, 151, 161, 173));
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
        graphics.Clear(Gdiplus::Color(255, 13, 16, 21));

        Gdiplus::Font titleFont(L"Yu Gothic UI", 20.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::Font subtitleFont(L"Yu Gothic UI", 9.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush titleBrush(Gdiplus::Color(255, 247, 249, 252));
        Gdiplus::SolidBrush mutedBrush(Gdiplus::Color(255, 145, 155, 167));
        graphics.DrawString(L"INZONE Buds", -1, &titleFont, Gdiplus::PointF(18, 12), &titleBrush);
        std::wstring updated = L"最終更新: " + widen(state_.newestLocalTime());
        if (state_.newestLocalTime().empty()) updated = L"INZONE HUBのログを待っています";
        graphics.DrawString(updated.c_str(), -1, &subtitleFont, Gdiplus::PointF(20, 39), &mutedBrush);

        const float swatchCenters[] = {374.0f, 405.0f, 436.0f};
        const Gdiplus::Color swatchColors[] = {Gdiplus::Color(255, 25, 28, 34), Gdiplus::Color(255, 238, 240, 244),
                                               Gdiplus::Color(255, 111, 72, 166)};
        int selectedColor = static_cast<int>(settings_.productColor);
        for (int i = 0; i < 3; ++i) {
            Gdiplus::SolidBrush swatch(swatchColors[i]);
            graphics.FillEllipse(&swatch, swatchCenters[i] - 8.0f, 19.0f, 16.0f, 16.0f);
            Gdiplus::Pen outline(i == selectedColor ? Gdiplus::Color(255, 255, 255, 255)
                                                    : Gdiplus::Color(255, 82, 91, 103),
                                 i == selectedColor ? 2.0f : 1.0f);
            graphics.DrawEllipse(&outline, swatchCenters[i] - 9.0f, 18.0f, 18.0f, 18.0f);
        }

        Gdiplus::RectF productStage(12, 54, static_cast<float>(width - 24), 132);
        fillRoundedRect(graphics, productStage, 8.0f, Gdiplus::Color(255, 25, 30, 37));
        auto& productImage = productImages_[static_cast<size_t>(selectedColor)];
        if (productImage && productImage->GetLastStatus() == Gdiplus::Ok) {
            float scale = std::min(410.0f / productImage->GetWidth(), 124.0f / productImage->GetHeight());
            float imageWidth = productImage->GetWidth() * scale;
            float imageHeight = productImage->GetHeight() * scale;
            graphics.DrawImage(productImage.get(), Gdiplus::RectF((width - imageWidth) / 2.0f,
                                                                   58.0f + (124.0f - imageHeight) / 2.0f,
                                                                   imageWidth, imageHeight));
        }

        drawBatteryBadge(graphics, 24, 158, L"L", state_.left.label(), false);
        drawBatteryBadge(graphics, width / 2.0f - 39, 158, L"CASE", state_.caseBattery.label(), false);
        drawBatteryBadge(graphics, width - 102.0f, 158, L"R", state_.right.label(), true);

        Gdiplus::Font legendFont(L"Yu Gothic UI", 10.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        const wchar_t* legendNames[] = {L"L", L"R", L"CASE"};
        Gdiplus::Color legendColors[] = {Gdiplus::Color(255, 84, 211, 164), Gdiplus::Color(255, 255, 91, 105),
                                         Gdiplus::Color(255, 174, 139, 255)};
        float legendX = 151.0f;
        for (int i = 0; i < 3; ++i) {
            Gdiplus::SolidBrush dot(legendColors[i]);
            graphics.FillEllipse(&dot, legendX, 205.0f, 7.0f, 7.0f);
            graphics.DrawString(legendNames[i], -1, &legendFont, Gdiplus::PointF(legendX + 11, 200), &mutedBrush);
            legendX += i == 2 ? 0.0f : 62.0f;
        }

        Gdiplus::RectF eqButton(104, 219, static_cast<float>(width - 116), 36);
        fillRoundedRect(graphics, eqButton, 6.0f, Gdiplus::Color(255, 31, 37, 45));
        Gdiplus::Pen eqBorder(Gdiplus::Color(255, 58, 68, 81), 1.0f);
        graphics.DrawRectangle(&eqBorder, eqButton.X, eqButton.Y, eqButton.Width, eqButton.Height);
        Gdiplus::Font sectionFont(L"Yu Gothic UI", 12.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        graphics.DrawString(L"EQ", -1, &sectionFont, Gdiplus::PointF(22, 228), &mutedBrush);
        std::wstring eqLabel = widen(state_.eqPreset) + L"   \x25BE";
        drawCenteredText(graphics, eqLabel, sectionFont, eqButton, Gdiplus::Color(255, 247, 249, 252));

        long long today = localDayStart(static_cast<long long>(std::time(nullptr)));
        long long weekStart = addLocalDays(today, -7);
        std::vector<BucketValues> weekBuckets;
        std::vector<std::wstring> weekLabels;
        static const wchar_t* weekdays[] = {L"日", L"月", L"火", L"水", L"木", L"金", L"土"};
        for (int i = 0; i < 7; ++i) {
            long long start = addLocalDays(weekStart, i);
            long long end = addLocalDays(weekStart, i + 1);
            weekBuckets.push_back(latestValuesInBucket(history_, start, end));
            std::time_t dayTime = static_cast<std::time_t>(start);
            std::tm day{};
            localtime_s(&day, &dayTime);
            weekLabels.push_back(std::wstring(weekdays[day.tm_wday]) + L"\n" + formatLocalDate(start, L"%m/%d"));
        }

        Gdiplus::RectF weekCard(12, 266, static_cast<float>(width - 24), 178);
        fillRoundedRect(graphics, weekCard, 8.0f, Gdiplus::Color(255, 23, 28, 35));
        graphics.DrawString(L"WEEK", -1, &sectionFont, Gdiplus::PointF(24, 278), &titleBrush);
        Gdiplus::Font periodFont(L"Segoe UI", 9.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        drawRightText(graphics, formatLocalDate(weekStart, L"%m/%d") + L" - " + formatLocalDate(today, L"%m/%d"),
                      periodFont, Gdiplus::RectF(220, 276, 216, 22), Gdiplus::Color(255, 145, 155, 167));
        drawBarChart(graphics, Gdiplus::RectF(42, 307, static_cast<float>(width - 66), 103),
                     weekBuckets, weekLabels, true);

        std::vector<BucketValues> dayBuckets;
        std::vector<std::wstring> dayLabels;
        long long dayEnd = addLocalDays(selectedDayStart_, 1);
        double bucketSeconds = static_cast<double>(dayEnd - selectedDayStart_) / 12.0;
        for (int i = 0; i < 12; ++i) {
            long long start = selectedDayStart_ + static_cast<long long>(bucketSeconds * i);
            long long end = i == 11 ? dayEnd : selectedDayStart_ + static_cast<long long>(bucketSeconds * (i + 1));
            dayBuckets.push_back(latestValuesInBucket(history_, start, end));
            dayLabels.push_back(std::to_wstring(i * 2));
        }
        Gdiplus::RectF dayCard(12, 452, static_cast<float>(width - 24), 216);
        fillRoundedRect(graphics, dayCard, 8.0f, Gdiplus::Color(255, 23, 28, 35));
        graphics.DrawString(L"DAY", -1, &sectionFont, Gdiplus::PointF(24, 464), &titleBrush);
        drawRightText(graphics, formatLocalDate(selectedDayStart_, L"%Y/%m/%d"), periodFont,
                      Gdiplus::RectF(220, 462, 216, 22), Gdiplus::Color(255, 145, 155, 167));
        drawBarChart(graphics, Gdiplus::RectF(42, 493, static_cast<float>(width - 66), 132),
                     dayBuckets, dayLabels, false);

        Gdiplus::Pen popupBorder(Gdiplus::Color(255, 58, 67, 79), 1.0f);
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
                if (y >= 12 && y <= 42) {
                    const int centers[] = {374, 405, 436};
                    for (int i = 0; i < 3; ++i) {
                        if (std::abs(x - centers[i]) <= 13) {
                            settings_.productColor = static_cast<ProductColor>(i);
                            saveSettings(settings_);
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        }
                    }
                }
                if (x >= 104 && x <= client.right - 12 && y >= 219 && y <= 255) {
                    showEqMenu();
                    return 0;
                }
                if (x >= 42 && x <= client.right - 24 && y >= 307 && y <= 439) {
                    float groupWidth = static_cast<float>(client.right - 66) / 7.0f;
                    int index = std::clamp(static_cast<int>((x - 42) / groupWidth), 0, 6);
                    long long today = localDayStart(static_cast<long long>(std::time(nullptr)));
                    selectedDayStart_ = addLocalDays(today, index - 7);
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
        if (id >= 2100 && id < 2100 + kEqPresets.size()) applyEqPreset(id - 2100);
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
        if (!settings_.eqPreset.empty()) state_.eqPreset = settings_.eqPreset;
        recordBatteryHistory(state_);
        addOrUpdateIcon(NIM_MODIFY);
        if (show) showDetails();
    }

    void applyEqPreset(size_t index) {
        if (index >= kEqPresets.size()) return;
        std::wstring error;
        if (!applyEqPresetToApo(kEqPresets[index], error)) {
            MessageBoxW(dashboard_, error.c_str(), kAppName, MB_OK | MB_ICONERROR);
            return;
        }
        settings_.eqPreset = kEqPresets[index].key;
        saveSettings(settings_);
        state_.eqPreset = settings_.eqPreset;
        InvalidateRect(dashboard_, nullptr, FALSE);
    }

    void showEqMenu() {
        HMENU menu = CreatePopupMenu();
        for (size_t i = 0; i < kEqPresets.size(); ++i) {
            UINT flags = MF_STRING;
            if (settings_.eqPreset == kEqPresets[i].key) flags |= MF_CHECKED;
            AppendMenuW(menu, flags, 2100 + static_cast<UINT>(i), kEqPresets[i].label);
        }
        POINT point{104, 255};
        ClientToScreen(dashboard_, &point);
        SetForegroundWindow(dashboard_);
        UINT command = TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_NONOTIFY,
                                      point.x, point.y, 0, dashboard_, nullptr);
        DestroyMenu(menu);
        if (command) PostMessageW(hwnd_, WM_COMMAND, command, 0);
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
        int x = static_cast<int>(monitorInfo.rcWork.right) - kDashboardWidth - 12;
        int y = static_cast<int>(monitorInfo.rcWork.bottom) - kDashboardHeight - 12;
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
