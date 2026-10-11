#pragma once
#include <JuceHeader.h>

namespace IDs
{
    #define OPENDAW_DECLARE_ID(name) inline const juce::Identifier name (#name);

    OPENDAW_DECLARE_ID (Project)
    OPENDAW_DECLARE_ID (bpm)
    OPENDAW_DECLARE_ID (name)
    OPENDAW_DECLARE_ID (scaleRoot)
    OPENDAW_DECLARE_ID (scaleType)
    OPENDAW_DECLARE_ID (songMode)

    OPENDAW_DECLARE_ID (createdEpoch)
    OPENDAW_DECLARE_ID (secondsWorked)

    OPENDAW_DECLARE_ID (Channels)
    OPENDAW_DECLARE_ID (Channel)
    OPENDAW_DECLARE_ID (id)
    OPENDAW_DECLARE_ID (type)
    OPENDAW_DECLARE_ID (samplePath)
    OPENDAW_DECLARE_ID (pluginDesc)
    OPENDAW_DECLARE_ID (pluginState)
    OPENDAW_DECLARE_ID (volume)
    OPENDAW_DECLARE_ID (pan)
    OPENDAW_DECLARE_ID (mute)
    OPENDAW_DECLARE_ID (solo)
    OPENDAW_DECLARE_ID (mixerTrack)
    OPENDAW_DECLARE_ID (colour)

    OPENDAW_DECLARE_ID (Patterns)
    OPENDAW_DECLARE_ID (Pattern)
    OPENDAW_DECLARE_ID (lengthSteps)
    OPENDAW_DECLARE_ID (ChannelSeq)
    OPENDAW_DECLARE_ID (channelId)
    OPENDAW_DECLARE_ID (steps)
    OPENDAW_DECLARE_ID (usePianoRoll)
    OPENDAW_DECLARE_ID (cutItself)
    OPENDAW_DECLARE_ID (declickMode)

    OPENDAW_DECLARE_ID (pitchSemis)
    OPENDAW_DECLARE_ID (pitchRange)
    OPENDAW_DECLARE_ID (rootNote)
    OPENDAW_DECLARE_ID (fineTune)
    OPENDAW_DECLARE_ID (fxRemoveDC)
    OPENDAW_DECLARE_ID (fxNormalize)
    OPENDAW_DECLARE_ID (fxReverse)
    OPENDAW_DECLARE_ID (fxRevPolarity)
    OPENDAW_DECLARE_ID (fxFadeStereo)
    OPENDAW_DECLARE_ID (fxSwapStereo)
    OPENDAW_DECLARE_ID (smpStart)
    OPENDAW_DECLARE_ID (smpLength)
    OPENDAW_DECLARE_ID (smpTrim)
    OPENDAW_DECLARE_ID (fadeIn)
    OPENDAW_DECLARE_ID (fadeOut)
    OPENDAW_DECLARE_ID (sampleKeepOnDisk)
    OPENDAW_DECLARE_ID (sampleResample)
    OPENDAW_DECLARE_ID (loadRegions)
    OPENDAW_DECLARE_ID (loadSliceMarkers)
    OPENDAW_DECLARE_ID (startOffset)
    OPENDAW_DECLARE_ID (useLoopPoints)
    OPENDAW_DECLARE_ID (pingPongLoop)
    OPENDAW_DECLARE_ID (loopStart)
    OPENDAW_DECLARE_ID (loopEnd)
    OPENDAW_DECLARE_ID (loopCrossfade)
    OPENDAW_DECLARE_ID (stretchMode)
    OPENDAW_DECLARE_ID (stretchPitchSemis)
    OPENDAW_DECLARE_ID (stretchMultiplier)
    OPENDAW_DECLARE_ID (stretchTime)
    OPENDAW_DECLARE_ID (stretchTempoMode)
    OPENDAW_DECLARE_ID (stretchSourceBpm)
    OPENDAW_DECLARE_ID (stretchBeats)
    OPENDAW_DECLARE_ID (formantShift)
    OPENDAW_DECLARE_ID (Note)
    OPENDAW_DECLARE_ID (key)
    OPENDAW_DECLARE_ID (start)
    OPENDAW_DECLARE_ID (length)
    OPENDAW_DECLARE_ID (velocity)
    OPENDAW_DECLARE_ID (slide)

    OPENDAW_DECLARE_ID (Playlist)
    OPENDAW_DECLARE_ID (Clip)
    OPENDAW_DECLARE_ID (patternId)
    OPENDAW_DECLARE_ID (path)
    OPENDAW_DECLARE_ID (track)
    OPENDAW_DECLARE_ID (startBeats)
    OPENDAW_DECLARE_ID (lengthBeats)
    OPENDAW_DECLARE_ID (offsetBeats)
    OPENDAW_DECLARE_ID (pending)
    OPENDAW_DECLARE_ID (recordedBy)
    OPENDAW_DECLARE_ID (PlaylistTrack)

    OPENDAW_DECLARE_ID (target)
    OPENDAW_DECLARE_ID (Point)
    OPENDAW_DECLARE_ID (pos)
    OPENDAW_DECLARE_ID (val)
    OPENDAW_DECLARE_ID (curveMode)
    OPENDAW_DECLARE_ID (tension)

    OPENDAW_DECLARE_ID (AudioFiles)
    OPENDAW_DECLARE_ID (AudioFile)
    OPENDAW_DECLARE_ID (audioPath)

    OPENDAW_DECLARE_ID (MidiFiles)
    OPENDAW_DECLARE_ID (MidiFile)
    OPENDAW_DECLARE_ID (midiPath)

    OPENDAW_DECLARE_ID (Mixer)
    OPENDAW_DECLARE_ID (Track)
    OPENDAW_DECLARE_ID (index)
    OPENDAW_DECLARE_ID (routeTo)
    OPENDAW_DECLARE_ID (audioInput)
    OPENDAW_DECLARE_ID (recArm)
    OPENDAW_DECLARE_ID (stereoSep)
    OPENDAW_DECLARE_ID (Slot)
    OPENDAW_DECLARE_ID (uid)
    OPENDAW_DECLARE_ID (slotIndex)
    OPENDAW_DECLARE_ID (enabled)
    OPENDAW_DECLARE_ID (mix)
    OPENDAW_DECLARE_ID (genericUI)

    #undef OPENDAW_DECLARE_ID
}

namespace Model
{
    constexpr int PPQ            = 96;
    constexpr int stepsPerBeat   = 4;
    constexpr int ticksPerStep   = PPQ / stepsPerBeat;
    constexpr int beatsPerBar    = 4;
    constexpr int ticksPerBar    = PPQ * beatsPerBar;

    constexpr int numMixerTracks    = 20;
    constexpr int maxMixerTracks    = 64;
    constexpr int numSlotsPerTrack  = 25;
    constexpr int numPlaylistTracks = 9;
    constexpr int defaultSteps      = 16;

    juce::ValueTree createDefaultProject();
    juce::ValueTree createEmptyProject();
    juce::ValueTree createBlankSamplerProject();

    juce::ValueTree createProjectFromTemplate (const juce::String& templateId, double defaultBpm = 140.0);

    int newId();

    juce::ValueTree getChannels (juce::ValueTree project);
    juce::ValueTree getPatterns (juce::ValueTree project);
    juce::ValueTree getPlaylist (juce::ValueTree project);
    juce::ValueTree getMixer    (juce::ValueTree project);
    juce::ValueTree getAudioFiles (juce::ValueTree project);
    juce::ValueTree getMidiFiles (juce::ValueTree project);

    juce::ValueTree findChannelById (juce::ValueTree project, int id);
    juce::ValueTree findPatternById (juce::ValueTree project, int id);
    juce::ValueTree getMixerTrack   (juce::ValueTree project, int index);

    int getMaxMixerTrackIndex (const juce::ValueTree& project);
    juce::ValueTree addMixerTrack (juce::ValueTree project);

    juce::ValueTree addSamplerChannel (juce::ValueTree project, const juce::String& name,
                                       const juce::String& samplePath);
    juce::ValueTree addPluginChannel  (juce::ValueTree project, const juce::String& name,
                                       const juce::String& descXml);
    juce::ValueTree addBuiltinChannel (juce::ValueTree project, const juce::String& name);
    juce::ValueTree addPattern        (juce::ValueTree project, const juce::String& name = {});
    juce::ValueTree addAudioResource   (juce::ValueTree project, const juce::File& file);
    juce::ValueTree addMidiResource   (juce::ValueTree project, const juce::File& file);

    juce::ValueTree getChannelSeq (juce::ValueTree pattern, int channelId, bool createIfMissing);

    void removeChannelSequences (juce::ValueTree project, int channelId);

    void mergeDuplicateChannelSeqs (juce::ValueTree pattern, juce::UndoManager* undo = nullptr);

    void convertStepsToNotes (juce::ValueTree pattern, int channelId, int key = 60);

    constexpr int middleC = 60;

    void ensurePlaylistTracks (juce::ValueTree project);
    juce::ValueTree getPlaylistTrack (juce::ValueTree project, int index);
    int countPlaylistTracks (const juce::ValueTree& project);
    bool isPlaylistTrackEnabled (const juce::ValueTree& project, int index);

    juce::String emptySteps (int numSteps);
    juce::Colour channelColour (int channelId);

    juce::String uniqueChannelName (const juce::ValueTree& project, const juce::String& base);

    juce::Colour patternColour (const juce::ValueTree& pattern);

    int patternLengthTicks (const juce::ValueTree& pattern);

    juce::int64 patternContentEndTicks (const juce::ValueTree& pattern);
}
