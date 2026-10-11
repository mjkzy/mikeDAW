#pragma once
#include <JuceHeader.h>

class Mp3Writer : public juce::AudioFormatWriter
{
public:
    Mp3Writer (const juce::File& destination, double sourceSampleRate,
               int numChannels = 2, int bitrateKbps = 320);
    ~Mp3Writer() override;

    bool write (const int** samplesToWrite, int numSamples) override;

private:
    bool ensureOpen();
    void finish();

    juce::File dest;
    int bitrate = 320;
    int outputSampleRate = 44100;

    struct Impl;
    std::unique_ptr<Impl> impl;

    bool opened = false;
    bool failed = false;
    bool comInitialised = false;
    bool mfStarted = false;
    juce::int64 sampleCounter = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Mp3Writer)
};
