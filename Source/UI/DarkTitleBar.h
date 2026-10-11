#pragma once
#include <JuceHeader.h>

#if JUCE_WINDOWS
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <dwmapi.h>
 #pragma comment (lib, "dwmapi.lib")
#endif

namespace DarkTitleBar
{
    inline void apply (juce::Component& window)
    {
       #if JUCE_WINDOWS
        if (auto* peer = window.getPeer())
        {
            const BOOL dark = TRUE;
            auto hwnd = (HWND) peer->getNativeHandle();
            if (DwmSetWindowAttribute (hwnd, 20, &dark, sizeof (dark)) != S_OK)
                DwmSetWindowAttribute (hwnd, 19, &dark, sizeof (dark));
        }
       #else
        juce::ignoreUnused (window);
       #endif
    }

    inline void applyThemeColours (juce::Component& window,
                                   juce::Colour caption, juce::Colour text)
    {
       #if JUCE_WINDOWS
        if (auto* peer = window.getPeer())
        {
            auto hwnd = (HWND) peer->getNativeHandle();
            COLORREF cap = RGB (caption.getRed(), caption.getGreen(), caption.getBlue());
            COLORREF txt = RGB (text.getRed(),    text.getGreen(),    text.getBlue());
            DwmSetWindowAttribute (hwnd, 35, &cap, sizeof (cap));
            DwmSetWindowAttribute (hwnd, 36, &txt, sizeof (txt));
        }
       #else
        juce::ignoreUnused (window, caption, text);
       #endif
    }
}
