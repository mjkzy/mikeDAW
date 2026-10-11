#pragma once
#include <JuceHeader.h>
#include "../Engine/PluginHost.h"
#include "../Engine/Bridge32Host.h"
#include "AppLookAndFeel.h"
#include "Dialogs.h"

class PluginManagerContent : public juce::Component,
                             private juce::ListBoxModel
{
public:
    explicit PluginManagerContent (PluginHost& h) : host (h), locations (h)
    {
        scanBtn.onClick = [this] { startScan(); };
        addAndMakeVisible (scanBtn);

        rescanBtn.setTooltip ("Clear the plugin list and all scan caches, then scan every location from scratch");
        rescanBtn.onClick = [this]
        {
            if (host.isScanning())
                return;
            juce::Component::SafePointer<PluginManagerContent> safe (this);
            Dialogs::showOkCancelAsync (juce::MessageBoxIconType::QuestionIcon,
                "Rescan all plugins",
                "Clear the plugin list and re-scan every location from scratch?",
                "Rescan", "Cancel",
                [safe] (bool ok)
                {
                    if (safe != nullptr && ok)
                    {
                        safe->host.clearPluginList();
                        safe->refresh();
                        safe->startScan();
                    }
                });
        };
        addAndMakeVisible (rescanBtn);

        addFolderBtn.setTooltip ("Add a custom folder to the scan locations");
        addFolderBtn.onClick = [this]
        {
            chooser = std::make_unique<juce::FileChooser> ("Add plugin folder",
                juce::File::getSpecialLocation (juce::File::globalApplicationsDirectory));
            chooser->launchAsync (juce::FileBrowserComponent::openMode
                                  | juce::FileBrowserComponent::canSelectDirectories,
                [this] (const juce::FileChooser& fc)
                {
                    if (fc.getResult().isDirectory())
                    {
                        host.addUserScanFolder (fc.getResult());
                        locations.rebuild();
                        resized();
                    }
                });
        };
        addAndMakeVisible (addFolderBtn);

        progressLabel.setColour (juce::Label::textColourId, Colors::textDim);
        progressLabel.setFont (juce::Font (juce::FontOptions (11.0f)));
        addAndMakeVisible (progressLabel);

        countLabel.setColour (juce::Label::textColourId, Colors::accent);
        countLabel.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        countLabel.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (countLabel);

        locationsHeader.setText ("SCAN LOCATIONS", juce::dontSendNotification);
        locationsHeader.setColour (juce::Label::textColourId, Colors::textDim);
        locationsHeader.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
        addAndMakeVisible (locationsHeader);

        locations.onRemoved = [this] { locations.rebuild(); resized(); };
        addAndMakeVisible (locations);

        list.setModel (this);
        list.setRowHeight (20);
        list.setColour (juce::ListBox::backgroundColourId, Colors::panelDark);
        addAndMakeVisible (list);

        refresh();
    }

    void refresh()
    {
        types = host.knownPlugins.getTypes();
        types.addArray (host.unverifiedPlugins.getTypes());
        std::sort (types.begin(), types.end(),
                   [] (const juce::PluginDescription& a, const juce::PluginDescription& b)
                   { return a.name.compareIgnoreCase (b.name) < 0; });

        auto unverified = host.unverifiedPlugins.getNumTypes();
        countLabel.setText (juce::String (types.size()) + " plugins"
                            + (unverified > 0 ? " (" + juce::String (unverified) + " ?)" : ""),
                            juce::dontSendNotification);
        locations.rebuild();
        list.updateContent();
        list.repaint();
    }

    void stopScan()
    {
        host.stopScan();
        scanBtn.setEnabled (true);
        rescanBtn.setEnabled (true);
    }

    int getNumRows() override { return types.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (! juce::isPositiveAndBelow (row, types.size()))
            return;
        const auto& d = types.getReference (row);

        if (selected)
            g.fillAll (Colors::accent.withAlpha (0.15f));
        else if (row % 2 == 0)
            g.fillAll (Colors::popupBg);

        bool fresh = host.newlyFound.contains (d.createIdentifierString());
        bool unverified = host.isUnverified (d);

        g.setColour (fresh ? Colors::accent2 : unverified ? Colors::text.withAlpha (0.7f)
                                                            : Colors::text);
        g.setFont (juce::Font (juce::FontOptions (12.0f)));
        g.drawText ((fresh ? "* " : "") + d.name, 8, 0, width * 45 / 100 - 10, height,
                    juce::Justification::centredLeft);

        const int colX = width * 45 / 100;
        juce::String fmt, arch;
        formatAndArch (d, fmt, arch);
        g.setColour (fresh ? Colors::accent2.withAlpha (0.8f) : Colors::textDim);
        g.drawText (fmt, colX, 0, 34, height, juce::Justification::centredLeft);
        g.setColour (arch == "x86" ? Colors::warning : Colors::textDim);
        g.drawText (arch, colX + 34, 0, 32, height, juce::Justification::centredLeft);
        g.setColour (fresh ? Colors::accent2.withAlpha (0.8f) : Colors::textDim);
        g.drawText (unverified ? "?" : d.manufacturerName,
                    colX + 68, 0, width - (colX + 72), height,
                    juce::Justification::centredLeft);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colors::panelBg);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);

        auto top = r.removeFromTop (26);
        scanBtn.setBounds (top.removeFromLeft (70));
        top.removeFromLeft (4);
        rescanBtn.setBounds (top.removeFromLeft (94));
        top.removeFromLeft (6);
        countLabel.setBounds (top.removeFromRight (150));
        progressLabel.setBounds (top);

        r.removeFromTop (6);
        auto header = r.removeFromTop (20);
        addFolderBtn.setBounds (header.removeFromRight (90));
        locationsHeader.setBounds (header);

        r.removeFromTop (2);
        locations.setBounds (r.removeFromTop (
            juce::jlimit (40, 170, locations.getNumRows() * 18 + 4)));

        r.removeFromTop (8);
        list.setBounds (r);
    }

private:
    static void formatAndArch (const juce::PluginDescription& d, juce::String& fmt, juce::String& arch)
    {
        if (Bridge32::isBridged (d))          { fmt = "VST2"; arch = "x86"; }
        else if (d.pluginFormatName == "VST") { fmt = "VST2"; arch = "x64"; }
        else                                  { fmt = d.pluginFormatName; arch = "x64"; }
    }

    class LocationsList : public juce::ListBox,
                          private juce::ListBoxModel
    {
    public:
        explicit LocationsList (PluginHost& h) : host (h)
        {
            setModel (this);
            setRowHeight (18);
            setColour (juce::ListBox::backgroundColourId, Colors::panelDark);
            rebuild();
        }

        std::function<void()> onRemoved;

        void rebuild()
        {
            rows.clear();
            for (auto& loc : host.getScanLocations())
                rows.add ({ loc, {}, false });
            for (auto& f : host.getUserScanFolders())
                rows.add ({ f + "   [user]", f, true });
            updateContent();
            repaint();
        }

        int getNumRows() override { return rows.size(); }

        void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool) override
        {
            if (! juce::isPositiveAndBelow (row, rows.size()))
                return;
            const auto& loc = rows.getReference (row);

            if (row % 2 == 0)
                g.fillAll (Colors::popupBg);

            g.setColour (loc.removable ? Colors::text : Colors::text.withAlpha (0.6f));
            g.setFont (juce::Font (juce::FontOptions (11.0f)));
            g.drawText (loc.text, 8, 0, width - (loc.removable ? 30 : 12), height,
                        juce::Justification::centredLeft);

            if (loc.removable)
            {
                g.setColour (Colors::textDim);
                g.drawText ("x", width - 22, 0, 16, height, juce::Justification::centred);
            }
        }

        void listBoxItemClicked (int row, const juce::MouseEvent& e) override
        {
            if (! juce::isPositiveAndBelow (row, rows.size()))
                return;
            const auto& loc = rows.getReference (row);
            if (loc.removable && e.getPosition().getX() > getWidth() - 30)
            {
                host.removeUserScanFolder (juce::File (loc.folder));
                if (onRemoved)
                    onRemoved();
            }
        }

        juce::String getTooltipForRow (int row) override
        {
            if (juce::isPositiveAndBelow (row, rows.size()) && rows.getReference (row).removable)
                return "Click the x to remove this folder from the scan";
            return {};
        }

    private:
        struct Row { juce::String text; juce::String folder; bool removable; };

        PluginHost& host;
        juce::Array<Row> rows;
    };

    void startScan()
    {
        if (host.isScanning())
            return;
        scanBtn.setEnabled (false);
        rescanBtn.setEnabled (false);
        progressLabel.setText ("Scanning...", juce::dontSendNotification);

        host.startScan (
            [safe = juce::Component::SafePointer<PluginManagerContent> (this)] (const juce::String& s)
            {
                if (safe != nullptr)
                {
                    safe->progressLabel.setText (s, juce::dontSendNotification);
                    if (safe->types.size() != safe->host.knownPlugins.getNumTypes()
                                              + safe->host.unverifiedPlugins.getNumTypes())
                        safe->refresh();
                }
            },
            [safe = juce::Component::SafePointer<PluginManagerContent> (this)] (int numFound)
            {
                if (safe != nullptr)
                {
                    safe->scanBtn.setEnabled (true);
                    safe->rescanBtn.setEnabled (true);
                    auto prefix = safe->host.lastScanCancelled
                        ? "Stopped"
                        : "Done";
                    auto& failures = safe->host.lastScanFailures;
                    auto text = prefix + juce::String (" in ")
                                + juce::String (safe->host.lastScanSeconds, 2)
                                + "s: " + juce::String (numFound)
                                + " plugins (" + juce::String (safe->host.newlyFound.size())
                                + " new"
                                + (failures.isEmpty() ? juce::String()
                                                      : ", " + juce::String (failures.size())
                                                        + " failed")
                                + (safe->host.lastScanBlacklisted > 0
                                       ? ", " + juce::String (safe->host.lastScanBlacklisted)
                                         + " blacklisted"
                                       : juce::String())
                                + ").";
                    auto tooltip = failures.isEmpty() ? juce::String()
                        : "Failed plugins (skipped from now on):\n" + failures.joinIntoString ("\n");
                    if (safe->host.lastScanBlacklisted > 0)
                        tooltip << (tooltip.isEmpty() ? "" : "\n\n")
                                << juce::String (safe->host.lastScanBlacklisted)
                                << " blacklisted plugin(s) skipped (a reinstall clears the "
                                   "blacklist automatically)";
                    safe->progressLabel.setText (text, juce::dontSendNotification);
                    safe->progressLabel.setTooltip (tooltip);
                    safe->refresh();

                    if (! failures.isEmpty())
                        Dialogs::showInfoAsync (juce::MessageBoxIconType::WarningIcon,
                            "Some plugins failed to scan",
                            juce::String (failures.size()) + " plugin(s) crashed or failed and "
                            "were ignored:\n\n"
                            + failures.joinIntoString ("\n"),
                            "OK", nullptr, safe.getComponent());
                }
            });
    }

    PluginHost& host;
    juce::Array<juce::PluginDescription> types;
    juce::TextButton scanBtn { "SCAN" };
    juce::TextButton rescanBtn { "RESCAN ALL" };
    juce::TextButton addFolderBtn { "+ FOLDER" };
    juce::Label progressLabel, countLabel, locationsHeader;
    LocationsList locations;
    juce::ListBox list;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginManagerContent)
};

class PluginManagerWindow : public juce::DocumentWindow
{
public:
    explicit PluginManagerWindow (PluginHost& h)
        : juce::DocumentWindow ("Plugin Manager", Colors::panelBg,
                                juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton)
    {
        setUsingNativeTitleBar (true);
        content = new PluginManagerContent (h);
        setResizable (true, false);
        setResizeLimits (480, 420, 10000, 10000);
        Dialogs::setContentFitted (*this, content, 620, 640);
    }

    void closeButtonPressed() override
    {
        if (content != nullptr)
            content->stopScan();
        setVisible (false);
    }

    PluginManagerContent* content = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginManagerWindow)
};
