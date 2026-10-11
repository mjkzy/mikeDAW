#include "ProjectModel.h"

namespace Model
{

int newId()
{
    return juce::Random::getSystemRandom().nextInt (0x3fffffff) + 1;
}

juce::String emptySteps (int numSteps)
{
    return juce::String::repeatedString ("0", numSteps);
}

juce::Colour channelColour (int channelId)
{
    static const juce::uint32 palette[] = {
        0xff62d0bd, 0xffef9c4d, 0xffe06c75, 0xff61afef,
        0xffc678dd, 0xff98c379, 0xffd19a66, 0xff56b6c2
    };
    return juce::Colour (palette[(juce::uint32) channelId % 8]);
}

juce::String uniqueChannelName (const juce::ValueTree& project, const juce::String& base)
{
    auto channels = project.getChildWithName (IDs::Channels);
    auto taken = [&channels] (const juce::String& candidate)
    {
        for (int i = 0; i < channels.getNumChildren(); ++i)
        {
            auto c = channels.getChild (i);
            if (c.hasType (IDs::Channel)
                && c.getProperty (IDs::name).toString() == candidate)
                return true;
        }
        return false;
    };

    if (! taken (base))
        return base;

    for (int n = 2;; ++n)
    {
        auto candidate = base + " (" + juce::String (n) + ")";
        if (! taken (candidate))
            return candidate;
    }
}

juce::Colour patternColour (const juce::ValueTree& pattern)
{
    auto stored = pattern.getProperty (IDs::colour).toString();
    if (stored.isNotEmpty())
    {
        auto c = juce::Colour::fromString (stored);
        if (! c.isTransparent())
            return c;
    }
    return channelColour ((int) pattern.getProperty (IDs::id));
}

juce::ValueTree getChannels (juce::ValueTree p) { return p.getOrCreateChildWithName (IDs::Channels, nullptr); }
juce::ValueTree getPatterns (juce::ValueTree p) { return p.getOrCreateChildWithName (IDs::Patterns, nullptr); }
juce::ValueTree getPlaylist (juce::ValueTree p) { return p.getOrCreateChildWithName (IDs::Playlist, nullptr); }
juce::ValueTree getMixer    (juce::ValueTree p) { return p.getOrCreateChildWithName (IDs::Mixer,    nullptr); }
juce::ValueTree getAudioFiles (juce::ValueTree p) { return p.getOrCreateChildWithName (IDs::AudioFiles, nullptr); }
juce::ValueTree getMidiFiles (juce::ValueTree p) { return p.getOrCreateChildWithName (IDs::MidiFiles, nullptr); }

juce::ValueTree findChannelById (juce::ValueTree project, int id)
{
    return getChannels (project).getChildWithProperty (IDs::id, id);
}

juce::ValueTree findPatternById (juce::ValueTree project, int id)
{
    return getPatterns (project).getChildWithProperty (IDs::id, id);
}

juce::ValueTree getMixerTrack (juce::ValueTree project, int index)
{
    return getMixer (project).getChildWithProperty (IDs::index, index);
}

static juce::ValueTree makeMixerTrack (int index)
{
    juce::ValueTree t (IDs::Track);
    t.setProperty (IDs::index, index, nullptr);
    t.setProperty (IDs::name, index == 0 ? juce::String ("Master") : "Insert " + juce::String (index), nullptr);
    t.setProperty (IDs::volume, 1.0, nullptr);
    t.setProperty (IDs::pan, 0.0, nullptr);
    t.setProperty (IDs::mute, false, nullptr);
    t.setProperty (IDs::solo, false, nullptr);
    t.setProperty (IDs::routeTo, 0, nullptr);
    return t;
}

int getMaxMixerTrackIndex (const juce::ValueTree& project)
{
    auto mixer = project.getChildWithName (IDs::Mixer);
    int maxIdx = 0;
    for (int i = 0; i < mixer.getNumChildren(); ++i)
    {
        auto t = mixer.getChild (i);
        if (t.hasType (IDs::Track))
            maxIdx = juce::jmax (maxIdx, (int) t.getProperty (IDs::index, 0));
    }
    return maxIdx;
}

juce::ValueTree addMixerTrack (juce::ValueTree project)
{
    int next = getMaxMixerTrackIndex (project) + 1;
    if (next > maxMixerTracks)
        return {};
    auto t = makeMixerTrack (next);
    getMixer (project).appendChild (t, nullptr);
    return t;
}

static juce::ValueTree makeChannel (const juce::String& type, const juce::String& name)
{
    juce::ValueTree c (IDs::Channel);
    c.setProperty (IDs::id, newId(), nullptr);
    c.setProperty (IDs::type, type, nullptr);
    c.setProperty (IDs::name, name, nullptr);
    c.setProperty (IDs::volume, 0.8, nullptr);
    c.setProperty (IDs::pan, 0.0, nullptr);
    c.setProperty (IDs::mute, false, nullptr);
    c.setProperty (IDs::mixerTrack, 1, nullptr);
    return c;
}

juce::ValueTree addSamplerChannel (juce::ValueTree project, const juce::String& name,
                                   const juce::String& samplePath)
{
    auto c = makeChannel ("sampler", name);
    c.setProperty (IDs::samplePath, samplePath, nullptr);
    c.setProperty (IDs::sampleKeepOnDisk, false, nullptr);
    c.setProperty (IDs::sampleResample, false, nullptr);
    c.setProperty (IDs::loadRegions, false, nullptr);
    c.setProperty (IDs::loadSliceMarkers, false, nullptr);
    c.setProperty (IDs::startOffset, 0.0, nullptr);
    c.setProperty (IDs::useLoopPoints, false, nullptr);
    c.setProperty (IDs::pingPongLoop, false, nullptr);
    c.setProperty (IDs::loopStart, 0.0, nullptr);
    c.setProperty (IDs::loopEnd, 1.0, nullptr);
    c.setProperty (IDs::loopCrossfade, 0.0, nullptr);
    c.setProperty (IDs::stretchMode, "resample", nullptr);
    c.setProperty (IDs::stretchPitchSemis, 0.0, nullptr);
    c.setProperty (IDs::stretchMultiplier, 1.0, nullptr);
    c.setProperty (IDs::stretchTime, 1.0, nullptr);
    c.setProperty (IDs::stretchTempoMode, 0, nullptr);
    c.setProperty (IDs::stretchSourceBpm, 0.0, nullptr);
    c.setProperty (IDs::stretchBeats, 0.0, nullptr);
    c.setProperty (IDs::formantShift, 0.0, nullptr);
    getChannels (project).appendChild (c, nullptr);
    return c;
}

juce::ValueTree addPluginChannel (juce::ValueTree project, const juce::String& name,
                                  const juce::String& descXml)
{
    auto c = makeChannel ("plugin", name);
    c.setProperty (IDs::pluginDesc, descXml, nullptr);
    getChannels (project).appendChild (c, nullptr);
    return c;
}

juce::ValueTree addBuiltinChannel (juce::ValueTree project, const juce::String& name)
{
    auto c = makeChannel ("builtin", name);
    getChannels (project).appendChild (c, nullptr);
    return c;
}

juce::ValueTree addPattern (juce::ValueTree project, const juce::String& name)
{
    auto patterns = getPatterns (project);
    juce::ValueTree pat (IDs::Pattern);
    pat.setProperty (IDs::id, newId(), nullptr);
    pat.setProperty (IDs::name, name.isNotEmpty() ? name
                        : "Pattern " + juce::String (patterns.getNumChildren() + 1), nullptr);
    pat.setProperty (IDs::lengthSteps, defaultSteps, nullptr);
    patterns.appendChild (pat, nullptr);
    return pat;
}

juce::ValueTree addAudioResource (juce::ValueTree project, const juce::File& file)
{
    if (! file.existsAsFile())
        return {};

    auto files = getAudioFiles (project);
    auto path = file.getFullPathName();
    auto existing = files.getChildWithProperty (IDs::audioPath, path);
    if (existing.hasType (IDs::AudioFile))
        return existing;

    juce::ValueTree item (IDs::AudioFile);
    item.setProperty (IDs::id, newId(), nullptr);
    item.setProperty (IDs::name, file.getFileNameWithoutExtension(), nullptr);
    item.setProperty (IDs::audioPath, path, nullptr);
    files.appendChild (item, nullptr);
    return item;
}

juce::ValueTree addMidiResource (juce::ValueTree project, const juce::File& file)
{
    if (! file.existsAsFile() || ! file.hasFileExtension ("mid;midi"))
        return {};

    auto files = getMidiFiles (project);
    auto path = file.getFullPathName();
    auto existing = files.getChildWithProperty (IDs::midiPath, path);
    if (existing.hasType (IDs::MidiFile))
        return existing;

    juce::ValueTree item (IDs::MidiFile);
    item.setProperty (IDs::id, newId(), nullptr);
    item.setProperty (IDs::name, file.getFileNameWithoutExtension(), nullptr);
    item.setProperty (IDs::midiPath, path, nullptr);
    files.appendChild (item, nullptr);
    return item;
}

void mergeDuplicateChannelSeqs (juce::ValueTree pattern, juce::UndoManager* undo)
{
    if (! pattern.isValid())
        return;

    std::map<int, std::vector<juce::ValueTree>> byChannel;
    std::vector<int> order;
    for (int i = 0; i < pattern.getNumChildren(); ++i)
    {
        auto seq = pattern.getChild (i);
        if (! seq.hasType (IDs::ChannelSeq))
            continue;
        int channelId = (int) seq.getProperty (IDs::channelId, -1);
        auto& group = byChannel[channelId];
        if (group.empty())
            order.push_back (channelId);
        group.push_back (seq);
    }

    for (int channelId : order)
    {
        auto& group = byChannel[channelId];
        if (group.size() < 2)
            continue;

        auto target = group[0];
        auto steps = target.getProperty (IDs::steps).toString();
        bool anyPianoRoll = (bool) target.getProperty (IDs::usePianoRoll, false);

        for (size_t k = 1; k < group.size(); ++k)
        {
            auto& dup = group[k];
            auto ds = dup.getProperty (IDs::steps).toString();
            int len = juce::jmax (steps.length(), ds.length());
            juce::String merged;
            merged.preallocateBytes ((size_t) len);
            for (int c = 0; c < len; ++c)
            {
                auto a = c < steps.length() ? steps[c] : (juce::juce_wchar) '0';
                auto b = c < ds.length()   ? ds[c]    : (juce::juce_wchar) '0';
                merged += (a == '1' || b == '1') ? '1' : '0';
            }
            steps = merged;
            anyPianoRoll = anyPianoRoll || (bool) dup.getProperty (IDs::usePianoRoll, false);

            while (dup.getNumChildren() > 0)
            {
                auto note = dup.getChild (0);
                dup.removeChild (note, undo);
                target.appendChild (note, undo);
            }
        }

        target.setProperty (IDs::steps, steps, undo);
        if (anyPianoRoll)
            target.setProperty (IDs::usePianoRoll, true, undo);

        for (size_t k = 1; k < group.size(); ++k)
            pattern.removeChild (group[k], undo);
    }
}

juce::ValueTree getChannelSeq (juce::ValueTree pattern, int channelId, bool createIfMissing)
{
    int matches = 0;
    for (int i = 0; i < pattern.getNumChildren(); ++i)
    {
        auto c = pattern.getChild (i);
        if (c.hasType (IDs::ChannelSeq) && (int) c.getProperty (IDs::channelId, -1) == channelId)
            ++matches;
    }
    if (matches > 1)
        mergeDuplicateChannelSeqs (pattern, nullptr);

    auto seq = pattern.getChildWithProperty (IDs::channelId, channelId);
    if (! seq.isValid() && createIfMissing)
    {
        seq = juce::ValueTree (IDs::ChannelSeq);
        seq.setProperty (IDs::channelId, channelId, nullptr);
        seq.setProperty (IDs::steps, emptySteps ((int) pattern.getProperty (IDs::lengthSteps, defaultSteps)), nullptr);
        pattern.appendChild (seq, nullptr);
    }
    return seq;
}

void removeChannelSequences (juce::ValueTree project, int channelId)
{
    auto patterns = getPatterns (project);
    for (int p = 0; p < patterns.getNumChildren(); ++p)
    {
        auto pattern = patterns.getChild (p);
        for (int i = pattern.getNumChildren(); --i >= 0;)
        {
            auto seq = pattern.getChild (i);
            if (seq.hasType (IDs::ChannelSeq)
                && (int) seq.getProperty (IDs::channelId, -1) == channelId)
                pattern.removeChild (i, nullptr);
        }
    }
}

void ensurePlaylistTracks (juce::ValueTree project)
{
    auto playlist = getPlaylist (project);

    int needed = numPlaylistTracks;
    for (int i = 0; i < playlist.getNumChildren(); ++i)
    {
        auto clip = playlist.getChild (i);
        if (clip.hasType (IDs::Clip))
            needed = juce::jmax (needed, (int) clip.getProperty (IDs::track) + 1);
    }

    for (int idx = 0; idx < needed; ++idx)
    {
        if (playlist.getChildWithProperty (IDs::index, idx).isValid())
            continue;
        juce::ValueTree t (IDs::PlaylistTrack);
        t.setProperty (IDs::index, idx, nullptr);
        t.setProperty (IDs::name, "Track " + juce::String (idx + 1), nullptr);
        t.setProperty (IDs::enabled, true, nullptr);
        playlist.appendChild (t, nullptr);
    }
}

juce::ValueTree getPlaylistTrack (juce::ValueTree project, int index)
{
    auto playlist = getPlaylist (project);
    auto t = playlist.getChildWithProperty (IDs::index, index);
    return t.hasType (IDs::PlaylistTrack) ? t : juce::ValueTree();
}

int countPlaylistTracks (const juce::ValueTree& project)
{
    int count = 0;
    auto playlist = project.getChildWithName (IDs::Playlist);
    for (int i = 0; i < playlist.getNumChildren(); ++i)
        if (playlist.getChild (i).hasType (IDs::PlaylistTrack))
            ++count;
    return juce::jmax (count, numPlaylistTracks);
}

bool isPlaylistTrackEnabled (const juce::ValueTree& project, int index)
{
    auto playlist = project.getChildWithName (IDs::Playlist);
    auto t = playlist.getChildWithProperty (IDs::index, index);
    if (t.hasType (IDs::PlaylistTrack))
        return (bool) t.getProperty (IDs::enabled, true);
    return true;
}

void convertStepsToNotes (juce::ValueTree pattern, int channelId, int key)
{
    if (! pattern.isValid())
        return;
    mergeDuplicateChannelSeqs (pattern, nullptr);
    auto seq = pattern.getChildWithProperty (IDs::channelId, channelId);
    if (! seq.isValid())
        return;
    if ((bool) seq.getProperty (IDs::usePianoRoll, false))
        return;

    auto steps = seq.getProperty (IDs::steps).toString();

    if (seq.getChildWithName (IDs::Note).isValid() && ! steps.containsChar ('1'))
        return;
    for (int s = 0; s < steps.length(); ++s)
    {
        if (steps[s] != '1')
            continue;
        juce::ValueTree note (IDs::Note);
        note.setProperty (IDs::key, key, nullptr);
        note.setProperty (IDs::start, s * ticksPerStep, nullptr);
        note.setProperty (IDs::length, ticksPerStep, nullptr);
        note.setProperty (IDs::velocity, 100, nullptr);
        seq.appendChild (note, nullptr);
    }

    seq.setProperty (IDs::usePianoRoll, true, nullptr);
    seq.setProperty (IDs::steps, emptySteps ((int) pattern.getProperty (IDs::lengthSteps, defaultSteps)), nullptr);
}

int patternLengthTicks (const juce::ValueTree& pattern)
{
    int steps = (int) pattern.getProperty (IDs::lengthSteps, defaultSteps);
    return juce::jmax (1, steps) * ticksPerStep;
}

juce::int64 patternContentEndTicks (const juce::ValueTree& pattern)
{
    juce::int64 end = 0;
    for (int i = 0; i < pattern.getNumChildren(); ++i)
    {
        auto seq = pattern.getChild (i);
        if (! seq.hasType (IDs::ChannelSeq))
            continue;

        auto steps = seq.getProperty (IDs::steps).toString();
        for (int s = 0; s < steps.length(); ++s)
            if (steps[s] == '1')
                end = juce::jmax (end, (juce::int64) (s + 1) * ticksPerStep);

        for (int n = 0; n < seq.getNumChildren(); ++n)
        {
            auto note = seq.getChild (n);
            if (! note.hasType (IDs::Note))
                continue;
            juce::int64 on  = (juce::int64) (int) note.getProperty (IDs::start);
            juce::int64 len = (juce::int64) juce::jmax (1, (int) note.getProperty (IDs::length));
            end = juce::jmax (end, on + len);
        }
    }
    return end;
}

juce::ValueTree createEmptyProject()
{
    juce::ValueTree p (IDs::Project);
    p.setProperty (IDs::name, "Untitled", nullptr);
    p.setProperty (IDs::bpm, 140.0, nullptr);
    p.setProperty (IDs::scaleRoot, 0, nullptr);
    p.setProperty (IDs::scaleType, "major", nullptr);
    p.setProperty (IDs::songMode, false, nullptr);
    p.setProperty (IDs::createdEpoch, (juce::int64) juce::Time::currentTimeMillis() / 1000, nullptr);
    p.setProperty (IDs::secondsWorked, (juce::int64) 0, nullptr);

    getChannels (p);
    getPatterns (p);
    getPlaylist (p);
    getAudioFiles (p);
    getMidiFiles (p);
    ensurePlaylistTracks (p);

    auto mixer = getMixer (p);
    for (int i = 0; i <= numMixerTracks; ++i)
        mixer.appendChild (makeMixerTrack (i), nullptr);
    return p;
}

juce::ValueTree createDefaultProject()
{
    auto p = createEmptyProject();

    addSamplerChannel (p, "Kick",  "builtin:kick");
    addSamplerChannel (p, "Clap",  "builtin:clap");
    addSamplerChannel (p, "Hat",   "builtin:hat");
    addSamplerChannel (p, "Snare", "builtin:snare");

    addPattern (p, "Pattern 1");
    return p;
}

juce::ValueTree createBlankSamplerProject()
{
    auto p = createEmptyProject();
    addSamplerChannel (p, "Sampler", {});
    addPattern (p, "Pattern 1");
    return p;
}

juce::ValueTree createProjectFromTemplate (const juce::String& templateId, double defaultBpm)
{
    auto p = templateId == "blank" ? createBlankSamplerProject()
                                   : createDefaultProject();
    p.setProperty (IDs::bpm, defaultBpm, nullptr);
    return p;
}

}
