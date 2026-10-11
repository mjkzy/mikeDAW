#include "BuiltinParamEQ.h"

namespace
{
    juce::NormalisableRange<float> logRange (float lo, float hi, float centre)
    {
        auto r = juce::NormalisableRange<float> (lo, hi);
        r.setSkewForCentre (centre);
        return r;
    }

    const float kButterQ[ParamEQProcessor::maxStages][ParamEQProcessor::maxStages] =
    {
        { 0.707f, 0.0f,   0.0f,   0.0f   },
        { 0.541f, 1.307f, 0.0f,   0.0f   },
        { 0.518f, 0.707f, 1.932f, 0.0f   },
        { 0.510f, 0.601f, 0.900f, 2.563f }
    };

    constexpr int kSubBlock = 32;

    int designSteepShelf (double sr, bool low, int stages, float f, float gainDb,
                          std::array<float, 6> out[ParamEQProcessor::maxStages])
    {
        const int order = stages * 2;
        const double g = juce::Decibels::decibelsToGain ((double) gainDb);
        const double spread = std::pow (g, 0.5 / order);
        const double nyq = sr * 0.49;
        const double fz = juce::jmin (nyq, (double) f * (low ? spread : 1.0 / spread));
        const double fp = juce::jmin (nyq, (double) f * (low ? 1.0 / spread : spread));
        const double wz = std::tan (juce::MathConstants<double>::pi * fz / sr);
        const double wp = std::tan (juce::MathConstants<double>::pi * fp / sr);
        const double k = low ? 1.0 : std::pow (g, 1.0 / stages);

        for (int st = 0; st < stages; ++st)
        {
            const double d = 1.0 / kButterQ[stages - 1][st];
            const double b0 = k * wz * wz, b1 = k * d * wz, b2 = k;
            const double a0 = wp * wp,     a1 = d * wp,     a2 = 1.0;
            out[st] = { (float) (b0 + b1 + b2), (float) (2.0 * (b0 - b2)), (float) (b0 - b1 + b2),
                        (float) (a0 + a1 + a2), (float) (2.0 * (a0 - a2)), (float) (a0 - a1 + a2) };
        }
        return stages;
    }
}

const float ParamEQProcessor::defaultFreq[ParamEQProcessor::numBands] =
    { 33.0f, 80.0f, 198.0f, 490.0f, 1213.0f, 3000.0f, 7423.0f };
const float ParamEQProcessor::defaultBw[ParamEQProcessor::numBands] =
    { 2.30f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 2.30f };
const int ParamEQProcessor::defaultType[ParamEQProcessor::numBands] =
    { LowShelf, Peaking, Peaking, Peaking, Peaking, Peaking, HighShelf };

ParamEQProcessor::ParamEQProcessor()
{
    const juce::StringArray typeChoices { "Off", "Low Pass", "Band Pass", "High Pass",
                                          "Notch", "Low Shelf", "Peaking", "High Shelf" };
    const juce::StringArray slopeChoices { "12 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct" };
    const juce::StringArray chanChoices  { "Stereo", "Left", "Right", "Mid", "Side" };

    for (int i = 0; i < numBands; ++i)
    {
        const juce::String p = "b" + juce::String (i + 1);
        const juce::String n = "Band " + juce::String (i + 1) + " ";

        addParameter (band[i].type = new juce::AudioParameterChoice (
            juce::ParameterID { p + "type", 1 }, n + "Type", typeChoices, defaultType[i]));
        addParameter (band[i].freq = new juce::AudioParameterFloat (
            juce::ParameterID { p + "freq", 1 }, n + "Freq",
            logRange (minFreq, maxFreq, 632.0f), defaultFreq[i]));
        addParameter (band[i].gain = new juce::AudioParameterFloat (
            juce::ParameterID { p + "gain", 1 }, n + "Gain (dB)",
            juce::NormalisableRange<float> (minGainDb, maxGainDb, 0.1f), 0.0f));
        addParameter (band[i].bw = new juce::AudioParameterFloat (
            juce::ParameterID { p + "bw", 1 }, n + "Bandwidth",
            juce::NormalisableRange<float> (minBw, maxBw, 0.01f), defaultBw[i]));
        addParameter (band[i].slope = new juce::AudioParameterChoice (
            juce::ParameterID { p + "slope", 1 }, n + "Slope", slopeChoices, 0));
        addParameter (band[i].chan = new juce::AudioParameterChoice (
            juce::ParameterID { p + "chan", 1 }, n + "Channel", chanChoices, 0));
    }

    addParameter (master = new juce::AudioParameterFloat (
        juce::ParameterID { "master", 1 }, "Main level (dB)",
        juce::NormalisableRange<float> (minGainDb, maxGainDb, 0.1f), 0.0f));
    addParameter (linphase = new juce::AudioParameterBool (
        juce::ParameterID { "linphase", 1 }, "Linear phase", false));
    addParameter (hq = new juce::AudioParameterBool (
        juce::ParameterID { "hq", 1 }, "HQ (oversample)", false));
    addParameter (monitor = new juce::AudioParameterChoice (
        juce::ParameterID { "monitor", 1 }, "Analyzer",
        juce::StringArray { "Off", "Input", "Output" }, 2));

    for (int i = 0; i < numBands; ++i)
        addParameter (band[i].gentle = new juce::AudioParameterBool (
            juce::ParameterID { "b" + juce::String (i + 1) + "gentle", 1 },
            "Band " + juce::String (i + 1) + " Gentle", false));

    addParameter (heatmap = new juce::AudioParameterBool (
        juce::ParameterID { "heatmap", 1 }, "Heatmap", true));

    for (auto* p : getParameters())
        p->addListener (this);

    startTimerHz (30);
}

ParamEQProcessor::~ParamEQProcessor()
{
    stopTimer();
    for (auto* p : getParameters())
        p->removeListener (this);
}

int ParamEQProcessor::slopeOrder (int i) const
{
    const int s = band[i].slope->getIndex();
    return band[i].gentle->get() ? s : -s;
}

void ParamEQProcessor::setSlopeOrder (int i, int order)
{
    order = juce::jlimit (-3, 3, order);
    band[i].slope->setValueNotifyingHost (band[i].slope->convertTo0to1 ((float) std::abs (order)));
    band[i].gentle->setValueNotifyingHost (order > 0 ? 1.0f : 0.0f);
}

void ParamEQProcessor::prepareToPlay (double sampleRate, int blockSize)
{
    sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    bandSr = sr;
    maxBlock = juce::jmax (1, blockSize);
    osChannels = juce::jmax (1, getTotalNumInputChannels());
    const juce::dsp::ProcessSpec spec { sr, (juce::uint32) maxBlock, 1 };

    for (int i = 0; i < numBands; ++i)
    {
        auto& b = bands[i];
        for (int st = 0; st < maxStages; ++st)
        {
            b.coef[st] = new juce::dsp::IIR::Coefficients<float>();
            for (int ch = 0; ch < 2; ++ch)
            {
                b.stage[st][ch].coefficients = b.coef[st];
                b.stage[st][ch].prepare (spec);
                b.stage[st][ch].reset();
            }
        }

        b.freqSm.reset (sr, 0.015); b.gainSm.reset (sr, 0.015); b.bwSm.reset (sr, 0.015);
        b.freqSm.setCurrentAndTargetValue (band[i].freq->get());
        b.gainSm.setCurrentAndTargetValue (band[i].gain->get());
        b.bwSm.setCurrentAndTargetValue   (band[i].bw->get());

        b.lastType = -1; b.lastSlope = -1; b.lastChan = -1; b.lastGentle = -1;
        b.lastFreq = -1.0f; b.lastGain = -1000.0f; b.lastBw = -1.0f;
        b.wasOff = true;
        b.nStages = 1; b.activeType = 0; b.chanMode = 0;
    }

    masterSm.reset (sr, 0.02);
    masterSm.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (master->get()));

    oversampler = std::make_unique<juce::dsp::Oversampling<float>> (
        (size_t) osChannels, 1,
        juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
    oversampler->initProcessing ((size_t) maxBlock);
    oversampler->reset();

    lastLin = false;
    lastHq  = false;

    prepared.store (true, std::memory_order_relaxed);
    firDirty.store (false, std::memory_order_relaxed);

    {
        const juce::ScopedLock fl (firLock);
        convolution.prepare ({ sr, (juce::uint32) maxBlock, (juce::uint32) osChannels });
        convolution.reset();
        firSpectrum.assign ((size_t) firFftSize, {});
        firTime.assign ((size_t) firFftSize, {});
        designKernel();
    }

    setLatencySamples (linphase->get() ? firLatency : 0);
}

int ParamEQProcessor::designBand (double sr, int type, int slope, bool gentle,
                                  float f, float gainDb, float bw, std::array<float, 6> out[maxStages])
{
    using AC = juce::dsp::IIR::ArrayCoefficients<float>;
    const float nyq = (float) (sr * 0.49);
    f  = juce::jlimit (10.0f, nyq, f);
    bw = juce::jlimit (minBw, maxBw, bw);
    const int stages = juce::jlimit (1, maxStages, slope + 1);

    if (type == LowPass || type == HighPass)
    {
        const float scale = juce::jlimit (0.5f, 8.0f, 0.707f / bw) / 0.707f;
        for (int st = 0; st < stages; ++st)
        {
            const float q = juce::jmax (0.3f, (gentle ? kButterQ[0][0] : kButterQ[stages - 1][st]) * scale);
            out[st] = type == LowPass ? AC::makeLowPass  (sr, f, q)
                                      : AC::makeHighPass (sr, f, q);
        }
        return stages;
    }

    if (type == LowShelf || type == HighShelf)
    {
        if (stages > 1 && ! gentle)
            return designSteepShelf (sr, type == LowShelf, stages, f, gainDb, out);

        const float gainLin = juce::Decibels::decibelsToGain (gainDb / (float) stages);
        const float s = juce::jlimit (0.2f, 1.2f, 0.9f / bw);
        for (int st = 0; st < stages; ++st)
            out[st] = type == LowShelf ? AC::makeLowShelf  (sr, f, s, gainLin)
                                       : AC::makeHighShelf (sr, f, s, gainLin);
        return stages;
    }

    const float gainLin = juce::Decibels::decibelsToGain (gainDb);
    const float p = std::pow (2.0f, bw);
    const float q = std::sqrt (p) / (p - 1.0f);
    switch (type)
    {
        case BandPass: out[0] = AC::makeBandPass (sr, f, q); break;
        case Notch:    out[0] = AC::makeNotch (sr, f, q); break;
        case Peaking:
        default:       out[0] = AC::makePeakFilter (sr, f, q, gainLin); break;
    }
    return 1;
}

void ParamEQProcessor::computeBandCoeffs (Band& b, int type, int slope, bool gentle, float f, float gainDb, float bw)
{
    std::array<float, 6> arr[maxStages];
    b.nStages = designBand (bandSr, type, slope, gentle, f, gainDb, bw, arr);
    for (int st = 0; st < b.nStages; ++st)
        *b.coef[st] = arr[st];
}

void ParamEQProcessor::updateSmoothingTargets()
{
    for (int i = 0; i < numBands; ++i)
    {
        bands[i].freqSm.setTargetValue (band[i].freq->get());
        bands[i].gainSm.setTargetValue (band[i].gain->get());
        bands[i].bwSm.setTargetValue   (band[i].bw->get());
    }
    masterSm.setTargetValue (juce::Decibels::decibelsToGain (master->get()));
}

void ParamEQProcessor::updateCoeffs (int sub)
{
    const float nyq = (float) (bandSr * 0.49);
    for (int i = 0; i < numBands; ++i)
    {
        auto& b = bands[i];
        const int type = band[i].type->getIndex();

        if (type == Off)
        {
            if (! b.wasOff) { b.resetState(); b.wasOff = true; }
            b.activeType = 0;
            b.freqSm.skip (sub); b.gainSm.skip (sub); b.bwSm.skip (sub);
            continue;
        }

        const float f    = juce::jlimit (10.0f, nyq, b.freqSm.getCurrentValue());
        const float gDb  = b.gainSm.getCurrentValue();
        const float bw   = juce::jlimit (0.10f, 6.00f, b.bwSm.getCurrentValue());
        const int   slope = band[i].slope->getIndex();
        const int   chan  = band[i].chan->getIndex();
        const int   gentle = band[i].gentle->get() ? 1 : 0;

        const bool structural = (type != b.lastType || slope != b.lastSlope
                                 || chan != b.lastChan || gentle != b.lastGentle || b.wasOff);
        if (structural)
            b.resetState();

        const bool moving = b.freqSm.isSmoothing() || b.gainSm.isSmoothing() || b.bwSm.isSmoothing();
        const bool changed = structural || moving
                             || ! juce::approximatelyEqual (f,   b.lastFreq)
                             || ! juce::approximatelyEqual (gDb, b.lastGain)
                             || ! juce::approximatelyEqual (bw,  b.lastBw);

        if (changed)
            computeBandCoeffs (b, type, slope, gentle != 0, f, gDb, bw);

        b.lastType = type; b.lastSlope = slope; b.lastChan = chan; b.lastGentle = gentle;
        b.lastFreq = f; b.lastGain = gDb; b.lastBw = bw;
        b.wasOff = false;
        b.activeType = type; b.chanMode = chan;

        b.freqSm.skip (sub); b.gainSm.skip (sub); b.bwSm.skip (sub);
    }
}

void ParamEQProcessor::resetAllBands()
{
    for (auto& b : bands)
    {
        b.resetState();
        b.lastType = -1; b.lastSlope = -1; b.lastChan = -1; b.lastGentle = -1;
        b.lastFreq = -1.0f; b.lastGain = -1000.0f; b.lastBw = -1.0f;
        b.wasOff = true;
    }
}

void ParamEQProcessor::reset()
{
    resetAllBands();

    if (oversampler != nullptr)
        oversampler->reset();

    convolution.reset();

    masterSm.setCurrentAndTargetValue (masterSm.getTargetValue());
    for (auto& b : bands)
    {
        b.freqSm.setCurrentAndTargetValue (b.freqSm.getTargetValue());
        b.gainSm.setCurrentAndTargetValue (b.gainSm.getTargetValue());
        b.bwSm.setCurrentAndTargetValue   (b.bwSm.getTargetValue());
    }
}

void ParamEQProcessor::processBands (float* ch0, float* ch1, int n, int nch, double rate)
{
    bandSr = rate;

    int pos = 0;
    while (pos < n)
    {
        const int sub = juce::jmin (kSubBlock, n - pos);
        updateCoeffs (sub);

        for (int s = 0; s < sub; ++s)
        {
            const int i = pos + s;
            float l = ch0[i];
            float r = ch1 != nullptr ? ch1[i] : l;

            for (auto& b : bands)
                if (b.activeType != Off)
                    processBandSample (b, l, r, nch);

            ch0[i] = l;
            if (ch1 != nullptr) ch1[i] = r;
        }

        pos += sub;
    }
}

void ParamEQProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int n   = buffer.getNumSamples();
    const int nch = buffer.getNumChannels();
    if (n == 0 || nch < 1 || sr <= 0.0)
        return;

    const bool linOn = linphase->get();
    const bool hqOn  = hq->get() && ! linOn && oversampler != nullptr && nch == osChannels;

    if (linOn != lastLin)
    {
        if (linOn) { convolution.reset(); firDirty.store (true, std::memory_order_relaxed); }
        else       { resetAllBands(); }
        lastLin = linOn;
    }
    if (hqOn != lastHq)
    {
        resetAllBands();
        if (oversampler != nullptr) oversampler->reset();

        const double smRate = hqOn ? sr * 2.0 : sr;
        for (int i = 0; i < numBands; ++i)
        {
            auto& b = bands[i];
            b.freqSm.reset (smRate, 0.015); b.gainSm.reset (smRate, 0.015); b.bwSm.reset (smRate, 0.015);
            b.freqSm.setCurrentAndTargetValue (band[i].freq->get());
            b.gainSm.setCurrentAndTargetValue (band[i].gain->get());
            b.bwSm.setCurrentAndTargetValue   (band[i].bw->get());
        }
        lastHq = hqOn;
    }

    updateSmoothingTargets();

    const int monitorMode = monitor->getIndex();
    float* ch0 = buffer.getWritePointer (0);
    float* ch1 = nch > 1 ? buffer.getWritePointer (1) : nullptr;

    if (monitorMode == 1)
        for (int i = 0; i < n; ++i)
            writeAnalyzer ((ch0[i] + (ch1 != nullptr ? ch1[i] : ch0[i])) * 0.5f);

    if (linOn)
    {
        juce::dsp::AudioBlock<float> block (buffer);
        juce::dsp::ProcessContextReplacing<float> ctx (block);
        convolution.process (ctx);
    }
    else if (hqOn)
    {
        juce::dsp::AudioBlock<float> block (buffer);
        auto up = oversampler->processSamplesUp (block);
        processBands (up.getChannelPointer (0),
                      up.getNumChannels() > 1 ? up.getChannelPointer (1) : nullptr,
                      (int) up.getNumSamples(), (int) up.getNumChannels(), sr * 2.0);
        oversampler->processSamplesDown (block);
    }
    else
    {
        processBands (ch0, ch1, n, nch, sr);
    }

    for (int i = 0; i < n; ++i)
    {
        const float g = masterSm.getNextValue();
        const float l = ch0[i] * g;
        ch0[i] = l;
        float r = l;
        if (ch1 != nullptr) { r = ch1[i] * g; ch1[i] = r; }
        if (monitorMode == 2)
            writeAnalyzer ((l + r) * 0.5f);
    }
}

void ParamEQProcessor::timerCallback()
{
    const bool lin = linphase->get();
    const int  wantLatency = lin ? firLatency : 0;
    if (getLatencySamples() != wantLatency)
        setLatencySamples (wantLatency);

    if (prepared.load (std::memory_order_relaxed) && lin
        && firDirty.exchange (false, std::memory_order_relaxed))
    {
        const juce::ScopedLock fl (firLock);
        designKernel();
    }
}

namespace
{
    using BT = ParamEQProcessor;

    juce::Colour bandColour (int i)
    {
        static const float hue[BT::numBands] =
            { 280.0f, 320.0f, 10.0f, 45.0f, 110.0f, 165.0f, 215.0f };
        return juce::Colour::fromHSV (hue[juce::jlimit (0, BT::numBands - 1, i)] / 360.0f,
                                      0.62f, 0.96f, 1.0f);
    }

    int buildUiCoeffs (double sr, int type, int slope, bool gentle, float f, float gainDb, float bw,
                       juce::dsp::IIR::Coefficients<float>::Ptr out[BT::maxStages])
    {
        std::array<float, 6> arr[BT::maxStages];
        const int n = BT::designBand (sr, type, slope, gentle, f, gainDb, bw, arr);
        for (int st = 0; st < n; ++st)
        {
            out[st] = new juce::dsp::IIR::Coefficients<float>();
            *out[st] = arr[st];
        }
        return n;
    }

    void setFloatP (juce::AudioParameterFloat* p, float v)
    {
        p->setValueNotifyingHost (p->convertTo0to1 (v));
    }

    void setChoiceP (juce::AudioParameterChoice* p, int i)
    {
        p->setValueNotifyingHost (p->convertTo0to1 ((float) i));
    }

    bool verticalIsBw (int type)
    {
        return type == BT::LowPass || type == BT::HighPass || type == BT::BandPass || type == BT::Notch;
    }

    juce::String freqText (float f)
    {
        if (f >= 1000.0f) return juce::String (f / 1000.0f, f >= 10000.0f ? 1 : 2) + " kHz";
        return juce::String (juce::roundToInt (f)) + " Hz";
    }

    juce::String dbText (float db)
    {
        return (db > 0.0f ? "+" : "") + juce::String (db, 1) + " dB";
    }

    juce::Path typeGlyph (int type, juce::Rectangle<float> r)
    {
        juce::Path p;
        auto P = [&r] (float x, float y) { return juce::Point<float> (r.getX() + x * r.getWidth(), r.getY() + y * r.getHeight()); };
        switch (type)
        {
            case BT::LowShelf:
                p.startNewSubPath (P (0.0f, 0.75f)); p.lineTo (P (0.3f, 0.75f)); p.lineTo (P (0.7f, 0.25f)); p.lineTo (P (1.0f, 0.25f));
                break;
            case BT::HighShelf:
                p.startNewSubPath (P (0.0f, 0.25f)); p.lineTo (P (0.3f, 0.25f)); p.lineTo (P (0.7f, 0.75f)); p.lineTo (P (1.0f, 0.75f));
                break;
            case BT::Peaking:
                p.startNewSubPath (P (0.0f, 0.8f)); p.lineTo (P (0.25f, 0.8f));
                p.quadraticTo (P (0.5f, -0.3f), P (0.75f, 0.8f)); p.lineTo (P (1.0f, 0.8f));
                break;
            case BT::Notch:
                p.startNewSubPath (P (0.0f, 0.2f)); p.lineTo (P (0.25f, 0.2f));
                p.quadraticTo (P (0.5f, 1.3f), P (0.75f, 0.2f)); p.lineTo (P (1.0f, 0.2f));
                break;
            case BT::LowPass:
                p.startNewSubPath (P (0.0f, 0.3f)); p.lineTo (P (0.5f, 0.3f)); p.lineTo (P (0.85f, 0.95f));
                break;
            case BT::HighPass:
                p.startNewSubPath (P (0.15f, 0.95f)); p.lineTo (P (0.5f, 0.3f)); p.lineTo (P (1.0f, 0.3f));
                break;
            case BT::BandPass:
                p.startNewSubPath (P (0.1f, 0.95f)); p.lineTo (P (0.5f, 0.2f)); p.lineTo (P (0.9f, 0.95f));
                break;
            default:
                p.startNewSubPath (P (0.2f, 0.5f)); p.lineTo (P (0.8f, 0.5f));
                break;
        }
        return p;
    }

    struct EQPreset
    {
        struct B { int band, type; float freq, gain, bw; int order; };
        const char* name;
        std::vector<B> bands;
        float master;
    };

    std::vector<EQPreset> makePresets()
    {
        auto def = [] (int i) { return BT::defaultFreq[i]; };
        std::vector<EQPreset> p;
        {
            EQPreset e { "Default", {}, 0.0f };
            for (int i = 0; i < BT::numBands; ++i)
                e.bands.push_back ({ i, BT::defaultType[i], def (i), 0.0f, BT::defaultBw[i], 0 });
            p.push_back (e);
        }
        p.push_back ({ "3 bands",
            { { 0, BT::LowShelf, 90.0f, 0.0f, 2.3f, 0 },
              { 3, BT::Peaking, 1000.0f, 0.0f, 1.0f, 0 },
              { 6, BT::HighShelf, 5000.0f, 0.0f, 2.3f, 0 } }, 0.0f });
        p.push_back ({ "20Hz + 18kHz cut",
            { { 0, BT::HighPass, 20.0f, 0.0f, 1.0f, -3 },
              { 6, BT::LowPass, 18000.0f, 0.0f, 1.0f, -3 } }, 0.0f });
        p.push_back ({ "30Hz + 18kHz cut",
            { { 0, BT::HighPass, 30.0f, 0.0f, 1.0f, -3 },
              { 6, BT::LowPass, 18000.0f, 0.0f, 1.0f, -3 } }, 0.0f });
        p.push_back ({ "40Hz + 18kHz cut",
            { { 0, BT::HighPass, 40.0f, 0.0f, 1.0f, -3 },
              { 6, BT::LowPass, 18000.0f, 0.0f, 1.0f, -3 } }, 0.0f });
        p.push_back ({ "40Hz cut",
            { { 0, BT::HighPass, 40.0f, 0.0f, 1.0f, -3 } }, 0.0f });
        p.push_back ({ "Kick",
            { { 0, BT::HighPass, 30.0f, 0.0f, 1.0f, -1 },
              { 1, BT::Peaking, 60.0f, 4.0f, 1.2f, 0 },
              { 2, BT::Peaking, 350.0f, -3.5f, 1.5f, 0 },
              { 5, BT::Peaking, 3200.0f, 2.5f, 1.2f, 0 } }, 0.0f });
        p.push_back ({ "Old Telephone",
            { { 0, BT::HighPass, 800.0f, 0.0f, 0.7f, -3 },
              { 3, BT::Peaking, 1700.0f, 6.0f, 1.4f, 0 },
              { 6, BT::LowPass, 3000.0f, 0.0f, 0.7f, -3 } }, 0.0f });
        {
            EQPreset e { "Peaking only", {}, 0.0f };
            for (int i = 0; i < BT::numBands; ++i)
                e.bands.push_back ({ i, BT::Peaking, def (i), 0.0f, 0.8f, 0 });
            p.push_back (e);
        }
        p.push_back ({ "Quick Low Roll-Off",
            { { 0, BT::HighPass, 120.0f, 0.0f, 1.0f, 0 } }, 0.0f });
        p.push_back ({ "Radio",
            { { 0, BT::HighPass, 300.0f, 0.0f, 1.0f, -1 },
              { 3, BT::Peaking, 2200.0f, 4.0f, 1.6f, 0 },
              { 6, BT::LowPass, 5000.0f, 0.0f, 1.0f, -1 } }, 0.0f });
        p.push_back ({ "Tight Drum Sizzle",
            { { 0, BT::HighPass, 60.0f, 0.0f, 1.0f, -1 },
              { 2, BT::Peaking, 400.0f, -2.5f, 1.6f, 0 },
              { 6, BT::HighShelf, 8000.0f, 4.5f, 2.3f, 0 } }, 0.0f });
        p.push_back ({ "Vocal Presence",
            { { 0, BT::HighPass, 100.0f, 0.0f, 1.0f, -1 },
              { 4, BT::Peaking, 3000.0f, 3.5f, 2.0f, 0 },
              { 6, BT::HighShelf, 12000.0f, 1.5f, 2.3f, 0 } }, 0.0f });
        return p;
    }

    const std::vector<EQPreset>& presets()
    {
        static const std::vector<EQPreset> table = makePresets();
        return table;
    }

    const juce::StringArray kTypeNames  { "Off", "Low Pass", "Band Pass", "High Pass",
                                          "Notch", "Low Shelf", "Peaking", "High Shelf" };
    const juce::StringArray kOrderNames { "Steep 8 (-48 dB/oct)", "Steep 6 (-36 dB/oct)", "Steep 4 (-24 dB/oct)",
                                          "2 (-12 dB/oct)",
                                          "Gentle 4 (-24 dB/oct)", "Gentle 6 (-36 dB/oct)", "Gentle 8 (-48 dB/oct)" };
    const juce::StringArray kChanNames  { "Stereo", "Left", "Right", "Mid", "Side" };

    struct Region { const char* name; float lo, hi; };
    const Region kRegions[8] = { { "", 20.0f, 40.0f }, { "SUB", 40.0f, 80.0f }, { "BASS", 80.0f, 250.0f }, { "LOW MID", 250.0f, 500.0f },
                                 { "MID", 500.0f, 2000.0f }, { "HIGH MID", 2000.0f, 4000.0f },
                                 { "PRS", 4000.0f, 6000.0f }, { "TREBLE", 6000.0f, 20000.0f } };
}

class ParamEQFader : public ValueSlider
{
public:
    ParamEQFader()
    {
        setSliderStyle (juce::Slider::LinearVertical);
        setColour (juce::Slider::thumbColourId, Colors::text);
    }

    juce::Colour colour = Colors::text;
    bool zeroCentred = true;

    void paint (juce::Graphics& g) override
    {
        const float cx = (float) getWidth() * 0.5f;
        const float top = (float) getPositionOfValue (getMaximum());
        const float bot = (float) getPositionOfValue (getMinimum());
        const float pos = (float) getPositionOfValue (getValue());
        const float zero = zeroCentred ? (float) getPositionOfValue (0.0) : bot;

        juce::Rectangle<float> track (cx - 3.0f, top, 6.0f, bot - top);
        g.setColour (Colors::eqPanelBg.darker (0.7f));
        g.fillRoundedRectangle (track, 3.0f);
        g.setColour (Colors::eqPanelBg.brighter (0.25f));
        g.drawRoundedRectangle (track, 3.0f, 1.0f);

        juce::Rectangle<float> fill (cx - 2.0f, juce::jmin (pos, zero), 4.0f, std::abs (pos - zero));
        g.setColour (colour.withAlpha (0.75f));
        g.fillRoundedRectangle (fill, 2.0f);

        if (zeroCentred)
        {
            g.setColour (Colors::eqCurve.withAlpha (0.35f));
            g.fillRect (cx - 6.0f, zero - 0.5f, 12.0f, 1.0f);
        }

        juce::Rectangle<float> thumb (cx - 9.0f, pos - 7.0f, 18.0f, 14.0f);
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRoundedRectangle (thumb.translated (0.0f, 1.5f), 4.0f);
        g.setColour (isMouseOverOrDragging() ? colour.brighter (0.25f) : colour);
        g.fillRoundedRectangle (thumb, 4.0f);
        g.setColour (juce::Colours::white.withAlpha (0.55f));
        g.fillRoundedRectangle (thumb.reduced (4.0f, 5.5f), 1.0f);
        g.setColour (juce::Colours::black.withAlpha (0.25f));
        g.drawRoundedRectangle (thumb, 4.0f, 1.0f);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamEQFader)
};

class ParamEQKnob : public ValueKnob
{
public:
    ParamEQKnob() {}

    juce::Colour colour = Colors::text;

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        const float d = juce::jmin (b.getWidth(), b.getHeight()) - 4.0f;
        auto disc = juce::Rectangle<float> (d, d).withCentre (b.getCentre());
        const float r = d * 0.5f;
        const float t = (float) valueToProportionOfLength (getValue());
        const float a0 = -2.356f, a1 = 2.356f;
        const float a = a0 + t * (a1 - a0);

        g.setColour (juce::Colours::black.withAlpha (0.4f));
        g.fillEllipse (disc.translated (0.0f, 1.0f));
        g.setColour (Colors::eqPanelBg.darker (0.55f));
        g.fillEllipse (disc);

        juce::Path ring;
        ring.addCentredArc (disc.getCentreX(), disc.getCentreY(), r - 1.5f, r - 1.5f, 0.0f, a0, a1, true);
        g.setColour (Colors::eqPanelBg.brighter (0.35f));
        g.strokePath (ring, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        juce::Path arc;
        arc.addCentredArc (disc.getCentreX(), disc.getCentreY(), r - 1.5f, r - 1.5f, 0.0f, a0, a, true);
        g.setColour (isMouseOverOrDragging() ? colour.brighter (0.2f) : colour);
        g.strokePath (arc, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        const auto c = disc.getCentre();
        const auto tip = c + juce::Point<float> (std::sin (a), -std::cos (a)) * (r - 5.0f);
        g.setColour (colour);
        g.drawLine (juce::Line<float> (c + juce::Point<float> (std::sin (a), -std::cos (a)) * 2.0f, tip), 2.0f);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamEQKnob)
};

class ParamEQBandTag : public juce::Component
{
public:
    ParamEQBandTag() { setMouseCursor (juce::MouseCursor::PointingHandCursor); }

    int index = 0;
    std::function<int()>   getType;
    std::function<bool()>  isSelected;
    std::function<void()>  onToggle;
    std::function<void()>  onMenu;

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) { if (onMenu) onMenu(); }
        else                      { if (onToggle) onToggle(); }
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced (1.0f);
        const int type = getType ? getType() : BT::Off;
        const bool on = type != BT::Off;
        const auto col = bandColour (index);
        g.setColour (on ? col : Colors::eqPanelBg.brighter (0.15f));
        g.fillRoundedRectangle (b, 3.0f);
        if (isSelected && isSelected())
        {
            g.setColour (Colors::eqCurve);
            g.drawRoundedRectangle (b, 3.0f, 1.2f);
        }
        g.setColour (on ? Colors::contrastText (col) : Colors::textDim);
        g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
        auto num = b.removeFromLeft (b.getHeight());
        g.drawText (juce::String (index + 1), num.toNearestInt(), juce::Justification::centred, false);
        g.strokePath (typeGlyph (type, b.reduced (2.0f, 3.5f)),
                      juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamEQBandTag)
};

class ParamEQGraph : public juce::Component
{
public:
    explicit ParamEQGraph (ParamEQProcessor& p) : proc (p)
    {
        std::fill (std::begin (specDb), std::end (specDb), -90.0f);
    }

    std::function<void (int, juce::Point<int>)> onBandMenu;
    std::function<void (int)> onBandSelected;
    int selectedBand = -1;

    static constexpr int headerH = 36;
    static constexpr int scaleW  = 26;
    static constexpr int footerH = 18;

    void updateSpectrum()
    {
        const int w = proc.analyzerWrite.load (std::memory_order_acquire);
        for (int i = 0; i < fftSize; ++i)
            fftData[i] = proc.analyzerRing[(w - fftSize + i) & (ParamEQProcessor::analyzerRingSize - 1)];
        for (int i = fftSize; i < fftSize * 2; ++i)
            fftData[i] = 0.0f;

        window.multiplyWithWindowingTable (fftData, fftSize);
        fft.performFrequencyOnlyForwardTransform (fftData);

        const double sr = juce::jmax (8000.0, proc.getSampleRate());
        for (int bin = 1; bin < fftSize / 2; ++bin)
        {
            const float freq = (float) (bin * sr / fftSize);
            const float mag  = fftData[bin] * (4.0f / (float) fftSize);
            const float raw  = juce::Decibels::gainToDecibels (mag, -120.0f);
            const float tilt = 4.5f * std::log2 (juce::jmax (1.0f, freq / 1000.0f));
            const float db   = raw <= -100.0f ? -90.0f
                                              : juce::jlimit (-90.0f, 6.0f, raw + tilt);
            rawDb[bin] = db;
            specDb[bin] = db > specDb[bin] ? db
                                           : specDb[bin] * 0.85f + (-90.0f) * 0.15f;
        }

        updateHeat (sr);

        float prev = specDb[1];
        for (int bin = 2; bin < fftSize / 2 - 1; ++bin)
        {
            const float cur = specDb[bin];
            specDb[bin] = (prev + cur * 2.0f + specDb[bin + 1]) * 0.25f;
            prev = cur;
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto plotArea = plot().toFloat();
        const double sr = juce::jmax (8000.0, proc.getSampleRate());

        g.setColour (Colors::eqGraphBg.darker (0.3f));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
        g.setColour (Colors::eqGraphBg);
        g.fillRect (plotArea);

        paintHeader (g);

        g.saveState();
        g.reduceClipRegion (plotArea.toNearestInt());
        if (proc.monitor->getIndex() != 0 && proc.heatmap->get())
            paintHeat (g);
        paintGrid (g);
        if (proc.monitor->getIndex() != 0)
            paintSpectrum (g, sr);

        juce::dsp::IIR::Coefficients<float>::Ptr coeffs[ParamEQProcessor::numBands][ParamEQProcessor::maxStages];
        int   nStages[ParamEQProcessor::numBands] = {};
        bool  active[ParamEQProcessor::numBands]  = {};
        for (int i = 0; i < ParamEQProcessor::numBands; ++i)
        {
            const int type = proc.band[i].type->getIndex();
            active[i] = (type != BT::Off);
            if (active[i])
                nStages[i] = buildUiCoeffs (sr, type, proc.band[i].slope->getIndex(), proc.band[i].gentle->get(),
                                            proc.band[i].freq->get(), proc.band[i].gain->get(),
                                            proc.band[i].bw->get(), coeffs[i]);
        }

        const float masterLin = juce::Decibels::decibelsToGain (proc.master->get());
        auto bandMag = [&] (int i, float f) -> float
        {
            float m = 1.0f;
            for (int st = 0; st < nStages[i]; ++st)
                m *= (float) coeffs[i][st]->getMagnitudeForFrequency (f, sr);
            return m;
        };

        const int hot = dragBand >= 0 ? dragBand : (hoverBand >= 0 ? hoverBand : selectedBand);
        const float x0 = plotArea.getX(), x1 = plotArea.getRight();
        const float zeroY = dbToY (0.0f);

        auto tracePath = [&] (auto&& magAt)
        {
            juce::Path path;
            for (float x = x0; x <= x1; x += 1.0f)
            {
                const float y = dbToY (juce::Decibels::gainToDecibels (juce::jmax (1.0e-4f, (float) magAt (xToFreq (x)))));
                if (x == x0) path.startNewSubPath (x, y);
                else         path.lineTo (x, y);
            }
            return path;
        };

        auto closedTo = [&] (const juce::Path& curve, float baseY)
        {
            juce::Path area (curve);
            area.lineTo (x1, baseY);
            area.lineTo (x0, baseY);
            area.closeSubPath();
            return area;
        };

        if (hot >= 0 && active[hot])
        {
            const auto bp = tracePath ([&] (float f) { return bandMag (hot, f); });
            const auto col = bandColour (hot);
            g.setColour (col.withAlpha (0.16f));
            g.fillPath (closedTo (bp, zeroY));
            g.setColour (col.withAlpha (0.55f));
            g.strokePath (bp, juce::PathStrokeType (1.2f));
        }

        const auto stroke = tracePath ([&] (float f)
        {
            float m = masterLin;
            for (int i = 0; i < ParamEQProcessor::numBands; ++i)
                if (active[i]) m *= bandMag (i, f);
            return m;
        });

        g.setGradientFill (juce::ColourGradient (Colors::eqCurve.withAlpha (0.08f), 0.0f, plotArea.getY(),
                                                 Colors::eqCurve.withAlpha (0.01f), 0.0f, plotArea.getBottom(), false));
        g.fillPath (closedTo (stroke, plotArea.getBottom()));

        g.setColour (Colors::eqCurve.withAlpha (0.10f));
        g.strokePath (stroke, juce::PathStrokeType (6.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (Colors::eqCurve.withAlpha (0.22f));
        g.strokePath (stroke, juce::PathStrokeType (3.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (Colors::eqCurve);
        g.strokePath (stroke, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        for (int i = 0; i < ParamEQProcessor::numBands; ++i)
        {
            if (! active[i]) continue;
            const auto pos = handlePos (i);
            const bool isHot = (i == hot);
            const float rad = isHot ? 10.0f : 8.5f;
            juce::Rectangle<float> circle (pos.x - rad, pos.y - rad, rad * 2.0f, rad * 2.0f);
            const auto col = bandColour (i);
            if (isHot)
            {
                g.setColour (col.withAlpha (0.25f));
                g.fillEllipse (circle.expanded (5.0f));
            }
            g.setColour (Colors::eqGraphBg.darker (0.5f).withAlpha (isHot ? 0.95f : 0.8f));
            g.fillEllipse (circle);
            g.setColour (isHot ? col : col.withAlpha (0.85f));
            g.drawEllipse (circle.reduced (0.5f), isHot ? 2.2f : 1.6f);
            g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
            g.drawText (juce::String (i + 1), circle, juce::Justification::centred, false);
        }
        g.restoreState();

        paintFooterAndScale (g);

        if (hot >= 0 && active[hot] && (dragBand >= 0 || hoverBand >= 0))
            paintReadout (g, hot);
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int b = bandAt (e.position);
        if (b != hoverBand) { hoverBand = b; repaint(); }
        setMouseCursor (hoverBand >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (hoverBand != -1) { hoverBand = -1; repaint(); }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int b = bandAt (e.position);
        if (e.mods.isPopupMenu())
        {
            if (b >= 0 && onBandMenu) onBandMenu (b, e.getScreenPosition());
            return;
        }
        dragBand = b;
        if (dragBand < 0) return;
        selectedBand = b;
        if (onBandSelected) onBandSelected (b);

        dragMode = e.mods.isCtrlDown() ? DragMode::bandwidth : DragMode::normal;
        startFreq = proc.band[b].freq->get();
        startGain = proc.band[b].gain->get();
        startBw   = proc.band[b].bw->get();
        proc.band[b].freq->beginChangeGesture();
        proc.band[b].gain->beginChangeGesture();
        proc.band[b].bw->beginChangeGesture();
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragBand < 0) return;
        const int b = dragBand;
        const float fine = e.mods.isShiftDown() ? 0.25f : 1.0f;
        const auto delta = e.getOffsetFromDragStart().toFloat() * fine;
        const auto pa = plot().toFloat();

        if (dragMode == DragMode::bandwidth)
        {
            const float t = (float) (-delta.y) / juce::jmax (1.0f, pa.getHeight()) * (BT::maxBw - BT::minBw) * 1.5f;
            setFloatP (proc.band[b].bw, juce::jlimit (BT::minBw, BT::maxBw, startBw + t));
        }
        else
        {
            if (! e.mods.isAltDown())
            {
                const float x = freqToX (startFreq) + delta.x;
                setFloatP (proc.band[b].freq, juce::jlimit (BT::minFreq, BT::maxFreq, xToFreq (x)));
            }
            if (verticalIsBw (proc.band[b].type->getIndex()))
            {
                const float t = (float) (-delta.y) / juce::jmax (1.0f, pa.getHeight()) * (BT::maxBw - BT::minBw);
                setFloatP (proc.band[b].bw, juce::jlimit (BT::minBw, BT::maxBw, startBw + t));
            }
            else
            {
                const float y = dbToY (startGain) + delta.y;
                setFloatP (proc.band[b].gain, juce::jlimit (BT::minGainDb, BT::maxGainDb, yToDb (y)));
            }
        }
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragBand < 0) return;
        proc.band[dragBand].freq->endChangeGesture();
        proc.band[dragBand].gain->endChangeGesture();
        proc.band[dragBand].bw->endChangeGesture();
        dragBand = -1;
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int b = bandAt (e.position);
        if (b < 0) return;
        proc.band[b].gain->beginChangeGesture();
        setFloatP (proc.band[b].gain, 0.0f);
        proc.band[b].gain->endChangeGesture();
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        const int b = dragBand >= 0 ? dragBand : bandAt (e.position);
        if (b < 0) { Component::mouseWheelMove (e, wheel); return; }
        const float step = (wheel.deltaY > 0 ? 0.12f : -0.12f) * (e.mods.isShiftDown() ? 0.25f : 1.0f);
        auto* bwp = proc.band[b].bw;
        bwp->beginChangeGesture();
        setFloatP (bwp, juce::jlimit (BT::minBw, BT::maxBw, bwp->get() + step));
        bwp->endChangeGesture();
        repaint();
    }

private:
    enum class DragMode { normal, bandwidth };
    static constexpr int fftSize = 8192;
    static constexpr int heatRes = 2048;

    juce::Rectangle<int> plot() const
    {
        return getLocalBounds().withTrimmedTop (headerH).withTrimmedRight (scaleW).withTrimmedBottom (footerH).withTrimmedLeft (4);
    }

    float xToFreq (float x) const
    {
        const auto p = plot().toFloat();
        const float t = juce::jlimit (0.0f, 1.0f, (x - p.getX()) / juce::jmax (1.0f, p.getWidth()));
        return BT::minFreq * std::pow (BT::maxFreq / BT::minFreq, t);
    }
    float freqToX (float f) const
    {
        const auto p = plot().toFloat();
        return p.getX() + std::log (juce::jlimit (BT::minFreq, BT::maxFreq, f) / BT::minFreq)
                          / std::log (BT::maxFreq / BT::minFreq) * p.getWidth();
    }
    float dbToY (float db) const
    {
        const auto p = plot().toFloat();
        const float t = juce::jlimit (0.0f, 1.0f, (db - BT::minGainDb) / (BT::maxGainDb - BT::minGainDb));
        return p.getY() + (1.0f - t) * p.getHeight();
    }
    float yToDb (float y) const
    {
        const auto p = plot().toFloat();
        const float t = 1.0f - juce::jlimit (0.0f, 1.0f, (y - p.getY()) / juce::jmax (1.0f, p.getHeight()));
        return BT::minGainDb + t * (BT::maxGainDb - BT::minGainDb);
    }
    float specToY (float db) const
    {
        const auto p = plot().toFloat();
        const float t = juce::jlimit (0.0f, 1.0f, (db + 90.0f) / 96.0f);
        return p.getY() + (1.0f - t) * p.getHeight();
    }

    juce::Point<float> handlePos (int i) const
    {
        const int type = proc.band[i].type->getIndex();
        const float x = freqToX (proc.band[i].freq->get());
        if (verticalIsBw (type))
        {
            const float t = (proc.band[i].bw->get() - BT::minBw) / (BT::maxBw - BT::minBw);
            return { x, dbToY (BT::minGainDb + t * (BT::maxGainDb - BT::minGainDb)) };
        }
        return { x, dbToY (proc.band[i].gain->get()) };
    }

    int bandAt (juce::Point<float> p) const
    {
        int best = -1;
        float bestD = 16.0f;
        for (int i = 0; i < ParamEQProcessor::numBands; ++i)
        {
            if (proc.band[i].type->getIndex() == BT::Off) continue;
            const float d = handlePos (i).getDistanceFrom (p);
            if (d < bestD) { bestD = d; best = i; }
        }
        return best;
    }

    void paintCell (juce::Graphics& g, juce::Rectangle<float> cell, const juce::String& text, juce::Justification just)
    {
        if (cell.getWidth() <= 0.0f) return;
        g.setColour (Colors::eqGraphGrid.withMultipliedSaturation (0.35f).brighter (0.12f));
        g.fillRect (cell);
        g.setColour (Colors::eqCurve.withAlpha (0.62f));
        g.setFont (juce::Font (juce::FontOptions (9.5f)));
        g.drawText (text, cell.withTrimmedLeft (just == juce::Justification::centredLeft ? 4.0f : 0.0f), just, false);
    }

    juce::Rectangle<float> cellFor (juce::Rectangle<float> row, float lo, float hi) const
    {
        const auto p = plot().toFloat();
        const float xa = juce::jmax (p.getX(), freqToX (lo));
        const float xb = juce::jmin (p.getRight(), freqToX (hi));
        const float left  = xa <= p.getX() ? xa : xa + 1.0f;
        const float right = xb >= p.getRight() ? xb : xb - 1.0f;
        return { left, row.getY(), juce::jmax (0.0f, right - left), row.getHeight() };
    }

    void paintHeader (juce::Graphics& g)
    {
        const auto p = plot().toFloat();
        auto hdr = juce::Rectangle<float> (p.getX(), 2.0f, p.getWidth(), p.getY() - 4.0f);
        const auto row1 = hdr.removeFromTop (hdr.getHeight() * 0.5f).withTrimmedBottom (1.0f);
        const auto row2 = hdr.withTrimmedTop (1.0f);

        constexpr float c0 = 16.3516f;
        paintCell (g, cellFor (row1, BT::minFreq, c0 * 2.0f), {}, juce::Justification::centredLeft);
        for (int oct = 1; oct <= 10; ++oct)
        {
            const float lo = c0 * (float) (1 << oct);
            if (lo >= BT::maxFreq) break;
            paintCell (g, cellFor (row1, lo, juce::jmin (lo * 2.0f, BT::maxFreq)),
                       oct <= 9 ? "C" + juce::String (oct) : juce::String(), juce::Justification::centredLeft);
        }

        for (const auto& r : kRegions)
            paintCell (g, cellFor (row2, r.lo, r.hi), r.name, juce::Justification::centred);
    }

    void paintGrid (juce::Graphics& g)
    {
        const auto p = plot().toFloat();
        for (int decade = 10; decade <= 10000; decade *= 10)
            for (int m = 2; m <= 9; ++m)
            {
                const float f = (float) (decade * m);
                if (f < BT::minFreq || f > BT::maxFreq) continue;
                const float x = freqToX (f);
                const bool major = (m == 2 || m == 5);
                g.setColour (Colors::eqGraphGrid.withAlpha (major ? 0.9f : 0.45f));
                g.drawVerticalLine ((int) x, p.getY(), p.getBottom());
            }
        for (int f : { 100, 1000, 10000 })
        {
            g.setColour (Colors::eqGraphGrid.withAlpha (0.9f));
            g.drawVerticalLine ((int) freqToX ((float) f), p.getY(), p.getBottom());
        }

        for (int db = -18; db <= 18; db += 3)
        {
            const float y = dbToY ((float) db);
            if (db == 0)
                g.setColour (Colors::eqCurve.withAlpha (0.45f));
            else
                g.setColour (Colors::eqGraphGrid.withAlpha ((db % 6) == 0 ? 0.9f : 0.45f));
            g.drawHorizontalLine ((int) y, p.getX(), p.getRight());
        }
    }

    void paintFooterAndScale (juce::Graphics& g)
    {
        const auto p = plot().toFloat();
        const juce::Rectangle<float> row (p.getX(), p.getBottom() + 2.0f, p.getWidth(), (float) footerH - 4.0f);
        const float freqTicks[10] = { 20.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f, BT::maxFreq };
        const char* freqLabels[9] = { "20", "50", "100", "200", "500", "1k", "2k", "5k", "10k" };
        for (int i = 0; i < 9; ++i)
            paintCell (g, cellFor (row, freqTicks[i], freqTicks[i + 1]), freqLabels[i], juce::Justification::centredLeft);

        auto scale = getLocalBounds().removeFromRight (scaleW);
        for (int db = -18; db <= 18; db += 6)
        {
            const int y = (int) dbToY ((float) db);
            g.setColour (Colors::eqCurve.withAlpha (db == 0 ? 0.9f : 0.55f));
            g.drawText ((db > 0 ? "+" : "") + juce::String (db), scale.getX() + 2, y - 6, scaleW - 4, 12,
                        juce::Justification::centredLeft);
        }
    }

    void paintReadout (juce::Graphics& g, int b)
    {
        const int type = proc.band[b].type->getIndex();
        juce::String txt = kTypeNames[type] + "   " + freqText (proc.band[b].freq->get());
        if (! verticalIsBw (type))
            txt += "   " + dbText (proc.band[b].gain->get());
        txt += "   BW " + juce::String (proc.band[b].bw->get(), 2);
        if (proc.slopeOrder (b) != 0)
            txt += "   " + kOrderNames[proc.slopeOrder (b) + 3];

        g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
        const int w = (int) juce::TextLayout::getStringWidth (g.getCurrentFont(), txt) + 16;
        auto pos = handlePos (b);
        const auto pa = plot();
        int x = (int) pos.x + 16, y = (int) pos.y - 26;
        if (x + w > pa.getRight()) x = (int) pos.x - 16 - w;
        if (y < pa.getY()) y = (int) pos.y + 16;
        juce::Rectangle<int> box (x, y, w, 20);
        g.setColour (Colors::eqGraphBg.darker (0.6f).withAlpha (0.92f));
        g.fillRoundedRectangle (box.toFloat(), 4.0f);
        g.setColour (bandColour (b));
        g.drawRoundedRectangle (box.toFloat(), 4.0f, 1.0f);
        g.setColour (Colors::eqCurve);
        g.drawText (txt, box, juce::Justification::centred, false);
    }

    void updateHeat (double sr)
    {
        for (auto& h : heat)
            h *= 0.86f;

        const float logSpan = std::log (BT::maxFreq / BT::minFreq);
        for (int k = 3; k < fftSize / 2 - 3; ++k)
        {
            const float b = rawDb[k];
            if (b < -66.0f || b <= rawDb[k - 1] || b < rawDb[k + 1] || b < rawDb[k - 2] || b < rawDb[k + 2])
                continue;
            const float a = rawDb[k - 1], c = rawDb[k + 1];
            const float den = a - 2.0f * b + c;
            const float off = den < 0.0f ? juce::jlimit (-0.5f, 0.5f, 0.5f * (a - c) / den) : 0.0f;
            const float peakDb = b - 0.25f * (a - c) * off;
            const float freq = (float) (((float) k + off) * sr / fftSize);
            if (freq < BT::minFreq || freq > BT::maxFreq) continue;

            const int idx = juce::jlimit (0, heatRes - 1,
                                          (int) (std::log (freq / BT::minFreq) / logSpan * (float) (heatRes - 1) + 0.5f));
            heat[idx] = juce::jmax (heat[idx], juce::jlimit (0.0f, 1.0f, (peakDb + 66.0f) / 60.0f));
        }
    }

    void paintHeat (juce::Graphics& g)
    {
        const auto p = plot().toFloat();
        for (int i = 0; i < heatRes; ++i)
        {
            const float h = heat[i];
            if (h < 0.03f) continue;
            const float x = p.getX() + (float) i / (float) (heatRes - 1) * p.getWidth();
            const auto col = Colors::eqHeat.interpolatedWith (Colors::eqHeatHot, h * h);
            g.setColour (col.withAlpha (0.10f * h));
            g.fillRect (x - 2.5f, p.getY(), 5.0f, p.getHeight());
            g.setColour (col.withAlpha (juce::jmin (1.0f, 0.18f + 0.7f * h)));
            g.fillRect (x - 0.6f, p.getY(), 1.2f, p.getHeight());
        }
    }

    void paintSpectrum (juce::Graphics& g, double sr)
    {
        const auto p = plot();
        auto levelAt = [&] (float f, float& prevBp)
        {
            const float bp = juce::jlimit (1.0f, (float) (fftSize / 2 - 2), f * (float) fftSize / (float) sr);
            float db;
            if (bp - prevBp >= 2.0f)
            {
                db = -90.0f;
                for (int b2 = (int) prevBp + 1; b2 <= (int) bp; ++b2)
                    db = juce::jmax (db, specDb[b2]);
            }
            else
            {
                const int   b0 = (int) bp;
                const float t  = bp - (float) b0;
                db = specDb[b0] * (1.0f - t) + specDb[b0 + 1] * t;
            }
            prevBp = bp;
            return specToY (db);
        };

        juce::Path area;
        area.startNewSubPath ((float) p.getX(), (float) p.getBottom());
        float prevBp = 0.0f;
        for (int x = p.getX(); x <= p.getRight(); x += 2)
            area.lineTo ((float) x, levelAt (xToFreq ((float) x), prevBp));
        area.lineTo ((float) p.getRight(), (float) p.getBottom());
        area.closeSubPath();
        g.setColour (Colors::eqCurve.withAlpha (0.09f));
        g.fillPath (area);

        juce::Path top;
        prevBp = 0.0f;
        for (int x = p.getX(); x <= p.getRight(); x += 2)
        {
            const float y = levelAt (xToFreq ((float) x), prevBp);
            if (x == p.getX()) top.startNewSubPath ((float) x, y);
            else               top.lineTo ((float) x, y);
        }
        g.setColour (Colors::eqCurve.withAlpha (0.28f));
        g.strokePath (top, juce::PathStrokeType (1.0f));
    }

    ParamEQProcessor& proc;
    juce::dsp::FFT fft { 13 };
    juce::dsp::WindowingFunction<float> window { (size_t) fftSize, juce::dsp::WindowingFunction<float>::hann };
    float fftData[fftSize * 2] = { 0.0f };
    float specDb[fftSize / 2]  = { -90.0f };
    float rawDb[fftSize / 2]   = { -90.0f };
    float heat[heatRes]        = { 0.0f };

    int hoverBand = -1;
    int dragBand  = -1;
    DragMode dragMode = DragMode::normal;
    float startFreq = 0.0f, startGain = 0.0f, startBw = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamEQGraph)
};

class ParamEQFlatButton : public juce::Button
{
public:
    explicit ParamEQFlatButton (const juce::String& name) : juce::Button (name) {}

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto b = getLocalBounds().toFloat().reduced (0.5f);
        const bool on = getToggleState();
        g.setColour (on ? Colors::accent : (down || over ? Colors::eqPanelBg.brighter (0.35f) : Colors::eqPanelBg.brighter (0.15f)));
        g.fillRoundedRectangle (b, 4.0f);
        g.setColour (on ? Colors::accent.brighter (0.3f) : Colors::eqPanelBg.brighter (0.45f));
        g.drawRoundedRectangle (b, 4.0f, 1.0f);
        g.setColour (on ? Colors::contrastText (Colors::accent) : Colors::eqCurve.withAlpha (0.85f));
        g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
        g.drawText (getButtonText(), getLocalBounds(), juce::Justification::centred, false);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamEQFlatButton)
};

class ParamEQEditor : public juce::AudioProcessorEditor,
                      private juce::Timer,
                      private juce::AudioProcessorListener
{
public:
    explicit ParamEQEditor (ParamEQProcessor& p);
    ~ParamEQEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (presetArea.contains (e.getPosition()))
            showPresetMenu();
    }

private:
    void timerCallback() override;
    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override { curveDirty.store (true); }
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}

    void showPresetMenu();
    void applyPreset (int index);
    void showBandMenu (int band, juce::Rectangle<int> screenArea = {});
    void toggleAB();
    void captureSnapshot (std::vector<float>& dest);
    void applySnapshot (const std::vector<float>& src);
    void refreshMonitorButton();

    ParamEQProcessor& proc;

    ParamEQFlatButton prevBtn { "<" }, nextBtn { ">" }, abBtn { "A" };
    juce::Label presetName;
    int currentPreset = -1;

    std::unique_ptr<ParamEQGraph> graph;

    ParamEQFader masterSl;
    ParamEQFader gainSl[ParamEQProcessor::numBands];
    ParamEQKnob  freqK[ParamEQProcessor::numBands];
    ParamEQKnob  bwK[ParamEQProcessor::numBands];
    ParamEQBandTag tags[ParamEQProcessor::numBands];

    std::unique_ptr<juce::SliderParameterAttachment> masterAtt;
    std::unique_ptr<juce::SliderParameterAttachment> gainAtt[ParamEQProcessor::numBands];
    std::unique_ptr<juce::SliderParameterAttachment> freqAtt[ParamEQProcessor::numBands];
    std::unique_ptr<juce::SliderParameterAttachment> bwAtt[ParamEQProcessor::numBands];

    ParamEQFlatButton linBtn { "LIN" }, hqBtn { "HQ" }, heatBtn { "HEAT" }, monBtn { "OUT" };
    std::unique_ptr<juce::ButtonParameterAttachment> linAtt, hqAtt, heatAtt;
    std::unique_ptr<juce::ParameterAttachment> monAtt;

    int lastType[ParamEQProcessor::numBands];
    std::vector<float> snap[2];
    bool snapValid[2] { false, false };
    int  curSnap = 0;

    std::atomic<bool> curveDirty { true };

    juce::Rectangle<int> secRight, secBottom, presetArea;
    juce::Rectangle<int> freqLabelArea, bwLabelArea, mainLabelArea, monitorLabelArea, compareLabelArea;
    int tickTop = 0, tickHeight = 1, tickX = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamEQEditor)
};

ParamEQEditor::ParamEQEditor (ParamEQProcessor& p)
    : juce::AudioProcessorEditor (p), proc (p)
{
    prevBtn.onClick = [this] { applyPreset (currentPreset <= 0 ? (int) presets().size() - 1 : currentPreset - 1); };
    nextBtn.onClick = [this] { applyPreset (currentPreset < 0 ? 0 : (currentPreset + 1) % (int) presets().size()); };
    abBtn.onClick   = [this] { toggleAB(); };
    addAndMakeVisible (prevBtn);
    addAndMakeVisible (nextBtn);
    addAndMakeVisible (abBtn);

    presetName.setText ("Default", juce::dontSendNotification);
    presetName.setJustificationType (juce::Justification::centred);
    presetName.setFont (juce::Font (juce::FontOptions (11.5f, juce::Font::bold)));
    presetName.setColour (juce::Label::textColourId, Colors::eqCurve);
    presetName.setColour (juce::Label::backgroundColourId, Colors::eqPanelBg.darker (0.5f));
    presetName.setMouseCursor (juce::MouseCursor::PointingHandCursor);
    presetName.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (presetName);

    graph = std::make_unique<ParamEQGraph> (proc);
    graph->onBandMenu = [this] (int b, juce::Point<int> pt) { showBandMenu (b, { pt.x, pt.y, 1, 1 }); };
    graph->onBandSelected = [this] (int) { for (auto& t : tags) t.repaint(); };
    addAndMakeVisible (*graph);

    masterSl.colour = Colors::eqCurve;
    masterSl.setDoubleClickReturnValue (true, 0.0);
    addAndMakeVisible (masterSl);
    masterAtt = std::make_unique<juce::SliderParameterAttachment> (*proc.master, masterSl);

    for (int i = 0; i < ParamEQProcessor::numBands; ++i)
    {
        const auto col = bandColour (i);

        gainSl[i].colour = col;
        gainSl[i].setDoubleClickReturnValue (true, 0.0);
        addAndMakeVisible (gainSl[i]);
        gainAtt[i] = std::make_unique<juce::SliderParameterAttachment> (*proc.band[i].gain, gainSl[i]);

        freqK[i].colour = col;
        bwK[i].colour   = col;
        freqK[i].setDoubleClickReturnValue (true, ParamEQProcessor::defaultFreq[i]);
        bwK[i].setDoubleClickReturnValue (true, ParamEQProcessor::defaultBw[i]);
        addAndMakeVisible (freqK[i]);
        addAndMakeVisible (bwK[i]);
        freqAtt[i] = std::make_unique<juce::SliderParameterAttachment> (*proc.band[i].freq, freqK[i]);
        bwAtt[i]   = std::make_unique<juce::SliderParameterAttachment> (*proc.band[i].bw, bwK[i]);

        lastType[i] = proc.band[i].type->getIndex() != ParamEQProcessor::Off
                    ? proc.band[i].type->getIndex() : ParamEQProcessor::defaultType[i];

        tags[i].index      = i;
        tags[i].getType    = [this, i] { return proc.band[i].type->getIndex(); };
        tags[i].isSelected = [this, i] { return graph != nullptr && graph->selectedBand == i; };
        tags[i].onMenu     = [this, i] { showBandMenu (i); };
        tags[i].onToggle   = [this, i]
        {
            const int cur = proc.band[i].type->getIndex();
            if (cur != ParamEQProcessor::Off) { lastType[i] = cur; setChoiceP (proc.band[i].type, ParamEQProcessor::Off); }
            else                              { setChoiceP (proc.band[i].type, lastType[i]); }
        };
        addAndMakeVisible (tags[i]);
    }

    linBtn.setClickingTogglesState (true);
    hqBtn.setClickingTogglesState (true);
    addAndMakeVisible (linBtn);
    addAndMakeVisible (hqBtn);
    linAtt = std::make_unique<juce::ButtonParameterAttachment> (*proc.linphase, linBtn);
    hqAtt  = std::make_unique<juce::ButtonParameterAttachment> (*proc.hq, hqBtn);
    heatBtn.setClickingTogglesState (true);
    heatBtn.setTooltip ("Frequency heatmap (needs MONITOR IN or OUT)");
    addAndMakeVisible (heatBtn);
    heatAtt = std::make_unique<juce::ButtonParameterAttachment> (*proc.heatmap, heatBtn);

    monBtn.onClick = [this] { setChoiceP (proc.monitor, (proc.monitor->getIndex() + 1) % 3); };
    addAndMakeVisible (monBtn);
    monAtt = std::make_unique<juce::ParameterAttachment> (*proc.monitor, [this] (float) { refreshMonitorButton(); });
    monAtt->sendInitialUpdate();

    proc.addListener (this);
    setSize (900, 540);
    startTimerHz (30);
}

ParamEQEditor::~ParamEQEditor()
{
    stopTimer();
    proc.removeListener (this);
}

void ParamEQEditor::refreshMonitorButton()
{
    static const char* names[3] = { "OFF", "IN", "OUT" };
    const int idx = proc.monitor->getIndex();
    monBtn.setButtonText (names[juce::jlimit (0, 2, idx)]);
    monBtn.setToggleState (idx != 0, juce::dontSendNotification);
}

void ParamEQEditor::timerCallback()
{
    if (graph == nullptr) return;

    bool needRepaint = false;
    if (proc.monitor->getIndex() != 0)
    {
        graph->updateSpectrum();
        needRepaint = true;
    }
    if (curveDirty.exchange (false))
    {
        needRepaint = true;
        for (auto& t : tags) t.repaint();
    }
    if (needRepaint)
        graph->repaint();
}

void ParamEQEditor::showPresetMenu()
{
    juce::PopupMenu m;
    const auto& list = presets();
    for (int i = 0; i < (int) list.size(); ++i)
        m.addItem (i + 1, list[(size_t) i].name, true, i == currentPreset);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (localAreaToGlobal (presetArea)),
        [safe = juce::Component::SafePointer<ParamEQEditor> (this)] (int r)
        {
            if (safe != nullptr && r > 0)
                safe->applyPreset (r - 1);
        });
}

void ParamEQEditor::applyPreset (int index)
{
    const auto& list = presets();
    if (index < 0 || index >= (int) list.size()) return;
    const auto& ps = list[(size_t) index];

    for (int i = 0; i < ParamEQProcessor::numBands; ++i)
    {
        setChoiceP (proc.band[i].type, ParamEQProcessor::Off);
        setFloatP  (proc.band[i].freq, ParamEQProcessor::defaultFreq[i]);
        setFloatP  (proc.band[i].gain, 0.0f);
        setFloatP  (proc.band[i].bw, ParamEQProcessor::defaultBw[i]);
        proc.setSlopeOrder (i, 0);
        setChoiceP (proc.band[i].chan, 0);
    }
    for (const auto& b : ps.bands)
    {
        setChoiceP (proc.band[b.band].type, b.type);
        setFloatP  (proc.band[b.band].freq, b.freq);
        setFloatP  (proc.band[b.band].gain, b.gain);
        setFloatP  (proc.band[b.band].bw, b.bw);
        proc.setSlopeOrder (b.band, b.order);
        lastType[b.band] = b.type;
    }
    setFloatP (proc.master, ps.master);

    currentPreset = index;
    presetName.setText (ps.name, juce::dontSendNotification);
}

void ParamEQEditor::showBandMenu (int band, juce::Rectangle<int> screenArea)
{
    juce::PopupMenu typeM, orderM, chanM;
    const int curType  = proc.band[band].type->getIndex();
    const int curOrder = proc.slopeOrder (band);
    const int curChan  = proc.band[band].chan->getIndex();

    for (int i = 0; i < kTypeNames.size(); ++i)
        typeM.addItem (100 + i, kTypeNames[i], true, i == curType);
    for (int i = 0; i < kOrderNames.size(); ++i)
        orderM.addItem (200 + i, kOrderNames[i], true, (i - 3) == curOrder);
    for (int i = 0; i < kChanNames.size(); ++i)
        chanM.addItem (300 + i, kChanNames[i], true, i == curChan);

    juce::PopupMenu m;
    m.addSectionHeader ("Band " + juce::String (band + 1));
    m.addSubMenu ("Type", typeM);
    m.addSubMenu ("Order", orderM);
    m.addSubMenu ("Channel", chanM);
    m.addSeparator();
    m.addItem (1, "Reset band");

    auto opts = screenArea.isEmpty()
        ? juce::PopupMenu::Options().withTargetComponent (&tags[band])
        : juce::PopupMenu::Options().withTargetScreenArea (screenArea);
    m.showMenuAsync (opts,
        [safe = juce::Component::SafePointer<ParamEQEditor> (this), band] (int r)
        {
            if (safe == nullptr || r == 0) return;
            auto& proc = safe->proc;
            if (r == 1)
            {
                setFloatP  (proc.band[band].freq, ParamEQProcessor::defaultFreq[band]);
                setFloatP  (proc.band[band].gain, 0.0f);
                setFloatP  (proc.band[band].bw, ParamEQProcessor::defaultBw[band]);
                proc.setSlopeOrder (band, 0);
                setChoiceP (proc.band[band].chan, 0);
                setChoiceP (proc.band[band].type, ParamEQProcessor::defaultType[band]);
                safe->lastType[band] = ParamEQProcessor::defaultType[band];
            }
            else if (r >= 100 && r < 200)
            {
                setChoiceP (proc.band[band].type, r - 100);
                if (r - 100 != ParamEQProcessor::Off) safe->lastType[band] = r - 100;
            }
            else if (r >= 200 && r < 300) proc.setSlopeOrder (band, r - 200 - 3);
            else if (r >= 300 && r < 400) setChoiceP (proc.band[band].chan, r - 300);
        });
}

void ParamEQEditor::captureSnapshot (std::vector<float>& dest)
{
    const auto& params = proc.getParameters();
    dest.resize ((size_t) params.size());
    for (int i = 0; i < params.size(); ++i)
        dest[(size_t) i] = params[i]->getValue();
}

void ParamEQEditor::applySnapshot (const std::vector<float>& src)
{
    const auto& params = proc.getParameters();
    for (int i = 0; i < params.size() && i < (int) src.size(); ++i)
        params[i]->setValueNotifyingHost (src[(size_t) i]);
}

void ParamEQEditor::toggleAB()
{
    captureSnapshot (snap[curSnap]);
    snapValid[curSnap] = true;
    const int other = curSnap ^ 1;
    if (! snapValid[other]) { snap[other] = snap[curSnap]; snapValid[other] = true; }
    curSnap = other;
    applySnapshot (snap[curSnap]);
    abBtn.setButtonText (curSnap == 0 ? "A" : "B");
    currentPreset = -1;
}

void ParamEQEditor::paint (juce::Graphics& g)
{
    g.fillAll (Colors::eqPanelBg);

    g.setColour (Colors::eqPanelBg.darker (0.25f));
    g.fillRoundedRectangle (secRight.toFloat(), 4.0f);
    g.setColour (Colors::eqPanelBg.brighter (0.2f));
    g.drawRoundedRectangle (secRight.toFloat(), 4.0f, 1.0f);

    g.setColour (Colors::eqCurve.withAlpha (0.55f));
    g.setFont (juce::Font (juce::FontOptions (9.0f)));
    for (int db = -18; db <= 18; db += 6)
    {
        const float t = (float) (db + 18) / 36.0f;
        const int y = tickTop + (int) ((1.0f - t) * (float) tickHeight);
        g.drawText ((db > 0 ? "+" : "") + juce::String (db), tickX, y - 6, 22, 12, juce::Justification::centred);
        g.fillRect (tickX + 22, y, 3, 1);
    }

    g.setColour (Colors::eqCurve.withAlpha (0.65f));
    g.setFont (juce::Font (juce::FontOptions (9.5f, juce::Font::bold)));
    g.drawText ("FREQ", freqLabelArea, juce::Justification::centred, false);
    g.drawText ("BW",   bwLabelArea,   juce::Justification::centred, false);
    g.drawText ("MAIN", mainLabelArea, juce::Justification::centred, false);
    g.drawText ("MONITOR", monitorLabelArea, juce::Justification::centred, false);
    g.drawText ("COMPARE", compareLabelArea, juce::Justification::centred, false);

    if (proc.linphase->get())
    {
        g.setColour (Colors::eqCurve.withAlpha (0.5f));
        g.setFont (juce::Font (juce::FontOptions (9.5f)));
        g.drawText ("LIN +46 ms", linBtn.getBounds().translated (0, -14).withHeight (12).withWidth (80),
                    juce::Justification::centredLeft, false);
    }
}

void ParamEQEditor::resized()
{
    auto full = getLocalBounds().reduced (8);

    secBottom = full.removeFromBottom (30);
    full.removeFromBottom (6);

    {
        auto c = secBottom;
        c.removeFromLeft (130);
        const int btnW = 44, btnH = 24;
        auto right = c.removeFromRight (btnW * 2 + 10);
        compareLabelArea = right.removeFromLeft (btnW + 4).withTrimmedTop (18);
        abBtn.setBounds (juce::Rectangle<int> (compareLabelArea.getX() + 4, secBottom.getY() - 2, btnW - 8, 18));
        compareLabelArea = compareLabelArea.withY (secBottom.getY() + 16).withHeight (12);
        auto monA = right.removeFromLeft (btnW + 4);
        monBtn.setBounds (juce::Rectangle<int> (monA.getX() + 4, secBottom.getY() - 2, btnW - 8, 18));
        monitorLabelArea = monA.withY (secBottom.getY() + 16).withHeight (12);
        c.removeFromRight (12);
        hqBtn.setBounds (c.removeFromRight (btnW).withSizeKeepingCentre (btnW - 6, btnH));
        c.removeFromRight (4);
        heatBtn.setBounds (c.removeFromRight (btnW + 6).withSizeKeepingCentre (btnW, btnH));
        c.removeFromRight (4);
        linBtn.setBounds (c.removeFromRight (btnW).withSizeKeepingCentre (btnW - 6, btnH));
        c.removeFromRight (16);

        auto pa = c.withSizeKeepingCentre (juce::jmin (c.getWidth(), 260), btnH);
        prevBtn.setBounds (pa.removeFromLeft (24));
        nextBtn.setBounds (pa.removeFromRight (24));
        presetArea = pa.reduced (4, 0);
        presetName.setBounds (presetArea);
    }

    secRight = full.removeFromRight ((int) (full.getWidth() * 0.30f));
    full.removeFromRight (8);
    if (graph) graph->setBounds (full);

    {
        auto panel = secRight.reduced (6, 6);

        auto bwRow = panel.removeFromBottom (32);
        auto freqRow = panel.removeFromBottom (32);
        panel.removeFromBottom (6);

        auto layoutKnobRow = [] (juce::Rectangle<int> row, juce::Rectangle<int>& labelArea,
                                 ParamEQKnob* knobs)
        {
            labelArea = row.removeFromLeft (34);
            const int kw = row.getWidth() / ParamEQProcessor::numBands;
            for (int i = 0; i < ParamEQProcessor::numBands; ++i)
                knobs[i].setBounds (row.removeFromLeft (kw).reduced (1));
        };
        layoutKnobRow (freqRow, freqLabelArea, freqK);
        layoutKnobRow (bwRow,   bwLabelArea,   bwK);

        const int headH = 18, headGap = 4, sliderInsetY = 2;
        auto masterCol = panel.removeFromLeft (34);
        mainLabelArea = masterCol.removeFromTop (headH);
        masterCol.removeFromTop (headGap);
        masterSl.setBounds (masterCol.reduced (0, sliderInsetY));

        auto scaleCol = panel.removeFromLeft (26);
        tickX = scaleCol.getX();
        tickTop = (int) masterSl.getPositionOfValue (masterSl.getMaximum()) + masterSl.getY();
        tickHeight = (int) (masterSl.getPositionOfValue (masterSl.getMinimum()) - masterSl.getPositionOfValue (masterSl.getMaximum()));

        const int colW = panel.getWidth() / ParamEQProcessor::numBands;
        for (int i = 0; i < ParamEQProcessor::numBands; ++i)
        {
            auto col = panel.removeFromLeft (colW);
            tags[i].setBounds (col.removeFromTop (headH).reduced (1, 0));
            col.removeFromTop (headGap);
            gainSl[i].setBounds (col.reduced (0, sliderInsetY));
        }
    }
}

void ParamEQProcessor::designKernel()
{
    const double rate = sr > 0.0 ? sr : 44100.0;
    const int N = firFftSize;

    for (int k = 0; k <= N / 2; ++k)
    {
        const double f = juce::jmax (1.0, (double) k * rate / (double) N);
        float mag = 1.0f;
        for (int i = 0; i < numBands; ++i)
        {
            const int type = band[i].type->getIndex();
            if (type == Off) continue;
            juce::dsp::IIR::Coefficients<float>::Ptr c[maxStages];
            const int ns = buildUiCoeffs (rate, type, band[i].slope->getIndex(), band[i].gentle->get(),
                                          band[i].freq->get(), band[i].gain->get(),
                                          band[i].bw->get(), c);
            for (int st = 0; st < ns; ++st)
                mag *= (float) c[st]->getMagnitudeForFrequency (f, rate);
        }
        firSpectrum[(size_t) k] = { mag, 0.0f };
    }
    for (int k = 1; k < N / 2; ++k)
        firSpectrum[(size_t) (N - k)] = firSpectrum[(size_t) k];

    firFft.perform (firSpectrum.data(), firTime.data(), true);

    juce::AudioBuffer<float> ir (1, firTaps);
    float* d = ir.getWritePointer (0);
    const double norm = 1.0 / (double) N;
    for (int t = 0; t < firTaps; ++t)
    {
        const int src = (N - firTaps / 2 + t) % N;
        const float w = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi
                                                * (float) t / (float) (firTaps - 1));
        d[t] = (float) (firTime[(size_t) src].real() * norm) * w;
    }

    double sum = 0.0;
    for (int t = 0; t < firTaps; ++t) sum += d[t];
    const float dcMag = firSpectrum[0].real();
    if (dcMag > 1.0e-4f && std::abs (sum) > 1.0e-9)
    {
        const float s = dcMag / (float) sum;
        for (int t = 0; t < firTaps; ++t) d[t] *= s;
    }

    convolution.loadImpulseResponse (std::move (ir), rate,
                                     juce::dsp::Convolution::Stereo::no,
                                     juce::dsp::Convolution::Trim::no,
                                     juce::dsp::Convolution::Normalise::no);
}

juce::AudioProcessorEditor* ParamEQProcessor::createEditor()
{
    return new ParamEQEditor (*this);
}
