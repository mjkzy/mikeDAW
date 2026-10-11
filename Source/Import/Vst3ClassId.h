#pragma once
#include <JuceHeader.h>

namespace FLVst3
{
    juce::MemoryBlock extractClassId (const juce::File& vst3File);
}
