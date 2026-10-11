#pragma once
#include <JuceHeader.h>

namespace SyncDebug
{
    inline juce::File dir()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("mikeDAW");
    }

    inline bool enabled()
    {
        static const bool on = dir().getChildFile ("sync-debug.enabled").existsAsFile();
        return on;
    }

    inline void log (const juce::String& msg)
    {
        if (! enabled())
            return;

        static juce::CriticalSection lock;
        const juce::ScopedLock sl (lock);

        auto line = juce::Time::getCurrentTime().formatted ("%H:%M:%S")
                    + "." + juce::String (juce::Time::getCurrentTime().getMilliseconds()).paddedLeft ('0', 3)
                    + "  " + msg + juce::newLine;
        dir().getChildFile ("sync-debug.log").appendText (line);
    }

    inline juce::uint32 sum (const void* data, size_t size)
    {
        juce::uint32 s = 0;
        auto* p = static_cast<const juce::uint8*> (data);
        for (size_t i = 0; i < size; ++i)
            s += p[i];
        return s;
    }
}

inline void synclog (const juce::String& msg) { SyncDebug::log (msg); }
