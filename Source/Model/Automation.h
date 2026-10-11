#pragma once
#include <JuceHeader.h>
#include "ProjectModel.h"

namespace Automation
{
    struct Target
    {
        enum Kind { trackVolume, trackPan, channelVolume, channelPan, invalid };
        Kind kind = invalid;
        int index = 0;
    };

    Target parseTarget (const juce::String& target);

    juce::String targetName (const juce::ValueTree& project, const juce::String& target);

    double shape (double x, int mode, double t);

    double evaluate (const juce::ValueTree& automationClip, double posBeatsInClip);

    float denormalize (Target, double val);
    double normalizeCurrent (const juce::ValueTree& project, Target);
    double normalizedDefault (Target);

    juce::ValueTree createClip (juce::ValueTree project, const juce::String& target);
}
