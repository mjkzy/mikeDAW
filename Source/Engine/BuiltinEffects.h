#pragma once
#include <JuceHeader.h>
#include "../UI/AppLookAndFeel.h"
#include "../UI/ValueKnob.h"

class BuiltinEffectBase : public juce::AudioProcessor
{
public:
    BuiltinEffectBase() : juce::AudioProcessor (BusesProperties()
                            .withInput ("In", juce::AudioChannelSet::stereo(), true)
                            .withOutput ("Out", juce::AudioChannelSet::stereo(), true)) {}

    const juce::String getName() const override            { return effectName(); }
    virtual juce::String effectName() const = 0;

    bool acceptsMidi() const override                       { return false; }
    bool producesMidi() const override                      { return false; }
    double getTailLengthSeconds() const override            { return 0.0; }
    int getNumPrograms() override                           { return 1; }
    int getCurrentProgram() override                        { return 0; }
    void setCurrentProgram (int) override                   {}
    const juce::String getProgramName (int) override        { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    bool hasEditor() const override                         { return true; }
    juce::AudioProcessorEditor* createEditor() override     { return new juce::GenericAudioProcessorEditor (*this); }
    void releaseResources() override                        {}

    void getStateInformation (juce::MemoryBlock& dest) override
    {
        juce::MemoryOutputStream out (dest, false);
        for (auto* p : getParameters())
            out.writeFloat (p->getValue());
    }

    void setStateInformation (const void* data, int size) override
    {
        juce::MemoryInputStream in (data, (size_t) size, false);
        for (auto* p : getParameters())
        {
            if (in.getNumBytesRemaining() < 4) break;
            p->setValueNotifyingHost (in.readFloat());
        }
    }
};

class PlaceholderSynth : public juce::AudioProcessor
{
public:
    PlaceholderSynth() : juce::AudioProcessor (BusesProperties()
                            .withOutput ("Out", juce::AudioChannelSet::stereo(), true)) {}

    const juce::String getName() const override      { return "Placeholder Piano"; }
    bool acceptsMidi() const override                { return true; }
    bool producesMidi() const override               { return false; }
    double getTailLengthSeconds() const override     { return 1.0; }
    int getNumPrograms() override                    { return 1; }
    int getCurrentProgram() override                 { return 0; }
    void setCurrentProgram (int) override            {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    bool hasEditor() const override                  { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}
    void releaseResources() override {}

    void prepareToPlay (double sampleRate, int) override
    {
        sr = sampleRate;
        for (auto& v : voices) v = {};
    }

    void reset() override
    {
        for (auto& v : voices) v = {};
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        buffer.clear();

        int nextEvent = 0;
        auto it = midi.cbegin();
        auto end = midi.cend();

        for (int s = 0; s < buffer.getNumSamples(); ++s)
        {
            while (it != end && (*it).samplePosition <= s)
            {
                handleMidi ((*it).getMessage());
                ++it;
                juce::ignoreUnused (nextEvent);
            }

            float mono = 0.0f;
            for (auto& v : voices)
            {
                if (! v.active)
                    continue;
                v.phase += v.inc;
                if (v.phase >= 1.0f) v.phase -= 1.0f;

                float tri = 2.0f * std::abs (2.0f * v.phase - 1.0f) - 1.0f;
                float h2 = std::sin (v.phase * juce::MathConstants<float>::twoPi * 2.0f) * 0.18f;
                float tone = tri * 0.5f + h2;

                if (v.releasing)
                    v.env *= v.relCoeff;
                else if (v.env < 1.0f && v.attacking)
                {
                    v.env += v.attInc;
                    if (v.env >= 1.0f) { v.env = 1.0f; v.attacking = false; }
                }
                else
                    v.env *= v.decCoeff;

                mono += tone * v.env * v.gain;
                if (v.env < 0.0003f)
                    v.active = false;
            }

            mono = std::tanh (mono * 0.8f);
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                buffer.getWritePointer (ch)[s] += mono;
        }
    }

private:
    struct Voice
    {
        bool active = false, attacking = false, releasing = false;
        int note = -1;
        float phase = 0.0f, inc = 0.0f, env = 0.0f, gain = 0.0f;
        float attInc = 0.0f, decCoeff = 0.0f, relCoeff = 0.0f;
    };

    void handleMidi (const juce::MidiMessage& m)
    {
        if (m.isNoteOn())
            startVoice (m.getNoteNumber(), m.getFloatVelocity());
        else if (m.isNoteOff() || (m.isNoteOn() && m.getVelocity() == 0))
            stopVoice (m.getNoteNumber());
        else if (m.isAllNotesOff() || m.isAllSoundOff())
            for (auto& v : voices) v.releasing = true;
    }

    void startVoice (int note, float velocity)
    {
        Voice* slot = nullptr;
        for (auto& v : voices)
            if (! v.active) { slot = &v; break; }
        if (slot == nullptr)
        {
            slot = &voices[0];
            for (auto& v : voices) if (v.env < slot->env) slot = &v;
        }

        double freq = juce::MidiMessage::getMidiNoteInHertz (note);
        *slot = {};
        slot->active = true;
        slot->attacking = true;
        slot->note = note;
        slot->inc = (float) (freq / juce::jmax (1.0, sr));
        slot->gain = 0.22f + 0.55f * velocity;
        slot->attInc = 1.0f / juce::jmax (1.0f, (float) (sr * 0.004));
        slot->decCoeff = std::exp (-1.0f / (float) (sr * 1.1));
        slot->relCoeff = std::exp (-1.0f / (float) (sr * 0.12));
    }

    void stopVoice (int note)
    {
        for (auto& v : voices)
            if (v.active && v.note == note)
                v.releasing = true;
    }

    static constexpr int maxVoices = 24;
    std::array<Voice, maxVoices> voices;
    double sr = 44100.0;
};

class GainProcessor : public BuiltinEffectBase
{
public:
    GainProcessor()
    {
        addParameter (gainDb = new juce::AudioParameterFloat (
            juce::ParameterID { "gain", 1 }, "Gain (dB)",
            juce::NormalisableRange<float> (-48.0f, 12.0f, 0.1f), 0.0f));
    }

    juce::String effectName() const override { return "Gain"; }

    void prepareToPlay (double, int) override { smoothed.reset (64); smoothed.setCurrentAndTargetValue (1.0f); }

    void reset() override { smoothed.setCurrentAndTargetValue (smoothed.getTargetValue()); }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        smoothed.setTargetValue (juce::Decibels::decibelsToGain (gainDb->get(), -48.0f));
        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            float g = smoothed.getNextValue();
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                buffer.getWritePointer (ch)[i] *= g;
        }
    }

private:
    juce::AudioParameterFloat* gainDb = nullptr;
    juce::SmoothedValue<float> smoothed { 1.0f };
};

class SimpleEQProcessor : public BuiltinEffectBase
{
public:
    SimpleEQProcessor()
    {
        auto range = juce::NormalisableRange<float> (-15.0f, 15.0f, 0.1f);
        addParameter (low  = new juce::AudioParameterFloat (juce::ParameterID { "low",  1 }, "Low (dB)",  range, 0.0f));
        addParameter (mid  = new juce::AudioParameterFloat (juce::ParameterID { "mid",  1 }, "Mid (dB)",  range, 0.0f));
        addParameter (high = new juce::AudioParameterFloat (juce::ParameterID { "high", 1 }, "High (dB)", range, 0.0f));
    }

    juce::String effectName() const override { return "EQ 3-Band"; }

    void prepareToPlay (double sampleRate, int blockSize) override
    {
        sr = sampleRate;
        juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, 2 };
        lowShelf.prepare (spec); peak.prepare (spec); highShelf.prepare (spec);
        lastLow = lastMid = lastHigh = -1000.0f;
        updateCoefficients();
    }

    void reset() override { lowShelf.reset(); peak.reset(); highShelf.reset(); }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (sr <= 0) return;
        updateCoefficients();
        juce::dsp::AudioBlock<float> block (buffer);
        auto stereo = block.getSubsetChannelBlock (0, juce::jmin ((size_t) 2, block.getNumChannels()));
        juce::dsp::ProcessContextReplacing<float> ctx (stereo);
        lowShelf.process (ctx); peak.process (ctx); highShelf.process (ctx);
    }

private:
    void updateCoefficients()
    {
        if (juce::approximatelyEqual (lastLow, low->get())
            && juce::approximatelyEqual (lastMid, mid->get())
            && juce::approximatelyEqual (lastHigh, high->get()))
            return;

        lastLow = low->get(); lastMid = mid->get(); lastHigh = high->get();
        *lowShelf.state  = *juce::dsp::IIR::Coefficients<float>::makeLowShelf  (sr, 220.0f,  0.7f, juce::Decibels::decibelsToGain (lastLow));
        *peak.state      = *juce::dsp::IIR::Coefficients<float>::makePeakFilter (sr, 1000.0f, 0.6f, juce::Decibels::decibelsToGain (lastMid));
        *highShelf.state = *juce::dsp::IIR::Coefficients<float>::makeHighShelf (sr, 4500.0f, 0.7f, juce::Decibels::decibelsToGain (lastHigh));
    }

    using Filter = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>>;
    Filter lowShelf, peak, highShelf;
    juce::AudioParameterFloat* low = nullptr;
    juce::AudioParameterFloat* mid = nullptr;
    juce::AudioParameterFloat* high = nullptr;
    double sr = 0;
    float lastLow = 0, lastMid = 0, lastHigh = 0;
};

class SoftClipperProcessor : public BuiltinEffectBase
{
public:
    SoftClipperProcessor()
    {
        addParameter (thres = new juce::AudioParameterFloat (
            juce::ParameterID { "thres", 1 }, "Threshold",
            juce::NormalisableRange<float> (0.10f, 1.00f, 0.001f), 0.80f));
        addParameter (post = new juce::AudioParameterFloat (
            juce::ParameterID { "post", 1 }, "Post gain",
            juce::NormalisableRange<float> (0.0f, 1.6f, 0.001f), 1.0f));
    }

    juce::String effectName() const override { return "Soft Clipper"; }

    void prepareToPlay (double, int) override {}

    void reset() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        const float t = thres->get();
        const float p = post->get();
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            auto* d = buffer.getWritePointer (ch);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                d[i] = shape (d[i], t) * p;
        }
    }

    static float shape (float x, float t)
    {
        const float a = std::abs (x);
        if (a <= t)
            return x;
        if (t >= 0.999f)
            return juce::jlimit (-1.0f, 1.0f, x);
        const float s = x < 0.0f ? -1.0f : 1.0f;
        return s * (t + (1.0f - t) * std::tanh ((a - t) / (1.0f - t)));
    }

    juce::AudioProcessorEditor* createEditor() override;

    juce::AudioParameterFloat* thres = nullptr;
    juce::AudioParameterFloat* post  = nullptr;
};

class SoftClipperEditor : public juce::AudioProcessorEditor,
                          private juce::Slider::Listener
{
public:
    SoftClipperEditor (SoftClipperProcessor& p)
        : juce::AudioProcessorEditor (p), proc (p)
    {
        auto setupKnob = [this] (juce::Slider& k)
        {
            k.addListener (this);
            addAndMakeVisible (k);
        };
        setupKnob (thresKnob);
        setupKnob (postKnob);
        thresKnob.setDoubleClickReturnValue (true, 0.80);
        postKnob.setDoubleClickReturnValue (true, 1.0);

        auto setupLabel = [this] (juce::Label& l, const juce::String& text)
        {
            l.setText (text, juce::dontSendNotification);
            l.setJustificationType (juce::Justification::centred);
            l.setFont (juce::Font (juce::FontOptions (11.0f)));
            l.setColour (juce::Label::textColourId, Colors::textDim);
            addAndMakeVisible (l);
        };
        setupLabel (thresLabel, "THRES");
        setupLabel (postLabel, "POST");

        addAndMakeVisible (curve);

        setSize (420, 150);
    }

    void sliderValueChanged (juce::Slider*) override { curve.repaint(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colors::panelBg);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8);
        auto graph = r.removeFromRight (juce::jmax (r.getHeight(), r.getWidth() / 2));
        curve.setBounds (graph);

        r.removeFromRight (8);
        auto knobH = r.getHeight() / 2;
        auto layout = [] (juce::Rectangle<int> area, juce::Slider& k, juce::Label& l)
        {
            l.setBounds (area.removeFromBottom (14));
            k.setBounds (area.reduced (4));
        };
        layout (r.removeFromTop (knobH), thresKnob, thresLabel);
        layout (r, postKnob, postLabel);
    }

private:
    struct CurveView : public juce::Component
    {
        SoftClipperProcessor& proc;
        CurveView (SoftClipperProcessor& p) : proc (p) {}

        void paint (juce::Graphics& g) override
        {
            auto b = getLocalBounds().toFloat();
            g.setColour (Colors::panelDark.darker (0.4f));
            g.fillRoundedRectangle (b, 3.0f);
            g.setColour (Colors::outline);
            g.drawRoundedRectangle (b, 3.0f, 1.0f);

            auto in = b.reduced (4.0f);
            g.setColour (Colors::gridLine.withAlpha (0.5f));
            for (int i = 1; i < 4; ++i)
            {
                float fx = in.getX() + in.getWidth() * (float) i / 4.0f;
                float fy = in.getY() + in.getHeight() * (float) i / 4.0f;
                g.drawVerticalLine ((int) fx, in.getY(), in.getBottom());
                g.drawHorizontalLine ((int) fy, in.getX(), in.getRight());
            }

            const float t = proc.thres->get();
            juce::Path path;
            const int steps = 64;
            for (int i = 0; i <= steps; ++i)
            {
                float x = (float) i / (float) steps;
                float y = SoftClipperProcessor::shape (x, t);
                float px = in.getX() + x * in.getWidth();
                float py = in.getBottom() - juce::jlimit (0.0f, 1.0f, y) * in.getHeight();
                if (i == 0) path.startNewSubPath (px, py);
                else        path.lineTo (px, py);
            }
            g.setColour (Colors::accent);
            g.strokePath (path, juce::PathStrokeType (2.0f));
        }
    };

    SoftClipperProcessor& proc;
    ValueKnob thresKnob, postKnob;
    juce::Label thresLabel, postLabel;
    CurveView curve { proc };

    juce::SliderParameterAttachment thresAtt { *proc.thres, thresKnob };
    juce::SliderParameterAttachment postAtt  { *proc.post,  postKnob };
};

inline juce::AudioProcessorEditor* SoftClipperProcessor::createEditor()
{
    return new SoftClipperEditor (*this);
}
