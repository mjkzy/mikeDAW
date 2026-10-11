#include "FLPExport.h"
#include "Vst2Fxb.h"
#include "Vst3ClassId.h"
#include "FLNativeFx.h"
#include "../Engine/AudioEngine.h"
#include "../Model/ProjectModel.h"
#include "../UI/AppLookAndFeel.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>

namespace
{

constexpr uint8_t OP_CHANNEL_ENABLED    = 0x00;
constexpr uint8_t OP_TIMESIG_NUM        = 0x11;
constexpr uint8_t OP_TIMESIG_DENOM      = 0x12;
constexpr uint8_t OP_CHANNEL_TYPE       = 0x15;
constexpr uint8_t OP_CHANNEL_ROUTED_TO  = 0x16;
constexpr uint8_t OP_PRE_PLAYLIST       = 0x24;
constexpr uint8_t OP_NEW_CHANNEL        = 0x40;
constexpr uint8_t OP_PATTERN_NEW        = 0x41;
constexpr uint8_t OP_NEW_SLOT           = 0x62;
constexpr uint8_t OP_ARRANGEMENT_NEW    = 0x63;
constexpr uint8_t OP_CHANNEL_COLOUR     = 0x80;
constexpr uint8_t OP_CHANNEL_GROUP      = 0x91;
constexpr uint8_t OP_INSERT_END         = 0x93;
constexpr uint8_t OP_INSERT_COLOUR      = 0x95;
constexpr uint8_t OP_PATTERN_COLOUR     = 0x96;
constexpr uint8_t OP_INSERT_INPUT       = 0x9a;
constexpr uint8_t OP_CHANNEL_ICON       = 0x9b;
constexpr uint8_t OP_TEMPO              = 0x9c;
constexpr uint8_t OP_BUILD_NUMBER       = 0x9f;
constexpr uint8_t OP_PATTERN_LENGTH     = 0xa4;
constexpr uint8_t OP_PATTERN_NAME       = 0xc1;
constexpr uint8_t OP_PROJECT_TITLE      = 0xc2;
constexpr uint8_t OP_CHANNEL_SAMPLE     = 0xc4;
constexpr uint8_t OP_PROJECT_FL_VERSION = 0xc7;
constexpr uint8_t OP_PLUGIN_INTERNAL    = 0xc9;
constexpr uint8_t OP_NAME               = 0xcb;
constexpr uint8_t OP_INSERT_NAME        = 0xcc;
constexpr uint8_t OP_PLUGIN_RUNTIME     = 0xd4;
constexpr uint8_t OP_PLUGIN_STATE       = 0xd5;
constexpr uint8_t OP_CHANNEL_LEVELS     = 0xdb;
constexpr uint8_t OP_PATTERN_NOTES      = 0xe0;
constexpr uint8_t OP_MIXER_PARAMS       = 0xe1;
constexpr uint8_t OP_PLAYLIST           = 0xe9;
constexpr uint8_t OP_INSERT_FLAGS       = 0xec;
constexpr uint8_t OP_TRACK_DATA         = 0xee;
constexpr uint8_t OP_TRACK_NAME         = 0xef;
constexpr uint8_t OP_GROUP_NAME         = 0xe7;
constexpr uint8_t OP_ARRANGEMENT_NAME   = 0xf1;

constexpr int PATTERN_BASE = 20480;
constexpr int FL_SLOTS_PER_INSERT = 10;
constexpr int FL_PPQ = 96;
constexpr int MAX_TRACK_IDX = 499;

constexpr int FL_NUM_TRACKS = MAX_TRACK_IDX + 1;

constexpr int FL_NUM_INSERTS = 127;

constexpr uint32_t FL_DEFAULT_CHANNEL_COLOUR = 0x6a655c;

constexpr uint32_t FL_DEFAULT_SLOT_COLOUR = 0x474541;

const char* const FL_VERSION_ASCII = "20.8.4.2576";
constexpr uint32_t FL_BUILD_NUMBER = 2576;
constexpr uint32_t WRAPPER_MARKER = 10;

const uint8_t FL_HEADER_C8[50] =
    { 0x70,0x00,0x62,0x00,0x63,0x00,0x6b,0x00,0x65,0x00,0x67,0x00,
      0x5f,0x00,0x5b,0x00,0x5e,0x00,0x56,0x00,0x52,0x00,0x5d,0x00,
      0x57,0x00,0x58,0x00,0x5b,0x00,0x61,0x00,0x3d,0x00,0x41,0x00,
      0x3b,0x00,0x40,0x00,0x3a,0x00,0x39,0x00,0x35,0x00,0x39,0x00,
      0x00,0x00 };

const uint8_t EMPTY_UTF16[2] = { 0, 0 };

class EventWriter
{
public:
    void writeU8 (uint8_t opcode, uint8_t value)
    {
        out.append (&opcode, 1);
        out.append (&value, 1);
    }

    void writeU16 (uint8_t opcode, uint16_t value)
    {
        out.append (&opcode, 1);
        uint8_t buf[2] = { (uint8_t) (value & 0xff), (uint8_t) (value >> 8) };
        out.append (buf, 2);
    }

    void writeU32 (uint8_t opcode, uint32_t value)
    {
        out.append (&opcode, 1);
        uint8_t buf[4];
        buf[0] = (uint8_t) (value & 0xff);
        buf[1] = (uint8_t) ((value >> 8) & 0xff);
        buf[2] = (uint8_t) ((value >> 16) & 0xff);
        buf[3] = (uint8_t) ((value >> 24) & 0xff);
        out.append (buf, 4);
    }

    void writeBlob (uint8_t opcode, const void* data, size_t len)
    {
        out.append (&opcode, 1);
        writeVarint ((uint32_t) len);
        if (len > 0)
            out.append (data, len);
    }

    void writeText (uint8_t opcode, const juce::String& text)
    {
        auto utf16 = encodeUTF16LE (text);
        writeBlob (opcode, utf16.getData(), utf16.getSize());
    }

    void writeAsciiZ (uint8_t opcode, const juce::String& text)
    {
        auto ascii = text.toRawUTF8();
        writeBlob (opcode, ascii, strlen (ascii) + 1);
    }

    const juce::MemoryBlock& getData() const { return out; }

private:
    juce::MemoryBlock out;

    void writeVarint (uint32_t v)
    {
        do
        {
            uint8_t b = (uint8_t) (v & 0x7f);
            v >>= 7;
            if (v > 0) b |= 0x80;
            out.append (&b, 1);
        } while (v > 0);
    }

    static juce::MemoryBlock encodeUTF16LE (const juce::String& s)
    {
        juce::MemoryBlock mb;
        for (auto cp = s.getCharPointer(); ! cp.isEmpty();)
        {
            auto c = cp.getAndAdvance();
            if (c <= 0xffff)
            {
                uint8_t lo = (uint8_t) (c & 0xff);
                uint8_t hi = (uint8_t) ((c >> 8) & 0xff);
                mb.append (&lo, 1);
                mb.append (&hi, 1);
            }
            else
            {
                uint32_t sub = (uint32_t) c - 0x10000;
                uint16_t hi = (uint16_t) (0xd800 + (sub >> 10));
                uint16_t lo = (uint16_t) (0xdc00 + (sub & 0x3ff));
                uint8_t buf[4] = { (uint8_t) (hi & 0xff), (uint8_t) (hi >> 8),
                                   (uint8_t) (lo & 0xff), (uint8_t) (lo >> 8) };
                mb.append (buf, 4);
            }
        }
        uint8_t nul[2] = { 0, 0 };
        mb.append (nul, 2);
        return mb;
    }
};

void putU16 (std::vector<uint8_t>& d, size_t off, uint16_t v)
{
    d[off]     = (uint8_t) (v & 0xff);
    d[off + 1] = (uint8_t) ((v >> 8) & 0xff);
}

void putU32 (std::vector<uint8_t>& d, size_t off, uint32_t v)
{
    d[off]     = (uint8_t) (v & 0xff);
    d[off + 1] = (uint8_t) ((v >> 8) & 0xff);
    d[off + 2] = (uint8_t) ((v >> 16) & 0xff);
    d[off + 3] = (uint8_t) ((v >> 24) & 0xff);
}

uint32_t packColour (juce::Colour c)
{
    return ((uint32_t) c.getBlue() << 16) | ((uint32_t) c.getGreen() << 8) | (uint32_t) c.getRed();
}

uint32_t packDisplayColour (juce::Colour c)
{
    return packColour (Colors::clipHeader (Colors::clipTone (c)));
}

bool readColour (const juce::ValueTree& node, uint32_t& packed, bool asClip = false)
{
    auto text = node.getProperty (IDs::colour).toString();
    if (text.isEmpty())
        return false;
    auto c = juce::Colour::fromString (text);
    if (c.isTransparent())
        return false;
    packed = asClip ? packDisplayColour (c) : packColour (c);
    return true;
}

const uint8_t PLAYLIST_TRACK_DEFAULT[66] =
    { 0x01,0x00,0x00,0x00, 0x48,0x51,0x56,0x00, 0x00,0x00,0x00,0x00,
      0x01, 0x00,0x00,0x80,0x3f, 0xff,0xff,0xff,0xff,
      0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
      0x05,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x01,0x00,0x00,0x00,
      0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
      0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff, 0x01,0x00,0x00,0x00,
      0x00 };

const uint8_t PLUGIN_RUNTIME_WRAPPED[52] =
    { 0,0,0,0, 0,0,0,0, 0x02,0,0,0, 0,0,0,0, 0x50,0x01,0,0,
      0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
      0x50,0x03,0,0, 0x0d,0x01,0,0, 0,0,0,0, 0,0,0,0 };

const uint8_t PLUGIN_RUNTIME_SLOT[52] =
    { 0,0,0,0, 0,0,0,0, 0x02,0,0,0, 0,0,0,0, 0x41,0x01,0,0,
      0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
      0xa8,0,0,0, 0xfc,0,0,0, 0x66,0x03,0,0, 0x1b,0x01,0,0 };

const uint8_t PLUGIN_RUNTIME_NONE[52] =
    { 0,0,0,0, 0,0,0,0, 0xff,0xff,0xff,0xff, 0,0,0,0, 0x50,0,0,0,
      0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
      0xc2,0,0,0, 0x16,0x01,0,0, 0,0,0,0, 0,0,0,0 };

const uint8_t PLUGIN_RUNTIME_AUDIO[52] =
    { 0,0,0,0, 0,0,0,0, 0xff,0xff,0xff,0xff, 0,0,0,0, 0x50,0,0,0,
      0x03,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
      0x74,0,0,0, 0xc8,0,0,0, 0,0,0,0, 0,0,0,0 };

enum class ChannelKind { sampler, wrapper, audioClip, automation };

struct FixedEvent { uint8_t op; uint8_t width; uint32_t value; };

const FixedEvent CHANNEL_PARAMS[] =
    { { 0x8a, 4, 0x800080 }, { 0x8b, 4, 0x10000 }, { 0x59, 2, 0 },    { 0x61, 2, 128 },
      { 0x45, 2, 128 },      { 0x56, 2, 256 },     { 0x47, 2, 1024 },
      { 0x53, 2, 0 },        { 0x4a, 2, 0 },       { 0x4b, 2, 0 },    { 0x4c, 2, 0 },
      { 0x55, 2, 2048 },     { 0x83, 4, 0x800000 },{ 0x46, 2, 0 },
      { OP_CHANNEL_ROUTED_TO, 1, 0 } };

const uint8_t CHANNEL_D1[20] =
    { 0,0,0,0, 0x00,0x19,0,0, 0,0,0,0, 0x04,0,0,0, 0x90,0,0,0 };

const uint8_t CHANNEL_E5[20] =
    { 0,0,0,0, 0x00,0x32,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 };

const uint8_t CHANNEL_D7_SAMPLER[158] =
    { 0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,
      0xff,0xff,0xff,0xff,0x3c,0x00,0x00,0x00,0x00,0x00,0x80,0x3f,
      0x00,0x00,0x80,0x3f,0x00,0x00,0x80,0x3f,0x00,0x00,0x80,0x3f,
      0x00,0x00,0x80,0x3f,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
      0xff,0xff,0xff,0xff,0x00,0x04,0x00,0x00,0x30,0x00,0x00,0x00,
      0x00,0x00,0x00,0x00,0xa7,0x05,0x00,0x00,0x00,0x00,0x00,0x00,
      0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
      0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
      0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
      0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0xfe,0xff,0xff,0xff,
      0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
      0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xf0,0x3f,
      0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,
      0x00,0x01 };

const uint8_t CHANNEL_D7_WRAPPER[158] =
    { 0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x01,
      0xff,0xff,0xff,0xff,0x3c,0x00,0x00,0x00,0x00,0x00,0x80,0x3f,
      0x00,0x00,0x80,0x3f,0x00,0x00,0x80,0x3f,0x00,0x00,0x80,0x3f,
      0x00,0x00,0x80,0x3f,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
      0xff,0xff,0xff,0xff,0x00,0x04,0x00,0x00,0x30,0x00,0x00,0x00,
      0x00,0x00,0x01,0x00,0xa7,0x05,0x00,0x00,0x00,0x00,0x00,0x00,
      0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
      0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
      0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
      0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0xfe,0xff,0xff,0xff,
      0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
      0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xf0,0x3f,
      0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,
      0x01,0x01 };

const uint8_t CHANNEL_D7_AUDIO[158] =
    { 0x8c,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
      0xff,0xff,0xff,0xff,0x3c,0x00,0x00,0x00,0x00,0x00,0x80,0x3f,
      0x00,0x00,0x80,0x3f,0x00,0x00,0x80,0x3f,0x00,0x00,0x80,0x3f,
      0x00,0x00,0x80,0x3f,0x00,0x00,0x00,0x00,0x03,0x00,0x00,0x00,
      0x00,0x00,0x00,0x00,0x00,0x04,0x00,0x00,0x30,0x00,0x00,0x00,
      0x00,0x00,0x01,0x00,0xa7,0x05,0x00,0x00,0x00,0x00,0x00,0x00,
      0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,
      0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
      0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
      0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0xfe,0xff,0xff,0xff,
      0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
      0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xf0,0x3f,
      0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,
      0x01,0x01 };

const uint8_t CHANNEL_E4_A[16] = { 0x64,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 };
const uint8_t CHANNEL_E4_B[16] = { 0x3c,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 };

const uint8_t CHANNEL_DA[68] =
    { 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x64,0x00,0x00,0x00,
      0x20,0x4e,0x00,0x00, 0x20,0x4e,0x00,0x00, 0x30,0x75,0x00,0x00,
      0x32,0x00,0x00,0x00, 0x20,0x4e,0x00,0x00, 0x00,0x00,0x00,0x00,
      0x64,0x00,0x00,0x00, 0x20,0x4e,0x00,0x00, 0x00,0x00,0x00,0x00,
      0xb6,0x80,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
      0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00 };

const uint8_t CHANNEL_DA_2[68] =
    { 0x04,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x64,0x00,0x00,0x00,
      0x20,0x4e,0x00,0x00, 0x20,0x4e,0x00,0x00, 0x30,0x75,0x00,0x00,
      0x32,0x00,0x00,0x00, 0x20,0x4e,0x00,0x00, 0x00,0x00,0x00,0x00,
      0x64,0x00,0x00,0x00, 0x20,0x4e,0x00,0x00, 0x00,0x00,0x00,0x00,
      0xb6,0x80,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
      0x00,0x00,0x00,0x00, 0x9b,0xff,0xff,0xff };

constexpr uint32_t AUDIO_CLIP_FLAGS = 0x4b;
constexpr double AUDIO_CLIP_VOLUME = 10000.0 / 12800.0;

constexpr double FL_STRETCH_UNITS_PER_BEAT = 192.0;
constexpr size_t CHANNEL_D7_STRETCH_OFFSET = 96;
constexpr double FL_STRETCH_UNITS_MAX = 9999999.0;

uint32_t stretchUnitsFor (const juce::ValueTree& node)
{
    if ((int) node.getProperty (IDs::stretchTempoMode, 0) != 3)
        return 0;
    double beats = node.getProperty (IDs::stretchBeats, 0.0);
    if (beats <= 0.0)
        return 0;
    return (uint32_t) juce::jlimit (0.0, FL_STRETCH_UNITS_MAX,
                                    std::round (beats * FL_STRETCH_UNITS_PER_BEAT));
}

constexpr double FADE_KNOB_GAMMA = 2.42;
const double FADE_GRID[10] = { 0.005, 0.015, 0.035, 0.07, 0.13, 0.22, 0.35, 0.52, 0.74, 1.0 };
constexpr double FADE_POINT_MERGE_BEATS = 0.01;
constexpr uint32_t FADE_POINT_MID  = 0x02000000;
constexpr uint32_t FADE_POINT_LAST = 0xff000000;

const uint8_t FADE_EA_HEADER[17] =
    { 0x01,0x00,0x00,0x00, 0x40,0x00,0x00,0x00, 0x00, 0x04,0x00,0x00,0x00,
      0x03,0x00,0x00,0x00 };

const uint8_t FADE_EA_TAIL[112] =
    { 0x01,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
      0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x80,0x00,0x00,0x00,
      0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x80,0x00,0x00,0x00,
      0x05,0x00,0x00,0x00,0x03,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
      0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
      0x00,0x00,0xf0,0x3f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
      0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,
      0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xfb,0xb2,0x00,0x00,
      0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
      0x00,0x00,0x00,0x00 };

const uint8_t PLUGIN_RUNTIME_AUTOMATION[52] =
    { 0,0,0,0, 0,0,0,0, 0xff,0xff,0xff,0xff, 0,0,0,0, 0x50,0,0,0,
      0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
      0xa8,0,0,0, 0xfc,0,0,0, 0,0,0,0, 0,0,0,0 };

const uint8_t CHANNEL_D7_AUTOMATION[158] =
    { 0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
      0xff,0xff,0xff,0xff,0x3c,0x00,0x00,0x00,0x00,0x00,0x80,0x3f,
      0x00,0x00,0x80,0x3f,0x00,0x00,0x80,0x3f,0x00,0x00,0x80,0x3f,
      0x00,0x00,0x80,0x3f,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
      0xff,0xff,0xff,0xff,0x00,0x04,0x00,0x00,0x30,0x00,0x00,0x00,
      0x00,0x00,0x01,0x00,0xa7,0x05,0x00,0x00,0x00,0x00,0x00,0x00,
      0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
      0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
      0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
      0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0xfe,0xff,0xff,0xff,
      0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
      0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xf0,0x3f,
      0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,
      0x01,0x01 };

const uint8_t AUTOMATION_LEVELS[24] =
    { 0x00,0x00,0x00,0x00, 0x00,0x32,0x00,0x00, 0x00,0x00,0x00,0x00,
      0x00,0x01,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00 };

void writeChannelDecoration (EventWriter& w, ChannelKind kind, int route,
                             const std::vector<uint8_t>& levels, int filterGroup,
                             uint32_t stretchUnits = 0, uint16_t cutGroup = 0,
                             const std::vector<uint8_t>* automationCurve = nullptr)
{
    w.writeBlob (0xd1, CHANNEL_D1, sizeof (CHANNEL_D1));

    for (auto& p : CHANNEL_PARAMS)
    {
        uint32_t value = p.op == OP_CHANNEL_ROUTED_TO ? (uint32_t) route : p.value;
        if (p.width == 1)      w.writeU8  (p.op, (uint8_t) value);
        else if (p.width == 2) w.writeU16 (p.op, (uint16_t) value);
        else                   w.writeU32 (p.op, value);
    }

    w.writeBlob (OP_CHANNEL_LEVELS, levels.data(), levels.size());
    w.writeBlob (0xe5, CHANNEL_E5, sizeof (CHANNEL_E5));

    const uint8_t dd[9] = { (uint8_t) (kind == ChannelKind::automation ? 0x01 : 0x00), 0, 0, 0,
                            (uint8_t) (kind == ChannelKind::sampler ? 0x34 : 0xf4),
                            (uint8_t) (kind == ChannelKind::sampler ? 0x03 : 0x01),
                            0, 0, 0 };
    w.writeBlob (0xdd, dd, sizeof (dd));

    const uint8_t* d7 = kind == ChannelKind::sampler    ? CHANNEL_D7_SAMPLER
                      : kind == ChannelKind::wrapper    ? CHANNEL_D7_WRAPPER
                      : kind == ChannelKind::automation ? CHANNEL_D7_AUTOMATION
                                                        : CHANNEL_D7_AUDIO;
    if (stretchUnits > 0)
    {
        std::vector<uint8_t> patched (d7, d7 + sizeof (CHANNEL_D7_SAMPLER));
        putU32 (patched, CHANNEL_D7_STRETCH_OFFSET, stretchUnits);
        w.writeBlob (0xd7, patched.data(), patched.size());
    }
    else
        w.writeBlob (0xd7, d7, sizeof (CHANNEL_D7_SAMPLER));

    w.writeU32 (0x84, ((uint32_t) cutGroup << 16) | cutGroup);
    w.writeU32 (0x90, 0);
    w.writeU32 (OP_CHANNEL_GROUP, (uint32_t) filterGroup);
    if (automationCurve != nullptr)
        w.writeBlob (0xea, automationCurve->data(), automationCurve->size());
    w.writeU8 (0x20, 0);
    w.writeBlob (0xe4, CHANNEL_E4_A, sizeof (CHANNEL_E4_A));
    w.writeBlob (0xe4, CHANNEL_E4_B, sizeof (CHANNEL_E4_B));

    for (int env = 0; env < 5; ++env)
    {
        if (env != 1)
        {
            w.writeBlob (0xda, CHANNEL_DA, sizeof (CHANNEL_DA));
            continue;
        }
        std::vector<uint8_t> second (CHANNEL_DA_2, CHANNEL_DA_2 + sizeof (CHANNEL_DA_2));
        second[0] = kind == ChannelKind::sampler ? 0x14 : 0x04;
        w.writeBlob (0xda, second.data(), second.size());
    }

    w.writeU32 (0x8f, kind == ChannelKind::wrapper   ? 10u
                    : kind == ChannelKind::audioClip ? 258u : 3u);
    w.writeU8 (0x14, 0);
}

struct FadeGroup
{
    int chanIid = 0;
    int autoIid = 0;
    int lane = 0;
    int lenTicks = 0;
    double lenBeats = 0.0;
    double fadeInBeats = 0.0, fadeOutBeats = 0.0;
    double level = 0.0;
    uint32_t colour = 0;
    juce::String name;
    double fadeIn = 0.0, fadeOut = 0.0;
};

struct FadePoint { double pos, value; float tension; uint32_t flags; };

std::vector<FadePoint> fadeRamp (double level, double span, bool fadeOut)
{
    std::vector<double> positions;
    if (fadeOut)
    {
        for (int i = (int) (sizeof (FADE_GRID) / sizeof (double)) - 2; i >= 0; --i)
            positions.push_back (1.0 - FADE_GRID[i]);
        positions.push_back (1.0);
    }
    else
        positions.assign (FADE_GRID, FADE_GRID + sizeof (FADE_GRID) / sizeof (double));

    std::vector<FadePoint> points;
    size_t last = positions.size() - 1;
    double prev = 0.0;

    for (size_t i = 0; i < positions.size(); ++i)
    {
        double p = positions[i];
        double delta = (p - prev) * span;
        if (i != last && delta < FADE_POINT_MERGE_BEATS)
            continue;
        double fraction = fadeOut ? 1.0 - p : p;
        points.push_back ({ delta, level * std::pow (fraction, 1.0 / FADE_KNOB_GAMMA),
                            0.0f, FADE_POINT_MID });
        prev = p;
    }
    return points;
}

std::vector<FadePoint> fadePoints (const FadeGroup& g)
{
    std::vector<FadePoint> points;

    if (g.fadeInBeats > 0.0)
    {
        points.push_back ({ 0.0, 0.0, 0.0f, 0 });
        auto ramp = fadeRamp (g.level, g.fadeInBeats, false);
        points.insert (points.end(), ramp.begin(), ramp.end());
    }
    else
        points.push_back ({ 0.0, g.level, 0.0f, 0 });

    if (g.fadeOutBeats > 0.0)
    {
        double hold = g.lenBeats - g.fadeInBeats - g.fadeOutBeats;
        if (hold > 1e-9)
            points.push_back ({ hold, g.level, 0.0f, FADE_POINT_MID });
        auto ramp = fadeRamp (g.level, g.fadeOutBeats, true);
        points.insert (points.end(), ramp.begin(), ramp.end());
        points.push_back ({ 0.0, g.level, 0.0f, FADE_POINT_LAST });
    }
    else
    {
        points.back().flags = FADE_POINT_LAST;
        double rest = g.lenBeats - g.fadeInBeats;
        if (rest > 1e-9)
        {
            points.back().flags = FADE_POINT_MID;
            points.push_back ({ rest, g.level, 0.0f, FADE_POINT_LAST });
        }
    }
    return points;
}

std::vector<uint8_t> buildFadeCurve (const FadeGroup& g)
{
    auto points = fadePoints (g);

    std::vector<uint8_t> ea (FADE_EA_HEADER, FADE_EA_HEADER + sizeof (FADE_EA_HEADER));
    size_t off = ea.size();
    ea.resize (off + 4);
    putU32 (ea, off, (uint32_t) points.size());

    for (auto& p : points)
    {
        off = ea.size();
        ea.resize (off + 24);
        memcpy (ea.data() + off, &p.pos, 8);
        memcpy (ea.data() + off + 8, &p.value, 8);
        memcpy (ea.data() + off + 16, &p.tension, 4);
        putU32 (ea, off + 20, p.flags);
    }

    ea.insert (ea.end(), FADE_EA_TAIL, FADE_EA_TAIL + sizeof (FADE_EA_TAIL));
    return ea;
}

std::vector<uint8_t> buildFadeLink (const FadeGroup& g)
{
    std::vector<uint8_t> link (20, 0);
    putU16 (link, 2, (uint16_t) g.autoIid);
    putU16 (link, 10, (uint16_t) g.chanIid);
    putU32 (link, 12, 8);
    putU32 (link, 16, 469);
    return link;
}

void writeAutomationChannel (EventWriter& w, const FadeGroup& g, int filterGroup)
{
    w.writeU16 (OP_NEW_CHANNEL, (uint16_t) g.autoIid);
    w.writeU8 (OP_CHANNEL_TYPE, 5);
    w.writeBlob (OP_PLUGIN_INTERNAL, "\0", 2);
    w.writeBlob (OP_PLUGIN_RUNTIME, PLUGIN_RUNTIME_AUTOMATION, sizeof (PLUGIN_RUNTIME_AUTOMATION));
    w.writeText (OP_NAME, g.name);
    w.writeU32 (OP_CHANNEL_ICON, 0);
    w.writeU32 (OP_CHANNEL_COLOUR, g.colour);
    w.writeU8 (OP_CHANNEL_ENABLED, 1);

    std::vector<uint8_t> levels (AUTOMATION_LEVELS,
                                 AUTOMATION_LEVELS + sizeof (AUTOMATION_LEVELS));
    auto curve = buildFadeCurve (g);
    writeChannelDecoration (w, ChannelKind::automation, 0, levels, filterGroup, 0, 0, &curve);
}

struct WrapperTarget
{
    juce::PluginDescription desc;
    juce::MemoryBlock state;
    juce::MemoryBlock vst3ClassId;
    juce::MemoryBlock vst3Component;
    juce::MemoryBlock vst3Controller;
    bool valid = false;
};

void parseVst3State (const juce::String& stateB64, WrapperTarget& t)
{
    constexpr uint32_t juceXmlMagic = 0x21324356;

    if (stateB64.isEmpty())
        return;

    juce::MemoryOutputStream decoded;
    if (! juce::Base64::convertFromBase64 (decoded, stateB64))
        return;

    auto blob = decoded.getMemoryBlock();
    if (blob.getSize() <= 8)
        return;

    auto* bytes = static_cast<const uint8_t*> (blob.getData());
    if (juce::ByteOrder::littleEndianInt (bytes) != juceXmlMagic)
        return;

    auto xmlLen = (size_t) juce::ByteOrder::littleEndianInt (bytes + 4);
    if (xmlLen == 0)
        return;

    auto xml = juce::parseXML (juce::String::fromUTF8 (static_cast<const char*> (blob.getData()) + 8,
                                                       (int) juce::jmin (xmlLen, blob.getSize() - 8)));
    if (xml == nullptr)
        return;

    auto readStream = [&xml] (const char* identifier, juce::MemoryBlock& dest)
    {
        if (auto* child = xml->getChildByName (identifier))
            if (! dest.fromBase64Encoding (child->getAllSubText()))
                dest.reset();
    };

    readStream ("IComponent", t.vst3Component);
    readStream ("IEditController", t.vst3Controller);
}

bool isExportableSlot (const juce::ValueTree& slot)
{
    const auto type = slot.getProperty (IDs::type).toString();
    return type == "plugin" || FLNativeFx::canExport (type);
}

WrapperTarget parseWrapperTarget (const juce::ValueTree& node)
{
    WrapperTarget t;
    auto descXml = node.getProperty (IDs::pluginDesc).toString();
    if (descXml.isEmpty())
        return t;
    auto xml = juce::parseXML (descXml);
    if (xml == nullptr || ! t.desc.loadFromXml (*xml))
        return t;
    if (t.desc.pluginFormatName != "VST" && t.desc.pluginFormatName != "VST3")
        return t;
    if (t.desc.fileOrIdentifier.isEmpty())
        return t;

    if (t.desc.pluginFormatName == "VST")
    {
        auto stateB64 = node.getProperty (IDs::pluginState).toString();
        if (stateB64.isNotEmpty())
        {
            juce::MemoryOutputStream decoded;
            if (juce::Base64::convertFromBase64 (decoded, stateB64))
                t.state = decoded.getMemoryBlock();
        }
    }
    else
    {
        t.vst3ClassId = FLVst3::extractClassId (juce::File (t.desc.fileOrIdentifier));
        if (t.vst3ClassId.getSize() != 16)
            return t;

        parseVst3State (node.getProperty (IDs::pluginState).toString(), t);
    }
    t.valid = true;
    return t;
}

void appendWrapperRecord (std::vector<uint8_t>& v, uint32_t id, const void* data, size_t len)
{
    size_t off = v.size();
    v.resize (off + 12 + len);
    putU32 (v, off, id);
    putU32 (v, off + 4, (uint32_t) len);
    putU32 (v, off + 8, 0);
    if (len > 0)
        memcpy (v.data() + off + 12, data, len);
}

std::vector<uint8_t> buildVst3StateContainer (const WrapperTarget& t)
{
    std::vector<uint8_t> v (0x50, 0);
    putU32 (v, 0, 1);
    putU32 (v, 4, 1);
    putU32 (v, 8, 0x40);
    putU32 (v, 16, 1);

    appendWrapperRecord (v, 3, t.vst3Component.getData(), t.vst3Component.getSize());
    if (t.vst3Controller.getSize() > 0)
        appendWrapperRecord (v, 4, t.vst3Controller.getData(), t.vst3Controller.getSize());

    return v;
}

std::vector<uint8_t> buildWrapperBlob (const WrapperTarget& t, bool mixerSlot)
{
    static const uint8_t rec1[20]  = { 0xff,0xff,0xff,0xff, 0xff,0xff,0xff,0xff,
                                       0x0c,0,0,0, 0,0,0,0, 0,0,0,0 };
    static const uint8_t rec2[25]  = { 0x00,0x6a,0x04,0x00, 0x00,0x76,0x02,0x00,
                                       0x00,0x8c,0x7d,0x00, 0xa4,0x00,0x00,0x00,
                                       0x00,0x01,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00 };
    static const uint8_t rec31[12]     = { 0,0,0,0, 1,0,0,0, 0,0,0,0 };
    static const uint8_t rec32Short[12] = { 0,0,0,0, 1,0,0,0, 0,0,0,0 };
    static const uint8_t rec32Chan[24] = { 0,0,0,0, 1,0,0,0, 0,0,0,0,
                                           0,0,0,0, 1,0,0,0, 0,0,0,0 };
    static const uint8_t rec57[4]      = { 0x60,0x09,0,0 };

    bool vst3 = t.desc.pluginFormatName == "VST3";

    uint8_t rec50[16] = { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 };
    rec50[0] = vst3 ? 8 : 0;

    uint8_t rec30[16] = { 0,0,0,0, 1,0,0,0, 0,0,0,0, 0,0,0,0 };
    rec30[0] = mixerSlot ? 1 : 0;
    rec30[4] = (! mixerSlot && ! vst3) ? 2 : 1;

    std::vector<uint8_t> v (4, 0);
    putU32 (v, 0, WRAPPER_MARKER);

    appendWrapperRecord (v, 1, rec1, sizeof (rec1));
    appendWrapperRecord (v, 2, rec2, sizeof (rec2));
    appendWrapperRecord (v, 30, rec30, sizeof (rec30));
    if (mixerSlot)
        appendWrapperRecord (v, 31, rec31, sizeof (rec31));
    if (mixerSlot || vst3)
        appendWrapperRecord (v, 32, rec32Short, sizeof (rec32Short));
    else
        appendWrapperRecord (v, 32, rec32Chan, sizeof (rec32Chan));
    appendWrapperRecord (v, 50, rec50, sizeof (rec50));

    juce::MemoryBlock vst2Chunk;

    if (t.desc.pluginFormatName == "VST")
    {
        auto uid = (uint32_t) (t.desc.uniqueId != 0 ? t.desc.uniqueId : t.desc.deprecatedUid);
        uint8_t fourCC[4] = { (uint8_t) (uid & 0xff),         (uint8_t) ((uid >> 8) & 0xff),
                              (uint8_t) ((uid >> 16) & 0xff), (uint8_t) ((uid >> 24) & 0xff) };
        appendWrapperRecord (v, 51, fourCC, 4);
        vst2Chunk = Vst2Fxb::extractChunk (t.state.getData(), t.state.getSize());
    }
    else if (t.vst3ClassId.getSize() == 16)
    {
        appendWrapperRecord (v, 52, t.vst3ClassId.getData(), 16);
    }

    if (! vst3)
        appendWrapperRecord (v, 57, rec57, sizeof (rec57));

    auto name = t.desc.name.toRawUTF8();
    appendWrapperRecord (v, 54, name, strlen (name));
    auto path = t.desc.fileOrIdentifier.toRawUTF8();
    appendWrapperRecord (v, 55, path, strlen (path));
    if (t.desc.manufacturerName.isNotEmpty())
    {
        auto vendor = t.desc.manufacturerName.toRawUTF8();
        appendWrapperRecord (v, 56, vendor, strlen (vendor));
    }

    if (t.desc.pluginFormatName == "VST")
    {
        if (vst2Chunk.getSize() > 0)
        {
            std::vector<uint8_t> framed (21 + vst2Chunk.getSize(), 0);
            static const uint8_t head[9] = { 0xf7,0xff,0xff,0xff, 0x0d, 0xfe,0xff,0xff,0xff };
            memcpy (framed.data(), head, 9);
            putU32 (framed, 9, (uint32_t) vst2Chunk.getSize());
            memcpy (framed.data() + 21, vst2Chunk.getData(), vst2Chunk.getSize());
            appendWrapperRecord (v, 53, framed.data(), framed.size());
        }
    }
    else if (t.vst3Component.getSize() > 0)
    {
        auto container = buildVst3StateContainer (t);
        appendWrapperRecord (v, 53, container.data(), container.size());
    }

    return v;
}

struct SampleBundle
{
    std::map<juce::String, juce::String> renames;
    FLPExport::PathResolver resolve;

    juce::File localFile (const juce::String& sourcePath) const
    {
        juce::File f (sourcePath);
        if (f.existsAsFile() || resolve == nullptr)
            return f;
        auto r = resolve (sourcePath);
        return r.existsAsFile() ? r : f;
    }

    juce::String add (const juce::String& sourcePath)
    {
        if (sourcePath.startsWith ("builtin:"))
        {
            auto leaf = "mikeDAW " + sourcePath.fromFirstOccurrenceOf (":", false, false) + ".wav";
            renames[leaf] = sourcePath;
            return leaf;
        }

        juce::File f = localFile (sourcePath);
        if (! f.existsAsFile())
            return {};

        auto leaf = f.getFileName();
        auto existing = renames.find (leaf);
        if (existing != renames.end() && existing->second != f.getFullPathName())
            leaf = f.getFileNameWithoutExtension() + "_" + juce::String (std::abs (sourcePath.hashCode()) % 1000)
                 + f.getFileExtension();
        renames[leaf] = f.getFullPathName();
        return leaf;
    }
};

void appendNoteRecord (std::vector<uint8_t>& blob, int pos, int len, int iid, int key, int vel)
{
    std::vector<uint8_t> rec (24, 0);
    putU32 (rec, 0, (uint32_t) juce::jmax (0, pos));
    putU16 (rec, 4, 0x4000);
    putU16 (rec, 6, (uint16_t) iid);
    putU32 (rec, 8, (uint32_t) juce::jmax (1, len));
    putU16 (rec, 12, (uint16_t) juce::jlimit (0, 131, key));
    rec[16] = 120;
    rec[18] = 64;
    rec[20] = 64;
    rec[21] = (uint8_t) juce::jlimit (1, 127, vel);
    rec[22] = 128;
    rec[23] = 128;
    blob.insert (blob.end(), rec.begin(), rec.end());
}

struct FLNoteEvent { int pos, len, key, vel, iid; };

juce::MemoryBlock buildFlpData (const juce::ValueTree& project,
                                SampleBundle& samples,
                                juce::String& summary)
{
    EventWriter w;

    auto title = project.getProperty (IDs::name).toString();
    double bpm = project.getProperty (IDs::bpm, 140.0);

    auto channels = Model::getChannels (project);
    std::map<int, int> chanIdToIid;
    int nextIid = 0;
    int vstChannels = 0, vstStates = 0, unresolvedPlugins = 0;

    for (int i = 0; i < channels.getNumChildren(); ++i)
        chanIdToIid[(int) channels.getChild (i).getProperty (IDs::id)] = nextIid++;

    auto playlist = Model::getPlaylist (project);
    std::map<juce::String, int> audioClipIid;
    for (int ci = 0; ci < playlist.getNumChildren(); ++ci)
    {
        auto clip = playlist.getChild (ci);
        if (! clip.hasType (IDs::Clip) || clip.getProperty (IDs::type).toString() != "audio")
            continue;
        auto path = clip.getProperty (IDs::path).toString();
        if (path.isNotEmpty() && audioClipIid.find (path) == audioClipIid.end())
            audioClipIid[path] = nextIid++;
    }

    bool hasAudioClips = ! audioClipIid.empty();
    int audioGroup = 0;
    int normalGroup = hasAudioClips ? 1 : 0;

    auto sourceChannelFor = [&channels] (const juce::String& path)
    {
        for (int i = 0; i < channels.getNumChildren(); ++i)
        {
            auto ch = channels.getChild (i);
            if (ch.getProperty (IDs::type).toString() == "sampler"
                && ch.getProperty (IDs::samplePath).toString() == path)
                return ch;
        }
        return juce::ValueTree();
    };

    auto audioFiles = Model::getAudioFiles (project);

    juce::AudioFormatManager audioFormats;
    audioFormats.registerBasicFormats();
    std::map<juce::String, double> sourceSecondsCache;
    auto sourceSeconds = [&] (const juce::String& path) -> double
    {
        auto cached = sourceSecondsCache.find (path);
        if (cached != sourceSecondsCache.end())
            return cached->second;
        double secs = 0.0;
        if (auto reader = std::unique_ptr<juce::AudioFormatReader> (
                audioFormats.createReaderFor (samples.localFile (path))))
            if (reader->sampleRate > 0.0)
                secs = (double) reader->lengthInSamples / reader->sampleRate;
        return sourceSecondsCache[path] = secs;
    };

    auto firstClipFor = [&playlist] (const juce::String& path)
    {
        for (int ci = 0; ci < playlist.getNumChildren(); ++ci)
        {
            auto clip = playlist.getChild (ci);
            if (clip.hasType (IDs::Clip)
                && clip.getProperty (IDs::type).toString() == "audio"
                && clip.getProperty (IDs::path).toString() == path)
                return clip;
        }
        return juce::ValueTree();
    };
    auto audioClipVolume01 = [&] (const juce::String& path)
    {
        auto clip = firstClipFor (path);
        return juce::jlimit (0.0, 1.0, clip.isValid() ? (double) clip.getProperty (IDs::volume, 1.0)
                                                      : AUDIO_CLIP_VOLUME);
    };
    auto audioClipPan = [&] (const juce::String& path)
    {
        auto clip = firstClipFor (path);
        return clip.isValid() ? (double) clip.getProperty (IDs::pan, 0.0) : 0.0;
    };
    auto audioClipRoute = [&] (const juce::String& path)
    {
        for (int ci = 0; ci < playlist.getNumChildren(); ++ci)
        {
            auto clip = playlist.getChild (ci);
            if (clip.hasType (IDs::Clip)
                && clip.getProperty (IDs::type).toString() == "audio"
                && clip.getProperty (IDs::path).toString() == path
                && clip.hasProperty (IDs::mixerTrack))
                return (int) clip.getProperty (IDs::mixerTrack);
        }
        auto src = sourceChannelFor (path);
        return src.isValid() ? (int) src.getProperty (IDs::mixerTrack, 0) : 0;
    };
    auto audioClipVolume = [&] (const juce::String& path)
    {
        return juce::roundToInt (audioClipVolume01 (path) * 12800.0);
    };

    auto audioClipColour = [&] (const juce::String& path)
    {
        uint32_t colour = packDisplayColour (Colors::clipDefault);
        readColour (audioFiles.getChildWithProperty (IDs::audioPath, path), colour, true);
        return colour;
    };

    auto audioClipStretchUnits = [&] (const juce::String& path)
    {
        for (int ci = 0; ci < playlist.getNumChildren(); ++ci)
        {
            auto clip = playlist.getChild (ci);
            if (clip.hasType (IDs::Clip)
                && clip.getProperty (IDs::type).toString() == "audio"
                && clip.getProperty (IDs::path).toString() == path)
            {
                auto units = stretchUnitsFor (clip);
                if (units > 0)
                    return units;
            }
        }
        auto src = sourceChannelFor (path);
        return src.isValid() ? stretchUnitsFor (src) : 0u;
    };

    std::vector<FadeGroup> fadeGroups;
    int fadesDropped = 0;
    {
        int maxLane = -1;
        for (int ci = 0; ci < playlist.getNumChildren(); ++ci)
        {
            auto clip = playlist.getChild (ci);
            if (clip.hasType (IDs::Clip))
                maxLane = juce::jmax (maxLane, juce::jlimit (0, MAX_TRACK_IDX,
                                                             (int) clip.getProperty (IDs::track, 0)));
        }

        std::vector<FadeGroup> planned;

        for (int ci = 0; ci < playlist.getNumChildren(); ++ci)
        {
            auto clip = playlist.getChild (ci);
            if (! clip.hasType (IDs::Clip) || clip.getProperty (IDs::type).toString() != "audio")
                continue;

            double fadeIn  = juce::jlimit (0.0, 1.0, (double) clip.getProperty (IDs::fadeIn, 0.0));
            double fadeOut = juce::jlimit (0.0, 1.0, (double) clip.getProperty (IDs::fadeOut, 0.0));
            if (fadeIn <= 0.0 && fadeOut <= 0.0)
                continue;

            auto path = clip.getProperty (IDs::path).toString();
            auto iid = audioClipIid.find (path);
            if (iid == audioClipIid.end())
                continue;

            double secs = sourceSeconds (path);
            if (secs <= 0.0)
            {
                ++fadesDropped;
                continue;
            }

            int lenTicks = juce::jmax (1, juce::roundToInt (
                (double) clip.getProperty (IDs::lengthBeats, 1.0) * FL_PPQ));

            if (std::any_of (planned.begin(), planned.end(), [&] (const FadeGroup& g)
                             { return g.chanIid == iid->second && g.lenTicks == lenTicks
                                   && g.fadeIn == fadeIn && g.fadeOut == fadeOut; }))
                continue;

            const double beatsPerSecond = juce::jlimit (10.0, 522.0, bpm) / 60.0;
            double inBeats  = fadeIn  * 0.5 * secs * beatsPerSecond;
            double outBeats = fadeOut * 0.5 * secs * beatsPerSecond;
            double lenBeats = lenTicks / (double) FL_PPQ;
            if (inBeats + outBeats > lenBeats && inBeats + outBeats > 0.0)
            {
                double scale = lenBeats / (inBeats + outBeats);
                inBeats *= scale;
                outBeats *= scale;
            }

            FadeGroup g;
            g.chanIid = iid->second;
            g.lenTicks = lenTicks;
            g.lenBeats = lenBeats;
            g.fadeInBeats = inBeats;
            g.fadeOutBeats = outBeats;
            g.level = audioClipVolume (path) / 12800.0;
            g.colour = audioClipColour (path);
            g.name = juce::File (path).getFileNameWithoutExtension() + " - Channel volume";
            g.fadeIn = fadeIn;
            g.fadeOut = fadeOut;
            planned.push_back (g);
        }

        for (auto& g : planned)
        {
            int lane = maxLane + 1 + (int) fadeGroups.size();
            if (lane > MAX_TRACK_IDX)
            {
                ++fadesDropped;
                continue;
            }
            g.lane = lane;
            g.autoIid = nextIid++;
            fadeGroups.push_back (g);
        }
    }

    auto findFadeGroup = [&fadeGroups] (int chanIid, int lenTicks, double fadeIn, double fadeOut)
                         -> const FadeGroup*
    {
        for (auto& g : fadeGroups)
            if (g.chanIid == chanIid && g.lenTicks == lenTicks
                && g.fadeIn == fadeIn && g.fadeOut == fadeOut)
                return &g;
        return nullptr;
    };

    w.writeAsciiZ (OP_PROJECT_FL_VERSION, FL_VERSION_ASCII);
    w.writeU32 (OP_BUILD_NUMBER, FL_BUILD_NUMBER);
    w.writeU8 (0x1c, 1);
    w.writeU8 (0x25, 1);
    w.writeBlob (0xc8, FL_HEADER_C8, sizeof (FL_HEADER_C8));
    w.writeU32 (OP_TEMPO, (uint32_t) juce::roundToInt (juce::jlimit (10.0, 522.0, bpm) * 1000.0));
    w.writeU16 (0x43, 1);
    w.writeU8 (0x09, 1);
    w.writeU8 (0x0b, 0);
    w.writeU16 (0x50, 0);
    w.writeU8 (OP_TIMESIG_NUM, 4);
    w.writeU8 (OP_TIMESIG_DENOM, 4);
    w.writeU8 (0x23, 1);
    w.writeU8 (0x17, 0);
    w.writeU8 (0x1e, 1);
    w.writeU8 (0x0a, 0);
    w.writeText (OP_PROJECT_TITLE, title.isNotEmpty() ? title : "mikeDAW Export");
    w.writeBlob (0xce, EMPTY_UTF16, sizeof (EMPTY_UTF16));
    w.writeBlob (0xcf, EMPTY_UTF16, sizeof (EMPTY_UTF16));
    w.writeBlob (0xca, EMPTY_UTF16, sizeof (EMPTY_UTF16));
    w.writeBlob (0xc3, EMPTY_UTF16, sizeof (EMPTY_UTF16));

    {
        constexpr double unixEpochAsOleDate = 25569.0;
        constexpr double secondsPerDay = 86400.0;
        double created = (double) project.getProperty (IDs::createdEpoch, 0);
        if (created <= 0.0)
            created = (double) (juce::Time::getCurrentTime().toMilliseconds() / 1000);
        double worked = (double) project.getProperty (IDs::secondsWorked, 0);

        double stamp[2] = { created / secondsPerDay + unixEpochAsOleDate, worked / secondsPerDay };
        uint8_t blob[16];
        memcpy (blob, stamp, sizeof (blob));
        w.writeBlob (0xed, blob, sizeof (blob));
    }

    if (hasAudioClips)
        w.writeText (OP_GROUP_NAME, "Audio");
    w.writeText (OP_GROUP_NAME, "Unsorted");
    w.writeU32 (0x92, 0);

    if (! fadeGroups.empty())
    {
        std::vector<uint8_t> controls;
        for (auto& g : fadeGroups)
        {
            std::vector<uint8_t> rec (12, 0);
            putU16 (rec, 6, (uint16_t) g.chanIid);
            putU32 (rec, 8, (uint32_t) juce::roundToInt (g.level * 12800.0));
            controls.insert (controls.end(), rec.begin(), rec.end());
        }
        w.writeBlob (0xd8, controls.data(), controls.size());
    }

    auto patterns = Model::getPatterns (project);
    int numPatterns = patterns.getNumChildren();
    std::map<int, int> patIdToFlId;
    int notesExported = 0;

    for (int pi = 0; pi < numPatterns; ++pi)
    {
        auto pat = patterns.getChild (pi);
        int flPatId = pi + 1;
        patIdToFlId[(int) pat.getProperty (IDs::id)] = flPatId;

        int lengthSteps = pat.getProperty (IDs::lengthSteps, Model::defaultSteps);
        std::vector<FLNoteEvent> notes;

        for (int si = 0; si < pat.getNumChildren(); ++si)
        {
            auto seq = pat.getChild (si);
            if (! seq.hasType (IDs::ChannelSeq))
                continue;

            auto iidIt = chanIdToIid.find ((int) seq.getProperty (IDs::channelId));
            if (iidIt == chanIdToIid.end())
                continue;
            int iid = iidIt->second;

            if ((bool) seq.getProperty (IDs::usePianoRoll, false))
            {
                for (int ni = 0; ni < seq.getNumChildren(); ++ni)
                {
                    auto note = seq.getChild (ni);
                    if (! note.hasType (IDs::Note))
                        continue;
                    notes.push_back ({ (int) note.getProperty (IDs::start, 0),
                                       (int) note.getProperty (IDs::length, Model::ticksPerStep),
                                       (int) note.getProperty (IDs::key, 60),
                                       (int) note.getProperty (IDs::velocity, 100), iid });
                }
            }
            else
            {
                auto stepStr = seq.getProperty (IDs::steps).toString();
                for (int s = 0; s < stepStr.length() && s < lengthSteps; ++s)
                    if (stepStr[s] == '1')
                        notes.push_back ({ s * Model::ticksPerStep, Model::ticksPerStep, 60, 100, iid });
            }
        }

        if (notes.empty())
            continue;

        std::stable_sort (notes.begin(), notes.end(),
                          [] (const FLNoteEvent& a, const FLNoteEvent& b) { return a.pos < b.pos; });

        std::vector<uint8_t> blob;
        for (auto& n : notes)
            appendNoteRecord (blob, n.pos, n.len, n.iid, n.key, n.vel);
        notesExported += (int) notes.size();

        w.writeU16 (OP_PATTERN_NEW, (uint16_t) flPatId);
        w.writeBlob (OP_PATTERN_NOTES, blob.data(), blob.size());
    }

    uint16_t nextCutGroup = 1;
    auto writeChannel = [&] (int iid, int type, const juce::String& name, bool muted,
                             const juce::String& sampleLeafOrPath, double vol, double pan,
                             int mixerTrack, uint32_t colour, const std::vector<uint8_t>* wrapper,
                             bool vst3, int filterGroup, uint32_t stretchUnits, bool cutItself)
    {
        bool audioClip = type == 4;

        w.writeU16 (OP_NEW_CHANNEL, (uint16_t) iid);
        w.writeU8 (OP_CHANNEL_TYPE, (uint8_t) type);
        if (wrapper != nullptr)
            w.writeText (OP_PLUGIN_INTERNAL, "Fruity Wrapper");
        else
            w.writeBlob (OP_PLUGIN_INTERNAL, "\0", 2);
        std::vector<uint8_t> runtime (PLUGIN_RUNTIME_WRAPPED,
                                      PLUGIN_RUNTIME_WRAPPED + sizeof (PLUGIN_RUNTIME_WRAPPED));
        if (wrapper == nullptr)
            runtime.assign (audioClip ? PLUGIN_RUNTIME_AUDIO : PLUGIN_RUNTIME_NONE,
                            (audioClip ? PLUGIN_RUNTIME_AUDIO : PLUGIN_RUNTIME_NONE)
                                + sizeof (PLUGIN_RUNTIME_WRAPPED));
        else if (vst3)
            runtime[16] = 0x51;
        w.writeBlob (OP_PLUGIN_RUNTIME, runtime.data(), runtime.size());
        if (name.isNotEmpty())
            w.writeText (OP_NAME, name);
        w.writeU32 (OP_CHANNEL_ICON, audioClip ? AUDIO_CLIP_FLAGS : 0);
        w.writeU32 (OP_CHANNEL_COLOUR, colour);
        if (wrapper != nullptr)
            w.writeBlob (OP_PLUGIN_STATE, wrapper->data(), wrapper->size());
        w.writeU8 (OP_CHANNEL_ENABLED, muted ? 0 : 1);

        std::vector<uint8_t> levels (24, 0);
        putU32 (levels, 0, (uint32_t) juce::roundToInt (juce::jlimit (-1.0, 1.0, pan) * 6400.0 + 6400.0));
        putU32 (levels, 4, (uint32_t) juce::roundToInt (juce::jlimit (0.0, 1.0, vol) * 12800.0));
        putU32 (levels, 12, 256);

        auto kind = audioClip           ? ChannelKind::audioClip
                  : wrapper != nullptr  ? ChannelKind::wrapper
                                        : ChannelKind::sampler;

        const uint16_t cutGroup = wrapper != nullptr || (cutItself && ! audioClip) ? nextCutGroup++ : 0;
        writeChannelDecoration (w, kind, juce::jlimit (0, 127, mixerTrack), levels, filterGroup,
                                stretchUnits, cutGroup);

        if (sampleLeafOrPath.isNotEmpty())
            w.writeText (OP_CHANNEL_SAMPLE, sampleLeafOrPath);
    };

    for (auto& g : fadeGroups)
    {
        auto link = buildFadeLink (g);
        w.writeBlob (0xe3, link.data(), link.size());
    }

    for (int i = 0; i < channels.getNumChildren(); ++i)
    {
        auto ch = channels.getChild (i);
        bool isPlugin = ch.getProperty (IDs::type).toString() == "plugin";

        std::vector<uint8_t> wrapper;
        bool hasWrapper = false;
        bool isVst3 = false;
        juce::String sampleRef;

        if (isPlugin)
        {
            auto target = parseWrapperTarget (ch);
            if (target.valid)
            {
                wrapper = buildWrapperBlob (target, false);
                hasWrapper = true;
                isVst3 = target.desc.pluginFormatName == "VST3";
                ++vstChannels;
                if (target.state.getSize() > 0 || target.vst3Component.getSize() > 0)
                    ++vstStates;
            }
            else
                ++unresolvedPlugins;
        }
        else
        {
            auto samplePath = ch.getProperty (IDs::samplePath).toString();
            if (samplePath.isNotEmpty())
            {
                auto leaf = samples.add (samplePath);
                sampleRef = leaf.isNotEmpty() ? leaf : samplePath;
            }
        }

        uint32_t colour = FL_DEFAULT_CHANNEL_COLOUR;
        readColour (ch, colour);

        writeChannel (chanIdToIid[(int) ch.getProperty (IDs::id)],
                      hasWrapper ? 2 : 0,
                      ch.getProperty (IDs::name).toString(),
                      (bool) ch.getProperty (IDs::mute, false),
                      sampleRef,
                      (double) ch.getProperty (IDs::volume, 0.78),
                      (double) ch.getProperty (IDs::pan, 0.0),
                      (int) ch.getProperty (IDs::mixerTrack, 0),
                      colour,
                      hasWrapper ? &wrapper : nullptr,
                      isVst3,
                      normalGroup,
                      isPlugin ? 0u : stretchUnitsFor (ch),
                      (bool) ch.getProperty (IDs::cutItself, false));
    }

    for (auto& [path, iid] : audioClipIid)
    {
        auto leaf = samples.add (path);

        writeChannel (iid, 4,
                      juce::File (path).getFileNameWithoutExtension(),
                      false, leaf.isNotEmpty() ? leaf : path,
                      audioClipVolume01 (path),
                      audioClipPan (path),
                      audioClipRoute (path),
                      audioClipColour (path), nullptr, false,
                      audioGroup,
                      audioClipStretchUnits (path), false);
    }

    for (auto& g : fadeGroups)
        writeAutomationChannel (w, g, normalGroup);

    for (int pi = 0; pi < numPatterns; ++pi)
    {
        auto pat = patterns.getChild (pi);
        w.writeU16 (OP_PATTERN_NEW, (uint16_t) (pi + 1));

        auto patName = pat.getProperty (IDs::name).toString();
        if (patName.isNotEmpty() && patName != "Pattern " + juce::String (pi + 1))
            w.writeText (OP_PATTERN_NAME, patName);
        w.writeU32 (OP_PATTERN_COLOUR, packDisplayColour (Model::patternColour (pat)));
        w.writeU32 (0x9d, 0xffffffff);
        w.writeU32 (0x9e, 0xffffffff);
        w.writeU32 (OP_PATTERN_LENGTH, 0);
    }

    w.writeU16 (OP_ARRANGEMENT_NEW, 0);
    w.writeText (OP_ARRANGEMENT_NAME, "Arrangement");
    w.writeU8 (OP_PRE_PLAYLIST, 0);

    std::vector<uint8_t> playlistBlob;
    int patClips = 0, audioClips = 0, clipsSkipped = 0, fadeClips = 0;

    std::vector<juce::ValueTree> clips;
    for (int ci = 0; ci < playlist.getNumChildren(); ++ci)
        if (playlist.getChild (ci).hasType (IDs::Clip))
            clips.push_back (playlist.getChild (ci));
    std::stable_sort (clips.begin(), clips.end(),
                      [] (const juce::ValueTree& a, const juce::ValueTree& b)
                      { return (double) a.getProperty (IDs::startBeats, 0.0)
                             < (double) b.getProperty (IDs::startBeats, 0.0); });

    for (auto& clip : clips)
    {
        auto clipType = clip.getProperty (IDs::type).toString();
        double offsetBeats = clip.getProperty (IDs::offsetBeats, 0.0);
        int lenTicks = juce::jmax (1, juce::roundToInt ((double) clip.getProperty (IDs::lengthBeats, 1.0) * FL_PPQ));
        int itemIndex = 0;
        uint32_t startOff = 0, endOff = 0;

        if (clipType == "pattern")
        {
            auto pit = patIdToFlId.find ((int) clip.getProperty (IDs::patternId));
            if (pit == patIdToFlId.end()) { ++clipsSkipped; continue; }
            itemIndex = PATTERN_BASE + pit->second;
            int offTicks = juce::jmax (0, juce::roundToInt (offsetBeats * FL_PPQ));
            startOff = (uint32_t) offTicks;
            endOff = (uint32_t) (offTicks + lenTicks);
            ++patClips;
        }
        else if (clipType == "audio")
        {
            auto path = clip.getProperty (IDs::path).toString();
            auto ait = audioClipIid.find (path);
            if (ait == audioClipIid.end()) { ++clipsSkipped; continue; }
            itemIndex = ait->second;
            const double msPerBeat = 60000.0 / juce::jlimit (10.0, 522.0, bpm);

            double ratio = 1.0;
            if (auto units = audioClipStretchUnits (path))
            {
                double stretchedMs = units / FL_STRETCH_UNITS_PER_BEAT * msPerBeat;
                double sourceMs = sourceSeconds (path) * 1000.0;
                if (sourceMs > 0.0 && stretchedMs > 0.0)
                    ratio = sourceMs / stretchedMs;
            }

            float s = (float) (offsetBeats * msPerBeat * ratio);
            float e = (float) ((offsetBeats + lenTicks / (double) FL_PPQ) * msPerBeat * ratio);
            memcpy (&startOff, &s, 4);
            memcpy (&endOff, &e, 4);
            ++audioClips;
        }
        else
        {
            ++clipsSkipped;
            continue;
        }

        int lane = juce::jlimit (0, MAX_TRACK_IDX, (int) clip.getProperty (IDs::track, 0));
        int posTicks = juce::jmax (0, juce::roundToInt ((double) clip.getProperty (IDs::startBeats, 0.0) * FL_PPQ));

        std::vector<uint8_t> rec (32, 0);
        putU32 (rec, 0, (uint32_t) posTicks);
        putU16 (rec, 4, PATTERN_BASE);
        putU16 (rec, 6, (uint16_t) itemIndex);
        putU32 (rec, 8, (uint32_t) lenTicks);
        putU16 (rec, 12, (uint16_t) (MAX_TRACK_IDX - lane));
        putU16 (rec, 16, 120);
        putU16 (rec, 18, 0x0040);
        rec[20] = 64; rec[21] = 100; rec[22] = 128; rec[23] = 128;
        putU32 (rec, 24, startOff);
        putU32 (rec, 28, endOff);
        playlistBlob.insert (playlistBlob.end(), rec.begin(), rec.end());

        if (clipType != "audio")
            continue;

        auto* fade = findFadeGroup (itemIndex, lenTicks,
                                    juce::jlimit (0.0, 1.0, (double) clip.getProperty (IDs::fadeIn, 0.0)),
                                    juce::jlimit (0.0, 1.0, (double) clip.getProperty (IDs::fadeOut, 0.0)));
        if (fade == nullptr)
            continue;

        std::vector<uint8_t> fadeRec (32, 0);
        putU32 (fadeRec, 0, (uint32_t) posTicks);
        putU16 (fadeRec, 4, PATTERN_BASE);
        putU16 (fadeRec, 6, (uint16_t) fade->autoIid);
        putU32 (fadeRec, 8, (uint32_t) fade->lenTicks);
        putU16 (fadeRec, 12, (uint16_t) (MAX_TRACK_IDX - fade->lane));
        putU16 (fadeRec, 16, 120);
        putU16 (fadeRec, 18, 0x0040);
        fadeRec[20] = 64; fadeRec[21] = 100; fadeRec[22] = 128; fadeRec[23] = 128;
        float fadeLen = (float) fade->lenBeats;
        memcpy (fadeRec.data() + 28, &fadeLen, 4);
        playlistBlob.insert (playlistBlob.end(), fadeRec.begin(), fadeRec.end());
        ++fadeClips;
    }

    w.writeBlob (OP_PLAYLIST, playlistBlob.data(), playlistBlob.size());

    for (int t = 0; t < FL_NUM_TRACKS; ++t)
    {
        std::vector<uint8_t> rec (PLAYLIST_TRACK_DEFAULT,
                                  PLAYLIST_TRACK_DEFAULT + sizeof (PLAYLIST_TRACK_DEFAULT));
        putU32 (rec, 0, (uint32_t) (t + 1));

        auto lane = Model::getPlaylistTrack (project, t);
        juce::String laneName;
        if (lane.isValid())
        {
            uint32_t laneColour = 0;
            if (readColour (lane, laneColour))
                putU32 (rec, 4, laneColour);
            rec[12] = (uint8_t) ((bool) lane.getProperty (IDs::enabled, true) ? 1 : 0);
            laneName = lane.getProperty (IDs::name).toString();
        }

        w.writeBlob (OP_TRACK_DATA, rec.data(), rec.size());
        if (laneName.isNotEmpty() && laneName != "Track " + juce::String (t + 1))
            w.writeText (OP_TRACK_NAME, laneName);
    }

    auto mixer = Model::getMixer (project);
    int numTracks = mixer.getNumChildren();
    int fxExported = 0, fxSkipped = 0;

    w.writeU16 (0x64, 0);
    w.writeU8 (0x1d, 1);
    w.writeU8 (0x27, 1);
    w.writeU8 (0x28, 0);
    w.writeU8 (0x1f, 0);
    w.writeU8 (0x26, 1);

    for (int ti = 0; ti < FL_NUM_INSERTS; ++ti)
    {
        auto track = ti < numTracks ? mixer.getChild (ti) : juce::ValueTree();

        uint32_t insertColour = 0;
        if (readColour (track, insertColour))
            w.writeU32 (OP_INSERT_COLOUR, insertColour);

        auto trackName = track.getProperty (IDs::name).toString();
        if (ti > 0 && trackName.isNotEmpty() && trackName != "Insert " + juce::String (ti))
            w.writeText (OP_INSERT_NAME, trackName);

        uint32_t flags = (ti == 0 || ti == FL_NUM_INSERTS - 1) ? 0x0Cu : 0x4Cu;
        if ((bool) track.getProperty (IDs::mute, false)) flags &= ~(1u << 3);
        if ((bool) track.getProperty (IDs::solo, false)) flags |= (1u << 12);
        std::vector<uint8_t> flagBlob (12, 0);
        putU32 (flagBlob, 4, flags);
        w.writeBlob (OP_INSERT_FLAGS, flagBlob.data(), flagBlob.size());

        std::map<int, juce::ValueTree> slotsByIndex;
        for (int si = 0; si < track.getNumChildren(); ++si)
        {
            auto slot = track.getChild (si);
            if (slot.hasType (IDs::Slot) && isExportableSlot (slot))
                slotsByIndex[(int) slot.getProperty (IDs::slotIndex, si)] = slot;
        }

        for (int slot = 0; slot < juce::jmin (Model::numSlotsPerTrack, FL_SLOTS_PER_INSERT); ++slot)
        {
            auto found = slotsByIndex.find (slot);
            if (found != slotsByIndex.end() && found->second.getProperty (IDs::type).toString() != "plugin")
            {
                FLNativeFx::Exported native;
                if (FLNativeFx::exportSlot (found->second.getProperty (IDs::type).toString(),
                                            found->second.getProperty (IDs::pluginState).toString(), native))
                {
                    std::vector<uint8_t> runtime (PLUGIN_RUNTIME_SLOT,
                                                  PLUGIN_RUNTIME_SLOT + sizeof (PLUGIN_RUNTIME_SLOT));
                    putU32 (runtime, 0, (uint32_t) ti);
                    putU32 (runtime, 4, (uint32_t) slot);
                    w.writeText (OP_PLUGIN_INTERNAL, native.internalName);
                    w.writeBlob (OP_PLUGIN_RUNTIME, runtime.data(), runtime.size());
                    w.writeU32 (OP_CHANNEL_ICON, 0);
                    w.writeU32 (OP_CHANNEL_COLOUR, FL_DEFAULT_SLOT_COLOUR);
                    w.writeBlob (OP_PLUGIN_STATE, native.state.data(), native.state.size());
                    ++fxExported;
                }
                else
                    ++fxSkipped;
            }
            else if (found != slotsByIndex.end())
            {
                auto target = parseWrapperTarget (found->second);
                if (target.valid)
                {
                    auto wrapper = buildWrapperBlob (target, true);

                    std::vector<uint8_t> runtime (PLUGIN_RUNTIME_SLOT,
                                                  PLUGIN_RUNTIME_SLOT + sizeof (PLUGIN_RUNTIME_SLOT));
                    putU32 (runtime, 0, (uint32_t) ti);
                    putU32 (runtime, 4, (uint32_t) slot);

                    w.writeText (OP_PLUGIN_INTERNAL, "Fruity Wrapper");
                    w.writeBlob (OP_PLUGIN_RUNTIME, runtime.data(), runtime.size());
                    w.writeText (OP_NAME, target.desc.name);
                    w.writeU32 (OP_CHANNEL_ICON, 0);
                    w.writeU32 (OP_CHANNEL_COLOUR, FL_DEFAULT_SLOT_COLOUR);
                    w.writeBlob (OP_PLUGIN_STATE, wrapper.data(), wrapper.size());
                    ++fxExported;
                }
                else
                    ++fxSkipped;
            }
            w.writeU16 (OP_NEW_SLOT, (uint16_t) slot);
        }

        std::vector<uint8_t> routeTable ((size_t) FL_NUM_INSERTS, 0);
        if (ti > 0 && ti < FL_NUM_INSERTS - 1)
            routeTable[(size_t) juce::jlimit (0, FL_NUM_INSERTS - 1,
                                              (int) track.getProperty (IDs::routeTo, 0))] = 1;
        w.writeBlob (0xeb, routeTable.data(), routeTable.size());
        w.writeU32 (OP_INSERT_INPUT, 0xffffffff);
        w.writeU32 (OP_INSERT_END, ti == 0 ? 0u : 0xffffffffu);
    }

    {
        std::vector<uint8_t> paramBlob;
        auto appendParam = [&paramBlob] (uint8_t id, uint8_t group, uint16_t target, int32_t value)
        {
            std::vector<uint8_t> rec (12, 0);
            rec[4] = id;
            rec[5] = group;
            putU16 (rec, 6, target);
            putU32 (rec, 8, (uint32_t) value);
            paramBlob.insert (paramBlob.end(), rec.begin(), rec.end());
        };

        appendParam (0, 0x00, 0x4000, 12800);

        for (int ti = 0; ti < FL_NUM_INSERTS; ++ti)
        {
            auto track = ti < numTracks ? mixer.getChild (ti) : juce::ValueTree();
            auto base = (uint16_t) (0x2000 + ti * 0x40);

            std::map<int, juce::ValueTree> slotsByIndex;
            for (int si = 0; si < track.getNumChildren(); ++si)
            {
                auto slot = track.getChild (si);
                if (slot.hasType (IDs::Slot) && isExportableSlot (slot))
                    slotsByIndex[(int) slot.getProperty (IDs::slotIndex, si)] = slot;
            }

            for (int slot = 0; slot < juce::jmin (Model::numSlotsPerTrack, FL_SLOTS_PER_INSERT); ++slot)
            {
                auto found = slotsByIndex.find (slot);
                bool slotEnabled = true;
                double slotMix = 1.0;
                if (found != slotsByIndex.end())
                {
                    slotEnabled = (bool) found->second.getProperty (IDs::enabled, true);
                    slotMix = (double) found->second.getProperty (IDs::mix, 1.0);
                }
                appendParam (0, 0x1f, (uint16_t) (base + slot), slotEnabled ? 1 : 0);
                appendParam (1, 0x1f, (uint16_t) (base + slot),
                             juce::roundToInt (juce::jlimit (0.0, 1.0, slotMix) * 12800.0));
            }

            appendParam (192, 0x1f, base, juce::roundToInt (juce::jlimit (0.0, 1.25,
                            (double) track.getProperty (IDs::volume, 1.0)) * 12800.0));
            appendParam (193, 0x1f, base, juce::roundToInt (juce::jlimit (-1.0, 1.0,
                            (double) track.getProperty (IDs::pan, 0.0)) * 6400.0));
            appendParam (194, 0x1f, base, juce::roundToInt (juce::jlimit (-1.0, 1.0,
                            (double) track.getProperty (IDs::stereoSep, 0.0)) * 64.0));
            appendParam (208, 0x1f, base, 0);
            appendParam (209, 0x1f, base, 0);
            appendParam (210, 0x1f, base, 0);
            appendParam (216, 0x1f, base, 5777);
            appendParam (217, 0x1f, base, 33145);
            appendParam (218, 0x1f, base, 55825);
            appendParam (224, 0x1f, base, 17500);
            appendParam (225, 0x1f, base, 17500);
            appendParam (226, 0x1f, base, 17500);
            if (ti < 100)
                for (uint8_t send = 164; send <= 167; ++send)
                    appendParam (send, 0x1f, base, 0);
            if (ti < 105)
                appendParam (168, 0x1f, base, 0);
            appendParam (190, 0x1f, base, 0);
        }

        w.writeBlob (OP_MIXER_PARAMS, paramBlob.data(), paramBlob.size());
    }

    w.writeU32 (0x85, FL_NUM_INSERTS - 1);

    int numChannels = nextIid;

    juce::MemoryBlock flp;
    flp.append ("FLhd", 4);
    uint8_t header[10] = { 6, 0, 0, 0,
                           0, 0,
                           (uint8_t) (numChannels & 0xff), (uint8_t) ((numChannels >> 8) & 0xff),
                           (uint8_t) (FL_PPQ & 0xff), (uint8_t) ((FL_PPQ >> 8) & 0xff) };
    flp.append (header, 10);

    flp.append ("FLdt", 4);
    auto& evData = w.getData();
    uint32_t dataLen = (uint32_t) evData.getSize();
    uint8_t dlBuf[4] = { (uint8_t) (dataLen & 0xff), (uint8_t) ((dataLen >> 8) & 0xff),
                         (uint8_t) ((dataLen >> 16) & 0xff), (uint8_t) ((dataLen >> 24) & 0xff) };
    flp.append (dlBuf, 4);
    flp.append (evData.getData(), evData.getSize());

    summary = "Exported " + juce::String (numChannels) + " channels, "
            + juce::String (numPatterns) + " patterns (" + juce::String (notesExported) + " notes), "
            + juce::String (patClips + audioClips) + " playlist clips, "
            + juce::String (numTracks) + " mixer tracks, "
            + juce::String (fxExported) + " mixer effects.";
    if (vstChannels > 0)
        summary += " " + juce::String (vstChannels) + " VST instrument(s) linked ("
                 + juce::String (vstStates) + " with saved state).";
    if (unresolvedPlugins > 0)
        summary += " " + juce::String (unresolvedPlugins)
                 + " plugin channel(s) had no loadable plugin description and were exported as empty samplers.";
    if (fxSkipped > 0)
        summary += " " + juce::String (fxSkipped) + " mixer effect(s) skipped.";
    if (clipsSkipped > 0)
        summary += " " + juce::String (clipsSkipped) + " clip(s) skipped.";
    if (fadeClips > 0)
        summary += " " + juce::String (fadeClips) + " clip fade(s) emulated with "
                 + juce::String ((int) fadeGroups.size()) + " channel-volume automation clip(s).";
    if (fadesDropped > 0)
        summary += " " + juce::String (fadesDropped)
                 + " clip fade(s) dropped (source length unreadable or no free playlist lane).";

    return flp;
}

uint32_t zipCrc32 (const void* data, size_t numBytes)
{
    static const std::array<uint32_t, 256> table = []
    {
        std::array<uint32_t, 256> t {};
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();

    auto* bytes = static_cast<const uint8_t*> (data);
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < numBytes; ++i)
        crc = table[(crc ^ bytes[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

void dosDateTime (const juce::File& f, uint16_t& dosTime, uint16_t& dosDate)
{
    auto t = f.getLastModificationTime();
    auto year = juce::jlimit (1980, 2107, t.getYear());
    dosDate = (uint16_t) (((year - 1980) << 9) | ((t.getMonth() + 1) << 5) | t.getDayOfMonth());
    dosTime = (uint16_t) ((t.getHours() << 11) | (t.getMinutes() << 5) | (t.getSeconds() / 2));
}

bool writeZipArchive (const juce::Array<juce::File>& files, juce::OutputStream& out)
{
    struct Entry
    {
        juce::String name;
        juce::MemoryBlock compressed;
        uint32_t crc = 0;
        uint32_t uncompressedSize = 0;
        uint32_t localHeaderOffset = 0;
        uint16_t dosTime = 0, dosDate = 0;
    };

    std::vector<Entry> entries;

    for (auto& f : files)
    {
        juce::MemoryBlock raw;
        if (! f.loadFileAsData (raw))
            return false;

        Entry e;
        e.name = f.getFileName();
        e.crc = zipCrc32 (raw.getData(), raw.getSize());
        e.uncompressedSize = (uint32_t) raw.getSize();
        dosDateTime (f, e.dosTime, e.dosDate);

        {
            juce::MemoryOutputStream mo (e.compressed, false);
            juce::GZIPCompressorOutputStream deflater (mo, 9, -15);
            if (! deflater.write (raw.getData(), raw.getSize()))
                return false;
            deflater.flush();
        }

        entries.push_back (std::move (e));
    }

    auto writeHeaderFields = [&out] (uint32_t crc, uint32_t compSize, uint32_t uncompSize,
                                     uint16_t dosTime, uint16_t dosDate, const juce::String& name)
    {
        out.writeShort ((short) 8);
        out.writeShort ((short) dosTime);
        out.writeShort ((short) dosDate);
        out.writeInt ((int) crc);
        out.writeInt ((int) compSize);
        out.writeInt ((int) uncompSize);
        out.writeShort ((short) name.getNumBytesAsUTF8());
        out.writeShort (0);
    };

    for (auto& e : entries)
    {
        e.localHeaderOffset = (uint32_t) out.getPosition();
        out.writeInt (0x04034b50);
        out.writeShort (20);
        out.writeShort (0);
        writeHeaderFields (e.crc, (uint32_t) e.compressed.getSize(), e.uncompressedSize,
                           e.dosTime, e.dosDate, e.name);
        out.write (e.name.toRawUTF8(), e.name.getNumBytesAsUTF8());
        out.write (e.compressed.getData(), e.compressed.getSize());
    }

    auto centralStart = (uint32_t) out.getPosition();

    for (auto& e : entries)
    {
        out.writeInt (0x02014b50);
        out.writeShort (20);
        out.writeShort (20);
        out.writeShort (0);
        writeHeaderFields (e.crc, (uint32_t) e.compressed.getSize(), e.uncompressedSize,
                           e.dosTime, e.dosDate, e.name);
        out.writeShort (0);
        out.writeShort (0);
        out.writeShort (0);
        out.writeInt ((int) 0x81B60000);
        out.writeInt ((int) e.localHeaderOffset);
        out.write (e.name.toRawUTF8(), e.name.getNumBytesAsUTF8());
    }

    auto centralSize = (uint32_t) out.getPosition() - centralStart;

    out.writeInt (0x06054b50);
    out.writeShort (0);
    out.writeShort (0);
    out.writeShort ((short) entries.size());
    out.writeShort ((short) entries.size());
    out.writeInt ((int) centralSize);
    out.writeInt ((int) centralStart);
    out.writeShort (0);

    return true;
}

}

FLPExport::Result FLPExport::exportAsZip (const juce::ValueTree& project, const juce::File& zipFile,
                                          PathResolver resolve)
{
    Result result;

    SampleBundle samples;
    samples.resolve = std::move (resolve);
    juce::String summary;
    auto flpData = buildFlpData (project, samples, summary);

    auto projectName = juce::File::createLegalFileName (project.getProperty (IDs::name).toString());
    if (projectName.isEmpty())
        projectName = "mikeDAW Export";

    auto tempDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("mikeDAW_export_" + juce::String (juce::Random::getSystemRandom().nextInt64()));
    tempDir.createDirectory();

    auto flpFile = tempDir.getChildFile (projectName + ".flp");
    flpFile.replaceWithData (flpData.getData(), flpData.getSize());

    for (auto& [leafName, sourcePath] : samples.renames)
    {
        auto dest = tempDir.getChildFile (leafName);
        if (sourcePath.startsWith ("builtin:"))
            AudioEngine::renderBuiltinSampleToWav (sourcePath.fromFirstOccurrenceOf (":", false, false), dest);
        else
            juce::File (sourcePath).copyFileTo (dest);
    }

    if (zipFile.existsAsFile())
        zipFile.deleteFile();

    juce::Array<juce::File> zipEntries;
    for (auto di = juce::DirectoryIterator (tempDir, false); di.next();)
        zipEntries.add (di.getFile());

    auto outStream = zipFile.createOutputStream();
    if (outStream == nullptr)
    {
        tempDir.deleteRecursively();
        result.error = "Could not create " + zipFile.getFullPathName();
        return result;
    }

    bool wrote = writeZipArchive (zipEntries, *outStream);
    outStream.reset();

    tempDir.deleteRecursively();

    if (! wrote)
    {
        result.error = "Failed to write ZIP data.";
        return result;
    }

    result.success = true;
    result.summary = summary;
    return result;
}
