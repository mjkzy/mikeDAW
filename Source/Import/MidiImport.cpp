#include "MidiImport.h"

namespace MidiImport
{

int applyToChannelSeq (const juce::File& file, juce::ValueTree pattern, juce::ValueTree seq)
{
    if (! file.existsAsFile() || ! pattern.isValid() || ! seq.isValid())
        return 0;

    juce::FileInputStream in (file);
    if (! in.openedOk())
        return 0;

    juce::MidiFile mf;
    if (! mf.readFrom (in))
        return 0;

    short tf = mf.getTimeFormat();
    double srcPPQ = tf > 0 ? (double) tf : (double) Model::PPQ;
    double scale = (double) Model::PPQ / srcPPQ;

    struct Pending { int key; int vel; double startTicks; };
    juce::Array<Pending> open;
    int imported = 0;
    int maxEndTicks = 0;

    for (int t = 0; t < mf.getNumTracks(); ++t)
    {
        auto* track = mf.getTrack (t);
        if (track == nullptr)
            continue;

        for (int i = 0; i < track->getNumEvents(); ++i)
        {
            const auto& m = track->getEventPointer (i)->message;
            double tick = m.getTimeStamp() * scale;

            if (m.isNoteOn() && m.getVelocity() > 0)
            {
                open.add ({ m.getNoteNumber(), juce::jlimit (1, 127, (int) m.getVelocity()), tick });
            }
            else if (m.isNoteOff() || (m.isNoteOn() && m.getVelocity() == 0))
            {
                int key = m.getNoteNumber();
                for (int j = open.size(); --j >= 0;)
                {
                    if (open.getReference (j).key == key)
                    {
                        auto p = open.getReference (j);
                        open.remove (j);
                        int start = juce::jmax (0, juce::roundToInt (p.startTicks));
                        int len = juce::jmax (1, juce::roundToInt (tick - p.startTicks));

                        juce::ValueTree note (IDs::Note);
                        note.setProperty (IDs::key,      juce::jlimit (0, 127, p.key), nullptr);
                        note.setProperty (IDs::start,    start, nullptr);
                        note.setProperty (IDs::length,   len, nullptr);
                        note.setProperty (IDs::velocity, p.vel, nullptr);
                        seq.appendChild (note, nullptr);

                        maxEndTicks = juce::jmax (maxEndTicks, start + len);
                        ++imported;
                        break;
                    }
                }
            }
        }
    }

    if (imported == 0)
        return 0;

    seq.setProperty (IDs::usePianoRoll, true, nullptr);

    int neededSteps = (maxEndTicks + Model::ticksPerStep - 1) / Model::ticksPerStep;
    int curSteps = (int) pattern.getProperty (IDs::lengthSteps, Model::defaultSteps);
    if (neededSteps > curSteps)
        pattern.setProperty (IDs::lengthSteps, neededSteps, nullptr);

    return imported;
}

}
