#pragma once
#include <JuceHeader.h>
#include "BuiltinEffects.h"

class DelayBankProcessor : public BuiltinEffectBase
{
public:
    static constexpr int numBanks = 8;

    DelayBankProcessor();

    juce::String effectName() const override { return "Delay Bank"; }

    void prepareToPlay (double sampleRate, int blockSize) override;
    void reset() override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    double getTailLengthSeconds() const override { return 10.0; }

    juce::AudioProcessorEditor* createEditor() override;

    struct Bank
    {
        juce::AudioParameterBool*   enable   = nullptr;
        juce::AudioParameterFloat*  inPan    = nullptr;
        juce::AudioParameterFloat*  inVol    = nullptr;
        juce::AudioParameterChoice* filtMode = nullptr;
        juce::AudioParameterBool*   filtPost = nullptr;
        juce::AudioParameterFloat*  filtGain = nullptr;
        juce::AudioParameterFloat*  filtCut  = nullptr;
        juce::AudioParameterFloat*  filtRes  = nullptr;
        juce::AudioParameterFloat*  time     = nullptr;
        juce::AudioParameterFloat*  ofs      = nullptr;
        juce::AudioParameterFloat*  sep      = nullptr;
        juce::AudioParameterChoice* fbMode   = nullptr;
        juce::AudioParameterFloat*  fbVol    = nullptr;
        juce::AudioParameterFloat*  fbPan    = nullptr;
        juce::AudioParameterChoice* fbfMode  = nullptr;
        juce::AudioParameterFloat*  fbfGain  = nullptr;
        juce::AudioParameterFloat*  fbfCut   = nullptr;
        juce::AudioParameterFloat*  fbfRes   = nullptr;
        juce::AudioParameterFloat*  grainDiv = nullptr;
        juce::AudioParameterFloat*  grainSH  = nullptr;
        juce::AudioParameterFloat*  outNext  = nullptr;
        juce::AudioParameterFloat*  outPan   = nullptr;
        juce::AudioParameterFloat*  outVol   = nullptr;
    };

    Bank bank[numBanks];

    juce::AudioParameterFloat* dry = nullptr;
    juce::AudioParameterFloat* wet = nullptr;
    juce::AudioParameterFloat* inLevel = nullptr;
    juce::AudioParameterFloat* fbScale = nullptr;

private:
    struct BankState
    {
        std::vector<float> buf[2];
        int   writePos = 0;
        float curDelay[2] = { 1.0f, 1.0f };
        float shHold[2]   = { 0.0f, 0.0f };
        float shPhase[2]  = { 1.0f, 1.0f };
        float dcX1[2]     = { 0.0f, 0.0f };
        float dcY1[2]     = { 0.0f, 0.0f };
    };

    float readDelay (const std::vector<float>& buf, int wp, float delaySamples) const;

    double sr = 44100.0;
    int    bufSize = 0;

    BankState st[numBanks];
    juce::dsp::StateVariableTPTFilter<float> inFilt[numBanks], fbFilt[numBanks];

    juce::SmoothedValue<float> drySm, wetSm, inSm, fbSm;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DelayBankProcessor)
};

class DBankLedToggle;
class DBankLedChoice;

class DelayBankEditor : public juce::AudioProcessorEditor
{
public:
    DelayBankEditor (DelayBankProcessor& p);
    ~DelayBankEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void selectBank (int b);
    void bindBank();

    DelayBankProcessor& proc;
    int selected = 0;

    juce::OwnedArray<DBankLedToggle> enableLeds;
    juce::OwnedArray<juce::TextButton> tabs;

    ValueKnob inPanK, inVolK, filtGainK, filtCutK, filtResK,
              fbVolK, fbPanK, fbfGainK, fbfCutK, fbfResK,
              outNextK, outPanK, outVolK;
    ValueSlider timeS, ofsS, sepS, grainDivS, grainShS;

    ValueKnob dryK, wetK, inK, fbK;
    juce::SliderParameterAttachment dryAtt { *proc.dry,     dryK };
    juce::SliderParameterAttachment wetAtt { *proc.wet,     wetK };
    juce::SliderParameterAttachment inAtt  { *proc.inLevel, inK };
    juce::SliderParameterAttachment fbAtt  { *proc.fbScale, fbK };

    juce::OwnedArray<juce::Label> labels;
    juce::OwnedArray<juce::SliderParameterAttachment> bankAtts;
    std::unique_ptr<DBankLedChoice> filtModeLed, fbModeLed, fbfModeLed;
    std::unique_ptr<DBankLedToggle> filtPostLed;

    juce::Rectangle<int> secTop, secIn, secFilter, secDelay,
                         secFbFilter, secGrain, secOut, secGlobal;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DelayBankEditor)
};
