#pragma once
#include <JuceHeader.h>

namespace FLPExport
{
    struct Result
    {
        bool success = false;
        juce::String error;
        juce::String summary;
    };

    using PathResolver = std::function<juce::File (const juce::String&)>;
    Result exportAsZip (const juce::ValueTree& project, const juce::File& zipFile,
                        PathResolver resolve = nullptr);
}
