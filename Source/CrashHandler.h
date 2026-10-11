#pragma once
#include <JuceHeader.h>
#include "UI/LoadProgress.h"
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
 #include <dbghelp.h>
 #include <csignal>
 #include <cstdlib>
 #include <exception>
 #pragma comment (lib, "dbghelp.lib")
#endif

namespace CrashHandler
{
    inline juce::File logDir()
    {
        auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                       .getChildFile ("mikeDAW").getChildFile ("CrashLogs");
        dir.createDirectory();
        return dir;
    }

   #if JUCE_WINDOWS
    namespace detail
    {
        inline std::atomic<EXCEPTION_POINTERS*> fatalEp { nullptr };
        inline std::atomic<bool> crashActive { false };
        inline std::atomic<bool> dumpClaimed { false };
        inline juce::WaitableEvent crashTrigger;
        inline juce::WaitableEvent dumpFinished { true };
        inline std::atomic<bool> dumpDone { false };

        inline juce::String fatalNote;
        inline std::atomic<int> invalidParamReports { 0 };
        inline std::atomic<bool> invalidParamRecorded { false };
        inline char firstInvalidParam[512] = {};
        inline std::atomic<bool> invalidParamPublished { false };

        inline std::atomic<DWORD>       crThread { 0 };
        inline std::atomic<DWORD>       crCode   { 0 };
        inline std::atomic<uintptr_t>  crAddr   { 0 };
    }

    inline bool isFaultCode (DWORD code) noexcept
    {
        switch (code)
        {
            case EXCEPTION_ACCESS_VIOLATION:
            case EXCEPTION_ILLEGAL_INSTRUCTION:
            case EXCEPTION_INT_DIVIDE_BY_ZERO:
            case EXCEPTION_STACK_OVERFLOW:
            case EXCEPTION_IN_PAGE_ERROR:
            case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
            case EXCEPTION_DATATYPE_MISALIGNMENT:
            case EXCEPTION_PRIV_INSTRUCTION:
            case EXCEPTION_NONCONTINUABLE_EXCEPTION:
                return true;
            default:
                return false;
        }
    }

    inline bool isFailFastCode (DWORD code) noexcept
    {
        return code == 0xC0000409
            || code == 0xC0000374
            || code == 0x40000015;
    }

    inline void writeDump (const juce::String& kind, EXCEPTION_POINTERS* ep,
                           const juce::String& note, bool fullMemory);

    inline void waitForDump (int maxMs) noexcept
    {
        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) maxMs;
        while (! detail::dumpDone.load (std::memory_order_acquire)
               && juce::Time::getMillisecondCounter() < deadline)
            detail::dumpFinished.wait (250);
    }

    inline void dumpAndDie (const juce::String& kind, EXCEPTION_POINTERS* ep,
                            const juce::String& note, UINT exitCode) noexcept
    {
        bool expected = false;
        if (detail::dumpClaimed.compare_exchange_strong (expected, true))
        {
            detail::crashActive.store (true, std::memory_order_release);
            writeDump (kind, ep, note, true);
            detail::dumpDone.store (true, std::memory_order_release);
            detail::dumpFinished.signal();
        }
        else
        {
            waitForDump (120000);
        }
        ::TerminateProcess (::GetCurrentProcess(), exitCode);
    }

    inline void dumpOffThreadAndDie (EXCEPTION_POINTERS* ep, const juce::String& note,
                                     UINT exitCode) noexcept
    {
        bool none = false;
        if (detail::dumpClaimed.compare_exchange_strong (none, true))
        {
            detail::crashActive.store (true, std::memory_order_release);
            detail::fatalNote = note;
            detail::fatalEp.store (ep);
            detail::crashTrigger.signal();
        }
        waitForDump (120000);
        ::TerminateProcess (::GetCurrentProcess(), exitCode);
        for (;;)
            ::WaitForSingleObject (::GetCurrentThread(), INFINITE);
    }

    inline LONG CALLBACK firstChanceVeh (EXCEPTION_POINTERS* ep) noexcept
    {
        const DWORD code = ep->ExceptionRecord->ExceptionCode;
        if (isFailFastCode (code))
            dumpOffThreadAndDie (ep,
                                 juce::String::formatted ("Fail-fast status 0x%08x (no unhandled-exception "
                                                          "filter is ever called for this).", (unsigned int) code),
                                 code);

        if (isFaultCode (code))
        {
            detail::crThread = ::GetCurrentThreadId();
            detail::crCode   = code;
            detail::crAddr   = (uintptr_t) ep->ExceptionRecord->ExceptionAddress;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    inline void writeDump (const juce::String& kind, EXCEPTION_POINTERS* ep,
                           const juce::String& note, bool fullMemory)
    {
        auto base = logDir().getChildFile (
            kind + "-" + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S"));

        auto dumpFile = base.withFileExtension ("dmp");
        HANDLE file = ::CreateFileW (dumpFile.getFullPathName().toWideCharPointer(),
                                     GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            MINIDUMP_EXCEPTION_INFORMATION mei { ::GetCurrentThreadId(), ep,
                                                 FALSE };
            ::MiniDumpWriteDump (::GetCurrentProcess(), ::GetCurrentProcessId(), file,
                                 MINIDUMP_TYPE (fullMemory
                                                  ? (MiniDumpWithFullMemory | MiniDumpWithThreadInfo
                                                     | MiniDumpWithHandleData | MiniDumpWithUnloadedModules)
                                                  : (MiniDumpWithThreadInfo | MiniDumpWithDataSegs)),
                                 ep != nullptr ? &mei : nullptr, nullptr, nullptr);
            ::CloseHandle (file);
        }

        juce::String txt;
        txt << "mikeDAW " << kind << " - "
            << juce::Time::getCurrentTime().toString (true, true) << "\n" << note << "\n";

        if (ep != nullptr)
        {
            auto* er = ep->ExceptionRecord;
            auto* ctx = ep->ContextRecord;
            txt << juce::String::formatted ("Exception code 0x%08x at %p\n",
                                            (unsigned int) er->ExceptionCode,
                                            er->ExceptionAddress);
            HMODULE mod = nullptr;
            if (::GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                          | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                      (LPCWSTR) er->ExceptionAddress, &mod))
            {
                wchar_t name[MAX_PATH] = {};
                ::GetModuleFileNameW (mod, name, MAX_PATH);
                txt << "Faulting module: " << juce::String (name) << "\n";
            }
            if (ctx != nullptr)
            {
                txt << juce::String::formatted (
                    "RAX=%016llx RBX=%016llx RCX=%016llx RDX=%016llx\n"
                    "RSI=%016llx RDI=%016llx RBP=%016llx RSP=%016llx\n"
                    "R8 =%016llx R9 =%016llx R10=%016llx R11=%016llx\n"
                    "R12=%016llx R13=%016llx R14=%016llx R15=%016llx\n"
                    "RIP=%016llx RFLAGS=%08llx\n",
                    (unsigned long long) ctx->Rax, (unsigned long long) ctx->Rbx,
                    (unsigned long long) ctx->Rcx, (unsigned long long) ctx->Rdx,
                    (unsigned long long) ctx->Rsi, (unsigned long long) ctx->Rdi,
                    (unsigned long long) ctx->Rbp, (unsigned long long) ctx->Rsp,
                    (unsigned long long) ctx->R8,  (unsigned long long) ctx->R9,
                    (unsigned long long) ctx->R10, (unsigned long long) ctx->R11,
                    (unsigned long long) ctx->R12, (unsigned long long) ctx->R13,
                    (unsigned long long) ctx->R14, (unsigned long long) ctx->R15,
                    (unsigned long long) ctx->Rip, (unsigned long long) ctx->EFlags);
            }
            if (detail::crCode.load() != 0)
            {
                txt << juce::String::formatted (
                        "First-chance breadcrumb: code 0x%08x at %p on thread 0x%x\n",
                        detail::crCode.load(), (void*) detail::crAddr.load(),
                        (unsigned int) detail::crThread.load());
            }
            else
            {
                detail::crCode = er->ExceptionCode;
                detail::crAddr = (uintptr_t) er->ExceptionAddress;
                detail::crThread = ::GetCurrentThreadId();
            }
        }
        else
        {
            if (detail::crCode.load() != 0)
            {
                txt << juce::String::formatted (
                        "First-chance breadcrumb: code 0x%08x at %p on thread 0x%x\n",
                        detail::crCode.load(), (void*) detail::crAddr.load(),
                        (unsigned int) detail::crThread.load());
            }
        }
        if (detail::invalidParamPublished.load (std::memory_order_acquire))
            txt << "First CRT invalid parameter (" << detail::invalidParamReports.load()
                << " reported): " << juce::String (detail::firstInvalidParam) << "\n";
        txt << "Open the .dmp next to this file in Visual Studio or WinDbg for all thread stacks.\n";
        base.withFileExtension ("txt").replaceWithText (txt);
    }

    inline void secondChancePark();

    class DumpThread : private juce::Thread
    {
    public:
        DumpThread() : juce::Thread ("Crash Dump")
        {
            startThread();
        }

        ~DumpThread() override
        {
            stopThread (3000);
        }

        static DumpThread& instance()
        {
            static DumpThread t;
            return t;
        }

    private:
        void run() override
        {
            while (! threadShouldExit())
            {
                if (detail::crashTrigger.wait (1000))
                {
                    if (auto* ep = detail::fatalEp.exchange (nullptr); ep != nullptr)
                    {
                        writeDump ("crash", ep,
                                   detail::fatalNote.isNotEmpty() ? detail::fatalNote
                                                                  : juce::String ("Unhandled exception."),
                                   true);
                        detail::dumpDone.store (true, std::memory_order_release);
                        detail::dumpFinished.signal();
                    }
                }
            }
        }
    };

    inline LONG CALLBACK unhandledFilter (EXCEPTION_POINTERS* ep) noexcept
    {
        bool none = false;
        if (detail::dumpClaimed.compare_exchange_strong (none, true))
        {
            detail::crashActive.store (true, std::memory_order_release);
            detail::fatalEp.store (ep);
            detail::crashTrigger.signal();
        }
        secondChancePark();
        return EXCEPTION_EXECUTE_HANDLER;
    }

    inline void secondChancePark()
    {
        waitForDump (120000);
        ::TerminateProcess (::GetCurrentProcess(), 0xDEAD0001);
        for (;;)
            ::WaitForSingleObject (::GetCurrentThread(), INFINITE);
    }

    inline bool crashed() noexcept
    {
        return detail::crashActive.load (std::memory_order_acquire);
    }

    inline void terminateHandler()
    {
        dumpAndDie ("crash", nullptr, "std::terminate (unhandled C++ exception or noexcept violation).",
                    0xDEAD0002);
    }

    inline void purecallHandler()
    {
        dumpAndDie ("crash", nullptr, "Pure virtual function call.", 0xDEAD0003);
    }

    inline void invalidParameterHandler (const wchar_t* expression, const wchar_t* function,
                                         const wchar_t* file, unsigned int line, uintptr_t)
    {
        auto what = "CRT invalid parameter: " + juce::String (function != nullptr ? function : L"?")
                        + " (" + juce::String (expression != nullptr ? expression : L"?") + ") at "
                        + juce::String (file != nullptr ? file : L"?") + ":" + juce::String ((int) line);
        bool recorded = false;
        if (detail::invalidParamRecorded.compare_exchange_strong (recorded, true))
        {
            what.copyToUTF8 (detail::firstInvalidParam, sizeof (detail::firstInvalidParam));
            detail::invalidParamPublished.store (true, std::memory_order_release);
        }

        if (detail::invalidParamReports.fetch_add (1) < 3)
            return;

        dumpAndDie ("crash", nullptr, what, 0xDEAD0004);
    }

    inline void abortHandler (int)
    {
        dumpAndDie ("crash", nullptr, "abort() / SIGABRT.", 0xDEAD0005);
    }

    inline void rearm()
    {
        ::SetUnhandledExceptionFilter (unhandledFilter);
        std::set_terminate (terminateHandler);
        _set_purecall_handler (purecallHandler);
        _set_invalid_parameter_handler (invalidParameterHandler);
        ::signal (SIGABRT, abortHandler);
        _set_abort_behavior (0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    }

    inline void install()
    {
        logDir();
        (void) DumpThread::instance();
        ::AddVectoredExceptionHandler (1, firstChanceVeh);
        rearm();
    }

    class HangWatchdog : private juce::Thread, private juce::Timer
    {
    public:
        void setShuttingDown() { shuttingDown.store (true); }

        HangWatchdog() : juce::Thread ("Hang Watchdog")
        {
            lastBeat = juce::Time::getMillisecondCounter();
            startTimer (500);
            startThread();
        }

        ~HangWatchdog() override
        {
            stopTimer();
            stopThread (3000);
        }

    private:
        void timerCallback() override
        {
            lastBeat = juce::Time::getMillisecondCounter();
        }

        void run() override
        {
            bool dumped = false;
            while (! threadShouldExit())
            {
                wait (1000);
                if (crashed())
                    continue;
                const bool quitting = shuttingDown.load();
                auto stalled = juce::Time::getMillisecondCounter() - lastBeat.load();
                if (stalled > (juce::uint32) (quitting ? 90000 : 20000))
                {
                    if (! dumped)
                    {
                        dumped = true;
                        auto culprit = LoadProgress::stuckPlugin (5.0);
                        writeDump (quitting ? "hang-shutdown" : "hang", nullptr,
                                   "Message thread unresponsive for " + juce::String (stalled / 1000)
                                       + "s.\n"
                                       + (culprit.isNotEmpty()
                                            ? "Stuck inside plugin: " + culprit + "\n"
                                            : juce::String())
                                       + "The main thread's stack in the dump shows what it is stuck on.\n"
                                         "(A very slow plugin load can also trigger this - check whether the "
                                         "app recovered afterwards.)",
                                   false);
                    }
                }
                else
                {
                    dumped = false;
                }
            }
        }

        std::atomic<juce::uint32> lastBeat { 0 };
        std::atomic<bool> shuttingDown { false };
    };
   #else
    inline void install() {}
    inline void rearm() {}
    struct HangWatchdog { void setShuttingDown() {} };
   #endif
}
