#include "BuiltinLowLifter.h"

namespace
{
    juce::NormalisableRange<float> logRange (float lo, float hi, float centre)
    {
        auto r = juce::NormalisableRange<float> (lo, hi);
        r.setSkewForCentre (centre);
        return r;
    }

    const float kDiffMs[LowLifterProcessor::numDiff] = { 4.7f, 6.1f, 9.3f, 12.7f };

    struct LowLifterPreset
    {
        const char* name;
        float preGain, slope;
        bool  invert, multiBand, lookahead;
        float diffDepth, diffMix;
        float lowGain, cutoff, highGain;
        float lowCut, dryWet;
    };

    const LowLifterPreset kPresets[3] =
    {
        { "Default",      4.0f, 0.30f,false, false, false, 0.20f,0.25f, 1.5f,100.0f, 1.0f, 20.0f, 1.0f },
        { "Sub presence", 8.0f, 0.15f,false, true,  false, 0.15f,0.20f, 2.0f, 55.0f, 0.0f, 25.0f, 1.0f },
        { "Density",     12.0f, 0.50f,false, false, true,  0.60f,0.55f, 2.0f,120.0f, 1.5f, 18.0f, 1.0f }
    };
}

LowLifterProcessor::LowLifterProcessor()
{
    addParameter (preGain = new juce::AudioParameterFloat (
        juce::ParameterID { "preGain", 1 }, "Pre Gain",
        juce::NormalisableRange<float> (0.0f, 24.0f, 0.01f), 4.0f));
    addParameter (slope = new juce::AudioParameterFloat (
        juce::ParameterID { "slope", 1 }, "Slope",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.3f));
    addParameter (invertPolarity = new juce::AudioParameterBool (
        juce::ParameterID { "invert", 1 }, "Invert Polarity", false));
    addParameter (multiBand = new juce::AudioParameterBool (
        juce::ParameterID { "multiBand", 1 }, "Multi Band", false));
    addParameter (lookahead = new juce::AudioParameterBool (
        juce::ParameterID { "lookahead", 1 }, "Lookahead", false));
    addParameter (diffDepth = new juce::AudioParameterFloat (
        juce::ParameterID { "diffDepth", 1 }, "Diffusion Depth",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.2f));
    addParameter (diffMix = new juce::AudioParameterFloat (
        juce::ParameterID { "diffMix", 1 }, "Diffusion Mix",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.25f));
    addParameter (lowGain = new juce::AudioParameterFloat (
        juce::ParameterID { "lowGain", 1 }, "Low Gain",
        juce::NormalisableRange<float> (-12.0f, 12.0f, 0.01f), 1.5f));
    addParameter (cutoff = new juce::AudioParameterFloat (
        juce::ParameterID { "cutoff", 1 }, "Cutoff",
        logRange (30.0f, 500.0f, 100.0f), 100.0f));
    addParameter (highGain = new juce::AudioParameterFloat (
        juce::ParameterID { "highGain", 1 }, "High Gain",
        juce::NormalisableRange<float> (-12.0f, 12.0f, 0.01f), 1.0f));
    addParameter (lowCut = new juce::AudioParameterFloat (
        juce::ParameterID { "lowCut", 1 }, "Low Cut",
        logRange (10.0f, 120.0f, 40.0f), 20.0f));
    addParameter (dryWet = new juce::AudioParameterFloat (
        juce::ParameterID { "dryWet", 1 }, "Dry/Wet",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 1.0f));
}

void LowLifterProcessor::prepareToPlay (double sampleRate, int blockSize)
{
    sr = sampleRate > 0 ? sampleRate : 44100.0;
    const int bs = juce::jmax (1, blockSize);

    juce::dsp::ProcessSpec spec { sr, (juce::uint32) bs, 2 };
    crossover.prepare (spec); crossover.reset();
    subSplit.prepare (spec);  subSplit.reset();
    outputHP.prepare (spec);  outputHP.reset();
    outputHP.setType (juce::dsp::StateVariableTPTFilterType::highpass);

    for (int ch = 0; ch < 2; ++ch)
    {
        for (int j = 0; j < numDiff; ++j)
            diffAp[ch][j].prepare ((int) (kDiffMs[j] * 0.001 * sr));
        laEnv[ch] = 0.0f;
    }

    const int maxLa = (int) std::ceil (0.003 * sr) + bs;
    for (int ch = 0; ch < 2; ++ch)
    {
        lowDelay[ch].prepare (maxLa);  highDelay[ch].prepare (maxLa);
        harmDelay[ch].prepare (maxLa); dryDelay[ch].prepare (maxLa);
    }

    lookaheadSamples = lookahead->get() ? (int) std::round (0.0015 * sr) : 0;
    reportedLatency  = lookaheadSamples;
    setLatencySamples (lookaheadSamples);

    preSm.reset (sr, 0.02); lowGainSm.reset (sr, 0.02);
    highGainSm.reset (sr, 0.02); dryWetSm.reset (sr, 0.02);
    preSm.setCurrentAndTargetValue      (juce::Decibels::decibelsToGain (preGain->get()));
    lowGainSm.setCurrentAndTargetValue  (juce::Decibels::decibelsToGain (lowGain->get()));
    highGainSm.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (highGain->get()));
    dryWetSm.setCurrentAndTargetValue   (dryWet->get());
}

void LowLifterProcessor::reset()
{
    crossover.reset();
    subSplit.reset();
    outputHP.reset();

    for (int ch = 0; ch < 2; ++ch)
    {
        for (int j = 0; j < numDiff; ++j)
            diffAp[ch][j].reset();
        lowDelay[ch].reset();  highDelay[ch].reset();
        harmDelay[ch].reset(); dryDelay[ch].reset();
        laEnv[ch] = 0.0f;
    }

    preSm.setCurrentAndTargetValue      (preSm.getTargetValue());
    lowGainSm.setCurrentAndTargetValue  (lowGainSm.getTargetValue());
    highGainSm.setCurrentAndTargetValue (highGainSm.getTargetValue());
    dryWetSm.setCurrentAndTargetValue   (dryWetSm.getTargetValue());
}

void LowLifterProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int n   = buffer.getNumSamples();
    const int nch = buffer.getNumChannels();
    if (n == 0 || nch < 1 || sr <= 0.0)
        return;

    const bool mb  = multiBand->get();
    const bool la  = lookahead->get();
    const float polarity = invertPolarity->get() ? -1.0f : 1.0f;

    const float fc = juce::jlimit (30.0f, 500.0f, cutoff->get());
    crossover.setCutoffFrequency (fc);
    subSplit.setCutoffFrequency (juce::jmax (20.0f, fc * 0.5f));
    outputHP.setCutoffFrequency (juce::jlimit (10.0f, 120.0f, lowCut->get()));

    lookaheadSamples = la ? (int) std::round (0.0015 * sr) : 0;
    lookaheadSamples = juce::jlimit (0, (int) lowDelay[0].buf.size() - 2, lookaheadSamples);
    if (reportedLatency != lookaheadSamples)
    {
        reportedLatency = lookaheadSamples;
        setLatencySamples (lookaheadSamples);
    }

    preSm.setTargetValue      (juce::Decibels::decibelsToGain (preGain->get()));
    lowGainSm.setTargetValue  (juce::Decibels::decibelsToGain (lowGain->get()));
    highGainSm.setTargetValue (juce::Decibels::decibelsToGain (highGain->get()));
    dryWetSm.setTargetValue   (dryWet->get());

    const float bias    = slope->get() * 0.9f;
    const float tanhB   = std::tanh (bias);
    const float sechSq  = 1.0f / (std::cosh (bias) * std::cosh (bias));
    const float depthG  = 0.55f * diffDepth->get();
    const float dMix    = diffMix->get();

    const float atkCoef = 1.0f - std::exp (-1.0f / juce::jmax (1.0f, (float) lookaheadSamples));
    const float relCoef = 1.0f - std::exp (-1.0f / (0.03f * (float) sr));

    const int nc = juce::jmin (nch, 2);
    float* ch0 = buffer.getWritePointer (0);
    float* ch1 = nch > 1 ? buffer.getWritePointer (1) : nullptr;

    double lowSq = 0.0, highSq = 0.0, harmSq = 0.0;

    for (int s = 0; s < n; ++s)
    {
        const float pre    = preSm.getNextValue();
        const float invPre = 1.0f / juce::jmax (1.0e-6f, pre);
        const float lg     = lowGainSm.getNextValue();
        const float hg     = highGainSm.getNextValue();
        const float mix    = dryWetSm.getNextValue();

        for (int ch = 0; ch < nc; ++ch)
        {
            float* d = (ch == 0) ? ch0 : ch1;
            const float in = d[s];

            float low = 0.0f, high = 0.0f;
            crossover.processSample (ch, in, low, high);
            float subLow = 0.0f, subHigh = 0.0f;
            subSplit.processSample (ch, low, subLow, subHigh);
            const float harmSource = mb ? subLow : low;

            const float xd     = harmSource * pre;
            const float shaped = std::tanh (xd + bias) - tanhB;
            const float sat    = shaped * invPre / sechSq;
            const float harm   = (sat - harmSource) * polarity;

            float lowClean = low, highClean = high, dryS = in, harmUse = harm;
            if (la)
            {
                const float rect = std::abs (harmSource) * pre;
                laEnv[ch] += (rect > laEnv[ch] ? atkCoef : relCoef) * (rect - laEnv[ch]);
                const float tg = 1.0f / (1.0f + laEnv[ch] * 1.5f);

                lowClean  = lowDelay[ch].process  (low,  lookaheadSamples);
                highClean = highDelay[ch].process (high, lookaheadSamples);
                dryS      = dryDelay[ch].process  (in,   lookaheadSamples);
                harmUse   = harmDelay[ch].process (harm, lookaheadSamples) * tg;
            }
            else
            {
                lowDelay[ch].process  (low,  0);  highDelay[ch].process (high, 0);
                dryDelay[ch].process  (in,   0);  harmDelay[ch].process (harm, 0);
                laEnv[ch] = 0.0f;
            }

            const float lowEnh = lowClean + harmUse;

            float diffed = lowEnh;
            for (int j = 0; j < numDiff; ++j)
                diffed = diffAp[ch][j].process (diffed, depthG);
            const float lowDiff = lowEnh + dMix * (diffed - lowEnh);

            const float lowOut  = lowDiff  * lg;
            const float highOut = highClean * hg;
            const float wet = outputHP.processSample (ch, lowOut + highOut);

            d[s] = dryS * (1.0f - mix) + wet * mix;

            if (ch == 0)
            {
                lowSq  += (double) (lowOut * lowOut);
                highSq += (double) (highOut * highOut);
                harmSq += (double) (harmUse * harmUse);
            }
        }
    }

    lowRms.store     ((float) std::sqrt (lowSq  / (double) n), std::memory_order_relaxed);
    highRms.store    ((float) std::sqrt (highSq / (double) n), std::memory_order_relaxed);
    harmAmount.store ((float) std::sqrt (harmSq / (double) n), std::memory_order_relaxed);
}

juce::AudioProcessorEditor* LowLifterProcessor::createEditor()
{
    return new LowLifterEditor (*this);
}

static void styleCaption (juce::Label& l, const juce::String& text)
{
    l.setText (text, juce::dontSendNotification);
    l.setJustificationType (juce::Justification::centred);
    l.setFont (juce::Font (juce::FontOptions (9.5f)));
    l.setColour (juce::Label::textColourId, Colors::textDim);
}

class LowLifterLed : public juce::Button
{
public:
    explicit LowLifterLed (const juce::String& t) : juce::Button (t) { setClickingTogglesState (true); }

    void paintButton (juce::Graphics& g, bool, bool) override
    {
        auto b = getLocalBounds().toFloat().reduced (1.0f);
        const bool on = getToggleState();

        g.setColour (on ? Colors::accent.withAlpha (0.20f) : Colors::panelDark);
        g.fillRoundedRectangle (b, 3.0f);
        g.setColour (on ? Colors::accent : Colors::outline);
        g.drawRoundedRectangle (b, 3.0f, on ? 1.4f : 1.0f);

        const float r = 3.5f;
        const float cyy = b.getCentreY();
        juce::Rectangle<float> dot (b.getX() + 7.0f, cyy - r, r * 2.0f, r * 2.0f);
        g.setColour (on ? Colors::accent : Colors::panelBg.darker (0.3f));
        g.fillEllipse (dot);
        if (on)
        {
            g.setColour (Colors::accent.withAlpha (0.4f));
            g.drawEllipse (dot.expanded (1.5f), 1.0f);
        }

        g.setColour (on ? Colors::text : Colors::textDim);
        g.setFont (juce::Font (juce::FontOptions (8.5f, juce::Font::bold)));
        g.drawText (getButtonText(), b.withTrimmedLeft (16.0f), juce::Justification::centredLeft, false);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LowLifterLed)
};

class LowLifterPresetLabel : public juce::Label
{
public:
    LowLifterPresetLabel()
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setColour (juce::Label::textColourId, Colors::textDim);
    }

    std::function<void()> onClick;

    void mouseDown (const juce::MouseEvent&) override { if (onClick) onClick(); }

    void mouseEnter (const juce::MouseEvent&) override
    {
        setColour (juce::Label::textColourId, Colors::textDim.brighter (0.4f));
        repaint();
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        setColour (juce::Label::textColourId, Colors::textDim);
        repaint();
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LowLifterPresetLabel)
};

class LowLifterGraph : public juce::Component
{
public:
    explicit LowLifterGraph (LowLifterProcessor& p)
        : proc (p),
          lowAtt  (*p.lowGain,  [this] (float v) { lowGainDb  = v; repaint(); }),
          highAtt (*p.highGain, [this] (float v) { highGainDb = v; repaint(); }),
          cutAtt  (*p.cutoff,   [this] (float v) { cutoffHz   = v; repaint(); })
    {
        lowAtt.sendInitialUpdate();
        highAtt.sendInitialUpdate();
        cutAtt.sendInitialUpdate();
    }

    void setLevels (float lo, float hi) { lowGlow = lo; highGlow = hi; repaint(); }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        g.setColour (Colors::windowBg);
        g.fillRoundedRectangle (b, 4.0f);
        g.setColour (Colors::outline);
        g.drawRoundedRectangle (b, 4.0f, 1.0f);

        auto in = b.reduced (2.0f);
        const int W = getWidth();
        const int H = getHeight();

        struct NoteMark { const char* n; float f; };
        const NoteMark notes[5] = { { "C1", 32.70f }, { "C2", 65.41f }, { "C3", 130.81f },
                                    { "C4", 261.63f }, { "C5", 523.25f } };
        g.setFont (juce::Font (juce::FontOptions (8.5f)));
        for (auto& nn : notes)
        {
            const float x = freqToX (nn.f);
            if (x < 2.0f || x > (float) W - 2.0f) continue;
            g.setColour (Colors::gridLine.withAlpha (0.5f));
            g.drawVerticalLine ((int) x, in.getY() + 12.0f, in.getBottom() - 12.0f);
            g.setColour (Colors::textDim);
            g.drawText (nn.n, (int) x - 12, (int) in.getY() + 1, 24, 10, juce::Justification::centred);
        }
        const float hzMarks[4] = { 50.0f, 100.0f, 200.0f, 500.0f };
        for (float f : hzMarks)
        {
            const float x = freqToX (f);
            if (x < 2.0f || x > (float) W - 2.0f) continue;
            g.setColour (Colors::textDim);
            g.drawText (juce::String ((int) f), (int) x - 14, (int) in.getBottom() - 11, 28, 10,
                        juce::Justification::centred);
        }

        {
            const float y0 = dbToY (0.0f);
            g.setColour (Colors::gridLine.withAlpha (0.4f));
            g.drawHorizontalLine ((int) y0, in.getX(), in.getRight());
        }

        const float lowLin  = juce::Decibels::decibelsToGain (lowGainDb);
        const float highLin = juce::Decibels::decibelsToGain (highGainDb);

        auto drawBand = [&] (auto&& ampFn, juce::Colour col, float glow, float sw, bool fill)
        {
            juce::Path stroke, area;
            for (int x = 0; x <= W; x += 2)
            {
                const float f  = xToFreq ((float) x);
                const float a  = juce::jmax (1.0e-4f, (float) ampFn (f));
                const float db = 20.0f * std::log10 (a);
                const float y  = dbToY (db);
                if (x == 0) { stroke.startNewSubPath ((float) x, y); area.startNewSubPath ((float) x, (float) H); area.lineTo ((float) x, y); }
                else        { stroke.lineTo ((float) x, y); area.lineTo ((float) x, y); }
            }
            area.lineTo ((float) W, (float) H);
            area.closeSubPath();
            if (fill)
            {
                g.setColour (col.withAlpha (0.09f + 0.16f * juce::jlimit (0.0f, 1.0f, glow)));
                g.fillPath (area);
            }
            g.setColour (col.withAlpha (0.9f));
            g.strokePath (stroke, juce::PathStrokeType (sw));
        };

        const float lowSw  = (hoverMode == Mode::lowGain  || dragMode == Mode::lowGain)  ? 2.6f : 1.7f;
        const float highSw = (hoverMode == Mode::highGain || dragMode == Mode::highGain) ? 2.6f : 1.7f;

        drawBand ([&] (float f) { return lowAmp (f)  * lowLin;  }, Colors::danger,    lowGlow,  lowSw,  true);
        drawBand ([&] (float f) { return highAmp (f) * highLin; }, Colors::noteGreen, highGlow, highSw, true);
        drawBand ([&] (float f) { return lowAmp (f) * lowLin + highAmp (f) * highLin; },
                  Colors::text, 0.0f, 2.1f, false);

        const float cx = freqToX (cutoffHz);
        const bool cutHot = (hoverMode == Mode::cutoff || dragMode == Mode::cutoff);
        g.setColour ((cutHot ? Colors::text : Colors::textDim).withAlpha (0.6f));
        g.drawVerticalLine ((int) cx, in.getY() + 12.0f, in.getBottom() - 12.0f);
        const float hy = dbToY (0.0f);
        g.setColour (cutHot ? Colors::accent : Colors::accent.withAlpha (0.7f));
        g.fillEllipse (cx - 4.0f, hy - 4.0f, 8.0f, 8.0f);
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto m = pick (e);
        if (m != hoverMode) { hoverMode = m; repaint(); }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (hoverMode != Mode::none) { hoverMode = Mode::none; repaint(); }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragMode = pick (e);
        if      (dragMode == Mode::lowGain)  lowAtt.beginGesture();
        else if (dragMode == Mode::highGain) highAtt.beginGesture();
        else if (dragMode == Mode::cutoff)   cutAtt.beginGesture();
        mouseDrag (e);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragMode == Mode::cutoff)
            cutAtt.setValueAsPartOfGesture (juce::jlimit (30.0f, 500.0f, xToFreq ((float) e.x)));
        else if (dragMode == Mode::lowGain)
            lowAtt.setValueAsPartOfGesture (juce::jlimit (-12.0f, 12.0f, yToDb ((float) e.y)));
        else if (dragMode == Mode::highGain)
            highAtt.setValueAsPartOfGesture (juce::jlimit (-12.0f, 12.0f, yToDb ((float) e.y)));
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if      (dragMode == Mode::lowGain)  lowAtt.endGesture();
        else if (dragMode == Mode::highGain) highAtt.endGesture();
        else if (dragMode == Mode::cutoff)   cutAtt.endGesture();
        dragMode = Mode::none;
    }

private:
    enum class Mode { none, lowGain, highGain, cutoff };

    static constexpr float fMin = 20.0f, fMax = 1200.0f;
    static constexpr float dbMin = -30.0f, dbMax = 15.0f;

    float xToFreq (float x) const
    {
        const float t = juce::jlimit (0.0f, 1.0f, x / juce::jmax (1.0f, (float) getWidth()));
        return fMin * std::pow (fMax / fMin, t);
    }
    float freqToX (float f) const
    {
        return std::log (f / fMin) / std::log (fMax / fMin) * (float) getWidth();
    }
    float dbToY (float db) const
    {
        const float t = juce::jlimit (0.0f, 1.0f, (db - dbMin) / (dbMax - dbMin));
        return (1.0f - t) * (float) getHeight();
    }
    float yToDb (float y) const
    {
        const float t = 1.0f - juce::jlimit (0.0f, 1.0f, y / juce::jmax (1.0f, (float) getHeight()));
        return dbMin + t * (dbMax - dbMin);
    }
    float lowAmp (float f) const  { const float r = f / juce::jmax (1.0f, cutoffHz); return 1.0f / (1.0f + r * r * r * r); }
    float highAmp (float f) const { const float r = juce::jmax (1.0f, cutoffHz) / f; return 1.0f / (1.0f + r * r * r * r); }

    Mode pick (const juce::MouseEvent& e) const
    {
        if (e.mods.isCtrlDown())
            return Mode::cutoff;
        if (std::abs ((float) e.x - freqToX (cutoffHz)) < 14.0f)
            return Mode::cutoff;

        const float f = xToFreq ((float) e.x);
        const float lowY  = dbToY (20.0f * std::log10 (juce::jmax (1.0e-4f, lowAmp (f)  * juce::Decibels::decibelsToGain (lowGainDb))));
        const float highY = dbToY (20.0f * std::log10 (juce::jmax (1.0e-4f, highAmp (f) * juce::Decibels::decibelsToGain (highGainDb))));
        return std::abs ((float) e.y - lowY) <= std::abs ((float) e.y - highY) ? Mode::lowGain : Mode::highGain;
    }

    LowLifterProcessor& proc;
    juce::ParameterAttachment lowAtt, highAtt, cutAtt;

    float lowGainDb = 0.0f, highGainDb = 0.0f, cutoffHz = 100.0f;
    float lowGlow = 0.0f, highGlow = 0.0f;
    Mode  hoverMode = Mode::none, dragMode = Mode::none;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LowLifterGraph)
};

LowLifterEditor::LowLifterEditor (LowLifterProcessor& p)
    : juce::AudioProcessorEditor (p), proc (p)
{
    auto styleButton = [] (juce::TextButton& btn)
    {
        btn.setColour (juce::TextButton::buttonColourId, Colors::raised);
        btn.setColour (juce::TextButton::textColourOffId, Colors::text);
    };
    styleButton (prevBtn); styleButton (nextBtn);

    prevBtn.onClick = [this] { applyPreset (currentPreset <= 0 ? 2 : currentPreset - 1); };
    nextBtn.onClick = [this] { applyPreset (currentPreset < 0 ? 0 : (currentPreset + 1) % 3); };
    addAndMakeVisible (prevBtn);
    addAndMakeVisible (nextBtn);

    presetName = std::make_unique<LowLifterPresetLabel>();
    presetName->setText ("Low Lifter", juce::dontSendNotification);
    presetName->setJustificationType (juce::Justification::centredLeft);
    presetName->setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
    presetName->onClick = [this] { showPresetMenu(); };
    addAndMakeVisible (*presetName);

    graph = std::make_unique<LowLifterGraph> (proc);
    addAndMakeVisible (*graph);

    multiBandLed = std::make_unique<LowLifterLed> ("MULTIBAND");
    lookaheadLed = std::make_unique<LowLifterLed> ("LOOKAHEAD");
    invertLed    = std::make_unique<LowLifterLed> ("INVERT");
    addAndMakeVisible (*multiBandLed);
    addAndMakeVisible (*lookaheadLed);
    addAndMakeVisible (*invertLed);

    for (auto* k : { &preGainK, &slopeK, &diffDepthK, &diffMixK,
                     &lowGainK, &cutoffK, &highGainK, &lowCutK, &dryWetK })
        addAndMakeVisible (*k);

    lowGainK.setColour  (juce::Slider::rotarySliderFillColourId, Colors::danger);
    highGainK.setColour (juce::Slider::rotarySliderFillColourId, Colors::noteGreen);

    preGainK.setDoubleClickReturnValue  (true, 4.0);
    slopeK.setDoubleClickReturnValue     (true, 0.3);
    diffDepthK.setDoubleClickReturnValue (true, 0.2);
    diffMixK.setDoubleClickReturnValue   (true, 0.25);
    lowGainK.setDoubleClickReturnValue   (true, 1.5);
    cutoffK.setDoubleClickReturnValue    (true, 100.0);
    highGainK.setDoubleClickReturnValue  (true, 1.0);
    lowCutK.setDoubleClickReturnValue    (true, 20.0);
    dryWetK.setDoubleClickReturnValue    (true, 1.0);

    auto addLabel = [this] (const juce::String& t) -> juce::Label&
    {
        auto* l = new juce::Label(); styleCaption (*l, t);
        labels.add (l); addAndMakeVisible (l); return *l;
    };
    addLabel ("PRE GAIN"); addLabel ("SLOPE");
    addLabel ("DEPTH");    addLabel ("MIX");
    addLabel ("LOW GAIN"); addLabel ("CUTOFF"); addLabel ("HIGH GAIN");
    addLabel ("LOW CUT");  addLabel ("DRY/WET");

    multiBandAtt = std::make_unique<juce::ButtonParameterAttachment> (*proc.multiBand, *multiBandLed);
    lookaheadAtt = std::make_unique<juce::ButtonParameterAttachment> (*proc.lookahead, *lookaheadLed);
    invertAtt    = std::make_unique<juce::ButtonParameterAttachment> (*proc.invertPolarity, *invertLed);

    setSize (720, 330);
    startTimerHz (30);
}

LowLifterEditor::~LowLifterEditor() { stopTimer(); }

void LowLifterEditor::timerCallback()
{
    if (graph == nullptr) return;
    graph->setLevels (juce::jlimit (0.0f, 1.0f, proc.lowRms.load (std::memory_order_relaxed)  * 4.0f),
                      juce::jlimit (0.0f, 1.0f, proc.highRms.load (std::memory_order_relaxed) * 4.0f));
}

void LowLifterEditor::showPresetMenu()
{
    juce::PopupMenu m;
    for (int i = 0; i < 3; ++i)
        m.addItem (i + 1, kPresets[i].name, true, i == currentPreset);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (presetName.get()),
        [safe = juce::Component::SafePointer<LowLifterEditor> (this)] (int r)
        {
            if (safe != nullptr && r > 0)
                safe->applyPreset (r - 1);
        });
}

void LowLifterEditor::applyPreset (int index)
{
    if (index < 0 || index >= 3) return;
    const auto& ps = kPresets[index];

    auto setP = [] (juce::RangedAudioParameter& prm, float plain)
    {
        prm.setValueNotifyingHost (prm.convertTo0to1 (plain));
    };

    setP (*proc.preGain, ps.preGain);
    setP (*proc.slope, ps.slope);
    setP (*proc.invertPolarity, ps.invert ? 1.0f : 0.0f);
    setP (*proc.multiBand, ps.multiBand ? 1.0f : 0.0f);
    setP (*proc.lookahead, ps.lookahead ? 1.0f : 0.0f);
    setP (*proc.diffDepth, ps.diffDepth);
    setP (*proc.diffMix, ps.diffMix);
    setP (*proc.lowGain, ps.lowGain);
    setP (*proc.cutoff, ps.cutoff);
    setP (*proc.highGain, ps.highGain);
    setP (*proc.lowCut, ps.lowCut);
    setP (*proc.dryWet, ps.dryWet);

    currentPreset = index;
    presetName->setText (ps.name, juce::dontSendNotification);
}

void LowLifterEditor::paint (juce::Graphics& g)
{
    g.fillAll (Colors::panelDark.darker (0.4f));

    g.setColour (Colors::panelDark);
    g.fillRoundedRectangle (secPreset.toFloat(), 3.0f);
    g.setColour (Colors::outline);
    g.drawRoundedRectangle (secPreset.toFloat(), 3.0f, 1.0f);

    auto section = [&g] (juce::Rectangle<int> r, const juce::String& title, juce::Colour tint)
    {
        g.setColour (Colors::panelDark.withAlpha (0.85f));
        g.fillRoundedRectangle (r.toFloat(), 4.0f);
        if (tint != juce::Colour())
        {
            g.setColour (tint.withAlpha (0.12f));
            g.fillRoundedRectangle (r.toFloat(), 4.0f);
        }
        g.setColour (Colors::outline);
        g.drawRoundedRectangle (r.toFloat(), 4.0f, 1.0f);
        auto header = r.removeFromTop (15);
        g.setColour (tint != juce::Colour() ? tint : Colors::accent);
        g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
        g.drawText (title, header.reduced (8, 0), juce::Justification::centredLeft, false);
    };
    section (secHarm,  "HARMONICS", Colors::danger);
    section (secDiff,  "DIFFUSION", juce::Colour());
    section (secXover, "CROSSOVER", juce::Colour());
    section (secMix,   "MIX",       juce::Colour());
}

void LowLifterEditor::resized()
{
    auto placeKnob = [this] (int li, juce::Slider& k, juce::Rectangle<int> r)
    {
        labels[li]->setBounds (r.removeFromBottom (12));
        k.setBounds (r.reduced (3));
    };

    auto full = getLocalBounds().reduced (8);

    secPreset = full.removeFromTop (24);
    full.removeFromTop (6);
    {
        auto c = secPreset.reduced (4, 0);
        prevBtn.setBounds (c.removeFromLeft (24).reduced (0, 2));
        c.removeFromLeft (4);
        nextBtn.setBounds (c.removeFromLeft (24).reduced (0, 2));
        c.removeFromLeft (10);
        presetName->setBounds (c);
    }

    secGraph = full.removeFromTop (146);
    full.removeFromTop (8);
    if (graph) graph->setBounds (secGraph);

    auto row = full;
    const int gap = 6;
    const int total = row.getWidth();
    const int wHarm  = (int) (total * 0.32f);
    const int wDiff  = (int) (total * 0.18f);
    const int wXover = (int) (total * 0.28f);
    secHarm  = row.removeFromLeft (wHarm);  row.removeFromLeft (gap);
    secDiff  = row.removeFromLeft (wDiff);  row.removeFromLeft (gap);
    secXover = row.removeFromLeft (wXover); row.removeFromLeft (gap);
    secMix   = row;

    {
        auto c = secHarm; c.removeFromTop (15); c = c.reduced (6, 5);
        auto leds = c.removeFromBottom (18);
        c.removeFromBottom (4);
        const int kw = c.getWidth() / 2;
        placeKnob (0, preGainK, c.removeFromLeft (kw));
        placeKnob (1, slopeK, c);
        const int lw = (leds.getWidth() - 8) / 3;
        multiBandLed->setBounds (leds.removeFromLeft (lw)); leds.removeFromLeft (4);
        lookaheadLed->setBounds (leds.removeFromLeft (lw)); leds.removeFromLeft (4);
        invertLed->setBounds (leds);
    }
    {
        auto c = secDiff; c.removeFromTop (15); c = c.reduced (6, 5);
        const int kw = c.getWidth() / 2;
        placeKnob (2, diffDepthK, c.removeFromLeft (kw));
        placeKnob (3, diffMixK, c);
    }
    {
        auto c = secXover; c.removeFromTop (15); c = c.reduced (6, 5);
        const int kw = c.getWidth() / 3;
        placeKnob (4, lowGainK, c.removeFromLeft (kw));
        placeKnob (5, cutoffK, c.removeFromLeft (kw));
        placeKnob (6, highGainK, c);
    }
    {
        auto c = secMix; c.removeFromTop (15); c = c.reduced (6, 5);
        const int kw = c.getWidth() / 2;
        placeKnob (7, lowCutK, c.removeFromLeft (kw));
        placeKnob (8, dryWetK, c);
    }
}
