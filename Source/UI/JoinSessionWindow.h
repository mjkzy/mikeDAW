#pragma once
#include <JuceHeader.h>
#include "AppLookAndFeel.h"
#include "Dialogs.h"
#include "../Net/LanDiscovery.h"
#include "../Net/Session.h"

class JoinSessionContent : public juce::Component,
                           private juce::Timer,
                           private juce::ListBoxModel
{
public:
    JoinSessionContent (LanDiscovery& d, std::function<void (juce::String, int)> onJoin,
                        std::function<void()> onClose)
        : discovery (d), joinCb (std::move (onJoin)), closeCb (std::move (onClose))
    {
        title.setText ("Join a Session", juce::dontSendNotification);
        title.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
        addAndMakeVisible (title);

        status.setFont (juce::Font (juce::FontOptions (11.5f)));
        status.setColour (juce::Label::textColourId, Colors::textDim);
        addAndMakeVisible (status);

        list.setModel (this);
        list.setRowHeight (30);
        list.setColour (juce::ListBox::backgroundColourId, Colors::panelDark);
        list.setColour (juce::ListBox::outlineColourId, Colors::outline);
        list.setOutlineThickness (1);
        addAndMakeVisible (list);

        addrLabel.setText ("Code/IP:", juce::dontSendNotification);
        addrLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
        addAndMakeVisible (addrLabel);

        addrBox.setTextToShowWhenEmpty ("paste invite code, ip, or ip:port", Colors::textDim);
        addrBox.onReturnKey = [this] { joinTyped(); };
        addAndMakeVisible (addrBox);

        refreshBtn.setButtonText ("Refresh");
        refreshBtn.onClick = [this] { rescan(); };
        addAndMakeVisible (refreshBtn);

        joinBtn.setButtonText ("Join");
        joinBtn.setColour (juce::TextButton::buttonColourId, Colors::accent.withAlpha (0.85f));
        joinBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::black);
        joinBtn.onClick = [this] { joinSelectedOrTyped(); };
        addAndMakeVisible (joinBtn);

        cancelBtn.setButtonText ("Cancel");
        cancelBtn.onClick = [this] { if (closeCb) closeCb(); };
        addAndMakeVisible (cancelBtn);

        discovery.setQuerying (true);
        refresh();
        startTimerHz (2);
        setSize (460, 380);
    }

    ~JoinSessionContent() override
    {
        discovery.setQuerying (false);
    }

    void paint (juce::Graphics& g) override { g.fillAll (Colors::panelBg); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        title.setBounds (r.removeFromTop (24));
        status.setBounds (r.removeFromTop (18));
        r.removeFromTop (4);

        auto bottom = r.removeFromBottom (30);
        cancelBtn.setBounds (bottom.removeFromRight (80));
        bottom.removeFromRight (6);
        joinBtn.setBounds (bottom.removeFromRight (80));
        bottom.removeFromLeft (0);
        refreshBtn.setBounds (bottom.removeFromLeft (80));
        r.removeFromBottom (8);

        auto addrRow = r.removeFromBottom (26);
        addrLabel.setBounds (addrRow.removeFromLeft (70));
        addrBox.setBounds (addrRow);
        r.removeFromBottom (8);

        list.setBounds (r);
    }

private:
    void timerCallback() override { refresh(); }

    void rescan()
    {
        discovery.setQuerying (false);
        discovery.setQuerying (true);
        refresh();
    }

    void refresh()
    {
        auto live = discovery.getSessions();

        bool changed = live.size() != entries.size();
        for (int i = 0; ! changed && i < live.size(); ++i)
            changed = live[i].address != entries[i].address
                   || live[i].port != entries[i].port
                   || live[i].people != entries[i].people
                   || live[i].name != entries[i].name;

        if (changed)
        {
            auto keep = getSelectedHost();
            entries = live;
            list.updateContent();
            for (int i = 0; i < entries.size(); ++i)
                if (entries[i].address == keep.first && entries[i].port == keep.second)
                    { list.selectRow (i); break; }
        }

        status.setText (entries.isEmpty()
                            ? "Searching LAN, or paste an internet invite code below..."
                            : juce::String (entries.size()) + " session"
                              + (entries.size() == 1 ? "" : "s") + " found",
                        juce::dontSendNotification);
    }

    std::pair<juce::String, int> getSelectedHost() const
    {
        int row = list.getSelectedRow();
        if (juce::isPositiveAndBelow (row, entries.size()))
            return { entries[row].address, entries[row].port };
        return { {}, 0 };
    }

    void joinSelectedOrTyped()
    {
        if (! addrBox.getText().trim().isEmpty())
        {
            joinTyped();
            return;
        }
        auto host = getSelectedHost();
        if (host.second > 0 && joinCb)
        {
            joinCb (host.first, host.second);
            if (closeCb) closeCb();
        }
    }

    void joinTyped()
    {
        auto typed = addrBox.getText().trim();
        if (typed.isEmpty())
            return;
        juce::String addr = typed;
        int port = Session::defaultPort;
        if (addr.containsChar (':'))
        {
            port = addr.fromLastOccurrenceOf (":", false, false).getIntValue();
            addr = addr.upToLastOccurrenceOf (":", false, false);
            if (port <= 0) port = Session::defaultPort;
        }
        if (addr.isNotEmpty() && joinCb)
        {
            joinCb (addr, port);
            if (closeCb) closeCb();
        }
    }

    int getNumRows() override { return entries.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (! juce::isPositiveAndBelow (row, entries.size()))
            return;
        const auto e = entries[row];

        if (selected)
            g.fillAll (Colors::accent.withAlpha (0.30f));
        else if (row % 2)
            g.fillAll (Colors::panelBg.withAlpha (0.35f));

        auto b = juce::Rectangle<int> (0, 0, w, h).reduced (8, 0);

        auto badge = b.removeFromRight (54);
        bool full = e.people >= Session::maxPeople;
        g.setColour (full ? Colors::danger.withAlpha (0.85f)
                          : Colors::success.withAlpha (0.85f));
        auto pill = badge.withSizeKeepingCentre (46, 18).toFloat();
        g.fillRoundedRectangle (pill, 9.0f);
        g.setColour (juce::Colours::black.withAlpha (0.85f));
        g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        g.drawText (juce::String (e.people) + "/" + juce::String (Session::maxPeople),
                    pill.toNearestInt(), juce::Justification::centred);

        g.setColour (Colors::text);
        g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
        auto nameLine = b.removeFromTop (h / 2 + 2);
        g.drawText (e.name.isEmpty() ? "Session" : e.name, nameLine,
                    juce::Justification::bottomLeft);

        g.setColour (Colors::textDim);
        g.setFont (juce::Font (juce::FontOptions (11.0f)));
        g.drawText (e.address + ":" + juce::String (e.port), b,
                    juce::Justification::topLeft);
    }

    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override
    {
        if (juce::isPositiveAndBelow (row, entries.size()) && joinCb)
        {
            joinCb (entries[row].address, entries[row].port);
            if (closeCb) closeCb();
        }
    }

    void returnKeyPressed (int) override { joinSelectedOrTyped(); }

    LanDiscovery& discovery;
    std::function<void (juce::String, int)> joinCb;
    std::function<void()> closeCb;

    juce::Label title, status, addrLabel;
    juce::ListBox list;
    juce::TextEditor addrBox;
    juce::TextButton refreshBtn, joinBtn, cancelBtn;
    juce::Array<LanDiscovery::Entry> entries;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (JoinSessionContent)
};

class JoinSessionWindow : public juce::DocumentWindow
{
public:
    JoinSessionWindow (LanDiscovery& d, std::function<void (juce::String, int)> onJoin,
                       std::function<void()> onClosed)
        : juce::DocumentWindow ("Join Session", Colors::panelBg,
                                juce::DocumentWindow::closeButton),
          closedCb (std::move (onClosed))
    {
        setUsingNativeTitleBar (true);
        content = new JoinSessionContent (d, std::move (onJoin), [this] { closeSelf(); });
        Dialogs::setContentFitted (*this, content, 460, 380);
        setResizable (false, false);
    }

    void closeButtonPressed() override { closeSelf(); }

    JoinSessionContent* content = nullptr;

private:
    void closeSelf()
    {
        auto cb = closedCb;
        juce::MessageManager::callAsync ([cb] { if (cb) cb(); });
    }

    std::function<void()> closedCb;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (JoinSessionWindow)
};
