#include "sound.h"

#include <algorithm>
#include <atomic>
#include <mutex>

#ifdef _WIN32
#include <array>
#include <cmath>
#include <cstdint>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#endif
#endif

namespace
{
std::atomic_bool soundEnabled{true};
std::atomic_int soundVolumePercent{100};
constexpr int DefaultRestoredVolumePercent = 60;

enum class SoundCue
{
    Move,
    SoftDrop,
    HardDrop,
    Hold,
    Rotate,
    ClearSingle,
    ClearDouble,
    ClearTriple,
    ClearTetris,
    SpinClear,
    BackToBack,
    PerfectClear,
    LevelUp,
    Pause,
    GameOver,
    Silent,
    Count
};

int ClampVolume(int value)
{
    return std::max(0, std::min(100, value));
}

#ifdef _WIN32
constexpr int SampleRate = 44100;
constexpr double Pi = 3.14159265358979323846;
constexpr double TwoPi = Pi * 2.0;
constexpr double MasterOutputGain = 0.30;
constexpr double SoftLimiterCeiling = 32767.0;
constexpr int CueCount = static_cast<int>(SoundCue::Count);
constexpr int OutputBufferSamples = 256;
constexpr int OutputBufferCount = 3;
constexpr std::size_t MaxActiveVoices = 24;

struct ActiveVoice
{
    const std::vector<short> *samples = nullptr;
    std::size_t position = 0;
    bool important = false;
};

std::array<std::vector<short>, CueCount> soundCache;
std::array<DWORD, CueCount> lastCuePlayTimes = {};
std::array<std::vector<short>, OutputBufferCount> mixerBuffers;
std::array<WAVEHDR, OutputBufferCount> mixerHeaders = {};
std::array<bool, OutputBufferCount> mixerBufferQueued = {};
std::mutex mixerLock;
std::mutex outputLock;
std::vector<ActiveVoice> activeVoices;
HANDLE mixerEvent = nullptr;
HANDLE mixerThread = nullptr;
HWAVEOUT mixerOutput = nullptr;
std::atomic_bool mixerShutdownRequested{false};
bool soundCacheReady = false;

int CueIndex(SoundCue cue)
{
    return static_cast<int>(cue);
}

int FrameFromMs(double milliseconds)
{
    return static_cast<int>(std::round(milliseconds * SampleRate / 1000.0));
}

double SmoothStep(double value)
{
    value = std::max(0.0, std::min(1.0, value));
    return value * value * (3.0 - 2.0 * value);
}

double GetEnvelope(double localMs, double durationMs, double attackMs, double releaseMs)
{
    double envelope = 1.0;
    if (attackMs > 0.0)
    {
        envelope = std::min(envelope, localMs / attackMs);
    }
    if (releaseMs > 0.0)
    {
        envelope = std::min(envelope, (durationMs - localMs) / releaseMs);
    }
    return SmoothStep(envelope);
}

void AddTone(std::vector<double> &mix,
             double startMs,
             double durationMs,
             double startFrequency,
             double endFrequency,
             double gain,
             double attackMs = 1.0,
             double releaseMs = 12.0)
{
    int startFrame = std::max(0, FrameFromMs(startMs));
    int endFrame = std::min(static_cast<int>(mix.size()), FrameFromMs(startMs + durationMs));
    double phase = 0.0;

    for (int frame = startFrame; frame < endFrame; frame++)
    {
        double localMs = (frame - startFrame) * 1000.0 / SampleRate;
        double progress = durationMs <= 0.0 ? 1.0 : localMs / durationMs;
        double frequency = startFrequency + (endFrequency - startFrequency) * progress;
        phase += TwoPi * frequency / SampleRate;
        mix[frame] += std::sin(phase) * gain * GetEnvelope(localMs, durationMs, attackMs, releaseMs);
    }
}

void AddNoise(std::vector<double> &mix,
              double startMs,
              double durationMs,
              double gain,
              double decayMs,
              unsigned int seed)
{
    int startFrame = std::max(0, FrameFromMs(startMs));
    int endFrame = std::min(static_cast<int>(mix.size()), FrameFromMs(startMs + durationMs));
    unsigned int state = seed;

    for (int frame = startFrame; frame < endFrame; frame++)
    {
        double localMs = (frame - startFrame) * 1000.0 / SampleRate;
        state = state * 1664525u + 1013904223u;
        double noise = (static_cast<double>((state >> 8) & 0xffffu) / 32767.5) - 1.0;
        double envelope = std::exp(-localMs / std::max(1.0, decayMs));
        envelope *= GetEnvelope(localMs, durationMs, 0.2, 4.0);
        mix[frame] += noise * gain * envelope;
    }
}

void AddNote(std::vector<double> &mix, double startMs, double durationMs, double frequency, double gain)
{
    AddTone(mix, startMs, durationMs, frequency, frequency * 1.015, gain, 1.0, durationMs * 0.55);
    AddTone(mix, startMs, durationMs * 0.8, frequency * 2.0, frequency * 2.02, gain * 0.32, 0.6, durationMs * 0.45);
}

std::vector<double> MakeMix(double durationMs)
{
    return std::vector<double>(std::max(1, FrameFromMs(durationMs)), 0.0);
}

std::vector<short> ConvertToPcm(const std::vector<double> &mix)
{
    std::vector<short> samples;
    samples.reserve(mix.size());

    for (double sample : mix)
    {
        sample *= MasterOutputGain;
        sample = std::max(-1.0, std::min(1.0, sample));
        samples.push_back(static_cast<short>(std::round(sample * 30000.0)));
    }
    return samples;
}

std::vector<short> BuildCueSamples(SoundCue cue)
{
    std::vector<double> mix;

    switch (cue)
    {
    case SoundCue::Move:
        mix = MakeMix(24.0);
        AddNoise(mix, 0.0, 7.0, 0.04, 2.0, 11u);
        AddTone(mix, 0.0, 22.0, 620.0, 760.0, 0.13, 0.4, 9.0);
        break;
    case SoundCue::SoftDrop:
        mix = MakeMix(20.0);
        AddNoise(mix, 0.0, 6.0, 0.035, 2.0, 17u);
        AddTone(mix, 0.0, 18.0, 980.0, 760.0, 0.12, 0.35, 8.0);
        break;
    case SoundCue::HardDrop:
        mix = MakeMix(54.0);
        AddNoise(mix, 0.0, 8.0, 0.10, 3.0, 23u);
        AddTone(mix, 0.0, 26.0, 1350.0, 2400.0, 0.16, 0.25, 9.0);
        AddTone(mix, 10.0, 24.0, 760.0, 980.0, 0.06, 0.4, 12.0);
        break;
    case SoundCue::Hold:
        mix = MakeMix(62.0);
        AddNoise(mix, 0.0, 8.0, 0.05, 3.0, 29u);
        AddTone(mix, 0.0, 38.0, 620.0, 880.0, 0.12, 0.6, 16.0);
        AddTone(mix, 18.0, 34.0, 880.0, 1180.0, 0.10, 0.6, 14.0);
        break;
    case SoundCue::Rotate:
        mix = MakeMix(28.0);
        AddNoise(mix, 0.0, 5.0, 0.03, 2.0, 31u);
        AddTone(mix, 0.0, 24.0, 980.0, 1320.0, 0.13, 0.35, 9.0);
        break;
    case SoundCue::ClearSingle:
        mix = MakeMix(115.0);
        AddNoise(mix, 0.0, 16.0, 0.06, 6.0, 37u);
        AddNote(mix, 0.0, 62.0, 760.0, 0.12);
        AddNote(mix, 38.0, 62.0, 1140.0, 0.10);
        break;
    case SoundCue::ClearDouble:
        mix = MakeMix(145.0);
        AddNoise(mix, 0.0, 20.0, 0.07, 7.0, 41u);
        AddNote(mix, 0.0, 64.0, 700.0, 0.11);
        AddNote(mix, 36.0, 70.0, 880.0, 0.11);
        AddNote(mix, 78.0, 58.0, 1175.0, 0.10);
        break;
    case SoundCue::ClearTriple:
        mix = MakeMix(178.0);
        AddNoise(mix, 0.0, 24.0, 0.08, 8.0, 43u);
        AddNote(mix, 0.0, 68.0, 700.0, 0.11);
        AddNote(mix, 36.0, 72.0, 880.0, 0.11);
        AddNote(mix, 76.0, 72.0, 1175.0, 0.10);
        AddNote(mix, 116.0, 54.0, 1480.0, 0.09);
        break;
    case SoundCue::ClearTetris:
        mix = MakeMix(255.0);
        AddNoise(mix, 0.0, 14.0, 0.11, 4.0, 47u);
        AddNoise(mix, 24.0, 135.0, 0.035, 34.0, 53u);
        AddNote(mix, 0.0, 72.0, 659.0, 0.11);
        AddNote(mix, 42.0, 76.0, 831.0, 0.12);
        AddNote(mix, 84.0, 80.0, 1046.0, 0.12);
        AddNote(mix, 126.0, 88.0, 1318.0, 0.13);
        AddNote(mix, 178.0, 60.0, 1760.0, 0.10);
        break;
    case SoundCue::SpinClear:
        mix = MakeMix(200.0);
        AddNoise(mix, 0.0, 18.0, 0.08, 6.0, 59u);
        AddTone(mix, 0.0, 115.0, 980.0, 1760.0, 0.12, 0.6, 48.0);
        AddTone(mix, 22.0, 98.0, 1320.0, 1975.0, 0.08, 0.8, 36.0);
        AddNote(mix, 112.0, 70.0, 1760.0, 0.09);
        break;
    case SoundCue::BackToBack:
        mix = MakeMix(205.0);
        AddNoise(mix, 0.0, 12.0, 0.10, 4.0, 61u);
        AddNote(mix, 0.0, 72.0, 1046.0, 0.11);
        AddNote(mix, 48.0, 76.0, 1318.0, 0.12);
        AddNote(mix, 98.0, 84.0, 1760.0, 0.12);
        break;
    case SoundCue::PerfectClear:
        mix = MakeMix(330.0);
        AddNoise(mix, 0.0, 16.0, 0.11, 5.0, 67u);
        AddNoise(mix, 36.0, 200.0, 0.035, 50.0, 71u);
        AddNote(mix, 0.0, 78.0, 880.0, 0.11);
        AddNote(mix, 44.0, 82.0, 1108.0, 0.11);
        AddNote(mix, 88.0, 86.0, 1318.0, 0.11);
        AddNote(mix, 132.0, 92.0, 1760.0, 0.12);
        AddNote(mix, 188.0, 90.0, 2217.0, 0.09);
        AddNote(mix, 236.0, 74.0, 2637.0, 0.07);
        break;
    case SoundCue::LevelUp:
        mix = MakeMix(250.0);
        AddNoise(mix, 0.0, 14.0, 0.11, 5.0, 73u);
        AddNote(mix, 0.0, 80.0, 740.0, 0.12);
        AddNote(mix, 54.0, 86.0, 932.0, 0.13);
        AddNote(mix, 112.0, 112.0, 1175.0, 0.14);
        break;
    case SoundCue::Pause:
        mix = MakeMix(82.0);
        AddTone(mix, 0.0, 42.0, 560.0, 460.0, 0.10, 0.8, 16.0);
        AddTone(mix, 32.0, 38.0, 420.0, 360.0, 0.08, 0.8, 16.0);
        break;
    case SoundCue::GameOver:
        mix = MakeMix(420.0);
        AddTone(mix, 0.0, 110.0, 330.0, 315.0, 0.11, 3.0, 45.0);
        AddTone(mix, 96.0, 120.0, 247.0, 238.0, 0.11, 3.0, 52.0);
        AddTone(mix, 202.0, 150.0, 196.0, 178.0, 0.10, 3.0, 72.0);
        AddTone(mix, 310.0, 90.0, 165.0, 150.0, 0.08, 3.0, 58.0);
        break;
    case SoundCue::Silent:
        mix = MakeMix(3.0);
        break;
    case SoundCue::Count:
        mix = MakeMix(1.0);
        break;
    }

    return ConvertToPcm(mix);
}

WAVEFORMATEX MakeWaveFormat()
{
    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = SampleRate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    return format;
}

double GetRuntimeVolumeScale()
{
    double normalizedVolume = static_cast<double>(ClampVolume(soundVolumePercent.load())) / 100.0;
    return std::pow(normalizedVolume, 1.45);
}

double GetOverlapDucking(int contributingVoices)
{
    if (contributingVoices <= 1)
    {
        return 1.0;
    }
    return 1.0 / std::sqrt(static_cast<double>(contributingVoices));
}

short LimitMixedSample(double sample)
{
    sample = std::tanh(sample / SoftLimiterCeiling) * SoftLimiterCeiling;
    sample = std::max(-32768.0, std::min(32767.0, sample));
    return static_cast<short>(std::round(sample));
}

bool MixOutputBuffer(std::vector<short> &buffer)
{
    std::fill(buffer.begin(), buffer.end(), 0);
    std::lock_guard<std::mutex> lock(mixerLock);
    if (activeVoices.empty())
    {
        return false;
    }

    bool mixedAnySamples = false;
    double volumeScale = GetRuntimeVolumeScale();

    for (std::size_t frame = 0; frame < buffer.size(); frame++)
    {
        double mixedSample = 0.0;
        int contributingVoices = 0;
        for (ActiveVoice &voice : activeVoices)
        {
            if (voice.samples != nullptr && voice.position < voice.samples->size())
            {
                mixedSample += static_cast<double>((*voice.samples)[voice.position]) * volumeScale;
                voice.position++;
                contributingVoices++;
                mixedAnySamples = true;
            }
        }
        mixedSample *= GetOverlapDucking(contributingVoices);
        buffer[frame] = LimitMixedSample(mixedSample);
    }

    activeVoices.erase(std::remove_if(activeVoices.begin(), activeVoices.end(),
                                      [](const ActiveVoice &voice) {
                                          return voice.samples == nullptr || voice.position >= voice.samples->size();
                                      }),
                       activeVoices.end());
    return mixedAnySamples;
}

bool HasActiveVoices()
{
    std::lock_guard<std::mutex> lock(mixerLock);
    return !activeVoices.empty();
}

int GetCueCooldownMs(SoundCue cue)
{
    switch (cue)
    {
    case SoundCue::Move:
        return 18;
    case SoundCue::SoftDrop:
        return 28;
    case SoundCue::Rotate:
        return 18;
    case SoundCue::Hold:
        return 60;
    default:
        return 0;
    }
}

bool ShouldSkipCue(SoundCue cue, bool important)
{
    if (important)
    {
        return false;
    }

    int cooldownMs = GetCueCooldownMs(cue);
    if (cooldownMs <= 0)
    {
        return false;
    }

    DWORD now = GetTickCount();
    int cueIndex = CueIndex(cue);
    DWORD elapsed = now - lastCuePlayTimes[cueIndex];
    if (elapsed < static_cast<DWORD>(cooldownMs))
    {
        return true;
    }

    lastCuePlayTimes[cueIndex] = now;
    return false;
}

bool EnsureSoundLibrary()
{
    if (soundCacheReady)
    {
        return true;
    }

    for (int index = 0; index < CueCount; index++)
    {
        SoundCue cue = static_cast<SoundCue>(index);
        soundCache[index] = BuildCueSamples(cue);
    }
    soundCacheReady = true;
    return true;
}

bool WriteReadyMixerBuffers()
{
    std::lock_guard<std::mutex> outputGuard(outputLock);
    if (mixerOutput == nullptr)
    {
        return false;
    }

    bool wroteAnyBuffer = false;
    while (!mixerShutdownRequested.load() && HasActiveVoices())
    {
        int readyIndex = -1;
        for (int index = 0; index < OutputBufferCount; index++)
        {
            if (!mixerBufferQueued[index] || (mixerHeaders[index].dwFlags & WHDR_DONE) != 0)
            {
                readyIndex = index;
                break;
            }
        }

        if (readyIndex < 0)
        {
            break;
        }

        if (!MixOutputBuffer(mixerBuffers[readyIndex]))
        {
            break;
        }

        MMRESULT result = waveOutWrite(mixerOutput, &mixerHeaders[readyIndex], sizeof(mixerHeaders[readyIndex]));
        if (result != MMSYSERR_NOERROR)
        {
            mixerBufferQueued[readyIndex] = false;
            break;
        }
        mixerBufferQueued[readyIndex] = true;
        wroteAnyBuffer = true;
    }
    return wroteAnyBuffer;
}

DWORD WINAPI MixerThreadMain(LPVOID)
{
    while (true)
    {
        WaitForSingleObject(mixerEvent, INFINITE);
        if (mixerShutdownRequested.load())
        {
            return 0;
        }

        WriteReadyMixerBuffers();
    }
}

bool EnsureMixer()
{
    if (!soundEnabled.load() || soundVolumePercent.load() <= 0)
    {
        return false;
    }
    if (!EnsureSoundLibrary())
    {
        return false;
    }
    if (mixerOutput != nullptr && mixerThread != nullptr && mixerEvent != nullptr)
    {
        return true;
    }

    mixerEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (mixerEvent == nullptr)
    {
        return false;
    }

    WAVEFORMATEX format = MakeWaveFormat();
    MMRESULT result = waveOutOpen(&mixerOutput, WAVE_MAPPER, &format,
                                  reinterpret_cast<DWORD_PTR>(mixerEvent), 0, CALLBACK_EVENT);
    if (result != MMSYSERR_NOERROR)
    {
        CloseHandle(mixerEvent);
        mixerEvent = nullptr;
        mixerOutput = nullptr;
        return false;
    }

    for (int index = 0; index < OutputBufferCount; index++)
    {
        mixerBuffers[index].assign(OutputBufferSamples, 0);
        mixerHeaders[index] = {};
        mixerHeaders[index].lpData = reinterpret_cast<LPSTR>(mixerBuffers[index].data());
        mixerHeaders[index].dwBufferLength = static_cast<DWORD>(mixerBuffers[index].size() * sizeof(short));

        result = waveOutPrepareHeader(mixerOutput, &mixerHeaders[index], sizeof(mixerHeaders[index]));
        if (result != MMSYSERR_NOERROR)
        {
            for (int preparedIndex = 0; preparedIndex < index; preparedIndex++)
            {
                waveOutUnprepareHeader(mixerOutput, &mixerHeaders[preparedIndex], sizeof(mixerHeaders[preparedIndex]));
            }
            waveOutClose(mixerOutput);
            CloseHandle(mixerEvent);
            mixerOutput = nullptr;
            mixerEvent = nullptr;
            return false;
        }
        mixerBufferQueued[index] = false;
    }

    mixerShutdownRequested.store(false);
    mixerThread = CreateThread(nullptr, 0, MixerThreadMain, nullptr, 0, nullptr);
    if (mixerThread == nullptr)
    {
        std::lock_guard<std::mutex> outputGuard(outputLock);
        waveOutReset(mixerOutput);
        for (int index = 0; index < OutputBufferCount; index++)
        {
            waveOutUnprepareHeader(mixerOutput, &mixerHeaders[index], sizeof(mixerHeaders[index]));
        }
        waveOutClose(mixerOutput);
        CloseHandle(mixerEvent);
        mixerOutput = nullptr;
        mixerEvent = nullptr;
        return false;
    }
    return true;
}

void StopSoundPlayback()
{
    std::lock_guard<std::mutex> outputGuard(outputLock);
    {
        std::lock_guard<std::mutex> lock(mixerLock);
        activeVoices.clear();
    }

    if (mixerOutput != nullptr)
    {
        waveOutReset(mixerOutput);
        mixerBufferQueued.fill(false);
    }
}

void MarkSoundCacheDirty()
{
    soundCacheReady = false;
}

void ShutdownMixer()
{
    StopSoundPlayback();

    mixerShutdownRequested.store(true);
    if (mixerEvent != nullptr)
    {
        SetEvent(mixerEvent);
    }

    if (mixerThread != nullptr)
    {
        WaitForSingleObject(mixerThread, INFINITE);
        CloseHandle(mixerThread);
        mixerThread = nullptr;
    }

    if (mixerOutput != nullptr)
    {
        waveOutReset(mixerOutput);
        for (int index = 0; index < OutputBufferCount; index++)
        {
            waveOutUnprepareHeader(mixerOutput, &mixerHeaders[index], sizeof(mixerHeaders[index]));
            mixerHeaders[index] = {};
            mixerBufferQueued[index] = false;
            mixerBuffers[index].clear();
        }
        waveOutClose(mixerOutput);
        mixerOutput = nullptr;
    }

    if (mixerEvent != nullptr)
    {
        CloseHandle(mixerEvent);
        mixerEvent = nullptr;
    }
    mixerShutdownRequested.store(false);
}

void QueueCueForMixer(SoundCue cue, bool important)
{
    const std::vector<short> &samples = soundCache[CueIndex(cue)];
    if (samples.empty())
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mixerLock);
        if (activeVoices.size() >= MaxActiveVoices)
        {
            auto removable = std::find_if(activeVoices.begin(), activeVoices.end(),
                                          [](const ActiveVoice &voice) {
                                              return !voice.important;
                                          });
            if (removable != activeVoices.end())
            {
                activeVoices.erase(removable);
            }
            else
            {
                activeVoices.erase(activeVoices.begin());
            }
        }
        activeVoices.push_back(ActiveVoice{&samples, 0, important});
    }

    SetEvent(mixerEvent);
}
#else
void StopSoundPlayback()
{
}

void MarkSoundCacheDirty()
{
}

void ShutdownMixer()
{
}
#endif

void PlayCue(SoundCue cue, bool important)
{
    if (!soundEnabled.load() || soundVolumePercent.load() <= 0)
    {
        return;
    }

#ifdef _WIN32
    if (!EnsureMixer())
    {
        return;
    }
    if (ShouldSkipCue(cue, important))
    {
        return;
    }
    QueueCueForMixer(cue, important);
#else
    (void)cue;
    (void)important;
#endif
}

SoundCue GetClearCue(int completedLines, bool spinClear, bool backToBack, bool perfectClear)
{
    if (perfectClear)
    {
        return SoundCue::PerfectClear;
    }
    if (backToBack)
    {
        return SoundCue::BackToBack;
    }
    if (spinClear)
    {
        return SoundCue::SpinClear;
    }

    switch (completedLines)
    {
    case 1:
        return SoundCue::ClearSingle;
    case 2:
        return SoundCue::ClearDouble;
    case 3:
        return SoundCue::ClearTriple;
    case 4:
        return SoundCue::ClearTetris;
    default:
        return SoundCue::ClearSingle;
    }
}
}

void WarmUpSound()
{
#ifdef _WIN32
    if (EnsureSoundLibrary())
    {
        PlayCue(SoundCue::Silent, true);
        StopSoundPlayback();
    }
#endif
}

void SetSoundEnabled(bool enabled)
{
    soundEnabled.store(enabled);
    if (!enabled)
    {
        soundVolumePercent.store(0);
        StopSoundPlayback();
        return;
    }
    if (soundVolumePercent.load() == 0)
    {
        soundVolumePercent.store(DefaultRestoredVolumePercent);
        MarkSoundCacheDirty();
    }
}

bool IsSoundEnabled()
{
    return soundEnabled.load();
}

void SetSoundVolumePercent(int volumePercent)
{
    int clampedVolumePercent = ClampVolume(volumePercent);
    if (soundVolumePercent.load() != clampedVolumePercent)
    {
        StopSoundPlayback();
    }
    soundVolumePercent.store(clampedVolumePercent);
    soundEnabled.store(clampedVolumePercent > 0);
}

int GetSoundVolumePercent()
{
    return soundVolumePercent.load();
}

void PlayMoveSound()
{
    PlayCue(SoundCue::Move, false);
}

void PlaySoftDropSound()
{
    PlayCue(SoundCue::SoftDrop, false);
}

void PlayHardDropSound()
{
    PlayCue(SoundCue::HardDrop, true);
}

void PlayHoldSound()
{
    PlayCue(SoundCue::Hold, false);
}

void PlayRotateSound()
{
    PlayCue(SoundCue::Rotate, false);
}

void PlayLineClearSound(int completedLines, bool spinClear)
{
    PlayCue(GetClearCue(completedLines, spinClear, false, false), true);
}

void PlayClearResultSound(int completedLines, bool spinClear, bool backToBack, bool perfectClear)
{
    PlayCue(GetClearCue(completedLines, spinClear, backToBack, perfectClear), true);
}

void PlayBackToBackSound()
{
    PlayCue(SoundCue::BackToBack, true);
}

void PlayPerfectClearSound()
{
    PlayCue(SoundCue::PerfectClear, true);
}

void PlayLevelUpSound()
{
    PlayCue(SoundCue::LevelUp, false);
}

void PlayPauseSound()
{
    PlayCue(SoundCue::Pause, true);
}

void PlayGameOverSound()
{
    PlayCue(SoundCue::GameOver, true);
}

void ShutdownSound()
{
    ShutdownMixer();
}

