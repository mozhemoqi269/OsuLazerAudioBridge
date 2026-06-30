#pragma once

#include "HostAudioOutput.h"

#include <memory>

namespace olab::host {

std::unique_ptr<IAudioOutput> CreateXAudio2Output(OutputConfig config);

} // namespace olab::host
