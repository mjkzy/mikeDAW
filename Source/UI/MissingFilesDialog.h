#pragma once
#include <JuceHeader.h>
#include <set>
#include <vector>
#include <functional>
#include "../Model/ProjectModel.h"
#include "AppLookAndFeel.h"
#include "Dialogs.h"

namespace MissingFiles
{
    struct Entry
    {
        juce::String path;
        juce::String fileName;
        juce::String kind;
        bool resolved = false;
    };

    inline void forEachFileRef (juce::ValueTree project,
        const std::function<void (juce::ValueTree, const juce::Identifier&, const char*)>& visit)
    {
        auto channels = project.getChildWithName (IDs::Channels);
        for (int i = 0; i < channels.getNumChildren(); ++i)
            if (channels.getChild (i).hasType (IDs::Channel))
                visit (channels.getChild (i), IDs::samplePath, "Sample");

        auto audioFiles = project.getChildWithName (IDs::AudioFiles);
        for (int i = 0; i < audioFiles.getNumChildren(); ++i)
            if (audioFiles.getChild (i).hasType (IDs::AudioFile))
                visit (audioFiles.getChild (i), IDs::audioPath, "Audio");

        auto midiFiles = project.getChildWithName (IDs::MidiFiles);
        for (int i = 0; i < midiFiles.getNumChildren(); ++i)
            if (midiFiles.getChild (i).hasType (IDs::MidiFile))
                visit (midiFiles.getChild (i), IDs::midiPath, "MIDI");

        auto playlist = project.getChildWithName (IDs::Playlist);
        for (int i = 0; i < playlist.getNumChildren(); ++i)
        {
            auto clip = playlist.getChild (i);
            if (clip.hasType (IDs::Clip)
                && clip.getProperty (IDs::type).toString() == "audio"
                && ! (bool) clip.getProperty (IDs::pending, false))
                visit (clip, IDs::path, "Audio");
        }
    }

    inline std::vector<Entry> scan (juce::ValueTree project,
        const std::function<juce::File (const juce::String&)>& resolve = {})
    {
        std::vector<Entry> out;
        std::set<juce::String> seen;
        forEachFileRef (project, [&] (juce::ValueTree node, const juce::Identifier& prop, const char* kind)
        {
            auto path = node.getProperty (prop).toString();
            if (path.isEmpty() || path.startsWith ("builtin:"))
                return;
            if (juce::File (path).existsAsFile())
                return;
            if (resolve != nullptr && resolve (path).existsAsFile())
                return;
            if (! seen.insert (path).second)
                return;
            Entry e;
            e.path = path;
            e.fileName = juce::File (path).getFileName();
            e.kind = kind;
            out.push_back (std::move (e));
        });
        return out;
    }
}

class MissingFilesContent : public juce::Component
{
public:
    MissingFilesContent (juce::ValueTree projectTree, std::vector<MissingFiles::Entry> missing,
                         std::function<void()> onAllResolvedFn)
        : project (projectTree), entries (std::move (missing)),
          onAllResolved (std::move (onAllResolvedFn))
    {
        title.setText ("Missing files", juce::dontSendNotification);
        title.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
        addAndMakeVisible (title);

        hint.setText ("These referenced files couldn't be found. Locate one and I'll look "
                      "for the rest in the same folder.", juce::dontSendNotification);
        hint.setFont (juce::Font (juce::FontOptions (11.0f)));
        hint.setColour (juce::Label::textColourId, Colors::textDim);
        addAndMakeVisible (hint);

        for (int i = 0; i < (int) entries.size(); ++i)
        {
            auto* row = new Row (*this, i);
            rows.add (row);
            addAndMakeVisible (row);
        }

        searchBtn.setButtonText ("Search folder...");
        searchBtn.onClick = [this] { chooseSearchFolder(); };
        addAndMakeVisible (searchBtn);

        closeBtn.setButtonText ("Close");
        closeBtn.onClick = [this] { closeSelf(); };
        addAndMakeVisible (closeBtn);

        setSize (560, 66 + (int) entries.size() * rowHeight + 48);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        title.setBounds (r.removeFromTop (24));
        hint.setBounds (r.removeFromTop (30));
        r.removeFromTop (6);

        auto bottom = r.removeFromBottom (30);
        closeBtn.setBounds (bottom.removeFromRight (90));
        bottom.removeFromRight (8);
        searchBtn.setBounds (bottom.removeFromRight (130));
        r.removeFromBottom (8);

        for (auto* row : rows)
            row->setBounds (r.removeFromTop (rowHeight));
    }

    void paint (juce::Graphics& g) override { g.fillAll (Colors::panelBg); }

private:
    static constexpr int rowHeight = 46;

    struct Row : public juce::Component
    {
        Row (MissingFilesContent& o, int idx) : owner (o), index (idx)
        {
            locateBtn.setButtonText ("Locate...");
            locateBtn.onClick = [this] { owner.locate (index); };
            addAndMakeVisible (locateBtn);
        }

        void refresh()
        {
            locateBtn.setVisible (! owner.entries[(size_t) index].resolved);
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            const auto& e = owner.entries[(size_t) index];
            auto r = getLocalBounds().reduced (0, 3);

            auto tag = r.removeFromLeft (54);
            g.setColour (Colors::raised);
            g.fillRoundedRectangle (tag.reduced (0, 12).toFloat(), 3.0f);
            g.setColour (e.resolved ? Colors::success : Colors::textDim);
            g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
            g.drawText (e.kind, tag, juce::Justification::centred);

            r.removeFromLeft (8);
            r.removeFromRight (100);

            g.setColour (e.resolved ? Colors::textDim : Colors::text);
            g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
            g.drawText (e.fileName, r.removeFromTop (r.getHeight() / 2),
                        juce::Justification::bottomLeft, true);

            g.setColour (Colors::textDim);
            g.setFont (juce::Font (juce::FontOptions (10.5f)));
            g.drawText (e.resolved ? juce::String ("Relinked") : e.path, r,
                        juce::Justification::topLeft, true);

            if (e.resolved)
            {
                auto a = juce::Rectangle<float> (getWidth() - 92.0f, getHeight() * 0.5f - 8.0f, 16.0f, 16.0f);
                g.setColour (Colors::success);
                juce::Path tick;
                tick.startNewSubPath (a.getX(), a.getCentreY());
                tick.lineTo (a.getX() + a.getWidth() * 0.38f, a.getBottom());
                tick.lineTo (a.getRight(), a.getY());
                g.strokePath (tick, juce::PathStrokeType (2.0f));
            }
        }

        void resized() override
        {
            locateBtn.setBounds (getWidth() - 92, (getHeight() - 26) / 2, 84, 26);
        }

        MissingFilesContent& owner;
        int index;
        juce::TextButton locateBtn;
    };

    void relinkPath (const juce::String& oldPath, const juce::String& newPath)
    {
        MissingFiles::forEachFileRef (project, [&] (juce::ValueTree node, const juce::Identifier& prop, const char*)
        {
            if (node.getProperty (prop).toString() == oldPath)
                node.setProperty (prop, newPath, nullptr);
        });
    }

    void resolveFromDirectory (const juce::File& dir)
    {
        if (! dir.isDirectory())
            return;
        auto files = dir.findChildFiles (juce::File::findFiles, true);
        for (auto& e : entries)
        {
            if (e.resolved)
                continue;
            for (auto& f : files)
                if (f.getFileName().equalsIgnoreCase (e.fileName))
                {
                    relinkPath (e.path, f.getFullPathName());
                    e.resolved = true;
                    break;
                }
        }
        refreshRows();
    }

    void locate (int index)
    {
        auto& e = entries[(size_t) index];
        auto ext = juce::File (e.fileName).getFileExtension();
        auto filter = ext.isEmpty() ? juce::String ("*") : "*" + ext;

        fileChooser = std::make_unique<juce::FileChooser> ("Locate " + e.fileName,
            juce::File(), filter);
        fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                                  | juce::FileBrowserComponent::canSelectFiles,
            [this, index] (const juce::FileChooser& fc)
            {
                auto f = fc.getResult();
                if (! f.existsAsFile())
                    return;
                auto& picked = entries[(size_t) index];
                relinkPath (picked.path, f.getFullPathName());
                picked.resolved = true;
                resolveFromDirectory (f.getParentDirectory());
            });
    }

    void chooseSearchFolder()
    {
        fileChooser = std::make_unique<juce::FileChooser> ("Search folder for missing files",
            juce::File());
        fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                                  | juce::FileBrowserComponent::canSelectDirectories,
            [this] (const juce::FileChooser& fc)
            {
                auto dir = fc.getResult();
                if (dir.isDirectory())
                    resolveFromDirectory (dir);
            });
    }

    void refreshRows()
    {
        for (auto* row : rows)
            row->refresh();

        bool allDone = true;
        for (auto& e : entries)
            allDone = allDone && e.resolved;
        if (allDone)
        {
            if (onAllResolved)
                onAllResolved();
            closeSelf();
        }
    }

    void closeSelf()
    {
        if (auto* w = getTopLevelComponent())
            w->setVisible (false);
    }

    juce::ValueTree project;
    std::vector<MissingFiles::Entry> entries;
    std::function<void()> onAllResolved;

    juce::Label title, hint;
    juce::OwnedArray<Row> rows;
    juce::TextButton searchBtn, closeBtn;
    std::unique_ptr<juce::FileChooser> fileChooser;

    friend struct Row;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MissingFilesContent)
};

class MissingFilesWindow : public juce::DocumentWindow
{
public:
    MissingFilesWindow (juce::ValueTree project, std::vector<MissingFiles::Entry> missing,
                        std::function<void()> onAllResolved)
        : juce::DocumentWindow ("Missing files", Colors::panelBg,
                                juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        content = new MissingFilesContent (project, std::move (missing), std::move (onAllResolved));
        Dialogs::setContentFitted (*this, content, content->getWidth(), content->getHeight());
        setResizable (false, false);
    }

    void closeButtonPressed() override { setVisible (false); }

    MissingFilesContent* content = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MissingFilesWindow)
};
