#include "BuiltinParterre.h"

namespace
{
    juce::NormalisableRange<float> skewedRange (float minimum, float maximum, float centre)
    {
        auto range = juce::NormalisableRange<float> (minimum, maximum);
        range.setSkewForCentre (centre);
        return range;
    }

    float frequencyToX (float frequency, juce::Rectangle<float> bounds)
    {
        constexpr float minimum = 30.0f;
        constexpr float maximum = 18000.0f;
        const float proportion = std::log (juce::jlimit (minimum, maximum, frequency) / minimum)
                               / std::log (maximum / minimum);
        return bounds.getX() + proportion * bounds.getWidth();
    }
}

ParterreProcessor::ParterreProcessor()
{
    addParameter (focus = new juce::AudioParameterFloat (
        juce::ParameterID { "focus", 1 }, "Focus",
        skewedRange (70.0f, 6000.0f, 900.0f), 900.0f));
    addParameter (spread = new juce::AudioParameterFloat (
        juce::ParameterID { "spread", 1 }, "Spread",
        juce::NormalisableRange<float> (0.15f, 1.4f, 0.001f), 0.65f));
    addParameter (growth = new juce::AudioParameterFloat (
        juce::ParameterID { "growth", 1 }, "Growth",
        skewedRange (0.7f, 8.0f, 2.0f), 2.0f));
    addParameter (bloom = new juce::AudioParameterFloat (
        juce::ParameterID { "bloom", 1 }, "Bloom",
        juce::NormalisableRange<float> (0.0f, 1.5f, 0.001f), 0.9f));
    addParameter (motion = new juce::AudioParameterFloat (
        juce::ParameterID { "motion", 1 }, "Motion",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.25f));
    addParameter (rate = new juce::AudioParameterFloat (
        juce::ParameterID { "rate", 1 }, "Rate",
        skewedRange (0.02f, 6.0f, 0.22f), 0.22f));
    addParameter (react = new juce::AudioParameterFloat (
        juce::ParameterID { "react", 1 }, "React",
        juce::NormalisableRange<float> (-1.0f, 1.0f, 0.001f), 0.15f));
    addParameter (mirror = new juce::AudioParameterFloat (
        juce::ParameterID { "mirror", 1 }, "Mirror",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.35f));
    addParameter (mix = new juce::AudioParameterFloat (
        juce::ParameterID { "mix", 1 }, "Mix",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.4f));
    addParameter (output = new juce::AudioParameterFloat (
        juce::ParameterID { "output", 1 }, "Output",
        juce::NormalisableRange<float> (-12.0f, 6.0f, 0.01f), -1.0f));

    for (int channel = 0; channel < 2; ++channel)
        for (int bed = 0; bed < numBeds; ++bed)
            visualFrequency[channel][bed].store (900.0f, std::memory_order_relaxed);
    for (auto& level : visualBedRms)
        level.store (0.0f, std::memory_order_relaxed);
}

void ParterreProcessor::prepareToPlay (double sampleRate, int blockSize)
{
    sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    juce::dsp::ProcessSpec spec { sr, (juce::uint32) juce::jmax (1, blockSize), 1 };

    for (int channel = 0; channel < 2; ++channel)
    {
        for (int bed = 0; bed < numBeds; ++bed)
        {
            filters[channel][bed].prepare (spec);
            filters[channel][bed].setType (juce::dsp::StateVariableTPTFilterType::bandpass);
            filters[channel][bed].reset();
        }
    }

    auto prepareSmoother = [this] (auto& smoother, float value, double seconds)
    {
        smoother.reset (sr, seconds);
        smoother.setCurrentAndTargetValue (value);
    };

    prepareSmoother (focusSm, focus->get(), 0.045);
    prepareSmoother (spreadSm, spread->get(), 0.045);
    prepareSmoother (growthSm, growth->get(), 0.045);
    prepareSmoother (bloomSm, bloom->get(), 0.025);
    prepareSmoother (motionSm, motion->get(), 0.045);
    prepareSmoother (rateSm, rate->get(), 0.08);
    prepareSmoother (reactSm, react->get(), 0.045);
    prepareSmoother (mirrorSm, mirror->get(), 0.045);
    prepareSmoother (mixSm, mix->get(), 0.025);
    prepareSmoother (outputSm, juce::Decibels::decibelsToGain (output->get()), 0.025);

    envelope = 0.0f;
    envelopeAttack = std::exp (-1.0f / (0.012f * (float) sr));
    envelopeRelease = std::exp (-1.0f / (0.18f * (float) sr));
    lfoPhase = 0.0f;
    lastResonance = -1.0f;
    activeResonance = growth->get();
    samplesUntilControl = 0;

    updateFilters (focus->get(), spread->get(), growth->get(), motion->get(),
                   react->get(), mirror->get(), 0.0f);
}

void ParterreProcessor::reset()
{
    for (int channel = 0; channel < 2; ++channel)
        for (int bed = 0; bed < numBeds; ++bed)
            filters[channel][bed].reset();

    for (auto* smoother : { &focusSm, &spreadSm, &growthSm, &bloomSm, &motionSm,
                            &rateSm, &reactSm, &mirrorSm, &mixSm, &outputSm })
        smoother->setCurrentAndTargetValue (smoother->getTargetValue());

    envelope = 0.0f;
    lfoPhase = 0.0f;
    samplesUntilControl = 0;
}

void ParterreProcessor::updateFilters (float focusHz, float spreadOctaves, float q,
                                       float motionOctaves, float reactOctaves,
                                       float mirrorAmount, float envelopeAmount)
{
    const float safeQ = juce::jlimit (0.7f, 8.0f, q);
    if (lastResonance < 0.0f || std::abs (safeQ - lastResonance) >= 0.004f)
    {
        for (int channel = 0; channel < 2; ++channel)
            for (int bed = 0; bed < numBeds; ++bed)
                filters[channel][bed].setResonance (safeQ);
        lastResonance = safeQ;
    }
    activeResonance = lastResonance;

    const float maximumFrequency = juce::jmax (21.0f, juce::jmin (18000.0f, (float) sr * 0.45f));
    for (int channel = 0; channel < 2; ++channel)
    {
        const float stereoPhase = (channel == 0 ? -0.125f : 0.125f) * mirrorAmount;
        for (int bed = 0; bed < numBeds; ++bed)
        {
            const float bedPhase = lfoPhase + 0.25f * (float) bed + stereoPhase;
            const float movement = std::sin (bedPhase * juce::MathConstants<float>::twoPi)
                                 * motionOctaves;
            const float bedPosition = ((float) bed - 1.5f) * spreadOctaves;
            const float octaveOffset = bedPosition + movement + reactOctaves * envelopeAmount;
            const float frequency = juce::jlimit (20.0f, maximumFrequency,
                                                  focusHz * std::exp2 (octaveOffset));
            filters[channel][bed].setCutoffFrequency (frequency);
            currentFrequency[channel][bed] = frequency;
        }
    }
}

void ParterreProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin (2, buffer.getNumChannels());
    if (numSamples <= 0 || numChannels <= 0 || sr <= 0.0)
        return;

    focusSm.setTargetValue (focus->get());
    spreadSm.setTargetValue (spread->get());
    growthSm.setTargetValue (growth->get());
    bloomSm.setTargetValue (bloom->get());
    motionSm.setTargetValue (motion->get());
    rateSm.setTargetValue (rate->get());
    reactSm.setTargetValue (react->get());
    mirrorSm.setTargetValue (mirror->get());
    mixSm.setTargetValue (mix->get());
    outputSm.setTargetValue (juce::Decibels::decibelsToGain (output->get()));

    double bedEnergy[numBeds] {};
    double outputEnergy = 0.0;
    float envelopeAmount = 0.0f;

    for (int sample = 0; sample < numSamples; ++sample)
    {
        float detector = 0.0f;
        for (int channel = 0; channel < numChannels; ++channel)
            detector = juce::jmax (detector, std::abs (buffer.getSample (channel, sample)));

        const float envelopeCoefficient = detector > envelope ? envelopeAttack : envelopeRelease;
        envelope = envelopeCoefficient * envelope + (1.0f - envelopeCoefficient) * detector;
        envelopeAmount = envelope / (envelope + 0.12f);

        const float focusNow = focusSm.getNextValue();
        const float spreadNow = spreadSm.getNextValue();
        const float growthNow = growthSm.getNextValue();
        const float bloomNow = bloomSm.getNextValue();
        const float motionNow = motionSm.getNextValue();
        const float rateNow = rateSm.getNextValue();
        const float reactNow = reactSm.getNextValue();
        const float mirrorNow = mirrorSm.getNextValue();
        const float mixNow = mixSm.getNextValue();
        const float outputNow = outputSm.getNextValue();

        if (samplesUntilControl == 0)
        {
            updateFilters (focusNow, spreadNow, growthNow, motionNow,
                           reactNow, mirrorNow, envelopeAmount);
            samplesUntilControl = controlInterval;
        }
        --samplesUntilControl;

        for (int channel = 0; channel < numChannels; ++channel)
        {
            const float dry = buffer.getSample (channel, sample);
            float wet = 0.0f;
            for (int bed = 0; bed < numBeds; ++bed)
            {
                const float filtered = filters[channel][bed].processSample (0, dry)
                                     / juce::jmax (0.7f, activeResonance);
                wet += filtered;
                bedEnergy[bed] += (double) filtered * (double) filtered;
            }

            wet *= 0.5f * bloomNow;
            const float result = (dry + mixNow * (wet - dry)) * outputNow;
            buffer.setSample (channel, sample, result);
            outputEnergy += (double) result * (double) result;
        }

        lfoPhase += rateNow / (float) sr;
        if (lfoPhase >= 1.0f)
            lfoPhase -= 1.0f;
    }

    const float energyScale = 1.0f / (float) (numSamples * numChannels);
    for (int bed = 0; bed < numBeds; ++bed)
        visualBedRms[bed].store (std::sqrt ((float) bedEnergy[bed] * energyScale),
                                 std::memory_order_relaxed);
    for (int channel = 0; channel < 2; ++channel)
        for (int bed = 0; bed < numBeds; ++bed)
            visualFrequency[channel][bed].store (currentFrequency[channel][bed],
                                                 std::memory_order_relaxed);
    visualEnvelope.store (envelopeAmount, std::memory_order_relaxed);
    visualPhase.store (lfoPhase, std::memory_order_relaxed);
    visualOutputRms.store (std::sqrt ((float) outputEnergy * energyScale),
                           std::memory_order_relaxed);
}

juce::AudioProcessorEditor* ParterreProcessor::createEditor()
{
    return new ParterreEditor (*this);
}

ParterreEditor::ParterreEditor (ParterreProcessor& processor)
    : juce::AudioProcessorEditor (processor), proc (processor)
{
    title.setText ("PARTERRE", juce::dontSendNotification);
    title.setJustificationType (juce::Justification::centredLeft);
    title.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
    title.setColour (juce::Label::textColourId, Colors::accent);
    addAndMakeVisible (title);

    subtitle.setText ("RESONANT FILTER GARDEN", juce::dontSendNotification);
    subtitle.setJustificationType (juce::Justification::centredRight);
    subtitle.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::bold)));
    subtitle.setColour (juce::Label::textColourId, Colors::textDim);
    addAndMakeVisible (subtitle);

    ValueKnob* knobs[] = { &focusK, &spreadK, &growthK, &bloomK, &motionK,
                           &rateK, &reactK, &mirrorK, &mixK, &outputK };
    const char* captions[] = { "FOCUS", "SPREAD", "GROWTH", "BLOOM", "MOTION",
                               "RATE", "REACT", "MIRROR", "MIX", "OUTPUT" };
    const double defaults[] = { 900.0, 0.65, 2.0, 0.9, 0.25,
                                0.22, 0.15, 0.35, 0.4, -1.0 };

    for (int i = 0; i < 10; ++i)
    {
        knobs[i]->setDoubleClickReturnValue (true, defaults[i]);
        knobs[i]->setName (captions[i]);
        addAndMakeVisible (*knobs[i]);

        labels[i].setText (captions[i], juce::dontSendNotification);
        labels[i].setJustificationType (juce::Justification::centred);
        labels[i].setFont (juce::Font (juce::FontOptions (8.5f, juce::Font::bold)));
        labels[i].setColour (juce::Label::textColourId, Colors::textDim);
        addAndMakeVisible (labels[i]);
    }

    for (auto* knob : { &focusK, &spreadK, &growthK })
        knob->setColour (juce::Slider::rotarySliderFillColourId, Colors::accent);
    for (auto* knob : { &bloomK, &motionK, &rateK, &reactK, &mirrorK })
        knob->setColour (juce::Slider::rotarySliderFillColourId, Colors::noteGreen);
    for (auto* knob : { &mixK, &outputK })
        knob->setColour (juce::Slider::rotarySliderFillColourId, Colors::accent2);

    for (int channel = 0; channel < 2; ++channel)
        for (int bed = 0; bed < ParterreProcessor::numBeds; ++bed)
            displayedFrequency[channel][bed] = proc.visualFrequency[channel][bed].load (std::memory_order_relaxed);

    setSize (820, 390);
    startTimerHz (30);
}

ParterreEditor::~ParterreEditor()
{
    stopTimer();
}

void ParterreEditor::timerCallback()
{
    for (int channel = 0; channel < 2; ++channel)
    {
        for (int bed = 0; bed < ParterreProcessor::numBeds; ++bed)
        {
            const float target = proc.visualFrequency[channel][bed].load (std::memory_order_relaxed);
            displayedFrequency[channel][bed] += (target - displayedFrequency[channel][bed]) * 0.38f;
        }
    }

    for (int bed = 0; bed < ParterreProcessor::numBeds; ++bed)
    {
        const float target = proc.visualBedRms[bed].load (std::memory_order_relaxed);
        const float coefficient = target > displayedBedRms[bed] ? 0.45f : 0.12f;
        displayedBedRms[bed] += (target - displayedBedRms[bed]) * coefficient;
    }

    const float envelopeTarget = proc.visualEnvelope.load (std::memory_order_relaxed);
    displayedEnvelope += (envelopeTarget - displayedEnvelope) * 0.3f;
    displayedPhase = proc.visualPhase.load (std::memory_order_relaxed);

    const float outputTarget = proc.visualOutputRms.load (std::memory_order_relaxed);
    const float outputCoefficient = outputTarget > displayedOutputRms ? 0.5f : 0.1f;
    displayedOutputRms += (outputTarget - displayedOutputRms) * outputCoefficient;
    repaint (displayBounds.expanded (2));
}

void ParterreEditor::paint (juce::Graphics& g)
{
    g.fillAll (Colors::panelBg);

    auto display = displayBounds.toFloat();
    g.setColour (Colors::windowBg);
    g.fillRoundedRectangle (display, 5.0f);
    g.setColour (Colors::outline);
    g.drawRoundedRectangle (display, 5.0f, 1.0f);

    auto plot = display.reduced (14.0f, 12.0f);
    const float frequencyMarks[] = { 100.0f, 300.0f, 1000.0f, 3000.0f, 10000.0f };
    const char* frequencyLabels[] = { "100", "300", "1K", "3K", "10K" };
    g.setFont (juce::Font (juce::FontOptions (8.0f)));
    for (int i = 0; i < 5; ++i)
    {
        const float x = frequencyToX (frequencyMarks[i], plot);
        g.setColour (Colors::gridLine.withAlpha (0.45f));
        g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom() - 12.0f);
        g.setColour (Colors::textDim);
        g.drawText (frequencyLabels[i], (int) x - 15, (int) plot.getBottom() - 10,
                    30, 10, juce::Justification::centred, false);
    }

    const float soilY = plot.getBottom() - 14.0f;
    g.setColour (Colors::gridBar.withAlpha (0.7f));
    g.drawHorizontalLine ((int) soilY, plot.getX(), plot.getRight());

    const juce::Colour bedColours[ParterreProcessor::numBeds] = {
        Colors::accent, Colors::noteGreen, Colors::accent2, Colors::warning
    };

    for (int bed = 0; bed < ParterreProcessor::numBeds; ++bed)
    {
        const float leftX = frequencyToX (displayedFrequency[0][bed], plot);
        const float rightX = frequencyToX (displayedFrequency[1][bed], plot);
        const float centreX = 0.5f * (leftX + rightX);
        const float level = juce::jlimit (0.0f, 1.0f, displayedBedRms[bed] * 3.5f);
        const float bedBase = 32.0f + 18.0f * (float) bed;
        const float height = bedBase + 70.0f * level + 18.0f * displayedEnvelope;
        const float topY = soilY - height;
        const float wind = std::sin ((displayedPhase + 0.25f * (float) bed)
                                    * juce::MathConstants<float>::twoPi);

        juce::Path stem;
        stem.startNewSubPath (centreX, soilY);
        stem.cubicTo (centreX + wind * 12.0f, soilY - height * 0.35f,
                      centreX - wind * 8.0f, soilY - height * 0.72f,
                      centreX, topY);
        g.setColour (bedColours[bed].withAlpha (0.45f + 0.45f * level));
        g.strokePath (stem, juce::PathStrokeType (1.4f + level * 1.6f));

        g.setColour (bedColours[bed].withAlpha (0.18f + level * 0.22f));
        g.fillEllipse (leftX - 7.0f - level * 4.0f, topY - 4.0f,
                       14.0f + level * 8.0f, 8.0f + level * 4.0f);
        g.fillEllipse (rightX - 7.0f - level * 4.0f, topY - 4.0f,
                       14.0f + level * 8.0f, 8.0f + level * 4.0f);

        g.setColour (bedColours[bed].withAlpha (0.85f));
        g.drawLine (centreX, topY, leftX, topY, 1.0f);
        g.drawLine (centreX, topY, rightX, topY, 1.0f);
        const float budRadius = 3.0f + level * 3.5f;
        g.fillEllipse (centreX - budRadius, topY - budRadius,
                       budRadius * 2.0f, budRadius * 2.0f);
    }

    auto meter = plot.removeFromRight (5.0f);
    meter.setTop (plot.getY() + 6.0f);
    meter.setBottom (soilY);
    g.setColour (Colors::panelDark);
    g.fillRoundedRectangle (meter, 2.0f);
    const float meterLevel = juce::jlimit (0.0f, 1.0f, displayedOutputRms * 2.5f);
    auto lit = meter;
    lit.setTop (juce::jmap (meterLevel, meter.getBottom(), meter.getY()));
    g.setColour ((meterLevel > 0.9f ? Colors::meterHot : Colors::meterMid).withAlpha (0.9f));
    g.fillRoundedRectangle (lit, 2.0f);
}

void ParterreEditor::resized()
{
    auto bounds = getLocalBounds().reduced (10);
    auto header = bounds.removeFromTop (24);
    title.setBounds (header.removeFromLeft (180));
    subtitle.setBounds (header);
    bounds.removeFromTop (5);

    displayBounds = bounds.removeFromTop (220);
    bounds.removeFromTop (8);

    ValueKnob* knobs[] = { &focusK, &spreadK, &growthK, &bloomK, &motionK,
                           &rateK, &reactK, &mirrorK, &mixK, &outputK };
    const int width = bounds.getWidth() / 10;
    for (int i = 0; i < 10; ++i)
    {
        auto cell = i == 9 ? bounds : bounds.removeFromLeft (width);
        labels[i].setBounds (cell.removeFromBottom (14));
        knobs[i]->setBounds (cell.reduced (7, 2));
    }
}
