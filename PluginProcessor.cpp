#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>
#include <algorithm>
#include <cstring>
#include <vector>

namespace
{
    constexpr int kPitchBendCentre = 8192;
    constexpr int kPitchBendMax = 16383;
    constexpr int kPitchBendRangeSemitones = 1;
    constexpr int kMinimumPartialVelocity = 1;

    struct QuantisedMidiPitch
    {
        bool valid = false;
        int midiNote = -1;
        int pitchWheelValue = kPitchBendCentre;
    };

    struct QuarterToneMidiClass
    {
        int semitoneWithinOctave = 0;
        float bendSemitones = 0.0f;
    };

    constexpr QuarterToneMidiClass kQuarterToneMidiClasses[24] =
    {
        { 0,  0.0f }, { 0,  0.5f }, { 1,  0.0f }, { 2, -0.5f },
        { 2,  0.0f }, { 2,  0.5f }, { 3,  0.0f }, { 4, -0.5f },
        { 4,  0.0f }, { 4,  0.5f }, { 5,  0.0f }, { 5,  0.5f },
        { 6,  0.0f }, { 7, -0.5f }, { 7,  0.0f }, { 7,  0.5f },
        { 8,  0.0f }, { 9, -0.5f }, { 9,  0.0f }, { 9,  0.5f },
        { 10, 0.0f }, { 11, -0.5f }, { 11, 0.0f }, { 11, 0.5f }
    };

    int pitchWheelValueForSemitoneOffset (float semitoneOffset)
    {
        const auto value = (int) std::round ((double) kPitchBendCentre
                                             + (double) semitoneOffset
                                               * (double) kPitchBendCentre
                                               / (double) kPitchBendRangeSemitones);
        return juce::jlimit (0, kPitchBendMax, value);
    }

    QuantisedMidiPitch quantiseFrequencyToMidiPitch (float freqHz, bool useQuarterToneMode)
    {
        if (freqHz <= 0.0f)
            return {};

        if (useQuarterToneMode)
        {
            const float quarterToneIndexFloat = 138.0f + 24.0f * std::log2 (freqHz / 440.0f);
            const int quarterToneIndex = (int) std::round (quarterToneIndexFloat);

            if (quarterToneIndex < 0 || quarterToneIndex > 255)
                return {};

            const int pitchClass = quarterToneIndex % 24;
            const int octave     = quarterToneIndex / 24 - 1;
            const auto pitch     = kQuarterToneMidiClasses[pitchClass];
            const int midiNote   = 12 * (octave + 1) + pitch.semitoneWithinOctave;

            if (! juce::isPositiveAndBelow (midiNote, 128))
                return {};

            return { true, midiNote, pitchWheelValueForSemitoneOffset (pitch.bendSemitones) };
        }

        const float midiFloat = 69.0f + 12.0f * std::log2 (freqHz / 440.0f);
        const int midiNote = (int) std::round (midiFloat);

        if (! juce::isPositiveAndBelow (midiNote, 128))
            return {};

        return { true, midiNote, kPitchBendCentre };
    }

    juce::uint8 midiVelocityForPartialIntensity (float intensity)
    {
        const float linearIntensity = juce::jlimit (0.0f, 1.0f, intensity);
        return (juce::uint8) std::round (juce::jmap (linearIntensity,
                                                     (float) kMinimumPartialVelocity,
                                                     127.0f));
    }

    void appendPitchBendRangeSetup (juce::MidiBuffer& midiMessages, int channel, int sampleOffset)
    {
        midiMessages.addEvent (juce::MidiMessage::controllerEvent (channel, 101, 0), sampleOffset);
        midiMessages.addEvent (juce::MidiMessage::controllerEvent (channel, 100, 0), sampleOffset);
        midiMessages.addEvent (juce::MidiMessage::controllerEvent (channel, 6, kPitchBendRangeSemitones), sampleOffset);
        midiMessages.addEvent (juce::MidiMessage::controllerEvent (channel, 38, 0), sampleOffset);
        midiMessages.addEvent (juce::MidiMessage::controllerEvent (channel, 101, 127), sampleOffset);
        midiMessages.addEvent (juce::MidiMessage::controllerEvent (channel, 100, 127), sampleOffset);
    }

}

//==============================================================================
AudioPluginAudioProcessor::AudioPluginAudioProcessor()
     : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      #if ! JucePlugin_IsSynth
                       .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                      #endif
                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                     #endif
                       )
{
}

AudioPluginAudioProcessor::~AudioPluginAudioProcessor()
{
}

//==============================================================================
const juce::String AudioPluginAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool AudioPluginAudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool AudioPluginAudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool AudioPluginAudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double AudioPluginAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int AudioPluginAudioProcessor::getNumPrograms()
{
    return 1;   // NB: some hosts don't cope very well if you tell them there are 0 programs,
                // so this should be at least 1, even if you're not really implementing programs.
}

int AudioPluginAudioProcessor::getCurrentProgram()
{
    return 0;
}

void AudioPluginAudioProcessor::setCurrentProgram (int index)
{
    juce::ignoreUnused (index);
}

const juce::String AudioPluginAudioProcessor::getProgramName (int index)
{
    juce::ignoreUnused (index);
    return {};
}

void AudioPluginAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
    juce::ignoreUnused (index, newName);
}


// 在 AudioPluginAudioProcessor.cpp 里实现：
void AudioPluginAudioProcessor::getSpectrumCopy (std::array<float, kFftSize / 2>& dest) const
{
    const juce::SpinLock::ScopedLockType lock (fftLock);
    dest = smoothedMagnitudesDb;
}

void AudioPluginAudioProcessor::getResidualCopy (std::array<float, kFftSize / 2>& dest) const
{
    const juce::SpinLock::ScopedLockType lock (fftLock);
    dest = residualDb;
}

void AudioPluginAudioProcessor::getTopPeaksCopy (std::array<float, kNumNoisyPeaks>& freqsHz,
                                                 std::array<float, kNumNoisyPeaks>& residualDbOut,
                                                 std::array<float, kNumNoisyPeaks>& peakLevelsDbFsOut) const
{
    const juce::SpinLock::ScopedLockType lock (peakLock);   // ⭐ 用跟 runFftAndFindPeaks 一样的锁

    for (int i = 0; i < kNumNoisyPeaks; ++i)
    {
        freqsHz[(size_t) i]       = topFrequenciesHz[(size_t) i];   // runFftAndFindPeaks 填的 noisy peaks 频率
        residualDbOut[(size_t) i] = topResidualsDb[(size_t) i];     // Raw dB above the envelope.
        peakLevelsDbFsOut[(size_t) i] = topPeakLevelsDbFs[(size_t) i];
    }
}

void AudioPluginAudioProcessor::setLiveFrozenMidiChord (
    const std::array<float, kNumNoisyPeaks>& freqsHz,
    const std::array<float, kNumNoisyPeaks>& partialIntensities,
    bool useQuarterToneMode)
{
    std::vector<LiveMidiNote> nextNotes;
    nextNotes.reserve (kNumNoisyPeaks);

    int nextQuarterToneChannel = 1;

    for (size_t peakIndex = 0; peakIndex < freqsHz.size(); ++peakIndex)
    {
        const float freqHz = freqsHz[peakIndex];
        const auto pitch = quantiseFrequencyToMidiPitch (freqHz, useQuarterToneMode);
        if (! pitch.valid)
            continue;

        const auto duplicate = std::find_if (nextNotes.begin(), nextNotes.end(),
                                             [&pitch] (const auto& note)
                                             {
                                                 return note.midiNote == pitch.midiNote
                                                     && note.pitchWheelValue == pitch.pitchWheelValue;
                                             });
        const auto velocity = midiVelocityForPartialIntensity (partialIntensities[peakIndex]);

        if (duplicate != nextNotes.end())
        {
            duplicate->velocity = juce::jmax (duplicate->velocity, velocity);
            continue;
        }

        LiveMidiNote liveNote;
        liveNote.channel = useQuarterToneMode ? nextQuarterToneChannel++ : 1;
        liveNote.midiNote = pitch.midiNote;
        liveNote.pitchWheelValue = pitch.pitchWheelValue;
        liveNote.velocity = velocity;
        nextNotes.push_back (liveNote);

        if (useQuarterToneMode && nextQuarterToneChannel > 16)
            break;
    }

    const juce::SpinLock::ScopedLockType lock (liveMidiLock);
    pendingLiveMidiNotes = std::move (nextNotes);
    pendingLiveMidiUpdate = true;
}

void AudioPluginAudioProcessor::clearLiveFrozenMidiChord()
{
    const juce::SpinLock::ScopedLockType lock (liveMidiLock);
    pendingLiveMidiNotes.clear();
    pendingLiveMidiUpdate = true;
}

void AudioPluginAudioProcessor::setBassBoostMode (bool shouldUseBassBoost)
{
    bassBoostMode.store (shouldUseBassBoost);
}

bool AudioPluginAudioProcessor::getBassBoostMode() const
{
    return bassBoostMode.load();
}

float AudioPluginAudioProcessor::getInputLevelDb() const
{
    return inputLevelDb.load();
}

bool AudioPluginAudioProcessor::getStereoPanAvailable() const
{
    return stereoPanAvailable.load();
}

float AudioPluginAudioProcessor::getStereoPanEnergy() const
{
    return stereoPanEnergy.load();
}

bool AudioPluginAudioProcessor::hasHostTransportState() const
{
    return hostTransportStateAvailable.load (std::memory_order_relaxed);
}

bool AudioPluginAudioProcessor::getHostTransportPlaying() const
{
    return hostTransportPlaying.load (std::memory_order_relaxed);
}

void AudioPluginAudioProcessor::applyPendingMidiOutputChanges (juce::MidiBuffer& midiMessages)
{
    std::vector<LiveMidiNote> nextNotes;

    {
        const juce::SpinLock::ScopedLockType lock (liveMidiLock);

        if (! pendingLiveMidiUpdate)
            return;

        nextNotes = pendingLiveMidiNotes;
        pendingLiveMidiUpdate = false;
    }

    constexpr int sampleOffset = 0;

    for (const auto& note : activeLiveMidiNotes)
    {
        midiMessages.addEvent (juce::MidiMessage::noteOff (note.channel, note.midiNote), sampleOffset);
        midiMessages.addEvent (juce::MidiMessage::pitchWheel (note.channel, kPitchBendCentre), sampleOffset);
    }

    activeLiveMidiNotes = nextNotes;

    for (const auto& note : activeLiveMidiNotes)
    {
        appendPitchBendRangeSetup (midiMessages, note.channel, sampleOffset);
        midiMessages.addEvent (juce::MidiMessage::pitchWheel (note.channel, note.pitchWheelValue), sampleOffset);
        midiMessages.addEvent (juce::MidiMessage::noteOn (note.channel,
                                                          note.midiNote,
                                                          note.velocity),
                               sampleOffset);
    }
}


// 这个名字可以保留不变：Editor 直接用它显示 10 行文本


void AudioPluginAudioProcessor::updateEnvelopeAndNoisyPeaks()
{
    const int numBins = (int) smoothedMagnitudesDb.size();
    if (numBins <= 0)
        return;

    // ===============================
    // 1) 先做一个粗糙的 envelope（平均到 60 个 band）
    // ===============================
    std::array<float, kNumEnvBands> bandSum   {};
    std::array<float, kNumEnvBands> bandCount {};

    for (int bin = 0; bin < numBins; ++bin)
    {
        float db = smoothedMagnitudesDb[(size_t) bin];

        // bin -> band 映射 (线性分配)：
        int bandIdx = (bin * kNumEnvBands) / numBins;
        bandIdx = juce::jlimit (0, kNumEnvBands - 1, bandIdx);

        bandSum[bandIdx]   += db;
        bandCount[bandIdx] += 1.0f;
    }

    // 求每个 band 的平均值
    std::array<float, kNumEnvBands> bandEnv {};
    for (int b = 0; b < kNumEnvBands; ++b)
    {
        if (bandCount[b] > 0.0f)
            bandEnv[b] = bandSum[b] / bandCount[b];
        else
            bandEnv[b] = -100.0f;     // 很小的 dB
    }

    // Interpolate between neighbouring band centres instead of assigning one
    // constant value to every bin in a band. A stepped envelope can create an
    // artificial residual maximum exactly at a band boundary.
    for (int bin = 0; bin < numBins; ++bin)
    {
        const float bandPosition = ((float) bin + 0.5f) * (float) kNumEnvBands
                                     / (float) numBins - 0.5f;
        const int leftBand = (int) std::floor (bandPosition);

        if (leftBand < 0)
        {
            envelopeDb[(size_t) bin] = bandEnv[0];
        }
        else if (leftBand >= kNumEnvBands - 1)
        {
            envelopeDb[(size_t) bin] = bandEnv[(size_t) kNumEnvBands - 1];
        }
        else
        {
            const float fraction = bandPosition - (float) leftBand;
            envelopeDb[(size_t) bin] = bandEnv[(size_t) leftBand]
                                      + fraction * (bandEnv[(size_t) leftBand + 1]
                                                    - bandEnv[(size_t) leftBand]);
        }
    }

    // ===============================
    // 2) residual = smoothedMagnitudesDb - envelopeDb
    // ===============================
    for (int bin = 0; bin < numBins; ++bin)
    {
        residualDb[(size_t) bin] = smoothedMagnitudesDb[(size_t) bin] - envelopeDb[(size_t) bin];
    }

    // ===============================
    // 3) 在 residual 上找 noisy peaks
    // ===============================
    struct Peak
    {
        int   bin      = 0;
        float residual = -1.0e9f;
    };

    // 简单存很多 peak 然后排序
    std::vector<Peak> candidates;
    candidates.reserve (numBins);

    const float residualThreshold = 3.5f; // 至少比 envelope 高 6dB 才算 noisy

    for (int bin = 1; bin < numBins - 1; ++bin)
    {
        float r = residualDb[(size_t) bin];

        // residual 要够大，而且是局部最大
        if (r > residualThreshold
            && r > residualDb[(size_t) bin - 1]
            && r > residualDb[(size_t) bin + 1])
        {
            Peak p;
            p.bin      = bin;
            p.residual = r;
            candidates.push_back (p);
        }
    }

    // 排序：按 residual 从大到小
    std::sort (candidates.begin(), candidates.end(),
               [] (const Peak& a, const Peak& b) { return a.residual > b.residual; });

    // ===============================
    // 4) 选前 kNumNoisyPeaks 个，算频率 Hz，填到 noisyPeakFreqsHz
    // ===============================
    const double sr      = getSampleRate();
    const double binToHz = (sr * 0.5) / (double) (numBins - 1);  // 0..Nyquist 映到 0..numBins-1

    for (int i = 0; i < kNumNoisyPeaks; ++i)
    {
        if (i < (int) candidates.size())
        {
            auto bin = candidates[(size_t) i].bin;
            noisyPeakResidualDb[(size_t) i] = candidates[(size_t) i].residual;
            noisyPeakFreqsHz[(size_t) i]    = (float) (bin * binToHz);
        }
        else
        {
            // 没有那么多 peak，就填 0
            noisyPeakResidualDb[(size_t) i] = -100.0f;
            noisyPeakFreqsHz[(size_t) i]    = 0.0f;
        }
    }
}



//==============================================================================
void AudioPluginAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;

    fifoIndex = 0;
    fifo.fill (0.0f);
    fftBuffer.fill (0.0f);
    magnitudeSpectrum.fill (0.0f);
    topFrequenciesHz.fill (0.0f);
    topResidualsDb.fill (0.0f);
    topPeakLevelsDbFs.fill (-120.0f);
    smoothedMagnitudes.fill (0.0f);
    smoothedMagnitudesDb.fill (0.0f);
    pendingLiveMidiNotes.clear();
    activeLiveMidiNotes.clear();
    pendingLiveMidiUpdate = false;

    // Use this method as the place to do any pre-playback
    // initialisation that you need..
    juce::ignoreUnused (sampleRate, samplesPerBlock);
}

void AudioPluginAudioProcessor::releaseResources()
{
    // When playback stops, you can use this as an opportunity to free up any
    // spare memory, etc.
}

bool AudioPluginAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
  #if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
  #else
    // This is the place where you check if the layout is supported.
    // In this template code we only support mono or stereo.
    // Some plugin hosts, such as certain GarageBand versions, will only
    // load plugins that support stereo bus layouts.
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    // This checks if the input layout matches the output layout
   #if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
   #endif

    return true;
  #endif
}


void AudioPluginAudioProcessor::pushSample (float s)
{
    fifo[(size_t) fifoIndex++] = s;

    if (fifoIndex >= kFftSize)
    {
        // 把时域数据复制到 FFT buffer 前半部分
        std::copy (fifo.begin(), fifo.end(), fftBuffer.begin());
        // 后半部分清零
        std::fill (fftBuffer.begin() + kFftSize, fftBuffer.end(), 0.0f);

        runFftAndFindPeaks();

        // Keep 75% overlap so the live spectrum responds more smoothly.
        std::memmove (fifo.data(),
                      fifo.data() + kFftHopSize,
                      (size_t) (kFftSize - kFftHopSize) * sizeof (float));
        fifoIndex = kFftSize - kFftHopSize;
    }
}


void AudioPluginAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                              juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    applyPendingMidiOutputChanges (midiMessages);

    // Cache the DAW transport state on the audio thread. The editor uses this
    // snapshot to stop an active analysis as soon as the host transport stops,
    // without querying AudioPlayHead directly from the message thread.
    bool transportStateAvailable = false;
    bool transportIsPlaying = false;
    if (auto* playHead = getPlayHead())
    {
        if (const auto position = playHead->getPosition())
        {
            transportStateAvailable = true;
            transportIsPlaying = position->getIsPlaying();
        }
    }

    hostTransportPlaying.store (transportIsPlaying, std::memory_order_relaxed);
    hostTransportStateAvailable.store (transportStateAvailable, std::memory_order_relaxed);

    auto totalNumInputChannels  = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    // 清理没有输入的输出通道（避免多余的噪声）
    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    const int numSamples = buffer.getNumSamples();
    const int numInputChannelsForAnalysis = juce::jmin (totalNumInputChannels, buffer.getNumChannels());

    // =====================================================
    // 1. 始终使用 host audio input；插件本身做直通，分析时将输入下混为单声道
    // =====================================================
    if (numInputChannelsForAnalysis > 0)
    {
        double sumSquares = 0.0;
        double leftSquares = 0.0;
        double rightSquares = 0.0;
        int sampleCount = 0;
        const bool canMeasureStereoPan = numInputChannelsForAnalysis >= 2;

        for (int n = 0; n < numSamples; ++n)
        {
            float mixedSample = 0.0f;

            for (int channel = 0; channel < numInputChannelsForAnalysis; ++channel)
                mixedSample += buffer.getReadPointer (channel)[n];

            if (canMeasureStereoPan)
            {
                const auto leftSample = buffer.getReadPointer (0)[n];
                const auto rightSample = buffer.getReadPointer (1)[n];
                leftSquares += (double) leftSample * (double) leftSample;
                rightSquares += (double) rightSample * (double) rightSample;
            }

            mixedSample /= (float) numInputChannelsForAnalysis;
            sumSquares += (double) mixedSample * (double) mixedSample;
            ++sampleCount;
            pushSample (mixedSample);
        }

        const auto rms = sampleCount > 0 ? std::sqrt (sumSquares / (double) sampleCount) : 0.0;
        const auto level = rms > 1.0e-8 ? 20.0 * std::log10 (rms) : -120.0;
        inputLevelDb.store ((float) juce::jlimit (-120.0, 12.0, level));

        if (canMeasureStereoPan && sampleCount > 0)
        {
            const auto leftEnergy = leftSquares / (double) sampleCount;
            const auto rightEnergy = rightSquares / (double) sampleCount;
            const auto totalEnergy = leftEnergy + rightEnergy;

            stereoPanAvailable.store (true);
            // Normalised stereo energy balance: -1 = left, 0 = centre, +1 = right.
            stereoPanEnergy.store (totalEnergy > 1.0e-12
                                     ? (float) juce::jlimit (-1.0, 1.0, (rightEnergy - leftEnergy) / totalEnergy)
                                     : 0.0f);
        }
        else
        {
            stereoPanAvailable.store (false);
            stereoPanEnergy.store (0.0f);
        }
    }
    else
    {
        inputLevelDb.store (-120.0f);
        stereoPanAvailable.store (false);
        stereoPanEnergy.store (0.0f);
        buffer.clear();
    }
}


void AudioPluginAudioProcessor::runFftAndFindPeaks()
{
    std::array<float, kFftSize> analysisFrame {};
    std::copy_n (fftBuffer.begin(), kFftSize, analysisFrame.begin());

    // 1. 加窗 + FFT
    window.multiplyWithWindowingTable (analysisFrame.data(), kFftSize);
    std::copy (analysisFrame.begin(), analysisFrame.end(), fftBuffer.begin());
    fft.performFrequencyOnlyForwardTransform (fftBuffer.data());

    const int numBins = kFftSize / 2;
    std::array<float, kFftSize / 2> currentSpectrumDb {};
    std::array<float, kFftSize / 2> currentEnvelopeDb {};
    std::array<float, kFftSize / 2> currentResidualDb {};
    const float fftMagnitudeToDbfs = juce::Decibels::gainToDecibels (
        2.0f / (float) kFftSize,
        -120.0f);

    // 2. 时间平滑：smoothedMagnitudes = 0.9 * old + 0.1 * new
    for (int bin = 0; bin < numBins; ++bin)
    {
        float mag = fftBuffer[(size_t) bin]; // frequency-only FFT 已经是 magnitude

        smoothedMagnitudes[(size_t) bin] =
            smoothingCoeff * smoothedMagnitudes[(size_t) bin]
          + (1.0f - smoothingCoeff) * mag;
    }

    // 3. 转成 dB（存在 smoothedMagnitudesDb 里）
    for (int bin = 0; bin < numBins; ++bin)
    {
        float mag = smoothedMagnitudes[(size_t) bin];

        if (mag <= 1.0e-6f)
            currentSpectrumDb[(size_t) bin] = -120.0f;  // 底噪
        else
            currentSpectrumDb[(size_t) bin] = 20.0f * std::log10 (mag);
    }

    {
        const juce::SpinLock::ScopedLockType lock (fftLock);
        smoothedMagnitudesDb = currentSpectrumDb;
    }

    // ==========================================================
    // 4. 先从 smoothedMagnitudesDb 生成一个粗糙的 spectral envelope
    // ==========================================================
    static constexpr int kNumEnvBands = 60;

    std::array<float, kNumEnvBands> bandSum   {};
    std::array<float, kNumEnvBands> bandCount {};
    std::array<float, kNumEnvBands> bandEnv   {};

    for (int bin = 0; bin < numBins; ++bin)
    {
        float db = currentSpectrumDb[(size_t) bin];

        int bandIdx = (bin * kNumEnvBands) / numBins;
        bandIdx = std::clamp (bandIdx, 0, kNumEnvBands - 1);

        bandSum[(size_t) bandIdx]   += db;
        bandCount[(size_t) bandIdx] += 1.0f;
    }

    for (int b = 0; b < kNumEnvBands; ++b)
    {
        if (bandCount[(size_t) b] > 0.0f)
            bandEnv[(size_t) b] = bandSum[(size_t) b] / bandCount[(size_t) b];
        else
            bandEnv[(size_t) b] = -120.0f;
    }

    // Linearly interpolate the band averages at their centres. This keeps the
    // whitening envelope continuous and prevents a quiet neighbouring band
    // from creating a false residual peak at the shared boundary.
    for (int bin = 0; bin < numBins; ++bin)
    {
        const float bandPosition = ((float) bin + 0.5f) * (float) kNumEnvBands
                                     / (float) numBins - 0.5f;
        const int leftBand = (int) std::floor (bandPosition);

        if (leftBand < 0)
        {
            currentEnvelopeDb[(size_t) bin] = bandEnv[0];
        }
        else if (leftBand >= kNumEnvBands - 1)
        {
            currentEnvelopeDb[(size_t) bin] = bandEnv[(size_t) kNumEnvBands - 1];
        }
        else
        {
            const float fraction = bandPosition - (float) leftBand;
            currentEnvelopeDb[(size_t) bin] = bandEnv[(size_t) leftBand]
                                             + fraction * (bandEnv[(size_t) leftBand + 1]
                                                           - bandEnv[(size_t) leftBand]);
        }
    }

    // ==========================================================
    // 5. residual = smoothedMagnitudesDb - envelopeDb （单位：dB）
    // ==========================================================
    for (int bin = 0; bin < numBins; ++bin)
        currentResidualDb[(size_t) bin] = currentSpectrumDb[(size_t) bin]
                                        - currentEnvelopeDb[(size_t) bin];

    // Publish envelope and residual as thread-safe snapshots. The editor reads
    // residualDb on the message thread for automatic spectral-flux detection.
    {
        const juce::SpinLock::ScopedLockType lock (fftLock);
        envelopeDb = currentEnvelopeDb;
        residualDb = currentResidualDb;
    }

    // ==========================================================
    // 6. Hybrid ranking:
    //    - keep the original residual/local-max detector
    //    - add a low-frequency harmonic-support path so bass fundamentals
    //      can survive even when the upper partials are stronger
    // ==========================================================
    struct Peak
    {
        float score = -1.0e9f;
        float residualDb = -120.0f;
        int   binIndex = 0;
    };

    std::vector<Peak> candidates;
    candidates.reserve ((size_t) kNumNoisyPeaks * 6);

    const auto refinedFrequencyForBin = [&currentSpectrumDb, this] (int binIndex)
    {
        const int safeBin = juce::jlimit (1, numBins - 2, binIndex);
        const float left  = currentSpectrumDb[(size_t) safeBin - 1];
        const float mid   = currentSpectrumDb[(size_t) safeBin];
        const float right = currentSpectrumDb[(size_t) safeBin + 1];
        const float denominator = left - 2.0f * mid + right;

        float delta = 0.0f;
        if (std::abs (denominator) > 1.0e-6f)
            delta = 0.5f * (left - right) / denominator;

        delta = juce::jlimit (-0.5f, 0.5f, delta);
        return ((float) safeBin + delta) * (float) currentSampleRate / (float) kFftSize;
    };

    const bool useBassBoost = bassBoostMode.load();

    const auto weightedResidualScore = [useBassBoost] (float residualValueDb, float hz)
    {
        if (! useBassBoost)
            return residualValueDb;

        float weight = 1.0f;

        if (hz < 90.0f)
            weight = 2.2f;
        else if (hz < 180.0f)
            weight = 1.6f;
        else if (hz < 320.0f)
            weight = 1.2f;

        return residualValueDb * weight;
    };

    const float residualThreshold = useBassBoost ? 2.4f : 3.0f;
    const float minHz = useBassBoost ? 28.0f : 40.0f;
    const float maxHz = 4000.0f;

    // Treat the user-selected peak count as a maximum rather than a quota.
    // A local residual can be large even when the underlying magnitude is
    // only numerical noise or a Hann-window sidelobe. Requiring candidates to
    // remain within 30 dB of the strongest in-range component prevents those
    // very weak local maxima from being selected merely to fill every slot.
    constexpr float kPeakCandidateDynamicRangeDb = 30.0f;
    float strongestCandidateMagnitudeDb = -120.0f;

    for (int bin = 1; bin < numBins - 1; ++bin)
    {
        const float hz = (float) bin * (float) currentSampleRate / (float) kFftSize;
        if (hz >= minHz && hz <= maxHz)
            strongestCandidateMagnitudeDb = juce::jmax (strongestCandidateMagnitudeDb,
                                                        currentSpectrumDb[(size_t) bin]);
    }

    const float candidateMagnitudeFloorDb = juce::jmax (-120.0f,
                                                         strongestCandidateMagnitudeDb
                                                           - kPeakCandidateDynamicRangeDb);

    // The ordinary peak path remains deliberately selective, but a piano or
    // other bass-rich source can have a real fundamental far below a strong
    // upper partial. Harmonic support supplies the extra evidence needed to
    // admit a wider dynamic range without weakening the default detector.
    constexpr float kBassCandidateDynamicRangeDb = 60.0f;
    const float bassCandidateMagnitudeFloorDb = juce::jmax (-120.0f,
                                                             strongestCandidateMagnitudeDb
                                                               - kBassCandidateDynamicRangeDb);

    Peak bestSupportedBassFundamental;
    bool hasSupportedBassFundamental = false;

    for (int bin = 1; bin < numBins - 1; ++bin)
    {
        const float hz = (float) bin * (float) currentSampleRate / (float) kFftSize;
        if (hz < minHz || hz > maxHz)
            continue;

        if (currentSpectrumDb[(size_t) bin] <= candidateMagnitudeFloorDb)
            continue;

        const float residual = currentResidualDb[(size_t) bin];
        if (residual <= residualThreshold)
            continue;

        if (residual <= currentResidualDb[(size_t) bin - 1]
            || residual <= currentResidualDb[(size_t) bin + 1])
            continue;

        candidates.push_back ({ weightedResidualScore (residual, hz), residual, bin });
    }

    // Reuse the main residual spectrum for bass harmonic support. Zero-padding
    // the same 2048-sample frame creates denser bins but no true frequency
    // resolution, while requiring another FFT on the audio thread.
    if (useBassBoost)
    {
        static constexpr float kBassHarmonicSearchMinHz = 40.0f;
        static constexpr float kBassHarmonicSearchMaxHz = 170.0f;
        static constexpr float kMinFundamentalResidualDb = 1.4f;
        static constexpr float kMinHarmonicResidualDb = 1.2f;
        static constexpr float kMinBassSalience = 3.9f;
        static constexpr float kFundamentalSalienceWeight = 0.82f;
        static constexpr float kBassHarmonicBonusWeight = 0.42f;

        // Empirical ranking weights tuned for bass fundamentals and their
        // second and third harmonics.
        static constexpr float harmonicWeights[] = { 0.0f, 0.0f, 0.44f, 0.24f };

        const auto strongestResidualNearBin = [&currentResidualDb] (int centerBin)
        {
            const int safeBin = juce::jlimit (1, numBins - 2, centerBin);
            float strongestResidual = currentResidualDb[(size_t) safeBin];

            for (int offset = -1; offset <= 1; ++offset)
                strongestResidual = juce::jmax (strongestResidual,
                                                currentResidualDb[(size_t) (safeBin + offset)]);

            return strongestResidual;
        };

        const int lowBinMin = juce::jlimit (1,
                                             numBins - 2,
                                             (int) std::ceil (kBassHarmonicSearchMinHz * (float) kFftSize
                                                              / (float) currentSampleRate));
        const int lowBinMax = juce::jlimit (lowBinMin,
                                             numBins - 2,
                                             (int) std::floor (kBassHarmonicSearchMaxHz * (float) kFftSize
                                                               / (float) currentSampleRate));

        // Require a visible low-frequency local peak plus support from its
        // second or third harmonic to reject broadband-noise false positives.
        for (int bin = lowBinMin; bin <= lowBinMax; ++bin)
        {
            const float fundamentalHz = (float) bin * (float) currentSampleRate / (float) kFftSize;
            const float fundamentalResidual = currentResidualDb[(size_t) bin];

            if (currentSpectrumDb[(size_t) bin] <= bassCandidateMagnitudeFloorDb)
                continue;

            if (fundamentalResidual < kMinFundamentalResidualDb)
                continue;

            if (fundamentalResidual <= currentResidualDb[(size_t) bin - 1]
                || fundamentalResidual <= currentResidualDb[(size_t) bin + 1])
                continue;

            float salience = fundamentalResidual * kFundamentalSalienceWeight;
            int supportedHarmonics = 0;

            for (int harmonic = 2; harmonic <= 3; ++harmonic)
            {
                const int harmonicBin = bin * harmonic;
                if (harmonicBin >= numBins - 1)
                    break;

                const float harmonicResidual = strongestResidualNearBin (harmonicBin);

                if (harmonicResidual > kMinHarmonicResidualDb)
                {
                    salience += harmonicResidual * harmonicWeights[harmonic];
                    ++supportedHarmonics;
                }
            }

            if (supportedHarmonics < 1 || salience < kMinBassSalience)
                continue;

            const float originalScore = weightedResidualScore (fundamentalResidual, fundamentalHz);
            const float bassBonus = juce::jmax (0.0f, salience - fundamentalResidual)
                                  * kBassHarmonicBonusWeight;

            const Peak bassCandidate { originalScore + bassBonus,
                                       fundamentalResidual,
                                       bin };
            candidates.push_back (bassCandidate);

            if (! hasSupportedBassFundamental
                || bassCandidate.score > bestSupportedBassFundamental.score)
            {
                bestSupportedBassFundamental = bassCandidate;
                hasSupportedBassFundamental = true;
            }
        }
    }

    if (candidates.empty())
    {
        const juce::SpinLock::ScopedLockType lock (peakLock);
        for (int k = 0; k < kNumNoisyPeaks; ++k)
        {
            topFrequenciesHz[(size_t) k] = 0.0f;
            topResidualsDb[(size_t) k]   = 0.0f;
            topPeakLevelsDbFs[(size_t) k] = -120.0f;
        }
        return;
    }

    std::sort (candidates.begin(), candidates.end(),
               [] (const Peak& a, const Peak& b)
               {
                   return a.score > b.score;
               });

    std::array<float, kNumNoisyPeaks> selectedFreqs {};
    std::array<float, kNumNoisyPeaks> selectedResidualsDb {};
    std::array<float, kNumNoisyPeaks> selectedPeakLevelsDbFs {};
    selectedFreqs.fill (0.0f);
    selectedResidualsDb.fill (0.0f);
    selectedPeakLevelsDbFs.fill (-120.0f);

    int selectedCount = 0;

    // Bass Boost guarantees one output position for the strongest candidate
    // that passed the fundamental-plus-harmonic test. In particular, this
    // keeps a weak piano fundamental from being displaced by ten louder upper
    // partials. The normal sorted pass below fills all remaining positions and
    // automatically skips the duplicate through its spacing check.
    if (useBassBoost && hasSupportedBassFundamental)
    {
        selectedFreqs[0] = refinedFrequencyForBin (bestSupportedBassFundamental.binIndex);
        selectedResidualsDb[0] = bestSupportedBassFundamental.residualDb;
        selectedPeakLevelsDbFs[0] = currentSpectrumDb[(size_t) bestSupportedBassFundamental.binIndex]
                                  + fftMagnitudeToDbfs;
        selectedCount = 1;
    }

    for (const auto& candidate : candidates)
    {
        if (selectedCount >= kNumNoisyPeaks)
            break;

        const float hz = refinedFrequencyForBin (candidate.binIndex);
        bool tooClose = false;

        for (int i = 0; i < selectedCount; ++i)
        {
            const float minSpacingHz = juce::jmax (18.0f, selectedFreqs[(size_t) i] * 0.045f);
            if (std::abs (selectedFreqs[(size_t) i] - hz) < minSpacingHz)
            {
                tooClose = true;
                break;
            }
        }

        if (tooClose)
            continue;

        selectedFreqs[(size_t) selectedCount] = hz;
        // Ranking may use bass weights and harmonic bonuses. Publish both the
        // unmodified residual and the selected bin's absolute dBFS level so
        // musical intensity never depends on the ranking score.
        selectedResidualsDb[(size_t) selectedCount] = candidate.residualDb;
        selectedPeakLevelsDbFs[(size_t) selectedCount] = currentSpectrumDb[(size_t) candidate.binIndex]
                                                        + fftMagnitudeToDbfs;
        ++selectedCount;
    }

    {
        const juce::SpinLock::ScopedLockType lock (peakLock);

        for (int k = 0; k < kNumNoisyPeaks; ++k)
        {
            if (k < selectedCount)
            {
                topFrequenciesHz[(size_t) k] = selectedFreqs[(size_t) k];
                topResidualsDb[(size_t) k]   = selectedResidualsDb[(size_t) k];
                topPeakLevelsDbFs[(size_t) k] = selectedPeakLevelsDbFs[(size_t) k];
            }
            else
            {
                topFrequenciesHz[(size_t) k] = 0.0f;
                topResidualsDb[(size_t) k]   = 0.0f;
                topPeakLevelsDbFs[(size_t) k] = -120.0f;
            }
        }
    }

// ⭐⭐ Debug：在 processor 侧打印所有 kNumPeaks
{
    juce::String s ("Processor noisy peaks (Hz): ");
    for (int k = 0; k < kNumNoisyPeaks; ++k)
        s << juce::String (topFrequenciesHz[(size_t) k], 1) << "  ";

    DBG (s);
}
}





void AudioPluginAudioProcessor::logTopFrequencies() const
{
    juce::String s ("Top 10 freqs (Hz): ");
    for (int k = 0; k < kNumNoisyPeaks; ++k)
    {
        s << juce::String (topFrequenciesHz[k], 1) << " ";
    }
    DBG (s);
}


//==============================================================================
bool AudioPluginAudioProcessor::hasEditor() const
{
    return true; // (change this to false if you choose to not supply an editor)
}

juce::AudioProcessorEditor* AudioPluginAudioProcessor::createEditor()
{
    return new AudioPluginAudioProcessorEditor (*this);
}

//==============================================================================
void AudioPluginAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // You should use this method to store your parameters in the memory block.
    // You could do that either as raw data, or use the XML or ValueTree classes
    // as intermediaries to make it easy to save and load complex data.
    juce::ignoreUnused (destData);
}

void AudioPluginAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    // You should use this method to restore your parameters from this memory block,
    // whose contents will have been created by the getStateInformation() call.
    juce::ignoreUnused (data, sizeInBytes);
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AudioPluginAudioProcessor();
}
