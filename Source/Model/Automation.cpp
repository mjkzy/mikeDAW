#include "Automation.h"

namespace Automation
{

static constexpr double volumeMax = 1.25;

Target parseTarget (const juce::String& s)
{
    Target t;
    auto parts = juce::StringArray::fromTokens (s, ".", "");
    if (parts.size() != 3)
        return t;
    int idx = parts[1].getIntValue();
    if (parts[0] == "track")
    {
        if (parts[2] == "volume")   { t.kind = Target::trackVolume;   t.index = idx; }
        else if (parts[2] == "pan") { t.kind = Target::trackPan;      t.index = idx; }
    }
    else if (parts[0] == "channel")
    {
        if (parts[2] == "volume")   { t.kind = Target::channelVolume; t.index = idx; }
        else if (parts[2] == "pan") { t.kind = Target::channelPan;    t.index = idx; }
    }
    return t;
}

juce::String targetName (const juce::ValueTree& project, const juce::String& target)
{
    auto t = parseTarget (target);
    switch (t.kind)
    {
        case Target::trackVolume:
        case Target::trackPan:
        {
            auto track = Model::getMixerTrack (project, t.index);
            juce::String name = track.isValid() ? track.getProperty (IDs::name).toString()
                                                : juce::String();
            if (name.isEmpty())
                name = t.index == 0 ? juce::String ("Master") : "Insert " + juce::String (t.index);
            return name + (t.kind == Target::trackVolume ? " - Volume" : " - Pan");
        }
        case Target::channelVolume:
        case Target::channelPan:
        {
            auto ch = Model::findChannelById (project, t.index);
            juce::String name = ch.isValid() ? ch.getProperty (IDs::name).toString() : juce::String();
            if (name.isEmpty())
                name = "Channel";
            return name + (t.kind == Target::channelVolume ? " - Volume" : " - Pan");
        }
        default:
            return "Automation";
    }
}

double shape (double x, int mode, double t)
{
    x = juce::jlimit (0.0, 1.0, x);
    if (mode == 1)
        return x >= 1.0 ? 1.0 : 0.0;

    if (mode == 2)
    {
        double s = 0.5 - 0.5 * std::cos (x * juce::MathConstants<double>::pi);
        if (t > 0.0)      s = std::pow (s, 1.0 + 4.0 * t);
        else if (t < 0.0) s = 1.0 - std::pow (1.0 - s, 1.0 - 4.0 * t);
        return s;
    }
    if (t > 0.0)      return std::pow (x, 1.0 + 4.0 * t);
    if (t < 0.0)      return 1.0 - std::pow (1.0 - x, 1.0 - 4.0 * t);
    return x;
}

namespace
{
    struct Pt { double pos, val; int mode; double tension; };

    std::vector<Pt> collect (const juce::ValueTree& clip)
    {
        std::vector<Pt> pts;
        for (int i = 0; i < clip.getNumChildren(); ++i)
        {
            auto p = clip.getChild (i);
            if (! p.hasType (IDs::Point))
                continue;
            pts.push_back ({ (double) p.getProperty (IDs::pos),
                             juce::jlimit (0.0, 1.0, (double) p.getProperty (IDs::val)),
                             (int) p.getProperty (IDs::curveMode, 0),
                             (double) p.getProperty (IDs::tension, 0.0) });
        }
        std::sort (pts.begin(), pts.end(), [] (const Pt& a, const Pt& b) { return a.pos < b.pos; });
        return pts;
    }
}

double evaluate (const juce::ValueTree& clip, double posBeatsInClip)
{
    auto pts = collect (clip);
    if (pts.empty())
        return 0.0;
    if (posBeatsInClip <= pts.front().pos)
        return pts.front().val;
    if (posBeatsInClip >= pts.back().pos)
        return pts.back().val;

    for (size_t i = 1; i < pts.size(); ++i)
    {
        if (posBeatsInClip < pts[i].pos)
        {
            const auto& a = pts[i - 1];
            const auto& b = pts[i];
            double span = b.pos - a.pos;
            double x = span > 1.0e-9 ? (posBeatsInClip - a.pos) / span : 0.0;
            return a.val + (b.val - a.val) * shape (x, b.mode, b.tension);
        }
    }
    return pts.back().val;
}

float denormalize (Target t, double val)
{
    val = juce::jlimit (0.0, 1.0, val);
    switch (t.kind)
    {
        case Target::trackVolume:
        case Target::channelVolume:
            return (float) (val * volumeMax);
        case Target::trackPan:
        case Target::channelPan:
            return (float) (val * 2.0 - 1.0);
        default:
            return 0.0f;
    }
}

double normalizeCurrent (const juce::ValueTree& project, Target t)
{
    switch (t.kind)
    {
        case Target::trackVolume:
        {
            auto track = Model::getMixerTrack (project, t.index);
            double v = (double) track.getProperty (IDs::volume, 1.0);
            return juce::jlimit (0.0, 1.0, v / volumeMax);
        }
        case Target::trackPan:
        {
            auto track = Model::getMixerTrack (project, t.index);
            double v = (double) track.getProperty (IDs::pan, 0.0);
            return juce::jlimit (0.0, 1.0, (v + 1.0) * 0.5);
        }
        case Target::channelVolume:
        {
            auto ch = Model::findChannelById (project, t.index);
            double v = (double) ch.getProperty (IDs::volume, 0.8);
            return juce::jlimit (0.0, 1.0, v / volumeMax);
        }
        case Target::channelPan:
        {
            auto ch = Model::findChannelById (project, t.index);
            double v = (double) ch.getProperty (IDs::pan, 0.0);
            return juce::jlimit (0.0, 1.0, (v + 1.0) * 0.5);
        }
        default:
            return 0.0;
    }
}

double normalizedDefault (Target t)
{
    switch (t.kind)
    {
        case Target::trackVolume:    return 1.0 / volumeMax;
        case Target::channelVolume:  return 0.8 / volumeMax;
        case Target::trackPan:
        case Target::channelPan:     return 0.5;
        default:                     return 0.5;
    }
}

juce::ValueTree createClip (juce::ValueTree project, const juce::String& target)
{
    auto t = parseTarget (target);
    if (t.kind == Target::invalid)
        return {};

    auto playlist = Model::getPlaylist (project);

    int numLanes = Model::countPlaylistTracks (project);
    std::vector<bool> laneUsed ((size_t) juce::jmax (1, numLanes), false);
    double maxEnd = 0.0;
    for (int i = 0; i < playlist.getNumChildren(); ++i)
    {
        auto clip = playlist.getChild (i);
        if (! clip.hasType (IDs::Clip))
            continue;
        maxEnd = juce::jmax (maxEnd, (double) clip.getProperty (IDs::startBeats)
                                     + (double) clip.getProperty (IDs::lengthBeats));
        int lane = (int) clip.getProperty (IDs::track);
        if (juce::isPositiveAndBelow (lane, numLanes))
            laneUsed[(size_t) lane] = true;
    }

    int lane = -1;
    for (int i = 0; i < numLanes; ++i)
        if (! laneUsed[(size_t) i]) { lane = i; break; }
    if (lane < 0)
        lane = numLanes;

    double length = juce::jmax (std::ceil (maxEnd), 16.0);
    double v = normalizeCurrent (project, t);

    juce::ValueTree clip (IDs::Clip);
    clip.setProperty (IDs::uid, Model::newId(), nullptr);
    clip.setProperty (IDs::type, "automation", nullptr);
    clip.setProperty (IDs::target, target, nullptr);
    clip.setProperty (IDs::track, lane, nullptr);
    clip.setProperty (IDs::startBeats, 0.0, nullptr);
    clip.setProperty (IDs::lengthBeats, length, nullptr);

    auto makePoint = [] (double pos, double val)
    {
        juce::ValueTree p (IDs::Point);
        p.setProperty (IDs::pos, pos, nullptr);
        p.setProperty (IDs::val, val, nullptr);
        p.setProperty (IDs::curveMode, 0, nullptr);
        p.setProperty (IDs::tension, 0.0, nullptr);
        return p;
    };
    clip.appendChild (makePoint (0.0, v), nullptr);
    clip.appendChild (makePoint (length, v), nullptr);

    playlist.appendChild (clip, nullptr);
    Model::ensurePlaylistTracks (project);
    return clip;
}

}
