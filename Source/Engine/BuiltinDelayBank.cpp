#include "BuiltinDelayBank.h"

namespace
{
    const float kDivisions[] = { 0.0625f, 0.125f, 0.1875f, 0.25f, 1.0f / 3.0f, 0.375f,
                                 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f };
    constexpr int kNumDiv = (int) (sizeof (kDivisions) / sizeof (float));

    float quantizeBeats (float t)
    {
        int idx = juce::jlimit (0, kNumDiv - 1, (int) std::round (t * (float) (kNumDiv - 1)));
        return kDivisions[idx];
    }

    juce::NormalisableRange<float> logRange (float lo, float hi, float centre)
    {
        auto r = juce::NormalisableRange<float> (lo, hi);
        r.setSkewForCentre (centre);
        return r;
    }

    void panGains (float p, float& gL, float& gR)
    {
        const float x = (juce::jlimit (-1.0f, 1.0f, p) + 1.0f) * 0.5f;
        gL = std::cos (x * juce::MathConstants<float>::halfPi);
        gR = std::sin (x * juce::MathConstants<float>::halfPi);
    }
}

DelayBankProcessor::DelayBankProcessor()
{
    addParameter (dry = new juce::AudioParameterFloat (
        juce::ParameterID { "dry", 1 }, "Dry",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 1.0f));
    addParameter (wet = new juce::AudioParameterFloat (
        juce::ParameterID { "wet", 1 }, "Wet",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.5f));
    addParameter (inLevel = new juce::AudioParameterFloat (
        juce::ParameterID { "in", 1 }, "In",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 1.0f));
    addParameter (fbScale = new juce::AudioParameterFloat (
        juce::ParameterID { "fb", 1 }, "Feedback",
        juce::NormalisableRange<float> (0.0f, 1.2f, 0.001f), 1.0f));

    const juce::StringArray filtChoices { "Off", "LP", "BP", "HP" };
    const juce::StringArray fbChoices   { "Off", "Norm", "Inv", "P.Pong" };

    for (int b = 0; b < numBanks; ++b)
    {
        auto& bk = bank[b];
        const juce::String pfx = "b" + juce::String (b + 1) + "_";
        const juce::String cap = "B" + juce::String (b + 1) + " ";
        const bool en = (b == 0);

        addParameter (bk.enable = new juce::AudioParameterBool (
            juce::ParameterID { pfx + "en", 1 }, cap + "Enable", en));
        addParameter (bk.inPan = new juce::AudioParameterFloat (
            juce::ParameterID { pfx + "inPan", 1 }, cap + "In Pan",
            juce::NormalisableRange<float> (-1.0f, 1.0f, 0.001f), 0.0f));
        addParameter (bk.inVol = new juce::AudioParameterFloat (
            juce::ParameterID { pfx + "inVol", 1 }, cap + "In Vol",
            juce::NormalisableRange<float> (0.0f, 1.5f, 0.001f), 1.0f));

        addParameter (bk.filtMode = new juce::AudioParameterChoice (
            juce::ParameterID { pfx + "filtMode", 1 }, cap + "Filter", filtChoices, 0));
        addParameter (bk.filtPost = new juce::AudioParameterBool (
            juce::ParameterID { pfx + "filtPost", 1 }, cap + "Filter Post", false));
        addParameter (bk.filtGain = new juce::AudioParameterFloat (
            juce::ParameterID { pfx + "filtGain", 1 }, cap + "Filter Gain",
            juce::NormalisableRange<float> (0.0f, 2.0f, 0.001f), 1.0f));
        addParameter (bk.filtCut = new juce::AudioParameterFloat (
            juce::ParameterID { pfx + "filtCut", 1 }, cap + "Filter Cut",
            logRange (100.0f, 21000.0f, 2000.0f), 8000.0f));
        addParameter (bk.filtRes = new juce::AudioParameterFloat (
            juce::ParameterID { pfx + "filtRes", 1 }, cap + "Filter Res",
            juce::NormalisableRange<float> (0.5f, 8.0f, 0.001f), 0.707f));

        addParameter (bk.time = new juce::AudioParameterFloat (
            juce::ParameterID { pfx + "time", 1 }, cap + "Time",
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.0001f), 0.5f));
        addParameter (bk.ofs = new juce::AudioParameterFloat (
            juce::ParameterID { pfx + "ofs", 1 }, cap + "Offset",
            juce::NormalisableRange<float> (-1.0f, 1.0f, 0.001f), 0.0f));
        addParameter (bk.sep = new juce::AudioParameterFloat (
            juce::ParameterID { pfx + "sep", 1 }, cap + "Separation",
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.5f));

        addParameter (bk.fbMode = new juce::AudioParameterChoice (
            juce::ParameterID { pfx + "fbMode", 1 }, cap + "FB Mode", fbChoices, 1));
        addParameter (bk.fbVol = new juce::AudioParameterFloat (
            juce::ParameterID { pfx + "fbVol", 1 }, cap + "FB Vol",
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.4f));
        addParameter (bk.fbPan = new juce::AudioParameterFloat (
            juce::ParameterID { pfx + "fbPan", 1 }, cap + "FB Pan",
            juce::NormalisableRange<float> (-1.0f, 1.0f, 0.001f), 0.0f));

        addParameter (bk.fbfMode = new juce::AudioParameterChoice (
            juce::ParameterID { pfx + "fbfMode", 1 }, cap + "FB Filter", filtChoices, 0));
        addParameter (bk.fbfGain = new juce::AudioParameterFloat (
            juce::ParameterID { pfx + "fbfGain", 1 }, cap + "FB Filter Gain",
            juce::NormalisableRange<float> (0.0f, 2.0f, 0.001f), 1.0f));
        addParameter (bk.fbfCut = new juce::AudioParameterFloat (
            juce::ParameterID { pfx + "fbfCut", 1 }, cap + "FB Filter Cut",
            logRange (100.0f, 21000.0f, 2000.0f), 6000.0f));
        addParameter (bk.fbfRes = new juce::AudioParameterFloat (
            juce::ParameterID { pfx + "fbfRes", 1 }, cap + "FB Filter Res",
            juce::NormalisableRange<float> (0.5f, 8.0f, 0.001f), 0.707f));

        addParameter (bk.grainDiv = new juce::AudioParameterFloat (
            juce::ParameterID { pfx + "grainDiv", 1 }, cap + "Grain Div",
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));
        addParameter (bk.grainSH = new juce::AudioParameterFloat (
            juce::ParameterID { pfx + "grainSH", 1 }, cap + "Grain S&H",
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));

        addParameter (bk.outNext = new juce::AudioParameterFloat (
            juce::ParameterID { pfx + "outNext", 1 }, cap + "Next",
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));
        addParameter (bk.outPan = new juce::AudioParameterFloat (
            juce::ParameterID { pfx + "outPan", 1 }, cap + "Out Pan",
            juce::NormalisableRange<float> (-1.0f, 1.0f, 0.001f), 0.0f));
        addParameter (bk.outVol = new juce::AudioParameterFloat (
            juce::ParameterID { pfx + "outVol", 1 }, cap + "Out Vol",
            juce::NormalisableRange<float> (0.0f, 1.5f, 0.001f), 1.0f));
    }
}

void DelayBankProcessor::prepareToPlay (double sampleRate, int blockSize)
{
    sr = sampleRate > 0 ? sampleRate : 44100.0;
    bufSize = (int) (4.0 * sr) + 4;

    juce::dsp::ProcessSpec spec { sr, (juce::uint32) juce::jmax (1, blockSize), 2 };
    for (int b = 0; b < numBanks; ++b)
    {
        auto& s = st[b];
        for (auto& buf : s.buf) buf.assign ((size_t) bufSize, 0.0f);
        s.writePos = 0;
        for (int ch = 0; ch < 2; ++ch)
        {
            s.curDelay[ch] = 1.0f; s.shHold[ch] = 0.0f; s.shPhase[ch] = 1.0f;
            s.dcX1[ch] = s.dcY1[ch] = 0.0f;
        }
        inFilt[b].prepare (spec); inFilt[b].reset();
        fbFilt[b].prepare (spec); fbFilt[b].reset();
    }

    drySm.reset (sr, 0.01); wetSm.reset (sr, 0.01);
    inSm.reset (sr, 0.01);  fbSm.reset (sr, 0.01);
    drySm.setCurrentAndTargetValue (dry->get());
    wetSm.setCurrentAndTargetValue (wet->get());
    inSm.setCurrentAndTargetValue (inLevel->get());
    fbSm.setCurrentAndTargetValue (fbScale->get());
}

void DelayBankProcessor::reset()
{
    for (int b = 0; b < numBanks; ++b)
    {
        auto& s = st[b];
        for (auto& buf : s.buf) std::fill (buf.begin(), buf.end(), 0.0f);
        s.writePos = 0;
        for (int ch = 0; ch < 2; ++ch)
        {
            s.curDelay[ch] = 1.0f; s.shHold[ch] = 0.0f; s.shPhase[ch] = 1.0f;
            s.dcX1[ch] = s.dcY1[ch] = 0.0f;
        }
        inFilt[b].reset();
        fbFilt[b].reset();
    }

    drySm.setCurrentAndTargetValue (drySm.getTargetValue());
    wetSm.setCurrentAndTargetValue (wetSm.getTargetValue());
    inSm.setCurrentAndTargetValue (inSm.getTargetValue());
    fbSm.setCurrentAndTargetValue (fbSm.getTargetValue());
}

float DelayBankProcessor::readDelay (const std::vector<float>& buf, int wp, float delaySamples) const
{
    const int n = (int) buf.size();
    float rp = (float) wp - delaySamples;
    while (rp < 0.0f)       rp += (float) n;
    while (rp >= (float) n) rp -= (float) n;

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

void DelayBankProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
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

    drySm.setTargetValue (dry->get());
    wetSm.setTargetValue (wet->get());
    inSm.setTargetValue (inLevel->get());
    fbSm.setTargetValue (fbScale->get());

    using FT = juce::dsp::StateVariableTPTFilterType;
    auto setFilterType = [] (juce::dsp::StateVariableTPTFilter<float>& f, int mode)
    {
        if (mode == 1)      f.setType (FT::lowpass);
        else if (mode == 2) f.setType (FT::bandpass);
        else                f.setType (FT::highpass);
    };

    const float maxD = (float) (bufSize - 8);
    const float beatMs = (float) (60000.0 / juce::jmax (1.0, bpm));

    bool  enabled[numBanks];
    int   fMode[numBanks], fbmMode[numBanks], fbfMd[numBanks];
    bool  fPost[numBanks];
    float fGain[numBanks], fbfGn[numBanks];
    float inGL[numBanks], inGR[numBanks], outGL[numBanks], outGR[numBanks];
    float fbGL[numBanks], fbGR[numBanks];
    float fbVolB[numBanks], nextB[numBanks], sepB[numBanks];
    float targetL[numBanks], targetR[numBanks];
    float shInc[numBanks], shMix[numBanks];

    for (int b = 0; b < numBanks; ++b)
    {
        auto& bk = bank[b];
        enabled[b] = bk.enable->get();
        if (! enabled[b]) continue;

        fMode[b]   = bk.filtMode->getIndex();
        fPost[b]   = bk.filtPost->get();
        fGain[b]   = bk.filtGain->get();
        fbmMode[b] = bk.fbMode->getIndex();
        fbfMd[b]   = bk.fbfMode->getIndex();
        fbfGn[b]   = bk.fbfGain->get();

        float ip = bk.inPan->get(), op = bk.outPan->get(), fp = bk.fbPan->get();
        float l, r;
        panGains (ip, l, r); inGL[b] = l * bk.inVol->get(); inGR[b] = r * bk.inVol->get();
        panGains (op, l, r); outGL[b] = l * bk.outVol->get(); outGR[b] = r * bk.outVol->get();
        panGains (fp, l, r); fbGL[b] = l; fbGR[b] = r;

        fbVolB[b] = bk.fbVol->get();
        nextB[b]  = bk.outNext->get();
        sepB[b]   = bk.sep->get();

        const float ms = quantizeBeats (bk.time->get()) * beatMs;
        const float ofs = bk.ofs->get();
        targetL[b] = juce::jlimit (1.0f, maxD, ms * 0.001f * (float) sr);
        targetR[b] = juce::jlimit (1.0f, maxD, ms * (1.0f + 0.5f * ofs) * 0.001f * (float) sr);

        if (fMode[b] != 0)   { setFilterType (inFilt[b], fMode[b]); inFilt[b].setCutoffFrequency (bk.filtCut->get()); inFilt[b].setResonance (bk.filtRes->get()); }
        if (fbfMd[b] != 0)   { setFilterType (fbFilt[b], fbfMd[b]); fbFilt[b].setCutoffFrequency (bk.fbfCut->get());  fbFilt[b].setResonance (bk.fbfRes->get()); }

        const float divisor = 1.0f + bk.grainDiv->get() * 63.0f;
        shInc[b] = 1.0f / divisor;
        shMix[b] = bk.grainSH->get();
    }

    const float slew = 1.0f - std::exp (-1.0f / (0.03f * (float) sr));
    const int chR = numChannels > 1 ? 1 : 0;

    for (int s = 0; s < numSamples; ++s)
    {
        const float ig = inSm.getNextValue();
        const float srcL = buffer.getReadPointer (0)[s] * ig;
        const float srcR = buffer.getReadPointer (chR)[s] * ig;
        const float fbg  = fbSm.getNextValue();

        float chainL = 0.0f, chainR = 0.0f;
        float wetL = 0.0f, wetR = 0.0f;

        for (int b = 0; b < numBanks; ++b)
        {
            if (! enabled[b])
                continue;

            auto& s2 = st[b];
            const int wp = s2.writePos;

            const float inL = srcL * inGL[b] + chainL;
            const float inR = srcR * inGR[b] + chainR;

            s2.curDelay[0] += (targetL[b] - s2.curDelay[0]) * slew;
            s2.curDelay[1] += (targetR[b] - s2.curDelay[1]) * slew;

            float tap[2] = { readDelay (s2.buf[0], wp, s2.curDelay[0]),
                             readDelay (s2.buf[1], wp, s2.curDelay[1]) };

            if (shInc[b] < 0.999f || shMix[b] > 0.0001f)
            {
                for (int ch = 0; ch < 2; ++ch)
                {
                    s2.shPhase[ch] += shInc[b];
                    if (s2.shPhase[ch] >= 1.0f) { s2.shPhase[ch] -= 1.0f; s2.shHold[ch] = tap[ch]; }
                    tap[ch] = tap[ch] * (1.0f - shMix[b]) + s2.shHold[ch] * shMix[b];
                }
            }

            float wtap[2] = { tap[0], tap[1] };
            if (fMode[b] != 0 && fPost[b])
                for (int ch = 0; ch < 2; ++ch)
                    wtap[ch] = inFilt[b].processSample (ch, wtap[ch]) * fGain[b];

            float fb[2] = { tap[0], tap[1] };
            if (fbfMd[b] != 0)
                for (int ch = 0; ch < 2; ++ch)
                    fb[ch] = fbFilt[b].processSample (ch, fb[ch]) * fbfGn[b];

            float fbL, fbR;
            const float fv = fbVolB[b] * fbg;
            switch (fbmMode[b])
            {
                case 1:  fbL =  fb[0] * fv; fbR =  fb[1] * fv; break;
                case 2:  fbL = -fb[0] * fv; fbR = -fb[1] * fv; break;
                case 3:  fbL =  fb[1] * fv; fbR =  fb[0] * fv; break;
                default: fbL = fbR = 0.0f; break;
            }
            fbL *= fbGL[b]; fbR *= fbGR[b];
            fbL = std::tanh (fbL);
            fbR = std::tanh (fbR);
            float oL = fbL - s2.dcX1[0] + 0.995f * s2.dcY1[0]; s2.dcX1[0] = fbL; s2.dcY1[0] = oL;
            float oR = fbR - s2.dcX1[1] + 0.995f * s2.dcY1[1]; s2.dcX1[1] = fbR; s2.dcY1[1] = oR;
            fbL = oL; fbR = oR;

            float wrL = inL, wrR = inR;
            if (fMode[b] != 0 && ! fPost[b])
            {
                wrL = inFilt[b].processSample (0, wrL) * fGain[b];
                wrR = inFilt[b].processSample (1, wrR) * fGain[b];
            }
            s2.buf[0][(size_t) wp] = juce::jlimit (-4.0f, 4.0f, wrL + fbL);
            s2.buf[1][(size_t) wp] = juce::jlimit (-4.0f, 4.0f, wrR + fbR);

            if (++s2.writePos >= bufSize) s2.writePos = 0;

            const float mid  = 0.5f * (wtap[0] + wtap[1]);
            const float side = 0.5f * (wtap[0] - wtap[1]) * (0.5f + 1.5f * sepB[b]);
            const float bwL = mid + side, bwR = mid - side;

            wetL += bwL * outGL[b];
            wetR += bwR * outGR[b];

            const float nx = nextB[b];
            chainL = bwL * outGL[b] * nx;
            chainR = bwR * outGR[b] * nx;
        }

        const float dg = drySm.getNextValue();
        const float wg = wetSm.getNextValue();
        const float inL0 = buffer.getReadPointer (0)[s];
        const float inR0 = buffer.getReadPointer (chR)[s];

        buffer.getWritePointer (0)[s] = inL0 * dg + wetL * wg;
        if (numChannels > 1)
            buffer.getWritePointer (1)[s] = inR0 * dg + wetR * wg;
    }
}

juce::AudioProcessorEditor* DelayBankProcessor::createEditor()
{
    return new DelayBankEditor (*this);
}

static void styleCaption (juce::Label& l, const juce::String& text)
{
    l.setText (text, juce::dontSendNotification);
    l.setJustificationType (juce::Justification::centred);
    l.setFont (juce::Font (juce::FontOptions (10.0f)));
    l.setColour (juce::Label::textColourId, Colors::textDim);
}

class DBankLedToggle : public juce::Component
{
public:
    DBankLedToggle (juce::AudioParameterBool& p, const juce::String& text, bool dotOnly = false)
        : caption (text), captionOnly (! dotOnly),
          att (p, [this] (float v) { on = v > 0.5f; repaint(); })
    {
        att.sendInitialUpdate();
    }

    void paint (juce::Graphics& g) override
    {
        const float r  = 4.0f;
        const float cy = (float) getHeight() * 0.5f;
        juce::Rectangle<float> dot (3.0f, cy - r, r * 2.0f, r * 2.0f);
        if (on) { g.setColour (Colors::accent); g.fillEllipse (dot); }
        else    { g.setColour (Colors::panelDark); g.fillEllipse (dot);
                  g.setColour (Colors::outline);   g.drawEllipse (dot, 1.0f); }
        if (captionOnly)
        {
            g.setColour (on ? Colors::text : Colors::textDim);
            g.setFont (juce::Font (juce::FontOptions (9.5f)));
            const int tx = (int) dot.getRight() + 5;
            g.drawText (caption, tx, 0, getWidth() - tx, getHeight(),
                        juce::Justification::centredLeft, false);
        }
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        att.setValueAsCompleteGesture (on ? 0.0f : 1.0f);
    }

private:
    juce::String caption;
    bool captionOnly = true;
    bool on = false;
    juce::ParameterAttachment att;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DBankLedToggle)
};

class DBankLedChoice : public juce::Component
{
public:
    DBankLedChoice (juce::AudioParameterChoice& p)
        : rows (p.choices),
          att (p, [this] (float v) { current = (int) std::round (v); repaint(); })
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
            if (on) { g.setColour (Colors::accent); g.fillEllipse (dot); }
            else    { g.setColour (Colors::panelDark); g.fillEllipse (dot);
                      g.setColour (Colors::outline);   g.drawEllipse (dot, 1.0f); }
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

private:
    juce::StringArray rows;
    int current = 0;
    juce::ParameterAttachment att;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DBankLedChoice)
};

DelayBankEditor::DelayBankEditor (DelayBankProcessor& p)
    : juce::AudioProcessorEditor (p), proc (p)
{
    for (int b = 0; b < DelayBankProcessor::numBanks; ++b)
    {
        auto* led = new DBankLedToggle (*proc.bank[b].enable, {}, true);
        enableLeds.add (led);
        addAndMakeVisible (led);

        auto* t = new juce::TextButton (juce::String (b + 1));
        t->setClickingTogglesState (false);
        t->onClick = [this, b] { selectBank (b); };
        tabs.add (t);
        addAndMakeVisible (t);
    }

    for (auto* k : { &inPanK, &inVolK, &filtGainK, &filtCutK, &filtResK,
                     &fbVolK, &fbPanK, &fbfGainK, &fbfCutK, &fbfResK,
                     &outNextK, &outPanK, &outVolK, &dryK, &wetK, &inK, &fbK })
        addAndMakeVisible (*k);
    for (auto* sl : { &timeS, &ofsS, &sepS, &grainDivS, &grainShS })
    {
        sl->setSliderStyle (juce::Slider::LinearHorizontal);
        addAndMakeVisible (*sl);
    }

    dryK.setDoubleClickReturnValue (true, 1.0);
    wetK.setDoubleClickReturnValue (true, 0.5);
    inK.setDoubleClickReturnValue  (true, 1.0);
    fbK.setDoubleClickReturnValue  (true, 1.0);

    auto addLabel = [this] (const juce::String& t) -> juce::Label&
    {
        auto* l = new juce::Label(); styleCaption (*l, t);
        labels.add (l); addAndMakeVisible (l); return *l;
    };
    addLabel ("PAN");  addLabel ("VOL");
    addLabel ("GAIN"); addLabel ("CUT"); addLabel ("RES");
    addLabel ("TIME"); addLabel ("OFS"); addLabel ("SEP");
    addLabel ("PAN");  addLabel ("VOL");
    addLabel ("GAIN"); addLabel ("CUT"); addLabel ("RES");
    addLabel ("DIV");  addLabel ("SH");
    addLabel ("NEXT"); addLabel ("PAN"); addLabel ("VOL");
    addLabel ("DRY");  addLabel ("WET"); addLabel ("IN"); addLabel ("FB");

    selectBank (0);
    setSize (860, 340);
}

DelayBankEditor::~DelayBankEditor() = default;

void DelayBankEditor::selectBank (int b)
{
    selected = juce::jlimit (0, DelayBankProcessor::numBanks - 1, b);
    for (int i = 0; i < tabs.size(); ++i)
        tabs[i]->setToggleState (i == selected, juce::dontSendNotification);
    bindBank();
    repaint();
}

void DelayBankEditor::bindBank()
{
    bankAtts.clear();
    auto bind = [this] (juce::RangedAudioParameter& prm, juce::Slider& s, double def)
    {
        bankAtts.add (new juce::SliderParameterAttachment (prm, s));
        s.setDoubleClickReturnValue (true, def);
    };

    auto& bk = proc.bank[selected];
    bind (*bk.inPan,  inPanK,  0.0);
    bind (*bk.inVol,  inVolK,  1.0);
    bind (*bk.filtGain, filtGainK, 1.0);
    bind (*bk.filtCut,  filtCutK,  8000.0);
    bind (*bk.filtRes,  filtResK,  0.707);
    bind (*bk.time, timeS, 0.5);
    bind (*bk.ofs,  ofsS,  0.0);
    bind (*bk.sep,  sepS,  0.5);
    bind (*bk.fbVol, fbVolK, 0.4);
    bind (*bk.fbPan, fbPanK, 0.0);
    bind (*bk.fbfGain, fbfGainK, 1.0);
    bind (*bk.fbfCut,  fbfCutK,  6000.0);
    bind (*bk.fbfRes,  fbfResK,  0.707);
    bind (*bk.grainDiv, grainDivS, 0.0);
    bind (*bk.grainSH,  grainShS,  0.0);
    bind (*bk.outNext, outNextK, 0.0);
    bind (*bk.outPan,  outPanK,  0.0);
    bind (*bk.outVol,  outVolK,  1.0);

    filtModeLed = std::make_unique<DBankLedChoice> (*bk.filtMode);
    fbModeLed   = std::make_unique<DBankLedChoice> (*bk.fbMode);
    fbfModeLed  = std::make_unique<DBankLedChoice> (*bk.fbfMode);
    filtPostLed = std::make_unique<DBankLedToggle> (*bk.filtPost, "POST");
    addAndMakeVisible (*filtModeLed);
    addAndMakeVisible (*fbModeLed);
    addAndMakeVisible (*fbfModeLed);
    addAndMakeVisible (*filtPostLed);
    resized();
}

void DelayBankEditor::paint (juce::Graphics& g)
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

    section (secTop,      "BANKS  (bank " + juce::String (selected + 1) + " selected)");
    section (secGlobal,   "GLOBAL");
    section (secIn,       "IN");
    section (secFilter,   "FILTER");
    section (secDelay,    "DELAY / FEEDBACK");
    section (secFbFilter, "FB FILTER");
    section (secGrain,    "GRAIN");
    section (secOut,      "OUT");
}

void DelayBankEditor::resized()
{
    auto placeKnob = [this] (int labelIdx, juce::Slider& k, juce::Rectangle<int> r)
    {
        labels[labelIdx]->setBounds (r.removeFromBottom (12));
        k.setBounds (r.reduced (2));
    };
    auto placeSlider = [this] (int labelIdx, juce::Slider& sl, juce::Rectangle<int> r)
    {
        labels[labelIdx]->setBounds (r.removeFromLeft (34));
        sl.setBounds (r.reduced (2, 4));
    };

    auto area = getLocalBounds().reduced (8);
    const int gap = 6;

    auto top = area.removeFromTop (58);
    area.removeFromTop (gap);
    secGlobal = top.removeFromRight (280);
    top.removeFromRight (gap);
    secTop = top;

    {
        auto c = secTop; c.removeFromTop (16); c = c.reduced (6, 4);
        const int n = DelayBankProcessor::numBanks;
        const int cw = c.getWidth() / n;
        for (int i = 0; i < n; ++i)
        {
            auto cell = c.removeFromLeft (cw);
            enableLeds[i]->setBounds (cell.removeFromTop (12).withSizeKeepingCentre (12, 12));
            tabs[i]->setBounds (cell.reduced (3, 2));
        }
    }
    {
        auto c = secGlobal; c.removeFromTop (16); c = c.reduced (6, 2);
        const int kw = c.getWidth() / 4;
        placeKnob (18, dryK, c.removeFromLeft (kw));
        placeKnob (19, wetK, c.removeFromLeft (kw));
        placeKnob (20, inK,  c.removeFromLeft (kw));
        placeKnob (21, fbK,  c);
    }

    const int rowGap = 6;
    const int rowH = (area.getHeight() - rowGap) / 2;
    auto topRow = area.removeFromTop (rowH);
    area.removeFromTop (rowGap);
    auto botRow = area;

    {
        auto row = topRow;
        secIn     = row.removeFromLeft (110); row.removeFromLeft (gap);
        secFilter = row.removeFromLeft (200); row.removeFromLeft (gap);
        secDelay  = row;
    }
    {
        auto row = botRow;
        secFbFilter = row.removeFromLeft (200); row.removeFromLeft (gap);
        secGrain    = row.removeFromLeft (150); row.removeFromLeft (gap);
        secOut      = row;
    }

    {
        auto c = secIn; c.removeFromTop (16); c = c.reduced (6, 4);
        const int kw = c.getWidth() / 2;
        placeKnob (0, inPanK, c.removeFromLeft (kw));
        placeKnob (1, inVolK, c);
    }
    {
        auto c = secFilter; c.removeFromTop (16); c = c.reduced (6, 4);
        if (filtModeLed) filtModeLed->setBounds (c.removeFromLeft (44));
        c.removeFromLeft (4);
        if (filtPostLed) filtPostLed->setBounds (c.removeFromTop (14));
        auto kk = c;
        const int kw = kk.getWidth() / 3;
        placeKnob (2, filtGainK, kk.removeFromLeft (kw));
        placeKnob (3, filtCutK,  kk.removeFromLeft (kw));
        placeKnob (4, filtResK,  kk);
    }
    {
        auto c = secDelay; c.removeFromTop (16); c = c.reduced (6, 4);
        if (fbModeLed) fbModeLed->setBounds (c.removeFromLeft (58));
        c.removeFromLeft (6);
        auto knobCol = c.removeFromRight (100);
        const int kw = knobCol.getWidth() / 2;
        placeKnob (8, fbPanK, knobCol.removeFromLeft (kw));
        placeKnob (9, fbVolK, knobCol);
        c.removeFromRight (6);
        const int sh = c.getHeight() / 3;
        placeSlider (5, timeS, c.removeFromTop (sh));
        placeSlider (6, ofsS,  c.removeFromTop (sh));
        placeSlider (7, sepS,  c);
    }
    {
        auto c = secFbFilter; c.removeFromTop (16); c = c.reduced (6, 4);
        if (fbfModeLed) fbfModeLed->setBounds (c.removeFromLeft (44));
        c.removeFromLeft (6);
        const int kw = c.getWidth() / 3;
        placeKnob (10, fbfGainK, c.removeFromLeft (kw));
        placeKnob (11, fbfCutK,  c.removeFromLeft (kw));
        placeKnob (12, fbfResK,  c);
    }
    {
        auto c = secGrain; c.removeFromTop (16); c = c.reduced (6, 8);
        const int sh = c.getHeight() / 2;
        placeSlider (13, grainDivS, c.removeFromTop (sh));
        placeSlider (14, grainShS,  c);
    }
    {
        auto c = secOut; c.removeFromTop (16); c = c.reduced (6, 4);
        const int kw = c.getWidth() / 3;
        placeKnob (15, outNextK, c.removeFromLeft (kw));
        placeKnob (16, outPanK,  c.removeFromLeft (kw));
        placeKnob (17, outVolK,  c);
    }
}
