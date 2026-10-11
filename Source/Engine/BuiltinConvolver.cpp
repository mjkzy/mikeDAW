#include "BuiltinConvolver.h"
#include "../UI/Dialogs.h"
#include <random>

namespace
{
    constexpr int stateMagic = 0x4f434e56;
    constexpr int stateVersion = 2;

    float envelope (float position, float attack, float decay)
    {
        const float rise = attack <= 0.0f ? 1.0f : juce::jmin (1.0f, position / attack);
        return rise * std::exp (-decay * position);
    }

    void normaliseBuffer (juce::AudioBuffer<float>& buffer)
    {
        double maxChannelEnergy = 0.0;
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            const float* data = buffer.getReadPointer (ch);
            double energy = 0.0;
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                energy += (double) data[i] * (double) data[i];
            maxChannelEnergy = juce::jmax (maxChannelEnergy, energy);
        }
        if (maxChannelEnergy > 1.0e-9)
            buffer.applyGain ((float) (1.0 / std::sqrt (maxChannelEnergy)));
    }

    juce::AudioBuffer<float> makeRoom (double sampleRate, double seconds, float decay,
                                       float damping, float width, int seed)
    {
        const int length = juce::jmax (64, (int) std::round (seconds * sampleRate));
        juce::AudioBuffer<float> result (2, length);
        result.clear();

        std::mt19937 generator ((unsigned int) seed);
        std::uniform_real_distribution<float> random (-1.0f, 1.0f);
        std::uniform_int_distribution<int> reflection (1, juce::jmax (1, (int) (sampleRate * 0.12)));
        result.setSample (0, 0, 0.9f);
        result.setSample (1, 0, 0.9f);

        for (int i = 0; i < 34; ++i)
        {
            const int left = reflection (generator);
            const int offset = (int) (width * random (generator) * 0.008f * (float) sampleRate);
            const int right = juce::jlimit (1, length - 1, left + offset);
            const float gain = random (generator) * std::exp (-3.0f * (float) left / (float) length) * 0.55f;
            result.addSample (0, left, gain);
            result.addSample (1, right, gain * (0.75f + 0.25f * random (generator)));
        }

        float lowL = 0.0f, lowR = 0.0f;
        const float colour = juce::jmap (damping, 0.0f, 1.0f, 0.42f, 0.035f);
        for (int i = (int) (sampleRate * 0.015); i < length; ++i)
        {
            const float t = (float) i / (float) length;
            lowL += colour * (random (generator) - lowL);
            lowR += colour * (random (generator) - lowR);
            const float gain = std::exp (-decay * t) * 0.095f;
            result.addSample (0, i, lowL * gain);
            result.addSample (1, i, juce::jmap (width, lowL, lowR) * gain);
        }

        normaliseBuffer (result);
        return result;
    }

    juce::AudioBuffer<float> makeBlur (double sampleRate, bool pink)
    {
        const int length = (int) std::round (sampleRate * 5.01);
        juce::AudioBuffer<float> result (2, length);
        std::mt19937 generator (pink ? 0x50494e4bU : 0x57484954U);
        std::uniform_real_distribution<float> random (-1.0f, 1.0f);

        for (int ch = 0; ch < 2; ++ch)
        {
            auto* data = result.getWritePointer (ch);
            float b0 = 0.0f, b1 = 0.0f, b2 = 0.0f, b3 = 0.0f, b4 = 0.0f, b5 = 0.0f, b6 = 0.0f;
            for (int i = 0; i < length; ++i)
            {
                const float white = random (generator);
                float value = white;
                if (pink)
                {
                    b0 = 0.99886f * b0 + white * 0.0555179f;
                    b1 = 0.99332f * b1 + white * 0.0750759f;
                    b2 = 0.96900f * b2 + white * 0.1538520f;
                    b3 = 0.86650f * b3 + white * 0.3104856f;
                    b4 = 0.55000f * b4 + white * 0.5329522f;
                    b5 = -0.7616f * b5 - white * 0.0168980f;
                    value = (b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362f) * 0.115f;
                    b6 = white * 0.115926f;
                }
                const float position = (float) i / (float) length;
                const float shape = envelope (position, 0.18f, 2.45f)
                                  * std::sin (juce::MathConstants<float>::pi * juce::jmin (1.0f, position * 1.04f));
                data[i] = value * shape;
            }
        }

        normaliseBuffer (result);
        return result;
    }

    juce::AudioBuffer<float> makeLinearPhaseFilter (double sampleRate, float lowHz, float highHz)
    {
        constexpr int taps = 4097;
        juce::AudioBuffer<float> result (2, taps);
        const int centre = taps / 2;
        const float low = juce::jlimit (0.0f, 0.49f, lowHz / (float) sampleRate);
        const float high = juce::jlimit (0.0f, 0.49f, highHz / (float) sampleRate);

        for (int i = 0; i < taps; ++i)
        {
            const int n = i - centre;
            auto sincLowpass = [n] (float cutoff)
            {
                if (cutoff <= 0.0f) return 0.0f;
                if (n == 0) return 2.0f * cutoff;
                return std::sin (juce::MathConstants<float>::twoPi * cutoff * (float) n)
                     / (juce::MathConstants<float>::pi * (float) n);
            };
            const float window = 0.42f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (taps - 1))
                                       + 0.08f * std::cos (2.0f * juce::MathConstants<float>::twoPi * (float) i / (float) (taps - 1));
            const float sample = (sincLowpass (high) - sincLowpass (low)) * window;
            result.setSample (0, i, sample);
            result.setSample (1, i, sample);
        }

        normaliseBuffer (result);
        return result;
    }

    juce::AudioBuffer<float> makeSpring (double sampleRate, float seconds, int seed)
    {
        const int length = (int) (sampleRate * seconds);
        juce::AudioBuffer<float> result (2, length);
        std::mt19937 generator ((unsigned int) seed);
        std::uniform_real_distribution<float> random (-1.0f, 1.0f);
        for (int ch = 0; ch < 2; ++ch)
        {
            float state = 0.0f;
            for (int i = 0; i < length; ++i)
            {
                const float t = (float) i / (float) sampleRate;
                const float chirp = std::sin (juce::MathConstants<float>::twoPi
                                    * (1900.0f * t - 1150.0f * t * t / seconds + 17.0f * ch));
                state = 0.76f * state + 0.24f * random (generator);
                result.setSample (ch, i, (0.7f * chirp + 0.3f * state)
                                              * std::exp (-3.7f * t / seconds));
            }
        }
        normaliseBuffer (result);
        return result;
    }
}

const std::vector<ConvolverProcessor::PresetInfo>& ConvolverProcessor::presets()
{
    static const std::vector<PresetInfo> list {
        { "default", "Default", "" },
        { "blur.pink", "Blur Pink", "Blur" },
        { "blur.white", "Blur White", "Blur" },
        { "chamber.bright", "Bright Chamber", "Chambers" },
        { "chamber.dark", "Dark Chamber", "Chambers" },
        { "corridor.long", "Long Corridor", "Corridors" },
        { "eq.lowpass", "Low-pass 6 kHz", "Equalizer" },
        { "eq.highpass", "High-pass 250 Hz", "Equalizer" },
        { "eq.telephone", "Telephone Band", "Equalizer" },
        { "exterior.courtyard", "Stone Courtyard", "Exteriors" },
        { "fx.reverse", "Reverse Swell", "FX" },
        { "fx.metal", "Metallic Wash", "FX" },
        { "cabinet.clean", "Clean 2x12", "Guitar Cabinet" },
        { "cabinet.dark", "Dark 4x12", "Guitar Cabinet" },
        { "hall.concert", "Concert Hall", "Halls" },
        { "hall.large", "Large Hall", "Halls" },
        { "real.studio", "Studio Live Room", "Real Spaces" },
        { "device.plate", "Vintage Plate", "Reverb Devices" },
        { "room.small", "Small Room", "Small Rooms" },
        { "room.drum", "Drum Room", "Small Rooms" },
        { "spring.short", "Short Spring", "Spring Reverb" },
        { "spring.long", "Long Spring", "Spring Reverb" },
        { "underground.tunnel", "Concrete Tunnel", "Underground" }
    };
    return list;
}

ConvolverProcessor::ConvolverProcessor()
{
    addParameter (dry = new juce::AudioParameterFloat (
        juce::ParameterID { "dry", 1 }, "Dry",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.5f));
    addParameter (inputStereo = new juce::AudioParameterFloat (
        juce::ParameterID { "inputStereo", 1 }, "Input stereo",
        juce::NormalisableRange<float> (-1.0f, 1.0f, 0.001f), 0.0f));
    addParameter (wet = new juce::AudioParameterFloat (
        juce::ParameterID { "wet", 1 }, "Wet",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.5f));
    addParameter (wetStereo = new juce::AudioParameterFloat (
        juce::ParameterID { "wetStereo", 1 }, "Wet stereo",
        juce::NormalisableRange<float> (-1.0f, 1.0f, 0.001f), 0.0f));
    addParameter (delay = new juce::AudioParameterFloat (
        juce::ParameterID { "delay", 1 }, "Delay",
        juce::NormalisableRange<float> (0.0f, 2000.0f, 0.1f, 0.42f), 0.0f));
    addParameter (selfConvolution = new juce::AudioParameterFloat (
        juce::ParameterID { "selfConvolution", 1 }, "Self convolution",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));
    addParameter (stretch = new juce::AudioParameterFloat (
        juce::ParameterID { "stretch", 1 }, "Stretch",
        juce::NormalisableRange<float> (0.25f, 4.0f, 0.001f, 0.45f), 1.0f));
    addParameter (eq = new juce::AudioParameterFloat (
        juce::ParameterID { "eq", 1 }, "EQ",
        juce::NormalisableRange<float> (-1.0f, 1.0f, 0.001f), 0.0f));
    juce::StringArray models;
    for (const auto& preset : presets())
        models.add (preset.name);
    models.add ("Custom IR");
    addParameter (impulseModel = new juce::AudioParameterChoice (
        juce::ParameterID { "impulseModel", 1 }, "Impulse",
        models, 0));
    impulseModel->addListener (this);
    stretch->addListener (this);
    selfConvolution->addListener (this);

    auto initial = makePresetImpulse ("default", 44100.0);
    installBaseImpulse (std::move (initial), 44100.0, "Default");
}

ConvolverProcessor::~ConvolverProcessor()
{
    cancelPendingUpdate();
    impulseModel->removeListener (this);
    stretch->removeListener (this);
    selfConvolution->removeListener (this);
}

void ConvolverProcessor::parameterValueChanged (int parameterIndex, float)
{
    if (parameterIndex == impulseModel->getParameterIndex())
    {
        const int selected = impulseModel->getIndex();
        if (selected < (int) presets().size() && selected != currentPreset.load())
            triggerAsyncUpdate();
    }
    else
    {
        impulseDirty.store (true);
        triggerAsyncUpdate();
    }
}

void ConvolverProcessor::handleAsyncUpdate()
{
    const int selected = impulseModel->getIndex();
    if (selected < (int) presets().size() && selected != currentPreset.load())
        installPresetImpulse (selected);

    if (impulseDirty.exchange (false))
        rebuildCurrentImpulse();
}

void ConvolverProcessor::prepareToPlay (double sampleRate, int blockSize)
{
    sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    preparedBlockSize = juce::jmax (1, blockSize);
    auto base = currentPreset.load() >= 0
                  ? makePresetImpulse (presets()[(size_t) currentPreset.load()].id, sr)
                  : baseImpulse;
    if (currentPreset.load() >= 0)
        baseImpulseSampleRate = sr;
    {
        const juce::ScopedLock lock (impulseLock);
        baseImpulse = base;
    }

    auto processed = transformImpulse (base);
    installProcessedImpulse (std::move (processed), currentPreset.load() >= 0 ? sr : baseImpulseSampleRate);

    juce::dsp::ProcessSpec spec { sr, (juce::uint32) preparedBlockSize, 2 };
    convolution.prepare (spec);
    toneFilter.prepare (spec);
    toneFilter.setResonance (0.707f);
    toneFilter.reset();
    toneActive = false;
    predelayWrite = 0;
    for (auto& channel : predelay)
        channel.assign ((size_t) (sr * 2.05) + (size_t) preparedBlockSize + 8, 0.0f);
    dryBuffer.setSize (2, preparedBlockSize, false, false, true);

    for (auto* smooth : { &drySmooth, &wetSmooth, &inputStereoSmooth, &wetStereoSmooth })
        smooth->reset (sr, 0.02);
    drySmooth.setCurrentAndTargetValue (dry->get());
    wetSmooth.setCurrentAndTargetValue (wet->get());
    inputStereoSmooth.setCurrentAndTargetValue (inputStereo->get());
    wetStereoSmooth.setCurrentAndTargetValue (wetStereo->get());
}

void ConvolverProcessor::reset()
{
    convolution.reset();
    toneFilter.reset();
    toneActive = false;
    for (auto& channel : predelay)
        std::fill (channel.begin(), channel.end(), 0.0f);
    predelayWrite = 0;

    drySmooth.setCurrentAndTargetValue (drySmooth.getTargetValue());
    wetSmooth.setCurrentAndTargetValue (wetSmooth.getTargetValue());
    inputStereoSmooth.setCurrentAndTargetValue (inputStereoSmooth.getTargetValue());
    wetStereoSmooth.setCurrentAndTargetValue (wetStereoSmooth.getTargetValue());
}

void ConvolverProcessor::releaseResources()
{
    convolution.reset();
    toneFilter.reset();
}

void ConvolverProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int channels = juce::jmin (2, buffer.getNumChannels());
    const int samples = buffer.getNumSamples();
    if (channels == 0 || samples == 0)
        return;

    if (dryBuffer.getNumSamples() < samples)
        dryBuffer.setSize (2, samples, false, false, true);
    for (int ch = 0; ch < channels; ++ch)
        dryBuffer.copyFrom (ch, 0, buffer, ch, 0, samples);
    if (channels == 1)
        dryBuffer.copyFrom (1, 0, buffer, 0, 0, samples);

    inputStereoSmooth.setTargetValue (inputStereo->get());
    if (channels > 1)
    {
        auto* left = buffer.getWritePointer (0);
        auto* right = buffer.getWritePointer (1);
        for (int i = 0; i < samples; ++i)
        {
            const float sideGain = std::pow (2.0f, inputStereoSmooth.getNextValue());
            const float mid = 0.5f * (left[i] + right[i]);
            const float side = 0.5f * (left[i] - right[i]) * sideGain;
            left[i] = mid + side;
            right[i] = mid - side;
        }
    }

    juce::dsp::AudioBlock<float> block (buffer);
    auto stereo = block.getSubsetChannelBlock (0, (size_t) channels);
    juce::dsp::ProcessContextReplacing<float> context (stereo);
    convolution.process (context);

    const float tone = eq->get();
    const bool filterWet = std::abs (tone) > 0.015f;
    if (filterWet)
    {
        using Type = juce::dsp::StateVariableTPTFilterType;
        const Type type = tone < 0.0f ? Type::lowpass : Type::highpass;
        if (! toneActive || type != lastToneType)
        {
            toneFilter.reset();
            lastToneType = type;
        }
        toneFilter.setType (type);
        if (tone < 0.0f)
            toneFilter.setCutoffFrequency (juce::jmap (-tone, 0.0f, 1.0f, 20000.0f, 350.0f));
        else
            toneFilter.setCutoffFrequency (juce::jmap (tone, 0.0f, 1.0f, 20.0f, 5000.0f));
    }
    toneActive = filterWet;

    drySmooth.setTargetValue (dry->get());
    wetSmooth.setTargetValue (wet->get());
    wetStereoSmooth.setTargetValue (wetStereo->get());
    const int delaySamples = juce::jlimit (0, predelay.empty() ? 0 : (int) predelay[0].size() - 1,
                                           (int) std::round (delay->get() * 0.001 * sr));
    const int ringSize = predelay.empty() ? 0 : (int) predelay[0].size();

    auto* left = buffer.getWritePointer (0);
    auto* right = buffer.getWritePointer (channels > 1 ? 1 : 0);
    for (int i = 0; i < samples; ++i)
    {
        float wetL = left[i];
        float wetR = channels > 1 ? right[i] : wetL;
        if (filterWet)
        {
            wetL = toneFilter.processSample (0, wetL);
            wetR = toneFilter.processSample (1, wetR);
        }

        if (ringSize > 0)
        {
            const int read = (predelayWrite - delaySamples + ringSize) % ringSize;
            predelay[0][(size_t) predelayWrite] = wetL;
            predelay[1][(size_t) predelayWrite] = wetR;
            wetL = predelay[0][(size_t) read];
            wetR = predelay[1][(size_t) read];
            predelayWrite = (predelayWrite + 1) % ringSize;
        }

        const float width = std::pow (2.0f, wetStereoSmooth.getNextValue());
        const float mid = 0.5f * (wetL + wetR);
        const float side = 0.5f * (wetL - wetR) * width;
        const float dryGain = drySmooth.getNextValue();
        const float wetGain = wetSmooth.getNextValue();
        left[i] = dryBuffer.getSample (0, i) * dryGain + (mid + side) * wetGain;
        if (channels > 1)
            right[i] = dryBuffer.getSample (1, i) * dryGain + (mid - side) * wetGain;
    }
}

double ConvolverProcessor::getTailLengthSeconds() const
{
    int numSamples;
    {
        const juce::ScopedLock lock (impulseLock);
        numSamples = displayImpulse.getNumSamples();
    }
    const double impulseSeconds = numSamples / juce::jmax (1.0, impulseSampleRate.load());
    return impulseSeconds + delay->get() * 0.001;
}

juce::AudioProcessorEditor* ConvolverProcessor::createEditor()
{
    return new ConvolverEditor (*this);
}

int ConvolverProcessor::getNumPrograms() { return (int) presets().size(); }
int ConvolverProcessor::getCurrentProgram() { return currentPreset.load(); }

void ConvolverProcessor::setCurrentProgram (int index)
{
    selectPreset (index);
}

const juce::String ConvolverProcessor::getProgramName (int index)
{
    return juce::isPositiveAndBelow (index, (int) presets().size()) ? presets()[(size_t) index].name : juce::String();
}

void ConvolverProcessor::getStateInformation (juce::MemoryBlock& destination)
{
    juce::MemoryOutputStream out (destination, false);
    out.writeInt (stateMagic);
    out.writeInt (stateVersion);
    out.writeInt (getParameters().size());
    for (auto* parameter : getParameters())
        out.writeFloat (parameter->getValue());
    out.writeInt (currentPreset.load());
    out.writeBool (normalise.load());
    out.writeString (customSourceName);

    if (currentPreset.load() < 0)
    {
        const juce::ScopedLock lock (impulseLock);
        out.writeDouble (baseImpulseSampleRate);
        out.writeInt (baseImpulse.getNumChannels());
        out.writeInt (baseImpulse.getNumSamples());
        for (int ch = 0; ch < baseImpulse.getNumChannels(); ++ch)
            out.write (baseImpulse.getReadPointer (ch), (size_t) baseImpulse.getNumSamples() * sizeof (float));
    }
}

void ConvolverProcessor::setStateInformation (const void* data, int size)
{
    juce::MemoryInputStream in (data, (size_t) size, false);
    if (in.readInt() != stateMagic)
    {
        BuiltinEffectBase::setStateInformation (data, size);
        rebuildCurrentImpulse();
        return;
    }

    const int version = in.readInt();
    if (version < 1 || version > stateVersion)
        return;
    const int parameterCount = in.readInt();
    for (int i = 0; i < parameterCount && i < getParameters().size(); ++i)
        getParameters()[i]->setValueNotifyingHost (in.readFloat());
    for (int i = getParameters().size(); i < parameterCount && in.getNumBytesRemaining() >= 4; ++i)
        in.readFloat();

    const int presetIndex = in.readInt();
    normalise.store (in.readBool());
    customSourceName = in.readString();
    if (presetIndex >= 0)
    {
        installPresetImpulse (presetIndex);
        setParameterValue (*impulseModel, (float) presetIndex, false);
        return;
    }

    if (in.getNumBytesRemaining() < 16)
        return;
    const double sampleRate = in.readDouble();
    const int channels = juce::jlimit (1, 2, in.readInt());
    const int samples = in.readInt();
    const size_t required = (size_t) channels * (size_t) juce::jmax (0, samples) * sizeof (float);
    if (samples <= 0 || samples > 4000000 || in.getNumBytesRemaining() < (juce::int64) required)
        return;

    juce::AudioBuffer<float> impulse (channels, samples);
    for (int ch = 0; ch < channels; ++ch)
        in.read (impulse.getWritePointer (ch), (size_t) samples * sizeof (float));
    currentPreset.store (-1);
    installBaseImpulse (std::move (impulse), sampleRate, customSourceName);
}

void ConvolverProcessor::setParameterValue (juce::RangedAudioParameter& parameter, float value, bool notify)
{
    const float normalisedValue = parameter.convertTo0to1 (value);
    if (notify)
        parameter.setValueNotifyingHost (normalisedValue);
    else
        parameter.setValue (normalisedValue);
}

void ConvolverProcessor::applyPresetParameterValues (const juce::String& id, bool notify)
{
    float dryValue = 0.5f, wetValue = 0.58f, inputWidth = 0.0f, outputWidth = 0.08f;
    float delayValue = 0.0f, selfValue = 0.0f, stretchValue = 1.0f, toneValue = 0.0f;
    if (id.startsWith ("blur."))
    {
        dryValue = 0.08f; wetValue = 0.92f; outputWidth = 0.35f;
        selfValue = id.endsWith ("pink") ? 0.18f : 0.08f;
    }
    else if (id.startsWith ("eq."))
    {
        dryValue = 0.0f; wetValue = 1.0f; outputWidth = 0.0f;
    }
    else if (id.startsWith ("cabinet."))
    {
        dryValue = 0.0f; wetValue = 1.0f; outputWidth = -0.65f;
        toneValue = id.endsWith ("dark") ? -0.28f : -0.08f;
    }
    else if (id == "fx.reverse")
    {
        dryValue = 0.12f; wetValue = 0.9f; outputWidth = 0.25f;
    }
    else if (id == "fx.metal")
    {
        dryValue = 0.2f; wetValue = 0.86f; selfValue = 0.32f; outputWidth = 0.5f;
    }
    setParameterValue (*dry, dryValue, notify);
    setParameterValue (*inputStereo, inputWidth, notify);
    setParameterValue (*wet, wetValue, notify);
    setParameterValue (*wetStereo, outputWidth, notify);
    setParameterValue (*delay, delayValue, notify);
    setParameterValue (*selfConvolution, selfValue, notify);
    setParameterValue (*stretch, stretchValue, notify);
    setParameterValue (*eq, toneValue, notify);
}

void ConvolverProcessor::installPresetImpulse (int index)
{
    index = juce::jlimit (0, (int) presets().size() - 1, index);
    currentPreset.store (index);
    const auto& info = presets()[(size_t) index];
    installBaseImpulse (makePresetImpulse (info.id, sr), sr, info.name);
}

void ConvolverProcessor::selectPreset (int index, bool notifyParameters)
{
    index = juce::jlimit (0, (int) presets().size() - 1, index);
    currentPreset.store (index);
    setParameterValue (*impulseModel, (float) index, notifyParameters);
    applyPresetParameterValues (presets()[(size_t) index].id, notifyParameters);
    installPresetImpulse (index);
}

void ConvolverProcessor::selectPresetById (const juce::String& id, bool notifyParameters)
{
    for (int i = 0; i < (int) presets().size(); ++i)
        if (presets()[(size_t) i].id == id)
        {
            selectPreset (i, notifyParameters);
            return;
        }
}

juce::String ConvolverProcessor::currentPresetName() const
{
    const int index = currentPreset.load();
    return juce::isPositiveAndBelow (index, (int) presets().size())
             ? presets()[(size_t) index].name : customSourceName;
}

juce::AudioBuffer<float> ConvolverProcessor::makePresetImpulse (const juce::String& id, double sampleRate) const
{
    if (id == "blur.pink")  return makeBlur (sampleRate, true);
    if (id == "blur.white") return makeBlur (sampleRate, false);
    if (id == "eq.lowpass") return makeLinearPhaseFilter (sampleRate, 0.0f, 6000.0f);
    if (id == "eq.highpass") return makeLinearPhaseFilter (sampleRate, 250.0f, (float) sampleRate * 0.49f);
    if (id == "eq.telephone") return makeLinearPhaseFilter (sampleRate, 320.0f, 3400.0f);
    if (id == "spring.short") return makeSpring (sampleRate, 1.8f, 771);
    if (id == "spring.long") return makeSpring (sampleRate, 3.8f, 991);

    if (id.startsWith ("cabinet."))
    {
        auto cabinet = makeRoom (sampleRate, id.endsWith ("dark") ? 0.16 : 0.11,
                                 7.0f, id.endsWith ("dark") ? 0.92f : 0.68f, 0.12f, 4301);
        return cabinet;
    }

    if (id == "fx.reverse")
    {
        auto impulse = makeRoom (sampleRate, 3.2, 3.8f, 0.42f, 0.85f, 9871);
        for (int ch = 0; ch < impulse.getNumChannels(); ++ch)
            std::reverse (impulse.getWritePointer (ch), impulse.getWritePointer (ch) + impulse.getNumSamples());
        return impulse;
    }
    if (id == "fx.metal") return makeSpring (sampleRate, 5.2f, 8821);
    if (id == "chamber.bright") return makeRoom (sampleRate, 1.65, 5.4f, 0.22f, 0.62f, 1101);
    if (id == "chamber.dark") return makeRoom (sampleRate, 2.35, 5.0f, 0.78f, 0.58f, 1102);
    if (id == "corridor.long") return makeRoom (sampleRate, 4.1, 4.5f, 0.48f, 0.38f, 2301);
    if (id == "exterior.courtyard") return makeRoom (sampleRate, 2.8, 5.8f, 0.25f, 0.95f, 3301);
    if (id == "hall.concert") return makeRoom (sampleRate, 4.7, 4.1f, 0.46f, 0.9f, 5101);
    if (id == "hall.large") return makeRoom (sampleRate, 6.2, 3.7f, 0.59f, 1.0f, 5102);
    if (id == "real.studio") return makeRoom (sampleRate, 1.2, 6.4f, 0.35f, 0.72f, 6101);
    if (id == "device.plate") return makeRoom (sampleRate, 4.0, 4.7f, 0.31f, 1.0f, 7101);
    if (id == "room.small") return makeRoom (sampleRate, 0.72, 7.2f, 0.44f, 0.55f, 8101);
    if (id == "room.drum") return makeRoom (sampleRate, 1.05, 6.1f, 0.18f, 0.82f, 8102);
    if (id == "underground.tunnel") return makeRoom (sampleRate, 5.8, 4.0f, 0.72f, 0.28f, 9101);
    return makeRoom (sampleRate, 3.423, 4.8f, 0.5f, 0.7f, 101);
}

juce::AudioBuffer<float> ConvolverProcessor::transformImpulse (const juce::AudioBuffer<float>& source) const
{
    const float stretchValue = stretch->get();
    const int outputLength = juce::jlimit (32, 4000000,
        (int) std::round ((double) source.getNumSamples() * stretchValue));
    juce::AudioBuffer<float> result (juce::jlimit (1, 2, source.getNumChannels()), outputLength);

    for (int ch = 0; ch < result.getNumChannels(); ++ch)
    {
        const auto* input = source.getReadPointer (juce::jmin (ch, source.getNumChannels() - 1));
        auto* output = result.getWritePointer (ch);
        for (int i = 0; i < outputLength; ++i)
        {
            const float sourcePosition = (float) i / juce::jmax (0.0001f, stretchValue);
            const int index = juce::jlimit (0, source.getNumSamples() - 1, (int) sourcePosition);
            const int next = juce::jmin (source.getNumSamples() - 1, index + 1);
            const float fraction = sourcePosition - (float) index;
            output[i] = juce::jmap (fraction, input[index], input[next]);
        }

        const float self = selfConvolution->get();
        if (self > 0.001f)
        {
            float diffuseA = 0.0f, diffuseB = 0.0f;
            for (int i = 0; i < outputLength; ++i)
            {
                diffuseA = output[i] + diffuseA * (0.72f + self * 0.18f);
                diffuseB = diffuseA - diffuseB * (0.38f + self * 0.25f);
                output[i] = juce::jmap (self * 0.72f, output[i], diffuseB * (1.0f - self * 0.55f));
            }
        }
    }

    if (normalise.load())
        normaliseBuffer (result);
    return result;
}

void ConvolverProcessor::installBaseImpulse (juce::AudioBuffer<float>&& impulse, double sampleRate,
                                             juce::String sourceName)
{
    baseImpulseSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    customSourceName = std::move (sourceName);
    {
        const juce::ScopedLock lock (impulseLock);
        baseImpulse = impulse;
    }
    auto processed = transformImpulse (impulse);
    installProcessedImpulse (std::move (processed), baseImpulseSampleRate);
}

void ConvolverProcessor::installProcessedImpulse (juce::AudioBuffer<float>&& impulse, double sampleRate)
{
    const bool stereo = impulse.getNumChannels() > 1;
    {
        const juce::ScopedLock lock (impulseLock);
        displayImpulse = impulse;
    }
    impulseSampleRate.store (sampleRate);
    convolution.loadImpulseResponse (std::move (impulse), sampleRate,
        stereo ? juce::dsp::Convolution::Stereo::yes : juce::dsp::Convolution::Stereo::no,
        juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::no);
}

void ConvolverProcessor::rebuildCurrentImpulse()
{
    juce::AudioBuffer<float> base;
    {
        const juce::ScopedLock lock (impulseLock);
        base = baseImpulse;
    }
    if (base.getNumSamples() > 0)
        installProcessedImpulse (transformImpulse (base), baseImpulseSampleRate);
}

void ConvolverProcessor::setNormalise (bool shouldNormalise)
{
    normalise.store (shouldNormalise);
    rebuildCurrentImpulse();
}

bool ConvolverProcessor::loadImpulseFile (const juce::File& file, juce::String& error)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    auto reader = std::unique_ptr<juce::AudioFormatReader> (formats.createReaderFor (file));
    if (reader == nullptr)
    {
        error = "The selected file is not a supported audio file.";
        return false;
    }

    const juce::int64 maxSamples = (juce::int64) std::round (reader->sampleRate * 30.0);
    const int samples = (int) juce::jmin (reader->lengthInSamples, maxSamples);
    juce::AudioBuffer<float> impulse (juce::jlimit (1, 2, (int) reader->numChannels), samples);
    if (! reader->read (&impulse, 0, samples, 0, true, true))
    {
        error = "The impulse response could not be read.";
        return false;
    }

    currentPreset.store (-1);
    setParameterValue (*impulseModel, (float) presets().size(), true);
    installBaseImpulse (std::move (impulse), reader->sampleRate, file.getFileNameWithoutExtension());
    return true;
}

bool ConvolverProcessor::saveImpulseFile (const juce::File& file, juce::String& error) const
{
    auto impulse = getImpulseForDisplay();
    if (impulse.getNumSamples() == 0)
    {
        error = "There is no impulse response to save.";
        return false;
    }

    auto stream = file.createOutputStream();
    if (stream == nullptr)
    {
        error = "The destination file could not be created.";
        return false;
    }
    juce::WavAudioFormat format;
    auto writer = std::unique_ptr<juce::AudioFormatWriter> (format.createWriterFor (
        stream.release(), impulseSampleRate.load(), (unsigned int) impulse.getNumChannels(), 24, {}, 0));
    if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (impulse, 0, impulse.getNumSamples()))
    {
        error = "The impulse response could not be written.";
        return false;
    }
    return true;
}

juce::AudioBuffer<float> ConvolverProcessor::getImpulseForDisplay() const
{
    const juce::ScopedLock lock (impulseLock);
    return displayImpulse;
}

ConvolverEditor::ImpulseView::ImpulseView (ConvolverProcessor& p, bool overview)
    : proc (p), isOverview (overview)
{
    setMouseCursor (overview ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::DraggingHandCursor);
}

void ConvolverEditor::ImpulseView::paint (juce::Graphics& g)
{
    g.fillAll (Colors::panelDark);
    auto bounds = getLocalBounds();
    g.setColour (Colors::gridLine.withAlpha (0.7f));
    const int divisions = isOverview ? 8 : 16;
    for (int i = 1; i < divisions; ++i)
        g.drawVerticalLine (bounds.getX() + bounds.getWidth() * i / divisions,
                            (float) bounds.getY(), (float) bounds.getBottom());
    g.drawHorizontalLine (bounds.getCentreY(), (float) bounds.getX(), (float) bounds.getRight());

    auto impulse = proc.getImpulseForDisplay();
    if (impulse.getNumSamples() == 0)
        return;
    float peak = 0.0f;
    for (int ch = 0; ch < impulse.getNumChannels(); ++ch)
        peak = juce::jmax (peak, impulse.getMagnitude (ch, 0, impulse.getNumSamples()));
    const float invPeak = peak > 1.0e-6f ? 1.0f / peak : 1.0f;
    const int first = juce::jlimit (0, impulse.getNumSamples() - 1,
                                    (int) (viewStart * (float) impulse.getNumSamples()));
    const int count = juce::jlimit (1, impulse.getNumSamples() - first,
                                    (int) (viewLength * (float) impulse.getNumSamples()));
    const float centre = (float) bounds.getCentreY();
    const float scale = bounds.getHeight() * (isOverview ? 0.42f : 0.46f) * invPeak;
    juce::Path positive, negative;
    for (int x = 0; x < bounds.getWidth(); ++x)
    {
        const int from = first + count * x / juce::jmax (1, bounds.getWidth());
        const int to = juce::jmax (from + 1, first + count * (x + 1) / juce::jmax (1, bounds.getWidth()));
        float minimum = 1.0f, maximum = -1.0f;
        for (int ch = 0; ch < impulse.getNumChannels(); ++ch)
        {
            auto range = impulse.findMinMax (ch, from, juce::jmin (to, impulse.getNumSamples()) - from);
            minimum = juce::jmin (minimum, range.getStart());
            maximum = juce::jmax (maximum, range.getEnd());
        }
        const float px = (float) bounds.getX() + (float) x;
        if (x == 0)
        {
            positive.startNewSubPath (px, centre - maximum * scale);
            negative.startNewSubPath (px, centre - minimum * scale);
        }
        else
        {
            positive.lineTo (px, centre - maximum * scale);
            negative.lineTo (px, centre - minimum * scale);
        }
    }
    juce::Path shape (positive);
    shape.lineTo ((float) bounds.getRight(), negative.getCurrentPosition().y);
    shape.addPath (negative, juce::AffineTransform());
    g.setColour (Colors::noteGreen.withAlpha (isOverview ? 0.62f : 0.82f));
    g.strokePath (positive, juce::PathStrokeType (1.1f));
    g.strokePath (negative, juce::PathStrokeType (1.1f));

    if (! isOverview)
    {
        const float tone = proc.eq->get();
        juce::Path curve;
        for (int x = 0; x < bounds.getWidth(); ++x)
        {
            const float u = (float) x / juce::jmax (1.0f, (float) bounds.getWidth() - 1.0f);
            const float base = 0.5f;
            const float y = base - tone * (u - 0.5f) * 0.48f;
            const float px = (float) bounds.getX() + (float) x;
            const float py = (float) bounds.getY() + y * (float) bounds.getHeight();
            if (x == 0) curve.startNewSubPath (px, py); else curve.lineTo (px, py);
        }
        g.setColour (Colors::accent2.withAlpha (0.9f));
        g.strokePath (curve, juce::PathStrokeType (1.4f));
    }
    g.setColour (Colors::outline);
    g.drawRect (bounds, 1);
}

void ConvolverEditor::ImpulseView::mouseWheelMove (const juce::MouseEvent& event,
                                                    const juce::MouseWheelDetails& wheel)
{
    if (isOverview)
        return;
    const float centre = juce::jlimit (0.0f, 1.0f, (float) event.x / juce::jmax (1.0f, (float) getWidth()));
    setZoom (viewLength * std::pow (1.8f, -wheel.deltaY), centre);
}

void ConvolverEditor::ImpulseView::mouseDown (const juce::MouseEvent& event)
{
    dragStartX = event.x;
    dragStartView = viewStart;
    if (isOverview)
        setZoom (viewLength, (float) event.x / juce::jmax (1.0f, (float) getWidth()));
}

void ConvolverEditor::ImpulseView::mouseDrag (const juce::MouseEvent& event)
{
    if (isOverview || viewLength >= 0.999f)
        return;
    const float delta = (float) (dragStartX - event.x) / juce::jmax (1.0f, (float) getWidth()) * viewLength;
    viewStart = juce::jlimit (0.0f, 1.0f - viewLength, dragStartView + delta);
    repaint();
}

void ConvolverEditor::ImpulseView::setZoom (float length, float centre)
{
    length = juce::jlimit (0.01f, 1.0f, length);
    const float absoluteCentre = viewStart + centre * viewLength;
    viewLength = length;
    viewStart = juce::jlimit (0.0f, 1.0f - viewLength, absoluteCentre - centre * viewLength);
    repaint();
}

ConvolverEditor::ConvolverEditor (ConvolverProcessor& p)
    : juce::AudioProcessorEditor (p), proc (p)
{
    auto setupKnob = [this] (ValueKnob& knob, juce::Label& label, const juce::String& text)
    {
        knob.addListener (this);
        knob.setMouseDragSensitivity (180);
        label.setText (text, juce::dontSendNotification);
        label.setFont (juce::Font (juce::FontOptions (13.0f)));
        label.setJustificationType (juce::Justification::centred);
        label.setColour (juce::Label::textColourId, Colors::text);
        addAndMakeVisible (knob);
        addAndMakeVisible (label);
    };
    setupKnob (dryKnob, dryLabel, "dry");
    setupKnob (inputKnob, inputLabel, "in");
    setupKnob (wetKnob, wetLabel, "wet");
    setupKnob (wetStereoKnob, wetStereoLabel, "wet");
    setupKnob (delayKnob, delayLabel, "delay");
    setupKnob (selfKnob, selfLabel, "self-conv");
    setupKnob (stretchKnob, stretchLabel, "stretch");
    setupKnob (eqKnob, eqLabel, "eq");
    dryKnob.setDoubleClickReturnValue (true, 0.5);
    inputKnob.setDoubleClickReturnValue (true, 0.0);
    wetKnob.setDoubleClickReturnValue (true, 0.5);
    wetStereoKnob.setDoubleClickReturnValue (true, 0.0);
    delayKnob.setDoubleClickReturnValue (true, 0.0);
    selfKnob.setDoubleClickReturnValue (true, 0.0);
    stretchKnob.setDoubleClickReturnValue (true, 1.0);
    eqKnob.setDoubleClickReturnValue (true, 0.0);

    for (auto* button : { &presetButton, &previousButton, &nextButton, &loadButton,
                          &saveButton, &reverseButton })
    {
        styleSmallButton (*button);
        addAndMakeVisible (*button);
    }
    normaliseButton.setToggleState (proc.isNormalising(), juce::dontSendNotification);
    normaliseButton.setColour (juce::ToggleButton::textColourId, Colors::textDim);
    normaliseButton.setColour (juce::ToggleButton::tickColourId, Colors::accent2);
    addAndMakeVisible (normaliseButton);

    presetButton.onClick = [this] { showPresetMenu(); };
    previousButton.onClick = [this] { movePreset (-1); };
    nextButton.onClick = [this] { movePreset (1); };
    loadButton.onClick = [this] { chooseImpulse(); };
    saveButton.onClick = [this] { saveImpulse(); };
    reverseButton.onClick = [this] { proc.selectPresetById ("fx.reverse"); updateLabels(); };
    normaliseButton.onClick = [this] { proc.setNormalise (normaliseButton.getToggleState()); };

    addAndMakeVisible (waveform);
    addAndMakeVisible (overview);
    updateLabels();
    setSize (720, 350);
    startTimerHz (15);
}

ConvolverEditor::~ConvolverEditor()
{
    stopTimer();
    for (auto* knob : { &dryKnob, &inputKnob, &wetKnob, &wetStereoKnob,
                        &delayKnob, &selfKnob, &stretchKnob, &eqKnob })
        knob->removeListener (this);
}

void ConvolverEditor::styleSmallButton (juce::Button& button)
{
    button.setColour (juce::TextButton::buttonColourId, Colors::raised);
    button.setColour (juce::TextButton::textColourOffId, Colors::text);
    button.setColour (juce::TextButton::buttonOnColourId, Colors::accent.withAlpha (0.5f));
}

void ConvolverEditor::paint (juce::Graphics& g)
{
    g.fillAll (Colors::panelBg);
    g.setGradientFill (juce::ColourGradient (Colors::header.brighter (0.08f), 0.0f, 0.0f,
                                             Colors::panelDark, 0.0f, 110.0f, false));
    g.fillRect (0, 0, getWidth(), 110);
    g.setColour (Colors::outline);
    g.drawHorizontalLine (109, 0.0f, (float) getWidth());

    g.setFont (juce::Font (juce::FontOptions (9.0f)));
    g.setColour (Colors::textDim);
    const auto impulse = proc.getImpulseForDisplay();
    const double seconds = impulse.getNumSamples() / juce::jmax (1.0, proc.getImpulseSampleRate());
    g.drawText ("SAMPLERATE", 4, 145, 70, 10, juce::Justification::left);
    g.drawText (juce::String ((int) proc.getImpulseSampleRate()) + "Hz", 4, 155, 70, 12, juce::Justification::left);
    g.drawText ("FORMAT", 78, 145, 42, 10, juce::Justification::left);
    g.drawText (juce::String (impulse.getNumChannels() > 1 ? "32  ST" : "32  MO"), 78, 155, 55, 12, juce::Justification::left);
    g.drawText ("TITLE", 138, 145, 40, 10, juce::Justification::left);
    g.setColour (Colors::text);
    g.drawText (proc.currentPresetName(), 138, 155, 260, 12, juce::Justification::left);
    g.setColour (Colors::textDim);
    g.drawText ("LENGTH / SEL", getWidth() - 95, 145, 90, 10, juce::Justification::right);
    g.drawText (juce::String (seconds, 3) + " s", getWidth() - 95, 155, 90, 12, juce::Justification::right);
}

void ConvolverEditor::resized()
{
    const int knobWidth = 58;
    const int startX = 10;
    for (int i = 0; i < 8; ++i)
    {
        auto area = juce::Rectangle<int> (startX + i * knobWidth, 8, knobWidth, 88);
        juce::Slider* knob = i == 0 ? &dryKnob : i == 1 ? &inputKnob : i == 2 ? &wetKnob
                            : i == 3 ? &wetStereoKnob : i == 4 ? &delayKnob : i == 5 ? &selfKnob
                            : i == 6 ? &stretchKnob : &eqKnob;
        juce::Label* label = i == 0 ? &dryLabel : i == 1 ? &inputLabel : i == 2 ? &wetLabel
                            : i == 3 ? &wetStereoLabel : i == 4 ? &delayLabel : i == 5 ? &selfLabel
                            : i == 6 ? &stretchLabel : &eqLabel;
        label->setBounds (area.removeFromBottom (20));
        knob->setBounds (area.reduced (6, 2));
    }

    previousButton.setBounds (getWidth() - 118, 7, 22, 22);
    nextButton.setBounds (getWidth() - 94, 7, 22, 22);
    presetButton.setBounds (getWidth() - 205, 7, 84, 22);
    normaliseButton.setBounds (getWidth() - 118, 31, 110, 19);

    loadButton.setBounds (8, 116, 54, 24);
    saveButton.setBounds (65, 116, 54, 24);
    reverseButton.setBounds (122, 116, 48, 24);
    waveform.setBounds (4, 170, getWidth() - 8, getHeight() - 212);
    overview.setBounds (4, getHeight() - 38, getWidth() - 8, 32);
}

void ConvolverEditor::timerCallback()
{
    if (lastPreset != proc.getCurrentProgram())
        updateLabels();
    waveform.repaint();
    overview.repaint();
}

void ConvolverEditor::sliderValueChanged (juce::Slider*)
{
    waveform.repaint();
}

void ConvolverEditor::showPresetMenu()
{
    juce::PopupMenu menu;
    const auto& list = ConvolverProcessor::presets();
    if (! list.empty())
        menu.addItem (1, list[0].name, true, proc.getCurrentProgram() == 0);
    juce::StringArray categories;
    for (const auto& preset : list)
        if (preset.category.isNotEmpty() && ! categories.contains (preset.category))
            categories.add (preset.category);
    menu.addSeparator();
    for (const auto& category : categories)
    {
        juce::PopupMenu submenu;
        for (int i = 0; i < (int) list.size(); ++i)
            if (list[(size_t) i].category == category)
                submenu.addItem (i + 1, list[(size_t) i].name, true, proc.getCurrentProgram() == i);
        menu.addSubMenu (category, submenu);
    }
    menu.addSeparator();
    menu.addItem (10001, "Load impulse response...");

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&presetButton),
        [safe = juce::Component::SafePointer<ConvolverEditor> (this)] (int result)
        {
            if (safe == nullptr || result == 0)
                return;
            if (result == 10001)
                safe->chooseImpulse();
            else
            {
                safe->proc.selectPreset (result - 1);
                safe->updateLabels();
            }
        });
}

void ConvolverEditor::chooseImpulse()
{
    fileChooser = std::make_unique<juce::FileChooser> ("Load impulse response", juce::File(),
                                                        "*.wav;*.aiff;*.aif;*.flac;*.ogg");
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                              | juce::FileBrowserComponent::canSelectFiles,
        [safe = juce::Component::SafePointer<ConvolverEditor> (this)] (const juce::FileChooser& chooser)
        {
            if (safe == nullptr)
                return;
            const auto file = chooser.getResult();
            if (file.existsAsFile())
            {
                juce::String error;
                if (! safe->proc.loadImpulseFile (file, error))
                    Dialogs::showInfoAsync (juce::MessageBoxIconType::WarningIcon, "Impulse response", error);
                safe->updateLabels();
            }
            safe->fileChooser.reset();
        });
}

void ConvolverEditor::saveImpulse()
{
    fileChooser = std::make_unique<juce::FileChooser> ("Save impulse response",
        juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
            .getChildFile (proc.currentPresetName() + ".wav"), "*.wav");
    fileChooser->launchAsync (juce::FileBrowserComponent::saveMode
                              | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
        [safe = juce::Component::SafePointer<ConvolverEditor> (this)] (const juce::FileChooser& chooser)
        {
            if (safe == nullptr)
                return;
            auto file = chooser.getResult();
            if (file.getFileExtension().isEmpty())
                file = file.withFileExtension ("wav");
            if (file != juce::File())
            {
                juce::String error;
                if (! safe->proc.saveImpulseFile (file, error))
                    Dialogs::showInfoAsync (juce::MessageBoxIconType::WarningIcon, "Impulse response", error);
            }
            safe->fileChooser.reset();
        });
}

void ConvolverEditor::movePreset (int delta)
{
    const int count = proc.getNumPrograms();
    int index = proc.getCurrentProgram();
    if (index < 0) index = 0;
    proc.selectPreset ((index + delta + count) % count);
    updateLabels();
}

void ConvolverEditor::updateLabels()
{
    lastPreset = proc.getCurrentProgram();
    presetButton.setButtonText (proc.currentPresetName());
    normaliseButton.setToggleState (proc.isNormalising(), juce::dontSendNotification);
    repaint();
}
