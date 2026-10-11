#include "BuiltinGrossBeat.h"

namespace
{
    using GB = GrossBeatProcessor;

    constexpr int kStateMagic = 0x31544247;
    constexpr int kPointBytes = 18;

    const uint8_t kDefaultExtHeader[49] = {
        0x03, 0x00, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0xe5, 0x06, 0x00, 0x00,
        0x68, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x01,
        0x01 };

    double wrapBeats (double x)
    {
        double m = std::fmod (x, GB::envBeats);
        return m < 0.0 ? m + GB::envBeats : m;
    }

    int stairLevels (float a)
    {
        return juce::jlimit (2, 1024, juce::roundToInt (16.0f * std::exp2 ((0.515f - a) / 0.09f)));
    }

    int pulseCount (float a)
    {
        static const float ts[] = { 0.39f, 0.455f, 0.51f, 0.54f };
        static const float ps[] = { 32.0f, 24.0f, 16.0f, 12.0f };
        float p;
        if (a <= ts[0])      p = ps[0] * std::exp2 ((ts[0] - a) / 0.09f);
        else if (a >= ts[3]) p = ps[3] * std::exp2 ((ts[3] - a) / 0.09f);
        else
        {
            int k = 0;
            while (a > ts[k + 1]) ++k;
            const float f = (a - ts[k]) / (ts[k + 1] - ts[k]);
            p = ps[k] + (ps[k + 1] - ps[k]) * f;
        }
        return juce::jlimit (1, 1024, juce::roundToInt (p));
    }

    float waveCycles (float a)
    {
        const float c = 16.0f * std::exp2 ((0.515f - a) / 0.09f);
        return juce::jmin (1024.0f, std::floor (c)) + 0.5f;
    }

    float singleShape (float v, float t)
    {
        if (t == 0.0f) return v;
        const float p = std::exp2 (std::abs (t) * 6.67f);
        return t < 0.0f ? std::pow (v, p) : 1.0f - std::pow (1.0f - v, p);
    }

    float expoShape (float v, float t)
    {
        const float k = t * 12.0f;
        if (std::abs (k) < 1.0e-3f) return v;
        return (1.0f - std::exp (-k * v)) / (1.0f - std::exp (-k));
    }

    float offsetY (double beatsBack) { return (float) (1.0 - beatsBack / GB::timeRangeBeats); }

    struct SlotBuilder
    {
        GB::Slot s;

        explicit SlotBuilder (const char* name, bool empty = false)
        {
            s.name = name;
            s.extFlags = { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, (uint8_t) (empty ? 0 : 1), 0 };
        }

        SlotBuilder& p (double x, float y, int curve = GB::hold, float t = 0.0f)
        {
            s.points.push_back ({ juce::jlimit (0.0, GB::envBeats, x), juce::jlimit (0.0f, 1.0f, y), t, curve,
                                  GB::auxForTension (t) });
            return *this;
        }

        GB::Slot get()
        {
            if (s.points.empty()) p (0.0, 1.0f);
            if (s.points.size() < 2) p (GB::envBeats, s.points.back().y);
            return s;
        }
    };

    GB::Slot repeatSlot (const char* name, double len)
    {
        SlotBuilder b (name);
        b.p (0.0, 1.0f);
        const int count = (int) std::ceil (GB::envBeats / len - 1.0e-6);
        for (int k = 1; k < count; ++k)
            b.p (k * len, offsetY (k * len));
        return b.p (GB::envBeats, offsetY ((count - 1) * len)).get();
    }

    GB::Slot beatStutter (const char* name, const std::array<double, 4>& lens)
    {
        SlotBuilder b (name);
        b.p (0.0, 1.0f);
        for (int beat = 0; beat < 4; ++beat)
        {
            const int count = (int) std::ceil (1.0 / lens[(size_t) beat] - 1.0e-6);
            for (int k = 0; k < count; ++k)
                if (beat + k > 0)
                    b.p (beat + k * lens[(size_t) beat], offsetY (k * lens[(size_t) beat]));
        }
        return b.p (GB::envBeats, b.s.points.back().y).get();
    }

    GB::Slot pitchSteps (const char* name, float semitonesPerStep)
    {
        SlotBuilder b (name);
        b.p (0.0, 1.0f);
        for (int k = 0; k < 16; ++k)
        {
            const double c = k * 0.25;
            const double speed = std::exp2 (semitonesPerStep * k / 12.0);
            if (k > 0) b.p (c, offsetY (c));
            b.p (c + 0.25, offsetY (c + (1.0 - speed) * 0.25), GB::singleCurve);
        }
        return b.get();
    }

    GB::Slot swingRepeat (const char* name)
    {
        SlotBuilder b (name);
        b.p (0.0, 1.0f);
        for (int beat = 0; beat < 4; ++beat)
            for (double o : { 0.0, 0.3125, 0.5, 0.8125 })
                if (beat + o > 0.0)
                    b.p (beat + o, offsetY (o - std::floor (o * 2.0) * 0.5));
        return b.p (GB::envBeats, b.s.points.back().y).get();
    }

    GB::Slot scratchSlot (const char* name, double len)
    {
        SlotBuilder b (name);
        b.p (0.0, 1.0f);
        float y = 1.0f;
        const int count = juce::roundToInt (GB::envBeats / len);
        for (int k = 0; k < count; ++k)
        {
            const double c = k * len;
            b.p (c + len * 0.25, y);
            y = juce::jmax (0.0f, y - (float) (len * 0.5 / GB::timeRangeBeats));
            b.p (c + len, y, GB::wave, -1.0f);
        }
        return b.get();
    }

    GB::Slot slowChunks (const char* name, double len, double speed)
    {
        SlotBuilder b (name);
        b.p (0.0, 1.0f);
        const int count = juce::roundToInt (GB::envBeats / len);
        for (int k = 0; k < count; ++k)
        {
            b.p ((k + 1) * len, offsetY ((1.0 - speed) * len), GB::singleCurve);
            if (k + 1 < count) b.p ((k + 1) * len, 1.0f);
        }
        return b.get();
    }

    GB::Slot gateSlot (const char* name, double period, double duty = 0.5)
    {
        SlotBuilder b (name);
        b.p (0.0, 1.0f);
        const int count = juce::roundToInt (GB::envBeats / period);
        for (int k = 0; k < count; ++k)
        {
            b.p (k * period + period * duty, 0.0f);
            b.p ((k + 1) * period, 1.0f);
        }
        return b.get();
    }

    GB::Slot beatGates (const char* name, const std::array<double, 4>& periods)
    {
        SlotBuilder b (name);
        b.p (0.0, 1.0f);
        for (int beat = 0; beat < 4; ++beat)
        {
            const double per = periods[(size_t) beat];
            const int count = juce::roundToInt (1.0 / per);
            for (int k = 0; k < count; ++k)
            {
                b.p (beat + k * per + per * 0.5, 0.0f);
                b.p (beat + (k + 1) * per, 1.0f);
            }
        }
        return b.get();
    }

    float stepLevel (char c)
    {
        if (c == 'x') return 1.0f;
        if (c >= '0' && c <= '9') return (float) (c - '0') / 9.0f;
        return 0.0f;
    }

    GB::Slot stepSlot (const char* name, const char* pattern)
    {
        SlotBuilder b (name);
        const int len = (int) std::strlen (pattern);
        const double step = GB::envBeats / len;
        b.p (0.0, stepLevel (pattern[0]));
        for (int j = 1; j < len; ++j)
            if (stepLevel (pattern[j]) != stepLevel (pattern[j - 1]))
                b.p (j * step, stepLevel (pattern[j]));
        return b.p (GB::envBeats, stepLevel (pattern[len - 1])).get();
    }

    GB::Slot sawSlot (const char* name, float accent)
    {
        SlotBuilder b (name);
        b.p (0.0, 1.0f);
        for (int k = 0; k < 16; ++k)
        {
            b.p ((k + 1) * 0.25, 0.0f, GB::singleCurve);
            if (k < 15) b.p ((k + 1) * 0.25, (k % 2 == 0) ? accent : 1.0f);
        }
        return b.get();
    }

    GB::Slot levelGates (const char* name, bool rising)
    {
        SlotBuilder b (name);
        for (int k = 0; k < 16; ++k)
        {
            const float lvl = rising ? (k + 1) / 16.0f : 1.0f - k / 16.0f;
            b.p (k * 0.25, lvl);
            b.p (k * 0.25 + 0.125, 0.0f);
        }
        return b.p (GB::envBeats, 0.0f).get();
    }

    GB::Slot waveGates (const char* name, double half, float low)
    {
        SlotBuilder b (name);
        b.p (0.0, 1.0f);
        const int count = juce::roundToInt (GB::envBeats / half);
        for (int k = 0; k < count; ++k)
            b.p ((k + 1) * half, (k % 2 == 0) ? low : 1.0f, GB::wave, -1.0f);
        return b.get();
    }

    GB::Slot timeFactory (int i)
    {
        switch (i)
        {
            case 0:  return SlotBuilder ("Empty", true).p (0.0, 1.0f).p (4.0, 1.0f, GB::singleCurve).get();
            case 1:  return repeatSlot ("2 Beat Rpt", 2.0);
            case 2:  return repeatSlot ("1.5 Beat Rpt", 1.5);
            case 3:  return repeatSlot ("1 Beat Rpt", 1.0);
            case 4:  return repeatSlot ("3/4 Repeat", 0.75);
            case 5:  return repeatSlot ("1/2 Repeat", 0.5);
            case 6:  return repeatSlot ("1/3 Repeat", 1.0 / 3.0);
            case 7:  return repeatSlot ("1/4 Repeat", 0.25);
            case 8:  return repeatSlot ("1/6 Repeat", 1.0 / 6.0);
            case 9:  return repeatSlot ("1/8 Repeat", 0.125);
            case 10: return repeatSlot ("1/12 Repeat", 1.0 / 12.0);
            case 11: return repeatSlot ("1/16 Repeat", 0.0625);
            case 12: return SlotBuilder ("1.5 Bt Juggle").p (0.0, offsetY (1.5)).p (4.0, offsetY (1.5), GB::singleCurve).get();
            case 13: return SlotBuilder ("1 Beat Juggle").p (0.0, offsetY (1.0)).p (4.0, offsetY (1.0), GB::singleCurve).get();
            case 14: return beatStutter ("Slw Dwn Rpt", { 0.0625, 0.125, 0.25, 0.5 });
            case 15: return beatStutter ("Spd Up Rpt", { 0.5, 0.25, 0.125, 0.0625 });
            case 16: return pitchSteps ("1/4 Pitch Dn", -1.0f);
            case 17: return pitchSteps ("1/4 Pitch Up", 1.0f);
            case 18: return swingRepeat ("1/4 Rpt Swng");
            case 19:
            {
                SlotBuilder b ("1/4 Jumps");
                b.p (0.0, 1.0f);
                for (int k = 1; k < 16; ++k) b.p (k * 0.25, 1.0f - k / 16.0f);
                return b.p (4.0, 1.0f - 15.0f / 16.0f).get();
            }
            case 20: return SlotBuilder ("Reverse").p (0.0, 1.0f).p (4.0, 0.0f, GB::singleCurve).get();
            case 21: return SlotBuilder ("Backspin").p (0.0, 1.0f).p (4.0, 0.0f, GB::halfSine, 0.5f).get();
            case 22: return SlotBuilder ("Backspinstart").p (0.0, 1.0f).p (4.0, 0.5f, GB::halfSine, 1.0f).get();
            case 23: return SlotBuilder ("Vinyl Off").p (0.0, 1.0f).p (4.0, offsetY (2.0), GB::singleCurve, -0.15f).get();
            case 24: return SlotBuilder ("Vinyl On").p (0.0, 1.0f).p (4.0, offsetY (2.0), GB::singleCurve, 0.15f).get();
            case 25: return SlotBuilder ("Vinyl Push").p (0.0, offsetY (2.0)).p (4.0, 1.0f, GB::singleCurve, 0.15f).get();
            case 26: return SlotBuilder ("Vinyl Stop").p (0.0, 1.0f).p (1.0, 1.0f).p (4.0, offsetY (1.5), GB::singleCurve, -0.15f).get();
            case 27: return scratchSlot ("Bar Scratch", 1.0);
            case 28: return scratchSlot ("Scratch", 0.5);
            case 29: return scratchSlot ("Trip Scratch", 1.0 / 3.0);
            case 30: return SlotBuilder ("1/2 Speed").p (0.0, 1.0f).p (4.0, offsetY (2.0), GB::singleCurve).get();
            case 31: return SlotBuilder ("X2 Speed").p (0.0, offsetY (4.0)).p (4.0, 1.0f, GB::singleCurve).get();
            case 32: return SlotBuilder ("1/2 Grains").p (0.0, 1.0f).p (4.0, offsetY (2.0), GB::stairs, 0.425f).get();
            case 33: return slowChunks ("Slow Tripplet", 2.0 / 3.0, 0.75);
            case 34: return SlotBuilder ("Underwater").p (0.0, 1.0f).p (4.0, 0.99f, GB::wave, 0.4f).get();
            case 35: return SlotBuilder ("Digital").p (0.0, 1.0f).p (4.0, 0.984f, GB::smoothStairs, 0.2f).get();
            default: break;
        }
        return SlotBuilder ("Empty", true).p (0.0, 1.0f).p (4.0, 1.0f, GB::singleCurve).get();
    }

    GB::Slot volumeFactory (int i)
    {
        switch (i)
        {
            case 0:  return SlotBuilder ("Empty", true).p (0.0, 1.0f).p (4.0, 1.0f).get();
            case 1:  return gateSlot ("2 Beat Gate", 2.0);
            case 2:  return gateSlot ("1 Beat Gate", 1.0);
            case 3:  return gateSlot ("1/2 Bt Gate", 0.5);
            case 4:  return gateSlot ("1/3 Bt Gate", 1.0 / 3.0);
            case 5:  return gateSlot ("1/4 Bt Gate", 0.25);
            case 6:  return gateSlot ("1/6 Bt Gate", 1.0 / 6.0);
            case 7:  return gateSlot ("1/8 Bt Gate", 0.125);
            case 8:  return stepSlot ("Trance Gt 1", "x-x-x---x-x-x---x-x-x---x-x-x-x-");
            case 9:  return stepSlot ("Trance Gt 2", "x-x---x-x-x---x-x-x---x-x-x-x-x-");
            case 10: return stepSlot ("Trance Gt 3", "x---x-x-x---x-x-x---x-xxx---x-x-");
            case 11: return stepSlot ("Trance Gt 4", "9-5-7-9-5-7-9-5-9-5-7-9-5-7-9-7-");
            case 12: return sawSlot ("Saw Gate 1", 1.0f);
            case 13: return sawSlot ("Saw Gate 2", 0.8f);
            case 14: return SlotBuilder ("1st Step").p (0.0, 1.0f).p (0.25, 0.0f).p (4.0, 0.0f).get();
            case 15: return SlotBuilder ("1st Stp Fade").p (0.0, 1.0f).p (0.25, 0.0f).p (4.0, 1.0f, GB::singleCurve, -0.4f).get();
            case 16: return stepSlot ("2 Step Gate", "x---x-----x-----");
            case 17: return stepSlot ("Off Beat", "-x-x-x-x");
            case 18: return stepSlot ("1/4 Dynamic", "9-4-7-4-9-4-7-4-9-4-7-4-9-4-7-4-");
            case 19: return stepSlot ("1/4 Swing", "xx---xx-xx---xx-xx---xx-xx---xx-xx---xx-xx---xx-xx---xx-xx---xx-");
            case 20: return levelGates ("1/4 Gate Out", false);
            case 21: return levelGates ("1/4 Gate In", true);
            case 22: return beatGates ("Spd Up Gate", { 0.5, 1.0 / 3.0, 0.25, 0.125 });
            case 23: return beatGates ("Slw Dn Gate", { 0.125, 0.25, 1.0 / 3.0, 0.5 });
            case 24:
            {
                SlotBuilder b ("Gate Fade");
                b.p (0.0, 1.0f);
                for (int beat = 0; beat < 4; ++beat)
                {
                    b.p (beat + 0.25, 0.0f);
                    b.p (beat + 1.0, 1.0f, GB::singleCurve, -0.6f);
                }
                return b.get();
            }
            case 25: return stepSlot ("Gate Beat", "x-x-x-xxx-x-xx-xx-x-x-xxx-x-x-x-");
            case 26: return waveGates ("Tremelo Slw", 0.5, 0.25f);
            case 27: return waveGates ("Tremelo Fst", 0.125, 0.25f);
            case 28:
            {
                SlotBuilder b ("Sidechain");
                b.p (0.0, 0.0625f);
                for (int beat = 0; beat < 4; ++beat)
                {
                    b.p (beat + 0.5, 1.0f, GB::singleCurve, -0.37f);
                    b.p (beat + 1.0, 1.0f);
                    if (beat < 3) b.p (beat + 1.0, 0.0625f);
                }
                return b.get();
            }
            case 29: return stepSlot ("Drum Loop", "9-5-2-5-9-5-7-2-9-5-2-5-9-2-7-5-");
            case 30: return gateSlot ("Copter", 0.125, 0.3);
            case 31: return SlotBuilder ("AM").p (0.0, 1.0f).p (4.0, 0.0f, GB::wave, 0.25f).get();
            case 32: return SlotBuilder ("Fade In").p (0.0, 0.0f).p (4.0, 1.0f, GB::singleCurve).get();
            case 33: return SlotBuilder ("Fade Out").p (0.0, 1.0f).p (4.0, 0.0f, GB::singleCurve).get();
            case 34: return SlotBuilder ("Fade In Out").p (0.0, 1.0f).p (2.0, 0.0f, GB::singleCurve, 0.285f)
                                                    .p (4.0, 1.0f, GB::singleCurve, -0.285f).get();
            case 35: return SlotBuilder ("Mute").p (0.0, 0.0f).p (4.0, 0.0f).get();
            default: break;
        }
        return SlotBuilder ("Empty", true).p (0.0, 1.0f).p (4.0, 1.0f).get();
    }

    void sanitize (GB::Slot& s)
    {
        if ((int) s.points.size() > GB::maxPoints)
            s.points.resize ((size_t) GB::maxPoints);
        double prev = 0.0;
        for (auto& p : s.points)
        {
            p.x = juce::jlimit (prev, GB::envBeats, p.x);
            p.y = juce::jlimit (0.0f, 1.0f, p.y);
            p.tension = juce::jlimit (-1.0f, 1.0f, p.tension);
            p.curve = juce::jlimit (0, GB::numCurves - 1, p.curve);
            prev = p.x;
        }
        if (s.points.empty())
            s.points.push_back ({});
        if (s.points.size() < 2)
            s.points.push_back ({ GB::envBeats, s.points.front().y, 0.0f, GB::singleCurve, 2 });
    }
}

float GrossBeatProcessor::shape (int curve, float t, float u)
{
    u = juce::jlimit (0.0f, 1.0f, u);
    const float a = std::abs (t);
    switch (curve)
    {
        case hold:        return u < 1.0f ? 0.0f : 1.0f;
        case doubleCurve: return u < 0.5f ? 0.5f * singleShape (2.0f * u, -t) : 0.5f + 0.5f * singleShape (2.0f * u - 1.0f, t);
        case altSingle:   return expoShape (u, t);
        case altDouble:   return u < 0.5f ? 0.5f * expoShape (2.0f * u, -t) : 0.5f + 0.5f * expoShape (2.0f * u - 1.0f, t);
        case stairs:
        {
            const int n = stairLevels (a);
            return (float) juce::jmin (n - 1, (int) (u * (float) n)) / (float) (n - 1);
        }
        case smoothStairs:
        {
            const int n = stairLevels (a);
            const float v = u * (float) (n - 1);
            const float k = std::floor (v), f = v - k;
            return (k + f * f * (3.0f - 2.0f * f)) / (float) (n - 1);
        }
        case pulse:
        {
            if (u >= 1.0f) return 1.0f;
            const float ph = u * (float) pulseCount (a);
            return ph - std::floor (ph) < 0.5f ? 0.0f : 1.0f;
        }
        case wave:
            return 0.5f * (1.0f - std::cos (juce::MathConstants<float>::twoPi * waveCycles (a) * u));
        case halfSine:
            if (t > 0.0f) return std::sin (juce::MathConstants<float>::halfPi * u);
            if (t < 0.0f) return 1.0f - std::cos (juce::MathConstants<float>::halfPi * u);
            return 0.5f * (1.0f - std::cos (juce::MathConstants<float>::pi * u));
        default:
            return singleShape (u, t);
    }
}

float GrossBeatProcessor::evaluate (const std::vector<Point>& pts, double x)
{
    const int n = (int) pts.size();
    if (n == 0) return 1.0f;
    if (x < pts[0].x) return pts[0].y;
    int i = 1;
    while (i < n && pts[(size_t) i].x <= x) ++i;
    if (i >= n) return pts[(size_t) n - 1].y;
    const auto& a = pts[(size_t) i - 1];
    const auto& b = pts[(size_t) i];
    const float u = (float) ((x - a.x) / (b.x - a.x));
    return a.y + (b.y - a.y) * shape (b.curve, b.tension, u);
}

float GrossBeatProcessor::Cursor::eval (const std::vector<Point>& pts, double x)
{
    const int n = (int) pts.size();
    if (n == 0) return 1.0f;
    if (x < pts[0].x) return pts[0].y;
    if (seg < 1 || seg > n || pts[(size_t) seg - 1].x > x) seg = 1;
    while (seg < n && pts[(size_t) seg].x <= x) ++seg;
    if (seg >= n) return pts[(size_t) n - 1].y;
    const auto& a = pts[(size_t) seg - 1];
    const auto& b = pts[(size_t) seg];
    const float u = (float) ((x - a.x) / (b.x - a.x));
    return a.y + (b.y - a.y) * shape (b.curve, b.tension, u);
}

GrossBeatProcessor::Slot GrossBeatProcessor::factorySlot (int index)
{
    return index < numSlots ? timeFactory (index) : volumeFactory (index - numSlots);
}

GrossBeatProcessor::GrossBeatProcessor()
{
    const juce::StringArray syncChoices { "Off", "1/4 beat", "1/2 beat", "1 beat", "2 beats", "4 beats" };

    addParameter (timeMix = new juce::AudioParameterFloat (juce::ParameterID { "time", 1 }, "Time",
                                                          juce::NormalisableRange<float> (0.0f, 1.0f), 1.0f));
    addParameter (volMix = new juce::AudioParameterFloat (juce::ParameterID { "volume", 1 }, "Volume",
                                                         juce::NormalisableRange<float> (0.0f, 1.0f), 1.0f));
    addParameter (timeSlot = new juce::AudioParameterInt (juce::ParameterID { "timeSlot", 1 }, "Time Slot", 1, numSlots, 1));
    addParameter (volSlot = new juce::AudioParameterInt (juce::ParameterID { "volSlot", 1 }, "Volume Slot", 1, numSlots, 1));
    addParameter (attack = new juce::AudioParameterFloat (juce::ParameterID { "attack", 1 }, "Attack",
                                                         juce::NormalisableRange<float> (0.0f, 500.0f, 0.0f, 1.0f / 3.0f), 5.0f,
                                                         juce::AudioParameterFloatAttributes().withLabel ("ms")));
    addParameter (release = new juce::AudioParameterFloat (juce::ParameterID { "release", 1 }, "Release",
                                                          juce::NormalisableRange<float> (0.0f, 1000.0f, 0.0f, 1.0f / 3.0f), 8.0f,
                                                          juce::AudioParameterFloatAttributes().withLabel ("ms")));
    addParameter (tension = new juce::AudioParameterFloat (juce::ParameterID { "tension", 1 }, "Tension",
                                                          juce::NormalisableRange<float> (-1.0f, 1.0f), 0.0f));
    addParameter (clickReduce = new juce::AudioParameterBool (juce::ParameterID { "clickReduction", 1 }, "Click Reduction", true));
    addParameter (trigSync = new juce::AudioParameterChoice (juce::ParameterID { "trigSync", 1 }, "Trig Sync", syncChoices, 1));
    addParameter (posSync = new juce::AudioParameterChoice (juce::ParameterID { "posSync", 1 }, "Pos Sync", syncChoices, 0));
    addParameter (linkSlots = new juce::AudioParameterBool (juce::ParameterID { "link", 1 }, "Link Slots", false));
    addParameter (holdMode = new juce::AudioParameterChoice (juce::ParameterID { "holdMode", 1 }, "Hold Mode",
                                                            juce::StringArray { "Off", "Last", "Next" }, 1));
    addParameter (holdOn = new juce::AudioParameterBool (juce::ParameterID { "hold", 1 }, "Hold", false));

    for (int i = 0; i < numSlots * 2; ++i)
        slots[(size_t) i] = factorySlot (i);
    extHeader.assign (std::begin (kDefaultExtHeader), std::end (kDefaultExtHeader));

    for (auto* v : { &liveTime, &liveVol, &livePendTime, &livePendVol })
        v->reserve ((size_t) maxPoints);
}

void GrossBeatProcessor::prepareToPlay (double sampleRate, int)
{
    sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    const int need = (int) std::ceil (sr * 12.0) + 16;
    int size = 1;
    while (size < need) size <<= 1;
    ring.setSize (2, size);
    ringMask = size - 1;
    holdBuf.setSize (2, (int) std::ceil (sr * 6.0) + 16);
    fadeLen = juce::jmax (1, (int) (sr * 0.004));
    reset();
}

void GrossBeatProcessor::reset()
{
    ring.clear();
    holdBuf.clear();
    written = recorded = 0;
    wasPlaying = false;
    havePrevRead = false;
    prevStep = 1.0;
    fadeRem = 0;
    gainState = 1.0f;
    holdState = holdLen = holdPhase = holdRecPos = 0;
    holdMix = 0.0f;
    pendTime = pendVol = -1;
}

double GrossBeatProcessor::syncBeats (int choiceIndex)
{
    static const double beats[] = { 0.0, 0.25, 0.5, 1.0, 2.0, 4.0 };
    return beats[juce::jlimit (0, 5, choiceIndex)];
}

void GrossBeatProcessor::resync (double songStart)
{
    songPos = songStart;
    timeStart = volStart = std::floor (songStart / envBeats) * envBeats;
    activeTime = timeSlot->get() - 1;
    activeVol = volSlot->get() - 1;
    pendTime = pendVol = -1;
    curTime.seg = curVol.seg = 1;
}

bool GrossBeatProcessor::refreshLive (int wantTime, int wantVol, int wantPendTime, int wantPendVol)
{
    const auto version = slotsVersion.load();
    if (version == liveVersion && wantTime == liveTimeIdx && wantVol == liveVolIdx
        && wantPendTime == livePendTimeIdx && wantPendVol == livePendVolIdx)
        return true;

    juce::SpinLock::ScopedTryLockType lock (slotLock);
    if (! lock.isLocked())
        return false;

    auto copyInto = [this] (std::vector<Point>& dst, int& dstIdx, int idx)
    {
        dstIdx = idx;
        if (idx < 0) { dst.clear(); return; }
        const auto& src = slots[(size_t) idx].points;
        dst.assign (src.begin(), src.begin() + (std::ptrdiff_t) juce::jmin ((int) src.size(), maxPoints));
    };
    copyInto (liveTime, liveTimeIdx, wantTime);
    copyInto (liveVol, liveVolIdx, wantVol);
    copyInto (livePendTime, livePendTimeIdx, wantPendTime);
    copyInto (livePendVol, livePendVolIdx, wantPendVol);
    liveVersion = version;
    curTime.seg = curVol.seg = 1;
    return true;
}

float GrossBeatProcessor::readRing (int ch, double pos) const
{
    const double fl = std::floor (pos);
    const int64_t i1 = (int64_t) fl;
    const float f = (float) (pos - fl);
    const float* d = ring.getReadPointer (ch);
    auto at = [&] (int64_t k) { return d[(int) ((k > written ? written : k) & (int64_t) ringMask)]; };
    const float y0 = at (i1 - 1), y1 = at (i1), y2 = at (i1 + 1), y3 = at (i1 + 2);
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * f + c2) * f + c1) * f + y1;
}

void GrossBeatProcessor::engageHold (int mode, double spb)
{
    const int len = juce::jlimit (16, holdBuf.getNumSamples(), (int) std::round (envBeats * spb));
    holdLen = len;
    holdPhase = 0;
    if (mode == 1 && recorded >= len)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            const float* src = ring.getReadPointer (ch);
            float* dst = holdBuf.getWritePointer (ch);
            for (int k = 0; k < len; ++k)
                dst[k] = src[(int) ((written - len + k) & (int64_t) ringMask)];
        }
        holdState = 2;
    }
    else
    {
        holdRecPos = 0;
        holdState = 1;
    }
}

void GrossBeatProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    if (n <= 0 || buffer.getNumChannels() <= 0 || ringMask == 0)
        return;
    float* outL = buffer.getWritePointer (0);
    float* outR = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;

    double bpm = 120.0, ppqEnd = 0.0;
    bool playing = false;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            if (auto b = pos->getBpm()) bpm = juce::jlimit (20.0, 999.0, *b);
            if (auto p = pos->getPpqPosition()) ppqEnd = *p;
            playing = pos->getIsPlaying();
        }

    const double spb = sr * 60.0 / bpm;
    const double bps = 1.0 / spb;
    const double blockBeats = n * bps;

    if (playing)
    {
        const double start = ppqEnd - blockBeats;
        if (! wasPlaying)
        {
            recorded = 0;
            havePrevRead = false;
            resync (start);
        }
        else if (std::abs (ppqEnd - (lastPpqEnd + blockBeats)) > juce::jmax (0.01, 0.5 * blockBeats))
        {
            resync (start);
        }
    }
    wasPlaying = playing;
    lastPpqEnd = ppqEnd;

    const int reqTime = timeSlot->get() - 1, reqVol = volSlot->get() - 1;
    auto triggerAt = [this] (double pos)
    {
        const double q = syncBeats (trigSync->getIndex());
        return q <= 0.0 ? pos : std::ceil (pos / q - 1.0e-7) * q;
    };
    auto startFor = [this] (double at)
    {
        const double ps = syncBeats (posSync->getIndex());
        if (ps <= 0.0) return at;
        double m = std::fmod (at, ps);
        if (m < 0.0) m += ps;
        return at - m;
    };

    if (! playing)
    {
        activeTime = reqTime;
        activeVol = reqVol;
        pendTime = pendVol = -1;
    }
    else
    {
        if (reqTime == activeTime) pendTime = -1;
        else if (reqTime != pendTime) { pendTime = reqTime; pendTimeAt = triggerAt (songPos); }
        if (reqVol == activeVol) pendVol = -1;
        else if (reqVol != pendVol) { pendVol = reqVol; pendVolAt = triggerAt (songPos); }
    }

    refreshLive (activeTime, numSlots + activeVol, pendTime >= 0 ? pendTime : -1,
                 pendVol >= 0 ? numSlots + pendVol : -1);

    const float tMix = timeMix->get(), vMix = volMix->get();
    const float tns = tension->get();
    const float tnsExp = std::pow (4.0f, -tns);
    const float attMs = attack->get(), relMs = release->get();
    const float attCoef = attMs < 0.01f ? 0.0f : std::exp (-1.0f / (attMs * 0.001f * (float) sr));
    const float relCoef = relMs < 0.01f ? 0.0f : std::exp (-1.0f / (relMs * 0.001f * (float) sr));
    const bool clickRed = clickReduce->get();
    const double maxDelay = (double) (ringMask - 8);

    const int holdModeIdx = holdMode->getIndex();
    const bool wantHold = holdOn->get() && playing && holdModeIdx != 0;
    if (wantHold && holdState == 0)      engageHold (holdModeIdx, spb);
    else if (! wantHold && holdState != 0) holdState = 0;
    const float holdStep = 1.0f / (float) fadeLen;

    float* rL = ring.getWritePointer (0);
    float* rR = ring.getWritePointer (1);

    for (int i = 0; i < n; ++i)
    {
        float yT = 1.0f, yV = 1.0f;
        if (playing)
        {
            if (pendTime >= 0 && songPos >= pendTimeAt)
            {
                activeTime = pendTime;
                pendTime = -1;
                timeStart = startFor (pendTimeAt);
                std::swap (liveTime, livePendTime);
                std::swap (liveTimeIdx, livePendTimeIdx);
                curTime.seg = 1;
            }
            if (pendVol >= 0 && songPos >= pendVolAt)
            {
                activeVol = pendVol;
                pendVol = -1;
                volStart = startFor (pendVolAt);
                std::swap (liveVol, livePendVol);
                std::swap (liveVolIdx, livePendVolIdx);
                curVol.seg = 1;
            }
            yT = curTime.eval (liveTime, wrapBeats (songPos - timeStart));
            yV = curVol.eval (liveVol, wrapBeats (songPos - volStart));
        }

        float inL = outL[i];
        float inR = outR != nullptr ? outR[i] : inL;

        if (holdState == 1)
        {
            holdBuf.setSample (0, holdRecPos, inL);
            holdBuf.setSample (1, holdRecPos, inR);
            if (++holdRecPos >= holdLen) { holdState = 2; holdPhase = 0; }
        }
        const float holdTarget = holdState == 2 ? 1.0f : 0.0f;
        if (holdMix < holdTarget)      holdMix = juce::jmin (holdTarget, holdMix + holdStep);
        else if (holdMix > holdTarget) holdMix = juce::jmax (holdTarget, holdMix - holdStep);
        if (holdMix > 0.0f && holdLen > 0)
        {
            inL += (holdBuf.getSample (0, holdPhase) - inL) * holdMix;
            inR += (holdBuf.getSample (1, holdPhase) - inR) * holdMix;
            if (++holdPhase >= holdLen) holdPhase = 0;
        }

        const int w = (int) (written & (int64_t) ringMask);
        rL[w] = inL;
        rR[w] = inR;

        const double d = playing ? juce::jlimit (0.0, maxDelay, (1.0 - yT) * timeRangeBeats * tMix * spb) : 0.0;
        const double readPos = (double) written - d;
        const double avail = (double) juce::jmin<int64_t> (recorded, (int64_t) ringMask - 8);

        float sL = 0.0f, sR = 0.0f;
        if (d <= avail)
        {
            sL = readRing (0, readPos);
            sR = readRing (1, readPos);
        }

        bool jumped = false;
        if (clickRed && havePrevRead)
        {
            const double expected = prevRead + prevStep;
            if (std::abs (readPos - expected) > 2.0)
            {
                fadeOldPos = expected;
                fadeOldStep = juce::jlimit (-4.0, 4.0, prevStep);
                fadeRem = fadeLen;
                jumped = true;
            }
        }
        if (! jumped)
            prevStep = havePrevRead ? readPos - prevRead : 1.0;
        prevRead = readPos;
        havePrevRead = true;

        if (fadeRem > 0)
        {
            const double op = juce::jmin (fadeOldPos, (double) written);
            float oL = 0.0f, oR = 0.0f;
            if ((double) written - op <= avail)
            {
                oL = readRing (0, op);
                oR = readRing (1, op);
            }
            const float wOld = (float) fadeRem / (float) fadeLen;
            sL += (oL - sL) * wOld;
            sR += (oR - sR) * wOld;
            fadeOldPos += fadeOldStep;
            --fadeRem;
        }

        float g = yV;
        if (tns != 0.0f) g = std::pow (juce::jmax (0.0f, g), tnsExp);
        g = 1.0f - vMix * (1.0f - g);
        gainState = g + (gainState - g) * (g > gainState ? attCoef : relCoef);

        outL[i] = sL * gainState;
        if (outR != nullptr) outR[i] = sR * gainState;

        ++written;
        ++recorded;
        if (playing) songPos += bps;
    }

    uiActiveTime = activeTime;
    uiActiveVol = activeVol;
    uiPendingTime = pendTime;
    uiPendingVol = pendVol;
    uiTimePos = playing ? wrapBeats (songPos - timeStart) : -1.0;
    uiVolPos = playing ? wrapBeats (songPos - volStart) : -1.0;
}

GrossBeatProcessor::Slot GrossBeatProcessor::getSlot (int index) const
{
    const juce::SpinLock::ScopedLockType lock (slotLock);
    return slots[(size_t) juce::jlimit (0, numSlots * 2 - 1, index)];
}

void GrossBeatProcessor::setSlot (int index, Slot s)
{
    sanitize (s);
    {
        const juce::SpinLock::ScopedLockType lock (slotLock);
        std::swap (slots[(size_t) juce::jlimit (0, numSlots * 2 - 1, index)], s);
    }
    ++slotsVersion;
}

void GrossBeatProcessor::swapSlots (int a, int b)
{
    if (a == b) return;
    {
        const juce::SpinLock::ScopedLockType lock (slotLock);
        std::swap (slots[(size_t) juce::jlimit (0, numSlots * 2 - 1, a)],
                   slots[(size_t) juce::jlimit (0, numSlots * 2 - 1, b)]);
    }
    ++slotsVersion;
}

void GrossBeatProcessor::resetAllSlots()
{
    std::array<Slot, numSlots * 2> fresh;
    for (int i = 0; i < numSlots * 2; ++i)
        fresh[(size_t) i] = factorySlot (i);
    {
        const juce::SpinLock::ScopedLockType lock (slotLock);
        std::swap (slots, fresh);
    }
    ++slotsVersion;
}

void GrossBeatProcessor::notifySlotsEdited()
{
    updateHostDisplay (ChangeDetails().withNonParameterStateChanged (true));
}

std::vector<uint8_t> GrossBeatProcessor::getExtHeader() const
{
    const juce::SpinLock::ScopedLockType lock (slotLock);
    return extHeader;
}

void GrossBeatProcessor::setExtHeader (std::vector<uint8_t> header)
{
    if (header.size() != sizeof (kDefaultExtHeader))
        return;
    const juce::SpinLock::ScopedLockType lock (slotLock);
    std::swap (extHeader, header);
}

void GrossBeatProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    juce::MemoryOutputStream out (dest, false);
    out.writeInt (kStateMagic);
    const auto& params = getParameters();
    out.writeInt (params.size());
    for (auto* p : params)
        out.writeFloat (p->getValue());

    std::array<Slot, numSlots * 2> copy;
    std::vector<uint8_t> header;
    {
        const juce::SpinLock::ScopedLockType lock (slotLock);
        copy = slots;
        header = extHeader;
    }

    juce::ValueTree tree ("GrossBeat");
    tree.setProperty ("extHeader", juce::MemoryBlock (header.data(), header.size()), nullptr);
    for (const auto& s : copy)
    {
        juce::ValueTree node ("Slot");
        node.setProperty ("name", s.name, nullptr);
        node.setProperty ("flags", juce::MemoryBlock (s.extFlags.data(), s.extFlags.size()), nullptr);
        node.setProperty ("a", (int) s.extA, nullptr);
        node.setProperty ("b", (int) s.extB, nullptr);
        node.setProperty ("trailer", (int) s.extTrailer, nullptr);
        juce::MemoryOutputStream pts;
        for (const auto& p : s.points)
        {
            pts.writeDouble (p.x);
            pts.writeFloat (p.y);
            pts.writeFloat (p.tension);
            pts.writeByte ((char) p.curve);
            pts.writeByte ((char) (uint8_t) p.aux);
        }
        node.setProperty ("pts", pts.getMemoryBlock(), nullptr);
        tree.appendChild (node, nullptr);
    }
    tree.writeToStream (out);
}

void GrossBeatProcessor::setStateInformation (const void* data, int size)
{
    juce::MemoryInputStream in (data, (size_t) juce::jmax (0, size), false);
    if (size < 8 || in.readInt() != kStateMagic)
    {
        BuiltinEffectBase::setStateInformation (data, size);
        return;
    }

    const int count = in.readInt();
    const auto& params = getParameters();
    for (int i = 0; i < count && in.getNumBytesRemaining() >= 4; ++i)
    {
        const float v = in.readFloat();
        if (i < params.size())
            params[i]->setValueNotifyingHost (v);
    }

    const auto tree = juce::ValueTree::readFromStream (in);
    if (! tree.hasType ("GrossBeat"))
        return;

    std::array<Slot, numSlots * 2> loaded;
    for (int i = 0; i < numSlots * 2; ++i)
    {
        const auto node = tree.getChild (i);
        const auto* pts = node.isValid() ? node.getProperty ("pts").getBinaryData() : nullptr;
        if (pts == nullptr)
        {
            loaded[(size_t) i] = factorySlot (i);
            continue;
        }
        auto& s = loaded[(size_t) i];
        s.name = node.getProperty ("name").toString();
        if (const auto* flags = node.getProperty ("flags").getBinaryData(); flags != nullptr && flags->getSize() == s.extFlags.size())
            std::memcpy (s.extFlags.data(), flags->getData(), s.extFlags.size());
        s.extA = (uint32_t) (int) node.getProperty ("a", 3);
        s.extB = (uint32_t) (int) node.getProperty ("b", 3);
        s.extTrailer = (uint32_t) (int) node.getProperty ("trailer", 0);
        juce::MemoryInputStream pin (*pts, false);
        while (pin.getNumBytesRemaining() >= kPointBytes && (int) s.points.size() < maxPoints)
        {
            Point p;
            p.x = pin.readDouble();
            p.y = pin.readFloat();
            p.tension = pin.readFloat();
            p.curve = (int) (uint8_t) pin.readByte();
            p.aux = (int) (uint8_t) pin.readByte();
            s.points.push_back (p);
        }
        sanitize (s);
    }

    std::vector<uint8_t> header;
    if (const auto* h = tree.getProperty ("extHeader").getBinaryData(); h != nullptr && h->getSize() == sizeof (kDefaultExtHeader))
        header.assign ((const uint8_t*) h->getData(), (const uint8_t*) h->getData() + h->getSize());
    else
        header.assign (std::begin (kDefaultExtHeader), std::end (kDefaultExtHeader));

    {
        const juce::SpinLock::ScopedLockType lock (slotLock);
        std::swap (slots, loaded);
        std::swap (extHeader, header);
    }
    ++slotsVersion;
}

namespace
{
    constexpr double kSnapBeats[] = { 0.0, 1.0 / 16.0, 1.0 / 12.0, 1.0 / 8.0, 1.0 / 6.0, 1.0 / 4.0, 1.0 / 3.0 };
    const char* const kSnapNames[] = { "X", "1/16", "1/12", "1/8", "1/6", "1/4", "1/3" };
    constexpr int kNumSnaps = 7;

    struct CurveItem { int curve; const char* name; };
    const CurveItem kCurveMenu[] = {
        { GB::singleCurve, "Single curve" }, { GB::doubleCurve, "Double curve" },
        { GB::altSingle, "Alt single curve" }, { GB::altDouble, "Alt double curve" },
        { GB::hold, "Hold" }, { GB::stairs, "Stairs" }, { GB::smoothStairs, "Smooth stairs" },
        { GB::pulse, "Pulse" }, { GB::wave, "Wave" }, { GB::halfSine, "Half sine" } };

    bool isFrequencyCurve (int c)
    {
        return c == GB::stairs || c == GB::smoothStairs || c == GB::pulse || c == GB::wave;
    }

    float defaultTensionFor (int c)
    {
        switch (c)
        {
            case GB::stairs:
            case GB::smoothStairs: return 0.425f;
            case GB::pulse:        return 0.51f;
            case GB::wave:         return 0.515f;
            default:               return 0.0f;
        }
    }

    juce::Colour modeColour (bool volume) { return volume ? Colors::gbVolume : Colors::gbTime; }

    void setParamValue (juce::RangedAudioParameter& p, float value)
    {
        p.beginChangeGesture();
        p.setValueNotifyingHost (p.convertTo0to1 (value));
        p.endChangeGesture();
    }

    std::optional<GB::Slot>& slotClipboard()
    {
        static std::optional<GB::Slot> clip;
        return clip;
    }

    void drawEnvelopePreview (juce::Graphics& g, const std::vector<GB::Point>& pts, juce::Rectangle<float> r,
                              juce::Colour c, float thickness)
    {
        juce::Path path;
        const int steps = juce::jmax (8, (int) r.getWidth());
        for (int k = 0; k <= steps; ++k)
        {
            const double x = GB::envBeats * k / steps;
            const float px = r.getX() + r.getWidth() * (float) k / (float) steps;
            const float py = r.getBottom() - r.getHeight() * GB::evaluate (pts, juce::jmin (x, GB::envBeats - 1.0e-9));
            if (k == 0) path.startNewSubPath (px, py);
            else        path.lineTo (px, py);
        }
        g.setColour (c);
        g.strokePath (path, juce::PathStrokeType (thickness));
    }
}

class GBKnob : public ValueKnob
{
public:
    GBKnob() {}

    juce::Colour colour = Colors::gbTime;

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        const float d = juce::jmin (b.getWidth(), b.getHeight()) - 4.0f;
        auto disc = juce::Rectangle<float> (d, d).withCentre (b.getCentre());
        const float r = d * 0.5f;
        const float t = (float) valueToProportionOfLength (getValue());
        const float a0 = -2.356f, a1 = 2.356f;
        const float a = a0 + t * (a1 - a0);

        g.setColour (Colors::gbEnvBg);
        g.fillEllipse (disc);
        juce::Path ring;
        ring.addCentredArc (disc.getCentreX(), disc.getCentreY(), r - 1.5f, r - 1.5f, 0.0f, a0, a1, true);
        g.setColour (Colors::gbGrid);
        g.strokePath (ring, juce::PathStrokeType (2.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        juce::Path arc;
        arc.addCentredArc (disc.getCentreX(), disc.getCentreY(), r - 1.5f, r - 1.5f, 0.0f, a0, a, true);
        g.setColour (isMouseOverOrDragging() ? colour.brighter (0.25f) : colour);
        g.strokePath (arc, juce::PathStrokeType (2.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        const auto c = disc.getCentre();
        const juce::Point<float> dir (std::sin (a), -std::cos (a));
        g.drawLine (juce::Line<float> (c + dir * 2.0f, c + dir * (r - 5.0f)), 2.0f);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GBKnob)
};

class GBButton : public juce::Component
{
public:
    juce::String text;
    std::function<bool()> isOn;
    std::function<void (const juce::MouseEvent&)> onDown, onUp;
    juce::Colour colour = Colors::gbTime;
    bool chevron = false;

    GBButton() { setMouseCursor (juce::MouseCursor::PointingHandCursor); }

    void mouseDown (const juce::MouseEvent& e) override { if (onDown) onDown (e); repaint(); }
    void mouseUp (const juce::MouseEvent& e) override   { if (onUp) onUp (e); repaint(); }
    void mouseEnter (const juce::MouseEvent&) override  { repaint(); }
    void mouseExit (const juce::MouseEvent&) override   { repaint(); }

    void paint (juce::Graphics& g) override
    {
        const bool on = isOn && isOn();
        auto b = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (on ? colour.withAlpha (0.85f) : Colors::gbEnvBg.brighter (isMouseOver() ? 0.12f : 0.05f));
        g.fillRoundedRectangle (b, 3.0f);
        g.setColour (on ? colour : Colors::gbGrid.brighter (0.2f));
        g.drawRoundedRectangle (b, 3.0f, 1.0f);
        const auto fg = on ? Colors::contrastText (colour) : Colors::text;
        if (chevron)
        {
            juce::Path p;
            const auto c = b.getCentre();
            p.startNewSubPath (c.x - 4.0f, c.y - 2.0f);
            p.lineTo (c.x, c.y + 2.5f);
            p.lineTo (c.x + 4.0f, c.y - 2.0f);
            g.setColour (fg);
            g.strokePath (p, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            return;
        }
        g.setColour (fg);
        g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
        g.drawFittedText (text, getLocalBounds().reduced (2, 0), juce::Justification::centred, 1, 0.8f);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GBButton)
};

class GBChoiceCell : public juce::Component
{
public:
    GBChoiceCell (juce::AudioParameterChoice& p, juce::String cap) : param (p), caption (std::move (cap))
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        attachment = std::make_unique<juce::ParameterAttachment> (param, [this] (float) { repaint(); });
        attachment->sendInitialUpdate();
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        juce::PopupMenu m;
        m.addSectionHeader (caption);
        for (int i = 0; i < param.choices.size(); ++i)
            m.addItem (i + 1, param.choices[i], true, param.getIndex() == i);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                         [safe = juce::Component::SafePointer<GBChoiceCell> (this)] (int r)
                         {
                             if (safe != nullptr && r > 0)
                                 safe->attachment->setValueAsCompleteGesture ((float) (r - 1));
                         });
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (Colors::gbEnvBg.brighter (isMouseOver() ? 0.12f : 0.05f));
        g.fillRoundedRectangle (b, 3.0f);
        g.setColour (Colors::gbGrid.brighter (0.2f));
        g.drawRoundedRectangle (b, 3.0f, 1.0f);
        auto r = getLocalBounds().reduced (6, 0);
        g.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::bold)));
        g.setColour (Colors::textDim);
        g.drawText (caption, r, juce::Justification::centredLeft, false);
        g.setFont (juce::Font (juce::FontOptions (10.5f)));
        g.setColour (Colors::text);
        g.drawText (param.getCurrentChoiceName(), r, juce::Justification::centredRight, false);
    }

    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { repaint(); }

private:
    juce::AudioParameterChoice& param;
    juce::String caption;
    std::unique_ptr<juce::ParameterAttachment> attachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GBChoiceCell)
};

class GBSlotGrid : public juce::Component
{
public:
    GBSlotGrid (GrossBeatProcessor& p, bool vol) : proc (p), volume (vol) {}

    std::function<void (int)> onSelect, onSwap, onRename;
    std::function<void (int)> onMenu;

    static constexpr int cols = 6, rows = 6;

    void refresh()
    {
        for (int i = 0; i < GB::numSlots; ++i)
            cache[(size_t) i] = proc.getSlot (GB::slotIndex (volume, i));
        repaint();
    }

    int selected() const { return (volume ? proc.volSlot->get() : proc.timeSlot->get()) - 1; }

    void paint (juce::Graphics& g) override
    {
        const auto col = modeColour (volume);
        const int active = proc.activeSlot (volume), pending = proc.pendingSlot (volume), sel = selected();
        for (int i = 0; i < GB::numSlots; ++i)
        {
            const auto r = cellRect (i);
            const bool isActive = i == active;
            g.setColour (isActive ? col.withAlpha (0.85f) : Colors::gbEnvBg);
            g.fillRoundedRectangle (r, 2.0f);
            drawEnvelopePreview (g, cache[(size_t) i].points, r.reduced (3.0f, 3.0f),
                                 isActive ? Colors::gbEnvBg.withAlpha (0.8f) : col.withAlpha (0.5f), 1.0f);
            if (i == pending)
            {
                g.setColour (col);
                g.drawRoundedRectangle (r.reduced (0.5f), 2.0f, 1.6f);
            }
            if (i == sel)
            {
                g.setColour (Colors::text);
                g.drawRoundedRectangle (r.reduced (0.5f), 2.0f, 1.2f);
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int i = cellAt (e.position);
        if (i < 0) return;
        if (e.mods.isPopupMenu())            { if (onMenu) onMenu (i); }
        else if (e.mods.isMiddleButtonDown()) { if (onSelect) onSelect (i); if (onRename) onRename (i); }
        else if (e.mods.isCtrlDown())         { if (onSwap) onSwap (i); }
        else if (onSelect)                    onSelect (i);
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int i = cellAt (e.position);
        setTooltipName (i >= 0 ? cache[(size_t) i].name : juce::String());
    }

    void mouseExit (const juce::MouseEvent&) override { setTooltipName ({}); }

    std::function<void (const juce::String&)> onHover;

private:
    void setTooltipName (const juce::String& n) { if (onHover) onHover (n); }

    juce::Rectangle<float> cellRect (int i) const
    {
        const float cw = (float) getWidth() / cols, ch = (float) getHeight() / rows;
        return { (float) (i % cols) * cw + 1.0f, (float) (i / cols) * ch + 1.0f, cw - 2.0f, ch - 2.0f };
    }

    int cellAt (juce::Point<float> p) const
    {
        const int c = (int) (p.x / ((float) getWidth() / cols)), r = (int) (p.y / ((float) getHeight() / rows));
        if (c < 0 || c >= cols || r < 0 || r >= rows) return -1;
        return r * cols + c;
    }

    GrossBeatProcessor& proc;
    bool volume;
    std::array<GB::Slot, GB::numSlots> cache;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GBSlotGrid)
};

class GBEnvelope : public juce::Component
{
public:
    explicit GBEnvelope (GrossBeatProcessor& p) : proc (p) {}

    bool volumeMode = false;
    int  snapIndex = 5;
    bool stepMode = false, slideMode = false, frozen = false;

    int editedIndex() const
    {
        return GB::slotIndex (volumeMode, (volumeMode ? proc.volSlot->get() : proc.timeSlot->get()) - 1);
    }

    void refresh()
    {
        cachedIndex = editedIndex();
        cachedVersion = proc.getSlotsVersion();
        work = proc.getSlot (cachedIndex);
        repaint();
    }

    void poll()
    {
        if (drag == DragKind::none && (cachedIndex != editedIndex() || cachedVersion != proc.getSlotsVersion()))
            refresh();
        const double pos = proc.envelopePos (volumeMode);
        if (pos != lastPos)
        {
            lastPos = pos;
            repaint();
        }
        setMouseCursor (frozen ? juce::MouseCursor::CrosshairCursor : juce::MouseCursor::NormalCursor);
    }

    GB::Slot& edited() { return work; }

    void commit (bool notify)
    {
        proc.setSlot (cachedIndex, work);
        work = proc.getSlot (cachedIndex);
        cachedVersion = proc.getSlotsVersion();
        repaint();
        if (notify) proc.notifySlotsEdited();
        else        dirty = true;
    }

    void paint (juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat();
        g.setColour (Colors::gbEnvBg);
        g.fillRoundedRectangle (area, 3.0f);
        const auto p = plot();
        const auto col = modeColour (volumeMode);

        {
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (p.toNearestInt());

            const double snap = kSnapBeats[snapIndex] > 0.0 ? kSnapBeats[snapIndex] : 0.25;
            const int cells = juce::roundToInt (GB::envBeats / snap);
            for (int k = 1; k < cells; ++k)
            {
                const double x = k * snap;
                const bool beat = std::abs (x - std::round (x)) < 1.0e-6;
                g.setColour (Colors::gbGrid.withAlpha (beat ? 1.0f : 0.45f));
                g.fillRect (xToPx (x) - 0.5f, p.getY(), 1.0f, p.getHeight());
            }

            if (! volumeMode)
            {
                for (int k = 1; k < (int) GB::timeRangeBeats; ++k)
                {
                    g.setColour (Colors::gbGrid.withAlpha (0.8f));
                    g.fillRect (p.getX(), yToPx (1.0f - (float) k / (float) GB::timeRangeBeats) - 0.5f, p.getWidth(), 1.0f);
                }
                g.setColour (Colors::gbGrid.brighter (0.15f).withAlpha (0.5f));
                for (int k = 1; k <= 12; ++k)
                {
                    const float c = 0.25f * (float) k;
                    g.drawLine (xToPx (0.0), yToPx (c), xToPx (GB::envBeats), yToPx (c - 1.0f), 1.0f);
                }
                g.setColour (Colors::gbGrid.darker (0.4f));
                g.drawLine (xToPx (0.0), yToPx (1.0f), xToPx (GB::envBeats), yToPx (0.5f), 2.5f);
            }
            else
            {
                for (int k = 1; k < 4; ++k)
                {
                    g.setColour (Colors::gbGrid.withAlpha (0.8f));
                    g.fillRect (p.getX(), yToPx ((float) k * 0.25f) - 0.5f, p.getWidth(), 1.0f);
                }
            }

            juce::Path curve;
            const int steps = (int) p.getWidth();
            for (int k = 0; k <= steps; ++k)
            {
                const double x = GB::envBeats * k / juce::jmax (1, steps);
                const float py = yToPx (GB::evaluate (work.points, juce::jmin (x, GB::envBeats - 1.0e-9)));
                const float px = p.getX() + (float) k;
                if (k == 0) curve.startNewSubPath (px, py);
                else        curve.lineTo (px, py);
            }
            juce::Path fill (curve);
            fill.lineTo (p.getRight(), p.getBottom());
            fill.lineTo (p.getX(), p.getBottom());
            fill.closeSubPath();
            g.setGradientFill (juce::ColourGradient (col.withAlpha (0.22f), 0.0f, p.getY(),
                                                     col.withAlpha (0.04f), 0.0f, p.getBottom(), false));
            g.fillPath (fill);
            g.setColour (col);
            g.strokePath (curve, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            const double pos = proc.envelopePos (volumeMode);
            if (pos >= 0.0)
            {
                g.setColour (Colors::gbTime.withAlpha (0.8f));
                g.fillRect (xToPx (pos) - 0.5f, p.getY(), 1.5f, p.getHeight());
            }
        }

        const auto& pts = work.points;
        for (int i = 1; i < (int) pts.size(); ++i)
            if (auto h = tensionHandle (i))
            {
                g.setColour (col.withAlpha (i == hoverTension ? 1.0f : 0.6f));
                g.drawEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (*h), 1.2f);
            }
        for (int i = 0; i < (int) pts.size(); ++i)
        {
            const juce::Point<float> c (xToPx (pts[(size_t) i].x), yToPx (pts[(size_t) i].y));
            const bool hot = i == hoverPoint || (drag == DragKind::point && i == dragIndex);
            const float d = hot ? 9.0f : 7.0f;
            g.setColour (hot ? col : Colors::gbEnvBg);
            g.fillEllipse (juce::Rectangle<float> (d, d).withCentre (c));
            g.setColour (col);
            g.drawEllipse (juce::Rectangle<float> (d, d).withCentre (c), 1.4f);
        }

        g.setFont (juce::Font (juce::FontOptions (9.0f)));
        g.setColour (Colors::textDim);
        const auto scale = area.withRight (p.getX() - 2.0f);
        if (! volumeMode)
        {
            for (int k = 0; k <= (int) GB::timeRangeBeats; k += 2)
                g.drawText (juce::String (-k), scale.withHeight (12.0f).withCentre ({ scale.getCentreX(), yToPx (1.0f - (float) k / 8.0f) })
                                               .constrainedWithin (scale), juce::Justification::centred, false);
        }
        else
        {
            for (int k = 0; k <= 4; ++k)
                g.drawText (juce::String (k * 25), scale.withHeight (12.0f).withCentre ({ scale.getCentreX(), yToPx ((float) k * 0.25f) })
                                                   .constrainedWithin (scale), juce::Justification::centred, false);
        }

        if (frozen)
        {
            g.setColour (Colors::textDim);
            g.drawText ("FROZEN", p.reduced (6.0f).toNearestInt(), juce::Justification::topRight, false);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int hp = hitPoint (e.position), ht = hp < 0 ? hitTension (e.position) : -1;
        if (hp != hoverPoint || ht != hoverTension)
        {
            hoverPoint = hp;
            hoverTension = ht;
            repaint();
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        hoverPoint = hoverTension = -1;
        repaint();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (frozen) return;
        auto& pts = work.points;
        const int pi = hitPoint (e.position);
        const int ti = pi < 0 ? hitTension (e.position) : -1;

        if (e.mods.isPopupMenu())
        {
            if (pi >= 0) { showPointMenu (pi); return; }
            if (ti >= 0)
            {
                pts[(size_t) ti].tension = 0.0f;
                pts[(size_t) ti].aux = GB::auxForTension (0.0f);
                commit (true);
                return;
            }
            const double x = snapX (pxToX (e.position.x), e.mods.isAltDown());
            const float y = snapY (pxToY (e.position.y), e.mods.isAltDown());
            int at = 1;
            while (at < (int) pts.size() && pts[(size_t) at].x <= x) ++at;
            const int inherit = at < (int) pts.size() ? pts[(size_t) at].curve : GB::singleCurve;
            const int curve = isFrequencyCurve (inherit) ? GB::singleCurve : inherit;
            pts.insert (pts.begin() + at, GB::Point { x, y, 0.0f, curve, GB::auxForTension (0.0f) });
            beginPointDrag (at);
            commit (false);
            return;
        }

        if (pi >= 0)
        {
            if (e.mods.isAltDown())
            {
                deletePoint (pi);
                return;
            }
            beginPointDrag (pi);
            return;
        }

        if (ti >= 0)
        {
            drag = DragKind::tension;
            dragIndex = ti;
            dragStartTension = pts[(size_t) ti].tension;
            dragStartMouse = e.position;
            return;
        }

        if (stepMode)
        {
            drag = DragKind::step;
            applyStep (e.position, e.mods.isShiftDown(), e.mods.isAltDown());
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (frozen) return;
        auto& pts = work.points;
        if (drag == DragKind::point && juce::isPositiveAndBelow (dragIndex, (int) pts.size()))
        {
            const bool noSnap = e.mods.isAltDown();
            double x = snapX (pxToX (e.position.x), noSnap);
            float y = snapY (pxToY (e.position.y), noSnap);
            if (e.mods.isShiftDown()) y = dragBase[(size_t) dragIndex].y;
            if (e.mods.isCtrlDown())  x = dragBase[(size_t) dragIndex].x;

            const int n = (int) pts.size();
            const double lo = dragIndex > 0 ? pts[(size_t) dragIndex - 1].x : 0.0;
            if (dragIndex == 0) x = 0.0;

            if (slideMode && dragIndex > 0)
            {
                const double tail = dragBase.back().x - dragBase[(size_t) dragIndex].x;
                x = juce::jlimit (lo, GB::envBeats - tail, x);
                const double dx = x - dragBase[(size_t) dragIndex].x;
                for (int j = dragIndex + 1; j < n; ++j)
                    pts[(size_t) j].x = dragBase[(size_t) j].x + dx;
            }
            else
            {
                const double hi = dragIndex + 1 < n ? pts[(size_t) dragIndex + 1].x : GB::envBeats;
                x = juce::jlimit (lo, hi, x);
            }
            pts[(size_t) dragIndex].x = x;
            pts[(size_t) dragIndex].y = y;
            commit (false);
        }
        else if (drag == DragKind::tension && juce::isPositiveAndBelow (dragIndex, (int) pts.size()))
        {
            auto& pt = pts[(size_t) dragIndex];
            const float sens = e.mods.isCtrlDown() ? 600.0f : 150.0f;
            const float dy = (dragStartMouse.y - e.position.y) / sens;
            const bool rising = pt.y >= pts[(size_t) dragIndex - 1].y;
            const float dir = isFrequencyCurve (pt.curve) ? -1.0f : (rising ? 1.0f : -1.0f);
            pt.tension = juce::jlimit (-1.0f, 1.0f, dragStartTension + dir * dy);
            pt.aux = GB::auxForTension (pt.tension);
            commit (false);
        }
        else if (drag == DragKind::step)
        {
            applyStep (e.position, e.mods.isShiftDown(), e.mods.isAltDown());
        }
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        drag = DragKind::none;
        dragIndex = -1;
        if (dirty)
        {
            dirty = false;
            proc.notifySlotsEdited();
        }
        refresh();
    }

private:
    enum class DragKind { none, point, tension, step };

    juce::Rectangle<float> plot() const
    {
        return getLocalBounds().toFloat().reduced (6.0f).withTrimmedLeft (20.0f);
    }

    float  xToPx (double x) const { const auto p = plot(); return p.getX() + (float) (x / GB::envBeats) * p.getWidth(); }
    double pxToX (float px) const { const auto p = plot(); return juce::jlimit (0.0, GB::envBeats, (double) ((px - p.getX()) / p.getWidth()) * GB::envBeats); }
    float  yToPx (float y) const  { const auto p = plot(); return p.getBottom() - y * p.getHeight(); }
    float  pxToY (float py) const { const auto p = plot(); return juce::jlimit (0.0f, 1.0f, (p.getBottom() - py) / p.getHeight()); }

    double snapX (double x, bool noSnap) const
    {
        const double s = kSnapBeats[snapIndex];
        return (noSnap || s <= 0.0) ? x : juce::jlimit (0.0, GB::envBeats, std::round (x / s) * s);
    }

    float snapY (float y, bool noSnap) const
    {
        const double s = kSnapBeats[snapIndex];
        if (noSnap || s <= 0.0) return y;
        const float q = volumeMode ? 1.0f / 32.0f : (float) (s / GB::timeRangeBeats);
        return juce::jlimit (0.0f, 1.0f, std::round (y / q) * q);
    }

    std::optional<juce::Point<float>> tensionHandle (int i) const
    {
        const auto& pts = work.points;
        const auto& a = pts[(size_t) i - 1];
        const auto& b = pts[(size_t) i];
        if (b.curve == GB::hold || xToPx (b.x) - xToPx (a.x) < 14.0f)
            return std::nullopt;
        const double xm = 0.5 * (a.x + b.x);
        const float ym = isFrequencyCurve (b.curve) ? 0.5f * (a.y + b.y)
                                                    : a.y + (b.y - a.y) * GB::shape (b.curve, b.tension, 0.5f);
        return juce::Point<float> (xToPx (xm), yToPx (ym));
    }

    int hitPoint (juce::Point<float> m) const
    {
        const auto& pts = work.points;
        int best = -1;
        float bestD = 7.0f;
        for (int i = (int) pts.size() - 1; i >= 0; --i)
        {
            const float d = m.getDistanceFrom ({ xToPx (pts[(size_t) i].x), yToPx (pts[(size_t) i].y) });
            if (d < bestD) { bestD = d; best = i; }
        }
        return best;
    }

    int hitTension (juce::Point<float> m) const
    {
        for (int i = 1; i < (int) work.points.size(); ++i)
            if (auto h = tensionHandle (i))
                if (m.getDistanceFrom (*h) < 6.0f)
                    return i;
        return -1;
    }

    void beginPointDrag (int i)
    {
        drag = DragKind::point;
        dragIndex = i;
        dragBase = work.points;
    }

    void deletePoint (int i)
    {
        auto& pts = work.points;
        if (i <= 0 || (int) pts.size() <= 2) return;
        pts.erase (pts.begin() + i);
        commit (true);
    }

    void applyStep (juce::Point<float> m, bool pulseMode, bool noSnap)
    {
        const double s = (noSnap || kSnapBeats[snapIndex] <= 0.0) ? 1.0 / 64.0 : kSnapBeats[snapIndex];
        const double x = pxToX (m.x);
        const double a = juce::jlimit (0.0, GB::envBeats - s, std::floor (x / s) * s);
        const double b = juce::jmin (GB::envBeats, a + s);
        const float y = snapY (pxToY (m.y), noSnap);
        auto& pts = work.points;
        const float after = GB::evaluate (pts, b);

        pts.erase (std::remove_if (pts.begin() + 1, pts.end(),
                                   [a, b] (const GB::Point& p) { return p.x >= a - 1.0e-9 && p.x <= b + 1.0e-9; }),
                   pts.end());
        std::vector<GB::Point> add;
        if (a <= 1.0e-9) pts.front().y = y;
        else             add.push_back ({ a, y, 0.0f, GB::hold, 2 });
        if (pulseMode && volumeMode)
            add.push_back ({ a + s * 0.5, 0.0f, 0.0f, GB::hold, 2 });
        add.push_back ({ b, after, 0.0f, GB::hold, 2 });
        for (const auto& p : add)
        {
            int at = 1;
            while (at < (int) pts.size() && pts[(size_t) at].x <= p.x) ++at;
            pts.insert (pts.begin() + at, p);
        }
        commit (false);
    }

    void showPointMenu (int i)
    {
        juce::PopupMenu m;
        const int cur = work.points[(size_t) i].curve;
        if (i > 0)
        {
            for (const auto& c : kCurveMenu)
                m.addItem (100 + c.curve, c.name, true, c.curve == cur);
            m.addSeparator();
            m.addItem (2, "Reset tension");
        }
        m.addItem (1, "Delete", i > 0 && (int) work.points.size() > 2);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this)
                                                    .withTargetScreenArea ({ juce::Desktop::getMousePosition(), juce::Desktop::getMousePosition() }),
                         [safe = juce::Component::SafePointer<GBEnvelope> (this), i] (int r)
                         {
                             if (safe == nullptr || r <= 0) return;
                             safe->refresh();
                             auto& pts = safe->work.points;
                             if (! juce::isPositiveAndBelow (i, (int) pts.size())) return;
                             if (r == 1) { safe->deletePoint (i); return; }
                             auto& p = pts[(size_t) i];
                             if (r == 2) p.tension = 0.0f;
                             else
                             {
                                 const int curve = r - 100;
                                 if (isFrequencyCurve (curve) != isFrequencyCurve (p.curve))
                                     p.tension = defaultTensionFor (curve);
                                 p.curve = curve;
                             }
                             p.aux = GB::auxForTension (p.tension);
                             safe->commit (true);
                         });
    }

    GrossBeatProcessor& proc;
    GB::Slot work;
    int cachedIndex = -1;
    uint32_t cachedVersion = 0;
    double lastPos = -2.0;
    bool dirty = false;

    DragKind drag = DragKind::none;
    int dragIndex = -1;
    std::vector<GB::Point> dragBase;
    float dragStartTension = 0.0f;
    juce::Point<float> dragStartMouse;
    int hoverPoint = -1, hoverTension = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GBEnvelope)
};

class GrossBeatEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit GrossBeatEditor (GrossBeatProcessor& p)
        : juce::AudioProcessorEditor (p), proc (p), timeGrid (p, false), volGrid (p, true), env (p)
    {
        auto setupKnob = [this] (GBKnob& k, juce::RangedAudioParameter& param, juce::Colour c, double def,
                                 std::unique_ptr<juce::SliderParameterAttachment>& att)
        {
            k.colour = c;
            k.setDoubleClickReturnValue (true, def);
            addAndMakeVisible (k);
            att = std::make_unique<juce::SliderParameterAttachment> (param, k);
        };
        setupKnob (timeK, *proc.timeMix, Colors::gbTime, 1.0, timeAtt);
        setupKnob (volK, *proc.volMix, Colors::gbVolume, 1.0, volAtt);
        setupKnob (attK, *proc.attack, Colors::gbVolume, 5.0, attAtt);
        setupKnob (relK, *proc.release, Colors::gbVolume, 8.0, relAtt);
        setupKnob (tnsK, *proc.tension, Colors::gbVolume, 0.0, tnsAtt);

        for (auto* grid : { &timeGrid, &volGrid })
        {
            const bool vol = grid == &volGrid;
            grid->onSelect = [this, vol] (int i) { selectSlot (vol, i); };
            grid->onSwap   = [this, vol] (int i)
            {
                const int sel = (vol ? proc.volSlot->get() : proc.timeSlot->get()) - 1;
                proc.swapSlots (GB::slotIndex (vol, sel), GB::slotIndex (vol, i));
                proc.notifySlotsEdited();
                selectSlot (vol, i);
            };
            grid->onRename = [this] (int) { nameLabel.showEditor(); };
            grid->onMenu   = [this, vol] (int i) { showSlotMenu (vol, i); };
            grid->onHover  = [this] (const juce::String& n) { hoverName = n; repaint (hintArea); };
            addAndMakeVisible (*grid);
        }

        addAndMakeVisible (env);

        menuBtn.chevron = true;
        menuBtn.onDown = [this] (const juce::MouseEvent&) { showOptionsMenu(); };
        addAndMakeVisible (menuBtn);

        holdBtn.text = "HOLD";
        holdBtn.colour = Colors::gbTime;
        holdBtn.isOn = [this] { return proc.holdOn->get(); };
        holdBtn.onDown = [this] (const juce::MouseEvent& e)
        {
            if (e.mods.isShiftDown()) setParamValue (*proc.holdOn, proc.holdOn->get() ? 0.0f : 1.0f);
            else                      setParamValue (*proc.holdOn, 1.0f);
        };
        holdBtn.onUp = [this] (const juce::MouseEvent& e)
        {
            if (! e.mods.isShiftDown()) setParamValue (*proc.holdOn, 0.0f);
        };
        addAndMakeVisible (holdBtn);

        for (int i = 0; i < kNumSnaps; ++i)
        {
            auto* b = snapBtns.add (new GBButton());
            b->text = kSnapNames[i];
            b->colour = Colors::gbGrid.brighter (0.6f);
            b->isOn = [this, i] { return env.snapIndex == i; };
            b->onDown = [this, i] (const juce::MouseEvent&) { env.snapIndex = i; repaintTools(); env.repaint(); };
            addAndMakeVisible (b);
        }

        auto setupToggle = [this] (GBButton& b, const char* text, bool& flag)
        {
            b.text = text;
            b.colour = Colors::gbGrid.brighter (0.6f);
            b.isOn = [&flag] { return flag; };
            b.onDown = [this, &flag] (const juce::MouseEvent&) { flag = ! flag; repaintTools(); env.repaint(); };
            addAndMakeVisible (b);
        };
        setupToggle (stepBtn, "STEP", env.stepMode);
        setupToggle (slideBtn, "SLIDE", env.slideMode);
        setupToggle (freezeBtn, "FRZ", env.frozen);

        timeModeBtn.text = "TIME";
        timeModeBtn.colour = Colors::gbTime;
        timeModeBtn.isOn = [this] { return ! env.volumeMode; };
        timeModeBtn.onDown = [this] (const juce::MouseEvent&) { setViewMode (false); };
        addAndMakeVisible (timeModeBtn);
        volModeBtn.text = "VOL";
        volModeBtn.colour = Colors::gbVolume;
        volModeBtn.isOn = [this] { return env.volumeMode; };
        volModeBtn.onDown = [this] (const juce::MouseEvent&) { setViewMode (true); };
        addAndMakeVisible (volModeBtn);

        linkBtn.text = "LINK";
        linkBtn.colour = Colors::gbGrid.brighter (0.6f);
        linkBtn.isOn = [this] { return proc.linkSlots->get(); };
        linkBtn.onDown = [this] (const juce::MouseEvent&) { setParamValue (*proc.linkSlots, proc.linkSlots->get() ? 0.0f : 1.0f); };
        addAndMakeVisible (linkBtn);

        addAndMakeVisible (holdCell);
        addAndMakeVisible (trigCell);
        addAndMakeVisible (posCell);

        nameLabel.setEditable (true, true, false);
        nameLabel.setFont (juce::Font (juce::FontOptions (11.5f, juce::Font::bold)));
        nameLabel.setColour (juce::Label::textColourId, Colors::text);
        nameLabel.setColour (juce::Label::backgroundColourId, Colors::gbEnvBg);
        nameLabel.setColour (juce::Label::outlineColourId, Colors::gbGrid.brighter (0.2f));
        nameLabel.onTextChange = [this]
        {
            auto s = proc.getSlot (env.editedIndex());
            s.name = nameLabel.getText().substring (0, 60);
            proc.setSlot (env.editedIndex(), s);
            proc.notifySlotsEdited();
        };
        addAndMakeVisible (nameLabel);

        timeGrid.refresh();
        volGrid.refresh();
        env.refresh();
        updateName();
        setSize (820, 430);
        startTimerHz (30);
    }

    ~GrossBeatEditor() override { stopTimer(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colors::gbPanel);
        g.setFont (Typography::caps (13.5f));
        g.setColour (Colors::gbTime);
        g.drawText ("TIME", timeCap, juce::Justification::centredLeft, false);
        g.setColour (Colors::gbVolume);
        g.drawText ("VOLUME", volCap, juce::Justification::centredLeft, false);

        g.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::bold)));
        g.setColour (Colors::textDim);
        const std::pair<GBKnob*, const char*> caps[] = { { &timeK, "MIX" }, { &volK, "MIX" }, { &attK, "ATT" },
                                                         { &relK, "REL" }, { &tnsK, "TNS" } };
        for (const auto& [k, cap] : caps)
            g.drawText (cap, k->getBounds().withY (k->getBottom()).withHeight (11), juce::Justification::centred, false);

        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        g.setColour (Colors::textDim);
        g.drawText (hoverName, hintArea, juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8);
        auto left = r.removeFromLeft (258);
        r.removeFromLeft (8);

        auto top = left.removeFromTop (24);
        menuBtn.setBounds (top.removeFromLeft (24));
        top.removeFromLeft (6);
        holdBtn.setBounds (top.removeFromRight (60));
        hintArea = top.reduced (4, 0);

        left.removeFromTop (6);
        auto tRow = left.removeFromTop (50);
        timeCap = tRow.removeFromLeft (80).withTrimmedTop (4).withHeight (18);
        timeK.setBounds (tRow.removeFromRight (40).withHeight (38));
        left.removeFromTop (2);
        timeGrid.setBounds (left.removeFromTop (138));

        left.removeFromTop (8);
        auto vRow = left.removeFromTop (50);
        volCap = vRow.removeFromLeft (80).withTrimmedTop (4).withHeight (18);
        for (auto* k : { &tnsK, &relK, &attK, &volK })
        {
            k->setBounds (vRow.removeFromRight (40).withHeight (38));
            vRow.removeFromRight (4);
        }
        left.removeFromTop (2);
        volGrid.setBounds (left.removeFromTop (138));

        auto bottom = r.removeFromBottom (26);
        r.removeFromBottom (6);
        auto tools = r.removeFromLeft (44);
        r.removeFromLeft (6);
        env.setBounds (r);

        auto placeTool = [&tools] (juce::Component& c, int h) { c.setBounds (tools.removeFromTop (h)); tools.removeFromTop (3); };
        placeTool (timeModeBtn, 22);
        placeTool (volModeBtn, 22);
        tools.removeFromTop (8);
        for (auto* b : snapBtns) placeTool (*b, 20);
        tools.removeFromTop (8);
        placeTool (stepBtn, 20);
        placeTool (slideBtn, 20);
        placeTool (freezeBtn, 20);

        bottom.removeFromLeft (50);
        nameLabel.setBounds (bottom.removeFromLeft (150));
        bottom.removeFromLeft (6);
        linkBtn.setBounds (bottom.removeFromLeft (46));
        bottom.removeFromLeft (6);
        const int w = (bottom.getWidth() - 12) / 3;
        holdCell.setBounds (bottom.removeFromLeft (w));
        bottom.removeFromLeft (6);
        trigCell.setBounds (bottom.removeFromLeft (w));
        bottom.removeFromLeft (6);
        posCell.setBounds (bottom);
    }

private:
    void timerCallback() override
    {
        const auto version = proc.getSlotsVersion();
        if (version != lastVersion)
        {
            lastVersion = version;
            timeGrid.refresh();
            volGrid.refresh();
        }
        const int stamp = proc.activeSlot (false) * 7 + proc.activeSlot (true) * 131 + proc.pendingSlot (false) * 1031
                        + proc.pendingSlot (true) * 7919 + proc.timeSlot->get() * 104729 + proc.volSlot->get() * 1299709;
        if (stamp != lastStamp)
        {
            lastStamp = stamp;
            timeGrid.repaint();
            volGrid.repaint();
        }
        env.poll();
        const int idx = env.editedIndex();
        if (idx != lastNameIndex || version != lastNameVersion)
            updateName();
        const int btnState = (proc.holdOn->get() ? 1 : 0) + (proc.linkSlots->get() ? 2 : 0);
        if (btnState != lastBtnState)
        {
            lastBtnState = btnState;
            holdBtn.repaint();
            linkBtn.repaint();
        }
    }

    void updateName()
    {
        lastNameIndex = env.editedIndex();
        lastNameVersion = proc.getSlotsVersion();
        if (! nameLabel.isBeingEdited())
            nameLabel.setText (proc.getSlot (lastNameIndex).name, juce::dontSendNotification);
    }

    void repaintTools()
    {
        for (auto* b : snapBtns) b->repaint();
        stepBtn.repaint();
        slideBtn.repaint();
        freezeBtn.repaint();
        timeModeBtn.repaint();
        volModeBtn.repaint();
    }

    void setViewMode (bool volume)
    {
        env.volumeMode = volume;
        env.refresh();
        updateName();
        repaintTools();
    }

    void selectSlot (bool volume, int i)
    {
        setParamValue (volume ? (juce::RangedAudioParameter&) *proc.volSlot : *proc.timeSlot, (float) (i + 1));
        if (proc.linkSlots->get())
            setParamValue (volume ? (juce::RangedAudioParameter&) *proc.timeSlot : *proc.volSlot, (float) (i + 1));
        setViewMode (volume);
    }

    void showSlotMenu (bool volume, int i)
    {
        juce::PopupMenu m;
        m.addItem (1, "Move slot left", i > 0);
        m.addItem (2, "Move slot right", i < GB::numSlots - 1);
        m.addItem (3, "Rename");
        m.addSeparator();
        m.addItem (4, "Copy");
        m.addItem (5, "Paste", slotClipboard().has_value());
        m.addSeparator();
        m.addItem (6, "Reset to default");
        m.addItem (7, "Clear");
        m.showMenuAsync (juce::PopupMenu::Options(),
                         [safe = juce::Component::SafePointer<GrossBeatEditor> (this), volume, i] (int r)
                         {
                             if (safe == nullptr || r <= 0) return;
                             auto& p = safe->proc;
                             const int idx = GB::slotIndex (volume, i);
                             switch (r)
                             {
                                 case 1: p.swapSlots (idx, idx - 1); safe->selectSlot (volume, i - 1); break;
                                 case 2: p.swapSlots (idx, idx + 1); safe->selectSlot (volume, i + 1); break;
                                 case 3: safe->selectSlot (volume, i); safe->nameLabel.showEditor(); return;
                                 case 4: slotClipboard() = p.getSlot (idx); return;
                                 case 5: if (slotClipboard()) p.setSlot (idx, *slotClipboard()); break;
                                 case 6: p.setSlot (idx, GB::factorySlot (idx)); break;
                                 case 7: p.setSlot (idx, SlotBuilder ("Empty", true).p (0.0, 1.0f)
                                                             .p (4.0, 1.0f, volume ? GB::hold : GB::singleCurve).get()); break;
                                 default: return;
                             }
                             p.notifySlotsEdited();
                         });
    }

    void showOptionsMenu()
    {
        juce::PopupMenu m;
        m.addItem (1, "Click reduction", true, proc.clickReduce->get());
        m.addSeparator();
        m.addItem (2, "Flip vertically");
        m.addItem (3, "Normalize levels", env.volumeMode);
        m.addSeparator();
        m.addItem (4, "Copy slot");
        m.addItem (5, "Paste slot", slotClipboard().has_value());
        m.addItem (6, "Reset slot to default");
        m.addItem (7, "Reset all slots to defaults");
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&menuBtn),
                         [safe = juce::Component::SafePointer<GrossBeatEditor> (this)] (int r)
                         {
                             if (safe == nullptr || r <= 0) return;
                             auto& p = safe->proc;
                             auto& env = safe->env;
                             const int idx = env.editedIndex();
                             env.refresh();
                             switch (r)
                             {
                                 case 1: setParamValue (*p.clickReduce, p.clickReduce->get() ? 0.0f : 1.0f); return;
                                 case 2:
                                     for (auto& pt : env.edited().points) { pt.y = 1.0f - pt.y; pt.tension = -pt.tension; pt.aux = GB::auxForTension (pt.tension); }
                                     env.commit (true);
                                     return;
                                 case 3:
                                 {
                                     float lo = 1.0f, hi = 0.0f;
                                     for (const auto& pt : env.edited().points) { lo = juce::jmin (lo, pt.y); hi = juce::jmax (hi, pt.y); }
                                     if (hi - lo > 1.0e-4f)
                                         for (auto& pt : env.edited().points) pt.y = (pt.y - lo) / (hi - lo);
                                     env.commit (true);
                                     return;
                                 }
                                 case 4: slotClipboard() = p.getSlot (idx); return;
                                 case 5: if (slotClipboard()) p.setSlot (idx, *slotClipboard()); break;
                                 case 6: p.setSlot (idx, GB::factorySlot (idx)); break;
                                 case 7: p.resetAllSlots(); break;
                                 default: return;
                             }
                             p.notifySlotsEdited();
                         });
    }

    GrossBeatProcessor& proc;
    GBSlotGrid timeGrid, volGrid;
    GBEnvelope env;
    GBKnob timeK, volK, attK, relK, tnsK;
    std::unique_ptr<juce::SliderParameterAttachment> timeAtt, volAtt, attAtt, relAtt, tnsAtt;
    GBButton menuBtn, holdBtn, stepBtn, slideBtn, freezeBtn, timeModeBtn, volModeBtn, linkBtn;
    juce::OwnedArray<GBButton> snapBtns;
    GBChoiceCell holdCell { *proc.holdMode, "HOLD" }, trigCell { *proc.trigSync, "TRIG SYNC" },
                 posCell { *proc.posSync, "POS SYNC" };
    juce::Label nameLabel;
    juce::Rectangle<int> timeCap, volCap, hintArea;
    juce::String hoverName;

    uint32_t lastVersion = 0, lastNameVersion = 0;
    int lastStamp = -1, lastNameIndex = -1, lastBtnState = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GrossBeatEditor)
};

juce::AudioProcessorEditor* GrossBeatProcessor::createEditor()
{
    return new GrossBeatEditor (*this);
}
