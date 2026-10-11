#pragma once
#include <JuceHeader.h>
#include "../Model/ProjectModel.h"

namespace MidiImport
{
    int applyToChannelSeq (const juce::File& file,
                           juce::ValueTree pattern,
                           juce::ValueTree seq);
}
