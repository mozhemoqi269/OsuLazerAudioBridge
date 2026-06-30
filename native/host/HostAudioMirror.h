#pragma once

#include "HostAudioOutput.h"
#include "HostLogging.h"
#include "HostNativeAsioOutput.h"
#include "HostWasapiOutput.h"
#include "HostXAudio2Output.h"
#include "SampleDecoder.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace olab::host {
class AudioMirror {
public:
    explicit AudioMirror(OutputConfig config = {})
        : config(std::move(config))
    {
    }
    AudioMirror(const AudioMirror&) = delete;
    AudioMirror& operator=(const AudioMirror&) = delete;

    ~AudioMirror()
    {
        output.reset();
    }

    bool Start(std::wstring& error)
    {
        if (config.backend == OutputBackend::WasapiExclusive) {
            output = CreateWasapiExclusiveOutput(config);
            if (!output->Start(error)) {
                output.reset();
                return false;
            }

            active = true;
            return true;
        }

        if (config.backend == OutputBackend::Asio) {
            output = CreateNativeAsioOutput(config);
            if (!output->Start(error)) {
                output.reset();
                return false;
            }

            active = true;
            return true;
        }

        output = CreateXAudio2Output(config);
        if (!output->Start(error)) {
            output.reset();
            return false;
        }

        active = true;
        return true;
    }

    bool IsActive() const
    {
        return active;
    }

    bool LoadMemorySample(
        std::uint64_t sample,
        const std::uint8_t* data,
        std::uint64_t length,
        ProbeLog& log)
    {
        if (!active || data == nullptr || length == 0)
            return false;

        decodedSamples.erase(sample);
        preparedOutputSamples.erase(sample);
        if (length > MaxMirroredEffectContainerBytes) {
            LogSkippedEffectSample(sample, length, L"container-too-large", log);
            return false;
        }

        olab::DecodedSample decoded;
        std::string error;
        if (!olab::DecodeSampleMemory(data, length, decoded, error)) {
            std::wstringstream line;
            line << L"MirrorDecodeFailed sample=0x" << std::hex << sample << std::dec
                 << L" bytes=" << length
                 << L" error=\"" << error.c_str() << L"\"";
            PrintAndLogLine(line.str(), log);
            return false;
        }

        TrimDecodedEffectSample(sample, decoded, log);
        decodedSamples[sample] = std::move(decoded);
        PrepareOutputSample(sample);
        const olab::DecodedSample& stored = decodedSamples[sample];
        std::wstringstream line;
        line << L"MirrorDecodedSample sample=0x" << std::hex << sample << std::dec
             << L" containerBytes=" << length
             << L" format=\"" << stored.format.c_str() << L"\""
             << L" sampleRate=" << stored.sampleRate
             << L" channels=" << stored.channels
             << L" pcmSamples=" << stored.frames.size();
        PrintAndLogLine(line.str(), log);
        return true;
    }

    bool LoadPcmSample(
        std::uint64_t sample,
        const std::uint8_t* data,
        std::uint64_t byteLength,
        std::uint32_t sampleRate,
        std::uint32_t channels,
        ProbeLog& log)
    {
        if (!active || data == nullptr || byteLength == 0 || sampleRate == 0 || channels == 0)
            return false;

        decodedSamples.erase(sample);
        preparedOutputSamples.erase(sample);
        if (byteLength > MaxMirroredEffectPcmBytes) {
            LogSkippedEffectSample(sample, byteLength, L"pcm-too-large", log);
            return false;
        }

        const std::size_t sourceSamples = static_cast<std::size_t>(byteLength / sizeof(std::int16_t));
        const std::size_t sourceFrames = sourceSamples / static_cast<std::size_t>(channels);
        if (sourceFrames == 0)
            return false;

        const auto* source = reinterpret_cast<const std::int16_t*>(data);
        const std::size_t keptFrames = std::min<std::size_t>(
            sourceFrames,
            MaxMirroredEffectFrameCount(sampleRate));
        const std::size_t keptSamples = keptFrames * static_cast<std::size_t>(channels);
        olab::DecodedSample decoded;
        decoded.sampleRate = sampleRate;
        decoded.channels = channels;
        decoded.format = "pcm16";
        decoded.frames.resize(keptSamples);
        for (std::size_t i = 0; i < keptSamples; i++)
            decoded.frames[i] = source[i] / 32768.0f;

        if (keptFrames < sourceFrames) {
            FadeDecodedEffectTail(decoded);
            LogTrimmedEffectSample(sample, sourceSamples, keptSamples, log);
        }
        decodedSamples[sample] = std::move(decoded);
        PrepareOutputSample(sample);
        const olab::DecodedSample& stored = decodedSamples[sample];
        std::wstringstream line;
        line << L"MirrorDecodedPcmSample sample=0x" << std::hex << sample << std::dec
             << L" bytes=" << byteLength
             << L" sampleRate=" << stored.sampleRate
             << L" channels=" << stored.channels
             << L" pcmSamples=" << stored.frames.size();
        PrintAndLogLine(line.str(), log);
        return true;
    }

    bool PlaySample(std::uint64_t sample, std::uint64_t channel, float volume, double eventAgeUs, ProbeLog& log)
    {
        if (!active)
            return false;

        auto found = decodedSamples.find(sample);
        if (found == decodedSamples.end())
            return false;

        const olab::DecodedSample& decoded = found->second;
        if (decoded.frames.empty() || decoded.channels == 0 || decoded.sampleRate == 0)
            return false;

        std::wstring successLine;
        if (ShouldLogNextOutputSubmission()) {
            std::wstringstream line;
            line << L"MirrorPlayedSample sample=0x" << std::hex << sample << std::dec
                 << L" channel=0x" << std::hex << channel << std::dec
                 << L" sampleRate=" << decoded.sampleRate
                 << L" channels=" << decoded.channels
                 << L" pcmSamples=" << decoded.frames.size()
                 << L" volume=" << volume;
            if (eventAgeUs >= 0.0)
                line << L" eventAgeUs=" << std::fixed << std::setprecision(1) << eventAgeUs << std::defaultfloat;
            successLine = line.str();
        }

        auto prepared = preparedOutputSamples.find(sample);
        if (prepared == preparedOutputSamples.end()) {
            PrepareOutputSample(sample);
            prepared = preparedOutputSamples.find(sample);
        }
        if (prepared != preparedOutputSamples.end())
            return SubmitPreparedOutputPcm(channel, prepared->second, volume, successLine, log);

        std::vector<std::int16_t> pcm16(decoded.frames.size());
        for (std::size_t i = 0; i < decoded.frames.size(); i++) {
            float clamped = std::clamp(decoded.frames[i], -1.0f, 1.0f);
            pcm16[i] = static_cast<std::int16_t>(clamped >= 0
                ? clamped * 32767.0f
                : clamped * 32768.0f);
        }

        return SubmitOutputPcm(channel, std::move(pcm16), static_cast<WORD>(decoded.channels), decoded.sampleRate, volume, false, successLine, log);
    }

    bool LoadMusicStream(
        std::uint64_t stream,
        const std::uint8_t* data,
        std::uint64_t length,
        ProbeLog& log)
    {
        if (!active || data == nullptr || length == 0)
            return false;

        olab::DecodedSample decoded;
        std::string error;
        if (!olab::DecodeSampleMemory(data, length, decoded, error)) {
            std::wstringstream line;
            line << L"MirrorMusicDecodeFailed stream=0x" << std::hex << stream << std::dec
                 << L" bytes=" << length
                 << L" error=\"" << error.c_str() << L"\"";
            PrintAndLogLine(line.str(), log);
            return false;
        }

        musicStreams[stream] = std::move(decoded);
        const olab::DecodedSample& stored = musicStreams[stream];
        std::wstringstream line;
        line << L"MirrorMusicDecoded stream=0x" << std::hex << stream << std::dec
             << L" containerBytes=" << length
             << L" format=\"" << stored.format.c_str() << L"\""
             << L" sampleRate=" << stored.sampleRate
             << L" channels=" << stored.channels
             << L" pcmSamples=" << stored.frames.size();
        PrintAndLogLine(line.str(), log);
        return true;
    }

    bool PlayMusicStream(std::uint64_t stream, bool restart, float volume, ProbeLog& log)
    {
        return PlayMusicStream(stream, stream, restart, volume, log);
    }

    bool PlayMusicStream(std::uint64_t stream, std::uint64_t playbackChannel, bool restart, float volume, ProbeLog& log)
    {
        if (!active)
            return false;

        auto found = musicStreams.find(stream);
        if (found == musicStreams.end())
            return false;

        const olab::DecodedSample& decoded = found->second;
        if (decoded.frames.empty() || decoded.channels == 0 || decoded.sampleRate == 0)
            return false;

        std::vector<std::int16_t> pcm16(decoded.frames.size());
        for (std::size_t i = 0; i < decoded.frames.size(); i++) {
            float clamped = std::clamp(decoded.frames[i], -1.0f, 1.0f);
            pcm16[i] = static_cast<std::int16_t>(clamped >= 0
                ? clamped * 32767.0f
                : clamped * 32768.0f);
        }

        std::wstring successLine;
        if (ShouldLogNextOutputSubmission()) {
            std::wstringstream line;
            line << L"MirrorMusicPlayed stream=0x" << std::hex << stream << std::dec
                 << L" playbackChannel=0x" << std::hex << playbackChannel << std::dec
                 << L" restart=" << (restart ? 1 : 0)
                 << L" sampleRate=" << decoded.sampleRate
                 << L" channels=" << decoded.channels
                 << L" pcmSamples=" << decoded.frames.size()
                 << L" volume=" << volume;
            successLine = line.str();
        }
        output->StopChannel(playbackChannel);
        const bool streamOutput = config.backend != OutputBackend::XAudio2;
        return SubmitOutputPcm(playbackChannel, std::move(pcm16), static_cast<WORD>(decoded.channels), decoded.sampleRate, volume, streamOutput, successLine, log);
    }

    bool PlayPcmChunk(
        std::uint64_t handle,
        const std::uint8_t* data,
        std::uint64_t byteLength,
        std::uint64_t requestFlags,
        std::uint32_t sampleRate,
        std::uint32_t channels,
        std::uint32_t flags,
        float volume,
        ProbeLog& log)
    {
        if (!active || data == nullptr || byteLength == 0 || sampleRate == 0 || channels == 0)
            return false;

        static constexpr std::uint32_t BassSampleFloat = 0x100;
        static constexpr std::uint64_t BassDataFloat = 0x40000000;
        static constexpr std::uint64_t BassDataFft = 0x80000000;
        static constexpr std::uint64_t BassDataSizeMask = 0x0fffffff;
        if ((requestFlags & BassDataFft) != 0)
            return false;
        if ((requestFlags & BassDataSizeMask) == 0)
            return false;

        std::vector<std::int16_t> pcm16;
        if ((flags & BassSampleFloat) != 0 || (requestFlags & BassDataFloat) != 0) {
            const std::size_t samples = static_cast<std::size_t>(byteLength / sizeof(float));
            const auto* source = reinterpret_cast<const float*>(data);
            pcm16.resize(samples);
            for (std::size_t i = 0; i < samples; i++) {
                const float clamped = std::clamp(source[i], -1.0f, 1.0f);
                pcm16[i] = static_cast<std::int16_t>(clamped >= 0
                    ? clamped * 32767.0f
                    : clamped * 32768.0f);
            }
        } else {
            const std::size_t samples = static_cast<std::size_t>(byteLength / sizeof(std::int16_t));
            const auto* source = reinterpret_cast<const std::int16_t*>(data);
            pcm16.assign(source, source + samples);
        }

        if (pcm16.empty())
            return false;

        std::wstring successLine;
        if (ShouldLogNextOutputSubmission()) {
            std::wstringstream line;
            line << L"MirrorPcmChunkQueued handle=0x" << std::hex << handle << std::dec
                 << L" bytes=" << byteLength
                 << L" request=0x" << std::hex << requestFlags << std::dec
                 << L" sampleRate=" << sampleRate
                 << L" channels=" << channels
                 << L" flags=0x" << std::hex << flags << std::dec
                 << L" samples=" << pcm16.size()
                 << L" volume=" << volume;
            successLine = line.str();
        }
        return SubmitOutputPcm(handle, std::move(pcm16), static_cast<WORD>(channels), sampleRate, volume, true, successLine, log);
    }

    bool SeekMusicStream(std::uint64_t stream, std::uint64_t bytePosition, float volume, ProbeLog& log)
    {
        return SeekMusicStream(stream, stream, bytePosition, volume, log);
    }

    bool SeekMusicStream(std::uint64_t stream, std::uint64_t playbackChannel, std::uint64_t bytePosition, float volume, ProbeLog& log)
    {
        if (!active)
            return false;

        auto found = musicStreams.find(stream);
        if (found == musicStreams.end())
            return false;

        const olab::DecodedSample& decoded = found->second;
        if (decoded.frames.empty() || decoded.channels == 0 || decoded.sampleRate == 0)
            return false;

        const std::uint64_t bytesPerFrame = decoded.channels * sizeof(float);
        const std::uint64_t startFrame = bytesPerFrame == 0
            ? 0
            : bytePosition / bytesPerFrame;
        const std::uint64_t startSample = std::min<std::uint64_t>(
            startFrame * decoded.channels,
            static_cast<std::uint64_t>(decoded.frames.size()));

        std::vector<std::int16_t> pcm16(decoded.frames.size() - static_cast<std::size_t>(startSample));
        for (std::size_t i = 0; i < pcm16.size(); i++) {
            float clamped = std::clamp(decoded.frames[static_cast<std::size_t>(startSample) + i], -1.0f, 1.0f);
            pcm16[i] = static_cast<std::int16_t>(clamped >= 0
                ? clamped * 32767.0f
                : clamped * 32768.0f);
        }

        std::wstringstream line;
        line << L"MirrorMusicSeek stream=0x" << std::hex << stream << std::dec
             << L" playbackChannel=0x" << std::hex << playbackChannel << std::dec
             << L" bytePosition=" << bytePosition
             << L" startFrame=" << startFrame
             << L" remainingSamples=" << pcm16.size()
             << L" volume=" << volume;
        output->StopChannel(playbackChannel);
        const bool streamOutput = config.backend != OutputBackend::XAudio2;
        return SubmitOutputPcm(playbackChannel, std::move(pcm16), static_cast<WORD>(decoded.channels), decoded.sampleRate, volume, streamOutput, line.str(), log);
    }

    std::uint64_t StopChannel(std::uint64_t channel, const wchar_t* reason, ProbeLog& log)
    {
        if (!active)
            return 0;

        const std::uint64_t stopped = output != nullptr && output->StopChannel(channel) ? 1 : 0;
        if (stopped == 0)
            return 0;

        outputStopped += stopped;
        if (outputStopped > 3 && outputStopped % 100 != 0)
            return stopped;

        std::wstringstream line;
        line << L"MirrorStoppedChannel channel=0x" << std::hex << channel << std::dec
             << L" voices=" << stopped
             << L" stopped=" << outputStopped
             << L" reason=\"" << reason << L"\"";
        PrintAndLogLine(line.str(), log);
        return stopped;
    }

    void SetChannelVolume(std::uint64_t channel, float volume)
    {
        if (!active)
            return;

        if (output != nullptr)
            output->SetChannelVolume(channel, volume);
    }

    void SetRuntimeConfig(const OutputConfig& value)
    {
        config.effectsVolume = std::clamp(value.effectsVolume, 0.0f, 2.0f);
        config.musicVolume = std::clamp(value.musicVolume, 0.0f, 2.0f);
        config.mixOverlapDucking = std::clamp(value.mixOverlapDucking, 0.0f, 1.0f);

        if (output != nullptr)
            output->SetMixOverlapDucking(config.mixOverlapDucking);
    }

    std::uint64_t StopAllPcmStreams(const wchar_t* reason, ProbeLog& log)
    {
        if (output != nullptr) {
            std::wstringstream line;
            line << L"MirrorOutputStreamReset backend=\"" << output->Name()
                 << L"\" reason=\"" << reason << L"\"";
            output->ResetStream();
            PrintAndLogLine(line.str(), log);
            return 1;
        }

        return 0;
    }

    std::uint64_t StopAll(const wchar_t* reason, ProbeLog& log)
    {
        if (!active)
            return 0;

        const std::uint64_t stoppedVoices = 0;
        const std::uint64_t stoppedStreams = 0;
        std::uint64_t stoppedOutput = 0;
        if (output != nullptr) {
            output->ResetAll();
            stoppedOutput = 1;
        }

        const std::uint64_t stopped = stoppedVoices + stoppedStreams + stoppedOutput;
        if (stopped != 0) {
            std::wstringstream line;
            line << L"MirrorStoppedAll reason=\"" << reason << L"\""
                 << L" voices=" << stoppedVoices
                 << L" streams=" << stoppedStreams
                 << L" outputReset=" << stoppedOutput;
            PrintAndLogLine(line.str(), log);
        }

        return stopped;
    }

    bool PlayTestTone(ProbeLog& log)
    {
        if (!active)
            return false;

        constexpr DWORD sampleRate = 48000;
        constexpr WORD channels = 2;
        constexpr double frequencyHz = 880.0;
        constexpr double seconds = 0.35;
        constexpr double pi = 3.14159265358979323846;
        const std::size_t frameCount = static_cast<std::size_t>(sampleRate * seconds);

        std::vector<std::int16_t> pcm16(frameCount * channels);
        for (std::size_t frame = 0; frame < frameCount; frame++) {
            const double t = static_cast<double>(frame) / static_cast<double>(sampleRate);
            const double envelope = frame < 480
                ? static_cast<double>(frame) / 480.0
                : frame + 480 > frameCount
                    ? static_cast<double>(frameCount - frame) / 480.0
                    : 1.0;
            const auto sample = static_cast<std::int16_t>(std::sin(2.0 * pi * frequencyHz * t) * envelope * 12000.0);
            pcm16[frame * channels] = sample;
            pcm16[frame * channels + 1] = sample;
        }

        std::wstringstream line;
        line << L"TestTonePlayed sampleRate=" << sampleRate
             << L" channels=" << channels
             << L" frames=" << frameCount;
        return SubmitOutputPcm(0, std::move(pcm16), channels, sampleRate, 1.0f, false, line.str(), log);
    }

private:
    static constexpr std::uint64_t MaxMirroredEffectContainerBytes = 512ull * 1024ull;
    static constexpr std::uint64_t MaxMirroredEffectPcmBytes = 2ull * 1024ull * 1024ull;
    static constexpr double MaxMirroredEffectSeconds = 5.0;
    static constexpr std::size_t EffectTrimFadeFrames = 240;

    static void LogSkippedEffectSample(std::uint64_t sample, std::uint64_t bytes, const wchar_t* reason, ProbeLog& log)
    {
        std::wstringstream line;
        line << L"MirrorSkippedSample sample=0x" << std::hex << sample << std::dec
             << L" bytes=" << bytes
             << L" reason=\"" << reason << L"\"";
        PrintAndLogLine(line.str(), log);
    }

    static std::size_t MaxMirroredEffectFrameCount(std::uint32_t sampleRate)
    {
        return std::max<std::size_t>(
            1,
            static_cast<std::size_t>(std::lround(static_cast<double>(sampleRate) * MaxMirroredEffectSeconds)));
    }

    static void LogTrimmedEffectSample(std::uint64_t sample, std::size_t originalSamples, std::size_t keptSamples, ProbeLog& log)
    {
        std::wstringstream line;
        line << L"MirrorTrimmedSample sample=0x" << std::hex << sample << std::dec
             << L" originalSamples=" << originalSamples
             << L" keptSamples=" << keptSamples
             << L" maxMs=" << static_cast<int>(std::lround(MaxMirroredEffectSeconds * 1000.0));
        PrintAndLogLine(line.str(), log);
    }

    static void FadeDecodedEffectTail(olab::DecodedSample& decoded)
    {
        if (decoded.frames.empty() || decoded.channels == 0 || decoded.sampleRate == 0)
            return;

        const std::size_t channels = static_cast<std::size_t>(decoded.channels);
        const std::size_t frameCount = decoded.frames.size() / channels;
        if (frameCount == 0)
            return;

        const std::size_t fadeFrames = std::min<std::size_t>(EffectTrimFadeFrames, frameCount);
        const std::size_t fadeStartFrame = frameCount - fadeFrames;

        for (std::size_t frame = fadeStartFrame; frame < frameCount; frame++) {
            const float gain = static_cast<float>(frameCount - frame) / static_cast<float>(fadeFrames + 1);
            for (std::size_t channel = 0; channel < channels; channel++)
                decoded.frames[frame * channels + channel] *= gain;
        }
    }

    static void TrimDecodedEffectSample(std::uint64_t sample, olab::DecodedSample& decoded, ProbeLog& log)
    {
        if (decoded.frames.empty() || decoded.channels == 0 || decoded.sampleRate == 0)
            return;

        const std::size_t channels = static_cast<std::size_t>(decoded.channels);
        const std::size_t frameCount = decoded.frames.size() / channels;
        const std::size_t maxFrames = MaxMirroredEffectFrameCount(decoded.sampleRate);
        if (frameCount <= maxFrames)
            return;

        const std::size_t originalSamples = decoded.frames.size();
        const std::size_t keptSamples = maxFrames * channels;
        decoded.frames.resize(keptSamples);
        FadeDecodedEffectTail(decoded);
        LogTrimmedEffectSample(sample, originalSamples, keptSamples, log);
    }

    void PrepareOutputSample(std::uint64_t sample)
    {
        preparedOutputSamples.erase(sample);
        if (output == nullptr)
            return;

        const auto found = decodedSamples.find(sample);
        if (found == decodedSamples.end())
            return;

        const olab::DecodedSample& decoded = found->second;
        if (decoded.frames.empty() || decoded.channels == 0 || decoded.sampleRate == 0)
            return;

        auto prepared = output->PrepareFloatClip(
            decoded.frames,
            static_cast<WORD>(decoded.channels),
            decoded.sampleRate);
        if (prepared != nullptr)
            preparedOutputSamples[sample] = std::move(prepared);
    }

    bool SubmitPreparedOutputPcm(
        std::uint64_t channel,
        std::shared_ptr<const std::vector<float>> pcmFloat,
        float volume,
        const std::wstring& successLine,
        ProbeLog& log)
    {
        if (output == nullptr || pcmFloat == nullptr || pcmFloat->empty())
            return false;

        std::wstring error;
        if (!output->SubmitPreparedFloat(channel, std::move(pcmFloat), volume, error)) {
            if (!error.empty())
                PrintAndLogLine(error, log);
            return false;
        }

        outputSubmitted++;
        if (!successLine.empty() && ShouldLogOutputSubmission(outputSubmitted)) {
            std::wstringstream line;
            line << successLine
                 << L" backend=\"" << output->Name()
                 << L"\" prepared=1 submitted=" << outputSubmitted;
            PrintAndLogLine(line.str(), log);
        }
        WriteOutputDiagnosticsIfDue(log);
        return true;
    }

    bool SubmitOutputPcm(
        std::uint64_t channel,
        std::vector<std::int16_t> pcm16,
        WORD channels,
        DWORD sampleRate,
        float volume,
        bool stream,
        const std::wstring& successLine,
        ProbeLog& log)
    {
        if (output == nullptr)
            return false;

        std::wstring error;
        if (!output->Submit(channel, std::move(pcm16), channels, sampleRate, volume, stream, error)) {
            if (!error.empty())
                PrintAndLogLine(error, log);
            return false;
        }

        outputSubmitted++;
        if (!successLine.empty() && ShouldLogOutputSubmission(outputSubmitted)) {
            std::wstringstream line;
            line << successLine
                 << L" backend=\"" << output->Name()
                 << L"\" submitted=" << outputSubmitted;
            PrintAndLogLine(line.str(), log);
        }
        WriteOutputDiagnosticsIfDue(log);
        return true;
    }

    void WriteOutputDiagnosticsIfDue(ProbeLog& log)
    {
        if (!log.IsEnabled() || output == nullptr)
            return;

        if (outputSubmitted > 3 && outputSubmitted % 100 != 0)
            return;

        const std::wstring diagnostics = output->ConsumeDiagnostics();
        if (!diagnostics.empty())
            log.WriteLine(diagnostics);
    }

    static bool ShouldLogOutputSubmission(std::uint64_t submitted)
    {
        return submitted <= 3 || submitted % 100 == 0;
    }

    bool ShouldLogNextOutputSubmission() const
    {
        return ShouldLogOutputSubmission(outputSubmitted + 1);
    }

    OutputConfig config;
    std::unique_ptr<IAudioOutput> output;
    std::uint64_t outputSubmitted = 0;
    std::uint64_t outputStopped = 0;
    std::unordered_map<std::uint64_t, olab::DecodedSample> decodedSamples;
    std::unordered_map<std::uint64_t, std::shared_ptr<const std::vector<float>>> preparedOutputSamples;
    std::unordered_map<std::uint64_t, olab::DecodedSample> musicStreams;
    bool active = false;
};

} // namespace olab::host
