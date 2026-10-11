#include "BuiltinSoundbeastifier.h"

namespace
{
    const SoundbeastProcessor::Tuning kTunings[4] =
    {
        { 140.0f, 3000.0f, { -14.0f, -16.0f, -18.0f }, { 3.0f, 3.0f, 4.0f },
          { 15.0f, 10.0f, 5.0f }, { 150.0f, 120.0f, 80.0f }, { 1.2f, 1.3f, 1.5f }, 1.0f },
        { 200.0f, 4500.0f, { -12.0f, -15.0f, -20.0f }, { 3.0f, 3.0f, 5.0f },
          { 20.0f, 12.0f, 4.0f }, { 160.0f, 110.0f, 60.0f }, { 1.1f, 1.3f, 1.8f }, 1.0f },
        { 120.0f, 2200.0f, { -16.0f, -13.0f, -16.0f }, { 3.0f, 4.0f, 3.0f },
          { 18.0f, 8.0f, 8.0f }, { 140.0f, 90.0f, 100.0f }, { 1.2f, 1.6f, 1.3f }, 1.0f },
        { 100.0f, 2600.0f, { -20.0f, -22.0f, -24.0f }, { 5.0f, 6.0f, 6.0f },
          { 8.0f, 5.0f, 3.0f }, { 100.0f, 80.0f, 50.0f }, { 1.6f, 1.9f, 2.2f }, 1.1f }
    };
}

const SoundbeastProcessor::Tuning& SoundbeastProcessor::tuning (int presetIndex)
{
    return kTunings[juce::jlimit (0, 3, presetIndex)];
}

SoundbeastProcessor::SoundbeastProcessor()
{
    addParameter (amount = new juce::AudioParameterFloat (
        juce::ParameterID { "amount", 1 }, "Amount",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.5f));
    addParameter (preset = new juce::AudioParameterChoice (
        juce::ParameterID { "preset", 1 }, "Preset",
        juce::StringArray { "A", "B", "C", "D" }, 0));
}

void SoundbeastProcessor::prepareToPlay (double sampleRate, int blockSize)
{
    sr = sampleRate > 0 ? sampleRate : 44100.0;
    juce::dsp::ProcessSpec spec { sr, (juce::uint32) juce::jmax (1, blockSize), 1 };
    for (auto& s : split) s.prepare (spec);
    for (auto& c : comp) c.prepare (sr);
}

void SoundbeastProcessor::reset()
{
    for (auto& s : split) s.reset();
    for (auto& c : comp) c.reset();
}

void SoundbeastProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    if (sr <= 0.0 || buffer.getNumSamples() == 0)
        return;

    const int n   = buffer.getNumSamples();
    const int nch = juce::jmin (2, buffer.getNumChannels());
    const float amt = amount->get();

    const Tuning& t = tuning (preset->getIndex());
    for (auto& s : split) s.setFrequencies (t.xLow, t.xHigh);
    for (int b = 0; b < numBands; ++b)
        comp[b].setTimes (t.att[b], t.rel[b]);

    float effThres[numBands], effDrive[numBands];
    for (int b = 0; b < numBands; ++b)
    {
        effThres[b] = t.thres[b] * amt;
        effDrive[b] = 1.0f + (t.drive[b] - 1.0f) * amt;
    }
    const float outG = 1.0f + (t.outGain - 1.0f) * amt;

    float* chL = buffer.getWritePointer (0);
    float* chR = nch > 1 ? buffer.getWritePointer (1) : chL;

    float peakRed = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        const float dryL = chL[i];
        const float dryR = chR[i];

        float bandS[numBands][2];
        for (int ch = 0; ch < nch; ++ch)
            split[ch].process (ch == 0 ? dryL : dryR, bandS[0][ch], bandS[1][ch], bandS[2][ch]);
        if (nch == 1)
            for (int b = 0; b < numBands; ++b) bandS[b][1] = bandS[b][0];

        float wetL = 0.0f, wetR = 0.0f;
        for (int b = 0; b < numBands; ++b)
        {
            float l = bandS[b][0], r = bandS[b][1];
            const float detect = juce::jmax (std::abs (l), std::abs (r));
            const float g = comp[b].process (detect, effThres[b], 6.0f, t.ratio[b]);
            l *= g; r *= g;
            peakRed = juce::jmax (peakRed, comp[b].currentReductionDb());

            const float d = effDrive[b];
            l = std::tanh (l * d) / d;
            r = std::tanh (r * d) / d;
            wetL += l; wetR += r;
        }
        wetL *= outG; wetR *= outG;

        chL[i] = dryL + amt * (wetL - dryL);
        if (nch > 1) chR[i] = dryR + amt * (wetR - dryR);
    }

    activity.store (peakRed, std::memory_order_relaxed);
}

juce::AudioProcessorEditor* SoundbeastProcessor::createEditor()
{
    return new SoundbeastEditor (*this);
}

struct SoundbeastEditor::PresetRow : public juce::Component
{
    PresetRow (juce::AudioParameterChoice& p)
        : att (p, [this] (float v) { current = (int) std::round (v); repaint(); })
    {
        att.sendInitialUpdate();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int i = juce::jlimit (0, 3, (int) ((float) e.x / (float) getWidth() * 4.0f));
        att.setValueAsCompleteGesture ((float) i);
    }

    void paint (juce::Graphics& g) override
    {
        static const char* letters[4] = { "A", "B", "C", "D" };
        const float cw = (float) getWidth() / 4.0f;
        for (int i = 0; i < 4; ++i)
        {
            juce::Rectangle<float> cell (i * cw, 0.0f, cw, (float) getHeight());
            auto box = cell.reduced (3.0f);
            const bool on = (i == current);
            g.setColour (on ? Colors::raised : Colors::panelDark);
            g.fillRoundedRectangle (box, 3.0f);
            g.setColour (on ? Colors::accent : Colors::outline);
            g.drawRoundedRectangle (box, 3.0f, 1.0f);

            g.setColour (on ? Colors::text : Colors::textDim);
            g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
            g.drawText (letters[i], box.withTrimmedBottom (8.0f), juce::Justification::centred, false);

            const float r = 2.5f;
            juce::Rectangle<float> dot (box.getCentreX() - r, box.getBottom() - 8.0f, r * 2.0f, r * 2.0f);
            g.setColour (on ? Colors::accent : Colors::panelBg.darker (0.3f));
            g.fillEllipse (dot);
        }
    }

    int current = 0;
    juce::ParameterAttachment att;
};

SoundbeastEditor::SoundbeastEditor (SoundbeastProcessor& p)
    : juce::AudioProcessorEditor (p), proc (p)
{
    title.setText ("SOUNDBEASTIFIER", juce::dontSendNotification);
    title.setJustificationType (juce::Justification::centred);
    title.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
    title.setColour (juce::Label::textColourId, Colors::accent);
    addAndMakeVisible (title);

    amountK.setDoubleClickReturnValue (true, 0.5);
    addAndMakeVisible (amountK);

    amountL.setText ("AMOUNT", juce::dontSendNotification);
    amountL.setJustificationType (juce::Justification::centred);
    amountL.setFont (juce::Font (juce::FontOptions (10.0f)));
    amountL.setColour (juce::Label::textColourId, Colors::textDim);
    addAndMakeVisible (amountL);

    presetRow = std::make_unique<PresetRow> (*proc.preset);
    addAndMakeVisible (*presetRow);

    setSize (320, 250);
    startTimerHz (30);
}

SoundbeastEditor::~SoundbeastEditor() { stopTimer(); }

void SoundbeastEditor::timerCallback()
{
    const float target = juce::jlimit (0.0f, 1.0f, proc.activity.load (std::memory_order_relaxed) / 12.0f);
    glow += (target - glow) * 0.4f;
    repaint();
}

void SoundbeastEditor::paint (juce::Graphics& g)
{
    g.fillAll (Colors::panelBg);

    auto kb = amountK.getBounds().toFloat();
    const float radius = juce::jmin (kb.getWidth(), kb.getHeight()) * 0.5f + 8.0f;
    const auto c = kb.getCentre();
    const float a0 = juce::MathConstants<float>::pi * 1.2f;
    const float a1 = juce::MathConstants<float>::pi * 2.8f;
    const float amt = proc.amount->get();

    juce::Path track;
    track.addCentredArc (c.x, c.y, radius, radius, 0.0f, a0, a1, true);
    g.setColour (Colors::panelDark);
    g.strokePath (track, juce::PathStrokeType (4.0f));

    juce::Path val;
    val.addCentredArc (c.x, c.y, radius, radius, 0.0f, a0, a0 + (a1 - a0) * amt, true);
    g.setColour (Colors::accent.withAlpha (0.55f + 0.45f * glow));
    g.strokePath (val, juce::PathStrokeType (4.0f));
}

void SoundbeastEditor::resized()
{
    auto r = getLocalBounds().reduced (10);
    title.setBounds (r.removeFromTop (18));
    presetRow->setBounds (r.removeFromBottom (34));
    amountL.setBounds (r.removeFromBottom (16));

    const int d = juce::jmin (r.getWidth(), r.getHeight()) - 24;
    amountK.setBounds (r.withSizeKeepingCentre (d, d));
}
