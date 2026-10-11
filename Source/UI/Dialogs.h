#pragma once
#include <JuceHeader.h>

namespace Dialogs
{
    class KeyNavAlertWindow : public juce::AlertWindow
    {
    public:
        KeyNavAlertWindow (const juce::String& title, const juce::String& message,
                           juce::MessageBoxIconType icon)
            : juce::AlertWindow (title, message, icon) {}

        bool keyPressed (const juce::KeyPress& key) override
        {
            if (key == juce::KeyPress::leftKey || key == juce::KeyPress::upKey)
                { moveKeyboardFocusToSibling (false); return true; }
            if (key == juce::KeyPress::rightKey || key == juce::KeyPress::downKey)
                { moveKeyboardFocusToSibling (true); return true; }
            return juce::AlertWindow::keyPressed (key);
        }
    };

    inline void showInfoAsync (juce::MessageBoxIconType icon,
                               const juce::String& title,
                               const juce::String& message,
                               const juce::String& buttonText = "OK",
                               std::function<void()> onDismiss = nullptr,
                               juce::Component* associated = nullptr)
    {
        auto* w = new KeyNavAlertWindow (title, message, icon);
        w->addButton (buttonText, 0, juce::KeyPress (juce::KeyPress::returnKey),
                                     juce::KeyPress (juce::KeyPress::escapeKey));
        if (associated != nullptr)
            w->setAlwaysOnTop (true);
        w->enterModalState (true, juce::ModalCallbackFunction::create (
            [onDismiss] (int) { if (onDismiss) onDismiss(); }), true);
    }

    inline void showOkCancelAsync (juce::MessageBoxIconType icon,
                                   const juce::String& title,
                                   const juce::String& message,
                                   const juce::String& okText,
                                   const juce::String& cancelText,
                                   std::function<void (bool)> callback,
                                   juce::Component* associated = nullptr)
    {
        auto* w = new KeyNavAlertWindow (title, message, icon);
        w->addButton (okText,     1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton (cancelText, 0, juce::KeyPress (juce::KeyPress::escapeKey));
        if (associated != nullptr)
            w->setAlwaysOnTop (true);
        w->enterModalState (true, juce::ModalCallbackFunction::create (
            [callback] (int r) { if (callback) callback (r == 1); }), true);
    }

    inline void setContentFitted (juce::DocumentWindow& window,
                                  juce::Component* content, int w, int h)
    {
        content->setSize (w, h);

        auto area = juce::Desktop::getInstance().getDisplays()
                        .getPrimaryDisplay()->userArea;
        const int maxW = area.getWidth()  - 40;
        const int maxH = area.getHeight() - 80;

        if (w <= maxW && h <= maxH)
        {
            window.setContentOwned (content, true);
            window.centreWithSize (w, h);
            return;
        }

        auto* vp = new juce::Viewport();
        vp->setScrollBarsShown (true, false);
        vp->setViewedComponent (content, true);
        const int vpW = juce::jmin (w + vp->getScrollBarThickness(), maxW);
        const int vpH = juce::jmin (h, maxH);
        vp->setSize (vpW, vpH);
        window.setContentOwned (vp, true);
        window.centreWithSize (vpW, vpH);
    }
}
