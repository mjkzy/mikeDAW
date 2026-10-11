#pragma once
#include <JuceHeader.h>
#include "ProjectModel.h"

namespace Scales
{
    struct Def { const char* id; const char* name; std::array<int, 12> mask; };

    inline const std::vector<Def>& all()
    {
        static const std::vector<Def> defs = {
            { "major",      "Major",           { 1,0,1,0,1,1,0,1,0,1,0,1 } },
            { "minor",      "Minor",           { 1,0,1,1,0,1,0,1,1,0,1,0 } },
            { "harmonicMin","Harmonic Minor",  { 1,0,1,1,0,1,0,1,1,0,0,1 } },
            { "melodicMin", "Melodic Minor",   { 1,0,1,1,0,1,0,1,0,1,0,1 } },
            { "dorian",     "Dorian",          { 1,0,1,1,0,1,0,1,0,1,1,0 } },
            { "phrygian",   "Phrygian",        { 1,1,0,1,0,1,0,1,1,0,1,0 } },
            { "lydian",     "Lydian",          { 1,0,1,0,1,0,1,1,0,1,0,1 } },
            { "mixolydian", "Mixolydian",      { 1,0,1,0,1,1,0,1,0,1,1,0 } },
            { "locrian",    "Locrian",         { 1,1,0,1,0,1,1,0,1,0,1,0 } },
            { "pentaMajor", "Pentatonic Major",{ 1,0,1,0,1,0,0,1,0,1,0,0 } },
            { "pentaMinor", "Pentatonic Minor",{ 1,0,0,1,0,1,0,1,0,0,1,0 } },
            { "blues",      "Blues",           { 1,0,0,1,0,1,1,1,0,0,1,0 } },
            { "chromatic",  "Chromatic",       { 1,1,1,1,1,1,1,1,1,1,1,1 } },
        };
        return defs;
    }

    inline const char* rootName (int root)
    {
        static const char* names[12] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
        return names[((root % 12) + 12) % 12];
    }

    inline const Def& defForId (const juce::String& id)
    {
        for (auto& d : all())
            if (id == d.id) return d;
        return all()[0];
    }

    inline int projectRoot (const juce::ValueTree& project)
    {
        return ((int) project.getProperty (IDs::scaleRoot, 0) % 12 + 12) % 12;
    }
    inline juce::String projectTypeId (const juce::ValueTree& project)
    {
        return project.getProperty (IDs::scaleType, "major").toString();
    }

    inline bool keyInScale (const juce::ValueTree& project, int midiKey)
    {
        const auto& d = defForId (projectTypeId (project));
        int pc = (((midiKey - projectRoot (project)) % 12) + 12) % 12;
        return d.mask[(size_t) pc] != 0;
    }

    struct DetectResult { int root = 0; juce::String typeId = "major"; bool hasNotes = false; };

    inline std::array<double, 12> pitchClassHistogram (const juce::ValueTree& project)
    {
        std::array<double, 12> hist { };
        auto patterns = project.getChildWithName (IDs::Patterns);
        for (auto pattern : patterns)
        {
            if (! pattern.hasType (IDs::Pattern)) continue;
            for (auto seq : pattern)
            {
                if (! seq.hasType (IDs::ChannelSeq)) continue;
                for (auto note : seq)
                {
                    if (! note.hasType (IDs::Note)) continue;
                    int key = (int) note.getProperty (IDs::key, -1);
                    if (key < 0) continue;
                    double w = (double) note.getProperty (IDs::length, 0);
                    if (w <= 0.0) w = 1.0;
                    hist[(size_t) (((key % 12) + 12) % 12)] += w;
                }
            }
        }
        return hist;
    }

    inline DetectResult detectKey (const juce::ValueTree& project)
    {
        static const std::array<double, 12> major {
            6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
        static const std::array<double, 12> minor {
            6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };

        auto hist = pitchClassHistogram (project);

        double total = 0.0;
        for (double v : hist) total += v;
        if (total <= 0.0) return {};

        auto correlate = [&hist] (const std::array<double, 12>& profile, int root) -> double
        {
            double mx = 0.0, my = 0.0;
            for (int i = 0; i < 12; ++i) { mx += hist[(size_t) i]; my += profile[(size_t) i]; }
            mx /= 12.0; my /= 12.0;
            double num = 0.0, dx = 0.0, dy = 0.0;
            for (int i = 0; i < 12; ++i)
            {
                double x = hist[(size_t) (((i - root) % 12 + 12) % 12)] - mx;
                double y = profile[(size_t) i] - my;
                num += x * y; dx += x * x; dy += y * y;
            }
            double den = std::sqrt (dx * dy);
            return den > 0.0 ? num / den : 0.0;
        };

        DetectResult best; best.hasNotes = true;
        double bestScore = -2.0;
        for (int root = 0; root < 12; ++root)
        {
            double sMaj = correlate (major, root);
            if (sMaj > bestScore) { bestScore = sMaj; best.root = root; best.typeId = "major"; }
            double sMin = correlate (minor, root);
            if (sMin > bestScore) { bestScore = sMin; best.root = root; best.typeId = "minor"; }
        }
        return best;
    }
}
