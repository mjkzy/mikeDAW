#pragma once
#include <JuceHeader.h>
#include <deque>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <vector>
#include "../Model/ProjectModel.h"
#include "../Model/Automation.h"

class PluginHost;

class AudioEngine : public juce::AudioIODeviceCallback,
                    public juce::AudioPlayHead,
                    private juce::ValueTree::Listener,
                    private juce::AsyncUpdater
{
public:
    AudioEngine (juce::ValueTree projectTree, PluginHost& pluginHost);
    ~AudioEngine() override;

    void play();
    void stop();
    void stopAt (double ticks);
    void pause();
    void togglePlay();
    bool isPlaying() const noexcept                 { return playing.load(); }
    double getPositionTicks() const noexcept        { return positionForUi.load(); }
    void seekTicks (double ticks, bool fireNotesAtTarget = false);
    double getBpm() const noexcept                  { return bpm.load(); }
    void setBpm (double newBpm);
    void setSongMode (bool shouldBeSongMode);
    bool isSongMode() const noexcept                { return songMode.load(); }
    void setMetronomeEnabled (bool on) noexcept     { metronomeOn.store (on); }
    bool isMetronomeEnabled() const noexcept        { return metronomeOn.load(); }
    void setRecordArmed (bool);
    bool isRecordArmed() const noexcept             { return recordArmed.load(); }
    bool isRecording() const noexcept               { return activeRecordingCount.load() > 0; }
    double getRecordStartTicks() const noexcept     { return recordStartTicks.load(); }

    struct RecordingInfo { int mixerTrack; double startTicks; };
    std::vector<RecordingInfo> getActiveRecordings() const;

    int getRecordingPeaks (int mixerTrack, float* minMaxPairs, int maxPairs) const;
    double getRecordingLengthSeconds (int mixerTrack) const;

    std::function<void (int mixerTrack, juce::File file, double startTicks,
                        double lengthSeconds, double latencySeconds)> onRecordingFinished;
    void setRecordingFolder (const juce::File&);

    void setAudioRecordingEnabled (bool on) noexcept { audioRecordingEnabled.store (on); }
    bool isAudioRecordingEnabled() const noexcept     { return audioRecordingEnabled.load(); }

    void setSmartDisableEnabled (bool on) noexcept  { smartDisable.store (on); wakeAllProcessors.store (true); }
    bool isSmartDisableEnabled() const noexcept     { return smartDisable.load(); }

    void setMulticoreRenderingEnabled (bool on);
    bool isMulticoreRenderingEnabled() const noexcept { return multicoreRendering.load(); }

    void setPlaybackVolume (double v);
    double getPlaybackVolume() const noexcept       { return playbackVolume.load(); }

    void setDeafened (bool d)                        { deafened.store (d); }
    bool isDeafened() const noexcept                { return deafened.load(); }
    void setInputMuted (bool m) noexcept            { inputMuted.store (m); }
    bool isInputMuted() const noexcept              { return inputMuted.load(); }
    void setCurrentPatternId (int patternId);
    int getCurrentPatternId() const noexcept        { return currentPatternId.load(); }
    double getLoopLengthTicks() const noexcept      { return (double) loopLengthForUi.load(); }

    std::function<void (int cmd, double posTicks)> onTransportBroadcast;
    std::atomic<bool> suppressTransportBroadcast { false };

    static float sampleTrimThreshold (float knob) noexcept
    {
        const float k = juce::jlimit (0.0f, 1.0f, knob);
        return k <= 0.0f ? 0.0f : juce::Decibels::decibelsToGain (-96.0f + k * 72.0f);
    }

    static constexpr double sampleTrimWindowMs = 5.0;

    struct SampleTrimBounds { double startSeconds = 0.0, lengthSeconds = 0.0; };
    bool getSampleTrimBounds (const juce::String& pathOrBuiltin,
                              const juce::ValueTree& settingsSource,
                              SampleTrimBounds& out);
    void previewFile (const juce::File& file, double normalizedStart = 0.0);
    void stopPreview();
    void prefetchFiles (const juce::Array<juce::File>& files);
    void auditionNote (int channelId, int key, int velocity);
    void auditionNoteOff (int channelId, int key);

    std::function<void (int channelId, bool on, int note, float velocity)> onAudition;
    std::atomic<bool> suppressAuditionBroadcast { false };
    juce::AudioPluginInstance* getPluginFor (const juce::String& key);
    juce::AudioProcessor* getProcessorFor (const juce::String& key);
    juce::String getPluginNameFor (const juce::String& key);
    void capturePluginStatesToTree();
    void applyPluginStateBlob (const juce::String& key, const void* data, int size);
    juce::MemoryBlock capturePluginState (const juce::String& key);

    juce::StringArray getAllPluginKeys() const;

    std::function<void (const juce::String& key, int paramIndex, float value)> onParamChanged;
    std::atomic<bool> suppressParamBroadcast { false };
    void setRemoteParam (const juce::String& key, int paramIndex, float value);

    std::function<void (const juce::String& key)> onNonParamStateChanged;

    std::function<void (const juce::String& key)> onPluginRemoved;

    std::function<void (const juce::String& key)> onPluginCreated;

    std::function<void (int channelId, const juce::String& pluginName)> onPluginMissing;

    std::function<void (const juce::String& pluginName)> onPluginLoading;

    bool isPluginLoading (const juce::String& key) const;

    std::function<void (const juce::String& key, const juce::String& pluginName)> onEffectMissing;

    bool isChannelPlaceholder (int channelId) const;
    bool renderToFile (juce::AudioFormatWriter& writer, double tailSeconds = 2.0,
                       std::function<void (float)> onProgress = {});

    static bool renderBuiltinSampleToWav (const juce::String& which, const juce::File& dest);

    void setSamplePathResolver (std::function<juce::File (const juce::String&)> resolver);
    void refreshResolvedSampleReferences (const juce::String& originalPath = {});
    juce::File resolveSampleFile (const juce::String& path);
    bool saveProcessedSampleToWav (const juce::String& pathOrBuiltin,
                                   const juce::ValueTree& settingsSource,
                                   const juce::File& destination,
                                   int bitDepth = 24);
    double getProcessedSampleSeconds (const juce::String& pathOrBuiltin,
                                      const juce::ValueTree& settingsSource);
    static double sourceSecondsPerBeat (const juce::ValueTree& clipOrChannel,
                                        double sourceSeconds, double bpmValue);
    void invalidateSampleCache (const juce::String& pathOrBuiltin = {});
    int drainNoteOnChannelIds (int* dest, int maxCount) noexcept;
    float getTrackPeak (int trackIndex) const;

    float getTrackAutoVolume (int trackIndex) const;
    float getTrackAutoPan (int trackIndex) const;

    static constexpr int spectrumSize = 1024;
    std::array<float, spectrumSize> spectrumSnapshot {};
    std::atomic<int> spectrumWritePos { 0 };

    struct AudioPerfStats
    {
        std::atomic<float> load { 0.0f };
        std::atomic<float> loadPeak { 0.0f };
        std::atomic<float> lockWaitMs { 0.0f };
        std::atomic<uint32_t> xruns { 0 };
        std::atomic<uint32_t> droppedClipVoices { 0 };
        std::atomic<uint32_t> callbacks { 0 };
        std::atomic<int> bufferSamples { 0 };
        std::atomic<double> sampleRate { 0.0 };

        static constexpr int historySize = 512;
        std::array<std::atomic<float>, historySize> loadHistory {};
        std::atomic<int> historyWritePos { 0 };

        void publish (double totalSec, double lockWaitSec, double periodSec,
                      int numSamples, double sr) noexcept
        {
            const float inst = periodSec > 0.0 ? (float) (totalSec / periodSec) : 0.0f;
            load.store (load.load (std::memory_order_relaxed) * 0.9f + inst * 0.1f,
                        std::memory_order_relaxed);

            const float pk = loadPeak.load (std::memory_order_relaxed);
            loadPeak.store (inst > pk ? inst : pk * 0.99f, std::memory_order_relaxed);

            const float lwInst = (float) (lockWaitSec * 1000.0);
            lockWaitMs.store (lockWaitMs.load (std::memory_order_relaxed) * 0.9f + lwInst * 0.1f,
                              std::memory_order_relaxed);

            if (totalSec > periodSec)
                xruns.fetch_add (1, std::memory_order_relaxed);
            callbacks.fetch_add (1, std::memory_order_relaxed);
            bufferSamples.store (numSamples, std::memory_order_relaxed);
            sampleRate.store (sr, std::memory_order_relaxed);

            const int wp = historyWritePos.load (std::memory_order_relaxed);
            loadHistory[(size_t) wp].store (inst, std::memory_order_relaxed);
            historyWritePos.store ((wp + 1) & (historySize - 1), std::memory_order_release);
        }
    };
    const AudioPerfStats& getAudioPerfStats() const noexcept { return audioPerfStats; }

    struct PluginPerfInfo
    {
        juce::String key;
        float load = 0.0f;
        float peak = 0.0f;
    };
    std::vector<PluginPerfInfo> getPluginPerfStats() const;
    void setPluginPerfMonitoringEnabled (bool enabled) noexcept
    {
        pluginPerfMonitoringEnabled.store (enabled, std::memory_order_release);
    }

    juce::AudioDeviceManager deviceManager;
    juce::AudioFormatManager audioFormats;

    void audioDeviceIOCallbackWithContext (const float* const*, int, float* const*,
                                           int, int, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;

    juce::Optional<juce::AudioPlayHead::PositionInfo> getPosition() const override;

private:
    struct ProcessorPerfStats
    {
        std::atomic<float> load { 0.0f };
        std::atomic<float> peak { 0.0f };

        void publish (float instantaneous) noexcept
        {
            load.store (load.load (std::memory_order_relaxed) * 0.85f + instantaneous * 0.15f,
                        std::memory_order_relaxed);
            const float previousPeak = peak.load (std::memory_order_relaxed);
            peak.store (instantaneous > previousPeak ? instantaneous : previousPeak * 0.995f,
                        std::memory_order_relaxed);
        }
    };
    struct SampleData
    {
        juce::AudioBuffer<float> buffer;
        double sourceSampleRate = 44100.0;
    };
    using SampleDataPtr = std::shared_ptr<SampleData>;

    struct SampleFxSettings
    {
        float start = 0.0f, length = 1.0f;
        float trim = 0.0f;
        int declickMode = 0;
        float fadeIn = 0.0f, fadeOut = 0.0f;
        bool keepOnDisk = false;
        bool removeDC = false, normalize = false, reverse = false,
             revPolarity = false, fadeStereo = false, swapStereo = false;

        static SampleFxSettings fromTree (const juce::ValueTree&);
        bool isDefault() const noexcept;
        juce::String signature() const;
    };

    struct SamplePlaybackSettings
    {
        bool cubic = false;
        bool useLoop = false;
        bool pingPong = false;
        float startOffset = 0.0f;
        float loopStart = 0.0f;
        float loopEnd = 1.0f;
        float loopCrossfade = 0.0f;
        int stretchMode = 0;
        float stretchPitchSemis = 0.0f;
        float stretchMultiplier = 1.0f;
        float stretchTime = 1.0f;
        int stretchTempoMode = 0;
        float stretchSourceBpm = 0.0f;
        float stretchBeats = 0.0f;
        float formantShift = 0.0f;

        static SamplePlaybackSettings fromTree (const juce::ValueTree&);
        juce::String signature() const;
    };

    struct SampleRegion
    {
        int windowStart = 0, windowLen = 0;
        int start = 0, len = 0;
    };
    static SampleRegion sampleRegion (const SampleData&, const SampleFxSettings&);

    struct Voice
    {
        SampleDataPtr sample;
        double pos = 0, ratio = 1.0;
        float gain = 1.0f;
        float gainL = 1.0f, gainR = 1.0f;
        int channelId = -1;
        int key = -1;
        int clipUid = 0;
        int startOffset = 0;
        int mixerTrack = -1;
        bool active = false;
        bool isAudioClip = false;
        bool audition = false;
        juce::int64 clipEndTick = 0;
        juce::int64 outSamples = 0;
        int fadeInTotal = 0;
        int releaseTotal = 0, releaseRemaining = -1;
        int releaseDelay = 0;
        bool cubic = false;
        bool loopEnabled = false;
        bool pingPong = false;
        bool granular = false;
        double loopStart = 0.0;
        double loopEnd = 0.0;
        double loopCrossfade = 0.0;
        double timeAdvance = 1.0;
        int grainLength = 0;
        int grainAge[2] { 0, 0 };
        double grainPos[2] { 0.0, 0.0 };
        float formantAmount = 0.0f;
        float formantCoefficient = 0.0f;
        float formantStateL = 0.0f;
        float formantStateR = 0.0f;
        double slideFactor = 1.0;
        juce::int64 slideRemaining = 0;
        double slideTargetRatio = 1.0;
        float gainStep = 0.0f;
    };

    struct ScopedAtomicFlag
    {
        explicit ScopedAtomicFlag (std::atomic<bool>& f) : flag (f), previous (f.exchange (true)) {}
        ~ScopedAtomicFlag() { flag.store (previous); }
        std::atomic<bool>& flag;
        const bool previous;
    };

    struct ParamListener : juce::AudioProcessorListener
    {
        ParamListener (AudioEngine& e, juce::String k) : engine (e), key (std::move (k)) {}
        void audioProcessorParameterChanged (juce::AudioProcessor*, int idx, float value) override
        {
            if (! engine.suppressParamBroadcast.load() && engine.onParamChanged)
                engine.onParamChanged (key, idx, value);
        }
        void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails& details) override
        {
            if (engine.suppressParamBroadcast.load())
                return;
            if ((details.programChanged || details.nonParameterStateChanged)
                && engine.onNonParamStateChanged)
                engine.onNonParamStateChanged (key);
        }
        AudioEngine& engine;
        juce::String key;
    };

    struct EngineChannel
    {
        int id = 0;
        bool isPlugin = false;
        bool placeholder = false;
        bool loading = false;
        juce::uint64 loadEpoch = 0;
        juce::String pluginId;
        juce::String sampleSig;
        SampleDataPtr sample;
        std::unique_ptr<juce::AudioProcessor> plugin;
        std::unique_ptr<ParamListener> paramListener;
        std::atomic<float> volume { 0.8f }, pan { 0.0f };
        std::atomic<bool> mute { false };
        std::atomic<bool> cutItself { false };
        std::atomic<int> mixerTrack { 1 };
        std::atomic<int> declickMode { 0 };
        std::atomic<float> autoVolume { -1.0f }, autoPan { -2.0f };
        std::atomic<float> pitchSemis { 0.0f };
        std::atomic<int> rootNote { 60 };
        std::atomic<float> fineTune { 0.0f };
        std::atomic<bool> sampleResample { false };
        std::atomic<bool> useLoopPoints { false };
        std::atomic<bool> pingPongLoop { false };
        std::atomic<float> startOffset { 0.0f };
        std::atomic<float> loopStart { 0.0f };
        std::atomic<float> loopEnd { 1.0f };
        std::atomic<float> loopCrossfade { 0.0f };
        std::atomic<int> stretchMode { 0 };
        std::atomic<float> stretchPitchSemis { 0.0f };
        std::atomic<float> stretchMultiplier { 1.0f };
        std::atomic<float> stretchTime { 1.0f };
        std::atomic<int> stretchTempoMode { 0 };
        std::atomic<float> stretchSourceBpm { 0.0f };
        std::atomic<float> stretchBeats { 0.0f };
        std::atomic<float> formantShift { 0.0f };
        juce::MidiBuffer midi;
        struct HeldSeqNote { int key, clipUid; };
        std::array<HeldSeqNote, 128> heldSeqNotes {};
        int heldSeqNoteCount = 0;
        bool slideActive = false;
        double slideStartTick = 0.0, slideEndTick = 0.0;
        double bendFrom = 0.0, bendTo = 0.0, bendCurrent = 0.0;
        int lastNoteKey = 60;
        int silentBlocks = 0;
        bool asleep = false;
        juce::AudioBuffer<float> scratch;
        int renderChans = 2;
        bool renderHadMidi = false;
        bool renderSilent = true;
        ProcessorPerfStats perf;
    };

    struct EngineEffect
    {
        int uid = 0;
        int slotIndex = 0;
        std::unique_ptr<juce::AudioProcessor> processor;
        std::unique_ptr<ParamListener> paramListener;
        std::atomic<bool> enabled { true };
        std::atomic<float> mix { 1.0f };
        bool isPlugin = false;
        bool loading = false;
        juce::uint64 loadEpoch = 0;
        juce::String key;
        int silentBlocks = 0;
        bool asleep = false;
        ProcessorPerfStats perf;
    };

    struct RecordJob;

    struct EngineMixerTrack
    {
        std::atomic<float> volume { 1.0f }, pan { 0.0f };
        std::atomic<float> stereoSep { 0.0f };
        std::atomic<float> autoVolume { -1.0f }, autoPan { -2.0f };
        std::atomic<bool> mute { false };
        std::atomic<int> routeTo { 0 };
        std::atomic<int> inputL { -1 }, inputR { -1 };
        std::atomic<bool> recArm { false };
        std::atomic<RecordJob*> recordJob { nullptr };
        std::vector<std::unique_ptr<EngineEffect>> effects;
        juce::AudioBuffer<float> buffer;
        bool bufferDirty = true;
        std::atomic<float> peak { 0.0f };
    };

    struct NoteEvent
    {
        juce::int64 tick;
        int channelId, key, velocity;
        bool noteOn;
        int clipUid = 0;
        bool operator< (const NoteEvent& o) const noexcept { return tick < o.tick; }
    };

    struct SlideEvent
    {
        juce::int64 tick;
        int channelId, key, velocity;
        juce::int64 lengthTicks;
        bool operator< (const SlideEvent& o) const noexcept { return tick < o.tick; }
    };

    struct CompiledAutoPoint { double posTicks; double val; int mode; double tension; };
    struct CompiledAutoLane
    {
        Automation::Target target;
        juce::int64 startTick = 0, endTick = 0, offsetTick = 0;
        std::vector<CompiledAutoPoint> points;
    };

    struct EngineAudioClip
    {
        SampleDataPtr sample;
        juce::int64 startTick = 0, endTick = 0;
        juce::int64 offsetTick = 0;
        float gain = 1.0f;
        float gainL = 1.0f, gainR = 1.0f;
        double pitchRatio = 1.0;
        int channelId = -1;
        int mixerTrack = -1;
        int uid = 0;
        juce::String contentKey;
        bool changed = false;
        int declickMode = 0;
        SamplePlaybackSettings playback;
    };
    struct LoadJob
    {
        bool isChannel = true;
        int channelId = 0;
        int trackIndex = 0, slotUid = 0;
        juce::String key, pluginId, displayName;
        juce::PluginDescription desc;
        juce::MemoryBlock state;
        juce::uint64 epoch = 0;
        double sampleRate = 44100.0;
        int blockSize = 512;
    };
    struct LoadResult
    {
        LoadJob job;
        std::unique_ptr<juce::AudioProcessor> instance;
        bool prepared = false;
        double preparedRate = 0;
        int preparedBlock = 0;
    };
    struct PluginLoaderThread : juce::Thread
    {
        explicit PluginLoaderThread (AudioEngine& e) : juce::Thread ("mikeDAW Plugin Loader"), engine (e) {}
        void run() override { engine.pluginLoaderRun(); }
        AudioEngine& engine;
    };

    void enqueuePluginLoad (LoadJob&&);
    void pluginLoaderRun();
    void triggerCompletionAsync();
    void processCompletedLoads();
    void finishPluginLoad (LoadResult&&);
    void applyLoadFailure (const LoadJob&);

    void scheduleMessageThreadLoad();
    void drainMessageThreadLoad();

    std::vector<std::unique_ptr<PluginLoaderThread>> loaderThreads;
    std::mutex loaderMutex;
    std::deque<LoadJob> loadQueue;
    std::vector<LoadResult> completedLoads;
    std::deque<LoadJob> messageThreadQueue;
    bool messageLoadScheduled = false;
    juce::WaitableEvent loaderWake;
    std::atomic<bool> loaderShutdown { false };
    juce::uint64 loadEpochCounter = 0;

    struct FilePrewarmThread : juce::Thread
    {
        explicit FilePrewarmThread (AudioEngine& e) : juce::Thread ("mikeDAW Plugin Prewarm"), engine (e) {}
        void run() override { engine.filePrewarmRun(); }
        AudioEngine& engine;
    };

    void queueFilePrewarm (const juce::String& path);
    void filePrewarmRun();

    std::unique_ptr<FilePrewarmThread> prewarmThread;
    std::mutex prewarmMutex;
    std::deque<juce::String> prewarmQueue;
    std::set<juce::String> prewarmSeen;
    juce::WaitableEvent prewarmWake;
    std::atomic<bool> prewarmShutdown { false };

    void rebuildChannels();
    void rebuildMixer();
    void recompileSequences();
    void updateChannelAtomics (EngineChannel&, const juce::ValueTree&);
    void updateTrackAtomics (EngineMixerTrack&, const juce::ValueTree&);
    void captureStateInto (juce::ValueTree& dest, const juce::String& key, juce::AudioProcessor& proc);
    std::map<juce::String, juce::int64> lastCapturedStateHash;

    SampleDataPtr loadSample (const juce::String& pathOrBuiltin);
    struct SampleCacheEntry
    {
        SampleDataPtr data;
        size_t bytes = 0;
        juce::String resolvedPath;
        juce::int64 fileSize = 0;
        juce::int64 modifiedMs = 0;
    };
    using SampleCacheMap = std::map<juce::String, SampleCacheEntry>;
    SampleCacheMap sampleCache;
    std::list<juce::String> sampleLru;
    size_t sampleCacheBytes = 0;
    static constexpr size_t sampleCacheMaxBytes = 512ull * 1024 * 1024;
    juce::CriticalSection sampleCacheLock;
    static void lruTouch (std::list<juce::String>& lru, const juce::String& key);
    static void lruEvict (SampleCacheMap&, std::list<juce::String>& lru,
                          size_t& bytes, size_t maxBytes);
    SampleDataPtr loadProcessedSample (const juce::String& pathOrBuiltin, const SampleFxSettings&);
    SampleCacheMap processedCache;
    std::list<juce::String> processedLru;
    size_t processedCacheBytes = 0;
    static constexpr size_t processedCacheMaxBytes = 128ull * 1024 * 1024;
    juce::CriticalSection processedCacheLock;
    static SampleDataPtr applySampleFx (const SampleDataPtr& raw, const SampleFxSettings&);
    static SampleDataPtr synthesizeBuiltin (const juce::String& which);
    void preparePlugin (juce::AudioProcessor&, bool isInstrument);
    EngineChannel* findChannel (int channelId) const;
    EngineEffect* findEffect (const juce::String& key, EngineMixerTrack** trackOut = nullptr) const;

    void processSequencerRange (double startTick, double endTick, int sampleOffsetBase, double ticksPerSample,
                                bool includeEndOffs = false, bool scanFromFloor = false);
    void fireSpanningNotesAt (double posTicks, int sampleOffset);
    void triggerSamplerVoice (EngineChannel&, int key, int velocity, int sampleOffset, bool audition = false,
                              int clipUid = 0);
    void chokeSamplerVoices (int channelId, int key, bool auditionOnly = false,
                             int clipUid = -1);
    void holdSeqNote (EngineChannel&, int key, int clipUid) noexcept;
    void releaseSeqNote (EngineChannel&, int key, int clipUid = -1) noexcept;
    void clearHeldSeqNotes() noexcept;
    SamplePlaybackSettings channelPlaybackSettings (const EngineChannel&) const noexcept;
    static double stretchDurationFactor (const SamplePlaybackSettings&, double sourceSeconds,
                                         double bpmValue) noexcept;
    void configureVoicePlayback (Voice&, const SamplePlaybackSettings&, double pitchRatio,
                                 double elapsedOutputSeconds, int declickMode);
    double voiceMappedPosition (const Voice&, double rawPosition) const noexcept;
    void sampleVoiceAt (const Voice&, double rawPosition, float& outL, float& outR) const noexcept;
    static double alignGrainOffset (const Voice&, double refPos, double nominalPos) noexcept;
    void mixVoiceInto (float* l, float* r, int numSamples, Voice&,
                       float gain, float gl, float gr);
    void renderVoices (int numSamples);
    void renderChannels (int numSamples);
    void renderChannelBody (EngineChannel&, int numSamples);
    void processMixer (float* const* out, int numOuts, int numSamples);

    void applyAutomation (double posTicks);
    float effVolume (const EngineChannel&) const;
    float effPan (const EngineChannel&) const;
    float effVolume (const EngineMixerTrack&) const;
    float effPan (const EngineMixerTrack&) const;
    void resyncAudioClipVoices (bool fullResync, double ticksPerSample);
    void startAudioClipsAtPosition (double tick, double ticksPerSample, int sampleOffset,
                                    bool changedOnly = false);
    void seatVoiceInClip (Voice&, const EngineAudioClip&, double tick);
    void pushAudioClipVoice (Voice&) noexcept;
    const EngineAudioClip* findAudioClip (int uid) const noexcept;
    juce::AudioBuffer<float>& trackBufferFor (int trackIndex);
    double ticksPerSample (double sr) const;

    void pushNoteOnActivity (int channelId) noexcept;

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override;
    void valueTreeParentChanged (juce::ValueTree&) override {}
    void valueTreeRedirected (juce::ValueTree&) override;
    void handleAsyncUpdate() override;
    void markDirty (bool channels, bool mixer, bool sequences);

    void flushAfterStop();
    struct StopFlusher : juce::AsyncUpdater
    {
        explicit StopFlusher (AudioEngine& e) : engine (e) {}
        void handleAsyncUpdate() override { engine.flushAfterStop(); }
        AudioEngine& engine;
    };
    StopFlusher stopFlusher { *this };
    std::atomic<bool> stopFlushPending { false };
    juce::CriticalSection stopFlushLock;
    float masterFlushGain = 1.0f;

    struct FinishedRecording;
    struct RecordWriter;

    void maybeStartRecording();
    void finalizeAllRecordings();
    void ensureRecordingFolder();
    void startWriterThread();
    void serviceRecordJobs();
    void drainJob (RecordJob&);
    void flushFinishedRecordings();
    void captureRecordBlock (RecordJob&, const float* const* in, int numIns,
                             int numSamples, double blockStartTick) noexcept;
    juce::ValueTree project;
    PluginHost& host;

    struct RenderPool;
    std::unique_ptr<RenderPool> renderPool;
    std::vector<EngineChannel*> renderList;
    bool renderProfile = false, renderSmart = false;
    double renderBudgetTicks = 0.0;
    int renderTailBlocks = 0, renderNumSamples = 0;

    juce::CriticalSection pluginLifetimeLock;

    juce::CriticalSection engineLock;
    std::vector<std::unique_ptr<EngineChannel>> channels;
    std::array<std::unique_ptr<EngineMixerTrack>, Model::maxMixerTracks + 1> mixerTracks;
    std::vector<NoteEvent> songEvents, patternEvents;
    std::vector<SlideEvent> songSlides, patternSlides;
    std::vector<EngineAudioClip> audioClips;
    std::vector<CompiledAutoLane> autoLanes;
    bool autoOverridesActive = false;
    juce::int64 songLengthTicks = Model::ticksPerBar * 4;
    juce::int64 patternLengthTicks = Model::PPQ * 4;

    static constexpr int maxVoices = 96;
    static constexpr int maxAudioClipVoices = 256;
    static constexpr int stretchGrainLength = 4096;
    static constexpr int auditionMidiChannel = 2;
    std::array<Voice, maxVoices> voices;
    std::array<float, stretchGrainLength> stretchHann {};
    int nextSteal = 0;
    Voice previewVoice;
    Voice previewTailVoice;
    std::vector<Voice> audioClipVoices;

    static constexpr int noteOnFifoCapacity = 256;
    juce::AbstractFifo noteOnFifo { noteOnFifoCapacity };
    std::array<int, noteOnFifoCapacity> noteOnChannelIds {};

    static constexpr int sampleGraveyardCapacity = 512;
    juce::AbstractFifo sampleGraveyardFifo { sampleGraveyardCapacity };
    std::array<SampleDataPtr, sampleGraveyardCapacity> sampleGraveyard;
    void retireSample (SampleDataPtr&) noexcept;
    void drainSampleGraveyard();
    struct GraveyardDrainer : juce::Timer
    {
        explicit GraveyardDrainer (AudioEngine& e) : engine (e) {}
        void timerCallback() override { engine.drainSampleGraveyard(); }
        AudioEngine& engine;
    };
    GraveyardDrainer graveyardDrainer { *this };

    struct Audition { int channelId, key, velocity; bool on; bool forceChoke = false;
                      int chokeClipUid = -1; bool noteRemoval = false; };
    std::vector<Audition> pendingAuditions;

    struct HeldAudition { int channelId, key, midiChan; };
    std::array<HeldAudition, 64> heldAuditions {};
    int heldAuditionCount = 0;

    juce::AudioBuffer<float> pluginScratch;
    juce::AudioBuffer<float> dryScratch;
    SampleDataPtr clickHi, clickLo;

    std::atomic<bool> smartDisable { true };
    std::atomic<bool> wakeAllProcessors { false };
    std::atomic<bool> multicoreRendering { true };
    std::atomic<bool> playing { false };
    std::atomic<double> bpm { 140.0 };
    std::atomic<double> playbackVolume { 1.0 };
    std::atomic<bool> deafened { false };
    std::atomic<bool> inputMuted { false };
    std::atomic<bool> songMode { false };
    std::atomic<bool> metronomeOn { false };
    std::atomic<int> currentPatternId { 0 };
    std::atomic<double> positionForUi { 0.0 };
    std::atomic<double> playAnchorTicks { 0.0 };
    std::atomic<double> stopReturnTicks { 0.0 };
    std::atomic<juce::int64> loopLengthForUi { Model::PPQ * 4 };
    std::atomic<double> seekRequest { -1.0 };
    std::atomic<bool> seekFiresNotes { false };
    std::atomic<bool> stopRequest { false };
    std::atomic<bool> pauseRequest { false };
    std::atomic<bool> modeSwitchCut { false };
    std::atomic<bool> patternSwitchCut { false };
    std::atomic<bool> resyncAudioClips { false };
    std::atomic<bool> resyncEditedClips { false };

    double positionTicks = 0;
    double currentSampleRate = 0;
    int currentBlockSize = 512;
    double preparedSampleRate = 0;
    int preparedBlockSize = 0;
    juce::int64 samplePosition = 0;
    double seekedFromTick = -1.0;
    bool wasPlaying = false;
    int spanRefireCountdown = -1;
    std::atomic<bool> offlineRendering { false };

    bool dirtyChannels = false, dirtyMixer = false, dirtySequences = false;
    bool rebuildInProgress = false;

    std::atomic<int> previewGeneration { 0 };
    juce::ThreadPool previewPool { 1 }, prefetchPool { 1 };

    juce::CriticalSection sampleResolverLock;
    std::function<juce::File (const juce::String&)> samplePathResolver;

    std::atomic<bool> recordArmed { false };
    std::atomic<bool> audioRecordingEnabled { true };
    std::atomic<int> deviceInputChannels { 0 };
    std::atomic<int> inputLatencySamples { 0 }, outputLatencySamples { 0 };
    std::atomic<int> activeRecordingCount { 0 };
    std::atomic<double> recordStartTicks { 0.0 };

    AudioPerfStats audioPerfStats;
    std::atomic<bool> pluginPerfMonitoringEnabled { false };

    juce::File recordingFolder;
    mutable std::mutex recordMutex;
    std::vector<std::shared_ptr<RecordJob>> recordJobs;
    std::vector<FinishedRecording> finishedResults;
    std::unique_ptr<RecordWriter> recordWriter;

    JUCE_DECLARE_WEAK_REFERENCEABLE (AudioEngine)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};
