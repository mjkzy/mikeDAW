#pragma once

#include <cstdint>

namespace bridge32
{

constexpr uint32_t magicValue      = 0xB397D0C5;
constexpr uint32_t protocolVersion = 2;

constexpr int32_t maxBlockSize  = 4096;
constexpr int32_t numIOChannels = 2;
constexpr int32_t maxMidiEvents = 2048;
constexpr int32_t paramRingSize = 1024;
constexpr int32_t maxParamName  = 64;
constexpr int32_t paramInfoPage = 256;
constexpr uint32_t chunkAreaBytes = 16u * 1024u * 1024u;

constexpr uint32_t startupTimeoutMs = 20000;
constexpr uint32_t processTimeoutMs = 2000;
constexpr uint32_t rpcTimeoutMs     = 10000;

enum Command : uint32_t
{
    cmdNone = 0,
    cmdPrepare,
    cmdSuspend,
    cmdProcess,
    cmdSetParam,
    cmdGetParam,
    cmdGetParamInfo,
    cmdGetChunk,
    cmdSetChunk,
    cmdGetProgram,
    cmdSetProgram,
    cmdGetProgramName,
    cmdShowEditor,
    cmdHideEditor,
    cmdShutdown
};

enum UiCommand : uint32_t
{
    uiNone = 0,
    uiShow,
    uiHide
};

enum State : uint32_t
{
    stateStarting = 0,
    stateReady    = 1,
    stateDead     = 2
};

#pragma pack(push, 4)

struct PluginInfo
{
    int32_t numParams;
    int32_t numPrograms;
    int32_t numInputs;
    int32_t numOutputs;
    int32_t initialDelay;
    int32_t uniqueId;
    int32_t version;
    uint32_t hasEditor;
    uint32_t hasChunk;
    uint32_t isSynth;
    char name[128];
    char vendor[128];
};

struct ParamInfo
{
    char name[maxParamName];
    char label[32];
    float defaultValue;
    uint32_t pad;
};

struct MidiEvent
{
    uint32_t sampleOffset;
    uint8_t data[4];
    uint32_t size;
};

struct TimeInfo
{
    double samplePos;
    double sampleRate;
    double ppqPos;
    double tempo;
    double barStartPos;
    int32_t timeSigNumerator;
    int32_t timeSigDenominator;
    uint32_t playing;
    uint32_t pad;
};

struct ParamChange
{
    int32_t index;
    float value;
};

struct ParamRing
{
    volatile uint32_t head;
    volatile uint32_t tail;
    ParamChange entries[paramRingSize];
};

struct SharedHeader
{
    uint32_t magic;
    uint32_t version;
    volatile uint32_t state;
    char loadError[256];

    PluginInfo info;

    volatile uint32_t command;
    double sampleRate;
    int32_t blockSize;
    int32_t numSamples;
    int32_t midiCount;
    int32_t paramIndex;
    float paramValue;
    uint32_t dataBytes;
    TimeInfo timeInfo;

    volatile uint32_t status;
    int32_t intReply;
    float floatReply;
    char strReply[256];

    volatile uint32_t editorVisible;

    ParamRing paramRing;

    MidiEvent midi[maxMidiEvents];
    float audioIn [numIOChannels][maxBlockSize];
    float audioOut[numIOChannels][maxBlockSize];

    volatile uint32_t uiCommand;
    uint32_t uiPad;
    uint64_t uiParentHwnd;
    volatile uint64_t editorHwnd;
    volatile int32_t editorWidth;
    volatile int32_t editorHeight;
};

#pragma pack(pop)

constexpr uint32_t headerBytes  = (uint32_t) sizeof (SharedHeader);
constexpr uint32_t mappingBytes = headerBytes + chunkAreaBytes;

static_assert (sizeof (ParamChange) == 8, "layout must match across x86/x64");
static_assert (sizeof (MidiEvent) == 12, "layout must match across x86/x64");
static_assert ((paramRingSize & (paramRingSize - 1)) == 0, "ring size must be a power of two");

}
