#include "PerfMonitor.h"
#include "DebugFlags.h"
#include <algorithm>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #define PSAPI_VERSION 2
 #include <windows.h>
 #include <psapi.h>
 #include <tlhelp32.h>
 #include <dbghelp.h>
 #pragma comment (lib, "dbghelp.lib")
#endif

namespace
{
    constexpr int kMaxHitchFrames = 64;

    std::mutex dbghelpMutex;

   #if JUCE_WINDOWS
    juce::uint64 fileTimeToU64 (const FILETIME& ft) noexcept
    {
        return ((juce::uint64) ft.dwHighDateTime << 32) | (juce::uint64) ft.dwLowDateTime;
    }

    juce::String threadDescription (HANDLE th)
    {
        using Fn = HRESULT (WINAPI*) (HANDLE, PWSTR*);
        static Fn fn = reinterpret_cast<Fn> (
            ::GetProcAddress (::GetModuleHandleW (L"kernel32.dll"), "GetThreadDescription"));
        if (fn == nullptr)
            return {};
        PWSTR desc = nullptr;
        juce::String out;
        if (SUCCEEDED (fn (th, &desc)) && desc != nullptr)
        {
            out = juce::String (desc);
            ::LocalFree (desc);
        }
        return out;
    }
   #endif
}

PerfMonitor::PerfMonitor()
{
    logicalCores = juce::jmax (1, juce::SystemStats::getNumCpus());
    startMs = juce::Time::getMillisecondCounterHiRes();
    ring.resize ((size_t) historyCapacity);

   #if JUCE_WINDOWS
    HANDLE dup = nullptr;
    ::DuplicateHandle (::GetCurrentProcess(), ::GetCurrentThread(), ::GetCurrentProcess(),
                       &dup, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                       FALSE, 0);
    msgThreadHandle = dup;
   #endif

    heartbeat.store (juce::Time::getHighResolutionTicks(), std::memory_order_relaxed);
    heartbeatTimer.startTimer (25);
    samplerThread.startThread();
    watcherThread.startThread();
}

PerfMonitor::~PerfMonitor()
{
    heartbeatTimer.stopTimer();
    samplerThread.stopThread (2000);
    watcherThread.stopThread (2000);

   #if JUCE_WINDOWS
    if (msgThreadHandle != nullptr)
        ::CloseHandle ((HANDLE) msgThreadHandle);
    std::lock_guard<std::mutex> lk (dbghelpMutex);
    if (symInitialized)
    {
        ::SymCleanup (::GetCurrentProcess());
        symInitialized = false;
    }
   #endif
}

juce::File PerfMonitor::reportDir()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("mikeDAW").getChildFile ("PerfReports");
}

void PerfMonitor::Heartbeat::timerCallback()
{
    const auto now = juce::Time::getHighResolutionTicks();
    const auto prev = owner.heartbeat.exchange (now, std::memory_order_relaxed);

    if (prev != 0)
    {
        const double ms = (double) (now - prev)
                            * 1000.0 / (double) juce::Time::getHighResolutionTicksPerSecond();
        owner.recordTick (ms);
    }
}

void PerfMonitor::recordTick (double intervalMs)
{
    const std::lock_guard<std::mutex> lk (tickMutex);
    tickRing[(size_t) tickWrite] = intervalMs;
    tickWrite = (tickWrite + 1) % tickCapacity;
    if (tickCount < tickCapacity)
        ++tickCount;
}

PerfMonitor::UiResponsiveness PerfMonitor::uiResponsiveness()
{
    std::vector<double> v;
    {
        const std::lock_guard<std::mutex> lk (tickMutex);
        v.assign (tickRing.begin(), tickRing.begin() + tickCount);
    }
    UiResponsiveness r;
    if (v.empty())
        return r;

    std::sort (v.begin(), v.end());
    r.samples = (int) v.size();
    r.medianMs = v[v.size() / 2];
    r.p95Ms = v[(size_t) ((double) (v.size() - 1) * 0.95)];
    r.worstMs = v.back();
    for (auto d : v)
        if (d > 40.0)
            ++r.lateTicks;
    return r;
}
void PerfMonitor::runSampler (juce::Thread& self)
{
   #if JUCE_WINDOWS
    const HANDLE proc = ::GetCurrentProcess();
    while (! self.threadShouldExit())
    {
        self.wait (1000);
        if (self.threadShouldExit())
            break;

        const double nowSec = (juce::Time::getMillisecondCounterHiRes() - startMs) * 0.001;

        FILETIME ftCreate, ftExit, ftKernel, ftUser;
        juce::uint64 procCpu = 0;
        if (::GetProcessTimes (proc, &ftCreate, &ftExit, &ftKernel, &ftUser))
            procCpu = fileTimeToU64 (ftKernel) + fileTimeToU64 (ftUser);

        PROCESS_MEMORY_COUNTERS_EX pmc {};
        pmc.cb = sizeof (pmc);
        ::GetProcessMemoryInfo (proc, reinterpret_cast<PROCESS_MEMORY_COUNTERS*> (&pmc), sizeof (pmc));

        DWORD handleCount = 0;
        ::GetProcessHandleCount (proc, &handleCount);

        Sample s;
        s.timeSeconds = nowSec;
        s.privateBytes = (juce::uint64) pmc.PrivateUsage;
        s.workingSet   = (juce::uint64) pmc.WorkingSetSize;
        s.handles = (juce::uint32) handleCount;
        s.gdiObjects  = (juce::uint32) ::GetGuiResources (proc, GR_GDIOBJECTS);
        s.userObjects = (juce::uint32) ::GetGuiResources (proc, GR_USEROBJECTS);

        const double wallDelta = primed ? (nowSec - prevWallSec) : 0.0;
        if (primed && wallDelta > 0.0)
        {
            const double cpuDeltaSec = (double) (procCpu - prevProcCpu100ns) * 1.0e-7;
            s.processCpu = (float) juce::jlimit (0.0, 100.0,
                               cpuDeltaSec / (wallDelta * (double) logicalCores) * 100.0);
        }

        auto tinfos = sampleThreads (wallDelta);

        prevProcCpu100ns = procCpu;
        prevWallSec = nowSec;
        primed = true;

        {
            std::lock_guard<std::mutex> lk (sampleMutex);
            ring[(size_t) ringWrite] = s;
            ringWrite = (ringWrite + 1) % historyCapacity;
            if (ringCount < historyCapacity)
                ++ringCount;
            latestSample = s;
            threadInfos = std::move (tinfos);
        }
    }
   #else
    while (! self.threadShouldExit())
        self.wait (1000);
   #endif
}

std::vector<PerfMonitor::ThreadInfo> PerfMonitor::sampleThreads (double wallDelta)
{
    std::vector<ThreadInfo> out;
   #if JUCE_WINDOWS
    const DWORD pid = ::GetCurrentProcessId();
    HANDLE snap = ::CreateToolhelp32Snapshot (TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return out;

    std::map<juce::uint32, ThreadTimes> seen;
    THREADENTRY32 te {};
    te.dwSize = sizeof (te);
    if (::Thread32First (snap, &te))
    {
        do
        {
            if (te.th32OwnerProcessID != pid)
                continue;
            const DWORD tid = te.th32ThreadID;
            HANDLE th = ::OpenThread (THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
            if (th == nullptr)
                continue;

            FILETIME c, e, k, u;
            if (::GetThreadTimes (th, &c, &e, &k, &u))
            {
                const ThreadTimes tt { fileTimeToU64 (k), fileTimeToU64 (u) };
                const juce::uint64 total = tt.kernel + tt.user;
                seen[(juce::uint32) tid] = tt;

                float cpu = 0.0f;
                const auto it = prevThreadTimes.find ((juce::uint32) tid);
                if (it != prevThreadTimes.end() && wallDelta > 0.0)
                {
                    const juce::uint64 prevTotal = it->second.kernel + it->second.user;
                    const double d = (double) (total - prevTotal) * 1.0e-7;
                    cpu = (float) juce::jlimit (0.0, 100.0, d / wallDelta * 100.0);
                }
                out.push_back ({ (juce::uint32) tid, threadDescription (th), cpu });
            }
            ::CloseHandle (th);
        }
        while (::Thread32Next (snap, &te));
    }
    ::CloseHandle (snap);
    prevThreadTimes.swap (seen);
   #else
    juce::ignoreUnused (wallDelta);
   #endif
    return out;
}
void PerfMonitor::runWatcher (juce::Thread& self)
{
    juce::int64 prevBeat = heartbeat.load (std::memory_order_relaxed);
    double maxGap = 0.0;
    bool captured = false;
    int frameCount = 0;
    void* frames[kMaxHitchFrames] = {};

    const double ticksPerMs = (double) juce::Time::getHighResolutionTicksPerSecond() / 1000.0;

    while (! self.threadShouldExit())
    {
        self.wait (25);
        const juce::int64 beat = heartbeat.load (std::memory_order_relaxed);
        const juce::int64 now = juce::Time::getHighResolutionTicks();

        if (beat != prevBeat)
        {
            if (maxGap >= 100.0)
                recordHitch (maxGap, frames, captured ? frameCount : 0);
            prevBeat = beat;
            maxGap = 0.0;
            captured = false;
            frameCount = 0;
            continue;
        }

        const double gapMs = ticksPerMs > 0.0 ? (double) (now - beat) / ticksPerMs : 0.0;
        if (gapMs > maxGap)
            maxGap = gapMs;
        if (gapMs > 250.0 && ! captured)
        {
            captured = true;
            frameCount = captureMessageThreadStack (frames, kMaxHitchFrames);
        }
    }
}

int PerfMonitor::captureMessageThreadStack (void** frames, int maxFrames)
{
   #if JUCE_WINDOWS
    if (msgThreadHandle == nullptr)
        return 0;
    const HANDLE th = (HANDLE) msgThreadHandle;
    const HANDLE proc = ::GetCurrentProcess();

    std::lock_guard<std::mutex> lk (dbghelpMutex);
    ensureSymInitialized();

    if (::SuspendThread (th) == (DWORD) -1)
        return 0;

    int count = 0;
    CONTEXT ctx {};
    ctx.ContextFlags = CONTEXT_FULL;
    if (::GetThreadContext (th, &ctx))
    {
        STACKFRAME64 sf {};
        sf.AddrPC.Offset    = ctx.Rip; sf.AddrPC.Mode    = AddrModeFlat;
        sf.AddrFrame.Offset = ctx.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
        sf.AddrStack.Offset = ctx.Rsp; sf.AddrStack.Mode = AddrModeFlat;
        while (count < maxFrames
               && ::StackWalk64 (IMAGE_FILE_MACHINE_AMD64, proc, th, &sf, &ctx, nullptr,
                                 ::SymFunctionTableAccess64, ::SymGetModuleBase64, nullptr))
        {
            if (sf.AddrPC.Offset == 0)
                break;
            frames[count++] = (void*) sf.AddrPC.Offset;
        }
    }
    ::ResumeThread (th);
    return count;
   #else
    juce::ignoreUnused (frames, maxFrames);
    return 0;
   #endif
}

void PerfMonitor::recordHitch (double durationMs, void* const* frames, int frameCount)
{
    Hitch h;
    h.wallTime = juce::Time::getCurrentTime();
    h.durationMs = durationMs;
    if (frameCount > 0)
        h.frames.assign (frames, frames + frameCount);

    totalHitches.fetch_add (1, std::memory_order_relaxed);
    if (durationMs >= hitchThresholdMs.load (std::memory_order_relaxed))
        majorHitches.fetch_add (1, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lk (hitchMutex);
        hitchDeque.push_front (std::move (h));
        while (hitchDeque.size() > 100)
            hitchDeque.pop_back();
    }

    if (DebugFlags::logHitchesToFile.load (std::memory_order_relaxed))
    {
        auto dir = reportDir();
        dir.createDirectory();
        dir.getChildFile ("hitches.log").appendText (
            juce::Time::getCurrentTime().toString (true, true)
            + "\t" + juce::String (durationMs, 1) + " ms\t"
            + juce::String (frameCount) + " frames\n");
    }
}

void PerfMonitor::ensureSymInitialized()
{
   #if JUCE_WINDOWS
    if (symInitialized)
        return;
    ::SymSetOptions (SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    if (::SymInitialize (::GetCurrentProcess(), nullptr, TRUE))
        symInitialized = true;
   #endif
}
PerfMonitor::Sample PerfMonitor::latest()
{
    std::lock_guard<std::mutex> lk (sampleMutex);
    return latestSample;
}

std::vector<PerfMonitor::Sample> PerfMonitor::history()
{
    std::lock_guard<std::mutex> lk (sampleMutex);
    std::vector<Sample> out;
    out.reserve ((size_t) ringCount);
    const int start = (ringCount < historyCapacity) ? 0 : ringWrite;
    for (int n = 0; n < ringCount; ++n)
        out.push_back (ring[(size_t) ((start + n) % historyCapacity)]);
    return out;
}

std::vector<PerfMonitor::ThreadInfo> PerfMonitor::threads()
{
    std::lock_guard<std::mutex> lk (sampleMutex);
    return threadInfos;
}

double PerfMonitor::memorySlopeBytesPerMin (int windowSeconds)
{
    auto h = history();
    if (h.size() < 2)
        return 0.0;
    const double cutoff = h.back().timeSeconds - (double) windowSeconds;
    double n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (auto& s : h)
    {
        if (s.timeSeconds < cutoff)
            continue;
        const double x = s.timeSeconds, y = (double) s.privateBytes;
        n += 1.0; sx += x; sy += y; sxx += x * x; sxy += x * y;
    }
    const double denom = n * sxx - sx * sx;
    if (n < 2.0 || denom == 0.0)
        return 0.0;
    return ((n * sxy - sx * sy) / denom) * 60.0;
}

void PerfMonitor::markBaseline()
{
    std::lock_guard<std::mutex> lk (sampleMutex);
    baseline = latestSample;
    haveBaseline = true;
}

PerfMonitor::BaselineDiff PerfMonitor::baselineDiff()
{
    std::lock_guard<std::mutex> lk (sampleMutex);
    BaselineDiff d;
    if (! haveBaseline)
        return d;
    d.seconds         = latestSample.timeSeconds - baseline.timeSeconds;
    d.privateDelta    = (juce::int64) latestSample.privateBytes - (juce::int64) baseline.privateBytes;
    d.workingSetDelta = (juce::int64) latestSample.workingSet   - (juce::int64) baseline.workingSet;
    d.handleDelta     = (int) latestSample.handles    - (int) baseline.handles;
    d.gdiDelta        = (int) latestSample.gdiObjects - (int) baseline.gdiObjects;
    return d;
}
std::vector<PerfMonitor::HitchSummary> PerfMonitor::hitches()
{
    std::lock_guard<std::mutex> lk (hitchMutex);
    std::vector<HitchSummary> out;
    out.reserve (hitchDeque.size());
    for (int i = 0; i < (int) hitchDeque.size(); ++i)
        out.push_back ({ hitchDeque[(size_t) i].wallTime.toString (true, true),
                         hitchDeque[(size_t) i].durationMs, i });
    return out;
}

juce::StringArray PerfMonitor::symbolizeHitch (int index)
{
    std::vector<void*> frames;
    {
        std::lock_guard<std::mutex> lk (hitchMutex);
        if (index < 0 || index >= (int) hitchDeque.size())
            return {};
        frames = hitchDeque[(size_t) index].frames;
    }

    juce::StringArray out;
   #if JUCE_WINDOWS
    std::lock_guard<std::mutex> lk (dbghelpMutex);
    ensureSymInitialized();
    const HANDLE proc = ::GetCurrentProcess();
    for (void* addr : frames)
    {
        const DWORD64 a = (DWORD64) addr;

        IMAGEHLP_MODULE64 mod {};
        mod.SizeOfStruct = sizeof (mod);
        juce::String modName;
        if (::SymGetModuleInfo64 (proc, a, &mod))
            modName = juce::String (mod.ModuleName);

        alignas (SYMBOL_INFO) char buf[sizeof (SYMBOL_INFO) + 512] = {};
        auto* sym = reinterpret_cast<SYMBOL_INFO*> (buf);
        sym->SizeOfStruct = sizeof (SYMBOL_INFO);
        sym->MaxNameLen = 511;

        DWORD64 disp = 0;
        if (::SymFromAddr (proc, a, &disp, sym))
            out.add ((modName.isNotEmpty() ? modName + "!" : juce::String())
                     + juce::String (sym->Name)
                     + juce::String::formatted ("+0x%llx", (unsigned long long) disp));
        else if (modName.isNotEmpty())
        {
            const DWORD64 base = ::SymGetModuleBase64 (proc, a);
            out.add (modName + juce::String::formatted ("+0x%llx", (unsigned long long) (a - base)));
        }
        else
            out.add (juce::String::formatted ("0x%llx", (unsigned long long) a));
    }
   #else
    juce::ignoreUnused (frames);
   #endif
    return out;
}

void PerfMonitor::clearHitches()
{
    std::lock_guard<std::mutex> lk (hitchMutex);
    hitchDeque.clear();
}
juce::String PerfMonitor::buildReportText()
{
    auto h = history();
    const auto lat = latest();
    auto th = threads();

    auto mb = [] (juce::uint64 bytes) { return juce::String (bytes / (1024.0 * 1024.0), 1) + " MB"; };

    juce::uint64 peakPriv = 0, peakWs = 0;
    double cpuSum = 0.0, cpuMax = 0.0;
    int cpuN = 0;
    juce::int64 handFirst = -1, handLast = 0, gdiFirst = -1, gdiLast = 0;
    for (auto& s : h)
    {
        peakPriv = juce::jmax (peakPriv, s.privateBytes);
        peakWs   = juce::jmax (peakWs, s.workingSet);
        if (s.processCpu > 0.0f || cpuN > 0) { cpuSum += s.processCpu; cpuMax = juce::jmax (cpuMax, (double) s.processCpu); ++cpuN; }
        if (handFirst < 0) handFirst = (juce::int64) s.handles;
        handLast = (juce::int64) s.handles;
        if (gdiFirst < 0) gdiFirst = (juce::int64) s.gdiObjects;
        gdiLast = (juce::int64) s.gdiObjects;
    }

    const double uptime = (juce::Time::getMillisecondCounterHiRes() - startMs) * 0.001;

    juce::String r;
    r << "mikeDAW Performance Report\n";
    r << "Generated: " << juce::Time::getCurrentTime().toString (true, true) << "\n";
    r << "Uptime: " << juce::String (uptime, 1) << " s\n\n";

    r << "[System]\n";
    r << "OS: " << juce::SystemStats::getOperatingSystemName() << "\n";
    r << "CPU: " << juce::SystemStats::getCpuModel() << " (" << juce::SystemStats::getCpuVendor() << ")\n";
    r << "LogicalCores: " << logicalCores << "\n";
    r << "PhysicalRAM_MB: " << juce::SystemStats::getMemorySizeInMegabytes() << "\n\n";

    r << "[Memory]\n";
    r << "PrivateCurrent: " << mb (lat.privateBytes) << "\n";
    r << "PrivatePeak: " << mb (peakPriv) << "\n";
    r << "WorkingSetCurrent: " << mb (lat.workingSet) << "\n";
    r << "WorkingSetPeak: " << mb (peakWs) << "\n";
    r << "LeakSlope_5min_MBperMin: " << juce::String (memorySlopeBytesPerMin (300) / (1024.0 * 1024.0), 3) << "\n\n";

    r << "[Handles]\n";
    r << "HandlesCurrent: " << juce::String ((juce::uint64) lat.handles)
      << " (trend " << (handLast - std::max<juce::int64> (0, handFirst)) << ")\n";
    r << "GdiObjects: " << juce::String ((juce::uint64) lat.gdiObjects)
      << " (trend " << (gdiLast - std::max<juce::int64> (0, gdiFirst)) << ")\n";
    r << "UserObjects: " << juce::String ((juce::uint64) lat.userObjects) << "\n\n";

    r << "[ProcessCPU]\n";
    r << "Current: " << juce::String (lat.processCpu, 1) << " %\n";
    r << "Average: " << juce::String (cpuN > 0 ? cpuSum / cpuN : 0.0, 1) << " %\n";
    r << "Max: " << juce::String (cpuMax, 1) << " %\n\n";

    std::sort (th.begin(), th.end(),
               [] (const ThreadInfo& a, const ThreadInfo& b) { return a.cpuPercent > b.cpuPercent; });
    r << "[TopThreads]\n";
    for (int i = 0; i < (int) th.size() && i < 10; ++i)
        r << juce::String (th[(size_t) i].cpuPercent, 1) << " %\ttid " << (int) th[(size_t) i].tid
          << "\t" << (th[(size_t) i].name.isNotEmpty() ? th[(size_t) i].name : juce::String ("(unnamed)")) << "\n";
    r << "\n";

    auto hs = hitches();
    r << "[Hitches] total=" << totalHitches.load() << " major=" << majorHitches.load()
      << " threshold=" << juce::String (hitchThresholdMs.load(), 0) << "ms\n";
    for (auto& s : hs)
    {
        r << s.wallTime << "\t" << juce::String (s.durationMs, 1) << " ms\n";
        for (auto& line : symbolizeHitch (s.index))
            r << "    " << line << "\n";
    }
    r << "\n";

    return r;
}

juce::File PerfMonitor::writeReport (const juce::String& extraSection)
{
    auto dir = reportDir();
    dir.createDirectory();
    auto f = dir.getChildFile ("perf-" + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S") + ".txt");

    juce::String text = buildReportText();
    if (extraSection.isNotEmpty())
        text << extraSection << "\n";
    f.replaceWithText (text);
    return f;
}
