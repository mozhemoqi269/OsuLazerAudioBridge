#pragma once

#include <Windows.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace olab::host {

enum class OutputBackend {
    XAudio2,
    WasapiExclusive,
    Asio,
};

std::wstring OutputBackendName(OutputBackend backend);
std::optional<OutputBackend> ParseOutputBackend(const std::wstring& value);

struct OutputConfig {
    OutputBackend backend = OutputBackend::XAudio2;
    std::wstring deviceId;
    DWORD sampleRate = 48000;
    WORD channels = 2;
    DWORD bufferMs = 10;
    float effectsVolume = 1.0f;
    float musicVolume = 1.0f;
    float mixOverlapDucking = 0.25f;
};

class IAudioOutput {
public:
    virtual ~IAudioOutput() = default;
    virtual bool Start(std::wstring& error) = 0;
    virtual bool Submit(std::uint64_t channel, std::vector<std::int16_t> pcm16, WORD channels, DWORD sampleRate, float volume, bool stream, std::wstring& error) = 0;
    virtual std::shared_ptr<const std::vector<float>> PrepareFloatClip(const std::vector<float>& pcmFloat, WORD channels, DWORD sampleRate);
    virtual bool SubmitPreparedFloat(std::uint64_t channel, std::shared_ptr<const std::vector<float>> pcmFloat, float volume, std::wstring& error);
    virtual void ResetStream() = 0;
    virtual void ResetAll() = 0;
    virtual bool StopChannel(std::uint64_t channel) = 0;
    virtual void Stop() = 0;
    virtual void SetVolume(float volume) = 0;
    virtual void SetChannelVolume(std::uint64_t channel, float volume);
    virtual void SetMixOverlapDucking(float value);
    virtual const wchar_t* Name() const = 0;
    virtual std::wstring ConsumeDiagnostics();
};

} // namespace olab::host
