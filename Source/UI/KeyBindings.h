#pragma once
#include <JuceHeader.h>

class KeyBindings
{
public:
    enum class Action
    {
        playPause, stop,
        saveProject, saveProjectAs,
        undo, redo,
        duplicate,
        copy, paste,
        selectAll, deselectAll, deleteSelection,
        legato,
        arpeggiate,
        strumize,
        randomizeNotes,
        octaveUp, octaveDown,
        semitoneUp, semitoneDown,
        toggleBrowser, toggleRack, toggleRoll, togglePlaylist, toggleMixer,
        toggleFullscreen,
        toggleTypingKeyboard,
        toggleDebugOverlay,
        numActions
    };

    struct Entry
    {
        Action action;
        juce::String id;
        juce::String label;
        juce::KeyPress defaultKey;
        juce::KeyPress key;
        bool warn = false;
    };

    KeyBindings()
    {
        using KP = juce::KeyPress;
        const int ctrl  = juce::ModifierKeys::ctrlModifier;
        const int shift = juce::ModifierKeys::shiftModifier;

        add (Action::playPause,   "play_pause",   "Play / Pause",       KP (KP::spaceKey));
        add (Action::stop,        "stop",         "Stop (rewind)",      KP (KP::returnKey));
        add (Action::saveProject,   "save_project",    "Save project",       KP ('s', ctrl, 0));
        add (Action::saveProjectAs, "save_project_as", "Save project as...", KP ('s', ctrl | shift, 0));
        add (Action::undo,        "undo",         "Undo",               KP ('z', ctrl, 0));
        add (Action::redo,        "redo",         "Redo",               KP ('y', ctrl, 0));
        add (Action::duplicate,   "duplicate",    "Duplicate (notes / clips)",  KP ('b', ctrl, 0));
        add (Action::copy,        "copy",         "Copy (notes)",       KP ('c', ctrl, 0));
        add (Action::paste,       "paste",        "Paste (notes)",      KP ('v', ctrl, 0));
        add (Action::selectAll,   "select_all",   "Select All (notes / clips)", KP ('a', ctrl, 0));
        add (Action::deselectAll, "deselect_all", "Deselect All",              KP ('d', ctrl, 0));
        add (Action::deleteSelection, "delete_selection", "Delete selection",   KP (KP::deleteKey));
        add (Action::legato,      "legato",       "Legato (extend to next note)", KP ('l', ctrl, 0));
        add (Action::arpeggiate,  "arpeggiate",   "Arpeggiate / repeat notes", KP ('a', juce::ModifierKeys::altModifier, 0));
        add (Action::strumize,    "strumize",     "Strum notes (strumizer)",   KP ('s', juce::ModifierKeys::altModifier, 0));
        add (Action::randomizeNotes, "randomize_notes", "Randomize notes weirdly", KP(), true);
        add (Action::octaveUp,    "octave_up",    "Octave Up",          KP (KP::upKey, ctrl, 0));
        add (Action::octaveDown,  "octave_down",  "Octave Down",        KP (KP::downKey, ctrl, 0));
        add (Action::semitoneUp,  "semitone_up",  "Semitone Up",        KP (KP::upKey, shift, 0));
        add (Action::semitoneDown,"semitone_down","Semitone Down",      KP (KP::downKey, shift, 0));
        add (Action::toggleBrowser, "toggle_browser",  "Toggle Browser",  KP (KP::F8Key, juce::ModifierKeys::altModifier, 0));
        add (Action::toggleRack,    "toggle_rack",     "Toggle Channel Rack", KP (KP::F6Key));
        add (Action::toggleRoll,    "toggle_roll",     "Toggle Piano Roll",   KP (KP::F7Key));
        add (Action::togglePlaylist,"toggle_playlist", "Toggle Playlist",     KP (KP::F5Key));
        add (Action::toggleMixer,   "toggle_mixer",    "Toggle Mixer",        KP (KP::F9Key));
        add (Action::toggleFullscreen, "toggle_fullscreen", "Toggle Window Fullscreen", KP());
        add (Action::toggleTypingKeyboard, "toggle_typing_keyboard",
             "Typing keyboard to piano (MIDI)", KP ('t', ctrl, 0));
        add (Action::toggleDebugOverlay, "toggle_debug_overlay",
             "Toggle Debug Overlay", KP ('d', ctrl | shift, 0));
    }

    void load (juce::PropertiesFile& props)
    {
        for (auto& e : entries)
        {
            auto stored = props.getValue ("key_" + e.id, "__unset__");
            if (stored == "__unset__")
                continue;
            e.key = stored.isEmpty() ? juce::KeyPress()
                                     : juce::KeyPress::createFromDescription (stored);
        }
    }

    void save (juce::PropertiesFile& props) const
    {
        for (auto& e : entries)
            props.setValue ("key_" + e.id,
                            e.key.isValid() ? e.key.getTextDescription() : juce::String());
        props.saveIfNeeded();
    }

    Action match (const juce::KeyPress& kp) const
    {
        for (auto& e : entries)
            if (e.key.isValid() && e.key == kp)
                return e.action;
        return Action::numActions;
    }

    juce::KeyPress keyFor (Action a) const
    {
        for (auto& e : entries)
            if (e.action == a)
                return e.key;
        return {};
    }

    std::vector<Entry>& all() { return entries; }
    const std::vector<Entry>& all() const { return entries; }

private:
    void add (Action a, const juce::String& id, const juce::String& label,
              juce::KeyPress def, bool warn = false)
    {
        entries.push_back ({ a, id, label, def, def, warn });
    }

    std::vector<Entry> entries;
};
