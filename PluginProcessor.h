#pragma once
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

//==============================================================================
class AudioPluginAudioProcessor final : public juce::AudioProcessor
{
public:
    //==============================================================================
    AudioPluginAudioProcessor();
    ~AudioPluginAudioProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    //==============================================================================
    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    // 保护 topFrequenciesHz 读写的锁
juce::SpinLock peakLock;

// 给 Editor 读取 top 10 频率
    //==============================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // ==== FFT config ====
static constexpr int kFftOrder = 11;                  // 2^11 = 2048
static constexpr int kFftSize  = 1 << kFftOrder;
static constexpr int kFftHopSize = kFftSize / 4;
static constexpr int kNumNoisyPeaks = 10;
static constexpr int kNumEnvBands   = 60;   // envelope 的频带数量（可调整）    // 2048



juce::dsp::FFT fft { kFftOrder };
juce::dsp::WindowingFunction<float> window {
    kFftSize,
    juce::dsp::WindowingFunction<float>::hann
};

// 时域缓冲，攒满 kFftSize 个 sample 再做一次 FFT
std::array<float, kFftSize> fifo {};
int fifoIndex = 0;

// FFT 工作区（JUCE 要求 2 * N 大小）
std::array<float, 2 * kFftSize> fftBuffer {};

// 振幅谱（只用前一半频率）
std::array<float, kFftSize / 2> magnitudeSpectrum {};

// 用来做时间平滑的频谱（只存前一半有意义的 bins）
std::array<float, kFftSize / 2> smoothedMagnitudes {};

std::array<float, kFftSize / 2> smoothedMagnitudesDb {};
// 新增：envelope & residual
std::array<float, kFftSize / 2> envelopeDb {};      // 每个 bin 对应的包络值
std::array<float, kFftSize / 2> residualDb {};      // spectrumDb - envelopeDb

// 新增：noisy peaks（通过 residual 找出来的 peaks）
std::array<float, kNumNoisyPeaks> noisyPeakFreqsHz      {};   // 频率（Hz）
std::array<float, kNumNoisyPeaks> noisyPeakResidualDb   {};   // 相对 envelope 高出的 dB 值

// 平滑系数（0~1，越接近 1 越“迟钝”、越慢）
float smoothingCoeff = 0.9f;


// Top 10 result
std::array<float, kNumNoisyPeaks> topFrequenciesHz {};
std::array<float, kNumNoisyPeaks> topResidualsDb {};
mutable juce::SpinLock fftLock;

    // ⭐⭐⭐ 现在新增的平滑相关成员 ⭐⭐⭐



double currentSampleRate = 44100.0;

// 在 AudioPluginAudioProcessor.h 里 public 区加：
void getSpectrumCopy (std::array<float, kFftSize / 2>& dest) const;
// Copies the full residual spectrum for the editor's spectral-flux onset detector.
void getResidualCopy (std::array<float, kFftSize / 2>& dest) const;
void getTopPeaksCopy (std::array<float, kNumNoisyPeaks>& freqs, std::array<float, kNumNoisyPeaks>& mags) const;
void setLiveFrozenMidiChord (const std::array<float, kNumNoisyPeaks>& freqsHz, bool useQuarterToneMode);
void clearLiveFrozenMidiChord();
void setBassBoostMode (bool shouldUseBassBoost);
bool getBassBoostMode() const;
float getInputLevelDb() const;
bool getStereoPanAvailable() const;
float getStereoPanEnergy() const;

// 小工具函数
void pushSample (float s);
void runFftAndFindPeaks();
void logTopFrequencies() const;
void updateEnvelopeAndNoisyPeaks();

    struct LiveMidiNote
    {
        int channel = 1;
        int midiNote = -1;
        int pitchWheelValue = 8192;
    };

private:
    void applyPendingMidiOutputChanges (juce::MidiBuffer& midiMessages);

    juce::SpinLock liveMidiLock;
    std::vector<LiveMidiNote> pendingLiveMidiNotes;
    std::vector<LiveMidiNote> activeLiveMidiNotes;
    bool pendingLiveMidiUpdate = false;
    std::atomic<bool> bassBoostMode { false };
    std::atomic<float> inputLevelDb { -120.0f };
    std::atomic<bool> stereoPanAvailable { false };
    std::atomic<float> stereoPanEnergy { 0.0f };

    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioPluginAudioProcessor)
};
