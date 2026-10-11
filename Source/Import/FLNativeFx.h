#pragma once
#include <JuceHeader.h>
#include <vector>

namespace FLNativeFx
{
    struct Imported
    {
        juce::String type;
        juce::String stateB64;
    };

    struct Exported
    {
        juce::String internalName;
        std::vector<uint8_t> state;
    };

    bool canImport (const juce::String& flInternalName);
    bool importSlot (const juce::String& flInternalName, const std::vector<uint8_t>& flState, Imported& out);

    bool canExport (const juce::String& builtinType);
    bool exportSlot (const juce::String& builtinType, const juce::String& stateB64, Exported& out);
}
