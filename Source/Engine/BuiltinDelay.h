#pragma once
#include <JuceHeader.h>
#include "BuiltinEffects.h"

class DelayProcessor : public BuiltinEffectBase
{
public:
    DelayProcessor();

    juce::String effectName() const override { return "Delay"; }

    void prepareToPlay (double sampleRate, int blockSize) override;
    void reset() override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    double getTailLengthSeconds() const override { return 8.0; }

    juce::AudioProcessorEditor* createEditor() override;

    static float distShape (float x, int mode, float level, float knee, float sym);

    juce::AudioParameterFloat*  inWet     = nullptr;
    juce::AudioParameterFloat*  time      = nullptr;
    juce::AudioParameterBool*   tempoSync = nullptr;
    juce::AudioParameterBool*   keepPitch = nullptr;
    juce::AudioParameterFloat*  smoothing = nullptr;
    juce::AudioParameterFloat*  offset    = nullptr;
    juce::AudioParameterChoice* model     = nullptr;
    juce::AudioParameterFloat*  stereo    = nullptr;
    juce::AudioParameterFloat*  fbLevel   = nullptr;
    juce::AudioParameterChoice* fbMode    = nullptr;
    juce::AudioParameterFloat*  fbCutoff  = nullptr;
    juce::AudioParameterFloat*  fbRes     = nullptr;
    juce::AudioParameterFloat*  smpRate   = nullptr;
    juce::AudioParameterFloat*  bits      = nullptr;
    juce::AudioParameterChoice* distMode  = nullptr;
    juce::AudioParameterFloat*  distLevel = nullptr;
    juce::AudioParameterFloat*  distKnee  = nullptr;
    juce::AudioParameterFloat*  distSym   = nullptr;
    juce::AudioParameterFloat*  modRate   = nullptr;
    juce::AudioParameterFloat*  modTime   = nullptr;
    juce::AudioParameterFloat*  modCutoff = nullptr;
    juce::AudioParameterFloat*  diffLevel = nullptr;
    juce::AudioParameterFloat*  diffSpread= nullptr;
    juce::AudioParameterFloat*  tone      = nullptr;
    juce::AudioParameterFloat*  wet       = nullptr;
    juce::AudioParameterFloat*  dry       = nullptr;

private:
    static constexpr int numAllpass = 4;

    struct Allpass
    {
        std::vector<float> buf;
        int idx = 0, len = 1;

        void prepare (int maxLen) { buf.assign ((size_t) juce::jmax (1, maxLen), 0.0f); idx = 0; len = 1; }
        void reset()              { std::fill (buf.begin(), buf.end(), 0.0f); idx = 0; }

        float process (float x, float g)
        {
            const int n = (int) buf.size();
            const int l = juce::jlimit (1, n, len);
            const float delayed = buf[(size_t) idx];
            const float y = -g * x + delayed;
            buf[(size_t) idx] = x + g * y;
            if (++idx >= l) idx = 0;
            return y;
        }
    };

    struct Chan
    {
        std::vector<float> buf;
        float curDelay = 1.0f;
        bool  xfActive = false;
        float xfOld = 1.0f, xfNew = 1.0f;
        int   xfPos = 0, xfLen = 1;
        float shHold = 0.0f, shPhase = 1.0f;
        float dcX1 = 0.0f, dcY1 = 0.0f;
        Allpass ap[numAllpass];
    };

    float readDelay (const std::vector<float>& buf, int wp, float delaySamples) const;

    double sr = 44100.0;
    int    bufSize = 0;
    int    writePos = 0;

    Chan chan[2];
    juce::dsp::StateVariableTPTFilter<float> fbFilter, toneFilter;

    float lfoPhase = 0.0f;
    juce::SmoothedValue<float> inWetSm, wetSm, drySm, fbSm;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DelayProcessor)
};

class DelayLedToggle;
class DelayLedChoice;
class DelayDistCurve;

class DelayEditor : public juce::AudioProcessorEditor,
                    private juce::Slider::Listener
{
public:
    DelayEditor (DelayProcessor& p);
    ~DelayEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void sliderValueChanged (juce::Slider*) override;

    struct KnobCell { ValueKnob* knob = nullptr; juce::Label* label = nullptr; };
    KnobCell mk (juce::RangedAudioParameter& p, const juce::String& cap, double def);

    DelayProcessor& proc;

    juce::OwnedArray<ValueKnob> knobs;
    juce::OwnedArray<juce::Label> labels;
    juce::OwnedArray<juce::SliderParameterAttachment> knobAtts;

    ValueSlider inWetFader;
    juce::Label inWetLabel;
    juce::SliderParameterAttachment inWetAtt { *proc.inWet, inWetFader };

    ValueSlider smoothFader;
    juce::Label smoothLabel;
    juce::SliderParameterAttachment smoothAtt { *proc.smoothing, smoothFader };

    std::unique_ptr<DelayLedToggle> syncLed, keepLed;
    std::unique_ptr<DelayLedChoice> modelLed, fbModeLed, distLed;
    std::unique_ptr<DelayDistCurve> distCurve;

    KnobCell timeK, offsetK, stereoK, fbLevelK, cutoffK, resK, smpK, bitsK,
             modRateK, modTimeK, modCutK, diffLevelK, diffSpreadK,
             distLevelK, distKneeK, distSymK, wetK, toneK, dryK;

    juce::Rectangle<int> secInput, secTime, secModel, secFeedback,
                         secMod, secDiff, secDist, secOutput, modArrowArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DelayEditor)
};
