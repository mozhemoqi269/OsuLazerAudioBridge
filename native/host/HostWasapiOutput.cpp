#include "HostWasapiOutput.h"

#include <Windows.h>
#include <Audioclient.h>
#include <Mmdeviceapi.h>
#include <mmreg.h>
#include <avrt.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace olab::host {
namespace {

using Microsoft::WRL::ComPtr;

std::wstring FormatHresultLine(const wchar_t* operation, HRESULT hr)
{
    std::wstringstream line;
    line << L"AudioMirrorError operation=\"" << operation << L"\" hr=0x" << std::hex << static_cast<unsigned long>(hr);
    return line.str();
}
class WasapiExclusiveOutput final : public IAudioOutput {
public:
    explicit WasapiExclusiveOutput(OutputConfig config)
        : config(std::move(config))
    {
    }
    WasapiExclusiveOutput(const WasapiExclusiveOutput&) = delete;
    WasapiExclusiveOutput& operator=(const WasapiExclusiveOutput&) = delete;

    ~WasapiExclusiveOutput() override
    {
        Stop();
    }

    bool Start(std::wstring& error) override
    {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(hr)) {
            comInitialized = true;
        } else if (hr != RPC_E_CHANGED_MODE) {
            error = FormatHresult(L"Wasapi.CoInitializeEx", hr);
            return false;
        }

        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (FAILED(hr)) {
            error = FormatHresult(L"Wasapi.CoCreateInstance", hr);
            return false;
        }

        if (!config.deviceId.empty()) {
            hr = enumerator->GetDevice(config.deviceId.c_str(), &device);
            if (FAILED(hr)) {
                error = FormatHresult(L"Wasapi.GetDevice", hr);
                return false;
            }
        } else {
            hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
            if (FAILED(hr)) {
                error = FormatHresult(L"Wasapi.GetDefaultAudioEndpoint", hr);
                return false;
            }
        }

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(audioClient.GetAddressOf()));
        if (FAILED(hr)) {
            error = FormatHresult(L"Wasapi.Activate", hr);
            return false;
        }

        hr = InitializeExclusiveFormat();
        if (FAILED(hr)) {
            error = FormatHresult(L"Wasapi.InitializeExclusive16Stereo", hr);
            return false;
        }

        hr = audioClient->GetBufferSize(&bufferFrameCount);
        if (FAILED(hr)) {
            error = FormatHresult(L"Wasapi.GetBufferSize", hr);
            return false;
        }

        hr = audioClient->GetService(IID_PPV_ARGS(&renderClient));
        if (FAILED(hr)) {
            error = FormatHresult(L"Wasapi.GetRenderClient", hr);
            return false;
        }

        renderEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (renderEvent == nullptr) {
            error = L"AudioMirrorError operation=\"Wasapi.CreateEvent\"";
            return false;
        }

        hr = audioClient->SetEventHandle(renderEvent);
        if (FAILED(hr)) {
            error = FormatHresult(L"Wasapi.SetEventHandle", hr);
            return false;
        }

        PrimeDeviceBuffer();
        running = true;
        hr = audioClient->Start();
        if (FAILED(hr)) {
            running = false;
            error = FormatHresult(L"Wasapi.Start", hr);
            return false;
        }

        renderThread = std::thread(&WasapiExclusiveOutput::RenderLoop, this);
        return true;
    }

    bool Submit(std::uint64_t channel, std::vector<std::int16_t> pcm16, WORD channels, DWORD sampleRate, float volume, bool stream, std::wstring& error) override
    {
        if (!running || pcm16.empty() || channels == 0 || sampleRate == 0)
            return false;

        if (stream) {
            std::lock_guard<std::mutex> submitLock(streamSubmitMutex);
            bool resetResampler = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                EnsureStreamRingLocked();
                if (streamChannel != 0 && streamChannel != channel) {
                    ClearStreamRingLocked();
                    resetResampler = true;
                }
                if (streamChannel != channel)
                    resetResampler = true;
                streamChannel = channel;
            }

            if (resetResampler)
                ResetStreamResamplerLocked();
            std::vector<std::int16_t> converted = ConvertStreamToOutputFormatLocked(pcm16, channels, sampleRate, volume);
            if (converted.empty())
                return false;

            {
                std::lock_guard<std::mutex> lock(mutex);
                const bool longStreamSubmission = converted.size() / outputChannels > LongStreamSubmissionFrames();
                PushStreamSamples(std::move(converted), longStreamSubmission);
                if (!streamLongForm && streamQueuedSamples / outputChannels > MaxStreamQueuedFrames())
                    TrimStreamQueueToFrames(CorrectionStartStreamQueuedFrames());
            }
        } else {
            std::vector<std::int16_t> converted = ConvertToOutputFormat(pcm16, channels, sampleRate, volume);
            if (converted.empty())
                return false;

            std::lock_guard<std::mutex> lock(mutex);
            activeClips.push_back(Clip { std::move(converted), channel, 0 });
        }

        error.clear();
        return true;
    }

    void ResetStream() override
    {
        std::lock_guard<std::mutex> submitLock(streamSubmitMutex);
        std::lock_guard<std::mutex> lock(mutex);
        ClearStreamRingLocked();
        ResetStreamResamplerLocked();
    }

    void ResetAll() override
    {
        std::lock_guard<std::mutex> submitLock(streamSubmitMutex);
        std::lock_guard<std::mutex> lock(mutex);
        ClearStreamRingLocked();
        ResetStreamResamplerLocked();
        activeClips.clear();
    }

    bool StopChannel(std::uint64_t channel) override
    {
        if (channel == 0)
            return false;

        std::lock_guard<std::mutex> submitLock(streamSubmitMutex);
        std::lock_guard<std::mutex> lock(mutex);
        bool stopped = false;
        if (streamChannel == channel) {
            ClearStreamRingLocked();
            ResetStreamResamplerLocked();
            stopped = true;
        }

        auto write = activeClips.begin();
        for (auto read = activeClips.begin(); read != activeClips.end(); ++read) {
            if (read->channel == channel) {
                stopped = true;
                continue;
            }

            *write++ = std::move(*read);
        }
        activeClips.erase(write, activeClips.end());
        return stopped;
    }

    void Stop() override
    {
        running.store(false);
        if (renderEvent != nullptr)
            SetEvent(renderEvent);

        if (audioClient != nullptr) {
            audioClient->Stop();
        }

        if (renderThread.joinable())
            renderThread.join();

        if (audioClient != nullptr)
            audioClient->Reset();

        if (renderEvent != nullptr) {
            CloseHandle(renderEvent);
            renderEvent = nullptr;
        }

        renderClient.Reset();
        audioClient.Reset();
        device.Reset();
        enumerator.Reset();

        {
            std::lock_guard<std::mutex> submitLock(streamSubmitMutex);
            std::lock_guard<std::mutex> lock(mutex);
            ClearStreamRingLocked();
            ResetStreamResamplerLocked();
            activeClips.clear();
        }

        if (comInitialized) {
            CoUninitialize();
            comInitialized = false;
        }
    }

    void SetVolume(float value) override
    {
        volume.store(std::clamp(value, 0.0f, 2.0f));
    }

    const wchar_t* Name() const override
    {
        return L"wasapi-exclusive";
    }

    std::wstring ConsumeDiagnostics() override
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto now = std::chrono::steady_clock::now();
        if (now < nextDiagnosticsTime)
            return {};

        const std::size_t queuedFrames = outputChannels == 0 ? 0 : streamQueuedSamples / outputChannels;
        if (queuedFrames == 0
            && streamUnderruns == lastReportedUnderruns
            && streamEmergencyTrims == lastReportedEmergencyTrims
            && streamRateCorrections == lastReportedRateCorrections)
            return {};

        nextDiagnosticsTime = now + std::chrono::seconds(2);
        lastReportedUnderruns = streamUnderruns;
        lastReportedEmergencyTrims = streamEmergencyTrims;
        lastReportedRateCorrections = streamRateCorrections;

        std::wstringstream line;
        line << L"WasapiDiagnostics queuedFrames=" << queuedFrames
             << L" targetFrames=" << TargetStreamQueuedFrames()
             << L" underruns=" << streamUnderruns
             << L" emergencyTrims=" << streamEmergencyTrims
             << L" concealedFrames=" << streamConcealedFrames
             << L" rateCorrections=" << streamRateCorrections
             << L" lastRate=" << lastStreamPlaybackRate
             << L" longForm=" << (streamLongForm ? 1 : 0)
             << L" outputRate=" << outputSampleRate
             << L" bufferFrames=" << bufferFrameCount
             << L" bufferMs=" << actualBufferMs;
        return line.str();
    }

private:
    static constexpr WORD OutputChannels = 2;

    struct Clip {
        std::vector<std::int16_t> samples;
        std::uint64_t channel = 0;
        std::size_t cursor = 0;
    };

    std::size_t MaxStreamQueuedFrames() const
    {
        const std::size_t outputRate = outputSampleRate == 0 ? 48000 : outputSampleRate;
        return std::max<std::size_t>(static_cast<std::size_t>(bufferFrameCount) * 2, outputRate / 100);
    }

    std::size_t LongStreamSubmissionFrames() const
    {
        const std::size_t outputRate = outputSampleRate == 0 ? 48000 : outputSampleRate;
        return std::max<std::size_t>(static_cast<std::size_t>(bufferFrameCount) * 4, outputRate / 4);
    }

    std::size_t TargetStreamQueuedFrames() const
    {
        const std::size_t outputRate = outputSampleRate == 0 ? 48000 : outputSampleRate;
        return std::max<std::size_t>(static_cast<std::size_t>(bufferFrameCount), outputRate / 200);
    }

    std::size_t CorrectionStartStreamQueuedFrames() const
    {
        const std::size_t outputRate = outputSampleRate == 0 ? 48000 : outputSampleRate;
        return TargetStreamQueuedFrames() + std::max<std::size_t>(static_cast<std::size_t>(bufferFrameCount) / 2, outputRate / 400);
    }

    std::size_t StreamPrebufferFrames() const
    {
        const std::size_t outputRate = outputSampleRate == 0 ? 48000 : outputSampleRate;
        return std::max<std::size_t>(static_cast<std::size_t>(bufferFrameCount), outputRate / 200);
    }

    std::size_t StreamRingCapacityFrames() const
    {
        const std::size_t outputRate = outputSampleRate == 0 ? 48000 : outputSampleRate;
        return std::max<std::size_t>(MaxStreamQueuedFrames() + static_cast<std::size_t>(bufferFrameCount) * 3, outputRate / 20);
    }

    void TrimStreamQueueToFrames(std::size_t targetFrames)
    {
        if (outputChannels == 0)
            return;

        const std::size_t queuedFrames = streamQueuedSamples / outputChannels;
        if (queuedFrames <= targetFrames)
            return;

        std::size_t framesToDrop = queuedFrames - targetFrames;
        DropStreamFrames(framesToDrop, true);
        streamEmergencyTrims++;

        if (streamQueuedSamples == 0)
            streamPrimed = false;
    }

    void EnsureStreamRingLocked()
    {
        const std::size_t desiredSamples = std::max<std::size_t>(
            StreamRingCapacityFrames() * outputChannels,
            static_cast<std::size_t>(bufferFrameCount) * outputChannels * 8);
        if (desiredSamples == 0)
            return;

        if (streamRing.size() >= desiredSamples)
            return;

        streamRing.assign(desiredSamples, 0);
        streamReadIndex = 0;
        streamWriteIndex = 0;
        streamQueuedSamples = 0;
        streamPrimed = false;
        streamChannel = 0;
    }

    void ClearStreamRingLocked()
    {
        streamReadIndex = 0;
        streamWriteIndex = 0;
        streamQueuedSamples = 0;
        streamReadFraction = 0.0;
        streamPrimed = false;
        streamLongForm = false;
        lastStreamFrame.assign(outputChannels, 0);
        hasLastStreamFrame = false;
        streamChannel = 0;
    }

    void ResetStreamResamplerLocked()
    {
        streamResampleInputSampleRate = 0;
        streamResampleInputChannels = 0;
        streamResamplePosition = 0.0;
        streamResampleCarry.clear();
        streamResampleWork.clear();
    }

    void DropStreamFrames(std::size_t framesToDrop, bool resetFraction, bool clearPrimedWhenEmpty = true)
    {
        if (streamRing.empty() || outputChannels == 0)
            return;

        const std::size_t samplesToDrop = framesToDrop * outputChannels;
        const std::size_t dropped = std::min(samplesToDrop, streamQueuedSamples);
        streamReadIndex = (streamReadIndex + dropped) % streamRing.size();
        streamQueuedSamples -= dropped;
        if (resetFraction)
            streamReadFraction = 0.0;
        if (streamQueuedSamples == 0) {
            streamWriteIndex = streamReadIndex;
            streamReadFraction = 0.0;
            if (clearPrimedWhenEmpty)
                streamPrimed = false;
            streamLongForm = false;
        }
    }

    void PushStreamSamples(std::vector<std::int16_t>&& samples, bool longStreamSubmission)
    {
        if (samples.empty() || streamRing.empty())
            return;

        if (longStreamSubmission) {
            streamRing = std::move(samples);
            streamReadIndex = 0;
            streamWriteIndex = 0;
            streamQueuedSamples = streamRing.size();
            streamReadFraction = 0.0;
            streamPrimed = true;
            streamLongForm = true;
            return;
        }

        const std::size_t capacity = streamRing.size();
        if (samples.size() >= capacity) {
            const auto start = samples.end() - static_cast<std::ptrdiff_t>(capacity);
            std::copy(start, samples.end(), streamRing.begin());
            streamReadIndex = 0;
            streamWriteIndex = 0;
            streamQueuedSamples = capacity;
            streamReadFraction = 0.0;
            streamPrimed = true;
            streamLongForm = false;
            return;
        }

        if (streamQueuedSamples + samples.size() > capacity)
            TrimStreamQueueToFrames((capacity - samples.size()) / outputChannels);

        const std::size_t first = std::min(samples.size(), capacity - streamWriteIndex);
        std::copy(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(first), streamRing.begin() + static_cast<std::ptrdiff_t>(streamWriteIndex));
        if (first < samples.size())
            std::copy(samples.begin() + static_cast<std::ptrdiff_t>(first), samples.end(), streamRing.begin());

        streamWriteIndex = (streamWriteIndex + samples.size()) % capacity;
        streamQueuedSamples += samples.size();
    }

    std::size_t RenderStreamSamples(std::int16_t* output, std::size_t outputFrames)
    {
        if (output == nullptr || outputFrames == 0 || outputChannels == 0)
            return 0;

        const std::size_t queuedFrames = streamQueuedSamples / outputChannels;
        lastStreamPlaybackRate = 1.0;
        streamReadFraction = 0.0;
        const std::size_t renderedFrames = std::min<std::size_t>(outputFrames, queuedFrames);

        if (renderedFrames != 0) {
            const std::size_t samplesToCopy = renderedFrames * outputChannels;
            const std::size_t first = std::min(samplesToCopy, streamRing.size() - streamReadIndex);
            std::copy(
                streamRing.begin() + static_cast<std::ptrdiff_t>(streamReadIndex),
                streamRing.begin() + static_cast<std::ptrdiff_t>(streamReadIndex + first),
                output);
            if (first < samplesToCopy) {
                std::copy(
                    streamRing.begin(),
                    streamRing.begin() + static_cast<std::ptrdiff_t>(samplesToCopy - first),
                    output + first);
            }

            RememberLastStreamFrame(output + (renderedFrames - 1) * outputChannels);
            DropStreamFrames(renderedFrames, false, false);
        }

        if (renderedFrames < outputFrames) {
            streamUnderruns++;
            FillStreamConcealment(output + renderedFrames * outputChannels, outputFrames - renderedFrames);
        }
        return renderedFrames * outputChannels;
    }

    void RememberLastStreamFrame(const std::int16_t* frame)
    {
        if (frame == nullptr || outputChannels == 0)
            return;

        if (lastStreamFrame.size() != outputChannels)
            lastStreamFrame.assign(outputChannels, 0);
        for (WORD channel = 0; channel < outputChannels; channel++)
            lastStreamFrame[channel] = frame[channel];
        hasLastStreamFrame = true;
    }

    void FillStreamConcealment(std::int16_t* output, std::size_t frames)
    {
        if (output == nullptr || frames == 0 || outputChannels == 0 || !hasLastStreamFrame)
            return;

        const std::size_t fadeFrames = std::min<std::size_t>(frames, std::max<std::size_t>(1, outputSampleRate / 200));
        for (std::size_t frame = 0; frame < frames; frame++) {
            const float gain = frame < fadeFrames
                ? 1.0f - static_cast<float>(frame) / static_cast<float>(fadeFrames)
                : 0.0f;
            for (WORD channel = 0; channel < outputChannels; channel++) {
                const float sample = static_cast<float>(lastStreamFrame[channel]) * gain;
                output[frame * outputChannels + channel] = static_cast<std::int16_t>(std::clamp(sample, -32768.0f, 32767.0f));
            }
        }
        streamConcealedFrames += frames;
    }

    std::vector<std::int16_t> ConvertStreamToOutputFormatLocked(
        const std::vector<std::int16_t>& input,
        WORD inputChannels,
        DWORD inputSampleRate,
        float inputVolume)
    {
        std::vector<std::int16_t> mapped = MapToOutputChannels(input, inputChannels, inputVolume);
        if (mapped.empty())
            return {};

        if (inputSampleRate == outputSampleRate) {
            ResetStreamResamplerLocked();
            return mapped;
        }

        if (streamResampleInputSampleRate != inputSampleRate
            || streamResampleInputChannels != inputChannels
            || streamResampleCarry.size() != outputChannels) {
            ResetStreamResamplerLocked();
            streamResampleInputSampleRate = inputSampleRate;
            streamResampleInputChannels = inputChannels;
        }

        const std::size_t mappedFrames = mapped.size() / outputChannels;
        if (mappedFrames == 0)
            return {};

        streamResampleWork.clear();
        streamResampleWork.reserve(mapped.size() + outputChannels);
        if (!streamResampleCarry.empty())
            streamResampleWork.insert(streamResampleWork.end(), streamResampleCarry.begin(), streamResampleCarry.end());
        streamResampleWork.insert(streamResampleWork.end(), mapped.begin(), mapped.end());

        const std::size_t workFrames = streamResampleWork.size() / outputChannels;
        if (workFrames < 2) {
            streamResampleCarry = std::move(streamResampleWork);
            return {};
        }

        const double step = static_cast<double>(inputSampleRate) / static_cast<double>(outputSampleRate);
        const double lastFramePosition = static_cast<double>(workFrames - 1);
        double position = streamResamplePosition;
        if (position < 0.0 || position >= lastFramePosition)
            position = 0.0;

        const std::size_t estimatedFrames = static_cast<std::size_t>(std::ceil((lastFramePosition - position) / step));
        std::vector<std::int16_t> output;
        output.reserve(std::max<std::size_t>(1, estimatedFrames) * outputChannels);

        while (position < lastFramePosition) {
            const auto index0 = static_cast<std::size_t>(std::floor(position));
            const std::size_t index1 = std::min(index0 + 1, workFrames - 1);
            const double fraction = position - static_cast<double>(index0);
            for (WORD channel = 0; channel < outputChannels; channel++) {
                const float a = static_cast<float>(streamResampleWork[index0 * outputChannels + channel]);
                const float b = static_cast<float>(streamResampleWork[index1 * outputChannels + channel]);
                const float sample = a + static_cast<float>((b - a) * fraction);
                output.push_back(static_cast<std::int16_t>(std::clamp(sample, -32768.0f, 32767.0f)));
            }
            position += step;
        }

        streamResamplePosition = position - lastFramePosition;
        streamResampleCarry.assign(
            streamResampleWork.end() - static_cast<std::ptrdiff_t>(outputChannels),
            streamResampleWork.end());
        return output;
    }

    std::vector<std::int16_t> MapToOutputChannels(
        const std::vector<std::int16_t>& input,
        WORD inputChannels,
        float inputVolume) const
    {
        const std::size_t inputFrames = input.size() / inputChannels;
        if (inputFrames == 0)
            return {};

        std::vector<std::int16_t> output(inputFrames * outputChannels);
        const float finalVolume = std::clamp(inputVolume, 0.0f, 2.0f) * volume.load();
        for (std::size_t frame = 0; frame < inputFrames; frame++) {
            for (WORD channel = 0; channel < outputChannels; channel++) {
                const WORD inputChannel = inputChannels == 1
                    ? 0
                    : std::min<WORD>(channel, inputChannels - 1);
                const float sample = static_cast<float>(input[frame * inputChannels + inputChannel]) * finalVolume;
                output[frame * outputChannels + channel] = static_cast<std::int16_t>(std::clamp(sample, -32768.0f, 32767.0f));
            }
        }

        return output;
    }

    static std::wstring FormatHresult(const wchar_t* operation, HRESULT hr)
    {
        return FormatHresultLine(operation, hr);
    }

    std::vector<std::int16_t> ConvertToOutputFormat(
        const std::vector<std::int16_t>& input,
        WORD inputChannels,
        DWORD inputSampleRate,
        float inputVolume) const
    {
        const std::size_t inputFrames = input.size() / inputChannels;
        if (inputFrames == 0)
            return {};

        const double ratio = static_cast<double>(inputSampleRate) / static_cast<double>(outputSampleRate);
        const std::size_t outputFrames = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(static_cast<double>(inputFrames) / ratio)));
        std::vector<std::int16_t> output(outputFrames * outputChannels);
        const float finalVolume = std::clamp(inputVolume, 0.0f, 2.0f) * volume.load();

        for (std::size_t frame = 0; frame < outputFrames; frame++) {
            const double sourcePosition = static_cast<double>(frame) * ratio;
            const auto index0 = static_cast<std::size_t>(std::floor(sourcePosition));
            const std::size_t index1 = std::min(index0 + 1, inputFrames - 1);
            const double fraction = sourcePosition - static_cast<double>(index0);

            for (WORD channel = 0; channel < outputChannels; channel++) {
                const WORD inputChannel = inputChannels == 1
                    ? 0
                    : std::min<WORD>(channel, inputChannels - 1);
                const float a = static_cast<float>(input[index0 * inputChannels + inputChannel]);
                const float b = static_cast<float>(input[index1 * inputChannels + inputChannel]);
                const float sample = (a + static_cast<float>((b - a) * fraction)) * finalVolume;
                output[frame * outputChannels + channel] = static_cast<std::int16_t>(std::clamp(sample, -32768.0f, 32767.0f));
            }
        }

        return output;
    }

    HRESULT InitializeExclusiveFormat()
    {
        const DWORD sampleRates[] = {
            config.sampleRate,
            config.sampleRate == 48000 ? 44100UL : 48000UL,
        };
        const DWORD bufferMsValues[] = {
            config.bufferMs,
            10,
            20,
        };

        HRESULT lastHr = E_FAIL;
        for (DWORD sampleRate : sampleRates) {
            if (sampleRate == 0)
                continue;

            format = {};
            format.wFormatTag = WAVE_FORMAT_PCM;
            format.nChannels = config.channels == 0 ? OutputChannels : config.channels;
            format.nSamplesPerSec = sampleRate;
            format.wBitsPerSample = 16;
            format.nBlockAlign = static_cast<WORD>(format.nChannels * sizeof(std::int16_t));
            format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
            format.cbSize = 0;

            for (DWORD bufferMs : bufferMsValues) {
                if (bufferMs == 0)
                    continue;

                const REFERENCE_TIME duration = static_cast<REFERENCE_TIME>(bufferMs) * 10000;
                lastHr = audioClient->Initialize(
                    AUDCLNT_SHAREMODE_EXCLUSIVE,
                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                    duration,
                    duration,
                    &format,
                    nullptr);
                if (SUCCEEDED(lastHr)) {
                    outputSampleRate = sampleRate;
                    outputChannels = format.nChannels;
                    actualBufferMs = bufferMs;
                    return S_OK;
                }
            }
        }

        return lastHr;
    }

    void PrimeDeviceBuffer()
    {
        if (renderClient == nullptr || bufferFrameCount == 0 || outputChannels == 0)
            return;

        BYTE* data = nullptr;
        if (FAILED(renderClient->GetBuffer(bufferFrameCount, &data)))
            return;

        std::memset(data, 0, static_cast<std::size_t>(bufferFrameCount) * outputChannels * sizeof(std::int16_t));
        renderClient->ReleaseBuffer(bufferFrameCount, 0);
    }

    void RenderLoop()
    {
        DWORD taskIndex = 0;
        HANDLE mmcssHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        if (mmcssHandle != nullptr)
            AvSetMmThreadPriority(mmcssHandle, AVRT_PRIORITY_HIGH);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        const DWORD waitMs = actualBufferMs == 0
            ? 20
            : std::clamp<DWORD>(actualBufferMs * 2, 10, 50);

        while (running) {
            WaitForSingleObject(renderEvent, waitMs);
            if (!running)
                break;

            UINT32 padding = 0;
            if (FAILED(audioClient->GetCurrentPadding(&padding)))
                continue;

            const UINT32 framesAvailable = bufferFrameCount > padding
                ? bufferFrameCount - padding
                : 0;
            if (framesAvailable == 0)
                continue;

            BYTE* data = nullptr;
            if (FAILED(renderClient->GetBuffer(framesAvailable, &data)))
                continue;

            auto* output = reinterpret_cast<std::int16_t*>(data);
            const std::size_t samplesNeeded = static_cast<std::size_t>(framesAvailable) * outputChannels;
            std::fill(output, output + samplesNeeded, 0);
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (streamQueuedSamples != 0 || streamPrimed) {
                    const std::size_t queuedFrames = streamQueuedSamples / outputChannels;
                    if (streamPrimed || queuedFrames >= StreamPrebufferFrames()) {
                        streamPrimed = true;
                        RenderStreamSamples(output, framesAvailable);
                    }
                }

                auto write = activeClips.begin();
                for (auto read = activeClips.begin(); read != activeClips.end(); ++read) {
                    Clip& clip = *read;
                    const std::size_t remaining = clip.samples.size() - clip.cursor;
                    const std::size_t toMix = std::min(samplesNeeded, remaining);
                    for (std::size_t i = 0; i < toMix; i++) {
                        const int mixed = static_cast<int>(output[i]) + static_cast<int>(clip.samples[clip.cursor + i]);
                        output[i] = static_cast<std::int16_t>(std::clamp(mixed, -32768, 32767));
                    }

                    clip.cursor += toMix;
                    if (clip.cursor < clip.samples.size())
                        *write++ = std::move(clip);
                }
                activeClips.erase(write, activeClips.end());
            }

            renderClient->ReleaseBuffer(framesAvailable, 0);
        }

        if (mmcssHandle != nullptr)
            AvRevertMmThreadCharacteristics(mmcssHandle);
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> audioClient;
    ComPtr<IAudioRenderClient> renderClient;
    OutputConfig config;
    WAVEFORMATEX format {};
    DWORD outputSampleRate = 48000;
    WORD outputChannels = 2;
    DWORD actualBufferMs = 0;
    UINT32 bufferFrameCount = 0;
    HANDLE renderEvent = nullptr;
    std::thread renderThread;
    std::atomic<bool> running = false;
    std::atomic<float> volume = 1.0f;
    std::mutex mutex;
    std::mutex streamSubmitMutex;
    std::vector<std::int16_t> streamRing;
    std::size_t streamReadIndex = 0;
    std::size_t streamWriteIndex = 0;
    std::size_t streamQueuedSamples = 0;
    double streamReadFraction = 0.0;
    std::uint64_t streamChannel = 0;
    DWORD streamResampleInputSampleRate = 0;
    WORD streamResampleInputChannels = 0;
    double streamResamplePosition = 0.0;
    std::vector<std::int16_t> streamResampleCarry;
    std::vector<std::int16_t> streamResampleWork;
    std::uint64_t streamUnderruns = 0;
    std::uint64_t streamEmergencyTrims = 0;
    std::uint64_t streamConcealedFrames = 0;
    std::uint64_t streamRateCorrections = 0;
    std::uint64_t lastReportedUnderruns = 0;
    std::uint64_t lastReportedEmergencyTrims = 0;
    std::uint64_t lastReportedRateCorrections = 0;
    double lastStreamPlaybackRate = 1.0;
    std::chrono::steady_clock::time_point nextDiagnosticsTime {};
    std::vector<Clip> activeClips;
    std::vector<std::int16_t> lastStreamFrame;
    bool streamPrimed = false;
    bool streamLongForm = false;
    bool hasLastStreamFrame = false;
    bool comInitialized = false;
};

} // namespace

std::unique_ptr<IAudioOutput> CreateWasapiExclusiveOutput(OutputConfig config)
{
    return std::make_unique<WasapiExclusiveOutput>(std::move(config));
}

} // namespace olab::host
