#include <windows.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstddef>
#include <vector>
#include <string>
#include <fstream>
#include <filesystem>

#include "pluginterfaces/vst2.x/aeffectx.h"
#include "Bridge32Protocol.h"

using namespace bridge32;

typedef AEffect* (VSTCALLBACK* VstEntryProc) (audioMasterCallback);

namespace
{

constexpr UINT WM_BRIDGE_SHOW     = WM_APP + 1;
constexpr UINT WM_BRIDGE_HIDE     = WM_APP + 2;
constexpr UINT WM_BRIDGE_SHUTDOWN = WM_APP + 3;

struct BridgeState
{
    SharedHeader* hdr        = nullptr;
    uint8_t*      chunkArea  = nullptr;
    HANDLE        reqEvent   = nullptr;
    HANDLE        respEvent  = nullptr;
    HANDLE        uiEvent    = nullptr;
    HMODULE       dll        = nullptr;
    AEffect*      effect     = nullptr;
    DWORD         guiThreadId = 0;

    HWND          editorWnd  = nullptr;
    bool          editorOpen = false;
    bool          editorIsChild = false;

    VstTimeInfo   timeInfo{};

    std::vector<float*>             inPtrs;
    std::vector<float*>             outPtrs;
    std::vector<float>              silence;
    std::vector<std::vector<float>> dump;
    VstEvents*     events     = nullptr;
    VstMidiEvent*  midiEvents = nullptr;
};

BridgeState bridgeState;
volatile bool pluginFaulted = false;

VstIntPtr sehDispatch (AEffect* e, VstInt32 op, VstInt32 idx, VstIntPtr val, void* ptr, float opt)
{
    __try { return e->dispatcher (e, op, idx, val, ptr, opt); }
    __except (EXCEPTION_EXECUTE_HANDLER) { pluginFaulted = true; return 0; }
}

AEffect* sehEntry (VstEntryProc entry, audioMasterCallback cb)
{
    __try { return entry (cb); }
    __except (EXCEPTION_EXECUTE_HANDLER) { pluginFaulted = true; return nullptr; }
}

void sehProcessReplacing (AEffect* e, float** in, float** out, VstInt32 n)
{
    __try { e->processReplacing (e, in, out, n); }
    __except (EXCEPTION_EXECUTE_HANDLER) { pluginFaulted = true; }
}

void sehProcess (AEffect* e, float** in, float** out, VstInt32 n)
{
    __try { e->process (e, in, out, n); }
    __except (EXCEPTION_EXECUTE_HANDLER) { pluginFaulted = true; }
}

void sehSetParam (AEffect* e, VstInt32 i, float v)
{
    __try { e->setParameter (e, i, v); }
    __except (EXCEPTION_EXECUTE_HANDLER) { pluginFaulted = true; }
}

float sehGetParam (AEffect* e, VstInt32 i)
{
    __try { return e->getParameter (e, i); }
    __except (EXCEPTION_EXECUTE_HANDLER) { pluginFaulted = true; return 0.0f; }
}

void pushParamRing (int32_t index, float value)
{
    if (bridgeState.hdr == nullptr)
        return;

    auto& ring = bridgeState.hdr->paramRing;
    const uint32_t head = ring.head;
    if (head - ring.tail >= (uint32_t) paramRingSize)
        return;

    const uint32_t slot = head & (paramRingSize - 1);
    ring.entries[slot].index = index;
    ring.entries[slot].value = value;
    MemoryBarrier();
    ring.head = head + 1;
}

void publishEditorSize (int w, int h)
{
    if (bridgeState.hdr == nullptr)
        return;
    bridgeState.hdr->editorWidth  = w;
    bridgeState.hdr->editorHeight = h;
}

void resizeEditor (int w, int h)
{
    if (bridgeState.editorWnd == nullptr || w <= 0 || h <= 0)
        return;

    publishEditorSize (w, h);
    RECT r { 0, 0, w, h };
    if (! bridgeState.editorIsChild)
        AdjustWindowRect (&r, (DWORD) GetWindowLongPtrW (bridgeState.editorWnd, GWL_STYLE), FALSE);
    SetWindowPos (bridgeState.editorWnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                  SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

VstIntPtr VSTCALLBACK hostCallback (AEffect*, VstInt32 opcode, VstInt32 index,
                                    VstIntPtr value, void* ptr, float opt)
{
    switch (opcode)
    {
        case audioMasterVersion:                 return 2400;
        case audioMasterCurrentId:               return 0;
        case audioMasterGetTime:                 return (VstIntPtr) &bridgeState.timeInfo;
        case audioMasterAutomate:                pushParamRing (index, opt); return 0;
        case audioMasterSizeWindow:              resizeEditor ((int) index, (int) value); return 1;
        case audioMasterIOChanged:               return 1;
        case audioMasterUpdateDisplay:           return 1;
        case audioMasterGetCurrentProcessLevel:  return kVstProcessLevelUnknown;
        case audioMasterGetAutomationState:      return kVstAutomationUnsupported;
        case audioMasterGetSampleRate:           return bridgeState.hdr ? (VstIntPtr) bridgeState.hdr->sampleRate : 44100;
        case audioMasterGetBlockSize:            return bridgeState.hdr ? bridgeState.hdr->blockSize : maxBlockSize;
        case audioMasterGetVendorVersion:        return 1;
        case audioMasterBeginEdit:
        case audioMasterEndEdit:                 return 1;

        case audioMasterGetVendorString:
            if (ptr) vst_strncpy ((char*) ptr, "mikeDAW", kVstMaxVendorStrLen);
            return 1;
        case audioMasterGetProductString:
            if (ptr) vst_strncpy ((char*) ptr, "mikeDAW-Bridge32", kVstMaxProductStrLen);
            return 1;

        case audioMasterCanDo:
            if (ptr != nullptr)
            {
                const char* s = (const char*) ptr;
                if (strcmp (s, "sendVstEvents") == 0 || strcmp (s, "sendVstMidiEvent") == 0
                    || strcmp (s, "sizeWindow") == 0 || strcmp (s, "sendVstTimeInfo") == 0)
                    return 1;
            }
            return 0;

        default:
            return 0;
    }
}

VstEntryProc resolveEntry (HMODULE dll)
{
    if (dll == nullptr)
        return nullptr;
    if (auto p = (VstEntryProc) GetProcAddress (dll, "VSTPluginMain"))
        return p;
    return (VstEntryProc) GetProcAddress (dll, "main");
}

void readEffectString (AEffect* eff, VstInt32 opcode, VstInt32 index, char* dst, size_t dstSize)
{
    char buf[256] = { 0 };
    sehDispatch (eff, opcode, index, 0, buf, 0);
    if (dstSize > 0)
    {
        strncpy (dst, buf, dstSize - 1);
        dst[dstSize - 1] = 0;
    }
}

const wchar_t* const kEditorClass = L"mikeDAWBridge32Editor";

void closeEditor()
{
    if (bridgeState.editorWnd == nullptr)
        return;
    KillTimer (bridgeState.editorWnd, 1);
    if (bridgeState.hdr != nullptr)
    {
        bridgeState.hdr->editorHwnd    = 0;
        bridgeState.hdr->editorVisible = 0;
    }
    if (bridgeState.effect != nullptr && bridgeState.editorOpen)
        sehDispatch (bridgeState.effect, effEditClose, 0, 0, nullptr, 0);
    DestroyWindow (bridgeState.editorWnd);
    bridgeState.editorWnd     = nullptr;
    bridgeState.editorOpen    = false;
    bridgeState.editorIsChild = false;
}

LRESULT CALLBACK editorWndProc (HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
        case WM_TIMER:
            if (bridgeState.effect != nullptr && bridgeState.editorOpen)
                sehDispatch (bridgeState.effect, effEditIdle, 0, 0, nullptr, 0);
            return 0;
        case WM_CLOSE:
            closeEditor();
            return 0;
        default:
            return DefWindowProcW (hwnd, msg, wp, lp);
    }
}

void ensureEditorClass()
{
    static bool registered = false;
    if (registered)
        return;
    WNDCLASSEXW wc { sizeof (WNDCLASSEXW) };
    wc.lpfnWndProc   = editorWndProc;
    wc.hInstance     = GetModuleHandleW (nullptr);
    wc.hCursor       = LoadCursor (nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH) (COLOR_WINDOW + 1);
    wc.lpszClassName = kEditorClass;
    RegisterClassExW (&wc);
    registered = true;
}

void editorRectSize (AEffect* eff, int& w, int& h)
{
    ERect* rect = nullptr;
    sehDispatch (eff, effEditGetRect, 0, 0, &rect, 0);
    if (rect != nullptr && rect->right > rect->left && rect->bottom > rect->top)
    {
        w = rect->right - rect->left;
        h = rect->bottom - rect->top;
    }
}

void showEditor (HWND parent)
{
    if (bridgeState.effect == nullptr)
        return;

    if (bridgeState.editorWnd != nullptr)
    {
        const HWND currentParent = bridgeState.editorIsChild ? GetParent (bridgeState.editorWnd) : nullptr;
        if (currentParent == parent)
        {
            ShowWindow (bridgeState.editorWnd, bridgeState.editorIsChild ? SW_SHOWNA : SW_SHOW);
            if (! bridgeState.editorIsChild)
                SetForegroundWindow (bridgeState.editorWnd);
            if (bridgeState.hdr != nullptr) bridgeState.hdr->editorVisible = 1;
            return;
        }
        closeEditor();
    }

    ensureEditorClass();

    int w = 400, h = 300;
    editorRectSize (bridgeState.effect, w, h);

    wchar_t title[160];
    MultiByteToWideChar (CP_ACP, 0, bridgeState.hdr ? bridgeState.hdr->info.name : "32-bit plugin", -1,
                         title, (int) (sizeof (title) / sizeof (wchar_t)));

    HWND wnd = nullptr;
    if (parent != nullptr)
    {
        wnd = CreateWindowExW (0, kEditorClass, title,
                               WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                               0, 0, w, h, parent, nullptr, GetModuleHandleW (nullptr), nullptr);
        bridgeState.editorIsChild = wnd != nullptr;
    }
    if (wnd == nullptr)
    {
        const DWORD style = WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_THICKFRAME);
        RECT wr { 0, 0, w, h };
        AdjustWindowRect (&wr, style, FALSE);
        wnd = CreateWindowExW (0, kEditorClass, title, style,
                               CW_USEDEFAULT, CW_USEDEFAULT,
                               wr.right - wr.left, wr.bottom - wr.top,
                               nullptr, nullptr, GetModuleHandleW (nullptr), nullptr);
        bridgeState.editorIsChild = false;
    }
    bridgeState.editorWnd = wnd;
    if (wnd == nullptr)
        return;

    sehDispatch (bridgeState.effect, effEditOpen, 0, 0, wnd, 0);
    bridgeState.editorOpen = true;

    int w2 = w, h2 = h;
    editorRectSize (bridgeState.effect, w2, h2);
    publishEditorSize (w2, h2);
    if (w2 != w || h2 != h)
        resizeEditor (w2, h2);

    SetTimer (wnd, 1, 50, nullptr);
    if (bridgeState.editorIsChild)
    {
        ShowWindow (wnd, SW_SHOWNA);
    }
    else
    {
        ShowWindow (wnd, SW_SHOW);
        SetForegroundWindow (wnd);
    }

    if (bridgeState.hdr != nullptr)
    {
        bridgeState.hdr->editorVisible = 1;
        bridgeState.hdr->editorHwnd    = (uint64_t) (uintptr_t) wnd;
    }
}

void fillInfo (AEffect* eff, PluginInfo& info)
{
    info.numParams   = eff->numParams;
    info.numPrograms = eff->numPrograms;
    info.numInputs   = eff->numInputs;
    info.numOutputs  = eff->numOutputs;
    info.initialDelay = eff->initialDelay;
    info.uniqueId    = eff->uniqueID;
    info.version     = eff->version;
    info.hasEditor   = (eff->flags & effFlagsHasEditor)     ? 1u : 0u;
    info.hasChunk    = (eff->flags & effFlagsProgramChunks) ? 1u : 0u;
    info.isSynth     = (eff->flags & effFlagsIsSynth)       ? 1u : 0u;

    readEffectString (eff, effGetEffectName, 0, info.name, sizeof (info.name));
    if (info.name[0] == 0)
        strncpy (info.name, "VST2 Plugin", sizeof (info.name) - 1);
    readEffectString (eff, effGetVendorString, 0, info.vendor, sizeof (info.vendor));
}

void setupBuffers (AEffect* eff)
{
    const int ni = eff->numInputs;
    const int no = eff->numOutputs;

    bridgeState.silence.assign ((size_t) maxBlockSize, 0.0f);

    bridgeState.inPtrs.resize ((size_t) (ni > 0 ? ni : 0));
    for (int i = 0; i < ni; ++i)
        bridgeState.inPtrs[(size_t) i] = (i < numIOChannels) ? bridgeState.hdr->audioIn[i] : bridgeState.silence.data();

    bridgeState.dump.clear();
    if (no > numIOChannels)
        bridgeState.dump.resize ((size_t) (no - numIOChannels), std::vector<float> ((size_t) maxBlockSize, 0.0f));

    bridgeState.outPtrs.resize ((size_t) (no > 0 ? no : 0));
    for (int i = 0; i < no; ++i)
        bridgeState.outPtrs[(size_t) i] = (i < numIOChannels) ? bridgeState.hdr->audioOut[i]
                                                    : bridgeState.dump[(size_t) (i - numIOChannels)].data();

    bridgeState.events = (VstEvents*) malloc (offsetof (VstEvents, events) + sizeof (VstEvent*) * (size_t) maxMidiEvents);
    bridgeState.midiEvents = (VstMidiEvent*) malloc (sizeof (VstMidiEvent) * (size_t) maxMidiEvents);
}

void doProcess (SharedHeader& h, AEffect* eff)
{
    int n = h.numSamples;
    if (n < 0) n = 0;
    if (n > maxBlockSize) n = maxBlockSize;

    bridgeState.timeInfo = VstTimeInfo{};
    bridgeState.timeInfo.samplePos          = h.timeInfo.samplePos;
    bridgeState.timeInfo.sampleRate         = h.timeInfo.sampleRate;
    bridgeState.timeInfo.ppqPos             = h.timeInfo.ppqPos;
    bridgeState.timeInfo.tempo              = h.timeInfo.tempo;
    bridgeState.timeInfo.barStartPos        = h.timeInfo.barStartPos;
    bridgeState.timeInfo.timeSigNumerator   = h.timeInfo.timeSigNumerator;
    bridgeState.timeInfo.timeSigDenominator = h.timeInfo.timeSigDenominator;
    bridgeState.timeInfo.flags = kVstTempoValid | kVstPpqPosValid | kVstTimeSigValid | kVstBarsValid;
    if (h.timeInfo.playing)
        bridgeState.timeInfo.flags |= kVstTransportPlaying;

    int mc = h.midiCount;
    if (mc < 0) mc = 0;
    if (mc > maxMidiEvents) mc = maxMidiEvents;

    if (mc > 0 && bridgeState.events != nullptr && bridgeState.midiEvents != nullptr)
    {
        for (int i = 0; i < mc; ++i)
        {
            VstMidiEvent& m = bridgeState.midiEvents[i];
            memset (&m, 0, sizeof (VstMidiEvent));
            m.type        = kVstMidiType;
            m.byteSize    = sizeof (VstMidiEvent);
            m.deltaFrames = (VstInt32) h.midi[i].sampleOffset;
            m.midiData[0] = (char) h.midi[i].data[0];
            m.midiData[1] = (char) h.midi[i].data[1];
            m.midiData[2] = (char) h.midi[i].data[2];
            bridgeState.events->events[i] = (VstEvent*) &m;
        }
        bridgeState.events->numEvents = mc;
        bridgeState.events->reserved  = 0;
        sehDispatch (eff, effProcessEvents, 0, 0, bridgeState.events, 0);
    }

    if (eff->flags & effFlagsCanReplacing)
    {
        sehProcessReplacing (eff, bridgeState.inPtrs.data(), bridgeState.outPtrs.data(), n);
    }
    else
    {
        for (int c = 0; c < numIOChannels; ++c)
            memset (h.audioOut[c], 0, sizeof (float) * (size_t) n);
        sehProcess (eff, bridgeState.inPtrs.data(), bridgeState.outPtrs.data(), n);
    }
}

void doGetParamInfo (SharedHeader& h, AEffect* eff)
{
    ParamInfo* arr = (ParamInfo*) bridgeState.chunkArea;
    int count = 0;
    for (int p = h.paramIndex; p < eff->numParams && count < paramInfoPage; ++p, ++count)
    {
        ParamInfo& pi = arr[count];
        memset (&pi, 0, sizeof (ParamInfo));
        readEffectString (eff, effGetParamName,  p, pi.name,  sizeof (pi.name));
        readEffectString (eff, effGetParamLabel, p, pi.label, sizeof (pi.label));
        pi.defaultValue = sehGetParam (eff, p);
    }
    h.intReply = count;
    h.status   = 0;
}

void doGetChunk (SharedHeader& h, AEffect* eff)
{
    if (! (eff->flags & effFlagsProgramChunks))
    {
        h.intReply = -1;
        h.status   = 0;
        return;
    }

    void* ptr = nullptr;
    VstIntPtr size = sehDispatch (eff, effGetChunk, 0, 0, &ptr, 0);
    if (size <= 0 || ptr == nullptr)
    {
        h.intReply = -1;
        h.status   = 0;
    }
    else if ((uint32_t) size > chunkAreaBytes)
    {
        h.intReply = -1;
        h.status   = 1;
    }
    else
    {
        memcpy (bridgeState.chunkArea, ptr, (size_t) size);
        h.intReply = (int32_t) size;
        h.status   = 0;
    }
}

void doSetChunk (SharedHeader& h, AEffect* eff)
{
    if (! (eff->flags & effFlagsProgramChunks))
    {
        h.intReply = -1;
        h.status   = 0;
        return;
    }
    if (h.dataBytes > chunkAreaBytes)
    {
        h.status = 1;
        return;
    }
    sehDispatch (eff, effSetChunk, 0, (VstIntPtr) h.dataBytes, bridgeState.chunkArea, 0);
    h.status = 0;
}

void doGetProgramName (SharedHeader& h, AEffect* eff)
{
    const VstInt32 program = h.paramIndex;
    char buf[kVstMaxProgNameLen + 8] = { 0 };

    VstIntPtr ok = sehDispatch (eff, effGetProgramNameIndexed, program, 0, buf, 0);
    if (! ok || buf[0] == 0)
    {
        const VstIntPtr current = sehDispatch (eff, effGetProgram, 0, 0, nullptr, 0);
        sehDispatch (eff, effSetProgram, 0, program, nullptr, 0);
        memset (buf, 0, sizeof (buf));
        sehDispatch (eff, effGetProgramName, 0, 0, buf, 0);
        sehDispatch (eff, effSetProgram, 0, current, nullptr, 0);
    }
    strncpy (h.strReply, buf, sizeof (h.strReply) - 1);
    h.strReply[sizeof (h.strReply) - 1] = 0;
    h.status = 0;
}

void bailIfFaulted()
{
    if (! pluginFaulted)
        return;
    if (bridgeState.hdr != nullptr)
    {
        strncpy (bridgeState.hdr->loadError, "32-bit plugin faulted", sizeof (bridgeState.hdr->loadError) - 1);
        bridgeState.hdr->status = 3;
        bridgeState.hdr->state  = stateDead;
    }
    SetEvent (bridgeState.respEvent);
    ExitProcess (1);
}

void handleCommand()
{
    SharedHeader& h = *bridgeState.hdr;
    AEffect* eff = bridgeState.effect;

    switch (h.command)
    {
        case cmdPrepare:
            sehDispatch (eff, effSetSampleRate, 0, 0, nullptr, (float) h.sampleRate);
            sehDispatch (eff, effSetBlockSize, 0, (VstIntPtr) h.blockSize, nullptr, 0);
            sehDispatch (eff, effMainsChanged, 0, 1, nullptr, 0);
            h.status = 0;
            break;

        case cmdSuspend:
            sehDispatch (eff, effMainsChanged, 0, 0, nullptr, 0);
            h.status = 0;
            break;

        case cmdProcess:
            doProcess (h, eff);
            h.status = 0;
            break;

        case cmdSetParam:
            sehSetParam (eff, h.paramIndex, h.paramValue);
            h.status = 0;
            break;

        case cmdGetParam:
            h.floatReply = sehGetParam (eff, h.paramIndex);
            h.status = 0;
            break;

        case cmdGetParamInfo:  doGetParamInfo (h, eff); break;
        case cmdGetChunk:      doGetChunk (h, eff); break;
        case cmdSetChunk:      doSetChunk (h, eff); break;

        case cmdGetProgram:
            h.intReply = (int32_t) sehDispatch (eff, effGetProgram, 0, 0, nullptr, 0);
            h.status = 0;
            break;

        case cmdSetProgram:
            sehDispatch (eff, effSetProgram, 0, (VstIntPtr) h.paramIndex, nullptr, 0);
            h.status = 0;
            break;

        case cmdGetProgramName: doGetProgramName (h, eff); break;

        case cmdShowEditor:
            PostThreadMessageW (bridgeState.guiThreadId, WM_BRIDGE_SHOW, 0, 0);
            h.status = 0;
            break;

        case cmdHideEditor:
            PostThreadMessageW (bridgeState.guiThreadId, WM_BRIDGE_HIDE, 0, 0);
            h.status = 0;
            break;

        case cmdShutdown:
            PostThreadMessageW (bridgeState.guiThreadId, WM_BRIDGE_SHUTDOWN, 0, 0);
            return;

        default:
            h.status = 0;
            break;
    }

    bailIfFaulted();

    h.command = cmdNone;
    SetEvent (bridgeState.respEvent);
}

DWORD WINAPI rpcThread (LPVOID)
{
    SetThreadPriority (GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    for (;;)
    {
        if (WaitForSingleObject (bridgeState.reqEvent, INFINITE) != WAIT_OBJECT_0)
            return 0;
        handleCommand();
    }
}

void shutdownFromGui()
{
    SharedHeader& h = *bridgeState.hdr;
    closeEditor();
    if (bridgeState.effect != nullptr)
        sehDispatch (bridgeState.effect, effClose, 0, 0, nullptr, 0);
    h.status  = 0;
    h.command = cmdNone;
    SetEvent (bridgeState.respEvent);
    ExitProcess (0);
}

void handleUiCommand()
{
    SharedHeader& h = *bridgeState.hdr;
    const uint32_t cmd = h.uiCommand;
    h.uiCommand = uiNone;

    switch (cmd)
    {
        case uiShow: showEditor ((HWND) (uintptr_t) h.uiParentHwnd); break;
        case uiHide: closeEditor(); break;
        default: break;
    }
    bailIfFaulted();
}

void pumpMessages()
{
    MSG msg;
    while (PeekMessageW (&msg, nullptr, 0, 0, PM_REMOVE))
    {
        if (msg.hwnd == nullptr)
        {
            switch (msg.message)
            {
                case WM_BRIDGE_SHOW:     showEditor (nullptr); continue;
                case WM_BRIDGE_HIDE:     closeEditor();        continue;
                case WM_BRIDGE_SHUTDOWN: shutdownFromGui();    continue;
                default: break;
            }
        }
        TranslateMessage (&msg);
        DispatchMessageW (&msg);
    }
    bailIfFaulted();
}

void guiLoop()
{
    for (;;)
    {
        DWORD r = MsgWaitForMultipleObjectsEx (1, &bridgeState.uiEvent, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (r == WAIT_OBJECT_0)
            handleUiCommand();
        else if (r == WAIT_OBJECT_0 + 1)
            pumpMessages();
        else
            return;
    }
}

DWORD WINAPI watchdogThread (LPVOID param)
{
    const DWORD pid = (DWORD) (uintptr_t) param;
    HANDLE hp = OpenProcess (SYNCHRONIZE, FALSE, pid);
    if (hp != nullptr)
    {
        WaitForSingleObject (hp, INFINITE);
        CloseHandle (hp);
    }
    ExitProcess (0);
    return 0;
}

int reportFatal (const char* msg)
{
    if (bridgeState.hdr != nullptr)
    {
        strncpy (bridgeState.hdr->loadError, msg, sizeof (bridgeState.hdr->loadError) - 1);
        bridgeState.hdr->loadError[sizeof (bridgeState.hdr->loadError) - 1] = 0;
        bridgeState.hdr->state = stateDead;
        if (bridgeState.respEvent != nullptr)
            SetEvent (bridgeState.respEvent);
    }
    return 1;
}

void matchHostDpiAwareness()
{
    typedef BOOL (WINAPI* SetCtxFn) (HANDLE);
    if (HMODULE user32 = GetModuleHandleW (L"user32.dll"))
        if (auto fn = (SetCtxFn) GetProcAddress (user32, "SetProcessDpiAwarenessContext"))
            if (fn ((HANDLE) -4))
                return;
    typedef BOOL (WINAPI* SetAwareFn)();
    if (HMODULE user32 = GetModuleHandleW (L"user32.dll"))
        if (auto fn = (SetAwareFn) GetProcAddress (user32, "SetProcessDPIAware"))
            fn();
}

int runHost (const std::wstring& dllPath, const std::wstring& shmBase, DWORD hostPid)
{
    matchHostDpiAwareness();
    SetPriorityClass (GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    CreateThread (nullptr, 0, watchdogThread, (LPVOID) (uintptr_t) hostPid, 0, nullptr);

    HANDLE mapping = OpenFileMappingW (FILE_MAP_ALL_ACCESS, FALSE, shmBase.c_str());
    if (mapping == nullptr)
        return 2;

    void* view = MapViewOfFile (mapping, FILE_MAP_ALL_ACCESS, 0, 0, mappingBytes);
    if (view == nullptr)
        return 2;

    bridgeState.hdr       = (SharedHeader*) view;
    bridgeState.chunkArea = (uint8_t*) view + headerBytes;

    if (bridgeState.hdr->magic != magicValue || bridgeState.hdr->version != protocolVersion)
        return 2;

    const std::wstring reqName  = shmBase + L".req";
    const std::wstring respName = shmBase + L".resp";
    const std::wstring uiName   = shmBase + L".ui";
    bridgeState.reqEvent  = OpenEventW (EVENT_ALL_ACCESS, FALSE, reqName.c_str());
    bridgeState.respEvent = OpenEventW (EVENT_ALL_ACCESS, FALSE, respName.c_str());
    bridgeState.uiEvent   = OpenEventW (EVENT_ALL_ACCESS, FALSE, uiName.c_str());
    if (bridgeState.reqEvent == nullptr || bridgeState.respEvent == nullptr || bridgeState.uiEvent == nullptr)
        return reportFatal ("failed to open bridge sync events");

    bridgeState.guiThreadId = GetCurrentThreadId();
    MSG dummy;
    PeekMessageW (&dummy, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    bridgeState.dll = LoadLibraryW (dllPath.c_str());
    if (bridgeState.dll == nullptr)
        return reportFatal ("LoadLibrary failed for 32-bit plugin");

    VstEntryProc entry = resolveEntry (bridgeState.dll);
    if (entry == nullptr)
        return reportFatal ("no VSTPluginMain/main export");

    AEffect* eff = sehEntry (entry, hostCallback);
    if (eff == nullptr || eff->magic != kEffectMagic)
        return reportFatal ("plugin entry did not return a valid AEffect");

    bridgeState.effect = eff;
    sehDispatch (eff, effOpen, 0, 0, nullptr, 0);
    if (pluginFaulted)
        return reportFatal ("plugin faulted during effOpen");

    fillInfo (eff, bridgeState.hdr->info);
    setupBuffers (eff);

    if (bridgeState.hdr->info.hasEditor)
    {
        int w = 0, h = 0;
        editorRectSize (eff, w, h);
        publishEditorSize (w, h);
        pluginFaulted = false;
    }

    if (CreateThread (nullptr, 0, rpcThread, nullptr, 0, nullptr) == nullptr)
        return reportFatal ("failed to start the bridge RPC thread");

    bridgeState.hdr->state = stateReady;
    SetEvent (bridgeState.respEvent);

    guiLoop();
    return 0;
}

int runScan (const std::wstring& dllPath, const std::wstring& outPath)
{
    HMODULE dll = LoadLibraryW (dllPath.c_str());
    if (dll == nullptr)
        return 2;

    VstEntryProc entry = resolveEntry (dll);
    if (entry == nullptr)
        return 3;

    AEffect* eff = sehEntry (entry, hostCallback);
    if (eff == nullptr || eff->magic != kEffectMagic)
        return 4;

    bridgeState.effect = eff;
    sehDispatch (eff, effOpen, 0, 0, nullptr, 0);
    if (pluginFaulted)
        return 5;

    char name[128]   = { 0 };
    char vendor[128] = { 0 };
    readEffectString (eff, effGetEffectName, 0, name, sizeof (name));
    readEffectString (eff, effGetVendorString, 0, vendor, sizeof (vendor));
    if (name[0] == 0)
        strncpy (name, "VST2 Plugin", sizeof (name) - 1);

    const bool isSynth   = (eff->flags & effFlagsIsSynth) != 0;
    const bool hasEditor = (eff->flags & effFlagsHasEditor) != 0;

    if (pluginFaulted)
        return 6;

    std::ofstream out (std::filesystem::path (outPath), std::ios::binary | std::ios::trunc);
    if (! out)
        return 7;

    out << "name="       << name << "\n";
    out << "vendor="     << vendor << "\n";
    out << "uniqueId="   << (int32_t) eff->uniqueID << "\n";
    out << "version="    << (int32_t) eff->version << "\n";
    out << "isSynth="    << (isSynth ? 1 : 0) << "\n";
    out << "hasEditor="  << (hasEditor ? 1 : 0) << "\n";
    out << "numInputs="  << (int32_t) eff->numInputs << "\n";
    out << "numOutputs=" << (int32_t) eff->numOutputs << "\n";
    out << "numParams="  << (int32_t) eff->numParams << "\n";
    out.flush();
    out.close();

    sehDispatch (eff, effClose, 0, 0, nullptr, 0);

    TerminateProcess (GetCurrentProcess(), 0);
    return 0;
}

}

int wmain (int argc, wchar_t** argv)
{
    if (argc >= 4 && wcscmp (argv[1], L"--scan") == 0)
        return runScan (argv[2], argv[3]);

    if (argc >= 5 && wcscmp (argv[1], L"--host") == 0)
    {
        const DWORD hostPid = (DWORD) _wtoi64 (argv[4]);
        return runHost (argv[2], argv[3], hostPid);
    }

    return 1;
}
