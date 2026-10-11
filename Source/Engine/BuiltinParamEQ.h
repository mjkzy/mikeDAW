#pragma once
#include <JuceHeader.h>
#include "BuiltinEffects.h"

class ParamEQProcessor : public BuiltinEffectBase,
                         private juce::Timer,
                         private juce::AudioProcessorParameter::Listener
{
public:
    static constexpr int numBands  = 7;
    static constexpr int maxStages = 4;

    enum BandType { Off = 0, LowPass, BandPass, HighPass, Notch, LowShelf, Peaking, HighShelf };

    static constexpr float minFreq = 20.0f, maxFreq = 20000.0f;
    static constexpr float minGainDb = -18.0f, maxGainDb = 18.0f;
    static constexpr float minBw = 0.10f, maxBw = 6.00f;

    static const float defaultFreq[numBands];
    static const float defaultBw[numBands];
    static const int   defaultType[numBands];

    ParamEQProcessor();
    ~ParamEQProcessor() override;

    juce::String effectName() const override { return "Parametric EQ 2"; }

    void prepareToPlay (double sampleRate, int blockSize) override;
    void reset() override;
    void releaseResources() override { prepared.store (false); }
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;

    struct BandParams
    {
        juce::AudioParameterChoice* type   = nullptr;
        juce::AudioParameterFloat*  freq   = nullptr;
        juce::AudioParameterFloat*  gain   = nullptr;
        juce::AudioParameterFloat*  bw     = nullptr;
        juce::AudioParameterChoice* slope  = nullptr;
        juce::AudioParameterChoice* chan   = nullptr;
        juce::AudioParameterBool*   gentle = nullptr;
    };
    BandParams band[numBands];

    juce::AudioParameterFloat*  master   = nullptr;
    juce::AudioParameterBool*   linphase = nullptr;
    juce::AudioParameterBool*   hq       = nullptr;
    juce::AudioParameterChoice* monitor  = nullptr;
    juce::AudioParameterBool*   heatmap  = nullptr;

    int  slopeOrder (int i) const;
    void setSlopeOrder (int i, int order);

    static int designBand (double sr, int type, int slope, bool gentle,
                           float f, float gainDb, float bw, std::array<float, 6> out[maxStages]);

    static constexpr int analyzerRingSize = 8192;
    float analyzerRing[analyzerRingSize] = { 0.0f };
    std::atomic<int> analyzerWrite { 0 };

private:
    struct Band
    {
        juce::dsp::IIR::Filter<float> stage[maxStages][2];
        juce::dsp::IIR::Coefficients<float>::Ptr coef[maxStages];
        juce::SmoothedValue<float> freqSm, gainSm, bwSm;

        int   lastType = -1, lastSlope = -1, lastChan = -1, lastGentle = -1;
        float lastFreq = -1.0f, lastGain = -1000.0f, lastBw = -1.0f;
        bool  wasOff   = true;

        int nStages   = 1;
        int activeType = 0;
        int chanMode  = 0;

        void resetState()
        {
            for (int st = 0; st < maxStages; ++st)
                for (int ch = 0; ch < 2; ++ch)
                    stage[st][ch].reset();
        }
    };

    void updateSmoothingTargets();
    void updateCoeffs (int sub);
    void computeBandCoeffs (Band& b, int type, int slope, bool gentle, float f, float gainDb, float bw);
    void processBands (float* ch0, float* ch1, int n, int nch, double rate);
    void resetAllBands();

    void timerCallback() override;
    void designKernel();
    void parameterValueChanged (int, float) override { firDirty.store (true, std::memory_order_relaxed); }
    void parameterGestureChanged (int, bool) override {}

    inline float filterChain (Band& b, int chIdx, float x)
    {
        for (int st = 0; st < b.nStages; ++st)
            x = b.stage[st][chIdx].processSample (x);
        return x;
    }

    inline void processBandSample (Band& b, float& l, float& r, int nch)
    {
        switch (b.chanMode)
        {
            case 1:
                l = filterChain (b, 0, l);
                break;
            case 2:
                if (nch > 1) r = filterChain (b, 1, r);
                break;
            case 3:
            {
                const float m = (l + r) * 0.5f, s = (l - r) * 0.5f;
                const float fm = filterChain (b, 0, m);
                l = fm + s; r = fm - s;
                break;
            }
            case 4:
            {
                const float m = (l + r) * 0.5f, s = (l - r) * 0.5f;
                const float fs = filterChain (b, 0, s);
                l = m + fs; r = m - fs;
                break;
            }
            default:
                l = filterChain (b, 0, l);
                if (nch > 1) r = filterChain (b, 1, r);
                break;
        }
    }

    inline void writeAnalyzer (float v) noexcept
    {
        const int w = analyzerWrite.load (std::memory_order_relaxed);
        analyzerRing[w & (analyzerRingSize - 1)] = v;
        analyzerWrite.store (w + 1, std::memory_order_release);
    }

    double sr = 44100.0;
    double bandSr = 44100.0;
    int    maxBlock = 0;
    int    osChannels = 2;
    Band bands[numBands];
    juce::SmoothedValue<float> masterSm { 1.0f };

    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    juce::dsp::Convolution convolution;

    static constexpr int firFftOrder = 13;
    static constexpr int firFftSize  = 1 << firFftOrder;
    static constexpr int firTaps     = 4096;
    static constexpr int firLatency  = 2048;
    juce::dsp::FFT firFft { firFftOrder };
    std::vector<juce::dsp::Complex<float>> firSpectrum, firTime;

    juce::CriticalSection firLock;

    std::atomic<bool> firDirty  { true };
    std::atomic<bool> prepared  { false };
    bool lastLin = false, lastHq = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamEQProcessor)
};
