#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace olab {

struct DecodedSample {
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
    std::vector<float> frames;
    std::string format;
};

bool DecodeSampleMemory(
    const std::uint8_t* data,
    std::uint64_t length,
    DecodedSample& output,
    std::string& error);

} // namespace olab

