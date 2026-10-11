#pragma once
#include <JuceHeader.h>
#include "AppLookAndFeel.h"
#include "HoverFade.h"
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

class FloatingPanel : public juce::Component,
                      public juce::TooltipClient
{
public:
    FloatingPanel (const juce::String& panelTitle, juce::Component* contentToOwn,
                   std::function<void()> onCloseFn = {})
        : title (panelTitle), content (contentToOwn), onClose (std::move (onCloseFn))
    {
        addAndMakeVisible (content.get());

        content->addMouseListener (&raiser, true);
        raiser.onClick = [this]
        {
            if (! docked)
            {
                toFront (true);
                raiseChildPeers();
            }
            if (onActivated) onActivated (this);
        };

        resizer = std::make_unique<juce::ResizableBorderComponent> (this, &constrainer);
        addAndMakeVisible (*resizer);
        constrainer.setMinimumSize (200, 80);
        constrainer.setMinimumOnscreenAmounts (titleHeight, 60, titleHeight, 60);
        minimisedConstrainer.setMinimumOnscreenAmounts (titleHeight, 60, titleHeight, 60);

        setOpaque (true);
        setPaintingIsUnclipped (true);
        setSize (600, 400);
    }

    ~FloatingPanel() override
    {
        if (content != nullptr)
            content->removeMouseListener (&raiser);
    }

    void setTitleSuffix (const juce::String& suffix)
    {
        titleSuffix = suffix;
        repaint();
        if (onTitleChanged)
            onTitleChanged();
    }

    const juce::String& getTitle() const       { return title; }
    const juce::String& getTitleSuffix() const { return titleSuffix; }
    std::function<void()> onTitleChanged;

    void setDocked (bool shouldDock, bool withTitleBar)
    {
        if (shouldDock && minimised)
            toggleMinimised();
        docked = shouldDock;
        dockedTitleBar = withTitleBar;
        if (docked)
            maximised = false;
        resizer->setVisible (! docked && resizableFlag && ! maximised && ! minimised);
        resized();
        repaint();
    }

    bool isDocked() const { return docked; }

    int barHeight() const
    {
        if (docked)
            return dockedTitleBar ? dockedBarHeight : 0;
        return titleHeight;
    }

    void setTitleBarComponent (juce::Component* c, int widthPx)
    {
        titleBarComp = c;
        titleBarWidth = widthPx;
        showTitleText = (c == nullptr);
        if (c != nullptr)
            addAndMakeVisible (c);
        resized();
        repaint();
    }

    void setPeerBadges (std::vector<std::pair<juce::int64, juce::String>> badges)
    {
        if (badges == peerBadges)
            return;
        peerBadges = std::move (badges);
        setMouseCursor (juce::MouseCursor::NormalCursor);
        repaint();
    }

    juce::Component* getContent() const { return content.get(); }

    void setDefaultPlacement (juce::Rectangle<float> fractionOfDesk, juce::Point<int> minSize)
    {
        defaultFraction = fractionOfDesk;
        minPanelSize = minSize;
    }

    void setMinimumPanelSize (int width, int height)
    {
        minPanelSize = { juce::jmax (200, width), juce::jmax (80, height) };
        constrainer.setMinimumSize (minPanelSize.x, minPanelSize.y);
        if (! maximised && ! minimised
            && (getWidth() < minPanelSize.x || getHeight() < minPanelSize.y))
            setSize (juce::jmax (getWidth(), minPanelSize.x),
                     juce::jmax (getHeight(), minPanelSize.y));
    }

    void materialiseDefault (juce::Rectangle<int> desk)
    {
        int w = juce::jmax (minPanelSize.x, juce::roundToInt (defaultFraction.getWidth()  * (float) desk.getWidth()));
        int h = juce::jmax (minPanelSize.y, juce::roundToInt (defaultFraction.getHeight() * (float) desk.getHeight()));
        int x = juce::roundToInt (defaultFraction.getX() * (float) desk.getWidth());
        int y = juce::roundToInt (defaultFraction.getY() * (float) desk.getHeight());
        setBounds (x, y, w, h);
        clampIntoDesk (desk);
    }

    void clampIntoDesk (juce::Rectangle<int> desk)
    {
        if (maximised)
        {
            setBounds (desk);
            return;
        }
        auto b = getBounds();
        if (! minimised)
        {
            int w = juce::jmin (b.getWidth(),  desk.getWidth());
            int h = juce::jmin (b.getHeight(), desk.getHeight());
            w = juce::jmax (w, juce::jmin (minPanelSize.x, desk.getWidth()));
            h = juce::jmax (h, juce::jmin (minPanelSize.y, desk.getHeight()));
            if (maxContentHeight > 0)
                h = juce::jmin (h, maxContentHeight);
            if (maxContentWidth > 0)
                w = juce::jmin (w, maxContentWidth);
            b = b.withSize (w, h);
        }
        int x = juce::jlimit (0, juce::jmax (0, desk.getWidth()  - b.getWidth()),  b.getX());
        int y = juce::jlimit (0, juce::jmax (0, desk.getHeight() - b.getHeight()), b.getY());
        setBounds (b.withPosition (x, y));
    }

    void setMaxHeight (int h)
    {
        maxContentHeight = h;
        applyMaxContentSize();
    }

    void setMaxWidth (int w)
    {
        maxContentWidth = w;
        applyMaxContentSize();
    }

    void setResizable (bool canResize)
    {
        resizableFlag = canResize;
        resizer->setVisible (canResize && ! maximised && ! minimised);
    }

    std::function<void (FloatingPanel*)> onActivated;

    bool isMaximised() const { return maximised; }
    bool isMinimised() const { return minimised; }

    void toggleMaximised()
    {
        if (docked)
            return;
        if (minimised)
            toggleMinimised();

        if (! maximised)
        {
            if (getParentComponent() == nullptr)
                return;
            restoreBounds = getBounds();
            maximised = true;
            setBounds (getParentComponent()->getLocalBounds());
        }
        else
        {
            maximised = false;
            auto target = restoreBounds;
            if (auto* parent = getParentComponent())
            {
                auto desk = parent->getLocalBounds();
                bool fillsDesk = target.getWidth()  >= desk.getWidth()  * 85 / 100
                              && target.getHeight() >= desk.getHeight() * 85 / 100;
                if (target.isEmpty() || fillsDesk)
                    target = desk.withSizeKeepingCentre (desk.getWidth() * 7 / 10, desk.getHeight() * 7 / 10);
                setBounds (target);
                clampIntoDesk (desk);
            }
            else
            {
                setBounds (target);
            }
        }
        resizer->setVisible (! maximised && resizableFlag);
        repaint();
    }

    void toggleMinimised()
    {
        if (docked && ! minimised)
            return;
        if (! minimised)
        {
            minRestoreBounds = getBounds();
            minimised = true;
            maximised = false;
            content->setVisible (false);
            resizer->setVisible (false);
            setBounds (getX(), getY(), 240, titleHeight);
        }
        else
        {
            minimised = false;
            setBounds (minRestoreBounds.withPosition (getPosition()));
            content->setVisible (true);
            resizer->setVisible (resizableFlag);
            if (auto* parent = getParentComponent())
                clampIntoDesk (parent->getLocalBounds());
        }
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        if (docked)
        {
            paintDocked (g);
            return;
        }

        auto r = getLocalBounds();
        g.setColour (Colors::windowBg);
        g.fillRect (r);
        g.setColour (Colors::panelBg);
        g.fillRoundedRectangle (r.toFloat(), 4.0f);

        auto bar = r.removeFromTop (titleHeight);
        g.setColour (Colors::header);
        g.fillRoundedRectangle (bar.toFloat().withTrimmedBottom (-4.0f), 4.0f);

        if (showTitleText)
        {
            g.setColour (Colors::text);
            g.setFont (Typography::caps (13.5f));
            g.drawText (title + titleSuffix, bar.withTrimmedLeft (10).withTrimmedRight (peerBadges.empty() ? 78 : 200),
                        juce::Justification::centredLeft);
        }

        if (! peerBadges.empty())
            paintPeerBadges (g, bar);

        {
            auto a = getMinArea();
            auto af = a.toFloat();
            g.setColour (Colors::textDim.interpolatedWith (Colors::text, minFade.value()));
            g.fillRoundedRectangle (af.getCentreX() - 5.0f, af.getCentreY() + 3.0f, 10.0f, 1.6f, 0.8f);
        }

        {
            auto a = getMaxArea();
            auto af = a.toFloat();
            auto maxCol = Colors::textDim.interpolatedWith (Colors::text, maxFade.value());
            g.setColour (maxCol);
            if (maximised)
            {
                float s = 8.0f, off = 2.5f;
                auto front = juce::Rectangle<float> (0, 0, s, s)
                                 .withCentre (af.getCentre()).translated (-off * 0.5f, off * 0.5f);
                auto back = front.translated (off, -off);
                g.drawRect (back, 1.2f);
                g.setColour (Colors::header);
                g.fillRect (front);
                g.setColour (maxCol);
                g.drawRect (front, 1.2f);
            }
            else
            {
                auto sq = juce::Rectangle<float> (0, 0, 10.0f, 10.0f).withCentre (af.getCentre());
                g.drawRect (sq, 1.3f);
            }
        }

        auto closeArea = getCloseArea().toFloat().withSizeKeepingCentre (11.0f, 11.0f);
        g.setColour (Colors::textDim.interpolatedWith (Colors::accent2, closeFade.value()));
        g.drawLine (closeArea.getX(), closeArea.getY(), closeArea.getRight(), closeArea.getBottom(), 1.6f);
        g.drawLine (closeArea.getX(), closeArea.getBottom(), closeArea.getRight(), closeArea.getY(), 1.6f);

        g.setColour (Colors::outline);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 4.0f, 1.0f);
    }

    void resized() override
    {
        if (docked)
        {
            content->setBounds (getLocalBounds().withTrimmedTop (barHeight()));
            if (titleBarComp != nullptr)
            {
                titleBarComp->setVisible (dockedTitleBar);
                titleBarComp->setBounds (8, (dockedBarHeight - titleHeight) / 2,
                                         juce::jmin (titleBarWidth, juce::jmax (0, getWidth() - 16)), titleHeight);
            }
            resizer->setBounds (getLocalBounds());
            return;
        }

        content->setBounds (getLocalBounds().withTrimmedTop (titleHeight).reduced (2));
        if (titleBarComp != nullptr)
            titleBarComp->setBounds (6, 0, juce::jmin (titleBarWidth, juce::jmax (0, getWidth() - 84)), titleHeight);
        resizer->setBounds (getLocalBounds());
        resizer->setBorderThickness (juce::BorderSize<int> (4));
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown())
            return;
        if (docked)
        {
            if (onActivated)
                onActivated (this);
            return;
        }
        toFront (true);
        raiseChildPeers();
        if (onActivated)
            onActivated (this);
        if (e.getPosition().getY() < titleHeight)
        {
            if (getCloseArea().contains (e.getPosition()))
            {
                setVisible (false);
                if (onClose)
                    onClose();
                return;
            }
            if (getMaxArea().contains (e.getPosition()))
            {
                toggleMaximised();
                return;
            }
            if (getMinArea().contains (e.getPosition()))
            {
                toggleMinimised();
                return;
            }
            if (e.getNumberOfClicks() == 2)
            {
                dragging = false;
                if (minimised)
                    toggleMinimised();
                else
                    toggleMaximised();
                return;
            }
            if (! maximised)
            {
                dragger.startDraggingComponent (this, e);
                dragging = true;
                setBufferedToImage (true);
            }
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown())
            return;
        if (dragging)
        {
            dragger.dragComponent (this, e, minimised ? &minimisedConstrainer : &constrainer);
            if (onDragged)
                onDragged();
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown())
            return;
        if (dragging)
            setBufferedToImage (false);
        dragging = false;
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        if (docked)
            return;
        bool close = getCloseArea().contains (e.getPosition());
        bool maxi = getMaxArea().contains (e.getPosition());
        bool mini = getMinArea().contains (e.getPosition());
        if (close != closeHovered || maxi != maxHovered || mini != minHovered)
        {
            closeHovered = close;
            maxHovered = maxi;
            minHovered = mini;
            closeFade.setHovered (close);
            maxFade.setHovered (maxi);
            minFade.setHovered (mini);
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (closeHovered || maxHovered || minHovered)
        {
            closeHovered = maxHovered = minHovered = false;
            closeFade.setHovered (false);
            maxFade.setHovered (false);
            minFade.setHovered (false);
        }
    }

    juce::String getTooltip() override
    {
        auto p = getMouseXYRelative();
        if (peerBadges.empty() || p.getY() >= barHeight() || p.getX() > badgeRight() + 2 || p.getX() < badgeRight() - 120)
            return {};
        juce::StringArray names;
        for (auto& b : peerBadges)
            names.add (b.second);
        return names.joinIntoString (", ") + " here";
    }

    static constexpr int titleHeight = 34;
    static constexpr int dockedBarHeight = 34;

    void raiseChildPeers()
    {
      #if JUCE_WINDOWS
        forEachOwnedHwnd ([](HWND ch)
        {
            ::SetWindowPos (ch, (HWND) 0, 0, 0, 0, 0,
                            0x0002 | 0x0001 | 0x0010);
        });
      #else
        walkChildPeers (this);
      #endif
    }

    void setNativePeersCover (const juce::Array<juce::Rectangle<int>>& coversPhysical)
    {
        auto origin = getScreenPosition();
        if (coversPhysical == lastCovers && origin == lastCoverOrigin)
            return;
        lastCovers = coversPhysical;
        lastCoverOrigin = origin;

      #if JUCE_WINDOWS
        forEachOwnedHwnd ([this] (HWND ch)
        {
            RECT wr;
            if (! ::GetWindowRect (ch, &wr))
                return;
            const int w = wr.right - wr.left, h = wr.bottom - wr.top;

            HRGN rgn = ::CreateRectRgn (0, 0, w, h);
            bool cut = false;
            for (auto c : lastCovers)
            {
                auto local = c.translated (-wr.left, -wr.top)
                                .getIntersection ({ 0, 0, w, h });
                if (local.isEmpty())
                    continue;
                HRGN hole = ::CreateRectRgn (local.getX(), local.getY(),
                                             local.getRight(), local.getBottom());
                ::CombineRgn (rgn, rgn, hole, RGN_DIFF);
                ::DeleteObject (hole);
                cut = true;
            }

            if (cut)
                ::SetWindowRgn (ch, rgn, TRUE);
            else
            {
                ::DeleteObject (rgn);
                ::SetWindowRgn (ch, nullptr, TRUE);
            }
        });
      #endif
    }

    std::function<void()> onDragged;

    std::function<juce::Rectangle<int>()> nativeRectProvider;

private:
    static void walkChildPeers (juce::Component* c)
    {
        for (int i = 0; i < c->getNumChildComponents(); ++i)
        {
            auto* child = c->getChildComponent (i);
            if (child->isOnDesktop())
                child->toFront (false);
            walkChildPeers (child);
        }
    }
    int badgeRight() const { return docked ? getWidth() - 10 : getWidth() - 80; }

    void paintPeerBadges (juce::Graphics& g, juce::Rectangle<int> bar)
    {
        const float chipH = 15.0f, dot = 6.0f, gap = 4.0f;
        const float cy = (float) bar.getCentreY();
        const float leftLimit = (float) (badgeRight() - 118);
        float right = (float) badgeRight();
        auto chipFont = Typography::ui (10.0f);
        for (auto it = peerBadges.rbegin(); it != peerBadges.rend(); ++it)
        {
            auto name = it->second.substring (0, 10);
            float tw = (float) juce::GlyphArrangement::getStringWidthInt (chipFont, name);
            float chipW = dot + tw + 22.0f;
            float x = right - chipW;
            if (x < leftLimit)
                break;

            juce::Rectangle<float> chip (x, cy - chipH * 0.5f, chipW, chipH);
            auto col = Colors::peerColour (it->first);
            g.setColour (col.withAlpha (0.18f));
            g.fillRoundedRectangle (chip, Metrics::rS);
            g.setColour (col);
            g.fillEllipse (chip.getX() + 6.0f, cy - dot * 0.5f, dot, dot);
            g.setColour (Colors::text);
            g.setFont (chipFont);
            g.drawText (name, chip.withTrimmedLeft (dot + 10.0f).withTrimmedRight (6.0f),
                        juce::Justification::centredLeft);
            right = x - gap;
        }
    }

    void paintDocked (juce::Graphics& g)
    {
        g.fillAll (Colors::panelBg);
        if (barHeight() <= 0)
            return;

        auto bar = getLocalBounds().removeFromTop (barHeight());
        g.setColour (Colors::header);
        g.fillRect (bar);
        g.setColour (Colors::outline);
        g.fillRect (bar.removeFromBottom (1));

        if (showTitleText)
        {
            g.setColour (Colors::text);
            g.setFont (Typography::semi (13.5f));
            g.drawText (title + titleSuffix, bar.withTrimmedLeft (14).withTrimmedRight (130),
                        juce::Justification::centredLeft);
        }

        if (! peerBadges.empty())
            paintPeerBadges (g, bar);
    }

    juce::Rectangle<int> getCloseArea() const { return { getWidth() - 24, 0, 24, titleHeight }; }
    juce::Rectangle<int> getMaxArea()   const { return { getWidth() - 48, 0, 24, titleHeight }; }
    juce::Rectangle<int> getMinArea()   const { return { getWidth() - 72, 0, 24, titleHeight }; }

    void applyMaxContentSize()
    {
        constrainer.setMaximumWidth  (maxContentWidth  > 0 ? maxContentWidth  : 0x3fffffff);
        constrainer.setMaximumHeight (maxContentHeight > 0 ? maxContentHeight : 0x3fffffff);

        if (maximised || minimised || docked)
            return;

        auto b = getBounds();
        int w = maxContentWidth  > 0 ? juce::jmin (b.getWidth(),  maxContentWidth)  : b.getWidth();
        int h = maxContentHeight > 0 ? juce::jmin (b.getHeight(), maxContentHeight) : b.getHeight();
        if (w != b.getWidth() || h != b.getHeight())
            setBounds (b.withSize (w, h));
    }

    struct Raiser : public juce::MouseListener
    {
        void mouseDown (const juce::MouseEvent& e) override
        {
            if (e.mods.isMiddleButtonDown())
                return;
            if (onClick) onClick();
        }
        std::function<void()> onClick;
    };

    juce::String title, titleSuffix;
    juce::Component* titleBarComp = nullptr;
    int titleBarWidth = 0;
    bool showTitleText = true;
    std::vector<std::pair<juce::int64, juce::String>> peerBadges;
    std::unique_ptr<juce::Component> content;
    std::function<void()> onClose;
    Raiser raiser;
    juce::ComponentDragger dragger;
    juce::ComponentBoundsConstrainer constrainer;
    juce::ComponentBoundsConstrainer minimisedConstrainer;
    std::unique_ptr<juce::ResizableBorderComponent> resizer;
    juce::Rectangle<int> restoreBounds, minRestoreBounds;
    juce::Rectangle<float> defaultFraction;
    juce::Point<int> minPanelSize { 200, 80 };
    int maxContentHeight = 0;
    int maxContentWidth = 0;
    juce::Array<juce::Rectangle<int>> lastCovers;
    juce::Point<int> lastCoverOrigin;

  #if JUCE_WINDOWS
    template <typename Fn>
    void forEachOwnedHwnd (Fn&& fn)
    {
        auto* top = getTopLevelComponent();
        if (top == nullptr) return;
        auto* peer = top->getPeer();
        if (peer == nullptr) return;

        auto topHwnd = static_cast<HWND> (peer->getNativeHandle());
        auto sb = content->getScreenBounds();
        const bool haveProvider = nativeRectProvider != nullptr;
        auto owned = haveProvider ? nativeRectProvider() : juce::Rectangle<int>();
        if (haveProvider && owned.isEmpty())
            return;
        owned = juce::Desktop::getInstance().getDisplays().logicalToPhysical (owned);

        for (auto ch = ::GetWindow (topHwnd, 5); ch != nullptr;
             ch = ::GetWindow (ch, 2))
        {
            RECT r;
            if (! ::GetWindowRect (ch, &r))
                continue;

            if (haveProvider)
            {
                if (std::abs (r.left - owned.getX()) <= 4
                    && std::abs (r.top - owned.getY()) <= 4)
                    fn (ch);
                continue;
            }

            int cx = (r.left + r.right) / 2;
            int cy = (r.top + r.bottom) / 2;
            if (sb.contains (cx, cy))
                fn (ch);
        }
    }
  #endif

    bool dragging = false, closeHovered = false, maxHovered = false, minHovered = false;
    bool maximised = false, minimised = false;
    bool resizableFlag = true;
    bool docked = false, dockedTitleBar = true;

    HoverFade closeFade { *this }, maxFade { *this }, minFade { *this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FloatingPanel)
};
