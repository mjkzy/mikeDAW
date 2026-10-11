#include "BuiltinDelay.h"

namespace
{
    const float kDivisions[] = { 0.125f, 0.25f, 1.0f / 3.0f, 0.5f, 2.0f / 3.0f, 0.75f,
                                 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f };
    constexpr int kNumDiv = (int) (sizeof (kDivisions) / sizeof (float));

    float quantizeBeats (float t)
    {
        int idx = juce::jlimit (0, kNumDiv - 1, (int) std::round (t * (float) (kNumDiv - 1)));
        return kDivisions[idx];
    }

    float freeMs (float t)
    {
        return std::pow (4000.0f, juce::jlimit (0.0f, 1.0f, t));
    }

    juce::NormalisableRange<float> logRange (float lo, float hi, float centre)
    {
        auto r = juce::NormalisableRange<float> (lo, hi);
        r.setSkewForCentre (centre);
        return r;
    }
}

DelayProcessor::DelayProcessor()
{
    addParameter (inWet = new juce::AudioParameterFloat (
        juce::ParameterID { "inWet", 1 }, "Input",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 1.0f));

    addParameter (time = new juce::AudioParameterFloat (
        juce::ParameterID { "time", 1 }, "Time",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.0001f), 5.0f / 12.0f));

    addParameter (tempoSync = new juce::AudioParameterBool (
        juce::ParameterID { "tempoSync", 1 }, "Tempo sync", true));

    addParameter (keepPitch = new juce::AudioParameterBool (
        juce::ParameterID { "keepPitch", 1 }, "Keep pitch", false));

    addParameter (smoothing = new juce::AudioParameterFloat (
        juce::ParameterID { "smoothing", 1 }, "Smoothing",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.5f));

    addParameter (offset = new juce::AudioParameterFloat (
        juce::ParameterID { "offset", 1 }, "Offset",
        juce::NormalisableRange<float> (-1.0f, 1.0f, 0.001f), 0.0f));

    addParameter (model = new juce::AudioParameterChoice (
        juce::ParameterID { "model", 1 }, "Model",
        juce::StringArray { "Mono", "Stereo", "Ping pong", "Off" }, 2));

    addParameter (stereo = new juce::AudioParameterFloat (
        juce::ParameterID { "stereo", 1 }, "Stereo",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.5f));

    addParameter (fbLevel = new juce::AudioParameterFloat (
        juce::ParameterID { "fbLevel", 1 }, "Feedback",
        juce::NormalisableRange<float> (0.0f, 1.1f, 0.001f), 0.5f));

    addParameter (fbMode = new juce::AudioParameterChoice (
        juce::ParameterID { "fbMode", 1 }, "FB Filter",
        juce::StringArray { "LP", "HP", "BP", "Off" }, 0));

    addParameter (fbCutoff = new juce::AudioParameterFloat (
        juce::ParameterID { "fbCutoff", 1 }, "Cutoff",
        logRange (100.0f, 21000.0f, 7000.0f), 7000.0f));

    addParameter (fbRes = new juce::AudioParameterFloat (
        juce::ParameterID { "fbRes", 1 }, "Res",
        juce::NormalisableRange<float> (0.5f, 8.0f, 0.001f), 0.707f));

    addParameter (smpRate = new juce::AudioParameterFloat (
        juce::ParameterID { "smpRate", 1 }, "Smp rate",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 1.0f));

    addParameter (bits = new juce::AudioParameterFloat (
        juce::ParameterID { "bits", 1 }, "Bits",
        juce::NormalisableRange<float> (1.0f, 16.0f, 1.0f), 16.0f));

    addParameter (distMode = new juce::AudioParameterChoice (
        juce::ParameterID { "distMode", 1 }, "Dist mode",
        juce::StringArray { "Off", "Limit", "Sat" }, 0));

    addParameter (distLevel = new juce::AudioParameterFloat (
        juce::ParameterID { "distLevel", 1 }, "Dist level",
        juce::NormalisableRange<float> (0.0f, 2.0f, 0.001f), 1.0f));

    addParameter (distKnee = new juce::AudioParameterFloat (
        juce::ParameterID { "distKnee", 1 }, "Knee",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.5f));

    addParameter (distSym = new juce::AudioParameterFloat (
        juce::ParameterID { "distSym", 1 }, "Symmetry",
        juce::NormalisableRange<float> (-1.0f, 1.0f, 0.001f), 0.0f));

    addParameter (modRate = new juce::AudioParameterFloat (
        juce::ParameterID { "modRate", 1 }, "Mod rate",
        logRange (0.01f, 10.0f, 0.5f), 0.5f));

    addParameter (modTime = new juce::AudioParameterFloat (
        juce::ParameterID { "modTime", 1 }, "Mod time",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));

    addParameter (modCutoff = new juce::AudioParameterFloat (
        juce::ParameterID { "modCutoff", 1 }, "Mod cutoff",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));

    addParameter (diffLevel = new juce::AudioParameterFloat (
        juce::ParameterID { "diffLevel", 1 }, "Diffusion",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));

    addParameter (diffSpread = new juce::AudioParameterFloat (
        juce::ParameterID { "diffSpread", 1 }, "Spread",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.5f));

    addParameter (tone = new juce::AudioParameterFloat (
        juce::ParameterID { "tone", 1 }, "Tone",
        juce::NormalisableRange<float> (-1.0f, 1.0f, 0.001f), 0.0f));

    addParameter (wet = new juce::AudioParameterFloat (
        juce::ParameterID { "wet", 1 }, "Wet",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.5f));

    addParameter (dry = new juce::AudioParameterFloat (
        juce::ParameterID { "dry", 1 }, "Dry",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 1.0f));
}

void DelayProcessor::prepareToPlay (double sampleRate, int blockSize)
{
    sr = sampleRate > 0 ? sampleRate : 44100.0;
    bufSize = (int) (8.0 * sr) + 4;
    writePos = 0;
    lfoPhase = 0.0f;

    const int apMax = (int) (0.045 * sr) + 4;
    for (auto& c : chan)
    {
        c.buf.assign ((size_t) bufSize, 0.0f);
        c.curDelay = 1.0f;
        c.xfActive = false;
        c.xfOld = c.xfNew = 1.0f;
        c.xfPos = 0; c.xfLen = 1;
        c.shHold = 0.0f; c.shPhase = 1.0f;
        c.dcX1 = c.dcY1 = 0.0f;
        for (auto& a : c.ap)
            a.prepare (apMax);
    }

    juce::dsp::ProcessSpec spec { sr, (juce::uint32) juce::jmax (1, blockSize), 2 };
    fbFilter.prepare (spec);   fbFilter.reset();
    toneFilter.prepare (spec); toneFilter.reset();

    inWetSm.reset (sr, 0.01); wetSm.reset (sr, 0.01);
    drySm.reset (sr, 0.01);   fbSm.reset (sr, 0.01);
    inWetSm.setCurrentAndTargetValue (inWet->get());
    wetSm.setCurrentAndTargetValue (wet->get());
    drySm.setCurrentAndTargetValue (dry->get());
    fbSm.setCurrentAndTargetValue (fbLevel->get());
}

void DelayProcessor::reset()
{
    writePos = 0;
    lfoPhase = 0.0f;

    for (auto& c : chan)
    {
        std::fill (c.buf.begin(), c.buf.end(), 0.0f);
        c.curDelay = 1.0f;
        c.xfActive = false;
        c.xfOld = c.xfNew = 1.0f;
        c.xfPos = 0; c.xfLen = 1;
        c.shHold = 0.0f; c.shPhase = 1.0f;
        c.dcX1 = c.dcY1 = 0.0f;
        for (auto& a : c.ap)
            a.reset();
    }

    fbFilter.reset();
    toneFilter.reset();

    inWetSm.setCurrentAndTargetValue (inWetSm.getTargetValue());
    wetSm.setCurrentAndTargetValue (wetSm.getTargetValue());
    drySm.setCurrentAndTargetValue (drySm.getTargetValue());
    fbSm.setCurrentAndTargetValue (fbSm.getTargetValue());
}

float DelayProcessor::readDelay (const std::vector<float>& buf, int wp, float delaySamples) const
{
    const int n = (int) buf.size();
    float rp = (float) wp - delaySamples;
    while (rp < 0.0f)        rp += (float) n;
    while (rp >= (float) n)  rp -= (float) n;

    const int i1 = (int) rp;
    const float f = rp - (float) i1;
    const int i0 = (i1 - 1 + n) % n;
    const int i2 = (i1 + 1) % n;
    const int i3 = (i1 + 2) % n;

    const float a = buf[(size_t) i0], b = buf[(size_t) i1];
    const float c = buf[(size_t) i2], d = buf[(size_t) i3];

    const float c0 = b;
    const float c1 = 0.5f * (c - a);
    const float c2 = a - 2.5f * b + 2.0f * c - 0.5f * d;
    const float c3 = 0.5f * (d - a) + 1.5f * (b - c);
    return ((c3 * f + c2) * f + c1) * f + c0;
}

void DelayProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    if (bufSize <= 0) return;

    const int numSamples  = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    if (numChannels < 1) return;

    double bpm = 140.0;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
            if (auto b = pos->getBpm())
                bpm = *b;

    const bool  sync = tempoSync->get();
    const bool  keep = keepPitch->get();
    const int   mdl  = model->getIndex();
    const int   fbm  = fbMode->getIndex();
    const int   dst  = distMode->getIndex();
    const float off  = offset->get();
    const float sm   = smoothing->get();
    const float st   = stereo->get();
    const float res  = fbRes->get();
    const float smpR = smpRate->get();
    const int   nBits= juce::jlimit (1, 16, (int) std::round (bits->get()));
    const float dLevel = distLevel->get();
    const float dKnee  = distKnee->get();
    const float dSym   = distSym->get();
    const float mRate  = modRate->get();
    const float mTime  = modTime->get();
    const float mCut   = modCutoff->get();
    const float diffL  = diffLevel->get();
    const float diffS  = diffSpread->get();
    const float ton    = tone->get();
    const float baseCut= fbCutoff->get();

    inWetSm.setTargetValue (inWet->get());
    wetSm.setTargetValue (wet->get());
    drySm.setTargetValue (dry->get());
    fbSm.setTargetValue (fbLevel->get());

    const float ms = sync ? quantizeBeats (time->get()) * (float) (60000.0 / juce::jmax (1.0, bpm))
                          : freeMs (time->get());
    const float maxD = (float) (bufSize - 8);
    const float targetL = juce::jlimit (1.0f, maxD, ms * 0.001f * (float) sr);
    const float targetR = juce::jlimit (1.0f, maxD, ms * (1.0f + 0.5f * off) * 0.001f * (float) sr);
    const float target[2] = { targetL, targetR };

    const float tau   = 0.02f * std::pow (100.0f, sm);
    const float slew  = 1.0f - std::exp (-1.0f / (tau * (float) sr));
    const int   xfLen = juce::jmax (1, (int) (tau * (float) sr));

    using FT = juce::dsp::StateVariableTPTFilterType;
    if (fbm == 0)      fbFilter.setType (FT::lowpass);
    else if (fbm == 1) fbFilter.setType (FT::highpass);
    else               fbFilter.setType (FT::bandpass);
    fbFilter.setResonance (res);

    const bool toneLP = ton < -0.02f;
    const bool toneHP = ton >  0.02f;
    if (toneLP)      { toneFilter.setType (FT::lowpass);  toneFilter.setCutoffFrequency (juce::jmap (ton, 0.0f, -1.0f, 21000.0f, 200.0f)); }
    else if (toneHP) { toneFilter.setType (FT::highpass); toneFilter.setCutoffFrequency (juce::jmap (ton, 0.0f,  1.0f, 20.0f, 8000.0f)); }
    toneFilter.setResonance (0.707f);

    const float apBaseMs[numAllpass] = { 5.0f, 8.0f, 13.0f, 21.0f };
    const float spreadScale = 0.5f + diffS;
    for (auto& c : chan)
        for (int k = 0; k < numAllpass; ++k)
            c.ap[k].len = juce::jmax (1, (int) (apBaseMs[k] * spreadScale * 0.001f * (float) sr));
    const float apG = 0.6f;

    const float effRate = 500.0f + ((float) sr - 500.0f) * juce::jlimit (0.0f, 1.0f, smpR);
    const float bitStep = nBits >= 16 ? 0.0f : 1.0f / (float) (1 << (nBits - 1));
    const float phaseInc = juce::MathConstants<float>::twoPi * mRate / (float) sr;
    const float modTimeSamp = 0.005f * (float) sr * mTime;

    const int chR = numChannels > 1 ? 1 : 0;

    for (int s = 0; s < numSamples; ++s)
    {
        const float inL = buffer.getReadPointer (0)[s];
        const float inR = buffer.getReadPointer (chR)[s];

        const float lfo = std::sin (lfoPhase);
        lfoPhase += phaseInc;
        if (lfoPhase >= juce::MathConstants<float>::twoPi) lfoPhase -= juce::MathConstants<float>::twoPi;

        const float cutMod = juce::jlimit (20.0f, (float) sr * 0.49f,
                                           baseCut * std::pow (2.0f, lfo * 2.0f * mCut));
        fbFilter.setCutoffFrequency (cutMod);

        float tap[2];
        for (int ch = 0; ch < 2; ++ch)
        {
            auto& c = chan[ch];
            if (! keep)
            {
                c.curDelay += (target[ch] - c.curDelay) * slew;
                const float d = juce::jlimit (1.0f, maxD, c.curDelay + lfo * modTimeSamp);
                tap[ch] = readDelay (c.buf, writePos, d);
            }
            else
            {
                if (! c.xfActive && std::abs (target[ch] - c.curDelay) > 1.0f)
                {
                    c.xfActive = true; c.xfOld = c.curDelay; c.xfNew = target[ch];
                    c.xfPos = 0; c.xfLen = xfLen;
                }
                if (c.xfActive)
                {
                    const float x = (float) c.xfPos / (float) c.xfLen;
                    const float dOld = juce::jlimit (1.0f, maxD, c.xfOld + lfo * modTimeSamp);
                    const float dNew = juce::jlimit (1.0f, maxD, c.xfNew + lfo * modTimeSamp);
                    tap[ch] = readDelay (c.buf, writePos, dOld) * (1.0f - x)
                            + readDelay (c.buf, writePos, dNew) * x;
                    if (++c.xfPos >= c.xfLen) { c.xfActive = false; c.curDelay = c.xfNew; }
                }
                else
                {
                    const float d = juce::jlimit (1.0f, maxD, c.curDelay + lfo * modTimeSamp);
                    tap[ch] = readDelay (c.buf, writePos, d);
                }
            }
        }

        const float fbGain = fbSm.getNextValue();

        float fProc[2];
        for (int ch = 0; ch < 2; ++ch)
        {
            auto& c = chan[ch];
            float x = tap[ch];

            if (diffL > 0.0001f)
            {
                float dsig = x;
                for (int k = 0; k < numAllpass; ++k)
                    dsig = c.ap[k].process (dsig, apG);
                x = x * (1.0f - diffL) + dsig * diffL;
            }

            if (fbm != 3)
                x = fbFilter.processSample (ch, x);

            c.shPhase += effRate / (float) sr;
            if (c.shPhase >= 1.0f) { c.shPhase -= 1.0f; c.shHold = x; }
            x = c.shHold;

            if (bitStep > 0.0f)
                x = std::round (x / bitStep) * bitStep;

            if (dst != 0)
            {
                const float y = distShape (x, dst, dLevel, dKnee, dSym);
                const float out = y - c.dcX1 + 0.995f * c.dcY1;
                c.dcX1 = y; c.dcY1 = out;
                x = out;
            }

            x *= fbGain;
            fProc[ch] = juce::jlimit (-4.0f, 4.0f, x);
        }

        const float iw = inWetSm.getNextValue();
        float wL, wR;
        switch (mdl)
        {
            case 0: { const float im = 0.5f * (inL + inR) * iw; wL = im + fProc[0]; wR = im + fProc[1]; } break;
            case 1: wL = inL * iw + fProc[0]; wR = inR * iw + fProc[1]; break;
            case 2: wL = inL * iw + fProc[1]; wR = inR * iw + fProc[0]; break;
            default: wL = fProc[0]; wR = fProc[1]; break;
        }
        chan[0].buf[(size_t) writePos] = juce::jlimit (-4.0f, 4.0f, wL);
        chan[1].buf[(size_t) writePos] = juce::jlimit (-4.0f, 4.0f, wR);

        float wetL = tap[0], wetR = tap[1];
        if (toneLP || toneHP)
        {
            wetL = toneFilter.processSample (0, wetL);
            wetR = toneFilter.processSample (1, wetR);
        }
        float mid  = 0.5f * (wetL + wetR);
        float side = 0.5f * (wetL - wetR) * juce::jlimit (0.0f, 2.0f, st * 2.0f);
        wetL = mid + side; wetR = mid - side;

        const float wg = wetSm.getNextValue();
        const float dg = drySm.getNextValue();
        const float outL = inL * dg + wetL * wg;
        const float outR = inR * dg + wetR * wg;

        buffer.getWritePointer (0)[s] = outL;
        if (numChannels > 1)
            buffer.getWritePointer (1)[s] = outR;

        if (++writePos >= bufSize) writePos = 0;
    }
}

float DelayProcessor::distShape (float x, int mode, float level, float knee, float sym)
{
    const float drive = juce::jmax (0.0001f, level);
    const float xin   = x * drive + sym * 0.5f;
    float y;
    if (mode == 1)
    {
        const float hard = juce::jlimit (-1.0f, 1.0f, xin);
        const float soft = std::tanh (xin);
        y = hard * (1.0f - knee) + soft * knee;
    }
    else
    {
        y = std::tanh (xin);
    }
    y /= (drive > 1.0f ? drive : 1.0f);
    return y;
}

juce::AudioProcessorEditor* DelayProcessor::createEditor()
{
    return new DelayEditor (*this);
}

static void styleCaption (juce::Label& l, const juce::String& text)
{
    l.setText (text, juce::dontSendNotification);
    l.setJustificationType (juce::Justification::centred);
    l.setFont (juce::Font (juce::FontOptions (10.5f)));
    l.setColour (juce::Label::textColourId, Colors::textDim);
}

class DelayLedToggle : public juce::Component
{
public:
    DelayLedToggle (juce::AudioParameterBool& p, const juce::String& text)
        : caption (text),
          att (p, [this] (float v) { on = v > 0.5f; repaint(); })
    {
        att.sendInitialUpdate();
    }

    void paint (juce::Graphics& g) override
    {
        const float r  = 4.0f;
        const float cy = (float) getHeight() * 0.5f;
        juce::Rectangle<float> dot (3.0f, cy - r, r * 2.0f, r * 2.0f);
        if (on)
        {
            g.setColour (Colors::accent);
            g.fillEllipse (dot);
        }
        else
        {
            g.setColour (Colors::panelDark);
            g.fillEllipse (dot);
            g.setColour (Colors::outline);
            g.drawEllipse (dot, 1.0f);
        }
        g.setColour (on ? Colors::text : Colors::textDim);
        g.setFont (juce::Font (juce::FontOptions (9.5f)));
        const int tx = (int) dot.getRight() + 5;
        g.drawText (caption, tx, 0, getWidth() - tx, getHeight(),
                    juce::Justification::centredLeft, false);
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        att.setValueAsCompleteGesture (on ? 0.0f : 1.0f);
    }

private:
    juce::String caption;
    bool on = false;
    juce::ParameterAttachment att;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DelayLedToggle)
};

class DelayLedChoice : public juce::Component
{
public:
    DelayLedChoice (juce::AudioParameterChoice& p)
        : rows (p.choices),
          att (p, [this] (float v) { current = (int) std::round (v); if (onChange) onChange(); repaint(); })
    {
        att.sendInitialUpdate();
    }

    void paint (juce::Graphics& g) override
    {
        const int   n    = juce::jmax (1, rows.size());
        const float rowH = (float) getHeight() / (float) n;
        const float r    = 4.0f;
        g.setFont (juce::Font (juce::FontOptions (9.5f)));
        for (int i = 0; i < rows.size(); ++i)
        {
            const float cy = (float) i * rowH + rowH * 0.5f;
            const bool  on = (i == current);
            juce::Rectangle<float> dot (3.0f, cy - r, r * 2.0f, r * 2.0f);
            if (on)
            {
                g.setColour (Colors::accent);
                g.fillEllipse (dot);
            }
            else
            {
                g.setColour (Colors::panelDark);
                g.fillEllipse (dot);
                g.setColour (Colors::outline);
                g.drawEllipse (dot, 1.0f);
            }
            g.setColour (on ? Colors::text : Colors::textDim);
            const int tx = (int) dot.getRight() + 5;
            g.drawText (rows[i], tx, (int) ((float) i * rowH),
                        getWidth() - tx, (int) rowH,
                        juce::Justification::centredLeft, false);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int n = juce::jmax (1, rows.size());
        const int i = juce::jlimit (0, n - 1,
                                    (int) ((float) e.y / (float) getHeight() * (float) n));
        att.setValueAsCompleteGesture ((float) i);
    }

    std::function<void()> onChange;

private:
    juce::StringArray rows;
    int current = 0;
    juce::ParameterAttachment att;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DelayLedChoice)
};

class DelayDistCurve : public juce::Component
{
public:
    DelayDistCurve (DelayProcessor& p) : proc (p) {}

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
            const float fx = in.getX() + in.getWidth()  * (float) i / 4.0f;
            const float fy = in.getY() + in.getHeight() * (float) i / 4.0f;
            g.drawVerticalLine   ((int) fx, in.getY(), in.getBottom());
            g.drawHorizontalLine ((int) fy, in.getX(), in.getRight());
        }

        const int   mode  = proc.distMode->getIndex();
        const float level = proc.distLevel->get();
        const float knee  = proc.distKnee->get();
        const float sym   = proc.distSym->get();

        juce::Path path;
        const int steps = 96;
        for (int i = 0; i <= steps; ++i)
        {
            const float x = -1.0f + 2.0f * (float) i / (float) steps;
            float y = (mode == 0) ? x : DelayProcessor::distShape (x, mode, level, knee, sym);
            y = juce::jlimit (-1.0f, 1.0f, y);
            const float px = in.getX() + (x * 0.5f + 0.5f) * in.getWidth();
            const float py = in.getBottom() - (y * 0.5f + 0.5f) * in.getHeight();
            if (i == 0) path.startNewSubPath (px, py);
            else        path.lineTo (px, py);
        }
        g.setColour (mode == 0 ? Colors::textDim : Colors::accent);
        g.strokePath (path, juce::PathStrokeType (1.8f));
    }

private:
    DelayProcessor& proc;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DelayDistCurve)
};

DelayEditor::DelayEditor (DelayProcessor& p)
    : juce::AudioProcessorEditor (p), proc (p)
{
    inWetFader.setSliderStyle (juce::Slider::LinearVertical);
    inWetFader.setDoubleClickReturnValue (true, 1.0);
    addAndMakeVisible (inWetFader);
    styleCaption (inWetLabel, "WET");
    addAndMakeVisible (inWetLabel);

    timeK   = mk (*proc.time,   "TIME",   5.0 / 12.0);
    offsetK = mk (*proc.offset, "OFFSET", 0.0);
    syncLed = std::make_unique<DelayLedToggle> (*proc.tempoSync, "TEMPO SYNC");
    keepLed = std::make_unique<DelayLedToggle> (*proc.keepPitch, "KEEP PITCH");
    addAndMakeVisible (*syncLed);
    addAndMakeVisible (*keepLed);
    smoothFader.setSliderStyle (juce::Slider::LinearHorizontal);
    smoothFader.setDoubleClickReturnValue (true, 0.5);
    addAndMakeVisible (smoothFader);
    styleCaption (smoothLabel, "SMOOTHING");
    addAndMakeVisible (smoothLabel);

    modelLed = std::make_unique<DelayLedChoice> (*proc.model);
    addAndMakeVisible (*modelLed);
    stereoK = mk (*proc.stereo, "STEREO", 0.5);

    fbLevelK  = mk (*proc.fbLevel,  "LEVEL",    0.5);
    cutoffK   = mk (*proc.fbCutoff, "CUTOFF",   7000.0);
    resK      = mk (*proc.fbRes,    "RES",      0.707);
    smpK      = mk (*proc.smpRate,  "SMP RATE", 1.0);
    bitsK     = mk (*proc.bits,     "BITS",     16.0);
    fbModeLed = std::make_unique<DelayLedChoice> (*proc.fbMode);
    addAndMakeVisible (*fbModeLed);

    modRateK = mk (*proc.modRate,   "RATE",   0.5);
    modTimeK = mk (*proc.modTime,   "TIME",   0.0);
    modCutK  = mk (*proc.modCutoff, "CUTOFF", 0.0);

    diffLevelK  = mk (*proc.diffLevel,  "LEVEL",  0.0);
    diffSpreadK = mk (*proc.diffSpread, "SPREAD", 0.5);

    distLed = std::make_unique<DelayLedChoice> (*proc.distMode);
    addAndMakeVisible (*distLed);
    distLevelK = mk (*proc.distLevel, "LEVEL",    1.0);
    distKneeK  = mk (*proc.distKnee,  "KNEE",     0.5);
    distSymK   = mk (*proc.distSym,   "SYMMETRY", 0.0);
    distCurve  = std::make_unique<DelayDistCurve> (proc);
    addAndMakeVisible (*distCurve);

    distLed->onChange = [this] { distCurve->repaint(); };
    for (auto* k : { distLevelK.knob, distKneeK.knob, distSymK.knob })
        k->addListener (this);

    wetK  = mk (*proc.wet,  "WET",  0.5);
    toneK = mk (*proc.tone, "TONE", 0.0);
    dryK  = mk (*proc.dry,  "DRY",  1.0);

    setSize (820, 300);
}

DelayEditor::~DelayEditor() = default;

void DelayEditor::sliderValueChanged (juce::Slider*)
{
    if (distCurve != nullptr)
        distCurve->repaint();
}

DelayEditor::KnobCell DelayEditor::mk (juce::RangedAudioParameter& param, const juce::String& cap, double def)
{
    auto* k = new ValueKnob();
    knobs.add (k);
    addAndMakeVisible (k);
    knobAtts.add (new juce::SliderParameterAttachment (param, *k));
    k->setDoubleClickReturnValue (true, def);

    auto* l = new juce::Label();
    styleCaption (*l, cap);
    labels.add (l);
    addAndMakeVisible (l);

    return { k, l };
}

void DelayEditor::paint (juce::Graphics& g)
{
    g.fillAll (Colors::panelBg);

    auto section = [&g] (juce::Rectangle<int> r, const juce::String& title)
    {
        g.setColour (Colors::panelDark);
        g.fillRoundedRectangle (r.toFloat(), 3.0f);
        g.setColour (Colors::outline);
        g.drawRoundedRectangle (r.toFloat(), 3.0f, 1.0f);

        auto header = r.removeFromTop (16);
        g.setColour (Colors::header);
        g.fillRect (header.reduced (1, 1));
        g.setColour (Colors::accent);
        g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
        g.drawText (title, header.reduced (6, 0), juce::Justification::centredLeft, false);
    };

    section (secInput,    "INPUT");
    section (secTime,     "DELAY TIME");
    section (secModel,    "DELAY MODEL");
    section (secFeedback, "FEEDBACK");
    section (secMod,      "MODULATION");
    section (secDiff,     "DIFFUSION");
    section (secDist,     "FEEDBACK DISTORTION");
    section (secOutput,   "OUTPUT");

    if (! modArrowArea.isEmpty())
    {
        g.setColour (Colors::textDim);
        g.setFont (juce::Font (juce::FontOptions (13.0f)));
        g.drawText (juce::String::fromUTF8 ("\xe2\x96\xb6"), modArrowArea,
                    juce::Justification::centred, false);
    }
}

void DelayEditor::resized()
{
    auto placeKnob = [] (KnobCell& c, juce::Rectangle<int> r)
    {
        c.label->setBounds (r.removeFromBottom (13));
        c.knob->setBounds (r.reduced (2));
    };

    auto area = getLocalBounds().reduced (8);
    const int gap = 6;

    secInput = area.removeFromLeft (55);
    area.removeFromLeft (gap);
    secOutput = area.removeFromRight (70);
    area.removeFromRight (gap);

    const int rowGap = 6;
    const int rowH = (area.getHeight() - rowGap) / 2;
    auto topRow = area.removeFromTop (rowH);
    area.removeFromTop (rowGap);
    auto botRow = area;

    {
        auto row = topRow;
        secTime  = row.removeFromLeft (240); row.removeFromLeft (gap);
        secModel = row.removeFromLeft (140); row.removeFromLeft (gap);
        secFeedback = row;
    }
    {
        auto row = botRow;
        secMod  = row.removeFromLeft (200); row.removeFromLeft (gap);
        secDiff = row.removeFromLeft (120); row.removeFromLeft (gap);
        secDist = row;
    }

    {
        auto c = secInput; c.removeFromTop (16); c = c.reduced (6, 6);
        inWetLabel.setBounds (c.removeFromBottom (14));
        inWetFader.setBounds (c.withSizeKeepingCentre (26, c.getHeight()));
    }

    {
        auto c = secOutput; c.removeFromTop (16); c = c.reduced (4, 6);
        const int cellH = c.getHeight() / 3;
        placeKnob (wetK,  c.removeFromTop (cellH));
        placeKnob (toneK, c.removeFromTop (cellH));
        placeKnob (dryK,  c);
    }

    {
        auto c = secTime; c.removeFromTop (16); c = c.reduced (6, 4);
        placeKnob (timeK, c.removeFromLeft (60));
        c.removeFromLeft (6);
        placeKnob (offsetK, c.removeFromRight (50));
        c.removeFromRight (6);
        auto mid = c;
        syncLed->setBounds (mid.removeFromTop (16));
        mid.removeFromTop (2);
        keepLed->setBounds (mid.removeFromTop (16));
        mid.removeFromTop (4);
        smoothLabel.setBounds (mid.removeFromBottom (13));
        smoothFader.setBounds (mid.removeFromTop (juce::jmin (18, mid.getHeight())));
    }

    {
        auto c = secModel; c.removeFromTop (16); c = c.reduced (6, 4);
        placeKnob (stereoK, c.removeFromRight (48));
        c.removeFromRight (4);
        modelLed->setBounds (c);
    }

    {
        auto c = secFeedback; c.removeFromTop (16); c = c.reduced (6, 4);
        placeKnob (fbLevelK, c.removeFromLeft (58));
        c.removeFromLeft (4);
        fbModeLed->setBounds (c.removeFromLeft (48));
        c.removeFromLeft (6);
        const int colW = c.getWidth() / 2;
        auto colA = c.removeFromLeft (colW);
        auto colB = c;
        const int halfH = colA.getHeight() / 2;
        placeKnob (cutoffK, colA.removeFromTop (halfH));
        placeKnob (smpK,    colA);
        placeKnob (resK,    colB.removeFromTop (halfH));
        placeKnob (bitsK,   colB);
    }

    {
        auto c = secMod; c.removeFromTop (16); c = c.reduced (6, 4);
        placeKnob (modRateK, c.removeFromLeft (52));
        modArrowArea = c.removeFromLeft (22);
        placeKnob (modTimeK, c.removeFromLeft (52));
        c.removeFromLeft (4);
        placeKnob (modCutK, c.removeFromLeft (52));
    }

    {
        auto c = secDiff; c.removeFromTop (16); c = c.reduced (6, 4);
        const int colW = c.getWidth() / 2;
        placeKnob (diffLevelK,  c.removeFromLeft (colW));
        placeKnob (diffSpreadK, c);
    }

    {
        auto c = secDist; c.removeFromTop (16); c = c.reduced (6, 4);
        distLed->setBounds (c.removeFromLeft (56));
        c.removeFromLeft (6);
        const int sq = juce::jmin (c.getHeight(), 104);
        auto curveArea = c.removeFromRight (sq);
        distCurve->setBounds (curveArea.withSizeKeepingCentre (sq, sq));
        c.removeFromRight (6);
        const int kw = c.getWidth() / 3;
        placeKnob (distLevelK, c.removeFromLeft (kw));
        placeKnob (distKneeK,  c.removeFromLeft (kw));
        placeKnob (distSymK,   c);
    }
}
