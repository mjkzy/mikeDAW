#include "BuiltinLimiter.h"

namespace
{
    juce::NormalisableRange<float> logRangeL (float lo, float hi)
    {
        juce::NormalisableRange<float> r (lo, hi);
        r.setSkewForCentre (std::sqrt (lo * hi));
        return r;
    }

    const float curveRelMult[4] = { 1.5f, 1.0f, 0.5f, 2.5f };

    float saturate (float x, float amount)
    {
        if (amount <= 1.0e-4f)
            return x;
        const float d = 1.0f + 6.0f * amount;
        const float y = std::tanh (x * d) / std::tanh (d);
        return x * (1.0f - amount) + y * amount;
    }
}

LimiterProcessor::LimiterProcessor()
{
    addParameter (gain = new juce::AudioParameterFloat (
        juce::ParameterID { "gain", 1 }, "Gain",
        juce::NormalisableRange<float> (-30.0f, 30.0f, 0.1f), 0.0f));
    addParameter (sat = new juce::AudioParameterFloat (
        juce::ParameterID { "sat", 1 }, "Sat",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));
    addParameter (thres = new juce::AudioParameterFloat (
        juce::ParameterID { "thres", 1 }, "Comp Thres",
        juce::NormalisableRange<float> (-60.0f, 0.0f, 0.1f), 0.0f));
    addParameter (knee = new juce::AudioParameterFloat (
        juce::ParameterID { "knee", 1 }, "Knee",
        juce::NormalisableRange<float> (0.0f, 24.0f, 0.1f), 6.0f));
    addParameter (ratio = new juce::AudioParameterFloat (
        juce::ParameterID { "ratio", 1 }, "Ratio", logRangeL (1.0f, 30.0f), 4.0f));
    addParameter (att = new juce::AudioParameterFloat (
        juce::ParameterID { "att", 1 }, "Attack", logRangeL (0.1f, 500.0f), 20.0f));
    addParameter (rel = new juce::AudioParameterFloat (
        juce::ParameterID { "rel", 1 }, "Release", logRangeL (5.0f, 1000.0f), 100.0f));
    addParameter (sustain = new juce::AudioParameterFloat (
        juce::ParameterID { "sustain", 1 }, "Sustain",
        juce::NormalisableRange<float> (0.0f, 500.0f, 1.0f), 0.0f));
    addParameter (curve = new juce::AudioParameterChoice (
        juce::ParameterID { "curve", 1 }, "Curve",
        juce::StringArray { "Smooth", "Linear", "Tight", "Vintage" }, 0));
    addParameter (gateThres = new juce::AudioParameterFloat (
        juce::ParameterID { "gThres", 1 }, "Gate Thres",
        juce::NormalisableRange<float> (-80.0f, 0.0f, 0.1f), -80.0f));
    addParameter (gateFloor = new juce::AudioParameterFloat (
        juce::ParameterID { "gFloor", 1 }, "Gate Floor",
        juce::NormalisableRange<float> (-80.0f, 0.0f, 0.1f), -80.0f));
    addParameter (gateRel = new juce::AudioParameterFloat (
        juce::ParameterID { "gRel", 1 }, "Gate Rel", logRangeL (5.0f, 1000.0f), 100.0f));
    addParameter (ceiling = new juce::AudioParameterFloat (
        juce::ParameterID { "ceiling", 1 }, "Ceiling",
        juce::NormalisableRange<float> (-20.0f, 0.0f, 0.1f), 0.0f));
}

void LimiterProcessor::prepareToPlay (double sampleRate, int blockSize)
{
    sr = sampleRate > 0 ? sampleRate : 44100.0;
    juce::ignoreUnused (blockSize);

    comp.prepare (sr);
    limit.prepare (sr, 2.0f, 100.0f);
    setLatencySamples (limit.lookahead());

    gateGain = 1.0f;
    gateAtkCoeff = std::exp (-1.0f / (0.001f * (float) sr));

    preSm.reset (sr, 0.02);
    preSm.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (gain->get()));
}

void LimiterProcessor::reset()
{
    comp.reset();
    limit.reset();
    gateGain = 1.0f;
    preSm.setCurrentAndTargetValue (preSm.getTargetValue());
}

void LimiterProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    if (sr <= 0.0 || buffer.getNumSamples() == 0)
        return;

    const int n   = buffer.getNumSamples();
    const int nch = juce::jmin (2, buffer.getNumChannels());

    preSm.setTargetValue (juce::Decibels::decibelsToGain (gain->get()));
    comp.setTimes (att->get(), rel->get() * curveRelMult[curve->getIndex()]);
    comp.setHoldMs (sustain->get());

    const float satAmt   = sat->get();
    const float thr      = thres->get();
    const float kn       = knee->get();
    const float rt       = ratio->get();
    const float ceilLin  = juce::Decibels::decibelsToGain (ceiling->get());
    const float gateThr  = gateThres->get();
    const float floorG   = juce::Decibels::decibelsToGain (gateFloor->get(), -120.0f);
    const float gateRelC = std::exp (-1.0f / (juce::jmax (1.0e-4f, gateRel->get() * 0.001f) * (float) sr));

    float* chL = buffer.getWritePointer (0);
    float* chR = nch > 1 ? buffer.getWritePointer (1) : chL;

    float peakRed = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        const float pg = preSm.getNextValue();
        float l = chL[i] * pg;
        float r = chR[i] * pg;

        const float preDetect = juce::jmax (std::abs (l), std::abs (r));
        const float gateTarget = juce::Decibels::gainToDecibels (preDetect, -120.0f) >= gateThr ? 1.0f : floorG;
        gateGain = gateTarget < gateGain ? gateRelC * gateGain + (1.0f - gateRelC) * gateTarget
                                         : gateAtkCoeff * gateGain + (1.0f - gateAtkCoeff) * gateTarget;
        l *= gateGain; r *= gateGain;

        l = saturate (l, satAmt);
        r = saturate (r, satAmt);

        const float detect = juce::jmax (std::abs (l), std::abs (r));
        const float g = comp.process (detect, thr, kn, rt);
        l *= g; r *= g;

        limit.process (l, r, ceilLin);

        chL[i] = l;
        if (nch > 1) chR[i] = r;

        peakRed = juce::jmax (peakRed, comp.currentReductionDb() - limit.redDb);
    }

    grMeter.store (peakRed, std::memory_order_relaxed);
}

juce::AudioProcessorEditor* LimiterProcessor::createEditor()
{
    return new LimiterEditor (*this);
}

namespace
{
    void styleKnobLabel (juce::Label& l, const juce::String& t)
    {
        l.setText (t, juce::dontSendNotification);
        l.setJustificationType (juce::Justification::centred);
        l.setFont (juce::Font (juce::FontOptions (10.0f)));
        l.setColour (juce::Label::textColourId, Colors::textDim);
    }

    void styleHeader (juce::Label& l, const juce::String& t)
    {
        l.setText (t, juce::dontSendNotification);
        l.setJustificationType (juce::Justification::centredLeft);
        l.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
        l.setColour (juce::Label::textColourId, Colors::accent);
    }
}

LimiterEditor::LimiterEditor (LimiterProcessor& p)
    : juce::AudioProcessorEditor (p), proc (p)
{
    addAndMakeVisible (grView);

    auto setupTab = [this] (juce::TextButton& b, bool comp)
    {
        b.setClickingTogglesState (false);
        b.setColour (juce::TextButton::buttonOnColourId, Colors::accent.withAlpha (0.8f));
        b.onClick = [this, comp] { showComp = comp; updateTab(); };
        addAndMakeVisible (b);
    };
    setupTab (compTab, true);
    setupTab (limitTab, false);

    auto setupKnob = [this] (ValueKnob& k, juce::Label& l, const juce::String& t,
                             juce::AudioParameterFloat& prm)
    {
        k.setDoubleClickReturnValue (true, prm.getNormalisableRange().convertFrom0to1 (
            static_cast<const juce::AudioProcessorParameter&> (prm).getDefaultValue()));
        addAndMakeVisible (k);
        styleKnobLabel (l, t);
        addAndMakeVisible (l);
    };
    setupKnob (gainK,    gainL,    "GAIN",  *proc.gain);
    setupKnob (satK,     satL,     "SAT",   *proc.sat);
    setupKnob (thresK,   thresL,   "THRES", *proc.thres);
    setupKnob (kneeK,    kneeL,    "KNEE",  *proc.knee);
    setupKnob (ratioK,   ratioL,   "RATIO", *proc.ratio);
    setupKnob (attK,     attL,     "ATT",   *proc.att);
    setupKnob (relK,     relL,     "REL",   *proc.rel);
    setupKnob (sustainK, sustainL, "SUS",   *proc.sustain);
    setupKnob (gThresK,  gThresL,  "THRES", *proc.gateThres);
    setupKnob (gFloorK,  gFloorL,  "GAIN",  *proc.gateFloor);
    setupKnob (gRelK,    gRelL,    "REL",   *proc.gateRel);
    setupKnob (ceilK,    ceilL,    "CEIL",  *proc.ceiling);

    addAndMakeVisible (curveBox);
    styleKnobLabel (curveL, "CURVE");
    addAndMakeVisible (curveL);

    styleHeader (loudHdr, "LOUDNESS");
    styleHeader (envHdr,  "ENVELOPE");
    styleHeader (gateHdr, "NOISE GATE");
    addAndMakeVisible (loudHdr);
    addAndMakeVisible (envHdr);
    addAndMakeVisible (gateHdr);

    setSize (640, 250);
    updateTab();
    startTimerHz (30);
}

LimiterEditor::~LimiterEditor() { stopTimer(); }

void LimiterEditor::timerCallback()
{
    grView.push (proc.grMeter.load (std::memory_order_relaxed));
    grView.repaint();
}

void LimiterEditor::updateTab()
{
    compTab.setToggleState (showComp, juce::dontSendNotification);
    limitTab.setToggleState (! showComp, juce::dontSendNotification);

    juce::Component* compGroup[] = { &thresK, &thresL, &kneeK, &kneeL, &ratioK, &ratioL,
                                     &attK, &attL, &relK, &relL, &sustainK, &sustainL,
                                     &curveBox, &curveL, &gThresK, &gThresL, &gFloorK, &gFloorL,
                                     &gRelK, &gRelL, &loudHdr, &envHdr, &gateHdr };
    juce::Component* limitGroup[] = { &ceilK, &ceilL };

    for (auto* c : compGroup)  c->setVisible (showComp);
    for (auto* c : limitGroup) c->setVisible (! showComp);
    gainK.setVisible (true);  gainL.setVisible (true);
    satK.setVisible (true);   satL.setVisible (true);

    resized();
}

void LimiterEditor::paint (juce::Graphics& g)
{
    g.fillAll (Colors::panelBg);
}

void LimiterEditor::resized()
{
    auto r = getLocalBounds().reduced (6);

    auto top = r.removeFromTop (54);
    auto tabs = top.removeFromLeft (70);
    compTab.setBounds (tabs.removeFromTop (25).reduced (1));
    limitTab.setBounds (tabs.removeFromTop (25).reduced (1));
    top.removeFromLeft (6);
    grView.setBounds (top);

    r.removeFromTop (6);

    auto cell = [] (juce::Rectangle<int> area, juce::Component& c, juce::Label& l)
    {
        l.setBounds (area.removeFromBottom (13));
        c.setBounds (area.reduced (3));
    };
    auto lay = [&cell] (juce::Rectangle<int> area, std::vector<std::pair<juce::Component*, juce::Label*>> items)
    {
        const int w = area.getWidth() / juce::jmax (1, (int) items.size());
        for (auto& it : items)
            cell (area.removeFromLeft (w), *it.first, *it.second);
    };

    if (showComp)
    {
        const int total = r.getWidth();
        auto loud = r.removeFromLeft (total * 5 / 12);
        auto env  = r.removeFromLeft (total * 4 / 12);
        auto gate = r;

        loudHdr.setBounds (loud.removeFromTop (14).reduced (2, 0));
        envHdr .setBounds (env .removeFromTop (14).reduced (2, 0));
        gateHdr.setBounds (gate.removeFromTop (14).reduced (2, 0));

        lay (loud, { { &gainK, &gainL }, { &satK, &satL }, { &thresK, &thresL }, { &kneeK, &kneeL }, { &ratioK, &ratioL } });
        lay (env,  { { &attK, &attL }, { &relK, &relL }, { &sustainK, &sustainL }, { &curveBox, &curveL } });
        lay (gate, { { &gThresK, &gThresL }, { &gFloorK, &gFloorL }, { &gRelK, &gRelL } });
    }
    else
    {
        auto row = r.withSizeKeepingCentre (juce::jmin (r.getWidth(), 360), juce::jmin (r.getHeight(), 130));
        lay (row, { { &gainK, &gainL }, { &satK, &satL }, { &ceilK, &ceilL } });
    }
}

void LimiterEditor::GrView::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    g.setColour (Colors::panelDark.darker (0.4f));
    g.fillRoundedRectangle (b, 3.0f);
    g.setColour (Colors::outline);
    g.drawRoundedRectangle (b, 3.0f, 1.0f);

    auto in = b.reduced (3.0f);
    g.setColour (Colors::gridLine.withAlpha (0.5f));
    for (int db = 6; db < 24; db += 6)
    {
        const float y = in.getY() + in.getHeight() * ((float) db / 24.0f);
        g.drawHorizontalLine ((int) y, in.getX(), in.getRight());
    }

    const int nPts = (int) history.size();
    juce::Path path;
    for (int i = 0; i < nPts; ++i)
    {
        const int idx = (head + i) % nPts;
        const float red = juce::jlimit (0.0f, 24.0f, history[(size_t) idx]);
        const float x = in.getX() + in.getWidth() * ((float) i / (float) (nPts - 1));
        const float y = in.getY() + in.getHeight() * (red / 24.0f);
        if (i == 0) path.startNewSubPath (x, y);
        else        path.lineTo (x, y);
    }
    g.setColour (Colors::accent2);
    g.strokePath (path, juce::PathStrokeType (1.5f));
}
