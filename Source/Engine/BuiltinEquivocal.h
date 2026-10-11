#pragma once

#include <JuceHeader.h>
#include "BuiltinEffects.h"

class EquivocalEditor;

class EquivocalProcessor final : public BuiltinEffectBase
{
public:
    static constexpr int programCount = 25;

    EquivocalProcessor();

    juce::String effectName() const override { return "Equivocal"; }
    void prepareToPlay (double sampleRate, int maximumBlockSize) override;
    void releaseResources() override;
    void reset() override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    double getTailLengthSeconds() const override { return 30.0; }
    juce::AudioProcessorEditor* createEditor() override;

    int getNumPrograms() override { return programCount; }
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}
    juce::String getProgramCategory (int index) const;
    static juce::StringArray getProgramCategories();

    juce::AudioParameterFloat* input = nullptr;
    juce::AudioParameterFloat* mix = nullptr;
    juce::AudioParameterFloat* output = nullptr;
    juce::AudioParameterFloat* verdict = nullptr;
    juce::AudioParameterFloat* doubt = nullptr;
    juce::AudioParameterFloat* uncertainty = nullptr;
    juce::AudioParameterFloat* nerve = nullptr;
    juce::AudioParameterChoice* listen = nullptr;
    juce::AudioParameterFloat* memory = nullptr;
    juce::AudioParameterChoice* clock = nullptr;
    juce::AudioParameterFloat* feedback = nullptr;
    juce::AudioParameterFloat* contradiction = nullptr;
    juce::AudioParameterFloat* orbit = nullptr;
    juce::AudioParameterFloat* tone = nullptr;
    juce::AudioParameterFloat* drive = nullptr;
    juce::AudioParameterFloat* focus = nullptr;
    juce::AudioParameterFloat* grain = nullptr;
    juce::AudioParameterFloat* reverse = nullptr;
    juce::AudioParameterFloat* pitch = nullptr;
    juce::AudioParameterFloat* scatter = nullptr;
    juce::AudioParameterFloat* diffuse = nullptr;
    juce::AudioParameterFloat* width = nullptr;
    juce::AudioParameterFloat* mercy = nullptr;
    juce::AudioParameterBool* freeze = nullptr;

    std::atomic<float> visualArbiter { 0.5f };
    std::atomic<float> visualFeature { 0.0f };
    std::atomic<float> visualWitnessRms { 0.0f };
    std::atomic<float> visualRumorRms { 0.0f };
    std::atomic<float> visualFeedbackRms { 0.0f };
    std::atomic<float> visualGrainPhase { 0.0f };
    std::atomic<float> visualDelayMs { 240.0f };
    std::atomic<float> visualMercyGain { 1.0f };

private:
    struct History
    {
        std::array<std::vector<float>, 2> channel;
        int writePosition = 0;

        void resize (int samples);
        void clear();
        float read (int channelIndex, float delaySamples) const noexcept;
        void write (float left, float right) noexcept;
    };

    struct Allpass
    {
        std::vector<float> buffer;
        int position = 0;

        void resize (int samples);
        void clear();
        float process (float inputSample, float coefficient) noexcept;
    };

    static juce::NormalisableRange<float> skewedRange (float minimum, float maximum,
                                                        float centre);
    static float shapedLoopSample (float sample, float driveAmount) noexcept;
    static float equalPowerHann (float phase) noexcept;
    static float clampUnit (float value) noexcept;

    float nextRandomBipolar() noexcept;
    float updateFeature (float left, float right, int mode) noexcept;
    float currentDelaySamples();
    float readRumor (int channelIndex, float delaySamples, float grainSamples,
                     float speed, float scatterAmount) noexcept;
    float diffuseSample (int timeline, int channelIndex, float sample,
                         float amount, bool energyPreserving) noexcept;
    void resetSmoothers();

    double sr = 44100.0;
    double currentTempo = 120.0;
    int historySize = 0;
    History histories[2];
    Allpass diffusers[2][2][3];
    float toneState[2][2] {};

    float grainPhase[2] { 0.0f, 0.5f };
    float grainScatter[2] {};
    float arbiterState = 0.5f;
    float randomState = 0.0f;
    juce::uint32 randomSeed = 0x6d2b79f5u;
    int randomCountdown = 0;

    float previousMid = 0.0f;
    float fastEnvelope = 0.0f;
    float slowEnvelope = 0.0f;
    float brightnessEnvelope = 0.0f;
    float midEnvelope = 0.0f;
    float sideEnvelope = 0.0f;
    float wanderState = 0.0f;
    float loopEnvelope = 0.0f;
    float fastAttackCoefficient = 0.0f;
    float fastReleaseCoefficient = 0.0f;
    float featureSlowCoefficient = 0.0f;
    float brightnessCoefficient = 0.0f;
    float widthCoefficient = 0.0f;
    float wanderCoefficient = 0.0f;

    juce::SmoothedValue<float> inputGainSm;
    juce::SmoothedValue<float> mixSm;
    juce::SmoothedValue<float> outputGainSm;
    juce::SmoothedValue<float> delaySm;
    juce::SmoothedValue<float> feedbackSm;
    juce::SmoothedValue<float> injectionSm;
    juce::SmoothedValue<float> contradictionSm;
    juce::SmoothedValue<float> orbitSm;
    juce::SmoothedValue<float> toneSm;
    juce::SmoothedValue<float> driveSm;
    juce::SmoothedValue<float> focusSm;
    juce::SmoothedValue<float> grainSm;
    juce::SmoothedValue<float> reverseSm;
    juce::SmoothedValue<float> pitchSm;
    juce::SmoothedValue<float> scatterSm;
    juce::SmoothedValue<float> diffuseSm;
    juce::SmoothedValue<float> widthSm;
    juce::SmoothedValue<float> mercySm;

    std::atomic<int> currentProgram { 1 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EquivocalProcessor)
};
