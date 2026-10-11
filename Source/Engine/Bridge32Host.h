#pragma once
#include <JuceHeader.h>

namespace Bridge32
{
    inline const char* const formatName = "VST2 (32-bit)";

    inline bool isBridged (const juce::PluginDescription& d)
    {
        return d.pluginFormatName == formatName;
    }

    inline bool formatsInterchangeable (const juce::String& a, const juce::String& b)
    {
        if (a == b)
            return true;
        auto isVst2 = [] (const juce::String& f) { return f == "VST" || f == formatName; };
        return isVst2 (a) && isVst2 (b);
    }

    juce::File bridgeExecutable();
    bool isAvailable();

    bool scanPlugin (const juce::File& dll, juce::PluginDescription& desc,
                     juce::String& error);

    std::unique_ptr<juce::AudioPluginInstance> createInstance (
        const juce::PluginDescription& desc,
        double sampleRate, int blockSize, juce::String& error);

    juce::MemoryBlock wrapChunkState (const void* data, size_t size);
}
