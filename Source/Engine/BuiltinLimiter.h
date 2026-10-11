#pragma once
#include <JuceHeader.h>
#include "BuiltinMultiband.h"

struct PeakLimiter
{
    void prepare (double sampleRate, float lookaheadMs, float releaseMs)
    {
        sr   = sampleRate > 0 ? sampleRate : 44100.0;
        look = juce::jmax (1, (int) (lookaheadMs * 0.001f * (float) sr));
        for (auto& b : buf) b.assign ((size_t) look + 1, 0.0f);
        writePos = 0;
        gain = 1.0f;
        atkCoeff = std::exp (-1.0f / (juce::jmax (1.0e-4f, lookaheadMs * 0.001f) * (float) sr));
        relCoeff = std::exp (-1.0f / (juce::jmax (1.0e-4f, releaseMs   * 0.001f) * (float) sr));
        redDb = 0.0f;
    }

    int lookahead() const { return look; }

    void reset()
    {
        for (auto& b : buf) std::fill (b.begin(), b.end(), 0.0f);
        writePos = 0;
        gain = 1.0f;
        redDb = 0.0f;
    }

    void process (float& l, float& r, float ceilingLin)
    {
        if (! std::isfinite (l)) l = 0.0f;
        if (! std::isfinite (r)) r = 0.0f;

        const float peak = juce::jmax (std::abs (l), std::abs (r));
        float target = 1.0f;
        if (peak > ceilingLin && peak > 1.0e-9f)
            target = ceilingLin / peak;

        gain = target < gain ? atkCoeff * gain + (1.0f - atkCoeff) * target
                             : relCoeff * gain + (1.0f - relCoeff) * target;

        buf[0][(size_t) writePos] = l;
        buf[1][(size_t) writePos] = r;
        int readPos = writePos - look;
        if (readPos < 0) readPos += (int) buf[0].size();
        float dl = buf[0][(size_t) readPos] * gain;
        float dr = buf[1][(size_t) readPos] * gain;
        if (++writePos >= (int) buf[0].size()) writePos = 0;

        l = juce::jlimit (-ceilingLin, ceilingLin, dl);
        r = juce::jlimit (-ceilingLin, ceilingLin, dr);
        redDb = juce::Decibels::gainToDecibels (gain);
    }

    std::vector<float> buf[2];
    int    look = 1, writePos = 0;
    float  gain = 1.0f, atkCoeff = 0.0f, relCoeff = 0.0f, redDb = 0.0f;
    double sr = 44100.0;
};

class LimiterProcessor : public BuiltinEffectBase
{
public:
    LimiterProcessor();

    juce::String effectName() const override { return "Limiter"; }

    void prepareToPlay (double sampleRate, int blockSize) override;
    void reset() override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    juce::AudioParameterFloat*  gain    = nullptr;
    juce::AudioParameterFloat*  sat     = nullptr;
    juce::AudioParameterFloat*  thres   = nullptr;
    juce::AudioParameterFloat*  knee    = nullptr;
    juce::AudioParameterFloat*  ratio   = nullptr;
    juce::AudioParameterFloat*  att     = nullptr;
    juce::AudioParameterFloat*  rel     = nullptr;
    juce::AudioParameterFloat*  sustain = nullptr;
    juce::AudioParameterChoice* curve   = nullptr;
    juce::AudioParameterFloat*  gateThres = nullptr;
    juce::AudioParameterFloat*  gateFloor = nullptr;
    juce::AudioParameterFloat*  gateRel   = nullptr;
    juce::AudioParameterFloat*  ceiling   = nullptr;

    std::atomic<float> grMeter { 0.0f };

private:
    double sr = 44100.0;
    BandCompressor comp;
    PeakLimiter    limit;
    float gateGain = 1.0f;
    float gateAtkCoeff = 0.0f;
    juce::SmoothedValue<float> preSm;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LimiterProcessor)
};

class LimiterEditor : public juce::AudioProcessorEditor,
                      private juce::Timer
{
public:
    LimiterEditor (LimiterProcessor& p);
    ~LimiterEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void updateTab();

    struct GrView : public juce::Component
    {
        std::array<float, 256> history {};
        int head = 0;
        void push (float db) { history[(size_t) head] = db; head = (head + 1) % (int) history.size(); }
        void paint (juce::Graphics&) override;
    };

    LimiterProcessor& proc;
    bool showComp = true;

    GrView grView;
    juce::TextButton compTab { "COMP" }, limitTab { "LIMIT" };

    ValueKnob gainK, satK, thresK, kneeK, ratioK, attK, relK, sustainK,
              gThresK, gFloorK, gRelK, ceilK;
    juce::ComboBox curveBox;
    juce::Label gainL, satL, thresL, kneeL, ratioL, attL, relL, sustainL, curveL,
                gThresL, gFloorL, gRelL, ceilL,
                loudHdr, envHdr, gateHdr;

    juce::SliderParameterAttachment gainAtt   { *proc.gain,      gainK };
    juce::SliderParameterAttachment satAtt    { *proc.sat,       satK };
    juce::SliderParameterAttachment thresAtt  { *proc.thres,     thresK };
    juce::SliderParameterAttachment kneeAtt   { *proc.knee,      kneeK };
    juce::SliderParameterAttachment ratioAtt  { *proc.ratio,     ratioK };
    juce::SliderParameterAttachment attAtt    { *proc.att,       attK };
    juce::SliderParameterAttachment relAtt    { *proc.rel,       relK };
    juce::SliderParameterAttachment sustainAtt{ *proc.sustain,   sustainK };
    juce::ComboBoxParameterAttachment curveAtt{ *proc.curve,     curveBox };
    juce::SliderParameterAttachment gThresAtt { *proc.gateThres, gThresK };
    juce::SliderParameterAttachment gFloorAtt { *proc.gateFloor, gFloorK };
    juce::SliderParameterAttachment gRelAtt   { *proc.gateRel,   gRelK };
    juce::SliderParameterAttachment ceilAtt   { *proc.ceiling,   ceilK };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LimiterEditor)
};
