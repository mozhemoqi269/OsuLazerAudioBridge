#include <Windows.h>
#include <Audioclient.h>
#include <Mmdeviceapi.h>
#include <mmreg.h>
#include <avrt.h>
#include <wrl/client.h>

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
#include "HostAudioMirror.h"
#include "HostAudioOutput.h"
#include "HostAsioDevices.h"
#include "HostLogging.h"
#include "HostSharedChannel.h"
#include "HostWin32.h"
#include "SampleDecoder.h"

IASIO* theAsioDriver = nullptr;

namespace {

using Microsoft::WRL::ComPtr;
using olab::host::AudioMirror;
using olab::host::CreateSharedChannel;
using olab::host::CurrentExeDirectory;
using olab::host::DefaultLogPath;
using olab::host::FindProcessId;
using olab::host::InjectDll;
using olab::host::ListAsioDevices;
using olab::host::OpenAsioControlPanel;
using olab::host::OutputBackend;
using olab::host::OutputBackendName;
using olab::host::OutputConfig;
using olab::host::ParseOutputBackend;
using olab::host::ProbeLog;
using olab::host::PrintAndLogLine;
using olab::host::SharedHandles;
using olab::host::UniqueHandle;

std::atomic<bool> g_stopRequested = false;
HANDLE g_consoleStopEvent = nullptr;

BOOL WINAPI ConsoleControlHandler(DWORD controlType)
{
    switch (controlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        g_stopRequested.store(true);
        if (g_consoleStopEvent != nullptr)
            SetEvent(g_consoleStopEvent);
        return TRUE;

    default:
        return FALSE;
    }
}

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
        << L"  --mix-overlap-ducking <0..100> Extra headroom for overlapping hits. Default 25.\n"
        << L"  --asio-control-panel Open the selected ASIO driver's control panel and exit.\n"
        << L"  --decode-dir <dir> Decode dumped sample .bin files and exit.\n"
        << L"  --shutdown-event <name> Exit when this named event is signaled.\n"
        << L"  --test-tone        Play a short output-backend test tone and exit.\n";
}

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
    std::uint64_t playbackEventAgeCount = 0;
    double playbackEventAgeTotalUs = 0.0;
    double playbackEventAgeMaxUs = 0.0;
    std::uint64_t playbackEventAgeSpikeCount = 0;
    std::uint64_t lastPrintedMirrorDecodedCount = 0;
    std::uint64_t lastPrintedMirrorPlayedCount = 0;
    std::uint64_t lastPrintedMirrorMissedCount = 0;
    std::uint64_t lastPrintedMirrorStoppedCount = 0;
    std::uint64_t lastPrintedMusicDecodedCount = 0;
    std::uint64_t lastPrintedMusicPlayedCount = 0;
    std::uint64_t lastPrintedMusicMissedCount = 0;
    AudioMirror* audioMirror = nullptr;
};

void PrintMirrorStats(ProbeState& state, ProbeLog& log)
{
    if (state.audioMirror == nullptr)
        return;

    const bool shouldPrint =
        (state.mirrorDecodedCount != state.lastPrintedMirrorDecodedCount
            && (state.mirrorDecodedCount <= 3 || state.mirrorDecodedCount >= state.lastPrintedMirrorDecodedCount + 50))
        || (state.mirrorPlayedCount != state.lastPrintedMirrorPlayedCount
            && (state.mirrorPlayedCount <= 3 || state.mirrorPlayedCount >= state.lastPrintedMirrorPlayedCount + 100))
        || state.mirrorMissedCount != state.lastPrintedMirrorMissedCount
        || (state.mirrorStoppedCount != state.lastPrintedMirrorStoppedCount
            && (state.mirrorStoppedCount <= 3 || state.mirrorStoppedCount >= state.lastPrintedMirrorStoppedCount + 50))
        || (state.musicDecodedCount != state.lastPrintedMusicDecodedCount
            && (state.musicDecodedCount <= 3 || state.musicDecodedCount >= state.lastPrintedMusicDecodedCount + 25))
        || (state.musicPlayedCount != state.lastPrintedMusicPlayedCount
            && (state.musicPlayedCount <= 3 || state.musicPlayedCount >= state.lastPrintedMusicPlayedCount + 100))
        || state.musicMissedCount != state.lastPrintedMusicMissedCount;
    if (!shouldPrint)
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
    if (state.playbackEventAgeCount != 0) {
        line << L" eventAgeAvgUs=" << std::fixed << std::setprecision(1)
             << (state.playbackEventAgeTotalUs / static_cast<double>(state.playbackEventAgeCount))
             << L" eventAgeMaxUs=" << state.playbackEventAgeMaxUs
             << std::defaultfloat;
    }
    PrintAndLogLine(line.str(), log);

    state.lastPrintedMirrorDecodedCount = state.mirrorDecodedCount;
    state.lastPrintedMirrorPlayedCount = state.mirrorPlayedCount;
    state.lastPrintedMirrorMissedCount = state.mirrorMissedCount;
    state.lastPrintedMirrorStoppedCount = state.mirrorStoppedCount;
    state.lastPrintedMusicDecodedCount = state.musicDecodedCount;
    state.lastPrintedMusicPlayedCount = state.musicPlayedCount;
    state.lastPrintedMusicMissedCount = state.musicMissedCount;
}

double MeasureEventAgeUs(const olab::SharedChannel& channel, const olab::EventRecord& record)
{
    if (record.qpc == 0 || channel.qpcFrequency.QuadPart == 0)
        return -1.0;

    LARGE_INTEGER now {};
    QueryPerformanceCounter(&now);
    const double elapsedTicks = static_cast<double>(now.QuadPart) - static_cast<double>(record.qpc);
    if (elapsedTicks < 0.0)
        return 0.0;

    return elapsedTicks * 1000000.0 / static_cast<double>(channel.qpcFrequency.QuadPart);
}

void ObservePlaybackEventAge(ProbeState& state, double eventAgeUs)
{
    if (eventAgeUs < 0.0)
        return;

    state.playbackEventAgeCount++;
    state.playbackEventAgeTotalUs += eventAgeUs;
    state.playbackEventAgeMaxUs = std::max(state.playbackEventAgeMaxUs, eventAgeUs);
}

void PrintEventAgeSpike(
    ProbeState& state,
    const wchar_t* source,
    const olab::EventRecord& record,
    std::uint64_t sample,
    std::uint64_t playbackChannel,
    double eventAgeUs,
    ProbeLog& log)
{
    if (eventAgeUs < 500.0)
        return;

    state.playbackEventAgeSpikeCount++;
    if (state.playbackEventAgeSpikeCount > 3 && state.playbackEventAgeSpikeCount % 100 != 0)
        return;

    std::wstringstream line;
    line << L"MirrorEventAgeSpike source=\"" << source << L"\""
         << L" count=" << state.playbackEventAgeSpikeCount
         << L" sequence=" << record.sequence
         << L" eventAgeUs=" << std::fixed << std::setprecision(1) << eventAgeUs << std::defaultfloat
         << L" sample=0x" << std::hex << sample
         << L" channel=0x" << playbackChannel
         << L" eventHandle=0x" << record.value0
         << L" eventValue1=0x" << record.value1
         << std::dec
         << L" kind=\"" << olab::ToString(record.kind) << L"\"";
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
    return backend == OutputBackend::Asio;
}

bool UsesDecodedMusicStreams(OutputBackend backend)
{
    return backend == OutputBackend::XAudio2
        || backend == OutputBackend::WasapiExclusive;
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

float ResolveEffectsVolume(const ProbeState& state, const OutputConfig& config, std::uint64_t channel, std::uint64_t resolved)
{
    (void)state;
    (void)channel;
    (void)resolved;
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
        || kind == olab::EventKind::ChannelPlay
        || kind == olab::EventKind::SampleGetChannel
        || kind == olab::EventKind::MixerStreamAddChannel
        || kind == olab::EventKind::MixerStreamRemoveChannel
        || kind == olab::EventKind::StreamCreate
        || kind == olab::EventKind::StreamCreateFileUser
        || kind == olab::EventKind::StreamFree
        || kind == olab::EventKind::MusicFree
        || kind == olab::EventKind::RuntimeConfig
        || kind == olab::EventKind::ChannelGetData
        || kind == olab::EventKind::ChannelGetInfo;
}

bool IsStatusEvent(olab::EventKind kind)
{
    return kind == olab::EventKind::HostStarted
        || kind == olab::EventKind::HookLoaded
        || kind == olab::EventKind::HookUnloaded
        || kind == olab::EventKind::BassModuleWaiting
        || kind == olab::EventKind::BassModuleFound
        || kind == olab::EventKind::HookInstalled
        || kind == olab::EventKind::HookInstallFailed
        || kind == olab::EventKind::BassStop
        || kind == olab::EventKind::BassFree;
}

bool ShouldPrintEvent(olab::EventKind kind, bool verbose, const ProbeLog& log)
{
    if (verbose)
        return true;
    if (!log.IsEnabled())
        return IsStatusEvent(kind);
    return !IsHighFrequencyEvent(kind);
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

    case olab::EventKind::RuntimeConfig:
        state.config.effectsVolume = std::clamp(record.float0, 0.0f, 2.0f);
        state.config.musicVolume = std::clamp(record.float1, 0.0f, 2.0f);
        state.config.mixOverlapDucking = std::clamp(static_cast<float>(record.value0) / 100.0f, 0.0f, 1.0f);
        if (state.audioMirror != nullptr)
            state.audioMirror->SetRuntimeConfig(state.config);
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
                const float volume = ResolveEffectsVolume(state, state.config, record.value0, source);
                const double eventAgeUs = MeasureEventAgeUs(channel, record);
                ObservePlaybackEventAge(state, eventAgeUs);
                PrintEventAgeSpike(state, L"ChannelPlay", record, sampleIt->second, source, eventAgeUs, log);
                StopRelatedChannels(*state.audioMirror, state, source, L"ChannelPlay", log);
                if (state.audioMirror->PlaySample(sampleIt->second, source, volume, eventAgeUs, log))
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
            const float volume = ResolveEffectsVolume(state, state.config, record.value1, source);
            const double eventAgeUs = MeasureEventAgeUs(channel, record);
            ObservePlaybackEventAge(state, eventAgeUs);
            PrintEventAgeSpike(state, L"MixerStreamAddChannel", record, sample, source, eventAgeUs, log);
            if (state.audioMirror->PlaySample(sample, source, volume, eventAgeUs, log))
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

void Listen(const SharedHandles& handles, HANDLE consoleStopEvent, HANDLE shutdownEvent, bool verbose, ProbeLog& log, ProbeState state)
{
    DWORD taskIndex = 0;
    HANDLE mmcssHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (mmcssHandle != nullptr)
        AvSetMmThreadPriority(mmcssHandle, AVRT_PRIORITY_HIGH);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    LONG64 nextSequence = 1;
    std::wcout << L"Listening for BASS probe events. Press Ctrl+C to stop.\n";
    if (log.IsEnabled())
        std::wcout << L"Writing probe log: " << log.Path() << L"\n";

    std::vector<HANDLE> waitHandles;
    waitHandles.push_back(handles.event);
    if (consoleStopEvent != nullptr)
        waitHandles.push_back(consoleStopEvent);
    if (shutdownEvent != nullptr)
        waitHandles.push_back(shutdownEvent);

    while (!g_stopRequested.load()) {
        const DWORD waitResult = WaitForMultipleObjects(
            static_cast<DWORD>(waitHandles.size()),
            waitHandles.data(),
            FALSE,
            1000);
        if (waitResult == WAIT_FAILED) {
            std::wstringstream line;
            line << L"WaitForMultipleObjects failed. GetLastError=" << GetLastError();
            PrintAndLogLine(line.str(), log);
            break;
        }
        if (waitResult >= WAIT_OBJECT_0 + 1 && waitResult < WAIT_OBJECT_0 + waitHandles.size())
            break;

        const LONG64 writeSequence = handles.channel->writeSequence;
        while (nextSequence <= writeSequence) {
            const olab::EventRecord& record = handles.channel->events[static_cast<std::size_t>(nextSequence % olab::EventCapacity)];
            if (record.sequence == nextSequence) {
                UpdateProbeStateAndPrintDerived(*handles.channel, record, state, log);
                if (ShouldPrintEvent(record.kind, verbose, log))
                    PrintAndLogEvent(*handles.channel, record, log);
            }
            nextSequence++;
        }
    }

    PrintAndLogLine(L"Shutdown requested. Releasing audio output.", log);
    if (mmcssHandle != nullptr)
        AvRevertMmThreadCharacteristics(mmcssHandle);
}

} // namespace

int wmain(int argc, wchar_t* argv[])
{
    UniqueHandle consoleStopEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (consoleStopEvent) {
        g_consoleStopEvent = consoleStopEvent.Get();
        SetConsoleCtrlHandler(ConsoleControlHandler, TRUE);
    }

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
    std::optional<std::wstring> shutdownEventName;

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
        if (arg == L"--mix-overlap-ducking" && i + 1 < argc) {
            outputConfig.mixOverlapDucking = std::clamp(static_cast<float>(std::wcstod(argv[++i], nullptr)) / 100.0f, 0.0f, 1.0f);
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
        if (arg == L"--shutdown-event" && i + 1 < argc) {
            shutdownEventName = argv[++i];
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

    UniqueHandle shutdownEvent;
    if (shutdownEventName) {
        shutdownEvent.Reset(OpenEventW(SYNCHRONIZE, FALSE, shutdownEventName->c_str()));
        if (!shutdownEvent) {
            std::wcerr << L"Could not open shutdown event: " << *shutdownEventName << L" GetLastError=" << GetLastError() << L"\n";
            return 1;
        }
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

            HANDLE waitHandles[2] {};
            DWORD waitCount = 0;
            if (consoleStopEvent)
                waitHandles[waitCount++] = consoleStopEvent.Get();
            if (shutdownEvent)
                waitHandles[waitCount++] = shutdownEvent.Get();
            if (waitCount == 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            else
                WaitForMultipleObjects(waitCount, waitHandles, FALSE, 1000);
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
                       << L" mixOverlapDucking=" << static_cast<int>(std::lround(outputConfig.mixOverlapDucking * 100.0f))
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

        Listen(handles, consoleStopEvent.Get(), shutdownEvent.Get(), verbose, log, std::move(probeState));
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        return 1;
    }

    return 0;
}
