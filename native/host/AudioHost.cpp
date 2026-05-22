#include <Windows.h>
#include <Audioclient.h>
#include <Mmdeviceapi.h>
#include <TlHelp32.h>
#include <mmreg.h>
#include <avrt.h>
#include <wrl/client.h>
#include <xaudio2.h>

#include <asiosys.h>
#include <asio.h>
#include <asiodrivers.h>
#include <iasiodrv.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functiondiscoverykeys_devpkey.h>
#include <iomanip>
#include <iostream>
#include <memory>
#include <limits>
#include <optional>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <olab/SharedChannel.h>
#include "SampleDecoder.h"

IASIO* theAsioDriver = nullptr;

namespace {

using Microsoft::WRL::ComPtr;

struct SharedHandles {
    HANDLE mapping = nullptr;
    HANDLE event = nullptr;
    olab::SharedChannel* channel = nullptr;

    SharedHandles() = default;
    SharedHandles(const SharedHandles&) = delete;
    SharedHandles& operator=(const SharedHandles&) = delete;

    SharedHandles(SharedHandles&& other) noexcept
        : mapping(other.mapping)
        , event(other.event)
        , channel(other.channel)
    {
        other.mapping = nullptr;
        other.event = nullptr;
        other.channel = nullptr;
    }

    SharedHandles& operator=(SharedHandles&& other) noexcept
    {
        if (this == &other)
            return *this;

        Reset();
        mapping = other.mapping;
        event = other.event;
        channel = other.channel;
        other.mapping = nullptr;
        other.event = nullptr;
        other.channel = nullptr;
        return *this;
    }

    ~SharedHandles()
    {
        Reset();
    }

    void Reset()
    {
        if (channel != nullptr)
            UnmapViewOfFile(channel);
        if (event != nullptr)
            CloseHandle(event);
        if (mapping != nullptr)
            CloseHandle(mapping);

        channel = nullptr;
        event = nullptr;
        mapping = nullptr;
    }
};

void PrintUsage()
{
    std::wcout
        << L"OsuLazerAudioHost probe\n"
        << L"Usage:\n"
        << L"  OsuLazerAudioHost.exe --process osu!.exe\n"
        << L"  OsuLazerAudioHost.exe --pid 1234\n"
        << L"Options:\n"
        << L"  --dll <path>       Hook DLL path. Defaults to OsuLazerBassHook.dll beside this exe.\n"
        << L"  --no-inject        Only create the shared channel and listen.\n"
        << L"  --verbose          Print high-frequency attribute/position events.\n"
        << L"  --log <path>       Write probe output to this UTF-8 log file.\n"
        << L"  --log-dir <path>   Write automatic probe logs under this directory.\n"
        << L"  --no-log           Disable automatic file logging.\n"
        << L"  --dump-samples <dir> Dump copied memory samples as raw .bin files.\n"
        << L"  --mirror-audio     Decode and play inferred sample events through the selected output backend.\n"
        << L"  --mirror-music     Enable experimental music stream mirroring mode.\n"
        << L"  --output-backend <xaudio2|wasapi-exclusive|asio>\n"
        << L"  --list-output-devices Print active Windows render device ids and exit.\n"
        << L"  --list-asio-devices Print registered ASIO driver names and exit.\n"
        << L"  --output-device <id> Use a specific Windows render device id where supported.\n"
        << L"  --output-sample-rate <hz> Preferred exclusive sample rate. Default 48000.\n"
        << L"  --output-channels <n> Preferred exclusive channel count. Default 2.\n"
        << L"  --output-buffer-ms <ms> Preferred exclusive buffer size. Default 10.\n"
        << L"  --effects-volume <0..200> Bridge effects volume percent. Default 100.\n"
        << L"  --music-volume <0..200> Bridge music volume percent. Default 100.\n"
        << L"  --asio-control-panel Open the selected ASIO driver's control panel and exit.\n"
        << L"  --decode-dir <dir> Decode dumped sample .bin files and exit.\n"
        << L"  --test-tone        Play a short output-backend test tone and exit.\n";
}

std::optional<DWORD> FindProcessId(const std::wstring& processName)
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
        if (_wcsicmp(entry.szExeFile, processName.c_str()) == 0) {
            CloseHandle(snapshot);
            return entry.th32ProcessID;
        }
    } while (Process32NextW(snapshot, &entry));

    CloseHandle(snapshot);
    return std::nullopt;
}

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

std::wstring QuoteCommandLineArgument(const std::wstring& value)
{
    std::wstring result = L"\"";
    unsigned int backslashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') {
            backslashes++;
            continue;
        }

        if (ch == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(ch);
        } else {
            result.append(backslashes, L'\\');
            result.push_back(ch);
        }
        backslashes = 0;
    }

    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

SharedHandles CreateSharedChannel()
{
    SharedHandles handles;
    handles.mapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        static_cast<DWORD>(sizeof(olab::SharedChannel)),
        olab::SharedMemoryName);
    if (handles.mapping == nullptr)
        throw std::runtime_error("CreateFileMappingW failed.");

    handles.channel = static_cast<olab::SharedChannel*>(
        MapViewOfFile(handles.mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(olab::SharedChannel)));
    if (handles.channel == nullptr)
        throw std::runtime_error("MapViewOfFile failed.");

    olab::InitializeSharedChannel(*handles.channel);

    handles.event = CreateEventW(nullptr, FALSE, FALSE, olab::EventName);
    if (handles.event == nullptr)
        throw std::runtime_error("CreateEventW failed.");

    olab::PublishEvent(handles.channel, handles.event, olab::EventKind::HostStarted);
    return handles;
}

std::wstring FormatLocalTimestampForFile()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local {};
    localtime_s(&local, &time);

    std::wstringstream stream;
    stream
        << std::put_time(&local, L"%Y%m%d-%H%M%S");
    return stream.str();
}

std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty())
        return {};

    int length = WideCharToMultiByte(
        CP_UTF8,
        0,
        value.c_str(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (length <= 0)
        return {};

    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        value.c_str(),
        static_cast<int>(value.size()),
        result.data(),
        length,
        nullptr,
        nullptr);
    return result;
}

std::string WideToAnsi(const std::wstring& value)
{
    if (value.empty())
        return {};

    int length = WideCharToMultiByte(CP_ACP, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return {};

    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_ACP, 0, value.c_str(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}

class ProbeLog {
public:
    explicit ProbeLog(std::optional<std::filesystem::path> path)
    {
        if (!path)
            return;

        std::filesystem::create_directories(path->parent_path());
        stream.open(*path, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!stream)
            throw std::runtime_error("Failed to open probe log file.");

        logPath = std::filesystem::absolute(*path);
        static constexpr unsigned char utf8Bom[] = { 0xEF, 0xBB, 0xBF };
        stream.write(reinterpret_cast<const char*>(utf8Bom), sizeof(utf8Bom));
    }

    bool IsEnabled() const
    {
        return stream.is_open();
    }

    const std::filesystem::path& Path() const
    {
        return logPath;
    }

    void WriteLine(const std::wstring& line)
    {
        if (!stream.is_open())
            return;

        const std::string utf8 = WideToUtf8(line);
        stream.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
        stream.put('\n');
        stream.flush();
    }

private:
    std::filesystem::path logPath;
    std::ofstream stream;
};

void PrintAndLogLine(const std::wstring& line, ProbeLog& log);

enum class OutputBackend {
    XAudio2,
    WasapiExclusive,
    Asio,
};

std::wstring OutputBackendName(OutputBackend backend)
{
    switch (backend) {
    case OutputBackend::XAudio2:
        return L"xaudio2";
    case OutputBackend::WasapiExclusive:
        return L"wasapi-exclusive";
    case OutputBackend::Asio:
        return L"asio";
    }

    return L"unknown";
}

std::optional<OutputBackend> ParseOutputBackend(const std::wstring& value)
{
    if (_wcsicmp(value.c_str(), L"xaudio2") == 0)
        return OutputBackend::XAudio2;
    if (_wcsicmp(value.c_str(), L"wasapi-exclusive") == 0 || _wcsicmp(value.c_str(), L"wasapi") == 0)
        return OutputBackend::WasapiExclusive;
    if (_wcsicmp(value.c_str(), L"asio") == 0)
        return OutputBackend::Asio;

    return std::nullopt;
}

struct OutputConfig {
    OutputBackend backend = OutputBackend::XAudio2;
    std::wstring deviceId;
    DWORD sampleRate = 48000;
    WORD channels = 2;
    DWORD bufferMs = 10;
    float effectsVolume = 1.0f;
    float musicVolume = 1.0f;
};


std::wstring FormatHresultLine(const wchar_t* operation, HRESULT hr)
{
    std::wstringstream line;
    line << L"AudioMirrorError operation=\"" << operation << L"\" hr=0x" << std::hex << static_cast<unsigned long>(hr);
    return line.str();
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

bool ListOutputDevices()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coInitialized = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        std::wcerr << FormatHresultLine(L"ListDevices.CoInitializeEx", hr) << L"\n";
        return false;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        std::wcerr << FormatHresultLine(L"ListDevices.CoCreateInstance", hr) << L"\n";
        if (coInitialized)
            CoUninitialize();
        return false;
    }

    ComPtr<IMMDeviceCollection> devices;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices);
    if (FAILED(hr)) {
        std::wcerr << FormatHresultLine(L"ListDevices.EnumAudioEndpoints", hr) << L"\n";
        if (coInitialized)
            CoUninitialize();
        return false;
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

        std::wcout << L"[" << i << L"] " << GetDeviceName(device.Get()) << L"\n"
                   << L"    " << rawId << L"\n";
        CoTaskMemFree(rawId);
    }

    if (coInitialized)
        CoUninitialize();
    return true;
}

struct AsioDriverInfo {
    std::wstring name;
    CLSID clsid {};
};

std::vector<AsioDriverInfo> EnumerateAsioDrivers()
{
    std::vector<AsioDriverInfo> drivers;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return drivers;

    DWORD index = 0;
    while (true) {
        wchar_t name[256] {};
        DWORD nameLength = static_cast<DWORD>(std::size(name));
        const LSTATUS enumStatus = RegEnumKeyExW(key, index++, name, &nameLength, nullptr, nullptr, nullptr, nullptr);
        if (enumStatus == ERROR_NO_MORE_ITEMS)
            break;
        if (enumStatus != ERROR_SUCCESS)
            continue;

        HKEY driverKey = nullptr;
        if (RegOpenKeyExW(key, name, 0, KEY_READ, &driverKey) != ERROR_SUCCESS)
            continue;

        wchar_t clsidText[128] {};
        DWORD clsidBytes = sizeof(clsidText);
        DWORD type = 0;
        CLSID clsid {};
        if (RegQueryValueExW(driverKey, L"CLSID", nullptr, &type, reinterpret_cast<BYTE*>(clsidText), &clsidBytes) == ERROR_SUCCESS
            && (type == REG_SZ || type == REG_EXPAND_SZ)
            && SUCCEEDED(CLSIDFromString(clsidText, &clsid))) {
            drivers.push_back(AsioDriverInfo { name, clsid });
        }

        RegCloseKey(driverKey);
    }

    RegCloseKey(key);
    return drivers;
}

bool ListAsioDevices()
{
    const std::vector<AsioDriverInfo> drivers = EnumerateAsioDrivers();
    for (std::size_t i = 0; i < drivers.size(); i++) {
        LPOLESTR clsidText = nullptr;
        StringFromCLSID(drivers[i].clsid, &clsidText);
        std::wcout << L"[" << i << L"] " << drivers[i].name << L"\n"
                   << L"    " << (clsidText == nullptr ? L"" : clsidText) << L"\n";
        if (clsidText != nullptr)
            CoTaskMemFree(clsidText);
    }
    return !drivers.empty();
}

std::optional<std::wstring> ResolveAsioDriverName(const std::wstring& requestedDeviceId)
{
    const std::vector<AsioDriverInfo> drivers = EnumerateAsioDrivers();
    if (drivers.empty())
        return std::nullopt;

    if (!requestedDeviceId.empty()) {
        for (const AsioDriverInfo& driver : drivers) {
            if (_wcsicmp(driver.name.c_str(), requestedDeviceId.c_str()) == 0)
                return driver.name;
        }
    }

    return drivers.front().name;
}

bool OpenAsioControlPanel(const OutputConfig& config)
{
    const auto driverName = ResolveAsioDriverName(config.deviceId);
    if (!driverName) {
        std::wcerr << L"AudioMirrorError operation=\"NativeASIO.ResolveDriver\" message=\"No ASIO driver found. Use --list-asio-devices.\"\n";
        return false;
    }

    const std::string driverNameAnsi = WideToAnsi(*driverName);
    if (driverNameAnsi.empty()) {
        std::wcerr << L"AudioMirrorError operation=\"NativeASIO.DriverName\" message=\"Could not convert driver name.\"\n";
        return false;
    }

    auto asioDrivers = std::make_unique<AsioDrivers>();
    if (!asioDrivers->loadDriver(const_cast<char*>(driverNameAnsi.c_str())) || theAsioDriver == nullptr) {
        std::wcerr << L"AudioMirrorError operation=\"NativeASIO.LoadDriver\" driver=\"" << *driverName << L"\"\n";
        asioDrivers->removeCurrentDriver();
        theAsioDriver = nullptr;
        return false;
    }

    ASIODriverInfo info {};
    info.asioVersion = 2;
    info.sysRef = nullptr;
    if (theAsioDriver->init(&info) != ASIOTrue) {
        std::wcerr << L"AudioMirrorError operation=\"NativeASIO.Init\" driver=\"" << *driverName << L"\"\n";
        asioDrivers->removeCurrentDriver();
        theAsioDriver = nullptr;
        return false;
    }

    const ASIOError result = theAsioDriver->controlPanel();
    std::wcout << L"AsioControlPanelOpened driver=\"" << *driverName << L"\" result=" << static_cast<long>(result) << L"\n";
    asioDrivers->removeCurrentDriver();
    theAsioDriver = nullptr;
    return true;
}

class IAudioOutput {
public:
    virtual ~IAudioOutput() = default;
    virtual bool Start(std::wstring& error) = 0;
    virtual bool Submit(std::uint64_t channel, std::vector<std::int16_t> pcm16, WORD channels, DWORD sampleRate, float volume, bool stream, std::wstring& error) = 0;
    virtual void ResetStream() = 0;
    virtual void ResetAll() = 0;
    virtual bool StopChannel(std::uint64_t channel) = 0;
    virtual void Stop() = 0;
    virtual void SetVolume(float volume) = 0;
    virtual const wchar_t* Name() const = 0;
};

class WasapiExclusiveOutput final : public IAudioOutput {
public:
    explicit WasapiExclusiveOutput(OutputConfig config)
        : config(std::move(config))
    {
    }
    WasapiExclusiveOutput(const WasapiExclusiveOutput&) = delete;
    WasapiExclusiveOutput& operator=(const WasapiExclusiveOutput&) = delete;

    ~WasapiExclusiveOutput() override
    {
        Stop();
    }

    bool Start(std::wstring& error) override
    {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(hr)) {
            comInitialized = true;
        } else if (hr != RPC_E_CHANGED_MODE) {
            error = FormatHresult(L"Wasapi.CoInitializeEx", hr);
            return false;
        }

        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (FAILED(hr)) {
            error = FormatHresult(L"Wasapi.CoCreateInstance", hr);
            return false;
        }

        if (!config.deviceId.empty()) {
            hr = enumerator->GetDevice(config.deviceId.c_str(), &device);
            if (FAILED(hr)) {
                error = FormatHresult(L"Wasapi.GetDevice", hr);
                return false;
            }
        } else {
            hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
            if (FAILED(hr)) {
                error = FormatHresult(L"Wasapi.GetDefaultAudioEndpoint", hr);
                return false;
            }
        }

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(audioClient.GetAddressOf()));
        if (FAILED(hr)) {
            error = FormatHresult(L"Wasapi.Activate", hr);
            return false;
        }

        hr = InitializeExclusiveFormat();
        if (FAILED(hr)) {
            error = FormatHresult(L"Wasapi.InitializeExclusive16Stereo", hr);
            return false;
        }

        hr = audioClient->GetBufferSize(&bufferFrameCount);
        if (FAILED(hr)) {
            error = FormatHresult(L"Wasapi.GetBufferSize", hr);
            return false;
        }

        hr = audioClient->GetService(IID_PPV_ARGS(&renderClient));
        if (FAILED(hr)) {
            error = FormatHresult(L"Wasapi.GetRenderClient", hr);
            return false;
        }

        renderEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (renderEvent == nullptr) {
            error = L"AudioMirrorError operation=\"Wasapi.CreateEvent\"";
            return false;
        }

        hr = audioClient->SetEventHandle(renderEvent);
        if (FAILED(hr)) {
            error = FormatHresult(L"Wasapi.SetEventHandle", hr);
            return false;
        }

        PrimeDeviceBuffer();
        running = true;
        hr = audioClient->Start();
        if (FAILED(hr)) {
            running = false;
            error = FormatHresult(L"Wasapi.Start", hr);
            return false;
        }

        renderThread = std::thread(&WasapiExclusiveOutput::RenderLoop, this);
        return true;
    }

    bool Submit(std::uint64_t channel, std::vector<std::int16_t> pcm16, WORD channels, DWORD sampleRate, float volume, bool stream, std::wstring& error) override
    {
        if (!running || pcm16.empty() || channels == 0 || sampleRate == 0)
            return false;

        if (stream) {
            std::lock_guard<std::mutex> lock(mutex);
            EnsureStreamRing();
            if (streamChannel != 0 && streamChannel != channel)
                ClearStreamLocked();
            streamChannel = channel;

            std::vector<std::int16_t> converted = ConvertStreamToOutputFormatLocked(pcm16, channels, sampleRate, volume);
            if (converted.empty())
                return false;

            PushStreamSamples(converted);
            if (streamQueuedSamples / outputChannels > MaxStreamQueuedFrames())
                TrimStreamQueueToFrames(CorrectionStartStreamQueuedFrames());
        } else {
            std::vector<std::int16_t> converted = ConvertToOutputFormat(pcm16, channels, sampleRate, volume);
            if (converted.empty())
                return false;

            std::lock_guard<std::mutex> lock(mutex);
            activeClips.push_back(Clip { std::move(converted), channel, 0 });
        }

        error.clear();
        return true;
    }

    void ResetStream() override
    {
        std::lock_guard<std::mutex> lock(mutex);
        ClearStreamLocked();
    }

    void ResetAll() override
    {
        std::lock_guard<std::mutex> lock(mutex);
        ClearStreamLocked();
        activeClips.clear();
    }

    bool StopChannel(std::uint64_t channel) override
    {
        if (channel == 0)
            return false;

        std::lock_guard<std::mutex> lock(mutex);
        bool stopped = false;
        if (streamChannel == channel) {
            ClearStreamLocked();
            stopped = true;
        }

        auto write = activeClips.begin();
        for (auto read = activeClips.begin(); read != activeClips.end(); ++read) {
            if (read->channel == channel) {
                stopped = true;
                continue;
            }

            *write++ = std::move(*read);
        }
        activeClips.erase(write, activeClips.end());
        return stopped;
    }

    void Stop() override
    {
        running = false;
        if (renderEvent != nullptr)
            SetEvent(renderEvent);

        if (audioClient != nullptr)
            audioClient->Stop();

        if (renderThread.joinable())
            renderThread.join();

        if (renderEvent != nullptr) {
            CloseHandle(renderEvent);
            renderEvent = nullptr;
        }

        renderClient.Reset();
        audioClient.Reset();
        device.Reset();
        enumerator.Reset();

        {
            std::lock_guard<std::mutex> lock(mutex);
            ClearStreamLocked();
            activeClips.clear();
        }

        if (comInitialized) {
            CoUninitialize();
            comInitialized = false;
        }
    }

    void SetVolume(float value) override
    {
        volume.store(std::clamp(value, 0.0f, 1.0f));
    }

    const wchar_t* Name() const override
    {
        return L"wasapi-exclusive";
    }

private:
    static constexpr WORD OutputChannels = 2;

    struct Clip {
        std::vector<std::int16_t> samples;
        std::uint64_t channel = 0;
        std::size_t cursor = 0;
    };

    std::size_t MaxStreamQueuedFrames() const
    {
        const std::size_t outputRate = outputSampleRate == 0 ? 48000 : outputSampleRate;
        return std::max<std::size_t>(static_cast<std::size_t>(bufferFrameCount) * 20, outputRate / 5);
    }

    std::size_t TargetStreamQueuedFrames() const
    {
        const std::size_t outputRate = outputSampleRate == 0 ? 48000 : outputSampleRate;
        return std::max<std::size_t>(static_cast<std::size_t>(bufferFrameCount) * 4, outputRate / 25);
    }

    std::size_t CorrectionStartStreamQueuedFrames() const
    {
        const std::size_t outputRate = outputSampleRate == 0 ? 48000 : outputSampleRate;
        return TargetStreamQueuedFrames() + std::max<std::size_t>(static_cast<std::size_t>(bufferFrameCount), outputRate / 100);
    }

    std::size_t StreamPrebufferFrames() const
    {
        const std::size_t outputRate = outputSampleRate == 0 ? 48000 : outputSampleRate;
        return std::max<std::size_t>(static_cast<std::size_t>(bufferFrameCount) * 3, outputRate / 40);
    }

    std::size_t StreamRingCapacityFrames() const
    {
        const std::size_t outputRate = outputSampleRate == 0 ? 48000 : outputSampleRate;
        return std::max<std::size_t>(MaxStreamQueuedFrames() + static_cast<std::size_t>(bufferFrameCount) * 4, outputRate / 4);
    }

    void TrimStreamQueueToFrames(std::size_t targetFrames)
    {
        if (outputChannels == 0)
            return;

        const std::size_t queuedFrames = streamQueuedSamples / outputChannels;
        if (queuedFrames <= targetFrames)
            return;

        std::size_t samplesToDrop = (queuedFrames - targetFrames) * outputChannels;
        DropStreamSamples(samplesToDrop);

        if (streamQueuedSamples == 0)
            streamPrimed = false;
    }

    void EnsureStreamRing()
    {
        const std::size_t desiredSamples = std::max<std::size_t>(
            StreamRingCapacityFrames() * outputChannels,
            static_cast<std::size_t>(bufferFrameCount) * outputChannels * 8);
        if (desiredSamples == 0)
            return;

        if (streamRing.size() == desiredSamples)
            return;

        streamRing.assign(desiredSamples, 0);
        streamReadIndex = 0;
        streamWriteIndex = 0;
        streamQueuedSamples = 0;
        streamPrimed = false;
        streamChannel = 0;
    }

    void ClearStreamLocked()
    {
        streamReadIndex = 0;
        streamWriteIndex = 0;
        streamQueuedSamples = 0;
        streamPrimed = false;
        streamChannel = 0;
        ResetStreamResamplerLocked();
    }

    void ResetStreamResamplerLocked()
    {
        streamResampleInputSampleRate = 0;
        streamResampleInputChannels = 0;
        streamResamplePosition = 0.0;
        streamResampleCarry.clear();
        streamResampleWork.clear();
    }

    void DropStreamSamples(std::size_t samplesToDrop)
    {
        if (streamRing.empty())
            return;

        const std::size_t dropped = std::min(samplesToDrop, streamQueuedSamples);
        streamReadIndex = (streamReadIndex + dropped) % streamRing.size();
        streamQueuedSamples -= dropped;
        if (streamQueuedSamples == 0)
            streamWriteIndex = streamReadIndex;
    }

    void PushStreamSamples(const std::vector<std::int16_t>& samples)
    {
        if (samples.empty() || streamRing.empty())
            return;

        const std::size_t capacity = streamRing.size();
        if (samples.size() >= capacity) {
            const auto start = samples.end() - static_cast<std::ptrdiff_t>(capacity);
            std::copy(start, samples.end(), streamRing.begin());
            streamReadIndex = 0;
            streamWriteIndex = 0;
            streamQueuedSamples = capacity;
            streamPrimed = true;
            return;
        }

        if (streamQueuedSamples + samples.size() > capacity)
            DropStreamSamples(streamQueuedSamples + samples.size() - capacity);

        const std::size_t first = std::min(samples.size(), capacity - streamWriteIndex);
        std::copy(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(first), streamRing.begin() + static_cast<std::ptrdiff_t>(streamWriteIndex));
        if (first < samples.size())
            std::copy(samples.begin() + static_cast<std::ptrdiff_t>(first), samples.end(), streamRing.begin());

        streamWriteIndex = (streamWriteIndex + samples.size()) % capacity;
        streamQueuedSamples += samples.size();
    }

    std::size_t ReadStreamSamples(std::int16_t* output, std::size_t samplesNeeded)
    {
        if (output == nullptr || samplesNeeded == 0 || streamRing.empty() || streamQueuedSamples == 0)
            return 0;

        const std::size_t copied = std::min(samplesNeeded, streamQueuedSamples);
        const std::size_t first = std::min(copied, streamRing.size() - streamReadIndex);
        std::copy(streamRing.begin() + static_cast<std::ptrdiff_t>(streamReadIndex), streamRing.begin() + static_cast<std::ptrdiff_t>(streamReadIndex + first), output);
        if (first < copied)
            std::copy(streamRing.begin(), streamRing.begin() + static_cast<std::ptrdiff_t>(copied - first), output + first);

        streamReadIndex = (streamReadIndex + copied) % streamRing.size();
        streamQueuedSamples -= copied;
        if (streamQueuedSamples == 0) {
            streamWriteIndex = streamReadIndex;
            streamPrimed = false;
        }
        return copied;
    }

    std::size_t RenderStreamSamples(std::int16_t* output, std::size_t outputFrames)
    {
        if (output == nullptr || outputFrames == 0 || outputChannels == 0 || streamQueuedSamples == 0)
            return 0;

        const std::size_t queuedFrames = streamQueuedSamples / outputChannels;
        if (queuedFrames == 0)
            return 0;

        const std::size_t correctionStart = CorrectionStartStreamQueuedFrames();
        std::size_t inputFrames = std::min(outputFrames, queuedFrames);
        if (queuedFrames > correctionStart && queuedFrames > outputFrames) {
            const std::size_t excessFrames = queuedFrames - TargetStreamQueuedFrames();
            const std::size_t proportionalExtra = std::max<std::size_t>(1, outputFrames / 160 + excessFrames / 160);
            const std::size_t maxExtra = std::max<std::size_t>(1, outputFrames / 40);
            const std::size_t extraFrames = std::min({ excessFrames, queuedFrames - outputFrames, proportionalExtra, maxExtra });
            inputFrames = outputFrames + extraFrames;
        }

        const std::size_t inputSamples = inputFrames * outputChannels;
        if (inputFrames <= outputFrames)
            return ReadStreamSamples(output, inputSamples);

        streamScratch.resize(inputSamples);
        const std::size_t copiedSamples = ReadStreamSamples(streamScratch.data(), inputSamples);
        const std::size_t copiedFrames = copiedSamples / outputChannels;
        if (copiedFrames == 0)
            return 0;

        if (copiedFrames <= outputFrames) {
            std::copy(streamScratch.begin(), streamScratch.begin() + static_cast<std::ptrdiff_t>(copiedFrames * outputChannels), output);
            return copiedFrames * outputChannels;
        }

        const double ratio = static_cast<double>(copiedFrames) / static_cast<double>(outputFrames);
        for (std::size_t frame = 0; frame < outputFrames; frame++) {
            const double sourcePosition = static_cast<double>(frame) * ratio;
            const auto index0 = static_cast<std::size_t>(std::floor(sourcePosition));
            const std::size_t index1 = std::min(index0 + 1, copiedFrames - 1);
            const double fraction = sourcePosition - static_cast<double>(index0);
            for (WORD channel = 0; channel < outputChannels; channel++) {
                const float a = static_cast<float>(streamScratch[index0 * outputChannels + channel]);
                const float b = static_cast<float>(streamScratch[index1 * outputChannels + channel]);
                const float sample = a + static_cast<float>((b - a) * fraction);
                output[frame * outputChannels + channel] = static_cast<std::int16_t>(std::clamp(sample, -32768.0f, 32767.0f));
            }
        }

        return outputFrames * outputChannels;
    }

    std::vector<std::int16_t> ConvertStreamToOutputFormatLocked(
        const std::vector<std::int16_t>& input,
        WORD inputChannels,
        DWORD inputSampleRate,
        float inputVolume)
    {
        std::vector<std::int16_t> mapped = MapToOutputChannels(input, inputChannels, inputVolume);
        if (mapped.empty())
            return {};

        if (inputSampleRate == outputSampleRate) {
            ResetStreamResamplerLocked();
            return mapped;
        }

        if (streamResampleInputSampleRate != inputSampleRate
            || streamResampleInputChannels != inputChannels
            || streamResampleCarry.size() != outputChannels) {
            ResetStreamResamplerLocked();
            streamResampleInputSampleRate = inputSampleRate;
            streamResampleInputChannels = inputChannels;
        }

        const std::size_t mappedFrames = mapped.size() / outputChannels;
        if (mappedFrames == 0)
            return {};

        streamResampleWork.clear();
        streamResampleWork.reserve(mapped.size() + outputChannels);
        if (!streamResampleCarry.empty())
            streamResampleWork.insert(streamResampleWork.end(), streamResampleCarry.begin(), streamResampleCarry.end());
        streamResampleWork.insert(streamResampleWork.end(), mapped.begin(), mapped.end());

        const std::size_t workFrames = streamResampleWork.size() / outputChannels;
        if (workFrames < 2) {
            streamResampleCarry = std::move(streamResampleWork);
            return {};
        }

        const double step = static_cast<double>(inputSampleRate) / static_cast<double>(outputSampleRate);
        const double lastFramePosition = static_cast<double>(workFrames - 1);
        double position = streamResamplePosition;
        if (position < 0.0 || position >= lastFramePosition)
            position = 0.0;

        const std::size_t estimatedFrames = static_cast<std::size_t>(std::ceil((lastFramePosition - position) / step));
        std::vector<std::int16_t> output;
        output.reserve(std::max<std::size_t>(1, estimatedFrames) * outputChannels);

        while (position < lastFramePosition) {
            const auto index0 = static_cast<std::size_t>(std::floor(position));
            const std::size_t index1 = std::min(index0 + 1, workFrames - 1);
            const double fraction = position - static_cast<double>(index0);
            for (WORD channel = 0; channel < outputChannels; channel++) {
                const float a = static_cast<float>(streamResampleWork[index0 * outputChannels + channel]);
                const float b = static_cast<float>(streamResampleWork[index1 * outputChannels + channel]);
                const float sample = a + static_cast<float>((b - a) * fraction);
                output.push_back(static_cast<std::int16_t>(std::clamp(sample, -32768.0f, 32767.0f)));
            }
            position += step;
        }

        streamResamplePosition = position - lastFramePosition;
        streamResampleCarry.assign(
            streamResampleWork.end() - static_cast<std::ptrdiff_t>(outputChannels),
            streamResampleWork.end());
        return output;
    }

    std::vector<std::int16_t> MapToOutputChannels(
        const std::vector<std::int16_t>& input,
        WORD inputChannels,
        float inputVolume) const
    {
        const std::size_t inputFrames = input.size() / inputChannels;
        if (inputFrames == 0)
            return {};

        std::vector<std::int16_t> output(inputFrames * outputChannels);
        const float finalVolume = std::clamp(inputVolume, 0.0f, 1.0f) * volume.load();
        for (std::size_t frame = 0; frame < inputFrames; frame++) {
            for (WORD channel = 0; channel < outputChannels; channel++) {
                const WORD inputChannel = inputChannels == 1
                    ? 0
                    : std::min<WORD>(channel, inputChannels - 1);
                const float sample = static_cast<float>(input[frame * inputChannels + inputChannel]) * finalVolume;
                output[frame * outputChannels + channel] = static_cast<std::int16_t>(std::clamp(sample, -32768.0f, 32767.0f));
            }
        }

        return output;
    }

    static std::wstring FormatHresult(const wchar_t* operation, HRESULT hr)
    {
        return FormatHresultLine(operation, hr);
    }

    std::vector<std::int16_t> ConvertToOutputFormat(
        const std::vector<std::int16_t>& input,
        WORD inputChannels,
        DWORD inputSampleRate,
        float inputVolume) const
    {
        const std::size_t inputFrames = input.size() / inputChannels;
        if (inputFrames == 0)
            return {};

        const double ratio = static_cast<double>(inputSampleRate) / static_cast<double>(outputSampleRate);
        const std::size_t outputFrames = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(static_cast<double>(inputFrames) / ratio)));
        std::vector<std::int16_t> output(outputFrames * outputChannels);
        const float finalVolume = std::clamp(inputVolume, 0.0f, 1.0f) * volume.load();

        for (std::size_t frame = 0; frame < outputFrames; frame++) {
            const double sourcePosition = static_cast<double>(frame) * ratio;
            const auto index0 = static_cast<std::size_t>(std::floor(sourcePosition));
            const std::size_t index1 = std::min(index0 + 1, inputFrames - 1);
            const double fraction = sourcePosition - static_cast<double>(index0);

            for (WORD channel = 0; channel < outputChannels; channel++) {
                const WORD inputChannel = inputChannels == 1
                    ? 0
                    : std::min<WORD>(channel, inputChannels - 1);
                const float a = static_cast<float>(input[index0 * inputChannels + inputChannel]);
                const float b = static_cast<float>(input[index1 * inputChannels + inputChannel]);
                const float sample = (a + static_cast<float>((b - a) * fraction)) * finalVolume;
                output[frame * outputChannels + channel] = static_cast<std::int16_t>(std::clamp(sample, -32768.0f, 32767.0f));
            }
        }

        return output;
    }

    HRESULT InitializeExclusiveFormat()
    {
        const DWORD sampleRates[] = {
            config.sampleRate,
            config.sampleRate == 48000 ? 44100UL : 48000UL,
        };
        const DWORD bufferMsValues[] = {
            config.bufferMs,
            10,
            20,
        };

        HRESULT lastHr = E_FAIL;
        for (DWORD sampleRate : sampleRates) {
            if (sampleRate == 0)
                continue;

            format = {};
            format.wFormatTag = WAVE_FORMAT_PCM;
            format.nChannels = config.channels == 0 ? OutputChannels : config.channels;
            format.nSamplesPerSec = sampleRate;
            format.wBitsPerSample = 16;
            format.nBlockAlign = static_cast<WORD>(format.nChannels * sizeof(std::int16_t));
            format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
            format.cbSize = 0;

            for (DWORD bufferMs : bufferMsValues) {
                if (bufferMs == 0)
                    continue;

                const REFERENCE_TIME duration = static_cast<REFERENCE_TIME>(bufferMs) * 10000;
                lastHr = audioClient->Initialize(
                    AUDCLNT_SHAREMODE_EXCLUSIVE,
                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                    duration,
                    duration,
                    &format,
                    nullptr);
                if (SUCCEEDED(lastHr)) {
                    outputSampleRate = sampleRate;
                    outputChannels = format.nChannels;
                    actualBufferMs = bufferMs;
                    return S_OK;
                }
            }
        }

        return lastHr;
    }

    void PrimeDeviceBuffer()
    {
        if (renderClient == nullptr || bufferFrameCount == 0 || outputChannels == 0)
            return;

        BYTE* data = nullptr;
        if (FAILED(renderClient->GetBuffer(bufferFrameCount, &data)))
            return;

        std::memset(data, 0, static_cast<std::size_t>(bufferFrameCount) * outputChannels * sizeof(std::int16_t));
        renderClient->ReleaseBuffer(bufferFrameCount, 0);
    }

    void RenderLoop()
    {
        DWORD taskIndex = 0;
        HANDLE mmcssHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        if (mmcssHandle != nullptr)
            AvSetMmThreadPriority(mmcssHandle, AVRT_PRIORITY_HIGH);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        const DWORD waitMs = actualBufferMs == 0
            ? 20
            : std::clamp<DWORD>(actualBufferMs * 2, 10, 50);

        while (running) {
            WaitForSingleObject(renderEvent, waitMs);
            if (!running)
                break;

            UINT32 padding = 0;
            if (FAILED(audioClient->GetCurrentPadding(&padding)))
                continue;

            const UINT32 framesAvailable = bufferFrameCount > padding
                ? bufferFrameCount - padding
                : 0;
            if (framesAvailable == 0)
                continue;

            BYTE* data = nullptr;
            if (FAILED(renderClient->GetBuffer(framesAvailable, &data)))
                continue;

            auto* output = reinterpret_cast<std::int16_t*>(data);
            const std::size_t samplesNeeded = static_cast<std::size_t>(framesAvailable) * outputChannels;
            std::fill(output, output + samplesNeeded, 0);
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (streamQueuedSamples != 0) {
                    const std::size_t queuedFrames = streamQueuedSamples / outputChannels;
                    if (streamPrimed || queuedFrames >= StreamPrebufferFrames()) {
                        streamPrimed = true;
                        RenderStreamSamples(output, framesAvailable);
                    }
                }

                auto write = activeClips.begin();
                for (auto read = activeClips.begin(); read != activeClips.end(); ++read) {
                    Clip& clip = *read;
                    const std::size_t remaining = clip.samples.size() - clip.cursor;
                    const std::size_t toMix = std::min(samplesNeeded, remaining);
                    for (std::size_t i = 0; i < toMix; i++) {
                        const int mixed = static_cast<int>(output[i]) + static_cast<int>(clip.samples[clip.cursor + i]);
                        output[i] = static_cast<std::int16_t>(std::clamp(mixed, -32768, 32767));
                    }

                    clip.cursor += toMix;
                    if (clip.cursor < clip.samples.size())
                        *write++ = std::move(clip);
                }
                activeClips.erase(write, activeClips.end());
            }

            renderClient->ReleaseBuffer(framesAvailable, 0);
        }

        if (mmcssHandle != nullptr)
            AvRevertMmThreadCharacteristics(mmcssHandle);
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> audioClient;
    ComPtr<IAudioRenderClient> renderClient;
    OutputConfig config;
    WAVEFORMATEX format {};
    DWORD outputSampleRate = 48000;
    WORD outputChannels = 2;
    DWORD actualBufferMs = 0;
    UINT32 bufferFrameCount = 0;
    HANDLE renderEvent = nullptr;
    std::thread renderThread;
    std::atomic<bool> running = false;
    std::atomic<float> volume = 1.0f;
    std::mutex mutex;
    std::vector<std::int16_t> streamRing;
    std::vector<std::int16_t> streamScratch;
    std::size_t streamReadIndex = 0;
    std::size_t streamWriteIndex = 0;
    std::size_t streamQueuedSamples = 0;
    std::uint64_t streamChannel = 0;
    DWORD streamResampleInputSampleRate = 0;
    WORD streamResampleInputChannels = 0;
    double streamResamplePosition = 0.0;
    std::vector<std::int16_t> streamResampleCarry;
    std::vector<std::int16_t> streamResampleWork;
    std::vector<Clip> activeClips;
    bool streamPrimed = false;
    bool comInitialized = false;
};

class NativeAsioOutput final : public IAudioOutput {
public:
    explicit NativeAsioOutput(OutputConfig config)
        : config(std::move(config))
    {
    }

    ~NativeAsioOutput() override
    {
        Stop();
    }

    bool Start(std::wstring& error) override
    {
        const auto driverNameToLoad = ResolveDriverName();
        if (!driverNameToLoad) {
            error = L"AudioMirrorError operation=\"NativeASIO.ResolveDriver\" message=\"No ASIO driver found. Use --list-asio-devices.\"";
            return false;
        }

        driverName = *driverNameToLoad;
        driverNameUtf8 = WideToAnsi(driverName);
        if (driverNameUtf8.empty()) {
            error = L"AudioMirrorError operation=\"NativeASIO.DriverName\" message=\"Could not convert driver name.\"";
            return false;
        }

        asioDrivers = std::make_unique<AsioDrivers>();
        if (!asioDrivers->loadDriver(driverNameUtf8.data()) || theAsioDriver == nullptr) {
            error = L"AudioMirrorError operation=\"NativeASIO.LoadDriver\" driver=\"" + driverName + L"\"";
            Stop();
            return false;
        }

        ASIODriverInfo info {};
        info.asioVersion = 2;
        info.sysRef = nullptr;
        if (theAsioDriver->init(&info) != ASIOTrue) {
            error = L"AudioMirrorError operation=\"NativeASIO.Init\" driver=\"" + driverName + L"\"";
            Stop();
            return false;
        }
        initialized = true;

        if (config.sampleRate != 0 && theAsioDriver->canSampleRate(config.sampleRate) == ASE_OK)
            theAsioDriver->setSampleRate(config.sampleRate);

        ASIOSampleRate asioRate = 0;
        if (theAsioDriver->getSampleRate(&asioRate) != ASE_OK || asioRate <= 0)
            asioRate = config.sampleRate == 0 ? 48000.0 : static_cast<double>(config.sampleRate);
        outputSampleRate = static_cast<DWORD>(std::lround(asioRate));
        outputChannels = config.channels == 0 ? 2 : config.channels;

        long inputChannels = 0;
        long driverOutputChannels = 0;
        if (theAsioDriver->getChannels(&inputChannels, &driverOutputChannels) != ASE_OK || driverOutputChannels <= 0) {
            error = L"AudioMirrorError operation=\"NativeASIO.GetChannels\"";
            Stop();
            return false;
        }
        outputChannels = static_cast<WORD>(std::min<long>(std::max<WORD>(1, outputChannels), driverOutputChannels));

        long minBuffer = 0;
        long maxBuffer = 0;
        long preferredBuffer = 0;
        long granularity = 0;
        if (theAsioDriver->getBufferSize(&minBuffer, &maxBuffer, &preferredBuffer, &granularity) != ASE_OK || preferredBuffer <= 0) {
            error = L"AudioMirrorError operation=\"NativeASIO.GetBufferSize\"";
            Stop();
            return false;
        }
        bufferFrames = preferredBuffer;

        bufferInfos.assign(outputChannels, {});
        channelInfos.assign(outputChannels, {});
        for (WORD channel = 0; channel < outputChannels; channel++) {
            bufferInfos[channel].isInput = ASIOFalse;
            bufferInfos[channel].channelNum = channel;
            channelInfos[channel].channel = channel;
            channelInfos[channel].isInput = ASIOFalse;
            if (theAsioDriver->getChannelInfo(&channelInfos[channel]) != ASE_OK) {
                error = L"AudioMirrorError operation=\"NativeASIO.GetChannelInfo\" channel=" + std::to_wstring(channel);
                Stop();
                return false;
            }
        }

        currentInstance = this;
        callbacks.bufferSwitch = &NativeAsioOutput::BufferSwitchThunk;
        callbacks.sampleRateDidChange = &NativeAsioOutput::SampleRateDidChangeThunk;
        callbacks.asioMessage = &NativeAsioOutput::AsioMessageThunk;
        callbacks.bufferSwitchTimeInfo = &NativeAsioOutput::BufferSwitchTimeInfoThunk;

        if (theAsioDriver->createBuffers(bufferInfos.data(), outputChannels, bufferFrames, &callbacks) != ASE_OK) {
            error = L"AudioMirrorError operation=\"NativeASIO.CreateBuffers\"";
            Stop();
            return false;
        }
        buffersCreated = true;

        long inputLatency = 0;
        long outputLatency = 0;
        theAsioDriver->getLatencies(&inputLatency, &outputLatency);
        reportedOutputLatency = outputLatency;

        if (theAsioDriver->start() != ASE_OK) {
            error = L"AudioMirrorError operation=\"NativeASIO.Start\"";
            Stop();
            return false;
        }
        running = true;
        return true;
    }

    bool Submit(std::uint64_t channel, std::vector<std::int16_t> pcm16, WORD channels, DWORD sampleRate, float volume, bool stream, std::wstring& error) override
    {
        if (!running || pcm16.empty() || channels == 0 || sampleRate == 0)
            return false;

        std::vector<float> converted = ConvertToFloat(pcm16, channels, sampleRate, volume);
        if (converted.empty())
            return false;

        std::lock_guard<std::mutex> lock(mutex);
        if (stream) {
            const std::size_t MaxQueuedFrames = std::max<std::size_t>(static_cast<std::size_t>(bufferFrames) * 3, 1);
            const std::size_t queuedFrames = streamQueuedSamples / outputChannels;
            if (queuedFrames > MaxQueuedFrames) {
                std::size_t samplesToDrop = (queuedFrames - MaxQueuedFrames) * outputChannels;
                while (samplesToDrop != 0 && !streamChunks.empty()) {
                    FloatChunk& chunk = streamChunks.front();
                    const std::size_t remaining = chunk.samples.size() - chunk.cursor;
                    const std::size_t dropped = std::min(samplesToDrop, remaining);
                    chunk.cursor += dropped;
                    samplesToDrop -= dropped;
                    streamQueuedSamples -= dropped;
                    if (chunk.cursor >= chunk.samples.size())
                        streamChunks.pop_front();
                }
            }

            streamQueuedSamples += converted.size();
            streamChunks.push_back(FloatChunk { std::move(converted), channel, 0 });
        } else {
            activeClips.push_back(FloatChunk { std::move(converted), channel, 0 });
        }

        error.clear();
        return true;
    }

    void ResetStream() override
    {
        std::lock_guard<std::mutex> lock(mutex);
        streamChunks.clear();
        streamQueuedSamples = 0;
    }

    void ResetAll() override
    {
        std::lock_guard<std::mutex> lock(mutex);
        streamChunks.clear();
        activeClips.clear();
        streamQueuedSamples = 0;
    }

    bool StopChannel(std::uint64_t channel) override
    {
        if (channel == 0)
            return false;

        std::lock_guard<std::mutex> lock(mutex);
        bool stopped = false;
        auto keptStream = std::deque<FloatChunk> {};
        for (FloatChunk& chunk : streamChunks) {
            if (chunk.channel == channel) {
                streamQueuedSamples -= std::min(streamQueuedSamples, chunk.samples.size() - chunk.cursor);
                stopped = true;
            } else {
                keptStream.push_back(std::move(chunk));
            }
        }
        streamChunks = std::move(keptStream);

        auto write = activeClips.begin();
        for (auto read = activeClips.begin(); read != activeClips.end(); ++read) {
            if (read->channel == channel) {
                stopped = true;
                continue;
            }
            *write++ = std::move(*read);
        }
        activeClips.erase(write, activeClips.end());
        return stopped;
    }

    void Stop() override
    {
        running = false;
        if (currentInstance == this)
            currentInstance = nullptr;

        if (theAsioDriver != nullptr) {
            theAsioDriver->stop();
            if (buffersCreated) {
                theAsioDriver->disposeBuffers();
                buffersCreated = false;
            }
        }
        if (asioDrivers) {
            asioDrivers->removeCurrentDriver();
            asioDrivers.reset();
        }
        theAsioDriver = nullptr;
        initialized = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            streamChunks.clear();
            activeClips.clear();
            streamQueuedSamples = 0;
        }
    }

    void SetVolume(float) override
    {
    }

    const wchar_t* Name() const override
    {
        return L"native-asio";
    }

private:
    struct FloatChunk {
        std::vector<float> samples;
        std::uint64_t channel = 0;
        std::size_t cursor = 0;
    };

    std::optional<std::wstring> ResolveDriverName() const
    {
        return ResolveAsioDriverName(config.deviceId);
    }

    static std::string WideToAnsi(const std::wstring& value)
    {
        if (value.empty())
            return {};

        int length = WideCharToMultiByte(CP_ACP, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        if (length <= 0)
            return {};

        std::string result(static_cast<std::size_t>(length), '\0');
        WideCharToMultiByte(CP_ACP, 0, value.c_str(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
        return result;
    }

    std::vector<float> ConvertToFloat(const std::vector<std::int16_t>& input, WORD inputChannels, DWORD inputSampleRate, float inputVolume) const
    {
        const std::size_t inputFrames = input.size() / inputChannels;
        if (inputFrames == 0)
            return {};

        const double ratio = static_cast<double>(inputSampleRate) / static_cast<double>(outputSampleRate);
        const std::size_t outputFrames = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(static_cast<double>(inputFrames) / ratio)));
        std::vector<float> output(outputFrames * outputChannels);
        const float gain = std::clamp(inputVolume, 0.0f, 1.0f);

        for (std::size_t frame = 0; frame < outputFrames; frame++) {
            const double sourcePosition = static_cast<double>(frame) * ratio;
            const auto index0 = static_cast<std::size_t>(std::floor(sourcePosition));
            const std::size_t index1 = std::min(index0 + 1, inputFrames - 1);
            const double fraction = sourcePosition - static_cast<double>(index0);

            for (WORD channel = 0; channel < outputChannels; channel++) {
                const WORD inputChannel = inputChannels == 1
                    ? 0
                    : std::min<WORD>(channel, inputChannels - 1);
                const float a = input[index0 * inputChannels + inputChannel] / 32768.0f;
                const float b = input[index1 * inputChannels + inputChannel] / 32768.0f;
                output[frame * outputChannels + channel] = std::clamp((a + static_cast<float>((b - a) * fraction)) * gain, -1.0f, 1.0f);
            }
        }

        return output;
    }

    void Render(long doubleBufferIndex)
    {
        if (doubleBufferIndex < 0 || doubleBufferIndex > 1)
            return;

        std::vector<float> interleaved(static_cast<std::size_t>(bufferFrames) * outputChannels);
        {
            std::lock_guard<std::mutex> lock(mutex);
            std::size_t cursor = 0;
            while (cursor < interleaved.size() && !streamChunks.empty()) {
                FloatChunk& chunk = streamChunks.front();
                const std::size_t remaining = chunk.samples.size() - chunk.cursor;
                const std::size_t copied = std::min(interleaved.size() - cursor, remaining);
                std::copy(
                    chunk.samples.begin() + static_cast<std::ptrdiff_t>(chunk.cursor),
                    chunk.samples.begin() + static_cast<std::ptrdiff_t>(chunk.cursor + copied),
                    interleaved.begin() + static_cast<std::ptrdiff_t>(cursor));
                chunk.cursor += copied;
                cursor += copied;
                streamQueuedSamples -= std::min(streamQueuedSamples, copied);
                if (chunk.cursor >= chunk.samples.size())
                    streamChunks.pop_front();
            }

            auto write = activeClips.begin();
            for (auto read = activeClips.begin(); read != activeClips.end(); ++read) {
                const std::size_t remaining = read->samples.size() - read->cursor;
                const std::size_t mixed = std::min(interleaved.size(), remaining);
                for (std::size_t i = 0; i < mixed; i++)
                    interleaved[i] = std::clamp(interleaved[i] + read->samples[read->cursor + i], -1.0f, 1.0f);

                read->cursor += mixed;
                if (read->cursor < read->samples.size())
                    *write++ = std::move(*read);
            }
            activeClips.erase(write, activeClips.end());
        }

        for (WORD channel = 0; channel < outputChannels; channel++) {
            void* buffer = bufferInfos[channel].buffers[doubleBufferIndex];
            if (buffer == nullptr)
                continue;

            WriteChannelBuffer(buffer, channelInfos[channel].type, interleaved, channel);
        }

        if (theAsioDriver != nullptr && theAsioDriver->outputReady() == ASE_OK) {
        }
    }

    void WriteChannelBuffer(void* buffer, ASIOSampleType type, const std::vector<float>& interleaved, WORD channel) const
    {
        switch (type) {
        case ASIOSTFloat32LSB: {
            auto* out = static_cast<float*>(buffer);
            for (long frame = 0; frame < bufferFrames; frame++)
                out[frame] = interleaved[static_cast<std::size_t>(frame) * outputChannels + channel];
            break;
        }
        case ASIOSTFloat64LSB: {
            auto* out = static_cast<double*>(buffer);
            for (long frame = 0; frame < bufferFrames; frame++)
                out[frame] = interleaved[static_cast<std::size_t>(frame) * outputChannels + channel];
            break;
        }
        case ASIOSTInt16LSB: {
            auto* out = static_cast<std::int16_t*>(buffer);
            for (long frame = 0; frame < bufferFrames; frame++) {
                const float sample = std::clamp(interleaved[static_cast<std::size_t>(frame) * outputChannels + channel], -1.0f, 1.0f);
                out[frame] = static_cast<std::int16_t>(sample >= 0 ? sample * 32767.0f : sample * 32768.0f);
            }
            break;
        }
        case ASIOSTInt24LSB: {
            auto* out = static_cast<std::uint8_t*>(buffer);
            for (long frame = 0; frame < bufferFrames; frame++) {
                const std::int32_t value = FloatToSignedInt(interleaved[static_cast<std::size_t>(frame) * outputChannels + channel], 24);
                out[frame * 3 + 0] = static_cast<std::uint8_t>(value & 0xff);
                out[frame * 3 + 1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
                out[frame * 3 + 2] = static_cast<std::uint8_t>((value >> 16) & 0xff);
            }
            break;
        }
        case ASIOSTInt32LSB16: {
            WriteInt32ChannelBuffer(buffer, interleaved, channel, 16, false);
            break;
        }
        case ASIOSTInt32LSB18: {
            WriteInt32ChannelBuffer(buffer, interleaved, channel, 18, false);
            break;
        }
        case ASIOSTInt32LSB20: {
            WriteInt32ChannelBuffer(buffer, interleaved, channel, 20, false);
            break;
        }
        case ASIOSTInt32LSB24: {
            WriteInt32ChannelBuffer(buffer, interleaved, channel, 24, false);
            break;
        }
        case ASIOSTInt32LSB: {
            WriteInt32ChannelBuffer(buffer, interleaved, channel, 32, false);
            break;
        }
        case ASIOSTFloat32MSB: {
            auto* out = static_cast<std::uint8_t*>(buffer);
            for (long frame = 0; frame < bufferFrames; frame++) {
                std::uint32_t bits = 0;
                const float value = interleaved[static_cast<std::size_t>(frame) * outputChannels + channel];
                std::memcpy(&bits, &value, sizeof(bits));
                WriteBigEndian(out + frame * 4, bits, 4);
            }
            break;
        }
        case ASIOSTFloat64MSB: {
            auto* out = static_cast<std::uint8_t*>(buffer);
            for (long frame = 0; frame < bufferFrames; frame++) {
                std::uint64_t bits = 0;
                const double value = interleaved[static_cast<std::size_t>(frame) * outputChannels + channel];
                std::memcpy(&bits, &value, sizeof(bits));
                WriteBigEndian(out + frame * 8, bits, 8);
            }
            break;
        }
        case ASIOSTInt16MSB: {
            auto* out = static_cast<std::uint8_t*>(buffer);
            for (long frame = 0; frame < bufferFrames; frame++) {
                const auto value = static_cast<std::uint16_t>(FloatToSignedInt(interleaved[static_cast<std::size_t>(frame) * outputChannels + channel], 16));
                WriteBigEndian(out + frame * 2, value, 2);
            }
            break;
        }
        case ASIOSTInt24MSB: {
            auto* out = static_cast<std::uint8_t*>(buffer);
            for (long frame = 0; frame < bufferFrames; frame++) {
                const auto value = static_cast<std::uint32_t>(FloatToSignedInt(interleaved[static_cast<std::size_t>(frame) * outputChannels + channel], 24));
                WriteBigEndian(out + frame * 3, value, 3);
            }
            break;
        }
        case ASIOSTInt32MSB16: {
            WriteInt32ChannelBuffer(buffer, interleaved, channel, 16, true);
            break;
        }
        case ASIOSTInt32MSB18: {
            WriteInt32ChannelBuffer(buffer, interleaved, channel, 18, true);
            break;
        }
        case ASIOSTInt32MSB20: {
            WriteInt32ChannelBuffer(buffer, interleaved, channel, 20, true);
            break;
        }
        case ASIOSTInt32MSB24: {
            WriteInt32ChannelBuffer(buffer, interleaved, channel, 24, true);
            break;
        }
        case ASIOSTInt32MSB: {
            WriteInt32ChannelBuffer(buffer, interleaved, channel, 32, true);
            break;
        }
        default:
            std::memset(buffer, 0, static_cast<std::size_t>(bufferFrames) * 4);
            break;
        }
    }

    static std::int32_t FloatToSignedInt(float sample, int bits)
    {
        sample = std::clamp(sample, -1.0f, 1.0f);
        const double positive = static_cast<double>((1ULL << (bits - 1)) - 1);
        const double negative = static_cast<double>(1ULL << (bits - 1));
        return static_cast<std::int32_t>(sample >= 0.0f ? sample * positive : sample * negative);
    }

    static void WriteBigEndian(std::uint8_t* output, std::uint64_t value, int bytes)
    {
        for (int i = 0; i < bytes; i++)
            output[i] = static_cast<std::uint8_t>((value >> ((bytes - 1 - i) * 8)) & 0xff);
    }

    void WriteInt32ChannelBuffer(void* buffer, const std::vector<float>& interleaved, WORD channel, int validBits, bool bigEndian) const
    {
        auto* out = static_cast<std::uint8_t*>(buffer);
        const int shift = validBits == 32 ? 0 : 32 - validBits;
        for (long frame = 0; frame < bufferFrames; frame++) {
            std::int32_t value = FloatToSignedInt(interleaved[static_cast<std::size_t>(frame) * outputChannels + channel], validBits);
            value <<= shift;
            if (bigEndian) {
                WriteBigEndian(out + frame * 4, static_cast<std::uint32_t>(value), 4);
            } else {
                auto* intOut = reinterpret_cast<std::int32_t*>(out);
                intOut[frame] = value;
            }
        }
    }

    static void BufferSwitchThunk(long doubleBufferIndex, ASIOBool)
    {
        if (currentInstance != nullptr)
            currentInstance->Render(doubleBufferIndex);
    }

    static void SampleRateDidChangeThunk(ASIOSampleRate)
    {
    }

    static long AsioMessageThunk(long selector, long, void*, double*)
    {
        switch (selector) {
        case kAsioSelectorSupported:
        case kAsioEngineVersion:
            return 2;
        case kAsioSupportsTimeInfo:
            return 1;
        default:
            return 0;
        }
    }

    static ASIOTime* BufferSwitchTimeInfoThunk(ASIOTime* params, long doubleBufferIndex, ASIOBool)
    {
        if (currentInstance != nullptr)
            currentInstance->Render(doubleBufferIndex);
        return params;
    }

    OutputConfig config;
    std::unique_ptr<AsioDrivers> asioDrivers;
    ASIOCallbacks callbacks {};
    std::vector<ASIOBufferInfo> bufferInfos;
    std::vector<ASIOChannelInfo> channelInfos;
    std::wstring driverName;
    std::string driverNameUtf8;
    WORD outputChannels = 2;
    DWORD outputSampleRate = 48000;
    long bufferFrames = 0;
    long reportedOutputLatency = 0;
    std::atomic<bool> running = false;
    bool initialized = false;
    bool buffersCreated = false;
    std::mutex mutex;
    std::deque<FloatChunk> streamChunks;
    std::vector<FloatChunk> activeClips;
    std::size_t streamQueuedSamples = 0;
    static inline NativeAsioOutput* currentInstance = nullptr;
};

class AudioMirror {
public:
    explicit AudioMirror(OutputConfig config = {})
        : config(std::move(config))
    {
    }
    AudioMirror(const AudioMirror&) = delete;
    AudioMirror& operator=(const AudioMirror&) = delete;

    ~AudioMirror()
    {
        DestroyAllPcmStreams();
        output.reset();

        for (VoiceBuffer* voice : activeVoices) {
            if (voice == nullptr)
                continue;
            if (voice->voice != nullptr) {
                voice->voice->Stop(0);
                voice->voice->DestroyVoice();
            }
            delete voice;
        }

        if (masteringVoice != nullptr) {
            masteringVoice->DestroyVoice();
            masteringVoice = nullptr;
        }

        if (engine != nullptr) {
            engine->StopEngine();
            engine.Reset();
        }

        if (comInitialized)
            CoUninitialize();
    }

    bool Start(std::wstring& error)
    {
        if (config.backend == OutputBackend::WasapiExclusive) {
            output = std::make_unique<WasapiExclusiveOutput>(config);
            if (!output->Start(error)) {
                output.reset();
                return false;
            }

            active = true;
            return true;
        }

        if (config.backend == OutputBackend::Asio) {
            output = std::make_unique<NativeAsioOutput>(config);
            if (!output->Start(error)) {
                output.reset();
                return false;
            }

            active = true;
            return true;
        }

        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr)) {
            comInitialized = true;
        } else if (hr != RPC_E_CHANGED_MODE) {
            error = FormatHresult(L"CoInitializeEx", hr);
            return false;
        }

        hr = XAudio2Create(&engine, 0, XAUDIO2_DEFAULT_PROCESSOR);
        if (FAILED(hr)) {
            error = FormatHresult(L"XAudio2Create", hr);
            return false;
        }

        hr = engine->CreateMasteringVoice(
            &masteringVoice,
            XAUDIO2_DEFAULT_CHANNELS,
            XAUDIO2_DEFAULT_SAMPLERATE,
            0,
            config.deviceId.empty() ? nullptr : config.deviceId.c_str());
        if (FAILED(hr)) {
            error = FormatHresult(L"CreateMasteringVoice", hr);
            return false;
        }

        active = true;
        return true;
    }

    bool IsActive() const
    {
        return active;
    }

    bool LoadMemorySample(
        std::uint64_t sample,
        const std::uint8_t* data,
        std::uint64_t length,
        ProbeLog& log)
    {
        if (!active || data == nullptr || length == 0)
            return false;

        olab::DecodedSample decoded;
        std::string error;
        if (!olab::DecodeSampleMemory(data, length, decoded, error)) {
            std::wstringstream line;
            line << L"MirrorDecodeFailed sample=0x" << std::hex << sample << std::dec
                 << L" bytes=" << length
                 << L" error=\"" << error.c_str() << L"\"";
            PrintAndLogLine(line.str(), log);
            return false;
        }

        decodedSamples[sample] = std::move(decoded);
        const olab::DecodedSample& stored = decodedSamples[sample];
        std::wstringstream line;
        line << L"MirrorDecodedSample sample=0x" << std::hex << sample << std::dec
             << L" containerBytes=" << length
             << L" format=\"" << stored.format.c_str() << L"\""
             << L" sampleRate=" << stored.sampleRate
             << L" channels=" << stored.channels
             << L" pcmSamples=" << stored.frames.size();
        PrintAndLogLine(line.str(), log);
        return true;
    }

    bool LoadPcmSample(
        std::uint64_t sample,
        const std::uint8_t* data,
        std::uint64_t byteLength,
        std::uint32_t sampleRate,
        std::uint32_t channels,
        ProbeLog& log)
    {
        if (!active || data == nullptr || byteLength == 0 || sampleRate == 0 || channels == 0)
            return false;

        const std::size_t samples = static_cast<std::size_t>(byteLength / sizeof(std::int16_t));
        if (samples == 0)
            return false;

        const auto* source = reinterpret_cast<const std::int16_t*>(data);
        olab::DecodedSample decoded;
        decoded.sampleRate = sampleRate;
        decoded.channels = channels;
        decoded.format = "pcm16";
        decoded.frames.resize(samples);
        for (std::size_t i = 0; i < samples; i++)
            decoded.frames[i] = source[i] / 32768.0f;

        decodedSamples[sample] = std::move(decoded);
        const olab::DecodedSample& stored = decodedSamples[sample];
        std::wstringstream line;
        line << L"MirrorDecodedPcmSample sample=0x" << std::hex << sample << std::dec
             << L" bytes=" << byteLength
             << L" sampleRate=" << stored.sampleRate
             << L" channels=" << stored.channels
             << L" pcmSamples=" << stored.frames.size();
        PrintAndLogLine(line.str(), log);
        return true;
    }

    bool PlaySample(std::uint64_t sample, std::uint64_t channel, float volume, ProbeLog& log)
    {
        if (!active)
            return false;

        auto found = decodedSamples.find(sample);
        if (found == decodedSamples.end())
            return false;

        const olab::DecodedSample& decoded = found->second;
        if (decoded.frames.empty() || decoded.channels == 0 || decoded.sampleRate == 0)
            return false;

        std::vector<std::int16_t> pcm16(decoded.frames.size());
        for (std::size_t i = 0; i < decoded.frames.size(); i++) {
            float clamped = std::clamp(decoded.frames[i], -1.0f, 1.0f);
            pcm16[i] = static_cast<std::int16_t>(clamped >= 0
                ? clamped * 32767.0f
                : clamped * 32768.0f);
        }

        std::wstringstream line;
        line << L"MirrorPlayedSample sample=0x" << std::hex << sample << std::dec
             << L" channel=0x" << std::hex << channel << std::dec
             << L" sampleRate=" << decoded.sampleRate
             << L" channels=" << decoded.channels
             << L" pcmSamples=" << decoded.frames.size()
             << L" volume=" << volume;
        return PlayPcm16(std::move(pcm16), static_cast<WORD>(decoded.channels), decoded.sampleRate, channel, volume, line.str(), log);
    }

    bool LoadMusicStream(
        std::uint64_t stream,
        const std::uint8_t* data,
        std::uint64_t length,
        ProbeLog& log)
    {
        if (!active || data == nullptr || length == 0)
            return false;

        olab::DecodedSample decoded;
        std::string error;
        if (!olab::DecodeSampleMemory(data, length, decoded, error)) {
            std::wstringstream line;
            line << L"MirrorMusicDecodeFailed stream=0x" << std::hex << stream << std::dec
                 << L" bytes=" << length
                 << L" error=\"" << error.c_str() << L"\"";
            PrintAndLogLine(line.str(), log);
            return false;
        }

        musicStreams[stream] = std::move(decoded);
        const olab::DecodedSample& stored = musicStreams[stream];
        std::wstringstream line;
        line << L"MirrorMusicDecoded stream=0x" << std::hex << stream << std::dec
             << L" containerBytes=" << length
             << L" format=\"" << stored.format.c_str() << L"\""
             << L" sampleRate=" << stored.sampleRate
             << L" channels=" << stored.channels
             << L" pcmSamples=" << stored.frames.size();
        PrintAndLogLine(line.str(), log);
        return true;
    }

    bool PlayMusicStream(std::uint64_t stream, bool restart, float volume, ProbeLog& log)
    {
        return PlayMusicStream(stream, stream, restart, volume, log);
    }

    bool PlayMusicStream(std::uint64_t stream, std::uint64_t playbackChannel, bool restart, float volume, ProbeLog& log)
    {
        if (!active)
            return false;

        auto found = musicStreams.find(stream);
        if (found == musicStreams.end())
            return false;

        StopVoicesForChannel(playbackChannel);
        StopPcmStreamForChannel(playbackChannel);

        const olab::DecodedSample& decoded = found->second;
        if (decoded.frames.empty() || decoded.channels == 0 || decoded.sampleRate == 0)
            return false;

        std::vector<std::int16_t> pcm16(decoded.frames.size());
        for (std::size_t i = 0; i < decoded.frames.size(); i++) {
            float clamped = std::clamp(decoded.frames[i], -1.0f, 1.0f);
            pcm16[i] = static_cast<std::int16_t>(clamped >= 0
                ? clamped * 32767.0f
                : clamped * 32768.0f);
        }

        std::wstringstream line;
        line << L"MirrorMusicPlayed stream=0x" << std::hex << stream << std::dec
             << L" playbackChannel=0x" << std::hex << playbackChannel << std::dec
             << L" restart=" << (restart ? 1 : 0)
             << L" sampleRate=" << decoded.sampleRate
             << L" channels=" << decoded.channels
             << L" pcmSamples=" << decoded.frames.size()
             << L" volume=" << volume;
        if (output != nullptr) {
            output->StopChannel(playbackChannel);
            return SubmitOutputPcm(playbackChannel, std::move(pcm16), static_cast<WORD>(decoded.channels), decoded.sampleRate, volume, true, line.str(), log);
        }

        return PlayPcm16(std::move(pcm16), static_cast<WORD>(decoded.channels), decoded.sampleRate, playbackChannel, volume, line.str(), log);
    }

    bool PlayPcmChunk(
        std::uint64_t handle,
        const std::uint8_t* data,
        std::uint64_t byteLength,
        std::uint64_t requestFlags,
        std::uint32_t sampleRate,
        std::uint32_t channels,
        std::uint32_t flags,
        float volume,
        ProbeLog& log)
    {
        if (!active || data == nullptr || byteLength == 0 || sampleRate == 0 || channels == 0)
            return false;

        static constexpr std::uint32_t BassSampleFloat = 0x100;
        static constexpr std::uint64_t BassDataFloat = 0x40000000;
        static constexpr std::uint64_t BassDataFft = 0x80000000;
        static constexpr std::uint64_t BassDataSizeMask = 0x0fffffff;
        if ((requestFlags & BassDataFft) != 0)
            return false;
        if ((requestFlags & BassDataSizeMask) == 0)
            return false;

        std::vector<std::int16_t> pcm16;
        if ((flags & BassSampleFloat) != 0 || (requestFlags & BassDataFloat) != 0) {
            const std::size_t samples = static_cast<std::size_t>(byteLength / sizeof(float));
            const auto* source = reinterpret_cast<const float*>(data);
            pcm16.resize(samples);
            for (std::size_t i = 0; i < samples; i++) {
                const float clamped = std::clamp(source[i], -1.0f, 1.0f);
                pcm16[i] = static_cast<std::int16_t>(clamped >= 0
                    ? clamped * 32767.0f
                    : clamped * 32768.0f);
            }
        } else {
            const std::size_t samples = static_cast<std::size_t>(byteLength / sizeof(std::int16_t));
            const auto* source = reinterpret_cast<const std::int16_t*>(data);
            pcm16.assign(source, source + samples);
        }

        if (pcm16.empty())
            return false;

        std::wstringstream line;
        line << L"MirrorPcmChunkQueued handle=0x" << std::hex << handle << std::dec
             << L" bytes=" << byteLength
             << L" request=0x" << std::hex << requestFlags << std::dec
             << L" sampleRate=" << sampleRate
             << L" channels=" << channels
             << L" flags=0x" << std::hex << flags << std::dec
             << L" samples=" << pcm16.size()
             << L" volume=" << volume;
        return QueuePcmChunk(std::move(pcm16), static_cast<WORD>(channels), sampleRate, handle, volume, line.str(), log);
    }

    bool SeekMusicStream(std::uint64_t stream, std::uint64_t bytePosition, float volume, ProbeLog& log)
    {
        return SeekMusicStream(stream, stream, bytePosition, volume, log);
    }

    bool SeekMusicStream(std::uint64_t stream, std::uint64_t playbackChannel, std::uint64_t bytePosition, float volume, ProbeLog& log)
    {
        if (!active)
            return false;

        auto found = musicStreams.find(stream);
        if (found == musicStreams.end())
            return false;

        const olab::DecodedSample& decoded = found->second;
        if (decoded.frames.empty() || decoded.channels == 0 || decoded.sampleRate == 0)
            return false;

        StopVoicesForChannel(playbackChannel);
        StopPcmStreamForChannel(playbackChannel);

        const std::uint64_t bytesPerFrame = decoded.channels * sizeof(float);
        const std::uint64_t startFrame = bytesPerFrame == 0
            ? 0
            : bytePosition / bytesPerFrame;
        const std::uint64_t startSample = std::min<std::uint64_t>(
            startFrame * decoded.channels,
            static_cast<std::uint64_t>(decoded.frames.size()));

        std::vector<std::int16_t> pcm16(decoded.frames.size() - static_cast<std::size_t>(startSample));
        for (std::size_t i = 0; i < pcm16.size(); i++) {
            float clamped = std::clamp(decoded.frames[static_cast<std::size_t>(startSample) + i], -1.0f, 1.0f);
            pcm16[i] = static_cast<std::int16_t>(clamped >= 0
                ? clamped * 32767.0f
                : clamped * 32768.0f);
        }

        std::wstringstream line;
        line << L"MirrorMusicSeek stream=0x" << std::hex << stream << std::dec
             << L" playbackChannel=0x" << std::hex << playbackChannel << std::dec
             << L" bytePosition=" << bytePosition
             << L" startFrame=" << startFrame
             << L" remainingSamples=" << pcm16.size()
             << L" volume=" << volume;
        if (output != nullptr) {
            output->StopChannel(playbackChannel);
            return SubmitOutputPcm(playbackChannel, std::move(pcm16), static_cast<WORD>(decoded.channels), decoded.sampleRate, volume, true, line.str(), log);
        }

        return PlayPcm16(std::move(pcm16), static_cast<WORD>(decoded.channels), decoded.sampleRate, playbackChannel, volume, line.str(), log);
    }

    std::uint64_t StopChannel(std::uint64_t channel, const wchar_t* reason, ProbeLog& log)
    {
        if (!active)
            return 0;

        std::uint64_t stopped = StopVoicesForChannel(channel) + StopPcmStreamForChannel(channel);
        if (output != nullptr && output->StopChannel(channel))
            stopped++;
        if (stopped == 0)
            return 0;

        std::wstringstream line;
        line << L"MirrorStoppedChannel channel=0x" << std::hex << channel << std::dec
             << L" voices=" << stopped
             << L" reason=\"" << reason << L"\"";
        PrintAndLogLine(line.str(), log);
        return stopped;
    }

    void SetChannelVolume(std::uint64_t channel, float volume)
    {
        if (!active)
            return;

        if (output != nullptr)
            return;

        CleanupFinishedVoices();
        const float clamped = std::clamp(volume, 0.0f, 1.0f);
        for (VoiceBuffer* voiceBuffer : activeVoices) {
            if (voiceBuffer != nullptr && voiceBuffer->voice != nullptr && voiceBuffer->channel == channel)
                voiceBuffer->voice->SetVolume(clamped);
        }

        auto streamIt = pcmStreams.find(channel);
        if (streamIt != pcmStreams.end() && streamIt->second != nullptr && streamIt->second->voice != nullptr)
            streamIt->second->voice->SetVolume(clamped);
    }

    std::uint64_t StopAllPcmStreams(const wchar_t* reason, ProbeLog& log)
    {
        if (output != nullptr) {
            std::wstringstream line;
            line << L"MirrorOutputStreamReset backend=\"" << output->Name()
                 << L"\" reason=\"" << reason << L"\"";
            output->ResetStream();
            PrintAndLogLine(line.str(), log);
            return 1;
        }

        if (!active || pcmStreams.empty())
            return 0;

        const std::uint64_t stopped = static_cast<std::uint64_t>(pcmStreams.size());
        DestroyAllPcmStreams();

        std::wstringstream line;
        line << L"MirrorStoppedPcmStreams streams=" << stopped
             << L" reason=\"" << reason << L"\"";
        PrintAndLogLine(line.str(), log);
        return stopped;
    }

    std::uint64_t StopAll(const wchar_t* reason, ProbeLog& log)
    {
        if (!active)
            return 0;

        const std::uint64_t stoppedVoices = static_cast<std::uint64_t>(activeVoices.size());
        const std::uint64_t stoppedStreams = static_cast<std::uint64_t>(pcmStreams.size());
        std::uint64_t stoppedOutput = 0;

        for (VoiceBuffer* voice : activeVoices)
            DestroyVoiceBuffer(voice);
        activeVoices.clear();

        DestroyAllPcmStreams();
        if (output != nullptr) {
            output->ResetAll();
            stoppedOutput = 1;
        }

        const std::uint64_t stopped = stoppedVoices + stoppedStreams + stoppedOutput;
        if (stopped != 0) {
            std::wstringstream line;
            line << L"MirrorStoppedAll reason=\"" << reason << L"\""
                 << L" voices=" << stoppedVoices
                 << L" streams=" << stoppedStreams
                 << L" outputReset=" << stoppedOutput;
            PrintAndLogLine(line.str(), log);
        }

        return stopped;
    }

    bool PlayTestTone(ProbeLog& log)
    {
        if (!active)
            return false;

        constexpr DWORD sampleRate = 48000;
        constexpr WORD channels = 2;
        constexpr double frequencyHz = 880.0;
        constexpr double seconds = 0.35;
        constexpr double pi = 3.14159265358979323846;
        const std::size_t frameCount = static_cast<std::size_t>(sampleRate * seconds);

        std::vector<std::int16_t> pcm16(frameCount * channels);
        for (std::size_t frame = 0; frame < frameCount; frame++) {
            const double t = static_cast<double>(frame) / static_cast<double>(sampleRate);
            const double envelope = frame < 480
                ? static_cast<double>(frame) / 480.0
                : frame + 480 > frameCount
                    ? static_cast<double>(frameCount - frame) / 480.0
                    : 1.0;
            const auto sample = static_cast<std::int16_t>(std::sin(2.0 * pi * frequencyHz * t) * envelope * 12000.0);
            pcm16[frame * channels] = sample;
            pcm16[frame * channels + 1] = sample;
        }

        std::wstringstream line;
        line << L"TestTonePlayed sampleRate=" << sampleRate
             << L" channels=" << channels
             << L" frames=" << frameCount;
        return SubmitOutputPcm(0, std::move(pcm16), channels, sampleRate, 1.0f, false, line.str(), log);
    }

private:
    struct VoiceBuffer {
        IXAudio2SourceVoice* voice = nullptr;
        std::uint64_t channel = 0;
        std::vector<std::int16_t> pcm16;
    };

    static void DestroyVoiceBuffer(VoiceBuffer* voiceBuffer)
    {
        if (voiceBuffer == nullptr)
            return;

        if (voiceBuffer->voice != nullptr) {
            voiceBuffer->voice->Stop(0);
            voiceBuffer->voice->FlushSourceBuffers();
            voiceBuffer->voice->DestroyVoice();
        }
        delete voiceBuffer;
    }

    struct PcmStreamVoice {
        IXAudio2SourceVoice* voice = nullptr;
        std::uint64_t channel = 0;
        WORD channels = 0;
        DWORD sampleRate = 0;
        std::deque<std::vector<std::int16_t>> queuedBuffers;
        std::uint64_t submitted = 0;
        std::uint64_t dropped = 0;
    };

    static std::wstring FormatHresult(const wchar_t* operation, HRESULT hr)
    {
        std::wstringstream line;
        line << L"AudioMirrorError operation=\"" << operation << L"\" hr=0x" << std::hex << static_cast<unsigned long>(hr);
        return line.str();
    }

    bool PlayPcm16(
        std::vector<std::int16_t> pcm16,
        WORD channels,
        DWORD sampleRate,
        std::uint64_t channel,
        float volume,
        const std::wstring& successLine,
        ProbeLog& log)
    {
        if (pcm16.empty() || channels == 0 || sampleRate == 0)
            return false;

        if (output != nullptr)
            return SubmitOutputPcm(channel, std::move(pcm16), channels, sampleRate, volume, false, successLine, log);

        CleanupFinishedVoices();

        auto voiceBuffer = std::make_unique<VoiceBuffer>();
        voiceBuffer->channel = channel;
        voiceBuffer->pcm16 = std::move(pcm16);

        WAVEFORMATEX format {};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = channels;
        format.nSamplesPerSec = sampleRate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = static_cast<WORD>(format.nChannels * sizeof(std::int16_t));
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

        HRESULT hr = engine->CreateSourceVoice(&voiceBuffer->voice, &format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, nullptr);
        if (FAILED(hr)) {
            PrintAndLogLine(FormatHresult(L"CreateSourceVoice", hr), log);
            return false;
        }

        XAUDIO2_BUFFER buffer {};
        buffer.AudioBytes = static_cast<UINT32>(voiceBuffer->pcm16.size() * sizeof(std::int16_t));
        buffer.pAudioData = reinterpret_cast<const BYTE*>(voiceBuffer->pcm16.data());
        buffer.Flags = XAUDIO2_END_OF_STREAM;

        hr = voiceBuffer->voice->SubmitSourceBuffer(&buffer);
        if (FAILED(hr)) {
            PrintAndLogLine(FormatHresult(L"SubmitSourceBuffer", hr), log);
            voiceBuffer->voice->DestroyVoice();
            return false;
        }

        hr = voiceBuffer->voice->Start(0);
        if (FAILED(hr)) {
            PrintAndLogLine(FormatHresult(L"Start", hr), log);
            voiceBuffer->voice->DestroyVoice();
            return false;
        }

        voiceBuffer->voice->SetVolume(std::clamp(volume, 0.0f, 1.0f));
        activeVoices.push_back(voiceBuffer.release());
        PrintAndLogLine(successLine, log);
        return true;
    }

    bool QueuePcmChunk(
        std::vector<std::int16_t> pcm16,
        WORD channels,
        DWORD sampleRate,
        std::uint64_t channel,
        float volume,
        const std::wstring& successLine,
        ProbeLog& log)
    {
        if (pcm16.empty() || channels == 0 || sampleRate == 0)
            return false;

        if (output != nullptr)
            return SubmitOutputPcm(channel, std::move(pcm16), channels, sampleRate, volume, true, successLine, log);

        constexpr UINT32 MaxQueuedBuffers = 4;
        PcmStreamVoice* stream = GetOrCreatePcmStream(channel, channels, sampleRate, log);
        if (stream == nullptr)
            return false;

        TrimCompletedPcmBuffers(*stream);

        XAUDIO2_VOICE_STATE state {};
        stream->voice->GetState(&state);
        if (state.BuffersQueued >= MaxQueuedBuffers) {
            stream->dropped++;
            if (stream->dropped <= 3 || stream->dropped % 100 == 0) {
                std::wstringstream line;
                line << L"MirrorPcmChunkDropped handle=0x" << std::hex << channel << std::dec
                     << L" queued=" << state.BuffersQueued
                     << L" dropped=" << stream->dropped
                     << L" samples=" << pcm16.size();
                PrintAndLogLine(line.str(), log);
            }
            return true;
        }

        stream->queuedBuffers.push_back(std::move(pcm16));
        const auto& queued = stream->queuedBuffers.back();

        XAUDIO2_BUFFER buffer {};
        buffer.AudioBytes = static_cast<UINT32>(queued.size() * sizeof(std::int16_t));
        buffer.pAudioData = reinterpret_cast<const BYTE*>(queued.data());

        HRESULT hr = stream->voice->SubmitSourceBuffer(&buffer);
        if (FAILED(hr)) {
            stream->queuedBuffers.pop_back();
            PrintAndLogLine(FormatHresult(L"SubmitPcmStreamBuffer", hr), log);
            return false;
        }

        stream->voice->SetVolume(std::clamp(volume, 0.0f, 1.0f));
        hr = stream->voice->Start(0);
        if (FAILED(hr)) {
            PrintAndLogLine(FormatHresult(L"StartPcmStream", hr), log);
            return false;
        }

        stream->submitted++;
        if (stream->submitted <= 3 || stream->submitted % 100 == 0) {
            std::wstringstream line;
            line << successLine
                 << L" queued=" << stream->queuedBuffers.size()
                 << L" submitted=" << stream->submitted;
            PrintAndLogLine(line.str(), log);
        }
        return true;
    }

    bool SubmitOutputPcm(
        std::uint64_t channel,
        std::vector<std::int16_t> pcm16,
        WORD channels,
        DWORD sampleRate,
        float volume,
        bool stream,
        const std::wstring& successLine,
        ProbeLog& log)
    {
        if (output == nullptr)
            return false;

        std::wstring error;
        if (!output->Submit(channel, std::move(pcm16), channels, sampleRate, volume, stream, error)) {
            if (!error.empty())
                PrintAndLogLine(error, log);
            return false;
        }

        outputSubmitted++;
        if (outputSubmitted <= 3 || outputSubmitted % 100 == 0) {
            std::wstringstream line;
            line << successLine
                 << L" backend=\"" << output->Name()
                 << L"\" submitted=" << outputSubmitted;
            PrintAndLogLine(line.str(), log);
        }
        return true;
    }

    PcmStreamVoice* GetOrCreatePcmStream(std::uint64_t channel, WORD channels, DWORD sampleRate, ProbeLog& log)
    {
        auto found = pcmStreams.find(channel);
        if (found != pcmStreams.end()) {
            PcmStreamVoice* stream = found->second;
            if (stream != nullptr && stream->channels == channels && stream->sampleRate == sampleRate)
                return stream;

            DestroyPcmStream(stream);
            pcmStreams.erase(found);
        }

        auto stream = std::make_unique<PcmStreamVoice>();
        stream->channel = channel;
        stream->channels = channels;
        stream->sampleRate = sampleRate;

        WAVEFORMATEX format {};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = channels;
        format.nSamplesPerSec = sampleRate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = static_cast<WORD>(format.nChannels * sizeof(std::int16_t));
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

        HRESULT hr = engine->CreateSourceVoice(&stream->voice, &format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, nullptr);
        if (FAILED(hr)) {
            PrintAndLogLine(FormatHresult(L"CreatePcmStreamVoice", hr), log);
            return nullptr;
        }

        PcmStreamVoice* raw = stream.release();
        pcmStreams[channel] = raw;

        std::wstringstream line;
        line << L"MirrorPcmStreamCreated handle=0x" << std::hex << channel << std::dec
             << L" sampleRate=" << sampleRate
             << L" channels=" << channels;
        PrintAndLogLine(line.str(), log);
        return raw;
    }

    void TrimCompletedPcmBuffers(PcmStreamVoice& stream)
    {
        if (stream.voice == nullptr)
            return;

        XAUDIO2_VOICE_STATE state {};
        stream.voice->GetState(&state);
        while (stream.queuedBuffers.size() > state.BuffersQueued)
            stream.queuedBuffers.pop_front();
    }

    static void DestroyPcmStream(PcmStreamVoice* stream)
    {
        if (stream == nullptr)
            return;

        if (stream->voice != nullptr) {
            stream->voice->Stop(0);
            stream->voice->FlushSourceBuffers();
            stream->voice->DestroyVoice();
        }
        delete stream;
    }

    std::uint64_t StopPcmStreamForChannel(std::uint64_t channel)
    {
        auto found = pcmStreams.find(channel);
        if (found == pcmStreams.end())
            return 0;

        DestroyPcmStream(found->second);
        pcmStreams.erase(found);
        return 1;
    }

    void DestroyAllPcmStreams()
    {
        for (auto& [channel, stream] : pcmStreams)
            DestroyPcmStream(stream);

        pcmStreams.clear();
    }

    std::uint64_t StopVoicesForChannel(std::uint64_t channel)
    {
        CleanupFinishedVoices();

        std::uint64_t stopped = 0;
        auto write = activeVoices.begin();
        for (auto read = activeVoices.begin(); read != activeVoices.end(); ++read) {
            VoiceBuffer* voiceBuffer = *read;
            if (voiceBuffer == nullptr)
                continue;

            if (voiceBuffer->channel == channel) {
                DestroyVoiceBuffer(voiceBuffer);
                stopped++;
                continue;
            }

            *write++ = voiceBuffer;
        }

        activeVoices.erase(write, activeVoices.end());
        return stopped;
    }

    void CleanupFinishedVoices()
    {
        auto write = activeVoices.begin();
        for (auto read = activeVoices.begin(); read != activeVoices.end(); ++read) {
            VoiceBuffer* voiceBuffer = *read;
            if (voiceBuffer == nullptr)
                continue;

            XAUDIO2_VOICE_STATE state {};
            voiceBuffer->voice->GetState(&state);
            if (state.BuffersQueued == 0) {
                DestroyVoiceBuffer(voiceBuffer);
                continue;
            }

            *write++ = voiceBuffer;
        }

        activeVoices.erase(write, activeVoices.end());
    }

    ComPtr<IXAudio2> engine;
    IXAudio2MasteringVoice* masteringVoice = nullptr;
    OutputConfig config;
    std::unique_ptr<IAudioOutput> output;
    std::uint64_t outputSubmitted = 0;
    std::vector<VoiceBuffer*> activeVoices;
    std::unordered_map<std::uint64_t, PcmStreamVoice*> pcmStreams;
    std::unordered_map<std::uint64_t, olab::DecodedSample> decodedSamples;
    std::unordered_map<std::uint64_t, olab::DecodedSample> musicStreams;
    bool comInitialized = false;
    bool active = false;
};

struct ProbeState {
    OutputConfig config;
    bool dumpSamples = false;
    std::filesystem::path sampleDumpDirectory;
    std::unordered_map<std::uint64_t, std::uint64_t> sampleLengths;
    std::unordered_map<std::uint64_t, std::uint64_t> sampleBlobOffsets;
    std::unordered_map<std::uint64_t, std::uint64_t> sampleBlobLengths;
    std::unordered_map<std::uint64_t, std::uint64_t> channelSamples;
    std::unordered_map<std::uint64_t, std::uint64_t> channelMixers;
    std::unordered_map<std::uint64_t, std::uint64_t> channelAliases;
    std::unordered_map<std::uint64_t, float> channelVolumes;
    std::unordered_set<std::uint64_t> musicStreams;
    struct ChannelFormat {
        std::uint32_t sampleRate = 0;
        std::uint32_t channels = 0;
        std::uint32_t flags = 0;
        std::uint32_t type = 0;
    };
    std::unordered_map<std::uint64_t, ChannelFormat> channelFormats;
    std::unordered_set<std::uint64_t> liveMusicChannels;
    std::unordered_set<std::uint64_t> pcmMirrorChannels;
    bool mirrorMusic = false;
    std::uint64_t inferredSamplePlaybackCount = 0;
    std::uint64_t mirrorDecodedCount = 0;
    std::uint64_t mirrorPlayedCount = 0;
    std::uint64_t mirrorMissedCount = 0;
    std::uint64_t mirrorStoppedCount = 0;
    std::uint64_t musicDecodedCount = 0;
    std::uint64_t musicPlayedCount = 0;
    std::uint64_t musicMissedCount = 0;
    std::uint64_t lastPrintedMirrorPlayedCount = 0;
    std::uint64_t lastPrintedMirrorMissedCount = 0;
    std::uint64_t lastPrintedMusicPlayedCount = 0;
    std::uint64_t lastPrintedMusicMissedCount = 0;
    AudioMirror* audioMirror = nullptr;
};

void PrintMirrorStats(const ProbeState& state, ProbeLog& log)
{
    if (state.audioMirror == nullptr)
        return;

    std::wstringstream line;
    line << L"MirrorStats decoded=" << state.mirrorDecodedCount
         << L" played=" << state.mirrorPlayedCount
         << L" missed=" << state.mirrorMissedCount
         << L" stopped=" << state.mirrorStoppedCount
         << L" inferred=" << state.inferredSamplePlaybackCount
         << L" musicDecoded=" << state.musicDecodedCount
         << L" musicPlayed=" << state.musicPlayedCount
         << L" musicMissed=" << state.musicMissedCount;
    PrintAndLogLine(line.str(), log);
}

std::uint64_t ResolveAlias(const ProbeState& state, std::uint64_t channel)
{
    std::uint64_t resolved = channel;
    for (int i = 0; i < 8; i++) {
        const auto found = state.channelAliases.find(resolved);
        if (found == state.channelAliases.end() || found->second == resolved)
            break;

        resolved = found->second;
    }

    return resolved;
}

bool UsesLivePcmMusic(OutputBackend backend)
{
    return backend == OutputBackend::Asio
        || backend == OutputBackend::WasapiExclusive;
}

bool UsesDecodedMusicStreams(OutputBackend backend)
{
    return backend == OutputBackend::XAudio2;
}

bool IsLiveMusicHandle(const ProbeState& state, std::uint64_t handle, std::uint64_t resolved)
{
    return state.pcmMirrorChannels.contains(handle)
        || state.pcmMirrorChannels.contains(resolved);
}

float ResolveVolume(const ProbeState& state, std::uint64_t channel, std::uint64_t resolved)
{
    const auto direct = state.channelVolumes.find(channel);
    if (direct != state.channelVolumes.end())
        return direct->second;

    const auto source = state.channelVolumes.find(resolved);
    if (source != state.channelVolumes.end())
        return source->second;

    return 1.0f;
}

float ApplyBridgeVolume(float eventVolume, float bridgeVolume)
{
    return std::clamp(eventVolume, 0.0f, 2.0f) * std::clamp(bridgeVolume, 0.0f, 2.0f);
}

float ResolveMusicVolume(const ProbeState& state, const OutputConfig& config, std::uint64_t channel, std::uint64_t resolved)
{
    return ApplyBridgeVolume(ResolveVolume(state, channel, resolved), config.musicVolume);
}

float ResolveEffectsVolume(const OutputConfig& config)
{
    return std::clamp(config.effectsVolume, 0.0f, 2.0f);
}

void PrintAliasLine(
    const wchar_t* label,
    std::uint64_t alias,
    std::uint64_t source,
    std::uint64_t flags,
    ProbeLog& log)
{
    std::wstringstream line;
    line << label
         << L" alias=0x" << std::hex << alias
         << L" source=0x" << source
         << L" flags=0x" << flags
         << std::dec;
    PrintAndLogLine(line.str(), log);
}

std::uint64_t StopRelatedChannels(
    AudioMirror& audioMirror,
    const ProbeState& state,
    std::uint64_t channel,
    const wchar_t* reason,
    ProbeLog& log)
{
    std::uint64_t stopped = audioMirror.StopChannel(channel, reason, log);
    const std::uint64_t resolved = ResolveAlias(state, channel);
    if (resolved != channel)
        stopped += audioMirror.StopChannel(resolved, reason, log);

    for (const auto& [alias, source] : state.channelAliases) {
        if (alias != channel && alias != resolved && ResolveAlias(state, alias) == resolved)
            stopped += audioMirror.StopChannel(alias, reason, log);
    }

    return stopped;
}

void EraseChannelState(ProbeState& state, std::uint64_t channel)
{
    state.channelSamples.erase(channel);
    state.channelMixers.erase(channel);
    state.channelAliases.erase(channel);
    state.channelVolumes.erase(channel);
    state.channelFormats.erase(channel);
    state.liveMusicChannels.erase(channel);
    state.pcmMirrorChannels.erase(channel);
    state.musicStreams.erase(channel);

    std::vector<std::uint64_t> aliasesToErase;
    for (const auto& [alias, source] : state.channelAliases) {
        if (source == channel)
            aliasesToErase.push_back(alias);
    }
    for (std::uint64_t alias : aliasesToErase)
        state.channelAliases.erase(alias);
}

void EraseDirectChannelState(ProbeState& state, std::uint64_t channel, bool keepMusicMarker)
{
    state.channelSamples.erase(channel);
    state.channelMixers.erase(channel);
    state.channelAliases.erase(channel);
    state.channelVolumes.erase(channel);
    state.channelFormats.erase(channel);
    state.liveMusicChannels.erase(channel);
    state.pcmMirrorChannels.erase(channel);
    if (!keepMusicMarker)
        state.musicStreams.erase(channel);
}

bool HasDependentAliases(const ProbeState& state, std::uint64_t channel)
{
    for (const auto& [alias, source] : state.channelAliases) {
        if (alias != channel && ResolveAlias(state, alias) == channel)
            return true;
    }

    return false;
}

std::uint64_t StopMixerChildren(
    AudioMirror& audioMirror,
    ProbeState& state,
    std::uint64_t mixer,
    const wchar_t* reason,
    ProbeLog& log)
{
    std::vector<std::uint64_t> children;
    for (const auto& [channel, channelMixer] : state.channelMixers) {
        if (channelMixer == mixer)
            children.push_back(channel);
    }

    std::uint64_t stopped = 0;
    for (std::uint64_t child : children) {
        stopped += StopRelatedChannels(audioMirror, state, child, reason, log);
        state.channelMixers.erase(child);
    }

    return stopped;
}

std::uint64_t StopPlaybackHandle(
    AudioMirror& audioMirror,
    ProbeState& state,
    std::uint64_t handle,
    const wchar_t* reason,
    ProbeLog& log)
{
    if (handle == 0)
        return audioMirror.StopAll(reason, log);

    std::uint64_t stopped = StopRelatedChannels(audioMirror, state, handle, reason, log);
    stopped += StopMixerChildren(audioMirror, state, handle, reason, log);
    return stopped;
}

std::uint64_t StopSampleChannels(
    AudioMirror& audioMirror,
    ProbeState& state,
    std::uint64_t sample,
    const wchar_t* reason,
    ProbeLog& log)
{
    std::vector<std::uint64_t> channels;
    for (const auto& [channel, channelSample] : state.channelSamples) {
        if (channelSample == sample)
            channels.push_back(channel);
    }

    std::uint64_t stopped = audioMirror.StopChannel(sample, reason, log);
    for (std::uint64_t channel : channels) {
        stopped += StopPlaybackHandle(audioMirror, state, channel, reason, log);
        EraseChannelState(state, channel);
    }

    state.sampleLengths.erase(sample);
    state.sampleBlobOffsets.erase(sample);
    state.sampleBlobLengths.erase(sample);
    return stopped;
}

void SetRelatedChannelVolumes(
    AudioMirror& audioMirror,
    const ProbeState& state,
    std::uint64_t channel,
    float volume)
{
    audioMirror.SetChannelVolume(channel, volume);
    const std::uint64_t resolved = ResolveAlias(state, channel);
    if (resolved != channel)
        audioMirror.SetChannelVolume(resolved, volume);

    for (const auto& [alias, source] : state.channelAliases) {
        if (alias != channel && alias != resolved && ResolveAlias(state, alias) == resolved)
            audioMirror.SetChannelVolume(alias, volume);
    }
}

void DumpSampleBlob(
    const olab::SharedChannel& channel,
    const olab::EventRecord& record,
    ProbeState& state,
    ProbeLog& log)
{
    if (!state.dumpSamples || record.value2 == 0)
        return;

    const std::uint64_t blobOffset = record.value1;
    const std::uint64_t blobLength = record.value2;
    if (blobOffset + blobLength > olab::SampleBlobCapacity)
        return;

    std::filesystem::create_directories(state.sampleDumpDirectory);
    std::wstringstream name;
    name
        << L"sample-"
        << std::hex << record.value0
        << L"-"
        << std::dec << blobLength
        << L".bin";

    const std::filesystem::path outputPath = state.sampleDumpDirectory / name.str();
    std::ofstream output(outputPath, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!output)
        return;

    output.write(
        reinterpret_cast<const char*>(channel.sampleBlob.data() + blobOffset),
        static_cast<std::streamsize>(blobLength));

    std::wstringstream line;
    line << L"DumpedSample sample=0x" << std::hex << record.value0
         << std::dec << L" bytes=" << blobLength
         << L" path=\"" << std::filesystem::absolute(outputPath).wstring() << L"\"";
    PrintAndLogLine(line.str(), log);
}

std::filesystem::path DefaultLogPath(const std::filesystem::path& logDirectory)
{
    return logDirectory / (L"probe-" + FormatLocalTimestampForFile() + L".log");
}

bool DecodeDumpDirectory(const std::filesystem::path& directory)
{
    if (!std::filesystem::exists(directory)) {
        std::wcerr << L"Decode directory not found: " << directory << L"\n";
        return false;
    }

    std::uint64_t ok = 0;
    std::uint64_t failed = 0;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file() || entry.path().extension() != L".bin")
            continue;

        std::ifstream input(entry.path(), std::ios::binary);
        std::vector<std::uint8_t> bytes(
            (std::istreambuf_iterator<char>(input)),
            std::istreambuf_iterator<char>());

        olab::DecodedSample decoded;
        std::string error;
        if (!olab::DecodeSampleMemory(bytes.data(), bytes.size(), decoded, error)) {
            failed++;
            std::wcout << L"DecodeFailed path=\"" << entry.path().wstring() << L"\" error=\"" << error.c_str() << L"\"\n";
            continue;
        }

        ok++;
        std::wcout
            << L"Decoded path=\"" << entry.path().filename().wstring() << L"\""
            << L" format=\"" << decoded.format.c_str() << L"\""
            << L" sampleRate=" << decoded.sampleRate
            << L" channels=" << decoded.channels
            << L" pcmSamples=" << decoded.frames.size()
            << L"\n";
    }

    std::wcout << L"DecodeSummary ok=" << ok << L" failed=" << failed << L"\n";
    return failed == 0;
}

bool InjectDll(DWORD processId, const std::filesystem::path& dllPath)
{
    const std::wstring path = std::filesystem::absolute(dllPath).wstring();
    HANDLE process = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE,
        processId);
    if (process == nullptr) {
        std::wcerr << L"OpenProcess failed: " << GetLastError() << L"\n";
        return false;
    }

    const SIZE_T bytes = (path.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remotePath == nullptr) {
        std::wcerr << L"VirtualAllocEx failed: " << GetLastError() << L"\n";
        CloseHandle(process);
        return false;
    }

    if (!WriteProcessMemory(process, remotePath, path.c_str(), bytes, nullptr)) {
        std::wcerr << L"WriteProcessMemory failed: " << GetLastError() << L"\n";
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(kernel32, "LoadLibraryW"));
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remotePath, 0, nullptr);
    if (thread == nullptr) {
        std::wcerr << L"CreateRemoteThread failed: " << GetLastError() << L"\n";
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    WaitForSingleObject(thread, INFINITE);
    DWORD remoteResult = 0;
    GetExitCodeThread(thread, &remoteResult);
    CloseHandle(thread);
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    CloseHandle(process);

    // GetExitCodeThread is DWORD-sized even in a 64-bit process, so this is only
    // a null failure check for LoadLibraryW rather than a reliable module handle.
    if (remoteResult == 0) {
        std::wcerr << L"Remote LoadLibraryW returned null.\n";
        return false;
    }

    return true;
}

const wchar_t* BassAttributeName(std::uint64_t attribute)
{
    switch (attribute) {
    case 1:
        return L"FREQ";
    case 2:
        return L"VOL";
    case 3:
        return L"PAN";
    case 0x10000:
        return L"BUFFER";
    case 0x11000:
        return L"GRANULE";
    default:
        return L"?";
    }
}

bool IsHighFrequencyEvent(olab::EventKind kind)
{
    return kind == olab::EventKind::ChannelSetAttribute
        || kind == olab::EventKind::ChannelSetPosition
        || kind == olab::EventKind::ChannelGetData
        || kind == olab::EventKind::ChannelGetInfo;
}

std::wstring FormatEvent(const olab::SharedChannel& channel, const olab::EventRecord& record)
{
    const double milliseconds = channel.qpcFrequency.QuadPart == 0
        ? 0.0
        : static_cast<double>(record.qpc) * 1000.0 / static_cast<double>(channel.qpcFrequency.QuadPart);

    std::wstringstream stream;
    stream
        << L"[" << record.sequence << L"] "
        << milliseconds << L" ms "
        << L"pid=" << record.processId << L" tid=" << record.threadId << L" "
        << olab::ToString(record.kind)
        << L" v0=0x" << std::hex << record.value0
        << L" v1=0x" << record.value1
        << L" v2=0x" << record.value2
        << L" v3=0x" << record.value3
        << std::dec;

    if (record.float0 != 0 || record.float1 != 0)
        stream << L" f0=" << record.float0 << L" f1=" << record.float1;

    if (record.kind == olab::EventKind::ChannelSetAttribute)
        stream << L" attr=" << BassAttributeName(record.value1);

    if (record.text[0] != L'\0')
        stream << L" text=\"" << record.text.data() << L"\"";

    return stream.str();
}

void PrintAndLogEvent(const olab::SharedChannel& channel, const olab::EventRecord& record, ProbeLog& log)
{
    const std::wstring line = FormatEvent(channel, record);
    std::wcout << line << L"\n";
    log.WriteLine(line);
}

void PrintAndLogLine(const std::wstring& line, ProbeLog& log)
{
    std::wcout << line << L"\n";
    log.WriteLine(line);
}

std::wstring FormatInferredSamplePlayback(
    std::uint64_t sequence,
    const olab::EventRecord& mixerRecord,
    std::uint64_t sample,
    std::uint64_t sampleLength,
    std::uint64_t blobOffset,
    std::uint64_t blobLength)
{
    std::wstringstream stream;
    stream
        << L"[" << sequence << L"] "
        << L"InferredSamplePlayback"
        << L" sample=0x" << std::hex << sample
        << L" channel=0x" << mixerRecord.value1
        << L" mixer=0x" << mixerRecord.value0
        << L" flags=0x" << mixerRecord.value2
        << std::dec;

    if (sampleLength != 0)
        stream << L" sampleBytes=" << sampleLength;
    if (blobLength != 0)
        stream << L" blobOffset=" << blobOffset << L" blobBytes=" << blobLength;

    return stream.str();
}

void UpdateProbeStateAndPrintDerived(
    const olab::SharedChannel& channel,
    const olab::EventRecord& record,
    ProbeState& state,
    ProbeLog& log)
{
    switch (record.kind) {
    case olab::EventKind::HookUnloaded:
    case olab::EventKind::BassStop:
    case olab::EventKind::BassFree:
        if (state.audioMirror != nullptr) {
            const std::uint64_t stopped = state.audioMirror->StopAll(olab::ToString(record.kind), log);
            if (stopped != 0) {
                state.mirrorStoppedCount += stopped;
                PrintMirrorStats(state, log);
            }
        }
        if (record.kind == olab::EventKind::BassFree || record.kind == olab::EventKind::HookUnloaded) {
            state.channelSamples.clear();
            state.channelMixers.clear();
            state.channelAliases.clear();
            state.channelVolumes.clear();
            state.channelFormats.clear();
            state.liveMusicChannels.clear();
            state.pcmMirrorChannels.clear();
            state.musicStreams.clear();
        }
        break;

    case olab::EventKind::StreamCreateFileMemory:
        if (state.mirrorMusic && state.audioMirror != nullptr && record.value2 != 0 && record.value1 + record.value2 <= olab::SampleBlobCapacity) {
            if (state.audioMirror->LoadMusicStream(
                record.value0,
                channel.sampleBlob.data() + record.value1,
                record.value2,
                log)) {
                state.musicStreams.insert(record.value0);
                state.musicDecodedCount++;
                PrintMirrorStats(state, log);
            }
        }
        break;

    case olab::EventKind::SampleLoadMemory:
        state.sampleLengths[record.value0] = record.value3;
        state.sampleBlobOffsets[record.value0] = record.value1;
        state.sampleBlobLengths[record.value0] = record.value2;
        if (state.audioMirror != nullptr && record.value2 != 0 && record.value1 + record.value2 <= olab::SampleBlobCapacity) {
            if (state.audioMirror->LoadMemorySample(
                record.value0,
                channel.sampleBlob.data() + record.value1,
                record.value2,
                log)) {
                state.mirrorDecodedCount++;
                PrintMirrorStats(state, log);
            }
        }
        DumpSampleBlob(channel, record, state, log);
        break;

    case olab::EventKind::SampleGetData:
        state.sampleLengths[record.value0] = record.value3;
        state.sampleBlobOffsets[record.value0] = record.value1;
        state.sampleBlobLengths[record.value0] = record.value2;
        if (state.audioMirror != nullptr && record.value2 != 0 && record.value1 + record.value2 <= olab::SampleBlobCapacity) {
            if (state.audioMirror->LoadPcmSample(
                record.value0,
                channel.sampleBlob.data() + record.value1,
                record.value2,
                static_cast<std::uint32_t>(record.float0),
                static_cast<std::uint32_t>(record.float1),
                log)) {
                state.mirrorDecodedCount++;
                PrintMirrorStats(state, log);
            }
        }
        break;

    case olab::EventKind::SampleLoadPath:
        state.sampleLengths[record.value0] = record.value3;
        break;

    case olab::EventKind::SampleFree:
        if (state.audioMirror != nullptr) {
            const std::uint64_t stopped = StopSampleChannels(*state.audioMirror, state, record.value0, L"SampleFree", log);
            if (stopped != 0) {
                state.mirrorStoppedCount += stopped;
                PrintMirrorStats(state, log);
            }
        } else {
            state.sampleLengths.erase(record.value0);
            state.sampleBlobOffsets.erase(record.value0);
            state.sampleBlobLengths.erase(record.value0);
        }
        break;

    case olab::EventKind::SampleGetChannel:
        state.channelSamples[record.value1] = record.value0;
        break;

    case olab::EventKind::FxTempoCreate: {
        const std::uint64_t source = ResolveAlias(state, record.value1);
        state.channelAliases[record.value0] = source;
        state.channelAliases[record.value0 + 1] = source;
        state.liveMusicChannels.insert(source);
        state.liveMusicChannels.insert(record.value0);
        state.liveMusicChannels.insert(record.value0 + 1);
        if (state.mirrorMusic && state.audioMirror != nullptr && !state.pcmMirrorChannels.contains(record.value0 + 1)) {
            const std::uint64_t stopped = state.audioMirror->StopAllPcmStreams(L"FxTempoCreate", log);
            if (stopped != 0)
                state.mirrorStoppedCount += stopped;
            state.pcmMirrorChannels.clear();
        }
        state.pcmMirrorChannels.insert(record.value0 + 1);
        if (state.musicStreams.contains(source)) {
            state.musicStreams.insert(record.value0);
            state.musicStreams.insert(record.value0 + 1);
        }
        PrintAliasLine(L"MirrorAliasFxTempo", record.value0, source, record.value2, log);
        PrintAliasLine(L"MirrorAliasFxTempoPlayable", record.value0 + 1, source, record.value2, log);
        break;
    }

    case olab::EventKind::StreamCreate:
    case olab::EventKind::StreamCreateFileUser:
        state.liveMusicChannels.insert(record.value0);
        break;

    case olab::EventKind::ChannelGetInfo:
        state.channelFormats[record.value0] = ProbeState::ChannelFormat {
            static_cast<std::uint32_t>(record.value1),
            static_cast<std::uint32_t>(record.value2),
            static_cast<std::uint32_t>(record.value3),
            0,
        };
        break;

    case olab::EventKind::ChannelGetData: {
        const std::uint64_t stream = ResolveAlias(state, record.value0);
        if (state.mirrorMusic
            && state.audioMirror != nullptr
            && UsesLivePcmMusic(state.config.backend)
            && record.value2 != 0
            && record.value3 + record.value2 <= olab::SampleBlobCapacity
            && IsLiveMusicHandle(state, record.value0, stream)) {
            const auto formatIt = state.channelFormats.find(record.value0);
            const ProbeState::ChannelFormat format = formatIt == state.channelFormats.end()
                ? ProbeState::ChannelFormat {}
                : formatIt->second;
            const float volume = ResolveMusicVolume(state, state.config, record.value0, stream);
            if (state.audioMirror->PlayPcmChunk(
                    record.value0,
                    channel.sampleBlob.data() + record.value3,
                    record.value2,
                    record.value1,
                    format.sampleRate,
                    format.channels,
                    format.flags,
                    volume,
                    log)) {
                state.musicPlayedCount++;
            } else {
                state.musicMissedCount++;
            }
            const bool shouldPrintMusicStats =
                state.musicPlayedCount <= 3
                || state.musicMissedCount > state.lastPrintedMusicMissedCount
                || state.musicPlayedCount >= state.lastPrintedMusicPlayedCount + 100;
            if (shouldPrintMusicStats) {
                PrintMirrorStats(state, log);
                state.lastPrintedMusicPlayedCount = state.musicPlayedCount;
                state.lastPrintedMusicMissedCount = state.musicMissedCount;
            }
        }
        break;
    }

    case olab::EventKind::ChannelSetAttribute:
        if (record.value1 == 2) {
            state.channelVolumes[record.value0] = std::clamp(record.float0, 0.0f, 1.0f);
            if (state.audioMirror != nullptr)
                SetRelatedChannelVolumes(*state.audioMirror, state, record.value0, record.float0);
        }
        break;

    case olab::EventKind::ChannelSetPosition: {
        const std::uint64_t stream = ResolveAlias(state, record.value0);
        if (state.mirrorMusic
            && state.audioMirror != nullptr
            && UsesDecodedMusicStreams(state.config.backend)
            && state.musicStreams.contains(stream)) {
            const float volume = ResolveMusicVolume(state, state.config, record.value0, stream);
            if (!state.audioMirror->SeekMusicStream(stream, record.value0, record.value1, volume, log))
                state.musicMissedCount++;
            PrintMirrorStats(state, log);
        }
        break;
    }

    case olab::EventKind::ChannelPlay: {
        const std::uint64_t stream = ResolveAlias(state, record.value0);
        if (state.mirrorMusic
            && state.audioMirror != nullptr
            && UsesDecodedMusicStreams(state.config.backend)
            && state.musicStreams.contains(stream)) {
            const float volume = ResolveMusicVolume(state, state.config, record.value0, stream);
            if (state.audioMirror->PlayMusicStream(stream, record.value0, record.value1 != 0, volume, log))
                state.musicPlayedCount++;
            else
                state.musicMissedCount++;
            PrintMirrorStats(state, log);
        } else {
            const std::uint64_t source = ResolveAlias(state, record.value0);
            auto sampleIt = state.channelSamples.find(source);
            if (sampleIt != state.channelSamples.end()
                && state.audioMirror != nullptr
                && !state.channelMixers.contains(record.value0)
                && !state.musicStreams.contains(stream)) {
                const float volume = ResolveEffectsVolume(state.config);
                StopRelatedChannels(*state.audioMirror, state, source, L"ChannelPlay", log);
                if (state.audioMirror->PlaySample(sampleIt->second, source, volume, log))
                    state.mirrorPlayedCount++;
                else
                    state.mirrorMissedCount++;
                PrintMirrorStats(state, log);
            }
        }
        break;
    }

    case olab::EventKind::ChannelPause:
        if (state.audioMirror != nullptr) {
            const std::uint64_t stopped = StopPlaybackHandle(*state.audioMirror, state, record.value0, L"ChannelPause", log);
            if (stopped != 0) {
                state.mirrorStoppedCount += stopped;
                PrintMirrorStats(state, log);
            }
        }
        break;

    case olab::EventKind::ChannelStop:
        if (state.audioMirror != nullptr) {
            const std::uint64_t stopped = StopPlaybackHandle(*state.audioMirror, state, record.value0, L"ChannelStop", log);
            if (stopped != 0) {
                state.mirrorStoppedCount += stopped;
                PrintMirrorStats(state, log);
            }
        }
        break;

    case olab::EventKind::StreamFree:
    case olab::EventKind::MusicFree: {
        const bool keepAliasedMusic = HasDependentAliases(state, record.value0);
        if (state.audioMirror != nullptr) {
            const std::uint64_t stopped = keepAliasedMusic
                ? state.audioMirror->StopChannel(record.value0, olab::ToString(record.kind), log)
                : StopPlaybackHandle(*state.audioMirror, state, record.value0, olab::ToString(record.kind), log);
            if (stopped != 0) {
                state.mirrorStoppedCount += stopped;
                PrintMirrorStats(state, log);
            }
        }
        if (keepAliasedMusic)
            EraseDirectChannelState(state, record.value0, true);
        else
            EraseChannelState(state, record.value0);
        break;
    }

    case olab::EventKind::MixerStreamRemoveChannel:
        state.channelMixers.erase(record.value0);
        if (state.audioMirror != nullptr) {
            const std::uint64_t stopped = StopPlaybackHandle(*state.audioMirror, state, record.value0, L"MixerStreamRemoveChannel", log);
            if (stopped != 0) {
                state.mirrorStoppedCount += stopped;
                PrintMirrorStats(state, log);
            }
        }
        break;

    case olab::EventKind::MixerStreamAddChannel: {
        state.channelMixers[record.value1] = record.value0;
        const std::uint64_t stream = ResolveAlias(state, record.value1);
        if (state.mirrorMusic
            && state.audioMirror != nullptr
            && UsesDecodedMusicStreams(state.config.backend)
            && state.musicStreams.contains(stream)) {
            const float volume = ResolveMusicVolume(state, state.config, record.value1, stream);
            if (state.audioMirror->PlayMusicStream(stream, record.value1, false, volume, log))
                state.musicPlayedCount++;
            else
                state.musicMissedCount++;
            PrintMirrorStats(state, log);
            break;
        }

        const std::uint64_t source = ResolveAlias(state, record.value1);
        auto sampleIt = state.channelSamples.find(source);
        if (sampleIt == state.channelSamples.end())
            break;

        const std::uint64_t sample = sampleIt->second;
        const auto lengthIt = state.sampleLengths.find(sample);
        const std::uint64_t sampleLength = lengthIt == state.sampleLengths.end()
            ? 0
            : lengthIt->second;
        const auto blobOffsetIt = state.sampleBlobOffsets.find(sample);
        const std::uint64_t blobOffset = blobOffsetIt == state.sampleBlobOffsets.end()
            ? 0
            : blobOffsetIt->second;
        const auto blobLengthIt = state.sampleBlobLengths.find(sample);
        const std::uint64_t blobLength = blobLengthIt == state.sampleBlobLengths.end()
            ? 0
            : blobLengthIt->second;
        state.inferredSamplePlaybackCount++;
        if (state.audioMirror != nullptr) {
            const float volume = ResolveEffectsVolume(state.config);
            if (state.audioMirror->PlaySample(sample, source, volume, log))
                state.mirrorPlayedCount++;
            else
                state.mirrorMissedCount++;
            const bool shouldPrintMirrorStats =
                state.mirrorPlayedCount <= 3
                || state.mirrorMissedCount > state.lastPrintedMirrorMissedCount
                || state.mirrorPlayedCount >= state.lastPrintedMirrorPlayedCount + 100;
            if (shouldPrintMirrorStats) {
                PrintMirrorStats(state, log);
                state.lastPrintedMirrorPlayedCount = state.mirrorPlayedCount;
                state.lastPrintedMirrorMissedCount = state.mirrorMissedCount;
            }
        }
        if (state.inferredSamplePlaybackCount <= 3 || state.inferredSamplePlaybackCount % 100 == 0) {
            PrintAndLogLine(
                FormatInferredSamplePlayback(state.inferredSamplePlaybackCount, record, sample, sampleLength, blobOffset, blobLength),
                log);
        }
        break;
    }

    default:
        break;
    }
}

void Listen(const SharedHandles& handles, bool verbose, ProbeLog& log, ProbeState state)
{
    LONG64 nextSequence = 1;
    std::wcout << L"Listening for BASS probe events. Press Ctrl+C to stop.\n";
    if (log.IsEnabled())
        std::wcout << L"Writing probe log: " << log.Path() << L"\n";

    while (true) {
        WaitForSingleObject(handles.event, 1000);
        const LONG64 writeSequence = handles.channel->writeSequence;
        while (nextSequence <= writeSequence) {
            const olab::EventRecord& record = handles.channel->events[static_cast<std::size_t>(nextSequence % olab::EventCapacity)];
            if (record.sequence == nextSequence) {
                UpdateProbeStateAndPrintDerived(*handles.channel, record, state, log);
                if (verbose || !IsHighFrequencyEvent(record.kind))
                    PrintAndLogEvent(*handles.channel, record, log);
            }
            nextSequence++;
        }
    }
}

} // namespace

int wmain(int argc, wchar_t* argv[])
{
    std::optional<DWORD> pid;
    std::wstring processName = L"osu!.exe";
    bool inject = true;
    bool verbose = false;
    bool logEnabled = false;
    bool mirrorAudio = false;
    bool mirrorMusic = false;
    bool testTone = false;
    bool asioControlPanel = false;
    OutputConfig outputConfig;
    std::filesystem::path dllPath = CurrentExeDirectory() / L"OsuLazerBassHook.dll";
    std::filesystem::path logDirectory = std::filesystem::current_path() / L"logs";
    std::optional<std::filesystem::path> explicitLogPath;
    ProbeState probeState;
    std::optional<std::filesystem::path> decodeDirectory;

    for (int i = 1; i < argc; i++) {
        const std::wstring arg = argv[i];
        if (arg == L"--help" || arg == L"-h") {
            PrintUsage();
            return 0;
        }
        if (arg == L"--pid" && i + 1 < argc) {
            pid = static_cast<DWORD>(std::wcstoul(argv[++i], nullptr, 10));
            continue;
        }
        if (arg == L"--process" && i + 1 < argc) {
            processName = argv[++i];
            continue;
        }
        if (arg == L"--dll" && i + 1 < argc) {
            dllPath = argv[++i];
            continue;
        }
        if (arg == L"--no-inject") {
            inject = false;
            continue;
        }
        if (arg == L"--verbose") {
            verbose = true;
            continue;
        }
        if (arg == L"--log" && i + 1 < argc) {
            explicitLogPath = argv[++i];
            logEnabled = true;
            continue;
        }
        if (arg == L"--log-dir" && i + 1 < argc) {
            logDirectory = argv[++i];
            logEnabled = true;
            continue;
        }
        if (arg == L"--no-log") {
            logEnabled = false;
            explicitLogPath.reset();
            continue;
        }
        if (arg == L"--dump-samples" && i + 1 < argc) {
            probeState.dumpSamples = true;
            probeState.sampleDumpDirectory = argv[++i];
            continue;
        }
        if (arg == L"--mirror-audio") {
            mirrorAudio = true;
            continue;
        }
        if (arg == L"--mirror-music") {
            mirrorMusic = true;
            continue;
        }
        if (arg == L"--output-backend" && i + 1 < argc) {
            const auto parsedBackend = ParseOutputBackend(argv[++i]);
            if (!parsedBackend) {
                std::wcerr << L"Unknown output backend: " << argv[i] << L"\n";
                PrintUsage();
                return 1;
            }
            outputConfig.backend = *parsedBackend;
            continue;
        }
        if (arg == L"--output-device" && i + 1 < argc) {
            outputConfig.deviceId = argv[++i];
            continue;
        }
        if (arg == L"--output-sample-rate" && i + 1 < argc) {
            outputConfig.sampleRate = static_cast<DWORD>(std::wcstoul(argv[++i], nullptr, 10));
            continue;
        }
        if (arg == L"--output-channels" && i + 1 < argc) {
            outputConfig.channels = static_cast<WORD>(std::wcstoul(argv[++i], nullptr, 10));
            continue;
        }
        if (arg == L"--output-buffer-ms" && i + 1 < argc) {
            outputConfig.bufferMs = static_cast<DWORD>(std::wcstoul(argv[++i], nullptr, 10));
            continue;
        }
        if (arg == L"--effects-volume" && i + 1 < argc) {
            outputConfig.effectsVolume = std::clamp(static_cast<float>(std::wcstod(argv[++i], nullptr)) / 100.0f, 0.0f, 2.0f);
            continue;
        }
        if (arg == L"--music-volume" && i + 1 < argc) {
            outputConfig.musicVolume = std::clamp(static_cast<float>(std::wcstod(argv[++i], nullptr)) / 100.0f, 0.0f, 2.0f);
            continue;
        }
        if (arg == L"--list-output-devices") {
            return ListOutputDevices() ? 0 : 1;
            continue;
        }
        if (arg == L"--list-asio-devices") {
            return ListAsioDevices() ? 0 : 1;
            continue;
        }
        if (arg == L"--asio-control-panel") {
            asioControlPanel = true;
            inject = false;
            continue;
        }
        if (arg == L"--decode-dir" && i + 1 < argc) {
            decodeDirectory = argv[++i];
            continue;
        }
        if (arg == L"--test-tone") {
            testTone = true;
            inject = false;
            continue;
        }

        PrintUsage();
        return 1;
    }

    if (decodeDirectory)
        return DecodeDumpDirectory(*decodeDirectory) ? 0 : 1;

    if (asioControlPanel)
        return OpenAsioControlPanel(outputConfig) ? 0 : 1;

    try {
        ProbeLog log(logEnabled
            ? std::optional<std::filesystem::path>(explicitLogPath.value_or(DefaultLogPath(logDirectory)))
            : std::nullopt);
        if (testTone) {
            AudioMirror audioMirror(outputConfig);
            std::wstring mirrorError;
            if (!audioMirror.Start(mirrorError)) {
                if (mirrorError.empty())
                    mirrorError = L"AudioMirrorError operation=\"AudioMirror.Start\"";
                PrintAndLogLine(mirrorError, log);
                return 1;
            }

            if (!audioMirror.PlayTestTone(log)) {
                PrintAndLogLine(L"AudioMirrorError operation=\"PlayTestTone\"", log);
                return 1;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            return 0;
        }

        SharedHandles handles = CreateSharedChannel();
        probeState.config = outputConfig;
        AudioMirror audioMirror(outputConfig);
        if (mirrorAudio || mirrorMusic) {
            std::wstring mirrorError;
            if (!audioMirror.Start(mirrorError)) {
                PrintAndLogLine(mirrorError, log);
                return 1;
            }

            probeState.audioMirror = &audioMirror;
            PrintAndLogLine(L"Audio output backend: " + OutputBackendName(outputConfig.backend), log);
            std::wstringstream outputLine;
            outputLine << L"Audio output config sampleRate=" << outputConfig.sampleRate
                       << L" channels=" << outputConfig.channels
                       << L" bufferMs=" << outputConfig.bufferMs
                       << L" effectsVolume=" << static_cast<int>(std::lround(outputConfig.effectsVolume * 100.0f))
                       << L" musicVolume=" << static_cast<int>(std::lround(outputConfig.musicVolume * 100.0f))
                       << L" device=" << (outputConfig.deviceId.empty() ? L"(default)" : outputConfig.deviceId);
            PrintAndLogLine(outputLine.str(), log);
            if (mirrorAudio)
                PrintAndLogLine(L"MirrorAudio enabled.", log);
        }

        if (mirrorMusic) {
            probeState.mirrorMusic = true;
            PrintAndLogLine(L"MirrorMusic enabled. Memory stream takeover is active for decoded BASS streams.", log);
        }

        if (inject) {
            if (!std::filesystem::exists(dllPath)) {
                std::wcerr << L"Hook DLL not found: " << dllPath << L"\n";
                return 1;
            }

            if (!pid)
                pid = FindProcessId(processName);

            if (!pid) {
                std::wcerr << L"Process not found: " << processName << L"\n";
                return 1;
            }

            std::wcout << L"Injecting " << dllPath << L" into pid " << *pid << L"...\n";
            if (!InjectDll(*pid, dllPath))
                return 1;

            std::wcout << L"Injection completed.\n";
        }

        Listen(handles, verbose, log, std::move(probeState));
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        return 1;
    }

    return 0;
}
