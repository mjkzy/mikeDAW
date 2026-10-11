#include "BuiltinReverb.h"

namespace
{
    juce::NormalisableRange<float> logRange (float lo, float hi)
    {
        juce::NormalisableRange<float> r (lo, hi);
        r.setSkewForCentre (std::sqrt (lo * hi));
        return r;
    }

    constexpr float kBaseMs[8]  = { 29.7f, 36.1f, 42.9f, 49.7f, 57.3f, 66.1f, 78.7f, 91.3f };

    constexpr float kDiffMs[4]  = { 5.3f, 7.9f, 10.4f, 12.7f };

    constexpr float kErMs[8]    = { 7.1f, 13.3f, 19.7f, 26.9f, 34.3f, 45.8f, 58.1f, 72.3f };

    float sizeMult (float sizeCur)  { return 0.35f + sizeCur * 1.25f; }

    void hadamard8 (float* v)
    {
        for (int len = 1; len < 8; len <<= 1)
            for (int i = 0; i < 8; i += len << 1)
                for (int j = i; j < i + len; ++j)
                {
                    const float a = v[j], b = v[j + len];
                    v[j] = a + b;
                    v[j + len] = a - b;
                }
        constexpr float norm = 0.35355339f;
        for (int i = 0; i < 8; ++i)
            v[i] *= norm;
    }

    float readInterp (const std::vector<float>& buf, int writeIdx, float delay)
    {
        const int len = (int) buf.size();
        float rp = (float) writeIdx - delay;
        while (rp < 0.0f) rp += (float) len;
        int i0 = (int) rp;
        const float f = rp - (float) i0;
        if (i0 >= len) i0 -= len;
        int i1 = i0 + 1;
        if (i1 >= len) i1 -= len;
        return buf[i0] + f * (buf[i1] - buf[i0]);
    }
}

ReverbProcessor::ReverbProcessor()
{
    addParameter (midSide = new juce::AudioParameterChoice (
        juce::ParameterID { "midSide", 1 }, "Mode",
        juce::StringArray { "Stereo", "Mid", "Side" }, 0));
    addParameter (lowCut = new juce::AudioParameterFloat (
        juce::ParameterID { "lowCut", 1 }, "Low cut", logRange (20.0f, 1000.0f), 100.0f));
    addParameter (highCut = new juce::AudioParameterFloat (
        juce::ParameterID { "highCut", 1 }, "High cut", logRange (1000.0f, 21000.0f), 16000.0f));
    addParameter (predelay = new juce::AudioParameterFloat (
        juce::ParameterID { "predelay", 1 }, "Delay",
        juce::NormalisableRange<float> (0.0f, 200.0f, 0.1f), 10.0f));
    addParameter (size = new juce::AudioParameterFloat (
        juce::ParameterID { "size", 1 }, "Size",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.6f));
    addParameter (diffusion = new juce::AudioParameterFloat (
        juce::ParameterID { "diffusion", 1 }, "Diff",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.7f));
    addParameter (mod = new juce::AudioParameterFloat (
        juce::ParameterID { "mod", 1 }, "Mod",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.25f));
    addParameter (speed = new juce::AudioParameterFloat (
        juce::ParameterID { "speed", 1 }, "Speed", logRange (0.05f, 3.0f), 0.5f));
    addParameter (decay = new juce::AudioParameterFloat (
        juce::ParameterID { "decay", 1 }, "Dec", logRange (0.1f, 20.0f), 2.5f));
    addParameter (bass = new juce::AudioParameterFloat (
        juce::ParameterID { "bass", 1 }, "Bass",
        juce::NormalisableRange<float> (0.0f, 2.0f, 0.001f), 1.0f));
    addParameter (cross = new juce::AudioParameterFloat (
        juce::ParameterID { "cross", 1 }, "Cross", logRange (100.0f, 2000.0f), 400.0f));
    addParameter (damp = new juce::AudioParameterFloat (
        juce::ParameterID { "damp", 1 }, "Damp",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.4f));
    addParameter (dry = new juce::AudioParameterFloat (
        juce::ParameterID { "dry", 1 }, "Dry",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 1.0f));
    addParameter (er = new juce::AudioParameterFloat (
        juce::ParameterID { "er", 1 }, "ER",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.5f));
    addParameter (wet = new juce::AudioParameterFloat (
        juce::ParameterID { "wet", 1 }, "Wet",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.35f));
    addParameter (separation = new juce::AudioParameterFloat (
        juce::ParameterID { "separation", 1 }, "Sep",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.5f));
}

double ReverbProcessor::getTailLengthSeconds() const
{
    return juce::jmin (20.0, (double) (decay != nullptr ? decay->get() : 2.5f));
}

void ReverbProcessor::prepareToPlay (double sampleRate, int blockSize)
{
    sr = sampleRate > 0 ? sampleRate : 44100.0;

    juce::dsp::ProcessSpec spec { sr, (juce::uint32) juce::jmax (1, blockSize), 2 };
    hpf.prepare (spec);
    lpf.prepare (spec);
    hpf.setType (juce::dsp::StateVariableTPTFilterType::highpass);
    lpf.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
    hpf.reset();
    lpf.reset();

    auto msToSamples = [this] (float ms) { return (int) std::ceil (ms * 0.001 * sr); };

    const int predelayLen = msToSamples (300.0f) + 4;
    for (auto& b : predelayBuf) { b.assign ((size_t) predelayLen, 0.0f); }
    predelayWrite = 0;
    predelaySamplesCur = 0.0f;

    const int erLen = msToSamples (160.0f) + 4;
    for (auto& b : erBuf) { b.assign ((size_t) erLen, 0.0f); }
    erWrite = 0;

    for (int i = 0; i < numDiffusers; ++i)
    {
        diffBuf[i].assign ((size_t) juce::jmax (4, msToSamples (kDiffMs[i])), 0.0f);
        diffWrite[i] = 0;
    }

    const int modHeadroom = msToSamples (3.0f) + 4;
    for (int i = 0; i < numLines; ++i)
    {
        const int len = msToSamples (kBaseMs[i] * 1.6f) + modHeadroom + 4;
        lineBuf[i].assign ((size_t) len, 0.0f);
        lineWrite[i] = 0;
        dampState[i] = crossState[i] = 0.0f;
        lfoPhase[i] = (float) i * 0.7853982f;
    }

    sizeCur = size->get();
    lastDecay = lastBass = lastSize = -1.0f;
    lastLowCut = lastHighCut = -1.0f;

    for (auto* s : { &drySm, &erSm, &wetSm, &sepSm })
        s->reset (sr, 0.02);
    drySm.setCurrentAndTargetValue (dry->get());
    erSm.setCurrentAndTargetValue (er->get());
    wetSm.setCurrentAndTargetValue (wet->get());
    sepSm.setCurrentAndTargetValue (separation->get());

    updateDerived();
}

void ReverbProcessor::reset()
{
    hpf.reset();
    lpf.reset();

    for (auto& b : predelayBuf) std::fill (b.begin(), b.end(), 0.0f);
    predelayWrite = 0;

    for (auto& b : erBuf) std::fill (b.begin(), b.end(), 0.0f);
    erWrite = 0;

    for (int i = 0; i < numDiffusers; ++i)
    {
        std::fill (diffBuf[i].begin(), diffBuf[i].end(), 0.0f);
        diffWrite[i] = 0;
    }

    for (int i = 0; i < numLines; ++i)
    {
        std::fill (lineBuf[i].begin(), lineBuf[i].end(), 0.0f);
        lineWrite[i] = 0;
        dampState[i] = crossState[i] = 0.0f;
    }

    drySm.setCurrentAndTargetValue (drySm.getTargetValue());
    erSm.setCurrentAndTargetValue (erSm.getTargetValue());
    wetSm.setCurrentAndTargetValue (wetSm.getTargetValue());
    sepSm.setCurrentAndTargetValue (sepSm.getTargetValue());
}

void ReverbProcessor::updateDerived()
{
    sizeCur += (size->get() - sizeCur) * 0.05f;
    const float mult = sizeMult (sizeCur);

    for (int i = 0; i < numLines; ++i)
        baseDelay[i] = juce::jmax (2.0f, kBaseMs[i] * mult * 0.001f * (float) sr);

    const float dampFc = 21000.0f * std::exp (damp->get() * std::log (1500.0f / 21000.0f));
    dampCoeff = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * dampFc / (float) sr);

    crossCoeff = 1.0f - std::exp (-juce::MathConstants<float>::twoPi
                                  * juce::jlimit (20.0f, 20000.0f, cross->get()) / (float) sr);

    lfoInc = juce::MathConstants<float>::twoPi * speed->get() / (float) sr;
    modDepthSamples = mod->get() * 3.0f * 0.001f * (float) sr;

    diffCoeff = 0.55f + 0.2f * diffusion->get();

    if (! juce::approximatelyEqual (lastDecay, decay->get())
        || ! juce::approximatelyEqual (lastBass, bass->get())
        || std::abs (lastSize - sizeCur) > 1.0e-4f)
    {
        lastDecay = decay->get();
        lastBass  = bass->get();
        lastSize  = sizeCur;

        const float dec     = juce::jmax (0.1f, lastDecay);
        const float decLow  = juce::jmax (0.1f, dec * juce::jmax (0.01f, lastBass));
        for (int i = 0; i < numLines; ++i)
        {
            const float ls = baseDelay[i] / (float) sr;
            gHigh[i] = juce::jmin (0.9995f, std::pow (10.0f, -3.0f * ls / dec));
            gLow[i]  = juce::jmin (0.9995f, std::pow (10.0f, -3.0f * ls / decLow));
        }
    }

    if (! juce::approximatelyEqual (lastLowCut, lowCut->get()))
    {
        lastLowCut = lowCut->get();
        hpf.setCutoffFrequency (juce::jlimit (20.0f, (float) sr * 0.45f, lastLowCut));
    }
    if (! juce::approximatelyEqual (lastHighCut, highCut->get()))
    {
        lastHighCut = highCut->get();
        lpf.setCutoffFrequency (juce::jlimit (20.0f, (float) sr * 0.45f, lastHighCut));
    }

    const float mult2 = mult;
    for (int k = 0; k < numErTaps; ++k)
    {
        erTapL[k] = juce::jmax (1.0f, kErMs[k] * mult2 * 0.001f * (float) sr);
        erTapR[k] = juce::jmax (1.0f, kErMs[k] * 1.07f * mult2 * 0.001f * (float) sr);
    }
}

void ReverbProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    if (sr <= 0.0 || buffer.getNumSamples() == 0)
        return;

    updateDerived();

    drySm.setTargetValue (dry->get());
    erSm.setTargetValue (er->get());
    wetSm.setTargetValue (wet->get());
    sepSm.setTargetValue (separation->get());

    const int   n       = buffer.getNumSamples();
    const int   mode    = midSide->getIndex();
    const float predTgt = juce::jmax (1.0f, predelay->get() * 0.001f * (float) sr);

    const int nch = buffer.getNumChannels();
    float* chL = buffer.getWritePointer (0);
    float* chR = nch > 1 ? buffer.getWritePointer (1) : chL;

    constexpr float injB[8] = { 1.0f, -1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f, -1.0f };
    const float erGain[8]   = { 1.0f, -0.78f, 0.62f, -0.48f, 0.38f, -0.30f, 0.24f, -0.19f };

    for (int s = 0; s < n; ++s)
    {
        const float origL = chL[s];
        const float origR = chR[s];

        float inL, inR;
        if (mode == 1)       { const float m = (origL + origR) * 0.5f; inL = inR = m; }
        else if (mode == 2)  { const float sd = (origL - origR) * 0.5f; inL = inR = sd; }
        else                 { inL = origL; inR = origR; }

        inL = lpf.processSample (0, hpf.processSample (0, inL));
        inR = lpf.processSample (1, hpf.processSample (1, inR));

        predelaySamplesCur += (predTgt - predelaySamplesCur) * 0.02f;
        const float preL = readInterp (predelayBuf[0], predelayWrite, predelaySamplesCur);
        const float preR = readInterp (predelayBuf[1], predelayWrite, predelaySamplesCur);
        predelayBuf[0][predelayWrite] = inL;
        predelayBuf[1][predelayWrite] = inR;
        if (++predelayWrite >= (int) predelayBuf[0].size()) predelayWrite = 0;

        float erL = 0.0f, erR = 0.0f;
        for (int k = 0; k < numErTaps; ++k)
        {
            erL += erGain[k]              * readInterp (erBuf[0], erWrite, erTapL[k]);
            erR += erGain[numErTaps - 1 - k] * readInterp (erBuf[1], erWrite, erTapR[k]);
        }
        erBuf[0][erWrite] = preL;
        erBuf[1][erWrite] = preR;
        if (++erWrite >= (int) erBuf[0].size()) erWrite = 0;

        float diff = (preL + preR) * 0.5f;
        for (int d = 0; d < numDiffusers; ++d)
        {
            auto& b = diffBuf[d];
            const float delayed = b[(size_t) diffWrite[d]];
            const float y = -diffCoeff * diff + delayed;
            b[(size_t) diffWrite[d]] = diff + diffCoeff * y;
            if (++diffWrite[d] >= (int) b.size()) diffWrite[d] = 0;
            diff = y;
        }

        float f[8];
        float tailL = 0.0f, tailR = 0.0f;
        for (int i = 0; i < numLines; ++i)
        {
            const float lfo = modDepthSamples * std::sin (lfoPhase[i]);
            const float rd  = readInterp (lineBuf[i], lineWrite[i], baseDelay[i] + lfo);

            dampState[i]  += dampCoeff * (rd - dampState[i]);
            const float damped = dampState[i];

            crossState[i] += crossCoeff * (damped - crossState[i]);
            const float low  = crossState[i];
            const float high = damped - low;
            f[i] = gLow[i] * low + gHigh[i] * high;

            if ((i & 1) == 0) tailL += damped;
            else              tailR += damped;
        }

        hadamard8 (f);

        for (int i = 0; i < numLines; ++i)
        {
            lineBuf[i][(size_t) lineWrite[i]] = f[i] + diff * injB[i] * 0.5f;
            if (++lineWrite[i] >= (int) lineBuf[i].size()) lineWrite[i] = 0;
            lfoPhase[i] += lfoInc;
            if (lfoPhase[i] >= juce::MathConstants<float>::twoPi)
                lfoPhase[i] -= juce::MathConstants<float>::twoPi;
        }

        tailL *= 0.5f;
        tailR *= 0.5f;

        const float dg = drySm.getNextValue();
        const float eg = erSm.getNextValue();
        const float wg = wetSm.getNextValue();
        const float sepv = sepSm.getNextValue();

        float wetL = eg * erL + wg * tailL;
        float wetR = eg * erR + wg * tailR;

        const float mid = (wetL + wetR) * 0.5f;
        float       sid = (wetL - wetR) * 0.5f;
        sid *= sepv * 2.0f;
        wetL = mid + sid;
        wetR = mid - sid;

        chL[s] = dg * origL + wetL;
        if (nch > 1)
            chR[s] = dg * origR + wetR;
    }
}

juce::AudioProcessorEditor* ReverbProcessor::createEditor()
{
    return new ReverbEditor (*this);
}

ReverbEditor::ReverbEditor (ReverbProcessor& p)
    : juce::AudioProcessorEditor (p), proc (p)
{
    addAndMakeVisible (room);
    addAndMakeVisible (modeBox);

    auto setupKnob = [this] (juce::Slider& k, double def)
    {
        k.addListener (this);
        k.setDoubleClickReturnValue (true, def);
        addAndMakeVisible (k);
    };
    setupKnob (lowCutKnob,  100.0);
    setupKnob (highCutKnob, 16000.0);
    setupKnob (delayKnob,   10.0);
    setupKnob (sizeKnob,    0.6);
    setupKnob (diffKnob,    0.7);
    setupKnob (modKnob,     0.25);
    setupKnob (speedKnob,   0.5);
    setupKnob (decKnob,     2.5);
    setupKnob (bassKnob,    1.0);
    setupKnob (crossKnob,   400.0);
    setupKnob (dampKnob,    0.4);
    setupKnob (sepKnob,     0.5);

    auto setupFader = [this] (juce::Slider& f, double def)
    {
        f.setSliderStyle (juce::Slider::LinearVertical);
        f.addListener (this);
        f.setDoubleClickReturnValue (true, def);
        addAndMakeVisible (f);
    };
    setupFader (dryFader, 1.0);
    setupFader (erFader,  0.5);
    setupFader (wetFader, 0.35);

    auto setupLabel = [this] (juce::Label& l, const juce::String& text)
    {
        l.setText (text, juce::dontSendNotification);
        l.setJustificationType (juce::Justification::centred);
        l.setFont (juce::Font (juce::FontOptions (11.0f)));
        l.setColour (juce::Label::textColourId, Colors::textDim);
        addAndMakeVisible (l);
    };
    setupLabel (modeLabel,   "MODE");
    setupLabel (lowCutLabel, "L.CUT");
    setupLabel (highCutLabel,"H.CUT");
    setupLabel (delayLabel,  "DELAY");
    setupLabel (sizeLabel,   "SIZE");
    setupLabel (diffLabel,   "DIFF");
    setupLabel (modLabel,    "MOD");
    setupLabel (speedLabel,  "SPEED");
    setupLabel (decLabel,    "DEC");
    setupLabel (bassLabel,   "BASS");
    setupLabel (crossLabel,  "CROSS");
    setupLabel (dampLabel,   "DAMP");
    setupLabel (sepLabel,    "SEP");
    setupLabel (dryLabel,    "DRY");
    setupLabel (erLabel,     "ER");
    setupLabel (wetLabel,    "WET");

    setSize (600, 230);
}

void ReverbEditor::sliderValueChanged (juce::Slider*)
{
    room.repaint();
}

void ReverbEditor::paint (juce::Graphics& g)
{
    g.fillAll (Colors::panelBg);

    auto band = [&g] (juce::Rectangle<int> r, const juce::String& title)
    {
        g.setColour (Colors::panelDark);
        g.fillRect (r);
        g.setColour (Colors::outline);
        g.drawRect (r, 1);
        g.setColour (Colors::accent);
        g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
        g.drawText (title, r.removeFromTop (14).reduced (6, 0),
                    juce::Justification::centredLeft, false);
    };

    band (inputArea,  "INPUT");
    band (spaceArea,  "SPACE");
    band (colorArea,  "COLOR");
    band (outputArea, "OUTPUT");
}

void ReverbEditor::resized()
{
    auto r = getLocalBounds().reduced (6);

    room.setBounds (r.removeFromLeft (120));
    r.removeFromLeft (6);

    outputArea = r.removeFromRight (128);
    r.removeFromRight (6);

    inputArea = r.removeFromTop (r.getHeight() / 3);
    spaceArea = r.removeFromTop (r.getHeight() / 2);
    colorArea = r;

    auto cell = [] (juce::Rectangle<int> area, juce::Component& c, juce::Label& l)
    {
        l.setBounds (area.removeFromBottom (13));
        c.setBounds (area.reduced (3));
    };

    auto lay = [&cell] (juce::Rectangle<int> area, std::vector<std::pair<juce::Component*, juce::Label*>> items)
    {
        area.removeFromTop (14);
        const int w = area.getWidth() / juce::jmax (1, (int) items.size());
        for (auto& it : items)
            cell (area.removeFromLeft (w), *it.first, *it.second);
    };

    lay (inputArea, { { &modeBox, &modeLabel }, { &lowCutKnob, &lowCutLabel }, { &highCutKnob, &highCutLabel } });
    lay (spaceArea, { { &delayKnob, &delayLabel }, { &sizeKnob, &sizeLabel }, { &diffKnob, &diffLabel }, { &modKnob, &modLabel }, { &speedKnob, &speedLabel } });
    lay (colorArea, { { &bassKnob, &bassLabel }, { &crossKnob, &crossLabel }, { &decKnob, &decLabel }, { &dampKnob, &dampLabel } });

    auto out = outputArea.reduced (4);
    out.removeFromTop (14);
    auto sepRow = out.removeFromBottom (52);
    const int fw = out.getWidth() / 3;
    auto fader = [] (juce::Rectangle<int> a, juce::Slider& sfad, juce::Label& l)
    {
        l.setBounds (a.removeFromBottom (13));
        sfad.setBounds (a.reduced (2));
    };
    fader (out.removeFromLeft (fw), dryFader, dryLabel);
    fader (out.removeFromLeft (fw), erFader,  erLabel);
    fader (out,                     wetFader, wetLabel);

    sepLabel.setBounds (sepRow.removeFromBottom (13));
    sepKnob.setBounds (sepRow.reduced (2));
}

void ReverbEditor::RoomView::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat().reduced (2.0f);
    g.setColour (Colors::panelDark.darker (0.4f));
    g.fillRoundedRectangle (b, 3.0f);
    g.setColour (Colors::outline);
    g.drawRoundedRectangle (b, 3.0f, 1.0f);

    auto in = b.reduced (10.0f);
    const float sz  = proc.size->get();
    const float dec = proc.decay->get();

    const float depth = juce::jmap (sz, 0.0f, 1.0f, 6.0f, 26.0f);
    juce::Rectangle<float> front = in.withSizeKeepingCentre (in.getWidth() * 0.62f,
                                                             in.getHeight() * 0.55f)
                                     .translated (-depth * 0.4f, depth * 0.4f);
    juce::Rectangle<float> back = front.translated (depth, -depth);

    g.setColour (Colors::gridLine.withAlpha (0.8f));
    g.drawRect (back, 1.0f);
    for (int c = 0; c < 4; ++c)
    {
        const juce::Point<float> fp[4] = { front.getTopLeft(), front.getTopRight(),
                                           front.getBottomRight(), front.getBottomLeft() };
        const juce::Point<float> bp[4] = { back.getTopLeft(), back.getTopRight(),
                                           back.getBottomRight(), back.getBottomLeft() };
        g.drawLine ({ fp[c], bp[c] }, 1.0f);
    }
    g.setColour (Colors::accent.withAlpha (0.9f));
    g.drawRect (front, 1.2f);

    auto bars = juce::Rectangle<float> (in.getX(), in.getBottom() - 22.0f, in.getWidth(), 20.0f);
    const int nb = 22;
    const float bw = bars.getWidth() / (float) nb;
    const float rt = juce::jlimit (0.1f, 20.0f, dec);
    for (int i = 0; i < nb; ++i)
    {
        const float t = (float) i / (float) nb;
        const float env = std::exp (-t * 6.0f / rt);
        const float h = bars.getHeight() * env;
        juce::Rectangle<float> bar (bars.getX() + i * bw + 1.0f, bars.getBottom() - h,
                                    bw - 2.0f, h);
        g.setColour (Colors::accent.withAlpha (0.25f + 0.5f * env));
        g.fillRect (bar);
    }
}
