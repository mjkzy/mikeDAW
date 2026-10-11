#pragma once
#include <JuceHeader.h>

class SmoothZoomer : private juce::Timer
{
public:
    std::function<void (double value)> onStep;

    bool isAnimating() const noexcept { return isTimerRunning(); }
    double getTarget() const noexcept { return target; }

    void animateTo (double actualCurrent, double newTarget)
    {
        target = newTarget;
        if (! isTimerRunning())
        {
            current = actualCurrent;
            startTimerHz (60);
        }
    }

    void stop() { stopTimer(); }

private:
    void timerCallback() override
    {
        current += (target - current) * ease;
        if (std::abs (target - current) < epsilon)
        {
            current = target;
            stopTimer();
        }
        if (onStep)
            onStep (current);
    }

    double current = 0.0, target = 0.0;
    static constexpr double ease = 0.35;
    static constexpr double epsilon = 0.02;
};
