#include "Bridge32Host.h"
#include "Bridge32Protocol.h"
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif
#include <atomic>
#include <mutex>
#include <vector>
#include "../UI/AppLookAndFeel.h"

using namespace bridge32;

namespace Bridge32
{

juce::File bridgeExecutable()
{
    return juce::File::getSpecialLocation (juce::File::currentExecutableFile)
               .getSiblingFile ("mikeDAW-Bridge32.exe");
}

bool isAvailable()
{
    return bridgeExecutable().existsAsFile();
}

#if ! JUCE_WINDOWS

bool scanPlugin (const juce::File&, juce::PluginDescription&, juce::String& error)
{
    error = "32-bit VST2 bridging is Windows-only";
    return false;
}

std::unique_ptr<juce::AudioPluginInstance> createInstance (
    const juce::PluginDescription&, double, int, juce::String& error)
{
    error = "32-bit VST2 bridging is Windows-only";
    return nullptr;
}

#else

namespace
{

constexpr uint32_t stateMagic = 0x42333253;

struct Connection
{
    HANDLE mapping   = nullptr;
    void*  view      = nullptr;
    HANDLE reqEvent  = nullptr;
    HANDLE respEvent = nullptr;
    HANDLE uiEvent   = nullptr;
    HANDLE process   = nullptr;
    HANDLE job       = nullptr;
    SharedHeader* header = nullptr;
    uint8_t* chunkArea   = nullptr;
};

void closeConnection (Connection& c)
{
    if (c.process && WaitForSingleObject (c.process, 0) != WAIT_OBJECT_0)
        TerminateProcess (c.process, 0);
    if (c.view)      UnmapViewOfFile (c.view);
    if (c.mapping)   CloseHandle (c.mapping);
    if (c.reqEvent)  CloseHandle (c.reqEvent);
    if (c.respEvent) CloseHandle (c.respEvent);
    if (c.uiEvent)   CloseHandle (c.uiEvent);
    if (c.process)   CloseHandle (c.process);
    if (c.job)       CloseHandle (c.job);
    c = Connection{};
}

DWORD waitBridge (HANDLE resp, HANDLE process, DWORD timeoutMs)
{
    HANDLE hs[2] = { resp, process };
    auto* mm = juce::MessageManager::getInstanceWithoutCreating();
    if (mm == nullptr || ! mm->isThisTheMessageThread())
        return WaitForMultipleObjects (2, hs, FALSE, timeoutMs);

    const DWORD start = GetTickCount();
    for (;;)
    {
        const DWORD elapsed = GetTickCount() - start;
        if (elapsed >= timeoutMs)
            return WAIT_TIMEOUT;
        const DWORD w = MsgWaitForMultipleObjectsEx (2, hs, timeoutMs - elapsed, QS_SENDMESSAGE, MWMO_INPUTAVAILABLE);
        if (w != WAIT_OBJECT_0 + 2)
            return w;
        MSG m;
        PeekMessageW (&m, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
    }
}

class Bridge32PluginInstance;
class BridgeEditor;

class BridgedParam : public juce::AudioPluginInstance::HostedParameter
{
public:
    BridgedParam (Bridge32PluginInstance& o, int idx, const ParamInfo& pi)
        : owner (o), index (idx), cached (pi.defaultValue),
          def (pi.defaultValue),
          nm (juce::String::fromUTF8 (pi.name)),
          lb (juce::String::fromUTF8 (pi.label)) {}

    float getValue() const override                        { return cached.load(); }
    void  setValue (float v) override;
    float getDefaultValue() const override                 { return def; }
    juce::String getName (int len) const override          { return nm.substring (0, len); }
    juce::String getLabel() const override                 { return lb; }
    float getValueForText (const juce::String& t) const override { return t.getFloatValue(); }
    juce::String getParameterID() const override           { return "p" + juce::String (index); }

    std::atomic<float> cached;
    const int index;

private:
    Bridge32PluginInstance& owner;
    const float def;
    const juce::String nm, lb;
};

class Bridge32PluginInstance : public juce::AudioPluginInstance,
                               private juce::Timer
{
public:
    Bridge32PluginInstance (const Connection& c, const PluginInfo& i,
                            const juce::PluginDescription& d)
        : juce::AudioPluginInstance (makeBuses (i)),
          conn (c), info (i), storedDesc (d)
    {
        setLatencySamples (juce::jmax (0, info.initialDelay));
        fetchParams();

        juce::WeakReference<Bridge32PluginInstance> weak (this);
        juce::MessageManager::callAsync ([weak]
        {
            if (auto* self = weak.get())
                self->startTimerHz (30);
        });
    }

    ~Bridge32PluginInstance() override
    {
        stopTimer();
        if (! dead.load())
        {
            rpc (cmdShutdown, 2000);
            WaitForSingleObject (conn.process, 2000);
        }
        closeConnection (conn);
    }

    const juce::String getName() const override
    {
        auto n = juce::String::fromUTF8 (info.name);
        return n.isNotEmpty() ? n : storedDesc.name;
    }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override
    {
        preparedRate = sampleRate;
        const int block = juce::jmin (samplesPerBlock, (int) maxBlockSize);
        rpc (cmdPrepare, rpcTimeoutMs, [sampleRate, block] (SharedHeader& h)
        {
            h.sampleRate = sampleRate;
            h.blockSize  = block;
        });
        setLatencySamples (juce::jmax (0, info.initialDelay));
    }

    void releaseResources() override
    {
        rpc (cmdSuspend, rpcTimeoutMs);
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        const int totalCh = buffer.getNumChannels();
        const int total   = buffer.getNumSamples();

        if (dead.load())
        {
            buffer.clear();
            midi.clear();
            return;
        }

        auto* h = conn.header;

        double bpm = 120.0, ppq = 0.0, barStart = 0.0;
        int tsn = 4, tsd = 4; bool playing = false; juce::int64 timeSamples = 0;
        if (auto* ph = getPlayHead())
            if (auto pos = ph->getPosition())
            {
                if (auto b  = pos->getBpm())                     bpm = *b;
                if (auto p  = pos->getPpqPosition())             ppq = *p;
                if (auto bs = pos->getPpqPositionOfLastBarStart()) barStart = *bs;
                if (auto ts = pos->getTimeSignature())           { tsn = ts->numerator; tsd = ts->denominator; }
                if (auto t  = pos->getTimeInSamples())           timeSamples = *t;
                playing = pos->getIsPlaying();
            }
        const double rate = preparedRate > 0.0 ? preparedRate : getSampleRate();

        for (int off = 0; off < total; off += (int) maxBlockSize)
        {
            const int n = juce::jmin (total - off, (int) maxBlockSize);

            for (int ch = 0; ch < numIOChannels; ++ch)
            {
                if (ch < totalCh)
                    std::memcpy (h->audioIn[ch], buffer.getReadPointer (ch) + off, (size_t) n * sizeof (float));
                else
                    std::memset (h->audioIn[ch], 0, (size_t) n * sizeof (float));
            }

            int mc = 0;
            for (const auto meta : midi)
            {
                const int sp = meta.samplePosition;
                if (sp < off || sp >= off + n) continue;
                if (mc >= (int) maxMidiEvents) break;
                const int sz = meta.numBytes;
                if (sz < 1 || sz > 3) continue;
                auto& e = h->midi[mc++];
                e.sampleOffset = (uint32_t) (sp - off);
                e.size = (uint32_t) sz;
                e.data[0] = meta.data[0];
                e.data[1] = sz > 1 ? meta.data[1] : 0;
                e.data[2] = sz > 2 ? meta.data[2] : 0;
                e.data[3] = 0;
            }

            h->numSamples = n;
            h->midiCount  = mc;

            const double sliceSec = rate > 0.0 ? off / rate : 0.0;
            auto& ti = h->timeInfo;
            ti.samplePos   = (double) (timeSamples + off);
            ti.sampleRate  = rate;
            ti.ppqPos      = ppq + sliceSec * bpm / 60.0;
            ti.tempo       = bpm;
            ti.barStartPos = barStart;
            ti.timeSigNumerator   = tsn;
            ti.timeSigDenominator = tsd;
            ti.playing     = playing ? 1u : 0u;
            ti.pad = 0;

            const bool ok = rpc (cmdProcess, processTimeoutMs);
            for (int ch = 0; ch < totalCh; ++ch)
            {
                if (ok && ch < numIOChannels)
                    std::memcpy (buffer.getWritePointer (ch) + off, h->audioOut[ch], (size_t) n * sizeof (float));
                else
                    buffer.clear (ch, off, n);
            }
        }

        midi.clear();
    }

    double getTailLengthSeconds() const override    { return 0.0; }
    bool acceptsMidi() const override               { return info.isSynth != 0; }
    bool producesMidi() const override              { return false; }

    bool isBusesLayoutSupported (const BusesLayout& l) const override
    {
        return l.getMainInputChannelSet().size()  <= 2
            && l.getMainOutputChannelSet().size() <= 2;
    }

    int getNumPrograms() override                   { return juce::jmax (1, info.numPrograms); }

    int getCurrentProgram() override
    {
        if (rpc (cmdGetProgram, rpcTimeoutMs))
            cachedProgram = conn.header->intReply;
        return cachedProgram;
    }

    void setCurrentProgram (int index) override
    {
        cachedProgram = index;
        rpc (cmdSetProgram, rpcTimeoutMs, [index] (SharedHeader& h) { h.paramIndex = index; });
    }

    const juce::String getProgramName (int index) override
    {
        if (rpc (cmdGetProgramName, rpcTimeoutMs, [index] (SharedHeader& h) { h.paramIndex = index; }))
            return juce::String::fromUTF8 (conn.header->strReply);
        return {};
    }

    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& dest) override
    {
        juce::MemoryOutputStream mos (dest, false);
        mos.writeInt ((int) stateMagic);

        if (info.hasChunk && rpc (cmdGetChunk, rpcTimeoutMs)
            && conn.header->status == 0 && conn.header->intReply > 0)
        {
            const int sz = conn.header->intReply;
            if ((uint32_t) sz <= chunkAreaBytes)
            {
                mos.writeInt (1);
                mos.writeInt (sz);
                mos.write (conn.chunkArea, (size_t) sz);
                return;
            }
        }

        mos.writeInt (0);
        mos.writeInt ((int) params.size());
        for (auto* p : params)
            mos.writeFloat (p->cached.load());
    }

    void setStateInformation (const void* data, int sizeInBytes) override
    {
        if (data == nullptr || sizeInBytes < 8)
            return;
        juce::MemoryInputStream mis (data, (size_t) sizeInBytes, false);
        if ((uint32_t) mis.readInt() != stateMagic)
            return;
        const int kind = mis.readInt();

        if (kind == 1)
        {
            const int sz = mis.readInt();
            if (sz <= 0 || (uint32_t) sz > chunkAreaBytes
                || mis.getNumBytesRemaining() < (juce::int64) sz)
                return;
            mis.read (conn.chunkArea, sz);
            rpc (cmdSetChunk, rpcTimeoutMs, [sz] (SharedHeader& h) { h.dataBytes = (uint32_t) sz; });
        }
        else if (kind == 0)
        {
            const int n = mis.readInt();
            for (int i = 0; i < n && mis.getNumBytesRemaining() >= 4; ++i)
            {
                const float v = mis.readFloat();
                if (i < (int) params.size())
                {
                    params[(size_t) i]->cached = v;
                    setParamRemote (i, v);
                }
            }
        }
    }

    bool hasEditor() const override                 { return info.hasEditor != 0; }
    juce::AudioProcessorEditor* createEditor() override;

    void fillInPluginDescription (juce::PluginDescription& d) const override { d = storedDesc; }

    void setParamRemote (int index, float v)
    {
        rpc (cmdSetParam, rpcTimeoutMs, [index, v] (SharedHeader& h)
        {
            h.paramIndex = index;
            h.paramValue = v;
        });
    }

    void sendUi (uint32_t cmd, void* parentHwnd)
    {
        if (dead.load())
            return;
        conn.header->uiParentHwnd = (uint64_t) (uintptr_t) parentHwnd;
        conn.header->uiCommand = cmd;
        SetEvent (conn.uiEvent);
    }

    void* publishedEditorHwnd() const
    {
        return (void*) (uintptr_t) conn.header->editorHwnd;
    }

    juce::Rectangle<int> publishedEditorSize() const
    {
        return { 0, 0, (int) conn.header->editorWidth, (int) conn.header->editorHeight };
    }

    BridgeEditor* activeEditor = nullptr;

private:
    static BusesProperties makeBuses (const PluginInfo& i)
    {
        BusesProperties b;
        if (i.isSynth == 0)
            b = b.withInput ("In", juce::AudioChannelSet::stereo(), true);
        return b.withOutput ("Out", juce::AudioChannelSet::stereo(), true);
    }

    void fetchParams()
    {
        const int totalParams = juce::jmax (0, info.numParams);
        for (int start = 0; start < totalParams; start += (int) paramInfoPage)
        {
            if (! rpc (cmdGetParamInfo, rpcTimeoutMs,
                       [start] (SharedHeader& h) { h.paramIndex = start; }))
                break;
            const int count = conn.header->intReply;
            if (count <= 0)
                break;
            auto* arr = reinterpret_cast<const ParamInfo*> (conn.chunkArea);
            for (int k = 0; k < count && (start + k) < totalParams; ++k)
            {
                auto p = std::make_unique<BridgedParam> (*this, start + k, arr[k]);
                params.push_back (p.get());
                addHostedParameter (std::move (p));
            }
        }
    }

    bool processAlive() const
    {
        return conn.process != nullptr
            && WaitForSingleObject (conn.process, 0) == WAIT_TIMEOUT;
    }

    template <typename Fn>
    bool rpc (uint32_t cmd, uint32_t timeoutMs, Fn&& prep)
    {
        std::lock_guard<std::mutex> lk (rpcMutex);
        if (dead.load())
            return false;

        if (awaitingResponse)
        {
            const DWORD w = waitBridge (conn.respEvent, conn.process, timeoutMs);
            if (w == WAIT_OBJECT_0 + 1) { dead = true; return false; }
            if (w != WAIT_OBJECT_0)     return false;
            awaitingResponse = false;
        }

        if (! processAlive()) { dead = true; return false; }

        prep (*conn.header);
        conn.header->command = cmd;
        SetEvent (conn.reqEvent);

        const DWORD w = waitBridge (conn.respEvent, conn.process, timeoutMs);
        if (w == WAIT_OBJECT_0)     return true;
        if (w == WAIT_OBJECT_0 + 1) { dead = true; return false; }
        awaitingResponse = true;
        return false;
    }

    bool rpc (uint32_t cmd, uint32_t timeoutMs)
    {
        return rpc (cmd, timeoutMs, [] (SharedHeader&) {});
    }

    void syncEditor();

    void timerCallback() override
    {
        if (dead.load())
            return;
        syncEditor();
        auto& ring = conn.header->paramRing;
        uint32_t head = ring.head;
        uint32_t tail = ring.tail;
        if ((uint32_t) (head - tail) > (uint32_t) paramRingSize)
            tail = head - (uint32_t) paramRingSize;
        while (tail != head)
        {
            const auto& e = ring.entries[tail & ((uint32_t) paramRingSize - 1)];
            const int idx = e.index;
            const float val = e.value;
            ++tail;
            if (idx >= 0 && idx < (int) params.size())
            {
                params[(size_t) idx]->cached = val;
                params[(size_t) idx]->sendValueChangedMessageToListeners (val);
            }
        }
        ring.tail = tail;
    }

    Connection conn;
    const PluginInfo info;
    const juce::PluginDescription storedDesc;
    std::vector<BridgedParam*> params;
    std::mutex rpcMutex;
    bool awaitingResponse = false;
    std::atomic<bool> dead { false };
    double preparedRate = 0.0;
    int cachedProgram = 0;

    JUCE_DECLARE_WEAK_REFERENCEABLE (Bridge32PluginInstance)
};

void BridgedParam::setValue (float v)
{
    cached = v;
    owner.setParamRemote (index, v);
}

class BridgeEditor : public juce::AudioProcessorEditor
{
public:
    explicit BridgeEditor (Bridge32PluginInstance& o)
        : juce::AudioProcessorEditor (o), owner (o)
    {
        auto sz = o.publishedEditorSize();
        setSize (sz.getWidth()  > 0 ? sz.getWidth()  : 400,
                 sz.getHeight() > 0 ? sz.getHeight() : 300);
        status.setJustificationType (juce::Justification::centred);
        status.setColour (juce::Label::textColourId, Colors::textDim);
        status.setText ("Loading 32-bit plugin editor...", juce::dontSendNotification);
        addAndMakeVisible (status);
        addChildComponent (native);
        owner.activeEditor = this;
    }

    ~BridgeEditor() override
    {
        owner.activeEditor = nullptr;
        owner.sendUi (uiHide, nullptr);
        native.setHWND (nullptr);
    }

    void paint (juce::Graphics& g) override { g.fillAll (Colors::panelBg); }

    void resized() override
    {
        status.setBounds (getLocalBounds().reduced (12));
        native.setBounds (getLocalBounds());
    }

    void parentHierarchyChanged() override
    {
        requestFromPeer();
    }

    void visibilityChanged() override
    {
        requestFromPeer();
    }

    void adopt (void* hwnd, juce::Rectangle<int> physicalSize)
    {
        if (hwnd != native.getHWND())
        {
            native.setHWND (hwnd);
            native.setVisible (hwnd != nullptr);
            status.setVisible (hwnd == nullptr);
            if (hwnd == nullptr)
                requestedParent = nullptr;
        }
        if (hwnd == nullptr || physicalSize.isEmpty())
            return;

        double scale = 1.0;
        if (auto* peer = getPeer())
            scale = peer->getPlatformScaleFactor();
        const int w = juce::roundToInt (physicalSize.getWidth()  / scale);
        const int h = juce::roundToInt (physicalSize.getHeight() / scale);
        if (w != getWidth() || h != getHeight())
            setSize (w, h);
    }

private:
    void requestFromPeer()
    {
        auto* peer = getPeer();
        if (peer == nullptr || ! isShowing())
            return;
        void* handle = peer->getNativeHandle();
        if (handle == requestedParent)
            return;
        requestedParent = handle;
        owner.sendUi (uiShow, handle);
    }

    Bridge32PluginInstance& owner;
    juce::HWNDComponent native;
    juce::Label status;
    void* requestedParent = nullptr;
};

void Bridge32PluginInstance::syncEditor()
{
    if (activeEditor != nullptr)
        activeEditor->adopt (publishedEditorHwnd(), publishedEditorSize());
}

juce::AudioProcessorEditor* Bridge32PluginInstance::createEditor()
{
    return new BridgeEditor (*this);
}

juce::String parseValue (const juce::StringArray& lines, juce::StringRef key)
{
    const juce::String prefix = juce::String (key) + "=";
    for (auto& line : lines)
        if (line.startsWith (prefix))
            return line.fromFirstOccurrenceOf ("=", false, false);
    return {};
}

bool hasKey (const juce::StringArray& lines, juce::StringRef key)
{
    const juce::String prefix = juce::String (key) + "=";
    for (auto& line : lines)
        if (line.startsWith (prefix))
            return true;
    return false;
}

}

bool scanPlugin (const juce::File& dll, juce::PluginDescription& desc, juce::String& error)
{
    auto exe = bridgeExecutable();
    if (! exe.existsAsFile())
    {
        error = "32-bit bridge (mikeDAW-Bridge32.exe) not built";
        return false;
    }
    if (! dll.existsAsFile())
    {
        error = "plugin file not found: " + dll.getFullPathName();
        return false;
    }

    auto outFile = juce::File::createTempFile ("b32scan");

    juce::ChildProcess proc;
    juce::StringArray args;
    args.add (exe.getFullPathName());
    args.add ("--scan");
    args.add (dll.getFullPathName());
    args.add (outFile.getFullPathName());

    if (! proc.start (args))
    {
        error = "failed to start the 32-bit bridge scanner";
        outFile.deleteFile();
        return false;
    }
    if (! proc.waitForProcessToFinish (30000))
    {
        proc.kill();
        error = "32-bit bridge scan timed out";
        outFile.deleteFile();
        return false;
    }
    auto lines = juce::StringArray::fromLines (outFile.loadFileAsString());
    outFile.deleteFile();

    for (auto* key : { "name", "vendor", "uniqueId", "version", "isSynth",
                       "hasEditor", "numInputs", "numOutputs", "numParams" })
    {
        if (! hasKey (lines, key))
        {
            error = "32-bit plugin failed to load in the bridge";
            return false;
        }
    }

    auto name = parseValue (lines, "name").trim();
    desc.name = name.isNotEmpty() ? name : dll.getFileNameWithoutExtension();
    desc.descriptiveName    = desc.name;
    desc.pluginFormatName   = formatName;
    desc.category           = "Bridged 32-bit";
    desc.manufacturerName   = parseValue (lines, "vendor").trim();
    desc.version            = parseValue (lines, "version").trim();
    desc.fileOrIdentifier   = dll.getFullPathName();
    desc.lastFileModTime    = dll.getLastModificationTime();
    desc.uniqueId           = parseValue (lines, "uniqueId").getIntValue();
    desc.deprecatedUid      = desc.uniqueId;
    desc.isInstrument       = parseValue (lines, "isSynth").getIntValue() != 0;
    desc.numInputChannels   = parseValue (lines, "numInputs").getIntValue();
    desc.numOutputChannels  = parseValue (lines, "numOutputs").getIntValue();
    return true;
}

std::unique_ptr<juce::AudioPluginInstance> createInstance (
    const juce::PluginDescription& desc, double sampleRate, int blockSize, juce::String& error)
{
    juce::ignoreUnused (sampleRate, blockSize);

    juce::File dll (desc.fileOrIdentifier);
    if (! dll.existsAsFile())
    {
        error = "plugin file not found: " + dll.getFullPathName();
        return nullptr;
    }
    auto exe = bridgeExecutable();
    if (! exe.existsAsFile())
    {
        error = "32-bit bridge (mikeDAW-Bridge32.exe) not built";
        return nullptr;
    }

    static std::atomic<uint32_t> seq { 0 };
    const DWORD pid = GetCurrentProcessId();
    const juce::String base = "Local\\mikedaw.b32." + juce::String ((int) pid)
                            + "." + juce::String ((int) seq.fetch_add (1));

    Connection conn;
    conn.mapping = CreateFileMappingW (INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                       0, mappingBytes, base.toWideCharPointer());
    if (conn.mapping == nullptr)
    {
        error = "CreateFileMapping failed";
        return nullptr;
    }
    conn.view = MapViewOfFile (conn.mapping, FILE_MAP_ALL_ACCESS, 0, 0, mappingBytes);
    if (conn.view == nullptr)
    {
        error = "MapViewOfFile failed";
        closeConnection (conn);
        return nullptr;
    }
    conn.header = reinterpret_cast<SharedHeader*> (conn.view);
    ZeroMemory (conn.header, sizeof (SharedHeader));
    conn.header->magic   = magicValue;
    conn.header->version = protocolVersion;
    conn.header->state   = stateStarting;
    conn.chunkArea = reinterpret_cast<uint8_t*> (conn.view) + headerBytes;

    const juce::String reqName  = base + ".req";
    const juce::String respName = base + ".resp";
    const juce::String uiName   = base + ".ui";
    conn.reqEvent  = CreateEventW (nullptr, FALSE, FALSE, reqName.toWideCharPointer());
    conn.respEvent = CreateEventW (nullptr, FALSE, FALSE, respName.toWideCharPointer());
    conn.uiEvent   = CreateEventW (nullptr, FALSE, FALSE, uiName.toWideCharPointer());
    if (conn.reqEvent == nullptr || conn.respEvent == nullptr || conn.uiEvent == nullptr)
    {
        error = "CreateEvent failed";
        closeConnection (conn);
        return nullptr;
    }

    conn.job = CreateJobObjectW (nullptr, nullptr);
    if (conn.job != nullptr)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION ji {};
        ji.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject (conn.job, JobObjectExtendedLimitInformation, &ji, sizeof (ji));
    }

    juce::String cmd = "\"" + exe.getFullPathName() + "\" --host \""
                     + dll.getFullPathName() + "\" " + base + " " + juce::String ((int) pid);
    std::vector<wchar_t> cmdBuf (cmd.toWideCharPointer(),
                                 cmd.toWideCharPointer() + cmd.length() + 1);

    STARTUPINFOW si {}; si.cb = sizeof (si);
    PROCESS_INFORMATION procInfo {};
    if (! CreateProcessW (nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                          CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &si, &procInfo))
    {
        error = "failed to start the 32-bit bridge process";
        closeConnection (conn);
        return nullptr;
    }
    conn.process = procInfo.hProcess;
    if (conn.job != nullptr)
        AssignProcessToJobObject (conn.job, procInfo.hProcess);
    ResumeThread (procInfo.hThread);
    CloseHandle (procInfo.hThread);

    HANDLE handles[2] = { conn.respEvent, conn.process };
    const DWORD w = WaitForMultipleObjects (2, handles, FALSE, startupTimeoutMs);
    const bool ready = (w == WAIT_OBJECT_0) && conn.header->state == stateReady;
    if (! ready)
    {
        if (conn.header->loadError[0] != 0)
            error = juce::String::fromUTF8 (conn.header->loadError);
        else if (w == WAIT_OBJECT_0 + 1)
            error = "bridge process exited during startup";
        else
            error = "bridge startup timed out";
        closeConnection (conn);
        return nullptr;
    }

    const PluginInfo info = conn.header->info;
    return std::make_unique<Bridge32PluginInstance> (conn, info, desc);
}

juce::MemoryBlock wrapChunkState (const void* data, size_t size)
{
    juce::MemoryBlock out;
    if (data == nullptr || size == 0 || size > chunkAreaBytes)
        return out;
    juce::MemoryOutputStream mos (out, false);
    mos.writeInt ((int) stateMagic);
    mos.writeInt (1);
    mos.writeInt ((int) size);
    mos.write (data, size);
    return out;
}

#endif

}
