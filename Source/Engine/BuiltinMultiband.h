#pragma once
#include <JuceHeader.h>
#include "BuiltinEffects.h"

struct BandCompressor
{
    void prepare (double sr)         { sampleRate = sr > 0 ? sr : 44100.0; grDb = 0.0f; holdCounter = 0; }
    void reset()                     { grDb = 0.0f; holdCounter = 0; }

    void setTimes (float attMs, float relMs)
    {
        attCoeff = coeff (attMs);
        relCoeff = coeff (relMs);
    }

    void setHoldMs (float ms)        { holdSamps = (int) (juce::jmax (0.0f, ms) * 0.001f * (float) sampleRate); }

    float process (float detectLin, float thresholdDb, float kneeDb, float ratio)
    {
        if (! std::isfinite (detectLin))
            detectLin = 0.0f;
        const float inDb     = juce::Decibels::gainToDecibels (detectLin, -120.0f);
        const float targetGr = juce::jlimit (-120.0f, 0.0f,
                                             staticGrDb (inDb, thresholdDb, kneeDb, ratio));

        if (targetGr < grDb)
        {
            grDb = attCoeff * grDb + (1.0f - attCoeff) * targetGr;
            holdCounter = holdSamps;
        }
        else if (holdCounter > 0)
        {
            --holdCounter;
        }
        else
        {
            grDb = relCoeff * grDb + (1.0f - relCoeff) * targetGr;
        }

        grDb = juce::jlimit (-120.0f, 0.0f, grDb);
        return juce::Decibels::decibelsToGain (grDb);
    }

    static float staticGrDb (float inDb, float thrDb, float kneeDb, float ratio)
    {
        const float slope = 1.0f - 1.0f / juce::jmax (1.0f, ratio);
        const float diff  = inDb - thrDb;
        if (kneeDb > 1.0e-4f && diff > -kneeDb * 0.5f && diff < kneeDb * 0.5f)
        {
            const float x = diff + kneeDb * 0.5f;
            return -slope * (x * x) / (2.0f * kneeDb);
        }
        return diff <= 0.0f ? 0.0f : -slope * diff;
    }

    float currentReductionDb() const { return -grDb; }

    float coeff (float ms) const
    {
        return std::exp (-1.0f / (juce::jmax (1.0e-4f, ms * 0.001f) * (float) sampleRate));
    }

    double sampleRate = 44100.0;
    float  grDb = 0.0f;
    float  attCoeff = 0.0f, relCoeff = 0.0f;
    int    holdSamps = 0, holdCounter = 0;
};

struct ThreeBandSplitter
{
    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        lr1.prepare (spec); lr2.prepare (spec); apLow.prepare (spec);
        apLow.setType (juce::dsp::LinkwitzRileyFilterType::allpass);
        reset();
    }

    void reset() { lr1.reset(); lr2.reset(); apLow.reset(); }

    void setFrequencies (float f1, float f2)
    {
        lr1.setCutoffFrequency (f1);
        lr2.setCutoffFrequency (f2);
        apLow.setCutoffFrequency (f2);
    }

    void process (float in, float& low, float& mid, float& high)
    {
        float lo, hiTmp;
        lr1.processSample (0, in, lo, hiTmp);
        lr2.processSample (0, hiTmp, mid, high);
        low = apLow.processSample (0, lo);
    }

    juce::dsp::LinkwitzRileyFilter<float> lr1, lr2, apLow;
};

class MultibandProcessor : public BuiltinEffectBase
{
public:
    static constexpr int numBands = 3;

    MultibandProcessor();

    juce::String effectName() const override { return "Multiband Compressor"; }

    void prepareToPlay (double sampleRate, int blockSize) override;
    void reset() override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;

    struct Band
    {
        juce::AudioParameterChoice* mode  = nullptr;
        juce::AudioParameterFloat*  thres = nullptr;
        juce::AudioParameterFloat*  ratio = nullptr;
        juce::AudioParameterFloat*  knee  = nullptr;
        juce::AudioParameterFloat*  att   = nullptr;
        juce::AudioParameterFloat*  rel   = nullptr;
        juce::AudioParameterFloat*  gain  = nullptr;
    };

    juce::AudioParameterFloat* inGain    = nullptr;
    juce::AudioParameterFloat* xoverLow  = nullptr;
    juce::AudioParameterFloat* xoverHigh = nullptr;
    Band band[numBands];
    juce::AudioParameterBool*  limiter   = nullptr;

    std::atomic<float> grMeter[numBands] { { 0.0f }, { 0.0f }, { 0.0f } };

private:
    double sr = 44100.0;
    ThreeBandSplitter split[2];
    BandCompressor    comp[numBands];
    juce::SmoothedValue<float> inSm, bandGainSm[numBands];

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MultibandProcessor)
};

class MultibandEditor : public juce::AudioProcessorEditor,
                        private juce::Timer
{
public:
    MultibandEditor (MultibandProcessor& p);
    ~MultibandEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    struct BandStrip;

    MultibandProcessor& proc;

    ValueKnob inGainK, xLowK, xHighK;
    juce::Label inGainL, xLowL, xHighL, title;
    std::unique_ptr<juce::ToggleButton> limiterBtn;
    std::unique_ptr<juce::ButtonParameterAttachment> limiterAtt;
    juce::OwnedArray<BandStrip> strips;

    juce::SliderParameterAttachment inGainAtt { *proc.inGain,    inGainK };
    juce::SliderParameterAttachment xLowAtt   { *proc.xoverLow,  xLowK };
    juce::SliderParameterAttachment xHighAtt  { *proc.xoverHigh, xHighK };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MultibandEditor)
};
