#pragma once
#include <JuceHeader.h>

class PluginHost;

namespace FLPImport
{
    struct Result
    {
        juce::ValueTree project;
        juce::String error;
        juce::String summary;
        juce::StringArray warnings;

        bool ok() const { return project.isValid(); }
    };

    Result importFile (const juce::File& flpFile, PluginHost& host);
}
