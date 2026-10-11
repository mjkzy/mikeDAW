#include "FLPImport.h"
#include "Vst2Fxb.h"
#include "FLNativeFx.h"
#include "../Engine/PluginHost.h"
#include "../Engine/Bridge32Host.h"
#include "../Model/ProjectModel.h"
#include "../UI/AppLookAndFeel.h"
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <vector>

namespace
{

struct Event
{
    uint8_t opcode = 0;
    uint32_t value = 0;
    std::vector<uint8_t> blob;
    bool isBlob = false;
};

uint16_t u16At (const std::vector<uint8_t>& d, size_t o) { return (uint16_t) (d[o] | (d[o + 1] << 8)); }
uint32_t u32At (const uint8_t* d, size_t o)
{
    return (uint32_t) d[o] | ((uint32_t) d[o + 1] << 8) | ((uint32_t) d[o + 2] << 16) | ((uint32_t) d[o + 3] << 24);
}
uint64_t u64At (const uint8_t* d, size_t o)
{
    return (uint64_t) u32At (d, o) | ((uint64_t) u32At (d, o + 4) << 32);
}
uint32_t u32At (const std::vector<uint8_t>& d, size_t o) { return u32At (d.data(), o); }
uint64_t u64At (const std::vector<uint8_t>& d, size_t o) { return u64At (d.data(), o); }

std::vector<Event> readEvents (juce::MemoryInputStream& in, juce::int64 dataEnd)
{
    std::vector<Event> events;
    while (in.getPosition() < dataEnd && ! in.isExhausted())
    {
        Event ev;
        ev.opcode = (uint8_t) in.readByte();

        if (ev.opcode == 0xac)
        {
            ev.isBlob = true;
            ev.blob.resize (3);
            if (in.read (ev.blob.data(), 3) != 3)
                break;
        }
        else if (ev.opcode >= 0xc0)
        {
            uint32_t len = 0;
            int shift = 0;
            uint8_t b = 0;
            do
            {
                b = (uint8_t) in.readByte();
                len |= (uint32_t) (b & 0x7f) << shift;
                shift += 7;
            } while ((b & 0x80) != 0 && shift <= 35);

            auto remaining = (uint32_t) juce::jmax ((juce::int64) 0, dataEnd - in.getPosition());
            len = juce::jmin (len, remaining);
            ev.isBlob = true;
            ev.blob.resize (len);
            if (len > 0 && in.read (ev.blob.data(), (int) len) != (int) len)
                break;
        }
        else if (ev.opcode < 0x40)  ev.value = (uint8_t) in.readByte();
        else if (ev.opcode < 0x80)  ev.value = (uint16_t) in.readShort();
        else                        ev.value = (uint32_t) in.readInt();

        events.push_back (std::move (ev));
    }
    return events;
}

juce::String decodeText (const std::vector<uint8_t>& d, bool legacy)
{
    if (legacy)
    {
        size_t end = 0;
        while (end < d.size() && d[end] != 0) ++end;
        return juce::String::fromUTF8 ((const char*) d.data(), (int) end);
    }
    if (d.size() < 2)
        return {};
    size_t n = 0;
    while (n + 1 < d.size() && ! (d[n] == 0 && d[n + 1] == 0)) n += 2;
    using CT = juce::CharPointer_UTF16::CharType;
    return juce::String (juce::CharPointer_UTF16 ((const CT*) d.data()),
                         juce::CharPointer_UTF16 ((const CT*) (d.data() + n)));
}

bool isFlDefaultColour (uint32_t packed)
{
    switch (packed & 0xffffffu)
    {
        case 0x3a3834: case 0x565148: case 0x6a655c: return true;
        default: return false;
    }
}

juce::Colour bgrToColour (uint32_t packed)
{
    return juce::Colour ((juce::uint8) (packed & 0xff),
                         (juce::uint8) ((packed >> 8) & 0xff),
                         (juce::uint8) ((packed >> 16) & 0xff));
}

juce::String colourFromBgr (uint32_t packed)
{
    return bgrToColour (packed).toString();
}

juce::String clipColourFromBgr (uint32_t packed)
{
    return Colors::clipHeaderLift (bgrToColour (packed)).toString();
}

constexpr uint8_t OP_CHANNEL_ENABLED    = 0x00;
constexpr uint8_t OP_CHANNEL_TYPE       = 0x15;
constexpr uint8_t OP_CHANNEL_ROUTED_TO  = 0x16;
constexpr uint8_t OP_CHANNEL_ROUTED_TO_25 = 0x68;
constexpr uint8_t OP_NEW_CHANNEL        = 0x40;
constexpr uint8_t OP_CHANNEL_CUT_GROUPS = 0x84;
constexpr uint8_t OP_PATTERN_NEW        = 0x41;
constexpr uint8_t OP_TEMPO_COARSE       = 0x42;
constexpr uint8_t OP_TEMPO_FINE         = 0x5d;
constexpr uint8_t OP_NEW_SLOT           = 0x62;
constexpr uint8_t OP_ARRANGEMENT_NEW    = 0x63;
constexpr uint8_t OP_INSERT_END         = 0x93;
constexpr uint8_t OP_INSERT_COLOUR      = 0x95;
constexpr uint8_t OP_PATTERN_COLOUR     = 0x96;
constexpr uint8_t OP_TEMPO              = 0x9c;
constexpr uint8_t OP_PATTERN_LENGTH     = 0xa4;
constexpr uint8_t OP_CHANNEL_NAME_OLD   = 0xc0;
constexpr uint8_t OP_PATTERN_NAME       = 0xc1;
constexpr uint8_t OP_PROJECT_TITLE      = 0xc2;
constexpr uint8_t OP_CHANNEL_SAMPLE     = 0xc4;
constexpr uint8_t OP_PROJECT_FL_VERSION = 0xc7;
constexpr uint8_t OP_PLUGIN_INTERNAL    = 0xc9;
constexpr uint8_t OP_NAME               = 0xcb;
constexpr uint8_t OP_INSERT_NAME        = 0xcc;
constexpr uint8_t OP_PATTERN_NOTES_OLD  = 0xd0;
constexpr uint8_t OP_PLUGIN_STATE       = 0xd5;
constexpr uint8_t OP_CHANNEL_PARAMS     = 0xd7;
constexpr uint8_t OP_CHANNEL_LEVELS     = 0xdb;
constexpr uint8_t OP_PATTERN_NOTES      = 0xe0;
constexpr uint8_t OP_MIXER_PARAMS       = 0xe1;
constexpr uint8_t OP_PLAYLIST           = 0xe9;
constexpr uint8_t OP_INSERT_FLAGS       = 0xec;
constexpr uint8_t OP_TRACK_DATA         = 0xee;
constexpr uint8_t OP_TRACK_NAME         = 0xef;

constexpr int PATTERN_BASE = 20480;
constexpr int MAX_TRACK_IDX = 499;

struct WrapperInfo
{
    juce::String name, vendor, path;
    juce::MemoryBlock state;
    uint32_t uid = 0;
    bool hasUid = false;
    bool valid = false;
};

WrapperInfo decodeWrapper (const std::vector<uint8_t>& d)
{
    WrapperInfo w;
    if (d.size() < 4)
        return w;

    size_t pos = 4;
    while (pos + 12 <= d.size())
    {
        auto id  = u32At (d, pos);
        auto len = u64At (d, pos + 4);
        if (len > (uint64_t) (d.size() - pos - 12))
            break;

        const char* p = (const char*) d.data() + pos + 12;
        switch (id)
        {
            case 51:
                if (len >= 4) { w.uid = u32At ((const uint8_t*) p, 0); w.hasUid = true; }
                break;
            case 53: w.state  = juce::MemoryBlock (p, (size_t) len);   break;
            case 54: w.name   = juce::String::fromUTF8 (p, (int) len); break;
            case 55: w.path   = juce::String::fromUTF8 (p, (int) len); break;
            case 56: w.vendor = juce::String::fromUTF8 (p, (int) len); break;
            default: break;
        }
        pos += 12 + (size_t) len;
    }
    w.valid = w.name.isNotEmpty() || w.path.isNotEmpty() || w.state.getSize() > 0;
    return w;
}

struct FLChannel
{
    int iid = 0;
    int kind = -1;
    juce::String name, samplePath, pluginInternalName;
    WrapperInfo vst;
    bool enabled = true;
    int targetInsert = -1;
    int rawVolume = 10000, rawPan = 6400;
    bool hasLevels = false;
    bool sawPluginEvent = false;
    double stretchBeats = 0.0;
    int cutGroup = 0, cutByGroup = 0;
};

struct FLNote  { int pos = 0, len = 0, key = 60, vel = 100, chanIid = 0; };
struct FLPattern
{
    int id = 0;
    juce::String name, colour;
    int lengthTicks = 0;
    std::vector<FLNote> notes;
};

struct FLSlot
{
    int index = 0;
    bool hasPlugin = false;
    juce::String pluginName, internalName;
    WrapperInfo vst;
    std::vector<uint8_t> nativeState;
    bool enabled = true;
    int mix = 12800;
};

struct FLInsert
{
    int index = 0;
    juce::String name, colour;
    bool muted = false, solo = false;
    int rawVolume = 12800, rawPan = 0, rawStereoSep = 0;
    std::vector<FLSlot> slots;
};

struct FLClip  { int posTicks = 0, itemIndex = 0, lenTicks = 0, trackRvidx = 0,
                     startOffsetTicks = -1;
                 float startOffsetMs = -1.0f;
                 float fadeInMs = 0.0f, fadeOutMs = 0.0f;
                 double stretchScale = 0.0; };
struct FLTrack { juce::String name, colour; bool enabled = true; };
struct FLArrangement
{
    std::vector<FLTrack> tracks;
    std::vector<FLClip> clips;
};

struct FLProject
{
    int ppq = 96;
    int versionMajor = 0;
    bool legacyText = false;
    double tempo = 0.0;
    juce::String title;
    std::vector<FLChannel> channels;
    std::vector<FLPattern> patterns;
    std::vector<FLInsert> inserts;
    std::vector<FLArrangement> arrangements;
};

void buildMetadata (const std::vector<Event>& events, FLProject& out)
{
    for (auto& ev : events)
    {
        if (ev.opcode == OP_PROJECT_FL_VERSION && ev.isBlob)
        {
            size_t end = 0;
            while (end < ev.blob.size() && ev.blob[end] != 0) ++end;
            auto s = juce::String::fromUTF8 ((const char*) ev.blob.data(), (int) end);
            auto parts = juce::StringArray::fromTokens (s, ".", {});
            if (parts.size() >= 3)
            {
                out.versionMajor = parts[0].getIntValue();
                out.legacyText = out.versionMajor < 11
                              || (out.versionMajor == 11 && parts[1].getIntValue() < 5);
            }
            break;
        }
    }

    for (auto& ev : events)
    {
        if (ev.opcode == OP_TEMPO && ! ev.isBlob)               { out.tempo = ev.value / 1000.0; break; }
    }
    if (out.tempo <= 0.0)
    {
        for (auto& ev : events)
            if (ev.opcode == OP_TEMPO_COARSE && ! ev.isBlob)    { out.tempo = ev.value; break; }
        for (auto& ev : events)
            if (ev.opcode == OP_TEMPO_FINE && ! ev.isBlob)      { out.tempo += ev.value / 1000.0; break; }
    }

    for (auto& ev : events)
        if (ev.opcode == OP_PROJECT_TITLE && ev.isBlob)
        {
            auto s = decodeText (ev.blob, out.legacyText);
            if (s.isNotEmpty() && ! s.startsWith ("FL Studio"))
                out.title = s;
            break;
        }
}

void buildChannels (const std::vector<Event>& events, FLProject& out)
{
    enum class Scope { outside, channel, slot };
    Scope scope = Scope::outside;
    FLChannel* current = nullptr;
    const bool legacy = out.legacyText;

    for (auto& ev : events)
    {
        if (ev.opcode == OP_NEW_CHANNEL && ! ev.isBlob)
        {
            out.channels.push_back ({});
            current = &out.channels.back();
            current->iid = (int) ev.value;
            scope = Scope::channel;
            continue;
        }
        if (ev.opcode == OP_NEW_SLOT)              { scope = Scope::slot; continue; }
        if (ev.opcode == OP_INSERT_END || ev.opcode == OP_INSERT_FLAGS)
        {
            scope = Scope::outside;
            continue;
        }
        if (scope != Scope::channel || current == nullptr)
            continue;

        switch (ev.opcode)
        {
            case OP_CHANNEL_TYPE:       current->kind = (int) ev.value; break;
            case OP_CHANNEL_ENABLED:    current->enabled = ev.value != 0; break;
            case OP_CHANNEL_ROUTED_TO:  current->targetInsert = ev.value > 127 ? (int) ev.value - 256
                                                                               : (int) ev.value; break;
            case OP_CHANNEL_ROUTED_TO_25:
                if (! ev.isBlob && ev.value <= 127)
                    current->targetInsert = (int) ev.value;
                break;
            case OP_CHANNEL_CUT_GROUPS:
                if (! ev.isBlob)
                {
                    current->cutGroup   = (int) (ev.value & 0xffff);
                    current->cutByGroup = (int) (ev.value >> 16);
                }
                break;
            case OP_CHANNEL_LEVELS:
                if (ev.isBlob && ev.blob.size() >= 24)
                {
                    current->rawPan    = (int32_t) u32At (ev.blob, 0);
                    current->rawVolume = (int32_t) u32At (ev.blob, 4);
                    current->hasLevels = true;
                }
                break;
            case OP_CHANNEL_SAMPLE:
                if (ev.isBlob) current->samplePath = decodeText (ev.blob, legacy);
                break;
            case OP_CHANNEL_PARAMS:
                if (ev.isBlob && ev.blob.size() >= 100)
                {
                    uint32_t raw = u32At (ev.blob, 96);
                    double units = (double) raw;
                    if (raw >= 0x10000000)
                    {
                        float f = 0.0f;
                        memcpy (&f, &raw, 4);
                        units = (double) f;
                    }
                    if (std::isfinite (units) && units > 0.0 && units < 10000000.0)
                        current->stretchBeats = units / 192.0;
                }
                break;
            case OP_PLUGIN_INTERNAL:
                if (ev.isBlob)
                {
                    current->sawPluginEvent = true;
                    if (current->pluginInternalName.isEmpty())
                        current->pluginInternalName = decodeText (ev.blob, legacy);
                }
                break;
            case OP_PLUGIN_STATE:
                if (ev.isBlob && current->pluginInternalName == "Fruity Wrapper" && ! current->vst.valid)
                    current->vst = decodeWrapper (ev.blob);
                break;
            case OP_NAME:
                if (ev.isBlob) current->name = decodeText (ev.blob, legacy);
                break;
            case OP_CHANNEL_NAME_OLD:
                if (ev.isBlob && current->name.isEmpty())
                    current->name = decodeText (ev.blob, legacy);
                break;
            default: break;
        }
    }

    for (auto& ch : out.channels)
        if (ch.kind == 4 && ch.samplePath.isNotEmpty() && ch.sawPluginEvent && ! ch.vst.valid
            && (ch.pluginInternalName.isEmpty() || ch.pluginInternalName == "Fruity Wrapper"))
            ch.kind = 0;
}

void buildMixer (const std::vector<Event>& events, FLProject& out)
{
    const bool legacy = out.legacyText;
    FLInsert pending;
    FLSlot pendingSlot;
    bool inMixerSection = false;

    struct ParamRecord { int id, insertIdx, slotIdx, msg; };
    std::vector<ParamRecord> params;

    for (auto& ev : events)
    {
        if (ev.opcode == OP_INSERT_END && ! ev.isBlob)
        {
            inMixerSection = true;
            if (pendingSlot.hasPlugin)
                pending.slots.push_back (pendingSlot);
            out.inserts.push_back (std::move (pending));
            pending = {};
            pending.index = (int) out.inserts.size();
            pendingSlot = {};
            continue;
        }
        if (ev.opcode == OP_INSERT_COLOUR && ! ev.isBlob)
        {
            inMixerSection = true;
            pending.colour = colourFromBgr (ev.value);
            continue;
        }
        if (ev.opcode == OP_NEW_SLOT && ! ev.isBlob)
        {
            inMixerSection = true;
            pendingSlot.index = (int) ev.value;
            pending.slots.push_back (pendingSlot);
            pendingSlot = {};
            continue;
        }
        if (ev.opcode == OP_INSERT_NAME && ev.isBlob)
        {
            inMixerSection = true;
            if (pending.name.isEmpty())
                pending.name = decodeText (ev.blob, legacy);
            continue;
        }
        if (ev.opcode == OP_INSERT_FLAGS && ev.isBlob && ev.blob.size() >= 12)
        {
            inMixerSection = true;
            auto raw = u32At (ev.blob, 4);
            pending.muted = (raw & (1u << 3)) == 0;
            pending.solo  = (raw & (1u << 12)) != 0;
            continue;
        }
        if (ev.opcode == OP_NAME && ev.isBlob && inMixerSection)
        {
            if (pendingSlot.pluginName.isEmpty())
                pendingSlot.pluginName = decodeText (ev.blob, legacy);
            continue;
        }
        if (ev.opcode == OP_PLUGIN_INTERNAL && ev.isBlob && inMixerSection)
        {
            if (pendingSlot.internalName.isEmpty())
                pendingSlot.internalName = decodeText (ev.blob, legacy);
            continue;
        }
        if (ev.opcode == OP_PLUGIN_STATE && ev.isBlob && inMixerSection)
        {
            pendingSlot.hasPlugin = true;
            if (pendingSlot.internalName == "Fruity Wrapper" && ! pendingSlot.vst.valid)
                pendingSlot.vst = decodeWrapper (ev.blob);
            else if (pendingSlot.internalName != "Fruity Wrapper")
                pendingSlot.nativeState = ev.blob;
            continue;
        }
        if (ev.opcode == OP_MIXER_PARAMS && ev.isBlob && ev.blob.size() % 12 == 0)
        {
            for (size_t p = 0; p + 12 <= ev.blob.size(); p += 12)
            {
                auto tgt = u16At (ev.blob, p + 6);
                int base = tgt >= 0x7000 ? 0x7000 : tgt >= 0x2000 ? 0x2000 : -1;
                if (base < 0)
                    continue;
                const int off = tgt - base;
                const int insert = off / 0x40;
                if (insert > 126)
                    continue;
                params.push_back ({ (int) ev.blob[p + 4], insert, off % 0x40,
                                    (int32_t) u32At (ev.blob, p + 8) });
            }
            continue;
        }
    }

    for (auto& rec : params)
    {
        if (rec.insertIdx < 0 || rec.insertIdx >= (int) out.inserts.size())
            continue;
        auto& ins = out.inserts[(size_t) rec.insertIdx];
        if (rec.id == 192)      { ins.rawVolume = rec.msg;    continue; }
        if (rec.id == 193)      { ins.rawPan = rec.msg;       continue; }
        if (rec.id == 194)      { ins.rawStereoSep = rec.msg; continue; }
        if (rec.id != 0 && rec.id != 1)
            continue;

        for (auto& slot : ins.slots)
            if (slot.index == rec.slotIdx)
            {
                if (rec.id == 0) slot.enabled = rec.msg != 0;
                else             slot.mix = rec.msg;
            }
    }
}

void buildPatterns (const std::vector<Event>& events, FLProject& out)
{
    std::map<int, size_t> byId;
    FLPattern* current = nullptr;

    auto get = [&] (int id) -> FLPattern&
    {
        auto it = byId.find (id);
        if (it == byId.end())
        {
            byId[id] = out.patterns.size();
            out.patterns.push_back ({});
            out.patterns.back().id = id;
            return out.patterns.back();
        }
        return out.patterns[it->second];
    };

    for (auto& ev : events)
    {
        if (ev.opcode == OP_PATTERN_NEW && ! ev.isBlob)
        {
            current = &get ((int) ev.value);
            continue;
        }
        if (current == nullptr)
            continue;

        if (ev.opcode == OP_PATTERN_NAME && ev.isBlob)
        {
            if (current->name.isEmpty())
                current->name = decodeText (ev.blob, out.legacyText);
        }
        else if (ev.opcode == OP_PATTERN_COLOUR && ! ev.isBlob)
        {
            if (! isFlDefaultColour (ev.value))
                current->colour = clipColourFromBgr (ev.value);
        }
        else if (ev.opcode == OP_PATTERN_LENGTH && ! ev.isBlob)
        {
            current->lengthTicks = (int) ev.value;
        }
        else if ((ev.opcode == OP_PATTERN_NOTES || ev.opcode == OP_PATTERN_NOTES_OLD)
                 && ev.isBlob && ! ev.blob.empty() && ev.blob.size() % 24 == 0)
        {
            for (size_t p = 0; p + 24 <= ev.blob.size(); p += 24)
            {
                FLNote n;
                n.pos      = (int) u32At (ev.blob, p);
                n.chanIid  = (int) u16At (ev.blob, p + 6);
                n.len      = (int) u32At (ev.blob, p + 8);
                n.key      = (int) u16At (ev.blob, p + 12);
                n.vel      = (int) ev.blob[p + 21];
                current->notes.push_back (n);
            }
        }
    }
}

float fadeTimeMs (const std::vector<uint8_t>& blob, size_t offset)
{
    float ms = 0.0f;
    memcpy (&ms, blob.data() + offset, 4);
    return (std::isfinite (ms) && ms > 0.05f && ms < 600000.0f) ? ms : 0.0f;
}

void buildArrangements (const std::vector<Event>& events, FLProject& out)
{
    FLArrangement* current = nullptr;

    int preferredClipSize = out.versionMajor >= 26 ? 88 : out.versionMajor >= 25 ? 80
                          : out.versionMajor >= 21 ? 60 : 32;

    for (auto& ev : events)
    {
        if (ev.opcode == OP_ARRANGEMENT_NEW && ! ev.isBlob)
        {
            out.arrangements.push_back ({});
            current = &out.arrangements.back();
            continue;
        }
        if (current == nullptr)
        {
            if (ev.opcode != OP_PLAYLIST && ev.opcode != OP_TRACK_DATA)
                continue;
            out.arrangements.push_back ({});
            current = &out.arrangements.back();
        }

        if (ev.opcode == OP_TRACK_DATA && ev.isBlob)
        {
            FLTrack t;
            if (ev.blob.size() >= 13)
            {
                auto packed = u32At (ev.blob, 4);
                if (! isFlDefaultColour (packed))
                    t.colour = colourFromBgr (packed);
                t.enabled = ev.blob[12] != 0;
            }
            current->tracks.push_back (t);
        }
        else if (ev.opcode == OP_TRACK_NAME && ev.isBlob)
        {
            if (! current->tracks.empty())
                current->tracks.back().name = decodeText (ev.blob, out.legacyText);
        }
        else if (ev.opcode == OP_PLAYLIST && ev.isBlob && ! ev.blob.empty())
        {
            size_t rec = 0;
            if      (ev.blob.size() % (size_t) preferredClipSize == 0) rec = (size_t) preferredClipSize;
            else if (ev.blob.size() % 88 == 0) rec = 88;
            else if (ev.blob.size() % 80 == 0) rec = 80;
            else if (ev.blob.size() % 60 == 0) rec = 60;
            else if (ev.blob.size() % 32 == 0) rec = 32;
            if (rec == 0)
                continue;

            for (size_t p = 0; p + rec <= ev.blob.size(); p += rec)
            {
                FLClip c;
                c.posTicks   = (int) u32At (ev.blob, p);
                c.itemIndex  = (int) u16At (ev.blob, p + 6);
                c.lenTicks   = (int) u32At (ev.blob, p + 8);
                c.trackRvidx = (int) u16At (ev.blob, p + 12);
                uint32_t rawOff = u32At (ev.blob, p + 24);
                c.startOffsetTicks = (int) (juce::int32) rawOff;
                memcpy (&c.startOffsetMs, &rawOff, 4);
                if (rec >= 60)
                {
                    c.fadeInMs  = fadeTimeMs (ev.blob, p + 36);
                    c.fadeOutMs = fadeTimeMs (ev.blob, p + 44);
                }
                if (rec >= 80)
                {
                    double s = 0.0;
                    uint64_t bits = u64At (ev.blob, p + 64);
                    memcpy (&s, &bits, 8);
                    if (std::isfinite (s) && s > 0.0)
                        c.stretchScale = s;
                }
                if (c.trackRvidx <= MAX_TRACK_IDX && c.lenTicks > 0)
                    current->clips.push_back (c);
            }
        }
    }
}

juce::StringArray tokenBaseDirs (const juce::String& token)
{
    juce::StringArray out;
    auto addChildren = [&out] (const juce::File& dir, const juce::String& wildcard)
    {
        for (auto& f : dir.findChildFiles (juce::File::findDirectories, false, wildcard))
            out.add (f.getFullPathName());
    };

    if (token == "FLStudioFactoryData")
    {
        auto programs = juce::File::getSpecialLocation (juce::File::globalApplicationsDirectory);
        addChildren (programs.getChildFile ("Image-Line"), "FL Studio*");
    }
    else if (token == "FLStudioUserData")
    {
        auto docs = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
        auto imageLine = docs.getChildFile ("Image-Line");
        out.add (imageLine.getFullPathName());
        addChildren (imageLine, "*");
    }
    auto env = juce::SystemStats::getEnvironmentVariable (token, {});
    if (env.isNotEmpty())
        out.add (env);
    return out;
}

juce::String resolveSamplePath (const juce::String& flPath, juce::StringArray& missing,
                                const juce::Array<juce::File>& extraSearchDirs = {})
{
    if (flPath.isEmpty())
        return {};

    auto path = flPath.replaceCharacter ('/', '\\');
    if (path.startsWith ("%"))
    {
        int close = path.indexOfChar (1, '%');
        if (close > 1)
        {
            auto token = path.substring (1, close);
            auto rest = path.substring (close + 1);
            for (auto& base : tokenBaseDirs (token))
            {
                juce::File f (base + rest);
                if (f.existsAsFile())
                    return f.getFullPathName();
            }
        }
    }
    else if (juce::File::isAbsolutePath (path))
    {
        if (juce::File (path).existsAsFile())
            return path;
    }

    auto leafName = juce::File (path).getFileName();
    for (auto& dir : extraSearchDirs)
    {
        auto candidate = dir.getChildFile (leafName);
        if (candidate.existsAsFile())
            return candidate.getFullPathName();
    }

    missing.addIfNotAlreadyThere (path);
    return path;
}

int formatRank (const juce::PluginDescription& d, bool preferVst3)
{
    if (Bridge32::isBridged (d))
        return 0;
    const bool isVst3 = d.pluginFormatName == "VST3";
    return isVst3 == preferVst3 ? 2 : 1;
}

bool pickBest (const juce::Array<juce::PluginDescription>& types, bool preferVst3,
               const std::function<bool (const juce::PluginDescription&)>& accept,
               juce::PluginDescription& out)
{
    int best = -1;
    for (auto& d : types)
    {
        if (! accept (d))
            continue;
        const int rank = formatRank (d, preferVst3);
        if (rank > best)
        {
            best = rank;
            out = d;
        }
    }
    return best >= 0;
}

bool preferNativeTwin (PluginHost& host, juce::PluginDescription& desc)
{
    if (! Bridge32::isBridged (desc))
        return false;

    const juce::File bridgedFile (desc.fileOrIdentifier);
    const juce::String stem = bridgedFile.getFileNameWithoutExtension();
    juce::StringArray twins { stem + "_x64", stem + "-x64", stem + " x64", stem + "64", stem + "_64" };
    if (stem.endsWithIgnoreCase ("_x86"))
        twins.add (stem.dropLastCharacters (4));

    auto isTwin = [&] (const juce::PluginDescription& d)
    {
        if (Bridge32::isBridged (d))
            return false;
        if (desc.uniqueId != 0 && d.uniqueId == desc.uniqueId)
            return true;
        const auto candidate = juce::File::createFileWithoutCheckingPath (d.fileOrIdentifier)
                                   .getFileNameWithoutExtension();
        for (auto& t : twins)
            if (candidate.equalsIgnoreCase (t))
                return true;
        return false;
    };

    const juce::File dir = bridgedFile.getParentDirectory();
    for (bool sameDirOnly : { true, false })
        for (auto* list : { &host.knownPlugins, &host.unverifiedPlugins })
            for (auto& d : list->getTypes())
                if (isTwin (d)
                    && (! sameDirOnly
                        || juce::File::createFileWithoutCheckingPath (d.fileOrIdentifier)
                               .getParentDirectory() == dir))
                {
                    desc = d;
                    return true;
                }
    return false;
}

bool matchVst (PluginHost& host, const WrapperInfo& w, juce::PluginDescription& out)
{
    auto types = host.knownPlugins.getTypes();
    const bool preferVst3 = w.path.endsWithIgnoreCase (".vst3");
    const bool hasPath = w.path.isNotEmpty() && juce::File::isAbsolutePath (w.path);
    const juce::File wFile = hasPath ? juce::File (w.path) : juce::File();

    auto samePath = [&] (const juce::PluginDescription& d)
    {
        return hasPath && juce::File::isAbsolutePath (d.fileOrIdentifier)
            && juce::File (d.fileOrIdentifier) == wFile;
    };

    if (pickBest (types, preferVst3, samePath, out))
        return true;

    if (w.hasUid && w.uid != 0)
    {
        const int candidates[] = { (int) w.uid, (int) juce::ByteOrder::swap (w.uid) };
        for (int uid : candidates)
            if (pickBest (types, preferVst3,
                          [uid] (const juce::PluginDescription& d)
                          { return d.uniqueId == uid || d.deprecatedUid == uid; }, out))
            {
                preferNativeTwin (host, out);
                return true;
            }
    }

    auto sameName = [&] (const juce::PluginDescription& d)
    {
        return w.name.isNotEmpty() && d.name.equalsIgnoreCase (w.name);
    };

    if (pickBest (types, preferVst3, sameName, out))
    {
        preferNativeTwin (host, out);
        return true;
    }

    auto unverified = host.unverifiedPlugins.getTypes();
    if (! unverified.isEmpty())
    {
        const auto stem = [] (const juce::String& p)
        {
            return juce::File::createFileWithoutCheckingPath (p)
                       .getFileNameWithoutExtension().toLowerCase();
        };
        const juce::String wStem = stem (w.path);

        if (pickBest (unverified, preferVst3, samePath, out))
            return true;
        if (pickBest (unverified, preferVst3, sameName, out))
        {
            preferNativeTwin (host, out);
            return true;
        }
        if (wStem.isNotEmpty()
            && pickBest (unverified, preferVst3,
                         [&] (const juce::PluginDescription& d) { return stem (d.fileOrIdentifier) == wStem; },
                         out))
        {
            preferNativeTwin (host, out);
            return true;
        }
    }
    return false;
}

juce::MemoryBlock vst3StateFromWrapper (const juce::MemoryBlock& flState)
{
    constexpr size_t headerSize = 0x50;
    const auto size = flState.getSize();
    if (size < headerSize + 12)
        return {};

    auto* d = (const uint8_t*) flState.getData();
    if (u32At (d, 0) != 1 || u32At (d, 4) != 1 || u64At (d, 8) != 0x40 || u32At (d, 16) != 1)
        return {};

    juce::MemoryBlock component, controller;
    for (size_t pos = headerSize; pos + 12 <= size;)
    {
        auto id  = u32At (d, pos);
        auto len = u64At (d, pos + 4);
        if (len > (uint64_t) (size - pos - 12))
            return {};

        if (len > 0)
        {
            if (id == 3)      component  = juce::MemoryBlock (d + pos + 12, (size_t) len);
            else if (id == 4) controller = juce::MemoryBlock (d + pos + 12, (size_t) len);
        }
        pos += 12 + (size_t) len;
    }

    if (component.getSize() == 0)
        return {};

    juce::XmlElement state ("VST3PluginState");
    state.createNewChildElement ("IComponent")->addTextElement (component.toBase64Encoding());
    if (controller.getSize() > 0)
        state.createNewChildElement ("IEditController")->addTextElement (controller.toBase64Encoding());

    juce::MemoryBlock out;
    {
        juce::MemoryOutputStream mo (out, false);
        mo.writeInt (0x21324356);
        mo.writeInt (0);
        state.writeTo (mo, juce::XmlElement::TextFormat().singleLine());
        mo.writeByte (0);
    }
    static_cast<juce::uint32*> (out.getData())[1]
        = juce::ByteOrder::swapIfBigEndian ((juce::uint32) out.getSize() - 9);
    return out;
}

juce::String importableStateB64 (const WrapperInfo& w, const juce::PluginDescription& desc)
{
    const bool bridged = Bridge32::isBridged (desc);
    if ((desc.pluginFormatName == "VST" || bridged) && w.state.getSize() > 0)
    {
        if (Vst2Fxb::isFxBank (w.state.getData(), w.state.getSize()))
            return bridged ? juce::String()
                           : juce::Base64::toBase64 (w.state.getData(), w.state.getSize());

        auto* raw = (const uint8_t*) w.state.getData();
        auto rawLen = w.state.getSize();
        if (rawLen > 21 && u32At (raw, 0) == 0xfffffff7u && u32At (raw, 5) == 0xfffffffeu)
        {
            auto chunkLen = (size_t) u32At (raw, 9);
            if (21 + chunkLen <= rawLen)
            {
                raw += 21;
                rawLen = chunkLen;
            }
        }

        if (bridged)
        {
            auto framed = Bridge32::wrapChunkState (raw, rawLen);
            return framed.getSize() > 0 ? juce::Base64::toBase64 (framed.getData(), framed.getSize())
                                        : juce::String();
        }

        auto uid = w.hasUid && w.uid != 0 ? w.uid : (juce::uint32) desc.uniqueId;
        auto bank = Vst2Fxb::wrapChunk (raw, rawLen, uid);
        if (bank.getSize() > 0)
            return juce::Base64::toBase64 (bank.getData(), bank.getSize());
        return {};
    }

    if (desc.pluginFormatName == "VST3" && w.state.getSize() > 0)
    {
        auto blob = vst3StateFromWrapper (w.state);
        if (blob.getSize() > 0)
            return juce::Base64::toBase64 (blob.getData(), blob.getSize());
    }

    return {};
}

struct SkipCounter
{
    juce::StringArray names;
    void add (const juce::String& n) { names.addIfNotAlreadyThere (n.isEmpty() ? "(unnamed)" : n); }
    bool any() const { return ! names.isEmpty(); }
    juce::String list() const { return names.joinIntoString (", "); }
};

juce::ValueTree convert (const FLProject& fl, PluginHost& host, const juce::File& file,
                         juce::String& summary, juce::StringArray& warnings,
                         const juce::Array<juce::File>& extraSearchDirs = {})
{
    auto p = Model::createEmptyProject();
    p.setProperty (IDs::name, fl.title.isNotEmpty() ? fl.title : file.getFileNameWithoutExtension(), nullptr);
    p.setProperty (IDs::bpm, fl.tempo > 0.0 ? fl.tempo : 140.0, nullptr);

    const double scale = (double) Model::PPQ / juce::jmax (1, fl.ppq);
    juce::StringArray missingSamples;
    SkipCounter nativeInstruments, nativeEffects, missingVsts;

    const bool anyVstNeeded = [&fl]
    {
        for (auto& ch : fl.channels) if (ch.vst.valid) return true;
        for (auto& ins : fl.inserts) for (auto& s : ins.slots) if (s.vst.valid) return true;
        return false;
    }();
    if (anyVstNeeded && host.knownPlugins.getNumTypes() == 0)
        warnings.add ("Plugin cache is empty - scan plugins in the Plugin Manager and re-import to link VSTs.");

    std::map<int, int> insertMap;
    insertMap[0] = 0;

    std::set<int> usedInserts;
    for (auto& ch : fl.channels)
        if (ch.targetInsert > 0)
            usedInserts.insert (ch.targetInsert);
    for (auto& ins : fl.inserts)
    {
        if (ins.index == 0)
            continue;
        bool hasFx = false;
        for (auto& s : ins.slots)
            if (s.hasPlugin) { hasFx = true; break; }
        if (hasFx || ins.name.isNotEmpty() || ins.colour.isNotEmpty()
            || ins.rawVolume != 12800 || ins.rawPan != 0 || ins.rawStereoSep != 0
            || ins.muted || ins.solo)
            usedInserts.insert (ins.index);
    }

    int nextTrack = 1;
    int overflowed = 0;
    int maxTarget = 0;
    for (int flIdx : usedInserts)
    {
        if (nextTrack > Model::maxMixerTracks)
        {
            ++overflowed;
            insertMap[flIdx] = 0;
            continue;
        }
        insertMap[flIdx] = nextTrack;
        maxTarget = juce::jmax (maxTarget, nextTrack);
        ++nextTrack;
    }

    while (Model::getMaxMixerTrackIndex (p) < maxTarget)
        if (! Model::addMixerTrack (p).isValid())
            break;

    if (overflowed > 0)
        warnings.add (juce::String (overflowed) + " mixer inserts didn't fit (max "
                      + juce::String (Model::maxMixerTracks) + ") and were routed to Master.");

    int fxImported = 0;
    for (auto& ins : fl.inserts)
    {
        auto it = insertMap.find (ins.index);
        if (it == insertMap.end() || (ins.index != 0 && it->second == 0))
            continue;

        auto track = Model::getMixerTrack (p, it->second);
        if (! track.isValid())
            continue;

        if (ins.name.isNotEmpty() && ins.index != 0)
            track.setProperty (IDs::name, ins.name, nullptr);
        if (ins.colour.isNotEmpty())
            track.setProperty (IDs::colour, ins.colour, nullptr);
        track.setProperty (IDs::volume, juce::jlimit (0.0, 1.25, ins.rawVolume / 12800.0), nullptr);
        track.setProperty (IDs::pan, juce::jlimit (-1.0, 1.0, ins.rawPan / 6400.0), nullptr);
        track.setProperty (IDs::stereoSep, juce::jlimit (-1.0, 1.0, ins.rawStereoSep / 64.0), nullptr);
        if (ins.muted) track.setProperty (IDs::mute, true, nullptr);
        if (ins.solo)  track.setProperty (IDs::solo, true, nullptr);

        for (auto& slot : ins.slots)
        {
            if (! slot.hasPlugin)
                continue;
            if (! slot.vst.valid)
            {
                FLNativeFx::Imported native;
                if (FLNativeFx::importSlot (slot.internalName, slot.nativeState, native))
                {
                    juce::ValueTree s (IDs::Slot);
                    s.setProperty (IDs::uid, Model::newId(), nullptr);
                    s.setProperty (IDs::slotIndex, juce::jlimit (0, Model::numSlotsPerTrack - 1, slot.index), nullptr);
                    s.setProperty (IDs::enabled, slot.enabled, nullptr);
                    s.setProperty (IDs::mix, juce::jlimit (0.0, 1.0, slot.mix / 12800.0), nullptr);
                    s.setProperty (IDs::type, native.type, nullptr);
                    s.setProperty (IDs::pluginState, native.stateB64, nullptr);
                    track.appendChild (s, nullptr);
                    ++fxImported;
                    continue;
                }
                nativeEffects.add (slot.internalName.isNotEmpty() ? slot.internalName : slot.pluginName);
                continue;
            }
            juce::PluginDescription desc;
            if (! matchVst (host, slot.vst, desc))
            {
                missingVsts.add (slot.vst.name);
                continue;
            }
            juce::ValueTree s (IDs::Slot);
            s.setProperty (IDs::uid, Model::newId(), nullptr);
            s.setProperty (IDs::slotIndex, juce::jlimit (0, Model::numSlotsPerTrack - 1, slot.index), nullptr);
            s.setProperty (IDs::enabled, slot.enabled, nullptr);
            s.setProperty (IDs::mix, juce::jlimit (0.0, 1.0, slot.mix / 12800.0), nullptr);
            s.setProperty (IDs::type, "plugin", nullptr);
            if (auto xml = desc.createXml())
                s.setProperty (IDs::pluginDesc, xml->toString(), nullptr);
            auto state = importableStateB64 (slot.vst, desc);
            if (state.isNotEmpty())
                s.setProperty (IDs::pluginState, state, nullptr);
            track.appendChild (s, nullptr);
            ++fxImported;
        }
    }

    {
        auto mixer = Model::getMixer (p);
        bool anySolo = false;
        for (auto t : mixer)
            if (t.hasType (IDs::Track) && (int) t.getProperty (IDs::index, 0) != 0
                && (bool) t.getProperty (IDs::solo, false))
            { anySolo = true; break; }
        if (anySolo)
            for (auto t : mixer)
                if (t.hasType (IDs::Track) && (int) t.getProperty (IDs::index, 0) != 0
                    && ! (bool) t.getProperty (IDs::solo, false))
                    t.setProperty (IDs::mute, true, nullptr);
    }

    std::map<int, int> chanMap;
    std::map<int, juce::String> chanSample;
    int automationSkipped = 0, layersSkipped = 0;

    std::map<int, double> chanStretchFold;
    {
        std::map<int, double> seen;
        std::set<int> conflicting;
        for (auto& arr : fl.arrangements)
            for (auto& c : arr.clips)
            {
                if (c.stretchScale <= 0.0 || c.itemIndex > PATTERN_BASE)
                    continue;
                auto it = seen.find (c.itemIndex);
                if (it == seen.end())
                    seen[c.itemIndex] = c.stretchScale;
                else if (std::fabs (it->second - c.stretchScale) > 1.0e-9)
                    conflicting.insert (c.itemIndex);
            }

        int mixedScales = 0, scaleWithoutStretch = 0;
        for (auto& [iid, s] : seen)
        {
            if (conflicting.count (iid) != 0) { ++mixedScales; continue; }
            if (std::fabs (s - 1.0) <= 1.0e-12)
                continue;

            const FLChannel* owner = nullptr;
            for (auto& candidate : fl.channels)
                if (candidate.iid == iid) { owner = &candidate; break; }

            if (owner == nullptr || owner->stretchBeats <= 0.0) { ++scaleWithoutStretch; continue; }

            chanStretchFold[iid] = s;
        }
        if (mixedScales > 0)
            warnings.add (juce::String (mixedScales) + " channel(s) use different per-clip stretch"
                          " scales; their clips play at the unscaled stretch time.");
        if (scaleWithoutStretch > 0)
            warnings.add (juce::String (scaleWithoutStretch) + " channel(s) have a per-clip stretch"
                          " scale but no sampler stretch time; the scale was not applied.");
    }

    for (auto& ch : fl.channels)
    {
        if (ch.kind == 5) { ++automationSkipped; continue; }
        if (ch.kind == 3) { ++layersSkipped; continue; }

        auto name = ch.name.isNotEmpty() ? ch.name
                  : ch.vst.name.isNotEmpty() ? ch.vst.name
                  : "Channel " + juce::String (ch.iid + 1);

        juce::ValueTree node;
        if (ch.vst.valid)
        {
            juce::PluginDescription desc;
            if (matchVst (host, ch.vst, desc))
            {
                juce::String descXml;
                if (auto xml = desc.createXml())
                    descXml = xml->toString();
                node = Model::addPluginChannel (p, name, descXml);
                auto state = importableStateB64 (ch.vst, desc);
                if (state.isNotEmpty())
                    node.setProperty (IDs::pluginState, state, nullptr);
            }
            else
            {
                missingVsts.add (ch.vst.name);
                node = Model::addSamplerChannel (p, name, {});
            }
        }
        else if (ch.pluginInternalName.isNotEmpty() && ch.samplePath.isEmpty())
        {
            nativeInstruments.add (ch.pluginInternalName);
            node = Model::addSamplerChannel (p, name, {});
        }
        else
        {
            auto resolved = resolveSamplePath (ch.samplePath, missingSamples, extraSearchDirs);
            node = Model::addSamplerChannel (p, name, resolved);
            chanSample[ch.iid] = resolved;
            if (ch.stretchBeats > 0.0 && resolved.isNotEmpty())
            {
                double beats = ch.stretchBeats;
                auto fold = chanStretchFold.find (ch.iid);
                if (fold != chanStretchFold.end())
                    beats *= fold->second;
                node.setProperty (IDs::stretchTempoMode, 3, nullptr);
                node.setProperty (IDs::stretchBeats, juce::jlimit (0.0, 1024.0, beats), nullptr);
            }
            if (ch.cutGroup != 0 && ch.cutGroup == ch.cutByGroup)
                node.setProperty (IDs::cutItself, true, nullptr);
        }

        if (ch.hasLevels)
        {
            node.setProperty (IDs::volume, juce::jlimit (0.0, 1.0, ch.rawVolume / 12800.0), nullptr);
            node.setProperty (IDs::pan, juce::jlimit (-1.0, 1.0, (ch.rawPan - 6400) / 6400.0), nullptr);
        }
        if (! ch.enabled)
            node.setProperty (IDs::mute, true, nullptr);

        int target = 0;
        if (ch.targetInsert > 0)
        {
            auto it = insertMap.find (ch.targetInsert);
            if (it != insertMap.end())
                target = it->second;
        }
        node.setProperty (IDs::mixerTrack, target, nullptr);

        chanMap[ch.iid] = (int) node.getProperty (IDs::id);
    }

    std::map<int, int> patMap;
    std::map<int, double> patBeats;
    int notesImported = 0, notesDropped = 0;

    for (auto& flp : fl.patterns)
    {
        auto pat = Model::addPattern (p, flp.name);
        if (flp.colour.isNotEmpty())
            pat.setProperty (IDs::colour, flp.colour, nullptr);

        int ticks = flp.lengthTicks > 0 ? juce::roundToInt (flp.lengthTicks * scale) : 0;
        if (ticks == 0)
            for (auto& n : flp.notes)
                ticks = juce::jmax (ticks, juce::roundToInt ((n.pos + juce::jmax (n.len, 1)) * scale));

        int steps = (ticks + Model::ticksPerStep - 1) / Model::ticksPerStep;
        steps = juce::jmax (Model::defaultSteps, ((steps + 3) / 4) * 4);
        pat.setProperty (IDs::lengthSteps, steps, nullptr);

        std::map<int, std::vector<const FLNote*>> byChannel;
        for (auto& n : flp.notes)
            byChannel[n.chanIid].push_back (&n);

        for (auto& [iid, notes] : byChannel)
        {
            auto cm = chanMap.find (iid);
            if (cm == chanMap.end())
            {
                notesDropped += (int) notes.size();
                continue;
            }
            auto seq = Model::getChannelSeq (pat, cm->second, true);

            bool stepLike = true;
            for (auto* n : notes)
            {
                int pos = juce::roundToInt (n->pos * scale);
                int len = juce::roundToInt (n->len * scale);
                if (pos % Model::ticksPerStep != 0 || len > Model::ticksPerStep
                    || pos / Model::ticksPerStep >= steps
                    || n->key != notes.front()->key || n->vel != notes.front()->vel)
                {
                    stepLike = false;
                    break;
                }
            }

            if (stepLike)
            {
                auto stepStr = Model::emptySteps (steps);
                for (auto* n : notes)
                    stepStr = stepStr.replaceSection (juce::roundToInt (n->pos * scale) / Model::ticksPerStep, 1, "1");
                seq.setProperty (IDs::steps, stepStr, nullptr);
                notesImported += (int) notes.size();
            }
            else
            {
                seq.setProperty (IDs::usePianoRoll, true, nullptr);
                for (auto* n : notes)
                {
                    juce::ValueTree note (IDs::Note);
                    note.setProperty (IDs::key, juce::jlimit (0, 127, n->key), nullptr);
                    note.setProperty (IDs::start, juce::jmax (0, juce::roundToInt (n->pos * scale)), nullptr);
                    note.setProperty (IDs::length, juce::jmax (1, juce::roundToInt (n->len * scale)), nullptr);
                    note.setProperty (IDs::velocity, juce::jlimit (1, 127, n->vel), nullptr);
                    seq.appendChild (note, nullptr);
                    ++notesImported;
                }
            }
        }

        patMap[flp.id] = (int) pat.getProperty (IDs::id);
        patBeats[flp.id] = steps / (double) Model::stepsPerBeat;
    }

    int clipsImported = 0, clipsDropped = 0;
    if (! fl.arrangements.empty())
    {
        if (fl.arrangements.size() > 1)
            warnings.add ("Project has " + juce::String (fl.arrangements.size())
                          + " arrangements; only the first was imported.");

        auto& arr = fl.arrangements.front();

        const int trackCount = fl.versionMajor >= 20 ? MAX_TRACK_IDX + 1
                             : fl.versionMajor >= 12 ? 199 : 99;

        std::set<int> usedLanes;
        for (auto& c : arr.clips)
        {
            int lane = trackCount - 1 - c.trackRvidx;
            if (lane >= 0)
                usedLanes.insert (lane);
        }

        std::map<int, int> laneMap;
        if (! usedLanes.empty() && *usedLanes.rbegin() < 64)
        {
            for (int lane : usedLanes)
                laneMap[lane] = lane;
        }
        else
        {
            int next = 0;
            for (int lane : usedLanes)
                laneMap[lane] = next++;
        }

        juce::AudioFormatManager audioFormats;
        audioFormats.registerBasicFormats();
        std::map<juce::String, double> sourceSecondsCache;
        auto sourceSeconds = [&] (const juce::String& path) -> double
        {
            auto it = sourceSecondsCache.find (path);
            if (it != sourceSecondsCache.end())
                return it->second;
            double secs = 0.0;
            if (auto reader = std::unique_ptr<juce::AudioFormatReader> (
                    audioFormats.createReaderFor (juce::File (path))))
                if (reader->sampleRate > 0.0)
                    secs = (double) reader->lengthInSamples / reader->sampleRate;
            return sourceSecondsCache[path] = secs;
        };

        auto playlist = Model::getPlaylist (p);
        for (auto& c : arr.clips)
        {
            int lane = trackCount - 1 - c.trackRvidx;
            if (lane < 0)
                continue;

            juce::ValueTree clip (IDs::Clip);
            clip.setProperty (IDs::uid, Model::newId(), nullptr);
            if (c.itemIndex > PATTERN_BASE)
            {
                auto pm = patMap.find (c.itemIndex - PATTERN_BASE);
                if (pm == patMap.end())
                {
                    ++clipsDropped;
                    continue;
                }
                clip.setProperty (IDs::type, "pattern", nullptr);
                clip.setProperty (IDs::patternId, pm->second, nullptr);
                if (c.startOffsetTicks > 0)
                    clip.setProperty (IDs::offsetBeats,
                                      c.startOffsetTicks / (double) fl.ppq, nullptr);
            }
            else
            {
                auto cs = chanSample.find (c.itemIndex);
                if (cs == chanSample.end() || cs->second.isEmpty())
                {
                    ++clipsDropped;
                    continue;
                }
                clip.setProperty (IDs::type, "audio", nullptr);
                clip.setProperty (IDs::path, cs->second, nullptr);
                if (c.startOffsetMs > 0.0f && fl.tempo > 0.0)
                    clip.setProperty (IDs::offsetBeats,
                                      c.startOffsetMs / 60000.0 * fl.tempo, nullptr);
                if (c.fadeInMs > 0.0f || c.fadeOutMs > 0.0f)
                {
                    const double secs = sourceSeconds (cs->second);
                    if (secs > 0.0)
                    {
                        if (c.fadeInMs > 0.0f)
                            clip.setProperty (IDs::fadeIn, juce::jlimit (0.0, 1.0,
                                c.fadeInMs / 1000.0 / (0.5 * secs)), nullptr);
                        if (c.fadeOutMs > 0.0f)
                            clip.setProperty (IDs::fadeOut, juce::jlimit (0.0, 1.0,
                                c.fadeOutMs / 1000.0 / (0.5 * secs)), nullptr);
                    }
                }
            }
            clip.setProperty (IDs::track, laneMap[lane], nullptr);
            clip.setProperty (IDs::startBeats, c.posTicks / (double) fl.ppq, nullptr);
            clip.setProperty (IDs::lengthBeats, juce::jmax (0.25, c.lenTicks / (double) fl.ppq), nullptr);
            playlist.appendChild (clip, nullptr);
            ++clipsImported;
        }

        Model::ensurePlaylistTracks (p);
        for (auto& [flLane, ourLane] : laneMap)
        {
            if (flLane >= (int) arr.tracks.size())
                continue;
            auto t = Model::getPlaylistTrack (p, ourLane);
            if (! t.isValid())
                continue;
            auto& src = arr.tracks[(size_t) flLane];
            if (src.name.isNotEmpty())
                t.setProperty (IDs::name, src.name, nullptr);
            if (src.colour.isNotEmpty())
                t.setProperty (IDs::colour, src.colour, nullptr);
            if (! src.enabled)
                t.setProperty (IDs::enabled, false, nullptr);
        }
    }

    summary = "Imported " + juce::String (chanMap.size()) + " channels, "
            + juce::String (patMap.size()) + " patterns ("
            + juce::String (notesImported) + " notes), "
            + juce::String (clipsImported) + " playlist clips, "
            + juce::String (fxImported) + " mixer effects.";

    if (nativeInstruments.any())
        warnings.add ("FL-native instruments have no VST equivalent (channels kept, silent): "
                      + nativeInstruments.list());
    if (nativeEffects.any())
        warnings.add ("FL-native effects skipped: " + nativeEffects.list());
    if (missingVsts.any())
        warnings.add ("VSTs not found in the plugin cache: " + missingVsts.list());
    if (! missingSamples.isEmpty())
        warnings.add (juce::String (missingSamples.size()) + " sample file(s) not found on disk, e.g. "
                      + missingSamples[0]);
    if (automationSkipped > 0)
        warnings.add (juce::String (automationSkipped) + " automation clip(s) skipped (not supported).");
    if (layersSkipped > 0)
        warnings.add (juce::String (layersSkipped) + " layer channel(s) skipped.");
    if (notesDropped > 0)
        warnings.add (juce::String (notesDropped) + " note(s) referenced skipped channels and were dropped.");
    if (clipsDropped > 0)
        warnings.add (juce::String (clipsDropped) + " playlist clip(s) referenced skipped content and were dropped.");

    return p;
}

}

static FLPImport::Result importFlpData (const juce::MemoryBlock& data, const juce::File& file,
                                        PluginHost& host,
                                        const juce::Array<juce::File>& extraSearchDirs = {})
{
    FLPImport::Result result;

    juce::MemoryInputStream in (data, false);
    char magic[4] = {};
    in.read (magic, 4);

    if (memcmp (magic, "FLhd", 4) != 0)
    {
        result.error = "Not an FL Studio project file: " + file.getFileName();
        return result;
    }

    FLProject fl;
    auto headerLen = (uint32_t) in.readInt();
    auto headerEnd = in.getPosition() + headerLen;
    in.readShort();
    in.readShort();
    fl.ppq = juce::jmax (1, (int) (uint16_t) in.readShort());
    in.setPosition (headerEnd);

    in.read (magic, 4);
    if (memcmp (magic, "FLdt", 4) != 0)
    {
        result.error = "Corrupt FLP: missing data chunk.";
        return result;
    }
    auto dataLen = (uint32_t) in.readInt();
    auto dataEnd = juce::jmin ((juce::int64) data.getSize(), in.getPosition() + (juce::int64) dataLen);

    auto events = readEvents (in, dataEnd);
    if (events.empty())
    {
        result.error = "Corrupt FLP: no events.";
        return result;
    }

    buildMetadata (events, fl);
    buildChannels (events, fl);
    buildMixer (events, fl);
    buildPatterns (events, fl);
    buildArrangements (events, fl);

    result.project = convert (fl, host, file, result.summary, result.warnings, extraSearchDirs);

    constexpr int maxWarnings = 12;
    if (result.warnings.size() > maxWarnings)
    {
        int extra = result.warnings.size() - maxWarnings;
        result.warnings.removeRange (maxWarnings, extra);
        result.warnings.add ("...and " + juce::String (extra) + " more.");
    }
    return result;
}

FLPImport::Result FLPImport::importFile (const juce::File& file, PluginHost& host)
{
    Result result;

    juce::MemoryBlock data;
    if (! file.loadFileAsData (data) || data.getSize() < 22)
    {
        result.error = "Could not read " + file.getFileName();
        return result;
    }

    char magic[4] = {};
    memcpy (magic, data.getData(), 4);

    if (memcmp (magic, "PK\x03\x04", 4) == 0)
    {
        juce::ZipFile zip (file);
        if (zip.getNumEntries() == 0)
        {
            result.error = "Empty or unreadable zip file: " + file.getFileName();
            return result;
        }

        auto tempDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("mikeDAW_zip_" + juce::String (juce::Random::getSystemRandom().nextInt64()));
        tempDir.createDirectory();

        juce::File flpFile;
        juce::Array<juce::File> searchDirs;

        for (int i = 0; i < zip.getNumEntries(); ++i)
        {
            auto* entry = zip.getEntry (i);
            if (entry == nullptr)
                continue;

            auto destFile = tempDir.getChildFile (entry->filename);
            zip.uncompressEntry (i, tempDir);

            if (destFile.hasFileExtension ("flp") && ! flpFile.existsAsFile())
                flpFile = destFile;

            auto dir = destFile.getParentDirectory();
            if (! searchDirs.contains (dir))
                searchDirs.add (dir);
        }

        if (! flpFile.existsAsFile())
        {
            tempDir.deleteRecursively();
            result.error = "No .flp file found inside " + file.getFileName();
            return result;
        }

        juce::MemoryBlock flpData;
        if (! flpFile.loadFileAsData (flpData) || flpData.getSize() < 22)
        {
            tempDir.deleteRecursively();
            result.error = "Could not read .flp inside zip: " + flpFile.getFileName();
            return result;
        }

        result = importFlpData (flpData, flpFile, host, searchDirs);
        return result;
    }

    if (memcmp (magic, "FLhd", 4) != 0)
    {
        result.error = "Not an FL Studio project file: " + file.getFileName();
        return result;
    }

    return importFlpData (data, file, host);
}
