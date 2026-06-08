#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>
#include <cwchar>

#include <MinHook.h>
#include <olab/SharedChannel.h>

namespace {

using QWORD = unsigned long long;
using HSAMPLE = DWORD;
using HCHANNEL = DWORD;
using HSTREAM = DWORD;

using BASS_SampleLoad_t = HSAMPLE(WINAPI*)(BOOL mem, const void* file, QWORD offset, DWORD length, DWORD max, DWORD flags);
using BASS_SampleCreate_t = HSAMPLE(WINAPI*)(DWORD length, DWORD freq, DWORD channels, DWORD max, DWORD flags);
using BASS_SampleFree_t = BOOL(WINAPI*)(HSAMPLE handle);
using BASS_StreamCreate_t = HSTREAM(WINAPI*)(DWORD freq, DWORD channels, DWORD flags, void* proc, void* user);
using BASS_StreamCreateFile_t = HSTREAM(WINAPI*)(BOOL mem, const void* file, QWORD offset, QWORD length, DWORD flags);
using BASS_StreamCreateFileUser_t = HSTREAM(WINAPI*)(DWORD system, DWORD flags, const void* procs, void* user);
using BASS_StreamFree_t = BOOL(WINAPI*)(HSTREAM handle);
using BASS_MusicFree_t = BOOL(WINAPI*)(DWORD handle);
using BASS_Stop_t = BOOL(WINAPI*)();
using BASS_Free_t = BOOL(WINAPI*)();
using BASS_SampleGetChannel_t = HCHANNEL(WINAPI*)(HSAMPLE handle, BOOL onlyNew);
using BASS_SampleGetInfo_t = BOOL(WINAPI*)(HSAMPLE handle, void* info);
using BASS_SampleGetData_t = BOOL(WINAPI*)(HSAMPLE handle, void* buffer);
using BASS_ChannelPlay_t = BOOL(WINAPI*)(DWORD handle, BOOL restart);
using BASS_ChannelPause_t = BOOL(WINAPI*)(DWORD handle);
using BASS_ChannelStop_t = BOOL(WINAPI*)(DWORD handle);
using BASS_ChannelSetAttribute_t = BOOL(WINAPI*)(DWORD handle, DWORD attrib, float value);
using BASS_ChannelSetPosition_t = BOOL(WINAPI*)(DWORD handle, QWORD position, DWORD mode);
using BASS_ChannelGetInfo_t = BOOL(WINAPI*)(DWORD handle, void* info);
using BASS_ChannelGetData_t = DWORD(WINAPI*)(DWORD handle, void* buffer, DWORD length);
using BASS_Mixer_StreamCreate_t = HSTREAM(WINAPI*)(DWORD freq, DWORD channels, DWORD flags);
using BASS_Mixer_StreamAddChannel_t = BOOL(WINAPI*)(HSTREAM handle, DWORD channel, DWORD flags);
using BASS_Mixer_StreamRemoveChannel_t = BOOL(WINAPI*)(DWORD channel);
using BASS_FX_TempoCreate_t = HSTREAM(WINAPI*)(DWORD channel, DWORD flags);
using BASS_FileCloseProc_t = void(WINAPI*)(void* user);
using BASS_FileLenProc_t = QWORD(WINAPI*)(void* user);
using BASS_FileReadProc_t = DWORD(WINAPI*)(void* buffer, DWORD length, void* user);
using BASS_FileSeekProc_t = BOOL(WINAPI*)(QWORD offset, void* user);

constexpr DWORD BassUnicodeFlag = 0x80000000;
constexpr DWORD BassDataFftFlag = 0x80000000;
constexpr DWORD BassDataSizeMask = 0x0fffffff;
constexpr DWORD MaxChannelDataBlobBytes = 1024 * 1024;
constexpr DWORD MaxLazySampleBlobBytes = 8 * 1024 * 1024;

struct BassChannelInfo {
    DWORD freq;
    DWORD chans;
    DWORD flags;
    DWORD ctype;
    DWORD origres;
    void* plugin;
    DWORD sample;
    const char* filename;
};

struct BassSampleInfo {
    DWORD freq;
    float volume;
    float pan;
    DWORD flags;
    DWORD length;
    DWORD max;
    DWORD origres;
    DWORD chans;
    DWORD mingap;
    DWORD mode3d;
    float mindist;
    float maxdist;
    DWORD iangle;
    DWORD oangle;
    float outvol;
    DWORD vam;
    DWORD priority;
};

struct BassFileProcs {
    BASS_FileCloseProc_t close;
    BASS_FileLenProc_t length;
    BASS_FileReadProc_t read;
    BASS_FileSeekProc_t seek;
};

HMODULE thisModule = nullptr;
HANDLE sharedMapping = nullptr;
HANDLE sharedEvent = nullptr;
olab::SharedChannel* channel = nullptr;

BASS_SampleLoad_t originalSampleLoad = nullptr;
BASS_SampleCreate_t originalSampleCreate = nullptr;
BASS_SampleFree_t originalSampleFree = nullptr;
BASS_StreamCreate_t originalStreamCreate = nullptr;
BASS_StreamCreateFile_t originalStreamCreateFile = nullptr;
BASS_StreamCreateFileUser_t originalStreamCreateFileUser = nullptr;
BASS_StreamFree_t originalStreamFree = nullptr;
BASS_MusicFree_t originalMusicFree = nullptr;
BASS_Stop_t originalBassStop = nullptr;
BASS_Free_t originalBassFree = nullptr;
BASS_SampleGetChannel_t originalSampleGetChannel = nullptr;
BASS_SampleGetInfo_t originalSampleGetInfo = nullptr;
BASS_SampleGetData_t originalSampleGetData = nullptr;
BASS_ChannelPlay_t originalChannelPlay = nullptr;
BASS_ChannelPause_t originalChannelPause = nullptr;
BASS_ChannelStop_t originalChannelStop = nullptr;
BASS_ChannelSetAttribute_t originalChannelSetAttribute = nullptr;
BASS_ChannelSetPosition_t originalChannelSetPosition = nullptr;
BASS_ChannelGetInfo_t originalChannelGetInfo = nullptr;
BASS_ChannelGetData_t originalChannelGetData = nullptr;
BASS_Mixer_StreamCreate_t originalMixerStreamCreate = nullptr;
BASS_Mixer_StreamAddChannel_t originalMixerStreamAddChannel = nullptr;
BASS_Mixer_StreamRemoveChannel_t originalMixerStreamRemoveChannel = nullptr;
BASS_FX_TempoCreate_t originalFxTempoCreate = nullptr;
std::mutex publishedSamplesMutex;
std::unordered_set<HSAMPLE> publishedSamples;

HSAMPLE WINAPI BASS_SampleLoad(BOOL mem, const void* file, QWORD offset, DWORD length, DWORD max, DWORD flags);
HSAMPLE WINAPI BASS_SampleCreate(DWORD length, DWORD freq, DWORD channels, DWORD max, DWORD flags);
BOOL WINAPI BASS_SampleFree(HSAMPLE handle);
HSTREAM WINAPI BASS_StreamCreate(DWORD freq, DWORD channels, DWORD flags, void* proc, void* user);
HSTREAM WINAPI BASS_StreamCreateFile(BOOL mem, const void* file, QWORD offset, QWORD length, DWORD flags);
HSTREAM WINAPI BASS_StreamCreateFileUser(DWORD system, DWORD flags, const void* procs, void* user);
BOOL WINAPI BASS_StreamFree(HSTREAM handle);
BOOL WINAPI BASS_MusicFree(DWORD handle);
BOOL WINAPI BASS_Stop();
BOOL WINAPI BASS_Free();
HCHANNEL WINAPI BASS_SampleGetChannel(HSAMPLE handle, BOOL onlyNew);
BOOL WINAPI BASS_ChannelPlay(DWORD handle, BOOL restart);
BOOL WINAPI BASS_ChannelPause(DWORD handle);
BOOL WINAPI BASS_ChannelStop(DWORD handle);
BOOL WINAPI BASS_ChannelSetAttribute(DWORD handle, DWORD attrib, float value);
BOOL WINAPI BASS_ChannelSetPosition(DWORD handle, QWORD position, DWORD mode);
BOOL WINAPI BASS_ChannelGetInfo(DWORD handle, void* info);
DWORD WINAPI BASS_ChannelGetData(DWORD handle, void* buffer, DWORD length);
HSTREAM WINAPI BASS_Mixer_StreamCreate(DWORD freq, DWORD channels, DWORD flags);
BOOL WINAPI BASS_Mixer_StreamAddChannel(HSTREAM handle, DWORD bassChannel, DWORD flags);
BOOL WINAPI BASS_Mixer_StreamRemoveChannel(DWORD bassChannel);
HSTREAM WINAPI BASS_FX_TempoCreate(DWORD bassChannel, DWORD flags);

std::wstring ToWideText(const char* text)
{
    std::wstring result;
    if (text == nullptr)
        return result;

    while (*text != '\0') {
        result.push_back(static_cast<unsigned char>(*text));
        text++;
    }

    return result;
}

std::wstring ToWidePath(const void* file, DWORD flags)
{
    if (file == nullptr)
        return {};

    if ((flags & BassUnicodeFlag) != 0) {
        const auto* wide = static_cast<const wchar_t*>(file);
        if (IsBadStringPtrW(wide, olab::EventTextLength))
            return L"<unreadable wide path>";

        return wide;
    }

    const auto* ansi = static_cast<const char*>(file);
    if (IsBadStringPtrA(ansi, olab::EventTextLength))
        return L"<unreadable ansi path>";

    int length = MultiByteToWideChar(CP_ACP, 0, ansi, -1, nullptr, 0);
    if (length <= 0)
        return L"<invalid ansi path>";

    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_ACP, 0, ansi, -1, result.data(), length);
    if (!result.empty() && result.back() == L'\0')
        result.pop_back();

    return result;
}

void Publish(
    olab::EventKind kind,
    std::uint64_t value0 = 0,
    std::uint64_t value1 = 0,
    std::uint64_t value2 = 0,
    std::uint64_t value3 = 0,
    float float0 = 0,
    float float1 = 0,
    const wchar_t* text = nullptr)
{
    olab::PublishEvent(channel, sharedEvent, kind, value0, value1, value2, value3, float0, float1, text);
}

bool TryCopyFileUserStream(
    const void* procs,
    void* user,
    std::uint64_t& blobOffset,
    std::uint64_t& blobLength,
    std::uint64_t& totalLength)
{
    blobOffset = 0;
    blobLength = 0;
    totalLength = 0;

    if (procs == nullptr || user == nullptr)
        return false;

    const auto* fileProcs = static_cast<const BassFileProcs*>(procs);
    if (fileProcs->length == nullptr || fileProcs->read == nullptr || fileProcs->seek == nullptr)
        return false;

    const QWORD length = fileProcs->length(user);
    if (length == 0 || length > static_cast<QWORD>(olab::MaxSampleBlobBytes))
        return false;

    if (!fileProcs->seek(0, user))
        return false;

    std::vector<std::uint8_t> data(static_cast<std::size_t>(length));
    std::size_t copied = 0;
    while (copied < data.size()) {
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(data.size() - copied, 1024 * 1024));
        const DWORD read = fileProcs->read(data.data() + copied, request, user);
        if (read == 0)
            break;
        copied += read;
    }

    fileProcs->seek(0, user);

    if (copied == 0)
        return false;

    data.resize(copied);
    totalLength = length;
    return olab::TryCopySampleBlob(channel, data.data(), static_cast<std::uint32_t>(data.size()), blobOffset, blobLength);
}

void MarkSamplePublished(HSAMPLE sample)
{
    if (sample == 0)
        return;

    std::lock_guard<std::mutex> lock(publishedSamplesMutex);
    publishedSamples.insert(sample);
}

bool IsSamplePublished(HSAMPLE sample)
{
    std::lock_guard<std::mutex> lock(publishedSamplesMutex);
    return publishedSamples.contains(sample);
}

bool IsPcmChannelDataRequest(DWORD length)
{
    if ((length & BassDataFftFlag) != 0)
        return false;

    return (length & BassDataSizeMask) != 0;
}

void TryPublishLazySampleData(HSAMPLE sample)
{
    if (sample == 0 || IsSamplePublished(sample) || originalSampleGetInfo == nullptr || originalSampleGetData == nullptr)
        return;

    BassSampleInfo info {};
    if (!originalSampleGetInfo(sample, &info) || info.length == 0 || info.length > MaxLazySampleBlobBytes)
        return;

    std::vector<std::uint8_t> data(info.length);
    if (!originalSampleGetData(sample, data.data()))
        return;

    std::uint64_t blobOffset = 0;
    std::uint64_t blobLength = 0;
    olab::TryCopySampleBlob(channel, data.data(), info.length, blobOffset, blobLength);
    Publish(
        olab::EventKind::SampleGetData,
        sample,
        blobOffset,
        blobLength,
        info.length,
        static_cast<float>(info.freq),
        static_cast<float>(info.chans),
        L"lazy sample pcm");
    MarkSamplePublished(sample);
}

bool MapSharedChannel()
{
    sharedMapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, olab::SharedMemoryName);
    if (sharedMapping == nullptr)
        return false;

    channel = static_cast<olab::SharedChannel*>(
        MapViewOfFile(sharedMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(olab::SharedChannel)));
    if (channel == nullptr)
        return false;

    sharedEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, olab::EventName);
    return sharedEvent != nullptr;
}

void HookExport(HMODULE module, const wchar_t* moduleName, const char* name, void* detour, void** original)
{
    void* target = reinterpret_cast<void*>(GetProcAddress(module, name));
    if (target == nullptr) {
        Publish(olab::EventKind::HookInstallFailed, 0, 0, 0, 0, 0, 0, L"missing export");
        return;
    }

    const MH_STATUS status = MH_CreateHook(target, detour, original);
    const std::wstring exportName = std::wstring(moduleName) + L"!" + ToWideText(name);
    if (status == MH_OK) {
        Publish(olab::EventKind::HookInstalled, reinterpret_cast<std::uint64_t>(target), 0, 0, 0, 0, 0, exportName.c_str());
        return;
    }

    Publish(olab::EventKind::HookInstallFailed, reinterpret_cast<std::uint64_t>(target), static_cast<std::uint64_t>(status), 0, 0, 0, 0, exportName.c_str());
}

HMODULE WaitForModule(const wchar_t* moduleName)
{
    HMODULE module = nullptr;
    bool waitingReported = false;
    while (module == nullptr) {
        module = GetModuleHandleW(moduleName);
        if (module != nullptr)
            break;

        if (!waitingReported) {
            Publish(olab::EventKind::BassModuleWaiting, 0, 0, 0, 0, 0, 0, moduleName);
            waitingReported = true;
        }
        Sleep(250);
    }

    return module;
}

DWORD WINAPI WorkerThread(void*)
{
    if (!MapSharedChannel())
        return 0;

    Publish(olab::EventKind::HookLoaded);

    HMODULE bass = WaitForModule(L"bass.dll");

    Publish(olab::EventKind::BassModuleFound, reinterpret_cast<std::uint64_t>(bass), 0, 0, 0, 0, 0, L"bass.dll");

    if (MH_Initialize() != MH_OK) {
        Publish(olab::EventKind::HookInstallFailed, 0, 0, 0, 0, 0, 0, L"MH_Initialize");
        return 0;
    }

    HookExport(bass, L"bass.dll", "BASS_SampleLoad", reinterpret_cast<void*>(&BASS_SampleLoad), reinterpret_cast<void**>(&originalSampleLoad));
    HookExport(bass, L"bass.dll", "BASS_SampleCreate", reinterpret_cast<void*>(&BASS_SampleCreate), reinterpret_cast<void**>(&originalSampleCreate));
    HookExport(bass, L"bass.dll", "BASS_SampleFree", reinterpret_cast<void*>(&BASS_SampleFree), reinterpret_cast<void**>(&originalSampleFree));
    HookExport(bass, L"bass.dll", "BASS_StreamCreate", reinterpret_cast<void*>(&BASS_StreamCreate), reinterpret_cast<void**>(&originalStreamCreate));
    HookExport(bass, L"bass.dll", "BASS_StreamCreateFile", reinterpret_cast<void*>(&BASS_StreamCreateFile), reinterpret_cast<void**>(&originalStreamCreateFile));
    HookExport(bass, L"bass.dll", "BASS_StreamCreateFileUser", reinterpret_cast<void*>(&BASS_StreamCreateFileUser), reinterpret_cast<void**>(&originalStreamCreateFileUser));
    HookExport(bass, L"bass.dll", "BASS_StreamFree", reinterpret_cast<void*>(&BASS_StreamFree), reinterpret_cast<void**>(&originalStreamFree));
    HookExport(bass, L"bass.dll", "BASS_MusicFree", reinterpret_cast<void*>(&BASS_MusicFree), reinterpret_cast<void**>(&originalMusicFree));
    HookExport(bass, L"bass.dll", "BASS_Stop", reinterpret_cast<void*>(&BASS_Stop), reinterpret_cast<void**>(&originalBassStop));
    HookExport(bass, L"bass.dll", "BASS_Free", reinterpret_cast<void*>(&BASS_Free), reinterpret_cast<void**>(&originalBassFree));
    HookExport(bass, L"bass.dll", "BASS_SampleGetChannel", reinterpret_cast<void*>(&BASS_SampleGetChannel), reinterpret_cast<void**>(&originalSampleGetChannel));
    originalSampleGetInfo = reinterpret_cast<BASS_SampleGetInfo_t>(GetProcAddress(bass, "BASS_SampleGetInfo"));
    originalSampleGetData = reinterpret_cast<BASS_SampleGetData_t>(GetProcAddress(bass, "BASS_SampleGetData"));
    HookExport(bass, L"bass.dll", "BASS_ChannelPlay", reinterpret_cast<void*>(&BASS_ChannelPlay), reinterpret_cast<void**>(&originalChannelPlay));
    HookExport(bass, L"bass.dll", "BASS_ChannelPause", reinterpret_cast<void*>(&BASS_ChannelPause), reinterpret_cast<void**>(&originalChannelPause));
    HookExport(bass, L"bass.dll", "BASS_ChannelStop", reinterpret_cast<void*>(&BASS_ChannelStop), reinterpret_cast<void**>(&originalChannelStop));
    HookExport(bass, L"bass.dll", "BASS_ChannelSetAttribute", reinterpret_cast<void*>(&BASS_ChannelSetAttribute), reinterpret_cast<void**>(&originalChannelSetAttribute));
    HookExport(bass, L"bass.dll", "BASS_ChannelSetPosition", reinterpret_cast<void*>(&BASS_ChannelSetPosition), reinterpret_cast<void**>(&originalChannelSetPosition));
    HookExport(bass, L"bass.dll", "BASS_ChannelGetInfo", reinterpret_cast<void*>(&BASS_ChannelGetInfo), reinterpret_cast<void**>(&originalChannelGetInfo));
    HookExport(bass, L"bass.dll", "BASS_ChannelGetData", reinterpret_cast<void*>(&BASS_ChannelGetData), reinterpret_cast<void**>(&originalChannelGetData));

    const MH_STATUS enableStatus = MH_EnableHook(MH_ALL_HOOKS);
    if (enableStatus != MH_OK)
        Publish(olab::EventKind::HookInstallFailed, 0, static_cast<std::uint64_t>(enableStatus), 0, 0, 0, 0, L"MH_EnableHook");

    bool bassmixHooked = false;
    bool bassFxHooked = false;
    while (true) {
        if (!bassmixHooked) {
            if (HMODULE bassmix = GetModuleHandleW(L"bassmix.dll")) {
                Publish(olab::EventKind::BassModuleFound, reinterpret_cast<std::uint64_t>(bassmix), 0, 0, 0, 0, 0, L"bassmix.dll");
                HookExport(bassmix, L"bassmix.dll", "BASS_Mixer_StreamCreate", reinterpret_cast<void*>(&BASS_Mixer_StreamCreate), reinterpret_cast<void**>(&originalMixerStreamCreate));
                HookExport(bassmix, L"bassmix.dll", "BASS_Mixer_StreamAddChannel", reinterpret_cast<void*>(&BASS_Mixer_StreamAddChannel), reinterpret_cast<void**>(&originalMixerStreamAddChannel));
                HookExport(bassmix, L"bassmix.dll", "BASS_Mixer_StreamRemoveChannel", reinterpret_cast<void*>(&BASS_Mixer_StreamRemoveChannel), reinterpret_cast<void**>(&originalMixerStreamRemoveChannel));
                MH_EnableHook(MH_ALL_HOOKS);
                bassmixHooked = true;
            }
        }

        if (!bassFxHooked) {
            if (HMODULE bassFx = GetModuleHandleW(L"bass_fx.dll")) {
                Publish(olab::EventKind::BassModuleFound, reinterpret_cast<std::uint64_t>(bassFx), 0, 0, 0, 0, 0, L"bass_fx.dll");
                HookExport(bassFx, L"bass_fx.dll", "BASS_FX_TempoCreate", reinterpret_cast<void*>(&BASS_FX_TempoCreate), reinterpret_cast<void**>(&originalFxTempoCreate));
                MH_EnableHook(MH_ALL_HOOKS);
                bassFxHooked = true;
            }
        }

        Sleep(250);
    }

    return 0;
}

HSAMPLE WINAPI BASS_SampleLoad(BOOL mem, const void* file, QWORD offset, DWORD length, DWORD max, DWORD flags)
{
    const HSAMPLE sample = originalSampleLoad(mem, file, offset, length, max, flags);
    if (sample == 0)
        return sample;

    if (mem) {
        std::uint64_t blobOffset = 0;
        std::uint64_t blobLength = 0;
        olab::TryCopySampleBlob(channel, file, length, blobOffset, blobLength);
        Publish(
            olab::EventKind::SampleLoadMemory,
            sample,
            blobOffset,
            blobLength,
            length,
            0,
            0,
            L"memory sample");
        MarkSamplePublished(sample);
    } else {
        const std::wstring path = ToWidePath(file, flags);
        Publish(
            olab::EventKind::SampleLoadPath,
            sample,
            reinterpret_cast<std::uint64_t>(file),
            offset,
            length,
            0,
            0,
            path.c_str());
    }

    return sample;
}

HSAMPLE WINAPI BASS_SampleCreate(DWORD length, DWORD freq, DWORD channels, DWORD max, DWORD flags)
{
    const HSAMPLE sample = originalSampleCreate(length, freq, channels, max, flags);
    if (sample != 0) {
        Publish(
            olab::EventKind::SampleCreate,
            sample,
            length,
            freq,
            channels,
            0,
            0,
            L"sample create");
    }

    return sample;
}

BOOL WINAPI BASS_SampleFree(HSAMPLE handle)
{
    Publish(olab::EventKind::SampleFree, handle);
    return originalSampleFree(handle);
}

HSTREAM WINAPI BASS_StreamCreate(DWORD freq, DWORD channels, DWORD flags, void* proc, void* user)
{
    const HSTREAM stream = originalStreamCreate(freq, channels, flags, proc, user);
    if (stream != 0)
        Publish(olab::EventKind::StreamCreate, stream, freq, channels, flags, 0, 0, L"BASS_StreamCreate");

    return stream;
}

HSTREAM WINAPI BASS_StreamCreateFile(BOOL mem, const void* file, QWORD offset, QWORD length, DWORD flags)
{
    const HSTREAM stream = originalStreamCreateFile(mem, file, offset, length, flags);
    if (stream == 0)
        return stream;

    if (mem) {
        std::uint64_t blobOffset = 0;
        std::uint64_t blobLength = 0;
        if (offset <= length && length <= static_cast<QWORD>(olab::MaxSampleBlobBytes))
            olab::TryCopySampleBlob(channel, static_cast<const std::uint8_t*>(file) + offset, static_cast<std::uint32_t>(length), blobOffset, blobLength);

        Publish(
            olab::EventKind::StreamCreateFileMemory,
            stream,
            blobOffset,
            blobLength,
            length,
            0,
            0,
            L"memory stream");
    } else {
        const std::wstring path = ToWidePath(file, flags);
        Publish(
            olab::EventKind::StreamCreateFilePath,
            stream,
            reinterpret_cast<std::uint64_t>(file),
            offset,
            length,
            0,
            0,
            path.c_str());
    }

    return stream;
}

HSTREAM WINAPI BASS_StreamCreateFileUser(DWORD system, DWORD flags, const void* procs, void* user)
{
    std::uint64_t blobOffset = 0;
    std::uint64_t blobLength = 0;
    std::uint64_t totalLength = 0;
    const bool copiedUserStream = TryCopyFileUserStream(procs, user, blobOffset, blobLength, totalLength);

    const HSTREAM stream = originalStreamCreateFileUser(system, flags, procs, user);
    if (stream != 0) {
        Publish(olab::EventKind::StreamCreateFileUser, stream, system, flags, reinterpret_cast<std::uint64_t>(procs), 0, 0, L"BASS_StreamCreateFileUser");
        if (copiedUserStream && blobLength != 0) {
            Publish(
                olab::EventKind::StreamCreateFileMemory,
                stream,
                blobOffset,
                blobLength,
                totalLength,
                0,
                0,
                L"user stream memory");
        }
    }

    return stream;
}

BOOL WINAPI BASS_StreamFree(HSTREAM handle)
{
    Publish(olab::EventKind::StreamFree, handle);
    return originalStreamFree(handle);
}

BOOL WINAPI BASS_MusicFree(DWORD handle)
{
    Publish(olab::EventKind::MusicFree, handle);
    return originalMusicFree(handle);
}

BOOL WINAPI BASS_Stop()
{
    Publish(olab::EventKind::BassStop);
    return originalBassStop();
}

BOOL WINAPI BASS_Free()
{
    Publish(olab::EventKind::BassFree);
    return originalBassFree();
}

HCHANNEL WINAPI BASS_SampleGetChannel(HSAMPLE handle, BOOL onlyNew)
{
    const HCHANNEL bassChannel = originalSampleGetChannel(handle, onlyNew);
    if (bassChannel != 0) {
        TryPublishLazySampleData(handle);
        Publish(
            olab::EventKind::SampleGetChannel,
            handle,
            bassChannel,
            onlyNew ? 1 : 0);
    }

    return bassChannel;
}

BOOL WINAPI BASS_ChannelPlay(DWORD handle, BOOL restart)
{
    Publish(olab::EventKind::ChannelPlay, handle, restart ? 1 : 0);
    return originalChannelPlay(handle, restart);
}

BOOL WINAPI BASS_ChannelPause(DWORD handle)
{
    Publish(olab::EventKind::ChannelPause, handle);
    return originalChannelPause(handle);
}

BOOL WINAPI BASS_ChannelStop(DWORD handle)
{
    Publish(olab::EventKind::ChannelStop, handle);
    return originalChannelStop(handle);
}

BOOL WINAPI BASS_ChannelSetAttribute(DWORD handle, DWORD attrib, float value)
{
    Publish(olab::EventKind::ChannelSetAttribute, handle, attrib, 0, 0, value);
    return originalChannelSetAttribute(handle, attrib, value);
}

BOOL WINAPI BASS_ChannelSetPosition(DWORD handle, QWORD position, DWORD mode)
{
    Publish(olab::EventKind::ChannelSetPosition, handle, position, mode);
    return originalChannelSetPosition(handle, position, mode);
}

BOOL WINAPI BASS_ChannelGetInfo(DWORD handle, void* info)
{
    const BOOL result = originalChannelGetInfo(handle, info);
    if (result && info != nullptr) {
        const auto* words = static_cast<const DWORD*>(info);
        Publish(olab::EventKind::ChannelGetInfo, handle, words[0], words[1], words[2]);
    }

    return result;
}

DWORD WINAPI BASS_ChannelGetData(DWORD handle, void* buffer, DWORD length)
{
    const DWORD result = originalChannelGetData(handle, buffer, length);
    if (result != static_cast<DWORD>(-1)) {
        if (originalChannelGetInfo != nullptr) {
            BassChannelInfo info {};
            if (originalChannelGetInfo(handle, &info))
                Publish(olab::EventKind::ChannelGetInfo, handle, info.freq, info.chans, info.flags, info.ctype);
        }

        std::uint64_t blobOffset = 0;
        std::uint64_t blobLength = 0;
        if (IsPcmChannelDataRequest(length) && buffer != nullptr && result != 0 && result <= MaxChannelDataBlobBytes)
            olab::TryCopySampleBlob(channel, buffer, result, blobOffset, blobLength);

        Publish(olab::EventKind::ChannelGetData, handle, length, blobLength, blobOffset);
    }

    return result;
}

HSTREAM WINAPI BASS_Mixer_StreamCreate(DWORD freq, DWORD channels, DWORD flags)
{
    const HSTREAM stream = originalMixerStreamCreate(freq, channels, flags);
    if (stream != 0)
        Publish(olab::EventKind::MixerStreamCreate, stream, freq, channels, flags);

    return stream;
}

BOOL WINAPI BASS_Mixer_StreamAddChannel(HSTREAM handle, DWORD bassChannel, DWORD flags)
{
    Publish(olab::EventKind::MixerStreamAddChannel, handle, bassChannel, flags);
    return originalMixerStreamAddChannel(handle, bassChannel, flags);
}

BOOL WINAPI BASS_Mixer_StreamRemoveChannel(DWORD bassChannel)
{
    Publish(olab::EventKind::MixerStreamRemoveChannel, bassChannel);
    return originalMixerStreamRemoveChannel(bassChannel);
}

HSTREAM WINAPI BASS_FX_TempoCreate(DWORD bassChannel, DWORD flags)
{
    const HSTREAM stream = originalFxTempoCreate(bassChannel, flags);
    if (stream != 0)
        Publish(olab::EventKind::FxTempoCreate, stream, bassChannel, flags, 0, 0, 0, L"BASS_FX_TempoCreate");

    return stream;
}

void Cleanup()
{
    Publish(olab::EventKind::HookUnloaded);
    Publish(olab::EventKind::ChannelStop, 0, 0, 0, 0, 0, 0, L"cleanup");
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    if (channel != nullptr) {
        UnmapViewOfFile(channel);
        channel = nullptr;
    }

    if (sharedEvent != nullptr) {
        CloseHandle(sharedEvent);
        sharedEvent = nullptr;
    }

    if (sharedMapping != nullptr) {
        CloseHandle(sharedMapping);
        sharedMapping = nullptr;
    }
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        thisModule = module;
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
        if (thread != nullptr)
            CloseHandle(thread);
    } else if (reason == DLL_PROCESS_DETACH) {
        Cleanup();
    }

    return TRUE;
}
