#pragma once
#include <JuceHeader.h>
#include <algorithm>
#include <set>
#include <vector>
#include "AppLookAndFeel.h"
#include "../Net/FriendsManager.h"

class FriendsPanel : public juce::Component,
                     private juce::Timer
{
public:
    explicit FriendsPanel (FriendsManager& fm) : friends (fm)
    {
        setOpaque (true);

        addAndMakeVisible (profile);
        profile.onHost  = [this] { if (onHostSession) onHostSession(); };
        profile.onJoin  = [this] { if (onJoinSession) onJoinSession(); };
        profile.onLeave = [this] { if (onLeaveSession) onLeaveSession(); };
        profile.onSetAway = [this] (bool away) { if (onSetAway) onSetAway (away); rebuild(); };

        viewport.setViewedComponent (&content, false);
        viewport.setScrollBarsShown (true, false);
        viewport.setScrollBarThickness (6);
        addAndMakeVisible (viewport);

        search.setMultiLine (false);
        search.setReturnKeyStartsNewLine (false);
        search.setJustification (juce::Justification::centredLeft);
        search.setTextToShowWhenEmpty ("Search friends", Colors::textDim);
        search.setFont (Typography::ui (12.5f));
        search.setIndents (10, 5);
        search.onTextChange = [this]
        {
            searchText = search.getText().trim().toLowerCase();
            rebuildRows (true);
        };
        addAndMakeVisible (search);

        startTimerHz (1);
        rebuild();
    }

    std::function<bool()> isHosting;
    std::function<bool()> isSessionActive;
    std::function<bool()> isLocalAway;
    std::function<void (bool away)> onSetAway;
    std::function<void()> onHostSession;
    std::function<void()> onJoinSession;
    std::function<void()> onLeaveSession;
    std::function<void (juce::int64)> onJoinFriend;
    std::function<void (juce::int64)> onInvite;
    std::function<void (juce::int64)> onRemoveFriend;
    std::function<void (juce::int64, juce::String)> onAddFriend;
    std::function<void (juce::int64, juce::String)> onAcceptRequest;
    std::function<void (juce::int64)> onDeclineRequest;

    std::function<juce::Image()> getLocalAvatar;
    std::function<juce::String()> getLocalName;
    std::function<juce::String()> getLocalStatus;

    void refresh() { rebuild(); }

    void resized() override
    {
        auto r = getLocalBounds();
        profile.setBounds (r.removeFromTop (profileH));
        search.setBounds (r.removeFromTop (searchH).reduced (11, 5));
        viewport.setBounds (r);
        rebuildRows (true);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colors::panelBg);
    }

private:
    static constexpr int profileH = 64;
    static constexpr int searchH  = 36;
    static constexpr int rowH     = 50;
    static constexpr int captionH = 24;
    static constexpr int sideMargin = 12;
    static constexpr int avatarD  = 34;
    static constexpr int textGap  = 9;

    static void paintAvatar (juce::Graphics& g, juce::Rectangle<float> circle,
                             const juce::Image& img, juce::int64 id,
                             const juce::String& name, float alpha)
    {
        if (img.isValid())
        {
            juce::Graphics::ScopedSaveState state (g);
            juce::Path clip;
            clip.addEllipse (circle);
            g.reduceClipRegion (clip);
            g.setOpacity (alpha);
            g.drawImage (img, circle,
                         juce::RectanglePlacement::centred | juce::RectanglePlacement::fillDestination);
        }
        else
        {
            g.setColour (Colors::peerColour (id).withAlpha (alpha));
            g.fillEllipse (circle);
            g.setColour (Colors::windowBg.withAlpha (alpha));
            g.setFont (Typography::semi (circle.getHeight() * 0.44f));
            g.drawText (name.substring (0, 1).toUpperCase(), circle, juce::Justification::centred);
        }

        g.setOpacity (1.0f);
    }

    static void paintStatusDot (juce::Graphics& g, juce::Rectangle<float> circle,
                                juce::Colour colour, juce::Colour behind, float alpha)
    {
        const float d = juce::jmax (7.0f, circle.getWidth() * 0.30f);
        juce::Rectangle<float> dot (circle.getRight() - d, circle.getBottom() - d, d, d);
        g.setColour (behind);
        g.fillEllipse (dot.expanded (2.0f));
        g.setColour (colour.withAlpha (alpha));
        g.fillEllipse (dot);
    }

    static juce::String awayFor (juce::int64 afkSinceMs)
    {
        if (afkSinceMs <= 0)
            return "<1m";
        auto mins = (int) (std::max<juce::int64> (0, juce::Time::currentTimeMillis() - afkSinceMs) / 60000);
        if (mins < 1)
            return "<1m";
        if (mins < 60)
            return juce::String (mins) + "m";
        return juce::String (mins / 60) + "h" + juce::String (mins % 60).paddedLeft ('0', 2) + "m";
    }

    static juce::String relTime (juce::int64 lastSeenMs)
    {
        if (lastSeenMs <= 0)
            return "Never online";

        auto seconds = std::max<juce::int64> (0, juce::Time::currentTimeMillis() - lastSeenMs) / 1000;
        if (seconds < 60)
            return "Last seen just now";

        auto minutes = seconds / 60;
        if (minutes < 60)
            return "Last seen " + juce::String (minutes) + " min ago";

        auto hours = minutes / 60;
        if (hours < 24)
            return "Last seen " + juce::String (hours) + (hours == 1 ? " hour ago" : " hours ago");

        auto days = hours / 24;
        if (days == 1)
            return "Last seen yesterday";
        if (days < 7)
            return "Last seen " + juce::String (days) + " days ago";
        return "Last seen " + juce::Time (lastSeenMs).formatted ("%b %d");
    }

    struct SmallButton : juce::Button
    {
        enum class Tone { neutral, accent, danger };

        SmallButton() : juce::Button ("Friend action") {}

        void setTone (Tone nextTone)
        {
            if (tone == nextTone)
                return;
            tone = nextTone;
            repaint();
        }

        void paintButton (juce::Graphics& g, bool over, bool down) override
        {
            auto bounds = getLocalBounds().toFloat().reduced (0.5f);

            if (! isEnabled())
            {
                g.setColour (Colors::textDim);
                g.setFont (Typography::semi (11.5f));
                g.drawText (getButtonText(), getLocalBounds(), juce::Justification::centred, true);
                return;
            }

            if (tone == Tone::accent)
            {
                auto base = Colors::accent;
                g.setColour (down ? base.darker (0.10f) : over ? base.brighter (0.12f) : base);
                g.fillRoundedRectangle (bounds, Metrics::rM);
                g.setColour (Colors::windowBg);
            }
            else
            {
                auto base = Colors::raised;
                g.setColour (down ? base.darker (0.10f) : over ? base.brighter (0.12f) : base);
                g.fillRoundedRectangle (bounds, Metrics::rM);
                g.setColour (tone == Tone::danger ? Colors::danger : Colors::text);
            }

            g.setFont (Typography::semi (11.5f));
            g.drawText (getButtonText(), getLocalBounds(), juce::Justification::centred, true);
        }

        Tone tone = Tone::neutral;
    };

    struct MoreButton : juce::Button
    {
        MoreButton() : juce::Button ("More options")
        {
            setTooltip ("More options");
        }

        void paintButton (juce::Graphics& g, bool over, bool down) override
        {
            auto bounds = getLocalBounds().toFloat().reduced (1.0f);
            if (over || down || hasKeyboardFocus (true))
            {
                g.setColour (juce::Colours::white.withAlpha (down ? 0.08f : 0.04f));
                g.fillRoundedRectangle (bounds, Metrics::rS);
            }

            g.setColour (over ? Colors::text : Colors::textDim);
            auto centre = bounds.getCentre();
            constexpr float dot = 2.5f;
            for (int i = -1; i <= 1; ++i)
                g.fillEllipse (centre.x + (float) i * 5.0f - dot * 0.5f,
                               centre.y - dot * 0.5f, dot, dot);
        }
    };

    struct ProfileHeader : juce::Component
    {
        ProfileHeader()
        {
            setOpaque (true);

            hostBtn.setButtonText ("Host");
            hostBtn.setTooltip ("Start a session your friends can join");
            hostBtn.onClick = [this] { if (onHost) onHost(); };
            addAndMakeVisible (hostBtn);

            joinBtn.setButtonText ("Join");
            joinBtn.setTooltip ("Join with an invite code or a nearby session");
            joinBtn.onClick = [this] { if (onJoin) onJoin(); };
            addAndMakeVisible (joinBtn);

            leaveBtn.setTooltip ("Disconnect from the current session");
            leaveBtn.onClick = [this] { if (onLeave) onLeave(); };
            addChildComponent (leaveBtn);
        }

        std::function<void()> onHost, onJoin, onLeave;
        std::function<void (bool away)> onSetAway;

        void setLocalAway (bool away)
        {
            if (away == localAway)
                return;
            localAway = away;
            repaint();
        }

        void setSessionState (bool active, bool hosting)
        {
            leaveBtn.setButtonText (hosting ? "End" : "Leave");
            if (active == activeSession && hosting == hostingSession)
                return;
            activeSession = active;
            hostingSession = hosting;
            hostBtn.setVisible (! active);
            joinBtn.setVisible (! active);
            leaveBtn.setVisible (active);
            resized();
            repaint();
        }

        void setProfile (const juce::Image& img, const juce::String& n, const juce::String& s)
        {
            if (n == name && s == status && img == avatar)
                return;
            avatar = img;
            name = n;
            status = s;
            repaint();
        }

        void applyColours()
        {
            auto style = [] (juce::TextButton& b, juce::Colour bg, juce::Colour fg)
            {
                b.setColour (juce::TextButton::buttonColourId, bg);
                b.setColour (juce::TextButton::textColourOffId, fg);
                b.setColour (juce::TextButton::textColourOnId, fg);
            };
            style (hostBtn, Colors::accent, Colors::windowBg);
            style (joinBtn, Colors::raised, Colors::text);
            style (leaveBtn, Colors::raised, Colors::danger);
        }

        void resized() override
        {
            auto inner = getLocalBounds().reduced (sideMargin - 6, 4).reduced (10, 0);
            avatarArea = inner.removeFromLeft (40).withSizeKeepingCentre (40, 40);
            inner.removeFromLeft (11);

            constexpr int bh = 26;
            if (activeSession)
                leaveBtn.setBounds (inner.removeFromRight (68).withSizeKeepingCentre (68, bh));
            else
            {
                joinBtn.setBounds (inner.removeFromRight (56).withSizeKeepingCentre (56, bh));
                inner.removeFromRight (6);
                hostBtn.setBounds (inner.removeFromRight (56).withSizeKeepingCentre (56, bh));
            }
            inner.removeFromRight (10);
            textArea = inner;
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Colors::panelBg);

            auto card = getLocalBounds().reduced (sideMargin - 6, 4);
            g.setColour (Colors::card);
            g.fillRoundedRectangle (card.toFloat(), Metrics::rM);

            auto shown = name.isNotEmpty() ? name : juce::String ("You");
            auto circle = avatarArea.toFloat();
            paintAvatar (g, circle, avatar, (juce::int64) shown.hashCode(), shown, 1.0f);
            paintStatusDot (g, circle, statusTint(), Colors::card, 1.0f);

            auto block = textArea.withSizeKeepingCentre (juce::jmax (0, textArea.getWidth()), 35);
            auto nameArea = block.removeFromTop (19);
            g.setColour (Colors::text);
            g.setFont (Typography::semi (14.5f));
            g.drawText (shown, nameArea, juce::Justification::centredLeft, true);

            g.setColour (statusTint());
            g.setFont (Typography::ui (12.0f));
            g.drawText (status, block, juce::Justification::centredLeft, true);
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (! dotHitArea().contains (e.getPosition()))
                return;

            juce::PopupMenu menu;
            menu.addItem (1, "Online", true, ! localAway);
            menu.addItem (2, "Away", true, localAway);

            auto safe = juce::Component::SafePointer<ProfileHeader> (this);
            menu.showMenuAsync (juce::PopupMenu::Options()
                                    .withTargetScreenArea (localAreaToGlobal (dotHitArea())),
                [safe] (int result)
                {
                    if (safe == nullptr || result == 0)
                        return;
                    if (safe->onSetAway)
                        safe->onSetAway (result == 2);
                });
        }

        void mouseMove (const juce::MouseEvent& e) override
        {
            setMouseCursor (dotHitArea().contains (e.getPosition())
                                ? juce::MouseCursor::PointingHandCursor
                                : juce::MouseCursor::NormalCursor);
        }

        juce::Colour statusTint() const
        {
            return localAway ? Colors::warning
                 : activeSession ? Colors::accent
                                 : Colors::success;
        }

        juce::Rectangle<int> dotHitArea() const
        {
            auto circle = avatarArea.toFloat();
            const float d = juce::jmax (7.0f, circle.getWidth() * 0.30f);
            juce::Rectangle<float> dot (circle.getRight() - d, circle.getBottom() - d, d, d);
            return dot.expanded (3.0f).getSmallestIntegerContainer();
        }

        juce::Image avatar;
        juce::String name, status;
        juce::Rectangle<int> avatarArea, textArea;
        bool activeSession = false, hostingSession = false, localAway = false;
        juce::TextButton hostBtn, joinBtn, leaveBtn;
    };

    struct Caption : juce::Component
    {
        Caption() { setInterceptsMouseClicks (false, false); }

        void setText (const juce::String& t)
        {
            if (t == text)
                return;
            text = t;
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().reduced (sideMargin, 0);
            auto font = Typography::caps (10.5f);
            g.setColour (Colors::textDim);
            g.setFont (font);
            g.drawText (text, r, juce::Justification::centredLeft, true);

            auto rule = r.toFloat().withTrimmedLeft ((float) juce::GlyphArrangement::getStringWidthInt (font, text) + 10.0f);
            if (rule.getWidth() > 4.0f)
            {
                g.setColour (Colors::outline.withAlpha (0.4f));
                g.fillRect (rule.getX(), rule.getCentreY(), rule.getWidth(), 1.0f);
            }
        }

        juce::String text;
    };

    struct Row : juce::Component
    {
        enum class Action { none, add, join, invite, accept, pending };

        explicit Row (FriendsPanel& o) : owner (o)
        {
            setMouseClickGrabsKeyboardFocus (false);

            actionBtn.onClick = [this]
            {
                if (action == Action::add && owner.onAddFriend) owner.onAddFriend (id, name);
                if (action == Action::join && owner.onJoinFriend) owner.onJoinFriend (id);
                if (action == Action::invite && owner.onInvite) owner.onInvite (id);
                if (action == Action::accept && owner.onAcceptRequest) owner.onAcceptRequest (id, name);
            };
            addChildComponent (actionBtn);

            secondaryBtn.onClick = [this] { if (owner.onDeclineRequest) owner.onDeclineRequest (id); };
            addChildComponent (secondaryBtn);

            moreBtn.onClick = [this] { showMenu (false); };
            addChildComponent (moreBtn);
        }

        void configure (juce::int64 friendId, const juce::String& friendName,
                        bool isNearby, bool isOnline, bool isDimmed, bool isAway,
                        const juce::String& presenceLine, juce::Colour lineTint,
                        int hostPort, int peopleCount, Action nextAction, bool pending)
        {
            id = friendId;
            name = friendName;
            nearby = isNearby;
            online = isOnline;
            dimmed = isDimmed;
            away = isAway;
            line = presenceLine;
            lineColour = lineTint;
            sessionPort = hostPort;
            people = peopleCount;
            request = false;
            action = nextAction;

            secondaryBtn.setVisible (false);
            actionBtn.setEnabled (true);

            if (action == Action::add)
            {
                if (pending)
                {
                    actionBtn.setButtonText ("Pending");
                    actionBtn.setTooltip ("Friend request sent to " + name);
                    actionBtn.setEnabled (false);
                    action = Action::pending;
                }
                else
                {
                    actionBtn.setButtonText ("Add");
                    actionBtn.setTooltip ("Send " + name + " a friend request");
                }
            }
            if (action == Action::join)
            {
                actionBtn.setButtonText ("Join");
                actionBtn.setTooltip ("Join " + name + "'s session");
            }
            if (action == Action::invite)
            {
                actionBtn.setButtonText ("Invite");
                actionBtn.setTooltip ("Invite " + name + " to your session");
            }

            actionBtn.setTone (action == Action::add || action == Action::join
                                   ? SmallButton::Tone::accent
                                   : SmallButton::Tone::neutral);
            updateControls();
            repaint();
        }

        void configureRequest (juce::int64 requestId, const juce::String& requestName, bool incoming)
        {
            id = requestId;
            name = requestName;
            nearby = false;
            online = false;
            dimmed = false;
            away = false;
            request = true;
            sessionPort = 0;
            people = 1;
            line = incoming ? "Wants to be your friend" : "Request sent";
            lineColour = Colors::textDim;

            if (incoming)
            {
                action = Action::accept;
                actionBtn.setButtonText ("Accept");
                actionBtn.setTooltip ("Accept " + name + "'s friend request");
                actionBtn.setTone (SmallButton::Tone::accent);
                actionBtn.setEnabled (true);

                secondaryBtn.setButtonText ("Decline");
                secondaryBtn.setTooltip ("Decline " + name + "'s friend request");
                secondaryBtn.setTone (SmallButton::Tone::neutral);
            }
            else
            {
                action = Action::pending;
                actionBtn.setButtonText ("Pending");
                actionBtn.setTooltip ("Waiting for " + name + " to accept");
                actionBtn.setTone (SmallButton::Tone::neutral);
                actionBtn.setEnabled (false);
            }

            updateControls();
            repaint();
        }

        bool inviteEligible() const
        {
            return online && owner.hostingSession && sessionPort == 0 && people <= 1;
        }

        void updateControls()
        {
            const bool showAction = request
                ? action != Action::none
                : (hovered && action != Action::none);
            const bool showSecondary = request && action == Action::accept;
            const bool showMore = ! nearby && ! request && hovered;

            if (actionBtn.isVisible() == showAction
                && secondaryBtn.isVisible() == showSecondary
                && moreBtn.isVisible() == showMore)
                return;

            actionBtn.setVisible (showAction);
            secondaryBtn.setVisible (showSecondary);
            moreBtn.setVisible (showMore);
            resized();
        }

        void resized() override
        {
            auto controls = getLocalBounds().reduced (sideMargin, 0)
                                            .withSizeKeepingCentre (getWidth() - sideMargin * 2, 26);
            if (moreBtn.isVisible())
            {
                moreBtn.setBounds (controls.removeFromRight (24));
                controls.removeFromRight (4);
            }

            if (secondaryBtn.isVisible())
            {
                secondaryBtn.setBounds (controls.removeFromRight (62));
                controls.removeFromRight (5);
            }

            if (actionBtn.isVisible())
                actionBtn.setBounds (controls.removeFromRight (60));
        }

        void paint (juce::Graphics& g) override
        {
            if (hovered)
            {
                g.setColour (juce::Colours::white.withAlpha (0.05f));
                g.fillRoundedRectangle (getLocalBounds().toFloat().reduced ((float) sideMargin - 6.0f, 2.0f),
                                        Metrics::rM);
            }

            const float alpha = dimmed ? 0.7f : 1.0f;
            auto r = getLocalBounds().reduced (sideMargin, 0);
            auto circle = r.removeFromLeft (avatarD).withSizeKeepingCentre (avatarD, avatarD).toFloat();

            paintAvatar (g, circle, owner.friends.avatarFor (id), id, name, alpha);
            paintStatusDot (g, circle,
                            ! online ? Colors::textDim.withAlpha (0.85f)
                            : away   ? Colors::warning
                            : (sessionPort > 0 || people > 1 ? Colors::accent : Colors::success),
                            Colors::panelBg, alpha);

            r.removeFromLeft (textGap);

            int textRight = r.getRight();
            for (auto* b : { (juce::Component*) &actionBtn, (juce::Component*) &secondaryBtn,
                             (juce::Component*) &moreBtn })
                if (b->isVisible())
                    textRight = juce::jmin (textRight, b->getX() - 8);
            r.setRight (juce::jmax (r.getX(), textRight));

            auto block = r.withSizeKeepingCentre (juce::jmax (0, r.getWidth()), 33);
            auto nameArea = block.removeFromTop (18);
            g.setColour ((dimmed ? Colors::text.withAlpha (0.75f) : Colors::text));
            g.setFont (Typography::semi (14.0f));
            g.drawText (name, nameArea, juce::Justification::centredLeft, true);

            g.setColour (lineColour.withAlpha (alpha));
            g.setFont (Typography::ui (12.0f));
            g.drawText (line, block, juce::Justification::centredLeft, true);
        }

        void mouseEnter (const juce::MouseEvent&) override
        {
            if (hovered)
                return;
            hovered = true;
            updateControls();
            repaint();
        }

        void mouseExit (const juce::MouseEvent&) override
        {
            if (isMouseOver (true))
                return;
            if (! hovered)
                return;
            hovered = false;
            updateControls();
            repaint();
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (e.mods.isPopupMenu() && ! nearby && ! request)
                showMenu (true);
        }

        void showMenu (bool atMouse)
        {
            juce::PopupMenu menu;
            bool hasSessionAction = false;
            if (online && sessionPort > 0 && ! owner.activeSession)
            {
                menu.addItem (1, "Join session");
                hasSessionAction = true;
            }
            if (inviteEligible())
            {
                menu.addItem (2, "Invite to my session");
                hasSessionAction = true;
            }
            if (hasSessionAction)
                menu.addSeparator();
            menu.addItem (3, "Remove friend");

            auto options = atMouse
                ? juce::PopupMenu::Options().withTargetScreenArea (
                      juce::Rectangle<int>().withPosition (juce::Desktop::getMousePosition()).withSize (1, 1))
                : juce::PopupMenu::Options().withTargetComponent (&moreBtn);

            auto safe = juce::Component::SafePointer<Row> (this);
            menu.showMenuAsync (options,
                [safe] (int result)
                {
                    if (safe == nullptr)
                        return;
                    if (result == 1 && safe->owner.onJoinFriend) safe->owner.onJoinFriend (safe->id);
                    if (result == 2 && safe->owner.onInvite) safe->owner.onInvite (safe->id);
                    if (result == 3 && safe->owner.onRemoveFriend) safe->owner.onRemoveFriend (safe->id);
                });
        }

        FriendsPanel& owner;
        juce::int64 id = 0;
        juce::String name, line;
        juce::Colour lineColour = Colors::textDim;
        int sessionPort = 0;
        int people = 1;
        bool nearby = false;
        bool online = false;
        bool dimmed = false;
        bool away = false;
        bool hovered = false;
        bool request = false;
        Action action = Action::none;
        SmallButton actionBtn;
        SmallButton secondaryBtn;
        MoreButton moreBtn;
    };

    struct Content : juce::Component
    {
        struct EmptyState
        {
            int y = 0;
            int height = 0;
            juce::String title, detail;
            bool nearby = false;
        };

        Content() { setOpaque (true); }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Colors::panelBg);

            for (auto& empty : emptyStates)
            {
                auto card = juce::Rectangle<int> (sideMargin - 6, empty.y,
                                                  getWidth() - (sideMargin - 6) * 2, empty.height);
                g.setColour (Colors::card);
                g.fillRoundedRectangle (card.toFloat(), Metrics::rM);

                auto iconTile = card.removeFromLeft (52).withSizeKeepingCentre (30, 30).toFloat();
                g.setColour (Colors::raised);
                g.fillRoundedRectangle (iconTile, Metrics::rS);
                if (empty.nearby)
                {
                    auto centre = iconTile.getCentre();
                    g.setColour (Colors::textDim.withAlpha (0.64f));
                    g.drawEllipse (iconTile.reduced (7.0f), 1.2f);
                    g.drawEllipse (iconTile.reduced (10.5f), 1.2f);
                    g.setColour (Colors::accent);
                    g.fillEllipse (centre.x - 2.0f, centre.y - 2.0f, 4.0f, 4.0f);
                }
                else
                {
                    drawPeopleIcon (g, iconTile.reduced (6.0f), Colors::textDim);
                }

                card.removeFromRight (Metrics::pad);
                auto block = card.withSizeKeepingCentre (juce::jmax (0, card.getWidth()), 33);
                auto title = block.removeFromTop (18);
                g.setColour (Colors::text);
                g.setFont (Typography::semi (13.0f));
                g.drawText (empty.title, title, juce::Justification::centredLeft, true);
                g.setColour (Colors::textDim);
                g.setFont (Typography::ui (11.5f));
                g.drawText (empty.detail, block, juce::Justification::centredLeft, true);
            }
        }

        static void drawPeopleIcon (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour)
        {
            auto x = area.getX();
            auto y = area.getY();
            auto w = area.getWidth();
            auto h = area.getHeight();

            g.setColour (colour);
            g.fillEllipse (x + w * 0.17f, y + h * 0.08f, w * 0.29f, h * 0.29f);
            g.fillEllipse (x + w * 0.61f, y + h * 0.18f, w * 0.23f, h * 0.23f);

            juce::Path people;
            people.startNewSubPath (x + w * 0.04f, y + h * 0.88f);
            people.cubicTo (x + w * 0.05f, y + h * 0.52f,
                            x + w * 0.60f, y + h * 0.52f,
                            x + w * 0.61f, y + h * 0.88f);
            people.startNewSubPath (x + w * 0.54f, y + h * 0.84f);
            people.cubicTo (x + w * 0.57f, y + h * 0.59f,
                            x + w * 0.94f, y + h * 0.59f,
                            x + w * 0.96f, y + h * 0.84f);
            g.strokePath (people, juce::PathStrokeType (1.7f,
                                                        juce::PathStrokeType::curved,
                                                        juce::PathStrokeType::rounded));
        }

        std::vector<EmptyState> emptyStates;
    };

    void applyControlColours()
    {
        profile.applyColours();

        search.setColour (juce::TextEditor::backgroundColourId, Colors::panelDark);
        search.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        search.setColour (juce::TextEditor::focusedOutlineColourId, Colors::accent);
        search.setColour (juce::TextEditor::textColourId, Colors::text);
        search.setColour (juce::TextEditor::highlightColourId, Colors::accent.withAlpha (0.3f));
        search.setColour (juce::TextEditor::highlightedTextColourId, Colors::windowBg);
        search.setColour (juce::CaretComponent::caretColourId, Colors::accent);
    }

    void rebuild()
    {
        activeSession = isSessionActive && isSessionActive();
        hostingSession = isHosting && isHosting();

        applyControlColours();
        profile.setSessionState (activeSession, hostingSession);
        profile.setLocalAway (isLocalAway && isLocalAway());

        auto localName = (getLocalName ? getLocalName() : juce::String());
        auto localStatus = (getLocalStatus ? getLocalStatus() : juce::String());
        if (localStatus.isEmpty())
            localStatus = hostingSession ? "Hosting a session"
                        : activeSession  ? "In a session"
                                         : "Online";
        profile.setProfile (getLocalAvatar ? getLocalAvatar() : juce::Image(), localName, localStatus);

        rebuildRows (false);
    }

    Row* nextRow (int& index)
    {
        while (rows.size() <= index)
        {
            auto* row = new Row (*this);
            content.addAndMakeVisible (row);
            rows.add (row);
        }
        auto* row = rows[index++];
        row->setVisible (true);
        return row;
    }

    Caption* nextCaption (int& index)
    {
        while (captions.size() <= index)
        {
            auto* c = new Caption();
            content.addAndMakeVisible (c);
            captions.add (c);
        }
        auto* c = captions[index++];
        c->setVisible (true);
        return c;
    }

    bool matchesSearch (const juce::String& name) const
    {
        return searchText.isEmpty() || name.toLowerCase().contains (searchText);
    }

    void rebuildRows (bool force)
    {
        auto friendsList = friends.getFriends();
        auto nearbyAll = friends.getNearbyNonFriends();
        auto incoming = friends.getIncomingRequests();
        auto outgoing = friends.getOutgoingRequests();

        std::set<juce::int64> nearbyIds;
        for (auto& p : nearbyAll)
            nearbyIds.insert (p.id);

        std::vector<FriendsManager::Request> outgoingRows;
        for (auto& r : outgoing)
            if (nearbyIds.find (r.id) == nearbyIds.end())
                outgoingRows.push_back (r);

        std::vector<FriendsManager::Presence> nearbyRows;
        for (auto& p : nearbyAll)
            if (matchesSearch (p.name))
                nearbyRows.push_back (p);
        std::stable_sort (nearbyRows.begin(), nearbyRows.end(),
            [] (const FriendsManager::Presence& a, const FriendsManager::Presence& b)
            {
                return a.name.compareIgnoreCase (b.name) < 0;
            });

        auto inSession = [] (const FriendsManager::Entry& e)
        {
            return e.sessionPort > 0 || e.people > 1;
        };
        auto rank = [&] (const FriendsManager::Entry& e)
        {
            if (! e.online)
                return 3;
            if (e.afk)
                return 2;
            return inSession (e) ? 0 : 1;
        };

        std::vector<FriendsManager::Entry> friendRows;
        for (auto& e : friendsList)
            if (matchesSearch (e.info.name))
                friendRows.push_back (e);
        std::stable_sort (friendRows.begin(), friendRows.end(),
            [&] (const FriendsManager::Entry& a, const FriendsManager::Entry& b)
            {
                auto ra = rank (a), rb = rank (b);
                if (ra != rb) return ra < rb;
                return a.info.name.compareIgnoreCase (b.info.name) < 0;
            });

        auto w = juce::jmax (10, viewport.getWidth() - viewport.getScrollBarThickness());

        juce::String signature;
        signature << w << '|' << (int) activeSession << (int) hostingSession << '|' << searchText << '|';
        for (auto& r : incoming)     signature << "i" << r.id << r.name << '|';
        for (auto& r : outgoingRows) signature << "o" << r.id << r.name << '|';
        for (auto& p : nearbyRows)   signature << "n" << p.id << p.name << p.status << p.sessionPort << p.people
                                               << (int) p.afk << (p.afk ? awayFor (p.afkSinceMs) : juce::String())
                                               << (int) friends.hasOutgoingRequest (p.id) << '|';
        for (auto& e : friendRows)   signature << "f" << e.info.id << e.info.name << (int) e.online << e.status
                                               << e.sessionPort << e.people << (int) e.afk
                                               << (e.online && e.afk ? awayFor (e.afkSinceMs) : juce::String())
                                               << (e.online ? juce::String() : relTime (e.info.lastSeenMs)) << '|';

        if (! force && signature == lastSignature)
            return;
        lastSignature = signature;

        content.emptyStates.clear();

        int y = 8;
        int rowIndex = 0;
        int captionIndex = 0;

        auto placeCaption = [&] (const juce::String& title, int count)
        {
            auto* c = nextCaption (captionIndex);
            c->setText (title.toUpperCase() + (count > 0 ? "  " + juce::String (count) : juce::String()));
            c->setBounds (0, y, w, captionH);
            y += captionH;
        };

        if (! incoming.empty() || ! outgoingRows.empty())
        {
            placeCaption ("Requests", (int) (incoming.size() + outgoingRows.size()));
            for (auto& r : incoming)
            {
                auto* row = nextRow (rowIndex);
                row->configureRequest (r.id, r.name.isNotEmpty() ? r.name : juce::String ("Someone"), true);
                row->setBounds (0, y, w, rowH);
                y += rowH;
            }
            for (auto& r : outgoingRows)
            {
                auto* row = nextRow (rowIndex);
                row->configureRequest (r.id, r.name.isNotEmpty() ? r.name : juce::String ("Someone"), false);
                row->setBounds (0, y, w, rowH);
                y += rowH;
            }
            y += 8;
        }

        if (! nearbyRows.empty())
        {
            placeCaption ("Nearby", (int) nearbyRows.size());
            for (auto& p : nearbyRows)
            {
                auto* row = nextRow (rowIndex);
                row->configure (p.id, p.name.isNotEmpty() ? p.name : juce::String ("Someone"),
                                true, true, false, p.afk,
                                p.afk ? "Away - " + awayFor (p.afkSinceMs)
                                      : p.status.isNotEmpty() ? p.status : juce::String ("Nearby"),
                                Colors::textDim, p.sessionPort, p.people,
                                Row::Action::add, friends.hasOutgoingRequest (p.id));
                row->setBounds (0, y, w, rowH);
                y += rowH;
            }
            y += 8;
        }

        placeCaption ("Friends", (int) friendRows.size());

        if (friendRows.empty())
        {
            content.emptyStates.push_back ({ y, 56,
                                             friendsList.empty() ? "No friends yet" : "No matches",
                                             friendsList.empty() ? "Add people from a session or your network."
                                                                 : "Nobody here matches that search.",
                                             false });
            y += 60;
        }

        for (auto& e : friendRows)
        {
            juce::String lineText;
            juce::Colour tint;
            auto action = Row::Action::none;

            if (e.online)
            {
                if (e.afk)
                {
                    lineText = "Away - " + awayFor (e.afkSinceMs);
                    tint = Colors::warning;
                }
                else if (inSession (e))
                {
                    lineText = e.status.isNotEmpty() ? e.status
                             : (e.sessionPort > 0 ? juce::String ("Hosting a session")
                                                  : juce::String ("In a session"));
                    tint = Colors::accent;
                }
                else
                {
                    lineText = e.status.isNotEmpty() ? e.status : juce::String ("Online");
                    tint = Colors::success;
                }

                if (e.sessionPort > 0 && ! activeSession)
                    action = Row::Action::join;
                else if (hostingSession && e.sessionPort == 0 && e.people <= 1)
                    action = Row::Action::invite;
            }
            else
            {
                lineText = relTime (e.info.lastSeenMs);
                tint = Colors::textDim;
            }

            auto* row = nextRow (rowIndex);
            row->configure (e.info.id, e.info.name, false, e.online, ! e.online, e.afk,
                            lineText, tint, e.sessionPort, e.people, action, false);
            row->setBounds (0, y, w, rowH);
            y += rowH;
        }

        for (int i = rowIndex; i < rows.size(); ++i)
            rows[i]->setVisible (false);
        for (int i = captionIndex; i < captions.size(); ++i)
            captions[i]->setVisible (false);

        content.setSize (w, juce::jmax (viewport.getHeight(), y + 10));
        content.repaint();
    }

    void timerCallback() override { if (isShowing()) rebuild(); }

    void visibilityChanged() override
    {
        if (isVisible())
            rebuild();
    }

    FriendsManager& friends;
    ProfileHeader profile;
    juce::Viewport viewport;
    Content content;
    juce::OwnedArray<Row> rows;
    juce::OwnedArray<Caption> captions;
    juce::TextEditor search;
    juce::String searchText, lastSignature;
    bool activeSession = false;
    bool hostingSession = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FriendsPanel)
};
