#pragma once
#include <JuceHeader.h>
#include "BuiltinEffects.h"

class VoidProcessor : public BuiltinEffectBase
{
public:
    static constexpr int numLines = 8;

    VoidProcessor();

    juce::String effectName() const override { return "Void"; }

    void prepareToPlay (double sampleRate, int blockSize) override;
    void reset() override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    double getTailLengthSeconds() const override { return 30.0; }

    juce::AudioProcessorEditor* createEditor() override;

    juce::AudioParameterFloat*  dry        = nullptr;
    juce::AudioParameterFloat*  wet        = nullptr;
    juce::AudioParameterFloat*  drive      = nullptr;
    juce::AudioParameterFloat*  expanse    = nullptr;
    juce::AudioParameterFloat*  decay      = nullptr;
    juce::AudioParameterFloat*  predelay   = nullptr;
    juce::AudioParameterFloat*  diffuse    = nullptr;
    juce::AudioParameterBool*   freeze     = nullptr;
    juce::AudioParameterFloat*  floorCut   = nullptr;
    juce::AudioParameterFloat*  airCut     = nullptr;
    juce::AudioParameterFloat*  warmth     = nullptr;
    juce::AudioParameterFloat*  shimmer    = nullptr;
    juce::AudioParameterChoice* shimmerInt = nullptr;
    juce::AudioParameterFloat*  driftRate  = nullptr;
    juce::AudioParameterFloat*  wobble     = nullptr;
    juce::AudioParameterFloat*  gravity    = nullptr;
    juce::AudioParameterFloat*  gravRel    = nullptr;
    juce::AudioParameterFloat*  width      = nullptr;
    juce::AudioParameterFloat*  bloom      = nullptr;

    std::atomic<float> tailLevel { 0.0f };
    std::atomic<float> duckGain  { 1.0f };

private:
    struct Allpass
    {
        std::vector<float> buf;
        int pos = 0;
        float process (float x, float g)
        {
            const float bufout = buf[(size_t) pos];
            const float y = -g * x + bufout;
            buf[(size_t) pos] = x + g * y;
            if (++pos >= (int) buf.size()) pos = 0;
            return y;
        }
    };

    struct GrainShifter
    {
        std::vector<float> buf;
        int write = 0;
        float phase = 0.0f;
        float window = 1.0f;

        float read (float delaySamples) const;
        float process (float in, float ratio);
    };

    struct Line
    {
        std::vector<float> buf;
        int   write   = 0;
        float baseLen = 1.0f;
        float curLen  = 1.0f;
        float damp    = 0.0f;
    };

    static float readDelay (const std::vector<float>& buf, int wp, float delaySamples);

    double sr = 44100.0;

    Line  lines[numLines];
    Allpass allpass[4];
    std::vector<float> predelayBuf;
    int   predelayWrite = 0;
    GrainShifter shifter;

    juce::dsp::StateVariableTPTFilter<float> floorHP;

    float lfoPhase = 0.0f;
    float gEnv     = 0.0f;
    float bloomG   = 0.0f;

    float gCur[numLines] = {};
    float injCur = 0.0f;
    bool  freezeInit = false;

    juce::SmoothedValue<float> drySm, wetSm, widthSm;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoidProcessor)
};

class VoidHorizon;
class VoidFreezeButton;
class VoidIntervalSelector;
class VoidPresetLabel;

class VoidEditor : public juce::AudioProcessorEditor,
                   private juce::Timer
{
public:
    VoidEditor (VoidProcessor& p);
    ~VoidEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void showPresetMenu();
    void applyPreset (int index);

    VoidProcessor& proc;

    juce::TextButton prevBtn { "<" }, nextBtn { ">" };
    std::unique_ptr<VoidPresetLabel> presetName;
    int currentPreset = -1;

    std::unique_ptr<VoidHorizon> horizon;
    std::unique_ptr<VoidFreezeButton> freezeBtn;
    std::unique_ptr<VoidIntervalSelector> intervalSel;
    std::unique_ptr<juce::ButtonParameterAttachment> freezeAtt;

    ValueKnob expanseK, decayK, predelayK, diffuseK,
              floorK, airK, warmthK,
              shimmerK,
              driftK, wobbleK,
              gravityK, releaseK,
              widthK, bloomK, driveK, dryK, wetK;

    juce::OwnedArray<juce::Label> labels;

    juce::SliderParameterAttachment expanseAtt  { *proc.expanse,  expanseK };
    juce::SliderParameterAttachment decayAtt     { *proc.decay,     decayK };
    juce::SliderParameterAttachment predelayAtt { *proc.predelay, predelayK };
    juce::SliderParameterAttachment diffuseAtt  { *proc.diffuse,  diffuseK };
    juce::SliderParameterAttachment floorAtt     { *proc.floorCut, floorK };
    juce::SliderParameterAttachment airAtt        { *proc.airCut,   airK };
    juce::SliderParameterAttachment warmthAtt    { *proc.warmth,   warmthK };
    juce::SliderParameterAttachment shimmerAtt   { *proc.shimmer,  shimmerK };
    juce::SliderParameterAttachment driftAtt      { *proc.driftRate, driftK };
    juce::SliderParameterAttachment wobbleAtt    { *proc.wobble,   wobbleK };
    juce::SliderParameterAttachment gravityAtt   { *proc.gravity,  gravityK };
    juce::SliderParameterAttachment releaseAtt   { *proc.gravRel,  releaseK };
    juce::SliderParameterAttachment widthAtt      { *proc.width,    widthK };
    juce::SliderParameterAttachment bloomAtt      { *proc.bloom,    bloomK };
    juce::SliderParameterAttachment driveAtt      { *proc.drive,    driveK };
    juce::SliderParameterAttachment dryAtt        { *proc.dry,      dryK };
    juce::SliderParameterAttachment wetAtt        { *proc.wet,      wetK };

    juce::Rectangle<int> secPreset, secSpace, secColor, secShimmer,
                         secMotion, secGravity, secOut, secCentre;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoidEditor)
};
