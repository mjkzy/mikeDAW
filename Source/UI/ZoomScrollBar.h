#pragma once
#include <JuceHeader.h>
#include "AppLookAndFeel.h"
#include "HoverFade.h"

class ZoomScrollBar : public juce::Component,
                      private juce::Timer
{
public:
    ZoomScrollBar() = default;
    static constexpr int barHeight = 22;

    std::function<void (double visibleStart, double visibleEnd)> onRangeChanged;

    std::function<void()> onDragEnd;

    void setRange (double totalContent, double viewStart, double viewEnd)
    {
        totalExtent = juce::jmax (1.0, totalContent);
        thumbStart = juce::jlimit (0.0, totalExtent, viewStart);
        thumbEnd = juce::jlimit (thumbStart + effectiveMinSpan(), totalExtent, viewEnd);
        if (maxSpan > 0.0)
            thumbEnd = juce::jmin (thumbEnd, thumbStart + maxSpan);
        repaint();
    }

    void setMaxSpan (double s) { maxSpan = juce::jmax (0.0, s); }

    void setMinSpan (double s) { minSpan = juce::jmax (0.0, s); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();

        g.setColour (Colors::panelDark);
        g.fillRoundedRectangle (r, 3.0f);

        auto thumb = getThumbRect();
        float hv = dragging != Drag::none ? 1.0f : hoverFade.value();
        g.setColour (Colors::scrollThumb.interpolatedWith (Colors::scrollThumbHover, hv));
        g.fillRoundedRectangle (thumb, 3.0f);

        g.setColour (Colors::outline);
        g.drawRoundedRectangle (thumb, 3.0f, 1.0f);

        float cx = thumb.getCentreY();
        g.setColour (Colors::textDim.withAlpha (0.6f));
        if (thumb.getWidth() > 24.0f)
        {
            g.fillRect (thumb.getX() + 4.0f, cx - 3.0f, 2.0f, 6.0f);
            g.fillRect (thumb.getRight() - 6.0f, cx - 3.0f, 2.0f, 6.0f);
        }

        float aw = arrowWidth();
        if (aw > 0.0f)
        {
            paintArrow (g, { 0.0f, 0.0f, aw, r.getHeight() }, true, isArrowHot (-1));
            paintArrow (g, { r.getWidth() - aw, 0.0f, aw, r.getHeight() }, false, isArrowHot (1));
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown())
            return;
        auto thumb = getThumbRect();
        float x = (float) e.getPosition().getX();

        if (int dir = arrowAt (x); dir != 0)
        {
            arrowDir = dir;
            panBy (dir);
            startTimer (350);
            return;
        }

        if (x < thumb.getX() + edgeZone && x >= thumb.getX() - 2)
            dragging = Drag::leftEdge;
        else if (x > thumb.getRight() - edgeZone && x <= thumb.getRight() + 2)
            dragging = Drag::rightEdge;
        else if (thumb.contains (e.getPosition().toFloat()))
            dragging = Drag::pan;
        else
        {
            double fraction = juce::jlimit (0.0, 1.0, (x - arrowWidth()) / (double) trackWidth());
            double halfThumb = (thumbEnd - thumbStart) / 2.0;
            thumbStart = juce::jlimit (0.0, totalExtent - (thumbEnd - thumbStart), fraction * totalExtent - halfThumb);
            thumbEnd = thumbStart + halfThumb * 2.0;
            dragging = Drag::pan;
            notify();
        }
        dragStartThumbStart = thumbStart;
        dragStartThumbEnd = thumbEnd;
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown() || arrowDir != 0)
            return;
        double pxPerUnit = trackWidth() / totalExtent;
        double dx = e.getDistanceFromDragStartX() / pxPerUnit;
        double smallest = effectiveMinSpan();

        if (dragging == Drag::pan)
        {
            double span = dragStartThumbEnd - dragStartThumbStart;
            thumbStart = juce::jlimit (0.0, totalExtent - span, dragStartThumbStart + dx);
            thumbEnd = thumbStart + span;
        }
        else if (dragging == Drag::leftEdge)
        {
            thumbStart = juce::jlimit (0.0, dragStartThumbEnd - smallest, dragStartThumbStart + dx);
            if (maxSpan > 0.0)
                thumbStart = juce::jmax (thumbStart, thumbEnd - maxSpan);
        }
        else if (dragging == Drag::rightEdge)
        {
            thumbEnd = juce::jlimit (dragStartThumbStart + smallest, totalExtent, dragStartThumbEnd + dx);
            if (maxSpan > 0.0)
                thumbEnd = juce::jmin (thumbEnd, thumbStart + maxSpan);
        }
        notify();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown())
            return;
        if (arrowDir != 0)
        {
            stopArrowRepeat();
            return;
        }
        bool wasDragging = dragging != Drag::none;
        dragging = Drag::none;
        repaint();
        if (wasDragging && onDragEnd)
            onDragEnd();
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        auto thumb = getThumbRect();
        float x = (float) e.getPosition().getX();
        int arrow = arrowAt (x);
        if (arrow != hoverArrow)
        {
            hoverArrow = arrow;
            repaint();
        }
        bool onEdge = arrow == 0
                      && ((x < thumb.getX() + edgeZone && x >= thumb.getX() - 2)
                          || (x > thumb.getRight() - edgeZone && x <= thumb.getRight() + 2));
        setMouseCursor (onEdge ? juce::MouseCursor::LeftRightResizeCursor
                               : juce::MouseCursor::NormalCursor);
    }

    void mouseEnter (const juce::MouseEvent&) override { hoverFade.setHovered (true); }

    void mouseExit (const juce::MouseEvent&) override
    {
        hoverFade.setHovered (false);
        hoverArrow = 0;
        if (arrowDir != 0)
            stopArrowRepeat();
        else
            repaint();
    }

private:
    double totalExtent = 1.0;
    double thumbStart = 0.0;
    double thumbEnd = 1.0;
    double dragStartThumbStart = 0.0;
    double dragStartThumbEnd = 1.0;
    double maxSpan = 0.0;
    double minSpan = 0.0;
    HoverFade hoverFade { *this };

    enum class Drag { none, pan, leftEdge, rightEdge };
    Drag dragging = Drag::none;

    int arrowDir = 0;
    int hoverArrow = 0;

    static constexpr float edgeZone = 7.0f;

    double effectiveMinSpan() const
    {
        return minSpan > 0.0 ? juce::jmin (minSpan, totalExtent) : 0.02 * totalExtent;
    }

    float arrowWidth() const { return getWidth() < 72 ? 0.0f : 16.0f; }
    float trackWidth() const { return juce::jmax (1.0f, getWidth() - 2.0f * arrowWidth()); }

    int arrowAt (float x) const
    {
        float aw = arrowWidth();
        if (aw <= 0.0f)
            return 0;
        if (x < aw)
            return -1;
        if (x >= getWidth() - aw)
            return 1;
        return 0;
    }

    bool isArrowHot (int dir) const { return arrowDir == dir || (arrowDir == 0 && hoverArrow == dir); }

    void paintArrow (juce::Graphics& g, juce::Rectangle<float> zone, bool pointLeft, bool hot) const
    {
        g.setColour (Colors::raised.withAlpha (hot ? 0.95f : 0.55f));
        g.fillRoundedRectangle (zone.reduced (2.0f), Metrics::rS);

        auto c = zone.getCentre();
        juce::Path p;
        if (pointLeft)
            p.addTriangle (c.x - 3.0f, c.y, c.x + 2.0f, c.y - 4.5f, c.x + 2.0f, c.y + 4.5f);
        else
            p.addTriangle (c.x + 3.0f, c.y, c.x - 2.0f, c.y - 4.5f, c.x - 2.0f, c.y + 4.5f);

        g.setColour (hot ? Colors::text : Colors::textDim);
        g.fillPath (p);
    }

    void panBy (int dir)
    {
        double span = thumbEnd - thumbStart;
        thumbStart = juce::jlimit (0.0, juce::jmax (0.0, totalExtent - span), thumbStart + dir * span * 0.2);
        thumbEnd = thumbStart + span;
        notify();
    }

    void stopArrowRepeat()
    {
        stopTimer();
        arrowDir = 0;
        repaint();
        if (onDragEnd)
            onDragEnd();
    }

    void timerCallback() override
    {
        if (arrowDir == 0)
        {
            stopTimer();
            return;
        }
        startTimer (60);
        panBy (arrowDir);
    }

    juce::Rectangle<float> getThumbRect() const
    {
        float aw = arrowWidth();
        float w = trackWidth();
        float x0 = aw + (float) (thumbStart / totalExtent * w);
        float x1 = aw + (float) (thumbEnd / totalExtent * w);
        return { x0, 0.0f, juce::jmax (12.0f, x1 - x0), (float) getHeight() };
    }

    void notify()
    {
        repaint();
        if (onRangeChanged)
            onRangeChanged (thumbStart, thumbEnd);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ZoomScrollBar)
};
