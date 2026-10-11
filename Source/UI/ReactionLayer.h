#pragma once
#include <JuceHeader.h>
#include "AppLookAndFeel.h"
#include <vector>

class ReactionLayer : public juce::Component,
                      private juce::Timer
{
public:
    ReactionLayer()
    {
        setInterceptsMouseClicks (false, false);
        setOpaque (false);
        startTimerHz (30);
    }

    void addReaction (juce::int64 id, const juce::String& name, const juce::String& emoji,
                      float nx = -1.0f, float ny = -1.0f)
    {
        Bubble b;
        b.name = name;
        b.emoji = emoji;
        b.colour = Colors::peerColour (id);
        b.positioned = (nx >= 0.0f && ny >= 0.0f);
        b.nx = juce::jlimit (0.0f, 1.0f, nx);
        b.ny = juce::jlimit (0.0f, 1.0f, ny);
        b.bornMs = juce::Time::getMillisecondCounter();
        b.drift = juce::Random::getSystemRandom().nextFloat() * 30.0f - 15.0f;
        b.xJitter = juce::Random::getSystemRandom().nextFloat() * 40.0f;
        bubbles.insert (bubbles.begin(), std::move (b));
        if (bubbles.size() > maxAlive)
            bubbles.resize (maxAlive);
        if (! isTimerRunning())
            startTimerHz (30);
        repaint();
    }

    void clear() { bubbles.clear(); repaint(); }

    void paint (juce::Graphics& g) override
    {
        auto now = juce::Time::getMillisecondCounter();
        const float defaultBaseX = (float) (getWidth() - 170);
        const float defaultBaseY = (float) (getHeight() - 220);

        for (auto& b : bubbles)
        {
            auto age = (int) (now - b.bornMs);
            if (age >= lifeMs)
                continue;

            float t = age / (float) lifeMs;
            float alpha = t < 0.15f ? t / 0.15f : (1.0f - (t - 0.15f) / 0.85f);
            alpha = juce::jlimit (0.0f, 1.0f, alpha);

            float baseX = b.positioned ? b.nx * getWidth() - 20.0f : defaultBaseX;
            float baseY = b.positioned ? b.ny * getHeight()        : defaultBaseY;

            float x = baseX + b.xJitter + b.drift * t;
            float y = baseY - t * risePx;

            juce::Font nameFont (juce::FontOptions (11.5f, juce::Font::bold));
            int nameW = juce::GlyphArrangement::getStringWidthInt (nameFont, b.name);
            float w = (float) (nameW + 60);
            juce::Rectangle<float> box (x, y, w, 30.0f);

            g.setColour (juce::Colours::black.withAlpha (0.30f * alpha));
            g.fillRoundedRectangle (box.translated (0.0f, 2.0f), 15.0f);
            g.setColour (Colors::panelBg.withAlpha (0.96f * alpha));
            g.fillRoundedRectangle (box, 15.0f);
            g.setColour (b.colour.withAlpha (0.85f * alpha));
            g.drawRoundedRectangle (box, 15.0f, 1.2f);

            auto inner = box.reduced (9.0f, 0.0f);
            auto glyphArea = inner.removeFromLeft (32.0f).reduced (1.0f, 2.0f);
            drawReactionGlyph (g, b.emoji, glyphArea, alpha);
            g.setColour (b.colour.withAlpha (alpha));
            g.setFont (nameFont);
            g.drawText (b.name, inner, juce::Justification::centredLeft);
        }
    }

private:
    struct Bubble
    {
        juce::String name, emoji;
        juce::Colour colour { juce::Colours::white };
        juce::uint32 bornMs = 0;
        float drift = 0.0f, xJitter = 0.0f;
        bool positioned = false;
        float nx = 0.0f, ny = 0.0f;
    };

    enum class ReactionKind { fire, nice, love, haha, huh, unknown };

    static ReactionKind reactionKind (const juce::String& emoji)
    {
        if (emoji.containsChar ((juce::juce_wchar) 0x1f525)) return ReactionKind::fire;
        if (emoji.containsChar ((juce::juce_wchar) 0x1f44d)) return ReactionKind::nice;
        if (emoji.containsChar ((juce::juce_wchar) 0x2764))  return ReactionKind::love;
        if (emoji.containsChar ((juce::juce_wchar) 0x1f602)) return ReactionKind::haha;
        if (emoji.containsChar ((juce::juce_wchar) 0x2753))  return ReactionKind::huh;
        return ReactionKind::unknown;
    }

    static void drawReactionGlyph (juce::Graphics& g, const juce::String& emoji,
                                   juce::Rectangle<float> area, float alpha)
    {
        const auto side = juce::jmin (area.getWidth(), area.getHeight());
        area = area.withSizeKeepingCentre (side, side).reduced (1.0f);

        switch (reactionKind (emoji))
        {
            case ReactionKind::fire:
            {
                juce::Path outer;
                outer.startNewSubPath (area.getCentreX(), area.getY());
                outer.cubicTo (area.getRight() - 1.0f, area.getY() + area.getHeight() * 0.32f,
                               area.getRight() - 2.0f, area.getBottom() - 2.0f,
                               area.getCentreX(), area.getBottom());
                outer.cubicTo (area.getX() + 2.0f, area.getBottom() - 2.0f,
                               area.getX() + 1.0f, area.getY() + area.getHeight() * 0.42f,
                               area.getCentreX() - 1.0f, area.getY() + area.getHeight() * 0.26f);
                outer.cubicTo (area.getCentreX() + area.getWidth() * 0.12f, area.getY() + area.getHeight() * 0.18f,
                               area.getCentreX() + area.getWidth() * 0.05f, area.getY() + area.getHeight() * 0.08f,
                               area.getCentreX(), area.getY());
                outer.closeSubPath();
                g.setColour (Colors::accent2.withAlpha (alpha));
                g.fillPath (outer);

                juce::Path inner;
                inner.startNewSubPath (area.getCentreX() + 1.0f, area.getY() + area.getHeight() * 0.42f);
                inner.cubicTo (area.getRight() - 5.0f, area.getY() + area.getHeight() * 0.62f,
                               area.getRight() - 6.0f, area.getBottom() - 3.0f,
                               area.getCentreX(), area.getBottom() - 2.0f);
                inner.cubicTo (area.getX() + 5.0f, area.getBottom() - 4.0f,
                               area.getX() + 8.0f, area.getY() + area.getHeight() * 0.58f,
                               area.getCentreX() + 1.0f, area.getY() + area.getHeight() * 0.42f);
                inner.closeSubPath();
                g.setColour (Colors::meterMid.withAlpha (0.9f * alpha));
                g.fillPath (inner);
                break;
            }

            case ReactionKind::nice:
            {
                const auto skin = juce::Colour (0xffffc34d).withAlpha (alpha);
                const auto shade = juce::Colour (0xffb26c18).withAlpha (0.36f * alpha);
                g.setColour (skin);

                juce::Path thumb;
                auto px = [&] (float v) { return area.getX() + area.getWidth() * v; };
                auto py = [&] (float v) { return area.getY() + area.getHeight() * v; };

                thumb.startNewSubPath (px (0.22f), py (0.56f));
                thumb.cubicTo (px (0.35f), py (0.48f), px (0.38f), py (0.28f), px (0.48f), py (0.14f));
                thumb.cubicTo (px (0.56f), py (0.03f), px (0.68f), py (0.12f), px (0.63f), py (0.27f));
                thumb.lineTo (px (0.58f), py (0.40f));
                thumb.lineTo (px (0.83f), py (0.40f));
                thumb.cubicTo (px (0.94f), py (0.40f), px (0.97f), py (0.50f), px (0.89f), py (0.56f));
                thumb.cubicTo (px (0.95f), py (0.63f), px (0.90f), py (0.72f), px (0.81f), py (0.72f));
                thumb.lineTo (px (0.76f), py (0.88f));
                thumb.cubicTo (px (0.74f), py (0.94f), px (0.67f), py (0.97f), px (0.57f), py (0.97f));
                thumb.lineTo (px (0.34f), py (0.97f));
                thumb.cubicTo (px (0.25f), py (0.97f), px (0.20f), py (0.90f), px (0.20f), py (0.80f));
                thumb.lineTo (px (0.20f), py (0.62f));
                thumb.closeSubPath();
                g.fillPath (thumb);

                g.setColour (skin.brighter (0.10f));
                g.fillRoundedRectangle (px (0.08f), py (0.58f),
                                        area.getWidth() * 0.20f,
                                        area.getHeight() * 0.34f, 3.0f);

                g.setColour (shade);
                g.drawLine (px (0.58f), py (0.55f), px (0.86f), py (0.55f), 1.0f);
                g.drawLine (px (0.56f), py (0.69f), px (0.82f), py (0.69f), 1.0f);
                g.drawLine (px (0.50f), py (0.83f), px (0.75f), py (0.83f), 1.0f);
                break;
            }

            case ReactionKind::love:
            {
                juce::Path heart;
                const float x = area.getX(), y = area.getY(), w = area.getWidth(), h = area.getHeight();
                heart.startNewSubPath (x + w * 0.50f, y + h * 0.88f);
                heart.cubicTo (x + w * 0.10f, y + h * 0.58f, x + w * 0.02f, y + h * 0.34f, x + w * 0.20f, y + h * 0.18f);
                heart.cubicTo (x + w * 0.34f, y + h * 0.05f, x + w * 0.47f, y + h * 0.16f, x + w * 0.50f, y + h * 0.29f);
                heart.cubicTo (x + w * 0.53f, y + h * 0.16f, x + w * 0.66f, y + h * 0.05f, x + w * 0.80f, y + h * 0.18f);
                heart.cubicTo (x + w * 0.98f, y + h * 0.34f, x + w * 0.90f, y + h * 0.58f, x + w * 0.50f, y + h * 0.88f);
                heart.closeSubPath();
                g.setColour (Colors::meterHot.withAlpha (alpha));
                g.fillPath (heart);
                break;
            }

            case ReactionKind::haha:
            {
                auto face = area.reduced (2.0f);
                g.setColour (Colors::meterMid.withAlpha (alpha));
                g.fillEllipse (face);
                g.setColour (Colors::panelDark.withAlpha (0.8f * alpha));
                const auto cx = face.getCentreX();
                const auto eyeY = face.getY() + face.getHeight() * 0.36f;
                g.drawLine (cx - face.getWidth() * 0.28f, eyeY,
                            cx - face.getWidth() * 0.12f, eyeY + 2.0f, 1.6f);
                g.drawLine (cx + face.getWidth() * 0.12f, eyeY + 2.0f,
                            cx + face.getWidth() * 0.28f, eyeY, 1.6f);
                g.drawEllipse (face.withSizeKeepingCentre (face.getWidth() * 0.48f,
                                                           face.getHeight() * 0.28f)
                                    .translated (0.0f, face.getHeight() * 0.20f),
                               1.8f);
                g.setColour (Colors::toolTrim.withAlpha (0.9f * alpha));
                g.fillEllipse (face.getX() - 1.0f, eyeY + 3.0f, 5.0f, 7.0f);
                g.fillEllipse (face.getRight() - 4.0f, eyeY + 3.0f, 5.0f, 7.0f);
                break;
            }

            case ReactionKind::huh:
            {
                g.setColour (Colors::meterHot.withAlpha (alpha));
                g.setFont (juce::Font (juce::FontOptions (area.getHeight() * 1.25f, juce::Font::bold)));
                g.drawText ("?", area.expanded (1.0f, 3.0f), juce::Justification::centred, false);
                break;
            }

            case ReactionKind::unknown:
            {
                g.setColour (juce::Colours::white.withAlpha (alpha));
                g.setFont (Colors::emojiFont (area.getHeight()));
                g.drawText (emoji, area, juce::Justification::centred, false);
                break;
            }
        }
    }

    void timerCallback() override
    {
        auto now = juce::Time::getMillisecondCounter();
        for (auto it = bubbles.begin(); it != bubbles.end();)
        {
            if ((int) (now - it->bornMs) >= lifeMs)
                it = bubbles.erase (it);
            else
                ++it;
        }
        if (! bubbles.empty())
            repaint();
        else
            stopTimer();
    }

    static constexpr int lifeMs = 3400;
    static constexpr float risePx = 165.0f;
    static constexpr size_t maxAlive = 5;
    std::vector<Bubble> bubbles;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReactionLayer)
};
