#pragma once
#include <JuceHeader.h>

namespace Leaderboard
{
    inline juce::File dbFile (juce::PropertiesFile& settings)
    {
        return settings.getFile().getParentDirectory().getChildFile ("leaderboard.xml");
    }

    inline juce::XmlElement loadDb (juce::PropertiesFile& settings)
    {
        if (auto xml = juce::parseXML (dbFile (settings)))
            if (xml->hasTagName ("Leaderboard"))
                return *xml;
        return juce::XmlElement ("Leaderboard");
    }

    inline void recordSave (juce::PropertiesFile& settings, const juce::File& file,
                            juce::int64 secondsWorked, juce::int64 createdEpoch)
    {
        if (file == juce::File() || file.getFullPathName().containsIgnoreCase ("autosave"))
            return;

        auto db = loadDb (settings);
        auto path = file.getFullPathName();

        juce::XmlElement* entry = nullptr;
        for (auto* e : db.getChildWithTagNameIterator ("entry"))
            if (e->getStringAttribute ("path") == path)
            {
                entry = e;
                break;
            }
        if (entry == nullptr)
            entry = db.createNewChildElement ("entry");

        entry->setAttribute ("path", path);
        entry->setAttribute ("filename", file.getFileName());
        entry->setAttribute ("minutesWasted", (int) (secondsWorked / 60));
        entry->setAttribute ("secondsWasted", juce::String (secondsWorked));
        entry->setAttribute ("createdEpoch", juce::String (createdEpoch));
        entry->setAttribute ("updatedEpoch", juce::String ((juce::int64) juce::Time::currentTimeMillis() / 1000));

        db.writeTo (dbFile (settings));
    }
}
