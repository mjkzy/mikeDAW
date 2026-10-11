#pragma once
#include <JuceHeader.h>

namespace LoadProgress
{
    void enter (const juce::String& pluginName);
    void leave();

    void noteQueued();
    void noteFinished();

    void shutdown();

    juce::String stuckPlugin (double minSeconds);
}
