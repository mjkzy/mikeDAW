#pragma once
#include <JuceHeader.h>
#include "AppLookAndFeel.h"

class PeerCursorLayer : public juce::Component,
                        private juce::Timer
{
public:
    PeerCursorLayer()
    {
        setInterceptsMouseClicks (false, false);
        setOpaque (false);
        startTimerHz (30);
    }

    std::function<juce::Image (juce::int64)> getAvatar;

    void updatePeer (juce::int64 id, const juce::String& name, float nx, float ny)
    {
        auto& c = cursors[id];
        c.name = name;
        c.nx = juce::jlimit (0.0f, 1.0f, nx);
        c.ny = juce::jlimit (0.0f, 1.0f, ny);
        c.lastSeenMs = juce::Time::getMillisecondCounter();
        if (c.colour.isTransparent())
            c.colour = colourForId (id);
    }

    void clear()
    {
        cursors.clear();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto now = juce::Time::getMillisecondCounter();
        for (auto& [id, c] : cursors)
        {
            auto age = now - c.lastSeenMs;
            if (age > timeoutMs)
                continue;

            float alpha = 0.85f;
            if (age > timeoutMs - fadeMs)
                alpha *= 1.0f - (age - (timeoutMs - fadeMs)) / (float) fadeMs;

            float x = c.nx * getWidth();
            float y = c.ny * getHeight();
            auto avatar = getAvatar ? getAvatar (id) : juce::Image();
            drawCursor (g, x, y, c.colour, c.name, alpha, avatar);
        }
    }

private:
    struct Cursor
    {
        juce::String name;
        float nx = 0, ny = 0;
        juce::uint32 lastSeenMs = 0;
        juce::Colour colour = juce::Colours::transparentBlack;
    };

    static juce::Colour colourForId (juce::int64 id)
    {
        return Colors::peerColour (id);
    }

    static void drawCursor (juce::Graphics& g, float x, float y, juce::Colour col,
                            const juce::String& name, float alpha,
                            const juce::Image& avatar = {})
    {
        juce::Path arrow;
        arrow.startNewSubPath (x, y);
        arrow.lineTo (x, y + 16.0f);
        arrow.lineTo (x + 4.5f, y + 12.0f);
        arrow.lineTo (x + 7.5f, y + 18.5f);
        arrow.lineTo (x + 10.0f, y + 17.5f);
        arrow.lineTo (x + 7.0f, y + 11.0f);
        arrow.lineTo (x + 12.5f, y + 11.0f);
        arrow.closeSubPath();

        g.setColour (juce::Colours::black.withAlpha (0.55f * alpha));
        g.strokePath (arrow, juce::PathStrokeType (2.4f));
        g.setColour (col.withAlpha (alpha));
        g.fillPath (arrow);
        g.setColour (juce::Colours::white.withAlpha (0.9f * alpha));
        g.strokePath (arrow, juce::PathStrokeType (0.8f));

        if (name.isNotEmpty())
        {
            juce::Font font (juce::FontOptions (11.5f, juce::Font::bold));
            const bool hasAvatar = avatar.isValid();
            const float avatarD = 14.0f;
            int w = juce::GlyphArrangement::getStringWidthInt (font, name) + 12
                        + (hasAvatar ? (int) avatarD + 2 : 0);
            juce::Rectangle<float> tag (x + 14.0f, y + 14.0f, (float) w, 17.0f);
            g.setColour (col.withAlpha (0.9f * alpha));
            g.fillRoundedRectangle (tag, 4.0f);

            auto textArea = tag;
            if (hasAvatar)
            {
                auto av = tag.removeFromLeft (avatarD + 4.0f)
                              .withSizeKeepingCentre (avatarD, avatarD);
                juce::Graphics::ScopedSaveState ss (g);
                juce::Path clip;
                clip.addEllipse (av);
                g.reduceClipRegion (clip);
                g.setOpacity (alpha);
                g.drawImage (avatar, av, juce::RectanglePlacement::centred
                                             | juce::RectanglePlacement::fillDestination);
                textArea = tag;
            }

            g.setColour (col.getPerceivedBrightness() > 0.55f
                             ? juce::Colours::black.withAlpha (alpha)
                             : juce::Colours::white.withAlpha (alpha));
            g.setFont (font);
            g.drawText (name, textArea, juce::Justification::centred);
        }
    }

    void timerCallback() override
    {
        auto now = juce::Time::getMillisecondCounter();
        bool any = false;
        for (auto it = cursors.begin(); it != cursors.end();)
        {
            if (now - it->second.lastSeenMs > timeoutMs)
                it = cursors.erase (it);
            else { any = true; ++it; }
        }
        if (any || wasAnyLastTick)
            repaint();
        wasAnyLastTick = any;
    }

    static constexpr juce::uint32 timeoutMs = 2500;
    static constexpr juce::uint32 fadeMs = 600;
    std::map<juce::int64, Cursor> cursors;
    bool wasAnyLastTick = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PeerCursorLayer)
};
