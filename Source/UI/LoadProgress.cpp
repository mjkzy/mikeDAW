#include "LoadProgress.h"

#if JUCE_WINDOWS
 #include <windows.h>
 #include <map>
 #include <thread>
 #include <mutex>
 #include <atomic>
#endif

namespace LoadProgress
{
#if JUCE_WINDOWS

namespace
{
    constexpr int winW = 460, winH = 168;
    constexpr double warnSeconds = 8.0;
    constexpr double stuckSeconds = 25.0;

    constexpr COLORREF colBg      = RGB (26, 27, 32);
    constexpr COLORREF colPanel   = RGB (38, 40, 47);
    constexpr COLORREF colText    = RGB (226, 228, 234);
    constexpr COLORREF colDim     = RGB (140, 145, 158);
    constexpr COLORREF colAccent  = RGB (86, 156, 214);
    constexpr COLORREF colWarn    = RGB (222, 168, 74);
    constexpr COLORREF colBad     = RGB (214, 88, 88);

    struct ActiveCall
    {
        juce::String name;
        juce::int64 startMs = 0;
        juce::uint64 seq = 0;
    };

    struct State
    {
        std::mutex activeMutex;
        std::map<std::thread::id, ActiveCall> active;
        juce::uint64 enterCounter = 0;

        std::atomic<int> queued { 0 }, finished { 0 };
        std::atomic<bool> threadRunning { false }, shouldExit { false };
        std::atomic<int> frame { 0 };
        std::atomic<bool> forceQuitHot { false };
        std::atomic<HWND> hwnd { nullptr };
        std::thread thread;
        std::mutex lifecycleMutex;
    };

    State& state()
    {
        static State s;
        return s;
    }

    juce::int64 nowMs() { return (juce::int64) juce::Time::getMillisecondCounterHiRes(); }

    struct Snapshot
    {
        juce::String recentName;
        juce::String longestName;
        double longestSeconds = 0.0;
    };

    Snapshot snapshot()
    {
        auto& s = state();
        Snapshot out;
        juce::uint64 bestSeq = 0;
        juce::int64 oldest = 0;

        const std::lock_guard<std::mutex> lk (s.activeMutex);
        for (const auto& e : s.active)
        {
            if (e.second.seq >= bestSeq)
            {
                bestSeq = e.second.seq;
                out.recentName = e.second.name;
            }
            if (oldest == 0 || e.second.startMs < oldest)
            {
                oldest = e.second.startMs;
                out.longestName = e.second.name;
            }
        }
        if (oldest != 0)
            out.longestSeconds = (double) (nowMs() - oldest) / 1000.0;
        return out;
    }

    RECT forceQuitRect()
    {
        RECT r;
        r.right = winW - 18;  r.left = r.right - 120;
        r.bottom = winH - 16; r.top = r.bottom - 26;
        return r;
    }

    bool isStuck() { return snapshot().longestSeconds >= stuckSeconds; }

    void fill (HDC dc, RECT r, COLORREF c)
    {
        HBRUSH b = CreateSolidBrush (c);
        FillRect (dc, &r, b);
        DeleteObject (b);
    }

    void drawText (HDC dc, const juce::String& text, int x, int y, int w, int h,
                   COLORREF colour, int height, bool bold, UINT flags)
    {
        HFONT f = CreateFontW (height, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HGDIOBJ old = SelectObject (dc, f);
        SetTextColor (dc, colour);
        SetBkMode (dc, TRANSPARENT);
        RECT r { x, y, x + w, y + h };
        DrawTextW (dc, text.toWideCharPointer(), -1, &r, flags | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject (dc, old);
        DeleteObject (f);
    }

    void drawSpinner (HDC dc, int cx, int cy, int radius, int frame, COLORREF tint)
    {
        constexpr int dots = 12;
        for (int i = 0; i < dots; ++i)
        {
            const double a = i * (2.0 * juce::MathConstants<double>::pi / dots);
            const int x = cx + (int) (std::cos (a) * radius);
            const int y = cy + (int) (std::sin (a) * radius);
            const int age = (i - frame % dots + dots) % dots;
            const double lit = 1.0 - (double) age / dots;
            const auto blend = [lit, tint] (int shift)
            {
                const int fg = (int) ((tint >> shift) & 0xff);
                const int bg = (int) ((colPanel >> shift) & 0xff);
                return (int) (bg + (fg - bg) * lit);
            };
            HBRUSH b = CreateSolidBrush (RGB (blend (0), blend (8), blend (16)));
            HGDIOBJ oldB = SelectObject (dc, b);
            HGDIOBJ oldP = SelectObject (dc, GetStockObject (NULL_PEN));
            Ellipse (dc, x - 3, y - 3, x + 4, y + 4);
            SelectObject (dc, oldP);
            SelectObject (dc, oldB);
            DeleteObject (b);
        }
    }

    void paint (HWND hwnd)
    {
        PAINTSTRUCT ps;
        HDC screen = BeginPaint (hwnd, &ps);

        HDC dc = CreateCompatibleDC (screen);
        HBITMAP bmp = CreateCompatibleBitmap (screen, winW, winH);
        HGDIOBJ oldBmp = SelectObject (dc, bmp);

        fill (dc, RECT { 0, 0, winW, winH }, colBg);
        fill (dc, RECT { 1, 1, winW - 1, winH - 1 }, colPanel);

        auto& s = state();
        const int done = s.finished.load(), total = juce::jmax (1, s.queued.load());
        const auto snap = snapshot();
        const double secs = snap.longestSeconds;
        const bool stuck = secs >= stuckSeconds;
        const bool warn = secs >= warnSeconds;
        const auto name = warn ? snap.longestName : snap.recentName;
        const COLORREF tint = stuck ? colBad : (warn ? colWarn : colAccent);

        drawSpinner (dc, 40, 46, 16, s.frame.load(), tint);

        drawText (dc, stuck ? "Plugin not responding" : "Loading plugins",
                  74, 22, winW - 96, 24, stuck ? colBad : colText, 19, true, DT_LEFT);
        drawText (dc, juce::String (juce::jmin (done + 1, total)) + " of " + juce::String (total),
                  74, 48, winW - 96, 20, colDim, 15, false, DT_LEFT);

        const int barY = 84, barX = 20, barW = winW - 40;
        fill (dc, RECT { barX, barY, barX + barW, barY + 6 }, RGB (24, 25, 30));
        const int filled = (int) ((double) barW * juce::jlimit (0.0, 1.0, (double) done / total));
        if (filled > 0)
            fill (dc, RECT { barX, barY, barX + filled, barY + 6 }, tint);

        juce::String status = name;
        if (status.isEmpty())
            status = "Preparing...";
        else if (stuck)
            status = name + " has not responded for " + juce::String ((int) secs) + "s";
        else if (warn)
            status = name + " - " + juce::String ((int) secs) + "s";
        drawText (dc, status, 20, 100, winW - 40, 20, warn ? tint : colDim, 15, false, DT_LEFT);

        if (stuck)
        {
            drawText (dc, "It may never finish. Your project is not lost.",
                      20, winH - 40, winW - 160, 20, colDim, 13, false, DT_LEFT);
            const RECT q = forceQuitRect();
            fill (dc, q, s.forceQuitHot.load() ? RGB (198, 74, 74) : RGB (86, 46, 46));
            drawText (dc, "Force quit", q.left, q.top + 4, q.right - q.left, 20,
                      colText, 14, true, DT_CENTER);
        }

        BitBlt (screen, 0, 0, winW, winH, dc, 0, 0, SRCCOPY);
        SelectObject (dc, oldBmp);
        DeleteObject (bmp);
        DeleteDC (dc);
        EndPaint (hwnd, &ps);
    }

    LRESULT CALLBACK wndProc (HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
            case WM_PAINT:
                paint (hwnd);
                return 0;

            case WM_ERASEBKGND:
                return 1;

            case WM_TIMER:
                state().frame.fetch_add (1);
                InvalidateRect (hwnd, nullptr, FALSE);
                return 0;

            case WM_MOUSEMOVE:
            {
                const POINT p { (int) (short) LOWORD (lp), (int) (short) HIWORD (lp) };
                const RECT q = forceQuitRect();
                const bool hot = isStuck() && p.x >= q.left && p.x <= q.right
                                           && p.y >= q.top && p.y <= q.bottom;
                state().forceQuitHot.store (hot);
                return 0;
            }

            case WM_LBUTTONDOWN:
            {
                const POINT p { (int) (short) LOWORD (lp), (int) (short) HIWORD (lp) };
                const RECT q = forceQuitRect();
                if (isStuck() && p.x >= q.left && p.x <= q.right
                              && p.y >= q.top && p.y <= q.bottom)
                {
                    TerminateProcess (GetCurrentProcess(), 1);
                }
                return 0;
            }

            default:
                break;
        }
        return DefWindowProcW (hwnd, msg, wp, lp);
    }

    void windowThreadRun()
    {
        auto& s = state();

        WNDCLASSEXW wc {};
        wc.cbSize = sizeof (wc);
        wc.lpfnWndProc = wndProc;
        wc.hInstance = GetModuleHandleW (nullptr);
        wc.hCursor = LoadCursorW (nullptr, (LPCWSTR) IDC_ARROW);
        wc.lpszClassName = L"mikeDAWLoadProgress";
        RegisterClassExW (&wc);

        const int sx = GetSystemMetrics (SM_CXSCREEN), sy = GetSystemMetrics (SM_CYSCREEN);
        HWND hwnd = CreateWindowExW (WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName,
                                     L"mikeDAW", WS_POPUP,
                                     (sx - winW) / 2, (sy - winH) / 2, winW, winH,
                                     nullptr, nullptr, wc.hInstance, nullptr);
        if (hwnd == nullptr)
        {
            s.threadRunning.store (false);
            return;
        }

        s.hwnd.store (hwnd);
        ShowWindow (hwnd, SW_SHOWNOACTIVATE);
        SetTimer (hwnd, 1, 33, nullptr);

        MSG msg;
        while (! s.shouldExit.load() && GetMessageW (&msg, nullptr, 0, 0) > 0)
        {
            TranslateMessage (&msg);
            DispatchMessageW (&msg);
        }

        KillTimer (hwnd, 1);
        s.hwnd.store (nullptr);
        DestroyWindow (hwnd);
        s.threadRunning.store (false);
    }

    void startWindow()
    {
        auto& s = state();
        const std::lock_guard<std::mutex> lk (s.lifecycleMutex);
        if (s.threadRunning.load())
            return;
        if (s.thread.joinable())
            s.thread.join();
        s.shouldExit.store (false);
        s.threadRunning.store (true);
        s.thread = std::thread (windowThreadRun);
    }

    void stopWindow()
    {
        auto& s = state();
        std::thread toJoin;
        {
            const std::lock_guard<std::mutex> lk (s.lifecycleMutex);
            if (! s.threadRunning.load() && ! s.thread.joinable())
                return;
            s.shouldExit.store (true);
            if (auto h = s.hwnd.load())
                PostMessageW (h, WM_NULL, 0, 0);
            toJoin = std::move (s.thread);
        }
        if (toJoin.joinable())
            toJoin.join();
    }
}

void enter (const juce::String& pluginName)
{
    auto& s = state();
    const std::lock_guard<std::mutex> lk (s.activeMutex);
    auto& call = s.active[std::this_thread::get_id()];
    call.name = pluginName;
    call.startMs = nowMs();
    call.seq = ++s.enterCounter;
}

void leave()
{
    auto& s = state();
    const std::lock_guard<std::mutex> lk (s.activeMutex);
    s.active.erase (std::this_thread::get_id());
}

void noteQueued()
{
    state().queued.fetch_add (1);
    startWindow();
}

void noteFinished()
{
    auto& s = state();
    const int done = s.finished.fetch_add (1) + 1;
    if (done >= s.queued.load())
    {
        s.queued.store (0);
        s.finished.store (0);
        {
            const std::lock_guard<std::mutex> lk (s.activeMutex);
            s.active.clear();
        }
        stopWindow();
    }
}

void shutdown() { stopWindow(); }

juce::String stuckPlugin (double minSeconds)
{
    const auto snap = snapshot();
    return snap.longestSeconds >= minSeconds ? snap.longestName : juce::String();
}

#else

void enter (const juce::String&) {}
void leave() {}
void noteQueued() {}
void noteFinished() {}
void shutdown() {}
juce::String stuckPlugin (double) { return {}; }

#endif
}
