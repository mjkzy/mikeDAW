#include "BuiltinMultiband.h"

namespace
{
    juce::NormalisableRange<float> logRange (float lo, float hi)
    {
        juce::NormalisableRange<float> r (lo, hi);
        r.setSkewForCentre (std::sqrt (lo * hi));
        return r;
    }

    const char* bandNames[MultibandProcessor::numBands] = { "LOW", "MID", "HIGH" };
}

MultibandProcessor::MultibandProcessor()
{
    addParameter (inGain = new juce::AudioParameterFloat (
        juce::ParameterID { "inGain", 1 }, "In Gain",
        juce::NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f));
    addParameter (xoverLow = new juce::AudioParameterFloat (
        juce::ParameterID { "xLow", 1 }, "Low/Mid", logRange (40.0f, 500.0f), 120.0f));
    addParameter (xoverHigh = new juce::AudioParameterFloat (
        juce::ParameterID { "xHigh", 1 }, "Mid/High", logRange (800.0f, 12000.0f), 2500.0f));

    const float defThres[numBands] = { -18.0f, -18.0f, -18.0f };
    for (int b = 0; b < numBands; ++b)
    {
        const juce::String pfx = juce::String ("b") + juce::String (b);
        const juce::String nm  = juce::String (bandNames[b]) + " ";
        addParameter (band[b].mode = new juce::AudioParameterChoice (
            juce::ParameterID { pfx + "mode", 1 }, nm + "Mode",
            juce::StringArray { "Active", "Mute", "Bypass" }, 0));
        addParameter (band[b].thres = new juce::AudioParameterFloat (
            juce::ParameterID { pfx + "thres", 1 }, nm + "Thres",
            juce::NormalisableRange<float> (-60.0f, 0.0f, 0.1f), defThres[b]));
        addParameter (band[b].ratio = new juce::AudioParameterFloat (
            juce::ParameterID { pfx + "ratio", 1 }, nm + "Ratio",
            logRange (1.0f, 30.0f), 3.0f));
        addParameter (band[b].knee = new juce::AudioParameterFloat (
            juce::ParameterID { pfx + "knee", 1 }, nm + "Knee",
            juce::NormalisableRange<float> (0.0f, 24.0f, 0.1f), 6.0f));
        addParameter (band[b].att = new juce::AudioParameterFloat (
            juce::ParameterID { pfx + "att", 1 }, nm + "Att", logRange (0.1f, 300.0f), 20.0f));
        addParameter (band[b].rel = new juce::AudioParameterFloat (
            juce::ParameterID { pfx + "rel", 1 }, nm + "Rel", logRange (5.0f, 1000.0f), 150.0f));
        addParameter (band[b].gain = new juce::AudioParameterFloat (
            juce::ParameterID { pfx + "gain", 1 }, nm + "Gain",
            juce::NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f));
    }

    addParameter (limiter = new juce::AudioParameterBool (
        juce::ParameterID { "limiter", 1 }, "Limiter", true));
}

void MultibandProcessor::prepareToPlay (double sampleRate, int blockSize)
{
    sr = sampleRate > 0 ? sampleRate : 44100.0;
    juce::dsp::ProcessSpec spec { sr, (juce::uint32) juce::jmax (1, blockSize), 1 };
    for (auto& s : split) { s.prepare (spec); s.setFrequencies (xoverLow->get(), xoverHigh->get()); }
    for (auto& c : comp) c.prepare (sr);

    inSm.reset (sr, 0.02);
    inSm.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (inGain->get()));
    for (int b = 0; b < numBands; ++b)
    {
        bandGainSm[b].reset (sr, 0.02);
        bandGainSm[b].setCurrentAndTargetValue (juce::Decibels::decibelsToGain (band[b].gain->get()));
    }
}

void MultibandProcessor::reset()
{
    for (auto& s : split) s.reset();
    for (auto& c : comp) c.reset();
    inSm.setCurrentAndTargetValue (inSm.getTargetValue());
    for (int b = 0; b < numBands; ++b)
        bandGainSm[b].setCurrentAndTargetValue (bandGainSm[b].getTargetValue());
}

void MultibandProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    if (sr <= 0.0 || buffer.getNumSamples() == 0)
        return;

    const int n   = buffer.getNumSamples();
    const int nch = juce::jmin (2, buffer.getNumChannels());

    for (auto& s : split) s.setFrequencies (xoverLow->get(), xoverHigh->get());
    for (int b = 0; b < numBands; ++b)
    {
        comp[b].setTimes (band[b].att->get(), band[b].rel->get());
        bandGainSm[b].setTargetValue (juce::Decibels::decibelsToGain (band[b].gain->get()));
    }
    inSm.setTargetValue (juce::Decibels::decibelsToGain (inGain->get()));

    const bool doLimit = limiter->get();
    float peakRed[numBands] = { 0.0f, 0.0f, 0.0f };

    float* chL = buffer.getWritePointer (0);
    float* chR = nch > 1 ? buffer.getWritePointer (1) : chL;

    for (int i = 0; i < n; ++i)
    {
        const float ig = inSm.getNextValue();
        float in[2] = { chL[i] * ig, chR[i] * ig };

        float bandS[numBands][2];
        for (int ch = 0; ch < nch; ++ch)
            split[ch].process (in[ch], bandS[0][ch], bandS[1][ch], bandS[2][ch]);
        if (nch == 1)
            for (int b = 0; b < numBands; ++b) bandS[b][1] = bandS[b][0];

        float outL = 0.0f, outR = 0.0f;
        for (int b = 0; b < numBands; ++b)
        {
            const int mode = band[b].mode->getIndex();
            if (mode == 1)
                continue;

            const float bg = bandGainSm[b].getNextValue();
            float l = bandS[b][0], r = bandS[b][1];

            if (mode == 0)
            {
                const float detect = juce::jmax (std::abs (l), std::abs (r));
                const float g = comp[b].process (detect, band[b].thres->get(),
                                                  band[b].knee->get(), band[b].ratio->get());
                l *= g; r *= g;
                peakRed[b] = juce::jmax (peakRed[b], comp[b].currentReductionDb());
            }
            l *= bg; r *= bg;
            outL += l; outR += r;
        }

        if (doLimit)
        {
            outL = std::tanh (outL);
            outR = std::tanh (outR);
        }

        chL[i] = outL;
        if (nch > 1) chR[i] = outR;
    }

    for (int b = 0; b < numBands; ++b)
        grMeter[b].store (peakRed[b], std::memory_order_relaxed);
}

juce::AudioProcessorEditor* MultibandProcessor::createEditor()
{
    return new MultibandEditor (*this);
}

namespace
{
    void styleLabel (juce::Label& l, const juce::String& t, float h = 10.0f,
                     juce::Colour c = Colors::textDim, bool bold = false)
    {
        l.setText (t, juce::dontSendNotification);
        l.setJustificationType (juce::Justification::centred);
        l.setFont (juce::Font (juce::FontOptions (h, bold ? juce::Font::bold : juce::Font::plain)));
        l.setColour (juce::Label::textColourId, c);
    }

    class MBModeRow : public juce::Component
    {
    public:
        MBModeRow (juce::AudioParameterChoice& p)
            : att (p, [this] (float v) { current = (int) std::round (v); refresh(); })
        {
            static const char* txt[3] = { "A", "M", "B" };
            for (int i = 0; i < 3; ++i)
            {
                auto* b = buttons.add (new juce::TextButton (txt[i]));
                b->setClickingTogglesState (false);
                b->onClick = [this, i] { att.setValueAsCompleteGesture ((float) i); };
                addAndMakeVisible (b);
            }
            att.sendInitialUpdate();
        }

        void resized() override
        {
            auto r = getLocalBounds();
            const int w = r.getWidth() / 3;
            for (int i = 0; i < 3; ++i)
                buttons[i]->setBounds (i < 2 ? r.removeFromLeft (w) : r);
        }

    private:
        void refresh()
        {
            for (int i = 0; i < buttons.size(); ++i)
                buttons[i]->setToggleState (i == current, juce::dontSendNotification);
        }

        juce::OwnedArray<juce::TextButton> buttons;
        int current = 0;
        juce::ParameterAttachment att;
    };
}

struct MultibandEditor::BandStrip : public juce::Component
{
    BandStrip (MultibandProcessor& p, int bandIdx, const juce::String& name)
        : proc (p), idx (bandIdx)
    {
        styleLabel (title, name, 11.0f, Colors::accent, true);
        addAndMakeVisible (title);

        modeRow = std::make_unique<MBModeRow> (*p.band[bandIdx].mode);
        addAndMakeVisible (*modeRow);

        auto* pr = &p.band[bandIdx];
        juce::AudioParameterFloat* params[6] = { pr->thres, pr->ratio, pr->knee, pr->att, pr->rel, pr->gain };
        const char* names[6] = { "THRES", "RATIO", "KNEE", "ATT", "REL", "GAIN" };
        for (int k = 0; k < 6; ++k)
        {
            auto* knob = knobs.add (new ValueKnob());
            knob->setDoubleClickReturnValue (true, params[k]->getNormalisableRange().convertFrom0to1 (
                static_cast<const juce::AudioProcessorParameter*> (params[k])->getDefaultValue()));
            addAndMakeVisible (knob);
            auto* lbl = labels.add (new juce::Label());
            styleLabel (*lbl, names[k]);
            addAndMakeVisible (lbl);
            atts.add (new juce::SliderParameterAttachment (*params[k], *knob));
        }
    }

    void setMeter (float db) { meterDb = db; repaint(); }

    void paint (juce::Graphics& g) override
    {
        g.setColour (Colors::panelDark);
        g.fillRect (getLocalBounds());
        g.setColour (Colors::outline);
        g.drawRect (getLocalBounds(), 1);

        g.setColour (Colors::panelBg.darker (0.3f));
        g.fillRect (meterArea);
        const float frac = juce::jlimit (0.0f, 1.0f, meterDb / 24.0f);
        auto fill = meterArea.toFloat().withHeight (meterArea.getHeight() * frac).withY ((float) meterArea.getY());
        g.setColour (Colors::accent2.withAlpha (0.85f));
        g.fillRect (fill);
        g.setColour (Colors::outline);
        g.drawRect (meterArea, 1);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (4);
        title.setBounds (r.removeFromTop (16));
        modeRow->setBounds (r.removeFromTop (18).reduced (2, 0));
        r.removeFromTop (4);
        meterArea = r.removeFromRight (10);
        r.removeFromRight (4);

        auto cell = [] (juce::Rectangle<int> area, juce::Slider& k, juce::Label& l)
        {
            l.setBounds (area.removeFromBottom (13));
            k.setBounds (area.reduced (2));
        };
        const int rowH = r.getHeight() / 2;
        for (int row = 0; row < 2; ++row)
        {
            auto rr = r.removeFromTop (rowH);
            const int cw = rr.getWidth() / 3;
            for (int c = 0; c < 3; ++c)
            {
                const int k = row * 3 + c;
                cell (c < 2 ? rr.removeFromLeft (cw) : rr, *knobs[k], *labels[k]);
            }
        }
    }

    MultibandProcessor& proc;
    int idx;
    juce::Label title;
    std::unique_ptr<MBModeRow> modeRow;
    juce::OwnedArray<ValueKnob> knobs;
    juce::OwnedArray<juce::Label> labels;
    juce::OwnedArray<juce::SliderParameterAttachment> atts;
    juce::Rectangle<int> meterArea;
    float meterDb = 0.0f;
};

MultibandEditor::MultibandEditor (MultibandProcessor& p)
    : juce::AudioProcessorEditor (p), proc (p)
{
    styleLabel (title, "MULTIBAND", 12.0f, Colors::text, true);
    addAndMakeVisible (title);

    auto setupKnob = [this] (ValueKnob& k, juce::Label& l, const juce::String& t,
                             juce::AudioParameterFloat& prm)
    {
        k.setDoubleClickReturnValue (true, prm.getNormalisableRange().convertFrom0to1 (
            static_cast<const juce::AudioProcessorParameter&> (prm).getDefaultValue()));
        addAndMakeVisible (k);
        styleLabel (l, t);
        addAndMakeVisible (l);
    };
    setupKnob (inGainK, inGainL, "GAIN",  *proc.inGain);
    setupKnob (xLowK,   xLowL,   "L/M",   *proc.xoverLow);
    setupKnob (xHighK,  xHighL,  "M/H",   *proc.xoverHigh);

    limiterBtn = std::make_unique<juce::ToggleButton> ("Limiter");
    addAndMakeVisible (*limiterBtn);
    limiterAtt = std::make_unique<juce::ButtonParameterAttachment> (*proc.limiter, *limiterBtn);

    static const char* names[MultibandProcessor::numBands] = { "LOW", "MID", "HIGH" };
    for (int b = 0; b < MultibandProcessor::numBands; ++b)
    {
        auto* s = strips.add (new BandStrip (proc, b, names[b]));
        addAndMakeVisible (s);
    }

    setSize (620, 250);
    startTimerHz (30);
}

MultibandEditor::~MultibandEditor() { stopTimer(); }

void MultibandEditor::timerCallback()
{
    for (int b = 0; b < MultibandProcessor::numBands; ++b)
        strips[b]->setMeter (proc.grMeter[b].load (std::memory_order_relaxed));
}

void MultibandEditor::paint (juce::Graphics& g)
{
    g.fillAll (Colors::panelBg);
}

void MultibandEditor::resized()
{
    auto r = getLocalBounds().reduced (6);

    auto left = r.removeFromLeft (74);
    title.setBounds (left.removeFromTop (18));
    auto cell = [] (juce::Rectangle<int> area, juce::Component& c, juce::Label& l)
    {
        l.setBounds (area.removeFromBottom (13));
        c.setBounds (area.reduced (3));
    };
    limiterBtn->setBounds (left.removeFromBottom (22));
    left.removeFromBottom (4);
    const int kh = left.getHeight() / 3;
    cell (left.removeFromTop (kh), inGainK, inGainL);
    cell (left.removeFromTop (kh), xLowK,   xLowL);
    cell (left,                    xHighK,  xHighL);

    r.removeFromLeft (6);
    const int bw = r.getWidth() / MultibandProcessor::numBands;
    for (int b = 0; b < MultibandProcessor::numBands; ++b)
        strips[b]->setBounds ((b < MultibandProcessor::numBands - 1) ? r.removeFromLeft (bw).reduced (2, 0)
                                                                     : r.reduced (2, 0));
}
