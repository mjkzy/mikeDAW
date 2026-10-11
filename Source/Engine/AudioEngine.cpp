#include "AudioEngine.h"
#include "PluginHost.h"
#include "../Net/License.h"
#include "BuiltinEffects.h"
#include "BuiltinDelay.h"
#include "BuiltinDelayBank.h"
#include "BuiltinReverb.h"
#include "BuiltinLimiter.h"
#include "BuiltinMultiband.h"
#include "BuiltinSoundbeastifier.h"
#include "BuiltinVoid.h"
#include "BuiltinLowLifter.h"
#include "BuiltinParamEQ.h"
#include "BuiltinGrossBeat.h"
#include "BuiltinConvolver.h"
#include "BuiltinParterre.h"
#include "BuiltinEquivocal.h"
#include "../Net/SyncDebug.h"
#include "../Debug/DebugFlags.h"
#include "../UI/LoadProgress.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>
#if JUCE_WINDOWS
 #include <objbase.h>
#endif
#if defined (_M_X64) || defined (__x86_64__)
 #include <emmintrin.h>
 #define OPENDAW_SILENCE_SSE2 1
#endif

namespace
{
    constexpr float silenceThreshold = 1.0e-5f;

    bool isPatternClip (const juce::ValueTree& clip) noexcept
    {
        const auto t = clip.getProperty (IDs::type).toString();
        return t != "audio" && t != "automation";
    }

    constexpr int legacyClipUidBase = 0x40000000;
    int legacyClipUid (int playlistIndex) noexcept { return legacyClipUidBase + playlistIndex; }

    float faderGain (float pos) noexcept
    {
        static const float norm = 1.0f / (std::exp (2.4f) - 1.0f);
        return (std::exp (2.4f * juce::jmax (0.0f, pos)) - 1.0f) * norm;
    }

    float channelGain (float pos) noexcept
    {
        return juce::jmax (0.0f, pos);
    }

    void panGains (float pan, float& gl, float& gr) noexcept
    {
        const float p = juce::jlimit (-1.0f, 1.0f, pan);
        gl = p <= 0.0f ? 1.0f : 1.0f - p;
        gr = p >= 0.0f ? 1.0f : 1.0f + p;
    }

    bool spanIsSilent (const float* p, int n) noexcept
    {
       #if OPENDAW_SILENCE_SSE2
        const __m128 thr  = _mm_set1_ps (silenceThreshold);
        const __m128 absk = _mm_castsi128_ps (_mm_set1_epi32 (0x7fffffff));
        int i = 0;
        for (; i + 16 <= n; i += 16)
        {
            const __m128 a = _mm_and_ps (_mm_loadu_ps (p + i),      absk);
            const __m128 b = _mm_and_ps (_mm_loadu_ps (p + i + 4),  absk);
            const __m128 c = _mm_and_ps (_mm_loadu_ps (p + i + 8),  absk);
            const __m128 d = _mm_and_ps (_mm_loadu_ps (p + i + 12), absk);
            const __m128 m = _mm_max_ps (_mm_max_ps (a, b), _mm_max_ps (c, d));
            if (_mm_movemask_ps (_mm_cmplt_ps (m, thr)) != 0xf)
                return false;
        }
        for (; i + 4 <= n; i += 4)
        {
            const __m128 v = _mm_and_ps (_mm_loadu_ps (p + i), absk);
            if (_mm_movemask_ps (_mm_cmplt_ps (v, thr)) != 0xf)
                return false;
        }
        for (; i < n; ++i)
            if (! (std::abs (p[i]) < silenceThreshold))
                return false;
        return true;
       #else
        for (int i = 0; i < n; ++i)
            if (! (std::abs (p[i]) < silenceThreshold))
                return false;
        return true;
       #endif
    }

    bool bufferIsSilent (const juce::AudioBuffer<float>& b, int n) noexcept
    {
        const int numCh = juce::jmin (2, b.getNumChannels());
        for (int c = 0; c < numCh; ++c)
            if (! spanIsSilent (b.getReadPointer (c), n))
                return false;
        return true;
    }

    juce::MemoryBlock blobFromBase64 (const juce::String& s)
    {
        juce::MemoryOutputStream mo;
        juce::Base64::convertFromBase64 (mo, s);
        return mo.getMemoryBlock();
    }

    juce::int64 hashStateBlob (const juce::MemoryBlock& mb)
    {
        juce::uint64 h = 1469598103934665603ull;
        auto* p = static_cast<const juce::uint8*> (mb.getData());
        for (size_t i = 0; i < mb.getSize(); ++i)
        {
            h ^= p[i];
            h *= 1099511628211ull;
        }
        return (juce::int64) h;
    }
}
struct AudioEngine::RecordJob
{
    RecordJob (int channels, int fifoFrames)
        : numChannels (channels), fifo (fifoFrames)
    {
        fifoBuffer.setSize (channels, fifoFrames);
        fifoBuffer.clear();
    }

    int mixerTrack = 0;
    int numChannels = 1;
    int inputL = -1, inputR = -1;
    double sampleRate = 44100.0;

    std::atomic<bool> active { true };
    std::atomic<bool> capturing { false };
    std::atomic<bool> overflow { false };
    std::atomic<double> startTicks { 0.0 };
    std::atomic<juce::int64> framesWritten { 0 };

    juce::AbstractFifo fifo;
    juce::AudioBuffer<float> fifoBuffer;

    juce::File file;
    std::unique_ptr<juce::AudioFormatWriter> writer;

    static constexpr int peakWindow = 512;
    mutable juce::CriticalSection peakLock;
    std::vector<float> peaks;
    int peakSamples = 0;
    float curMin = 0.0f, curMax = 0.0f;
};

struct AudioEngine::FinishedRecording
{
    int mixerTrack = 0;
    juce::File file;
    double startTicks = 0.0, lengthSeconds = 0.0, latencySeconds = 0.0;
};

struct AudioEngine::RenderPool
{
    explicit RenderPool (AudioEngine& e) : engine (e) {}

    ~RenderPool() { shutdown(); }

    void start (int numWorkers)
    {
        shutdown();
        for (int i = 0; i < numWorkers; ++i)
        {
            auto w = std::make_unique<Worker> (*this, i);
            w->startThread (juce::Thread::Priority::highest);
            workers.push_back (std::move (w));
        }
    }

    void shutdown()
    {
        for (auto& w : workers)
        {
            w->signalThreadShouldExit();
            w->start.signal();
        }
        for (auto& w : workers)
            w->stopThread (2000);
        workers.clear();
    }

    bool hasWorkers() const noexcept { return ! workers.empty(); }

    void run (int total)
    {
        count.store (total, std::memory_order_relaxed);
        nextIndex.store (0, std::memory_order_relaxed);
        outstanding.store (total, std::memory_order_release);

        for (auto& w : workers)
            w->start.signal();

        drain();

        int spins = 0;
        while (outstanding.load (std::memory_order_acquire) > 0)
        {
            if (++spins < 4000)
            {
               #if OPENDAW_SILENCE_SSE2
                _mm_pause();
               #endif
            }
            else
                juce::Thread::yield();
        }
    }

    void drain() noexcept
    {
        const int total = count.load (std::memory_order_relaxed);
        for (;;)
        {
            const int i = nextIndex.fetch_add (1, std::memory_order_acq_rel);
            if (i >= total)
                return;
            engine.renderChannelBody (*engine.renderList[(size_t) i], engine.renderNumSamples);
            outstanding.fetch_sub (1, std::memory_order_release);
        }
    }

private:
    struct Worker : juce::Thread
    {
        Worker (RenderPool& p, int index)
            : juce::Thread ("mikeDAW render " + juce::String (index)), pool (p) {}

        void run() override
        {
            while (! threadShouldExit())
            {
                start.wait (-1);
                if (threadShouldExit())
                    return;
                pool.drain();
            }
        }

        RenderPool& pool;
        juce::WaitableEvent start { false };
    };

    AudioEngine& engine;
    std::vector<std::unique_ptr<Worker>> workers;
    std::atomic<int> nextIndex { 0 }, outstanding { 0 }, count { 0 };
};

struct AudioEngine::RecordWriter : public juce::Thread
{
    explicit RecordWriter (AudioEngine& e) : juce::Thread ("mikeDAW Record Writer"), engine (e) {}
    void run() override
    {
        while (! threadShouldExit())
        {
            engine.serviceRecordJobs();
            wait (6);
        }
        engine.serviceRecordJobs();
    }
    AudioEngine& engine;
};

AudioEngine::AudioEngine (juce::ValueTree projectTree, PluginHost& pluginHost)
    : project (std::move (projectTree)), host (pluginHost)
{
    audioFormats.registerBasicFormats();
    for (int i = 0; i < stretchGrainLength; ++i)
        stretchHann[(size_t) i] = 0.5f - 0.5f * std::cos (
            juce::MathConstants<float>::twoPi * (float) i / (float) stretchGrainLength);

    for (auto& t : mixerTracks)
        t = std::make_unique<EngineMixerTrack>();

    const int numLoaders = juce::jlimit (2, 4, (int) std::thread::hardware_concurrency() / 2);
    for (int i = 0; i < numLoaders; ++i)
    {
        loaderThreads.push_back (std::make_unique<PluginLoaderThread> (*this));
        loaderThreads.back()->startThread();
    }

    prewarmThread = std::make_unique<FilePrewarmThread> (*this);
    prewarmThread->startThread (juce::Thread::Priority::low);

    audioClipVoices.reserve (maxAudioClipVoices);
    pendingAuditions.reserve (64);
    renderList.reserve (512);

    graveyardDrainer.startTimer (500);

    renderPool = std::make_unique<RenderPool> (*this);
    renderPool->start (juce::jlimit (0, 7, juce::SystemStats::getNumCpus() - 2));

    deviceManager.initialiseWithDefaultDevices (2, 2);

    recordingFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("mikeDAW Recordings");
    deviceManager.addAudioCallback (this);

    project.addListener (this);
    bpm.store ((double) project.getProperty (IDs::bpm, 140.0));

    auto patterns = Model::getPatterns (project);
    if (patterns.getNumChildren() > 0)
        currentPatternId.store ((int) patterns.getChild (0).getProperty (IDs::id));

    rebuildChannels();
    rebuildMixer();
    recompileSequences();
}

AudioEngine::~AudioEngine()
{
    loaderShutdown.store (true);
    for (auto& t : loaderThreads)
        if (t != nullptr)
            t->signalThreadShouldExit();
    for (auto& t : loaderThreads)
    {
        if (t == nullptr)
            continue;
        loaderWake.signal();
        t->stopThread (5000);
    }
    loaderThreads.clear();

    prewarmShutdown.store (true);
    if (prewarmThread != nullptr)
    {
        prewarmThread->signalThreadShouldExit();
        prewarmWake.signal();
        prewarmThread->stopThread (5000);
        prewarmThread.reset();
    }
    {
        const std::lock_guard<std::mutex> lk (prewarmMutex);
        prewarmQueue.clear();
    }
    {
        const std::lock_guard<std::mutex> lk (loaderMutex);
        loadQueue.clear();
        completedLoads.clear();
    }
    messageThreadQueue.clear();
    LoadProgress::shutdown();

    finalizeAllRecordings();
    if (recordWriter != nullptr)
        recordWriter->stopThread (3000);
    recordWriter.reset();

    project.removeListener (this);
    deviceManager.removeAudioCallback (this);
    deviceManager.closeAudioDevice();

    if (renderPool != nullptr)
        renderPool->shutdown();

    for (auto& ch : channels)
        if (ch->plugin && ch->paramListener)
            ch->plugin->removeListener (ch->paramListener.get());

    for (auto& t : mixerTracks)
        for (auto& fx : t->effects)
            if (fx->processor && fx->paramListener)
                fx->processor->removeListener (fx->paramListener.get());
}

void AudioEngine::play()
{
    if (offlineRendering.load())
        return;
    stopPreview();
    if (playing.exchange (true))
        return;
    playAnchorTicks.store (positionForUi.load());
    resyncAudioClips.store (true);
    maybeStartRecording();
    if (! suppressTransportBroadcast.load() && onTransportBroadcast)
        onTransportBroadcast (1, positionForUi.load());
}

void AudioEngine::stop()
{
    if (offlineRendering.load())
        return;
    double target = playing.exchange (false) ? playAnchorTicks.load() : 0.0;
    playAnchorTicks.store (target);
    stopAt (target);
}

void AudioEngine::stopAt (double ticks)
{
    if (offlineRendering.load())
        return;
    stopPreview();
    playing.store (false);
    finalizeAllRecordings();
    stopReturnTicks.store (ticks);
    stopRequest.store (true);
    seekRequest.store (-1.0);
    positionForUi.store (ticks);
    if (! suppressTransportBroadcast.load() && onTransportBroadcast)
        onTransportBroadcast (0, ticks);
}

void AudioEngine::pause()
{
    if (offlineRendering.load())
        return;
    stopPreview();
    playing.store (false);
    finalizeAllRecordings();
    pauseRequest.store (true);
    if (! suppressTransportBroadcast.load() && onTransportBroadcast)
        onTransportBroadcast (3, positionForUi.load());
}

void AudioEngine::togglePlay()
{
    if (isPlaying()) pause();
    else             play();
}

void AudioEngine::seekTicks (double ticks, bool fireNotesAtTarget)
{
    if (offlineRendering.load())
        return;
    seekFiresNotes.store (fireNotesAtTarget);
    seekRequest.store (juce::jmax (0.0, ticks));
    positionForUi.store (juce::jmax (0.0, ticks));
    if (! suppressTransportBroadcast.load() && onTransportBroadcast)
        onTransportBroadcast (2, ticks);
}

void AudioEngine::setBpm (double newBpm)
{
    project.setProperty (IDs::bpm, juce::jlimit (20.0, 999.0, std::round (newBpm)), nullptr);
}

void AudioEngine::setPlaybackVolume (double v)
{
    playbackVolume.store (juce::jlimit (0.0, 2.0, v));
}

void AudioEngine::setSongMode (bool s)
{
    songMode.store (s);
    resyncAudioClips.store (true);
    if (playing.load())
        modeSwitchCut.store (true);
}

void AudioEngine::setCurrentPatternId (int patternId)
{
    if (currentPatternId.exchange (patternId) != patternId)
    {
        markDirty (false, false, true);
        if (playing.load() && ! songMode.load())
            patternSwitchCut.store (true);
    }
}

void AudioEngine::previewFile (const juce::File& file, double normalizedStart)
{
    const int gen = ++previewGeneration;
    previewPool.addJob ([this, file, gen, normalizedStart]
    {
        auto data = loadSample (file.getFullPathName());
        if (data == nullptr || gen != previewGeneration.load())
            return;
        const juce::ScopedLock sl (engineLock);
        if (currentSampleRate <= 0)
            return;
        if (previewVoice.active)
        {
            previewTailVoice = previewVoice;
            if (previewTailVoice.releaseRemaining < 0)
            {
                int rel = (int) (0.003 * currentSampleRate);
                if (rel <= 0)
                    previewTailVoice.active = false;
                else
                {
                    previewTailVoice.releaseTotal = rel;
                    previewTailVoice.releaseRemaining = rel;
                }
            }
        }
        previewVoice = {};
        previewVoice.sample = data;
        previewVoice.pos = juce::jlimit (0.0, 1.0, normalizedStart)
                         * (double) juce::jmax (0, data->buffer.getNumSamples() - 1);
        previewVoice.fadeInTotal = (int) (0.0025 * currentSampleRate);
        previewVoice.ratio = data->sourceSampleRate / currentSampleRate;
        previewVoice.gain = channelGain (0.8f);
        previewVoice.active = true;
    });
}

void AudioEngine::stopPreview()
{
    ++previewGeneration;
    const juce::ScopedLock sl (engineLock);
    if (previewVoice.active && previewVoice.releaseRemaining < 0)
    {
        int rel = (int) (0.010 * currentSampleRate);
        if (rel <= 0)
            previewVoice.active = false;
        else
        {
            previewVoice.releaseTotal = rel;
            previewVoice.releaseRemaining = rel;
        }
    }
}

void AudioEngine::prefetchFiles (const juce::Array<juce::File>& files)
{
    int queued = 0;
    for (auto& f : files)
    {
        if (f.getSize() > 2 * 1024 * 1024)
            continue;
        prefetchPool.addJob ([this, f] { loadSample (f.getFullPathName()); });
        if (++queued >= 150)
            break;
    }
}

void AudioEngine::auditionNote (int channelId, int key, int velocity)
{
    {
        const juce::ScopedLock sl (engineLock);
        pendingAuditions.push_back ({ channelId, key, velocity, true });
    }
    if (onAudition && ! suppressAuditionBroadcast.load())
        onAudition (channelId, true, key, velocity / 127.0f);
}

void AudioEngine::auditionNoteOff (int channelId, int key)
{
    {
        const juce::ScopedLock sl (engineLock);
        pendingAuditions.push_back ({ channelId, key, 0, false });
    }
    if (onAudition && ! suppressAuditionBroadcast.load())
        onAudition (channelId, false, key, 0.0f);
}

AudioEngine::EngineChannel* AudioEngine::findChannel (int channelId) const
{
    for (auto& c : channels)
        if (c->id == channelId)
            return c.get();
    return nullptr;
}

bool AudioEngine::isChannelPlaceholder (int channelId) const
{
    auto* c = findChannel (channelId);
    return c != nullptr && c->placeholder;
}

bool AudioEngine::isPluginLoading (const juce::String& key) const
{
    if (key.startsWith ("c"))
    {
        if (auto* c = findChannel (key.substring (1).getIntValue()))
            return c->loading;
        return false;
    }
    if (auto* fx = findEffect (key))
        return fx->loading;
    return false;
}

AudioEngine::EngineEffect* AudioEngine::findEffect (const juce::String& key, EngineMixerTrack** trackOut) const
{
    for (auto& t : mixerTracks)
        for (auto& fx : t->effects)
            if (fx->key == key)
            {
                if (trackOut) *trackOut = t.get();
                return fx.get();
            }
    return nullptr;
}

juce::AudioPluginInstance* AudioEngine::getPluginFor (const juce::String& key)
{
    if (key.startsWith ("c"))
    {
        if (auto* c = findChannel (key.substring (1).getIntValue()))
            return dynamic_cast<juce::AudioPluginInstance*> (c->plugin.get());
        return nullptr;
    }
    if (auto* fx = findEffect (key))
        return dynamic_cast<juce::AudioPluginInstance*> (fx->processor.get());
    return nullptr;
}

juce::AudioProcessor* AudioEngine::getProcessorFor (const juce::String& key)
{
    if (key.startsWith ("c"))
    {
        if (auto* c = findChannel (key.substring (1).getIntValue()))
            return c->plugin.get();
        return nullptr;
    }
    if (auto* fx = findEffect (key))
        return fx->processor.get();
    return nullptr;
}

juce::String AudioEngine::getPluginNameFor (const juce::String& key)
{
    if (key.startsWith ("c"))
    {
        if (auto* c = findChannel (key.substring (1).getIntValue()))
            if (c->plugin)
                return c->plugin->getName();
    }
    else if (auto* fx = findEffect (key))
    {
        if (fx->processor)
            return fx->processor->getName();
    }
    return "Plugin";
}

void AudioEngine::setRemoteParam (const juce::String& key, int paramIndex, float value)
{
    const juce::ScopedLock lifetime (pluginLifetimeLock);

    juce::AudioProcessor* proc = nullptr;
    {
        const juce::ScopedLock sl (engineLock);
        if (key.startsWith ("c"))
        {
            if (auto* c = findChannel (key.substring (1).getIntValue()))
                proc = c->plugin.get();
        }
        else if (auto* fx = findEffect (key))
            proc = fx->processor.get();
    }

    if (proc == nullptr)
        return;

    auto& params = proc->getParameters();
    if (! juce::isPositiveAndBelow (paramIndex, params.size()))
        return;

    const ScopedAtomicFlag suppress (suppressParamBroadcast);
    const juce::ScopedLock sl (proc->getCallbackLock());
    params[paramIndex]->setValueNotifyingHost (value);
}

void AudioEngine::captureStateInto (juce::ValueTree& dest, const juce::String& key,
                                    juce::AudioProcessor& proc)
{
    juce::MemoryBlock mb;
    {
        const juce::ScopedLock sl (proc.getCallbackLock());
        proc.getStateInformation (mb);
    }

    const auto h = hashStateBlob (mb);
    auto it = lastCapturedStateHash.find (key);
    if (it != lastCapturedStateHash.end() && it->second == h && dest.hasProperty (IDs::pluginState))
        return;

    dest.setProperty (IDs::pluginState, juce::Base64::toBase64 (mb.getData(), mb.getSize()), nullptr);
    lastCapturedStateHash[key] = h;
}

void AudioEngine::capturePluginStatesToTree()
{
    auto chans = Model::getChannels (project);
    for (auto& c : channels)
        if (c->plugin)
        {
            auto tree = chans.getChildWithProperty (IDs::id, c->id);
            if (tree.isValid())
                captureStateInto (tree, "c" + juce::String (c->id), *c->plugin);
        }

    auto mixer = Model::getMixer (project);
    for (int ti = 0; ti < (int) mixerTracks.size(); ++ti)
    {
        auto trackTree = mixer.getChildWithProperty (IDs::index, ti);
        if (! trackTree.isValid())
            continue;
        for (auto& fx : mixerTracks[(size_t) ti]->effects)
        {
            if (fx->processor == nullptr)
                continue;
            auto slot = trackTree.getChildWithProperty (IDs::uid, fx->uid);
            if (slot.isValid())
                captureStateInto (slot, fx->key, *fx->processor);
        }
    }
}

juce::MemoryBlock AudioEngine::capturePluginState (const juce::String& key)
{
    juce::MemoryBlock mb;
    juce::AudioProcessor* proc = nullptr;
    if (key.startsWith ("c"))
    {
        if (auto* c = findChannel (key.substring (1).getIntValue()))
            proc = c->plugin.get();
    }
    else if (auto* fx = findEffect (key))
        proc = fx->processor.get();

    if (proc != nullptr)
    {
        const juce::ScopedLock sl (proc->getCallbackLock());
        proc->getStateInformation (mb);
    }
    return mb;
}

juce::StringArray AudioEngine::getAllPluginKeys() const
{
    juce::StringArray keys;
    for (auto& c : channels)
        if (c->plugin)
            keys.add ("c" + juce::String (c->id));
    for (auto& t : mixerTracks)
        for (auto& fx : t->effects)
            if (fx->isPlugin && fx->processor)
                keys.add (fx->key);
    return keys;
}

std::vector<AudioEngine::PluginPerfInfo> AudioEngine::getPluginPerfStats() const
{
    std::vector<PluginPerfInfo> result;
    for (const auto& channel : channels)
        if (channel->isPlugin)
            result.push_back ({
                "c" + juce::String (channel->id),
                channel->perf.load.load (std::memory_order_relaxed),
                channel->perf.peak.load (std::memory_order_relaxed)
            });
    for (const auto& track : mixerTracks)
        for (const auto& effect : track->effects)
            if (effect->isPlugin)
                result.push_back ({
                    effect->key,
                    effect->perf.load.load (std::memory_order_relaxed),
                    effect->perf.peak.load (std::memory_order_relaxed)
                });
    return result;
}

void AudioEngine::applyPluginStateBlob (const juce::String& key, const void* data, int size)
{
    const juce::ScopedLock lifetime (pluginLifetimeLock);

    juce::AudioProcessor* proc = nullptr;
    {
        const juce::ScopedLock sl (engineLock);
        if (key.startsWith ("c"))
        {
            if (auto* c = findChannel (key.substring (1).getIntValue()))
                proc = c->plugin.get();
        }
        else if (auto* fx = findEffect (key))
            proc = fx->processor.get();
    }

    if (proc == nullptr || size <= 0)
        return;

    const ScopedAtomicFlag suppress (suppressParamBroadcast);
    const juce::ScopedLock sl (proc->getCallbackLock());
    proc->setStateInformation (data, size);
}

float AudioEngine::getTrackPeak (int trackIndex) const
{
    if (! juce::isPositiveAndBelow (trackIndex, (int) mixerTracks.size()))
        return 0.0f;
    return mixerTracks[(size_t) trackIndex]->peak.load();
}

void AudioEngine::pushNoteOnActivity (int channelId) noexcept
{
    int start1, size1, start2, size2;
    noteOnFifo.prepareToWrite (1, start1, size1, start2, size2);
    if (size1 > 0)
        noteOnChannelIds[(size_t) start1] = channelId;
    noteOnFifo.finishedWrite (size1);
}

int AudioEngine::drainNoteOnChannelIds (int* dest, int maxCount) noexcept
{
    int total = 0;
    int start1, size1, start2, size2;
    noteOnFifo.prepareToRead (juce::jmin (maxCount, noteOnFifo.getNumReady()), start1, size1, start2, size2);
    for (int i = 0; i < size1; ++i) dest[total++] = noteOnChannelIds[(size_t) (start1 + i)];
    for (int i = 0; i < size2; ++i) dest[total++] = noteOnChannelIds[(size_t) (start2 + i)];
    noteOnFifo.finishedRead (size1 + size2);
    return total;
}
bool AudioEngine::renderToFile (juce::AudioFormatWriter& writer, double tailSeconds,
                                std::function<void (float)> onProgress)
{
    recompileSequences();

    const bool licDegrade = License::shouldDegrade();

    int blockSize = 0;
    double tps = 0.0;
    juce::int64 lenTicks = 0, bodySamples = 0, minTail = 0, maxTail = 0;

    bool wasPlayingSaved = false;
    double savedPos = 0.0, savedSeekedFrom = 0.0, savedPosForUi = 0.0;
    juce::int64 savedSamplePos = 0;
    bool wasMetronomeOn = false;
    std::vector<juce::AudioProcessor*> hostedProcessors;

    {
        const juce::ScopedLock fl (stopFlushLock);
        const juce::ScopedLock sl (engineLock);
        if (currentSampleRate <= 0)
            return false;

        blockSize = juce::jmax (64, currentBlockSize);
        tps = ticksPerSample (currentSampleRate);

        lenTicks = songMode.load() ? songLengthTicks : patternLengthTicks;
        if (lenTicks <= 0)
            lenTicks = Model::PPQ * 4;

        bodySamples = (juce::int64) std::ceil (lenTicks / tps);
        minTail = (juce::int64) std::ceil (tailSeconds * currentSampleRate);
        maxTail = (juce::int64) std::ceil (30.0 * currentSampleRate);
        if (bodySamples <= 0)
            return false;

        offlineRendering.store (true);

        wasPlayingSaved = playing.load();
        savedPos        = positionTicks;
        savedSamplePos  = samplePosition;
        savedSeekedFrom = seekedFromTick;
        savedPosForUi   = positionForUi.load();

        if (stopRequest.exchange (false))
            savedPos = stopReturnTicks.load();
        pauseRequest.store (false);
        stopFlushPending.store (false);
        stopFlusher.cancelPendingUpdate();

        wasMetronomeOn = metronomeOn.exchange (false);

        for (auto& v : voices) v.active = false;
        audioClipVoices.clear();
        for (auto& c : channels)
            if (c->isPlugin && c->plugin != nullptr)
            {
                const juce::ScopedLock pl (c->plugin->getCallbackLock());
                c->plugin->reset();
                hostedProcessors.push_back (c->plugin.get());
            }
        for (auto& t : mixerTracks)
            for (auto& fx : t->effects)
                if (fx->processor != nullptr)
                {
                    const juce::ScopedLock pl (fx->processor->getCallbackLock());
                    fx->processor->reset();
                    fx->asleep = false;
                    fx->silentBlocks = 0;
                    hostedProcessors.push_back (fx->processor.get());
                }

        positionTicks = 0;
        seekedFromTick = -1.0;
        samplePosition = 0;
        positionForUi.store (0.0);
        playing.store (true);

        if (songMode.load())
            startAudioClipsAtPosition (0.0, tps, 0);
    }

    for (auto* p : hostedProcessors)
    {
        {
            const juce::ScopedLock pl (p->getCallbackLock());
            p->setNonRealtime (true);
        }
        preparePlugin (*p, false);
    }

    juce::AudioBuffer<float> block (2, blockSize);
    bool ok = true;

    juce::int64 tailDone = 0;
    for (juce::int64 done = 0; ok;)
    {
        const bool inTail = done >= bodySamples;

        if (inTail && tailDone >= maxTail)
            break;

        int n = blockSize;
        if (! inTail)
            n = (int) juce::jmin ((juce::int64) blockSize, bodySamples - done);
        if (n <= 0)
            n = blockSize;

        {
            const juce::ScopedLock sl (engineLock);

            for (auto& t : mixerTracks)
            {
                if (t->buffer.getNumSamples() < n)
                    t->buffer.setSize (2, n, false, false, true);
                t->buffer.clear (0, 0, n);
                t->buffer.clear (1, 0, n);
            }
            for (auto& c : channels)
                c->midi.clear();

            if (! inTail)
            {
                double startTick = positionTicks;
                double endTick   = startTick + n * tps;
                if (endTick < (double) lenTicks)
                {
                    processSequencerRange (startTick, endTick, 0, tps);
                    positionTicks = endTick;
                }
                else
                {
                    processSequencerRange (startTick, (double) lenTicks, 0, tps);
                    int splitSamples = juce::jlimit (0, n, (int) (((double) lenTicks - startTick) / tps));
                    double remainder = endTick - (double) lenTicks;

                    for (auto& c : channels)
                        if (c->isPlugin && c->plugin != nullptr)
                            c->midi.addEvent (juce::MidiMessage::allNotesOff (1), splitSamples);

                    positionTicks = (double) lenTicks;
                    juce::ignoreUnused (remainder);
                }
            }

            positionForUi.store (positionTicks);

            applyAutomation (positionTicks);

            renderChannels (n);
            renderVoices (n);

            block.clear();
            float* outs[2] = { block.getWritePointer (0), block.getWritePointer (1) };
            processMixer (outs, 2, n);

            samplePosition += n;
        }

        if (licDegrade)
        {
            auto& rng = juce::Random::getSystemRandom();
            for (int ch = 0; ch < block.getNumChannels(); ++ch)
            {
                auto* d = block.getWritePointer (ch);
                for (int i = 0; i < n; ++i)
                    d[i] = (rng.nextFloat() - 0.5f) * 0.06f;
            }
        }

        ok = writer.writeFromAudioSampleBuffer (block, 0, n);

        done += n;

        if (inTail)
        {
            tailDone += n;
            float mag = juce::jmax (block.getMagnitude (0, 0, n), block.getMagnitude (1, 0, n));
            if (tailDone >= minTail && mag < 1.0e-5f)
                break;
        }

        if (onProgress)
        {
            float p = inTail ? 1.0f : (float) done / (float) bodySamples;
            onProgress (juce::jlimit (0.0f, 1.0f, p));
        }
    }

    std::vector<juce::AudioProcessor*> restored;
    {
        const juce::ScopedLock sl (engineLock);
        for (auto& v : voices) v.active = false;
        audioClipVoices.clear();
        for (auto& c : channels)
            if (c->isPlugin && c->plugin != nullptr)
            {
                const juce::ScopedLock pl (c->plugin->getCallbackLock());
                c->plugin->reset();
                restored.push_back (c->plugin.get());
            }
        for (auto& t : mixerTracks)
            for (auto& fx : t->effects)
                if (fx->processor != nullptr)
                    restored.push_back (fx->processor.get());
        positionTicks = savedPos;
        samplePosition = savedSamplePos;
        seekedFromTick = savedSeekedFrom;
        positionForUi.store (savedPosForUi);
        playing.store (wasPlayingSaved);
        metronomeOn.store (wasMetronomeOn);
        resyncAudioClips.store (true);
    }

    for (auto* p : restored)
    {
        {
            const juce::ScopedLock pl (p->getCallbackLock());
            p->setNonRealtime (false);
        }
        preparePlugin (*p, false);
    }
    offlineRendering.store (false);

    return ok;
}

AudioEngine::SampleDataPtr AudioEngine::synthesizeBuiltin (const juce::String& which)
{
    const double sr = 44100.0;
    juce::Random rnd (0x5eed + which.hashCode());
    auto data = std::make_shared<SampleData>();
    data->sourceSampleRate = sr;

    auto make = [&] (double seconds) -> juce::AudioBuffer<float>&
    {
        data->buffer.setSize (2, (int) (sr * seconds));
        data->buffer.clear();
        return data->buffer;
    };

    if (which == "kick")
    {
        auto& b = make (0.35);
        double phase = 0;
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            double t = i / sr;
            double freq = 40.0 + 110.0 * std::exp (-t * 28.0);
            phase += freq * juce::MathConstants<double>::twoPi / sr;
            float env = (float) std::exp (-t * 9.0);
            float click = (float) std::exp (-t * 300.0) * 0.6f;
            float s = (float) std::sin (phase) * env + click * (rnd.nextFloat() * 2 - 1);
            b.setSample (0, i, s * 0.9f);
            b.setSample (1, i, s * 0.9f);
        }
    }
    else if (which == "snare")
    {
        auto& b = make (0.25);
        double phase = 0;
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            double t = i / sr;
            phase += 190.0 * juce::MathConstants<double>::twoPi / sr;
            float tone = (float) (std::sin (phase) * std::exp (-t * 25.0)) * 0.5f;
            float noise = (rnd.nextFloat() * 2 - 1) * (float) std::exp (-t * 16.0) * 0.7f;
            float s = tone + noise;
            b.setSample (0, i, s * 0.8f);
            b.setSample (1, i, s * 0.8f);
        }
    }
    else if (which == "clap")
    {
        auto& b = make (0.3);
        float lp = 0;
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            double t = i / sr;
            double burst = std::fmod (t, 0.011);
            float env = t < 0.033 ? (float) std::exp (-burst * 220.0)
                                  : (float) std::exp (-(t - 0.033) * 22.0);
            float n = (rnd.nextFloat() * 2 - 1);
            lp += 0.25f * (n - lp);
            float s = (n - lp) * env;
            b.setSample (0, i, s * 0.9f);
            b.setSample (1, i, s * 0.9f);
        }
    }
    else
    {
        auto& b = make (0.09);
        float hp = 0, prev = 0;
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            double t = i / sr;
            float n = (rnd.nextFloat() * 2 - 1);
            hp = n - prev + 0.6f * hp;
            prev = n;
            float s = hp * (float) std::exp (-t * 60.0);
            b.setSample (0, i, s * 0.55f);
            b.setSample (1, i, s * 0.55f);
        }
    }

    return data;
}

bool AudioEngine::renderBuiltinSampleToWav (const juce::String& which, const juce::File& dest)
{
    auto data = synthesizeBuiltin (which);
    if (data == nullptr || data->buffer.getNumSamples() == 0)
        return false;

    dest.deleteFile();
    auto out = dest.createOutputStream();
    if (out == nullptr)
        return false;

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wav.createWriterFor (out.get(), data->sourceSampleRate,
                             (unsigned int) data->buffer.getNumChannels(), 16, {}, 0));
    if (writer == nullptr)
        return false;

    out.release();
    return writer->writeFromAudioSampleBuffer (data->buffer, 0, data->buffer.getNumSamples());
}

void AudioEngine::setSamplePathResolver (std::function<juce::File (const juce::String&)> resolver)
{
    const juce::ScopedLock sl (sampleResolverLock);
    samplePathResolver = std::move (resolver);
}

juce::File AudioEngine::resolveSampleFile (const juce::String& path)
{
    auto f = juce::File (path);
    if (f.existsAsFile())
        return f;

    std::function<juce::File (const juce::String&)> resolver;
    {
        const juce::ScopedLock sl (sampleResolverLock);
        resolver = samplePathResolver;
    }

    if (resolver != nullptr)
    {
        auto resolved = resolver (path);
        if (resolved.existsAsFile())
            return resolved;
    }

    return f;
}

void AudioEngine::invalidateSampleCache (const juce::String& pathOrBuiltin)
{
    {
        const juce::ScopedLock sl (sampleCacheLock);
        if (pathOrBuiltin.isEmpty())
        {
            sampleCache.clear();
            sampleLru.clear();
            sampleCacheBytes = 0;
        }
        else
        {
            auto it = sampleCache.find (pathOrBuiltin);
            if (it != sampleCache.end())
            {
                sampleCacheBytes -= it->second.bytes;
                sampleCache.erase (it);
            }
            sampleLru.remove (pathOrBuiltin);
        }
    }

    {
        const juce::ScopedLock sl (processedCacheLock);
        if (pathOrBuiltin.isEmpty())
        {
            processedCache.clear();
            processedLru.clear();
            processedCacheBytes = 0;
        }
        else
        {
            const auto prefix = pathOrBuiltin + "|";
            for (auto it = processedCache.begin(); it != processedCache.end();)
            {
                if (it->first.startsWith (prefix))
                {
                    processedCacheBytes -= it->second.bytes;
                    processedLru.remove (it->first);
                    it = processedCache.erase (it);
                }
                else
                    ++it;
            }
        }
    }
}

double AudioEngine::getProcessedSampleSeconds (const juce::String& pathOrBuiltin,
                                               const juce::ValueTree& settingsSource)
{
    if (pathOrBuiltin.isEmpty())
        return 0.0;

    auto sample = loadProcessedSample (pathOrBuiltin, SampleFxSettings::fromTree (settingsSource));
    if (sample == nullptr || sample->sourceSampleRate <= 0.0)
        return 0.0;

    return (double) juce::jmax (0, sample->buffer.getNumSamples() - 1)
         / sample->sourceSampleRate;
}

bool AudioEngine::saveProcessedSampleToWav (const juce::String& pathOrBuiltin,
                                            const juce::ValueTree& settingsSource,
                                            const juce::File& destination,
                                            int bitDepth)
{
    auto sample = loadProcessedSample (pathOrBuiltin, SampleFxSettings::fromTree (settingsSource));
    if (sample == nullptr || sample->buffer.getNumSamples() <= 0 || destination == juce::File())
        return false;

    if (! pathOrBuiltin.startsWith ("builtin:")
        && destination == resolveSampleFile (pathOrBuiltin))
        return false;

    const auto temporary = destination.getSiblingFile (
        "." + destination.getFileNameWithoutExtension() + "-export.wav")
                                     .getNonexistentSibling (false);
    auto stream = temporary.createOutputStream();
    if (stream == nullptr)
        return false;

    bitDepth = bitDepth <= 16 ? 16 : bitDepth <= 24 ? 24 : 32;
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wav.createWriterFor (stream.get(), sample->sourceSampleRate,
                             (unsigned int) sample->buffer.getNumChannels(),
                             bitDepth, {}, 0));
    if (writer == nullptr)
    {
        stream.reset();
        temporary.deleteFile();
        return false;
    }

    stream.release();
    const bool wrote = writer->writeFromAudioSampleBuffer (
        sample->buffer, 0, sample->buffer.getNumSamples());
    writer.reset();
    if (! wrote || ! temporary.replaceFileIn (destination))
    {
        temporary.deleteFile();
        return false;
    }

    invalidateSampleCache (destination.getFullPathName());
    return true;
}

void AudioEngine::refreshResolvedSampleReferences (const juce::String& originalPath)
{
    std::vector<std::pair<int, SampleDataPtr>> channelSamples;
    auto chansTree = Model::getChannels (project);
    for (int i = 0; i < chansTree.getNumChildren(); ++i)
    {
        auto t = chansTree.getChild (i);
        if (! t.hasType (IDs::Channel) || t.getProperty (IDs::type).toString() != "sampler")
            continue;

        auto path = t.getProperty (IDs::samplePath).toString();
        if (originalPath.isNotEmpty() && path != originalPath)
            continue;

        channelSamples.push_back ({ (int) t.getProperty (IDs::id),
                                    loadProcessedSample (path, SampleFxSettings::fromTree (t)) });
    }

    if (! channelSamples.empty())
    {
        const juce::ScopedLock sl (engineLock);
        for (auto& entry : channelSamples)
            if (auto* c = findChannel (entry.first))
                c->sample = entry.second;
    }

    recompileSequences();
}

AudioEngine::SampleDataPtr AudioEngine::loadSample (const juce::String& pathOrBuiltin)
{
    if (pathOrBuiltin.isEmpty())
        return nullptr;

    const bool builtin = pathOrBuiltin.startsWith ("builtin:");
    juce::File sourceFile;
    juce::String resolvedPath;
    juce::int64 fileSize = 0;
    juce::int64 modifiedMs = 0;
    if (! builtin)
    {
        sourceFile = resolveSampleFile (pathOrBuiltin);
        if (sourceFile.existsAsFile())
        {
            resolvedPath = sourceFile.getFullPathName();
            fileSize = sourceFile.getSize();
            modifiedMs = sourceFile.getLastModificationTime().toMilliseconds();
        }
    }

    {
        const juce::ScopedLock sl (sampleCacheLock);
        auto it = sampleCache.find (pathOrBuiltin);
        if (it != sampleCache.end())
        {
            const bool current = builtin
                || (it->second.resolvedPath == resolvedPath
                    && it->second.fileSize == fileSize
                    && it->second.modifiedMs == modifiedMs);
            if (current)
            {
                lruTouch (sampleLru, pathOrBuiltin);
                return it->second.data;
            }
            sampleCacheBytes -= it->second.bytes;
            sampleCache.erase (it);
            sampleLru.remove (pathOrBuiltin);
        }
    }

    SampleDataPtr data;
    if (builtin)
    {
        data = synthesizeBuiltin (pathOrBuiltin.fromFirstOccurrenceOf (":", false, false));
    }
    else if (sourceFile.existsAsFile())
    {
        if (auto reader = std::unique_ptr<juce::AudioFormatReader> (audioFormats.createReaderFor (sourceFile)))
        {
            auto len = (int) std::min<juce::int64> (reader->lengthInSamples,
                                                    (juce::int64) (reader->sampleRate * 600));
            if (len > 0)
            {
                data = std::make_shared<SampleData>();
                data->sourceSampleRate = reader->sampleRate;
                data->buffer.setSize ((int) juce::jmin (2u, reader->numChannels), len);
                reader->read (&data->buffer, 0, len, 0, true, reader->numChannels > 1);

                for (int ch = 0; ch < data->buffer.getNumChannels(); ++ch)
                {
                    auto* w = data->buffer.getWritePointer (ch);
                    for (int i = 0; i < len; ++i)
                        if (! std::isfinite (w[i]))
                            w[i] = 0.0f;
                }
            }
        }
    }

    if (data != nullptr)
    {
        const juce::ScopedLock sl (sampleCacheLock);
        const size_t bytes = (size_t) data->buffer.getNumChannels()
                           * (size_t) juce::jmax (0, data->buffer.getNumSamples())
                           * sizeof (float);
        auto existing = sampleCache.find (pathOrBuiltin);
        if (existing != sampleCache.end())
            sampleCacheBytes -= existing->second.bytes;
        sampleCache[pathOrBuiltin] = { data, bytes, resolvedPath, fileSize, modifiedMs };
        sampleCacheBytes += bytes;
        lruTouch (sampleLru, pathOrBuiltin);
        lruEvict (sampleCache, sampleLru, sampleCacheBytes, sampleCacheMaxBytes);
    }
    return data;
}

void AudioEngine::lruTouch (std::list<juce::String>& lru, const juce::String& key)
{
    lru.remove (key);
    lru.push_front (key);
}

void AudioEngine::lruEvict (SampleCacheMap& cache, std::list<juce::String>& lru,
                            size_t& bytes, size_t maxBytes)
{
    while (bytes > maxBytes && lru.size() > 1)
    {
        const auto victim = lru.back();
        lru.pop_back();
        auto it = cache.find (victim);
        if (it != cache.end())
        {
            bytes -= it->second.bytes;
            cache.erase (it);
        }
    }
}

static int declickFadeInSamples (int mode, double sr);
static int declickReleaseSamples (int mode, double sr);

static void applyCosineFade (juce::AudioBuffer<float>& buffer, int start, int count, bool fadeIn)
{
    if (count <= 1)
        return;
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        auto* samples = buffer.getWritePointer (ch, start);
        for (int i = 0; i < count; ++i)
        {
            const float phase = (float) i / (float) (count - 1);
            const float rising = 0.5f - 0.5f * std::cos (
                juce::MathConstants<float>::pi * phase);
            samples[i] *= fadeIn ? rising : 1.0f - rising;
        }
    }
}

static int samplerStretchMode (const juce::var& value)
{
    if (value.isInt() || value.isInt64())
        return juce::jlimit (0, 5, (int) value);

    auto mode = value.toString().toLowerCase().removeCharacters (" _-");
    if (mode.isEmpty() || mode == "resample" || mode == "realtime")
        return 0;
    if (mode == "stretch")
        return 1;
    if (mode == "stretchpro")
        return 2;
    if (mode == "auto")
        return 3;
    if (mode.contains ("slice"))
        return 5;
    return 4;
}

AudioEngine::SampleFxSettings AudioEngine::SampleFxSettings::fromTree (const juce::ValueTree& t)
{
    SampleFxSettings s;
    s.start       = juce::jlimit (0.0f, 1.0f, (float) (double) t.getProperty (IDs::smpStart, 0.0));
    s.length      = juce::jlimit (0.0f, 1.0f, (float) (double) t.getProperty (IDs::smpLength, 1.0));
    s.trim        = juce::jlimit (0.0f, 1.0f, (float) (double) t.getProperty (IDs::smpTrim, 0.0));
    s.declickMode = juce::jlimit (0, 6, (int) t.getProperty (IDs::declickMode, 0));
    s.fadeIn      = juce::jlimit (0.0f, 1.0f, (float) (double) t.getProperty (IDs::fadeIn, 0.0));
    s.fadeOut     = juce::jlimit (0.0f, 1.0f, (float) (double) t.getProperty (IDs::fadeOut, 0.0));
    s.keepOnDisk  = (bool) t.getProperty (IDs::sampleKeepOnDisk, false);
    s.removeDC    = (bool) t.getProperty (IDs::fxRemoveDC,    false);
    s.normalize   = (bool) t.getProperty (IDs::fxNormalize,   false);
    s.reverse     = (bool) t.getProperty (IDs::fxReverse,     false);
    s.revPolarity = (bool) t.getProperty (IDs::fxRevPolarity, false);
    s.fadeStereo  = (bool) t.getProperty (IDs::fxFadeStereo,  false);
    s.swapStereo  = (bool) t.getProperty (IDs::fxSwapStereo,  false);
    return s;
}

AudioEngine::SamplePlaybackSettings AudioEngine::SamplePlaybackSettings::fromTree (
    const juce::ValueTree& t)
{
    SamplePlaybackSettings s;
    s.cubic = (bool) t.getProperty (IDs::sampleResample, false);
    s.useLoop = (bool) t.getProperty (IDs::useLoopPoints, false);
    s.pingPong = (bool) t.getProperty (IDs::pingPongLoop, false);
    s.startOffset = juce::jlimit (0.0f, 1.0f,
        (float) (double) t.getProperty (IDs::startOffset, 0.0));
    s.loopStart = juce::jlimit (0.0f, 1.0f,
        (float) (double) t.getProperty (IDs::loopStart, 0.0));
    s.loopEnd = juce::jlimit (0.0f, 1.0f,
        (float) (double) t.getProperty (IDs::loopEnd, 1.0));
    s.loopCrossfade = juce::jlimit (0.0f, 1.0f,
        (float) (double) t.getProperty (IDs::loopCrossfade, 0.0));
    s.stretchMode = samplerStretchMode (t.getProperty (IDs::stretchMode, "resample"));
    s.stretchPitchSemis = juce::jlimit (-48.0f, 48.0f,
        (float) (double) t.getProperty (IDs::stretchPitchSemis, 0.0));
    s.stretchMultiplier = juce::jlimit (0.01f, 64.0f,
        (float) (double) t.getProperty (IDs::stretchMultiplier, 1.0));
    s.stretchTime = juce::jlimit (0.01f, 64.0f,
        (float) (double) t.getProperty (IDs::stretchTime, 1.0));
    s.stretchTempoMode = juce::jlimit (0, 3,
        (int) t.getProperty (IDs::stretchTempoMode, 0));
    s.stretchSourceBpm = juce::jlimit (0.0f, 999.0f,
        (float) (double) t.getProperty (IDs::stretchSourceBpm, 0.0));
    s.stretchBeats = juce::jlimit (0.0f, 1024.0f,
        (float) (double) t.getProperty (IDs::stretchBeats, 0.0));
    s.formantShift = juce::jlimit (-1.0f, 1.0f,
        (float) (double) t.getProperty (IDs::formantShift, 0.0));
    return s;
}

juce::String AudioEngine::SamplePlaybackSettings::signature() const
{
    return juce::String::formatted (
        "q%d_u%d_p%d_a%.9f_ls%.9f_le%.9f_x%.9f_m%d_sp%.9f_sm%.9f_st%.9f_tm%d_sb%.9f_b%.9f_f%.9f",
        (int) cubic, (int) useLoop, (int) pingPong, startOffset,
        loopStart, loopEnd, loopCrossfade, stretchMode, stretchPitchSemis,
        stretchMultiplier, stretchTime, stretchTempoMode, stretchSourceBpm,
        stretchBeats, formantShift);
}

bool AudioEngine::SampleFxSettings::isDefault() const noexcept
{
    return start == 0.0f && length == 1.0f && fadeIn == 0.0f && fadeOut == 0.0f
        && ! removeDC && ! normalize && ! reverse && ! revPolarity && ! fadeStereo && ! swapStereo;
}

juce::String AudioEngine::SampleFxSettings::signature() const
{
    return juce::String::formatted ("s%.9f_l%.9f_t%.9f_d%d_i%.9f_o%.9f_k%d_%d%d%d%d%d%d",
                                    start, length, trim, declickMode, fadeIn, fadeOut,
                                    (int) keepOnDisk,
                                    (int) removeDC, (int) normalize, (int) reverse,
                                    (int) revPolarity, (int) fadeStereo, (int) swapStereo);
}

AudioEngine::SampleRegion AudioEngine::sampleRegion (const SampleData& raw, const SampleFxSettings& s)
{
    SampleRegion r;
    const int srcLen = raw.buffer.getNumSamples();
    const int numCh  = raw.buffer.getNumChannels();
    if (srcLen <= 0 || numCh <= 0)
        return r;

    r.windowStart = juce::jlimit (0, srcLen - 1,
        (int) std::llround ((double) s.start * (double) (srcLen - 1)));
    r.windowLen = juce::jlimit (1, srcLen - r.windowStart,
        juce::jmax (1, (int) std::llround ((double) s.length * srcLen)));
    r.start = r.windowStart;
    r.len   = r.windowLen;
    if (s.trim <= 0.0f)
        return r;

    const float thresh = sampleTrimThreshold (s.trim);
    const int win = juce::jmax (1, (int) std::llround (raw.sourceSampleRate
                                                         * sampleTrimWindowMs * 0.001));
    const int regionEnd = r.windowStart + r.windowLen;

    auto windowPeak = [&] (int wStart, int wEnd)
    {
        float peak = 0.0f;
        for (int ch = 0; ch < numCh; ++ch)
            peak = juce::jmax (peak, raw.buffer.getMagnitude (ch, wStart, wEnd - wStart));
        return peak;
    };

    int head = regionEnd - 1;
    for (int wStart = r.windowStart; wStart < regionEnd; wStart += win)
    {
        if (windowPeak (wStart, juce::jmin (regionEnd, wStart + win)) > thresh)
        {
            head = wStart;
            break;
        }
    }

    int tail = head + 1;
    for (int wEnd = regionEnd; wEnd > head; wEnd -= win)
    {
        if (windowPeak (juce::jmax (head, wEnd - win), wEnd) > thresh)
        {
            tail = wEnd;
            break;
        }
    }

    r.start = head;
    r.len   = juce::jlimit (1, srcLen - head, tail - head);
    return r;
}

bool AudioEngine::getSampleTrimBounds (const juce::String& pathOrBuiltin,
                                       const juce::ValueTree& settingsSource,
                                       SampleTrimBounds& out)
{
    SampleDataPtr raw;
    {
        const juce::ScopedLock sl (sampleCacheLock);
        auto it = sampleCache.find (pathOrBuiltin);
        if (it == sampleCache.end())
            return false;
        raw = it->second.data;
    }
    if (raw == nullptr || raw->sourceSampleRate <= 0.0)
        return false;

    const auto settings = SampleFxSettings::fromTree (settingsSource);
    const auto r = settings.keepOnDisk
        ? SampleRegion { 0, raw->buffer.getNumSamples(), 0, raw->buffer.getNumSamples() }
        : sampleRegion (*raw, settings);
    if (r.len <= 0)
        return false;

    out.startSeconds  = r.start / raw->sourceSampleRate;
    out.lengthSeconds = r.len   / raw->sourceSampleRate;
    return true;
}

AudioEngine::SampleDataPtr AudioEngine::applySampleFx (const SampleDataPtr& raw, const SampleFxSettings& s)
{
    if (raw == nullptr)
        return raw;
    if (s.keepOnDisk)
        return raw;

    const int srcLen = raw->buffer.getNumSamples();
    const int numCh  = raw->buffer.getNumChannels();
    if (srcLen <= 0 || numCh <= 0)
        return raw;

    const auto region = sampleRegion (*raw, s);
    int startS = region.start;
    int len    = region.len;
    if (len <= 0)
        return raw;

    const int untrimmedStart = region.windowStart;
    const int untrimmedLen   = region.windowLen;

    if (s.isDefault() && startS == untrimmedStart && len == untrimmedLen)
        return raw;

    auto out = std::make_shared<SampleData>();
    out->sourceSampleRate = raw->sourceSampleRate;
    out->buffer.setSize (numCh, len);
    for (int ch = 0; ch < numCh; ++ch)
        out->buffer.copyFrom (ch, 0, raw->buffer, ch, startS, len);

    auto& buf = out->buffer;

    if (startS > untrimmedStart)
    {
        const int fade = juce::jmin (len, declickFadeInSamples (s.declickMode, raw->sourceSampleRate));
        if (fade > 1)
            applyCosineFade (buf, 0, fade, true);
    }

    if (startS + len < untrimmedStart + untrimmedLen)
    {
        const int fade = juce::jmin (len, declickReleaseSamples (s.declickMode, raw->sourceSampleRate));
        if (fade > 1)
            applyCosineFade (buf, len - fade, fade, false);
    }

    if (s.removeDC)
        for (int ch = 0; ch < numCh; ++ch)
        {
            double sum = 0.0;
            const float* p = buf.getReadPointer (ch);
            for (int i = 0; i < len; ++i) sum += p[i];
            float mean = (float) (sum / len);
            float* w = buf.getWritePointer (ch);
            for (int i = 0; i < len; ++i) w[i] -= mean;
        }

    if (s.normalize)
    {
        float peak = 0.0f;
        for (int ch = 0; ch < numCh; ++ch)
            peak = juce::jmax (peak, buf.getMagnitude (ch, 0, len));
        if (peak > 1.0e-6f)
            buf.applyGain (1.0f / peak);
    }

    if (s.reverse)
        for (int ch = 0; ch < numCh; ++ch)
            std::reverse (buf.getWritePointer (ch), buf.getWritePointer (ch) + len);

    if (s.revPolarity)
        buf.applyGain (-1.0f);

    if (s.swapStereo && numCh > 1)
    {
        float* a = buf.getWritePointer (0);
        float* b = buf.getWritePointer (1);
        for (int i = 0; i < len; ++i) std::swap (a[i], b[i]);
    }

    if (s.fadeStereo && numCh > 1 && len > 1)
    {
        float* a = buf.getWritePointer (0);
        float* b = buf.getWritePointer (1);
        for (int i = 0; i < len; ++i)
        {
            float t = (float) i / (float) (len - 1);
            a[i] *= (1.0f - t);
            b[i] *= t;
        }
    }

    int fadeInS  = juce::jlimit (0, len, (int) std::llround ((double) s.fadeIn  * 0.5 * len));
    int fadeOutS = juce::jlimit (0, len, (int) std::llround ((double) s.fadeOut * 0.5 * len));
    if (fadeInS > 1)
        applyCosineFade (buf, 0, fadeInS, true);
    if (fadeOutS > 1)
        applyCosineFade (buf, len - fadeOutS, fadeOutS, false);

    return out;
}

AudioEngine::SampleDataPtr AudioEngine::loadProcessedSample (const juce::String& path, const SampleFxSettings& s)
{
    juce::String version;
    if (! path.startsWith ("builtin:"))
    {
        auto file = resolveSampleFile (path);
        if (file.existsAsFile())
            version = "|" + file.getFullPathName()
                    + "|" + juce::String (file.getSize())
                    + "|" + juce::String (file.getLastModificationTime().toMilliseconds());
    }
    const auto key = path + version + "|" + s.signature();

    {
        const juce::ScopedLock sl (processedCacheLock);
        auto it = processedCache.find (key);
        if (it != processedCache.end())
        {
            lruTouch (processedLru, key);
            return it->second.data;
        }
    }

    auto raw = loadSample (path);
    if (raw == nullptr)
        return raw;

    auto processed = applySampleFx (raw, s);
    if (processed != nullptr && processed != raw)
    {
        const juce::ScopedLock sl (processedCacheLock);
        const size_t bytes = (size_t) processed->buffer.getNumChannels()
                           * (size_t) juce::jmax (0, processed->buffer.getNumSamples())
                           * sizeof (float);
        auto existing = processedCache.find (key);
        if (existing != processedCache.end())
            processedCacheBytes -= existing->second.bytes;
        processedCache[key] = { processed, bytes };
        processedCacheBytes += bytes;
        lruTouch (processedLru, key);
        lruEvict (processedCache, processedLru, processedCacheBytes, processedCacheMaxBytes);
    }
    return processed;
}

void AudioEngine::updateChannelAtomics (EngineChannel& c, const juce::ValueTree& t)
{
    c.volume.store ((float) (double) t.getProperty (IDs::volume, 0.8));
    c.pan.store ((float) (double) t.getProperty (IDs::pan, 0.0));
    c.mute.store ((bool) t.getProperty (IDs::mute, false));
    c.cutItself.store ((bool) t.getProperty (IDs::cutItself, false));
    c.mixerTrack.store (juce::jlimit (0, Model::maxMixerTracks, (int) t.getProperty (IDs::mixerTrack, 1)));
    c.declickMode.store (juce::jlimit (0, 6, (int) t.getProperty (IDs::declickMode, 0)));
    c.pitchSemis.store ((float) (double) t.getProperty (IDs::pitchSemis, 0.0));
    c.rootNote.store (juce::jlimit (0, 127, (int) t.getProperty (IDs::rootNote, 60)));
    c.fineTune.store (juce::jlimit (-100.0f, 100.0f, (float) (double) t.getProperty (IDs::fineTune, 0.0)));
    auto playback = SamplePlaybackSettings::fromTree (t);
    c.sampleResample.store (playback.cubic);
    c.useLoopPoints.store (playback.useLoop);
    c.pingPongLoop.store (playback.pingPong);
    c.startOffset.store (playback.startOffset);
    c.loopStart.store (playback.loopStart);
    c.loopEnd.store (playback.loopEnd);
    c.loopCrossfade.store (playback.loopCrossfade);
    c.stretchMode.store (playback.stretchMode);
    c.stretchPitchSemis.store (playback.stretchPitchSemis);
    c.stretchMultiplier.store (playback.stretchMultiplier);
    c.stretchTime.store (playback.stretchTime);
    c.stretchTempoMode.store (playback.stretchTempoMode);
    c.stretchSourceBpm.store (playback.stretchSourceBpm);
    c.stretchBeats.store (playback.stretchBeats);
    c.formantShift.store (playback.formantShift);
}

void AudioEngine::updateTrackAtomics (EngineMixerTrack& tr, const juce::ValueTree& t)
{
    tr.volume.store ((float) (double) t.getProperty (IDs::volume, 1.0));
    tr.pan.store ((float) (double) t.getProperty (IDs::pan, 0.0));
    tr.stereoSep.store (juce::jlimit (-1.0f, 1.0f, (float) (double) t.getProperty (IDs::stereoSep, 0.0)));
    tr.mute.store ((bool) t.getProperty (IDs::mute, false));
    tr.routeTo.store (juce::jlimit (-1, Model::maxMixerTracks, (int) t.getProperty (IDs::routeTo, 0)));

    int il = -1, ir = -1;
    auto inStr = t.getProperty (IDs::audioInput).toString().trim();
    if (inStr.isNotEmpty())
    {
        il = inStr.upToFirstOccurrenceOf (",", false, false).getIntValue();
        if (inStr.containsChar (','))
            ir = inStr.fromFirstOccurrenceOf (",", false, false).getIntValue();
        if (il < 0)
            il = ir = -1;
    }
    tr.inputL.store (il);
    tr.inputR.store (ir);
    tr.recArm.store ((bool) t.getProperty (IDs::recArm, false));
}

void AudioEngine::preparePlugin (juce::AudioProcessor& p, bool)
{
    if (currentSampleRate <= 0)
        return;
    p.suspendProcessing (true);
    p.setRateAndBufferSizeDetails (currentSampleRate, currentBlockSize);
    p.prepareToPlay (currentSampleRate, currentBlockSize);
    p.suspendProcessing (false);
}

void AudioEngine::enqueuePluginLoad (LoadJob&& job)
{
    LoadProgress::noteQueued();

    auto displayName = job.displayName;
    queueFilePrewarm (job.desc.fileOrIdentifier);
    if (job.desc.pluginFormatName == "VST3")
    {
        messageThreadQueue.push_back (std::move (job));
        scheduleMessageThreadLoad();
    }
    else
    {
        {
            const std::lock_guard<std::mutex> lk (loaderMutex);
            loadQueue.push_back (std::move (job));
        }
        loaderWake.signal();
    }
    if (onPluginLoading && displayName.isNotEmpty())
        onPluginLoading (displayName);
}

void AudioEngine::scheduleMessageThreadLoad()
{
    if (messageLoadScheduled)
        return;
    messageLoadScheduled = true;
    juce::WeakReference<AudioEngine> weak (this);
    juce::Timer::callAfterDelay (40, [weak]
    {
        if (auto* self = weak.get())
            self->drainMessageThreadLoad();
    });
}

void AudioEngine::drainMessageThreadLoad()
{
    messageLoadScheduled = false;
    if (messageThreadQueue.empty())
        return;

    LoadJob job = std::move (messageThreadQueue.front());
    messageThreadQueue.pop_front();

    juce::String error;
    LoadProgress::enter (job.desc.name);
    auto inst = host.createInstance (job.desc, job.sampleRate, job.blockSize, error);
    LoadProgress::leave();
    if (inst == nullptr)
        DBG ("VST3 message-thread load failed: " + job.desc.name + " - " + error);

    LoadResult res;
    res.instance = std::move (inst);
    res.job = std::move (job);
    finishPluginLoad (std::move (res));

    if (! messageThreadQueue.empty())
    {
        messageLoadScheduled = true;
        juce::WeakReference<AudioEngine> weak (this);
        juce::MessageManager::callAsync ([weak]
        {
            if (auto* self = weak.get())
                self->drainMessageThreadLoad();
        });
    }
}

void AudioEngine::pluginLoaderRun()
{
   #if JUCE_WINDOWS
    const bool comOk = SUCCEEDED (CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED));
   #endif

    while (! loaderShutdown.load())
    {
        LoadJob job;
        bool have = false;
        {
            const std::lock_guard<std::mutex> lk (loaderMutex);
            if (! loadQueue.empty())
            {
                job = std::move (loadQueue.front());
                loadQueue.pop_front();
                have = true;
            }
        }
        if (! have)
        {
            loaderWake.wait (200);
            continue;
        }

        juce::String error;
        LoadProgress::enter (job.desc.name);
        auto inst = host.createInstance (job.desc, job.sampleRate, job.blockSize, error);
        LoadProgress::leave();
        if (inst == nullptr)
            DBG ("Background load failed: " + job.desc.name + " - " + error);

        LoadResult res;
        if (inst != nullptr)
        {
            LoadProgress::enter (job.desc.name);
            inst->setPlayHead (this);
            if (job.state.getSize() > 0)
            {
                inst->setStateInformation (job.state.getData(), (int) job.state.getSize());
                synclog ("state INSTANCE-CREATE key=" + job.key + " bytes="
                         + juce::String ((int) job.state.getSize()) + " sum="
                         + juce::String::toHexString ((int) SyncDebug::sum (job.state.getData(), job.state.getSize()))
                         + " (background load, applied before prepareToPlay)");
            }
            res.preparedRate = currentSampleRate;
            res.preparedBlock = currentBlockSize;
            preparePlugin (*inst, job.isChannel);
            res.prepared = true;
            LoadProgress::leave();
        }
        res.instance = std::move (inst);
        res.job = std::move (job);
        {
            const std::lock_guard<std::mutex> lk (loaderMutex);
            completedLoads.push_back (std::move (res));
        }
        triggerCompletionAsync();
    }

   #if JUCE_WINDOWS
    if (comOk) CoUninitialize();
   #endif
}

void AudioEngine::queueFilePrewarm (const juce::String& path)
{
    if (path.isEmpty() || ! juce::File::isAbsolutePath (path))
        return;

    {
        const std::lock_guard<std::mutex> lk (prewarmMutex);
        if (! prewarmSeen.insert (path).second)
            return;
        prewarmQueue.push_back (path);
    }
    prewarmWake.signal();
}

void AudioEngine::filePrewarmRun()
{
    static constexpr int chunkBytes = 1024 * 1024;
    static constexpr juce::int64 maxBytesPerFile = 512ll * 1024 * 1024;
    juce::HeapBlock<char> scratch (chunkBytes);

    auto warmFile = [this, &scratch] (const juce::File& f)
    {
        juce::FileInputStream in (f);
        if (! in.openedOk())
            return;
        juce::int64 total = 0;
        while (total < maxBytesPerFile && ! in.isExhausted() && ! prewarmShutdown.load())
        {
            const int got = in.read (scratch.getData(), chunkBytes);
            if (got <= 0)
                break;
            total += got;
        }
    };

    while (! prewarmShutdown.load())
    {
        juce::String path;
        {
            const std::lock_guard<std::mutex> lk (prewarmMutex);
            if (! prewarmQueue.empty())
            {
                path = prewarmQueue.front();
                prewarmQueue.pop_front();
            }
        }
        if (path.isEmpty())
        {
            prewarmWake.wait (250);
            continue;
        }

        const juce::File f (path);
        if (f.isDirectory())
        {
            for (const auto& entry : juce::RangedDirectoryIterator (f, true, "*", juce::File::findFiles))
            {
                if (prewarmShutdown.load())
                    break;
                warmFile (entry.getFile());
            }
        }
        else if (f.existsAsFile())
        {
            warmFile (f);
        }
    }
}

void AudioEngine::triggerCompletionAsync()
{
    juce::WeakReference<AudioEngine> weak (this);
    juce::MessageManager::callAsync ([weak]
    {
        if (auto* self = weak.get())
            self->processCompletedLoads();
    });
}

void AudioEngine::processCompletedLoads()
{
    std::vector<LoadResult> done;
    {
        const std::lock_guard<std::mutex> lk (loaderMutex);
        done.swap (completedLoads);
    }
    for (auto& r : done)
        finishPluginLoad (std::move (r));
}

void AudioEngine::finishPluginLoad (LoadResult&& r)
{
    auto& job = r.job;

    bool wanted = false;
    if (job.isChannel)
    {
        if (auto* c = findChannel (job.channelId))
            wanted = c->loading && c->loadEpoch == job.epoch;
    }
    else if (auto* fx = findEffect (job.key))
        wanted = fx->loading && fx->loadEpoch == job.epoch;

    if (! wanted)
    {
        r.instance.reset();
        LoadProgress::noteFinished();
        return;
    }

    if (r.instance == nullptr)
    {
        juce::String error;
        LoadProgress::enter (job.desc.name);
        r.instance = host.createInstance (job.desc, job.sampleRate, job.blockSize, error);
        LoadProgress::leave();
    }

    if (r.instance == nullptr)
    {
        applyLoadFailure (job);
        return;
    }

    LoadProgress::enter (job.desc.name);
    auto instance = std::move (r.instance);
    if (! r.prepared)
    {
        instance->setPlayHead (this);
        if (job.state.getSize() > 0)
        {
            instance->setStateInformation (job.state.getData(), (int) job.state.getSize());
            synclog ("state INSTANCE-CREATE key=" + job.key + " bytes="
                     + juce::String ((int) job.state.getSize()) + " sum="
                     + juce::String::toHexString ((int) SyncDebug::sum (job.state.getData(), job.state.getSize()))
                     + " (background load, applied before prepareToPlay)");
        }
    }
    if (! r.prepared
        || ! juce::approximatelyEqual (r.preparedRate, currentSampleRate)
        || currentBlockSize > r.preparedBlock)
        preparePlugin (*instance, job.isChannel);
    LoadProgress::leave();
    LoadProgress::noteFinished();
    auto listener = std::make_unique<ParamListener> (*this, job.key);
    instance->addListener (listener.get());

    std::unique_ptr<juce::AudioProcessor> oldInstance;
    std::unique_ptr<ParamListener> oldListener;
    {
        const juce::ScopedLock lifetime (pluginLifetimeLock);
        const juce::ScopedLock sl (engineLock);
        if (job.isChannel)
        {
            auto* c = findChannel (job.channelId);
            oldInstance = std::move (c->plugin);
            oldListener = std::move (c->paramListener);
            c->plugin = std::move (instance);
            c->paramListener = std::move (listener);
            c->loading = false;
            c->asleep = false;
            c->silentBlocks = 0;
        }
        else
        {
            auto* fx = findEffect (job.key);
            oldInstance = std::move (fx->processor);
            oldListener = std::move (fx->paramListener);
            fx->processor = std::move (instance);
            fx->paramListener = std::move (listener);
            fx->loading = false;
            fx->asleep = false;
            fx->silentBlocks = 0;
        }
    }

    if (oldInstance != nullptr && oldListener != nullptr)
        oldInstance->removeListener (oldListener.get());
    oldInstance.reset();
    oldListener.reset();

    if (onPluginCreated)
        onPluginCreated (job.key);
}

void AudioEngine::applyLoadFailure (const LoadJob& job)
{
    LoadProgress::leave();
    LoadProgress::noteFinished();
    auto missingName = job.displayName.isNotEmpty() ? job.displayName : juce::String ("a plugin");
    if (job.isChannel)
    {
        auto placeholder = std::make_unique<PlaceholderSynth>();
        placeholder->setPlayHead (this);
        preparePlugin (*placeholder, true);
        {
            const juce::ScopedLock sl (engineLock);
            if (auto* c = findChannel (job.channelId))
            {
                c->plugin = std::move (placeholder);
                c->placeholder = true;
                c->loading = false;
                c->asleep = false;
                c->silentBlocks = 0;
            }
        }
        if (onPluginMissing)
            onPluginMissing (job.channelId, missingName);
    }
    else
    {
        {
            const juce::ScopedLock sl (engineLock);
            if (auto* fx = findEffect (job.key))
                fx->loading = false;
        }
        if (onEffectMissing)
            onEffectMissing (job.key, missingName);
    }
}

void AudioEngine::rebuildChannels()
{
    if (rebuildInProgress)
    {
        markDirty (true, false, false);
        return;
    }
    const juce::ScopedValueSetter<bool> rebuilding (rebuildInProgress, true);

    auto chansTree = Model::getChannels (project);

    std::vector<std::unique_ptr<EngineChannel>> created;
    std::vector<int> wantedIds;
    juce::StringArray createdKeys;

    for (int i = 0; i < chansTree.getNumChildren(); ++i)
    {
        auto t = chansTree.getChild (i);
        if (! t.hasType (IDs::Channel))
            continue;
        int id = (int) t.getProperty (IDs::id);
        wantedIds.push_back (id);

        auto typeStr = t.getProperty (IDs::type).toString();
        bool wantsBuiltin = typeStr == "builtin";
        bool wantsPlugin = typeStr == "plugin" || wantsBuiltin;

        juce::PluginDescription desc;
        if (wantsPlugin && ! wantsBuiltin)
            if (auto xml = juce::parseXML (t.getProperty (IDs::pluginDesc).toString()))
                desc.loadFromXml (*xml);
        juce::String wantedPluginId = wantsBuiltin ? juce::String ("builtin:piano")
                                    : wantsPlugin  ? desc.createIdentifierString()
                                                   : juce::String();

        if (auto* existing = findChannel (id))
        {
            if (existing->isPlugin == wantsPlugin && existing->pluginId == wantedPluginId)
            {
                updateChannelAtomics (*existing, t);
                if (! wantsPlugin)
                {
                    auto fx = SampleFxSettings::fromTree (t);
                    auto sig = fx.signature();
                    if (existing->sampleSig != sig)
                    {
                        auto sample = loadProcessedSample (t.getProperty (IDs::samplePath).toString(), fx);
                        const juce::ScopedLock sl (engineLock);
                        existing->sample = sample;
                        existing->sampleSig = sig;
                    }
                }
                continue;
            }
        }

        auto ec = std::make_unique<EngineChannel>();
        ec->id = id;
        ec->isPlugin = wantsPlugin;
        ec->pluginId = wantedPluginId;
        updateChannelAtomics (*ec, t);

        if (wantsBuiltin)
        {
            ec->plugin = std::make_unique<PlaceholderSynth>();
            ec->plugin->setPlayHead (this);
            preparePlugin (*ec->plugin, true);
            ec->paramListener = std::make_unique<ParamListener> (*this, "c" + juce::String (id));
            ec->plugin->addListener (ec->paramListener.get());
        }
        else if (ec->isPlugin)
        {
            ec->loading = true;
            ec->loadEpoch = ++loadEpochCounter;

            LoadJob job;
            job.isChannel = true;
            job.channelId = id;
            job.key = "c" + juce::String (id);
            job.pluginId = wantedPluginId;
            job.displayName = desc.name;
            job.desc = desc;
            job.state = blobFromBase64 (t.getProperty (IDs::pluginState).toString());
            job.epoch = ec->loadEpoch;
            job.sampleRate = currentSampleRate;
            job.blockSize = currentBlockSize;
            enqueuePluginLoad (std::move (job));
        }
        else
        {
            auto fx = SampleFxSettings::fromTree (t);
            ec->sampleSig = fx.signature();
            ec->sample = loadProcessedSample (t.getProperty (IDs::samplePath).toString(), fx);
        }
        if (ec->plugin != nullptr)
            createdKeys.add ("c" + juce::String (id));
        created.push_back (std::move (ec));
    }

    std::vector<std::unique_ptr<EngineChannel>> graveyard;
    {
        const juce::ScopedLock sl (engineLock);
        std::vector<std::unique_ptr<EngineChannel>> newList;

        for (int id : wantedIds)
        {
            bool found = false;
            for (auto& c : created)
                if (c != nullptr && c->id == id)
                {
                    newList.push_back (std::move (c));
                    found = true;
                    break;
                }
            if (! found)
                for (auto& existing : channels)
                    if (existing != nullptr && existing->id == id)
                    {
                        newList.push_back (std::move (existing));
                        break;
                    }
        }

        for (auto& leftover : channels)
            if (leftover != nullptr)
                graveyard.push_back (std::move (leftover));

        channels = std::move (newList);
    }

    {
        const juce::ScopedLock lifetime (pluginLifetimeLock);
        for (auto& dead : graveyard)
        {
            if (dead->plugin != nullptr)
            {
                if (onPluginRemoved)
                    onPluginRemoved ("c" + juce::String (dead->id));
                if (dead->paramListener)
                    dead->plugin->removeListener (dead->paramListener.get());
                const juce::ScopedLock sl (dead->plugin->getCallbackLock());
                dead->plugin->reset();
            }
        }
        graveyard.clear();
    }

    if (onPluginCreated)
        for (auto& key : createdKeys)
            onPluginCreated (key);
}

void AudioEngine::rebuildMixer()
{
    if (rebuildInProgress)
    {
        markDirty (false, true, false);
        return;
    }
    const juce::ScopedValueSetter<bool> rebuilding (rebuildInProgress, true);

    auto mixerTree = Model::getMixer (project);
    std::vector<std::unique_ptr<EngineEffect>> graveyard;
    juce::StringArray createdKeys;

    for (int ti = 0; ti <= Model::maxMixerTracks; ++ti)
    {
        auto trackTree = mixerTree.getChildWithProperty (IDs::index, ti);
        auto& track = *mixerTracks[(size_t) ti];
        if (! trackTree.isValid())
            continue;

        updateTrackAtomics (track, trackTree);

        juce::Array<juce::ValueTree> slots;
        for (int i = 0; i < trackTree.getNumChildren(); ++i)
        {
            auto s = trackTree.getChild (i);
            if (s.hasType (IDs::Slot))
                slots.add (s);
        }
        std::sort (slots.begin(), slots.end(),
                   [] (const juce::ValueTree& a, const juce::ValueTree& b)
                   { return (int) a.getProperty (IDs::slotIndex) < (int) b.getProperty (IDs::slotIndex); });

        std::vector<std::unique_ptr<EngineEffect>> created;
        for (auto& s : slots)
        {
            int uid = (int) s.getProperty (IDs::uid);
            bool exists = false;
            for (auto& fx : track.effects)
                if (fx != nullptr && fx->uid == uid)
                { exists = true; break; }
            if (exists)
                continue;

            auto fx = std::make_unique<EngineEffect>();
            fx->uid = uid;
            fx->slotIndex = (int) s.getProperty (IDs::slotIndex);
            fx->enabled.store ((bool) s.getProperty (IDs::enabled, true));
            fx->mix.store (juce::jlimit (0.0f, 1.0f, (float) (double) s.getProperty (IDs::mix, 1.0)));
            fx->key = "m" + juce::String (ti) + "s" + juce::String (uid);

            auto type = s.getProperty (IDs::type).toString();
            if (type == "gain")      fx->processor = std::make_unique<GainProcessor>();
            else if (type == "eq")   fx->processor = std::make_unique<SimpleEQProcessor>();
            else if (type == "softclip") fx->processor = std::make_unique<SoftClipperProcessor>();
            else if (type == "delay")  fx->processor = std::make_unique<DelayProcessor>();
            else if (type == "delaybank") fx->processor = std::make_unique<DelayBankProcessor>();
            else if (type == "reverb") fx->processor = std::make_unique<ReverbProcessor>();
            else if (type == "limiter") fx->processor = std::make_unique<LimiterProcessor>();
            else if (type == "multiband") fx->processor = std::make_unique<MultibandProcessor>();
            else if (type == "soundbeast") fx->processor = std::make_unique<SoundbeastProcessor>();
            else if (type == "void") fx->processor = std::make_unique<VoidProcessor>();
            else if (type == "lowlifter") fx->processor = std::make_unique<LowLifterProcessor>();
            else if (type == "parameq") fx->processor = std::make_unique<ParamEQProcessor>();
            else if (type == "convolver") fx->processor = std::make_unique<ConvolverProcessor>();
            else if (type == "parterre") fx->processor = std::make_unique<ParterreProcessor>();
            else if (type == "equivocal") fx->processor = std::make_unique<EquivocalProcessor>();
            else if (type == "grossbeat") fx->processor = std::make_unique<GrossBeatProcessor>();
            else
            {
                juce::PluginDescription desc;
                if (auto xml = juce::parseXML (s.getProperty (IDs::pluginDesc).toString()))
                    desc.loadFromXml (*xml);
                fx->isPlugin = true;
                fx->loading = true;
                fx->loadEpoch = ++loadEpochCounter;

                LoadJob job;
                job.isChannel = false;
                job.trackIndex = ti;
                job.slotUid = uid;
                job.key = fx->key;
                job.pluginId = desc.createIdentifierString();
                job.displayName = desc.name;
                job.desc = desc;
                job.state = blobFromBase64 (s.getProperty (IDs::pluginState).toString());
                job.epoch = fx->loadEpoch;
                job.sampleRate = currentSampleRate;
                job.blockSize = currentBlockSize;
                enqueuePluginLoad (std::move (job));
            }

            if (fx->processor != nullptr)
            {
                fx->processor->setPlayHead (this);
                auto stateB64 = s.getProperty (IDs::pluginState).toString();
                if (stateB64.isNotEmpty())
                {
                    auto blob = blobFromBase64 (stateB64);
                    if (blob.getSize() > 0)
                    {
                        fx->processor->setStateInformation (blob.getData(), (int) blob.getSize());
                        synclog ("state INSTANCE-CREATE key=" + fx->key + " bytes="
                                 + juce::String ((int) blob.getSize()) + " sum="
                                 + juce::String::toHexString ((int) SyncDebug::sum (blob.getData(), blob.getSize()))
                                 + " (applied before prepareToPlay)");
                    }
                }
                preparePlugin (*fx->processor, false);
                fx->paramListener = std::make_unique<ParamListener> (*this, fx->key);
                fx->processor->addListener (fx->paramListener.get());
                createdKeys.add (fx->key);
            }
            created.push_back (std::move (fx));
        }

        {
            const juce::ScopedLock sl (engineLock);
            std::vector<std::unique_ptr<EngineEffect>> newFx;
            for (auto& s : slots)
            {
                int uid = (int) s.getProperty (IDs::uid);
                bool moved = false;
                for (auto& fx : track.effects)
                    if (fx != nullptr && fx->uid == uid)
                    {
                        fx->slotIndex = (int) s.getProperty (IDs::slotIndex);
                        fx->enabled.store ((bool) s.getProperty (IDs::enabled, true));
                        fx->mix.store (juce::jlimit (0.0f, 1.0f, (float) (double) s.getProperty (IDs::mix, 1.0)));
                        newFx.push_back (std::move (fx));
                        moved = true;
                        break;
                    }
                if (! moved)
                    for (auto& fx : created)
                        if (fx != nullptr && fx->uid == uid)
                        {
                            newFx.push_back (std::move (fx));
                            break;
                        }
            }
            for (auto& leftover : track.effects)
                if (leftover != nullptr)
                    graveyard.push_back (std::move (leftover));
            track.effects = std::move (newFx);
        }
    }

    {
        const juce::ScopedLock lifetime (pluginLifetimeLock);
        for (auto& dead : graveyard)
        {
            if (dead->processor != nullptr)
            {
                if (onPluginRemoved)
                    onPluginRemoved (dead->key);
                if (dead->paramListener)
                    dead->processor->removeListener (dead->paramListener.get());
                const juce::ScopedLock sl (dead->processor->getCallbackLock());
                dead->processor->reset();
            }
        }
        graveyard.clear();
    }

    if (onPluginCreated)
        for (auto& key : createdKeys)
            onPluginCreated (key);
}

void AudioEngine::recompileSequences()
{
    std::vector<NoteEvent> newSong, newPattern;
    std::vector<SlideEvent> newSongSlides, newPatternSlides;
    std::vector<EngineAudioClip> newClips;
    std::vector<CompiledAutoLane> newAutoLanes;
    juce::int64 newSongLen = Model::ticksPerBar * 4;
    juce::int64 newPatternLen = Model::PPQ * 4;

    auto compilePattern = [] (const juce::ValueTree& pattern, juce::int64 baseTick,
                              juce::int64 clipStart, juce::int64 clipEnd,
                              std::vector<NoteEvent>& out, std::vector<SlideEvent>& slideOut,
                              int clipUid)
    {
        for (int i = 0; i < pattern.getNumChildren(); ++i)
        {
            auto seq = pattern.getChild (i);
            if (! seq.hasType (IDs::ChannelSeq))
                continue;
            int chanId = (int) seq.getProperty (IDs::channelId);

            auto steps = seq.getProperty (IDs::steps).toString();
            for (int s = 0; s < steps.length(); ++s)
            {
                if (steps[s] != '1')
                    continue;
                juce::int64 on = baseTick + (juce::int64) s * Model::ticksPerStep;
                if (on < clipStart || on >= clipEnd)
                    continue;
                juce::int64 off = juce::jmin (clipEnd, on + (juce::int64) Model::ticksPerStep);
                out.push_back ({ on, chanId, 60, 100, true, clipUid });
                out.push_back ({ off, chanId, 60, 100, false, clipUid });
            }

            for (int n = 0; n < seq.getNumChildren(); ++n)
            {
                auto note = seq.getChild (n);
                if (! note.hasType (IDs::Note))
                    continue;
                juce::int64 on = baseTick + (juce::int64) (int) note.getProperty (IDs::start);
                if (on < clipStart || on >= clipEnd)
                    continue;
                juce::int64 off = juce::jmin (clipEnd,
                    on + (juce::int64) juce::jmax (1, (int) note.getProperty (IDs::length)));
                int key = juce::jlimit (0, 127, (int) note.getProperty (IDs::key));
                int vel = juce::jlimit (1, 127, (int) note.getProperty (IDs::velocity, 100));
                if ((bool) note.getProperty (IDs::slide, false))
                {
                    slideOut.push_back ({ on, chanId, key, vel, off - on });
                    continue;
                }
                out.push_back ({ on, chanId, key, vel, true, clipUid });
                out.push_back ({ off, chanId, key, vel, false, clipUid });
            }
        }
    };

    auto curPat = Model::findPatternById (project, currentPatternId.load());
    if (! curPat.isValid() && Model::getPatterns (project).getNumChildren() > 0)
    {
        curPat = Model::getPatterns (project).getChild (0);
        currentPatternId.store ((int) curPat.getProperty (IDs::id));
    }
    if (curPat.isValid())
    {
        auto contentEnd = Model::patternContentEndTicks (curPat);
        if (contentEnd > 0)
            newPatternLen = ((contentEnd + Model::ticksPerBar - 1) / Model::ticksPerBar) * Model::ticksPerBar;
        else
            newPatternLen = juce::jmin ((juce::int64) Model::patternLengthTicks (curPat),
                                        (juce::int64) Model::ticksPerBar);

        compilePattern (curPat, 0, 0, newPatternLen, newPattern, newPatternSlides, 0);
    }

    auto playlist = Model::getPlaylist (project);
    juce::int64 maxEnd = 0;
    for (int i = 0; i < playlist.getNumChildren(); ++i)
    {
        auto clip = playlist.getChild (i);
        if (! clip.hasType (IDs::Clip))
            continue;

        if (! Model::isPlaylistTrackEnabled (project, (int) clip.getProperty (IDs::track)))
            continue;

        auto clipStart = (juce::int64) std::llround ((double) clip.getProperty (IDs::startBeats) * Model::PPQ);
        auto clipEnd = clipStart + (juce::int64) std::llround ((double) clip.getProperty (IDs::lengthBeats) * Model::PPQ);
        if (clipEnd <= clipStart)
            continue;
        maxEnd = juce::jmax (maxEnd, clipEnd);

        auto offTicks = (juce::int64) std::llround (
            (double) clip.getProperty (IDs::offsetBeats, 0.0) * Model::PPQ);

        auto clipType = clip.getProperty (IDs::type).toString();

        if (clipType == "automation")
        {
            if ((bool) clip.getProperty (IDs::mute, false))
                continue;
            auto target = Automation::parseTarget (clip.getProperty (IDs::target).toString());
            if (target.kind == Automation::Target::invalid)
                continue;

            CompiledAutoLane lane;
            lane.target = target;
            lane.startTick = clipStart;
            lane.endTick = clipEnd;
            lane.offsetTick = offTicks;
            for (int c = 0; c < clip.getNumChildren(); ++c)
            {
                auto pt = clip.getChild (c);
                if (! pt.hasType (IDs::Point))
                    continue;
                lane.points.push_back ({ (double) pt.getProperty (IDs::pos) * Model::PPQ,
                                         juce::jlimit (0.0, 1.0, (double) pt.getProperty (IDs::val)),
                                         (int) pt.getProperty (IDs::curveMode, 0),
                                         (double) pt.getProperty (IDs::tension, 0.0) });
            }
            std::sort (lane.points.begin(), lane.points.end(),
                       [] (const CompiledAutoPoint& a, const CompiledAutoPoint& b) { return a.posTicks < b.posTicks; });
            if (! lane.points.empty())
                newAutoLanes.push_back (std::move (lane));
            continue;
        }

        if (clipType == "audio")
        {
            if ((bool) clip.getProperty (IDs::mute, false))
                continue;
            auto clipPath = clip.getProperty (IDs::path).toString();
            auto fx = SampleFxSettings::fromTree (clip);
            if (auto sample = loadProcessedSample (clipPath, fx))
            {
                EngineAudioClip ec;
                ec.sample = sample;
                ec.uid = (int) clip.getProperty (IDs::uid, 0);
                if (ec.uid == 0)
                    ec.uid = -(i + 1);
                ec.playback = SamplePlaybackSettings::fromTree (clip);
                ec.declickMode = juce::jlimit (0, 6,
                    (int) clip.getProperty (IDs::declickMode, 0));
                ec.contentKey = clipPath + "|" + fx.signature()
                              + "|" + ec.playback.signature();
                ec.mixerTrack = clip.hasProperty (IDs::mixerTrack)
                                    ? juce::jlimit (0, Model::maxMixerTracks,
                                                    (int) clip.getProperty (IDs::mixerTrack, 0))
                                    : -1;
                ec.startTick = clipStart;
                ec.endTick = clipEnd;
                ec.offsetTick = offTicks;
                ec.gain = channelGain ((float) (double) clip.getProperty (IDs::volume, 1.0));
                panGains ((float) (double) clip.getProperty (IDs::pan, 0.0), ec.gainL, ec.gainR);
                ec.pitchRatio = std::pow (2.0, (double) clip.getProperty (IDs::pitchSemis, 0.0) / 12.0);

                auto channelsTree = Model::getChannels (project);
                for (int ci = 0; ci < channelsTree.getNumChildren(); ++ci)
                {
                    auto chan = channelsTree.getChild (ci);
                    if (chan.getProperty (IDs::type).toString() == "sampler"
                        && chan.getProperty (IDs::samplePath).toString() == clipPath)
                    {
                        ec.channelId = (int) chan.getProperty (IDs::id);
                        break;
                    }
                }
                newClips.push_back (std::move (ec));
            }
        }
        else if (isPatternClip (clip))
        {
            auto pat = Model::findPatternById (project, (int) clip.getProperty (IDs::patternId));
            if (! pat.isValid())
                continue;
            int clipUid = (int) clip.getProperty (IDs::uid, 0);
            if (clipUid == 0)
                clipUid = legacyClipUid (i);
            compilePattern (pat, clipStart - offTicks, clipStart, clipEnd, newSong, newSongSlides, clipUid);
        }
    }
    if (maxEnd > 0)
        newSongLen = maxEnd;

    auto eventLess = [] (const NoteEvent& a, const NoteEvent& b)
    {
        if (a.tick != b.tick) return a.tick < b.tick;
        return (int) a.noteOn < (int) b.noteOn;
    };
    std::sort (newSong.begin(), newSong.end(), eventLess);
    std::sort (newPattern.begin(), newPattern.end(), eventLess);
    std::sort (newSongSlides.begin(), newSongSlides.end());
    std::sort (newPatternSlides.begin(), newPatternSlides.end());

    auto dropInnerOffs = [] (std::vector<NoteEvent>& events)
    {
        std::map<std::pair<int, int>, int> open;
        std::vector<NoteEvent> filtered;
        filtered.reserve (events.size());
        for (auto& e : events)
        {
            auto key = std::make_pair (e.channelId, e.key);
            if (e.noteOn)
            {
                ++open[key];
                filtered.push_back (e);
            }
            else
            {
                auto it = open.find (key);
                int depth = it != open.end() ? it->second : 0;
                if (depth > 0)
                    it->second = --depth;
                if (depth == 0)
                    filtered.push_back (e);
            }
        }
        events = std::move (filtered);
    };
    dropInnerOffs (newSong);
    dropInnerOffs (newPattern);

    auto sameContent = [] (const EngineAudioClip& a, const EngineAudioClip& b)
    {
        return a.contentKey == b.contentKey && a.startTick == b.startTick
            && a.endTick == b.endTick && a.offsetTick == b.offsetTick
            && a.gain == b.gain && a.gainL == b.gainL && a.gainR == b.gainR
            && a.pitchRatio == b.pitchRatio && a.channelId == b.channelId
            && a.mixerTrack == b.mixerTrack;
    };

    const bool inheritChanged = resyncEditedClips.load();
    bool clipsChanged = newClips.size() != audioClips.size();
    for (auto& nc : newClips)
    {
        const EngineAudioClip* prev = nullptr;
        for (const auto& oc : audioClips)
            if (oc.uid == nc.uid)
            {
                prev = &oc;
                break;
            }
        nc.changed = prev == nullptr || ! sameContent (nc, *prev)
                  || (inheritChanged && prev->changed);
        clipsChanged |= nc.changed;
    }

    std::vector<int> survivingClipUids;
    survivingClipUids.reserve (64);
    for (auto& e : newSong)
        if (e.noteOn && e.clipUid > 0)
            survivingClipUids.push_back (e.clipUid);
    std::sort (survivingClipUids.begin(), survivingClipUids.end());
    survivingClipUids.erase (std::unique (survivingClipUids.begin(), survivingClipUids.end()),
                             survivingClipUids.end());

    {
        const juce::ScopedLock sl (engineLock);

        songEvents.swap (newSong);
        patternEvents.swap (newPattern);
        songSlides.swap (newSongSlides);
        patternSlides.swap (newPatternSlides);
        audioClips.swap (newClips);
        autoLanes.swap (newAutoLanes);
        songLengthTicks = newSongLen;
        patternLengthTicks = newPatternLen;
        for (auto& c : channels)
        {
            if (c == nullptr)
                continue;
            int lastQueued = 0;
            for (int i = 0; i < c->heldSeqNoteCount; ++i)
            {
                const int uid = c->heldSeqNotes[(size_t) i].clipUid;
                if (uid <= 0 || uid == lastQueued
                    || std::binary_search (survivingClipUids.begin(), survivingClipUids.end(), uid))
                    continue;
                lastQueued = uid;
                Audition a;
                a.channelId = c->id; a.key = -1; a.velocity = 0; a.on = false;
                a.forceChoke = true; a.chokeClipUid = uid;
                pendingAuditions.push_back (a);
            }
        }
        if (clipsChanged)
            resyncEditedClips.store (true);
    }
}

void AudioEngine::markDirty (bool chans, bool mixer, bool seqs)
{
    dirtyChannels |= chans;
    dirtyMixer |= mixer;
    dirtySequences |= seqs;
    triggerAsyncUpdate();
}

void AudioEngine::handleAsyncUpdate()
{
    bool c = dirtyChannels, m = dirtyMixer, s = dirtySequences;
    dirtyChannels = dirtyMixer = dirtySequences = false;
    if (c) rebuildChannels();
    if (m) rebuildMixer();
    if (s || c) recompileSequences();
}

void AudioEngine::flushAfterStop()
{
    const juce::ScopedLock fl (stopFlushLock);
    if (offlineRendering.load())
    {
        stopFlushPending.store (false);
        return;
    }

    std::vector<juce::AudioProcessor*> procs;
    {
        const juce::ScopedLock sl (engineLock);
        for (auto& c : channels)
            if (c->plugin != nullptr)
                procs.push_back (c->plugin.get());
        for (auto& t : mixerTracks)
            for (auto& fx : t->effects)
                if (fx->processor != nullptr)
                    procs.push_back (fx->processor.get());
    }
    for (auto* p : procs)
    {
        const juce::ScopedLock pl (p->getCallbackLock());
        p->reset();
    }
    stopFlushPending.store (false);
}

void AudioEngine::valueTreePropertyChanged (juce::ValueTree& t, const juce::Identifier& p)
{
    if (p == IDs::pluginState)
        return;

    if (t.hasType (IDs::Project))
    {
        if (p == IDs::bpm)
        {
            bpm.store ((double) t.getProperty (IDs::bpm, 140.0));
            resyncAudioClips.store (true);
        }
        else if (p == IDs::songMode)
        {
            const bool s = (bool) t.getProperty (IDs::songMode, false);
            if (s != songMode.load())
                setSongMode (s);
        }
        return;
    }

    if (t.hasType (IDs::Channel))
    {
        int id = (int) t.getProperty (IDs::id);
        if (p == IDs::samplePath)
        {
            auto fx = SampleFxSettings::fromTree (t);
            auto sample = loadProcessedSample (t.getProperty (IDs::samplePath).toString(), fx);
            auto sig = fx.signature();
            const juce::ScopedLock sl (engineLock);
            if (auto* c = findChannel (id))
            {
                c->sample = sample;
                c->sampleSig = sig;
            }
        }
        else if (p == IDs::pluginDesc || p == IDs::type)
        {
            markDirty (true, false, true);
        }
        else if (p == IDs::smpStart || p == IDs::smpLength || p == IDs::smpTrim
                 || p == IDs::declickMode || p == IDs::fadeIn || p == IDs::fadeOut
                 || p == IDs::fxRemoveDC || p == IDs::fxNormalize || p == IDs::fxReverse
                 || p == IDs::fxRevPolarity || p == IDs::fxFadeStereo || p == IDs::fxSwapStereo
                 || p == IDs::sampleKeepOnDisk)
        {
            markDirty (true, false, true);
        }
        else if (auto* c = findChannel (id))
        {
            updateChannelAtomics (*c, t);
        }
        return;
    }

    if (t.hasType (IDs::Track))
    {
        int idx = (int) t.getProperty (IDs::index, -1);
        if (juce::isPositiveAndBelow (idx, (int) mixerTracks.size()))
            updateTrackAtomics (*mixerTracks[(size_t) idx], t);
        return;
    }

    if (t.hasType (IDs::Slot))
    {
        if (p == IDs::enabled || p == IDs::mix)
        {
            auto trackTree = t.getParent();
            int ti = (int) trackTree.getProperty (IDs::index, -1);
            if (juce::isPositiveAndBelow (ti, (int) mixerTracks.size()))
            {
                int uid = (int) t.getProperty (IDs::uid);
                const juce::ScopedLock sl (engineLock);
                for (auto& fx : mixerTracks[(size_t) ti]->effects)
                    if (fx->uid == uid)
                    {
                        fx->enabled.store ((bool) t.getProperty (IDs::enabled, true));
                        fx->mix.store (juce::jlimit (0.0f, 1.0f, (float) (double) t.getProperty (IDs::mix, 1.0)));
                    }
            }
        }
        else
            markDirty (false, true, false);
        return;
    }

    if (t.hasType (IDs::Note) || t.hasType (IDs::ChannelSeq)
        || t.hasType (IDs::Pattern) || t.hasType (IDs::Clip)
        || t.hasType (IDs::PlaylistTrack) || t.hasType (IDs::Point))
        markDirty (false, false, true);
}

void AudioEngine::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child)
{
    if (child.hasType (IDs::Channel) || parent.hasType (IDs::Channels))       markDirty (true, false, true);
    else if (child.hasType (IDs::Slot) || child.hasType (IDs::Track))         markDirty (false, true, false);
    else if (child.hasType (IDs::Note) || child.hasType (IDs::ChannelSeq)
             || child.hasType (IDs::Pattern) || child.hasType (IDs::Clip)
             || child.hasType (IDs::PlaylistTrack) || child.hasType (IDs::Point)) markDirty (false, false, true);
    else if (child.hasType (IDs::AudioFile) || child.hasType (IDs::MidiFile)
             || child.hasType (IDs::AudioFiles) || child.hasType (IDs::MidiFiles)) {}
    else                                                                      markDirty (true, true, true);
}

void AudioEngine::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int indexRemoved)
{
    std::vector<Audition> chokes;
    auto queueChoke = [&chokes] (int channelId, int key, int clipUid, bool noteRemoval)
    {
        if (noteRemoval && key < 0)
            return;
        chokes.push_back ({ channelId, key, 0, false, true,
                            clipUid, noteRemoval });
    };

    auto forEachClipOfPattern = [this] (int patId, auto&& fn)
    {
        auto playlist = Model::getPlaylist (project);
        for (int i = 0; i < playlist.getNumChildren(); ++i)
        {
            auto clip = playlist.getChild (i);
            if (! clip.hasType (IDs::Clip)
                || ! isPatternClip (clip)
                || (int) clip.getProperty (IDs::patternId, -1) != patId
                || ! Model::isPlaylistTrackEnabled (project, (int) clip.getProperty (IDs::track)))
                continue;
            int uid = (int) clip.getProperty (IDs::uid, 0);
            if (uid == 0)
                uid = legacyClipUid (i);
            fn (uid);
        }
    };

    auto chokePatternChannels = [&] (const juce::ValueTree& pattern)
    {
        if (! pattern.isValid())
            return;
        const bool song = songMode.load();
        const int patId = (int) pattern.getProperty (IDs::id, -1);
        for (int i = 0; i < pattern.getNumChildren(); ++i)
        {
            auto seq = pattern.getChild (i);
            if (! seq.hasType (IDs::ChannelSeq))
                continue;
            int cid = (int) seq.getProperty (IDs::channelId, -1);
            if (cid < 0)
                continue;
            if (song)
                forEachClipOfPattern (patId, [&] (int uid) { queueChoke (cid, -1, uid, false); });
            else
                queueChoke (cid, -1, -1, false);
        }
    };

    if (child.hasType (IDs::Note))
    {
        int channelId = (int) parent.getProperty (IDs::channelId, -1);
        if (channelId >= 0)
            queueChoke (channelId, (int) child.getProperty (IDs::key, -1), -1, true);
    }
    else if (child.hasType (IDs::ChannelSeq))
    {
        int channelId = (int) child.getProperty (IDs::channelId, -1);
        if (channelId >= 0)
        {
            if (songMode.load())
                forEachClipOfPattern ((int) parent.getProperty (IDs::id, -1),
                                      [&] (int uid) { queueChoke (channelId, -1, uid, false); });
            else if ((int) parent.getProperty (IDs::id, -1) == currentPatternId.load())
                queueChoke (channelId, -1, -1, false);
        }
    }
    else if (playing.load() && child.hasType (IDs::Pattern))
    {
        int patId = (int) child.getProperty (IDs::id, -1);
        bool couldSound = false;
        if (! songMode.load())
        {
            couldSound = (patId == currentPatternId.load());
        }
        else
        {
            double pos = positionForUi.load();
            auto playlist = Model::getPlaylist (project);
            for (int i = 0; i < playlist.getNumChildren() && ! couldSound; ++i)
            {
                auto clip = playlist.getChild (i);
                if (! clip.hasType (IDs::Clip)
                    || ! isPatternClip (clip)
                    || (int) clip.getProperty (IDs::patternId, -1) != patId
                    || ! Model::isPlaylistTrackEnabled (project, (int) clip.getProperty (IDs::track)))
                    continue;
                double s = (double) clip.getProperty (IDs::startBeats) * Model::PPQ;
                double e = s + (double) clip.getProperty (IDs::lengthBeats) * Model::PPQ;
                if (pos >= s && pos < e)
                    couldSound = true;
            }
        }
        if (couldSound)
            chokePatternChannels (child);
    }
    else if (playing.load() && songMode.load() && child.hasType (IDs::Clip)
             && isPatternClip (child))
    {
        double pos = positionForUi.load();
        double s = (double) child.getProperty (IDs::startBeats) * Model::PPQ;
        double e = s + (double) child.getProperty (IDs::lengthBeats) * Model::PPQ;
        if (pos >= s && pos < e)
        {
            int uid = (int) child.getProperty (IDs::uid, 0);
            if (uid == 0)
                uid = legacyClipUid (indexRemoved);
            auto pat = Model::findPatternById (project, (int) child.getProperty (IDs::patternId, -1));
            for (int i = 0; i < pat.getNumChildren(); ++i)
            {
                auto seq = pat.getChild (i);
                if (! seq.hasType (IDs::ChannelSeq))
                    continue;
                int cid = (int) seq.getProperty (IDs::channelId, -1);
                if (cid >= 0)
                    queueChoke (cid, -1, uid, false);
            }
        }
    }

    if (! chokes.empty())
    {
        const juce::ScopedLock sl (engineLock);
        pendingAuditions.insert (pendingAuditions.end(), chokes.begin(), chokes.end());
    }

    valueTreeChildAdded (parent, child);
}

void AudioEngine::valueTreeChildOrderChanged (juce::ValueTree& parent, int, int)
{
    if (parent.hasType (IDs::Channels))
        markDirty (true, false, false);
}

void AudioEngine::valueTreeRedirected (juce::ValueTree&)
{
    bpm.store ((double) project.getProperty (IDs::bpm, 140.0));
    markDirty (true, true, true);
}

void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    const double sr = device->getCurrentSampleRate();
    const int bs = device->getCurrentBufferSizeSamples();

    finalizeAllRecordings();

    deviceInputChannels.store (device->getActiveInputChannels().countNumberOfSetBits());
    inputLatencySamples.store (device->getInputLatencyInSamples());
    outputLatencySamples.store (device->getOutputLatencyInSamples());

    const bool needPrepare = ! juce::approximatelyEqual (sr, preparedSampleRate)
                          || bs > preparedBlockSize;

    {
        const juce::ScopedLock sl (engineLock);
        currentSampleRate = sr;
        currentBlockSize = bs;

        for (auto& t : mixerTracks)
        {
            t->buffer.setSize (2, juce::jmax (currentBlockSize, 16));
            t->bufferDirty = true;
        }
        pluginScratch.setSize (16, juce::jmax (currentBlockSize, 16));

        auto makeClick = [this] (double freq)
        {
            auto d = std::make_shared<SampleData>();
            d->sourceSampleRate = currentSampleRate;
            int n = (int) (currentSampleRate * 0.05);
            d->buffer.setSize (2, n);
            for (int i = 0; i < n; ++i)
            {
                double t = i / currentSampleRate;
                float s = (float) (std::sin (t * freq * juce::MathConstants<double>::twoPi) * std::exp (-t * 90.0));
                d->buffer.setSample (0, i, s * 0.5f);
                d->buffer.setSample (1, i, s * 0.5f);
            }
            return d;
        };
        clickHi = makeClick (1568.0);
        clickLo = makeClick (784.0);
    }

    if (needPrepare)
    {
        for (auto& c : channels)
            if (c->plugin)
                preparePlugin (*c->plugin, true);
        for (auto& t : mixerTracks)
            for (auto& fx : t->effects)
                if (fx->processor)
                    preparePlugin (*fx->processor, false);
        preparedSampleRate = sr;
        preparedBlockSize = bs;
    }
}

void AudioEngine::audioDeviceStopped()
{
    const juce::ScopedLock sl (engineLock);
    currentSampleRate = 0;
}

void AudioEngine::setMulticoreRenderingEnabled (bool on)
{
    multicoreRendering.store (on);
}

double AudioEngine::ticksPerSample (double sr) const
{
    return (bpm.load() / 60.0) * (double) Model::PPQ / sr;
}

juce::AudioBuffer<float>& AudioEngine::trackBufferFor (int trackIndex)
{
    auto& t = *mixerTracks[(size_t) juce::jlimit (0, Model::maxMixerTracks, trackIndex)];
    t.bufferDirty = true;
    return t.buffer;
}

static int declickFadeInSamples (int mode, double sr)
{
    static constexpr double ms[] = { 0.0, 0.0, 0.0, 20.0, 100.0, 200.0, 0.0 };
    return (int) (ms[juce::jlimit (0, 6, mode)] * 0.001 * sr);
}

static int declickReleaseSamples (int mode, double sr)
{
    static constexpr double ms[] = { 10.0, 10.0, 20.0, 20.0, 100.0, 200.0, 0.0 };
    return (int) (ms[juce::jlimit (0, 6, mode)] * 0.001 * sr);
}

void AudioEngine::retireSample (SampleDataPtr& p) noexcept
{
    if (p == nullptr)
        return;

    if (p.use_count() > 1)
    {
        p = nullptr;
        return;
    }

    int start1, size1, start2, size2;
    sampleGraveyardFifo.prepareToWrite (1, start1, size1, start2, size2);
    if (size1 > 0)
    {
        sampleGraveyard[(size_t) start1] = std::move (p);
        sampleGraveyardFifo.finishedWrite (1);
        return;
    }
    p = nullptr;
}

void AudioEngine::drainSampleGraveyard()
{
    int start1, size1, start2, size2;
    sampleGraveyardFifo.prepareToRead (sampleGraveyardCapacity, start1, size1, start2, size2);
    for (int i = 0; i < size1; ++i) sampleGraveyard[(size_t) (start1 + i)] = nullptr;
    for (int i = 0; i < size2; ++i) sampleGraveyard[(size_t) (start2 + i)] = nullptr;
    sampleGraveyardFifo.finishedRead (size1 + size2);
}

AudioEngine::SamplePlaybackSettings AudioEngine::channelPlaybackSettings (
    const EngineChannel& channel) const noexcept
{
    SamplePlaybackSettings s;
    s.cubic = channel.sampleResample.load();
    s.useLoop = channel.useLoopPoints.load();
    s.pingPong = channel.pingPongLoop.load();
    s.startOffset = channel.startOffset.load();
    s.loopStart = channel.loopStart.load();
    s.loopEnd = channel.loopEnd.load();
    s.loopCrossfade = channel.loopCrossfade.load();
    s.stretchMode = channel.stretchMode.load();
    s.stretchPitchSemis = channel.stretchPitchSemis.load();
    s.stretchMultiplier = channel.stretchMultiplier.load();
    s.stretchTime = channel.stretchTime.load();
    s.stretchTempoMode = channel.stretchTempoMode.load();
    s.stretchSourceBpm = channel.stretchSourceBpm.load();
    s.stretchBeats = channel.stretchBeats.load();
    s.formantShift = channel.formantShift.load();
    return s;
}

double AudioEngine::stretchDurationFactor (const SamplePlaybackSettings& settings,
                                           double sourceSeconds, double bpmValue) noexcept
{
    double durationFactor = juce::jlimit (
        0.01, 64.0, (double) settings.stretchMultiplier * settings.stretchTime);
    const double currentBpm = juce::jmax (1.0, bpmValue);
    if ((settings.stretchTempoMode == 1 || settings.stretchTempoMode == 2)
        && settings.stretchSourceBpm > 0.0f)
    {
        durationFactor *= (double) settings.stretchSourceBpm / currentBpm;
    }
    else if (settings.stretchTempoMode == 3 && settings.stretchBeats > 0.0f)
    {
        const double targetSeconds = (double) settings.stretchBeats * 60.0 / currentBpm;
        if (sourceSeconds > 0.0)
            durationFactor *= targetSeconds / sourceSeconds;
    }
    return juce::jlimit (0.01, 256.0, durationFactor);
}

double AudioEngine::sourceSecondsPerBeat (const juce::ValueTree& clipOrChannel,
                                          double sourceSeconds, double bpmValue)
{
    const double beatSeconds = 60.0 / juce::jmax (1.0, bpmValue);
    const double factor = stretchDurationFactor (
        SamplePlaybackSettings::fromTree (clipOrChannel), sourceSeconds, bpmValue);
    if (factor <= 0.0)
        return 0.0;
    return beatSeconds / factor;
}

void AudioEngine::configureVoicePlayback (Voice& voice,
                                          const SamplePlaybackSettings& settings,
                                          double pitchRatio,
                                          double elapsedOutputSeconds,
                                          int declickMode)
{
    if (voice.sample == nullptr || currentSampleRate <= 0.0)
        return;

    const int length = voice.sample->buffer.getNumSamples();
    if (length <= 1)
    {
        voice.active = false;
        return;
    }

    const double maxPosition = (double) (length - 1);
    const double sourceStep = voice.sample->sourceSampleRate / currentSampleRate;
    const double stretchPitch = std::pow (
        2.0, (double) settings.stretchPitchSemis / 12.0);
    const double pitchedStep = juce::jmax (1.0e-8, pitchRatio * stretchPitch);

    const double durationFactor = stretchDurationFactor (
        settings,
        maxPosition / juce::jmax (1.0, voice.sample->sourceSampleRate),
        bpm.load());

    bool granular = settings.stretchMode != 0;
    if (settings.stretchMode == 3)
    {
        const double pitchFactor = pitchedStep / juce::jmax (1.0e-8, sourceStep);
        granular = settings.stretchTempoMode != 0
                || std::abs (durationFactor - 1.0) > 1.0e-4
                || std::abs (pitchFactor - 1.0) > 1.0e-4;
    }

    voice.cubic = settings.cubic;
    voice.granular = granular;
    voice.ratio = granular ? pitchedStep : pitchedStep / durationFactor;
    voice.timeAdvance = granular ? sourceStep / durationFactor : voice.ratio;

    voice.loopStart = juce::jlimit (
        0.0, maxPosition, (double) settings.loopStart * maxPosition);
    voice.loopEnd = juce::jlimit (
        0.0, maxPosition, (double) settings.loopEnd * maxPosition);
    voice.loopEnabled = settings.useLoop && voice.loopEnd - voice.loopStart >= 2.0;
    voice.pingPong = voice.loopEnabled && settings.pingPong;
    const double loopSpan = juce::jmax (0.0, voice.loopEnd - voice.loopStart);
    voice.loopCrossfade = voice.loopEnabled && ! voice.pingPong
        ? juce::jlimit (0.0, loopSpan * 0.5,
                       (double) settings.loopCrossfade * loopSpan * 0.5)
        : 0.0;

    const double basePosition = juce::jlimit (
        0.0, maxPosition, (double) settings.startOffset * maxPosition);
    const double elapsed = juce::jmax (0.0, elapsedOutputSeconds);
    voice.pos = basePosition + elapsed * currentSampleRate * voice.timeAdvance;

    const double formantScale = settings.stretchMode == 2
        ? std::pow (2.0, -(double) settings.formantShift)
        : 1.0;
    voice.formantAmount = settings.stretchMode == 2 ? settings.formantShift : 0.0f;
    voice.formantCoefficient = 1.0f - std::exp (
        -juce::MathConstants<float>::twoPi * 1200.0f
        / (float) juce::jmax (1.0, currentSampleRate));
    voice.formantStateL = voice.formantStateR = 0.0f;
    const double baseGrain = 0.0464 * currentSampleRate;
    voice.grainLength = granular
        ? juce::jlimit (512, stretchGrainLength,
                       ((int) std::llround (baseGrain * formantScale) + 1) & ~1)
        : 0;
    if (granular)
    {
        const int hop = voice.grainLength / 2;
        voice.grainAge[0] = hop;
        voice.grainPos[0] = voice.pos;
        voice.grainAge[1] = 0;
        voice.grainPos[1] = voice.pos
                          + (double) hop * (voice.timeAdvance - voice.ratio);
    }
    else
    {
        voice.grainAge[0] = voice.grainAge[1] = 0;
        voice.grainPos[0] = voice.grainPos[1] = voice.pos;
    }

    voice.fadeInTotal = declickFadeInSamples (
        juce::jlimit (0, 6, declickMode), currentSampleRate);
}

double AudioEngine::voiceMappedPosition (const Voice& voice, double rawPosition) const noexcept
{
    if (voice.sample == nullptr)
        return 0.0;
    const double maximum = (double) juce::jmax (
        0, voice.sample->buffer.getNumSamples() - 1);
    if (! voice.loopEnabled)
        return juce::jlimit (0.0, maximum, rawPosition);

    const double start = voice.loopStart;
    const double end = voice.loopEnd;
    const double span = end - start;
    if (span < 2.0 || rawPosition < end)
        return juce::jlimit (0.0, maximum, rawPosition);

    auto positiveModulo = [] (double value, double divisor)
    {
        double result = std::fmod (value, divisor);
        return result < 0.0 ? result + divisor : result;
    };

    if (voice.pingPong)
    {
        const double phase = positiveModulo (rawPosition - start, span * 2.0);
        return phase <= span ? start + phase : end - (phase - span);
    }

    const double cycleStart = start + voice.loopCrossfade;
    const double cycleLength = end - cycleStart;
    if (cycleLength <= 1.0)
        return start;
    return cycleStart + positiveModulo (rawPosition - end, cycleLength);
}

void AudioEngine::sampleVoiceAt (const Voice& voice, double rawPosition,
                                 float& outL, float& outR) const noexcept
{
    outL = outR = 0.0f;
    if (voice.sample == nullptr || voice.sample->buffer.getNumSamples() <= 0)
        return;

    const auto& buffer = voice.sample->buffer;
    const auto* sourceL = buffer.getReadPointer (0);
    const auto* sourceR = buffer.getReadPointer (buffer.getNumChannels() > 1 ? 1 : 0);
    const int length = buffer.getNumSamples();

    const bool   highQuality = voice.cubic || offlineRendering.load();
    const double aaRatio     = juce::jmax (1.0, voice.ratio);
    const int    radius      = juce::jlimit (3, 96,
                                 (int) std::ceil ((double) (highQuality ? 22 : 12) * aaRatio));
    const double invAA       = 1.0 / aaRatio;

    auto interpolate = [&] (double timelinePosition, float& l, float& r)
    {
        const double base     = std::floor (timelinePosition);
        const double fraction = timelinePosition - base;

        const double pi = juce::MathConstants<double>::pi;
        const double sincStep = pi * invAA;
        const double sincCos = std::cos (sincStep), sincSin = std::sin (sincStep);
        double d = (double) -radius - fraction;
        double sincS = std::sin (sincStep * d), sincC = std::cos (sincStep * d);

        const double winStep = pi / (double) radius;
        const double winCos = std::cos (winStep), winSin = std::sin (winStep);
        const double win2Cos = std::cos (2.0 * winStep), win2Sin = std::sin (2.0 * winStep);
        double w1c = 1.0, w1s = 0.0, w2c = 1.0, w2s = 0.0;

        const int firstIndex = (int) base - radius;
        const int lastIndex  = (int) base + radius;
        const bool direct = ! voice.loopEnabled && firstIndex >= 0 && lastIndex < length;

        double sumL = 0.0, sumR = 0.0, norm = 0.0;
        for (int k = -radius; k <= radius; ++k)
        {
            const double window = 0.42 - 0.5 * w1c + 0.08 * w2c;
            const double h = std::abs (d) < 1.0e-9 ? invAA : sincS / (pi * d);
            const double c = h * window;
            int index = firstIndex + k + radius;
            if (! direct)
                index = juce::jlimit (0, length - 1,
                    (int) std::llround (voiceMappedPosition (voice, base + (double) k)));
            sumL += c * sourceL[index];
            sumR += c * sourceR[index];
            norm += c;

            d += 1.0;
            const double ns = sincS * sincCos + sincC * sincSin;
            sincC = sincC * sincCos - sincS * sincSin;
            sincS = ns;
            const double n1 = w1s * winCos + w1c * winSin;
            w1c = w1c * winCos - w1s * winSin;
            w1s = n1;
            const double n2 = w2s * win2Cos + w2c * win2Sin;
            w2c = w2c * win2Cos - w2s * win2Sin;
            w2s = n2;
        }
        const double scale = norm > 1.0e-12 ? 1.0 / norm : 1.0;
        l = (float) (sumL * scale);
        r = (float) (sumR * scale);
    };

    const double mapped = voiceMappedPosition (voice, rawPosition);
    if (! voice.loopEnabled || voice.pingPong || voice.loopCrossfade <= 1.0
        || mapped < voice.loopEnd - voice.loopCrossfade)
    {
        interpolate (rawPosition, outL, outR);
        return;
    }

    const double phase = juce::jlimit (
        0.0, 1.0,
        (mapped - (voice.loopEnd - voice.loopCrossfade)) / voice.loopCrossfade);
    const int fadeIndex = juce::jlimit (
        0, stretchGrainLength / 2,
        (int) (phase * (double) (stretchGrainLength / 2) + 0.5));
    const float mix = stretchHann[(size_t) fadeIndex];
    const double head = voice.loopStart
                      + mapped - (voice.loopEnd - voice.loopCrossfade);
    float tailL, tailR, headL, headR;
    interpolate (rawPosition, tailL, tailR);
    interpolate (head, headL, headR);
    const float tailGain = std::sqrt (1.0f - mix), headGain = std::sqrt (mix);
    outL = tailL * tailGain + headL * headGain;
    outR = tailR * tailGain + headR * headGain;
}

double AudioEngine::alignGrainOffset (const Voice& voice, double refPos, double nominalPos) noexcept
{
    if (voice.sample == nullptr || voice.pingPong)
        return 0.0;

    const auto& buffer = voice.sample->buffer;
    const int length = buffer.getNumSamples();
    if (length <= 1 || buffer.getNumChannels() <= 0)
        return 0.0;

    const int hop = voice.grainLength / 2;
    const int range = juce::jmin ((int) ((double) hop * voice.ratio) - 1,
        (int) std::llround (0.010 * voice.sample->sourceSampleRate));
    if (range < 1)
        return 0.0;

    const double cycleStart = voice.loopStart + voice.loopCrossfade;
    const double cycleLength = voice.loopEnd - cycleStart;
    if (voice.loopEnabled && cycleLength <= 1.0)
        return 0.0;

    constexpr int decim = 4;
    const int taps = juce::jlimit (8, 128, hop / (2 * decim));
    const double step = voice.ratio * (double) decim;

    const float* source = buffer.getReadPointer (0);
    const double loopEnd = voice.loopEnd;
    const bool looping = voice.loopEnabled;

    auto mapBase = [&] (double position) noexcept
    {
        if (! looping || position < loopEnd)
            return position;
        double phase = std::fmod (position - loopEnd, cycleLength);
        if (phase < 0.0)
            phase += cycleLength;
        return cycleStart + phase;
    };

    const double refBase = mapBase (refPos);

    auto score = [&] (double candidate, double& out) noexcept
    {
        double a = refBase;
        double b = mapBase (candidate);
        double dot = 0.0, energy = 0.0;
        int inRange = 0;
        for (int i = 0; i < taps; ++i)
        {
            const long long ia = (long long) std::llround (a);
            const long long ib = (long long) std::llround (b);
            const double va = ia >= 0 && ia < (long long) length ? (double) source[(size_t) ia] : 0.0;
            const bool bValid = ib >= 0 && ib < (long long) length;
            const double vb = bValid ? (double) source[(size_t) ib] : 0.0;
            if (bValid)
                ++inRange;
            dot += va * vb;
            energy += vb * vb;
            a += step;
            b += step;
            if (looping)
            {
                if (a >= loopEnd)
                    a -= cycleLength;
                if (b >= loopEnd)
                    b -= cycleLength;
            }
        }
        if (inRange < taps / 2 || energy < 1.0e-12)
            return false;
        out = dot / std::sqrt (energy + 1.0e-9);
        return true;
    };

    int best = 0;
    double bestScore = 0.0;
    if (! score (nominalPos, bestScore))
        return 0.0;

    const int coarseStep = juce::jmax (4, (2 * range) / 256);
    for (int d = -range; d <= range; d += coarseStep)
    {
        double s = 0.0;
        if (score (nominalPos + (double) d, s) && s > bestScore)
        {
            bestScore = s;
            best = d;
        }
    }
    const int fineLow = juce::jmax (-range, best - (coarseStep - 1));
    const int fineHigh = juce::jmin (range, best + (coarseStep - 1));
    for (int d = fineLow; d <= fineHigh; ++d)
    {
        double s = 0.0;
        if (score (nominalPos + (double) d, s) && s > bestScore)
        {
            bestScore = s;
            best = d;
        }
    }

    double result = (double) best;
    if (nominalPos + result < 0.0)
        result = -nominalPos;
    return result;
}

void AudioEngine::triggerSamplerVoice (EngineChannel& ch, int key, int velocity, int sampleOffset, bool audition,
                                       int clipUid)
{
    if (ch.sample == nullptr || ch.sample->buffer.getNumSamples() <= 1 || currentSampleRate <= 0)
        return;

    Voice* v = nullptr;
    for (auto& candidate : voices)
        if (! candidate.active)
        { v = &candidate; break; }
    if (v == nullptr)
        v = &voices[(size_t) (nextSteal++ % maxVoices)];

    if (v->sample != ch.sample)
        retireSample (v->sample);
    v->sample = ch.sample;
    const double pitchRatio = std::pow (
        2.0, ((key - ch.rootNote.load()) + ch.pitchSemis.load()
              + ch.fineTune.load() / 100.0) / 12.0)
        * ch.sample->sourceSampleRate / currentSampleRate;
    configureVoicePlayback (*v, channelPlaybackSettings (ch), pitchRatio, 0.0,
                            ch.declickMode.load());
    v->gain = (float) velocity / 127.0f;
    v->channelId = ch.id;
    v->key = key;
    v->startOffset = sampleOffset;
    v->active = true;
    v->isAudioClip = false;
    v->audition = audition;
    v->clipUid = clipUid;
    v->outSamples = 0;
    v->releaseTotal = 0;
    v->releaseRemaining = -1;
    v->releaseDelay = 0;
    v->slideFactor = 1.0;
    v->slideRemaining = 0;
    v->slideTargetRatio = 1.0;
    v->gainStep = 0.0f;
}

void AudioEngine::holdSeqNote (EngineChannel& ch, int key, int clipUid) noexcept
{
    for (int i = 0; i < ch.heldSeqNoteCount; ++i)
        if (ch.heldSeqNotes[(size_t) i].key == key && ch.heldSeqNotes[(size_t) i].clipUid == clipUid)
            return;
    if (ch.heldSeqNoteCount == (int) ch.heldSeqNotes.size())
    {
        for (int i = 1; i < ch.heldSeqNoteCount; ++i)
            ch.heldSeqNotes[(size_t) (i - 1)] = ch.heldSeqNotes[(size_t) i];
        --ch.heldSeqNoteCount;
    }
    ch.heldSeqNotes[(size_t) ch.heldSeqNoteCount++] = { key, clipUid };
}

void AudioEngine::releaseSeqNote (EngineChannel& ch, int key, int clipUid) noexcept
{
    for (int i = ch.heldSeqNoteCount - 1; i >= 0; --i)
        if (ch.heldSeqNotes[(size_t) i].key == key
            && (clipUid < 0 || ch.heldSeqNotes[(size_t) i].clipUid == clipUid))
        {
            ch.heldSeqNotes[(size_t) i] = ch.heldSeqNotes[(size_t) --ch.heldSeqNoteCount];
            if (clipUid >= 0)
                return;
        }
}

void AudioEngine::clearHeldSeqNotes() noexcept
{
    for (auto& c : channels)
        if (c != nullptr)
            c->heldSeqNoteCount = 0;
}

void AudioEngine::chokeSamplerVoices (int channelId, int key, bool auditionOnly, int clipUid)
{
    auto* ch = findChannel (channelId);
    int rel = declickReleaseSamples (ch != nullptr ? ch->declickMode.load() : 0, currentSampleRate);

    for (auto& v : voices)
        if (v.active && v.channelId == channelId && (key < 0 || v.key == key)
            && (! auditionOnly || v.audition)
            && (clipUid < 0 || v.clipUid == clipUid))
        {
            if (rel <= 0)
                v.active = false;
            else if (v.releaseRemaining < 0 || v.releaseRemaining > rel)
            {
                v.releaseTotal = rel;
                v.releaseRemaining = rel;
            }
        }
}

const AudioEngine::EngineAudioClip* AudioEngine::findAudioClip (int uid) const noexcept
{
    for (auto& clip : audioClips)
        if (clip.uid == uid)
            return &clip;
    return nullptr;
}

void AudioEngine::seatVoiceInClip (Voice& v, const EngineAudioClip& clip, double tick)
{
    double secondsIn = (tick - (double) clip.startTick + (double) clip.offsetTick)
                       / (double) Model::PPQ * 60.0 / juce::jmax (1.0, bpm.load());
    const double pitchRatio = clip.sample->sourceSampleRate
                            / currentSampleRate * clip.pitchRatio;
    configureVoicePlayback (
        v, clip.playback, pitchRatio, secondsIn, clip.declickMode);
    v.gain = clip.gain;
    v.gainL = clip.gainL;
    v.gainR = clip.gainR;
    v.channelId = clip.channelId;
    v.mixerTrack = clip.mixerTrack;
    v.clipUid = clip.uid;
    v.clipEndTick = clip.endTick;
    v.active = v.loopEnabled
            || v.pos <= (double) juce::jmax (0, clip.sample->buffer.getNumSamples() - 1);
}

void AudioEngine::pushAudioClipVoice (Voice& v) noexcept
{
    if (audioClipVoices.size() < (size_t) maxAudioClipVoices)
    {
        audioClipVoices.push_back (std::move (v));
        return;
    }

    audioPerfStats.droppedClipVoices.fetch_add (1, std::memory_order_relaxed);
    retireSample (v.sample);
}

void AudioEngine::resyncAudioClipVoices (bool fullResync, double tps)
{
    const int fade = juce::jmax (1, (int) (0.010 * currentSampleRate));

    for (auto& v : audioClipVoices)
    {
        if (! v.active)
            continue;

        if (! fullResync)
        {
            if (v.clipUid == 0)
                continue;

            const auto* clip = findAudioClip (v.clipUid);
            if (clip != nullptr && ! clip->changed)
                continue;

            if (clip != nullptr && v.releaseRemaining < 0 && v.sample == clip->sample
                && v.outSamples < (juce::int64) v.fadeInTotal
                && positionTicks >= (double) clip->startTick
                && positionTicks < (double) clip->endTick)
            {
                seatVoiceInClip (v, *clip, positionTicks);
                continue;
            }
        }

        if (v.releaseRemaining < 0 || v.releaseRemaining > fade)
        {
            v.releaseTotal = fade;
            v.releaseRemaining = fade;
        }
    }

    const size_t before = audioClipVoices.size();
    startAudioClipsAtPosition (positionTicks, tps, 0, ! fullResync);
    for (size_t i = before; i < audioClipVoices.size(); ++i)
        audioClipVoices[i].fadeInTotal = juce::jmax (audioClipVoices[i].fadeInTotal, fade);
}

void AudioEngine::startAudioClipsAtPosition (double tick, double, int sampleOffset, bool changedOnly)
{
    for (auto& clip : audioClips)
    {
        if (clip.sample == nullptr || tick < (double) clip.startTick || tick >= (double) clip.endTick)
            continue;

        if (changedOnly)
        {
            if (! clip.changed)
                continue;
            bool alreadyVoiced = false;
            for (auto& v : audioClipVoices)
                if (v.active && v.clipUid == clip.uid && v.releaseRemaining < 0)
                {
                    alreadyVoiced = true;
                    break;
                }
            if (alreadyVoiced)
                continue;
        }

        Voice v;
        v.sample = clip.sample;
        seatVoiceInClip (v, clip, tick);
        v.startOffset = sampleOffset;
        v.isAudioClip = true;
        if (v.active)
            pushAudioClipVoice (v);
    }
}

void AudioEngine::processSequencerRange (double startTick, double endTick, int sampleOffsetBase, double tps,
                                         bool includeEndOffs, bool scanFromFloor)
{
    auto& events = songMode.load() ? songEvents : patternEvents;

    double lowBound = scanFromFloor ? std::floor (startTick) : startTick;

    NoteEvent probe {};
    probe.tick = (juce::int64) std::ceil (lowBound - 1.0e-9);
    probe.noteOn = false;

    auto it = std::lower_bound (events.begin(), events.end(), probe,
                                [] (const NoteEvent& a, const NoteEvent& b) { return a.tick < b.tick; });
    for (; it != events.end(); ++it)
    {
        if ((double) it->tick > endTick)
            break;
        if ((double) it->tick == endTick)
        {
            if (! includeEndOffs || it->noteOn)
                break;
        }
        if ((double) it->tick < lowBound)
            continue;

        if (it->noteOn && seekedFromTick >= 0.0
            && (double) it->tick == std::floor (seekedFromTick))
            continue;

        int offset = sampleOffsetBase + juce::jmax (0, (int) (((double) it->tick - startTick) / tps));

        auto* ch = findChannel (it->channelId);
        if (ch == nullptr)
            continue;

        if (ch->isPlugin && ch->plugin != nullptr)
        {
            if (it->noteOn)
            {
                if (ch->bendCurrent != 0.0 || ch->slideActive)
                {
                    ch->midi.addEvent (juce::MidiMessage::pitchWheel (1, 8192), offset);
                    ch->bendCurrent = ch->bendFrom = ch->bendTo = 0.0;
                    ch->slideActive = false;
                }
                ch->lastNoteKey = it->key;
            }
            auto msg = it->noteOn
                ? juce::MidiMessage::noteOn (1, it->key, (juce::uint8) it->velocity)
                : juce::MidiMessage::noteOff (1, it->key);
            ch->midi.addEvent (msg, offset);
            if (it->noteOn)
            {
                holdSeqNote (*ch, it->key, it->clipUid);
                pushNoteOnActivity (ch->id);
            }
            else
                releaseSeqNote (*ch, it->key, it->clipUid);
        }
        else if (! ch->isPlugin)
        {
            if (it->noteOn)
            {
                if (ch->cutItself.load())
                    chokeSamplerVoices (ch->id, -1);
                triggerSamplerVoice (*ch, it->key, it->velocity, offset, false, it->clipUid);
                holdSeqNote (*ch, it->key, it->clipUid);
                pushNoteOnActivity (ch->id);
            }
            else
            {
                releaseSeqNote (*ch, it->key, it->clipUid);
                if (ch->cutItself.load() || ch->useLoopPoints.load())
                    chokeSamplerVoices (ch->id, it->key);
            }
        }
    }

    {
        auto& slides = songMode.load() ? songSlides : patternSlides;
        SlideEvent sprobe {};
        sprobe.tick = (juce::int64) std::ceil (lowBound - 1.0e-9);
        auto sit = std::lower_bound (slides.begin(), slides.end(), sprobe);
        for (; sit != slides.end(); ++sit)
        {
            if ((double) sit->tick >= endTick)
                break;
            if ((double) sit->tick < lowBound)
                continue;

            auto* ch = findChannel (sit->channelId);
            if (ch == nullptr)
                continue;

            if (ch->isPlugin && ch->plugin != nullptr)
            {
                ch->bendFrom = ch->bendCurrent;
                ch->bendTo = juce::jlimit (-1.0, 1.0, (double) (sit->key - ch->lastNoteKey) / 2.0);
                ch->slideStartTick = (double) sit->tick;
                ch->slideEndTick = (double) sit->tick + (double) sit->lengthTicks;
                ch->slideActive = true;
                ch->lastNoteKey = sit->key;
            }
            else if (! ch->isPlugin)
            {
                juce::int64 glideSamples = juce::jmax ((juce::int64) 1,
                                                       (juce::int64) ((double) sit->lengthTicks / tps));
                for (auto& v : voices)
                {
                    if (! v.active || v.audition || v.channelId != ch->id || v.releaseRemaining >= 0
                        || v.sample == nullptr)
                        continue;
                    const double pitched = std::pow (
                        2.0, ((sit->key - ch->rootNote.load())
                              + ch->pitchSemis.load()
                              + ch->fineTune.load() / 100.0) / 12.0)
                        * v.sample->sourceSampleRate / currentSampleRate;
                    Voice probe;
                    probe.sample = v.sample;
                    configureVoicePlayback (
                        probe, channelPlaybackSettings (*ch), pitched, 0.0,
                        ch->declickMode.load());
                    const double targetRatio = probe.ratio;
                    v.slideFactor = std::pow (targetRatio / v.ratio, 1.0 / (double) glideSamples);
                    v.slideRemaining = glideSamples;
                    v.slideTargetRatio = targetRatio;
                    v.gainStep = ((float) sit->velocity / 127.0f - v.gain) / (float) glideSamples;
                    v.key = sit->key;
                }
            }
        }
    }

    if (metronomeOn.load())
    {
        auto firstBeat = (juce::int64) std::ceil (startTick / Model::PPQ - 1.0e-9);
        for (auto beat = firstBeat; (double) beat * Model::PPQ < endTick; ++beat)
        {
            double beatTick = (double) beat * Model::PPQ;
            if (beatTick < startTick)
                continue;
            Voice v;
            v.sample = (beat % Model::beatsPerBar == 0) ? clickHi : clickLo;
            if (v.sample == nullptr)
                continue;
            v.ratio = 1.0;
            v.gain = 0.8f;
            v.startOffset = sampleOffsetBase + (int) ((beatTick - startTick) / tps);
            v.active = true;
            v.isAudioClip = true;
            v.clipEndTick = (juce::int64) 1 << 60;
            pushAudioClipVoice (v);
        }
    }

    if (songMode.load())
    {
        for (auto& clip : audioClips)
        {
            if (clip.sample == nullptr)
                continue;
            if ((double) clip.startTick >= startTick && (double) clip.startTick < endTick)
            {
                Voice v;
                v.sample = clip.sample;
                seatVoiceInClip (v, clip, (double) clip.startTick);
                v.startOffset = sampleOffsetBase + (int) (((double) clip.startTick - startTick) / tps);
                v.isAudioClip = true;
                if (v.active)
                    pushAudioClipVoice (v);
            }
        }
        for (auto& v : audioClipVoices)
        {
            if (! v.active || v.releaseRemaining >= 0
                || (double) v.clipEndTick < startTick
                || (double) v.clipEndTick > endTick)
                continue;

            const auto* clip = findAudioClip (v.clipUid);
            const int release = declickReleaseSamples (
                clip != nullptr ? clip->declickMode : 0, currentSampleRate);
            const int endOffset = sampleOffsetBase + juce::jmax (
                0, (int) (((double) v.clipEndTick - startTick) / tps));
            v.releaseTotal = release;
            v.releaseRemaining = release;
            v.releaseDelay = juce::jmax (0, endOffset - v.startOffset);
        }
    }
}

void AudioEngine::fireSpanningNotesAt (double posTicks, int sampleOffset)
{
    auto& events = songMode.load() ? songEvents : patternEvents;

    struct Held { int channelId, key, velocity, clipUid; };
    std::vector<Held> held;

    for (const auto& e : events)
    {
        if ((double) e.tick > posTicks)
            break;
        if (e.noteOn)
        {
            if ((double) e.tick < posTicks)
            {
                bool alreadyHeld = false;
                for (const auto& h : held)
                    if (h.channelId == e.channelId && h.key == e.key) { alreadyHeld = true; break; }
                if (! alreadyHeld)
                    held.push_back ({ e.channelId, e.key, e.velocity, e.clipUid });
            }
        }
        else
        {
            for (int i = (int) held.size() - 1; i >= 0; --i)
                if (held[(size_t) i].channelId == e.channelId && held[(size_t) i].key == e.key)
                    held.erase (held.begin() + i);
        }
    }

    for (const auto& h : held)
    {
        auto* ch = findChannel (h.channelId);
        if (ch == nullptr)
            continue;

        if (ch->isPlugin && ch->plugin != nullptr)
        {
            ch->midi.addEvent (juce::MidiMessage::noteOn (1, h.key, (juce::uint8) h.velocity), sampleOffset);
            ch->lastNoteKey = h.key;
        }
        else if (! ch->isPlugin)
        {
            if (ch->cutItself.load())
                chokeSamplerVoices (ch->id, -1);
            triggerSamplerVoice (*ch, h.key, h.velocity, sampleOffset, false, h.clipUid);
        }
        holdSeqNote (*ch, h.key, h.clipUid);
        pushNoteOnActivity (ch->id);
    }
}

void AudioEngine::mixVoiceInto (float* l, float* r, int numSamples, Voice& v,
                                float gain, float gl, float gr)
{
    auto& buf = v.sample->buffer;
    const int len = buf.getNumSamples();

    for (int i = juce::jlimit (0, numSamples, v.startOffset); i < numSamples; ++i)
    {
        if (! v.loopEnabled && v.pos > (double) (len - 1))
        {
            v.active = false;
            break;
        }

        if (v.slideRemaining > 0)
        {
            v.ratio *= v.slideFactor;
            v.gain += v.gainStep;
            if (--v.slideRemaining == 0)
                v.ratio = v.slideTargetRatio;
        }

        float env = 1.0f;
        if (v.outSamples < (juce::int64) v.fadeInTotal)
        {
            const float phase = (float) (v.outSamples + 1)
                              / (float) juce::jmax (1, v.fadeInTotal);
            const int fadeIndex = juce::jlimit (
                0, stretchGrainLength / 2,
                (int) (phase * (float) (stretchGrainLength / 2) + 0.5f));
            env = stretchHann[(size_t) fadeIndex];
        }
        if (v.releaseRemaining >= 0)
        {
            if (v.releaseDelay > 0)
            {
                --v.releaseDelay;
            }
            else if (v.releaseRemaining == 0)
            {
                v.active = false;
                break;
            }
            else
            {
                const float phase = (float) v.releaseRemaining
                                  / (float) juce::jmax (1, v.releaseTotal);
                const int fadeIndex = juce::jlimit (
                    0, stretchGrainLength / 2,
                    (int) (phase * (float) (stretchGrainLength / 2) + 0.5f));
                env *= stretchHann[(size_t) fadeIndex];
                --v.releaseRemaining;
            }
        }

        float sampleL = 0.0f;
        float sampleR = 0.0f;
        if (v.granular && std::abs (v.ratio - v.timeAdvance) > 1.0e-8)
        {
            float weightSum = 0.0f;
            for (int grain = 0; grain < 2; ++grain)
            {
                const int phase = juce::jlimit (
                    0, v.grainLength - 1, v.grainAge[grain]);
                const int windowIndex = juce::jlimit (
                    0, stretchGrainLength - 1,
                    phase * stretchGrainLength / v.grainLength);
                const float weight = stretchHann[(size_t) windowIndex];
                if (weight > 1.0e-7f)
                {
                    const double gp = v.grainPos[grain];
                    if (v.loopEnabled
                        || (gp > -2.0 && gp < (double) (len - 1) + 2.0))
                    {
                        float gl, gr;
                        sampleVoiceAt (v, gp, gl, gr);
                        sampleL += gl * weight;
                        sampleR += gr * weight;
                        weightSum += weight;
                    }
                }
            }
            if (weightSum > 1.0e-7f)
            {
                sampleL /= weightSum;
                sampleR /= weightSum;
            }
        }
        else
        {
            sampleVoiceAt (v, v.pos, sampleL, sampleR);
        }

        if (std::abs (v.formantAmount) > 1.0e-4f)
        {
            v.formantStateL += v.formantCoefficient * (sampleL - v.formantStateL);
            v.formantStateR += v.formantCoefficient * (sampleR - v.formantStateR);
            if (v.formantAmount > 0.0f)
            {
                sampleL += (sampleL - v.formantStateL) * v.formantAmount * 0.75f;
                sampleR += (sampleR - v.formantStateR) * v.formantAmount * 0.75f;
            }
            else
            {
                const float blend = -v.formantAmount * 0.75f;
                sampleL += (v.formantStateL - sampleL) * blend;
                sampleR += (v.formantStateR - sampleR) * blend;
            }
        }

        l[i] += sampleL * gain * env * gl;
        r[i] += sampleR * gain * env * gr;

        if (v.granular)
        {
            v.pos += v.timeAdvance;
            const int hop = v.grainLength / 2;
            const bool aligning = std::abs (v.ratio - v.timeAdvance) > 1.0e-8;
            for (int grain = 0; grain < 2; ++grain)
                v.grainPos[grain] += v.ratio;
            for (int grain = 0; grain < 2; ++grain)
            {
                if (++v.grainAge[grain] >= v.grainLength)
                {
                    v.grainAge[grain] = 0;
                    double seat = v.pos + (double) hop * (v.timeAdvance - v.ratio);
                    if (aligning)
                        seat += alignGrainOffset (v, v.grainPos[1 - grain], seat);
                    v.grainPos[grain] = seat;
                }
            }
        }
        else
        {
            v.pos += v.ratio;
        }
        ++v.outSamples;
    }
    v.startOffset = 0;
}

float AudioEngine::effVolume (const EngineChannel& c) const
{
    float a = c.autoVolume.load();
    return a >= 0.0f ? a : c.volume.load();
}
float AudioEngine::effPan (const EngineChannel& c) const
{
    float a = c.autoPan.load();
    return a >= -1.5f ? a : c.pan.load();
}
float AudioEngine::effVolume (const EngineMixerTrack& t) const
{
    float a = t.autoVolume.load();
    return a >= 0.0f ? a : t.volume.load();
}
float AudioEngine::effPan (const EngineMixerTrack& t) const
{
    float a = t.autoPan.load();
    return a >= -1.5f ? a : t.pan.load();
}

void AudioEngine::applyAutomation (double posTicks)
{
    auto clearAll = [this]
    {
        for (auto& c : channels) { c->autoVolume.store (-1.0f); c->autoPan.store (-2.0f); }
        for (auto& t : mixerTracks) { t->autoVolume.store (-1.0f); t->autoPan.store (-2.0f); }
    };

    if (! songMode.load())
    {
        if (autoOverridesActive)
        {
            clearAll();
            autoOverridesActive = false;
        }
        return;
    }

    clearAll();

    for (auto& lane : autoLanes)
    {
        if (posTicks < (double) lane.startTick || posTicks >= (double) lane.endTick
            || lane.points.empty())
            continue;

        double p = posTicks - (double) lane.startTick + (double) lane.offsetTick;

        double val;
        const auto& pts = lane.points;
        if (p <= pts.front().posTicks)
            val = pts.front().val;
        else if (p >= pts.back().posTicks)
            val = pts.back().val;
        else
        {
            size_t hi = 1;
            while (hi < pts.size() && p >= pts[hi].posTicks)
                ++hi;
            const auto& a = pts[hi - 1];
            const auto& b = pts[hi];
            double span = b.posTicks - a.posTicks;
            double x = span > 1.0e-9 ? (p - a.posTicks) / span : 0.0;
            val = a.val + (b.val - a.val) * Automation::shape (x, b.mode, b.tension);
        }

        float denorm = Automation::denormalize (lane.target, val);
        switch (lane.target.kind)
        {
            case Automation::Target::trackVolume:
                if (juce::isPositiveAndBelow (lane.target.index, (int) mixerTracks.size()))
                    mixerTracks[(size_t) lane.target.index]->autoVolume.store (denorm);
                break;
            case Automation::Target::trackPan:
                if (juce::isPositiveAndBelow (lane.target.index, (int) mixerTracks.size()))
                    mixerTracks[(size_t) lane.target.index]->autoPan.store (denorm);
                break;
            case Automation::Target::channelVolume:
                if (auto* c = findChannel (lane.target.index)) c->autoVolume.store (denorm);
                break;
            case Automation::Target::channelPan:
                if (auto* c = findChannel (lane.target.index)) c->autoPan.store (denorm);
                break;
            default: break;
        }
    }
    autoOverridesActive = true;
}

float AudioEngine::getTrackAutoVolume (int trackIndex) const
{
    if (! juce::isPositiveAndBelow (trackIndex, (int) mixerTracks.size()))
        return -1.0f;
    return mixerTracks[(size_t) trackIndex]->autoVolume.load();
}

float AudioEngine::getTrackAutoPan (int trackIndex) const
{
    if (! juce::isPositiveAndBelow (trackIndex, (int) mixerTracks.size()))
        return -2.0f;
    return mixerTracks[(size_t) trackIndex]->autoPan.load();
}

void AudioEngine::renderVoices (int numSamples)
{
    for (auto& v : voices)
    {
        if (! v.active || v.sample == nullptr)
            continue;
        auto* ch = findChannel (v.channelId);
        if (ch == nullptr)
        {
            v.active = false;
            continue;
        }
        float vol = ch->mute.load() ? 0.0f : channelGain (effVolume (*ch));
        float gl, gr;
        panGains (effPan (*ch), gl, gr);
        auto& target = trackBufferFor (ch->mixerTrack.load());
        mixVoiceInto (target.getWritePointer (0), target.getWritePointer (1), numSamples,
                      v, v.gain * vol, gl, gr);
    }

    auto& master = trackBufferFor (0);
    for (auto& v : audioClipVoices)
    {
        if (! v.active || v.sample == nullptr)
            continue;

        auto* dst = &master;
        float gain = v.gain;
        if (v.mixerTrack >= 0)
        {
            dst = &trackBufferFor (v.mixerTrack);
        }
        else if (v.channelId >= 0)
            if (auto* ch = findChannel (v.channelId))
            {
                dst = &trackBufferFor (ch->mixerTrack.load());
                if (ch->mute.load())
                    gain = 0.0f;
            }

        mixVoiceInto (dst->getWritePointer (0), dst->getWritePointer (1), numSamples,
                      v, gain, v.gainL, v.gainR);
    }
    for (auto& v : audioClipVoices)
        if (! v.active)
            retireSample (v.sample);
    audioClipVoices.erase (std::remove_if (audioClipVoices.begin(), audioClipVoices.end(),
                                           [] (const Voice& v) { return ! v.active; }),
                           audioClipVoices.end());
}

void AudioEngine::renderChannelBody (EngineChannel& ch, int numSamples)
{
    auto& plugin = *ch.plugin;
    juce::AudioBuffer<float> sub (ch.scratch.getArrayOfWritePointers(), ch.renderChans, 0, numSamples);
    sub.clear();

    const juce::int64 profileStart = renderProfile ? juce::Time::getHighResolutionTicks() : 0;
    bool processed = false;
    {
        const juce::ScopedLock pl (plugin.getCallbackLock());
        if (! plugin.isSuspended())
        {
            plugin.processBlock (sub, ch.midi);
            processed = true;
        }
    }
    if (renderProfile)
    {
        const auto elapsed = juce::Time::getHighResolutionTicks() - profileStart;
        ch.perf.publish (processed && renderBudgetTicks > 0.0
                            ? (float) ((double) elapsed / renderBudgetTicks) : 0.0f);
    }
    ch.midi.clear();

    if (! renderSmart)
    {
        ch.renderSilent = false;
        return;
    }

    const int rightCh = ch.renderChans > 1 ? 1 : 0;
    const bool outSilent = spanIsSilent (sub.getReadPointer (0), numSamples)
                        && (rightCh == 0 || spanIsSilent (sub.getReadPointer (1), numSamples));
    if (! ch.renderHadMidi && outSilent)
    {
        if (++ch.silentBlocks > renderTailBlocks)
            ch.asleep = true;
    }
    else
    {
        ch.silentBlocks = 0;
        ch.asleep = false;
    }
    ch.renderSilent = outSilent;
}

void AudioEngine::renderChannels (int numSamples)
{
    renderProfile = pluginPerfMonitoringEnabled.load (std::memory_order_acquire);
    renderBudgetTicks = renderProfile && currentSampleRate > 0.0
                      ? (double) juce::Time::getHighResolutionTicksPerSecond()
                          * (double) numSamples / currentSampleRate
                      : 0.0;
    renderSmart = smartDisable.load (std::memory_order_relaxed);
    renderTailBlocks = (int) (2.0 * currentSampleRate / (double) juce::jmax (1, numSamples));
    renderNumSamples = numSamples;

    renderList.clear();
    for (auto& ch : channels)
    {
        if (! ch->isPlugin || ch->plugin == nullptr)
        {
            if (renderProfile && ch->isPlugin)
                ch->perf.publish (0.0f);
            ch->midi.clear();
            continue;
        }

        const bool haveMidi = ! ch->midi.isEmpty();
        if (renderSmart && ch->asleep && ! haveMidi)
        {
            if (renderProfile)
                ch->perf.publish (0.0f);
            ch->midi.clear();
            continue;
        }

        auto& plugin = *ch->plugin;
        ch->renderChans = juce::jmax (2, plugin.getTotalNumInputChannels(),
                                         plugin.getTotalNumOutputChannels());
        if (ch->scratch.getNumChannels() < ch->renderChans || ch->scratch.getNumSamples() < numSamples)
            ch->scratch.setSize (juce::jmax (ch->renderChans, ch->scratch.getNumChannels()),
                                 juce::jmax (numSamples, ch->scratch.getNumSamples()), false, false, true);

        ch->renderHadMidi = haveMidi;
        ch->renderSilent = true;
        renderList.push_back (ch.get());
    }

    if (renderList.empty())
        return;

    if (renderList.size() > 1 && renderPool != nullptr && renderPool->hasWorkers()
        && multicoreRendering.load (std::memory_order_relaxed))
        renderPool->run ((int) renderList.size());
    else
        for (auto* ch : renderList)
            renderChannelBody (*ch, numSamples);

    for (auto* ch : renderList)
    {
        if (ch->renderSilent)
            continue;

        float vol = ch->mute.load() ? 0.0f : channelGain (effVolume (*ch));
        float gl, gr;
        panGains (effPan (*ch), gl, gr);
        gl *= vol;
        gr *= vol;

        auto& target = trackBufferFor (ch->mixerTrack.load());
        target.addFrom (0, 0, ch->scratch, 0, 0, numSamples, gl);
        target.addFrom (1, 0, ch->scratch, ch->renderChans > 1 ? 1 : 0, 0, numSamples, gr);
    }
}

void AudioEngine::processMixer (float* const* out, int numOuts, int numSamples)
{
    juce::MidiBuffer emptyMidi;
    const bool profilePlugins = pluginPerfMonitoringEnabled.load (std::memory_order_acquire);
    const double callbackBudgetTicks = profilePlugins && currentSampleRate > 0.0
                                     ? (double) juce::Time::getHighResolutionTicksPerSecond()
                                         * (double) numSamples / currentSampleRate
                                     : 0.0;

    const bool smart = smartDisable.load (std::memory_order_relaxed);

    auto processChain = [&] (EngineMixerTrack& track)
    {
        bool silenceKnown = false, inSilent = false;

        for (auto& fx : track.effects)
        {
            if (fx->processor == nullptr || ! fx->enabled.load())
            {
                if (profilePlugins && fx->isPlugin)
                    fx->perf.publish (0.0f);
                continue;
            }
            auto& proc = *fx->processor;
            int chans = juce::jmax (2, proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());

            if (smart)
            {
                if (! silenceKnown)
                {
                    inSilent = bufferIsSilent (track.buffer, numSamples);
                    silenceKnown = true;
                }
                if (inSilent && fx->asleep)
                {
                    if (profilePlugins && fx->isPlugin)
                        fx->perf.publish (0.0f);
                    continue;
                }
            }

            const float wet = juce::jlimit (0.0f, 1.0f, fx->mix.load());
            if (wet < 0.999f)
            {
                if (dryScratch.getNumChannels() < 2 || dryScratch.getNumSamples() < numSamples)
                    dryScratch.setSize (2, juce::jmax (numSamples, dryScratch.getNumSamples()), false, false, true);
                dryScratch.copyFrom (0, 0, track.buffer, 0, 0, numSamples);
                dryScratch.copyFrom (1, 0, track.buffer, 1, 0, numSamples);
            }

            const bool profileThis = profilePlugins && fx->isPlugin;
            const juce::int64 profileStart = profileThis ? juce::Time::getHighResolutionTicks() : 0;
            bool processed = false;
            if (chans <= 2)
            {
                juce::AudioBuffer<float> sub (track.buffer.getArrayOfWritePointers(), 2, 0, numSamples);
                const juce::ScopedLock pl (proc.getCallbackLock());
                if (! proc.isSuspended())
                {
                    proc.processBlock (sub, emptyMidi);
                    processed = true;
                }
            }
            else
            {
                if (pluginScratch.getNumChannels() < chans || pluginScratch.getNumSamples() < numSamples)
                    pluginScratch.setSize (juce::jmax (chans, pluginScratch.getNumChannels()),
                                           juce::jmax (numSamples, pluginScratch.getNumSamples()), false, false, true);
                juce::AudioBuffer<float> sub (pluginScratch.getArrayOfWritePointers(), chans, 0, numSamples);
                sub.clear();
                sub.copyFrom (0, 0, track.buffer, 0, 0, numSamples);
                sub.copyFrom (1, 0, track.buffer, 1, 0, numSamples);
                {
                    const juce::ScopedLock pl (proc.getCallbackLock());
                    if (! proc.isSuspended())
                    {
                        proc.processBlock (sub, emptyMidi);
                        processed = true;
                    }
                }
                track.buffer.copyFrom (0, 0, sub, 0, 0, numSamples);
                track.buffer.copyFrom (1, 0, sub, 1, 0, numSamples);
            }
            if (profileThis)
            {
                const auto elapsed = juce::Time::getHighResolutionTicks() - profileStart;
                fx->perf.publish (processed && callbackBudgetTicks > 0.0
                                    ? (float) ((double) elapsed / callbackBudgetTicks) : 0.0f);
            }

            if (wet < 0.999f)
            {
                track.buffer.applyGain (0, numSamples, wet);
                track.buffer.addFrom (0, 0, dryScratch, 0, 0, numSamples, 1.0f - wet);
                track.buffer.addFrom (1, 0, dryScratch, 1, 0, numSamples, 1.0f - wet);
            }
            emptyMidi.clear();

            if (smart)
            {
                const bool outSilent = bufferIsSilent (track.buffer, numSamples);
                if (inSilent && outSilent)
                {
                    int tailBlocks = (int) (2.0 * currentSampleRate / (double) juce::jmax (1, numSamples));
                    if (++fx->silentBlocks > tailBlocks)
                        fx->asleep = true;
                }
                else
                {
                    fx->silentBlocks = 0;
                    fx->asleep = false;
                }
                inSilent = outSilent;
            }
        }
    };

    auto& master = *mixerTracks[0];
    master.bufferDirty = true;

    auto applyStereoSep = [] (juce::AudioBuffer<float>& buf, int n, float sep)
    {
        if (std::abs (sep) < 0.001f || buf.getNumChannels() < 2)
            return;
        auto* L = buf.getWritePointer (0);
        auto* R = buf.getWritePointer (1);
        for (int s = 0; s < n; ++s)
        {
            float mid  = (L[s] + R[s]) * 0.5f;
            float side = (L[s] - R[s]) * 0.5f;
            if (sep > 0.0f) side *= (1.0f - sep);
            else            mid  *= (1.0f + sep);
            L[s] = mid + side;
            R[s] = mid - side;
        }
    };

    for (int i = 1; i <= Model::maxMixerTracks; ++i)
    {
        auto& track = *mixerTracks[(size_t) i];

        if (! track.bufferDirty)
        {
            bool anyAwake = false;
            for (auto& fx : track.effects)
                if (fx->processor != nullptr && fx->enabled.load() && ! (smart && fx->asleep))
                { anyAwake = true; break; }

            if (! anyAwake)
            {
                const auto decayed = track.peak.load() * 0.92f;
                track.peak.store (decayed < 1.0e-6f ? 0.0f : decayed);
                continue;
            }
            track.bufferDirty = true;
        }

        processChain (track);
        applyStereoSep (track.buffer, numSamples, track.stereoSep.load());

        bool muted = track.mute.load();
        float trackVol = faderGain (effVolume (track));
        float vol = muted ? 0.0f : trackVol;
        float pan = effPan (track);
        float gl = (pan <= 0 ? 1.0f : 1.0f - pan) * vol;
        float gr = (pan >= 0 ? 1.0f : 1.0f + pan) * vol;

        float mag = track.buffer.getMagnitude (0, numSamples) * trackVol;
        auto prevPeak = track.peak.load();
        track.peak.store (juce::jmax (mag, prevPeak * 0.92f));

        int dest = track.routeTo.load();
        if (dest != -1)
        {
            if (dest <= i || dest > Model::maxMixerTracks)
                dest = 0;
            auto& destTrack = *mixerTracks[(size_t) dest];
            destTrack.bufferDirty = true;
            destTrack.buffer.addFrom (0, 0, track.buffer, 0, 0, numSamples, gl);
            destTrack.buffer.addFrom (1, 0, track.buffer, 1, 0, numSamples, gr);
        }
    }

    processChain (master);
    applyStereoSep (master.buffer, numSamples, master.stereoSep.load());
    float masterVol = faderGain (effVolume (master));
    float mvol = master.mute.load() ? 0.0f : masterVol;
    if (! offlineRendering.load())
        mvol *= (float) playbackVolume.load();

    float mag = master.buffer.getMagnitude (0, numSamples) * masterVol;
    auto prevPeak = master.peak.load();
    master.peak.store (juce::jmax (mag, prevPeak * 0.92f));

    master.buffer.applyGain (0, numSamples, mvol);

    if (! DebugFlags::disableSpectrum.load (std::memory_order_relaxed))
    {
        auto* L = master.buffer.getReadPointer (0);
        auto* R = master.buffer.getNumChannels() > 1 ? master.buffer.getReadPointer (1) : L;
        int wp = spectrumWritePos.load (std::memory_order_relaxed);
        for (int i = 0; i < numSamples; ++i)
        {
            spectrumSnapshot[wp] = (L[i] + R[i]) * 0.5f;
            wp = (wp + 1) & (spectrumSize - 1);
        }
        spectrumWritePos.store (wp, std::memory_order_release);
    }

    if (numOuts >= 1)
        juce::FloatVectorOperations::add (out[0], master.buffer.getReadPointer (0), numSamples);
    if (numOuts >= 2)
        juce::FloatVectorOperations::add (out[1], master.buffer.getReadPointer (1), numSamples);
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* in, int numIns,
                                                    float* const* out, int numOuts, int numSamples,
                                                    const juce::AudioIODeviceCallbackContext&)
{
    struct CallbackTimer
    {
        AudioEngine::AudioPerfStats& stats;
        const juce::int64 entry = juce::Time::getHighResolutionTicks();
        juce::int64 lockWaitTicks = 0;
        int numSamples = 0;
        double sampleRate = 0.0;
        bool publish = false;

        ~CallbackTimer() noexcept
        {
            if (! publish || sampleRate <= 0.0)
                return;
            const double perTick = 1.0 / (double) juce::Time::getHighResolutionTicksPerSecond();
            const double total    = (double) (juce::Time::getHighResolutionTicks() - entry) * perTick;
            const double lockWait = (double) lockWaitTicks * perTick;
            stats.publish (total, lockWait, numSamples / sampleRate, numSamples, sampleRate);
        }
    } perfTimer { audioPerfStats };

    for (int i = 0; i < numOuts; ++i)
        if (out[i] != nullptr)
            juce::FloatVectorOperations::clear (out[i], numSamples);

    if (offlineRendering.load())
        return;

    const juce::int64 lockWaitStart = juce::Time::getHighResolutionTicks();
    const juce::ScopedLock sl (engineLock);
    perfTimer.lockWaitTicks = juce::Time::getHighResolutionTicks() - lockWaitStart;
    perfTimer.numSamples = numSamples;
    perfTimer.sampleRate = currentSampleRate;
    perfTimer.publish = true;
    if (currentSampleRate <= 0 || numOuts < 1)
        return;

    for (auto& t : mixerTracks)
    {
        if (t->buffer.getNumSamples() < numSamples)
        {
            t->buffer.setSize (2, numSamples, false, false, true);
            t->bufferDirty = true;
        }
        if (t->bufferDirty)
        {
            t->buffer.clear();
            t->bufferDirty = false;
        }
    }

    for (auto& c : channels)
        c->midi.clear();

    bool doStop = stopRequest.exchange (false);
    if (doStop || pauseRequest.exchange (false))
    {
        int rel = juce::jmax (1, (int) (0.010 * currentSampleRate));
        auto beginRelease = [rel] (Voice& v)
        {
            if (v.active && (v.releaseRemaining < 0 || v.releaseRemaining > rel))
            {
                v.releaseTotal = rel;
                v.releaseRemaining = rel;
            }
        };
        clearHeldSeqNotes();
        for (auto& v : voices) beginRelease (v);
        for (auto& v : audioClipVoices) beginRelease (v);
        for (auto& c : channels)
            if (c->isPlugin && c->plugin != nullptr)
            {
                c->midi.addEvent (juce::MidiMessage::allNotesOff (1), 0);
                if (c->slideActive || c->bendCurrent != 0.0)
                    c->midi.addEvent (juce::MidiMessage::pitchWheel (1, 8192), 0);
                c->slideActive = false;
                c->bendCurrent = c->bendFrom = c->bendTo = 0.0;
            }
        if (doStop)
        {
            beginRelease (previewVoice);
            beginRelease (previewTailVoice);
            for (auto& c : channels)
                if (c->isPlugin && c->plugin != nullptr)
                    for (int mch : { 1, auditionMidiChannel })
                    {
                        c->midi.addEvent (juce::MidiMessage::allNotesOff (mch), 0);
                        c->midi.addEvent (juce::MidiMessage::allSoundOff (mch), 0);
                    }
            positionTicks = stopReturnTicks.load();
            spanRefireCountdown = -1;
            stopFlushPending.store (true);
            stopFlusher.triggerAsyncUpdate();
        }
    }

    if (modeSwitchCut.exchange (false))
    {
        int rel = juce::jmax (1, (int) (0.010 * currentSampleRate));
        auto beginRelease = [rel] (Voice& v)
        {
            if (v.active && (v.releaseRemaining < 0 || v.releaseRemaining > rel))
            {
                v.releaseTotal = rel;
                v.releaseRemaining = rel;
            }
        };
        clearHeldSeqNotes();
        for (auto& v : voices) beginRelease (v);
        for (auto& v : audioClipVoices) beginRelease (v);
        for (auto& c : channels)
            if (c->isPlugin && c->plugin != nullptr)
            {
                c->midi.addEvent (juce::MidiMessage::allNotesOff (1), 0);
                if (c->slideActive || c->bendCurrent != 0.0)
                    c->midi.addEvent (juce::MidiMessage::pitchWheel (1, 8192), 0);
                c->slideActive = false;
                c->bendCurrent = c->bendFrom = c->bendTo = 0.0;
            }
        positionTicks = 0;
        spanRefireCountdown = -1;
        positionForUi.store (0.0);
        resyncAudioClips.store (true);
    }

    if (patternSwitchCut.exchange (false))
    {
        clearHeldSeqNotes();
        int rel = juce::jmax (1, (int) (0.010 * currentSampleRate));
        for (auto& v : voices)
            if (v.active && (v.releaseRemaining < 0 || v.releaseRemaining > rel))
            {
                v.releaseTotal = rel;
                v.releaseRemaining = rel;
            }
        for (auto& c : channels)
            if (c->isPlugin && c->plugin != nullptr)
            {
                c->midi.addEvent (juce::MidiMessage::allNotesOff (1), 0);
                if (c->slideActive || c->bendCurrent != 0.0)
                    c->midi.addEvent (juce::MidiMessage::pitchWheel (1, 8192), 0);
                c->slideActive = false;
                c->bendCurrent = c->bendFrom = c->bendTo = 0.0;
            }
    }

    for (auto& a : pendingAuditions)
    {
        if (auto* ch = findChannel (a.channelId))
        {
            if (! a.on && a.forceChoke)
            {
                const bool hosted = ch->isPlugin && ch->plugin != nullptr;

                if (a.noteRemoval)
                {
                    int matches = 0, uid = 0;
                    for (int hi = 0; hi < ch->heldSeqNoteCount; ++hi)
                        if (ch->heldSeqNotes[(size_t) hi].key == a.key)
                        {
                            if (matches == 0)
                                uid = ch->heldSeqNotes[(size_t) hi].clipUid;
                            ++matches;
                        }
                    if (matches > 1)
                        continue;
                    releaseSeqNote (*ch, a.key, uid);
                    if (hosted)
                        ch->midi.addEvent (juce::MidiMessage::noteOff (1, a.key), 0);
                    chokeSamplerVoices (ch->id, a.key, false, uid);
                }
                else if (a.chokeClipUid >= 0)
                {
                    for (int hi = ch->heldSeqNoteCount - 1; hi >= 0; --hi)
                    {
                        const auto held = ch->heldSeqNotes[(size_t) hi];
                        if (held.clipUid != a.chokeClipUid || (a.key >= 0 && held.key != a.key))
                            continue;
                        if (hosted)
                            ch->midi.addEvent (juce::MidiMessage::noteOff (1, held.key), 0);
                        chokeSamplerVoices (ch->id, held.key, false, held.clipUid);
                        ch->heldSeqNotes[(size_t) hi] = ch->heldSeqNotes[(size_t) --ch->heldSeqNoteCount];
                    }
                }
                else
                {
                    if (hosted)
                        ch->midi.addEvent (a.key < 0 ? juce::MidiMessage::allNotesOff (1)
                                                     : juce::MidiMessage::noteOff (1, a.key), 0);
                    if (a.key < 0)
                        ch->heldSeqNoteCount = 0;
                    else
                        releaseSeqNote (*ch, a.key);
                    chokeSamplerVoices (ch->id, a.key);
                }
                continue;
            }

            if (ch->isPlugin && ch->plugin != nullptr)
            {
                if (a.on)
                {
                    const int mchan = playing.load() ? auditionMidiChannel : 1;
                    ch->midi.addEvent (juce::MidiMessage::noteOn (mchan, a.key, (juce::uint8) a.velocity), 0);
                    if (heldAuditionCount < (int) heldAuditions.size())
                        heldAuditions[(size_t) heldAuditionCount++] = { a.channelId, a.key, mchan };
                }
                else
                {
                    int mchan = -1;
                    for (int hi = 0; hi < heldAuditionCount; ++hi)
                        if (heldAuditions[(size_t) hi].channelId == a.channelId
                            && heldAuditions[(size_t) hi].key == a.key)
                        {
                            mchan = heldAuditions[(size_t) hi].midiChan;
                            heldAuditions[(size_t) hi] = heldAuditions[(size_t) --heldAuditionCount];
                            break;
                        }
                    if (mchan > 0)
                        ch->midi.addEvent (juce::MidiMessage::noteOff (mchan, a.key), 0);
                    else
                    {
                        if (! playing.load())
                            ch->midi.addEvent (juce::MidiMessage::noteOff (1, a.key), 0);
                        ch->midi.addEvent (juce::MidiMessage::noteOff (auditionMidiChannel, a.key), 0);
                    }
                }
            }
            else if (a.on)
            {
                if (ch->cutItself.load())
                    chokeSamplerVoices (ch->id, -1, true);
                triggerSamplerVoice (*ch, a.key, a.velocity, 0, true);
            }

            if (a.on)
                pushNoteOnActivity (ch->id);
            else if (ch->cutItself.load() || ch->useLoopPoints.load())
                chokeSamplerVoices (ch->id, a.key, true);
        }
    }
    pendingAuditions.clear();

    double tps = ticksPerSample (currentSampleRate);

    double seek = seekRequest.exchange (-1.0);
    if (seek >= 0)
    {
        positionTicks = seek;
        clearHeldSeqNotes();
        spanRefireCountdown = juce::jmax (1, (int) (0.025 * currentSampleRate));
        int rel = juce::jmax (1, (int) (0.010 * currentSampleRate));
        for (auto& v : voices)
            if (v.active && (v.releaseRemaining < 0 || v.releaseRemaining > rel))
            {
                v.releaseTotal = rel;
                v.releaseRemaining = rel;
            }
        for (auto& c : channels)
            if (c->isPlugin && c->plugin != nullptr)
            {
                c->midi.addEvent (juce::MidiMessage::allNotesOff (1), 0);
                if (c->slideActive || c->bendCurrent != 0.0)
                    c->midi.addEvent (juce::MidiMessage::pitchWheel (1, 8192), 0);
                c->slideActive = false;
                c->bendCurrent = c->bendFrom = c->bendTo = 0.0;
            }
        for (auto& v : audioClipVoices)
            retireSample (v.sample);
        audioClipVoices.clear();
        resyncAudioClips.store (true);
        seekedFromTick = seekFiresNotes.exchange (false) ? -1.0 : seek;
    }

    auto loopLen = songMode.load() ? songLengthTicks : patternLengthTicks;
    if (loopLen <= 0)
        loopLen = Model::PPQ * 4;

    bool nowPlaying = playing.load();
    double blockStartTick = positionTicks;
    if (nowPlaying)
    {
        bool repositioned = false;
        if (positionTicks >= (double) loopLen)
        {
            positionTicks = std::fmod (positionTicks, (double) loopLen);
            repositioned = true;
        }
        blockStartTick = positionTicks;

        if (songMode.load())
        {
            const bool fullResync = resyncAudioClips.exchange (false);
            const bool editedResync = resyncEditedClips.exchange (false);
            if (fullResync || editedResync)
                resyncAudioClipVoices (fullResync, tps);
        }

        if (! wasPlaying)
        {
            spanRefireCountdown = -1;
            fireSpanningNotesAt (positionTicks, 0);
        }
        else if (spanRefireCountdown > 0)
        {
            spanRefireCountdown -= numSamples;
            if (spanRefireCountdown <= 0)
            {
                spanRefireCountdown = -1;
                fireSpanningNotesAt (positionTicks, 0);
            }
        }

        double endTick = positionTicks + numSamples * tps;
        if (endTick < (double) loopLen)
        {
            processSequencerRange (positionTicks, endTick, 0, tps, false,
 repositioned);
            positionTicks = endTick;
        }
        else
        {
            int splitSamples = juce::jlimit (0, numSamples, (int) (((double) loopLen - positionTicks) / tps));
            double remainder = endTick - (double) loopLen;

            processSequencerRange (positionTicks, (double) loopLen, 0, tps, true,
 repositioned);

            const int wrapFade = juce::jmax (1, (int) (0.010 * currentSampleRate));
            for (auto& v : audioClipVoices)
            {
                if (! v.active)
                    continue;
                if (v.releaseRemaining < 0 || v.releaseRemaining > wrapFade)
                {
                    v.releaseTotal = wrapFade;
                    v.releaseRemaining = wrapFade;
                    v.releaseDelay = splitSamples;
                }
            }
            if (songMode.load())
            {
                const size_t before = audioClipVoices.size();
                startAudioClipsAtPosition (0.0, tps, splitSamples);
                for (size_t i = before; i < audioClipVoices.size(); ++i)
                    audioClipVoices[i].fadeInTotal = juce::jmax (audioClipVoices[i].fadeInTotal, wrapFade);
            }

            processSequencerRange (0.0, remainder, splitSamples, tps);
            positionTicks = remainder;
        }
    }

    seekedFromTick = -1.0;

    wasPlaying = nowPlaying;

    positionForUi.store (positionTicks);
    loopLengthForUi.store (loopLen);

    if (in != nullptr && numIns > 0 && ! inputMuted.load())
    {
        for (auto& trPtr : mixerTracks)
        {
            auto& tr = *trPtr;
            const int il = tr.inputL.load();
            if (il >= 0)
            {
                const int ir = tr.inputR.load();
                if (il < numIns && in[il] != nullptr)
                {
                    tr.bufferDirty = true;
                    tr.buffer.addFrom (0, 0, in[il], numSamples);
                    if (ir < 0)
                        tr.buffer.addFrom (1, 0, in[il], numSamples);
                }
                if (ir >= 0 && ir < numIns && in[ir] != nullptr)
                {
                    tr.bufferDirty = true;
                    tr.buffer.addFrom (1, 0, in[ir], numSamples);
                }
            }

            if (nowPlaying)
                if (auto* job = tr.recordJob.load())
                    captureRecordBlock (*job, in, numIns, numSamples, blockStartTick);
        }
    }

    if (nowPlaying)
        for (auto& c : channels)
            if (c->slideActive && c->isPlugin && c->plugin != nullptr)
            {
                double denom = c->slideEndTick - c->slideStartTick;
                double t = denom > 0.0
                    ? juce::jlimit (0.0, 1.0, (positionTicks - c->slideStartTick) / denom)
                    : 1.0;
                c->bendCurrent = c->bendFrom + (c->bendTo - c->bendFrom) * t;
                c->midi.addEvent (juce::MidiMessage::pitchWheel (1, 8192 + (int) (c->bendCurrent * 8191.0)), 0);
                if (t >= 1.0)
                    c->slideActive = false;
            }

    applyAutomation (positionTicks);

    if (wakeAllProcessors.exchange (false))
    {
        for (auto& c : channels)
        {
            c->asleep = false;
            c->silentBlocks = 0;
        }
        for (auto& t : mixerTracks)
            for (auto& fx : t->effects)
            {
                fx->asleep = false;
                fx->silentBlocks = 0;
            }
    }

    renderChannels (numSamples);
    renderVoices (numSamples);
    processMixer (out, numOuts, numSamples);

    if (previewVoice.active && previewVoice.sample != nullptr && numOuts >= 2)
    {
        float mvol = faderGain (mixerTracks[0]->volume.load());
        if (! offlineRendering.load())
            mvol *= (float) playbackVolume.load();
        mixVoiceInto (out[0], out[1], numSamples, previewVoice, previewVoice.gain * mvol, 1.0f, 1.0f);
    }
    if (previewTailVoice.active && previewTailVoice.sample != nullptr && numOuts >= 2)
    {
        float mvol = faderGain (mixerTracks[0]->volume.load());
        if (! offlineRendering.load())
            mvol *= (float) playbackVolume.load();
        mixVoiceInto (out[0], out[1], numSamples, previewTailVoice, previewTailVoice.gain * mvol, 1.0f, 1.0f);
    }

    {
        const bool flushing = stopFlushPending.load();
        if (flushing || masterFlushGain < 1.0f)
        {
            const float target = flushing ? 0.0f : 1.0f;
            const float inc = 1.0f / juce::jmax (1.0f, (float) (0.010 * currentSampleRate));
            for (int i = 0; i < numSamples; ++i)
            {
                masterFlushGain = masterFlushGain < target ? juce::jmin (target, masterFlushGain + inc)
                                                           : juce::jmax (target, masterFlushGain - inc);
                for (int ch = 0; ch < numOuts; ++ch)
                    if (out[ch] != nullptr)
                        out[ch][i] *= masterFlushGain;
            }
        }
    }

    if (! offlineRendering.load())
    {
        float g = deafened.load() ? 0.0f : 1.0f;
        if (g != 1.0f)
            for (int i = 0; i < numOuts; ++i)
                if (out[i] != nullptr)
                    juce::FloatVectorOperations::multiply (out[i], g, numSamples);
    }

    if (License::shouldDegrade() && playing.load() && ! offlineRendering.load())
    {
        const juce::int64 cycle = (juce::int64) (45.0 * currentSampleRate);
        const juce::int64 gate  = (juce::int64) (0.30 * currentSampleRate);
        if (cycle > 0 && (samplePosition % cycle) < gate)
            for (int ch = 0; ch < numOuts; ++ch)
                if (out[ch] != nullptr)
                    juce::FloatVectorOperations::clear (out[ch], numSamples);
    }

    samplePosition += numSamples;
}

juce::Optional<juce::AudioPlayHead::PositionInfo> AudioEngine::getPosition() const
{
    juce::AudioPlayHead::PositionInfo info;
    info.setBpm (bpm.load());
    info.setTimeSignature (juce::AudioPlayHead::TimeSignature { 4, 4 });
    info.setIsPlaying (playing.load());
    info.setTimeInSamples (samplePosition);
    auto ppq = positionForUi.load() / (double) Model::PPQ;
    info.setPpqPosition (ppq);
    info.setTimeInSeconds (ppq * 60.0 / juce::jmax (1.0, bpm.load()));
    return info;
}

void AudioEngine::setRecordingFolder (const juce::File& folder)
{
    if (folder != juce::File())
        recordingFolder = folder;
}

void AudioEngine::ensureRecordingFolder()
{
    if (recordingFolder == juce::File())
        recordingFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                              .getChildFile ("mikeDAW Recordings");
    recordingFolder.createDirectory();
}

void AudioEngine::setRecordArmed (bool shouldArm)
{
    if (offlineRendering.load())
        return;
    const bool was = recordArmed.exchange (shouldArm);
    if (was == shouldArm)
        return;

    if (shouldArm)
    {
        if (playing.load())
            maybeStartRecording();
    }
    else
    {
        finalizeAllRecordings();
    }
}

void AudioEngine::startWriterThread()
{
    if (recordWriter == nullptr)
        recordWriter = std::make_unique<RecordWriter> (*this);
    if (! recordWriter->isThreadRunning())
        recordWriter->startThread();
}

void AudioEngine::maybeStartRecording()
{
    if (offlineRendering.load() || ! recordArmed.load() || ! audioRecordingEnabled.load())
        return;

    double sr = 0.0;
    {
        const juce::ScopedLock sl (engineLock);
        sr = currentSampleRate;
    }
    if (sr <= 0.0)
        return;

    ensureRecordingFolder();

    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S");

    std::vector<std::shared_ptr<RecordJob>> newJobs;
    for (int ti = 0; ti <= Model::maxMixerTracks; ++ti)
    {
        auto& tr = *mixerTracks[(size_t) ti];
        if (! tr.recArm.load() || tr.recordJob.load() != nullptr)
            continue;

        const int il = tr.inputL.load();
        if (il < 0)
            continue;
        const int ir = tr.inputR.load();
        const int nCh = ir >= 0 ? 2 : 1;

        auto file = recordingFolder.getChildFile ("Insert" + juce::String (ti) + "-" + stamp + ".wav");
        auto out = file.createOutputStream();
        if (out == nullptr)
            continue;

        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer (
            wav.createWriterFor (out.get(), sr, (unsigned int) nCh, 24, {}, 0));
        if (writer == nullptr)
            continue;
        out.release();

        auto job = std::make_shared<RecordJob> (nCh, (int) std::ceil (sr * 8.0));
        job->mixerTrack = ti;
        job->inputL = il;
        job->inputR = ir;
        job->sampleRate = sr;
        job->file = file;
        job->writer = std::move (writer);
        newJobs.push_back (std::move (job));
    }

    if (newJobs.empty())
        return;

    {
        std::lock_guard<std::mutex> lk (recordMutex);
        for (auto& j : newJobs)
            recordJobs.push_back (j);
    }
    {
        const juce::ScopedLock sl (engineLock);
        for (auto& j : newJobs)
            mixerTracks[(size_t) j->mixerTrack]->recordJob.store (j.get());
    }
    activeRecordingCount.fetch_add ((int) newJobs.size());
    startWriterThread();
}

void AudioEngine::finalizeAllRecordings()
{
    bool any = false;
    {
        const juce::ScopedLock sl (engineLock);
        for (auto& trPtr : mixerTracks)
            if (trPtr->recordJob.load() != nullptr)
            {
                trPtr->recordJob.store (nullptr);
                any = true;
            }
    }
    {
        std::lock_guard<std::mutex> lk (recordMutex);
        for (auto& j : recordJobs)
            j->active.store (false);
        if (! recordJobs.empty())
            any = true;
    }
    if (any && recordWriter != nullptr)
        recordWriter->notify();
}

void AudioEngine::captureRecordBlock (RecordJob& job, const float* const* in, int numIns,
                                      int numSamples, double blockStartTick) noexcept
{
    if (! job.capturing.exchange (true))
    {
        job.startTicks.store (blockStartTick);
        recordStartTicks.store (blockStartTick);
    }

    int start1, size1, start2, size2;
    job.fifo.prepareToWrite (numSamples, start1, size1, start2, size2);
    if (size1 + size2 < numSamples)
    {
        job.fifo.finishedWrite (0);
        job.overflow.store (true);
        return;
    }

    for (int ch = 0; ch < job.numChannels; ++ch)
    {
        const int srcIdx = ch == 0 ? job.inputL : job.inputR;
        const float* src = (srcIdx >= 0 && srcIdx < numIns && in[srcIdx] != nullptr) ? in[srcIdx] : nullptr;
        float* dst = job.fifoBuffer.getWritePointer (ch);
        if (src != nullptr)
        {
            if (size1 > 0) juce::FloatVectorOperations::copy (dst + start1, src, size1);
            if (size2 > 0) juce::FloatVectorOperations::copy (dst + start2, src + size1, size2);
        }
        else
        {
            if (size1 > 0) juce::FloatVectorOperations::clear (dst + start1, size1);
            if (size2 > 0) juce::FloatVectorOperations::clear (dst + start2, size2);
        }
    }
    job.fifo.finishedWrite (size1 + size2);
}

void AudioEngine::drainJob (RecordJob& job)
{
    if (job.writer == nullptr)
        return;

    for (int ready = job.fifo.getNumReady(); ready > 0; ready = job.fifo.getNumReady())
    {
        int start1, size1, start2, size2;
        job.fifo.prepareToRead (ready, start1, size1, start2, size2);

        auto writeRegion = [&] (int start, int size)
        {
            if (size <= 0)
                return;
            job.writer->writeFromAudioSampleBuffer (job.fifoBuffer, start, size);

            std::vector<std::pair<float, float>> newPairs;
            {
                for (int i = 0; i < size; ++i)
                {
                    float v = 0.0f;
                    for (int ch = 0; ch < job.numChannels; ++ch)
                        v += job.fifoBuffer.getSample (ch, start + i);
                    v /= (float) job.numChannels;

                    if (job.peakSamples == 0) { job.curMin = v; job.curMax = v; }
                    else { job.curMin = juce::jmin (job.curMin, v); job.curMax = juce::jmax (job.curMax, v); }

                    if (++job.peakSamples >= RecordJob::peakWindow)
                    {
                        newPairs.emplace_back (job.curMin, job.curMax);
                        job.peakSamples = 0;
                    }
                }
            }
            if (! newPairs.empty())
            {
                const juce::ScopedLock pl (job.peakLock);
                for (auto& p : newPairs)
                {
                    job.peaks.push_back (p.first);
                    job.peaks.push_back (p.second);
                }
            }
        };

        writeRegion (start1, size1);
        writeRegion (start2, size2);
        job.fifo.finishedRead (size1 + size2);
        job.framesWritten.fetch_add (size1 + size2);
    }
}

void AudioEngine::serviceRecordJobs()
{
    std::vector<std::shared_ptr<RecordJob>> jobs;
    {
        std::lock_guard<std::mutex> lk (recordMutex);
        jobs = recordJobs;
    }
    if (jobs.empty())
        return;

    std::vector<std::shared_ptr<RecordJob>> finishedNow;
    for (auto& job : jobs)
    {
        drainJob (*job);
        if (! job->active.load())
        {
            drainJob (*job);
            job->writer.reset();
            finishedNow.push_back (job);
        }
    }

    if (finishedNow.empty())
        return;

    {
        std::lock_guard<std::mutex> lk (recordMutex);
        for (auto& f : finishedNow)
        {
            recordJobs.erase (std::remove (recordJobs.begin(), recordJobs.end(), f), recordJobs.end());

            FinishedRecording fr;
            fr.mixerTrack   = f->mixerTrack;
            fr.file         = f->file;
            fr.startTicks   = f->startTicks.load();
            fr.lengthSeconds = f->sampleRate > 0.0 ? (double) f->framesWritten.load() / f->sampleRate : 0.0;
            fr.latencySeconds = f->sampleRate > 0.0
                ? (double) (inputLatencySamples.load() + outputLatencySamples.load()) / f->sampleRate : 0.0;
            finishedResults.push_back (fr);
        }
    }
    activeRecordingCount.fetch_sub ((int) finishedNow.size());

    juce::MessageManager::callAsync ([safe = juce::WeakReference<AudioEngine> (this)]
    {
        if (safe != nullptr)
            safe->flushFinishedRecordings();
    });
}

void AudioEngine::flushFinishedRecordings()
{
    std::vector<FinishedRecording> done;
    {
        std::lock_guard<std::mutex> lk (recordMutex);
        done.swap (finishedResults);
    }
    for (auto& r : done)
        if (onRecordingFinished)
            onRecordingFinished (r.mixerTrack, r.file, r.startTicks, r.lengthSeconds, r.latencySeconds);
}

std::vector<AudioEngine::RecordingInfo> AudioEngine::getActiveRecordings() const
{
    std::vector<RecordingInfo> out;
    std::lock_guard<std::mutex> lk (recordMutex);
    for (auto& j : recordJobs)
        if (j->active.load())
            out.push_back ({ j->mixerTrack, j->startTicks.load() });
    return out;
}

int AudioEngine::getRecordingPeaks (int mixerTrack, float* minMaxPairs, int maxPairs) const
{
    if (minMaxPairs == nullptr || maxPairs <= 0)
        return 0;

    std::shared_ptr<RecordJob> job;
    {
        std::lock_guard<std::mutex> lk (recordMutex);
        for (auto& j : recordJobs)
            if (j->mixerTrack == mixerTrack && j->active.load())
            {
                job = j;
                break;
            }
    }
    if (job == nullptr)
        return 0;

    const juce::ScopedLock pl (job->peakLock);
    const int available = (int) (job->peaks.size() / 2);
    const int n = juce::jmin (maxPairs, available);
    if (n > 0)
        std::memcpy (minMaxPairs, job->peaks.data(), (size_t) n * 2 * sizeof (float));
    return n;
}

double AudioEngine::getRecordingLengthSeconds (int mixerTrack) const
{
    std::lock_guard<std::mutex> lk (recordMutex);
    for (auto& j : recordJobs)
        if (j->mixerTrack == mixerTrack && j->active.load())
            return j->sampleRate > 0.0 ? (double) j->framesWritten.load() / j->sampleRate : 0.0;
    return 0.0;
}
