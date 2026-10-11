#pragma once
#include <JuceHeader.h>
#include "../Engine/PluginHost.h"
#include "../Engine/Bridge32Host.h"
#include "AppLookAndFeel.h"

class PluginPickerPopup : public juce::Component,
                          private juce::ListBoxModel,
                          private juce::TextEditor::Listener
{
public:
    PluginPickerPopup (juce::Array<juce::PluginDescription> plugins,
                       std::vector<std::pair<int, juce::String>> extraItems,
                       std::function<void (const juce::PluginDescription&)> onPick,
                       std::function<void (int)> onExtra,
                       std::function<bool (const juce::PluginDescription&)> isEnabled,
                       std::function<bool (const juce::PluginDescription&)> isNew)
        : allPlugins (std::move (plugins)),
          extras (std::move (extraItems)),
          pickCallback (std::move (onPick)),
          extraCallback (std::move (onExtra)),
          enabledPredicate (std::move (isEnabled)),
          newPredicate (std::move (isNew))
    {
        search.setTextToShowWhenEmpty ("Search plugins...", Colors::textDim);
        search.setColour (juce::TextEditor::backgroundColourId, Colors::panelDark);
        search.setColour (juce::TextEditor::textColourId, Colors::text);
        search.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        search.applyFontToAllText (Typography::ui (14.0f));
        search.setIndents (10, 4);
        search.addListener (this);
        search.onTextChange = [this] { applyFilter(); };
        addAndMakeVisible (search);

        list.setModel (this);
        list.setRowHeight (22);
        list.setColour (juce::ListBox::backgroundColourId, Colors::popupBg);
        list.addMouseListener (this, true);
        addAndMakeVisible (list);

        applyFilter();
        setSize (340, juce::jmin (420, 32 + (int) (extras.size() + allPlugins.size()) * 22 + 8));
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (Metrics::padS);
        search.setBounds (r.removeFromTop (24));
        r.removeFromTop (Metrics::padS);
        list.setBounds (r);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colors::popupBg);
    }

    void mouseEnter (const juce::MouseEvent& e) override { updateHover (e); }
    void mouseMove  (const juce::MouseEvent& e) override { updateHover (e); }
    void mouseDrag  (const juce::MouseEvent& e) override { updateHover (e); }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (! list.isMouseOver (true))
            setHoverRow (-1);
    }

    void visibilityChanged() override
    {
        if (isVisible())
        {
            search.grabKeyboardFocus();
            search.selectAll();
        }
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key.isKeyCode (juce::KeyPress::escapeKey))
        {
            dismiss();
            return true;
        }
        if (key.isKeyCode (juce::KeyPress::returnKey))
        {
            activateRow (list.getSelectedRow());
            return true;
        }
        if (key.isKeyCode (juce::KeyPress::downKey))
        {
            list.selectRow (juce::jmin (list.getSelectedRow() + 1, getNumRows() - 1));
            return true;
        }
        if (key.isKeyCode (juce::KeyPress::upKey))
        {
            list.selectRow (juce::jmax (0, list.getSelectedRow() - 1));
            return true;
        }
        return false;
    }

    static void show (juce::Component& target,
                      juce::Array<juce::PluginDescription> plugins,
                      std::vector<std::pair<int, juce::String>> extraItems,
                      std::function<void (const juce::PluginDescription&)> onPick,
                      std::function<void (int)> onExtra = {},
                      std::function<bool (const juce::PluginDescription&)> isEnabled = {},
                      std::function<bool (const juce::PluginDescription&)> isNew = {})
    {
        auto* content = new PluginPickerPopup (std::move (plugins), std::move (extraItems),
                                                std::move (onPick), std::move (onExtra),
                                                std::move (isEnabled), std::move (isNew));
        auto& box = juce::CallOutBox::launchAsynchronously (
            std::unique_ptr<juce::Component> (content),
            target.getScreenBounds(), nullptr);
        box.setLookAndFeel (&target.getLookAndFeel());
    }

private:
    static void formatAndArch (const juce::PluginDescription& d, juce::String& fmt, juce::String& arch)
    {
        if (Bridge32::isBridged (d))          { fmt = "VST2"; arch = "x86"; }
        else if (d.pluginFormatName == "VST") { fmt = "VST2"; arch = "x64"; }
        else                                  { fmt = d.pluginFormatName; arch = "x64"; }
    }

    void applyFilter()
    {
        auto query = search.getText().trim().toLowerCase();
        hoverRow = -1;
        filtered.clear();
        filteredExtras.clear();

        for (auto& ex : extras)
            if (query.isEmpty() || ex.second.toLowerCase().contains (query))
                filteredExtras.push_back (ex);

        for (auto& p : allPlugins)
        {
            juce::String fmt, arch;
            formatAndArch (p, fmt, arch);
            if (query.isEmpty() || p.name.toLowerCase().contains (query)
                || p.manufacturerName.toLowerCase().contains (query)
                || (fmt + " " + arch).toLowerCase().contains (query))
                filtered.add (p);
        }

        list.updateContent();
        list.repaint();
        if (getNumRows() > 0)
            list.selectRow (0);
    }

    int getNumRows() override
    {
        return (int) filteredExtras.size() + filtered.size();
    }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        auto pill = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (2.0f, 1.0f);
        if (selected)
        {
            g.setColour (Colors::accent.withAlpha (0.16f));
            g.fillRoundedRectangle (pill, Metrics::rS);
        }
        else if (row == hoverRow)
        {
            g.setColour (juce::Colours::white.withAlpha (0.04f));
            g.fillRoundedRectangle (pill, Metrics::rS);
        }

        int extraCount = (int) filteredExtras.size();
        if (row < extraCount)
        {
            g.setColour (Colors::text);
            g.setFont (Typography::ui (15.0f));
            g.drawText (filteredExtras[(size_t) row].second, 10, 0, width - 14, height,
                        juce::Justification::centredLeft);
        }
        else
        {
            int pi = row - extraCount;
            if (! juce::isPositiveAndBelow (pi, filtered.size()))
                return;
            auto& d = filtered.getReference (pi);
            const bool enabled = isRowEnabled (d);
            const bool fresh = enabled && isRowNew (d);

            int nameX = 10;
            if (! enabled)
            {
                const float ts = 9.0f;
                const float ty = (height - ts) * 0.5f;
                juce::Path tri;
                tri.addTriangle (4.0f + ts * 0.5f, ty,
                                 4.0f, ty + ts,
                                 4.0f + ts, ty + ts);
                g.setColour (Colors::warning.withAlpha (0.55f));
                g.fillPath (tri);
                g.setColour (Colors::popupBg);
                g.setFont (Typography::bold (7.0f));
                g.drawText ("!", juce::Rectangle<float> (4.0f, ty, ts, ts),
                            juce::Justification::centred);
                nameX = 10 + (int) ts + 4;
            }

            auto label = enabled ? d.name : d.name + "  (not available to all peers)";
            const int rightCol = width * 65 / 100;
            int nameW = rightCol - nameX - 2;

            if (fresh)
            {
                const int tagW = 32;
                juce::Rectangle<int> tag (rightCol - tagW - 4, (height - 13) / 2, tagW, 13);
                g.setColour (Colors::accent.withAlpha (0.22f));
                g.fillRoundedRectangle (tag.toFloat(), Metrics::rS);
                g.setColour (Colors::accent);
                g.setFont (Typography::caps (10.0f));
                g.drawText ("NEW", tag, juce::Justification::centred);
                nameW -= tagW + 4;
            }

            g.setColour (enabled ? Colors::text : Colors::textDim.withAlpha (0.5f));
            g.setFont (Typography::ui (15.0f));
            g.drawText (label, nameX, 0, nameW, height,
                        juce::Justification::centredLeft);
            g.setFont (Typography::ui (11.0f));

            juce::String fmt, arch;
            formatAndArch (d, fmt, arch);
            const int archW = 26;
            auto archCol = arch == "x86" ? Colors::warning : Colors::textDim;
            g.setColour (enabled ? archCol : archCol.withAlpha (0.45f));
            g.drawText (arch, width - archW - 6, 0, archW, height,
                        juce::Justification::centredRight);

            g.setColour (enabled ? Colors::textDim : Colors::textDim.withAlpha (0.45f));
            g.drawText (fmt, rightCol, 0, width - rightCol - 6 - archW - 2, height,
                        juce::Justification::centredRight);
        }
    }

    void updateHover (const juce::MouseEvent& e)
    {
        auto p = e.getEventRelativeTo (&list).getPosition();
        setHoverRow (list.getLocalBounds().contains (p) ? list.getRowContainingPosition (p.x, p.y) : -1);
    }

    void setHoverRow (int row)
    {
        if (row == hoverRow)
            return;
        hoverRow = row;
        list.repaint();
    }

    bool isRowEnabled (const juce::PluginDescription& d) const
    {
        return enabledPredicate == nullptr || enabledPredicate (d);
    }

    bool isRowNew (const juce::PluginDescription& d) const
    {
        return newPredicate != nullptr && newPredicate (d);
    }

    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override
    {
        activateRow (row);
    }

    void returnKeyPressed (int row) override
    {
        activateRow (row);
    }

    void activateRow (int row)
    {
        int extraCount = (int) filteredExtras.size();
        if (row < 0)
            return;
        if (row < extraCount)
        {
            if (extraCallback)
                extraCallback (filteredExtras[(size_t) row].first);
        }
        else
        {
            int pi = row - extraCount;
            if (! juce::isPositiveAndBelow (pi, filtered.size()))
                return;
            auto& d = filtered.getReference (pi);
            if (! isRowEnabled (d))
                return;
            if (pickCallback)
                pickCallback (d);
        }
        dismiss();
    }

    void dismiss()
    {
        if (auto* callout = findParentComponentOfClass<juce::CallOutBox>())
            callout->dismiss();
    }

    void textEditorReturnKeyPressed (juce::TextEditor&) override
    {
        activateRow (list.getSelectedRow());
    }

    void textEditorEscapeKeyPressed (juce::TextEditor&) override
    {
        dismiss();
    }

    juce::Array<juce::PluginDescription> allPlugins;
    juce::Array<juce::PluginDescription> filtered;
    std::vector<std::pair<int, juce::String>> extras;
    std::vector<std::pair<int, juce::String>> filteredExtras;
    std::function<void (const juce::PluginDescription&)> pickCallback;
    std::function<void (int)> extraCallback;
    std::function<bool (const juce::PluginDescription&)> enabledPredicate;
    std::function<bool (const juce::PluginDescription&)> newPredicate;

    juce::TextEditor search;
    juce::ListBox list;
    int hoverRow = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginPickerPopup)
};
