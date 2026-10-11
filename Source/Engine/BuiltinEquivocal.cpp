#include "BuiltinEquivocal.h"

namespace
{
    struct EquivocalPreset
    {
        const char* name;
        float input;
        float mix;
        float output;
        float verdict;
        float doubt;
        float uncertainty;
        float nerve;
        int listen;
        float memory;
        int clock;
        float feedback;
        float contradiction;
        float orbit;
        float tone;
        float drive;
        float focus;
        float grain;
        float reverse;
        float pitch;
        float scatter;
        float diffuse;
        float width;
        float mercy;
        bool freeze;
    };

    constexpr EquivocalPreset presets[] = {
        { "Clean Alibi",             0, .22f, -1, -.70f, .20f, 0,    45,   0, 18,  0, .05f,  0,     0,    18000, .02f, .90f, 35,  0,    0,   0,    .03f, 1.08f, .10f, false },
        { "Gentle Disagreement",     0, .50f, -1, 0,     .55f, .12f, 180,  0, 240, 0, .42f,  .30f,  .15f, 9000,  .12f, .65f, 90,  .55f, 0,   .20f, .25f, 1.15f, .20f, false },
        { "Parallel Polish",         0, .33f, -1, -.45f, .55f, .03f, 75,   1, 28,  0, .14f,  .12f,  .08f, 14000, .08f, 1,    45,  .15f, 0,   .05f, .10f, 1.22f, .25f, false },
        { "Centered Story",          0, .30f, -1, -.30f, .40f, 0,    120,  2, 12,  0, .08f,  0,     0,    12000, .05f, .65f, 30,  .20f, 0,   0,    .04f, .45f,  .20f, false },
        { "Witness Protection",      0, .55f, -1, -.65f, 1,    0,    15,   0, 240, 3, .22f,  .05f, -.15f, 13000, .12f, 1,    42,  .10f, 0,   .08f, .10f, 1.25f, .15f, false },
        { "Cross Examination",       0, .62f, -1, 0,     .72f, .08f, 90,   2, 240, 5, .72f,  .92f,  .90f, 8200,  .16f, .55f, 70,  .35f, 0,   .22f, .28f, 1.65f, .25f, false },
        { "Objection Sustained",     0, .70f, -1, .15f,  .85f, .05f, 350,  3, 240, 7, .68f,  .45f,  .35f, 6500,  .10f, .45f, 170, .78f, 0,   .22f, .65f, 1.45f, .65f, false },
        { "Split Decision",          0, .75f, -1, 0,     1,    0,    8,    0, 240, 5, .48f,  .65f, -.55f, 10000, .20f, .90f, 55,  .75f, 0,   .20f, .35f, 1.60f, .05f, false },
        { "Hearsay Eighths",         0, .65f, -1, .55f,  .30f, .10f, 140,  0, 240, 5, .67f,  .32f,  .70f, 7500,  .18f, .40f, 80,  .18f, 0,   .12f, .18f, 1.55f, .30f, false },
        { "Alibi in Triplets",       0, .60f, -1, -.40f, .55f, .08f, 110,  1, 240, 4, .62f,  .40f, -.65f, 9800,  .12f, .85f, 60,  .28f, 0,   .10f, .20f, 1.45f, .22f, false },
        { "False Memory",            0, .78f, -1, .65f,  .38f, .20f, 420,  3, 620, 0, .58f,  .62f,  .18f, 6200,  .12f, .35f, 190, .78f, 0,   .55f, .68f, 1.55f, .30f, false },
        { "Reverse Testimony",       0, .85f, -1, .85f,  .15f, .05f, 220,  0, 240, 7, .52f,  .30f,  .55f, 10500, .08f, .30f, 240, 1,    0,   .15f, .42f, 1.50f, .20f, false },
        { "Amber Afterimage",        0, .72f, -2, .55f,  .45f, .25f, 600,  1, 420, 0, .72f,  .55f, -.25f, 3800,  .22f, .25f, 210, .82f, -5,  .38f, .82f, 1.40f, .35f, false },
        { "Glass Rumor",             0, .74f, -3, .72f,  .32f, .12f, 300,  1, 240, 8, .66f,  .38f,  .42f, 16000, .05f, .50f, 150, .62f, 12,  .26f, .55f, 1.75f, .40f, false },
        { "Basement Witness",        0, .68f, -2, -.55f, .40f, .10f, 260,  3, 240, 6, .78f,  .52f, -.20f, 1800,  .48f, .70f, 180, .72f, -12, .18f, .58f, 1.15f, .18f, false },
        { "Endless Appeal",          0, .80f, -4, .15f,  .62f, .18f, 700,  4, 240, 9, .985f, .17f,  .28f, 11000, .08f, .55f, 300, .65f, 0,   .25f, .88f, 1.80f, .55f, false },
        { "Nervous Jury",            0, .62f, -1, 0,     .92f, .85f, 8,    4, 38,  0, .38f,  .76f,  .82f, 12500, .24f, .75f, 25,  .50f, 3,   .82f, .18f, 1.70f, .08f, false },
        { "Slow Consensus",          0, .76f, -1, .05f,  .68f, .52f, 1400, 4, 900, 0, .78f,  .48f, -.18f, 7200,  .12f, .35f, 270, .66f, -2,  .34f, .74f, 1.65f, .48f, false },
        { "Bipolar Orbit",           0, .72f, -1, 0,     .92f, .18f, 55,   2, 240, 7, .73f,  1,    -1,    9000,  .20f, .62f, 95,  .45f, 0,   .25f, .35f, 2,     .18f, false },
        { "Unreliable Narrator",     0, .82f, -1, .35f,  .80f, .55f, 190,  3, 180, 0, .64f,  .88f,  .50f, 5800,  .28f, .30f, 48,  .58f, -3,  1,    .48f, 1.70f, .22f, false },
        { "Plausible Deniability",   0, .58f, -3, .25f,  1,    .70f, 10,   4, 240, 1, .54f,  .72f,  .35f, 6800,  .55f, .80f, 12,  .42f, 7,   .75f, .08f, 1.65f, .05f, false },
        { "Hostile Witness",         0, .70f, -5, -.85f, .35f, .02f, 35,   0, 240, 3, 1.02f, .65f, -.38f, 5500,  .88f, 1,    38,  .12f, 0,   .12f, .10f, 1.25f, .05f, false },
        { "Data Corruption",         0, .88f, -5, .45f,  .85f, .75f, 18,   1, 240, 3, .61f,  .95f,  .72f, 15000, .68f, .95f, 15,  .62f, 12,  1,    .12f, 1.85f, 0,    false },
        { "No Further Questions",    0, 1,    -4, .25f,  .90f, .32f, 600,  0, 240, 9, .94f,  1,     .75f, 5200,  .32f, .42f, 360, 1,    -5,  .72f, 1,    2,     .38f, false },
        { "Frozen Deposition",       0, .85f, -3, .20f,  .50f, 0,    300,  0, 740, 0, .95f,  .55f,  .25f, 10000, .05f, .50f, 240, .70f, 0,   .20f, .80f, 1.80f, .20f, true  }
    };

    constexpr double beatDivisions[] = {
        0.0, 0.125, 1.0 / 6.0, 0.25, 1.0 / 3.0, 0.5, 2.0 / 3.0, 1.0, 2.0, 4.0
    };

    static_assert (std::size (presets) == 25);

    template <typename Parameter>
    void setActualValue (Parameter* parameter, float value)
    {
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
    }
}

juce::NormalisableRange<float> EquivocalProcessor::skewedRange (float minimum, float maximum,
                                                                 float centre)
{
    auto range = juce::NormalisableRange<float> (minimum, maximum);
    range.setSkewForCentre (centre);
    return range;
}

EquivocalProcessor::EquivocalProcessor()
{
    addParameter (input = new juce::AudioParameterFloat (
        juce::ParameterID { "input", 1 }, "Input",
        juce::NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f));
    addParameter (mix = new juce::AudioParameterFloat (
        juce::ParameterID { "mix", 1 }, "Mix",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.50f));
    addParameter (output = new juce::AudioParameterFloat (
        juce::ParameterID { "output", 1 }, "Output",
        juce::NormalisableRange<float> (-24.0f, 12.0f, 0.1f), -1.0f));
    addParameter (verdict = new juce::AudioParameterFloat (
        juce::ParameterID { "verdict", 1 }, "Verdict",
        juce::NormalisableRange<float> (-1.0f, 1.0f, 0.001f), 0.0f));
    addParameter (doubt = new juce::AudioParameterFloat (
        juce::ParameterID { "doubt", 1 }, "Doubt",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.55f));
    addParameter (uncertainty = new juce::AudioParameterFloat (
        juce::ParameterID { "uncertainty", 1 }, "Uncertainty",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.12f));
    addParameter (nerve = new juce::AudioParameterFloat (
        juce::ParameterID { "nerve", 1 }, "Nerve",
        skewedRange (5.0f, 1500.0f, 180.0f), 180.0f));
    addParameter (listen = new juce::AudioParameterChoice (
        juce::ParameterID { "listen", 1 }, "Listen",
        { "Transient", "Brightness", "Width", "Level", "Wander" }, 0));
    addParameter (memory = new juce::AudioParameterFloat (
        juce::ParameterID { "memory", 1 }, "Memory",
        skewedRange (8.0f, 2000.0f, 240.0f), 240.0f));
    addParameter (clock = new juce::AudioParameterChoice (
        juce::ParameterID { "clock", 1 }, "Clock",
        { "Free", "1/32", "1/16T", "1/16", "1/8T", "1/8", "1/4T", "1/4", "1/2", "1 Bar" }, 0));
    addParameter (feedback = new juce::AudioParameterFloat (
        juce::ParameterID { "feedback", 1 }, "Feedback",
        juce::NormalisableRange<float> (0.0f, 1.05f, 0.001f), 0.42f));
    addParameter (contradiction = new juce::AudioParameterFloat (
        juce::ParameterID { "contradiction", 1 }, "Contradiction",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.30f));
    addParameter (orbit = new juce::AudioParameterFloat (
        juce::ParameterID { "orbit", 1 }, "Orbit",
        juce::NormalisableRange<float> (-1.0f, 1.0f, 0.001f), 0.15f));
    addParameter (tone = new juce::AudioParameterFloat (
        juce::ParameterID { "tone", 1 }, "Tone",
        skewedRange (300.0f, 20000.0f, 5000.0f), 9000.0f));
    addParameter (drive = new juce::AudioParameterFloat (
        juce::ParameterID { "drive", 1 }, "Drive",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.12f));
    addParameter (focus = new juce::AudioParameterFloat (
        juce::ParameterID { "focus", 1 }, "Focus",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.65f));
    addParameter (grain = new juce::AudioParameterFloat (
        juce::ParameterID { "grain", 1 }, "Grain",
        skewedRange (12.0f, 400.0f, 90.0f), 90.0f));
    addParameter (reverse = new juce::AudioParameterFloat (
        juce::ParameterID { "reverse", 1 }, "Reverse",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.55f));
    addParameter (pitch = new juce::AudioParameterFloat (
        juce::ParameterID { "pitch", 1 }, "Pitch",
        juce::NormalisableRange<float> (-12.0f, 12.0f, 0.01f), 0.0f));
    addParameter (scatter = new juce::AudioParameterFloat (
        juce::ParameterID { "scatter", 1 }, "Scatter",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.20f));
    addParameter (diffuse = new juce::AudioParameterFloat (
        juce::ParameterID { "diffuse", 1 }, "Diffuse",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.25f));
    addParameter (width = new juce::AudioParameterFloat (
        juce::ParameterID { "width", 1 }, "Width",
        juce::NormalisableRange<float> (0.0f, 2.0f, 0.001f), 1.15f));
    addParameter (mercy = new juce::AudioParameterFloat (
        juce::ParameterID { "mercy", 1 }, "Mercy",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.20f));
    addParameter (freeze = new juce::AudioParameterBool (
        juce::ParameterID { "freeze", 1 }, "Freeze", false));
}

void EquivocalProcessor::History::resize (int samples)
{
    for (auto& samplesForChannel : channel)
        samplesForChannel.assign ((size_t) samples, 0.0f);
    writePosition = 0;
}

void EquivocalProcessor::History::clear()
{
    for (auto& samplesForChannel : channel)
        std::fill (samplesForChannel.begin(), samplesForChannel.end(), 0.0f);
    writePosition = 0;
}

float EquivocalProcessor::History::read (int channelIndex, float delaySamples) const noexcept
{
    const auto& samples = channel[(size_t) juce::jlimit (0, 1, channelIndex)];
    const int size = (int) samples.size();
    if (size < 4)
        return 0.0f;

    float position = (float) writePosition - juce::jlimit (2.0f, (float) size - 3.0f, delaySamples);
    while (position < 0.0f)
        position += (float) size;
    while (position >= (float) size)
        position -= (float) size;

    const int index1 = (int) position;
    const float fraction = position - (float) index1;
    const int index0 = (index1 - 1 + size) % size;
    const int index2 = (index1 + 1) % size;
    const int index3 = (index1 + 2) % size;
    const float y0 = samples[(size_t) index0];
    const float y1 = samples[(size_t) index1];
    const float y2 = samples[(size_t) index2];
    const float y3 = samples[(size_t) index3];
    const float a = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
    const float b = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float c = -0.5f * y0 + 0.5f * y2;
    return ((a * fraction + b) * fraction + c) * fraction + y1;
}

void EquivocalProcessor::History::write (float left, float right) noexcept
{
    if (channel[0].empty())
        return;
    channel[0][(size_t) writePosition] = left;
    channel[1][(size_t) writePosition] = right;
    if (++writePosition >= (int) channel[0].size())
        writePosition = 0;
}

void EquivocalProcessor::Allpass::resize (int samples)
{
    buffer.assign ((size_t) juce::jmax (2, samples), 0.0f);
    position = 0;
}

void EquivocalProcessor::Allpass::clear()
{
    std::fill (buffer.begin(), buffer.end(), 0.0f);
    position = 0;
}

float EquivocalProcessor::Allpass::process (float inputSample, float coefficient) noexcept
{
    if (buffer.empty())
        return inputSample;
    const float delayed = buffer[(size_t) position];
    const float outputSample = delayed - coefficient * inputSample;
    buffer[(size_t) position] = inputSample + coefficient * outputSample;
    if (++position >= (int) buffer.size())
        position = 0;
    return outputSample;
}

void EquivocalProcessor::prepareToPlay (double sampleRate, int)
{
    sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    historySize = (int) std::ceil (sr * 14.0) + 8;
    for (auto& history : histories)
        history.resize (historySize);

    constexpr float diffuserMilliseconds[3] { 2.3f, 3.7f, 5.9f };
    for (int timeline = 0; timeline < 2; ++timeline)
        for (int channelIndex = 0; channelIndex < 2; ++channelIndex)
            for (int stage = 0; stage < 3; ++stage)
            {
                const float offset = 0.29f * (float) timeline + 0.17f * (float) channelIndex;
                diffusers[timeline][channelIndex][stage].resize (
                    (int) std::round ((diffuserMilliseconds[stage] + offset) * 0.001 * sr));
            }

    for (auto& timeline : toneState)
        for (auto& channel : timeline)
            channel = 0.0f;
    grainPhase[0] = 0.0f;
    grainPhase[1] = 0.5f;
    grainScatter[0] = nextRandomBipolar();
    grainScatter[1] = nextRandomBipolar();
    arbiterState = 0.5f;
    randomState = 0.0f;
    randomCountdown = 0;
    previousMid = 0.0f;
    fastEnvelope = 0.0f;
    slowEnvelope = 0.0f;
    brightnessEnvelope = 0.0f;
    midEnvelope = 0.0f;
    sideEnvelope = 0.0f;
    wanderState = 0.0f;
    loopEnvelope = 0.0f;
    fastAttackCoefficient = (float) std::exp (-1.0 / (0.0015 * sr));
    fastReleaseCoefficient = (float) std::exp (-1.0 / (0.028 * sr));
    featureSlowCoefficient = (float) std::exp (-1.0 / (0.11 * sr));
    brightnessCoefficient = (float) std::exp (-1.0 / (0.018 * sr));
    widthCoefficient = (float) std::exp (-1.0 / (0.06 * sr));
    wanderCoefficient = 1.0f - (float) std::exp (-1.0 / (0.55 * sr));
    resetSmoothers();
}

void EquivocalProcessor::releaseResources()
{
    for (auto& history : histories)
    {
        for (auto& channel : history.channel)
            channel.clear();
        history.writePosition = 0;
    }
    for (auto& timeline : diffusers)
        for (auto& channel : timeline)
            for (auto& diffuser : channel)
            {
                diffuser.buffer.clear();
                diffuser.position = 0;
            }
    historySize = 0;
}

void EquivocalProcessor::reset()
{
    for (auto& history : histories)
        history.clear();
    for (auto& timeline : diffusers)
        for (auto& channel : timeline)
            for (auto& diffuser : channel)
                diffuser.clear();

    for (auto& timeline : toneState)
        for (auto& channel : timeline)
            channel = 0.0f;

    grainPhase[0] = 0.0f;
    grainPhase[1] = 0.5f;
    grainScatter[0] = 0.0f;
    grainScatter[1] = 0.0f;
    arbiterState = 0.5f;
    randomState = 0.0f;
    randomCountdown = 0;
    previousMid = 0.0f;
    fastEnvelope = 0.0f;
    slowEnvelope = 0.0f;
    brightnessEnvelope = 0.0f;
    midEnvelope = 0.0f;
    sideEnvelope = 0.0f;
    wanderState = 0.0f;
    loopEnvelope = 0.0f;

    resetSmoothers();
}

void EquivocalProcessor::resetSmoothers()
{
    auto initialise = [this] (auto& smoother, float value, double seconds)
    {
        smoother.reset (sr, seconds);
        smoother.setCurrentAndTargetValue (value);
    };

    initialise (inputGainSm, juce::Decibels::decibelsToGain (input->get()), 0.02);
    initialise (mixSm, mix->get(), 0.02);
    initialise (outputGainSm, juce::Decibels::decibelsToGain (output->get()), 0.02);
    initialise (delaySm, currentDelaySamples(), 0.08);
    initialise (feedbackSm, freeze->get() ? 1.0f : feedback->get(), 0.04);
    initialise (injectionSm, freeze->get() ? 0.0f : 1.0f, 0.04);
    initialise (contradictionSm, contradiction->get(), 0.03);
    initialise (orbitSm, orbit->get(), 0.03);
    initialise (toneSm, tone->get(), 0.03);
    initialise (driveSm, drive->get(), 0.03);
    initialise (focusSm, focus->get(), 0.03);
    initialise (grainSm, grain->get() * 0.001f * (float) sr, 0.05);
    initialise (reverseSm, reverse->get(), 0.03);
    initialise (pitchSm, std::exp2 (pitch->get() / 12.0f), 0.03);
    initialise (scatterSm, scatter->get(), 0.03);
    initialise (diffuseSm, diffuse->get(), 0.03);
    initialise (widthSm, width->get(), 0.03);
    initialise (mercySm, mercy->get(), 0.03);
}

float EquivocalProcessor::clampUnit (float value) noexcept
{
    return juce::jlimit (0.0f, 1.0f, value);
}

float EquivocalProcessor::shapedLoopSample (float sample, float driveAmount) noexcept
{
    const float magnitude = std::abs (sample);
    const float knee = 1.15f - 0.55f * clampUnit (driveAmount);
    if (magnitude <= knee)
        return sample;
    const float shape = 0.35f + 3.65f * clampUnit (driveAmount);
    const float excess = magnitude - knee;
    const float shaped = knee + excess / (1.0f + shape * excess);
    return std::copysign (shaped, sample);
}

float EquivocalProcessor::equalPowerHann (float phase) noexcept
{
    return 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * phase);
}

float EquivocalProcessor::nextRandomBipolar() noexcept
{
    randomSeed = randomSeed * 1664525u + 1013904223u;
    return (float) ((randomSeed >> 8) & 0x00ffffffu) * (2.0f / 16777215.0f) - 1.0f;
}

float EquivocalProcessor::updateFeature (float left, float right, int mode) noexcept
{
    const float mid = 0.5f * (left + right);
    const float side = 0.5f * (left - right);
    const float magnitude = std::abs (mid);
    const float fastCoefficient = magnitude > fastEnvelope
        ? fastAttackCoefficient : fastReleaseCoefficient;
    fastEnvelope = fastCoefficient * fastEnvelope + (1.0f - fastCoefficient) * magnitude;
    slowEnvelope = featureSlowCoefficient * slowEnvelope
                 + (1.0f - featureSlowCoefficient) * magnitude;
    const float high = std::abs (mid - previousMid);
    previousMid = mid;
    brightnessEnvelope = brightnessCoefficient * brightnessEnvelope
                       + (1.0f - brightnessCoefficient) * high;
    midEnvelope = widthCoefficient * midEnvelope + (1.0f - widthCoefficient) * magnitude;
    sideEnvelope = widthCoefficient * sideEnvelope
                 + (1.0f - widthCoefficient) * std::abs (side);
    wanderState += (mid - wanderState) * wanderCoefficient;

    switch (mode)
    {
        case 0:
            return 1.0f - clampUnit (0.5f + 1.8f * (fastEnvelope - slowEnvelope)
                                           / (slowEnvelope + 0.025f));
        case 1:
            return 1.0f - clampUnit (brightnessEnvelope
                / (brightnessEnvelope + slowEnvelope * 0.32f + 0.0001f));
        case 2:
            return clampUnit (sideEnvelope / (sideEnvelope + midEnvelope + 0.0001f));
        case 3:
            return 1.0f - clampUnit (slowEnvelope / (slowEnvelope + 0.16f));
        default:
            return clampUnit (0.5f + 0.5f * std::tanh (
                2.4f * wanderState / (slowEnvelope + 0.004f)));
    }
}

float EquivocalProcessor::currentDelaySamples()
{
    if (auto* processorPlayHead = getPlayHead())
    {
        if (auto position = processorPlayHead->getPosition())
        {
            if (auto bpm = position->getBpm())
            {
                const double candidate = *bpm;
                if (std::isfinite (candidate) && candidate >= 10.0 && candidate <= 999.0)
                    currentTempo = candidate;
            }
        }
    }

    const int clockIndex = juce::jlimit (0, 9, clock->getIndex());
    const double seconds = clockIndex == 0
        ? (double) memory->get() * 0.001
        : beatDivisions[clockIndex] * 60.0 / currentTempo;
    const float maximum = juce::jmax (4.0f, (float) historySize - (float) sr * 1.55f - 8.0f);
    return juce::jlimit (4.0f, maximum, (float) (seconds * sr));
}

float EquivocalProcessor::readRumor (int channelIndex, float delaySamples, float grainSamples,
                                      float speed, float scatterAmount) noexcept
{
    const float guard = speed > 1.0f ? (speed - 1.0f) * grainSamples + 4.0f : 0.0f;
    float result = 0.0f;
    float weight = 0.0f;
    for (int grainIndex = 0; grainIndex < 2; ++grainIndex)
    {
        const float phase = grainPhase[grainIndex];
        const float age = phase * grainSamples;
        const float start = delaySamples + guard
                          + grainScatter[grainIndex] * scatterAmount * grainSamples * 0.72f;
        const float readDelay = start + age * (1.0f - speed);
        const float window = equalPowerHann (phase);
        result += histories[1].read (channelIndex, readDelay) * window;
        weight += window;
    }
    return weight > 0.0001f ? result / weight : 0.0f;
}

float EquivocalProcessor::diffuseSample (int timeline, int channelIndex, float sample,
                                          float amount, bool energyPreserving) noexcept
{
    const float blend = clampUnit (amount);
    if (blend <= 0.0f)
        return sample;
    const float coefficient = 0.72f * blend;
    float diffused = sample;
    for (int stage = 0; stage < 3; ++stage)
        diffused = diffusers[timeline][channelIndex][stage].process (diffused, coefficient);
    return energyPreserving ? diffused : sample + blend * (diffused - sample);
}

void EquivocalProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin (2, buffer.getNumChannels());
    if (numSamples <= 0 || numChannels <= 0 || historySize < 8)
        return;

    const bool frozen = freeze->get();
    inputGainSm.setTargetValue (juce::Decibels::decibelsToGain (input->get()));
    mixSm.setTargetValue (mix->get());
    outputGainSm.setTargetValue (juce::Decibels::decibelsToGain (output->get()));
    delaySm.setTargetValue (currentDelaySamples());
    feedbackSm.setTargetValue (frozen ? 1.0f : feedback->get());
    injectionSm.setTargetValue (frozen ? 0.0f : 1.0f);
    contradictionSm.setTargetValue (contradiction->get());
    orbitSm.setTargetValue (orbit->get());
    toneSm.setTargetValue (tone->get());
    driveSm.setTargetValue (drive->get());
    focusSm.setTargetValue (focus->get());
    grainSm.setTargetValue (grain->get() * 0.001f * (float) sr);
    reverseSm.setTargetValue (reverse->get());
    pitchSm.setTargetValue (std::exp2 (pitch->get() / 12.0f));
    scatterSm.setTargetValue (scatter->get());
    diffuseSm.setTargetValue (diffuse->get());
    widthSm.setTargetValue (width->get());
    mercySm.setTargetValue (mercy->get());

    const float manualVerdict = 0.5f * (verdict->get() + 1.0f);
    const float doubtAmount = doubt->get();
    const float uncertaintyAmount = uncertainty->get();
    const float arbiterCoefficient = 1.0f - (float) std::exp (
        -1.0 / (juce::jmax (5.0f, nerve->get()) * 0.001 * sr));
    const int listenMode = listen->getIndex();
    const float loopAttack = (float) std::exp (-1.0 / (0.004 * sr));
    const float loopRelease = (float) std::exp (-1.0 / (0.22 * sr));

    double witnessEnergy = 0.0;
    double rumorEnergy = 0.0;
    double feedbackEnergy = 0.0;
    float lastFeature = 0.0f;
    float lastMercyGain = 1.0f;

    for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
    {
        const float dryLeft = buffer.getSample (0, sampleIndex);
        const float dryRight = numChannels > 1 ? buffer.getSample (1, sampleIndex) : dryLeft;
        const float inputGainNow = inputGainSm.getNextValue();
        const float inputLeft = dryLeft * inputGainNow;
        const float inputRight = dryRight * inputGainNow;

        lastFeature = updateFeature (inputLeft, inputRight, listenMode);
        if (--randomCountdown <= 0)
        {
            randomState += (nextRandomBipolar() - randomState) * 0.22f;
            randomCountdown = 48;
        }
        const float featureVerdict = manualVerdict
                                   + doubtAmount * (lastFeature - manualVerdict);
        const float arbiterTarget = clampUnit (featureVerdict
                                              + randomState * uncertaintyAmount * 0.5f);
        arbiterState += (arbiterTarget - arbiterState) * arbiterCoefficient;

        const float delayNow = delaySm.getNextValue();
        const float focusNow = focusSm.getNextValue();
        const float grainNow = juce::jmax (8.0f, grainSm.getNextValue());
        const float focusOffset = juce::jlimit (2.0f, grainNow * 0.35f,
                                                juce::jmax (2.0f, delayNow * 0.12f));
        float witness[2] {};
        float rumor[2] {};
        const float reverseNow = reverseSm.getNextValue();
        const float speed = pitchSm.getNextValue() * (1.0f - 2.0f * reverseNow);
        const float scatterNow = scatterSm.getNextValue();

        for (int channelIndex = 0; channelIndex < 2; ++channelIndex)
        {
            const float centre = histories[0].read (channelIndex, delayNow);
            const float near = histories[0].read (channelIndex,
                                                  juce::jmax (2.0f, delayNow - focusOffset));
            const float far = histories[0].read (channelIndex, delayNow + focusOffset);
            const float blurred = 0.25f * near + 0.5f * centre + 0.25f * far;
            witness[channelIndex] = centre + 1.6f * focusNow * (centre - blurred);
            rumor[channelIndex] = readRumor (channelIndex, delayNow, grainNow,
                                             speed, scatterNow);
            witnessEnergy += (double) witness[channelIndex] * witness[channelIndex];
            rumorEnergy += (double) rumor[channelIndex] * rumor[channelIndex];
        }

        const float diffuseNow = diffuseSm.getNextValue();
        const float driveNow = driveSm.getNextValue();
        const float toneNow = juce::jlimit (40.0f, (float) sr * 0.47f,
                                            toneSm.getNextValue());
        const float tonePole = std::exp (-juce::MathConstants<float>::twoPi
                                         * toneNow / (float) sr);
        float timeline[2][2] {
            { witness[0], witness[1] },
            { rumor[0], rumor[1] }
        };

        for (int timelineIndex = 0; timelineIndex < 2; ++timelineIndex)
        {
            for (int channelIndex = 0; channelIndex < 2; ++channelIndex)
            {
                auto& state = toneState[timelineIndex][channelIndex];
                state = tonePole * state + (1.0f - tonePole)
                      * timeline[timelineIndex][channelIndex];
                const float toned = frozen ? timeline[timelineIndex][channelIndex] : state;
                timeline[timelineIndex][channelIndex] = shapedLoopSample (
                    diffuseSample (timelineIndex, channelIndex, toned, diffuseNow, frozen),
                    driveNow);
            }
        }

        const float contradictionAngle = contradictionSm.getNextValue()
                                       * juce::MathConstants<float>::halfPi;
        const float contradictionCos = std::cos (contradictionAngle);
        const float contradictionSin = std::sin (contradictionAngle);
        float rotated[2][2] {};
        for (int channelIndex = 0; channelIndex < 2; ++channelIndex)
        {
            rotated[0][channelIndex] = contradictionCos * timeline[0][channelIndex]
                                     - contradictionSin * timeline[1][channelIndex];
            rotated[1][channelIndex] = contradictionSin * timeline[0][channelIndex]
                                     + contradictionCos * timeline[1][channelIndex];
        }

        const float orbitAngle = orbitSm.getNextValue()
                               * juce::MathConstants<float>::halfPi * 0.5f;
        const float orbitCos = std::cos (orbitAngle);
        const float orbitSin = std::sin (orbitAngle);
        for (int timelineIndex = 0; timelineIndex < 2; ++timelineIndex)
        {
            const float left = rotated[timelineIndex][0];
            const float right = rotated[timelineIndex][1];
            rotated[timelineIndex][0] = orbitCos * left - orbitSin * right;
            rotated[timelineIndex][1] = orbitSin * left + orbitCos * right;
        }

        const float loopDetector = juce::jmax (
            juce::jmax (std::abs (rotated[0][0]), std::abs (rotated[0][1])),
            juce::jmax (std::abs (rotated[1][0]), std::abs (rotated[1][1])));
        const float loopCoefficient = loopDetector > loopEnvelope ? loopAttack : loopRelease;
        loopEnvelope = loopCoefficient * loopEnvelope
                     + (1.0f - loopCoefficient) * loopDetector;
        const float mercyNow = mercySm.getNextValue();
        const float overload = clampUnit ((loopEnvelope - 0.62f) * 2.3f);
        lastMercyGain = frozen ? 1.0f : 1.0f - mercyNow * overload * 0.82f;
        const float feedbackNow = feedbackSm.getNextValue() * lastMercyGain;
        const float injectionNow = injectionSm.getNextValue();

        const float historyALeft = shapedLoopSample (
            injectionNow * inputLeft + feedbackNow * rotated[0][0], driveNow);
        const float historyARight = shapedLoopSample (
            injectionNow * inputRight + feedbackNow * rotated[0][1], driveNow);
        const float historyBLeft = shapedLoopSample (
            injectionNow * inputRight + feedbackNow * rotated[1][0], driveNow);
        const float historyBRight = shapedLoopSample (
            injectionNow * inputLeft + feedbackNow * rotated[1][1], driveNow);
        histories[0].write (historyALeft, historyARight);
        histories[1].write (historyBLeft, historyBRight);
        feedbackEnergy += (double) historyALeft * historyALeft
                        + (double) historyARight * historyARight
                        + (double) historyBLeft * historyBLeft
                        + (double) historyBRight * historyBRight;

        for (int grainIndex = 0; grainIndex < 2; ++grainIndex)
        {
            grainPhase[grainIndex] += 1.0f / grainNow;
            if (grainPhase[grainIndex] >= 1.0f)
            {
                grainPhase[grainIndex] -= 1.0f;
                grainScatter[grainIndex] = nextRandomBipolar();
            }
        }

        const float wetLeft = witness[0] + arbiterState * (rumor[0] - witness[0]);
        const float wetRight = witness[1] + arbiterState * (rumor[1] - witness[1]);
        const float wetMid = 0.5f * (wetLeft + wetRight);
        const float wetSide = 0.5f * (wetLeft - wetRight) * widthSm.getNextValue();
        const float mixNow = mixSm.getNextValue();
        const float outputNow = outputGainSm.getNextValue();
        const float resultLeft = ((1.0f - mixNow) * dryLeft
                                + mixNow * (wetMid + wetSide)) * outputNow;
        const float resultRight = ((1.0f - mixNow) * dryRight
                                 + mixNow * (wetMid - wetSide)) * outputNow;
        buffer.setSample (0, sampleIndex, resultLeft);
        if (numChannels > 1)
            buffer.setSample (1, sampleIndex, resultRight);
    }

    const float stereoScale = 1.0f / (float) (numSamples * 2);
    visualArbiter.store (arbiterState, std::memory_order_relaxed);
    visualFeature.store (lastFeature, std::memory_order_relaxed);
    visualWitnessRms.store (std::sqrt ((float) witnessEnergy * stereoScale),
                            std::memory_order_relaxed);
    visualRumorRms.store (std::sqrt ((float) rumorEnergy * stereoScale),
                          std::memory_order_relaxed);
    visualFeedbackRms.store (std::sqrt ((float) feedbackEnergy
                                       / (float) (numSamples * 4)),
                             std::memory_order_relaxed);
    visualGrainPhase.store (grainPhase[0], std::memory_order_relaxed);
    visualDelayMs.store (delaySm.getCurrentValue() * 1000.0f / (float) sr,
                         std::memory_order_relaxed);
    visualMercyGain.store (lastMercyGain, std::memory_order_relaxed);
}

int EquivocalProcessor::getCurrentProgram()
{
    return currentProgram.load (std::memory_order_relaxed);
}

void EquivocalProcessor::setCurrentProgram (int index)
{
    index = juce::jlimit (0, programCount - 1, index);
    const auto& preset = presets[index];
    setActualValue (input, preset.input);
    setActualValue (mix, preset.mix);
    setActualValue (output, preset.output);
    setActualValue (verdict, preset.verdict);
    setActualValue (doubt, preset.doubt);
    setActualValue (uncertainty, preset.uncertainty);
    setActualValue (nerve, preset.nerve);
    setActualValue (listen, (float) preset.listen);
    setActualValue (memory, preset.memory);
    setActualValue (clock, (float) preset.clock);
    setActualValue (feedback, preset.feedback);
    setActualValue (contradiction, preset.contradiction);
    setActualValue (orbit, preset.orbit);
    setActualValue (tone, preset.tone);
    setActualValue (drive, preset.drive);
    setActualValue (focus, preset.focus);
    setActualValue (grain, preset.grain);
    setActualValue (reverse, preset.reverse);
    setActualValue (pitch, preset.pitch);
    setActualValue (scatter, preset.scatter);
    setActualValue (diffuse, preset.diffuse);
    setActualValue (width, preset.width);
    setActualValue (mercy, preset.mercy);
    setActualValue (freeze, preset.freeze ? 1.0f : 0.0f);
    currentProgram.store (index, std::memory_order_relaxed);
}

const juce::String EquivocalProcessor::getProgramName (int index)
{
    return juce::isPositiveAndBelow (index, programCount) ? presets[index].name : juce::String();
}

juce::String EquivocalProcessor::getProgramCategory (int index) const
{
    if (index < 0 || index >= programCount)
        return {};
    if (index < 4)
        return "Foundation";
    if (index < 10)
        return "Rhythm";
    if (index < 16)
        return "Space";
    if (index < 20)
        return "Motion";
    return "Damage/Performance";
}

juce::StringArray EquivocalProcessor::getProgramCategories()
{
    return { "Foundation", "Rhythm", "Space", "Motion", "Damage/Performance" };
}
