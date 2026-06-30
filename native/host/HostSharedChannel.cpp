#include "HostSharedChannel.h"

#include <stdexcept>

namespace olab::host {

SharedHandles::SharedHandles(SharedHandles&& other) noexcept
    : mapping(other.mapping)
    , event(other.event)
    , channel(other.channel)
{
    other.mapping = nullptr;
    other.event = nullptr;
    other.channel = nullptr;
}

SharedHandles& SharedHandles::operator=(SharedHandles&& other) noexcept
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

SharedHandles::~SharedHandles()
{
    Reset();
}

void SharedHandles::Reset()
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

} // namespace olab::host
