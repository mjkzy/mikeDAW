#pragma once
#include <JuceHeader.h>
#include "AppLookAndFeel.h"
#include "Icons.h"

class DockHeader : public juce::Component,
                   public juce::TooltipClient
{
public:
    struct Tab
    {
        juce::String name, label, shortcut;
        bool operator== (const Tab& o) const { return name == o.name && label == o.label && shortcut == o.shortcut; }
    };

    static constexpr int height = 36;

    std::function<void (const juce::String&)> onTabClicked;
    std::function<void()> onToggleCollapsed;
    std::function<void()> onResizeStart;
    std::function<void (int)> onResizeDrag;
    std::function<void()> onResizeEnd;

    DockHeader() { setOpaque (true); }

    void setTabs (std::vector<Tab> newTabs)
    {
        if (newTabs == tabs)
            return;
        tabs = std::move (newTabs);
        layoutTabs();
        repaint();
    }

    void setState (const juce::String& activeName, bool isCollapsed)
    {
        if (activeName == active && isCollapsed == collapsed)
            return;
        active = activeName;
        collapsed = isCollapsed;
        repaint();
    }

    void setBreadcrumb (const juce::String& primaryText, const juce::String& secondaryText)
    {
        if (primaryText == primary && secondaryText == secondary)
            return;
        primary = primaryText;
        secondary = secondaryText;
        layoutTabs();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        g.fillAll (Colors::header);
        g.setColour (Colors::outline);
        g.fillRect (r.removeFromTop (1));

        auto crumb = breadcrumbArea();
        g.setFont (Typography::semi (13.5f));
        g.setColour (Colors::text);
        auto primaryW = juce::GlyphArrangement::getStringWidthInt (Typography::semi (13.5f), primary);
        g.drawText (primary, crumb.removeFromLeft (primaryW + 2), juce::Justification::centredLeft, false);
        if (secondary.isNotEmpty())
        {
            g.setColour (Colors::textDim.withAlpha (0.6f));
            g.setFont (Typography::ui (13.5f));
            g.drawText ("/", crumb.removeFromLeft (18), juce::Justification::centred, false);
            g.setColour (Colors::textDim);
            g.drawText (secondary, crumb, juce::Justification::centredLeft, true);
        }

        if (! tabRects.empty())
        {
            auto well = segmentBounds().toFloat();
            g.setColour (Colors::outline);
            g.fillRect (juce::Rectangle<float> (well.getX() - 12.0f, well.getY() + 3.0f, 1.0f, well.getHeight() - 6.0f));
            g.setColour (Colors::panelDark);
            g.fillRoundedRectangle (well, 7.0f);
            g.setColour (Colors::outline);
            g.drawRoundedRectangle (well.reduced (0.5f), 7.0f, 1.0f);

            for (size_t i = 0; i < tabs.size(); ++i)
            {
                auto tr = tabRects[i].toFloat();
                const bool isActive = tabs[i].name == active && ! collapsed;
                const bool isHover = (int) i == hoverTab;
                if (isActive)
                {
                    g.setColour (Colors::raised);
                    g.fillRoundedRectangle (tr.reduced (2.0f), 5.0f);
                }
                else if (isHover)
                {
                    g.setColour (Colors::raised.withAlpha (0.45f));
                    g.fillRoundedRectangle (tr.reduced (2.0f), 5.0f);
                }
                g.setColour (isActive ? Colors::text : Colors::textDim.interpolatedWith (Colors::text, isHover ? 0.5f : 0.0f));
                g.setFont (isActive ? Typography::semi (13.0f) : Typography::ui (13.0f));
                g.drawText (tabs[i].label, tr.toNearestInt(), juce::Justification::centred, false);
            }
        }

        auto chev = collapseArea().toFloat();
        if (hoverCollapse)
        {
            g.setColour (Colors::raised.withAlpha (0.6f));
            g.fillRoundedRectangle (chev.reduced (3.0f), 5.0f);
        }
        Icons::draw (g, collapsed ? Icons::Id::chevronUp : Icons::Id::chevronDown,
                     chev.withSizeKeepingCentre (16.0f, 16.0f),
                     hoverCollapse ? Colors::text : Colors::textDim, 1.6f);
    }

    void resized() override { layoutTabs(); }

    void mouseMove (const juce::MouseEvent& e) override
    {
        auto p = e.getPosition();
        int t = tabAt (p);
        bool c = collapseArea().contains (p);
        if (t != hoverTab || c != hoverCollapse)
        {
            hoverTab = t;
            hoverCollapse = c;
            repaint();
        }
        setMouseCursor (t < 0 && ! c ? juce::MouseCursor::UpDownResizeCursor
                                     : juce::MouseCursor::PointingHandCursor);
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (hoverTab >= 0 || hoverCollapse)
        {
            hoverTab = -1;
            hoverCollapse = false;
            repaint();
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = false;
        if (e.mods.isMiddleButtonDown())
            return;
        auto p = e.getPosition();
        if (collapseArea().contains (p))
        {
            if (onToggleCollapsed) onToggleCollapsed();
            return;
        }
        if (auto t = tabAt (p); t >= 0)
        {
            if (onTabClicked) onTabClicked (tabs[(size_t) t].name);
            return;
        }
        if (e.getNumberOfClicks() >= 2)
        {
            if (onToggleCollapsed) onToggleCollapsed();
            return;
        }
        dragging = true;
        if (onResizeStart) onResizeStart();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging && onResizeDrag)
            onResizeDrag (e.getDistanceFromDragStartY());
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging && onResizeEnd)
            onResizeEnd();
        dragging = false;
    }

    juce::String getTooltip() override
    {
        auto t = tabAt (getMouseXYRelative());
        if (t >= 0 && tabs[(size_t) t].shortcut.isNotEmpty())
            return tabs[(size_t) t].label + " (" + tabs[(size_t) t].shortcut + ")";
        if (collapseArea().contains (getMouseXYRelative()))
            return collapsed ? "Expand editor (F5)" : "Collapse editor (F5)";
        return {};
    }

private:
    juce::Rectangle<int> collapseArea() const { return { getWidth() - 36, 1, 32, getHeight() - 1 }; }

    juce::Rectangle<int> breadcrumbArea() const
    {
        return getLocalBounds().withTrimmedTop (1).withTrimmedLeft (14).withWidth (breadcrumbWidth);
    }

    juce::Rectangle<int> segmentBounds() const
    {
        if (tabRects.empty())
            return {};
        return tabRects.front().getUnion (tabRects.back()).expanded (2, 0);
    }

    int tabAt (juce::Point<int> p) const
    {
        for (size_t i = 0; i < tabRects.size(); ++i)
            if (tabRects[i].contains (p))
                return (int) i;
        return -1;
    }

    void layoutTabs()
    {
        auto tabFont = Typography::semi (13.0f);
        int naturalCrumb = juce::GlyphArrangement::getStringWidthInt (Typography::semi (13.5f), primary)
                         + (secondary.isNotEmpty()
                                ? 18 + juce::GlyphArrangement::getStringWidthInt (Typography::ui (13.5f), secondary)
                                : 0)
                         + 4;

        int tabsW = 0;
        std::vector<int> widths;
        for (auto& t : tabs)
        {
            widths.push_back (juce::GlyphArrangement::getStringWidthInt (tabFont, t.label) + 26);
            tabsW += widths.back();
        }

        int available = getWidth() - 14 - 36 - 28 - tabsW;
        breadcrumbWidth = juce::jlimit (0, juce::jmax (0, available), naturalCrumb);

        tabRects.clear();
        int x = 14 + breadcrumbWidth + 26;
        const int tabH = 26;
        const int y = 1 + (getHeight() - 1 - tabH) / 2;
        for (auto w : widths)
        {
            tabRects.push_back ({ x, y, w, tabH });
            x += w;
        }
    }

    std::vector<Tab> tabs;
    std::vector<juce::Rectangle<int>> tabRects;
    juce::String active, primary, secondary;
    int breadcrumbWidth = 0;
    int hoverTab = -1;
    bool hoverCollapse = false;
    bool collapsed = false;
    bool dragging = false;
};

class StatusBar : public juce::Component,
                  private juce::Timer
{
public:
    static constexpr int height = 26;

    std::function<juce::String()> getSessionText;
    std::function<bool()> isSessionActive;
    std::function<juce::String()> getEngineText;
    std::function<juce::String()> getFormatText;
    std::function<float()> getCpuLoad;
    std::function<void()> onSessionClicked;

    StatusBar()
    {
        setOpaque (true);
        startTimerHz (4);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        g.fillAll (Colors::windowBg);
        g.setColour (Colors::outline);
        g.fillRect (r.removeFromTop (1));

        auto font = Typography::ui (12.5f);
        g.setFont (font);

        auto left = sessionArea().toFloat();
        auto iconBox = left.removeFromLeft (16.0f).withSizeKeepingCentre (14.0f, 14.0f);
        auto sessionColour = sessionOn ? Colors::success : Colors::textDim;
        Icons::draw (g, sessionOn ? Icons::Id::users : Icons::Id::cloud, iconBox,
                     hoverSession ? Colors::text : sessionColour, 1.4f);
        g.setColour (hoverSession ? Colors::text : Colors::textDim);
        g.drawText (sessionText, left.withTrimmedLeft (8.0f).toNearestInt(), juce::Justification::centredLeft, true);

        auto right = r.reduced (14, 0);
        auto drawPair = [&] (const juce::String& label, const juce::String& value, juce::Colour valueColour)
        {
            int vw = juce::GlyphArrangement::getStringWidthInt (font, value);
            int lw = juce::GlyphArrangement::getStringWidthInt (font, label);
            g.setColour (valueColour);
            g.drawText (value, right.removeFromRight (vw + 2), juce::Justification::centredRight, false);
            g.setColour (Colors::textDim);
            g.drawText (label, right.removeFromRight (lw + 6), juce::Justification::centredRight, false);
            right.removeFromRight (22);
        };

        auto cpuColour = cpuPercent >= 85 ? Colors::danger
                       : cpuPercent >= 60 ? Colors::warning
                                          : Colors::text;
        drawPair ("CPU:", juce::String (cpuPercent) + "%", cpuColour);
        if (formatText.isNotEmpty())
            drawPair ("", formatText, Colors::text);
        drawPair ("Audio engine:", engineText, engineText == "Ready" ? Colors::text : Colors::warning);
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        bool h = sessionArea().contains (e.getPosition());
        if (h != hoverSession)
        {
            hoverSession = h;
            setMouseCursor (h ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
            repaint();
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (hoverSession)
        {
            hoverSession = false;
            repaint();
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isLeftButtonDown() && sessionArea().contains (e.getPosition()) && onSessionClicked)
            onSessionClicked();
    }

private:
    juce::Rectangle<int> sessionArea() const
    {
        int w = juce::GlyphArrangement::getStringWidthInt (Typography::ui (12.5f), sessionText) + 16 + 8 + 6;
        return { 12, 1, w, getHeight() - 1 };
    }

    void timerCallback() override
    {
        auto s = getSessionText ? getSessionText() : juce::String();
        auto on = isSessionActive ? isSessionActive() : false;
        auto e = getEngineText ? getEngineText() : juce::String();
        auto f = getFormatText ? getFormatText() : juce::String();
        auto c = getCpuLoad ? juce::jlimit (0, 999, juce::roundToInt (getCpuLoad() * 100.0f)) : 0;
        if (s == sessionText && on == sessionOn && e == engineText && f == formatText && c == cpuPercent)
            return;
        sessionText = s;
        sessionOn = on;
        engineText = e;
        formatText = f;
        cpuPercent = c;
        repaint();
    }

    juce::String sessionText, engineText, formatText;
    bool sessionOn = false;
    bool hoverSession = false;
    int cpuPercent = 0;
};
