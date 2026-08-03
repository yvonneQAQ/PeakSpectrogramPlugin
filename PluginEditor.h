#pragma once

#include "PluginProcessor.h"
#include <array>
#include <functional>
#include <memory>
#include <vector>


class AudioPluginAudioProcessor;
class FrozenChordStaffComponent;
class DescriptorMidiDragComponent;
class MidiTimeScaleSelector;
class AutoDetectionXYPad;

struct FrozenChordSnapshot
{
    std::array<float, AudioPluginAudioProcessor::kNumNoisyPeaks> freqsHz {};
    juce::String label;
    bool useQuarterToneMode = false;
    double startTimeSeconds = 0.0;
    double endTimeSeconds = 0.25;
    double visualCreatedWallTimeSeconds = 0.0;
};

struct DetailAnalysisFrame
{
    double timeSeconds = 0.0;
    float spectralFlatness = 0.0f;
    float roughness = 0.0f;
    float spectralFlux = 0.0f;
    float centroidHz = 0.0f;
    float rmsDb = -120.0f;
    float stereoPanEnergy = 0.0f;
    bool stereoPanAvailable = false;
};
//==============================================================================
class AudioPluginAudioProcessorEditor  : public juce::AudioProcessorEditor,
                                         public juce::Timer       // ⭐ 一定要继承 Timer
{
public:
    AudioPluginAudioProcessorEditor (AudioPluginAudioProcessor&);
    ~AudioPluginAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    static constexpr int kNumNoisyPeaks = AudioPluginAudioProcessor::kNumNoisyPeaks;

private:
    bool useQuarterToneMode() const;
    int getActivePeakCount() const;
    juce::String getCurrentHostTimeLabel() const;
    juce::String formatTimeLabel (double seconds) const;
    void setAutoPageActive (bool shouldUseAutoPage);
    void refreshModeButtonStates();
    void refreshAutoButtonStates();
    void resetAutoDetectorState();
    void finishAutoAnalysis (double nowSeconds,
                             const juce::String& statusMessage,
                             bool clearLiveChord);
    void recordDetailAnalysisFrame (double nowSeconds);
    DetailAnalysisFrame calculateDetailAnalysisFrame (double nowSeconds);
    void refreshDetailAnalysisButtonState();
    void drawDetailAnalysisPanel (juce::Graphics& g, juce::Rectangle<int> area);
    void drawAnalysisCurve (juce::Graphics& g,
                            juce::Rectangle<int> area,
                            const juce::String& title,
                            const juce::String& valueText,
                            float minValue,
                            float maxValue,
                            bool available,
                            const std::function<float (const DetailAnalysisFrame&)>& valueForFrame);
    void processAutoOnsetDetection (double nowSeconds);
    void captureAutoChord (double nowSeconds);
    void refreshBassBoostButtonText();
    void captureFrozenChord();
    void refreshStaffComponent();
    void refreshExportButtonState();
    void showTransientStatusMessage (juce::String message, double seconds = 2.5);
    void resetFrozenState();
    void closeActiveFrozenChord();
    void clearSpectrogramImage();

    void timerCallback() override;      // ⭐ Timer 回调
    void pushSpectrumToImage();


    AudioPluginAudioProcessor& processorRef;
    // ==== FFT 绘图相关 ====
    static constexpr int kFftOrder = 11;
    static constexpr int kFftSize  = 1 << kFftOrder;




// 当前这一帧的频谱（dB）
    std::array<float, kFftSize / 2> spectrumForDrawing { };
    std::array<float, kNumNoisyPeaks> topFreqsForDrawing { };
    std::array<float, kNumNoisyPeaks> topMagsForDrawing  { };

    // 🔹 后台最新分析结果（每帧更新，但不一定马上显示）
    std::array<float, kFftSize / 2>   latestSpectrum   { };
    std::array<float, kFftSize / 2>   latestResidual   { };
    std::array<float, kFftSize / 2>   previousAutoResidual { };
    std::array<float, kFftSize / 2>   previousDetailSpectrum { };
    std::array<float, kNumNoisyPeaks> latestTopFreqs   { };
    std::array<float, kNumNoisyPeaks> latestTopMags    { };


    juce::Image spectrogramImage { juce::Image::RGB, 400, 300, true };

    bool isFrozen = false;
    juce::TextButton freezeButton { "Freeze" };
    juce::TextButton unfreezeButton { "Unfreeze" };
    juce::TextButton resetButton { "Reset" };
    juce::TextButton bassBoostButton { "Bass Boost" };
    juce::TextButton quarterToneButton { "Quarter-Tone" };
    juce::TextButton exportMidiButton { "Export MIDI" };
    juce::TextButton manualModeButton { "Manual" };
    juce::TextButton autoModeButton { "Auto" };
    juce::TextButton autoStartButton { "Start" };
    juce::TextButton autoStopButton { "Stop" };
    juce::TextButton autoClearButton { "Clear" };
    juce::TextButton detailAnalysisButton { "Detail Analysis" };
    juce::Slider topPeakCountSlider;
    std::unique_ptr<AutoDetectionXYPad> autoDetectionPad;
    std::unique_ptr<juce::FileChooser> exportFileChooser;
    std::unique_ptr<juce::LookAndFeel_V4> lookAndFeel;

    std::vector<FrozenChordSnapshot> frozenChords;
    std::unique_ptr<FrozenChordStaffComponent> staffComponent;
    std::unique_ptr<DescriptorMidiDragComponent> descriptorMidiDragComponent;
    std::unique_ptr<MidiTimeScaleSelector> midiTimeScaleSelector;
    int freezeCaptureCount = 0;
    double activeFreezeSessionStartWallTimeSeconds = 0.0;
    double activeFreezeSessionOffsetSeconds = 0.0;
    juce::String firstFreezeTimeLabel;
    bool isAutoPageActive = false;
    bool isDetailPageActive = false;
    bool isAutoRunning = false;
    bool hasPreviousAutoResidual = false;
    bool hasPreviousAutoInputLevel = false;
    bool hasPreviousDetailSpectrum = false;
    bool hasCompletedAutoAnalysis = false;
    int autoCaptureCount = 0;
    double autoStartWallTimeSeconds = 0.0;
    double lastAutoOnsetWallTimeSeconds = -10.0;
    float autoFluxMean = 0.0f;
    float autoFluxDeviation = 1.0f;
    float autoOnsetStrength = 0.0f;
    float previousAutoInputLevelDb = -120.0f;
    std::vector<DetailAnalysisFrame> detailAnalysisHistory;
    bool pendingFreezeMarker = false;
    juce::String transientStatusMessage;
    double transientStatusMessageExpirySeconds = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioPluginAudioProcessorEditor)
};
