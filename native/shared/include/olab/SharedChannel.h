#pragma once

#include <Windows.h>
#include <array>
#include <cstdint>
#include <cwchar>
#include <cstring>

namespace olab {

inline constexpr wchar_t SharedMemoryName[] = L"Local\\OsuLazerAudioBridge.Channel.v1";
inline constexpr wchar_t EventName[] = L"Local\\OsuLazerAudioBridge.Events.v1";

inline constexpr std::uint32_t ProtocolMagic = 0x42414c4f; // OLAB
inline constexpr std::uint32_t ProtocolVersion = 1;
inline constexpr std::uint32_t EventCapacity = 4096;
inline constexpr std::uint32_t EventTextLength = 260;
inline constexpr std::uint32_t SampleBlobCapacity = 64 * 1024 * 1024;
inline constexpr std::uint32_t MaxSampleBlobBytes = 32 * 1024 * 1024;

enum class EventKind : std::uint32_t {
    HostStarted = 1,
    HookLoaded,
    HookUnloaded,
    BassModuleWaiting,
    BassModuleFound,
    HookInstalled,
    HookInstallFailed,
    SampleLoadPath,
    SampleLoadMemory,
    SampleCreate,
    StreamCreateFilePath,
    StreamCreateFileMemory,
    SampleGetChannel,
    ChannelPlay,
    ChannelPause,
    ChannelStop,
    ChannelSetAttribute,
    ChannelSetPosition,
    MixerStreamCreate,
    MixerStreamAddChannel,
    MixerStreamRemoveChannel,
    FxTempoCreate,
    StreamCreate,
    StreamCreateFileUser,
    ChannelGetInfo,
    ChannelGetData,
    SampleGetData,
    SampleFree,
    StreamFree,
    MusicFree,
    BassStop,
    BassFree,
    RuntimeConfig,
};

struct EventRecord {
    volatile LONG64 sequence;
    std::uint64_t qpc;
    std::uint32_t processId;
    std::uint32_t threadId;
    EventKind kind;
    std::uint32_t flags;
    std::uint64_t value0;
    std::uint64_t value1;
    std::uint64_t value2;
    std::uint64_t value3;
    float float0;
    float float1;
    std::array<wchar_t, EventTextLength> text;
};

struct SharedChannel {
    std::uint32_t magic;
    std::uint32_t version;
    volatile LONG64 generation;
    volatile LONG64 writeSequence;
    volatile LONG droppedEvents;
    volatile LONG64 blobWriteOffset;
    LARGE_INTEGER qpcFrequency;
    std::array<EventRecord, EventCapacity> events;
    std::array<std::uint8_t, SampleBlobCapacity> sampleBlob;
};

inline void InitializeSharedChannel(SharedChannel& channel)
{
    channel.magic = ProtocolMagic;
    channel.version = ProtocolVersion;
    LARGE_INTEGER now {};
    QueryPerformanceCounter(&now);
    channel.generation = now.QuadPart;
    channel.writeSequence = 0;
    channel.droppedEvents = 0;
    channel.blobWriteOffset = 0;
    QueryPerformanceFrequency(&channel.qpcFrequency);
    for (EventRecord& event : channel.events) {
        event.sequence = 0;
        event.text[0] = L'\0';
    }
}

inline bool TryCopySampleBlob(
    SharedChannel* channel,
    const void* source,
    std::uint32_t length,
    std::uint64_t& offset,
    std::uint64_t& copiedLength)
{
    offset = 0;
    copiedLength = 0;

    if (channel == nullptr || source == nullptr || length == 0 || length > MaxSampleBlobBytes)
        return false;

    LONG64 reserved = InterlockedAdd64(&channel->blobWriteOffset, length);
    LONG64 start = reserved - length;
    if (start < 0)
        return false;

    std::uint64_t wrapped = static_cast<std::uint64_t>(start) % SampleBlobCapacity;
    if (wrapped + length > SampleBlobCapacity) {
        reserved = InterlockedAdd64(&channel->blobWriteOffset, length);
        start = reserved - length;
        wrapped = static_cast<std::uint64_t>(start) % SampleBlobCapacity;
    }

    if (wrapped + length > SampleBlobCapacity)
        return false;

    std::memcpy(channel->sampleBlob.data() + wrapped, source, length);
    offset = wrapped;
    copiedLength = length;
    return true;
}

inline const wchar_t* ToString(EventKind kind)
{
    switch (kind) {
    case EventKind::HostStarted:
        return L"HostStarted";
    case EventKind::HookLoaded:
        return L"HookLoaded";
    case EventKind::HookUnloaded:
        return L"HookUnloaded";
    case EventKind::BassModuleWaiting:
        return L"BassModuleWaiting";
    case EventKind::BassModuleFound:
        return L"BassModuleFound";
    case EventKind::HookInstalled:
        return L"HookInstalled";
    case EventKind::HookInstallFailed:
        return L"HookInstallFailed";
    case EventKind::SampleLoadPath:
        return L"SampleLoadPath";
    case EventKind::SampleLoadMemory:
        return L"SampleLoadMemory";
    case EventKind::SampleCreate:
        return L"SampleCreate";
    case EventKind::StreamCreateFilePath:
        return L"StreamCreateFilePath";
    case EventKind::StreamCreateFileMemory:
        return L"StreamCreateFileMemory";
    case EventKind::SampleGetChannel:
        return L"SampleGetChannel";
    case EventKind::ChannelPlay:
        return L"ChannelPlay";
    case EventKind::ChannelPause:
        return L"ChannelPause";
    case EventKind::ChannelStop:
        return L"ChannelStop";
    case EventKind::ChannelSetAttribute:
        return L"ChannelSetAttribute";
    case EventKind::ChannelSetPosition:
        return L"ChannelSetPosition";
    case EventKind::MixerStreamCreate:
        return L"MixerStreamCreate";
    case EventKind::MixerStreamAddChannel:
        return L"MixerStreamAddChannel";
    case EventKind::MixerStreamRemoveChannel:
        return L"MixerStreamRemoveChannel";
    case EventKind::FxTempoCreate:
        return L"FxTempoCreate";
    case EventKind::StreamCreate:
        return L"StreamCreate";
    case EventKind::StreamCreateFileUser:
        return L"StreamCreateFileUser";
    case EventKind::ChannelGetInfo:
        return L"ChannelGetInfo";
    case EventKind::ChannelGetData:
        return L"ChannelGetData";
    case EventKind::SampleGetData:
        return L"SampleGetData";
    case EventKind::SampleFree:
        return L"SampleFree";
    case EventKind::StreamFree:
        return L"StreamFree";
    case EventKind::MusicFree:
        return L"MusicFree";
    case EventKind::BassStop:
        return L"BassStop";
    case EventKind::BassFree:
        return L"BassFree";
    case EventKind::RuntimeConfig:
        return L"RuntimeConfig";
    default:
        return L"Unknown";
    }
}

inline void CopyText(EventRecord& record, const wchar_t* text)
{
    if (text == nullptr) {
        record.text[0] = L'\0';
        return;
    }

    wcsncpy_s(record.text.data(), record.text.size(), text, _TRUNCATE);
}

inline void PublishEvent(
    SharedChannel* channel,
    HANDLE eventHandle,
    EventKind kind,
    std::uint64_t value0 = 0,
    std::uint64_t value1 = 0,
    std::uint64_t value2 = 0,
    std::uint64_t value3 = 0,
    float float0 = 0,
    float float1 = 0,
    const wchar_t* text = nullptr)
{
    if (channel == nullptr || channel->magic != ProtocolMagic || channel->version != ProtocolVersion)
        return;

    const LONG64 sequence = InterlockedIncrement64(&channel->writeSequence);
    EventRecord& record = channel->events[static_cast<std::size_t>(sequence % EventCapacity)];
    record.sequence = 0;
    LARGE_INTEGER now {};
    QueryPerformanceCounter(&now);
    record.qpc = static_cast<std::uint64_t>(now.QuadPart);
    record.processId = GetCurrentProcessId();
    record.threadId = GetCurrentThreadId();
    record.kind = kind;
    record.flags = 0;
    record.value0 = value0;
    record.value1 = value1;
    record.value2 = value2;
    record.value3 = value3;
    record.float0 = float0;
    record.float1 = float1;
    CopyText(record, text);
    MemoryBarrier();
    record.sequence = sequence;

    if (eventHandle != nullptr)
        SetEvent(eventHandle);
}

} // namespace olab
