#pragma once
#include <JuceHeader.h>

class HoverFade : private juce::Timer
{
public:
    explicit HoverFade (juce::Component& c, int fadeInMs = 90, int fadeOutMs = 170)
        : comp (c), inMs (juce::jmax (1, fadeInMs)), outMs (juce::jmax (1, fadeOutMs)) {}

    std::function<void()> onTick;

    void setHovered (bool h)
    {
        float t = h ? 1.0f : 0.0f;
        if (target == t)
            return;
        target = t;
        last = juce::Time::getMillisecondCounter();
        startTimerHz (60);
    }

    float value() const   { return current; }
    void snapTo (bool h)  { target = current = h ? 1.0f : 0.0f; stopTimer(); }

private:
    void timerCallback() override
    {
        auto now = juce::Time::getMillisecondCounter();
        float dt = (float) (now - last);
        last = now;
        float step = dt / (float) (target > current ? inMs : outMs);
        current = target > current ? juce::jmin (target, current + step)
                                   : juce::jmax (target, current - step);
        if (current == target)
            stopTimer();
        if (onTick)
            onTick();
        else
            comp.repaint();
    }

    juce::Component& comp;
    int inMs, outMs;
    float current = 0.0f, target = 0.0f;
    juce::uint32 last = 0;
};
