#include <Windows.h>
#include <Mmdeviceapi.h>
#include <TlHelp32.h>
#include <commctrl.h>
#include <functiondiscoverykeys_devpkey.h>
#include <shellapi.h>
#include <ShlObj.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t WindowClassName[] = L"OsuLazerAudioBridge.Window";
constexpr UINT WM_APPEND_LOG = WM_APP + 1;
constexpr UINT WM_PROCESS_EXITED = WM_APP + 2;

enum ControlId {
    ProcessEditId = 1001,
    LogDirEditId = 1002,
    MirrorCheckId = 1003,
    VerboseCheckId = 1004,
    LogEnabledCheckId = 1005,
    StartButtonId = 1006,
    StopButtonId = 1007,
    TestToneButtonId = 1008,
    OpenLogsButtonId = 1009,
    ClearButtonId = 1010,
    OutputEditId = 1011,
    StatusStaticId = 1012,
    FindProcessButtonId = 1013,
    AudioPageButtonId = 1014,
    HookPageButtonId = 1015,
    ConsolePageButtonId = 1016,
    PageTitleStaticId = 1017,
    MirrorMusicCheckId = 1018,
    MusicModeHintStaticId = 1019,
    OutputBackendComboId = 1020,
    OutputDeviceComboId = 1021,
    OutputSampleRateComboId = 1022,
    OutputBufferComboId = 1023,
    EffectsVolumeComboId = 1024,
    MusicVolumeComboId = 1025,
    RefreshDevicesButtonId = 1026,
    AsioPanelButtonId = 1027,
    OpenSettingsButtonId = 1028,
    OutputChannelsComboId = 1029,
    DumpSamplesCheckId = 1030,
    DumpDirEditId = 1031,
    BrowseLogDirButtonId = 1032,
    BrowseDumpDirButtonId = 1033,
    OpenDumpDirButtonId = 1034,
    InjectHookCheckId = 1035,
    CopyCommandButtonId = 1036,
    CheckSetupButtonId = 1037,
    DecodeDumpButtonId = 1038,
    BrandStaticId = 1039,
    SidebarSubtitleStaticId = 1040,
    PageSubtitleStaticId = 1041,
    SectionOneStaticId = 1042,
    SectionTwoStaticId = 1043,
    SectionThreeStaticId = 1044,
    EffectsVolumeValueStaticId = 1045,
    MusicVolumeValueStaticId = 1046,
};

enum class UiPage {
    Audio,
    Hook,
    Console,
};

struct ChildProcess {
    PROCESS_INFORMATION process {};
    HANDLE stdoutRead = nullptr;
    HANDLE stdoutWrite = nullptr;
    HANDLE shutdownEvent = nullptr;
    std::wstring shutdownEventName;
    std::thread readerThread;
    std::atomic<bool> active = false;

    bool IsActive() const
    {
        return active.load();
    }
};

struct AudioDeviceItem {
    std::wstring id;
    std::wstring name;
};

HINSTANCE g_instance = nullptr;
HWND g_window = nullptr;
HWND g_processEdit = nullptr;
HWND g_logDirEdit = nullptr;
HWND g_mirrorCheck = nullptr;
HWND g_verboseCheck = nullptr;
HWND g_logEnabledCheck = nullptr;
HWND g_startButton = nullptr;
HWND g_stopButton = nullptr;
HWND g_testToneButton = nullptr;
HWND g_openLogsButton = nullptr;
HWND g_clearButton = nullptr;
HWND g_findProcessButton = nullptr;
HWND g_injectHookCheck = nullptr;
HWND g_copyCommandButton = nullptr;
HWND g_checkSetupButton = nullptr;
HWND g_outputEdit = nullptr;
HWND g_statusStatic = nullptr;
HWND g_brandStatic = nullptr;
HWND g_sidebarSubtitleStatic = nullptr;
HWND g_pageTitleStatic = nullptr;
HWND g_pageSubtitleStatic = nullptr;
HWND g_sectionOneStatic = nullptr;
HWND g_sectionTwoStatic = nullptr;
HWND g_sectionThreeStatic = nullptr;
HWND g_audioPageButton = nullptr;
HWND g_hookPageButton = nullptr;
HWND g_consolePageButton = nullptr;
HWND g_mirrorMusicCheck = nullptr;
HWND g_musicModeHintStatic = nullptr;
HWND g_outputBackendCombo = nullptr;
HWND g_outputDeviceCombo = nullptr;
HWND g_outputSampleRateCombo = nullptr;
HWND g_outputBufferCombo = nullptr;
HWND g_outputChannelsCombo = nullptr;
HWND g_effectsVolumeSlider = nullptr;
HWND g_musicVolumeSlider = nullptr;
HWND g_effectsVolumeValueStatic = nullptr;
HWND g_musicVolumeValueStatic = nullptr;
HWND g_refreshDevicesButton = nullptr;
HWND g_asioPanelButton = nullptr;
HWND g_openSettingsButton = nullptr;
HWND g_dumpSamplesCheck = nullptr;
HWND g_dumpDirEdit = nullptr;
HWND g_browseLogDirButton = nullptr;
HWND g_browseDumpDirButton = nullptr;
HWND g_openDumpDirButton = nullptr;
HWND g_decodeDumpButton = nullptr;
HFONT g_font = nullptr;
HFONT g_brandFont = nullptr;
HFONT g_titleFont = nullptr;
HFONT g_navFont = nullptr;
HFONT g_sectionFont = nullptr;
HFONT g_monoFont = nullptr;
HBRUSH g_canvasBrush = nullptr;
HBRUSH g_surfaceBrush = nullptr;
HBRUSH g_sidebarBrush = nullptr;
HBRUSH g_controlBrush = nullptr;
HBRUSH g_terminalBrush = nullptr;
ChildProcess g_child;
UiPage g_currentPage = UiPage::Hook;
std::uint64_t g_decodedCount = 0;
std::uint64_t g_playedCount = 0;
std::uint64_t g_missedCount = 0;
std::uint64_t g_stoppedCount = 0;
std::uint64_t g_inferredCount = 0;
std::vector<AudioDeviceItem> g_audioDevices;
std::wstring g_lastNumericBufferMs = L"10";

constexpr COLORREF ColorCanvas = RGB(241, 243, 246);
constexpr COLORREF ColorSurface = RGB(250, 251, 252);
constexpr COLORREF ColorControl = RGB(255, 255, 255);
constexpr COLORREF ColorSidebar = RGB(33, 35, 40);
constexpr COLORREF ColorSidebarRaised = RGB(45, 48, 55);
constexpr COLORREF ColorSidebarText = RGB(232, 236, 240);
constexpr COLORREF ColorSidebarMuted = RGB(155, 164, 176);
constexpr COLORREF ColorMutedText = RGB(104, 112, 124);
constexpr COLORREF ColorText = RGB(31, 35, 40);
constexpr COLORREF ColorAccent = RGB(31, 153, 137);
constexpr COLORREF ColorAccentPressed = RGB(25, 130, 117);
constexpr COLORREF ColorDanger = RGB(206, 77, 77);
constexpr COLORREF ColorDangerPressed = RGB(178, 58, 58);
constexpr COLORREF ColorButton = RGB(235, 238, 242);
constexpr COLORREF ColorButtonPressed = RGB(218, 223, 229);
constexpr COLORREF ColorBorder = RGB(216, 221, 228);
constexpr COLORREF ColorTerminal = RGB(18, 20, 24);
constexpr COLORREF ColorTerminalText = RGB(220, 226, 232);
constexpr int SidebarWidth = 226;
constexpr int WindowMargin = 18;

std::filesystem::path CurrentExeDirectory()
{
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    while (length == buffer.size()) {
        buffer.resize(buffer.size() * 2);
        length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    }

    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path DefaultLogDirectory()
{
    const std::filesystem::path exeDir = CurrentExeDirectory();
    const std::filesystem::path repoCandidate = exeDir.parent_path().parent_path();
    if (std::filesystem::exists(repoCandidate / L"CMakeLists.txt"))
        return repoCandidate / L"artifacts" / L"mirror-logs";

    return exeDir / L"logs";
}

std::filesystem::path DefaultDumpDirectory()
{
    const std::filesystem::path exeDir = CurrentExeDirectory();
    const std::filesystem::path repoCandidate = exeDir.parent_path().parent_path();
    if (std::filesystem::exists(repoCandidate / L"CMakeLists.txt"))
        return repoCandidate / L"artifacts" / L"sample-dumps";

    return exeDir / L"sample-dumps";
}

std::filesystem::path HostExecutablePath()
{
    return CurrentExeDirectory() / L"OsuLazerAudioHost.exe";
}

std::filesystem::path HookDllPath()
{
    return CurrentExeDirectory() / L"OsuLazerBassHook.dll";
}

std::filesystem::path SettingsPath()
{
    wchar_t localAppData[MAX_PATH] {};
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, static_cast<DWORD>(std::size(localAppData)));
    if (length > 0 && length < std::size(localAppData)) {
        std::filesystem::path directory = std::filesystem::path(localAppData) / L"OsuLazerAudioBridge";
        std::filesystem::create_directories(directory);
        return directory / L"settings.ini";
    }

    return CurrentExeDirectory() / L"settings.ini";
}

std::optional<std::wstring> FindRunningProcess(const std::vector<std::wstring>& processNames)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return std::nullopt;

    PROCESSENTRY32W entry {};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot, &entry)) {
        CloseHandle(snapshot);
        return std::nullopt;
    }

    do {
        for (const std::wstring& processName : processNames) {
            if (_wcsicmp(entry.szExeFile, processName.c_str()) == 0) {
                CloseHandle(snapshot);
                return processName;
            }
        }
    } while (Process32NextW(snapshot, &entry));

    CloseHandle(snapshot);
    return std::nullopt;
}

std::wstring PropVariantToWString(PROPVARIANT& value)
{
    if (value.vt == VT_LPWSTR && value.pwszVal != nullptr)
        return value.pwszVal;
    if (value.vt == VT_BSTR && value.bstrVal != nullptr)
        return value.bstrVal;
    return {};
}

std::wstring GetDeviceName(IMMDevice* device)
{
    if (device == nullptr)
        return L"(unknown)";

    ComPtr<IPropertyStore> properties;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &properties)))
        return L"(unknown)";

    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring name = L"(unknown)";
    if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value))) {
        const std::wstring parsed = PropVariantToWString(value);
        if (!parsed.empty())
            name = parsed;
    }
    PropVariantClear(&value);
    return name;
}

std::vector<AudioDeviceItem> EnumerateAudioDevices()
{
    std::vector<AudioDeviceItem> result;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coInitialized = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
        return result;

    ComPtr<IMMDeviceEnumerator> enumerator;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        if (coInitialized)
            CoUninitialize();
        return result;
    }

    ComPtr<IMMDeviceCollection> devices;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices);
    if (FAILED(hr)) {
        if (coInitialized)
            CoUninitialize();
        return result;
    }

    UINT count = 0;
    devices->GetCount(&count);
    for (UINT i = 0; i < count; i++) {
        ComPtr<IMMDevice> device;
        if (FAILED(devices->Item(i, &device)))
            continue;

        LPWSTR rawId = nullptr;
        if (FAILED(device->GetId(&rawId)) || rawId == nullptr)
            continue;

        result.push_back(AudioDeviceItem { rawId, GetDeviceName(device.Get()) });
        CoTaskMemFree(rawId);
    }

    if (coInitialized)
        CoUninitialize();
    return result;
}

std::vector<AudioDeviceItem> EnumerateAsioDevices()
{
    std::vector<AudioDeviceItem> result;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return result;

    DWORD index = 0;
    while (true) {
        wchar_t name[256] {};
        DWORD nameLength = static_cast<DWORD>(std::size(name));
        const LSTATUS status = RegEnumKeyExW(key, index++, name, &nameLength, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS)
            break;
        if (status != ERROR_SUCCESS)
            continue;

        result.push_back(AudioDeviceItem { name, name });
    }

    RegCloseKey(key);
    return result;
}

bool IsProcessRunning(const std::wstring& processName)
{
    if (processName.empty())
        return false;

    return FindRunningProcess({ processName }).has_value();
}

std::wstring GetWindowTextString(HWND handle)
{
    const int length = GetWindowTextLengthW(handle);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    if (length > 0)
        GetWindowTextW(handle, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(length));
    return text;
}

std::wstring QuoteArgument(const std::wstring& value)
{
    std::wstring result = L"\"";
    for (wchar_t ch : value) {
        if (ch == L'"')
            result += L'\\';
        result += ch;
    }
    result += L"\"";
    return result;
}

std::optional<std::wstring> BrowseForFolder(const wchar_t* title)
{
    const HRESULT coResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool coInitialized = SUCCEEDED(coResult);

    BROWSEINFOW browse {};
    browse.hwndOwner = g_window;
    browse.lpszTitle = title;
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;

    PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&browse);
    if (item == nullptr) {
        if (coInitialized)
            CoUninitialize();
        return std::nullopt;
    }

    wchar_t path[MAX_PATH] {};
    const BOOL ok = SHGetPathFromIDListW(item, path);
    CoTaskMemFree(item);
    if (coInitialized)
        CoUninitialize();
    if (!ok || path[0] == L'\0')
        return std::nullopt;

    return std::wstring(path);
}

std::wstring CurrentOutputBackend()
{
    if (g_outputBackendCombo == nullptr)
        return L"asio";

    const LRESULT index = SendMessageW(g_outputBackendCombo, CB_GETCURSEL, 0, 0);
    if (index == 1)
        return L"wasapi-exclusive";
    if (index == 2)
        return L"asio";

    return L"xaudio2";
}

std::wstring CurrentComboText(HWND combo)
{
    if (combo == nullptr)
        return {};

    const LRESULT index = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (index < 0)
        return {};

    wchar_t buffer[128] {};
    SendMessageW(combo, CB_GETLBTEXT, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(buffer));
    return buffer;
}

std::wstring CurrentOutputBufferArgument()
{
    if (CurrentOutputBackend() == L"asio")
        return L"0";

    const std::wstring selected = CurrentComboText(g_outputBufferCombo);
    return selected == L"Driver" || selected.empty() ? L"10" : selected;
}

std::wstring CurrentOutputBufferDisplay()
{
    return CurrentOutputBackend() == L"asio" ? L"driver" : CurrentOutputBufferArgument();
}

int ParseVolumePercent(const std::wstring& text, int fallback)
{
    if (text.empty())
        return fallback;

    wchar_t* end = nullptr;
    const long parsed = std::wcstol(text.c_str(), &end, 10);
    if (end == text.c_str())
        return fallback;

    return std::clamp(static_cast<int>(parsed), 0, 200);
}

std::wstring SliderValueText(HWND slider)
{
    if (slider == nullptr)
        return L"100";

    return std::to_wstring(static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0)));
}

void SetVolumeSlider(HWND slider, const std::wstring& value)
{
    if (slider == nullptr)
        return;

    SendMessageW(slider, TBM_SETPOS, TRUE, ParseVolumePercent(value, 100));
}

void UpdateVolumeSliderLabels()
{
    if (g_effectsVolumeValueStatic != nullptr) {
        const std::wstring text = SliderValueText(g_effectsVolumeSlider) + L"%";
        SetWindowTextW(g_effectsVolumeValueStatic, text.c_str());
    }

    if (g_musicVolumeValueStatic != nullptr) {
        const std::wstring text = SliderValueText(g_musicVolumeSlider) + L"%";
        SetWindowTextW(g_musicVolumeValueStatic, text.c_str());
    }
}

std::wstring CurrentOutputDeviceId()
{
    if (g_outputDeviceCombo == nullptr)
        return {};

    const LRESULT index = SendMessageW(g_outputDeviceCombo, CB_GETCURSEL, 0, 0);
    if (index <= 0)
        return {};

    const std::size_t deviceIndex = static_cast<std::size_t>(index - 1);
    if (deviceIndex >= g_audioDevices.size())
        return {};

    return g_audioDevices[deviceIndex].id;
}

void SelectOutputBackend(const std::wstring& backend)
{
    if (g_outputBackendCombo == nullptr)
        return;

    int index = 2;
    if (_wcsicmp(backend.c_str(), L"xaudio2") == 0)
        index = 0;
    else if (_wcsicmp(backend.c_str(), L"wasapi-exclusive") == 0 || _wcsicmp(backend.c_str(), L"wasapi") == 0)
        index = 1;
    else if (_wcsicmp(backend.c_str(), L"asio") == 0)
        index = 2;

    SendMessageW(g_outputBackendCombo, CB_SETCURSEL, index, 0);
}

void SelectComboText(HWND combo, const std::wstring& text, int fallbackIndex)
{
    if (combo == nullptr)
        return;

    const LRESULT count = SendMessageW(combo, CB_GETCOUNT, 0, 0);
    for (LRESULT i = 0; i < count; i++) {
        wchar_t buffer[128] {};
        SendMessageW(combo, CB_GETLBTEXT, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(buffer));
        if (_wcsicmp(buffer, text.c_str()) == 0) {
            SendMessageW(combo, CB_SETCURSEL, i, 0);
            return;
        }
    }

    SendMessageW(combo, CB_SETCURSEL, fallbackIndex, 0);
}

void PopulateOutputDeviceCombo(const std::wstring& selectedDeviceId)
{
    if (g_outputDeviceCombo == nullptr)
        return;

    SendMessageW(g_outputDeviceCombo, CB_RESETCONTENT, 0, 0);
    const bool asio = CurrentOutputBackend() == L"asio";
    SendMessageW(
        g_outputDeviceCombo,
        CB_ADDSTRING,
        0,
        reinterpret_cast<LPARAM>(asio ? L"Default ASIO driver" : L"Default Windows device"));
    g_audioDevices = asio ? EnumerateAsioDevices() : EnumerateAudioDevices();

    int selectedIndex = 0;
    for (std::size_t i = 0; i < g_audioDevices.size(); i++) {
        SendMessageW(g_outputDeviceCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(g_audioDevices[i].name.c_str()));
        if (!selectedDeviceId.empty() && g_audioDevices[i].id == selectedDeviceId)
            selectedIndex = static_cast<int>(i + 1);
    }

    SendMessageW(g_outputDeviceCombo, CB_SETCURSEL, selectedIndex, 0);
}

std::wstring BytesToWide(const std::string& bytes)
{
    if (bytes.empty())
        return {};

    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (length <= 0) {
        codePage = CP_ACP;
        flags = 0;
        length = MultiByteToWideChar(codePage, flags, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    }

    if (length <= 0)
        return L"<unreadable output>";

    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(codePage, flags, bytes.data(), static_cast<int>(bytes.size()), result.data(), length);
    return result;
}

std::optional<std::uint64_t> ParseUnsignedField(const std::wstring& text, const std::wstring& field)
{
    const std::wstring pattern = field + L"=";
    const std::size_t start = text.find(pattern);
    if (start == std::wstring::npos)
        return std::nullopt;

    const std::size_t valueStart = start + pattern.size();
    std::size_t valueEnd = valueStart;
    while (valueEnd < text.size() && text[valueEnd] >= L'0' && text[valueEnd] <= L'9')
        valueEnd++;

    if (valueEnd == valueStart)
        return std::nullopt;

    return std::wcstoull(text.substr(valueStart, valueEnd - valueStart).c_str(), nullptr, 10);
}

void SetStatus(const wchar_t* text)
{
    SetWindowTextW(g_statusStatic, text);
}

void UpdateStatsStatus()
{
    std::wstringstream status;
    status << L"Running\r\n"
           << L"decoded " << g_decodedCount
           << L"  played " << g_playedCount
           << L"\r\nmissed " << g_missedCount
           << L"  stopped " << g_stoppedCount;
    SetWindowTextW(g_statusStatic, status.str().c_str());
}

void ParseRuntimeStatus(const std::wstring& text)
{
    if (text.find(L"MirrorStats") != std::wstring::npos) {
        if (const auto value = ParseUnsignedField(text, L"decoded"))
            g_decodedCount = *value;
        if (const auto value = ParseUnsignedField(text, L"played"))
            g_playedCount = *value;
        if (const auto value = ParseUnsignedField(text, L"missed"))
            g_missedCount = *value;
        if (const auto value = ParseUnsignedField(text, L"stopped"))
            g_stoppedCount = *value;
        if (const auto value = ParseUnsignedField(text, L"inferred"))
            g_inferredCount = *value;
        UpdateStatsStatus();
        return;
    }

    if (text.find(L"MirrorAudio enabled.") != std::wstring::npos)
        SetStatus(L"Mirror enabled");
    else if (text.find(L"Injection completed.") != std::wstring::npos)
        SetStatus(L"Injected");
    else if (text.find(L"Process not found") != std::wstring::npos)
        SetStatus(L"Process not found");
    else if (text.find(L"AudioMirrorError") != std::wstring::npos)
        SetStatus(L"Audio error");
}

void AppendLog(const std::wstring& text)
{
    if (g_outputEdit == nullptr || text.empty())
        return;

    const int length = GetWindowTextLengthW(g_outputEdit);
    SendMessageW(g_outputEdit, EM_SETSEL, length, length);
    SendMessageW(g_outputEdit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
    ParseRuntimeStatus(text);
}

void PostLog(HWND window, const std::wstring& text)
{
    PostMessageW(window, WM_APPEND_LOG, 0, reinterpret_cast<LPARAM>(new std::wstring(text)));
}

void UpdateDumpSampleControls();

void UpdateButtons()
{
    const BOOL active = g_child.IsActive() ? TRUE : FALSE;
    const bool asio = CurrentOutputBackend() == L"asio";
    EnableWindow(g_startButton, !active);
    EnableWindow(g_testToneButton, !active);
    EnableWindow(g_findProcessButton, !active);
    EnableWindow(g_injectHookCheck, !active);
    EnableWindow(g_copyCommandButton, !active);
    EnableWindow(g_checkSetupButton, !active);
    EnableWindow(g_refreshDevicesButton, !active);
    EnableWindow(g_asioPanelButton, !active && asio);
    EnableWindow(g_browseLogDirButton, !active);
    EnableWindow(g_dumpSamplesCheck, !active);
    EnableWindow(g_decodeDumpButton, !active);
    EnableWindow(g_stopButton, active);
    UpdateDumpSampleControls();
}

void UpdateMusicModeHint()
{
    if (g_musicModeHintStatic == nullptr || g_mirrorMusicCheck == nullptr)
        return;

    const bool mirrorMusic = SendMessageW(g_mirrorMusicCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const std::wstring backend = CurrentOutputBackend();
    const bool asio = backend == L"asio";
    if (g_outputBufferCombo != nullptr) {
        const std::wstring selectedBuffer = CurrentComboText(g_outputBufferCombo);
        if (!asio && !selectedBuffer.empty() && selectedBuffer != L"Driver")
            g_lastNumericBufferMs = selectedBuffer;
        if (asio) {
            SelectComboText(g_outputBufferCombo, L"Driver", 0);
        } else if (selectedBuffer == L"Driver") {
            SelectComboText(g_outputBufferCombo, g_lastNumericBufferMs, 3);
        }
        EnableWindow(g_outputBufferCombo, asio ? FALSE : TRUE);
    }
    if (g_asioPanelButton != nullptr)
        EnableWindow(g_asioPanelButton, asio && !g_child.IsActive() ? TRUE : FALSE);
    HWND bufferLabel = GetDlgItem(g_window, OutputBufferComboId + 10000);
    if (bufferLabel != nullptr)
        SetWindowTextW(bufferLabel, asio ? L"Buffer (driver)" : L"Buffer");
    SetWindowTextW(
        g_musicModeHintStatic,
        asio
            ? (mirrorMusic
                ? L"ASIO music takeover on: set osu! itself to a silent/virtual device. ASIO buffer is controlled in the driver control panel, not by the ms box here."
                : L"ASIO effects only: use two audio devices. Keep osu! music on another output and send effects through the bridge. ASIO buffer is controlled by the driver.")
            : mirrorMusic
            ? L"Music takeover on: set osu! itself to a silent/virtual device. The bridge should be the only path to your real output, otherwise music will double and sound blurry."
                : backend == L"wasapi-exclusive"
                    ? L"Music takeover off: WASAPI Exclusive will only output effects here. Enable Mirror music for one-device playback, or keep osu! music on another device."
                : L"Music takeover off: use two audio devices. Let osu! send music to a silent/virtual device, and let the bridge output effects to your real device.");
}

void UpdateLogOutputControls()
{
    if (g_logEnabledCheck == nullptr || g_openLogsButton == nullptr)
        return;

    const bool logEnabled = SendMessageW(g_logEnabledCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    EnableWindow(g_openLogsButton, logEnabled);
}

void UpdateDumpSampleControls()
{
    if (g_dumpSamplesCheck == nullptr || g_dumpDirEdit == nullptr)
        return;

    const BOOL enabled = !g_child.IsActive() ? TRUE : FALSE;
    EnableWindow(g_dumpDirEdit, enabled);
    EnableWindow(g_browseDumpDirButton, enabled);
    EnableWindow(g_openDumpDirButton, enabled);
}

void SetVisible(HWND handle, bool visible)
{
    if (handle == nullptr)
        return;

    ShowWindow(handle, visible ? SW_SHOW : SW_HIDE);
}

const wchar_t* PageSettingsValue(UiPage page)
{
    switch (page) {
    case UiPage::Audio:
        return L"audio";
    case UiPage::Console:
        return L"console";
    case UiPage::Hook:
    default:
        return L"hook";
    }
}

UiPage ParsePageSettingsValue(const std::wstring& value)
{
    if (_wcsicmp(value.c_str(), L"audio") == 0)
        return UiPage::Audio;
    if (_wcsicmp(value.c_str(), L"console") == 0)
        return UiPage::Console;

    return UiPage::Hook;
}

void SelectPage(UiPage page)
{
    g_currentPage = page;
    const bool audio = page == UiPage::Audio;
    const bool hook = page == UiPage::Hook;
    const bool console = page == UiPage::Console;

    SetVisible(g_mirrorCheck, audio);
    SetVisible(g_mirrorMusicCheck, audio);
    SetVisible(g_musicModeHintStatic, audio);
    SetVisible(GetDlgItem(g_window, OutputBackendComboId + 10000), audio);
    SetVisible(g_outputBackendCombo, audio);
    SetVisible(GetDlgItem(g_window, OutputDeviceComboId + 10000), audio);
    SetVisible(g_outputDeviceCombo, audio);
    SetVisible(GetDlgItem(g_window, OutputSampleRateComboId + 10000), audio);
    SetVisible(g_outputSampleRateCombo, audio);
    SetVisible(GetDlgItem(g_window, OutputBufferComboId + 10000), audio);
    SetVisible(g_outputBufferCombo, audio);
    SetVisible(GetDlgItem(g_window, OutputChannelsComboId + 10000), audio);
    SetVisible(g_outputChannelsCombo, audio);
    SetVisible(GetDlgItem(g_window, EffectsVolumeComboId + 10000), audio);
    SetVisible(g_effectsVolumeSlider, audio);
    SetVisible(g_effectsVolumeValueStatic, audio);
    SetVisible(GetDlgItem(g_window, MusicVolumeComboId + 10000), audio);
    SetVisible(g_musicVolumeSlider, audio);
    SetVisible(g_musicVolumeValueStatic, audio);
    SetVisible(g_testToneButton, audio);
    SetVisible(g_refreshDevicesButton, audio);
    SetVisible(g_asioPanelButton, audio);

    SetVisible(GetDlgItem(g_window, ProcessEditId + 10000), hook);
    SetVisible(g_processEdit, hook);
    SetVisible(g_findProcessButton, hook);
    SetVisible(g_injectHookCheck, hook);
    SetVisible(g_startButton, hook);
    SetVisible(g_stopButton, hook);
    SetVisible(g_copyCommandButton, hook);
    SetVisible(g_checkSetupButton, hook);

    SetVisible(GetDlgItem(g_window, LogDirEditId + 10000), console);
    SetVisible(g_logDirEdit, console);
    SetVisible(g_browseLogDirButton, console);
    SetVisible(GetDlgItem(g_window, LogEnabledCheckId + 10000), console);
    SetVisible(g_logEnabledCheck, console);
    SetVisible(g_verboseCheck, console);
    SetVisible(g_dumpSamplesCheck, console);
    SetVisible(GetDlgItem(g_window, DumpDirEditId + 10000), console);
    SetVisible(g_dumpDirEdit, console);
    SetVisible(g_browseDumpDirButton, console);
    SetVisible(g_openDumpDirButton, console);
    SetVisible(g_decodeDumpButton, console);
    SetVisible(g_openSettingsButton, console);
    SetVisible(g_openLogsButton, console);
    SetVisible(g_clearButton, console);
    SetVisible(g_outputEdit, console);
    SetVisible(g_pageSubtitleStatic, true);
    SetVisible(g_sectionOneStatic, true);
    SetVisible(g_sectionTwoStatic, true);
    SetVisible(g_sectionThreeStatic, audio || console);
    UpdateLogOutputControls();
    UpdateDumpSampleControls();
    UpdateMusicModeHint();

    SendMessageW(g_audioPageButton, BM_SETCHECK, audio ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_hookPageButton, BM_SETCHECK, hook ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_consolePageButton, BM_SETCHECK, console ? BST_CHECKED : BST_UNCHECKED, 0);
    InvalidateRect(g_audioPageButton, nullptr, TRUE);
    InvalidateRect(g_hookPageButton, nullptr, TRUE);
    InvalidateRect(g_consolePageButton, nullptr, TRUE);

    const wchar_t* title = hook
        ? L"Game hook settings"
        : audio
            ? L"Audio output settings"
            : L"Debug console";
    SetWindowTextW(g_pageTitleStatic, title);
    SetWindowTextW(
        g_pageSubtitleStatic,
        hook
            ? L"Attach the native BASS hook and run the audio host listener."
            : audio
                ? L"Choose the bridge output path and decide whether music is taken over."
                : L"Inspect host output, logs, and captured sample data.");
    SetWindowTextW(g_sectionOneStatic, hook ? L"Target process" : audio ? L"Routing" : L"Logging");
    SetWindowTextW(g_sectionTwoStatic, hook ? L"Runtime controls" : audio ? L"Output backend" : L"Sample capture");
    SetWindowTextW(g_sectionThreeStatic, audio ? L"Latency and mix" : L"Host output");
    InvalidateRect(g_window, nullptr, TRUE);
}

void LoadSettings(
    std::wstring& processName,
    std::wstring& logDirectory,
    bool& mirrorAudio,
    bool& mirrorMusic,
    std::wstring& outputBackend,
    std::wstring& outputDeviceId,
    std::wstring& outputSampleRate,
    std::wstring& outputBufferMs,
    std::wstring& outputChannels,
    std::wstring& effectsVolume,
    std::wstring& musicVolume,
    bool& verboseEvents,
    bool& logEnabled,
    bool& dumpSamples,
    std::wstring& dumpDirectory,
    bool& injectHook,
    UiPage& currentPage)
{
    const std::wstring path = SettingsPath().wstring();
    wchar_t buffer[4096] {};

    GetPrivateProfileStringW(L"ui", L"process", L"osu!.exe", buffer, static_cast<DWORD>(std::size(buffer)), path.c_str());
    processName = buffer;

    GetPrivateProfileStringW(L"ui", L"logDir", DefaultLogDirectory().wstring().c_str(), buffer, static_cast<DWORD>(std::size(buffer)), path.c_str());
    logDirectory = buffer;

    mirrorAudio = GetPrivateProfileIntW(L"ui", L"mirrorAudio", 1, path.c_str()) != 0;
    mirrorMusic = GetPrivateProfileIntW(L"ui", L"mirrorMusic", 0, path.c_str()) != 0;
    GetPrivateProfileStringW(L"ui", L"outputBackend", L"asio", buffer, static_cast<DWORD>(std::size(buffer)), path.c_str());
    outputBackend = buffer;
    GetPrivateProfileStringW(L"ui", L"outputDeviceId", L"", buffer, static_cast<DWORD>(std::size(buffer)), path.c_str());
    outputDeviceId = buffer;
    GetPrivateProfileStringW(L"ui", L"outputSampleRate", L"48000", buffer, static_cast<DWORD>(std::size(buffer)), path.c_str());
    outputSampleRate = buffer;
    GetPrivateProfileStringW(L"ui", L"outputBufferMs", L"10", buffer, static_cast<DWORD>(std::size(buffer)), path.c_str());
    outputBufferMs = buffer;
    GetPrivateProfileStringW(L"ui", L"outputChannels", L"2", buffer, static_cast<DWORD>(std::size(buffer)), path.c_str());
    outputChannels = buffer;
    GetPrivateProfileStringW(L"ui", L"effectsVolume", L"100", buffer, static_cast<DWORD>(std::size(buffer)), path.c_str());
    effectsVolume = buffer;
    GetPrivateProfileStringW(L"ui", L"musicVolume", L"100", buffer, static_cast<DWORD>(std::size(buffer)), path.c_str());
    musicVolume = buffer;
    verboseEvents = GetPrivateProfileIntW(L"ui", L"verboseEvents", 0, path.c_str()) != 0;
    logEnabled = GetPrivateProfileIntW(L"ui", L"logEnabled", 0, path.c_str()) != 0;
    dumpSamples = GetPrivateProfileIntW(L"ui", L"dumpSamples", 0, path.c_str()) != 0;
    GetPrivateProfileStringW(L"ui", L"dumpDir", DefaultDumpDirectory().wstring().c_str(), buffer, static_cast<DWORD>(std::size(buffer)), path.c_str());
    dumpDirectory = buffer;
    injectHook = GetPrivateProfileIntW(L"ui", L"injectHook", 1, path.c_str()) != 0;
    GetPrivateProfileStringW(L"ui", L"page", L"hook", buffer, static_cast<DWORD>(std::size(buffer)), path.c_str());
    currentPage = ParsePageSettingsValue(buffer);
}

void SaveSettings()
{
    if (g_processEdit == nullptr || g_logDirEdit == nullptr)
        return;

    const std::wstring path = SettingsPath().wstring();
    const std::wstring processName = GetWindowTextString(g_processEdit);
    const std::wstring logDirectory = GetWindowTextString(g_logDirEdit);
    const bool mirrorAudio = SendMessageW(g_mirrorCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool mirrorMusic = SendMessageW(g_mirrorMusicCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool verboseEvents = SendMessageW(g_verboseCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const std::wstring outputBackend = CurrentOutputBackend();
    const std::wstring outputDeviceId = CurrentOutputDeviceId();
    const std::wstring outputSampleRate = CurrentComboText(g_outputSampleRateCombo);
    const std::wstring outputBufferMs = CurrentComboText(g_outputBufferCombo);
    const std::wstring outputChannels = CurrentComboText(g_outputChannelsCombo);
    const std::wstring effectsVolume = SliderValueText(g_effectsVolumeSlider);
    const std::wstring musicVolume = SliderValueText(g_musicVolumeSlider);
    const bool logEnabled = SendMessageW(g_logEnabledCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool dumpSamples = SendMessageW(g_dumpSamplesCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const std::wstring dumpDirectory = GetWindowTextString(g_dumpDirEdit);
    const bool injectHook = SendMessageW(g_injectHookCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;

    WritePrivateProfileStringW(L"ui", L"process", processName.c_str(), path.c_str());
    WritePrivateProfileStringW(L"ui", L"logDir", logDirectory.c_str(), path.c_str());
    WritePrivateProfileStringW(L"ui", L"mirrorAudio", mirrorAudio ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"ui", L"mirrorMusic", mirrorMusic ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"ui", L"outputBackend", outputBackend.c_str(), path.c_str());
    WritePrivateProfileStringW(L"ui", L"outputDeviceId", outputDeviceId.c_str(), path.c_str());
    WritePrivateProfileStringW(L"ui", L"outputSampleRate", outputSampleRate.c_str(), path.c_str());
    WritePrivateProfileStringW(L"ui", L"outputBufferMs", outputBufferMs.c_str(), path.c_str());
    WritePrivateProfileStringW(L"ui", L"outputChannels", outputChannels.c_str(), path.c_str());
    WritePrivateProfileStringW(L"ui", L"effectsVolume", effectsVolume.c_str(), path.c_str());
    WritePrivateProfileStringW(L"ui", L"musicVolume", musicVolume.c_str(), path.c_str());
    WritePrivateProfileStringW(L"ui", L"verboseEvents", verboseEvents ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"ui", L"logEnabled", logEnabled ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"ui", L"dumpSamples", dumpSamples ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"ui", L"dumpDir", dumpDirectory.c_str(), path.c_str());
    WritePrivateProfileStringW(L"ui", L"injectHook", injectHook ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"ui", L"page", PageSettingsValue(g_currentPage), path.c_str());
}

void CloseHandleIfSet(HANDLE& handle)
{
    if (handle != nullptr) {
        CloseHandle(handle);
        handle = nullptr;
    }
}

std::wstring MakeShutdownEventName()
{
    std::wstringstream stream;
    stream << L"Local\\OsuLazerAudioBridge.Shutdown." << GetCurrentProcessId() << L"." << GetTickCount64();
    return stream.str();
}

void CleanupChild()
{
    if (g_child.readerThread.joinable())
        g_child.readerThread.join();

    CloseHandleIfSet(g_child.stdoutRead);
    CloseHandleIfSet(g_child.stdoutWrite);
    CloseHandleIfSet(g_child.shutdownEvent);
    CloseHandleIfSet(g_child.process.hThread);
    CloseHandleIfSet(g_child.process.hProcess);
    g_child.process = {};
    g_child.shutdownEventName.clear();
    g_child.active = false;
}

void ReaderThread(HWND window, HANDLE readPipe, HANDLE processHandle)
{
    std::vector<char> buffer(4096);
    while (true) {
        DWORD bytesRead = 0;
        if (!ReadFile(readPipe, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr) || bytesRead == 0)
            break;

        PostLog(window, BytesToWide(std::string(buffer.data(), buffer.data() + bytesRead)));
    }

    WaitForSingleObject(processHandle, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(processHandle, &exitCode);

    std::wstringstream line;
    line << L"\r\nProcess exited with code " << exitCode << L".\r\n";
    PostLog(window, line.str());
    PostMessageW(window, WM_PROCESS_EXITED, exitCode, 0);
}

bool LaunchHost(const std::wstring& arguments)
{
    if (g_child.IsActive())
        return false;

    const std::filesystem::path hostPath = HostExecutablePath();
    if (!std::filesystem::exists(hostPath)) {
        MessageBoxW(g_window, (L"Host executable not found:\n" + hostPath.wstring()).c_str(), L"OsuLazerAudioBridge", MB_ICONERROR);
        return false;
    }

    SECURITY_ATTRIBUTES security {};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    if (!CreatePipe(&g_child.stdoutRead, &g_child.stdoutWrite, &security, 0)) {
        MessageBoxW(g_window, L"Failed to create stdout pipe.", L"OsuLazerAudioBridge", MB_ICONERROR);
        CleanupChild();
        return false;
    }

    SetHandleInformation(g_child.stdoutRead, HANDLE_FLAG_INHERIT, 0);

    g_child.shutdownEventName = MakeShutdownEventName();
    g_child.shutdownEvent = CreateEventW(nullptr, TRUE, FALSE, g_child.shutdownEventName.c_str());
    if (g_child.shutdownEvent == nullptr) {
        std::wstringstream error;
        error << L"Failed to create shutdown event. GetLastError=" << GetLastError();
        MessageBoxW(g_window, error.str().c_str(), L"OsuLazerAudioBridge", MB_ICONERROR);
        CleanupChild();
        return false;
    }

    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = g_child.stdoutWrite;
    startup.hStdError = g_child.stdoutWrite;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    std::wstring commandLine = QuoteArgument(hostPath.wstring()) + L" " + arguments
        + L" --shutdown-event " + QuoteArgument(g_child.shutdownEventName);
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    PROCESS_INFORMATION process {};
    const BOOL created = CreateProcessW(
        nullptr,
        mutableCommand.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
        nullptr,
        CurrentExeDirectory().c_str(),
        &startup,
        &process);

    CloseHandleIfSet(g_child.stdoutWrite);

    if (!created) {
        std::wstringstream error;
        error << L"Failed to start host. GetLastError=" << GetLastError();
        MessageBoxW(g_window, error.str().c_str(), L"OsuLazerAudioBridge", MB_ICONERROR);
        CleanupChild();
        return false;
    }

    g_child.process = process;
    g_child.active = true;
    g_decodedCount = 0;
    g_playedCount = 0;
    g_missedCount = 0;
    g_stoppedCount = 0;
    g_inferredCount = 0;
    g_child.readerThread = std::thread(ReaderThread, g_window, g_child.stdoutRead, g_child.process.hProcess);

    AppendLog(L"> " + commandLine + L"\r\n");
    SetStatus(L"Running");
    UpdateButtons();
    return true;
}

std::wstring BuildLogArguments();
std::wstring BuildOutputArguments();
std::wstring BuildStartArguments();

void StartBridge()
{
    const std::wstring processName = GetWindowTextString(g_processEdit);
    const std::wstring logDir = GetWindowTextString(g_logDirEdit);
    const std::wstring dumpDir = GetWindowTextString(g_dumpDirEdit);
    const bool logEnabled = SendMessageW(g_logEnabledCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool dumpSamples = SendMessageW(g_dumpSamplesCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool injectHook = SendMessageW(g_injectHookCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (injectHook && processName.empty()) {
        MessageBoxW(g_window, L"Process name is empty.", L"OsuLazerAudioBridge", MB_ICONWARNING);
        return;
    }
    if (logEnabled && logDir.empty()) {
        MessageBoxW(g_window, L"Log directory is empty.", L"OsuLazerAudioBridge", MB_ICONWARNING);
        return;
    }
    if (dumpSamples && dumpDir.empty()) {
        MessageBoxW(g_window, L"Sample dump directory is empty.", L"OsuLazerAudioBridge", MB_ICONWARNING);
        return;
    }

    if (injectHook && !IsProcessRunning(processName)) {
        const int result = MessageBoxW(
            g_window,
            (L"Process is not running yet:\n" + processName + L"\n\nStart the host anyway?").c_str(),
            L"OsuLazerAudioBridge",
            MB_ICONWARNING | MB_YESNO);
        if (result != IDYES)
            return;
    }

    SaveSettings();
    if (logEnabled)
        std::filesystem::create_directories(logDir);
    if (dumpSamples)
        std::filesystem::create_directories(dumpDir);
    if (SendMessageW(g_mirrorMusicCheck, BM_GETCHECK, 0, 0) != BST_CHECKED
        && CurrentOutputBackend() == L"wasapi-exclusive") {
        const int result = MessageBoxW(
            g_window,
            L"WASAPI Exclusive is selected, but Mirror music is off.\n\n"
            L"This mode will only bridge effects; music must come from another osu! output device.\n\n"
            L"Start anyway?",
            L"OsuLazerAudioBridge",
            MB_ICONWARNING | MB_YESNO);
        if (result != IDYES)
            return;
    }

    LaunchHost(BuildStartArguments());
}

void RunTestTone()
{
    const std::wstring logDir = GetWindowTextString(g_logDirEdit);
    const bool logEnabled = SendMessageW(g_logEnabledCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (logEnabled && logDir.empty()) {
        MessageBoxW(g_window, L"Log directory is empty.", L"OsuLazerAudioBridge", MB_ICONWARNING);
        return;
    }
    SaveSettings();
    std::wstring arguments = L"--test-tone";
    if (logEnabled)
        std::filesystem::create_directories(logDir);
    arguments += BuildLogArguments();
    arguments += BuildOutputArguments();
    LaunchHost(arguments);
}

void OpenAsioPanel()
{
    if (CurrentOutputBackend() != L"asio") {
        MessageBoxW(g_window, L"Select ASIO as the output backend first.", L"OsuLazerAudioBridge", MB_ICONINFORMATION);
        return;
    }

    SaveSettings();
    std::wstring arguments = L"--asio-control-panel --output-backend \"asio\" --no-log";
    if (!CurrentOutputDeviceId().empty())
        arguments += L" --output-device " + QuoteArgument(CurrentOutputDeviceId());
    LaunchHost(arguments);
}

void RefreshOutputDevices()
{
    const std::wstring selectedDeviceId = CurrentOutputDeviceId();
    PopulateOutputDeviceCombo(selectedDeviceId);
    UpdateMusicModeHint();
    SaveSettings();
    SetStatus(L"Devices refreshed");
}

std::wstring BuildLogArguments()
{
    const std::wstring logDir = GetWindowTextString(g_logDirEdit);
    const bool logEnabled = SendMessageW(g_logEnabledCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (!logEnabled)
        return L" --no-log";

    return L" --log-dir " + QuoteArgument(logDir);
}

std::wstring BuildOutputArguments()
{
    const std::wstring backend = CurrentOutputBackend();
    std::wstring arguments = L" --output-backend " + QuoteArgument(backend);
    if (!CurrentOutputDeviceId().empty())
        arguments += L" --output-device " + QuoteArgument(CurrentOutputDeviceId());
    arguments += L" --output-sample-rate " + QuoteArgument(CurrentComboText(g_outputSampleRateCombo));
    arguments += L" --output-channels " + QuoteArgument(CurrentComboText(g_outputChannelsCombo));
    arguments += L" --output-buffer-ms " + QuoteArgument(CurrentOutputBufferArgument());
    arguments += L" --effects-volume " + QuoteArgument(SliderValueText(g_effectsVolumeSlider));
    arguments += L" --music-volume " + QuoteArgument(SliderValueText(g_musicVolumeSlider));
    return arguments;
}

std::wstring BuildStartArguments()
{
    const std::wstring processName = GetWindowTextString(g_processEdit);
    const std::wstring dumpDir = GetWindowTextString(g_dumpDirEdit);
    const bool dumpSamples = SendMessageW(g_dumpSamplesCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool injectHook = SendMessageW(g_injectHookCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;

    std::wstring arguments = L"--process " + QuoteArgument(processName.empty() ? L"osu!.exe" : processName);
    if (!injectHook)
        arguments += L" --no-inject";
    arguments += BuildLogArguments();
    if (dumpSamples)
        arguments += L" --dump-samples " + QuoteArgument(dumpDir);
    if (SendMessageW(g_mirrorCheck, BM_GETCHECK, 0, 0) == BST_CHECKED)
        arguments += L" --mirror-audio";
    if (SendMessageW(g_mirrorMusicCheck, BM_GETCHECK, 0, 0) == BST_CHECKED)
        arguments += L" --mirror-music";
    arguments += BuildOutputArguments();
    if (SendMessageW(g_verboseCheck, BM_GETCHECK, 0, 0) == BST_CHECKED)
        arguments += L" --verbose";

    return arguments;
}

std::wstring BuildFullHostCommand(const std::wstring& arguments)
{
    return QuoteArgument(HostExecutablePath().wstring()) + L" " + arguments;
}

void CopyTextToClipboard(const std::wstring& text)
{
    if (!OpenClipboard(g_window))
        return;

    EmptyClipboard();
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory != nullptr) {
        void* data = GlobalLock(memory);
        if (data != nullptr) {
            std::memcpy(data, text.c_str(), bytes);
            GlobalUnlock(memory);
            if (SetClipboardData(CF_UNICODETEXT, memory) != nullptr)
                memory = nullptr;
        }
    }

    if (memory != nullptr)
        GlobalFree(memory);
    CloseClipboard();
}

void CopyStartCommand()
{
    SaveSettings();
    const std::wstring commandLine = BuildFullHostCommand(BuildStartArguments());
    CopyTextToClipboard(commandLine);
    AppendLog(L"> " + commandLine + L"\r\n");
    SetStatus(L"Command copied");
}

const wchar_t* OnOff(bool value)
{
    return value ? L"on" : L"off";
}

void CheckSetup()
{
    SaveSettings();

    const bool injectHook = SendMessageW(g_injectHookCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool logEnabled = SendMessageW(g_logEnabledCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool dumpSamples = SendMessageW(g_dumpSamplesCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool mirrorAudio = SendMessageW(g_mirrorCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool mirrorMusic = SendMessageW(g_mirrorMusicCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool verbose = SendMessageW(g_verboseCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const std::wstring processName = GetWindowTextString(g_processEdit);
    const std::wstring logDir = GetWindowTextString(g_logDirEdit);
    const std::wstring dumpDir = GetWindowTextString(g_dumpDirEdit);
    const std::filesystem::path hostPath = HostExecutablePath();
    const std::filesystem::path hookPath = HookDllPath();
    const bool hostExists = std::filesystem::exists(hostPath);
    const bool hookExists = std::filesystem::exists(hookPath);
    const bool processRunning = !processName.empty() && IsProcessRunning(processName);
    const std::wstring backend = CurrentOutputBackend();
    const std::wstring deviceName = CurrentComboText(g_outputDeviceCombo);
    const std::wstring commandLine = BuildFullHostCommand(BuildStartArguments());

    std::wstringstream report;
    report << L"\r\nSetup check\r\n";
    report << L"  Host: " << (hostExists ? L"OK" : L"MISSING") << L" \"" << hostPath.wstring() << L"\"\r\n";
    report << L"  Hook DLL: ";
    if (!injectHook)
        report << L"skipped because Inject hook is off";
    else
        report << (hookExists ? L"OK" : L"MISSING") << L" \"" << hookPath.wstring() << L"\"";
    report << L"\r\n";
    report << L"  Process: ";
    if (!injectHook)
        report << L"skipped because Inject hook is off";
    else if (processName.empty())
        report << L"missing process name";
    else
        report << (processRunning ? L"running " : L"not running ") << processName;
    report << L"\r\n";
    report << L"  Output: backend=" << backend
           << L" device=\"" << (deviceName.empty() ? L"(none)" : deviceName) << L"\""
           << L" rate=" << CurrentComboText(g_outputSampleRateCombo)
           << L" channels=" << CurrentComboText(g_outputChannelsCombo)
           << L" buffer=" << CurrentOutputBufferDisplay()
           << L"\r\n";
    report << L"  Mirror: effects=" << OnOff(mirrorAudio)
           << L" music=" << OnOff(mirrorMusic)
           << L" verbose=" << OnOff(verbose)
           << L"\r\n";
    report << L"  Logs: " << (logEnabled ? L"on " : L"off ");
    if (logEnabled)
        report << L"\"" << logDir << L"\"";
    report << L"\r\n";
    report << L"  Sample dumps: " << (dumpSamples ? L"on " : L"off ") << L"\"" << dumpDir << L"\"\r\n";
    report << L"  Command: " << commandLine << L"\r\n";

    AppendLog(report.str());
    SelectPage(UiPage::Console);
    SetStatus(hostExists && (!injectHook || hookExists) && (!injectHook || processRunning) ? L"Setup OK" : L"Check warnings");
}

void DecodeDumpDirectory()
{
    const std::wstring dumpDir = GetWindowTextString(g_dumpDirEdit);
    if (dumpDir.empty()) {
        MessageBoxW(g_window, L"Sample dump directory is empty.", L"OsuLazerAudioBridge", MB_ICONWARNING);
        return;
    }

    if (!std::filesystem::exists(dumpDir)) {
        MessageBoxW(g_window, (L"Sample dump directory does not exist:\n" + dumpDir).c_str(), L"OsuLazerAudioBridge", MB_ICONWARNING);
        return;
    }

    SaveSettings();
    SelectPage(UiPage::Console);
    LaunchHost(L"--decode-dir " + QuoteArgument(dumpDir) + L" --no-log");
}

void FindProcess()
{
    const std::optional<std::wstring> found = FindRunningProcess({ L"osu!.exe", L"osu.exe" });
    if (!found) {
        MessageBoxW(g_window, L"Could not find a running osu! process.", L"OsuLazerAudioBridge", MB_ICONINFORMATION);
        return;
    }

    SetWindowTextW(g_processEdit, found->c_str());
    SetStatus(L"Process found");
    SaveSettings();
}

void RequestChildShutdown(DWORD gracefulTimeoutMs, bool forceAfterTimeout)
{
    if (!g_child.IsActive())
        return;

    if (g_child.shutdownEvent != nullptr)
        SetEvent(g_child.shutdownEvent);

    if (g_child.process.hProcess == nullptr)
        return;

    const DWORD waitResult = WaitForSingleObject(g_child.process.hProcess, gracefulTimeoutMs);
    if (waitResult == WAIT_OBJECT_0)
        return;

    if (forceAfterTimeout) {
        AppendLog(L"Graceful shutdown timed out; forcing host process to exit.\r\n");
        TerminateProcess(g_child.process.hProcess, 1);
        WaitForSingleObject(g_child.process.hProcess, 1500);
    }
}

void StopBridge()
{
    if (!g_child.IsActive())
        return;

    AppendLog(L"Requesting graceful host shutdown...\r\n");
    SetStatus(L"Stopping");
    RequestChildShutdown(3000, true);
}

void OpenLogDirectory()
{
    const std::wstring logDir = GetWindowTextString(g_logDirEdit);
    std::filesystem::create_directories(logDir);
    SaveSettings();
    ShellExecuteW(g_window, L"open", logDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void BrowseLogDirectory()
{
    if (const auto selected = BrowseForFolder(L"Select log output directory")) {
        SetWindowTextW(g_logDirEdit, selected->c_str());
        SaveSettings();
    }
}

void BrowseDumpDirectory()
{
    if (const auto selected = BrowseForFolder(L"Select sample dump directory")) {
        SetWindowTextW(g_dumpDirEdit, selected->c_str());
        SaveSettings();
    }
}

void OpenDumpDirectory()
{
    const std::wstring dumpDir = GetWindowTextString(g_dumpDirEdit);
    std::filesystem::create_directories(dumpDir);
    SaveSettings();
    ShellExecuteW(g_window, L"open", dumpDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void OpenSettingsFile()
{
    SaveSettings();
    const std::filesystem::path path = SettingsPath();
    ShellExecuteW(g_window, L"open", path.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

HFONT CreateUiFont(int pixelHeight, int weight, const wchar_t* face)
{
    return CreateFontW(
        -pixelHeight,
        0,
        0,
        0,
        weight,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH,
        face);
}

template <typename T>
void DeleteGdiObjectIfSet(T& object)
{
    if (object != nullptr) {
        DeleteObject(object);
        object = nullptr;
    }
}

HWND CreateControl(
    const wchar_t* className,
    const wchar_t* text,
    DWORD style,
    int id)
{
    HWND handle = CreateWindowExW(
        0,
        className,
        text,
        WS_CHILD | WS_VISIBLE | style,
        0,
        0,
        10,
        10,
        g_window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        g_instance,
        nullptr);
    SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    return handle;
}

void DrawRoundedRect(HDC dc, const RECT& rect, COLORREF fill, COLORREF border, int radius);

void LayoutControls(int width, int height)
{
    constexpr int margin = WindowMargin;
    constexpr int navWidth = SidebarWidth - WindowMargin * 2;
    constexpr int labelWidth = 92;
    constexpr int rowHeight = 26;
    constexpr int gap = 8;
    constexpr int navButtonHeight = 48;
    constexpr int buttonWidth = 116;
    constexpr int buttonHeight = 30;
    constexpr int findButtonWidth = 92;
    constexpr int sectionHeight = 22;
    constexpr int sectionGap = 26;

    MoveWindow(g_brandStatic, margin, margin, navWidth, 26, TRUE);
    MoveWindow(g_sidebarSubtitleStatic, margin, margin + 30, navWidth, 22, TRUE);

    int navY = margin + 72;
    MoveWindow(g_audioPageButton, margin, navY, navWidth, navButtonHeight, TRUE);
    navY += navButtonHeight + gap;
    MoveWindow(g_hookPageButton, margin, navY, navWidth, navButtonHeight, TRUE);
    navY += navButtonHeight + gap;
    MoveWindow(g_consolePageButton, margin, navY, navWidth, navButtonHeight, TRUE);
    MoveWindow(g_statusStatic, margin, height - margin - 68, navWidth, 68, TRUE);

    const int contentX = SidebarWidth + WindowMargin;
    int contentWidth = width - contentX - WindowMargin;
    if (contentWidth < 520)
        contentWidth = 520;
    int y = margin;
    MoveWindow(g_pageTitleStatic, contentX, y, contentWidth, 32, TRUE);
    MoveWindow(g_pageSubtitleStatic, contentX, y + 34, contentWidth, 24, TRUE);
    y += 80;

    MoveWindow(g_sectionOneStatic, contentX, y, contentWidth, sectionHeight, TRUE);
    int sectionOneRow = y + sectionGap;
    MoveWindow(g_mirrorCheck, contentX, sectionOneRow, 160, rowHeight, TRUE);
    MoveWindow(g_mirrorMusicCheck, contentX + 180, sectionOneRow, 220, rowHeight, TRUE);
    MoveWindow(GetDlgItem(g_window, ProcessEditId + 10000), contentX, sectionOneRow + 4, labelWidth, rowHeight, TRUE);
    MoveWindow(g_processEdit, contentX + labelWidth, sectionOneRow, contentWidth - labelWidth - findButtonWidth - gap, rowHeight, TRUE);
    MoveWindow(g_findProcessButton, contentX + contentWidth - findButtonWidth, sectionOneRow, findButtonWidth, rowHeight, TRUE);
    MoveWindow(GetDlgItem(g_window, LogDirEditId + 10000), contentX, sectionOneRow + 4, labelWidth, rowHeight, TRUE);
    MoveWindow(g_logDirEdit, contentX + labelWidth, sectionOneRow, contentWidth - labelWidth - 88, rowHeight, TRUE);
    MoveWindow(g_browseLogDirButton, contentX + contentWidth - 80, sectionOneRow, 80, rowHeight, TRUE);
    MoveWindow(g_logEnabledCheck, contentX, sectionOneRow + rowHeight + gap, 190, rowHeight, TRUE);
    MoveWindow(g_verboseCheck, contentX + 210, sectionOneRow + rowHeight + gap, 190, rowHeight, TRUE);

    int sectionTwoY = sectionOneRow + rowHeight + 34;
    MoveWindow(g_sectionTwoStatic, contentX, sectionTwoY, contentWidth, sectionHeight, TRUE);
    int sectionTwoRow = sectionTwoY + sectionGap;
    MoveWindow(GetDlgItem(g_window, OutputBackendComboId + 10000), contentX, sectionTwoRow + 4, labelWidth, rowHeight, TRUE);
    MoveWindow(g_outputBackendCombo, contentX + labelWidth, sectionTwoRow, 220, 120, TRUE);
    MoveWindow(GetDlgItem(g_window, OutputDeviceComboId + 10000), contentX, sectionTwoRow + rowHeight + gap + 4, labelWidth, rowHeight, TRUE);
    MoveWindow(g_outputDeviceCombo, contentX + labelWidth, sectionTwoRow + rowHeight + gap, contentWidth - labelWidth, 180, TRUE);
    MoveWindow(g_injectHookCheck, contentX, sectionTwoRow, 180, rowHeight, TRUE);
    MoveWindow(g_startButton, contentX, sectionTwoRow + rowHeight + gap, buttonWidth, buttonHeight, TRUE);
    MoveWindow(g_stopButton, contentX + buttonWidth + gap, sectionTwoRow + rowHeight + gap, buttonWidth, buttonHeight, TRUE);
    MoveWindow(g_copyCommandButton, contentX + (buttonWidth + gap) * 2, sectionTwoRow + rowHeight + gap, buttonWidth + 8, buttonHeight, TRUE);
    MoveWindow(g_checkSetupButton, contentX + (buttonWidth + gap) * 3 + 8, sectionTwoRow + rowHeight + gap, buttonWidth, buttonHeight, TRUE);
    MoveWindow(g_dumpSamplesCheck, contentX, sectionTwoRow, 190, rowHeight, TRUE);
    MoveWindow(GetDlgItem(g_window, DumpDirEditId + 10000), contentX, sectionTwoRow + rowHeight + gap + 4, labelWidth, rowHeight, TRUE);
    MoveWindow(g_dumpDirEdit, contentX + labelWidth, sectionTwoRow + rowHeight + gap, contentWidth - labelWidth - 180, rowHeight, TRUE);
    MoveWindow(g_browseDumpDirButton, contentX + contentWidth - 172, sectionTwoRow + rowHeight + gap, 80, rowHeight, TRUE);
    MoveWindow(g_openDumpDirButton, contentX + contentWidth - 84, sectionTwoRow + rowHeight + gap, 84, rowHeight, TRUE);

    int sectionThreeY = sectionTwoRow + (rowHeight + gap) * 2 + 24;
    MoveWindow(g_sectionThreeStatic, contentX, sectionThreeY, contentWidth, sectionHeight, TRUE);
    int sectionThreeRow = sectionThreeY + sectionGap;
    MoveWindow(GetDlgItem(g_window, OutputSampleRateComboId + 10000), contentX, sectionThreeRow + 4, labelWidth, rowHeight, TRUE);
    MoveWindow(g_outputSampleRateCombo, contentX + labelWidth, sectionThreeRow, 120, 120, TRUE);
    MoveWindow(GetDlgItem(g_window, OutputChannelsComboId + 10000), contentX + labelWidth + 140, sectionThreeRow + 4, 76, rowHeight, TRUE);
    MoveWindow(g_outputChannelsCombo, contentX + labelWidth + 216, sectionThreeRow, 80, 120, TRUE);
    MoveWindow(GetDlgItem(g_window, OutputBufferComboId + 10000), contentX + labelWidth + 316, sectionThreeRow + 4, 96, rowHeight, TRUE);
    MoveWindow(g_outputBufferCombo, contentX + labelWidth + 408, sectionThreeRow, 90, 120, TRUE);
    MoveWindow(GetDlgItem(g_window, EffectsVolumeComboId + 10000), contentX, sectionThreeRow + rowHeight + gap + 4, labelWidth, rowHeight, TRUE);
    MoveWindow(g_effectsVolumeSlider, contentX + labelWidth, sectionThreeRow + rowHeight + gap - 2, contentWidth - labelWidth - 64, rowHeight + 6, TRUE);
    MoveWindow(g_effectsVolumeValueStatic, contentX + contentWidth - 56, sectionThreeRow + rowHeight + gap + 4, 56, rowHeight, TRUE);
    MoveWindow(GetDlgItem(g_window, MusicVolumeComboId + 10000), contentX, sectionThreeRow + (rowHeight + gap) * 2 + 4, labelWidth, rowHeight, TRUE);
    MoveWindow(g_musicVolumeSlider, contentX + labelWidth, sectionThreeRow + (rowHeight + gap) * 2 - 2, contentWidth - labelWidth - 64, rowHeight + 6, TRUE);
    MoveWindow(g_musicVolumeValueStatic, contentX + contentWidth - 56, sectionThreeRow + (rowHeight + gap) * 2 + 4, 56, rowHeight, TRUE);
    MoveWindow(g_musicModeHintStatic, contentX, sectionThreeRow + (rowHeight + gap) * 3, contentWidth, 64, TRUE);
    MoveWindow(g_testToneButton, contentX, sectionThreeRow + (rowHeight + gap) * 3 + 72, buttonWidth, buttonHeight, TRUE);
    MoveWindow(g_refreshDevicesButton, contentX + buttonWidth + gap, sectionThreeRow + (rowHeight + gap) * 3 + 72, buttonWidth + 18, buttonHeight, TRUE);
    MoveWindow(g_asioPanelButton, contentX + (buttonWidth + gap) * 2 + 18, sectionThreeRow + (rowHeight + gap) * 3 + 72, buttonWidth + 6, buttonHeight, TRUE);
    MoveWindow(g_openSettingsButton, contentX, sectionThreeRow, buttonWidth, buttonHeight, TRUE);
    MoveWindow(g_openLogsButton, contentX + buttonWidth + gap, sectionThreeRow, buttonWidth, buttonHeight, TRUE);
    MoveWindow(g_clearButton, contentX + (buttonWidth + gap) * 2, sectionThreeRow, buttonWidth, buttonHeight, TRUE);
    MoveWindow(g_decodeDumpButton, contentX + (buttonWidth + gap) * 3, sectionThreeRow, buttonWidth, buttonHeight, TRUE);
    MoveWindow(g_outputEdit, contentX, sectionThreeRow + buttonHeight + margin, contentWidth, height - (sectionThreeRow + buttonHeight + margin) - margin, TRUE);

    SelectPage(g_currentPage);
}

void CreateUi(HWND window)
{
    g_window = window;
    g_font = CreateUiFont(15, FW_NORMAL, L"Segoe UI");
    g_brandFont = CreateUiFont(18, FW_SEMIBOLD, L"Segoe UI");
    g_titleFont = CreateUiFont(21, FW_SEMIBOLD, L"Segoe UI");
    g_navFont = CreateUiFont(16, FW_SEMIBOLD, L"Segoe UI");
    g_sectionFont = CreateUiFont(15, FW_SEMIBOLD, L"Segoe UI");
    g_monoFont = CreateUiFont(14, FW_NORMAL, L"Consolas");
    g_canvasBrush = CreateSolidBrush(ColorCanvas);
    g_surfaceBrush = CreateSolidBrush(ColorSurface);
    g_sidebarBrush = CreateSolidBrush(ColorSidebar);
    g_controlBrush = CreateSolidBrush(ColorControl);
    g_terminalBrush = CreateSolidBrush(ColorTerminal);

    g_brandStatic = CreateControl(L"STATIC", L"OsuLazerAudioBridge", SS_LEFT | SS_ENDELLIPSIS, BrandStaticId);
    g_sidebarSubtitleStatic = CreateControl(L"STATIC", L"Native audio bridge", SS_LEFT | SS_ENDELLIPSIS, SidebarSubtitleStaticId);
    SendMessageW(g_brandStatic, WM_SETFONT, reinterpret_cast<WPARAM>(g_brandFont), TRUE);

    CreateControl(L"STATIC", L"Process", 0, ProcessEditId + 10000);
    std::wstring processName;
    std::wstring logDirectory;
    std::wstring outputBackend;
    std::wstring outputDeviceId;
    std::wstring outputSampleRate;
    std::wstring outputBufferMs;
    std::wstring outputChannels;
    std::wstring effectsVolume;
    std::wstring musicVolume;
    std::wstring dumpDirectory;
    bool mirrorAudio = true;
    bool mirrorMusic = false;
    bool verboseEvents = false;
    bool logEnabled = false;
    bool dumpSamples = false;
    bool injectHook = true;
    LoadSettings(processName, logDirectory, mirrorAudio, mirrorMusic, outputBackend, outputDeviceId, outputSampleRate, outputBufferMs, outputChannels, effectsVolume, musicVolume, verboseEvents, logEnabled, dumpSamples, dumpDirectory, injectHook, g_currentPage);

    g_audioPageButton = CreateControl(L"BUTTON", L"Audio output", BS_AUTORADIOBUTTON | BS_PUSHLIKE | BS_OWNERDRAW, AudioPageButtonId);
    g_hookPageButton = CreateControl(L"BUTTON", L"Game hook", BS_AUTORADIOBUTTON | BS_PUSHLIKE | BS_OWNERDRAW, HookPageButtonId);
    g_consolePageButton = CreateControl(L"BUTTON", L"Debug console", BS_AUTORADIOBUTTON | BS_PUSHLIKE | BS_OWNERDRAW, ConsolePageButtonId);
    g_pageTitleStatic = CreateControl(L"STATIC", L"Game hook settings", 0, PageTitleStaticId);
    g_pageSubtitleStatic = CreateControl(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, PageSubtitleStaticId);
    g_sectionOneStatic = CreateControl(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, SectionOneStaticId);
    g_sectionTwoStatic = CreateControl(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, SectionTwoStaticId);
    g_sectionThreeStatic = CreateControl(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, SectionThreeStaticId);
    SendMessageW(g_audioPageButton, WM_SETFONT, reinterpret_cast<WPARAM>(g_navFont), TRUE);
    SendMessageW(g_hookPageButton, WM_SETFONT, reinterpret_cast<WPARAM>(g_navFont), TRUE);
    SendMessageW(g_consolePageButton, WM_SETFONT, reinterpret_cast<WPARAM>(g_navFont), TRUE);
    SendMessageW(g_pageTitleStatic, WM_SETFONT, reinterpret_cast<WPARAM>(g_titleFont), TRUE);
    SendMessageW(g_sectionOneStatic, WM_SETFONT, reinterpret_cast<WPARAM>(g_sectionFont), TRUE);
    SendMessageW(g_sectionTwoStatic, WM_SETFONT, reinterpret_cast<WPARAM>(g_sectionFont), TRUE);
    SendMessageW(g_sectionThreeStatic, WM_SETFONT, reinterpret_cast<WPARAM>(g_sectionFont), TRUE);

    g_processEdit = CreateControl(L"EDIT", processName.c_str(), WS_BORDER | ES_AUTOHSCROLL, ProcessEditId);
    g_findProcessButton = CreateControl(L"BUTTON", L"Find osu!", BS_PUSHBUTTON | BS_OWNERDRAW, FindProcessButtonId);
    g_injectHookCheck = CreateControl(L"BUTTON", L"Inject hook", BS_AUTOCHECKBOX, InjectHookCheckId);
    SendMessageW(g_injectHookCheck, BM_SETCHECK, injectHook ? BST_CHECKED : BST_UNCHECKED, 0);

    CreateControl(L"STATIC", L"Log dir", 0, LogDirEditId + 10000);
    g_logDirEdit = CreateControl(L"EDIT", logDirectory.c_str(), WS_BORDER | ES_AUTOHSCROLL, LogDirEditId);

    g_mirrorCheck = CreateControl(L"BUTTON", L"Mirror audio", BS_AUTOCHECKBOX, MirrorCheckId);
    SendMessageW(g_mirrorCheck, BM_SETCHECK, mirrorAudio ? BST_CHECKED : BST_UNCHECKED, 0);
    g_mirrorMusicCheck = CreateControl(L"BUTTON", L"Mirror music", BS_AUTOCHECKBOX, MirrorMusicCheckId);
    SendMessageW(g_mirrorMusicCheck, BM_SETCHECK, mirrorMusic ? BST_CHECKED : BST_UNCHECKED, 0);
    CreateControl(L"STATIC", L"Backend", 0, OutputBackendComboId + 10000);
    g_outputBackendCombo = CreateControl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, OutputBackendComboId);
    SendMessageW(g_outputBackendCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"XAudio2"));
    SendMessageW(g_outputBackendCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"WASAPI Exclusive"));
    SendMessageW(g_outputBackendCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"ASIO"));
    SelectOutputBackend(outputBackend);
    CreateControl(L"STATIC", L"Device", 0, OutputDeviceComboId + 10000);
    g_outputDeviceCombo = CreateControl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, OutputDeviceComboId);
    PopulateOutputDeviceCombo(outputDeviceId);
    CreateControl(L"STATIC", L"Rate", 0, OutputSampleRateComboId + 10000);
    g_outputSampleRateCombo = CreateControl(L"COMBOBOX", L"", CBS_DROPDOWNLIST, OutputSampleRateComboId);
    SendMessageW(g_outputSampleRateCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"48000"));
    SendMessageW(g_outputSampleRateCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"44100"));
    SelectComboText(g_outputSampleRateCombo, outputSampleRate, 0);
    CreateControl(L"STATIC", L"Buffer", 0, OutputBufferComboId + 10000);
    g_outputBufferCombo = CreateControl(L"COMBOBOX", L"", CBS_DROPDOWNLIST, OutputBufferComboId);
    SendMessageW(g_outputBufferCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Driver"));
    SendMessageW(g_outputBufferCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"3"));
    SendMessageW(g_outputBufferCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"5"));
    SendMessageW(g_outputBufferCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"10"));
    SendMessageW(g_outputBufferCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"20"));
    SelectComboText(g_outputBufferCombo, outputBufferMs, 3);
    if (!outputBufferMs.empty() && outputBufferMs != L"Driver")
        g_lastNumericBufferMs = outputBufferMs;
    CreateControl(L"STATIC", L"Channels", 0, OutputChannelsComboId + 10000);
    g_outputChannelsCombo = CreateControl(L"COMBOBOX", L"", CBS_DROPDOWNLIST, OutputChannelsComboId);
    SendMessageW(g_outputChannelsCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"2"));
    SendMessageW(g_outputChannelsCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"4"));
    SendMessageW(g_outputChannelsCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"6"));
    SendMessageW(g_outputChannelsCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"8"));
    SelectComboText(g_outputChannelsCombo, outputChannels, 0);
    CreateControl(L"STATIC", L"Effects vol", 0, EffectsVolumeComboId + 10000);
    g_effectsVolumeSlider = CreateControl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | TBS_TOOLTIPS, EffectsVolumeComboId);
    SendMessageW(g_effectsVolumeSlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 200));
    SendMessageW(g_effectsVolumeSlider, TBM_SETTICFREQ, 25, 0);
    SendMessageW(g_effectsVolumeSlider, TBM_SETLINESIZE, 0, 1);
    SendMessageW(g_effectsVolumeSlider, TBM_SETPAGESIZE, 0, 5);
    SetVolumeSlider(g_effectsVolumeSlider, effectsVolume);
    g_effectsVolumeValueStatic = CreateControl(L"STATIC", L"", SS_RIGHT, EffectsVolumeValueStaticId);
    CreateControl(L"STATIC", L"Music vol", 0, MusicVolumeComboId + 10000);
    g_musicVolumeSlider = CreateControl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | TBS_TOOLTIPS, MusicVolumeComboId);
    SendMessageW(g_musicVolumeSlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 200));
    SendMessageW(g_musicVolumeSlider, TBM_SETTICFREQ, 25, 0);
    SendMessageW(g_musicVolumeSlider, TBM_SETLINESIZE, 0, 1);
    SendMessageW(g_musicVolumeSlider, TBM_SETPAGESIZE, 0, 5);
    SetVolumeSlider(g_musicVolumeSlider, musicVolume);
    g_musicVolumeValueStatic = CreateControl(L"STATIC", L"", SS_RIGHT, MusicVolumeValueStaticId);
    UpdateVolumeSliderLabels();
    g_musicModeHintStatic = CreateControl(
        L"STATIC",
        L"",
        SS_LEFT,
        MusicModeHintStaticId);
    g_logEnabledCheck = CreateControl(L"BUTTON", L"Output logs", BS_AUTOCHECKBOX, LogEnabledCheckId);
    SendMessageW(g_logEnabledCheck, BM_SETCHECK, logEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
    g_verboseCheck = CreateControl(L"BUTTON", L"Verbose events", BS_AUTOCHECKBOX, VerboseCheckId);
    SendMessageW(g_verboseCheck, BM_SETCHECK, verboseEvents ? BST_CHECKED : BST_UNCHECKED, 0);
    g_dumpSamplesCheck = CreateControl(L"BUTTON", L"Dump samples", BS_AUTOCHECKBOX, DumpSamplesCheckId);
    SendMessageW(g_dumpSamplesCheck, BM_SETCHECK, dumpSamples ? BST_CHECKED : BST_UNCHECKED, 0);
    CreateControl(L"STATIC", L"Dump dir", 0, DumpDirEditId + 10000);
    g_dumpDirEdit = CreateControl(L"EDIT", dumpDirectory.c_str(), WS_BORDER | ES_AUTOHSCROLL, DumpDirEditId);
    g_browseLogDirButton = CreateControl(L"BUTTON", L"Browse", BS_PUSHBUTTON | BS_OWNERDRAW, BrowseLogDirButtonId);
    g_browseDumpDirButton = CreateControl(L"BUTTON", L"Browse", BS_PUSHBUTTON | BS_OWNERDRAW, BrowseDumpDirButtonId);
    g_openDumpDirButton = CreateControl(L"BUTTON", L"Open dumps", BS_PUSHBUTTON | BS_OWNERDRAW, OpenDumpDirButtonId);

    g_startButton = CreateControl(L"BUTTON", L"Start", BS_PUSHBUTTON | BS_OWNERDRAW, StartButtonId);
    g_stopButton = CreateControl(L"BUTTON", L"Stop", BS_PUSHBUTTON | BS_OWNERDRAW, StopButtonId);
    g_copyCommandButton = CreateControl(L"BUTTON", L"Copy command", BS_PUSHBUTTON | BS_OWNERDRAW, CopyCommandButtonId);
    g_checkSetupButton = CreateControl(L"BUTTON", L"Check setup", BS_PUSHBUTTON | BS_OWNERDRAW, CheckSetupButtonId);
    g_testToneButton = CreateControl(L"BUTTON", L"Test tone", BS_PUSHBUTTON | BS_OWNERDRAW, TestToneButtonId);
    g_refreshDevicesButton = CreateControl(L"BUTTON", L"Refresh devices", BS_PUSHBUTTON | BS_OWNERDRAW, RefreshDevicesButtonId);
    g_asioPanelButton = CreateControl(L"BUTTON", L"ASIO panel", BS_PUSHBUTTON | BS_OWNERDRAW, AsioPanelButtonId);
    g_openSettingsButton = CreateControl(L"BUTTON", L"Settings", BS_PUSHBUTTON | BS_OWNERDRAW, OpenSettingsButtonId);
    g_openLogsButton = CreateControl(L"BUTTON", L"Open logs", BS_PUSHBUTTON | BS_OWNERDRAW, OpenLogsButtonId);
    g_clearButton = CreateControl(L"BUTTON", L"Clear", BS_PUSHBUTTON | BS_OWNERDRAW, ClearButtonId);
    g_decodeDumpButton = CreateControl(L"BUTTON", L"Decode dumps", BS_PUSHBUTTON | BS_OWNERDRAW, DecodeDumpButtonId);
    g_statusStatic = CreateControl(L"STATIC", L"Idle", SS_LEFT, StatusStaticId);

    g_outputEdit = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT",
        L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
        0,
        0,
        10,
        10,
        window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(OutputEditId)),
        g_instance,
        nullptr);
    SendMessageW(g_outputEdit, WM_SETFONT, reinterpret_cast<WPARAM>(g_monoFont), TRUE);
    SendMessageW(g_statusStatic, WM_SETFONT, reinterpret_cast<WPARAM>(g_navFont), TRUE);

    UpdateButtons();
    UpdateMusicModeHint();
    UpdateLogOutputControls();
    SelectPage(g_currentPage);
}

void PaintBackground(HWND window, HDC dc)
{
    RECT client {};
    GetClientRect(window, &client);
    FillRect(dc, &client, g_surfaceBrush != nullptr ? g_surfaceBrush : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));

    RECT sidebar = client;
    sidebar.right = client.right < SidebarWidth ? client.right : SidebarWidth;
    FillRect(dc, &sidebar, g_sidebarBrush != nullptr ? g_sidebarBrush : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));

    if (client.bottom > 160) {
        RECT statusPanel = {
            WindowMargin - 4,
            client.bottom - WindowMargin - 78,
            SidebarWidth - WindowMargin + 4,
            client.bottom - WindowMargin + 2,
        };
        DrawRoundedRect(dc, statusPanel, ColorSidebarRaised, ColorSidebarRaised, 12);
    }

    HPEN oldPen = static_cast<HPEN>(SelectObject(dc, GetStockObject(DC_PEN)));
    SetDCPenColor(dc, ColorBorder);
    MoveToEx(dc, SidebarWidth, client.top, nullptr);
    LineTo(dc, SidebarWidth, client.bottom);

    const int contentX = SidebarWidth + WindowMargin;
    const int contentRight = client.right - WindowMargin;
    if (contentRight > contentX) {
        auto drawContentLine = [&](int y) {
            MoveToEx(dc, contentX, y, nullptr);
            LineTo(dc, contentRight, y);
        };

        drawContentLine(WindowMargin + 68);
        const int sectionOneY = WindowMargin + 80;
        const int sectionTwoY = sectionOneY + 86;
        const int sectionThreeY = sectionTwoY + 118;
        drawContentLine(sectionOneY + 22);
        drawContentLine(sectionTwoY + 22);
        if (g_currentPage != UiPage::Hook)
            drawContentLine(sectionThreeY + 22);
    }

    SelectObject(dc, oldPen);
}

INT_PTR PaintControlBackground(HDC dc, HWND child)
{
    if (child == g_outputEdit) {
        SetTextColor(dc, ColorTerminalText);
        SetBkColor(dc, ColorTerminal);
        return reinterpret_cast<INT_PTR>(g_terminalBrush);
    }

    if (child == g_statusStatic) {
        SetTextColor(dc, ColorSidebarText);
        SetBkColor(dc, ColorSidebar);
        return reinterpret_cast<INT_PTR>(g_sidebarBrush);
    }

    if (child == g_brandStatic || child == g_sidebarSubtitleStatic) {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, child == g_brandStatic ? ColorSidebarText : ColorSidebarMuted);
        return reinterpret_cast<INT_PTR>(g_sidebarBrush);
    }

    if (child == g_pageSubtitleStatic) {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, ColorMutedText);
        return reinterpret_cast<INT_PTR>(g_surfaceBrush);
    }

    if (child == g_sectionOneStatic || child == g_sectionTwoStatic || child == g_sectionThreeStatic) {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, ColorText);
        return reinterpret_cast<INT_PTR>(g_surfaceBrush);
    }

    wchar_t className[32] {};
    GetClassNameW(child, className, static_cast<int>(std::size(className)));
    if (_wcsicmp(className, L"Edit") == 0) {
        SetTextColor(dc, ColorText);
        SetBkColor(dc, ColorControl);
        return reinterpret_cast<INT_PTR>(g_controlBrush);
    }

    SetBkMode(dc, TRANSPARENT);
    if (child == g_musicModeHintStatic)
        SetTextColor(dc, ColorMutedText);
    else
        SetTextColor(dc, ColorText);

    return reinterpret_cast<INT_PTR>(g_surfaceBrush != nullptr ? g_surfaceBrush : g_canvasBrush);
}

bool IsNavButton(int id)
{
    return id == AudioPageButtonId || id == HookPageButtonId || id == ConsolePageButtonId;
}

bool IsPrimaryButton(int id)
{
    return id == StartButtonId || id == TestToneButtonId;
}

bool IsDangerButton(int id)
{
    return id == StopButtonId;
}

void DrawRoundedRect(HDC dc, const RECT& rect, COLORREF fill, COLORREF border, int radius)
{
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

bool DrawOwnerButton(const DRAWITEMSTRUCT* item)
{
    if (item == nullptr || item->CtlType != ODT_BUTTON)
        return false;

    const int id = static_cast<int>(item->CtlID);
    const bool nav = IsNavButton(id);
    const bool checked = SendMessageW(item->hwndItem, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool pressed = (item->itemState & ODS_SELECTED) != 0;
    const bool disabled = (item->itemState & ODS_DISABLED) != 0;
    const bool focused = (item->itemState & ODS_FOCUS) != 0;

    RECT rect = item->rcItem;
    HDC dc = item->hDC;
    SetBkMode(dc, TRANSPARENT);

    if (nav) {
        FillRect(dc, &rect, g_sidebarBrush);
        RECT pill = rect;
        pill.right -= 2;
        const COLORREF fill = checked || pressed ? ColorSidebarRaised : ColorSidebar;
        DrawRoundedRect(dc, pill, fill, fill, 12);
        if (checked) {
            RECT accent = { pill.left + 4, pill.top + 10, pill.left + 8, pill.bottom - 10 };
            HBRUSH accentBrush = CreateSolidBrush(ColorAccent);
            FillRect(dc, &accent, accentBrush);
            DeleteObject(accentBrush);
        }

        std::wstring text = GetWindowTextString(item->hwndItem);
        RECT textRect = pill;
        textRect.left += 20;
        textRect.right -= 12;
        SelectObject(dc, g_navFont != nullptr ? g_navFont : g_font);
        SetTextColor(dc, disabled ? ColorSidebarMuted : ColorSidebarText);
        DrawTextW(dc, text.c_str(), -1, &textRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    } else {
        RECT button = rect;
        button.right -= 1;
        button.bottom -= 1;

        COLORREF fill = ColorButton;
        COLORREF border = ColorBorder;
        COLORREF textColor = ColorText;
        if (disabled) {
            fill = RGB(230, 233, 237);
            border = RGB(224, 228, 233);
            textColor = ColorMutedText;
        } else if (IsDangerButton(id)) {
            fill = pressed ? ColorDangerPressed : ColorDanger;
            border = fill;
            textColor = RGB(255, 255, 255);
        } else if (IsPrimaryButton(id)) {
            fill = pressed ? ColorAccentPressed : ColorAccent;
            border = fill;
            textColor = RGB(255, 255, 255);
        } else if (pressed) {
            fill = ColorButtonPressed;
        }

        DrawRoundedRect(dc, button, fill, border, 8);

        std::wstring text = GetWindowTextString(item->hwndItem);
        RECT textRect = button;
        textRect.left += 8;
        textRect.right -= 8;
        SelectObject(dc, g_font);
        SetTextColor(dc, textColor);
        DrawTextW(dc, text.c_str(), -1, &textRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }

    if (focused) {
        RECT focus = rect;
        InflateRect(&focus, -4, -4);
        DrawFocusRect(dc, &focus);
    }

    return true;
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CREATE:
        CreateUi(window);
        return 0;

    case WM_ERASEBKGND:
        PaintBackground(window, reinterpret_cast<HDC>(wParam));
        return 1;

    case WM_SIZE:
        LayoutControls(LOWORD(lParam), HIWORD(lParam));
        InvalidateRect(window, nullptr, TRUE);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = 920;
        info->ptMinTrackSize.y = 560;
        return 0;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
        return PaintControlBackground(reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam));

    case WM_CTLCOLORBTN: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, ColorText);
        return reinterpret_cast<INT_PTR>(g_surfaceBrush);
    }

    case WM_CTLCOLORLISTBOX: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, ColorText);
        SetBkColor(dc, ColorControl);
        return reinterpret_cast<INT_PTR>(g_controlBrush);
    }

    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(lParam) == g_effectsVolumeSlider || reinterpret_cast<HWND>(lParam) == g_musicVolumeSlider) {
            UpdateVolumeSliderLabels();
            SaveSettings();
            return 0;
        }
        break;

    case WM_DRAWITEM:
        if (DrawOwnerButton(reinterpret_cast<DRAWITEMSTRUCT*>(lParam)))
            return TRUE;
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case StartButtonId:
            StartBridge();
            return 0;
        case StopButtonId:
            StopBridge();
            return 0;
        case CopyCommandButtonId:
            CopyStartCommand();
            return 0;
        case CheckSetupButtonId:
            CheckSetup();
            return 0;
        case TestToneButtonId:
            RunTestTone();
            return 0;
        case RefreshDevicesButtonId:
            RefreshOutputDevices();
            return 0;
        case AsioPanelButtonId:
            OpenAsioPanel();
            return 0;
        case FindProcessButtonId:
            FindProcess();
            return 0;
        case OpenSettingsButtonId:
            OpenSettingsFile();
            return 0;
        case BrowseLogDirButtonId:
            BrowseLogDirectory();
            return 0;
        case BrowseDumpDirButtonId:
            BrowseDumpDirectory();
            return 0;
        case OpenDumpDirButtonId:
            OpenDumpDirectory();
            return 0;
        case OpenLogsButtonId:
            OpenLogDirectory();
            return 0;
        case ClearButtonId:
            SetWindowTextW(g_outputEdit, L"");
            return 0;
        case DecodeDumpButtonId:
            DecodeDumpDirectory();
            return 0;
        case MirrorCheckId:
        case MirrorMusicCheckId:
            UpdateMusicModeHint();
            SaveSettings();
            return 0;
        case InjectHookCheckId:
            SaveSettings();
            return 0;
        case LogEnabledCheckId:
            UpdateLogOutputControls();
            SaveSettings();
            return 0;
        case DumpSamplesCheckId:
            UpdateDumpSampleControls();
            SaveSettings();
            return 0;
        case VerboseCheckId:
            SaveSettings();
            return 0;
        case OutputBackendComboId:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                PopulateOutputDeviceCombo(L"");
                UpdateMusicModeHint();
                SaveSettings();
            }
            return 0;
        case OutputDeviceComboId:
        case OutputSampleRateComboId:
        case OutputChannelsComboId:
        case OutputBufferComboId:
            if (HIWORD(wParam) == CBN_SELCHANGE)
                SaveSettings();
            return 0;
        case ProcessEditId:
        case LogDirEditId:
        case DumpDirEditId:
            if (HIWORD(wParam) == EN_KILLFOCUS)
                SaveSettings();
            return 0;
        case AudioPageButtonId:
            SelectPage(UiPage::Audio);
            SaveSettings();
            return 0;
        case HookPageButtonId:
            SelectPage(UiPage::Hook);
            SaveSettings();
            return 0;
        case ConsolePageButtonId:
            SelectPage(UiPage::Console);
            SaveSettings();
            return 0;
        default:
            break;
        }
        break;

    case WM_APPEND_LOG: {
        std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lParam));
        AppendLog(*text);
        return 0;
    }

    case WM_PROCESS_EXITED:
        CleanupChild();
        SetStatus(L"Idle");
        UpdateButtons();
        return 0;

    case WM_DESTROY:
        SaveSettings();
        if (g_child.IsActive())
            RequestChildShutdown(3000, true);
        CleanupChild();
        DeleteGdiObjectIfSet(g_font);
        DeleteGdiObjectIfSet(g_brandFont);
        DeleteGdiObjectIfSet(g_titleFont);
        DeleteGdiObjectIfSet(g_navFont);
        DeleteGdiObjectIfSet(g_sectionFont);
        DeleteGdiObjectIfSet(g_monoFont);
        DeleteGdiObjectIfSet(g_canvasBrush);
        DeleteGdiObjectIfSet(g_surfaceBrush);
        DeleteGdiObjectIfSet(g_sidebarBrush);
        DeleteGdiObjectIfSet(g_controlBrush);
        DeleteGdiObjectIfSet(g_terminalBrush);
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    g_instance = instance;

    INITCOMMONCONTROLSEX controls {};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);

    WNDCLASSW windowClass {};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = WindowClassName;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;

    RegisterClassW(&windowClass);

    HWND window = CreateWindowExW(
        0,
        WindowClassName,
        L"OsuLazerAudioBridge",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1040,
        660,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (window == nullptr)
        return 1;

    ShowWindow(window, showCommand);
    UpdateWindow(window);

    MSG message {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return static_cast<int>(message.wParam);
}
