#pragma once
#include <JuceHeader.h>
#include "BuiltinMultiband.h"

class SoundbeastProcessor : public BuiltinEffectBase
{
public:
    static constexpr int numBands = 3;

    SoundbeastProcessor();

    juce::String effectName() const override { return "Soundbeastifier"; }

    void prepareToPlay (double sampleRate, int blockSize) override;
    void reset() override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;

    juce::AudioParameterFloat*  amount = nullptr;
    juce::AudioParameterChoice* preset = nullptr;

    std::atomic<float> activity { 0.0f };

    struct Tuning
    {
        float xLow, xHigh;
        float thres[numBands];
        float ratio[numBands];
        float att[numBands];
        float rel[numBands];
        float drive[numBands];
        float outGain;
    };
    static const Tuning& tuning (int presetIndex);

private:
    double sr = 44100.0;
    ThreeBandSplitter split[2];
    BandCompressor    comp[numBands];

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SoundbeastProcessor)
};

class SoundbeastEditor : public juce::AudioProcessorEditor,
                         private juce::Timer
{
public:
    SoundbeastEditor (SoundbeastProcessor& p);
    ~SoundbeastEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    struct PresetRow;

    SoundbeastProcessor& proc;
    ValueKnob amountK;
    juce::Label title, amountL;
    std::unique_ptr<PresetRow> presetRow;
    float glow = 0.0f;

    juce::SliderParameterAttachment amountAtt { *proc.amount, amountK };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SoundbeastEditor)
};
