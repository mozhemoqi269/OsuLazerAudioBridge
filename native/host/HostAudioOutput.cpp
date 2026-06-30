#include "HostAudioOutput.h"

namespace olab::host {

std::wstring OutputBackendName(OutputBackend backend)
{
    switch (backend) {
    case OutputBackend::XAudio2:
        return L"xaudio2";
    case OutputBackend::WasapiExclusive:
        return L"wasapi-exclusive";
    case OutputBackend::Asio:
        return L"asio";
    }

    return L"unknown";
}

std::optional<OutputBackend> ParseOutputBackend(const std::wstring& value)
{
    if (_wcsicmp(value.c_str(), L"xaudio2") == 0)
        return OutputBackend::XAudio2;
    if (_wcsicmp(value.c_str(), L"wasapi-exclusive") == 0 || _wcsicmp(value.c_str(), L"wasapi") == 0)
        return OutputBackend::WasapiExclusive;
    if (_wcsicmp(value.c_str(), L"asio") == 0)
        return OutputBackend::Asio;

    return std::nullopt;
}

std::shared_ptr<const std::vector<float>> IAudioOutput::PrepareFloatClip(const std::vector<float>&, WORD, DWORD)
{
    return nullptr;
}

bool IAudioOutput::SubmitPreparedFloat(std::uint64_t, std::shared_ptr<const std::vector<float>>, float, std::wstring&)
{
    return false;
}

std::wstring IAudioOutput::ConsumeDiagnostics()
{
    return {};
}

void IAudioOutput::SetChannelVolume(std::uint64_t, float)
{
}

void IAudioOutput::SetMixOverlapDucking(float)
{
}

} // namespace olab::host
