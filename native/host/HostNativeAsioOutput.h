#pragma once

#include "HostAudioOutput.h"

#include <memory>

namespace olab::host {

std::unique_ptr<IAudioOutput> CreateNativeAsioOutput(OutputConfig config);

} // namespace olab::host
