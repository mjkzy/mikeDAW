#pragma once
#include <JuceHeader.h>
#include "BuiltinEffects.h"

class GrossBeatProcessor : public BuiltinEffectBase
{
public:
    static constexpr int    numSlots      = 36;
    static constexpr int    maxPoints     = 4096;
    static constexpr double envBeats      = 4.0;
    static constexpr double timeRangeBeats = 8.0;

    enum Curve { singleCurve = 0, doubleCurve, hold, stairs, smoothStairs, pulse, wave, halfSine,
                 altSingle, altDouble, numCurves };

    struct Point
    {
        double x = 0.0;
        float  y = 1.0f;
        float  tension = 0.0f;
        int    curve = singleCurve;
        int    aux = 2;
    };

    struct Slot
    {
        juce::String name;
        std::vector<Point> points;
        std::array<uint8_t, 15> extFlags {};
        uint32_t extA = 3, extB = 3, extTrailer = 0;
    };

    GrossBeatProcessor();

    juce::String effectName() const override { return "Gross Beat"; }

    void prepareToPlay (double sampleRate, int blockSize) override;
    void reset() override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    static float shape (int curve, float tension, float u);
    static float evaluate (const std::vector<Point>& pts, double x);
    static int   auxForTension (float t) { return t < 0.0f ? 0xff : (t > 0.0f ? 1 : 2); }
    static Slot  factorySlot (int index);
    static int   slotIndex (bool volume, int i) { return (volume ? numSlots : 0) + i; }

    Slot getSlot (int index) const;
    void setSlot (int index, Slot s);
    void swapSlots (int a, int b);
    void resetAllSlots();
    void notifySlotsEdited();
    uint32_t getSlotsVersion() const { return slotsVersion.load(); }

    std::vector<uint8_t> getExtHeader() const;
    void setExtHeader (std::vector<uint8_t> header);

    int    activeSlot (bool volume) const  { return volume ? uiActiveVol.load() : uiActiveTime.load(); }
    int    pendingSlot (bool volume) const { return volume ? uiPendingVol.load() : uiPendingTime.load(); }
    double envelopePos (bool volume) const { return volume ? uiVolPos.load() : uiTimePos.load(); }

    juce::AudioParameterFloat*  timeMix    = nullptr;
    juce::AudioParameterFloat*  volMix     = nullptr;
    juce::AudioParameterInt*    timeSlot   = nullptr;
    juce::AudioParameterInt*    volSlot    = nullptr;
    juce::AudioParameterFloat*  attack     = nullptr;
    juce::AudioParameterFloat*  release    = nullptr;
    juce::AudioParameterFloat*  tension    = nullptr;
    juce::AudioParameterBool*   clickReduce = nullptr;
    juce::AudioParameterChoice* trigSync   = nullptr;
    juce::AudioParameterChoice* posSync    = nullptr;
    juce::AudioParameterBool*   linkSlots  = nullptr;
    juce::AudioParameterChoice* holdMode   = nullptr;
    juce::AudioParameterBool*   holdOn     = nullptr;

private:
    struct Cursor
    {
        int seg = 1;
        float eval (const std::vector<Point>& pts, double x);
    };

    static double syncBeats (int choiceIndex);
    void   resync (double songStart);
    bool   refreshLive (int wantTime, int wantVol, int wantPendTime, int wantPendVol);
    float  readRing (int ch, double pos) const;
    void   engageHold (int mode, double spb);

    mutable juce::SpinLock slotLock;
    std::array<Slot, numSlots * 2> slots;
    std::vector<uint8_t> extHeader;
    std::atomic<uint32_t> slotsVersion { 1 };

    std::vector<Point> liveTime, liveVol, livePendTime, livePendVol;
    uint32_t liveVersion = 0;
    int liveTimeIdx = -1, liveVolIdx = -1, livePendTimeIdx = -1, livePendVolIdx = -1;

    double sr = 44100.0;
    juce::AudioBuffer<float> ring;
    int     ringMask = 0;
    int64_t written = 0;
    int64_t recorded = 0;

    bool   wasPlaying = false;
    double lastPpqEnd = 0.0;
    double songPos = 0.0;
    double timeStart = 0.0, volStart = 0.0;
    int    activeTime = 0, activeVol = 0;
    int    pendTime = -1, pendVol = -1;
    double pendTimeAt = 0.0, pendVolAt = 0.0;
    Cursor curTime, curVol;

    bool   havePrevRead = false;
    double prevRead = 0.0, prevStep = 1.0;
    double fadeOldPos = 0.0, fadeOldStep = 1.0;
    int    fadeRem = 0, fadeLen = 1;

    float  gainState = 1.0f;

    juce::AudioBuffer<float> holdBuf;
    int    holdState = 0, holdLen = 0, holdPhase = 0, holdRecPos = 0;
    float  holdMix = 0.0f;

    std::atomic<int>    uiActiveTime { 0 }, uiActiveVol { 0 }, uiPendingTime { -1 }, uiPendingVol { -1 };
    std::atomic<double> uiTimePos { -1.0 }, uiVolPos { -1.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GrossBeatProcessor)
};
