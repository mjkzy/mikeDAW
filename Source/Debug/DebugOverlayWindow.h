#pragma once
#include <JuceHeader.h>
#include "ImGuiCanvas.h"
#include "../UI/AppLookAndFeel.h"
#include "../UI/DarkTitleBar.h"

class DebugOverlayWindow : public juce::DocumentWindow
{
public:
    DebugOverlayWindow()
        : juce::DocumentWindow ("Debug Overlay", Colors::panelBg,
                                juce::DocumentWindow::closeButton
                              | juce::DocumentWindow::minimiseButton)
    {
        setUsingNativeTitleBar (true);
        setResizable (true, false);
        setResizeLimits (480, 320, 4096, 2160);

        canvasComp = new ImGuiCanvas();
        setContentOwned (canvasComp, false);
        centreWithSize (980, 640);
    }

    ImGuiCanvas& canvas() { return *canvasComp; }

    std::function<void()> onClosed;

    void closeButtonPressed() override
    {
        setVisible (false);
        if (onClosed)
            onClosed();
    }

    void lookAndFeelChanged() override
    {
        juce::DocumentWindow::lookAndFeelChanged();
        setBackgroundColour (Colors::panelBg);
        DarkTitleBar::applyThemeColours (*this, Colors::header, Colors::text);
    }

private:
    ImGuiCanvas* canvasComp = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DebugOverlayWindow)
};
