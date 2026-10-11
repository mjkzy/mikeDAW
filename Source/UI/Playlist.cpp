#include "Playlist.h"
#include "SmoothValue.h"
#include "BrowserPanel.h"
#include "AppLookAndFeel.h"
#include "Dialogs.h"
#include "FloatingPanel.h"
#include "../Import/MidiImport.h"
#include "../Model/Automation.h"
#include "../Debug/DebugFlags.h"

static juce::PopupMenu::Options playlistMenuOptions (juce::Component& c)
{
    return juce::PopupMenu::Options().withParentComponent (c.getTopLevelComponent());
}

class Playlist::CanvasViewport : public juce::Viewport
{
public:
    explicit CanvasViewport (Playlist& o) : owner (o) {}
    void visibleAreaChanged (const juce::Rectangle<int>&) override { owner.syncScroll(); }
    Playlist& owner;
};

class PatternColourSelector : public juce::ColourSelector,
                              private juce::ChangeListener
{
public:
    PatternColourSelector (juce::Array<juce::ValueTree> targetNodes)
        : juce::ColourSelector (juce::ColourSelector::showColourAtTop
                                | juce::ColourSelector::showColourspace
                                | juce::ColourSelector::editableColour),
          nodes (std::move (targetNodes))
    {
        addChangeListener (this);
    }

    ~PatternColourSelector() override { removeChangeListener (this); }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        auto c = getCurrentColour().toString();
        for (auto& node : nodes)
            if (node.isValid())
                node.setProperty (IDs::colour, c, nullptr);
    }

    juce::Array<juce::ValueTree> nodes;
};

static juce::Colour randomPlaylistColour()
{
    auto& rnd = juce::Random::getSystemRandom();
    return juce::Colour::fromHSV ((float) rnd.nextDouble(),
                                  0.55f + 0.20f * (float) rnd.nextDouble(),
                                  0.75f + 0.15f * (float) rnd.nextDouble(),
                                  1.0f);
}

static juce::Colour playlistLaneColour (const juce::ValueTree& track)
{
    if (! track.isValid())
        return {};
    auto stored = track.getProperty (IDs::colour).toString();
    return stored.isNotEmpty() ? juce::Colour::fromString (stored) : juce::Colour();
}

class Playlist::TrackHeaders : public juce::Component
{
public:
    explicit TrackHeaders (Playlist& o) : owner (o) {}

    void updateSize()
    {
        setSize (Playlist::headerWidth,
                 (owner.numTracks() + Playlist::extraLanes) * owner.trackHeight);
    }

    juce::Rectangle<int> ledArea (int t) const
    {
        int th = owner.trackHeight;
        return { getWidth() - 22, t * th + (th - 14) / 2, 14, 14 };
    }

    void paint (juce::Graphics& g) override
    {
        int n = owner.numTracks();
        int th = owner.trackHeight;
        g.fillAll (Colors::panelDark);
        g.setFont (Typography::semi (juce::jmin (15.0f, (float) th - 8.0f)));

        for (int t = 0; t < n + Playlist::extraLanes; ++t)
        {
            juce::Rectangle<int> row (0, t * th, getWidth(), th);

            if (t >= n)
            {
                g.setColour (Colors::panelDark);
                g.fillRect (row);
                continue;
            }

            g.setColour (t % 2 == 0 ? Colors::header : Colors::header.darker (0.12f));
            g.fillRect (row);

            auto track = Model::getPlaylistTrack (owner.project, t);
            auto tint = playlistLaneColour (track);
            if (! tint.isTransparent())
            {
                g.setColour (tint.withAlpha (t == hoverTrack ? 0.34f : 0.24f));
                g.fillRect (row);
                g.setColour (tint);
                g.fillRect (row.getX(), row.getY(), 3, row.getHeight());
            }
            else if (t == hoverTrack)
            {
                g.setColour (juce::Colours::white.withAlpha (0.04f));
                g.fillRect (row);
            }

            g.setColour (Colors::outline);
            g.drawHorizontalLine (row.getBottom() - 1, 0.0f, (float) getWidth());

            bool enabled = ! track.isValid() || (bool) track.getProperty (IDs::enabled, true);
            auto name = track.isValid() ? track.getProperty (IDs::name).toString()
                                        : "Track " + juce::String (t + 1);

            g.setColour (enabled ? Colors::text : Colors::textDim.withAlpha (0.7f));
            g.drawText (name, row.reduced (8, 0).withTrimmedRight (20),
                        juce::Justification::centredLeft);

            auto led = ledArea (t).toFloat();
            if (enabled)
            {
                g.setColour (Colors::stepOn.withAlpha (0.35f));
                g.fillEllipse (led.expanded (2.0f));
                g.setColour (Colors::stepOn);
            }
            else
            {
                g.setColour (Colors::stepOff);
            }
            g.fillEllipse (led);
            g.setColour (Colors::outline);
            g.drawEllipse (led, 1.0f);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown())
            return;
        int t = e.getPosition().getY() / juce::jmax (1, owner.trackHeight);
        if (! juce::isPositiveAndBelow (t, owner.numTracks()))
            return;

        if (e.mods.isPopupMenu())
        {
            if (ledArea (t).expanded (4).contains (e.getPosition()))
                toggleSolo (t);
            else
                showTrackMenu (t);
            return;
        }

        if (! ledArea (t).expanded (4).contains (e.getPosition()))
            return;

        auto track = Model::getPlaylistTrack (owner.project, t);
        if (! track.isValid())
        {
            Model::ensurePlaylistTracks (owner.project);
            track = Model::getPlaylistTrack (owner.project, t);
        }
        if (track.isValid())
        {
            track.setProperty (IDs::enabled,
                               ! (bool) track.getProperty (IDs::enabled, true), nullptr);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        int t = e.getPosition().getY() / juce::jmax (1, owner.trackHeight);
        if (! juce::isPositiveAndBelow (t, owner.numTracks()))
            t = -1;
        if (t != hoverTrack)
        {
            hoverTrack = t;
            repaint();
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (hoverTrack >= 0)
        {
            hoverTrack = -1;
            repaint();
        }
    }

    void showTrackMenu (int trackIndex)
    {
        Model::ensurePlaylistTracks (owner.project);
        auto track = Model::getPlaylistTrack (owner.project, trackIndex);
        if (! track.isValid())
            return;

        juce::PopupMenu m;
        m.addItem (1, "Solo track", true, soloedTrack == trackIndex);
        m.addSeparator();
        m.addItem (2, "Track colour...");
        m.addItem (3, "Random colour");
        m.addItem (4, "Reset colour", track.hasProperty (IDs::colour));

        m.showMenuAsync (playlistMenuOptions (*this),
            [safe = juce::Component::SafePointer<TrackHeaders> (this), trackIndex] (int r)
            {
                if (safe == nullptr || r == 0)
                    return;
                if (r == 1)
                {
                    safe->toggleSolo (trackIndex);
                    return;
                }

                auto node = Model::getPlaylistTrack (safe->owner.project, trackIndex);
                if (! node.isValid())
                    return;

                if (r == 2)      safe->showColourPickerForTrack (trackIndex);
                else if (r == 3) node.setProperty (IDs::colour, randomPlaylistColour().toString(), nullptr);
                else if (r == 4) node.removeProperty (IDs::colour, nullptr);
            });
    }

    void showColourPickerForTrack (int trackIndex)
    {
        auto track = Model::getPlaylistTrack (owner.project, trackIndex);
        if (! track.isValid())
            return;

        auto tint = playlistLaneColour (track);
        auto* selector = new PatternColourSelector (juce::Array<juce::ValueTree> { track });
        selector->setCurrentColour (tint.isTransparent() ? Colors::header : tint,
                                    juce::dontSendNotification);
        selector->setSize (280, 300);
        selector->setColour (juce::ColourSelector::backgroundColourId, Colors::panelBg);

        auto anchor = localAreaToGlobal (juce::Rectangle<int> (0, trackIndex * owner.trackHeight,
                                                               getWidth(), owner.trackHeight));
        auto& box = juce::CallOutBox::launchAsynchronously (
            std::unique_ptr<juce::Component> (selector), anchor, nullptr);
        box.setLookAndFeel (&getLookAndFeel());
    }

    void toggleSolo (int trackIndex)
    {
        int n = owner.numTracks();
        Model::ensurePlaylistTracks (owner.project);

        if (soloedTrack == trackIndex)
        {
            for (int i = 0; i < n; ++i)
            {
                auto t = Model::getPlaylistTrack (owner.project, i);
                if (t.isValid())
                    t.setProperty (IDs::enabled, true, nullptr);
            }
            soloedTrack = -1;
        }
        else
        {
            for (int i = 0; i < n; ++i)
            {
                auto t = Model::getPlaylistTrack (owner.project, i);
                if (t.isValid())
                    t.setProperty (IDs::enabled, i == trackIndex, nullptr);
            }
            soloedTrack = trackIndex;
        }

        if (owner.onSoloToggled)
        {
            auto t = Model::getPlaylistTrack (owner.project, trackIndex);
            auto name = t.isValid() ? t.getProperty (IDs::name).toString() : juce::String();
            juce::String label = "Playlist track " + juce::String (trackIndex + 1);
            if (name.isNotEmpty() && name != "Track " + juce::String (trackIndex + 1))
                label += " \"" + name + "\"";
            owner.onSoloToggled (label, soloedTrack == trackIndex);
        }
    }

    Playlist& owner;
    int soloedTrack = -1;
    int hoverTrack = -1;
};

class Playlist::PatternPicker : public juce::Component
{
public:
    explicit PatternPicker (Playlist& o) : owner (o)
    {
        setWantsKeyboardFocus (true);
    }

    static constexpr int rowHeight = 36;

    enum class EntryKind { pattern, audio, midi };
    struct Entry
    {
        EntryKind kind = EntryKind::pattern;
        int id = 0;
        juce::String path;
        juce::String name;
        juce::String sub;
        juce::Colour colour;
    };

    void updateSize()
    {
        auto e = entries();
        auto& vp = owner.patternPickerViewport;
        setSize (juce::jmax (60, vp.getWidth() - vp.getScrollBarThickness()),
                 juce::jmax ((int) e.size() * rowHeight, vp.getHeight()));
    }

    std::vector<Entry> entries() const
    {
        std::vector<Entry> out;

        if (owner.resourceView == ResourceView::patterns)
        {
            auto patterns = Model::getPatterns (owner.project);
            for (int i = 0; i < patterns.getNumChildren(); ++i)
            {
                auto pat = patterns.getChild (i);
                int pid = (int) pat.getProperty (IDs::id);
                out.push_back ({ EntryKind::pattern, pid, {},
                                 pat.getProperty (IDs::name).toString(), {},
                                 Model::patternColour (pat) });
            }
        }
        else if (owner.resourceView == ResourceView::audio)
        {
            struct Info { juce::String name; juce::Colour colour; int id = 0; };
            std::map<juce::String, Info> paths;

            auto audioFiles = Model::getAudioFiles (owner.project);
            for (int i = 0; i < audioFiles.getNumChildren(); ++i)
            {
                auto item = audioFiles.getChild (i);
                if (! item.hasType (IDs::AudioFile))
                    continue;
                auto path = item.getProperty (IDs::audioPath).toString();
                if (path.isEmpty())
                    continue;
                auto& info = paths[path];
                info.name = item.getProperty (IDs::name).toString();
                if (info.name.isEmpty())
                    info.name = juce::File (path).getFileNameWithoutExtension();
                info.id = (int) item.getProperty (IDs::id);
                auto stored = item.getProperty (IDs::colour).toString();
                if (stored.isNotEmpty())
                    info.colour = juce::Colour::fromString (stored);
            }

            auto addPath = [&] (const juce::String& path)
            {
                if (path.isEmpty() || path.startsWithIgnoreCase ("builtin:"))
                    return;
                juce::File f (path);
                if (! BrowserPanel::isAudioFile (f))
                    return;
                auto& info = paths[path];
                if (info.name.isEmpty())
                    info.name = f.getFileNameWithoutExtension();
            };

            auto playlist = Model::getPlaylist (owner.project);
            for (int i = 0; i < playlist.getNumChildren(); ++i)
            {
                auto clip = playlist.getChild (i);
                if (clip.hasType (IDs::Clip) && clip.getProperty (IDs::type).toString() == "audio")
                    addPath (clip.getProperty (IDs::path).toString());
            }

            for (auto& kv : paths)
            {
                auto colour = kv.second.colour;
                if (colour.isTransparent())
                    colour = Model::channelColour ((int) (kv.first.hashCode() & 0x3fffffff));
                out.push_back ({ EntryKind::audio, kv.second.id, kv.first, kv.second.name, {}, colour });
            }
        }
        else
        {
            auto files = Model::getMidiFiles (owner.project);
            for (int i = 0; i < files.getNumChildren(); ++i)
            {
                auto item = files.getChild (i);
                if (! item.hasType (IDs::MidiFile))
                    continue;
                auto path = item.getProperty (IDs::midiPath).toString();
                if (path.isEmpty())
                    continue;
                auto name = item.getProperty (IDs::name).toString();
                if (name.isEmpty())
                    name = juce::File (path).getFileNameWithoutExtension();
                out.push_back ({ EntryKind::midi, (int) item.getProperty (IDs::id, 0), path, name,
                                 {}, juce::Colour (0xff9b7bd7) });
            }
        }

        return out;
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colors::panelDark);

        pruneMultiSelection();
        auto list = entries();
        paintedRows = (int) list.size();
        if (list.empty())
        {
            g.setColour (Colors::textDim.withAlpha (0.7f));
            g.setFont (Typography::bold (12.0f));
            juce::String msg = owner.resourceView == ResourceView::patterns ? "No patterns"
                             : owner.resourceView == ResourceView::audio    ? "No project audio"
                                                                            : "No MIDI files";
            g.drawText (msg, getLocalBounds().reduced (8), juce::Justification::centredTop, true);
            return;
        }

        for (int i = 0; i < (int) list.size(); ++i)
        {
            auto& item = list[(size_t) i];
            bool active = item.kind == EntryKind::pattern
                            ? item.id == owner.selectedPatternId
                            : item.path == selectedResourcePath;
            bool multi = item.kind == EntryKind::pattern && multiSelectedPatternIds.contains (item.id);
            juce::Rectangle<int> row (0, i * rowHeight, getWidth(), rowHeight);
            auto box = row.reduced (4, 3);

            if (active)
            {
                g.setColour (Colors::header.brighter (0.08f));
                g.fillRect (box);
                g.setColour (Colors::accent);
                g.drawRect (box, 1);
            }
            else if (multi)
            {
                g.setColour (Colors::header.brighter (0.08f));
                g.fillRect (box);
                g.setColour (Colors::accent.withAlpha (0.6f));
                g.drawRect (box, 1);
            }
            else if (i == hoverRow)
            {
                g.setColour (Colors::header);
                g.fillRect (box);
            }

            auto swatch = juce::Rectangle<float> ((float) box.getX() + 7.0f,
                                                  (float) row.getCentreY() - 7.0f,
                                                  14.0f, 14.0f);
            g.setColour (item.colour.withAlpha (active ? 1.0f : 0.75f));
            g.fillRoundedRectangle (swatch, 2.0f);
            g.setColour (item.colour.brighter (0.4f));
            g.drawRoundedRectangle (swatch, 2.0f, 0.8f);

            g.setColour (active ? Colors::text : Colors::textDim);
            g.setFont (Typography::bold (item.sub.isEmpty() ? 14.0f : 12.5f));
            auto text = row.withTrimmedLeft (34).withTrimmedRight (8);
            if (item.sub.isEmpty())
                g.drawText (item.name, text, juce::Justification::centredLeft, true);
            else
            {
                g.drawText (item.name, text.withTrimmedBottom (13), juce::Justification::centredLeft, true);
                g.setColour (Colors::textDim.withAlpha (0.75f));
                g.setFont (Typography::ui (10.0f));
                g.drawText (item.sub, text.withTrimmedTop (18), juce::Justification::centredLeft, true);
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown())
            return;
        grabKeyboardFocus();
        pressedEntry = entryAt (e.getPosition());
        dragStarted = false;
        if (pressedEntry.name.isEmpty())
            return;

        if (pressedEntry.kind == EntryKind::pattern)
        {
            pressedPatternId = pressedEntry.id;
            pruneMultiSelection();

            if (e.mods.isPopupMenu())
            {
                if (! multiSelectedPatternIds.contains (pressedEntry.id))
                {
                    multiSelectedPatternIds.clearQuick();
                    multiSelectedPatternIds.add (pressedEntry.id);
                    lastClickedPatternId = pressedEntry.id;
                    selectPattern (pressedPatternId);
                }
            }
            else if (e.mods.isCommandDown() && e.mods.isShiftDown())
            {
                rangeSelectTo (pressedEntry.id);
                lastClickedPatternId = pressedEntry.id;
            }
            else if (e.mods.isCommandDown())
            {
                bool wasEmpty = multiSelectedPatternIds.isEmpty();
                if (multiSelectedPatternIds.contains (pressedEntry.id))
                    multiSelectedPatternIds.removeAllInstancesOf (pressedEntry.id);
                else
                    multiSelectedPatternIds.add (pressedEntry.id);
                lastClickedPatternId = pressedEntry.id;
                if (wasEmpty)
                    selectPattern (pressedPatternId);
                repaint();
            }
            else
            {
                multiSelectedPatternIds.clearQuick();
                multiSelectedPatternIds.add (pressedEntry.id);
                lastClickedPatternId = pressedEntry.id;
                selectPattern (pressedPatternId);
                repaint();
            }
        }
        else
        {
            pressedPatternId = 0;
            selectedResourcePath = pressedEntry.path;
            if (pressedEntry.kind == EntryKind::audio)
            {
                owner.audioBrushPath = pressedEntry.path;
                owner.audioBrushTemplate = {};
                owner.repaintCanvas();
            }
            repaint();
        }

        if (e.mods.isPopupMenu())
            showContextMenu (pressedEntry);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown())
            return;
        if (pressedEntry.name.isEmpty() || dragStarted || e.getDistanceFromDragStart() < 4)
            return;

        if (pressedEntry.kind != EntryKind::pattern && ! juce::File (pressedEntry.path).existsAsFile())
            return;

        dragStarted = true;
        if (auto* dnd = juce::DragAndDropContainer::findParentDragContainerFor (this))
        {
            juce::Image img (juce::Image::ARGB, 170, 30, true);
            juce::Graphics g (img);
            g.setColour (Colors::panelDark.withAlpha (0.92f));
            g.fillRoundedRectangle (1.0f, 1.0f, 168.0f, 28.0f, 3.0f);
            g.setColour (pressedEntry.colour.withAlpha (0.95f));
            g.fillRoundedRectangle (7.0f, 8.0f, 14.0f, 14.0f, 2.0f);
            g.setColour (Colors::text);
            g.setFont (Typography::bold (13.0f));
            g.drawText (pressedEntry.name, 28, 0, 134, 30, juce::Justification::centredLeft, true);

            juce::String desc;
            if (pressedEntry.kind == EntryKind::pattern) desc = "playlist-pattern:" + juce::String (pressedEntry.id);
            else if (pressedEntry.kind == EntryKind::audio) desc = "playlist-audio:" + pressedEntry.path;
            else desc = "playlist-midi:" + pressedEntry.path;
            dnd->startDragging (desc, this, img);
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown())
            return;
        pressedPatternId = 0;
        pressedEntry = {};
        dragStarted = false;
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        int r = e.getPosition().getY() / rowHeight;
        if (! juce::isPositiveAndBelow (r, paintedRows))
            r = -1;
        if (r != hoverRow)
        {
            hoverRow = r;
            repaint();
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (hoverRow >= 0)
        {
            hoverRow = -1;
            repaint();
        }
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key.isKeyCode (juce::KeyPress::deleteKey) || key.isKeyCode (juce::KeyPress::backspaceKey))
        {
            if (owner.resourceView == ResourceView::patterns)
            {
                pruneMultiSelection();
                if (multiSelectedPatternIds.size() > 1)
                {
                    auto ids = multiSelectedPatternIds;
                    for (int id : ids)
                    {
                        Entry e;
                        e.kind = EntryKind::pattern;
                        e.id = id;
                        deleteEntry (e);
                    }
                    return true;
                }
                if (owner.selectedPatternId > 0)
                {
                    Entry e;
                    e.kind = EntryKind::pattern;
                    e.id = owner.selectedPatternId;
                    deleteEntry (e);
                    return true;
                }
            }
            if (selectedResourcePath.isNotEmpty())
            {
                Entry e;
                e.kind = owner.resourceView == ResourceView::audio ? EntryKind::audio : EntryKind::midi;
                e.path = selectedResourcePath;
                deleteEntry (e);
                return true;
            }
        }
        return false;
    }

    Entry entryAt (juce::Point<int> p) const
    {
        auto list = entries();
        int idx = p.getY() / rowHeight;
        if (! juce::isPositiveAndBelow (idx, (int) list.size()))
            return {};
        return list[(size_t) idx];
    }

    void selectPattern (int pid)
    {
        owner.clearAudioBrush();
        owner.setSelectedPattern (pid);
        owner.engine.setCurrentPatternId (pid);
        if (owner.onPatternSelected)
            owner.onPatternSelected (pid);
    }

    void pruneMultiSelection()
    {
        for (int i = multiSelectedPatternIds.size(); --i >= 0;)
            if (! Model::findPatternById (owner.project, multiSelectedPatternIds[i]).isValid())
                multiSelectedPatternIds.remove (i);
    }

    void rangeSelectTo (int toId)
    {
        auto list = entries();
        int fromIdx = -1, toIdx = -1;
        for (int i = 0; i < (int) list.size(); ++i)
        {
            if (list[(size_t) i].kind != EntryKind::pattern)
                continue;
            if (list[(size_t) i].id == lastClickedPatternId) fromIdx = i;
            if (list[(size_t) i].id == toId)                 toIdx = i;
        }
        if (toIdx < 0)
            return;
        if (fromIdx < 0)
            fromIdx = toIdx;

        for (int i = juce::jmin (fromIdx, toIdx); i <= juce::jmax (fromIdx, toIdx); ++i)
            if (list[(size_t) i].kind == EntryKind::pattern
                && ! multiSelectedPatternIds.contains (list[(size_t) i].id))
                multiSelectedPatternIds.add (list[(size_t) i].id);
        repaint();
    }

    juce::Array<int> targetPatternIds (const Entry& entry)
    {
        pruneMultiSelection();
        juce::Array<int> ids;
        if (entry.kind == EntryKind::pattern
            && multiSelectedPatternIds.contains (entry.id)
            && multiSelectedPatternIds.size() > 1)
            ids = multiSelectedPatternIds;
        else
            ids.add (entry.id);
        return ids;
    }

    void showContextMenu (Entry entry)
    {
        juce::PopupMenu m;
        if (entry.kind == EntryKind::pattern)
        {
            m.addItem (3, "Rename...");
            m.addItem (4, "Random color");
            m.addItem (5, "Change color...");
            m.addSeparator();
            m.addItem (1, "Split Pattern");
            m.addItem (2, "Delete Pattern from Project");
        }
        else if (entry.kind == EntryKind::audio)
        {
            m.addItem (3, "Rename...");
            m.addItem (4, "Random color");
            m.addItem (5, "Change color...");
            m.addSeparator();
            m.addItem (2, "Delete Audio from Project");
        }
        else
        {
            m.addItem (3, "Rename...");
            m.addItem (4, "Random color");
            m.addItem (5, "Change color...");
            m.addSeparator();
            m.addItem (2, "Delete MIDI from Project");
        }

        m.showMenuAsync (playlistMenuOptions (*this).withMousePosition(),
            [this, entry] (int r)
            {
                if (r == 1 && entry.kind == EntryKind::pattern)
                    owner.splitPatternByChannel (entry.id);
                else if (r == 2)
                {
                    if (entry.kind == EntryKind::pattern)
                        for (int id : targetPatternIds (entry))
                        {
                            Entry pe; pe.kind = EntryKind::pattern; pe.id = id;
                            deleteEntry (pe);
                        }
                    else
                        deleteEntry (entry);
                }
                else if (r == 3)
                    showRenameDialogForEntry (entry);
                else if (r == 4)
                {
                    if (entry.kind == EntryKind::pattern)
                        for (int id : targetPatternIds (entry))
                        {
                            Entry pe; pe.kind = EntryKind::pattern; pe.id = id;
                            randomiseColourForEntry (pe);
                        }
                    else
                        randomiseColourForEntry (entry);
                }
                else if (r == 5)
                    showColourPickerForEntry (entry);
            });
    }

    juce::ValueTree findTreeNode (const Entry& entry)
    {
        if (entry.kind == EntryKind::pattern)
            return Model::findPatternById (owner.project, entry.id);
        if (entry.kind == EntryKind::audio)
        {
            auto files = Model::getAudioFiles (owner.project);
            return files.getChildWithProperty (IDs::audioPath, entry.path);
        }
        auto files = Model::getMidiFiles (owner.project);
        return files.getChildWithProperty (IDs::midiPath, entry.path);
    }

    void showRenameDialogForEntry (Entry entry)
    {
        auto node = findTreeNode (entry);
        if (! node.isValid())
            return;

        auto label = entry.kind == EntryKind::pattern ? "Rename Pattern"
                   : entry.kind == EntryKind::audio   ? "Rename Audio"
                                                      : "Rename MIDI";
        auto* w = new juce::AlertWindow (label,
                                         "Enter a new name:",
                                         juce::MessageBoxIconType::NoIcon);
        w->addTextEditor ("name", node.getProperty (IDs::name).toString());
        w->addButton ("Rename", 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

        w->enterModalState (true, juce::ModalCallbackFunction::create (
            [safe = juce::Component::SafePointer<PatternPicker> (this), w, entry] (int result)
            {
                if (safe == nullptr || result != 1)
                    return;
                auto txt = w->getTextEditorContents ("name").trim();
                if (txt.isEmpty())
                    return;
                auto n = safe->findTreeNode (entry);
                if (n.isValid())
                    n.setProperty (IDs::name, txt, nullptr);
            }), true);
    }

    void randomiseColourForEntry (Entry entry)
    {
        auto node = findTreeNode (entry);
        if (! node.isValid())
            return;

        node.setProperty (IDs::colour, randomPlaylistColour().toString(), nullptr);
    }

    void showColourPickerForEntry (Entry entry)
    {
        auto node = findTreeNode (entry);
        if (! node.isValid())
            return;

        juce::Colour current;
        if (entry.kind == EntryKind::pattern)
            current = Model::patternColour (node);
        else
        {
            auto stored = node.getProperty (IDs::colour).toString();
            current = stored.isNotEmpty() ? juce::Colour::fromString (stored) : entry.colour;
        }

        juce::Array<juce::ValueTree> nodes;
        if (entry.kind == EntryKind::pattern)
            for (int id : targetPatternIds (entry))
            {
                auto n = Model::findPatternById (owner.project, id);
                if (n.isValid())
                    nodes.add (n);
            }
        if (nodes.isEmpty())
            nodes.add (node);

        auto* selector = new PatternColourSelector (nodes);
        selector->setCurrentColour (current, juce::dontSendNotification);
        selector->setSize (280, 300);
        selector->setColour (juce::ColourSelector::backgroundColourId, Colors::panelBg);

        auto anchor = localAreaToGlobal (juce::Rectangle<int> (getMouseXYRelative(), getMouseXYRelative()));
        auto& box = juce::CallOutBox::launchAsynchronously (
            std::unique_ptr<juce::Component> (selector), anchor, nullptr);
        box.setLookAndFeel (&getLookAndFeel());
    }

    void deleteEntry (Entry entry)
    {
        if (entry.kind == EntryKind::pattern)
        {
            auto playlist = Model::getPlaylist (owner.project);
            for (int i = playlist.getNumChildren(); --i >= 0;)
            {
                auto clip = playlist.getChild (i);
                if (clip.hasType (IDs::Clip) && clip.getProperty (IDs::type).toString() == "pattern"
                    && (int) clip.getProperty (IDs::patternId) == entry.id)
                    playlist.removeChild (i, nullptr);
            }

            auto patterns = Model::getPatterns (owner.project);
            for (int i = patterns.getNumChildren(); --i >= 0;)
                if ((int) patterns.getChild (i).getProperty (IDs::id) == entry.id)
                    patterns.removeChild (i, nullptr);

            owner.refreshPatternBox();
            if (owner.selectedPatternId > 0)
            {
                owner.engine.setCurrentPatternId (owner.selectedPatternId);
                if (owner.onPatternSelected)
                    owner.onPatternSelected (owner.selectedPatternId);
            }
        }
        else if (entry.kind == EntryKind::audio)
        {
            auto playlist = Model::getPlaylist (owner.project);
            for (int i = playlist.getNumChildren(); --i >= 0;)
            {
                auto clip = playlist.getChild (i);
                if (clip.hasType (IDs::Clip) && clip.getProperty (IDs::type).toString() == "audio"
                    && clip.getProperty (IDs::path).toString() == entry.path)
                    playlist.removeChild (i, nullptr);
            }

            juce::Array<int> removedChannels;
            auto channels = Model::getChannels (owner.project);
            for (int i = channels.getNumChildren(); --i >= 0;)
            {
                auto ch = channels.getChild (i);
                if (ch.hasType (IDs::Channel) && ch.getProperty (IDs::samplePath).toString() == entry.path)
                {
                    removedChannels.add ((int) ch.getProperty (IDs::id));
                    channels.removeChild (i, nullptr);
                }
            }

            if (! removedChannels.isEmpty())
            {
                auto patterns = Model::getPatterns (owner.project);
                for (int p = 0; p < patterns.getNumChildren(); ++p)
                {
                    auto pat = patterns.getChild (p);
                    for (int i = pat.getNumChildren(); --i >= 0;)
                    {
                        auto seq = pat.getChild (i);
                        if (seq.hasType (IDs::ChannelSeq)
                            && removedChannels.contains ((int) seq.getProperty (IDs::channelId)))
                            pat.removeChild (i, nullptr);
                    }
                }
            }

            auto audioFiles = Model::getAudioFiles (owner.project);
            for (int i = audioFiles.getNumChildren(); --i >= 0;)
                if (audioFiles.getChild (i).getProperty (IDs::audioPath).toString() == entry.path)
                    audioFiles.removeChild (i, nullptr);

            selectedResourcePath.clear();
        }
        else
        {
            auto files = Model::getMidiFiles (owner.project);
            for (int i = files.getNumChildren(); --i >= 0;)
                if (files.getChild (i).getProperty (IDs::midiPath).toString() == entry.path)
                    files.removeChild (i, nullptr);
            selectedResourcePath.clear();
        }

        owner.refreshResourcePanel();
    }

    Playlist& owner;
    int pressedPatternId = 0;
    Entry pressedEntry;
    juce::String selectedResourcePath;
    bool dragStarted = false;
    juce::Array<int> multiSelectedPatternIds;
    int lastClickedPatternId = 0;
    int hoverRow = -1;
    int paintedRows = 0;
};

class Playlist::Canvas : public juce::Component
{
public:
    explicit Canvas (Playlist& o) : owner (o)
    {
        sparkles.reserve (24);
        noteGlows.reserve (128);
        hZoom.onStep = [this] (double v) { applyHZoom (v); };
        vZoom.onStep = [this] (double v) { applyVZoom ((int) std::lround (v)); };
    }

    void stopZoomAnim() { hZoom.stop(); vZoom.stop(); }

    void applyHZoom (double newPx)
    {
        owner.setHZoom (newPx, hAnchorBeat, hAnchorViewX);
    }

    void applyVZoom (int newTh)
    {
        newTh = juce::jlimit (18, 80, newTh);
        if (newTh == owner.trackHeight)
            return;
        double lane = (owner.viewport->getViewPositionY() + vAnchorViewportY - rulerHeight)
                        / (double) owner.trackHeight;
        owner.trackHeight = newTh;
        updateSize();
        owner.headers->updateSize();
        owner.viewport->setViewPosition (owner.viewport->getViewPositionX(),
            juce::jmax (0, rulerHeight + (int) std::lround (lane * newTh) - vAnchorViewportY));
        owner.syncScroll();
        owner.headers->repaint();
        repaint();
    }

    double contentBeats() const
    {
        double maxEnd = 64.0;
        auto playlist = Model::getPlaylist (owner.project);
        for (int i = 0; i < playlist.getNumChildren(); ++i)
        {
            auto clip = playlist.getChild (i);
            if (! clip.hasType (IDs::Clip))
                continue;
            maxEnd = juce::jmax (maxEnd, (double) clip.getProperty (IDs::startBeats)
                                         + (double) clip.getProperty (IDs::lengthBeats));
        }
        return ((int) std::ceil (maxEnd / Model::beatsPerBar) + 16) * Model::beatsPerBar;
    }

    void refreshExtent()
    {
        double content = contentBeats();
        if (content >= stickyEndBeats || owner.viewport == nullptr)
        {
            stickyEndBeats = content;
            return;
        }
        double viewRightBeat = (owner.viewport->getViewPositionX()
                                + owner.viewport->getMaximumVisibleWidth()) / owner.pxPerBeat;
        if (viewRightBeat <= content)
            stickyEndBeats = content;
    }

    int totalBeats() const
    {
        return (int) juce::jmax (contentBeats(), stickyEndBeats);
    }

    void updateSize()
    {
        refreshExtent();
        int w = (int) (totalBeats() * owner.pxPerBeat);
        int h = rulerHeight + (owner.numTracks() + Playlist::extraLanes) * owner.trackHeight;
        if (owner.viewport != nullptr)
        {
            w = juce::jmax (w, owner.viewport->getMaximumVisibleWidth());
            h = juce::jmax (h, owner.viewport->getMaximumVisibleHeight());
        }
        setSize (w, h);
    }

    juce::Rectangle<int> clipRect (const juce::ValueTree& clip) const
    {
        double start = clip.getProperty (IDs::startBeats);
        double len = clip.getProperty (IDs::lengthBeats);
        int track = clip.getProperty (IDs::track);
        return { (int) (start * owner.pxPerBeat),
                 rulerHeight + track * owner.trackHeight,
                 juce::jmax (4, (int) (len * owner.pxPerBeat)),
                 owner.trackHeight };
    }

    juce::Colour clipColour (const juce::ValueTree& clip) const
    {
        auto type = clip.getProperty (IDs::type).toString();
        auto stored = type == "audio"
            ? Model::getAudioFiles (owner.project)
                  .getChildWithProperty (IDs::audioPath, clip.getProperty (IDs::path).toString())
                  .getProperty (IDs::colour).toString()
            : clip.getProperty (IDs::colour).toString();

        if (type == "audio" || type == "automation")
        {
            auto path = clip.getProperty (IDs::path).toString();
            auto fallback = type == "audio"
                ? Model::channelColour ((int) (path.hashCode() & 0x3fffffff))
                : Colors::automationClip;
            if (stored.isEmpty())
                return fallback;
            auto c = juce::Colour::fromString (stored);
            return c.isTransparent() ? fallback : c;
        }
        return Model::patternColour (Model::findPatternById (owner.project,
                                                             (int) clip.getProperty (IDs::patternId)));
    }

    static juce::Rectangle<float> clipBodyRect (juce::Rectangle<int> cr)
    {
        return cr.toFloat().reduced (0.5f, 1.5f);
    }

    static float clipBarHeight (juce::Rectangle<float> r)
    {
        if (r.getHeight() < compactClipHeight)
            return 0.0f;
        return juce::jmin ((float) clipHeaderHeight, r.getHeight() * 0.45f);
    }

    juce::Rectangle<int> clipMenuZone (const juce::ValueTree& clip) const
    {
        auto cr = clipRect (clip);
        int barH = (int) std::round (clipBarHeight (clipBodyRect (cr)));
        if (barH <= 0)
            return {};
        return { cr.getX(), cr.getY(),
                 juce::jmin (22, juce::jmax (1, cr.getWidth() / 3)), barH + 2 };
    }

    bool isHeldClip (const juce::ValueTree& clip) const
    {
        if (! clipsHeld)
            return false;
        for (auto& info : dragInfos)
            if (info.clip == clip)
                return true;
        return false;
    }

    struct ClipRect { juce::ValueTree clip; juce::Rectangle<int> rect; };
    std::vector<ClipRect> clipRectCache;
    int clipCacheGen = -1, clipCacheTrackHeight = 0, clipCacheCount = -1;
    double clipCachePxPerBeat = 0.0;

    struct PreviewEv { double beat, len; int key; bool slide; };
    struct PatternPreview { std::vector<PreviewEv> evs; int minKey = 127, maxKey = 0; };
    std::map<int, PatternPreview> previewCache;
    int previewCacheGen = -1;

    struct PreviewGeom
    {
        juce::Rectangle<float> inner;
        float noteH = 0.0f;
        int minKey = 0, maxKey = 0;
        bool valid = false;
    };

    struct NoteGlow
    {
        int clipUid;
        int evIndex;
        float glow;
        bool active;
        juce::Rectangle<int> bounds;
    };
    std::vector<NoteGlow> noteGlows;
    int noteGlowGen = -1;
    juce::uint32 lastNoteGlowMs = 0;
    static constexpr size_t maxNoteGlows = 256;

    const std::vector<ClipRect>& cachedClipRects()
    {
        auto playlist = Model::getPlaylist (owner.project);
        const int n = playlist.getNumChildren();
        if (clipCacheGen == owner.treeGen && clipCacheCount == n
            && clipCachePxPerBeat == owner.pxPerBeat && clipCacheTrackHeight == owner.trackHeight)
            return clipRectCache;

        clipRectCache.clear();
        clipRectCache.reserve ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            auto clip = playlist.getChild (i);
            if (clip.hasType (IDs::Clip))
                clipRectCache.push_back ({ clip, clipRect (clip) });
        }
        clipCacheGen = owner.treeGen;
        clipCacheCount = n;
        clipCachePxPerBeat = owner.pxPerBeat;
        clipCacheTrackHeight = owner.trackHeight;
        return clipRectCache;
    }

    static bool isAutomationClip (const juce::ValueTree& clip)
    {
        return clip.isValid() && clip.getProperty (IDs::type).toString() == "automation";
    }

    juce::Rectangle<float> autoBodyRect (const juce::ValueTree& clip) const
    {
        auto r = clipBodyRect (clipRect (clip));
        return r.withTrimmedTop (clipBarHeight (r));
    }

    juce::Point<float> pointPos (const juce::ValueTree& clip, const juce::ValueTree& pt) const
    {
        auto body = autoBodyRect (clip);
        double clipBeats = juce::jmax (1.0e-9, (double) clip.getProperty (IDs::lengthBeats));
        double offset = clip.getProperty (IDs::offsetBeats, 0.0);
        double rel = (double) pt.getProperty (IDs::pos) - offset;
        float x = body.getX() + (float) (rel / clipBeats) * body.getWidth();
        float y = body.getBottom() - (float) juce::jlimit (0.0, 1.0, (double) pt.getProperty (IDs::val)) * body.getHeight();
        return { x, y };
    }

    juce::Array<juce::ValueTree> autoPoints (const juce::ValueTree& clip) const
    {
        juce::Array<juce::ValueTree> pts;
        for (int i = 0; i < clip.getNumChildren(); ++i)
            if (clip.getChild (i).hasType (IDs::Point))
                pts.add (clip.getChild (i));
        std::sort (pts.begin(), pts.end(), [] (const juce::ValueTree& a, const juce::ValueTree& b)
                   { return (double) a.getProperty (IDs::pos) < (double) b.getProperty (IDs::pos); });
        return pts;
    }

    juce::ValueTree pointAt (const juce::ValueTree& clip, juce::Point<int> pos) const
    {
        for (auto& pt : autoPoints (clip))
            if (pointPos (clip, pt).getDistanceFrom (pos.toFloat()) <= 8.0f)
                return pt;
        return {};
    }

    void drawAutomationCurve (juce::Graphics& g, const juce::ValueTree& clip,
                              juce::Rectangle<float> body, juce::Colour colour)
    {
        if (body.getWidth() < 2.0f || body.getHeight() < 2.0f)
            return;

        double clipBeats = juce::jmax (1.0e-9, (double) clip.getProperty (IDs::lengthBeats));
        double offset = clip.getProperty (IDs::offsetBeats, 0.0);
        auto valToY = [&] (double v) { return body.getBottom() - (float) juce::jlimit (0.0, 1.0, v) * body.getHeight(); };

        const int cols = juce::jmax (2, (int) (body.getWidth() / 3.0f));
        const double colW = body.getWidth() / (double) cols;
        auto visible = body.getSmallestIntegerContainer().getIntersection (g.getClipBounds());
        if (! visible.isEmpty())
        {
            const int i0 = juce::jlimit (0, cols, (int) std::floor ((visible.getX() - body.getX()) / colW) - 1);
            const int i1 = juce::jlimit (i0, cols, (int) std::ceil ((visible.getRight() - body.getX()) / colW) + 1);

            juce::Path curve, fill;
            for (int i = i0; i <= i1; ++i)
            {
                double frac = (double) i / cols;
                double v = Automation::evaluate (clip, offset + frac * clipBeats);
                float x = body.getX() + (float) frac * body.getWidth();
                float y = valToY (v);
                if (i == i0) { curve.startNewSubPath (x, y); fill.startNewSubPath (x, body.getBottom()); fill.lineTo (x, y); }
                else         { curve.lineTo (x, y); fill.lineTo (x, y); }
            }
            fill.lineTo (body.getX() + (float) ((double) i1 / cols) * body.getWidth(), body.getBottom());
            fill.closeSubPath();

            g.setColour (colour.withAlpha (0.22f));
            g.fillPath (fill);
            g.setColour (colour.brighter (0.3f));
            g.strokePath (curve, juce::PathStrokeType (2.0f));
        }

        if (body.getHeight() < 5.0f)
            return;

        const float rad = juce::jlimit (1.5f, 3.5f, body.getHeight() * 0.35f);
        const auto dot = selection.contains (clip) ? Colors::accent : colour.brighter (0.5f);
        for (auto& pt : autoPoints (clip))
        {
            double rel = (double) pt.getProperty (IDs::pos) - offset;
            if (rel < -1.0e-6 || rel > clipBeats + 1.0e-6)
                continue;
            auto p = pointPos (clip, pt);
            g.setColour (dot);
            g.fillEllipse (p.x - rad, p.y - rad, rad * 2.0f, rad * 2.0f);
        }
    }

    bool handleAutomationMouseDown (juce::ValueTree clip, const juce::MouseEvent& e)
    {
        auto pt = pointAt (clip, e.getPosition());

        if (e.mods.isPopupMenu())
        {
            if (pt.isValid())
            {
                showAutoPointMenu (clip, pt);
                return true;
            }
            if (selection.contains (clip))
            {
                insertAutoPoint (clip, e.getPosition(), true);
                return true;
            }
            return false;
        }

        if (e.mods.isCtrlDown() && ! pt.isValid())
        {
            auto pts = autoPoints (clip);
            double px = (double) e.getPosition().getX();
            for (int i = 1; i < pts.size(); ++i)
            {
                if (px >= pointPos (clip, pts[i - 1]).x && px <= pointPos (clip, pts[i]).x)
                {
                    autoClip = clip;
                    autoPointNode = pts[i];
                    autoTensionStart = (double) pts[i].getProperty (IDs::tension, 0.0);
                    autoDragStartY = e.getPosition().getY();
                    mode = Mode::autoTension;
                    return true;
                }
            }
            return false;
        }

        if (e.getNumberOfClicks() >= 2 && ! pt.isValid())
        {
            insertAutoPoint (clip, e.getPosition());
            return true;
        }

        if (pt.isValid())
        {
            auto pts = autoPoints (clip);
            autoClip = clip;
            autoPointNode = pt;
            autoPointLocked = (pt == pts.getFirst());
            mode = Mode::autoPoint;
            selectOnly (clip);
            return true;
        }

        return false;
    }

    void insertAutoPoint (juce::ValueTree clip, juce::Point<int> pos, bool valueFromY = false)
    {
        auto body = autoBodyRect (clip);
        if (body.getWidth() < 1.0f)
            return;
        double clipBeats = juce::jmax (1.0e-9, (double) clip.getProperty (IDs::lengthBeats));
        double offset = clip.getProperty (IDs::offsetBeats, 0.0);
        double frac = juce::jlimit (0.0, 1.0, (pos.getX() - body.getX()) / (double) body.getWidth());
        double posInClip = offset + frac * clipBeats;
        double v = valueFromY
            ? juce::jlimit (0.0, 1.0, (body.getBottom() - pos.getY()) / (double) juce::jmax (1.0f, body.getHeight()))
            : Automation::evaluate (clip, posInClip);

        int cm = 0; double tension = 0.0;
        for (auto& p : autoPoints (clip))
            if ((double) p.getProperty (IDs::pos) > posInClip)
            {
                cm = (int) p.getProperty (IDs::curveMode, 0);
                tension = (double) p.getProperty (IDs::tension, 0.0);
                break;
            }

        juce::ValueTree pt (IDs::Point);
        pt.setProperty (IDs::pos, posInClip, nullptr);
        pt.setProperty (IDs::val, v, nullptr);
        pt.setProperty (IDs::curveMode, cm, nullptr);
        pt.setProperty (IDs::tension, tension, nullptr);
        clip.appendChild (pt, nullptr);
        repaint();
    }

    void showAutoPointMenu (juce::ValueTree clip, juce::ValueTree pt)
    {
        auto pts = autoPoints (clip);
        bool isEnd = (pt == pts.getFirst() || pt == pts.getLast());
        int cm = (int) pt.getProperty (IDs::curveMode, 0);

        auto clipTxt = juce::SystemClipboard::getTextFromClipboard().trim();
        bool pasteOk = clipTxt.containsOnly ("0123456789.eE+-")
                       && clipTxt.retainCharacters ("0123456789").isNotEmpty();

        juce::PopupMenu m;
        m.addItem (1, "Delete", ! isEnd);
        m.addSeparator();
        m.addItem (12, "Smooth", true, cm == 2);
        m.addItem (11, "Hold", true, cm == 1);
        m.addItem (10, "Single curve", true, cm == 0);
        m.addSeparator();
        m.addItem (2, "Type in value...");
        m.addItem (3, "Copy Value");
        m.addItem (4, "Paste Value", pasteOk);
        m.addItem (5, "Reset to Default");

        m.showMenuAsync (playlistMenuOptions (*this).withMousePosition(),
            [this, clip, pt] (int r) mutable
            {
                if (r == 1)      { clip.removeChild (pt, nullptr); repaint(); }
                else if (r == 10) pt.setProperty (IDs::curveMode, 0, nullptr);
                else if (r == 11) pt.setProperty (IDs::curveMode, 1, nullptr);
                else if (r == 12) pt.setProperty (IDs::curveMode, 2, nullptr);
                else if (r == 2)  showAutoValueDialog (pt);
                else if (r == 3)  juce::SystemClipboard::copyTextToClipboard (
                                      juce::String ((double) pt.getProperty (IDs::val), 4));
                else if (r == 4)
                {
                    auto txt = juce::SystemClipboard::getTextFromClipboard().trim();
                    pt.setProperty (IDs::val, juce::jlimit (0.0, 1.0, txt.getDoubleValue()), nullptr);
                }
                else if (r == 5)
                {
                    auto tgt = Automation::parseTarget (clip.getProperty (IDs::target).toString());
                    pt.setProperty (IDs::val, Automation::normalizedDefault (tgt), nullptr);
                }
            });
    }

    void showAutoValueDialog (juce::ValueTree pt)
    {
        auto* w = new juce::AlertWindow ("Set Value", "Enter a value between 0 and 1:",
                                         juce::MessageBoxIconType::NoIcon);
        w->addTextEditor ("value", juce::String ((double) pt.getProperty (IDs::val), 4));
        w->addButton ("Set", 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        w->enterModalState (true, juce::ModalCallbackFunction::create (
            [safe = juce::Component::SafePointer<Canvas> (this), w, pt] (int result) mutable
            {
                if (safe != nullptr && result == 1)
                {
                    auto txt = w->getTextEditorContents ("value").trim();
                    if (txt.isNotEmpty())
                        pt.setProperty (IDs::val, juce::jlimit (0.0, 1.0, txt.getDoubleValue()), nullptr);
                }
            }), true);
    }

    const PatternPreview& cachedPreview (const juce::ValueTree& pattern)
    {
        if (previewCacheGen != owner.treeGen)
        {
            previewCache.clear();
            previewCacheGen = owner.treeGen;
        }

        const int id = (int) pattern.getProperty (IDs::id, 0);
        auto found = previewCache.find (id);
        if (found != previewCache.end())
            return found->second;

        PatternPreview p;
        int chanRow = 0;
        for (int i = 0; i < pattern.getNumChildren(); ++i)
        {
            auto seq = pattern.getChild (i);
            if (! seq.hasType (IDs::ChannelSeq))
                continue;

            auto steps = seq.getProperty (IDs::steps).toString();
            for (int s = 0; s < steps.length(); ++s)
                if (steps[s] == '1')
                    p.evs.push_back ({ s / (double) Model::stepsPerBeat,
                                       1.0 / Model::stepsPerBeat, 60 - chanRow, false });

            for (int n = 0; n < seq.getNumChildren(); ++n)
            {
                auto note = seq.getChild (n);
                if (! note.hasType (IDs::Note))
                    continue;
                p.evs.push_back ({ (int) note.getProperty (IDs::start) / (double) Model::PPQ,
                                   juce::jmax (1, (int) note.getProperty (IDs::length)) / (double) Model::PPQ,
                                   (int) note.getProperty (IDs::key),
                                   (bool) note.getProperty (IDs::slide, false) });
            }
            ++chanRow;
        }

        for (auto& ev : p.evs)
        {
            p.minKey = juce::jmin (p.minKey, ev.key);
            p.maxKey = juce::jmax (p.maxKey, ev.key);
        }
        return previewCache.emplace (id, std::move (p)).first->second;
    }

    PreviewGeom previewGeom (const PatternPreview& preview, juce::Rectangle<float> r, float barH) const
    {
        PreviewGeom gm;
        const float pad = r.getHeight() < compactClipHeight ? 1.0f : 2.0f;
        gm.inner = r.reduced (pad).withTrimmedTop (barH);
        if (gm.inner.getHeight() < 3.0f)
            return gm;
        gm.minKey = preview.minKey;
        gm.maxKey = preview.maxKey;
        gm.noteH = juce::jlimit (gm.inner.getHeight() < 8.0f ? 1.0f : 1.5f, 4.0f,
                                 gm.inner.getHeight() / (float) (gm.maxKey - gm.minKey + 1));
        gm.valid = true;
        return gm;
    }

    bool previewNoteRect (const PreviewEv& ev, const PreviewGeom& gm, juce::Rectangle<float> r,
                          double clipLenBeats, double offsetBeats, juce::Rectangle<float>& out) const
    {
        double b = ev.beat - offsetBeats;
        double len = ev.len;
        if (b < 0.0)
        {
            len += b;
            b = 0.0;
            if (len <= 0.0)
                return false;
        }
        if (b >= clipLenBeats)
            return false;
        len = juce::jmin (len, clipLenBeats - b);
        float y = gm.maxKey == gm.minKey
            ? gm.inner.getCentreY() - gm.noteH * 0.5f
            : gm.inner.getY() + (float) (gm.maxKey - ev.key) / (float) (gm.maxKey - gm.minKey)
                                * (gm.inner.getHeight() - gm.noteH);
        out = { (float) (r.getX() + b * owner.pxPerBeat), y,
                juce::jmax (1.5f, (float) (len * owner.pxPerBeat) - 0.5f), gm.noteH };
        return true;
    }

    void drawPatternPreview (juce::Graphics& g, const juce::ValueTree& pattern,
                             juce::Rectangle<float> r, double clipLenBeats, juce::Colour colour,
                             double offsetBeats, float barH, int clipUid)
    {
        double patBeats = Model::patternLengthTicks (pattern) / (double) Model::PPQ;
        if (patBeats <= 0.0)
            return;

        const auto& preview = cachedPreview (pattern);
        const auto& evs = preview.evs;
        if (evs.empty())
            return;

        const auto gm = previewGeom (preview, r, barH);
        if (! gm.valid)
            return;

        g.setColour (colour.brighter (0.8f).withAlpha (0.9f));
        juce::Rectangle<float> nr;
        for (auto& ev : evs)
            if (previewNoteRect (ev, gm, r, clipLenBeats, offsetBeats, nr))
                g.fillRect (nr);

        if (noteGlows.empty() || ! Fx::glowEnabled)
            return;
        const float gi = Fx::playlistGlowIntensity;
        if (gi <= 0.001f)
            return;

        const float expand[3] = { 4.0f, 2.5f, 1.25f };
        const float haloAlpha[3] = { 0.05f, 0.09f, 0.16f };
        auto glowCol = colour.brighter (0.8f);
        auto coreCol = colour.brighter (1.0f);
        for (auto& e : noteGlows)
        {
            if (e.clipUid != clipUid || e.glow <= 0.0f
                || ! juce::isPositiveAndBelow (e.evIndex, (int) evs.size()))
                continue;
            if (! previewNoteRect (evs[(size_t) e.evIndex], gm, r, clipLenBeats, offsetBeats, nr))
                continue;
            const float rad = juce::jmin (Metrics::rS, nr.getHeight() * 0.5f);
            for (int p = 0; p < 3; ++p)
            {
                g.setColour (glowCol.withAlpha (juce::jmin (1.0f, haloAlpha[p] * gi * e.glow)));
                g.fillRoundedRectangle (nr.expanded (expand[p]), rad + expand[p] * 0.5f);
            }
            g.setColour (coreCol.withAlpha (juce::jmin (1.0f, 0.25f * gi * e.glow)));
            g.fillRoundedRectangle (nr, rad);
        }
    }

    bool hasNoteGlowActivity() const { return ! noteGlows.empty(); }

    NoteGlow* findNoteGlow (int clipUid, int evIndex)
    {
        for (auto& e : noteGlows)
            if (e.clipUid == clipUid && e.evIndex == evIndex)
                return &e;
        return nullptr;
    }

    void animateClipNoteGlow()
    {
        const juce::uint32 now = juce::Time::getMillisecondCounter();

        if (noteGlowGen != owner.treeGen)
        {
            noteGlows.clear();
            noteGlowGen = owner.treeGen;
        }

        if (! Fx::glowEnabled || Fx::playlistGlowIntensity <= 0.001f)
        {
            lastNoteGlowMs = now;
            if (noteGlows.empty())
                return;
            const auto visible = owner.viewport != nullptr ? owner.viewport->getViewArea() : getLocalBounds();
            juce::Rectangle<int> dirty;
            for (auto& e : noteGlows)
                dirty = dirty.getUnion (e.bounds.getIntersection (visible));
            noteGlows.clear();
            if (! dirty.isEmpty())
                repaint (dirty);
            return;
        }

        float dt = lastNoteGlowMs == 0 ? 1.0f / 60.0f : (now - lastNoteGlowMs) / 1000.0f;
        lastNoteGlowMs = now;
        dt = juce::jlimit (1.0f / 240.0f, 0.1f, dt);

        for (auto& e : noteGlows)
            e.active = false;

        if (owner.engine.isPlaying() && owner.engine.isSongMode() && mode != Mode::seek)
        {
            const double posBeats = owner.engine.getPositionTicks() / (double) Model::PPQ;
            auto view = owner.viewport != nullptr ? owner.viewport->getViewArea() : getLocalBounds();

            for (auto& entry : cachedClipRects())
            {
                if (entry.rect.getRight() < view.getX() || entry.rect.getX() > view.getRight())
                    continue;

                auto clip = entry.clip;
                if (clip.getProperty (IDs::type).toString() != "pattern")
                    continue;
                if (! Model::isPlaylistTrackEnabled (owner.project, (int) clip.getProperty (IDs::track)))
                    continue;

                const double startBeats = clip.getProperty (IDs::startBeats);
                const double clipBeats = clip.getProperty (IDs::lengthBeats);
                if (posBeats < startBeats || posBeats >= startBeats + clipBeats)
                    continue;

                auto pat = Model::findPatternById (owner.project, (int) clip.getProperty (IDs::patternId));
                if (! pat.isValid())
                    continue;
                const double patBeats = Model::patternLengthTicks (pat) / (double) Model::PPQ;
                if (patBeats <= 0.0)
                    continue;

                const auto& preview = cachedPreview (pat);
                if (preview.evs.empty())
                    continue;

                auto r = clipBodyRect (entry.rect);
                const float barH = clipBarHeight (r);
                const auto gm = previewGeom (preview, r, barH);
                if (! gm.valid)
                    continue;

                const double offsetBeats = clip.getProperty (IDs::offsetBeats, 0.0);
                const double contentBeats = juce::jlimit (0.0, clipBeats, patBeats - offsetBeats);
                const double local = posBeats - startBeats + offsetBeats;
                const int uid = (int) clip.getProperty (IDs::uid, 0);

                juce::Rectangle<float> nr;
                for (size_t i = 0; i < preview.evs.size(); ++i)
                {
                    const auto& ev = preview.evs[i];
                    if (ev.slide || local < ev.beat || local >= ev.beat + ev.len)
                        continue;
                    if (! previewNoteRect (ev, gm, r, contentBeats, offsetBeats, nr) || nr.getWidth() < 2.0f)
                        continue;

                    auto* e = findNoteGlow (uid, (int) i);
                    if (e == nullptr)
                    {
                        if (noteGlows.size() >= maxNoteGlows)
                            continue;
                        noteGlows.push_back ({ uid, (int) i, 0.0f, true, {} });
                        e = &noteGlows.back();
                    }
                    e->active = true;
                    e->bounds = nr.getSmallestIntegerContainer().expanded (8);
                }
            }
        }

        const auto visible = owner.viewport != nullptr ? owner.viewport->getViewArea() : getLocalBounds();

        juce::Rectangle<int> dirty;
        for (size_t i = 0; i < noteGlows.size();)
        {
            auto& e = noteGlows[i];
            e.glow = e.active ? juce::jmin (1.0f, e.glow + dt / 0.06f)
                              : juce::jmax (0.0f, e.glow - dt / 0.30f);
            auto vis = e.bounds.getIntersection (visible);
            if (! vis.isEmpty())
                dirty = dirty.getUnion (vis);
            if (! e.active && e.glow <= 0.0001f)
            {
                noteGlows[i] = noteGlows.back();
                noteGlows.pop_back();
                continue;
            }
            ++i;
        }

        if (! dirty.isEmpty())
            repaint (dirty);
    }

    void drawPeerAvatars (juce::Graphics& g, juce::Rectangle<float> r, float barH, int patternId)
    {
        if (! owner.getPeersOnPattern)
            return;
        auto peers = owner.getPeersOnPattern (patternId);
        if (peers.empty())
            return;

        auto vis = r.getIntersection (g.getClipBounds().toFloat());
        if (vis.isEmpty())
            return;

        const float d = juce::jlimit (8.0f, 16.0f, barH - 4.0f);
        const float cy = r.getY() + (barH - d) * 0.5f;
        float cx = vis.getRight() - 4.0f - d;
        for (auto& p : peers)
        {
            if (cx < vis.getX() + 1.0f)
                break;
            juce::Rectangle<float> circle (cx, cy, d, d);

            if (p.avatar.isValid())
            {
                juce::Path clip;
                clip.addEllipse (circle);
                g.saveState();
                g.reduceClipRegion (clip);
                g.drawImage (p.avatar, circle,
                             juce::RectanglePlacement::centred | juce::RectanglePlacement::fillDestination);
                g.restoreState();
            }
            else
            {
                g.setColour (p.colour);
                g.fillEllipse (circle);
                g.setColour (juce::Colours::white.withAlpha (0.95f));
                g.setFont (Typography::semi (juce::jmax (8.0f, d * 0.62f)));
                g.drawText (p.name.substring (0, 1).toUpperCase(),
                            circle.toNearestInt(), juce::Justification::centred);
            }

            g.setColour (Colors::text.withAlpha (0.9f));
            g.drawEllipse (circle, 1.2f);
            cx -= (d + 3.0f);
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colors::windowBg);
        auto clipB = g.getClipBounds();
        int nTracks = owner.numTracks();
        int th = owner.trackHeight;

        for (int t = juce::jmax (0, (clipB.getY() - rulerHeight) / juce::jmax (1, th));
             t < nTracks + Playlist::extraLanes; ++t)
        {
            int y = rulerHeight + t * th;
            if (y > clipB.getBottom())
                break;
            g.setColour (t % 2 == 0 ? Colors::panelDark : Colors::windowBg);
            g.fillRect (clipB.getX(), y, clipB.getWidth(), th);
            auto tint = playlistLaneColour (Model::getPlaylistTrack (owner.project, t));
            if (! tint.isTransparent())
            {
                g.setColour (tint.withAlpha (0.06f));
                g.fillRect (clipB.getX(), y, clipB.getWidth(), th);
            }
            if (! Model::isPlaylistTrackEnabled (owner.project, t))
            {
                g.setColour (juce::Colours::black.withAlpha (0.35f));
                g.fillRect (clipB.getX(), y, clipB.getWidth(), th);
            }
        }

        {
            const double groupBeats = 4.0 * Model::beatsPerBar;
            const double groupPx = groupBeats * owner.pxPerBeat;
            int first = juce::jmax (0, (int) std::floor (clipB.getX() / groupPx));
            int last  = (int) std::ceil (clipB.getRight() / groupPx);
            for (int gI = first; gI <= last; ++gI)
            {
                if ((gI & 1) == 0)
                    continue;
                int x0 = (int) std::round (gI * groupPx);
                int x1 = (int) std::round ((gI + 1) * groupPx);
                g.setColour (juce::Colours::black.withAlpha (0.18f));
                g.fillRect (x0, rulerHeight, x1 - x0, getHeight() - rulerHeight);
            }
        }

        int nBeats = totalBeats();
        {
            constexpr double minLinePx = 7.0;
            double gridStep = 1.0;
            while (gridStep * owner.pxPerBeat < minLinePx)
                gridStep *= 2.0;
            while (gridStep > 1.0 / 8.0 && gridStep * 0.5 * owner.pxPerBeat >= 10.0)
                gridStep *= 0.5;
            const double totalB = (double) nBeats;
            const double lastB = juce::jmin (totalB, (clipB.getRight() + 2) / owner.pxPerBeat);
            double b = juce::jmax (0.0, std::floor (((clipB.getX() - 2) / owner.pxPerBeat) / gridStep) * gridStep);
            for (; b <= lastB; b += gridStep)
            {
                int x = (int) (b * owner.pxPerBeat);
                int beatInt = (int) std::round (b);
                double frac = std::abs (b - std::round (b));
                bool onBeat = frac < 1.0e-6;
                bool onBar = onBeat && (beatInt % Model::beatsPerBar == 0);
                if (onBar)       g.setColour (Colors::gridBar);
                else if (onBeat) g.setColour (Colors::gridLine);
                else             g.setColour (Colors::gridLine.withAlpha (0.4f));
                g.drawVerticalLine (x, (float) rulerHeight, (float) getHeight());
            }
        }

        g.setColour (Colors::header);
        g.fillRect (clipB.getX(), 0, clipB.getWidth(), rulerHeight);
        g.setFont (Typography::ui (10.0f));
        int labelBars = 1;
        while (labelBars * Model::beatsPerBar * owner.pxPerBeat < 34.0)
            labelBars *= 2;
        const double barPx = juce::jmax (1.0, Model::beatsPerBar * owner.pxPerBeat);
        const int firstBar = juce::jmax (0, (int) std::floor ((clipB.getX() - 40) / barPx));
        const int lastBar  = juce::jmin (nBeats / Model::beatsPerBar,
                                         (int) std::ceil ((clipB.getRight() + 40) / barPx));
        for (int b = firstBar * Model::beatsPerBar; b <= lastBar * Model::beatsPerBar; b += Model::beatsPerBar)
        {
            int x = (int) (b * owner.pxPerBeat);
            int bar = b / Model::beatsPerBar;
            if (bar % labelBars != 0)
                continue;
            g.setColour (Colors::gridBar);
            g.drawVerticalLine (x, 4.0f, (float) rulerHeight);
            g.setColour (Colors::textDim);
            g.drawText (juce::String (bar + 1), x + 3, 2, 30, rulerHeight - 4,
                        juce::Justification::centredLeft);
        }

        for (auto& entry : cachedClipRects())
        {
            if (! entry.rect.expanded (8).intersects (clipB))
                continue;
            auto clip = entry.clip;
            auto r = clipBodyRect (entry.rect);
            auto clipType = clip.getProperty (IDs::type).toString();
            bool isAudio = clipType == "audio";
            bool isAuto  = clipType == "automation";
            bool selected = selection.contains (clip);
            double offsetBeats = clip.getProperty (IDs::offsetBeats, 0.0);
            double clipBeats = clip.getProperty (IDs::lengthBeats);

            const float barH = clipBarHeight (r);
            const bool compact = barH <= 0.0f;
            auto bar = r.withHeight (barH);
            auto body = r.withTrimmedTop (barH);

            juce::Colour colour;
            if (isAudio)
            {
                colour = Colors::clipTone (clipColour (clip));
                auto audioPath = clip.getProperty (IDs::path).toString();
                g.setColour (colour.darker (0.28f).withAlpha (0.95f));
                g.fillRect (r);
                if (! DebugFlags::disableWaveformThumbnails.load())
                if (auto* thumb = owner.getThumbnail (clip.getProperty (IDs::path).toString()))
                {
                    double head = 0.0, avail = thumb->getTotalLength();
                    AudioEngine::SampleTrimBounds tb;
                    if (owner.getClipTrimBounds (clip, audioPath, tb))
                    {
                        head = tb.startSeconds;
                        avail = tb.lengthSeconds;
                    }
                    double bpm = (double) owner.project.getProperty (IDs::bpm, 140.0);
                    double secondsPerBeat = 60.0 / juce::jmax (1.0, bpm);
                    if (avail > 0.0)
                        if (double mapped = AudioEngine::sourceSecondsPerBeat (clip, avail, bpm);
                            mapped > 0.0)
                            secondsPerBeat = mapped;
                    double t0 = head + offsetBeats * secondsPerBeat;
                    double t1 = juce::jmin (head + avail, t0 + clipBeats * secondsPerBeat);
                    const bool tinyBody = body.getHeight() < 8.0f;
                    auto waveArea = body.toNearestInt().reduced (tinyBody ? 1 : 2, tinyBody ? 0 : 2);
                    if (secondsPerBeat > 0.0)
                    {
                        const int contentW = (int) std::ceil ((t1 - t0) / secondsPerBeat * owner.pxPerBeat);
                        waveArea = waveArea.withWidth (juce::jlimit (0, waveArea.getWidth(), contentW));
                    }
                    auto visible = waveArea.getIntersection (clipB);
                    if (waveArea.getWidth() > 0 && waveArea.getHeight() > 0 && ! visible.isEmpty())
                    {
                        constexpr int bandGrid = 256;
                        const int rel0 = ((visible.getX() - waveArea.getX()) / bandGrid) * bandGrid;
                        const int rel1 = ((visible.getRight() - waveArea.getX() + bandGrid - 1) / bandGrid) * bandGrid;
                        auto band = juce::Rectangle<int> (waveArea.getX() + rel0, waveArea.getY(),
                                                          rel1 - rel0, waveArea.getHeight())
                                        .getIntersection (waveArea);
                        if (! band.isEmpty())
                        {
                            const double perPx = (t1 - t0) / (double) waveArea.getWidth();
                            const double bt0 = t0 + perPx * (band.getX() - waveArea.getX());
                            g.setColour (colour.brighter (0.85f).withAlpha (0.9f));
                            thumb->drawChannel (g, band, bt0, bt0 + perPx * band.getWidth(), 0, 0.9f);
                        }
                    }
                }
                if (isStretchDrag (clip))
                {
                    g.setColour (Colors::accent.withAlpha (0.15f));
                    g.fillRect (r);
                }
            }
            else if (isAuto)
            {
                colour = Colors::clipTone (clipColour (clip));
                g.setColour (colour.withAlpha (compact ? 0.25f : 0.14f));
                g.fillRect (r);
                drawAutomationCurve (g, clip, body, colour);
            }
            else
            {
                auto pat = Model::findPatternById (owner.project, (int) clip.getProperty (IDs::patternId));
                colour = Colors::clipTone (Model::patternColour (pat));

                double patBeats = pat.isValid() ? Model::patternLengthTicks (pat) / (double) Model::PPQ
                                                : clipBeats;

                double contentBeats = juce::jlimit (0.0, clipBeats, patBeats - offsetBeats);
                auto content = body.withWidth ((float) (contentBeats * owner.pxPerBeat));

                g.setColour (colour.withAlpha (0.10f));
                g.fillRect (r);
                g.setColour (colour.withAlpha (compact ? 0.5f : 0.22f));
                g.fillRect (content);

                if (pat.isValid())
                    drawPatternPreview (g, pat, r, contentBeats, colour, offsetBeats, barH,
                                        (int) clip.getProperty (IDs::uid, 0));
            }

            if (! compact)
            {
                g.setColour (Colors::clipHeader (colour).withAlpha (0.96f));
                g.fillRect (bar);

                juce::String label;
                if (isAudio)
                {
                    auto ap = clip.getProperty (IDs::path).toString();
                    auto af = Model::getAudioFiles (owner.project).getChildWithProperty (IDs::audioPath, ap);
                    label = af.isValid() ? af.getProperty (IDs::name).toString() : juce::String();
                    if (label.isEmpty())
                        label = juce::File (ap).getFileNameWithoutExtension();
                }
                else if (isAuto)
                {
                    label = Automation::targetName (owner.project, clip.getProperty (IDs::target).toString());
                }
                else
                {
                    label = Model::findPatternById (owner.project, (int) clip.getProperty (IDs::patternId))
                                .getProperty (IDs::name).toString();
                }
                auto textColour = Colors::contrastText (Colors::clipHeader (colour));

                int labelIndent = 6;
                if (barH >= 12.0f && bar.getWidth() > 30.0f)
                {
                    bool hot = clip == hoveredMenuZoneClip;
                    auto zone = clipMenuZone (clip).toFloat()
                                    .withY (bar.getY()).withHeight (bar.getHeight());
                    if (hot)
                    {
                        g.setColour (juce::Colours::white.withAlpha (0.12f));
                        g.fillRect (zone);
                    }
                    const float aw = 11.0f, ah = 6.0f;
                    const float cy = bar.getCentreY() - 0.5f;
                    const float ax = zone.getCentreX() - aw * 0.5f, ay = cy - ah / 3.0f;
                    juce::Path tri;
                    tri.addTriangle (ax, ay, ax + aw, ay, ax + aw * 0.5f, ay + ah);
                    g.setColour (textColour.withAlpha (hot ? 1.0f : 0.6f));
                    g.fillPath (tri);
                    labelIndent = (int) zone.getWidth() + 4;
                }

                const float labelFontH = juce::jmin (16.0f, barH - 2.0f);
                if (labelFontH >= 8.0f)
                {
                    g.setColour (textColour);
                    g.setFont (Typography::semi (labelFontH));
                    g.drawText (label,
                                bar.withTrimmedLeft ((float) labelIndent).withTrimmedRight (6.0f)
                                   .translated (0.0f, -0.5f),
                                juce::Justification::centredLeft, true);
                }
            }

            if (r.getWidth() > 6.0f)
            {
                g.setColour (colour.brighter (0.15f).withAlpha (0.55f));
                g.fillRect (r.getX() + 2.5f, r.getY() + 0.5f, r.getWidth() - 5.0f, 1.0f);
            }

            if (isHeldClip (clip))
            {
                g.setColour (juce::Colours::white.withAlpha (0.16f));
                g.fillRect (r);
            }

            if (selected)
            {
                if (Fx::glowEnabled)
                {
                    const float ex[3] = { 6.0f, 3.0f, 1.0f };
                    const float ga[3] = { 0.028f, 0.05f, 0.09f };
                    for (int i = 0; i < 3; ++i)
                    {
                        g.setColour (Colors::accent.withAlpha (ga[i]));
                        g.fillRect (r.expanded (ex[i]));
                    }
                }
                g.setColour (Colors::accent.withAlpha (0.75f));
                g.drawRect (r.reduced (0.75f), 1.3f);
            }

            if (isAudio && (bool) clip.getProperty (IDs::pending, false))
            {
                g.setColour (juce::Colours::black.withAlpha (0.35f));
                g.fillRect (r);

                juce::Path src, dashed;
                src.addRectangle (r);
                float dashes[] = { 4.0f, 3.0f };
                juce::PathStrokeType (1.4f).createDashedStroke (dashed, src, dashes, 2);
                g.setColour (Colors::record);
                g.fillPath (dashed);

                auto recBy = clip.getProperty (IDs::recordedBy).toString();
                auto localId = owner.getLocalPeerIdString ? owner.getLocalPeerIdString() : juce::String();
                bool mine = recBy.isEmpty() || (localId.isNotEmpty() && recBy == localId);
                auto acc = pendingCheckRect (clip).toFloat();
                auto rej = pendingRefuseRect (clip).toFloat();

                if (mine && ! acc.isEmpty() && ! rej.isEmpty())
                {
                    juce::Path tick;
                    tick.startNewSubPath (acc.getX(), acc.getCentreY() + acc.getHeight() * 0.05f);
                    tick.lineTo (acc.getCentreX() - acc.getWidth() * 0.10f, acc.getBottom());
                    tick.lineTo (acc.getRight(), acc.getY());
                    g.setColour (Colors::noteGreen);
                    g.strokePath (tick, juce::PathStrokeType (2.4f, juce::PathStrokeType::curved,
                                                              juce::PathStrokeType::rounded));

                    juce::Path cross;
                    cross.startNewSubPath (rej.getTopLeft());
                    cross.lineTo (rej.getBottomRight());
                    cross.startNewSubPath (rej.getTopRight());
                    cross.lineTo (rej.getBottomLeft());
                    g.setColour (Colors::record);
                    g.strokePath (cross, juce::PathStrokeType (2.4f, juce::PathStrokeType::curved,
                                                              juce::PathStrokeType::rounded));
                }
                else if (! mine && body.getHeight() >= 12.0f)
                {
                    g.setColour (Colors::text.withAlpha (0.85f));
                    g.setFont (Typography::ui (10.0f));
                    g.drawText ("waiting for share", body, juce::Justification::centred);
                }
            }

            if (! isAudio && ! isAuto && barH >= 12.0f)
                drawPeerAvatars (g, r, barH, (int) clip.getProperty (IDs::patternId));
        }

        for (auto& ghost : ghosts)
        {
            if (! ghost.rect.getSmallestIntegerContainer().intersects (clipB))
                continue;
            float shrink = (1.0f - ghost.alpha) * ghost.rect.getHeight() * 0.18f;
            g.setColour (ghost.colour.withAlpha (ghost.alpha * 0.7f));
            g.fillRect (ghost.rect.reduced (0.0f, shrink));
        }

        if (owner.engine.isRecording())
        {
            auto recs = owner.engine.getActiveRecordings();
            double playBeats = owner.engine.getPositionTicks() / (double) Model::PPQ;
            for (auto& rec : recs)
            {
                int lane = owner.getRecordingLane ? owner.getRecordingLane (rec.mixerTrack) : -1;
                if (lane < 0 || lane >= nTracks)
                    continue;
                double startBeats = rec.startTicks / (double) Model::PPQ;
                float x0 = (float) (startBeats * owner.pxPerBeat);
                float x1 = juce::jmax (x0, (float) (playBeats * owner.pxPerBeat));
                juce::Rectangle<float> region (x0, (float) (rulerHeight + lane * th),
                                               juce::jmax (2.0f, x1 - x0), (float) th);

                g.setColour (Colors::record.withAlpha (0.22f));
                g.fillRect (region);
                g.setColour (Colors::record);
                g.drawRect (region.reduced (0.75f), 1.5f);

                static constexpr int maxPairs = 4096;
                float pairs[maxPairs * 2];
                int n = owner.engine.getRecordingPeaks (rec.mixerTrack, pairs, maxPairs);
                if (n > 1 && region.getWidth() > 3.0f)
                {
                    auto wf = region.reduced (1.0f, 3.0f);
                    float mid = wf.getCentreY(), halfH = wf.getHeight() * 0.5f;
                    int cols = juce::jmax (1, (int) wf.getWidth());
                    int c0 = juce::jmax (0, (int) std::floor (clipB.getX() - wf.getX()));
                    int c1 = juce::jmin (cols - 1, (int) std::ceil (clipB.getRight() - wf.getX()));
                    g.setColour (Colors::record.brighter (0.4f));
                    for (int c = c0; c <= c1; ++c)
                    {
                        int p0 = (int) ((juce::int64) c       * n / cols);
                        int p1 = juce::jmax (p0 + 1, (int) ((juce::int64) (c + 1) * n / cols));
                        float mn = pairs[p0 * 2], mx = pairs[p0 * 2 + 1];
                        for (int p = p0 + 1; p < p1 && p < n; ++p)
                        {
                            mn = juce::jmin (mn, pairs[p * 2]);
                            mx = juce::jmax (mx, pairs[p * 2 + 1]);
                        }
                        float xx = wf.getX() + (float) c;
                        g.drawLine (xx, mid - juce::jlimit (-1.0f, 1.0f, mx) * halfH,
                                    xx, mid - juce::jlimit (-1.0f, 1.0f, mn) * halfH, 1.0f);
                    }
                }

                g.setColour (Colors::text.withAlpha (0.9f));
                g.setFont (Typography::semi (10.0f));
                g.drawText ("Recording", region.reduced (4.0f, 1.0f).withHeight ((float) clipHeaderHeight),
                            juce::Justification::centredLeft);
            }
        }

        if (mode == Mode::rubber)
        {
            g.setColour (Colors::accent.withAlpha (0.15f));
            g.fillRect (rubberRect);
            g.setColour (Colors::accent.withAlpha (0.7f));
            g.drawRect (rubberRect, 1);
        }

        if (mode == Mode::slice)
        {
            int x = (int) std::round (sliceBeat * owner.pxPerBeat);
            g.setColour (juce::Colours::white.withAlpha (0.85f));
            g.drawVerticalLine (x, (float) juce::jmin (sliceY0, sliceY1),
                                   (float) juce::jmax (sliceY0, sliceY1));
            g.setColour (juce::Colours::white.withAlpha (0.25f));
            g.drawVerticalLine (x - 1, (float) juce::jmin (sliceY0, sliceY1),
                                       (float) juce::jmax (sliceY0, sliceY1));
            g.drawVerticalLine (x + 1, (float) juce::jmin (sliceY0, sliceY1),
                                       (float) juce::jmax (sliceY0, sliceY1));
        }

        double beats = mode == Mode::seek ? seekPreviewBeats
                                          : owner.engine.getPositionTicks() / (double) Model::PPQ;
        float x = (float) (beats * owner.pxPerBeat);
        float h = (float) getHeight();
        bool playing = owner.engine.isPlaying() && mode != Mode::seek;

        const float pgi = Fx::playlistGlowIntensity;
        const bool  glowOn = Fx::glowEnabled && pgi > 0.001f;

        if (glowOn && playing && ! playheadWrapped)
        {
            const float tail = 52.0f;
            juce::ColourGradient trail (Colors::accent2.withAlpha (juce::jmin (1.0f, 0.18f * pgi)), x, 0.0f,
                                        Colors::accent2.withAlpha (0.0f), x - tail, 0.0f, false);
            g.setGradientFill (trail);
            g.fillRect (juce::Rectangle<float> (x - tail, 0.0f, tail, h));
        }

        if (glowOn)
        {
            for (auto& s : sparkles)
            {
                float a = juce::jlimit (0.0f, 1.0f, s.life / s.maxLife);
                g.setColour (Colors::accent2.brighter (0.6f).withAlpha (juce::jmin (1.0f, a * 0.85f * pgi)));
                g.fillEllipse (s.x - s.size, s.y - s.size, s.size * 2.0f, s.size * 2.0f);
            }

            const float glowW[3] = { 8.0f, 4.0f, 2.4f };
            const float glowA[3] = { 0.08f, 0.15f, 0.25f };
            for (int i = 0; i < 3; ++i)
            {
                g.setColour (Colors::accent2.withAlpha (juce::jmin (1.0f, glowA[i] * pgi)));
                g.fillRect (juce::Rectangle<float> (x - glowW[i] * 0.5f, 0.0f, glowW[i], h));
            }
        }

        g.setColour (Colors::accent2);
        g.fillRect (juce::Rectangle<float> (x - 0.75f, 0.0f, 1.5f, h));

        juce::Path marker;
        marker.addTriangle (x - 5.0f, 2.0f, x + 5.0f, 2.0f, x, 12.0f);
        g.setColour (Colors::accent2.withAlpha (0.25f));
        g.fillPath (marker, juce::AffineTransform::scale (1.5f, 1.35f, x, 6.0f));
        g.setColour (Colors::accent2);
        g.fillPath (marker);
    }

    void repaintPlayhead()
    {
        double beats = owner.engine.getPositionTicks() / (double) Model::PPQ;
        int x = (int) (beats * owner.pxPerBeat);

        advanceSparkles ((float) x, mode != Mode::seek);
        repaintSparkles();

        if (lastPlayheadPaintX < 0)
        {
            playheadWrapped = false;
            repaintPlayheadBand (x);
            lastPlayheadPaintX = x;
            return;
        }
        playheadWrapped = (x < lastPlayheadPaintX);

        int half = owner.viewport != nullptr ? owner.viewport->getMaximumVisibleWidth() / 2 : 0;
        if (std::abs (x - lastPlayheadPaintX) > juce::jmax (1, half))
        {
            repaintPlayheadBand (lastPlayheadPaintX);
            repaintPlayheadBand (x);
        }
        else
        {
            int lo = juce::jmin (x, lastPlayheadPaintX) - 64;
            int hi = juce::jmax (x, lastPlayheadPaintX) + 20;
            repaint (lo, 0, hi - lo, getHeight());
        }
        lastPlayheadPaintX = x;
    }

    void repaintPlayheadBand (int x) { repaint (x - 64, 0, 84, getHeight()); }

    void repaintSparkles()
    {
        if (sparkles.empty())
            return;

        float x0 = sparkles[0].x, x1 = x0, y0 = sparkles[0].y, y1 = y0;
        for (auto& s : sparkles)
        {
            x0 = juce::jmin (x0, s.x - s.size);  x1 = juce::jmax (x1, s.x + s.size);
            y0 = juce::jmin (y0, s.y - s.size);  y1 = juce::jmax (y1, s.y + s.size);
        }
        const int ix = (int) std::floor (x0) - 2, iy = (int) std::floor (y0) - 2;
        juce::Rectangle<int> dirty (ix, iy, (int) std::ceil (x1) - ix + 3, (int) std::ceil (y1) - iy + 3);
        if (owner.viewport != nullptr)
            dirty = dirty.getIntersection (owner.viewport->getViewArea());
        if (! dirty.isEmpty())
            repaint (dirty);
    }

    void advanceSparkles (float emitX, bool emit)
    {
        juce::uint32 now = juce::Time::getMillisecondCounter();
        float dt = lastFxTickMs == 0 ? 1.0f / 60.0f : (float) (now - lastFxTickMs) / 1000.0f;
        lastFxTickMs = now;
        dt = juce::jlimit (0.0f, 0.1f, dt);

        for (size_t i = 0; i < sparkles.size(); )
        {
            auto& s = sparkles[i];
            s.y   += s.vy * dt;
            s.life -= dt;
            if (s.life <= 0.0f) { sparkles[i] = sparkles.back(); sparkles.pop_back(); }
            else ++i;
        }

        const float pgi = Fx::playlistGlowIntensity;
        const int   cap = juce::jlimit (0, 40, (int) std::round (18.0f * pgi));
        if (emit && Fx::glowEnabled && pgi > 0.001f && (int) sparkles.size() < cap)
        {
            int n = 1 + (sparkleRng.nextFloat() < 0.4f ? 1 : 0);
            for (int k = 0; k < n && (int) sparkles.size() < cap; ++k)
            {
                Sparkle s;
                s.x       = emitX + (sparkleRng.nextFloat() - 0.5f) * 8.0f;
                s.y       = (float) getHeight() * (0.35f + sparkleRng.nextFloat() * 0.6f);
                s.vy      = -(18.0f + sparkleRng.nextFloat() * 26.0f);
                s.maxLife = 0.5f + sparkleRng.nextFloat() * 0.3f;
                s.life    = s.maxLife;
                s.size    = 0.8f + sparkleRng.nextFloat() * 1.4f;
                sparkles.push_back (s);
            }
        }
    }

    void tickPlayheadFx()
    {
        advanceSparkles (0.0f, false);
        repaintSparkles();
    }

    bool playheadFxAlive() const { return ! sparkles.empty(); }

    void resetPlayheadPaint() { lastPlayheadPaintX = -1; }

    juce::Rectangle<int> pendingIconRect (const juce::ValueTree& clip, bool refuse) const
    {
        auto cr = clipRect (clip);
        int barH = (int) std::round (clipBarHeight (clipBodyRect (cr)));
        int avail = cr.getHeight() - barH - 2;
        int icon = juce::jmin (15, juce::jmin (cr.getWidth() / 2 - 4, avail));
        if (icon < 8)
            return {};
        int gap  = juce::jmin (10, juce::jmax (2, (cr.getWidth() - icon * 2) / 4));
        int top  = cr.getY() + barH + icon / 2;
        int cy   = juce::jlimit (top, juce::jmax (top, cr.getBottom() - icon / 2 - 1),
                                 (cr.getY() + barH + cr.getBottom()) / 2);
        int cx   = cr.getCentreX() + (refuse ? (icon + gap) / 2 : -(icon + gap) / 2);
        return juce::Rectangle<int> (0, 0, icon, icon).withCentre ({ cx, cy });
    }

    juce::Rectangle<int> pendingCheckRect  (const juce::ValueTree& clip) const { return pendingIconRect (clip, false); }
    juce::Rectangle<int> pendingRefuseRect (const juce::ValueTree& clip) const { return pendingIconRect (clip, true); }

    juce::ValueTree clipAt (juce::Point<int> pos) const
    {
        auto playlist = Model::getPlaylist (owner.project);
        for (int i = playlist.getNumChildren(); --i >= 0;)
        {
            auto clip = playlist.getChild (i);
            if (clip.hasType (IDs::Clip) && clipRect (clip).contains (pos))
                return clip;
        }
        return {};
    }

    double snapBeat (double beats) const
    {
        double snap = owner.getSnapBeats();
        if (snap <= 0.0)
            return juce::jmax (0.0, beats);
        return juce::jmax (0.0, std::floor (beats / snap) * snap);
    }

    int trackAt (int y) const
    {
        return juce::jlimit (0, owner.numTracks() - 1, (y - rulerHeight) / owner.trackHeight);
    }

    struct DragInfo
    {
        juce::ValueTree clip;
        double startBeats;
        int track;
        double lengthBeats = 0.0;
        double offsetBeats = 0.0;
        double stretchSpanBeats = 0.0;
    };

    double stretchSpanFor (const juce::ValueTree& clip) const
    {
        if (clip.getProperty (IDs::type).toString() != "audio")
            return 0.0;

        const double seconds = owner.engine.getProcessedSampleSeconds (
            clip.getProperty (IDs::path).toString(), clip);
        if (seconds <= 0.0)
            return 0.0;

        const double bpm = (double) owner.project.getProperty (IDs::bpm, 140.0);
        const double perBeat = AudioEngine::sourceSecondsPerBeat (clip, seconds, bpm);
        if (perBeat <= 0.0)
            return 0.0;

        return seconds / perBeat;
    }

    bool isStretchDrag (const juce::ValueTree& clip) const
    {
        if (mode != Mode::resizeRight && mode != Mode::resizeLeft)
            return false;
        for (auto& info : dragInfos)
            if (info.clip == clip)
                return info.stretchSpanBeats > 0.0;
        return false;
    }

    static void applyStretch (const DragInfo& info, double newLengthBeats)
    {
        const double scale = newLengthBeats / juce::jmax (1.0e-9, info.lengthBeats);
        auto clip = info.clip;
        clip.setProperty (IDs::stretchTempoMode, 3, nullptr);
        clip.setProperty (IDs::stretchBeats, info.stretchSpanBeats * scale, nullptr);
        clip.setProperty (IDs::offsetBeats, info.offsetBeats * scale, nullptr);
        clip.setProperty (IDs::stretchTime, 1.0, nullptr);
        clip.setProperty (IDs::stretchMultiplier, 1.0, nullptr);
    }

    int trackAtForDrop (int y) const
    {
        return juce::jlimit (0, owner.numTracks() + Playlist::extraLanes - 1,
                             (y - rulerHeight) / owner.trackHeight);
    }

    void removeClip (juce::ValueTree clip)
    {
        if (ghosts.size() < 64)
        {
            if (ghosts.empty())
                lastGhostTickMs = juce::Time::getMillisecondCounter();
            ghosts.push_back ({ clipRect (clip).toFloat().reduced (0.5f, 1.5f),
                                Colors::clipTone (clipColour (clip)), 1.0f });
        }
        if (clip == hoveredMenuZoneClip)
            hoveredMenuZoneClip = {};
        selection.removeAllInstancesOf (clip);
        clip.getParent().removeChild (clip, nullptr);
        repaint();
    }

    void tickGhosts()
    {
        juce::uint32 now = juce::Time::getMillisecondCounter();
        float dt = lastGhostTickMs == 0 ? 1.0f / 60.0f : (float) (now - lastGhostTickMs) / 1000.0f;
        lastGhostTickMs = now;
        dt = juce::jlimit (0.0f, 0.1f, dt);

        for (size_t i = 0; i < ghosts.size(); )
        {
            repaint (ghosts[i].rect.getSmallestIntegerContainer().expanded (2));
            ghosts[i].alpha -= dt / 0.14f;
            if (ghosts[i].alpha <= 0.0f) { ghosts[i] = ghosts.back(); ghosts.pop_back(); }
            else ++i;
        }
    }

    bool ghostsAlive() const { return ! ghosts.empty(); }

    void selectOnly (juce::ValueTree clip)
    {
        selection.clearQuick();
        if (clip.isValid())
            selection.add (clip);
        repaint();
    }

    void keepGroupSelection()
    {
        if (dragInfos.size() > 1)
            transientSelection = false;
    }

    void selectPatternForClip (const juce::ValueTree& clip)
    {
        if (! clip.isValid())
            return;

        auto clipType = clip.getProperty (IDs::type).toString();
        if (clipType == "audio")
        {
            owner.setAudioBrushFromClip (clip);
            return;
        }
        if (clipType != "pattern")
            return;

        owner.clearAudioBrush();
        int pid = (int) clip.getProperty (IDs::patternId);
        owner.setSelectedPattern (pid);
        owner.engine.setCurrentPatternId (pid);
        if (owner.onPatternSelected)
            owner.onPatternSelected (pid);
    }

    bool openPatternClip (juce::ValueTree clip)
    {
        if (! clip.isValid() || clip.getProperty (IDs::type).toString() != "pattern")
            return false;

        mode = Mode::none;
        dragClip = {};
        dragInfos.clear();
        selectOnly (clip);
        selectPatternForClip (clip);
        if (owner.onOpenPianoRoll)
            owner.onOpenPianoRoll ((int) clip.getProperty (IDs::patternId));
        return true;
    }

    juce::ValueTree placePatternClip (int patternId, juce::Point<int> pos)
    {
        auto pattern = Model::findPatternById (owner.project, patternId);
        if (! pattern.isValid())
            return {};

        double patBeats = Model::patternLengthTicks (pattern) / (double) Model::PPQ;
        double len = juce::jmax (0.25, patBeats);
        int track = trackAtForDrop (pos.getY());
        double start = snapBeat (pos.getX() / owner.pxPerBeat);

        juce::ValueTree newClip (IDs::Clip);
        newClip.setProperty (IDs::uid, Model::newId(), nullptr);
        newClip.setProperty (IDs::type, "pattern", nullptr);
        newClip.setProperty (IDs::patternId, patternId, nullptr);
        newClip.setProperty (IDs::track, track, nullptr);
        newClip.setProperty (IDs::startBeats, start, nullptr);
        newClip.setProperty (IDs::lengthBeats, len, nullptr);
        Model::getPlaylist (owner.project).appendChild (newClip, nullptr);
        owner.growPatternToBeats (pattern, len);
        owner.lastClipLenBeats = len;
        owner.setSelectedPattern (patternId);
        owner.engine.setCurrentPatternId (patternId);
        if (owner.onPatternSelected)
            owner.onPatternSelected (patternId);
        selectOnly (newClip);
        updateSize();
        return newClip;
    }

    void duplicateSelection()
    {
        auto playlist = Model::getPlaylist (owner.project);

        juce::Array<juce::ValueTree> src = selection;
        if (src.isEmpty())
            for (int i = 0; i < playlist.getNumChildren(); ++i)
                if (playlist.getChild (i).hasType (IDs::Clip))
                    src.add (playlist.getChild (i));
        if (src.isEmpty())
            return;

        double minStart = 1.0e18, maxEnd = 0.0;
        for (auto& c : src)
        {
            double s = (double) c.getProperty (IDs::startBeats);
            double e = s + (double) c.getProperty (IDs::lengthBeats);
            minStart = juce::jmin (minStart, s);
            maxEnd = juce::jmax (maxEnd, e);
        }
        double span = maxEnd - minStart;
        double bars = std::ceil (span / Model::beatsPerBar);
        double shift = juce::jmax ((double) Model::beatsPerBar, bars * Model::beatsPerBar);

        juce::Array<juce::ValueTree> newSel;
        for (auto& c : src)
        {
            auto copy = c.createCopy();
            copy.setProperty (IDs::uid, Model::newId(), nullptr);
            copy.setProperty (IDs::startBeats, (double) c.getProperty (IDs::startBeats) + shift, nullptr);
            playlist.appendChild (copy, nullptr);
            newSel.add (copy);
        }
        selection = newSel;
        updateSize();
        repaint();
        if (owner.engine.isPlaying() && owner.engine.isSongMode())
            owner.engine.seekTicks ((minStart + shift) * Model::PPQ);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown())
            return;
        moveDidDrag = false;
        clipsHeld = false;
        if (e.getPosition().getY() < rulerHeight)
        {
            if (e.mods.isPopupMenu())
            {
                mode = Mode::rubber;
                rubberFromRuler = true;
                rubberAnchor = { e.getPosition().getX(), rulerHeight };
                rubberRect = {};
                if (! e.mods.isShiftDown())
                    selection.clearQuick();
                repaint();
                return;
            }
            mode = Mode::seek;
            seekTo (e.getPosition().getX());
            return;
        }

        auto clip = clipAt (e.getPosition());

        if (clip.isValid() && ! e.mods.isPopupMenu()
            && clip.getProperty (IDs::type).toString() == "audio"
            && (bool) clip.getProperty (IDs::pending, false))
        {
            auto recBy = clip.getProperty (IDs::recordedBy).toString();
            auto localId = owner.getLocalPeerIdString ? owner.getLocalPeerIdString() : juce::String();
            bool mine = recBy.isEmpty() || (localId.isNotEmpty() && recBy == localId);
            if (mine && pendingCheckRect (clip).contains (e.getPosition()))
            {
                clip.removeProperty (IDs::pending, nullptr);
                repaint();
                return;
            }
            if (mine && pendingRefuseRect (clip).contains (e.getPosition()))
            {
                removeClip (clip);
                return;
            }
        }

        if (isAutomationClip (clip) && handleAutomationMouseDown (clip, e))
            return;

        if (e.mods.isPopupMenu())
        {
            mode = Mode::erase;
            if (clip.isValid())
                removeClip (clip);
            return;
        }

        if (e.mods.isCtrlDown() && ! e.mods.isPopupMenu())
        {
            mode = Mode::rubber;
            rubberAnchor = e.getPosition();
            rubberRect = {};
            if (! e.mods.isShiftDown())
                selection.clearQuick();
            repaint();
            return;
        }

        if (clip.isValid())
        {
            if (clipMenuZone (clip).contains (e.getPosition()))
            {
                mode = Mode::none;
                dragClip = {};
                bool isPat = clip.getProperty (IDs::type).toString() == "pattern";

                if (isPat)
                {
                    int pid = (int) clip.getProperty (IDs::patternId);
                    juce::PopupMenu m;
                    m.addItem (3, "Rename...");
                    m.addItem (4, "Random color");
                    m.addItem (5, "Change color...");
                    m.addSeparator();
                    m.addItem (6, "Make Unique");
                    m.addItem (1, "Split Pattern");
                    m.addItem (2, "Delete Pattern from Project");
                    m.showMenuAsync (playlistMenuOptions (*this),
                        [this, pid, clip] (int res)
                        {
                            if (res == 1)
                                owner.splitPatternByChannel (pid);
                            else if (res == 6)
                                owner.makeClipUnique (clip);
                            else if (res == 2 && owner.patternPicker)
                            {
                                PatternPicker::Entry e;
                                e.kind = PatternPicker::EntryKind::pattern;
                                e.id = pid;
                                owner.patternPicker->deleteEntry (e);
                            }
                            else if (res == 3)
                            {
                                PatternPicker::Entry e;
                                e.kind = PatternPicker::EntryKind::pattern;
                                e.id = pid;
                                owner.patternPicker->showRenameDialogForEntry (e);
                            }
                            else if (res == 4)
                            {
                                PatternPicker::Entry e;
                                e.kind = PatternPicker::EntryKind::pattern;
                                e.id = pid;
                                owner.patternPicker->randomiseColourForEntry (e);
                            }
                            else if (res == 5)
                            {
                                PatternPicker::Entry e;
                                e.kind = PatternPicker::EntryKind::pattern;
                                e.id = pid;
                                owner.patternPicker->showColourPickerForEntry (e);
                            }
                        });
                }
                else
                {
                    auto audioPath = clip.getProperty (IDs::path).toString();
                    juce::PopupMenu m;
                    m.addItem (3, "Rename...");
                    m.addItem (4, "Random color");
                    m.addItem (5, "Change color...");
                    m.addSeparator();
                    m.addItem (2, "Delete Audio from Project");
                    m.showMenuAsync (playlistMenuOptions (*this),
                        [this, audioPath] (int res)
                        {
                            if (res == 2 && owner.patternPicker)
                            {
                                PatternPicker::Entry e;
                                e.kind = PatternPicker::EntryKind::audio;
                                e.path = audioPath;
                                owner.patternPicker->deleteEntry (e);
                            }
                            else if (res == 3)
                            {
                                PatternPicker::Entry e;
                                e.kind = PatternPicker::EntryKind::audio;
                                e.path = audioPath;
                                owner.patternPicker->showRenameDialogForEntry (e);
                            }
                            else if (res == 4)
                            {
                                PatternPicker::Entry e;
                                e.kind = PatternPicker::EntryKind::audio;
                                e.path = audioPath;
                                owner.patternPicker->randomiseColourForEntry (e);
                            }
                            else if (res == 5)
                            {
                                PatternPicker::Entry e;
                                e.kind = PatternPicker::EntryKind::audio;
                                e.path = audioPath;
                                owner.patternPicker->showColourPickerForEntry (e);
                            }
                        });
                }
                return;
            }
        }

        if (owner.tool == Tool::slice)
        {
            mode = Mode::slice;
            sliceBeat = snapBeat (e.getPosition().getX() / owner.pxPerBeat);
            sliceY0 = sliceY1 = e.getPosition().getY();
            repaint();
            return;
        }

        bool justPainted = clip.isValid() && clip == lastPaintedClip;
        lastPaintedClip = {};
        if (e.getNumberOfClicks() >= 2 && ! justPainted && openPatternClip (clip))
            return;

        if (clip.isValid())
        {
            auto r = clipRect (clip);
            dragClip = clip;
            selectPatternForClip (clip);

            owner.lastClipLenBeats = juce::jmax (0.25, (double) clip.getProperty (IDs::lengthBeats));

            if (! selection.contains (clip))
            {
                if (! e.mods.isShiftDown())
                {
                    selection.clearQuick();
                    transientSelection = true;
                }
                selection.add (clip);
            }
            else if (! e.mods.isShiftDown() && selection.size() > 1)
            {
                transientSelection = true;
            }

            int zone = resizeZoneAt (e.getPosition(), clip);

            if (zone > 0)           mode = Mode::resizeRight;
            else if (zone < 0)      mode = Mode::resizeLeft;
            else
            {
                mode = Mode::move;
                mouseDownBeats = e.getPosition().getX() / owner.pxPerBeat;
                mouseDownTrack = trackAt (e.getPosition().getY());
            }

            dragInfos.clear();
            const bool stretching = owner.stretchEnabled
                                 && (mode == Mode::resizeRight || mode == Mode::resizeLeft);
            auto capture = [this, stretching] (const juce::ValueTree& c)
            {
                dragInfos.add ({ c, (double) c.getProperty (IDs::startBeats),
                                 (int) c.getProperty (IDs::track),
                                 (double) c.getProperty (IDs::lengthBeats),
                                 (double) c.getProperty (IDs::offsetBeats, 0.0),
                                 stretching ? stretchSpanFor (c) : 0.0 });
            };
            if (selection.contains (clip))
                for (auto& sel : selection)
                    capture (sel);
            else
                capture (clip);
            clipsHeld = true;
            repaint();
            return;
        }

        if (owner.tool == Tool::select)
        {
            mode = Mode::rubber;
            rubberAnchor = e.getPosition();
            rubberRect = {};
            if (! e.mods.isShiftDown())
                selection.clearQuick();
            repaint();
            return;
        }

        if (owner.audioBrushPath.isNotEmpty())
        {
            juce::File audioFile (owner.audioBrushPath);
            if (! audioFile.existsAsFile())
                return;

            int audioTrack = trackAtForDrop (e.getPosition().getY());
            double audioStart = snapBeat (e.getPosition().getX() / owner.pxPerBeat);
            owner.paintAudioBrush (audioFile, audioTrack, audioStart);

            auto playlist = Model::getPlaylist (owner.project);
            auto placed = playlist.getChild (playlist.getNumChildren() - 1);
            if (placed.hasType (IDs::Clip))
            {
                dragClip = placed;
                selectOnly (placed);
                transientSelection = true;
                lastPaintedClip = placed;
                mode = Mode::move;
                mouseDownBeats = e.getPosition().getX() / owner.pxPerBeat;
                mouseDownTrack = audioTrack;
                dragInfos.clear();
                dragInfos.add ({ placed, (double) placed.getProperty (IDs::startBeats), audioTrack,
                                 (double) placed.getProperty (IDs::lengthBeats),
                                 (double) placed.getProperty (IDs::offsetBeats, 0.0) });
                clipsHeld = true;
            }
            updateSize();
            repaint();
            return;
        }

        int patternId = owner.getCurrentPatternId ? owner.getCurrentPatternId() : 0;
        auto pattern = Model::findPatternById (owner.project, patternId);
        if (! pattern.isValid())
            return;

        double patBeats = Model::patternLengthTicks (pattern) / (double) Model::PPQ;
        double drawBeats = owner.lastClipLenBeats > 0.0 ? owner.lastClipLenBeats : patBeats;
        int track = trackAtForDrop (e.getPosition().getY());
        double start = snapBeat (e.getPosition().getX() / owner.pxPerBeat);

        juce::ValueTree newClip (IDs::Clip);
        newClip.setProperty (IDs::uid, Model::newId(), nullptr);
        newClip.setProperty (IDs::type, "pattern", nullptr);
        newClip.setProperty (IDs::patternId, patternId, nullptr);
        newClip.setProperty (IDs::track, track, nullptr);
        newClip.setProperty (IDs::startBeats, start, nullptr);
        newClip.setProperty (IDs::lengthBeats, drawBeats, nullptr);
        Model::getPlaylist (owner.project).appendChild (newClip, nullptr);
        owner.growPatternToBeats (pattern, drawBeats);

        dragClip = newClip;
        selectOnly (newClip);
        transientSelection = true;
        lastPaintedClip = newClip;
        mode = Mode::move;
        mouseDownBeats = e.getPosition().getX() / owner.pxPerBeat;
        mouseDownTrack = track;
        dragInfos.clear();
        dragInfos.add ({ newClip, start, track });
        clipsHeld = true;
        updateSize();
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (e.getPosition().getY() < rulerHeight)
            return;
        auto clip = clipAt (e.getPosition());
        if (clip.isValid() && clip.getProperty (IDs::type).toString() == "audio")
        {
            if (owner.onOpenAudioClip)
                owner.onOpenAudioClip (clip);
            return;
        }
        openPatternClip (clip);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown())
            return;
        if (mode == Mode::seek)
        {
            seekTo (e.getPosition().getX());
            return;
        }

        if (mode == Mode::erase)
        {
            auto clip = clipAt (e.getPosition());
            if (clip.isValid())
                removeClip (clip);
            return;
        }

        if (mode == Mode::slice)
        {
            sliceBeat = snapBeat (e.getPosition().getX() / owner.pxPerBeat);
            sliceY1 = e.getPosition().getY();
            repaint();
            return;
        }

        if (mode == Mode::rubber)
        {
            int y0 = rubberFromRuler ? rulerHeight
                                     : juce::jmin (rubberAnchor.getY(), e.getPosition().getY());
            int y1 = rubberFromRuler ? getHeight()
                                     : juce::jmax (rubberAnchor.getY(), e.getPosition().getY());
            rubberRect = juce::Rectangle<int>::leftTopRightBottom (
                juce::jmin (rubberAnchor.getX(), e.getPosition().getX()),
                y0,
                juce::jmax (rubberAnchor.getX(), e.getPosition().getX()),
                y1);

            selection.clearQuick();
            auto playlist = Model::getPlaylist (owner.project);
            for (int i = 0; i < playlist.getNumChildren(); ++i)
            {
                auto clip = playlist.getChild (i);
                if (clip.hasType (IDs::Clip) && clipRect (clip).intersects (rubberRect))
                    selection.add (clip);
            }
            repaint();
            return;
        }

        if (mode == Mode::autoPoint)
        {
            if (! autoClip.isValid() || ! autoPointNode.isValid())
                return;
            auto body = autoBodyRect (autoClip);
            double v = juce::jlimit (0.0, 1.0,
                (body.getBottom() - e.getPosition().getY()) / (double) juce::jmax (1.0f, body.getHeight()));
            autoPointNode.setProperty (IDs::val, v, nullptr);

            if (! autoPointLocked)
            {
                auto pts = autoPoints (autoClip);
                int idx = pts.indexOf (autoPointNode);
                if (idx > 0)
                {
                    double clipStart = autoClip.getProperty (IDs::startBeats);
                    double offset = autoClip.getProperty (IDs::offsetBeats, 0.0);
                    double lengthBeats = juce::jmax (1.0e-9, (double) autoClip.getProperty (IDs::lengthBeats));

                    double lo = (double) pts[idx - 1].getProperty (IDs::pos);
                    double hi = idx < pts.size() - 1 ? (double) pts[idx + 1].getProperty (IDs::pos)
                                                     : offset + lengthBeats;

                    double absBeat = e.getPosition().getX() / owner.pxPerBeat;
                    double newPos = absBeat - clipStart + offset;
                    double snap = owner.getSnapBeats();
                    if (snap > 0.0 && ! e.mods.isAltDown())
                    {
                        double snapped = std::round (absBeat / snap) * snap - clipStart + offset;
                        if (snapped > lo && snapped < hi)
                            newPos = snapped;
                    }
                    newPos = juce::jlimit (lo + 1.0e-4, hi - 1.0e-4, newPos);
                    autoPointNode.setProperty (IDs::pos, newPos, nullptr);
                }
            }
            repaint();
            return;
        }

        if (mode == Mode::autoTension)
        {
            if (! autoPointNode.isValid())
                return;
            double dy = (double) (autoDragStartY - e.getPosition().getY());
            autoPointNode.setProperty (IDs::tension,
                juce::jlimit (-1.0, 1.0, autoTensionStart + dy / 120.0), nullptr);
            repaint();
            return;
        }

        if (! dragClip.isValid())
            return;

        if (mode == Mode::move)
        {
            double rawDelta = e.getPosition().getX() / owner.pxPerBeat - mouseDownBeats;
            double primaryStart = 0.0;
            for (auto& info : dragInfos)
                if (info.clip == dragClip)
                    primaryStart = info.startBeats;
            double delta = snapBeat (primaryStart + rawDelta) - primaryStart;

            int groupMin = owner.numTracks(), groupMax = 0;
            for (auto& info : dragInfos)
            {
                groupMin = juce::jmin (groupMin, info.track);
                groupMax = juce::jmax (groupMax, info.track);
            }
            int dTrack = trackAtForDrop (e.getPosition().getY()) - mouseDownTrack;
            dTrack = juce::jlimit (-groupMin,
                                   owner.numTracks() + Playlist::extraLanes - 1 - groupMax, dTrack);

            if (delta != 0.0 || dTrack != 0)
            {
                keepGroupSelection();
                moveDidDrag = true;
            }

            for (auto& info : dragInfos)
            {
                info.clip.setProperty (IDs::startBeats, juce::jmax (0.0, info.startBeats + delta), nullptr);
                info.clip.setProperty (IDs::track,
                                       juce::jlimit (0, owner.numTracks() + Playlist::extraLanes - 1,
                                                     info.track + dTrack), nullptr);
            }
        }
        else if (mode == Mode::resizeRight)
        {
            double snap = juce::jmax (0.25, owner.getSnapBeats());
            double primaryLen = dragClip.getProperty (IDs::lengthBeats);
            for (auto& info : dragInfos)
                if (info.clip == dragClip)
                    primaryLen = info.lengthBeats;
            double start = dragClip.getProperty (IDs::startBeats);
            double end = snapBeat (e.getPosition().getX() / owner.pxPerBeat + snap * 0.5);
            double len = juce::jmax (snap, end - start);
            double delta = len - primaryLen;
            owner.lastClipLenBeats = len;

            if (delta != 0.0)
                keepGroupSelection();

            for (auto& info : dragInfos)
            {
                double newLen = juce::jmax (snap, info.lengthBeats + delta);
                info.clip.setProperty (IDs::lengthBeats, newLen, nullptr);
                if (info.stretchSpanBeats > 0.0)
                    applyStretch (info, newLen);
                else if (info.clip.getProperty (IDs::type).toString() == "pattern")
                    owner.growPatternToBeats (Model::findPatternById (owner.project,
                                                  (int) info.clip.getProperty (IDs::patternId)), newLen);
            }
        }
        else if (mode == Mode::resizeLeft)
        {
            double snap = juce::jmax (0.25, owner.getSnapBeats());
            double primaryStart = dragClip.getProperty (IDs::startBeats);
            double primaryOff = dragClip.getProperty (IDs::offsetBeats, 0.0);
            double primaryEnd = primaryStart + (double) dragClip.getProperty (IDs::lengthBeats);
            bool primaryStretch = false;
            for (auto& info : dragInfos)
                if (info.clip == dragClip)
                {
                    primaryStart = info.startBeats;
                    primaryOff = info.offsetBeats;
                    primaryEnd = info.startBeats + info.lengthBeats;
                    primaryStretch = info.stretchSpanBeats > 0.0;
                }
            double newStart = snapBeat (e.getPosition().getX() / owner.pxPerBeat);
            newStart = juce::jlimit (primaryStretch ? 0.0 : juce::jmax (0.0, primaryStart - primaryOff),
                                     primaryEnd - snap, newStart);
            double delta = newStart - primaryStart;

            if (delta != 0.0)
                keepGroupSelection();

            for (auto& info : dragInfos)
            {
                const bool stretch = info.stretchSpanBeats > 0.0;
                double d = juce::jlimit (stretch ? -info.startBeats
                                                 : juce::jmax (-info.offsetBeats, -info.startBeats),
                                         info.lengthBeats - snap, delta);
                double newLen = info.lengthBeats - d;
                info.clip.setProperty (IDs::startBeats, info.startBeats + d, nullptr);
                info.clip.setProperty (IDs::lengthBeats, newLen, nullptr);
                if (stretch)
                    applyStretch (info, newLen);
                else
                    info.clip.setProperty (IDs::offsetBeats, info.offsetBeats + d, nullptr);
            }
        }
        updateSize();
        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown())
            return;

        clipsHeld = false;

        if (mode == Mode::autoPoint || mode == Mode::autoTension)
        {
            autoClip = {};
            autoPointNode = {};
            mode = Mode::none;
            repaint();
            return;
        }

        if (mode == Mode::slice)
            splitCrossedClips();

        if (mode == Mode::move && moveDidDrag)
            Model::ensurePlaylistTracks (owner.project);

        if (transientSelection)
        {
            selection.clearQuick();
            transientSelection = false;
        }

        dragClip = {};
        dragInfos.clear();
        mode = Mode::none;
        rubberRect = {};
        rubberFromRuler = false;
        repaint();
    }

    void splitCrossedClips()
    {
        int yTop = juce::jmin (sliceY0, sliceY1);
        int yBot = juce::jmax (sliceY0, sliceY1);
        auto playlist = Model::getPlaylist (owner.project);

        juce::Array<juce::ValueTree> toSplit;
        for (int i = 0; i < playlist.getNumChildren(); ++i)
        {
            auto clip = playlist.getChild (i);
            if (! clip.hasType (IDs::Clip))
                continue;
            if (clip.getProperty (IDs::type).toString() == "automation")
                continue;
            auto r = clipRect (clip);
            double start = clip.getProperty (IDs::startBeats);
            double len = clip.getProperty (IDs::lengthBeats);
            if (sliceBeat > start + 1.0e-6 && sliceBeat < start + len - 1.0e-6
                && yBot >= r.getY() && yTop <= r.getBottom())
                toSplit.add (clip);
        }

        for (auto& clip : toSplit)
        {
            if (! clip.hasProperty (IDs::uid))
                clip.setProperty (IDs::uid, Model::newId(), nullptr);

            double start = clip.getProperty (IDs::startBeats);
            double len = clip.getProperty (IDs::lengthBeats);
            double off = clip.getProperty (IDs::offsetBeats, 0.0);
            double leftLen = sliceBeat - start;

            auto right = clip.createCopy();
            right.setProperty (IDs::uid, Model::newId(), nullptr);
            right.setProperty (IDs::startBeats, sliceBeat, nullptr);
            right.setProperty (IDs::lengthBeats, len - leftLen, nullptr);
            right.setProperty (IDs::offsetBeats, off + leftLen, nullptr);
            playlist.appendChild (right, nullptr);

            clip.setProperty (IDs::lengthBeats, leftLen, nullptr);
        }
    }

    int resizeZoneAt (juce::Point<int> pos, const juce::ValueTree& clip) const
    {
        auto r = clipRect (clip);
        int edge = juce::jmin (resizeEdge, juce::jmax (3, r.getWidth() / 4));

        if (clipMenuZone (clip).contains (pos))
            return 0;

        if (pos.getX() > r.getRight() - edge)
            return 1;
        if (pos.getX() < r.getX() + edge)
            return -1;
        return 0;
    }

    void setHoveredMenuZone (juce::ValueTree clip)
    {
        if (clip == hoveredMenuZoneClip)
            return;
        auto previous = hoveredMenuZoneClip;
        hoveredMenuZoneClip = clip;
        if (previous.isValid())
            repaint (clipMenuZone (previous).expanded (2));
        if (clip.isValid())
            repaint (clipMenuZone (clip).expanded (2));
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        if (owner.tool == Tool::slice)
        {
            setHoveredMenuZone ({});
            setMouseCursor (juce::MouseCursor::CrosshairCursor);
            return;
        }
        auto clip = clipAt (e.getPosition());
        setHoveredMenuZone (clip.isValid() && clipMenuZone (clip).contains (e.getPosition())
                                ? clip : juce::ValueTree());
        bool onEdge = clip.isValid() && resizeZoneAt (e.getPosition(), clip) != 0;
        setMouseCursor (onEdge ? juce::MouseCursor::LeftRightResizeCursor
                               : juce::MouseCursor::NormalCursor);
    }

    void mouseExit (const juce::MouseEvent&) override { setHoveredMenuZone ({}); }

    void selectAll()
    {
        selection.clearQuick();
        auto playlist = Model::getPlaylist (owner.project);
        for (int i = 0; i < playlist.getNumChildren(); ++i)
            if (playlist.getChild (i).hasType (IDs::Clip))
                selection.add (playlist.getChild (i));
        repaint();
    }

    void deleteSelection()
    {
        auto doomed = selection;
        for (auto& clip : doomed)
            removeClip (clip);
        updateSize();
        repaint();
    }

    void moveSelectionTracks (int delta)
    {
        if (selection.isEmpty())
            return;

        int minTrack = owner.numTracks(), maxTrack = 0;
        for (auto& clip : selection)
        {
            int t = (int) clip.getProperty (IDs::track);
            minTrack = juce::jmin (minTrack, t);
            maxTrack = juce::jmax (maxTrack, t);
        }

        if (minTrack + delta < 0 || maxTrack + delta > owner.numTracks() - 1)
            return;

        for (auto& clip : selection)
            clip.setProperty (IDs::track, (int) clip.getProperty (IDs::track) + delta, nullptr);
        repaint();
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        if (e.mods.isAltDown())
        {
            vAnchorViewportY = e.getEventRelativeTo (owner.viewport.get()).getPosition().getY();
            int base = vZoom.isAnimating() ? (int) std::lround (vZoom.getTarget()) : owner.trackHeight;
            int newTarget = juce::jlimit (18, 80, base + (wheel.deltaY > 0 ? 3 : -3));
            vZoom.animateTo (owner.trackHeight, newTarget);
        }
        else if (e.mods.isCtrlDown())
        {
            hAnchorBeat = e.getPosition().getX() / owner.pxPerBeat;
            hAnchorViewX = e.getPosition().getX() - owner.viewport->getViewPositionX();
            double base = hZoom.isAnimating() ? hZoom.getTarget() : owner.pxPerBeat;
            double newTarget = juce::jlimit (owner.minZoomPxPerBeat(), maxPxPerBeat,
                                             base * (wheel.deltaY > 0 ? 1.2 : 1.0 / 1.2));
            hZoom.animateTo (owner.pxPerBeat, newTarget);
        }
        else
        {
            Component::mouseWheelMove (e, wheel);
        }
    }

    void seekTo (int x)
    {
        double beats = juce::jmax (0.0, x / owner.pxPerBeat);
        double snap = owner.getSnapBeats();
        if (snap > 0.0)
            beats = juce::jmax (0.0, std::round (beats / snap) * snap);
        seekPreviewBeats = beats;
        owner.engine.seekTicks (seekPreviewBeats * Model::PPQ);
        repaint();
    }

    static constexpr int rulerHeight = Playlist::rulerHeight;

    enum class Mode { none, move, resizeRight, resizeLeft, erase, seek, rubber, slice,
                      autoPoint, autoTension };
    static constexpr int resizeEdge = 8;
    static constexpr int clipHeaderHeight = 20;
    static constexpr float compactClipHeight = 26.0f;
    Playlist& owner;
    SmoothZoomer hZoom, vZoom;
    int vAnchorViewportY = 0;
    double hAnchorBeat = 0.0;
    int hAnchorViewX = 0;
    juce::ValueTree dragClip;
    juce::ValueTree lastPaintedClip;
    juce::Array<juce::ValueTree> selection;
    juce::Array<DragInfo> dragInfos;
    bool transientSelection = false;
    juce::ValueTree hoveredMenuZoneClip;
    bool moveDidDrag = false;
    bool clipsHeld = false;
    Mode mode = Mode::none;
    double mouseDownBeats = 0;
    int mouseDownTrack = 0;
    double seekPreviewBeats = 0;
    double sliceBeat = 0;
    int sliceY0 = 0, sliceY1 = 0;
    juce::Point<int> rubberAnchor;
    juce::Rectangle<int> rubberRect;
    bool rubberFromRuler = false;
    double stickyEndBeats = 0.0;
    int lastPlayheadPaintX = -1;
    bool playheadWrapped = false;

    struct Sparkle { float x, y, vy, life, maxLife, size; };
    std::vector<Sparkle> sparkles;
    juce::Random sparkleRng;
    juce::uint32 lastFxTickMs = 0;

    struct ClipGhost { juce::Rectangle<float> rect; juce::Colour colour; float alpha; };
    std::vector<ClipGhost> ghosts;
    juce::uint32 lastGhostTickMs = 0;

    juce::ValueTree autoClip;
    juce::ValueTree autoPointNode;
    bool autoPointLocked = false;
    double autoTensionStart = 0.0;
    int autoDragStartY = 0;
};

Playlist::Playlist (juce::ValueTree p, AudioEngine& e)
    : project (p), engine (e)
{
    Model::ensurePlaylistTracks (project);
    project.addListener (this);

    selectedPatternId = engine.getCurrentPatternId();

    addPatternBtn.setTooltip ("Add pattern");
    addPatternBtn.onClick = [this]
    {
        auto pat = Model::addPattern (project);
        refreshPatternBox();
        setSelectedPattern ((int) pat.getProperty (IDs::id));
        engine.setCurrentPatternId ((int) pat.getProperty (IDs::id));
        if (onPatternCreated)
            onPatternCreated ((int) pat.getProperty (IDs::id));
    };
    addAndMakeVisible (addPatternBtn);

    auto setupFilter = [this] (juce::TextButton& b, ResourceView view, const juce::String& tip)
    {
        b.setTooltip (tip);
        b.setClickingTogglesState (false);
        b.onClick = [this, view] { setResourceView (view); };
        addAndMakeVisible (b);
    };
    setupFilter (patternFilterBtn, ResourceView::patterns, "Show project patterns");
    setupFilter (audioFilterBtn,   ResourceView::audio,    "Show audio clips and sampler files used by this project");
    setupFilter (midiFilterBtn,    ResourceView::midi,     "Show MIDI files imported into this project");

    snapBox.addItem ("None", 1);
    snapBox.addItem ("1/6 Step", 2);
    snapBox.addItem ("1/4 Step", 3);
    snapBox.addItem ("1/3 Step", 4);
    snapBox.addItem ("1/2 Step", 5);
    snapBox.addItem ("Step", 6);
    snapBox.addItem ("1/6 Beat", 7);
    snapBox.addItem ("1/4 Beat", 8);
    snapBox.addItem ("1/3 Beat", 9);
    snapBox.addItem ("1/2 Beat", 10);
    snapBox.addItem ("Beat", 11);
    snapBox.addItem ("Bar", 12);
    snapBox.setSelectedId (11, juce::dontSendNotification);

    snapBtn.onClick = [this]
    {
        juce::PopupMenu m;
        for (int i = 1; i <= snapBox.getNumItems(); ++i)
            m.addItem (i, snapBox.getItemText (i - 1), true, snapBox.getSelectedId() == i);
        m.showMenuAsync (playlistMenuOptions (*this).withTargetComponent (&snapBtn),
            [this] (int r) { if (r > 0) { snapBox.setSelectedId (r, juce::dontSendNotification); if (canvas) canvas->repaint(); } });
    };
    toolStrip.addAndMakeVisible (snapBtn);

    drawBtn.activeColour = Colors::accent2;
    drawBtn.onClick = [this] { setTool (Tool::draw); };
    toolStrip.addAndMakeVisible (drawBtn);

    selectBtn.activeColour = Colors::toolSelect;
    selectBtn.onClick = [this] { setTool (Tool::select); };
    toolStrip.addAndMakeVisible (selectBtn);

    sliceBtn.activeColour = Colors::toolTrim;
    sliceBtn.onClick = [this] { setTool (Tool::slice); };
    toolStrip.addAndMakeVisible (sliceBtn);

    stretchBtn.activeColour = Colors::accent;
    stretchBtn.onClick = [this]
    {
        stretchEnabled = ! stretchEnabled;
        stretchBtn.setActive (stretchEnabled);
    };
    toolStrip.addAndMakeVisible (stretchBtn);

    toolStrip.setInterceptsMouseClicks (false, true);
    toolStrip.setSize (toolStripWidth, toolRowHeight);
    addAndMakeVisible (toolStrip);
    {
        constexpr int buttonWidth = 32;
        auto icons = juce::Rectangle<int> (0, 0, toolStripWidth, toolRowHeight);
        for (auto* b : { &snapBtn, &drawBtn, &selectBtn, &sliceBtn, &stretchBtn })
        {
            b->iconInset = 3.0f;
            b->pillBackground = true;
        }
        snapBtn.setBounds (icons.removeFromLeft (buttonWidth));
        icons.removeFromLeft (6);
        drawBtn.setBounds (icons.removeFromLeft (buttonWidth));
        icons.removeFromLeft (2);
        selectBtn.setBounds (icons.removeFromLeft (buttonWidth));
        icons.removeFromLeft (2);
        sliceBtn.setBounds (icons.removeFromLeft (buttonWidth));
        icons.removeFromLeft (6);
        stretchBtn.setBounds (icons.removeFromLeft (buttonWidth));
    }

    refreshPatternBox();
    setSelectedPattern (engine.getCurrentPatternId());

    canvas = std::make_unique<Canvas> (*this);
    viewport = std::make_unique<CanvasViewport> (*this);
    viewport->setViewedComponent (canvas.get(), false);
    viewport->setScrollBarsShown (true, false);
    addAndMakeVisible (*viewport);

    headers = std::make_unique<TrackHeaders> (*this);
    headerClip.addAndMakeVisible (headers.get());
    headerClip.setInterceptsMouseClicks (false, true);
    addAndMakeVisible (headerClip);

    patternPicker = std::make_unique<PatternPicker> (*this);
    patternPickerViewport.setViewedComponent (patternPicker.get(), false);
    patternPickerViewport.setScrollBarsShown (true, false);
    addAndMakeVisible (patternPickerViewport);

    zoomBar.onRangeChanged = [this] (double vStart, double vEnd)
    {
        if (canvas == nullptr || viewport == nullptr)
            return;
        canvas->stopZoomAnim();
        double span = juce::jmax (1.0e-6, vEnd - vStart);
        double visibleW = (double) viewport->getMaximumVisibleWidth();
        const juce::ScopedValueSetter<bool> barIsAuthoritative (suppressZoomBarSync, true);
        setHZoom (visibleW / span, vStart, 0);
    };
    zoomBar.onDragEnd = [this] { syncZoomBar(); };
    addAndMakeVisible (zoomBar);

    setTool (Tool::draw);
    setResourceView (ResourceView::patterns);
    prewarmAllThumbnails();
    startTimerHz (60);
}

Playlist::~Playlist()
{
    project.removeListener (this);
}

int Playlist::numTracks() const
{
    return Model::countPlaylistTracks (project);
}

void Playlist::repaintCanvas()
{
    if (canvas != nullptr)
        canvas->repaint();
}

void Playlist::growPatternToBeats (juce::ValueTree pattern, double beats)
{
    if (! pattern.isValid())
        return;
    int needSteps = (int) std::ceil (beats * Model::stepsPerBeat - 1.0e-6);
    if (needSteps > (int) pattern.getProperty (IDs::lengthSteps, Model::defaultSteps))
        pattern.setProperty (IDs::lengthSteps, needSteps, nullptr);
}

void Playlist::setTool (Tool t)
{
    tool = t;
    drawBtn.setActive (t == Tool::draw);
    selectBtn.setActive (t == Tool::select);
    sliceBtn.setActive (t == Tool::slice);
}

void Playlist::setResourceView (ResourceView v)
{
    resourceView = v;
    patternFilterBtn.setToggleState (v == ResourceView::patterns, juce::dontSendNotification);
    audioFilterBtn.setToggleState (v == ResourceView::audio, juce::dontSendNotification);
    midiFilterBtn.setToggleState (v == ResourceView::midi, juce::dontSendNotification);
    addPatternBtn.setVisible (v == ResourceView::patterns);

    if (patternPicker != nullptr)
    {
        patternPicker->updateSize();
        patternPicker->repaint();
    }
    patternPickerViewport.setViewPosition (0, 0);
}

void Playlist::setAudioBrush (const juce::String& path)
{
    audioBrushPath = path;
    audioBrushTemplate = {};
    if (path.isNotEmpty() && resourceView != ResourceView::audio)
        setResourceView (ResourceView::audio);

    if (patternPicker != nullptr)
    {
        patternPicker->selectedResourcePath = path;
        patternPicker->repaint();
    }
    repaintCanvas();
}

void Playlist::setAudioBrushFromClip (const juce::ValueTree& clip)
{
    setAudioBrush (clip.getProperty (IDs::path).toString());
    audioBrushTemplate = clip.createCopy();
}

void Playlist::clearAudioBrush()
{
    audioBrushPath.clear();
    audioBrushTemplate = {};
    repaintCanvas();
}

void Playlist::paintAudioBrush (const juce::File& f, int trackIndex, double startBeats)
{
    if (! audioBrushTemplate.isValid())
    {
        addAudioClip (f, trackIndex, startBeats);
        return;
    }

    Model::addAudioResource (project, f);

    auto clip = audioBrushTemplate.createCopy();
    clip.setProperty (IDs::uid, Model::newId(), nullptr);
    clip.setProperty (IDs::track,
                      juce::jlimit (0, numTracks() + extraLanes - 1, trackIndex), nullptr);
    clip.setProperty (IDs::startBeats, juce::jmax (0.0, startBeats), nullptr);
    clip.removeProperty (IDs::pending, nullptr);
    clip.removeProperty (IDs::recordedBy, nullptr);
    Model::getPlaylist (project).appendChild (clip, nullptr);
}

void Playlist::refreshResourcePanel()
{
    if (patternPicker != nullptr)
    {
        patternPicker->updateSize();
        patternPicker->repaint();
    }
    if (canvas != nullptr)
    {
        canvas->updateSize();
        canvas->repaint();
    }
    if (headers != nullptr)
    {
        headers->updateSize();
        headers->repaint();
    }
}

void Playlist::queueEnsureTracks()
{
    if (ensureTracksQueued)
        return;
    ensureTracksQueued = true;
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<Playlist> (this)]
    {
        if (safe != nullptr)
        {
            safe->ensureTracksQueued = false;
            Model::ensurePlaylistTracks (safe->project);
            safe->headers->updateSize();
            safe->headers->repaint();
            safe->canvas->updateSize();
            safe->syncScroll();
        }
    });
}

void Playlist::queueFullRefresh()
{
    trimCache.clear();
    if (fullRefreshQueued)
        return;
    fullRefreshQueued = true;
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<Playlist> (this)]
    {
        if (safe == nullptr)
            return;
        safe->fullRefreshQueued = false;
        safe->refreshPatternBox();
        safe->setSelectedPattern (safe->engine.getCurrentPatternId());
        Model::ensurePlaylistTracks (safe->project);
        safe->headers->updateSize();
        safe->headers->repaint();
        safe->patternPicker->updateSize();
        safe->patternPicker->repaint();
        safe->canvas->updateSize();
        safe->canvas->repaint();
        safe->syncScroll();
        safe->prewarmAllThumbnails();
    });
}

double Playlist::getSnapBeats() const
{
    const double step = 1.0 / Model::stepsPerBeat;
    switch (snapBox.getSelectedId())
    {
        case 1:  return 0.0;
        case 2:  return step / 6.0;
        case 3:  return step / 4.0;
        case 4:  return step / 3.0;
        case 5:  return step / 2.0;
        case 6:  return step;
        case 7:  return 1.0 / 6.0;
        case 8:  return 1.0 / 4.0;
        case 9:  return 1.0 / 3.0;
        case 10: return 1.0 / 2.0;
        case 11: return 1.0;
        case 12: return (double) Model::beatsPerBar;
        default: return 1.0;
    }
}

void Playlist::refreshPatternBox()
{
    auto patterns = Model::getPatterns (project);
    if (selectedPatternId > 0 && Model::findPatternById (project, selectedPatternId).isValid())
        return;

    if (patterns.getNumChildren() > 0)
    {
        selectedPatternId = (int) patterns.getChild (0).getProperty (IDs::id);
        engine.setCurrentPatternId (selectedPatternId);
    }
    else
    {
        selectedPatternId = 0;
        engine.setCurrentPatternId (0);
    }
}

void Playlist::setSelectedPattern (int patternId)
{
    if (selectedPatternId == patternId)
        return;
    selectedPatternId = patternId;
    if (canvas != nullptr)
        canvas->repaint();
    if (patternPicker != nullptr)
        patternPicker->repaint();
}

void Playlist::duplicateSelection()
{
    canvas->duplicateSelection();
}

void Playlist::selectAll()
{
    canvas->selectAll();
}

void Playlist::deleteSelection()
{
    canvas->deleteSelection();
}

void Playlist::moveSelectionTracks (int delta)
{
    canvas->moveSelectionTracks (delta);
}

void Playlist::splitPatternByChannel (int patId)
{
    auto pattern = Model::findPatternById (project, patId);
    if (! pattern.isValid())
        return;

    int patSteps = (int) pattern.getProperty (IDs::lengthSteps, Model::defaultSteps);
    double patBeats = patSteps / (double) Model::stepsPerBeat;

    juce::Array<juce::ValueTree> seqs;
    for (int i = 0; i < pattern.getNumChildren(); ++i)
    {
        auto child = pattern.getChild (i);
        if (! child.hasType (IDs::ChannelSeq))
            continue;
        if (child.getNumChildren() > 0
            || child.getProperty (IDs::steps).toString().containsChar ('1'))
            seqs.add (child);
    }

    if (seqs.size() < 2)
        return;

    auto playlist = Model::getPlaylist (project);

    juce::Array<int> newPatIds;
    for (auto& seq : seqs)
    {
        int chId = (int) seq.getProperty (IDs::channelId);
        auto channel = Model::findChannelById (project, chId);
        auto chName = channel.isValid() ? channel.getProperty (IDs::name).toString()
                                        : "Ch " + juce::String (chId);

        auto newPat = Model::addPattern (project, chName);
        newPat.setProperty (IDs::lengthSteps, patSteps, nullptr);
        newPat.appendChild (seq.createCopy(), nullptr);
        newPatIds.add ((int) newPat.getProperty (IDs::id));
    }

    struct Placement { double startBeats, lengthBeats; int track; };
    juce::Array<Placement> placements;
    for (int i = 0; i < playlist.getNumChildren(); ++i)
    {
        auto clip = playlist.getChild (i);
        if (clip.hasType (IDs::Clip) && clip.getProperty (IDs::type).toString() == "pattern"
            && (int) clip.getProperty (IDs::patternId) == patId)
            placements.add ({ (double) clip.getProperty (IDs::startBeats),
                              (double) clip.getProperty (IDs::lengthBeats),
                              (int)    clip.getProperty (IDs::track) });
    }
    if (placements.isEmpty())
        placements.add ({ 0.0, patBeats, 0 });

    auto trackIsFree = [&] (int track, double start, double len)
    {
        for (int i = 0; i < playlist.getNumChildren(); ++i)
        {
            auto clip = playlist.getChild (i);
            if (! clip.hasType (IDs::Clip) || (int) clip.getProperty (IDs::track) != track)
                continue;
            if (clip.getProperty (IDs::type).toString() == "pattern"
                && (int) clip.getProperty (IDs::patternId) == patId)
                continue;
            double s = (double) clip.getProperty (IDs::startBeats);
            double l = (double) clip.getProperty (IDs::lengthBeats);
            if (start < s + l && s < start + len)
                return false;
        }
        return true;
    };

    for (auto& p : placements)
    {
        int track = p.track;
        for (int newPatId : newPatIds)
        {
            while (! trackIsFree (track, p.startBeats, p.lengthBeats))
                ++track;

            juce::ValueTree clip (IDs::Clip);
            clip.setProperty (IDs::uid, Model::newId(), nullptr);
            clip.setProperty (IDs::type, "pattern", nullptr);
            clip.setProperty (IDs::patternId, newPatId, nullptr);
            clip.setProperty (IDs::track, track, nullptr);
            clip.setProperty (IDs::startBeats, p.startBeats, nullptr);
            clip.setProperty (IDs::lengthBeats, p.lengthBeats, nullptr);
            playlist.appendChild (clip, nullptr);
            ++track;
        }
    }

    int firstNewPatId = newPatIds.isEmpty() ? 0 : newPatIds.getFirst();

    for (int i = playlist.getNumChildren(); --i >= 0;)
    {
        auto clip = playlist.getChild (i);
        if (clip.hasType (IDs::Clip) && clip.getProperty (IDs::type).toString() == "pattern"
            && (int) clip.getProperty (IDs::patternId) == patId)
            playlist.removeChild (i, nullptr);
    }

    auto patterns = Model::getPatterns (project);
    for (int i = patterns.getNumChildren(); --i >= 0;)
        if ((int) patterns.getChild (i).getProperty (IDs::id) == patId)
            patterns.removeChild (i, nullptr);

    Model::ensurePlaylistTracks (project);

    if (firstNewPatId > 0)
    {
        setSelectedPattern (firstNewPatId);
        engine.setCurrentPatternId (firstNewPatId);
        if (onPatternSelected)
            onPatternSelected (firstNewPatId);
    }

    refreshPatternBox();
    patternPicker->updateSize();
    patternPicker->repaint();
}

void Playlist::makeClipUnique (juce::ValueTree clip)
{
    if (! clip.isValid() || clip.getProperty (IDs::type).toString() != "pattern")
        return;

    int patId = (int) clip.getProperty (IDs::patternId);
    auto pattern = Model::findPatternById (project, patId);
    if (! pattern.isValid())
        return;

    auto srcName = pattern.getProperty (IDs::name).toString();
    auto baseName = srcName;
    int copyIndex = 2;
    int hashPos = srcName.lastIndexOf (" #");
    if (hashPos > 0)
    {
        auto tail = srcName.substring (hashPos + 2);
        if (tail.isNotEmpty() && tail.containsOnly ("0123456789"))
        {
            baseName = srcName.substring (0, hashPos);
            copyIndex = tail.getIntValue() + 1;
        }
    }

    auto newPat = Model::addPattern (project, baseName + " #" + juce::String (copyIndex));
    newPat.setProperty (IDs::lengthSteps,
        pattern.getProperty (IDs::lengthSteps, Model::defaultSteps), nullptr);
    if (pattern.hasProperty (IDs::colour))
        newPat.setProperty (IDs::colour, pattern.getProperty (IDs::colour), nullptr);
    for (int i = 0; i < pattern.getNumChildren(); ++i)
        newPat.appendChild (pattern.getChild (i).createCopy(), nullptr);

    int newId = (int) newPat.getProperty (IDs::id);
    clip.setProperty (IDs::patternId, newId, nullptr);

    setSelectedPattern (newId);
    engine.setCurrentPatternId (newId);
    if (onPatternSelected)
        onPatternSelected (newId);

    refreshPatternBox();
    patternPicker->updateSize();
    patternPicker->repaint();
}

juce::AudioThumbnail* Playlist::getThumbnail (const juce::String& path)
{
    if (path.isEmpty())
        return nullptr;
    auto it = thumbnails.find (path);
    if (it != thumbnails.end())
        return it->second.get();

    juce::File f (path);
    if (! f.existsAsFile())
    {
        f = engine.resolveSampleFile (path);
        if (! f.existsAsFile())
            return nullptr;
    }

    auto thumb = std::make_unique<juce::AudioThumbnail> (256, engine.audioFormats, thumbCache);
    thumb->setSource (new juce::FileInputSource (f));
    auto* raw = thumb.get();
    thumbnails[path] = std::move (thumb);
    return raw;
}

void Playlist::prewarmThumbnail (const juce::ValueTree& clip)
{
    if (DebugFlags::disableWaveformThumbnails.load())
        return;
    if (! clip.hasType (IDs::Clip) || ! clip.getParent().isValid())
        return;
    if (clip.getProperty (IDs::type).toString() != "audio")
        return;
    auto path = clip.getProperty (IDs::path).toString();
    if (path.isEmpty() || path.startsWith ("builtin:"))
        return;
    getThumbnail (path);
}

void Playlist::prewarmAllThumbnails()
{
    if (DebugFlags::disableWaveformThumbnails.load())
        return;
    auto playlist = project.getChildWithName (IDs::Playlist);
    for (int i = 0; i < playlist.getNumChildren(); ++i)
        prewarmThumbnail (playlist.getChild (i));
}

bool Playlist::getClipTrimBounds (const juce::ValueTree& clip, const juce::String& path,
                                  AudioEngine::SampleTrimBounds& out)
{
    const int uid = clip.getProperty (IDs::uid, 0);
    if (uid != 0)
    {
        auto it = trimCache.find (uid);
        if (it != trimCache.end() && it->second.path == path)
        {
            out = it->second.bounds;
            return true;
        }
    }

    if (! engine.getSampleTrimBounds (path, clip, out))
        return false;

    if (uid != 0)
        trimCache[uid] = { path, out };
    return true;
}

void Playlist::addAudioClip (const juce::File& f, int trackIndex, double startBeats)
{
    Model::addAudioResource (project, f);

    double lengthBeats = 4.0;
    if (auto reader = std::unique_ptr<juce::AudioFormatReader> (engine.audioFormats.createReaderFor (f)))
    {
        double seconds = (double) reader->lengthInSamples / juce::jmax (1.0, reader->sampleRate);
        double bpm = (double) project.getProperty (IDs::bpm, 140.0);
        lengthBeats = juce::jmax (0.25, seconds * bpm / 60.0);
    }

    juce::ValueTree clip (IDs::Clip);
    clip.setProperty (IDs::uid, Model::newId(), nullptr);
    clip.setProperty (IDs::type, "audio", nullptr);
    clip.setProperty (IDs::path, f.getFullPathName(), nullptr);
    clip.setProperty (IDs::track,
                      juce::jlimit (0, numTracks() + extraLanes - 1, trackIndex), nullptr);
    clip.setProperty (IDs::startBeats, juce::jmax (0.0, startBeats), nullptr);
    clip.setProperty (IDs::lengthBeats, lengthBeats, nullptr);
    Model::getPlaylist (project).appendChild (clip, nullptr);

    auto path = f.getFullPathName();
    auto channels = Model::getChannels (project);
    bool hasChannel = false;
    for (int i = 0; i < channels.getNumChildren(); ++i)
    {
        auto ch = channels.getChild (i);
        if (ch.hasType (IDs::Channel) && ch.getProperty (IDs::samplePath).toString() == path)
        {
            hasChannel = true;
            break;
        }
    }
    if (! hasChannel)
        Model::addSamplerChannel (project, f.getFileNameWithoutExtension(), path);
}

juce::ValueTree Playlist::addRecordedClip (const juce::File& f, int trackIndex, double startBeats,
                                           double lengthBeats, bool pending, const juce::String& recordedBy)
{
    juce::ValueTree clip (IDs::Clip);
    clip.setProperty (IDs::uid, Model::newId(), nullptr);
    clip.setProperty (IDs::type, "audio", nullptr);
    clip.setProperty (IDs::path, f.getFullPathName(), nullptr);
    clip.setProperty (IDs::track, juce::jlimit (0, numTracks(), trackIndex), nullptr);
    clip.setProperty (IDs::startBeats, juce::jmax (0.0, startBeats), nullptr);
    clip.setProperty (IDs::lengthBeats, juce::jmax (0.25, lengthBeats), nullptr);
    if (pending)
    {
        clip.setProperty (IDs::pending, true, nullptr);
        if (recordedBy.isNotEmpty())
            clip.setProperty (IDs::recordedBy, recordedBy, nullptr);
    }
    Model::getPlaylist (project).appendChild (clip, nullptr);

    Model::addAudioResource (project, f);

    auto path = f.getFullPathName();
    auto channels = Model::getChannels (project);
    bool hasChannel = false;
    for (int i = 0; i < channels.getNumChildren(); ++i)
    {
        auto ch = channels.getChild (i);
        if (ch.hasType (IDs::Channel) && ch.getProperty (IDs::samplePath).toString() == path)
        {
            hasChannel = true;
            break;
        }
    }
    if (! hasChannel)
        Model::addSamplerChannel (project, f.getFileNameWithoutExtension(), path);
    return clip;
}

void Playlist::addMidiFileAsPatternClip (const juce::File& f, int trackIndex, double startBeats)
{
    if (! f.existsAsFile() || ! BrowserPanel::isMidiFile (f))
        return;

    Model::addMidiResource (project, f);

    auto channels = Model::getChannels (project);
    juce::ValueTree channel;
    for (int i = 0; i < channels.getNumChildren(); ++i)
    {
        auto ch = channels.getChild (i);
        if (ch.hasType (IDs::Channel))
        {
            channel = ch;
            break;
        }
    }
    if (! channel.isValid())
        channel = Model::addSamplerChannel (project, f.getFileNameWithoutExtension(), {});

    auto pattern = Model::addPattern (project, f.getFileNameWithoutExtension());
    auto seq = Model::getChannelSeq (pattern, (int) channel.getProperty (IDs::id), true);
    int imported = MidiImport::applyToChannelSeq (f, pattern, seq);
    if (imported <= 0)
    {
        pattern.getParent().removeChild (pattern, nullptr);
        Dialogs::showInfoAsync (juce::MessageBoxIconType::WarningIcon,
            "Import MIDI", "No notes found in " + f.getFileName());
        return;
    }

    double patBeats = Model::patternLengthTicks (pattern) / (double) Model::PPQ;
    double len = juce::jmax (0.25, patBeats);
    juce::ValueTree clip (IDs::Clip);
    clip.setProperty (IDs::uid, Model::newId(), nullptr);
    clip.setProperty (IDs::type, "pattern", nullptr);
    clip.setProperty (IDs::patternId, (int) pattern.getProperty (IDs::id), nullptr);
    clip.setProperty (IDs::track,
                      juce::jlimit (0, numTracks() + extraLanes - 1, trackIndex), nullptr);
    clip.setProperty (IDs::startBeats, juce::jmax (0.0, startBeats), nullptr);
    clip.setProperty (IDs::lengthBeats, len, nullptr);
    Model::getPlaylist (project).appendChild (clip, nullptr);

    setSelectedPattern ((int) pattern.getProperty (IDs::id));
    engine.setCurrentPatternId ((int) pattern.getProperty (IDs::id));
    if (onPatternCreated)
        onPatternCreated ((int) pattern.getProperty (IDs::id));
}

bool Playlist::isInterestedInDragSource (const SourceDetails& d)
{
    auto desc = d.description.toString();
    return desc == BrowserPanel::dragDescription || desc.startsWith ("playlist-pattern:")
        || desc.startsWith ("playlist-audio:") || desc.startsWith ("playlist-midi:");
}

void Playlist::itemDropped (const SourceDetails& d)
{
    auto posInCanvas = canvas->getLocalPoint (this, d.localPosition);

    auto desc = d.description.toString();
    if (desc.startsWith ("playlist-pattern:"))
    {
        int patternId = desc.fromFirstOccurrenceOf (":", false, false).getIntValue();
        canvas->placePatternClip (patternId, posInCanvas);
        return;
    }

    if (desc.startsWith ("playlist-audio:"))
    {
        juce::File f (desc.substring (15));
        if (f.existsAsFile() && BrowserPanel::isAudioFile (f))
        {
            int track = canvas->trackAtForDrop (posInCanvas.getY());
            addAudioClip (f, track, canvas->snapBeat (posInCanvas.getX() / pxPerBeat));
        }
        return;
    }

    if (desc.startsWith ("playlist-midi:"))
    {
        juce::File f (desc.substring (14));
        if (f.existsAsFile() && BrowserPanel::isMidiFile (f))
        {
            int track = canvas->trackAtForDrop (posInCanvas.getY());
            addMidiFileAsPatternClip (f, track, canvas->snapBeat (posInCanvas.getX() / pxPerBeat));
        }
        return;
    }

    if (getBrowserFile == nullptr)
        return;
    auto f = getBrowserFile();
    if (! f.existsAsFile())
        return;

    int track = canvas->trackAtForDrop (posInCanvas.getY());
    double beats = canvas->snapBeat (posInCanvas.getX() / pxPerBeat);
    if (BrowserPanel::isAudioFile (f))
        addAudioClip (f, track, beats);
    else if (BrowserPanel::isMidiFile (f))
        addMidiFileAsPatternClip (f, track, beats);
}

bool Playlist::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (BrowserPanel::isAudioFile (juce::File (f)) || BrowserPanel::isMidiFile (juce::File (f)))
            return true;
    return false;
}

void Playlist::filesDropped (const juce::StringArray& files, int x, int y)
{
    auto posInCanvas = canvas->getLocalPoint (this, juce::Point<int> (x, y));
    int track = canvas->trackAtForDrop (posInCanvas.getY());
    double beats = canvas->snapBeat (posInCanvas.getX() / pxPerBeat);
    for (auto& path : files)
    {
        juce::File f (path);
        if (BrowserPanel::isAudioFile (f))
        {
            addAudioClip (f, track, beats);
            ++track;
        }
        else if (BrowserPanel::isMidiFile (f))
        {
            addMidiFileAsPatternClip (f, track, beats);
            ++track;
        }
    }
}

void Playlist::syncScroll()
{
    if (headers != nullptr)
        headers->setTopLeftPosition (0, -viewport->getViewPositionY());
    syncZoomBar();
}

void Playlist::syncZoomBar()
{
    if (canvas == nullptr || viewport == nullptr || suppressZoomBarSync)
        return;
    canvas->updateSize();
    const double visibleW = (double) viewport->getMaximumVisibleWidth();
    double total = juce::jmax ((double) canvas->totalBeats(), visibleW / pxPerBeat);
    double viewStartBeat = viewport->getViewPositionX() / pxPerBeat;
    double viewEndBeat = (viewport->getViewPositionX() + visibleW) / pxPerBeat;
    zoomBar.setRange (total, viewStartBeat, viewEndBeat);
}

double Playlist::minZoomPxPerBeat() const
{
    if (viewport == nullptr || canvas == nullptr)
        return minPxPerBeat;
    double fit = (double) viewport->getMaximumVisibleWidth()
                    / juce::jmax (1.0, (double) canvas->totalBeats());
    return juce::jlimit (minPxPerBeat, maxPxPerBeat, fit);
}

void Playlist::updateZoomBarSpanLimits()
{
    if (viewport == nullptr)
        return;
    const double visibleW = (double) viewport->getMaximumVisibleWidth();
    const double minSpan = visibleW / maxPxPerBeat;
    double maxSpan = visibleW / minPxPerBeat;
    if (canvas != nullptr)
        maxSpan = juce::jmin (maxSpan, (double) canvas->totalBeats());
    zoomBar.setMaxSpan (juce::jmax (minSpan, maxSpan));
    zoomBar.setMinSpan (minSpan);
}

void Playlist::setHZoom (double newPxPerBeat, double anchorBeat, int anchorViewX)
{
    if (canvas == nullptr || viewport == nullptr)
        return;

    const bool wasSuppressed = suppressZoomBarSync;
    suppressZoomBarSync = true;

    pxPerBeat = juce::jlimit (minZoomPxPerBeat(), maxPxPerBeat, newPxPerBeat);
    canvas->updateSize();
    viewport->setViewPosition (
        juce::jmax (0, (int) std::lround (anchorBeat * pxPerBeat) - anchorViewX),
        viewport->getViewPositionY());

    suppressZoomBarSync = wasSuppressed;
    updateZoomBarSpanLimits();
    syncZoomBar();
    canvas->repaint();
}

void Playlist::timerCallback()
{
    bool playing = engine.isPlaying();
    if (canvas->ghostsAlive())
        canvas->tickGhosts();
    if (engine.isRecording())
    {
        canvas->animateClipNoteGlow();
        canvas->repaint();
    }
    else if (playing)
    {
        canvas->animateClipNoteGlow();
        canvas->repaintPlayhead();
    }
    else if (wasPlaying)
    {
        canvas->animateClipNoteGlow();
        canvas->resetPlayheadPaint();
        canvas->repaint();
    }
    else
    {
        if (canvas->playheadFxAlive())
            canvas->tickPlayheadFx();
        if (canvas->hasNoteGlowActivity())
            canvas->animateClipNoteGlow();
    }
    wasPlaying = playing;
}

static bool affectsSampleTrim (const juce::Identifier& p)
{
    return p == IDs::path || p == IDs::smpStart || p == IDs::smpLength || p == IDs::smpTrim
        || p == IDs::sampleKeepOnDisk
        || p == IDs::declickMode || p == IDs::fadeIn || p == IDs::fadeOut
        || p == IDs::fxRemoveDC || p == IDs::fxNormalize || p == IDs::fxReverse
        || p == IDs::fxRevPolarity || p == IDs::fxFadeStereo || p == IDs::fxSwapStereo;
}

void Playlist::valueTreePropertyChanged (juce::ValueTree& t, const juce::Identifier& p)
{
    if (t.hasType (IDs::Clip) && affectsSampleTrim (p))
        trimCache.erase ((int) t.getProperty (IDs::uid, 0));

    if (t.hasType (IDs::Clip) && (p == IDs::path || p == IDs::type))
        prewarmThumbnail (t);

    if (t.hasType (IDs::Project) && p == IDs::bpm)
        canvas->repaint();

    if (t.hasType (IDs::Clip) || t.hasType (IDs::Pattern) || t.hasType (IDs::Note)
        || t.hasType (IDs::ChannelSeq) || t.hasType (IDs::AudioFile) || t.hasType (IDs::Point))
    {
        ++treeGen;
        canvas->repaint();
    }
    if (t.hasType (IDs::Pattern) && (p == IDs::name || p == IDs::colour))
    {
        refreshPatternBox();
        patternPicker->repaint();
    }
    if ((t.hasType (IDs::Channel) && (p == IDs::samplePath || p == IDs::name))
        || (t.hasType (IDs::Clip) && (p == IDs::path || p == IDs::type))
        || t.hasType (IDs::AudioFile) || t.hasType (IDs::MidiFile))
    {
        patternPicker->updateSize();
        patternPicker->repaint();
    }
    if (t.hasType (IDs::PlaylistTrack))
    {
        headers->repaint();
        canvas->repaint();
    }
}

void Playlist::valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& child)
{
    ++treeGen;

    if (child.hasType (IDs::Playlist) || child.hasType (IDs::Patterns)
        || child.hasType (IDs::Channels) || child.hasType (IDs::AudioFiles)
        || child.hasType (IDs::MidiFiles))
    {
        queueFullRefresh();
    }
    else if (child.hasType (IDs::Clip))
    {
        prewarmThumbnail (child);
        canvas->updateSize();
        updateZoomBarSpanLimits();
        canvas->repaint();
        patternPicker->updateSize();
        patternPicker->repaint();
        queueEnsureTracks();
    }
    else if (child.hasType (IDs::Pattern) || child.hasType (IDs::Channel)
             || child.hasType (IDs::AudioFile) || child.hasType (IDs::MidiFile)
             || child.hasType (IDs::AudioFiles) || child.hasType (IDs::MidiFiles))
    {
        refreshPatternBox();
        patternPicker->updateSize();
        patternPicker->repaint();
    }
    else if (child.hasType (IDs::PlaylistTrack))
    {
        headers->updateSize();
        headers->repaint();
        canvas->updateSize();
        canvas->repaint();
    }
    else if (child.hasType (IDs::Note) || child.hasType (IDs::ChannelSeq)
             || child.hasType (IDs::Point))
    {
        canvas->repaint();
    }
}

void Playlist::valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& child, int)
{
    if (child.hasType (IDs::Clip))
        trimCache.erase ((int) child.getProperty (IDs::uid, 0));
    valueTreeChildAdded (project, child);
}

void Playlist::paint (juce::Graphics& g)
{
    g.fillAll (Colors::panelBg);

    const int zoomRowY = 4;
    g.setColour (Colors::header);
    g.fillRect (0, 0, getWidth(), zoomRowY + toolRowHeight + pickerGroupGap + rulerHeight);
}

void Playlist::resized()
{
    auto r = getLocalBounds().reduced (4);

    auto topRow = r.removeFromTop (toolRowHeight);
    auto toolArea = topRow.removeFromLeft (patternPickerWidth + headerWidth);
    toolStrip.setTopLeftPosition (toolArea.getX(), toolArea.getY());
    zoomBar.setBounds (topRow.withSizeKeepingCentre (topRow.getWidth(), zoomBarHeight));
    r.removeFromTop (pickerGroupGap);

    auto pickerCol = r.removeFromLeft (patternPickerWidth);
    auto filterRow = pickerCol.removeFromTop (rulerHeight).reduced (2, 1);
    int tabW = filterRow.getWidth() / 3;
    patternFilterBtn.setBounds (filterRow.removeFromLeft (tabW).reduced (1, 0));
    audioFilterBtn.setBounds (filterRow.removeFromLeft (tabW).reduced (1, 0));
    midiFilterBtn.setBounds (filterRow.reduced (1, 0));
    addPatternBtn.setBounds (pickerCol.removeFromBottom (22)
                                 .withTrimmedRight (patternPickerViewport.getScrollBarThickness())
                                 .reduced (4, 1));
    addPatternBtn.setVisible (resourceView == ResourceView::patterns);
    pickerCol.removeFromBottom (6);
    patternPickerViewport.setBounds (pickerCol);
    patternPicker->updateSize();

    auto headerCol = r.removeFromLeft (headerWidth);
    headerCol.removeFromTop (rulerHeight);
    headerClip.setBounds (headerCol);
    viewport->setBounds (r);

    canvas->updateSize();
    headers->updateSize();
    updateZoomBarSpanLimits();
    syncScroll();
}
