#include "FLNativeFx.h"
#include "../Engine/BuiltinParamEQ.h"
#include "../Engine/BuiltinGrossBeat.h"
#include "../Engine/BuiltinEffects.h"
#include <cmath>
#include <cstring>

namespace
{
    const char* const kParamEqName    = "Fruity Parametric EQ 2";
    const char* const kSoftClipName   = "Fruity Soft Clipper";
    const char* const kGrossBeatName  = "Nasty Beat";

    int32_t i32At (const std::vector<uint8_t>& d, size_t o)
    {
        return (int32_t) ((uint32_t) d[o] | ((uint32_t) d[o + 1] << 8)
                        | ((uint32_t) d[o + 2] << 16) | ((uint32_t) d[o + 3] << 24));
    }

    void putI32 (std::vector<uint8_t>& d, size_t o, int32_t v)
    {
        const auto u = (uint32_t) v;
        d[o] = (uint8_t) u; d[o + 1] = (uint8_t) (u >> 8); d[o + 2] = (uint8_t) (u >> 16); d[o + 3] = (uint8_t) (u >> 24);
    }

    juce::MemoryBlock decodeB64 (const juce::String& b64)
    {
        juce::MemoryOutputStream out;
        if (b64.isNotEmpty())
            juce::Base64::convertFromBase64 (out, b64);
        return out.getMemoryBlock();
    }

    juce::String encodeState (juce::AudioProcessor& p)
    {
        juce::MemoryBlock mb;
        p.getStateInformation (mb);
        return juce::Base64::toBase64 (mb.getData(), mb.getSize());
    }

    constexpr double kFlFreqLo = 8.413, kFlFreqRatio = 3411.65;
    constexpr int    kFlFreqDefault[ParamEQProcessor::numBands] = { 10923, 18204, 25486, 32768, 40050, 47332, 54613 };
    constexpr int    kFlBwDefault[ParamEQProcessor::numBands]   = { 40124, 25412, 25412, 25412, 25412, 25412, 40124 };
    constexpr int    kFlTypeDefault[ParamEQProcessor::numBands] = { 5, 6, 6, 6, 6, 6, 7 };
    constexpr int    kParamEqV3Size = 309;

    float flFreqToHz (int v)
    {
        return (float) (kFlFreqLo * std::pow (kFlFreqRatio, juce::jlimit (0, 65536, v) / 65536.0));
    }
    int hzToFlFreq (float hz)
    {
        return juce::jlimit (0, 65536, juce::roundToInt (65536.0 * std::log (juce::jmax (1.0, (double) hz) / kFlFreqLo)
                                                          / std::log (kFlFreqRatio)));
    }
    float flBwToOctaves (int v)
    {
        const double t = juce::jlimit (0, 65536, v) / 65536.0;
        return (float) (ParamEQProcessor::minBw + (ParamEQProcessor::maxBw - ParamEQProcessor::minBw) * t * t);
    }
    int octavesToFlBw (float oct)
    {
        const double t = std::sqrt (juce::jlimit (0.0, 1.0, (double) (oct - ParamEQProcessor::minBw)
                                                             / (double) (ParamEQProcessor::maxBw - ParamEQProcessor::minBw)));
        return juce::jlimit (0, 65536, juce::roundToInt (65536.0 * t));
    }

    void setF (juce::AudioParameterFloat* p, float v)  { p->setValueNotifyingHost (p->convertTo0to1 (v)); }
    void setC (juce::AudioParameterChoice* p, int i)   { p->setValueNotifyingHost (p->convertTo0to1 ((float) i)); }

    bool importParamEq (const std::vector<uint8_t>& d, FLNativeFx::Imported& out)
    {
        if (d.size() < kParamEqV3Size)
            return false;
        ParamEQProcessor eq;
        constexpr int nb = ParamEQProcessor::numBands;
        for (int i = 0; i < nb; ++i)
        {
            const int gain  = i32At (d, 4 + i * 4);
            const int freq  = i32At (d, 32 + i * 4);
            const int bw    = i32At (d, 60 + i * 4);
            const int type  = i32At (d, 88 + i * 4);
            const int order = i32At (d, 116 + i * 4);
            setF (eq.band[i].gain, juce::jlimit (ParamEQProcessor::minGainDb, ParamEQProcessor::maxGainDb, gain / 100.0f));
            setF (eq.band[i].freq, juce::jlimit (ParamEQProcessor::minFreq, ParamEQProcessor::maxFreq, flFreqToHz (freq)));
            setF (eq.band[i].bw, flBwToOctaves (bw));
            setC (eq.band[i].type, juce::jlimit (0, 7, type));
            eq.setSlopeOrder (i, order);
        }
        setF (eq.master, juce::jlimit (ParamEQProcessor::minGainDb, ParamEQProcessor::maxGainDb, i32At (d, 288) / 100.0f));
        out.type = "parameq";
        out.stateB64 = encodeState (eq);
        return true;
    }

    bool exportParamEq (const juce::String& stateB64, FLNativeFx::Exported& out)
    {
        ParamEQProcessor eq;
        auto mb = decodeB64 (stateB64);
        if (mb.getSize() > 0)
            eq.setStateInformation (mb.getData(), (int) mb.getSize());

        std::vector<uint8_t> d ((size_t) kParamEqV3Size, 0);
        putI32 (d, 0, 3);
        constexpr int nb = ParamEQProcessor::numBands;
        for (int i = 0; i < nb; ++i)
        {
            putI32 (d, 4 + i * 4,   juce::roundToInt (eq.band[i].gain->get() * 100.0f));
            putI32 (d, 32 + i * 4,  hzToFlFreq (eq.band[i].freq->get()));
            putI32 (d, 60 + i * 4,  octavesToFlBw (eq.band[i].bw->get()));
            putI32 (d, 88 + i * 4,  eq.band[i].type->getIndex());
            putI32 (d, 116 + i * 4, eq.slopeOrder (i));
            putI32 (d, 176 + i * 4, kFlFreqDefault[i]);
            putI32 (d, 204 + i * 4, kFlBwDefault[i]);
            putI32 (d, 232 + i * 4, kFlTypeDefault[i]);
        }
        putI32 (d, 288, juce::roundToInt (eq.master->get() * 100.0f));
        putI32 (d, 292, 3);
        d[300] = 1; d[301] = 2;
        out.internalName = kParamEqName;
        out.state = std::move (d);
        return true;
    }

    bool importSoftClip (const std::vector<uint8_t>& d, FLNativeFx::Imported& out)
    {
        if (d.size() < 8)
            return false;
        SoftClipperProcessor sc;
        setF (sc.thres, juce::jlimit (0.10f, 1.00f, i32At (d, 0) / 128.0f));
        setF (sc.post,  juce::jlimit (0.0f, 1.6f, i32At (d, 4) / 128.0f));
        out.type = "softclip";
        out.stateB64 = encodeState (sc);
        return true;
    }

    bool exportSoftClip (const juce::String& stateB64, FLNativeFx::Exported& out)
    {
        SoftClipperProcessor sc;
        auto mb = decodeB64 (stateB64);
        if (mb.getSize() > 0)
            sc.setStateInformation (mb.getData(), (int) mb.getSize());
        std::vector<uint8_t> d (8, 0);
        putI32 (d, 0, juce::roundToInt (sc.thres->get() * 128.0f));
        putI32 (d, 4, juce::roundToInt (sc.post->get() * 128.0f));
        out.internalName = kSoftClipName;
        out.state = std::move (d);
        return true;
    }
}

namespace
{
    constexpr size_t kGbHeaderSize = 49;
    constexpr int    kGbMixUnity   = 256;
    constexpr double kGbKnobScale  = 8192.0;

    struct Reader
    {
        const std::vector<uint8_t>& d;
        size_t p = 0;
        bool ok = true;

        bool need (size_t n) { ok = ok && p + n <= d.size(); return ok; }
        uint8_t  u8()  { if (! need (1)) return 0; return d[p++]; }
        uint32_t u32() { if (! need (4)) return 0; const auto v = (uint32_t) i32At (d, p); p += 4; return v; }
        float    f32() { const auto u = u32(); float f; std::memcpy (&f, &u, 4); return f; }
        double   f64()
        {
            if (! need (8)) return 0.0;
            uint64_t u = 0;
            for (int i = 7; i >= 0; --i) u = (u << 8) | d[p + (size_t) i];
            p += 8;
            double v; std::memcpy (&v, &u, 8); return v;
        }
    };

    struct Writer
    {
        std::vector<uint8_t> d;
        void u8 (uint8_t v)   { d.push_back (v); }
        void u32 (uint32_t v) { for (int i = 0; i < 4; ++i) d.push_back ((uint8_t) (v >> (8 * i))); }
        void f32 (float f)    { uint32_t u; std::memcpy (&u, &f, 4); u32 (u); }
        void f64 (double f)
        {
            uint64_t u; std::memcpy (&u, &f, 8);
            for (int i = 0; i < 8; ++i) d.push_back ((uint8_t) (u >> (8 * i)));
        }
    };

    bool importGrossBeat (const std::vector<uint8_t>& d, FLNativeFx::Imported& out)
    {
        using GB = GrossBeatProcessor;
        if (d.size() < kGbHeaderSize || (i32At (d, 0) != 0x00090003 && i32At (d, 0) != 0x00080003))
            return false;

        GB gb;
        Reader r { d, kGbHeaderSize };
        for (int i = 0; i < GB::numSlots * 2; ++i)
        {
            GB::Slot s;
            const int nameLen = r.u8();
            if (! r.need ((size_t) nameLen)) return false;
            s.name = juce::String::createStringFromData (d.data() + r.p, nameLen);
            r.p += (size_t) nameLen;
            for (auto& f : s.extFlags) f = r.u8();
            s.extA = r.u32();
            s.extB = r.u32();
            const uint32_t count = r.u32();
            if (! r.ok || count > 1000000u) return false;
            double x = 0.0;
            for (uint32_t k = 0; k < count && r.ok; ++k)
            {
                GB::Point p;
                x += r.f64();
                p.x = x;
                p.y = (float) r.f64();
                p.tension = r.f32();
                const uint32_t type = r.u32();
                p.curve = (int) (type & 0xffu);
                p.aux = (int) (type >> 24);
                if ((int) s.points.size() < GB::maxPoints)
                    s.points.push_back (p);
            }
            s.extTrailer = r.u32();
            if (! r.need (16)) return false;
            r.p += 16;
            gb.setSlot (i, std::move (s));
        }
        if (! r.ok || r.p + 1 != d.size()) return false;

        gb.setExtHeader (std::vector<uint8_t> (d.begin(), d.begin() + (std::ptrdiff_t) kGbHeaderSize));
        auto setInt = [] (juce::AudioParameterInt* p, int v) { p->setValueNotifyingHost (p->convertTo0to1 ((float) v)); };
        setInt (gb.timeSlot, juce::jlimit (1, GB::numSlots, i32At (d, 4) + 1));
        setInt (gb.volSlot, juce::jlimit (1, GB::numSlots, i32At (d, 8) + 1));
        setF (gb.timeMix, juce::jlimit (0.0f, 1.0f, (float) i32At (d, 12) / (float) kGbMixUnity));
        setF (gb.volMix, juce::jlimit (0.0f, 1.0f, (float) i32At (d, 16) / (float) kGbMixUnity));
        gb.attack->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, (float) (i32At (d, 20) / kGbKnobScale)));
        gb.release->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, (float) (i32At (d, 24) / kGbKnobScale)));

        out.type = "grossbeat";
        out.stateB64 = encodeState (gb);
        return true;
    }

    bool exportGrossBeat (const juce::String& stateB64, FLNativeFx::Exported& out)
    {
        using GB = GrossBeatProcessor;
        GB gb;
        auto mb = decodeB64 (stateB64);
        if (mb.getSize() > 0)
            gb.setStateInformation (mb.getData(), (int) mb.getSize());

        Writer w;
        w.d = gb.getExtHeader();
        putI32 (w.d, 4, gb.timeSlot->get() - 1);
        putI32 (w.d, 8, gb.volSlot->get() - 1);
        putI32 (w.d, 12, juce::roundToInt (gb.timeMix->get() * kGbMixUnity));
        putI32 (w.d, 16, juce::roundToInt (gb.volMix->get() * kGbMixUnity));
        putI32 (w.d, 20, juce::roundToInt (gb.attack->convertTo0to1 (gb.attack->get()) * kGbKnobScale));
        putI32 (w.d, 24, juce::roundToInt (gb.release->convertTo0to1 (gb.release->get()) * kGbKnobScale));

        for (int i = 0; i < GB::numSlots * 2; ++i)
        {
            const auto s = gb.getSlot (i);
            const auto name = s.name.toStdString().substr (0, 255);
            w.u8 ((uint8_t) name.size());
            for (char c : name) w.u8 ((uint8_t) c);
            for (auto f : s.extFlags) w.u8 (f);
            w.u32 (s.extA);
            w.u32 (s.extB);
            w.u32 ((uint32_t) s.points.size());
            double prevX = 0.0;
            for (const auto& p : s.points)
            {
                w.f64 (p.x - prevX);
                prevX = p.x;
                w.f64 ((double) p.y);
                w.f32 (p.tension);
                w.u32 (((uint32_t) (p.aux & 0xff) << 24) | (uint32_t) (p.curve & 0xff));
            }
            w.u32 (s.extTrailer);
            for (int k = 0; k < 16; ++k) w.u8 (0xff);
        }
        w.u8 (1);

        out.internalName = kGrossBeatName;
        out.state = std::move (w.d);
        return true;
    }
}

namespace FLNativeFx
{
    bool canImport (const juce::String& n)
    {
        return n == kParamEqName || n == kSoftClipName || n == kGrossBeatName;
    }

    bool importSlot (const juce::String& n, const std::vector<uint8_t>& state, Imported& out)
    {
        if (n == kParamEqName)  return importParamEq (state, out);
        if (n == kSoftClipName) return importSoftClip (state, out);
        if (n == kGrossBeatName) return importGrossBeat (state, out);
        return false;
    }

    bool canExport (const juce::String& type)
    {
        return type == "parameq" || type == "softclip" || type == "grossbeat";
    }

    bool exportSlot (const juce::String& type, const juce::String& stateB64, Exported& out)
    {
        if (type == "parameq")  return exportParamEq (stateB64, out);
        if (type == "softclip") return exportSoftClip (stateB64, out);
        if (type == "grossbeat") return exportGrossBeat (stateB64, out);
        return false;
    }
}
