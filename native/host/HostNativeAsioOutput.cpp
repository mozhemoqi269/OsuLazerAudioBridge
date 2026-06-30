#include "HostNativeAsioOutput.h"

#include "HostAsioDevices.h"

#include <Windows.h>

#include <asiosys.h>
#include <asio.h>
#include <asiodrivers.h>
#include <iasiodrv.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

extern IASIO* theAsioDriver;

namespace olab::host {
namespace {
class NativeAsioOutput final : public IAudioOutput {
    struct FloatChunk {
        std::vector<float> samples;
        std::shared_ptr<const std::vector<float>> sharedSamples;
        std::uint64_t channel = 0;
        std::size_t cursor = 0;
        float gain = 1.0f;

        const std::vector<float>& Data() const
        {
            return sharedSamples != nullptr ? *sharedSamples : samples;
        }

        std::size_t SampleCount() const
        {
            return Data().size();
        }
    };

    enum class PendingCommandKind {
        AddClip,
        AddStream,
        StopChannel,
        ResetStream,
        ResetAll,
        SetMixOverlapDucking,
    };

    struct PendingCommand {
        PendingCommandKind kind = PendingCommandKind::ResetAll;
        FloatChunk chunk;
        std::uint64_t channel = 0;
        float value = 0.0f;
    };

public:
    explicit NativeAsioOutput(OutputConfig config)
        : config(std::move(config))
    {
    }

    ~NativeAsioOutput() override
    {
        Stop();
    }

    bool Start(std::wstring& error) override
    {
        const auto driverNameToLoad = ResolveDriverName();
        if (!driverNameToLoad) {
            error = L"AudioMirrorError operation=\"NativeASIO.ResolveDriver\" message=\"No ASIO driver found. Use --list-asio-devices.\"";
            return false;
        }

        driverName = *driverNameToLoad;
        driverNameUtf8 = WideToAnsi(driverName);
        if (driverNameUtf8.empty()) {
            error = L"AudioMirrorError operation=\"NativeASIO.DriverName\" message=\"Could not convert driver name.\"";
            return false;
        }

        asioDrivers = std::make_unique<AsioDrivers>();
        if (!asioDrivers->loadDriver(driverNameUtf8.data()) || theAsioDriver == nullptr) {
            error = L"AudioMirrorError operation=\"NativeASIO.LoadDriver\" driver=\"" + driverName + L"\"";
            Stop();
            return false;
        }

        ASIODriverInfo info {};
        info.asioVersion = 2;
        info.sysRef = nullptr;
        if (theAsioDriver->init(&info) != ASIOTrue) {
            error = L"AudioMirrorError operation=\"NativeASIO.Init\" driver=\"" + driverName + L"\"";
            Stop();
            return false;
        }
        initialized = true;

        if (config.sampleRate != 0 && theAsioDriver->canSampleRate(config.sampleRate) == ASE_OK)
            theAsioDriver->setSampleRate(config.sampleRate);

        ASIOSampleRate asioRate = 0;
        if (theAsioDriver->getSampleRate(&asioRate) != ASE_OK || asioRate <= 0)
            asioRate = config.sampleRate == 0 ? 48000.0 : static_cast<double>(config.sampleRate);
        outputSampleRate = static_cast<DWORD>(std::lround(asioRate));
        outputChannels = config.channels == 0 ? 2 : config.channels;

        long inputChannels = 0;
        long driverOutputChannels = 0;
        if (theAsioDriver->getChannels(&inputChannels, &driverOutputChannels) != ASE_OK || driverOutputChannels <= 0) {
            error = L"AudioMirrorError operation=\"NativeASIO.GetChannels\"";
            Stop();
            return false;
        }
        outputChannels = static_cast<WORD>(std::min<long>(std::max<WORD>(1, outputChannels), driverOutputChannels));

        long minBuffer = 0;
        long maxBuffer = 0;
        long preferredBuffer = 0;
        long granularity = 0;
        if (theAsioDriver->getBufferSize(&minBuffer, &maxBuffer, &preferredBuffer, &granularity) != ASE_OK || preferredBuffer <= 0) {
            error = L"AudioMirrorError operation=\"NativeASIO.GetBufferSize\"";
            Stop();
            return false;
        }
        bufferFrames = ResolveBufferFrames(minBuffer, maxBuffer, preferredBuffer, granularity);

        bufferInfos.assign(outputChannels, {});
        channelInfos.assign(outputChannels, {});
        for (WORD channel = 0; channel < outputChannels; channel++) {
            bufferInfos[channel].isInput = ASIOFalse;
            bufferInfos[channel].channelNum = channel;
            channelInfos[channel].channel = channel;
            channelInfos[channel].isInput = ASIOFalse;
            if (theAsioDriver->getChannelInfo(&channelInfos[channel]) != ASE_OK) {
                error = L"AudioMirrorError operation=\"NativeASIO.GetChannelInfo\" channel=" + std::to_wstring(channel);
                Stop();
                return false;
            }
        }

        currentInstance = this;
        callbacks.bufferSwitch = &NativeAsioOutput::BufferSwitchThunk;
        callbacks.sampleRateDidChange = &NativeAsioOutput::SampleRateDidChangeThunk;
        callbacks.asioMessage = &NativeAsioOutput::AsioMessageThunk;
        callbacks.bufferSwitchTimeInfo = &NativeAsioOutput::BufferSwitchTimeInfoThunk;

        if (theAsioDriver->createBuffers(bufferInfos.data(), outputChannels, bufferFrames, &callbacks) != ASE_OK) {
            error = L"AudioMirrorError operation=\"NativeASIO.CreateBuffers\"";
            Stop();
            return false;
        }
        buffersCreated = true;
        renderScratch.assign(static_cast<std::size_t>(bufferFrames) * outputChannels, 0.0f);
        activeClips.reserve(256);
        pendingCommands.reserve(256);
        renderCommands.reserve(256);
        renderMixOverlapDucking = std::clamp(config.mixOverlapDucking, 0.0f, 1.0f);
        mixOverlapDuckingSnapshot.store(renderMixOverlapDucking, std::memory_order_relaxed);

        long inputLatency = 0;
        long outputLatency = 0;
        theAsioDriver->getLatencies(&inputLatency, &outputLatency);
        reportedOutputLatency = outputLatency;

        if (theAsioDriver->start() != ASE_OK) {
            error = L"AudioMirrorError operation=\"NativeASIO.Start\"";
            Stop();
            return false;
        }
        running = true;
        return true;
    }

    bool Submit(std::uint64_t channel, std::vector<std::int16_t> pcm16, WORD channels, DWORD sampleRate, float volume, bool stream, std::wstring& error) override
    {
        if (!running || pcm16.empty() || channels == 0 || sampleRate == 0)
            return false;

        std::vector<float> converted = ConvertToFloat(pcm16, channels, sampleRate, volume);
        if (converted.empty())
            return false;

        EnqueueCommand(PendingCommand {
            stream ? PendingCommandKind::AddStream : PendingCommandKind::AddClip,
            FloatChunk { std::move(converted), nullptr, channel, 0, 1.0f },
            channel,
            0.0f,
        });

        error.clear();
        return true;
    }

    std::shared_ptr<const std::vector<float>> PrepareFloatClip(const std::vector<float>& pcmFloat, WORD channels, DWORD sampleRate) override
    {
        if (!running || pcmFloat.empty() || channels == 0 || sampleRate == 0)
            return nullptr;

        std::vector<float> converted = ConvertFloatToOutput(pcmFloat, channels, sampleRate, 1.0f);
        if (converted.empty())
            return nullptr;

        return std::make_shared<const std::vector<float>>(std::move(converted));
    }

    bool SubmitPreparedFloat(std::uint64_t channel, std::shared_ptr<const std::vector<float>> pcmFloat, float volume, std::wstring& error) override
    {
        if (!running || pcmFloat == nullptr || pcmFloat->empty())
            return false;

        EnqueueCommand(PendingCommand {
            PendingCommandKind::AddClip,
            FloatChunk { {}, std::move(pcmFloat), channel, 0, std::clamp(volume, 0.0f, 2.0f) },
            channel,
            0.0f,
        });
        error.clear();
        return true;
    }

    void ResetStream() override
    {
        EnqueueCommand(PendingCommand { PendingCommandKind::ResetStream });
    }

    void ResetAll() override
    {
        EnqueueCommand(PendingCommand { PendingCommandKind::ResetAll });
    }

    bool StopChannel(std::uint64_t channel) override
    {
        if (channel == 0)
            return false;

        EnqueueCommand(PendingCommand { PendingCommandKind::StopChannel, {}, channel });
        return true;
    }

    void Stop() override
    {
        running = false;
        if (currentInstance == this)
            currentInstance = nullptr;

        if (theAsioDriver != nullptr) {
            theAsioDriver->stop();
            if (buffersCreated) {
                theAsioDriver->disposeBuffers();
                buffersCreated = false;
            }
        }
        if (asioDrivers) {
            asioDrivers->removeCurrentDriver();
            asioDrivers.reset();
        }
        theAsioDriver = nullptr;
        initialized = false;
        {
            std::lock_guard<std::mutex> lock(commandMutex);
            pendingCommands.clear();
        }
        streamChunks.clear();
        activeClips.clear();
        streamQueuedSamples = 0;
        observedStreamQueuedSamples.store(0, std::memory_order_relaxed);
        observedActiveClips.store(0, std::memory_order_relaxed);
    }

    void SetVolume(float) override
    {
    }

    void SetMixOverlapDucking(float value) override
    {
        EnqueueCommand(PendingCommand {
            PendingCommandKind::SetMixOverlapDucking,
            {},
            0,
            std::clamp(value, 0.0f, 1.0f),
        });
    }

    const wchar_t* Name() const override
    {
        return L"native-asio";
    }

    std::wstring ConsumeDiagnostics() override
    {
        std::wstring driverNameSnapshot;
        DWORD outputSampleRateSnapshot = 0;
        WORD outputChannelsSnapshot = 0;
        long bufferFramesSnapshot = 0;
        long reportedOutputLatencySnapshot = 0;
        std::size_t streamQueuedSamplesSnapshot = 0;
        std::size_t maxObservedStreamQueuedFramesSnapshot = 0;
        std::size_t targetStreamQueuedFramesSnapshot = 0;
        std::uint64_t callbackCountSnapshot = 0;
        std::uint64_t underrunCallbacksSnapshot = 0;
        std::uint64_t droppedStreamFramesSnapshot = 0;
        std::uint64_t clippedSamplesSnapshot = 0;
        std::uint64_t softLimitedSamplesSnapshot = 0;
        float maxMixedAbsSampleSnapshot = 0.0f;
        float mixOverlapDuckingSnapshot = 0.0f;
        float lastMixBusGainSnapshot = 1.0f;
        std::size_t activeClipCountSnapshot = 0;
        const std::uint64_t lockMisses = realtimeLockMisses.load(std::memory_order_relaxed);
        std::lock_guard<std::mutex> diagnosticsLock(diagnosticsMutex);

        const auto now = std::chrono::steady_clock::now();
        if (now < nextDiagnosticsTime)
            return {};

        callbackCountSnapshot = callbackCount.load(std::memory_order_relaxed);
        underrunCallbacksSnapshot = underrunCallbacks.load(std::memory_order_relaxed);
        droppedStreamFramesSnapshot = droppedStreamFrames.load(std::memory_order_relaxed);
        clippedSamplesSnapshot = clippedSamples.load(std::memory_order_relaxed);
        softLimitedSamplesSnapshot = softLimitedSamples.load(std::memory_order_relaxed);
        if (callbackCountSnapshot == lastReportedCallbackCount
            && underrunCallbacksSnapshot == lastReportedUnderrunCallbacks
            && droppedStreamFramesSnapshot == lastReportedDroppedStreamFrames
            && clippedSamplesSnapshot == lastReportedClippedSamples
            && softLimitedSamplesSnapshot == lastReportedSoftLimitedSamples
            && lockMisses == lastReportedRealtimeLockMisses)
            return {};

        nextDiagnosticsTime = now + std::chrono::seconds(2);
        lastReportedCallbackCount = callbackCountSnapshot;
        lastReportedUnderrunCallbacks = underrunCallbacksSnapshot;
        lastReportedDroppedStreamFrames = droppedStreamFramesSnapshot;
        lastReportedClippedSamples = clippedSamplesSnapshot;
        lastReportedSoftLimitedSamples = softLimitedSamplesSnapshot;
        lastReportedRealtimeLockMisses = lockMisses;

        driverNameSnapshot = driverName;
        outputSampleRateSnapshot = outputSampleRate;
        outputChannelsSnapshot = outputChannels;
        bufferFramesSnapshot = bufferFrames;
        reportedOutputLatencySnapshot = reportedOutputLatency;
        streamQueuedSamplesSnapshot = observedStreamQueuedSamples.load(std::memory_order_relaxed);
        maxObservedStreamQueuedFramesSnapshot = maxObservedStreamQueuedFrames.load(std::memory_order_relaxed);
        targetStreamQueuedFramesSnapshot = MaxStreamQueuedFrames();
        maxMixedAbsSampleSnapshot = maxMixedAbsSample.load(std::memory_order_relaxed);
        mixOverlapDuckingSnapshot = this->mixOverlapDuckingSnapshot.load(std::memory_order_relaxed);
        lastMixBusGainSnapshot = lastMixBusGain.load(std::memory_order_relaxed);
        activeClipCountSnapshot = observedActiveClips.load(std::memory_order_relaxed);

        std::wstringstream line;
        line << L"AsioDiagnostics driver=\"" << driverNameSnapshot
             << L"\" sampleRate=" << outputSampleRateSnapshot
             << L" channels=" << outputChannelsSnapshot
             << L" bufferFrames=" << bufferFramesSnapshot
             << L" outputLatencyFrames=" << reportedOutputLatencySnapshot
             << L" callbacks=" << callbackCountSnapshot
             << L" streamQueuedFrames=" << (outputChannelsSnapshot == 0 ? 0 : streamQueuedSamplesSnapshot / outputChannelsSnapshot)
             << L" maxStreamQueuedFrames=" << maxObservedStreamQueuedFramesSnapshot
             << L" targetStreamQueuedFrames=" << targetStreamQueuedFramesSnapshot
             << L" streamUnderruns=" << underrunCallbacksSnapshot
             << L" droppedStreamFrames=" << droppedStreamFramesSnapshot
             << L" realtimeLockMisses=" << lockMisses
             << L" clippedSamples=" << clippedSamplesSnapshot
             << L" softLimitedSamples=" << softLimitedSamplesSnapshot
             << L" peak=" << std::fixed << std::setprecision(3) << maxMixedAbsSampleSnapshot << std::defaultfloat
             << L" mixOverlapDucking=" << static_cast<int>(std::lround(mixOverlapDuckingSnapshot * 100.0f))
             << L" lastMixBusGain=" << std::fixed << std::setprecision(3) << lastMixBusGainSnapshot << std::defaultfloat
             << L" activeClips=" << activeClipCountSnapshot;
        return line.str();
    }

private:
    void EnqueueCommand(PendingCommand command)
    {
        std::lock_guard<std::mutex> lock(commandMutex);
        pendingCommands.push_back(std::move(command));
    }

    void ApplyPendingCommands()
    {
        renderCommands.clear();
        if (commandMutex.try_lock()) {
            renderCommands.swap(pendingCommands);
            commandMutex.unlock();
        } else {
            realtimeLockMisses.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        for (PendingCommand& command : renderCommands) {
            switch (command.kind) {
            case PendingCommandKind::AddClip:
                activeClips.push_back(std::move(command.chunk));
                break;

            case PendingCommandKind::AddStream:
                streamQueuedSamples += command.chunk.SampleCount();
                streamChunks.push_back(std::move(command.chunk));
                TrimStreamQueueToFrames(MaxStreamQueuedFrames());
                ObserveMax(maxObservedStreamQueuedFrames, outputChannels == 0 ? 0 : streamQueuedSamples / outputChannels);
                break;

            case PendingCommandKind::StopChannel:
                StopChannelNow(command.channel);
                break;

            case PendingCommandKind::ResetStream:
                streamChunks.clear();
                streamQueuedSamples = 0;
                break;

            case PendingCommandKind::ResetAll:
                streamChunks.clear();
                activeClips.clear();
                streamQueuedSamples = 0;
                break;

            case PendingCommandKind::SetMixOverlapDucking:
                renderMixOverlapDucking = std::clamp(command.value, 0.0f, 1.0f);
                mixOverlapDuckingSnapshot.store(renderMixOverlapDucking, std::memory_order_relaxed);
                break;
            }
        }
    }

    bool StopChannelNow(std::uint64_t channel)
    {
        bool stopped = false;
        auto keptStream = std::deque<FloatChunk> {};
        for (FloatChunk& chunk : streamChunks) {
            if (chunk.channel == channel) {
                streamQueuedSamples -= std::min(streamQueuedSamples, chunk.SampleCount() - chunk.cursor);
                stopped = true;
            } else {
                keptStream.push_back(std::move(chunk));
            }
        }
        streamChunks = std::move(keptStream);

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

    static void ObserveMax(std::atomic<std::size_t>& target, std::size_t value)
    {
        std::size_t observed = target.load(std::memory_order_relaxed);
        while (value > observed && !target.compare_exchange_weak(observed, value, std::memory_order_relaxed)) {
        }
    }

    static void ObserveMax(std::atomic<float>& target, float value)
    {
        float observed = target.load(std::memory_order_relaxed);
        while (value > observed && !target.compare_exchange_weak(observed, value, std::memory_order_relaxed)) {
        }
    }

    std::optional<std::wstring> ResolveDriverName() const
    {
        return ResolveAsioDriverName(config.deviceId);
    }

    long ResolveBufferFrames(long minBuffer, long maxBuffer, long preferredBuffer, long granularity) const
    {
        if (config.bufferMs == 0 || config.bufferMs == 10 || outputSampleRate == 0)
            return preferredBuffer;

        long requested = static_cast<long>(std::lround(
            static_cast<double>(outputSampleRate) * static_cast<double>(config.bufferMs) / 1000.0));
        requested = std::clamp(requested, minBuffer, maxBuffer);

        if (granularity == -1) {
            long best = minBuffer;
            for (long value = 1; value > 0 && value <= maxBuffer; value <<= 1) {
                if (value < minBuffer)
                    continue;
                if (std::llabs(static_cast<long long>(value) - requested) < std::llabs(static_cast<long long>(best) - requested))
                    best = value;
            }
            return best;
        }

        if (granularity > 1) {
            const long steps = std::max<long>(0, (requested - minBuffer + granularity / 2) / granularity);
            requested = minBuffer + steps * granularity;
        }

        return std::clamp(requested, minBuffer, maxBuffer);
    }

    std::size_t MaxStreamQueuedFrames() const
    {
        if (outputSampleRate == 0)
            return std::max<std::size_t>(static_cast<std::size_t>(bufferFrames), 1);

        return std::max<std::size_t>(
            static_cast<std::size_t>(bufferFrames),
            static_cast<std::size_t>(outputSampleRate) / 200);
    }

    void TrimStreamQueueToFrames(std::size_t maxFrames)
    {
        if (outputChannels == 0)
            return;

        const std::size_t maxSamples = maxFrames * outputChannels;
        if (streamQueuedSamples <= maxSamples)
            return;

        std::size_t samplesToDrop = streamQueuedSamples - maxSamples;
        droppedStreamFrames.fetch_add(samplesToDrop / outputChannels, std::memory_order_relaxed);
        while (samplesToDrop != 0 && !streamChunks.empty()) {
            FloatChunk& chunk = streamChunks.front();
            const std::size_t remaining = chunk.SampleCount() - chunk.cursor;
            const std::size_t dropped = std::min(samplesToDrop, remaining);
            chunk.cursor += dropped;
            samplesToDrop -= dropped;
            streamQueuedSamples -= dropped;
            if (chunk.cursor >= chunk.SampleCount())
                streamChunks.pop_front();
        }
    }

    static std::string WideToAnsi(const std::wstring& value)
    {
        if (value.empty())
            return {};

        int length = WideCharToMultiByte(CP_ACP, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        if (length <= 0)
            return {};

        std::string result(static_cast<std::size_t>(length), '\0');
        WideCharToMultiByte(CP_ACP, 0, value.c_str(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
        return result;
    }

    std::vector<float> ConvertToFloat(const std::vector<std::int16_t>& input, WORD inputChannels, DWORD inputSampleRate, float inputVolume) const
    {
        const std::size_t inputFrames = input.size() / inputChannels;
        if (inputFrames == 0)
            return {};

        const double ratio = static_cast<double>(inputSampleRate) / static_cast<double>(outputSampleRate);
        const std::size_t outputFrames = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(static_cast<double>(inputFrames) / ratio)));
        std::vector<float> output(outputFrames * outputChannels);
        const float gain = std::clamp(inputVolume, 0.0f, 2.0f);

        for (std::size_t frame = 0; frame < outputFrames; frame++) {
            const double sourcePosition = static_cast<double>(frame) * ratio;
            const auto index0 = static_cast<std::size_t>(std::floor(sourcePosition));
            const std::size_t index1 = std::min(index0 + 1, inputFrames - 1);
            const double fraction = sourcePosition - static_cast<double>(index0);

            for (WORD channel = 0; channel < outputChannels; channel++) {
                const WORD inputChannel = inputChannels == 1
                    ? 0
                    : std::min<WORD>(channel, inputChannels - 1);
                const float a = input[index0 * inputChannels + inputChannel] / 32768.0f;
                const float b = input[index1 * inputChannels + inputChannel] / 32768.0f;
                output[frame * outputChannels + channel] = std::clamp((a + static_cast<float>((b - a) * fraction)) * gain, -1.0f, 1.0f);
            }
        }

        return output;
    }

    std::vector<float> ConvertFloatToOutput(const std::vector<float>& input, WORD inputChannels, DWORD inputSampleRate, float inputVolume) const
    {
        const std::size_t inputFrames = input.size() / inputChannels;
        if (inputFrames == 0)
            return {};

        const double ratio = static_cast<double>(inputSampleRate) / static_cast<double>(outputSampleRate);
        const std::size_t outputFrames = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(static_cast<double>(inputFrames) / ratio)));
        std::vector<float> output(outputFrames * outputChannels);
        const float gain = std::clamp(inputVolume, 0.0f, 2.0f);

        for (std::size_t frame = 0; frame < outputFrames; frame++) {
            const double sourcePosition = static_cast<double>(frame) * ratio;
            const auto index0 = static_cast<std::size_t>(std::floor(sourcePosition));
            const std::size_t index1 = std::min(index0 + 1, inputFrames - 1);
            const double fraction = sourcePosition - static_cast<double>(index0);

            for (WORD channel = 0; channel < outputChannels; channel++) {
                const WORD inputChannel = inputChannels == 1
                    ? 0
                    : std::min<WORD>(channel, inputChannels - 1);
                const float a = std::clamp(input[index0 * inputChannels + inputChannel], -1.0f, 1.0f);
                const float b = std::clamp(input[index1 * inputChannels + inputChannel], -1.0f, 1.0f);
                output[frame * outputChannels + channel] = std::clamp((a + static_cast<float>((b - a) * fraction)) * gain, -1.0f, 1.0f);
            }
        }

        return output;
    }

    void Render(long doubleBufferIndex)
    {
        if (doubleBufferIndex < 0 || doubleBufferIndex > 1)
            return;

        if (renderScratch.size() != static_cast<std::size_t>(bufferFrames) * outputChannels)
            return;

        std::fill(renderScratch.begin(), renderScratch.end(), 0.0f);
        ApplyPendingCommands();
        float busGain = 1.0f;

        callbackCount.fetch_add(1, std::memory_order_relaxed);
        const bool streamWasQueued = streamQueuedSamples != 0;
        std::size_t cursor = 0;
        while (cursor < renderScratch.size() && !streamChunks.empty()) {
            FloatChunk& chunk = streamChunks.front();
            const std::vector<float>& samples = chunk.Data();
            const std::size_t remaining = samples.size() - chunk.cursor;
            const std::size_t copied = std::min(renderScratch.size() - cursor, remaining);
            std::copy(
                samples.begin() + static_cast<std::ptrdiff_t>(chunk.cursor),
                samples.begin() + static_cast<std::ptrdiff_t>(chunk.cursor + copied),
                renderScratch.begin() + static_cast<std::ptrdiff_t>(cursor));
            chunk.cursor += copied;
            cursor += copied;
            streamQueuedSamples -= std::min(streamQueuedSamples, copied);
            if (chunk.cursor >= samples.size())
                streamChunks.pop_front();
        }
        if (streamWasQueued && cursor < renderScratch.size())
            underrunCallbacks.fetch_add(1, std::memory_order_relaxed);

        std::size_t mixedClipCount = 0;
        auto write = activeClips.begin();
        for (auto read = activeClips.begin(); read != activeClips.end(); ++read) {
            const std::vector<float>& samples = read->Data();
            const std::size_t remaining = samples.size() - read->cursor;
            const std::size_t mixed = std::min(renderScratch.size(), remaining);
            if (mixed != 0)
                mixedClipCount++;
            for (std::size_t i = 0; i < mixed; i++) {
                renderScratch[i] += samples[read->cursor + i] * read->gain;
            }

            read->cursor += mixed;
            if (read->cursor < samples.size())
                *write++ = std::move(*read);
        }
        activeClips.erase(write, activeClips.end());
        observedStreamQueuedSamples.store(streamQueuedSamples, std::memory_order_relaxed);
        observedActiveClips.store(activeClips.size(), std::memory_order_relaxed);

        busGain = MixBusGain(mixedClipCount);
        lastMixBusGain.store(busGain, std::memory_order_relaxed);

        std::uint64_t localClippedSamples = 0;
        std::uint64_t localSoftLimitedSamples = 0;
        float localMaxMixedAbsSample = 0.0f;
        for (float& sample : renderScratch) {
            sample *= busGain;
            const float absolute = std::fabs(sample);
            localMaxMixedAbsSample = std::max(localMaxMixedAbsSample, absolute);
            if (absolute > 1.0f)
                localClippedSamples++;
            if (absolute > SoftLimitThreshold) {
                sample = SoftLimit(sample);
                localSoftLimitedSamples++;
            }
        }
        if (localMaxMixedAbsSample != 0.0f || localClippedSamples != 0 || localSoftLimitedSamples != 0) {
            ObserveMax(maxMixedAbsSample, localMaxMixedAbsSample);
            clippedSamples.fetch_add(localClippedSamples, std::memory_order_relaxed);
            softLimitedSamples.fetch_add(localSoftLimitedSamples, std::memory_order_relaxed);
        }

        for (WORD channel = 0; channel < outputChannels; channel++) {
            void* buffer = bufferInfos[channel].buffers[doubleBufferIndex];
            if (buffer == nullptr)
                continue;

            WriteChannelBuffer(buffer, channelInfos[channel].type, renderScratch, channel);
        }

        if (theAsioDriver != nullptr && theAsioDriver->outputReady() == ASE_OK) {
        }
    }

    void WriteChannelBuffer(void* buffer, ASIOSampleType type, const std::vector<float>& interleaved, WORD channel) const
    {
        switch (type) {
        case ASIOSTFloat32LSB: {
            auto* out = static_cast<float*>(buffer);
            for (long frame = 0; frame < bufferFrames; frame++)
                out[frame] = interleaved[static_cast<std::size_t>(frame) * outputChannels + channel];
            break;
        }
        case ASIOSTFloat64LSB: {
            auto* out = static_cast<double*>(buffer);
            for (long frame = 0; frame < bufferFrames; frame++)
                out[frame] = interleaved[static_cast<std::size_t>(frame) * outputChannels + channel];
            break;
        }
        case ASIOSTInt16LSB: {
            auto* out = static_cast<std::int16_t*>(buffer);
            for (long frame = 0; frame < bufferFrames; frame++) {
                const float sample = std::clamp(interleaved[static_cast<std::size_t>(frame) * outputChannels + channel], -1.0f, 1.0f);
                out[frame] = static_cast<std::int16_t>(sample >= 0 ? sample * 32767.0f : sample * 32768.0f);
            }
            break;
        }
        case ASIOSTInt24LSB: {
            auto* out = static_cast<std::uint8_t*>(buffer);
            for (long frame = 0; frame < bufferFrames; frame++) {
                const std::int32_t value = FloatToSignedInt(interleaved[static_cast<std::size_t>(frame) * outputChannels + channel], 24);
                out[frame * 3 + 0] = static_cast<std::uint8_t>(value & 0xff);
                out[frame * 3 + 1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
                out[frame * 3 + 2] = static_cast<std::uint8_t>((value >> 16) & 0xff);
            }
            break;
        }
        case ASIOSTInt32LSB16: {
            WriteInt32ChannelBuffer(buffer, interleaved, channel, 16, false);
            break;
        }
        case ASIOSTInt32LSB18: {
            WriteInt32ChannelBuffer(buffer, interleaved, channel, 18, false);
            break;
        }
        case ASIOSTInt32LSB20: {
            WriteInt32ChannelBuffer(buffer, interleaved, channel, 20, false);
            break;
        }
        case ASIOSTInt32LSB24: {
            WriteInt32ChannelBuffer(buffer, interleaved, channel, 24, false);
            break;
        }
        case ASIOSTInt32LSB: {
            WriteInt32ChannelBuffer(buffer, interleaved, channel, 32, false);
            break;
        }
        case ASIOSTFloat32MSB: {
            auto* out = static_cast<std::uint8_t*>(buffer);
            for (long frame = 0; frame < bufferFrames; frame++) {
                std::uint32_t bits = 0;
                const float value = interleaved[static_cast<std::size_t>(frame) * outputChannels + channel];
                std::memcpy(&bits, &value, sizeof(bits));
                WriteBigEndian(out + frame * 4, bits, 4);
            }
            break;
        }
        case ASIOSTFloat64MSB: {
            auto* out = static_cast<std::uint8_t*>(buffer);
            for (long frame = 0; frame < bufferFrames; frame++) {
                std::uint64_t bits = 0;
                const double value = interleaved[static_cast<std::size_t>(frame) * outputChannels + channel];
                std::memcpy(&bits, &value, sizeof(bits));
                WriteBigEndian(out + frame * 8, bits, 8);
            }
            break;
        }
        case ASIOSTInt16MSB: {
            auto* out = static_cast<std::uint8_t*>(buffer);
            for (long frame = 0; frame < bufferFrames; frame++) {
                const auto value = static_cast<std::uint16_t>(FloatToSignedInt(interleaved[static_cast<std::size_t>(frame) * outputChannels + channel], 16));
                WriteBigEndian(out + frame * 2, value, 2);
            }
            break;
        }
        case ASIOSTInt24MSB: {
            auto* out = static_cast<std::uint8_t*>(buffer);
            for (long frame = 0; frame < bufferFrames; frame++) {
                const auto value = static_cast<std::uint32_t>(FloatToSignedInt(interleaved[static_cast<std::size_t>(frame) * outputChannels + channel], 24));
                WriteBigEndian(out + frame * 3, value, 3);
            }
            break;
        }
        case ASIOSTInt32MSB16: {
            WriteInt32ChannelBuffer(buffer, interleaved, channel, 16, true);
            break;
        }
        case ASIOSTInt32MSB18: {
            WriteInt32ChannelBuffer(buffer, interleaved, channel, 18, true);
            break;
        }
        case ASIOSTInt32MSB20: {
            WriteInt32ChannelBuffer(buffer, interleaved, channel, 20, true);
            break;
        }
        case ASIOSTInt32MSB24: {
            WriteInt32ChannelBuffer(buffer, interleaved, channel, 24, true);
            break;
        }
        case ASIOSTInt32MSB: {
            WriteInt32ChannelBuffer(buffer, interleaved, channel, 32, true);
            break;
        }
        default:
            std::memset(buffer, 0, static_cast<std::size_t>(bufferFrames) * 4);
            break;
        }
    }

    static std::int32_t FloatToSignedInt(float sample, int bits)
    {
        sample = std::clamp(sample, -1.0f, 1.0f);
        const double positive = static_cast<double>((1ULL << (bits - 1)) - 1);
        const double negative = static_cast<double>(1ULL << (bits - 1));
        return static_cast<std::int32_t>(sample >= 0.0f ? sample * positive : sample * negative);
    }

    static float SoftLimit(float sample)
    {
        const float absolute = std::fabs(sample);
        if (absolute <= SoftLimitThreshold)
            return sample;

        const float sign = sample < 0.0f ? -1.0f : 1.0f;
        const float range = 1.0f - SoftLimitThreshold;
        const float excess = absolute - SoftLimitThreshold;
        const float limited = SoftLimitThreshold + range * (1.0f - std::exp(-excess / range));
        return sign * std::min(limited, 1.0f);
    }

    float MixBusGain(std::size_t activeClipCount) const
    {
        if (activeClipCount <= 1)
            return 1.0f;

        const float ducking = std::clamp(renderMixOverlapDucking, 0.0f, 1.0f);
        return 1.0f / (1.0f + static_cast<float>(activeClipCount - 1) * ducking);
    }

    static void WriteBigEndian(std::uint8_t* output, std::uint64_t value, int bytes)
    {
        for (int i = 0; i < bytes; i++)
            output[i] = static_cast<std::uint8_t>((value >> ((bytes - 1 - i) * 8)) & 0xff);
    }

    void WriteInt32ChannelBuffer(void* buffer, const std::vector<float>& interleaved, WORD channel, int validBits, bool bigEndian) const
    {
        auto* out = static_cast<std::uint8_t*>(buffer);
        const int shift = validBits == 32 ? 0 : 32 - validBits;
        for (long frame = 0; frame < bufferFrames; frame++) {
            std::int32_t value = FloatToSignedInt(interleaved[static_cast<std::size_t>(frame) * outputChannels + channel], validBits);
            value <<= shift;
            if (bigEndian) {
                WriteBigEndian(out + frame * 4, static_cast<std::uint32_t>(value), 4);
            } else {
                auto* intOut = reinterpret_cast<std::int32_t*>(out);
                intOut[frame] = value;
            }
        }
    }

    static void BufferSwitchThunk(long doubleBufferIndex, ASIOBool)
    {
        if (currentInstance != nullptr)
            currentInstance->Render(doubleBufferIndex);
    }

    static void SampleRateDidChangeThunk(ASIOSampleRate)
    {
    }

    static long AsioMessageThunk(long selector, long, void*, double*)
    {
        switch (selector) {
        case kAsioSelectorSupported:
        case kAsioEngineVersion:
            return 2;
        case kAsioSupportsTimeInfo:
            return 1;
        default:
            return 0;
        }
    }

    static ASIOTime* BufferSwitchTimeInfoThunk(ASIOTime* params, long doubleBufferIndex, ASIOBool)
    {
        if (currentInstance != nullptr)
            currentInstance->Render(doubleBufferIndex);
        return params;
    }

    OutputConfig config;
    std::unique_ptr<AsioDrivers> asioDrivers;
    ASIOCallbacks callbacks {};
    std::vector<ASIOBufferInfo> bufferInfos;
    std::vector<ASIOChannelInfo> channelInfos;
    std::wstring driverName;
    std::string driverNameUtf8;
    std::vector<float> renderScratch;
    WORD outputChannels = 2;
    DWORD outputSampleRate = 48000;
    long bufferFrames = 0;
    long reportedOutputLatency = 0;
    std::atomic<bool> running = false;
    bool initialized = false;
    bool buffersCreated = false;
    std::mutex commandMutex;
    std::mutex diagnosticsMutex;
    std::vector<PendingCommand> pendingCommands;
    std::vector<PendingCommand> renderCommands;
    std::deque<FloatChunk> streamChunks;
    std::vector<FloatChunk> activeClips;
    std::size_t streamQueuedSamples = 0;
    float renderMixOverlapDucking = 0.25f;
    std::atomic<std::size_t> observedStreamQueuedSamples { 0 };
    std::atomic<std::size_t> maxObservedStreamQueuedFrames { 0 };
    std::atomic<std::size_t> observedActiveClips { 0 };
    std::atomic<std::uint64_t> callbackCount { 0 };
    std::atomic<std::uint64_t> underrunCallbacks { 0 };
    std::atomic<std::uint64_t> droppedStreamFrames { 0 };
    std::atomic<std::uint64_t> clippedSamples { 0 };
    std::atomic<std::uint64_t> softLimitedSamples { 0 };
    std::uint64_t lastReportedCallbackCount = 0;
    std::uint64_t lastReportedUnderrunCallbacks = 0;
    std::uint64_t lastReportedDroppedStreamFrames = 0;
    std::uint64_t lastReportedClippedSamples = 0;
    std::uint64_t lastReportedSoftLimitedSamples = 0;
    std::uint64_t lastReportedRealtimeLockMisses = 0;
    std::atomic<std::uint64_t> realtimeLockMisses { 0 };
    std::atomic<float> maxMixedAbsSample { 0.0f };
    std::atomic<float> mixOverlapDuckingSnapshot { 0.25f };
    std::atomic<float> lastMixBusGain { 1.0f };
    static constexpr float SoftLimitThreshold = 0.92f;
    std::chrono::steady_clock::time_point nextDiagnosticsTime {};
    static inline NativeAsioOutput* currentInstance = nullptr;
};

} // namespace

std::unique_ptr<IAudioOutput> CreateNativeAsioOutput(OutputConfig config)
{
    return std::make_unique<NativeAsioOutput>(std::move(config));
}

} // namespace olab::host
