#pragma once
#include <JuceHeader.h>
#include "AppLookAndFeel.h"

class ToastLayer : public juce::Component,
                   private juce::Timer
{
public:
    enum class Kind { info, success, warning, error };

    ToastLayer()
    {
        setInterceptsMouseClicks (true, false);
        setOpaque (false);
        startTimerHz (30);
    }

    void post (const juce::String& message, Kind kind = Kind::info, int holdMs = 4200)
    {
        post (message, kind, holdMs, {}, {});
    }

    void post (const juce::String& message, Kind kind, int holdMs,
               const juce::String& actionText, std::function<void()> onAction)
    {
        post (message, kind, holdMs, actionText, std::move (onAction), {}, {});
    }

    void post (const juce::String& message, Kind kind, int holdMs,
               const juce::String& actionText, std::function<void()> onAction,
               const juce::String& secondaryText, std::function<void()> onSecondary,
               juce::Colour actionColour = {}, juce::Colour secondaryColour = {})
    {
        Toast t;
        t.message = message;
        t.kind = kind;
        t.bornMs = juce::Time::getMillisecondCounter();
        t.holdMs = holdMs;
        t.actionText = actionText;
        t.onAction = std::move (onAction);
        t.secondaryText = secondaryText;
        t.onSecondary = std::move (onSecondary);
        t.actionColour = actionColour.isTransparent() ? accentFor (kind) : actionColour;
        t.secondaryColour = secondaryColour.isTransparent() ? accentFor (kind) : secondaryColour;
        toasts.insert (toasts.begin(), std::move (t));
        if (toasts.size() > maxVisible)
            toasts.resize (maxVisible);
        repaint();
    }

    void clear() { toasts.clear(); repaint(); }

    void paint (juce::Graphics& g) override
    {
        auto now = juce::Time::getMillisecondCounter();
        const int margin = 22, gap = 12, minW = 460;
        const int maxW = juce::jmax (minW, getWidth() - margin * 2);
        int y = getHeight() - margin;

        for (auto& t : toasts)
        {
            t.actionRect = {};
            t.secondaryRect = {};

            auto age = (int) (now - t.bornMs);
            int total = fadeInMs + t.holdMs + fadeOutMs;
            if (age >= total)
                continue;

            float alpha = 1.0f;
            if (age < fadeInMs)                 alpha = age / (float) fadeInMs;
            else if (age > fadeInMs + t.holdMs) alpha = 1.0f - (age - fadeInMs - t.holdMs) / (float) fadeOutMs;
            alpha = juce::jlimit (0.0f, 1.0f, alpha);

            const bool hasAction = t.actionText.isNotEmpty() && t.onAction != nullptr;
            const bool hasSecondary = hasAction && t.secondaryText.isNotEmpty();

            auto font = Typography::semi (15.0f);
            auto actionFont = Typography::semi (13.5f);
            const int actionPadX = 14;
            int actionW = hasAction ? juce::GlyphArrangement::getStringWidthInt (actionFont, t.actionText) + actionPadX * 2 : 0;
            int secondaryW = hasSecondary ? juce::GlyphArrangement::getStringWidthInt (actionFont, t.secondaryText) + actionPadX * 2 : 0;
            int buttonsW = actionW + (hasSecondary ? secondaryW + 8 : 0);
            int messageW = juce::GlyphArrangement::getStringWidthInt (font, t.message);
            int w = juce::jlimit (minW, maxW, 34 + messageW + (hasAction ? buttonsW + 12 : 0) + 4);
            int textW = juce::jmax (40, w - 34 - (hasAction ? buttonsW + 12 : 0));
            int lines = juce::jmax (1, messageW / textW + 1);
            int h = 26 + lines * 24;

            juce::Rectangle<int> box (getWidth() - margin - w, y - h, w, h);
            y = box.getY() - gap;

            float slide = (1.0f - alpha) * 16.0f;
            auto r = box.toFloat().translated (slide, 0.0f);

            g.setColour (juce::Colours::black.withAlpha (0.18f * alpha));
            g.fillRoundedRectangle (r.expanded (7.0f), Metrics::rL + 5.0f);
            g.setColour (juce::Colours::black.withAlpha (0.30f * alpha));
            g.fillRoundedRectangle (r.expanded (3.0f), Metrics::rL + 2.0f);

            g.setColour (Colors::card.darker (0.35f).withAlpha (alpha));
            g.fillRoundedRectangle (r, Metrics::rL);
            g.setColour (accentFor (t.kind).withAlpha (0.35f * alpha));
            g.drawRoundedRectangle (r.reduced (0.6f), Metrics::rL, 1.2f);

            {
                juce::Graphics::ScopedSaveState ss (g);
                g.reduceClipRegion (r.withWidth (4.0f).getSmallestIntegerContainer());
                g.setColour (accentFor (t.kind).withAlpha (0.95f * alpha));
                g.fillRoundedRectangle (r.withWidth (4.0f + Metrics::rL), Metrics::rL);
            }

            auto textArea = r.toNearestInt().reduced (17, 11);
            if (hasAction)
            {
                const float bh = juce::jmin (30.0f, r.getHeight() - 16.0f);
                auto btnRow = r.reduced (10.0f);

                if (hasSecondary)
                {
                    auto secondary = btnRow.removeFromRight ((float) secondaryW).withSizeKeepingCentre ((float) secondaryW, bh);
                    t.secondaryRect = secondary.toNearestInt();
                    g.setColour (t.secondaryColour.withAlpha (0.90f * alpha));
                    g.fillRoundedRectangle (secondary, Metrics::rM);
                    g.setColour (labelOn (t.secondaryColour).withAlpha (alpha));
                    g.setFont (actionFont);
                    g.drawText (t.secondaryText, t.secondaryRect, juce::Justification::centred);
                    btnRow.removeFromRight (8.0f);
                }

                auto primary = btnRow.removeFromRight ((float) actionW).withSizeKeepingCentre ((float) actionW, bh);
                t.actionRect = primary.toNearestInt();
                g.setColour (t.actionColour.withAlpha (0.90f * alpha));
                g.fillRoundedRectangle (primary, Metrics::rM);
                g.setColour (labelOn (t.actionColour).withAlpha (alpha));
                g.setFont (actionFont);
                g.drawText (t.actionText, t.actionRect, juce::Justification::centred);

                textArea = textArea.withTrimmedRight (buttonsW + 12);
            }

            g.setColour (Colors::contrastText (Colors::card.darker (0.35f)).withAlpha (alpha));
            g.setFont (font);
            g.drawFittedText (t.message, textArea, juce::Justification::centredLeft, lines, 1.0f);
        }
    }

    bool hitTest (int x, int y) override
    {
        for (auto& t : toasts)
        {
            if (! t.actionRect.isEmpty() && t.actionRect.contains (x, y))
                return true;
            if (! t.secondaryRect.isEmpty() && t.secondaryRect.contains (x, y))
                return true;
        }
        return false;
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        for (size_t i = 0; i < toasts.size(); ++i)
        {
            auto& t = toasts[i];
            if (! t.actionRect.isEmpty() && t.actionRect.contains (e.getPosition()) && t.onAction != nullptr)
            {
                auto cb = t.onAction;
                toasts.erase (toasts.begin() + (long) i);
                repaint();
                cb();
                return;
            }
            if (! t.secondaryRect.isEmpty() && t.secondaryRect.contains (e.getPosition()))
            {
                auto cb = t.onSecondary;
                toasts.erase (toasts.begin() + (long) i);
                repaint();
                if (cb != nullptr)
                    cb();
                return;
            }
        }
    }

private:
    struct Toast
    {
        juce::String message;
        Kind kind = Kind::info;
        juce::uint32 bornMs = 0;
        int holdMs = 4200;
        juce::String actionText;
        std::function<void()> onAction;
        juce::Rectangle<int> actionRect;
        juce::String secondaryText;
        std::function<void()> onSecondary;
        juce::Rectangle<int> secondaryRect;
        juce::Colour actionColour;
        juce::Colour secondaryColour;
    };

    static juce::Colour accentFor (Kind k)
    {
        switch (k)
        {
            case Kind::success: return Colors::success;
            case Kind::warning: return Colors::warning;
            case Kind::error:   return Colors::danger;
            case Kind::info:
            default:            return Colors::accent;
        }
    }

    static juce::Colour labelOn (juce::Colour fill)
    {
        return fill.getPerceivedBrightness() > 0.6f ? Colors::windowBg : Colors::text;
    }

    void timerCallback() override
    {
        auto now = juce::Time::getMillisecondCounter();
        bool any = false;
        for (auto it = toasts.begin(); it != toasts.end();)
        {
            int total = fadeInMs + it->holdMs + fadeOutMs;
            if ((int) (now - it->bornMs) >= total)
                it = toasts.erase (it);
            else { any = true; ++it; }
        }
        if (any || wasAnyLastTick)
            repaint();
        wasAnyLastTick = any;
    }

    static constexpr int fadeInMs = 180;
    static constexpr int fadeOutMs = 500;
    static constexpr size_t maxVisible = 5;
    std::vector<Toast> toasts;
    bool wasAnyLastTick = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ToastLayer)
};
