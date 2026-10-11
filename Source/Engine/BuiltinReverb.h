#pragma once
#include <JuceHeader.h>
#include "BuiltinEffects.h"

class ReverbProcessor : public BuiltinEffectBase
{
public:
    ReverbProcessor();

    juce::String effectName() const override { return "Reverb"; }

    void prepareToPlay (double sampleRate, int blockSize) override;
    void reset() override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    double getTailLengthSeconds() const override;

    juce::AudioProcessorEditor* createEditor() override;

    juce::AudioParameterChoice* midSide    = nullptr;
    juce::AudioParameterFloat*  lowCut     = nullptr;
    juce::AudioParameterFloat*  highCut    = nullptr;
    juce::AudioParameterFloat*  predelay   = nullptr;
    juce::AudioParameterFloat*  size       = nullptr;
    juce::AudioParameterFloat*  diffusion  = nullptr;
    juce::AudioParameterFloat*  mod        = nullptr;
    juce::AudioParameterFloat*  speed      = nullptr;
    juce::AudioParameterFloat*  decay      = nullptr;
    juce::AudioParameterFloat*  bass       = nullptr;
    juce::AudioParameterFloat*  cross      = nullptr;
    juce::AudioParameterFloat*  damp       = nullptr;
    juce::AudioParameterFloat*  dry        = nullptr;
    juce::AudioParameterFloat*  er         = nullptr;
    juce::AudioParameterFloat*  wet        = nullptr;
    juce::AudioParameterFloat*  separation = nullptr;

private:
    static constexpr int numLines     = 8;
    static constexpr int numDiffusers = 4;
    static constexpr int numErTaps    = 8;

    void updateDerived();

    double sr = 44100.0;

    juce::dsp::StateVariableTPTFilter<float> hpf, lpf;

    std::vector<float> predelayBuf[2];
    int   predelayWrite = 0;
    float predelaySamplesCur = 0.0f;

    std::vector<float> erBuf[2];
    int erWrite = 0;
    float erTapL[numErTaps] = {}, erTapR[numErTaps] = {};

    std::vector<float> diffBuf[numDiffusers];
    int   diffWrite[numDiffusers] = {};
    float diffCoeff = 0.65f;

    std::vector<float> lineBuf[numLines];
    int   lineWrite[numLines]  = {};
    float dampState[numLines]  = {};
    float crossState[numLines] = {};
    float lfoPhase[numLines]   = {};
    float baseDelay[numLines]  = {};
    float gLow[numLines]       = {};
    float gHigh[numLines]      = {};

    float sizeCur   = 0.6f;
    float dampCoeff = 0.1f;
    float crossCoeff = 0.1f;
    float lfoInc = 0.0f;
    float modDepthSamples = 0.0f;

    float lastDecay = -1, lastBass = -1, lastSize = -1;
    float lastLowCut = -1, lastHighCut = -1;

    juce::SmoothedValue<float> drySm, erSm, wetSm, sepSm;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReverbProcessor)
};

class ReverbEditor : public juce::AudioProcessorEditor,
                     private juce::Slider::Listener
{
public:
    ReverbEditor (ReverbProcessor& p);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void sliderValueChanged (juce::Slider*) override;

    struct RoomView : public juce::Component
    {
        ReverbProcessor& proc;
        RoomView (ReverbProcessor& p) : proc (p) {}
        void paint (juce::Graphics&) override;
    };

    ReverbProcessor& proc;
    RoomView room { proc };

    juce::Rectangle<int> inputArea, spaceArea, colorArea, outputArea;

    juce::ComboBox modeBox;

    ValueKnob lowCutKnob, highCutKnob, delayKnob, sizeKnob, diffKnob, modKnob,
              speedKnob, decKnob, bassKnob, crossKnob, dampKnob, sepKnob;
    ValueSlider dryFader, erFader, wetFader;

    juce::Label modeLabel, lowCutLabel, highCutLabel, delayLabel, sizeLabel,
                diffLabel, modLabel, speedLabel, decLabel, bassLabel, crossLabel,
                dampLabel, sepLabel, dryLabel, erLabel, wetLabel;

    juce::ComboBoxParameterAttachment modeAtt { *proc.midSide, modeBox };
    juce::SliderParameterAttachment lowCutAtt  { *proc.lowCut,     lowCutKnob };
    juce::SliderParameterAttachment highCutAtt { *proc.highCut,    highCutKnob };
    juce::SliderParameterAttachment delayAtt   { *proc.predelay,   delayKnob };
    juce::SliderParameterAttachment sizeAtt    { *proc.size,       sizeKnob };
    juce::SliderParameterAttachment diffAtt    { *proc.diffusion,  diffKnob };
    juce::SliderParameterAttachment modAtt     { *proc.mod,        modKnob };
    juce::SliderParameterAttachment speedAtt   { *proc.speed,      speedKnob };
    juce::SliderParameterAttachment decAtt     { *proc.decay,      decKnob };
    juce::SliderParameterAttachment bassAtt    { *proc.bass,       bassKnob };
    juce::SliderParameterAttachment crossAtt   { *proc.cross,      crossKnob };
    juce::SliderParameterAttachment dampAtt     { *proc.damp,       dampKnob };
    juce::SliderParameterAttachment dryAtt     { *proc.dry,        dryFader };
    juce::SliderParameterAttachment erAtt      { *proc.er,         erFader };
    juce::SliderParameterAttachment wetAtt      { *proc.wet,        wetFader };
    juce::SliderParameterAttachment sepAtt      { *proc.separation, sepKnob };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReverbEditor)
};
