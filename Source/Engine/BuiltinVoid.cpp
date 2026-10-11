#include "BuiltinVoid.h"

namespace
{
    juce::NormalisableRange<float> logRange (float lo, float hi, float centre)
    {
        auto r = juce::NormalisableRange<float> (lo, hi);
        r.setSkewForCentre (centre);
        return r;
    }

    const float kSemis[5] = { -12.0f, 5.0f, 7.0f, 12.0f, 19.0f };

    const float kLineMs[VoidProcessor::numLines] =
        { 31.1f, 37.7f, 43.3f, 51.7f, 59.9f, 67.1f, 78.3f, 89.9f };

    const float kLineOffset[VoidProcessor::numLines] =
        { 0.0f, 2.1f, 4.0f, 1.1f, 3.3f, 5.4f, 0.7f, 2.8f };

    const float kInjSign[VoidProcessor::numLines] =
        { 1.0f, -1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f, -1.0f };

    const float kApMs[4] = { 4.7f, 3.6f, 12.5f, 9.2f };

    struct VoidPreset
    {
        const char* name;
        float dry, wet, drive, expanse, decay, predelay, diffuse;
        bool  freeze;
        float floorCut, airCut, warmth, shimmer;
        int   shimmerInt;
        float driftRate, wobble, gravity, gravRel, width, bloom;
    };

    const VoidPreset kVoidPresets[8] =
    {
        { "First Light",     1.0f, 0.35f,0.10f,0.55f, 3.0f,20.0f,0.80f,false, 90.0f, 9000.0f,0.20f,0.00f, 3, 0.30f,0.30f,0.30f,400.0f,1.0f,0.00f },
        { "Event Horizon",   1.0f, 0.45f,0.15f,0.90f,14.0f,40.0f,0.85f,false, 70.0f, 3500.0f,0.40f,0.10f, 3, 0.25f,0.40f,0.55f,500.0f,1.1f,0.20f },
        { "Stellar Nursery", 0.80f,0.50f,0.15f,0.80f, 8.0f,30.0f,0.85f,false, 90.0f,11000.0f,0.30f,0.60f, 3, 0.40f,0.50f,0.25f,450.0f,1.2f,0.50f },
        { "Solar Winds",     1.0f, 0.40f,0.10f,0.70f, 6.0f,20.0f,0.80f,false,120.0f,16000.0f,0.15f,0.20f, 2, 1.20f,0.70f,0.20f,350.0f,1.3f,0.10f },
        { "Dark Matter",     1.0f, 0.42f,0.20f,0.85f,10.0f,35.0f,0.85f,false, 60.0f, 4500.0f,0.60f,0.50f, 0, 0.20f,0.35f,0.35f,500.0f,1.0f,0.15f },
        { "Gravity Well",    1.0f, 0.50f,0.15f,0.80f,10.0f,25.0f,0.80f,false, 90.0f, 8000.0f,0.30f,0.15f, 3, 0.40f,0.40f,0.90f,250.0f,1.1f,0.20f },
        { "Cryo Chamber",    1.0f, 0.30f,0.05f,0.25f, 1.2f,10.0f,0.70f,false,150.0f,12000.0f,0.10f,0.00f, 3, 0.50f,0.25f,0.20f,300.0f,1.3f,0.00f },
        { "Deep Field",      0.85f,0.50f,0.15f,1.00f,30.0f,50.0f,0.90f,false, 80.0f, 7000.0f,0.35f,0.30f, 3, 0.15f,0.40f,0.25f,600.0f,1.2f,0.70f }
    };
}

VoidProcessor::VoidProcessor()
{
    addParameter (dry = new juce::AudioParameterFloat (
        juce::ParameterID { "dry", 1 }, "Dry",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 1.0f));
    addParameter (wet = new juce::AudioParameterFloat (
        juce::ParameterID { "wet", 1 }, "Wet",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.35f));
    addParameter (drive = new juce::AudioParameterFloat (
        juce::ParameterID { "drive", 1 }, "Drive",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.15f));
    addParameter (expanse = new juce::AudioParameterFloat (
        juce::ParameterID { "expanse", 1 }, "Expanse",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.6f));
    addParameter (decay = new juce::AudioParameterFloat (
        juce::ParameterID { "decay", 1 }, "Decay",
        logRange (0.2f, 30.0f, 4.0f), 4.0f));
    addParameter (predelay = new juce::AudioParameterFloat (
        juce::ParameterID { "predelay", 1 }, "Predelay",
        juce::NormalisableRange<float> (0.0f, 250.0f, 0.1f), 20.0f));
    addParameter (diffuse = new juce::AudioParameterFloat (
        juce::ParameterID { "diffuse", 1 }, "Diffuse",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.8f));
    addParameter (freeze = new juce::AudioParameterBool (
        juce::ParameterID { "freeze", 1 }, "Freeze", false));
    addParameter (floorCut = new juce::AudioParameterFloat (
        juce::ParameterID { "floorCut", 1 }, "Floor",
        logRange (20.0f, 1000.0f, 200.0f), 90.0f));
    addParameter (airCut = new juce::AudioParameterFloat (
        juce::ParameterID { "airCut", 1 }, "Air",
        logRange (1000.0f, 20000.0f, 6000.0f), 9000.0f));
    addParameter (warmth = new juce::AudioParameterFloat (
        juce::ParameterID { "warmth", 1 }, "Warmth",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.25f));
    addParameter (shimmer = new juce::AudioParameterFloat (
        juce::ParameterID { "shimmer", 1 }, "Shimmer",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));
    addParameter (shimmerInt = new juce::AudioParameterChoice (
        juce::ParameterID { "shimmerInt", 1 }, "Interval",
        juce::StringArray { "-12", "+5", "+7", "+12", "+19" }, 3));
    addParameter (driftRate = new juce::AudioParameterFloat (
        juce::ParameterID { "driftRate", 1 }, "Drift",
        logRange (0.02f, 4.0f, 0.5f), 0.4f));
    addParameter (wobble = new juce::AudioParameterFloat (
        juce::ParameterID { "wobble", 1 }, "Wobble",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.35f));
    addParameter (gravity = new juce::AudioParameterFloat (
        juce::ParameterID { "gravity", 1 }, "Gravity",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.3f));
    addParameter (gravRel = new juce::AudioParameterFloat (
        juce::ParameterID { "gravRel", 1 }, "Release",
        logRange (50.0f, 1500.0f, 400.0f), 400.0f));
    addParameter (width = new juce::AudioParameterFloat (
        juce::ParameterID { "width", 1 }, "Width",
        juce::NormalisableRange<float> (0.0f, 1.5f, 0.001f), 1.0f));
    addParameter (bloom = new juce::AudioParameterFloat (
        juce::ParameterID { "bloom", 1 }, "Bloom",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));
}

void VoidProcessor::prepareToPlay (double sampleRate, int blockSize)
{
    sr = sampleRate > 0 ? sampleRate : 44100.0;

    const int modGuard = (int) (0.006 * sr) + 4;
    for (int i = 0; i < numLines; ++i)
    {
        lines[i].baseLen = juce::jmax (4.0f, (float) (kLineMs[i] * 0.001 * sr));
        const int sz = (int) std::ceil (lines[i].baseLen) + modGuard + 8;
        lines[i].buf.assign ((size_t) sz, 0.0f);
        lines[i].write  = 0;
        lines[i].curLen = lines[i].baseLen * 0.6f;
        lines[i].damp   = 0.0f;
    }

    for (int j = 0; j < 4; ++j)
    {
        const int sz = juce::jmax (4, (int) (kApMs[j] * 0.001 * sr));
        allpass[j].buf.assign ((size_t) sz, 0.0f);
        allpass[j].pos = 0;
    }

    const int pdSz = (int) (0.25 * sr) + juce::jmax (1, blockSize) + 8;
    predelayBuf.assign ((size_t) pdSz, 0.0f);
    predelayWrite = 0;

    shifter.window = (float) (0.1 * sr);
    shifter.buf.assign ((size_t) ((int) shifter.window + 8), 0.0f);
    shifter.write = 0;
    shifter.phase = 0.0f;

    juce::dsp::ProcessSpec spec { sr, (juce::uint32) juce::jmax (1, blockSize), 1 };
    floorHP.prepare (spec);
    floorHP.reset();
    floorHP.setType (juce::dsp::StateVariableTPTFilterType::highpass);

    lfoPhase = 0.0f;
    gEnv     = 0.0f;
    bloomG   = 0.0f;
    freezeInit = false;

    drySm.reset (sr, 0.02); wetSm.reset (sr, 0.02); widthSm.reset (sr, 0.02);
    drySm.setCurrentAndTargetValue (dry->get());
    wetSm.setCurrentAndTargetValue (wet->get());
    widthSm.setCurrentAndTargetValue (width->get());
}

void VoidProcessor::reset()
{
    for (int i = 0; i < numLines; ++i)
    {
        std::fill (lines[i].buf.begin(), lines[i].buf.end(), 0.0f);
        lines[i].write  = 0;
        lines[i].curLen = lines[i].baseLen * 0.6f;
        lines[i].damp   = 0.0f;
    }
    for (int j = 0; j < 4; ++j)
    {
        std::fill (allpass[j].buf.begin(), allpass[j].buf.end(), 0.0f);
        allpass[j].pos = 0;
    }
    std::fill (predelayBuf.begin(), predelayBuf.end(), 0.0f);
    predelayWrite = 0;

    std::fill (shifter.buf.begin(), shifter.buf.end(), 0.0f);
    shifter.write = 0;
    shifter.phase = 0.0f;

    floorHP.reset();

    lfoPhase = 0.0f;
    gEnv     = 0.0f;
    bloomG   = 0.0f;
    freezeInit = false;

    drySm.setCurrentAndTargetValue (drySm.getTargetValue());
    wetSm.setCurrentAndTargetValue (wetSm.getTargetValue());
    widthSm.setCurrentAndTargetValue (widthSm.getTargetValue());

    tailLevel.store (0.0f, std::memory_order_relaxed);
    duckGain.store (1.0f, std::memory_order_relaxed);
}

float VoidProcessor::readDelay (const std::vector<float>& buf, int wp, float delaySamples)
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

float VoidProcessor::GrainShifter::read (float delaySamples) const
{
    const int n = (int) buf.size();
    float rp = (float) write - delaySamples;
    while (rp < 0.0f)       rp += (float) n;
    while (rp >= (float) n) rp -= (float) n;
    const int i0 = (int) rp;
    const float f = rp - (float) i0;
    const int i1 = (i0 + 1) % n;
    return buf[(size_t) i0] * (1.0f - f) + buf[(size_t) i1] * f;
}

float VoidProcessor::GrainShifter::process (float in, float ratio)
{
    buf[(size_t) write] = in;

    const float W = window;
    float d1 = phase;
    float d2 = phase + W * 0.5f;
    if (d2 >= W) d2 -= W;

    const float twoPi = juce::MathConstants<float>::twoPi;
    const float g1 = 0.5f - 0.5f * std::cos (twoPi * d1 / W);
    const float g2 = 0.5f - 0.5f * std::cos (twoPi * d2 / W);
    const float out = read (d1) * g1 + read (d2) * g2;

    phase += (1.0f - ratio);
    while (phase >= W)    phase -= W;
    while (phase < 0.0f)  phase += W;

    if (++write >= (int) buf.size()) write = 0;
    return out;
}

void VoidProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int n   = buffer.getNumSamples();
    const int nch = buffer.getNumChannels();
    if (n == 0 || nch < 1 || sr <= 0.0)
        return;

    const bool  frozen    = freeze->get();
    const float driveGain = 1.0f + drive->get() * 5.0f;
    const float apCoef    = 0.7f * diffuse->get();
    const float lenScale  = 0.25f + 0.75f * expanse->get();
    const float decayS    = juce::jmax (0.05f, decay->get());
    const float warmthAmt = warmth->get();
    const float wDrive    = 1.0f + 3.0f * warmthAmt;
    const float shimmerAmt = shimmer->get();
    const float ratio     = std::pow (2.0f, kSemis[juce::jlimit (0, 4, shimmerInt->getIndex())] / 12.0f);
    const float lfoInc    = juce::MathConstants<float>::twoPi * driftRate->get() / (float) sr;
    const float wobbleDepth = wobble->get() * (float) (0.004 * sr);
    const float gravAmt   = gravity->get();
    const float bloomAmt  = bloom->get();

    const float ac = juce::jlimit (200.0f, (float) (sr * 0.45), airCut->get());
    const float dampCoef = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * ac / (float) sr);

    floorHP.setCutoffFrequency (juce::jlimit (20.0f, 1000.0f, floorCut->get()));

    const float predSamples = juce::jlimit (0.0f, (float) (0.25 * sr),
                                            predelay->get() * 0.001f * (float) sr);

    float targetLen[numLines], gTarget[numLines];
    for (int i = 0; i < numLines; ++i)
    {
        targetLen[i] = juce::jmax (4.0f, lines[i].baseLen * lenScale);
        if (frozen)
            gTarget[i] = 1.0f;
        else
        {
            const float lineSec = targetLen[i] / (float) sr;
            gTarget[i] = juce::jmin (0.999f, std::pow (10.0f, -3.0f * lineSec / decayS));
        }
    }

    const float injTarget = frozen ? 0.0f : 0.45f;
    if (! freezeInit)
    {
        for (int i = 0; i < numLines; ++i) gCur[i] = gTarget[i];
        injCur = injTarget;
        freezeInit = true;
    }
    for (int i = 0; i < numLines; ++i)
        if (std::abs (gTarget[i] - gCur[i]) < 1.0e-4f) gCur[i] = gTarget[i];
    if (std::abs (injTarget - injCur) < 1.0e-4f) injCur = injTarget;
    const float freezeSlew = 1.0f - std::exp (-1.0f / (0.03f * (float) sr));

    const float atkCoef  = 1.0f - std::exp (-1.0f / (0.005f * (float) sr));
    const float relTime  = juce::jmax (0.01f, gravRel->get() * 0.001f);
    const float relCoef  = 1.0f - std::exp (-1.0f / (relTime * (float) sr));
    const float bloomAtk = 1.0f - std::exp (-1.0f / ((0.02f + bloomAmt * 1.5f) * (float) sr));
    const float bloomRel = 1.0f - std::exp (-1.0f / (0.08f * (float) sr));
    const float lenSlew  = 1.0f - std::exp (-1.0f / (0.04f * (float) sr));

    drySm.setTargetValue (dry->get());
    wetSm.setTargetValue (wet->get());
    widthSm.setTargetValue (width->get());

    const float outScale     = 0.35f;
    const float shimmerLevel = 0.5f;

    float* chL = buffer.getWritePointer (0);
    float* chRp = nch > 1 ? buffer.getWritePointer (1) : chL;

    double sumSq = 0.0;
    float  lastDuck = 1.0f;

    for (int s = 0; s < n; ++s)
    {
        const float dryL = chL[s];
        const float dryR = chRp[s];
        const float monoRaw = 0.5f * (dryL + dryR);

        const float rect = std::abs (monoRaw);
        gEnv += (rect > gEnv ? atkCoef : relCoef) * (rect - gEnv);
        const float envNorm = juce::jlimit (0.0f, 1.0f, gEnv * 4.0f);

        const float duck = 1.0f - gravAmt * 0.968f * envNorm;
        lastDuck = duck;

        bloomG += (envNorm > bloomG ? bloomAtk : bloomRel) * (envNorm - bloomG);
        const float bloomGain = (1.0f - bloomAmt) + bloomAmt * bloomG;

        float x = std::tanh (monoRaw * driveGain);
        x = floorHP.processSample (0, x);
        for (int j = 0; j < 4; ++j)
            x = allpass[j].process (x, apCoef);

        predelayBuf[(size_t) predelayWrite] = x;
        const float xp = predSamples < 1.0f ? x : readDelay (predelayBuf, predelayWrite, predSamples);
        if (++predelayWrite >= (int) predelayBuf.size()) predelayWrite = 0;

        lfoPhase += lfoInc;
        if (lfoPhase >= juce::MathConstants<float>::twoPi)
            lfoPhase -= juce::MathConstants<float>::twoPi;

        float sL[numLines];
        for (int i = 0; i < numLines; ++i)
        {
            lines[i].curLen += (targetLen[i] - lines[i].curLen) * lenSlew;
            const float mod = wobbleDepth * std::sin (lfoPhase + kLineOffset[i]);
            float rd = readDelay (lines[i].buf, lines[i].write,
                                  juce::jmax (1.0f, lines[i].curLen + mod));
            lines[i].damp += dampCoef * (rd - lines[i].damp);
            if (! frozen)
            {
                rd = lines[i].damp;
                const float sat = std::tanh (rd * wDrive) / wDrive;
                rd = rd + warmthAmt * (sat - rd);
            }
            sL[i] = rd;
        }

        float wetL = (sL[0] + sL[2] + sL[4] + sL[6]) * outScale;
        float wetR = (sL[1] + sL[3] + sL[5] + sL[7]) * outScale;

        const float mix = (sL[0] + sL[1] + sL[2] + sL[3] + sL[4] + sL[5] + sL[6] + sL[7]) * 0.125f;
        const float shifted = shifter.process (mix, ratio);
        const float shimmerFeed = shimmerAmt * shifted * shimmerLevel;

        float sum = 0.0f;
        for (int i = 0; i < numLines; ++i) sum += sL[i];
        const float v = sum * (2.0f / (float) numLines);

        injCur += (injTarget - injCur) * freezeSlew;

        for (int i = 0; i < numLines; ++i)
        {
            gCur[i] += (gTarget[i] - gCur[i]) * freezeSlew;
            const float fb  = (sL[i] - v) * gCur[i];
            const float inj = injCur * xp * kInjSign[i];
            float w = inj + fb + shimmerFeed;
            const float lin  = juce::jlimit (-6.0f, 6.0f, w);
            const float soft = std::tanh (w * 0.5f) * 2.0f;
            w = lin + shimmerAmt * (soft - lin);
            lines[i].buf[(size_t) lines[i].write] = w;
            if (++lines[i].write >= (int) lines[i].buf.size()) lines[i].write = 0;
        }

        const float wdt  = widthSm.getNextValue();
        const float mid  = 0.5f * (wetL + wetR);
        const float side = 0.5f * (wetL - wetR) * wdt;
        const float outWL = mid + side;
        const float outWR = mid - side;

        const float dg = drySm.getNextValue();
        const float wetGain = wetSm.getNextValue() * duck * bloomGain;

        chL[s] = dryL * dg + outWL * wetGain;
        if (nch > 1)
            chRp[s] = dryR * dg + outWR * wetGain;

        sumSq += (double) (outWL * outWL + outWR * outWR);
    }

    tailLevel.store ((float) std::sqrt (sumSq / (2.0 * (double) n)), std::memory_order_relaxed);
    duckGain.store (lastDuck, std::memory_order_relaxed);
}

juce::AudioProcessorEditor* VoidProcessor::createEditor()
{
    return new VoidEditor (*this);
}

static void styleCaption (juce::Label& l, const juce::String& text)
{
    l.setText (text, juce::dontSendNotification);
    l.setJustificationType (juce::Justification::centred);
    l.setFont (juce::Font (juce::FontOptions (9.5f)));
    l.setColour (juce::Label::textColourId, Colors::textDim);
}

class VoidHorizon : public juce::Component
{
public:
    VoidHorizon()
    {
        juce::Random rng (0x5eed);
        particles.resize (40);
        for (auto& p : particles)
        {
            p.radius   = 0.34f + rng.nextFloat() * 0.58f;
            p.angle    = rng.nextFloat() * juce::MathConstants<float>::twoPi;
            p.speed    = (0.15f + rng.nextFloat() * 0.5f) * (rng.nextBool() ? 1.0f : -1.0f);
            p.size     = 1.2f + rng.nextFloat() * 2.2f;
            p.wobPhase = rng.nextFloat() * juce::MathConstants<float>::twoPi;
            p.bright   = 0.4f + rng.nextFloat() * 0.6f;
        }
    }

    void setState (float drift, float wob, float tail, float duck, bool frozen)
    {
        driftN = drift; wobN = wob; tailN = tail; duckN = duck; freezeN = frozen;
    }

    void update()
    {
        const float sp = 0.02f + driftN * 0.12f;
        for (auto& p : particles)
        {
            p.angle    += p.speed * sp;
            p.wobPhase += 0.05f + wobN * 0.15f;
        }
        const float target = freezeN ? 1.0f : tailN;
        ringPulse += (target - ringPulse) * 0.3f;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        const float cx = b.getCentreX(), cy = b.getCentreY();
        const float R  = juce::jmin (b.getWidth(), b.getHeight()) * 0.5f - 4.0f;

        const float pull  = 1.0f - juce::jlimit (0.0f, 1.0f, duckN);
        const float ringR = R * (0.94f - 0.18f * pull);
        const float glow  = juce::jlimit (0.0f, 1.0f, 0.25f + 0.75f * ringPulse);

        for (int i = 3; i >= 1; --i)
        {
            const float rr = ringR + (float) i * 3.0f;
            g.setColour (Colors::accent.withAlpha (glow * 0.12f / (float) i));
            g.drawEllipse (cx - rr, cy - rr, rr * 2.0f, rr * 2.0f, 2.0f + (float) i);
        }
        g.setColour (Colors::accent.withAlpha (0.35f + 0.55f * glow));
        g.drawEllipse (cx - ringR, cy - ringR, ringR * 2.0f, ringR * 2.0f, 2.0f);

        g.setColour (Colors::windowBg.darker (0.7f));
        g.fillEllipse (cx - ringR, cy - ringR, ringR * 2.0f, ringR * 2.0f);
        const float innerR = ringR * 0.62f;
        g.setColour (Colors::panelDark.darker (0.9f).withAlpha (0.7f));
        g.fillEllipse (cx - innerR, cy - innerR, innerR * 2.0f, innerR * 2.0f);

        for (auto& p : particles)
        {
            const float wob = wobN * 0.12f * std::sin (p.wobPhase);
            const float r   = ringR * (p.radius * (1.0f + wob));
            const float px  = cx + r * std::cos (p.angle);
            const float py  = cy + r * std::sin (p.angle);
            const float a   = juce::jlimit (0.0f, 1.0f, p.bright * (0.3f + 0.7f * (freezeN ? 1.0f : tailN)));
            g.setColour (Colors::accent.withAlpha (a));
            g.fillEllipse (px - p.size, py - p.size, p.size * 2.0f, p.size * 2.0f);
        }

        g.setColour (Colors::accent.withAlpha (0.2f + 0.6f * glow));
        g.fillEllipse (cx - 3.0f, cy - 3.0f, 6.0f, 6.0f);
    }

private:
    struct Particle { float radius, angle, speed, size, wobPhase, bright; };
    std::vector<Particle> particles;
    float driftN = 0.3f, wobN = 0.35f, tailN = 0.0f, duckN = 1.0f, ringPulse = 0.0f;
    bool  freezeN = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoidHorizon)
};

class VoidFreezeButton : public juce::Button
{
public:
    VoidFreezeButton() : juce::Button ("FREEZE") { setClickingTogglesState (true); }

    void paintButton (juce::Graphics& g, bool, bool) override
    {
        auto b = getLocalBounds().toFloat().reduced (1.0f);
        const bool on = getToggleState();

        g.setColour (on ? Colors::accent.withAlpha (0.22f) : Colors::panelDark);
        g.fillRoundedRectangle (b, 4.0f);
        g.setColour (on ? Colors::accent : Colors::outline);
        g.drawRoundedRectangle (b, 4.0f, on ? 1.6f : 1.0f);

        const float r = 4.0f;
        const float cyy = b.getCentreY();
        juce::Rectangle<float> dot (b.getX() + 10.0f, cyy - r, r * 2.0f, r * 2.0f);
        g.setColour (on ? Colors::accent : Colors::panelBg.darker (0.3f));
        g.fillEllipse (dot);
        if (on)
        {
            g.setColour (Colors::accent.withAlpha (0.4f));
            g.drawEllipse (dot.expanded (2.0f), 1.5f);
        }

        g.setColour (on ? Colors::text : Colors::textDim);
        g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        g.drawText ("FREEZE", b.withTrimmedLeft (22.0f), juce::Justification::centred, false);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoidFreezeButton)
};

class VoidIntervalSelector : public juce::Component
{
public:
    VoidIntervalSelector (juce::AudioParameterChoice& p)
        : rows (p.choices),
          att (p, [this] (float v) { current = (int) std::round (v); repaint(); })
    {
        att.sendInitialUpdate();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int nn = juce::jmax (1, rows.size());
        const int i  = juce::jlimit (0, nn - 1, (int) ((float) e.x / (float) getWidth() * (float) nn));
        att.setValueAsCompleteGesture ((float) i);
    }

    void paint (juce::Graphics& g) override
    {
        const int nn = juce::jmax (1, rows.size());
        const float cw = (float) getWidth() / (float) nn;
        g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
        for (int i = 0; i < rows.size(); ++i)
        {
            juce::Rectangle<float> cell ((float) i * cw, 0.0f, cw, (float) getHeight());
            auto box = cell.reduced (2.0f);
            const bool on = (i == current);
            g.setColour (on ? Colors::accent.withAlpha (0.25f) : Colors::panelDark);
            g.fillRoundedRectangle (box, 3.0f);
            g.setColour (on ? Colors::accent : Colors::outline);
            g.drawRoundedRectangle (box, 3.0f, 1.0f);
            g.setColour (on ? Colors::text : Colors::textDim);
            g.drawText (rows[i], box, juce::Justification::centred, false);
        }
    }

private:
    juce::StringArray rows;
    int current = 0;
    juce::ParameterAttachment att;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoidIntervalSelector)
};

class VoidPresetLabel : public juce::Label
{
public:
    VoidPresetLabel()
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setColour (juce::Label::textColourId, Colors::textDim);
    }

    std::function<void()> onClick;

    void mouseDown (const juce::MouseEvent&) override
    {
        if (onClick) onClick();
    }

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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoidPresetLabel)
};

VoidEditor::VoidEditor (VoidProcessor& p)
    : juce::AudioProcessorEditor (p), proc (p)
{
    auto styleButton = [] (juce::TextButton& btn)
    {
        btn.setColour (juce::TextButton::buttonColourId, Colors::raised);
        btn.setColour (juce::TextButton::textColourOffId, Colors::text);
    };
    styleButton (prevBtn); styleButton (nextBtn);

    prevBtn.onClick   = [this] { applyPreset (currentPreset <= 0 ? 7 : currentPreset - 1); };
    nextBtn.onClick   = [this] { applyPreset (currentPreset < 0 ? 0 : (currentPreset + 1) % 8); };
    addAndMakeVisible (prevBtn);
    addAndMakeVisible (nextBtn);

    presetName = std::make_unique<VoidPresetLabel>();
    presetName->setText ("Void", juce::dontSendNotification);
    presetName->setJustificationType (juce::Justification::centredLeft);
    presetName->setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
    presetName->onClick = [this] { showPresetMenu(); };
    addAndMakeVisible (*presetName);

    horizon = std::make_unique<VoidHorizon>();
    addAndMakeVisible (*horizon);
    freezeBtn = std::make_unique<VoidFreezeButton>();
    addAndMakeVisible (*freezeBtn);
    intervalSel = std::make_unique<VoidIntervalSelector> (*proc.shimmerInt);
    addAndMakeVisible (*intervalSel);

    for (auto* k : { &expanseK, &decayK, &predelayK, &diffuseK, &floorK, &airK, &warmthK,
                     &shimmerK, &driftK, &wobbleK, &gravityK, &releaseK,
                     &widthK, &bloomK, &driveK, &dryK, &wetK })
        addAndMakeVisible (*k);

    expanseK.setDoubleClickReturnValue  (true, 0.6);
    decayK.setDoubleClickReturnValue     (true, 4.0);
    predelayK.setDoubleClickReturnValue (true, 20.0);
    diffuseK.setDoubleClickReturnValue  (true, 0.8);
    floorK.setDoubleClickReturnValue     (true, 90.0);
    airK.setDoubleClickReturnValue        (true, 9000.0);
    warmthK.setDoubleClickReturnValue    (true, 0.25);
    shimmerK.setDoubleClickReturnValue   (true, 0.0);
    driftK.setDoubleClickReturnValue      (true, 0.4);
    wobbleK.setDoubleClickReturnValue    (true, 0.35);
    gravityK.setDoubleClickReturnValue   (true, 0.3);
    releaseK.setDoubleClickReturnValue   (true, 400.0);
    widthK.setDoubleClickReturnValue      (true, 1.0);
    bloomK.setDoubleClickReturnValue      (true, 0.0);
    driveK.setDoubleClickReturnValue      (true, 0.15);
    dryK.setDoubleClickReturnValue        (true, 1.0);
    wetK.setDoubleClickReturnValue        (true, 0.35);

    auto addLabel = [this] (const juce::String& t) -> juce::Label&
    {
        auto* l = new juce::Label(); styleCaption (*l, t);
        labels.add (l); addAndMakeVisible (l); return *l;
    };
    addLabel ("EXPANSE"); addLabel ("DECAY"); addLabel ("PREDELAY"); addLabel ("DIFFUSE");
    addLabel ("FLOOR");   addLabel ("AIR");   addLabel ("WARMTH");
    addLabel ("SHIMMER"); addLabel ("INTERVAL");
    addLabel ("DRIFT");   addLabel ("WOBBLE");
    addLabel ("GRAVITY"); addLabel ("RELEASE");
    addLabel ("WIDTH");   addLabel ("BLOOM");  addLabel ("DRIVE"); addLabel ("DRY"); addLabel ("WET");

    freezeAtt = std::make_unique<juce::ButtonParameterAttachment> (*proc.freeze, *freezeBtn);

    setSize (740, 430);
    startTimerHz (30);
}

VoidEditor::~VoidEditor() { stopTimer(); }

void VoidEditor::timerCallback()
{
    if (horizon == nullptr) return;
    horizon->setState (proc.driftRate->get() / 4.0f,
                       proc.wobble->get(),
                       juce::jlimit (0.0f, 1.0f, proc.tailLevel.load (std::memory_order_relaxed) * 3.0f),
                       proc.duckGain.load (std::memory_order_relaxed),
                       proc.freeze->get());
    horizon->update();
}

void VoidEditor::showPresetMenu()
{
    juce::PopupMenu m;
    for (int i = 0; i < 8; ++i)
        m.addItem (i + 1, kVoidPresets[i].name, true, i == currentPreset);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (presetName.get()),
        [safe = juce::Component::SafePointer<VoidEditor> (this)] (int r)
        {
            if (safe != nullptr && r > 0)
                safe->applyPreset (r - 1);
        });
}

void VoidEditor::applyPreset (int index)
{
    if (index < 0 || index >= 8) return;
    const auto& ps = kVoidPresets[index];

    auto setP = [] (juce::RangedAudioParameter& prm, float plain)
    {
        prm.setValueNotifyingHost (prm.convertTo0to1 (plain));
    };

    setP (*proc.dry, ps.dry);
    setP (*proc.wet, ps.wet);
    setP (*proc.drive, ps.drive);
    setP (*proc.expanse, ps.expanse);
    setP (*proc.decay, ps.decay);
    setP (*proc.predelay, ps.predelay);
    setP (*proc.diffuse, ps.diffuse);
    setP (*proc.freeze, ps.freeze ? 1.0f : 0.0f);
    setP (*proc.floorCut, ps.floorCut);
    setP (*proc.airCut, ps.airCut);
    setP (*proc.warmth, ps.warmth);
    setP (*proc.shimmer, ps.shimmer);
    setP (*proc.shimmerInt, (float) ps.shimmerInt);
    setP (*proc.driftRate, ps.driftRate);
    setP (*proc.wobble, ps.wobble);
    setP (*proc.gravity, ps.gravity);
    setP (*proc.gravRel, ps.gravRel);
    setP (*proc.width, ps.width);
    setP (*proc.bloom, ps.bloom);

    currentPreset = index;
    presetName->setText (ps.name, juce::dontSendNotification);
}

void VoidEditor::paint (juce::Graphics& g)
{
    g.fillAll (Colors::panelDark.darker (0.55f));

    g.setColour (Colors::accent.withAlpha (0.04f));
    g.fillRect (getLocalBounds().removeFromTop (44));

    g.setColour (Colors::panelDark);
    g.fillRoundedRectangle (secPreset.toFloat(), 3.0f);
    g.setColour (Colors::outline);
    g.drawRoundedRectangle (secPreset.toFloat(), 3.0f, 1.0f);

    auto section = [&g] (juce::Rectangle<int> r, const juce::String& title)
    {
        g.setColour (Colors::panelDark.withAlpha (0.85f));
        g.fillRoundedRectangle (r.toFloat(), 4.0f);
        g.setColour (Colors::outline);
        g.drawRoundedRectangle (r.toFloat(), 4.0f, 1.0f);
        auto header = r.removeFromTop (16);
        g.setColour (Colors::accent);
        g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
        g.drawText (title, header.reduced (8, 0), juce::Justification::centredLeft, false);
    };
    section (secSpace,   "SPACE");
    section (secColor,   "COLOR");
    section (secShimmer, "SHIMMER");
    section (secMotion,  "MOTION");
    section (secGravity, "GRAVITY");
    section (secOut,     "OUT");
}

void VoidEditor::resized()
{
    auto placeKnob = [this] (int li, juce::Slider& k, juce::Rectangle<int> r)
    {
        labels[li]->setBounds (r.removeFromBottom (12));
        k.setBounds (r.reduced (3));
    };
    auto placeKnobBig = [this] (int li, juce::Slider& k, juce::Rectangle<int> r)
    {
        labels[li]->setBounds (r.removeFromBottom (14));
        const int d = juce::jmin (r.getWidth(), r.getHeight());
        k.setBounds (r.withSizeKeepingCentre (d, d));
    };

    auto full = getLocalBounds().reduced (8);

    secPreset = full.removeFromTop (26);
    full.removeFromTop (8);
    {
        auto c = secPreset.reduced (4, 0);
        prevBtn.setBounds (c.removeFromLeft (26).reduced (0, 2));
        c.removeFromLeft (4);
        nextBtn.setBounds (c.removeFromLeft (26).reduced (0, 2));
        c.removeFromLeft (10);
        presetName->setBounds (c);
    }

    auto left  = full.removeFromLeft (205);
    full.removeFromLeft (8);
    auto right = full.removeFromRight (205);
    full.removeFromRight (8);
    auto centre = full;

    secSpace = left.removeFromTop (230);
    left.removeFromTop (8);
    secColor = left;

    secCentre = centre.removeFromTop (228);
    auto fz = centre.removeFromTop (34);
    centre.removeFromTop (6);
    secShimmer = centre;

    {
        const int d = juce::jmin (secCentre.getWidth(), secCentre.getHeight()) - 8;
        if (horizon) horizon->setBounds (secCentre.withSizeKeepingCentre (d, d));
        if (freezeBtn) freezeBtn->setBounds (fz.reduced (36, 2));
    }

    secMotion = right.removeFromTop (96);
    right.removeFromTop (8);
    secGravity = right.removeFromTop (96);
    right.removeFromTop (8);
    secOut = right;

    {
        auto c = secSpace; c.removeFromTop (16); c = c.reduced (8, 6);
        placeKnobBig (0, expanseK, c.removeFromTop (108));
        c.removeFromTop (4);
        const int kw = c.getWidth() / 3;
        placeKnob (1, decayK,    c.removeFromLeft (kw));
        placeKnob (2, predelayK, c.removeFromLeft (kw));
        placeKnob (3, diffuseK,  c);
    }
    {
        auto c = secColor; c.removeFromTop (16); c = c.reduced (8, 6);
        const int kw = c.getWidth() / 3;
        placeKnob (4, floorK,  c.removeFromLeft (kw));
        placeKnob (5, airK,    c.removeFromLeft (kw));
        placeKnob (6, warmthK, c);
    }
    {
        auto c = secShimmer; c.removeFromTop (16); c = c.reduced (8, 6);
        auto kcol = c.removeFromLeft (c.getWidth() / 2);
        placeKnob (7, shimmerK, kcol);
        labels[8]->setBounds (c.removeFromTop (12));
        if (intervalSel) intervalSel->setBounds (c.reduced (2, 4).removeFromTop (30));
    }
    {
        auto c = secMotion; c.removeFromTop (16); c = c.reduced (8, 6);
        const int kw = c.getWidth() / 2;
        placeKnob (9, driftK,  c.removeFromLeft (kw));
        placeKnob (10, wobbleK, c);
    }
    {
        auto c = secGravity; c.removeFromTop (16); c = c.reduced (8, 6);
        const int kw = c.getWidth() / 2;
        placeKnob (11, gravityK, c.removeFromLeft (kw));
        placeKnob (12, releaseK, c);
    }
    {
        auto c = secOut; c.removeFromTop (16); c = c.reduced (8, 6);
        const int rowH = c.getHeight() / 2;
        auto topRow = c.removeFromTop (rowH);
        c.removeFromTop (2);
        auto botRow = c;
        {
            const int kw = topRow.getWidth() / 3;
            placeKnob (13, widthK, topRow.removeFromLeft (kw));
            placeKnob (14, bloomK, topRow.removeFromLeft (kw));
            placeKnob (15, driveK, topRow);
        }
        {
            const int kw = botRow.getWidth() / 2;
            placeKnob (16, dryK, botRow.removeFromLeft (kw));
            placeKnob (17, wetK, botRow);
        }
    }
}
