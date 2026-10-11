#pragma once
#include <JuceHeader.h>
#include "BuiltinEffects.h"

class LowLifterProcessor : public BuiltinEffectBase
{
public:
    static constexpr int numDiff = 4;

    LowLifterProcessor();

    juce::String effectName() const override { return "Low Lifter"; }

    void prepareToPlay (double sampleRate, int blockSize) override;
    void reset() override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;

    juce::AudioParameterFloat* preGain        = nullptr;
    juce::AudioParameterFloat* slope          = nullptr;
    juce::AudioParameterBool*  invertPolarity = nullptr;
    juce::AudioParameterBool*  multiBand       = nullptr;
    juce::AudioParameterBool*  lookahead       = nullptr;
    juce::AudioParameterFloat* diffDepth       = nullptr;
    juce::AudioParameterFloat* diffMix         = nullptr;
    juce::AudioParameterFloat* lowGain         = nullptr;
    juce::AudioParameterFloat* cutoff          = nullptr;
    juce::AudioParameterFloat* highGain        = nullptr;
    juce::AudioParameterFloat* lowCut          = nullptr;
    juce::AudioParameterFloat* dryWet          = nullptr;

    std::atomic<float> lowRms     { 0.0f };
    std::atomic<float> highRms    { 0.0f };
    std::atomic<float> harmAmount { 0.0f };

private:
    struct Allpass
    {
        std::vector<float> buf;
        int pos = 0;
        void prepare (int len) { buf.assign ((size_t) juce::jmax (1, len), 0.0f); pos = 0; }
        void reset() { std::fill (buf.begin(), buf.end(), 0.0f); pos = 0; }
        float process (float x, float g)
        {
            const float bufout = buf[(size_t) pos];
            const float y = -g * x + bufout;
            buf[(size_t) pos] = x + g * y;
            if (++pos >= (int) buf.size()) pos = 0;
            return y;
        }
    };

    struct Delay
    {
        std::vector<float> buf;
        int write = 0;
        void prepare (int maxLen) { buf.assign ((size_t) juce::jmax (2, maxLen + 4), 0.0f); write = 0; }
        void reset() { std::fill (buf.begin(), buf.end(), 0.0f); write = 0; }
        float process (float x, int d)
        {
            buf[(size_t) write] = x;
            int r = write - d;
            if (r < 0) r += (int) buf.size();
            const float out = buf[(size_t) r];
            if (++write >= (int) buf.size()) write = 0;
            return out;
        }
    };

    double sr = 44100.0;
    int    lookaheadSamples = 0;
    int    reportedLatency  = 0;

    juce::dsp::LinkwitzRileyFilter<float> crossover;
    juce::dsp::LinkwitzRileyFilter<float> subSplit;
    juce::dsp::StateVariableTPTFilter<float> outputHP;

    Allpass diffAp[2][numDiff];
    Delay   lowDelay[2], highDelay[2], harmDelay[2], dryDelay[2];
    float   laEnv[2] { 0.0f, 0.0f };

    juce::SmoothedValue<float> preSm, lowGainSm, highGainSm, dryWetSm;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LowLifterProcessor)
};

class LowLifterGraph;
class LowLifterPresetLabel;
class LowLifterLed;

class LowLifterEditor : public juce::AudioProcessorEditor,
                        private juce::Timer
{
public:
    LowLifterEditor (LowLifterProcessor& p);
    ~LowLifterEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void showPresetMenu();
    void applyPreset (int index);

    LowLifterProcessor& proc;

    juce::TextButton prevBtn { "<" }, nextBtn { ">" };
    std::unique_ptr<LowLifterPresetLabel> presetName;
    int currentPreset = -1;

    std::unique_ptr<LowLifterGraph> graph;

    std::unique_ptr<LowLifterLed> multiBandLed, lookaheadLed, invertLed;
    std::unique_ptr<juce::ButtonParameterAttachment> multiBandAtt, lookaheadAtt, invertAtt;

    ValueKnob preGainK, slopeK, diffDepthK, diffMixK,
              lowGainK, cutoffK, highGainK, lowCutK, dryWetK;

    juce::OwnedArray<juce::Label> labels;

    juce::SliderParameterAttachment preGainAtt   { *proc.preGain,  preGainK };
    juce::SliderParameterAttachment slopeAtt      { *proc.slope,    slopeK };
    juce::SliderParameterAttachment diffDepthAtt { *proc.diffDepth, diffDepthK };
    juce::SliderParameterAttachment diffMixAtt   { *proc.diffMix,   diffMixK };
    juce::SliderParameterAttachment lowGainAtt   { *proc.lowGain,   lowGainK };
    juce::SliderParameterAttachment cutoffAtt     { *proc.cutoff,    cutoffK };
    juce::SliderParameterAttachment highGainAtt  { *proc.highGain,  highGainK };
    juce::SliderParameterAttachment lowCutAtt     { *proc.lowCut,    lowCutK };
    juce::SliderParameterAttachment dryWetAtt     { *proc.dryWet,    dryWetK };

    juce::Rectangle<int> secPreset, secGraph, secHarm, secDiff, secXover, secMix;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LowLifterEditor)
};
