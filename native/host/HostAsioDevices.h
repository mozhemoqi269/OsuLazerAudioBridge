#pragma once

#include "HostAudioOutput.h"

#include <optional>
#include <string>

namespace olab::host {

bool ListAsioDevices();
std::optional<std::wstring> ResolveAsioDriverName(const std::wstring& requestedDeviceId);
bool OpenAsioControlPanel(const OutputConfig& config);

} // namespace olab::host
