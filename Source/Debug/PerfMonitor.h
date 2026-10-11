#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

class PerfMonitor
{
public:
    PerfMonitor();
    ~PerfMonitor();

    struct ThreadInfo
    {
        juce::uint32 tid = 0;
        juce::String name;
        float cpuPercent = 0.0f;
    };

    struct Sample
    {
        double timeSeconds = 0.0;
        float processCpu = 0.0f;
        juce::uint64 privateBytes = 0, workingSet = 0;
        juce::uint32 handles = 0, gdiObjects = 0, userObjects = 0;
    };

    struct BaselineDiff
    {
        double seconds = 0.0;
        juce::int64 privateDelta = 0, workingSetDelta = 0;
        int handleDelta = 0, gdiDelta = 0;
    };

    struct HitchSummary
    {
        juce::String wallTime;
        double durationMs = 0.0;
        int index = 0;
    };

    Sample latest();
    std::vector<Sample> history();
    std::vector<ThreadInfo> threads();
    int numLogicalCores() const noexcept { return logicalCores; }

    double memorySlopeBytesPerMin (int windowSeconds);
    void markBaseline();
    BaselineDiff baselineDiff();

    struct UiResponsiveness
    {
        double medianMs = 0.0, p95Ms = 0.0, worstMs = 0.0;
        int lateTicks = 0, samples = 0;
    };
    UiResponsiveness uiResponsiveness();

    std::vector<HitchSummary> hitches();
    juce::StringArray symbolizeHitch (int index);
    void clearHitches();

    std::atomic<double> hitchThresholdMs { 500.0 };
    std::atomic<int> totalHitches { 0 };
    std::atomic<int> majorHitches { 0 };

    juce::String buildReportText();
    juce::File writeReport (const juce::String& extraSection = {});

private:
    struct ThreadTimes { juce::uint64 kernel = 0, user = 0; };
    struct Hitch { juce::Time wallTime; double durationMs = 0.0; std::vector<void*> frames; };

    struct SamplerThread : juce::Thread
    {
        explicit SamplerThread (PerfMonitor& o) : juce::Thread ("mikeDAW Perf Sampler"), owner (o) {}
        void run() override { owner.runSampler (*this); }
        PerfMonitor& owner;
    };
    struct WatcherThread : juce::Thread
    {
        explicit WatcherThread (PerfMonitor& o) : juce::Thread ("mikeDAW Hitch Watcher"), owner (o) {}
        void run() override { owner.runWatcher (*this); }
        PerfMonitor& owner;
    };
    struct Heartbeat : juce::Timer
    {
        explicit Heartbeat (PerfMonitor& o) : owner (o) {}
        void timerCallback() override;
        PerfMonitor& owner;
    };

    void recordTick (double intervalMs);
    void runSampler (juce::Thread&);
    void runWatcher (juce::Thread&);
    std::vector<ThreadInfo> sampleThreads (double wallDelta);
    int captureMessageThreadStack (void** frames, int maxFrames);
    void recordHitch (double durationMs, void* const* frames, int frameCount);
    void ensureSymInitialized();
    static juce::File reportDir();

    static constexpr int historyCapacity = 600;

    int logicalCores = 1;
    double startMs = 0.0;

    std::mutex sampleMutex;
    std::vector<Sample> ring;
    int ringWrite = 0, ringCount = 0;
    Sample latestSample;
    std::vector<ThreadInfo> threadInfos;
    Sample baseline;
    bool haveBaseline = false;

    std::map<juce::uint32, ThreadTimes> prevThreadTimes;
    juce::uint64 prevProcCpu100ns = 0;
    double prevWallSec = 0.0;
    bool primed = false;

    static constexpr int tickCapacity = 400;
    std::mutex tickMutex;
    std::array<double, tickCapacity> tickRing {};
    int tickWrite = 0, tickCount = 0;

    std::atomic<juce::int64> heartbeat { 0 };
    std::mutex hitchMutex;
    std::deque<Hitch> hitchDeque;

    void* msgThreadHandle = nullptr;
    bool symInitialized = false;

    SamplerThread samplerThread { *this };
    WatcherThread watcherThread { *this };
    Heartbeat heartbeatTimer { *this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PerfMonitor)
};
