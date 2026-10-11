#pragma once
#include <JuceHeader.h>
#include "BuiltinEffects.h"

class ConvolverProcessor : public BuiltinEffectBase,
                           private juce::AudioProcessorParameter::Listener,
                           private juce::AsyncUpdater
{
public:
    struct PresetInfo
    {
        juce::String id;
        juce::String name;
        juce::String category;
    };

    ConvolverProcessor();
    ~ConvolverProcessor() override;

    juce::String effectName() const override { return "Convolver"; }
    void prepareToPlay (double sampleRate, int blockSize) override;
    void reset() override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    double getTailLengthSeconds() const override;
    juce::AudioProcessorEditor* createEditor() override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int) override;
    const juce::String getProgramName (int) override;

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    static const std::vector<PresetInfo>& presets();
    void selectPreset (int index, bool notifyParameters = true);
    void selectPresetById (const juce::String&, bool notifyParameters = true);
    juce::String currentPresetName() const;
    bool loadImpulseFile (const juce::File&, juce::String& error);
    bool saveImpulseFile (const juce::File&, juce::String& error) const;
    void rebuildCurrentImpulse();
    void setNormalise (bool);
    bool isNormalising() const noexcept { return normalise.load(); }
    juce::AudioBuffer<float> getImpulseForDisplay() const;
    double getImpulseSampleRate() const noexcept { return impulseSampleRate.load(); }

    juce::AudioParameterFloat* dry = nullptr;
    juce::AudioParameterFloat* inputStereo = nullptr;
    juce::AudioParameterFloat* wet = nullptr;
    juce::AudioParameterFloat* wetStereo = nullptr;
    juce::AudioParameterFloat* delay = nullptr;
    juce::AudioParameterFloat* selfConvolution = nullptr;
    juce::AudioParameterFloat* stretch = nullptr;
    juce::AudioParameterFloat* eq = nullptr;
    juce::AudioParameterChoice* impulseModel = nullptr;

private:
    juce::AudioBuffer<float> makePresetImpulse (const juce::String&, double) const;
    juce::AudioBuffer<float> transformImpulse (const juce::AudioBuffer<float>&) const;
    void installBaseImpulse (juce::AudioBuffer<float>&&, double, juce::String sourceName);
    void installProcessedImpulse (juce::AudioBuffer<float>&&, double);
    void installPresetImpulse (int index);
    void applyPresetParameterValues (const juce::String&, bool);
    static void setParameterValue (juce::RangedAudioParameter&, float, bool);
    void parameterValueChanged (int, float) override;
    void parameterGestureChanged (int, bool) override {}
    void handleAsyncUpdate() override;

    mutable juce::CriticalSection impulseLock;
    juce::AudioBuffer<float> baseImpulse;
    juce::AudioBuffer<float> displayImpulse;
    double baseImpulseSampleRate = 44100.0;
    juce::String customSourceName;

    juce::dsp::Convolution convolution { juce::dsp::Convolution::NonUniform { 256 } };
    juce::dsp::StateVariableTPTFilter<float> toneFilter;
    juce::dsp::StateVariableTPTFilterType lastToneType = juce::dsp::StateVariableTPTFilterType::lowpass;
    bool toneActive = false;
    juce::AudioBuffer<float> dryBuffer;
    std::array<std::vector<float>, 2> predelay;
    int predelayWrite = 0;
    int preparedBlockSize = 0;
    double sr = 44100.0;
    std::atomic<double> impulseSampleRate { 44100.0 };
    std::atomic<int> currentPreset { 0 };
    std::atomic<bool> normalise { true };
    std::atomic<bool> impulseDirty { false };
    juce::SmoothedValue<float> drySmooth, wetSmooth, inputStereoSmooth, wetStereoSmooth;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ConvolverProcessor)
};

class ConvolverEditor : public juce::AudioProcessorEditor,
                        private juce::Timer,
                        private juce::Slider::Listener
{
public:
    explicit ConvolverEditor (ConvolverProcessor&);
    ~ConvolverEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class ImpulseView : public juce::Component
    {
    public:
        ImpulseView (ConvolverProcessor&, bool overview);
        void paint (juce::Graphics&) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void setZoom (float, float centre = 0.5f);
        float getViewStart() const noexcept { return viewStart; }
        float getViewLength() const noexcept { return viewLength; }

    private:
        ConvolverProcessor& proc;
        bool isOverview = false;
        float viewStart = 0.0f;
        float viewLength = 1.0f;
        float dragStartView = 0.0f;
        int dragStartX = 0;
    };

    void timerCallback() override;
    void sliderValueChanged (juce::Slider*) override;
    void showPresetMenu();
    void chooseImpulse();
    void saveImpulse();
    void movePreset (int delta);
    void updateLabels();
    void styleSmallButton (juce::Button&);

    ConvolverProcessor& proc;
    ImpulseView waveform { proc, false };
    ImpulseView overview { proc, true };

    ValueKnob dryKnob, inputKnob, wetKnob, wetStereoKnob,
              delayKnob, selfKnob, stretchKnob, eqKnob;
    juce::Label dryLabel, inputLabel, wetLabel, wetStereoLabel,
                delayLabel, selfLabel, stretchLabel, eqLabel;

    juce::TextButton presetButton, previousButton { "<" }, nextButton { ">" };
    juce::TextButton loadButton { "LOAD" }, saveButton { "SAVE" }, reverseButton { "REV" };
    juce::ToggleButton normaliseButton { "NORMALIZE" };

    juce::SliderParameterAttachment dryAttachment { *proc.dry, dryKnob };
    juce::SliderParameterAttachment inputAttachment { *proc.inputStereo, inputKnob };
    juce::SliderParameterAttachment wetAttachment { *proc.wet, wetKnob };
    juce::SliderParameterAttachment wetStereoAttachment { *proc.wetStereo, wetStereoKnob };
    juce::SliderParameterAttachment delayAttachment { *proc.delay, delayKnob };
    juce::SliderParameterAttachment selfAttachment { *proc.selfConvolution, selfKnob };
    juce::SliderParameterAttachment stretchAttachment { *proc.stretch, stretchKnob };
    juce::SliderParameterAttachment eqAttachment { *proc.eq, eqKnob };

    std::unique_ptr<juce::FileChooser> fileChooser;
    int lastPreset = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ConvolverEditor)
};
