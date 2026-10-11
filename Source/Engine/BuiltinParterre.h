#pragma once
#include <JuceHeader.h>
#include "BuiltinEffects.h"

class ParterreProcessor : public BuiltinEffectBase
{
public:
    static constexpr int numBeds = 4;

    ParterreProcessor();

    juce::String effectName() const override { return "Parterre"; }

    void prepareToPlay (double sampleRate, int blockSize) override;
    void releaseResources() override {}
    void reset() override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;

    juce::AudioParameterFloat* focus  = nullptr;
    juce::AudioParameterFloat* spread = nullptr;
    juce::AudioParameterFloat* growth = nullptr;
    juce::AudioParameterFloat* bloom  = nullptr;
    juce::AudioParameterFloat* motion = nullptr;
    juce::AudioParameterFloat* rate   = nullptr;
    juce::AudioParameterFloat* react  = nullptr;
    juce::AudioParameterFloat* mirror = nullptr;
    juce::AudioParameterFloat* mix    = nullptr;
    juce::AudioParameterFloat* output = nullptr;

    std::atomic<float> visualFrequency[2][numBeds];
    std::atomic<float> visualBedRms[numBeds];
    std::atomic<float> visualEnvelope { 0.0f };
    std::atomic<float> visualPhase { 0.0f };
    std::atomic<float> visualOutputRms { 0.0f };

private:
    static constexpr int controlInterval = 16;

    void updateFilters (float focusHz, float spreadOctaves, float q,
                        float motionOctaves, float reactOctaves,
                        float mirrorAmount, float envelopeAmount);

    double sr = 44100.0;
    juce::dsp::StateVariableTPTFilter<float> filters[2][numBeds];

    juce::SmoothedValue<float> focusSm, spreadSm, growthSm, bloomSm, motionSm,
                                      rateSm, reactSm, mirrorSm, mixSm, outputSm;

    float lfoPhase = 0.0f;
    float envelope = 0.0f;
    float envelopeAttack = 0.0f;
    float envelopeRelease = 0.0f;
    float lastResonance = -1.0f;
    float activeResonance = 2.0f;
    float currentFrequency[2][numBeds] {};
    int samplesUntilControl = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParterreProcessor)
};

class ParterreEditor : public juce::AudioProcessorEditor,
                       private juce::Timer
{
public:
    explicit ParterreEditor (ParterreProcessor&);
    ~ParterreEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    ParterreProcessor& proc;

    ValueKnob focusK, spreadK, growthK, bloomK, motionK,
              rateK, reactK, mirrorK, mixK, outputK;
    juce::Label title, subtitle;
    juce::Label labels[10];

    juce::SliderParameterAttachment focusAtt  { *proc.focus,  focusK };
    juce::SliderParameterAttachment spreadAtt { *proc.spread, spreadK };
    juce::SliderParameterAttachment growthAtt { *proc.growth, growthK };
    juce::SliderParameterAttachment bloomAtt  { *proc.bloom,  bloomK };
    juce::SliderParameterAttachment motionAtt { *proc.motion, motionK };
    juce::SliderParameterAttachment rateAtt   { *proc.rate,   rateK };
    juce::SliderParameterAttachment reactAtt  { *proc.react,  reactK };
    juce::SliderParameterAttachment mirrorAtt { *proc.mirror, mirrorK };
    juce::SliderParameterAttachment mixAtt    { *proc.mix,    mixK };
    juce::SliderParameterAttachment outputAtt { *proc.output, outputK };

    juce::Rectangle<int> displayBounds;
    float displayedFrequency[2][ParterreProcessor::numBeds] {};
    float displayedBedRms[ParterreProcessor::numBeds] {};
    float displayedEnvelope = 0.0f;
    float displayedPhase = 0.0f;
    float displayedOutputRms = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParterreEditor)
};
