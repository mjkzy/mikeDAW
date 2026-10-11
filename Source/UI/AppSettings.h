#pragma once
#include <JuceHeader.h>
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

namespace AppSettings
{
    inline juce::String startupTemplate (juce::PropertiesFile& s)
    {
        return s.getValue ("startupTemplate", "drums");
    }
    inline void setStartupTemplate (juce::PropertiesFile& s, const juce::String& v)
    {
        s.setValue ("startupTemplate", v);
    }

    inline double defaultBpm (juce::PropertiesFile& s)
    {
        return juce::jlimit (20.0, 999.0, s.getDoubleValue ("defaultBpm", 140.0));
    }
    inline void setDefaultBpm (juce::PropertiesFile& s, double v)
    {
        s.setValue ("defaultBpm", juce::jlimit (20.0, 999.0, v));
    }

    inline bool autoSaveEnabled (juce::PropertiesFile& s)
    {
        return s.getBoolValue ("autoSaveEnabled", true);
    }
    inline void setAutoSaveEnabled (juce::PropertiesFile& s, bool v)
    {
        s.setValue ("autoSaveEnabled", v);
    }

    inline int autoSaveMinutes (juce::PropertiesFile& s)
    {
        return juce::jlimit (1, 120, s.getIntValue ("autoSaveMinutes", 5));
    }
    inline void setAutoSaveMinutes (juce::PropertiesFile& s, int v)
    {
        s.setValue ("autoSaveMinutes", juce::jlimit (1, 120, v));
    }

    inline juce::File autoSaveFolder (juce::PropertiesFile& s)
    {
        auto def = s.getFile().getParentDirectory();
        auto path = s.getValue ("autoSaveFolder", def.getFullPathName());
        juce::File f (path);
        return f.getFullPathName().isNotEmpty() ? f : def;
    }
    inline void setAutoSaveFolder (juce::PropertiesFile& s, const juce::File& f)
    {
        s.setValue ("autoSaveFolder", f.getFullPathName());
    }

    inline juce::File autoSaveDir (juce::PropertiesFile& s)
    {
        return autoSaveFolder (s).getChildFile ("Autosaves");
    }

    inline juce::File autoSaveFile (juce::PropertiesFile& s)
    {
        return autoSaveFolder (s).getChildFile ("autosave.odaw");
    }

    inline juce::File projectsFolder (juce::PropertiesFile& s)
    {
        auto def = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                       .getChildFile ("mikeDAW").getChildFile ("Projects");
        juce::File f (s.getValue ("projectsFolder", def.getFullPathName()));
        if (f.getFullPathName().isEmpty())
            f = def;
        f.createDirectory();
        return f;
    }
    inline void setProjectsFolder (juce::PropertiesFile& s, const juce::File& f)
    {
        s.setValue ("projectsFolder", f.getFullPathName());
    }

    inline juce::String displayName (juce::PropertiesFile& s)
    {
        auto stored = s.getValue ("displayName", "");
        if (stored.trim().isNotEmpty())
            return stored.trim();
        auto user = juce::SystemStats::getFullUserName();
        if (user.trim().isEmpty())
            user = juce::SystemStats::getLogonName();
        return user.trim().isNotEmpty() ? user.trim() : juce::String ("A Kind Soul");
    }
    inline void setDisplayName (juce::PropertiesFile& s, const juce::String& v)
    {
        s.setValue ("displayName", v.trim());
    }

    inline juce::int64 localUserId (juce::PropertiesFile& s)
    {
        auto id = s.getValue ("localUserId", "").getLargeIntValue();
        if (id == 0)
        {
            while (id == 0)
                id = juce::Random::getSystemRandom().nextInt64();
            s.setValue ("localUserId", juce::String (id));
            s.saveIfNeeded();
        }
        return id;
    }

    inline juce::File avatarFile (juce::PropertiesFile& s)
    {
        return s.getFile().getParentDirectory().getChildFile ("avatar.png");
    }
    inline bool hasAvatar (juce::PropertiesFile& s)
    {
        return s.getBoolValue ("hasAvatar", false) && avatarFile (s).existsAsFile();
    }
    inline void setHasAvatar (juce::PropertiesFile& s, bool v)
    {
        s.setValue ("hasAvatar", v);
    }

    inline juce::StringArray recentProjects (juce::PropertiesFile& s)
    {
        juce::StringArray list;
        list.addTokens (s.getValue ("recentProjects"), "|", {});
        list.removeEmptyStrings();
        return list;
    }

    inline void addRecentProject (juce::PropertiesFile& s, const juce::File& f)
    {
        auto path = f.getFullPathName();
        if (path.containsIgnoreCase ("autosave"))
            return;
        auto list = recentProjects (s);
        list.removeString (path);
        list.insert (0, path);
        while (list.size() > 8)
            list.remove (list.size() - 1);
        s.setValue ("recentProjects", list.joinIntoString ("|"));
        s.saveIfNeeded();
    }

    inline juce::File templatesFolder (juce::PropertiesFile& s)
    {
        return s.getFile().getParentDirectory().getChildFile ("templates");
    }

    inline juce::String startupTemplateName (const juce::String& id)
    {
        if (id == "blank") return "empty";
        if (id == "drums") return "basic drums";
        return id;
    }

    inline int processPriority (juce::PropertiesFile& s)
    {
        return juce::jlimit (0, 2, s.getIntValue ("processPriority", 2));
    }
    inline void setProcessPriority (juce::PropertiesFile& s, int v)
    {
        s.setValue ("processPriority", juce::jlimit (0, 2, v));
    }
    inline void applyProcessPriority (int level)
    {
        using P = juce::Process;
        P::setPriority (level >= 2 ? P::HighPriority
                                   : level == 1 ? P::HighPriority : P::NormalPriority);
       #if JUCE_WINDOWS
        if (level == 1)
            SetPriorityClass (GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
       #endif
    }

    inline bool welcomeShown (juce::PropertiesFile& s)
    {
        return s.getBoolValue ("welcomeShown", false);
    }
    inline void setWelcomeShown (juce::PropertiesFile& s, bool v)
    {
        s.setValue ("welcomeShown", v);
    }

    inline bool followDefaultOutput (juce::PropertiesFile& s)
    {
        return s.getBoolValue ("followDefaultOutput", true);
    }
    inline void setFollowDefaultOutput (juce::PropertiesFile& s, bool v)
    {
        s.setValue ("followDefaultOutput", v);
    }

    inline bool smartDisable (juce::PropertiesFile& s)
    {
        return s.getBoolValue ("smartDisable", true);
    }
    inline void setSmartDisable (juce::PropertiesFile& s, bool v)
    {
        s.setValue ("smartDisable", v);
    }

    inline bool multicoreRendering (juce::PropertiesFile& s)
    {
        return s.getBoolValue ("multicoreRendering", true);
    }
    inline void setMulticoreRendering (juce::PropertiesFile& s, bool v)
    {
        s.setValue ("multicoreRendering", v);
    }

    inline double uiScale (juce::PropertiesFile& s)
    {
        return juce::jlimit (0.75, 2.0, s.getDoubleValue ("uiScale", 1.0));
    }
    inline void setUiScale (juce::PropertiesFile& s, double v)
    {
        s.setValue ("uiScale", juce::jlimit (0.75, 2.0, v));
    }

    inline bool showScaleHighlight (juce::PropertiesFile& s)
    {
        return s.getBoolValue ("showScaleHighlight", true);
    }
    inline void setShowScaleHighlight (juce::PropertiesFile& s, bool v)
    {
        s.setValue ("showScaleHighlight", v);
    }

    inline bool recNotes (juce::PropertiesFile& s)            { return s.getBoolValue ("recNotes", true); }
    inline void setRecNotes (juce::PropertiesFile& s, bool v) { s.setValue ("recNotes", v); }

    inline bool recAudio (juce::PropertiesFile& s)            { return s.getBoolValue ("recAudio", true); }
    inline void setRecAudio (juce::PropertiesFile& s, bool v) { s.setValue ("recAudio", v); }

    inline bool recStartsPlayback (juce::PropertiesFile& s)            { return s.getBoolValue ("recStartsPlayback", true); }
    inline void setRecStartsPlayback (juce::PropertiesFile& s, bool v) { s.setValue ("recStartsPlayback", v); }

    inline bool recDisarmOnStop (juce::PropertiesFile& s)            { return s.getBoolValue ("recDisarmOnStop", false); }
    inline void setRecDisarmOnStop (juce::PropertiesFile& s, bool v) { s.setValue ("recDisarmOnStop", v); }

    inline bool recQuantizeStart (juce::PropertiesFile& s)            { return s.getBoolValue ("recQuantizeStart", true); }
    inline void setRecQuantizeStart (juce::PropertiesFile& s, bool v) { s.setValue ("recQuantizeStart", v); }

    inline bool recQuantizeEnd (juce::PropertiesFile& s)            { return s.getBoolValue ("recQuantizeEnd", false); }
    inline void setRecQuantizeEnd (juce::PropertiesFile& s, bool v) { s.setValue ("recQuantizeEnd", v); }

    inline juce::File recordedDir (juce::PropertiesFile& s)
    {
        return autoSaveFolder (s).getChildFile ("Recorded");
    }

    inline bool discordRpc (juce::PropertiesFile& s)            { return s.getBoolValue ("discordRpc", true); }
    inline void setDiscordRpc (juce::PropertiesFile& s, bool v) { s.setValue ("discordRpc", v); }

    inline bool manualAway (juce::PropertiesFile& s)             { return s.getBoolValue ("manualAway", false); }
    inline void setManualAway (juce::PropertiesFile& s, bool v)  { s.setValue ("manualAway", v); }
    inline juce::int64 manualAwaySince (juce::PropertiesFile& s)             { return (juce::int64) s.getDoubleValue ("manualAwaySince", 0.0); }
    inline void setManualAwaySince (juce::PropertiesFile& s, juce::int64 ms) { s.setValue ("manualAwaySince", (double) ms); }

    inline bool micMuted (juce::PropertiesFile& s)              { return s.getBoolValue ("micMuted", false); }
    inline void setMicMuted (juce::PropertiesFile& s, bool v)   { s.setValue ("micMuted", v); }

    inline bool relayEnabled (juce::PropertiesFile& s)
    {
        return s.getBoolValue ("relayEnabled", true);
    }
    inline void setRelayEnabled (juce::PropertiesFile& s, bool v)
    {
        s.setValue ("relayEnabled", v);
    }

    inline juce::String relayHost (juce::PropertiesFile& s)
    {
       #if OPENDAW_PUBLIC_BUILD
        juce::ignoreUnused (s);
        return {};
       #else
        auto v = s.getValue ("relayHost", "148.113.175.195").trim();
        return v.isNotEmpty() ? v : juce::String ("148.113.175.195");
       #endif
    }
    inline void setRelayHost (juce::PropertiesFile& s, const juce::String& v)
    {
        s.setValue ("relayHost", v.trim());
    }

    inline int relayPort (juce::PropertiesFile& s)
    {
        return juce::jlimit (1, 65535, s.getIntValue ("relayPort", 41236));
    }
    inline void setRelayPort (juce::PropertiesFile& s, int v)
    {
        s.setValue ("relayPort", juce::jlimit (1, 65535, v));
    }

    inline juce::String mixerRackMode (juce::PropertiesFile& s)
    {
        auto v = s.getValue ("mixerRackMode", "rack");
        return v == "chain" ? juce::String ("chain") : juce::String ("rack");
    }
    inline void setMixerRackMode (juce::PropertiesFile& s, const juce::String& v)
    {
        s.setValue ("mixerRackMode", v == "chain" ? "chain" : "rack");
    }

    inline bool dockedLayout (juce::PropertiesFile& s)            { return s.getBoolValue ("dockedLayout", false); }
    inline void setDockedLayout (juce::PropertiesFile& s, bool v) { s.setValue ("dockedLayout", v); }

    inline int dockHeight (juce::PropertiesFile& s)            { return juce::jlimit (80, 4000, s.getIntValue ("dockHeight", 380)); }
    inline void setDockHeight (juce::PropertiesFile& s, int v) { s.setValue ("dockHeight", juce::jlimit (80, 4000, v)); }

    inline juce::String dockTab (juce::PropertiesFile& s)
    {
        auto v = s.getValue ("dockTab", "rack");
        return v == "roll" || v == "mixer" ? v : juce::String ("rack");
    }
    inline void setDockTab (juce::PropertiesFile& s, const juce::String& v) { s.setValue ("dockTab", v); }

    inline bool dockCollapsed (juce::PropertiesFile& s)            { return s.getBoolValue ("dockCollapsed", false); }
    inline void setDockCollapsed (juce::PropertiesFile& s, bool v) { s.setValue ("dockCollapsed", v); }
}
