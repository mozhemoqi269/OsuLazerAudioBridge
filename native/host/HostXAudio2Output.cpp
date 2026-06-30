#include "HostXAudio2Output.h"

#include <Windows.h>
#include <wrl/client.h>
#include <xaudio2.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

namespace olab::host {
namespace {

using Microsoft::WRL::ComPtr;

class XAudio2Output final : public IAudioOutput {
public:
    explicit XAudio2Output(OutputConfig config)
        : config(std::move(config))
    {
    }

    ~XAudio2Output() override
    {
        Stop();
    }

    bool Start(std::wstring& error) override
    {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr)) {
            comInitialized = true;
        } else if (hr != RPC_E_CHANGED_MODE) {
            error = FormatHresult(L"CoInitializeEx", hr);
            return false;
        }

        hr = XAudio2Create(&engine, 0, XAUDIO2_DEFAULT_PROCESSOR);
        if (FAILED(hr)) {
            error = FormatHresult(L"XAudio2Create", hr);
            return false;
        }

        hr = engine->CreateMasteringVoice(
            &masteringVoice,
            XAUDIO2_DEFAULT_CHANNELS,
            XAUDIO2_DEFAULT_SAMPLERATE,
            0,
            config.deviceId.empty() ? nullptr : config.deviceId.c_str());
        if (FAILED(hr)) {
            error = FormatHresult(L"CreateMasteringVoice", hr);
            return false;
        }

        active = true;
        return true;
    }

    bool Submit(std::uint64_t channel, std::vector<std::int16_t> pcm16, WORD channels, DWORD sampleRate, float volume, bool stream, std::wstring& error) override
    {
        if (!active || pcm16.empty() || channels == 0 || sampleRate == 0)
            return false;

        std::lock_guard<std::mutex> lock(mutex);
        if (stream)
            return QueuePcmChunk(channel, std::move(pcm16), channels, sampleRate, volume, error);

        return PlayClip(channel, std::move(pcm16), channels, sampleRate, volume, error);
    }

    void ResetStream() override
    {
        std::lock_guard<std::mutex> lock(mutex);
        DestroyAllPcmStreams();
    }

    void ResetAll() override
    {
        std::lock_guard<std::mutex> lock(mutex);
        DestroyAllVoices();
        DestroyAllPcmStreams();
    }

    bool StopChannel(std::uint64_t channel) override
    {
        if (channel == 0)
            return false;

        std::lock_guard<std::mutex> lock(mutex);
        const std::uint64_t stopped = StopVoicesForChannel(channel) + StopPcmStreamForChannel(channel);
        return stopped != 0;
    }

    void Stop() override
    {
        active = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            DestroyAllVoices();
            DestroyAllPcmStreams();
        }

        if (masteringVoice != nullptr) {
            masteringVoice->DestroyVoice();
            masteringVoice = nullptr;
        }

        if (engine != nullptr) {
            engine->StopEngine();
            engine.Reset();
        }

        if (comInitialized) {
            CoUninitialize();
            comInitialized = false;
        }
    }

    void SetVolume(float) override
    {
    }

    void SetChannelVolume(std::uint64_t channel, float volume) override
    {
        if (!active)
            return;

        std::lock_guard<std::mutex> lock(mutex);
        CleanupFinishedVoices();
        const float clamped = std::clamp(volume, 0.0f, 2.0f);
        for (VoiceBuffer* voiceBuffer : activeVoices) {
            if (voiceBuffer != nullptr && voiceBuffer->voice != nullptr && voiceBuffer->channel == channel)
                voiceBuffer->voice->SetVolume(clamped);
        }

        auto streamIt = pcmStreams.find(channel);
        if (streamIt != pcmStreams.end() && streamIt->second != nullptr && streamIt->second->voice != nullptr)
            streamIt->second->voice->SetVolume(clamped);
    }

    const wchar_t* Name() const override
    {
        return L"xaudio2";
    }

private:
    struct VoiceBuffer {
        IXAudio2SourceVoice* voice = nullptr;
        std::uint64_t channel = 0;
        std::vector<std::int16_t> pcm16;
    };

    struct PcmStreamVoice {
        IXAudio2SourceVoice* voice = nullptr;
        std::uint64_t channel = 0;
        WORD channels = 0;
        DWORD sampleRate = 0;
        std::deque<std::vector<std::int16_t>> queuedBuffers;
        std::uint64_t dropped = 0;
    };

    static std::wstring FormatHresult(const wchar_t* operation, HRESULT hr)
    {
        std::wstringstream line;
        line << L"AudioMirrorError operation=\"" << operation << L"\" hr=0x" << std::hex << static_cast<unsigned long>(hr);
        return line.str();
    }

    bool PlayClip(std::uint64_t channel, std::vector<std::int16_t> pcm16, WORD channels, DWORD sampleRate, float volume, std::wstring& error)
    {
        CleanupFinishedVoices();

        auto voiceBuffer = std::make_unique<VoiceBuffer>();
        voiceBuffer->channel = channel;
        voiceBuffer->pcm16 = std::move(pcm16);

        WAVEFORMATEX format {};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = channels;
        format.nSamplesPerSec = sampleRate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = static_cast<WORD>(format.nChannels * sizeof(std::int16_t));
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

        HRESULT hr = engine->CreateSourceVoice(&voiceBuffer->voice, &format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, nullptr);
        if (FAILED(hr)) {
            error = FormatHresult(L"CreateSourceVoice", hr);
            return false;
        }

        XAUDIO2_BUFFER buffer {};
        buffer.AudioBytes = static_cast<UINT32>(voiceBuffer->pcm16.size() * sizeof(std::int16_t));
        buffer.pAudioData = reinterpret_cast<const BYTE*>(voiceBuffer->pcm16.data());
        buffer.Flags = XAUDIO2_END_OF_STREAM;

        hr = voiceBuffer->voice->SubmitSourceBuffer(&buffer);
        if (FAILED(hr)) {
            error = FormatHresult(L"SubmitSourceBuffer", hr);
            voiceBuffer->voice->DestroyVoice();
            return false;
        }

        hr = voiceBuffer->voice->Start(0);
        if (FAILED(hr)) {
            error = FormatHresult(L"Start", hr);
            voiceBuffer->voice->DestroyVoice();
            return false;
        }

        voiceBuffer->voice->SetVolume(std::clamp(volume, 0.0f, 2.0f));
        activeVoices.push_back(voiceBuffer.release());
        error.clear();
        return true;
    }

    bool QueuePcmChunk(std::uint64_t channel, std::vector<std::int16_t> pcm16, WORD channels, DWORD sampleRate, float volume, std::wstring& error)
    {
        static constexpr UINT32 MaxQueuedBuffers = 4;
        PcmStreamVoice* stream = GetOrCreatePcmStream(channel, channels, sampleRate, error);
        if (stream == nullptr)
            return false;

        TrimCompletedPcmBuffers(*stream);

        XAUDIO2_VOICE_STATE state {};
        stream->voice->GetState(&state);
        if (state.BuffersQueued >= MaxQueuedBuffers) {
            stream->dropped++;
            error.clear();
            return true;
        }

        stream->queuedBuffers.push_back(std::move(pcm16));
        const auto& queued = stream->queuedBuffers.back();

        XAUDIO2_BUFFER buffer {};
        buffer.AudioBytes = static_cast<UINT32>(queued.size() * sizeof(std::int16_t));
        buffer.pAudioData = reinterpret_cast<const BYTE*>(queued.data());

        HRESULT hr = stream->voice->SubmitSourceBuffer(&buffer);
        if (FAILED(hr)) {
            stream->queuedBuffers.pop_back();
            error = FormatHresult(L"SubmitPcmStreamBuffer", hr);
            return false;
        }

        stream->voice->SetVolume(std::clamp(volume, 0.0f, 2.0f));
        hr = stream->voice->Start(0);
        if (FAILED(hr)) {
            error = FormatHresult(L"StartPcmStream", hr);
            return false;
        }

        error.clear();
        return true;
    }

    PcmStreamVoice* GetOrCreatePcmStream(std::uint64_t channel, WORD channels, DWORD sampleRate, std::wstring& error)
    {
        auto found = pcmStreams.find(channel);
        if (found != pcmStreams.end()) {
            PcmStreamVoice* stream = found->second;
            if (stream != nullptr && stream->channels == channels && stream->sampleRate == sampleRate)
                return stream;

            DestroyPcmStream(stream);
            pcmStreams.erase(found);
        }

        auto stream = std::make_unique<PcmStreamVoice>();
        stream->channel = channel;
        stream->channels = channels;
        stream->sampleRate = sampleRate;

        WAVEFORMATEX format {};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = channels;
        format.nSamplesPerSec = sampleRate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = static_cast<WORD>(format.nChannels * sizeof(std::int16_t));
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

        HRESULT hr = engine->CreateSourceVoice(&stream->voice, &format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, nullptr);
        if (FAILED(hr)) {
            error = FormatHresult(L"CreatePcmStreamVoice", hr);
            return nullptr;
        }

        PcmStreamVoice* raw = stream.release();
        pcmStreams[channel] = raw;
        error.clear();
        return raw;
    }

    static void TrimCompletedPcmBuffers(PcmStreamVoice& stream)
    {
        if (stream.voice == nullptr)
            return;

        XAUDIO2_VOICE_STATE state {};
        stream.voice->GetState(&state);
        while (stream.queuedBuffers.size() > state.BuffersQueued)
            stream.queuedBuffers.pop_front();
    }

    static void DestroyVoiceBuffer(VoiceBuffer* voiceBuffer)
    {
        if (voiceBuffer == nullptr)
            return;

        if (voiceBuffer->voice != nullptr) {
            voiceBuffer->voice->Stop(0);
            voiceBuffer->voice->FlushSourceBuffers();
            voiceBuffer->voice->DestroyVoice();
        }
        delete voiceBuffer;
    }

    static void DestroyPcmStream(PcmStreamVoice* stream)
    {
        if (stream == nullptr)
            return;

        if (stream->voice != nullptr) {
            stream->voice->Stop(0);
            stream->voice->FlushSourceBuffers();
            stream->voice->DestroyVoice();
        }
        delete stream;
    }

    std::uint64_t StopPcmStreamForChannel(std::uint64_t channel)
    {
        auto found = pcmStreams.find(channel);
        if (found == pcmStreams.end())
            return 0;

        DestroyPcmStream(found->second);
        pcmStreams.erase(found);
        return 1;
    }

    void DestroyAllPcmStreams()
    {
        for (auto& [channel, stream] : pcmStreams)
            DestroyPcmStream(stream);

        pcmStreams.clear();
    }

    std::uint64_t StopVoicesForChannel(std::uint64_t channel)
    {
        CleanupFinishedVoices();

        std::uint64_t stopped = 0;
        auto write = activeVoices.begin();
        for (auto read = activeVoices.begin(); read != activeVoices.end(); ++read) {
            VoiceBuffer* voiceBuffer = *read;
            if (voiceBuffer == nullptr)
                continue;

            if (voiceBuffer->channel == channel) {
                DestroyVoiceBuffer(voiceBuffer);
                stopped++;
                continue;
            }

            *write++ = voiceBuffer;
        }

        activeVoices.erase(write, activeVoices.end());
        return stopped;
    }

    void DestroyAllVoices()
    {
        for (VoiceBuffer* voice : activeVoices)
            DestroyVoiceBuffer(voice);
        activeVoices.clear();
    }

    void CleanupFinishedVoices()
    {
        auto write = activeVoices.begin();
        for (auto read = activeVoices.begin(); read != activeVoices.end(); ++read) {
            VoiceBuffer* voiceBuffer = *read;
            if (voiceBuffer == nullptr)
                continue;

            XAUDIO2_VOICE_STATE state {};
            voiceBuffer->voice->GetState(&state);
            if (state.BuffersQueued == 0) {
                DestroyVoiceBuffer(voiceBuffer);
                continue;
            }

            *write++ = voiceBuffer;
        }

        activeVoices.erase(write, activeVoices.end());
    }

    OutputConfig config;
    ComPtr<IXAudio2> engine;
    IXAudio2MasteringVoice* masteringVoice = nullptr;
    std::vector<VoiceBuffer*> activeVoices;
    std::unordered_map<std::uint64_t, PcmStreamVoice*> pcmStreams;
    std::mutex mutex;
    bool comInitialized = false;
    bool active = false;
};

} // namespace

std::unique_ptr<IAudioOutput> CreateXAudio2Output(OutputConfig config)
{
    return std::make_unique<XAudio2Output>(std::move(config));
}

} // namespace olab::host
