#pragma once
#include <JuceHeader.h>

class WheelComboBox : public juce::ComboBox
{
public:
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        int n = getNumItems();
        if (n <= 1)
        {
            juce::ComboBox::mouseWheelMove (e, wheel);
            return;
        }

        double dy = wheel.deltaY * (wheel.isReversed ? -1.0 : 1.0);
        if (dy == 0.0)
            return;

        int dir = dy > 0 ? -1 : 1;
        auto now = juce::Time::getMillisecondCounter();
        if (dir == lastDir && now - lastStepMs < cooldownMs)
            return;
        lastDir = dir;
        lastStepMs = now;

        int idx = juce::jlimit (0, n - 1, getSelectedItemIndex() + dir);
        if (idx != getSelectedItemIndex())
            setSelectedItemIndex (idx, juce::sendNotificationSync);
    }

    std::function<void()> onPopupAboutToShow;

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown())
            return;
        if (onPopupAboutToShow != nullptr && ! e.mods.isPopupMenu())
            onPopupAboutToShow();
        juce::ComboBox::mouseDown (e);
    }

    int lastDir = 0;
    juce::uint32 lastStepMs = 0;
    static constexpr juce::uint32 cooldownMs = 90;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WheelComboBox)
public:
    WheelComboBox() = default;
};
