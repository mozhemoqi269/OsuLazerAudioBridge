#pragma once

#include <Windows.h>

#include <olab/SharedChannel.h>

namespace olab::host {

struct SharedHandles {
    HANDLE mapping = nullptr;
    HANDLE event = nullptr;
    olab::SharedChannel* channel = nullptr;

    SharedHandles() = default;
    SharedHandles(const SharedHandles&) = delete;
    SharedHandles& operator=(const SharedHandles&) = delete;
    SharedHandles(SharedHandles&& other) noexcept;
    SharedHandles& operator=(SharedHandles&& other) noexcept;
    ~SharedHandles();

    void Reset();
};

SharedHandles CreateSharedChannel();

} // namespace olab::host
