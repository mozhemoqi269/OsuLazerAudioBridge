#include "SampleDecoder.h"

#include <algorithm>
#include <cstring>

#define DR_WAV_IMPLEMENTATION
#include <dr_wav.h>

#define DR_MP3_IMPLEMENTATION
#include <dr_mp3.h>

#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>
#undef STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>

namespace olab {
namespace {

bool HasHeader(const std::uint8_t* data, std::uint64_t length, const char* header, std::uint64_t headerLength)
{
    return data != nullptr
        && length >= headerLength
        && std::memcmp(data, header, static_cast<std::size_t>(headerLength)) == 0;
}

bool DecodeWav(const std::uint8_t* data, std::uint64_t length, DecodedSample& output, std::string& error)
{
    unsigned int channels = 0;
    unsigned int sampleRate = 0;
    drwav_uint64 frameCount = 0;
    float* decoded = drwav_open_memory_and_read_pcm_frames_f32(
        data,
        static_cast<std::size_t>(length),
        &channels,
        &sampleRate,
        &frameCount,
        nullptr);
    if (decoded == nullptr) {
        error = "dr_wav failed to decode memory sample.";
        return false;
    }

    const std::uint64_t sampleCount = frameCount * channels;
    output.sampleRate = sampleRate;
    output.channels = channels;
    output.format = "wav";
    output.frames.assign(decoded, decoded + sampleCount);
    drwav_free(decoded, nullptr);
    return true;
}

bool DecodeMp3(const std::uint8_t* data, std::uint64_t length, DecodedSample& output, std::string& error)
{
    drmp3_config config {};
    drmp3_uint64 frameCount = 0;
    float* decoded = drmp3_open_memory_and_read_pcm_frames_f32(
        data,
        static_cast<std::size_t>(length),
        &config,
        &frameCount,
        nullptr);
    if (decoded == nullptr) {
        error = "dr_mp3 failed to decode memory sample.";
        return false;
    }

    const std::uint64_t sampleCount = frameCount * config.channels;
    output.sampleRate = config.sampleRate;
    output.channels = config.channels;
    output.format = "mp3";
    output.frames.assign(decoded, decoded + sampleCount);
    drmp3_free(decoded, nullptr);
    return true;
}

bool DecodeOggVorbis(const std::uint8_t* data, std::uint64_t length, DecodedSample& output, std::string& error)
{
    int channels = 0;
    int sampleRate = 0;
    short* decoded = nullptr;
    int sampleCountPerChannel = stb_vorbis_decode_memory(
        data,
        static_cast<int>(std::min<std::uint64_t>(length, static_cast<std::uint64_t>(INT_MAX))),
        &channels,
        &sampleRate,
        &decoded);
    if (sampleCountPerChannel < 0 || decoded == nullptr) {
        error = "stb_vorbis failed to decode memory sample.";
        return false;
    }

    const std::uint64_t sampleCount = static_cast<std::uint64_t>(sampleCountPerChannel) * static_cast<std::uint64_t>(channels);
    output.sampleRate = static_cast<std::uint32_t>(sampleRate);
    output.channels = static_cast<std::uint32_t>(channels);
    output.format = "ogg-vorbis";
    output.frames.resize(static_cast<std::size_t>(sampleCount));
    for (std::uint64_t i = 0; i < sampleCount; i++)
        output.frames[static_cast<std::size_t>(i)] = decoded[i] / 32768.0f;

    free(decoded);
    return true;
}

} // namespace

bool DecodeSampleMemory(
    const std::uint8_t* data,
    std::uint64_t length,
    DecodedSample& output,
    std::string& error)
{
    output = {};
    error.clear();

    if (data == nullptr || length == 0) {
        error = "empty sample";
        return false;
    }

    if (HasHeader(data, length, "RIFF", 4))
        return DecodeWav(data, length, output, error);

    if (HasHeader(data, length, "OggS", 4))
        return DecodeOggVorbis(data, length, output, error);

    if (HasHeader(data, length, "ID3", 3) || (length >= 2 && data[0] == 0xff && (data[1] & 0xe0) == 0xe0))
        return DecodeMp3(data, length, output, error);

    error = "unknown sample container";
    return false;
}

} // namespace olab

