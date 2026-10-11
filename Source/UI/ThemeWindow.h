#pragma once
#include <JuceHeader.h>
#include "AppLookAndFeel.h"
#include "ThemeManager.h"
#include "ValueKnob.h"

class ColourSwatch : public juce::Component,
                     private juce::ChangeListener
{
public:
    ColourSwatch (ThemeManager& t, ThemeManager::Slot s) : theme (t), slot (std::move (s)) {}

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        auto sw = r.removeFromTop (22).reduced (2).toFloat();

        g.setColour (theme.getBaseColour (slot.key));
        g.fillRoundedRectangle (sw, 4.0f);
        g.setColour (hovered ? Colors::accent : Colors::outline);
        g.drawRoundedRectangle (sw, 4.0f, hovered ? 1.5f : 1.0f);

        g.setColour (Colors::text);
        g.setFont (juce::Font (juce::FontOptions (10.5f)));
        g.drawFittedText (slot.displayName, r.reduced (1, 0), juce::Justification::centredTop, 2);
    }

    void mouseEnter (const juce::MouseEvent&) override { hovered = true;  repaint(); }
    void mouseExit  (const juce::MouseEvent&) override { hovered = false; repaint(); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown())
            return;
        auto sel = std::make_unique<juce::ColourSelector> (
            juce::ColourSelector::showColourAtTop
            | juce::ColourSelector::showSliders
            | juce::ColourSelector::showColourspace);
        sel->setCurrentColour (theme.getBaseColour (slot.key), juce::dontSendNotification);
        sel->setSize (260, 300);
        sel->addChangeListener (this);

        juce::CallOutBox::launchAsynchronously (std::move (sel),
            getScreenBounds(), nullptr);
    }

private:
    void changeListenerCallback (juce::ChangeBroadcaster* src) override
    {
        if (auto* cs = dynamic_cast<juce::ColourSelector*> (src))
            theme.setColour (slot.key, cs->getCurrentColour());
    }

    ThemeManager& theme;
    ThemeManager::Slot slot;
    bool hovered = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ColourSwatch)
};

class ColoursPanel : public juce::Component
{
public:
    explicit ColoursPanel (ThemeManager& t) : theme (t)
    {
        juce::String cur;
        for (auto& s : ThemeManager::registry())
        {
            if (s.group != cur)
            {
                cur = s.group;
                auto* l = new juce::Label();
                l->setText (cur, juce::dontSendNotification);
                l->setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
                l->setColour (juce::Label::textColourId, Colors::accent);
                addAndMakeVisible (l);
                headers.add (l);
                items.push_back ({ l, true });
            }

            auto* sw = new ColourSwatch (theme, s);
            addAndMakeVisible (sw);
            swatches.add (sw);
            items.push_back ({ sw, false });
        }
    }

    int computeLayout (int width)
    {
        const int cols     = juce::jmax (2, width / 130);
        const int swW      = width / cols;
        const int swH      = 44;
        const int headerH  = 24;

        int y = 4, col = 0;
        for (auto& it : items)
        {
            if (it.header)
            {
                if (col != 0) { y += swH; col = 0; }
                it.comp->setBounds (2, y, width - 4, headerH);
                y += headerH;
            }
            else
            {
                it.comp->setBounds (col * swW, y, swW, swH);
                if (++col == cols) { col = 0; y += swH; }
            }
        }
        if (col != 0) y += swH;
        return y + 4;
    }

    void applyThemeColours()
    {
        for (auto* l : headers)
            l->setColour (juce::Label::textColourId, Colors::accent);
        repaint();
    }

private:
    struct Item { juce::Component* comp; bool header; };

    ThemeManager& theme;
    juce::OwnedArray<juce::Label> headers;
    juce::OwnedArray<ColourSwatch> swatches;
    std::vector<Item> items;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ColoursPanel)
};

class ThemeContent : public juce::Component,
                     private juce::ChangeListener
{
public:
    explicit ThemeContent (ThemeManager& t) : theme (t)
    {
        auto styleHeader = [this] (juce::Label& l, const juce::String& txt)
        {
            l.setText (txt, juce::dontSendNotification);
            l.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
            addAndMakeVisible (l);
        };
        styleHeader (presetsHeader, "Presets");
        styleHeader (adjustHeader,  "Adjustments");
        styleHeader (effectsHeader, "Effects");
        styleHeader (coloursHeader, "Colors");

        buildPresetButtons();

        auto adj = theme.getAdjustments();
        setupKnob (hueKnob, hueLbl, "Hue",        -180.0, 180.0, 0.0, adj.hue);
        setupKnob (satKnob, satLbl, "Saturation",    0.0,   2.0, 1.0, adj.saturation);
        setupKnob (briKnob, briLbl, "Brightness",    0.0,   2.0, 1.0, adj.brightness);
        setupKnob (conKnob, conLbl, "Contrast",      0.0,   2.0, 1.0, adj.contrast);
        setupKnob (textKnob,textLbl,"Text",          0.0,   2.0, 1.0, adj.text);

        setupEffectSlider (pianoGlowSlider,    pianoGlowLbl,    "Piano roll glow",         "pianoGlowIntensity");
        setupEffectSlider (pianoOpacitySlider, pianoOpacityLbl, "Piano roll glow opacity", "pianoGlowOpacity");
        setupEffectSlider (playlistGlowSlider, playlistGlowLbl, "Playlist playhead glow",  "playlistGlowIntensity");

        glowEnableToggle.setButtonText ("Enable glow effects");
        glowEnableToggle.setToggleState (theme.isGlowEnabled(), juce::dontSendNotification);
        glowEnableToggle.onClick = [this] { theme.setGlowEnabled (glowEnableToggle.getToggleState()); };
        addAndMakeVisible (glowEnableToggle);

        coloursViewport.setViewedComponent (&coloursPanel, false);
        coloursViewport.setScrollBarsShown (true, false);
        addAndMakeVisible (coloursViewport);

        savePresetBtn.setButtonText ("Save preset...");
        savePresetBtn.onClick = [this] { promptSavePreset(); };
        addAndMakeVisible (savePresetBtn);

        importBtn.setButtonText ("Import...");
        importBtn.onClick = [this] { importTheme(); };
        addAndMakeVisible (importBtn);

        exportBtn.setButtonText ("Export...");
        exportBtn.onClick = [this] { exportTheme(); };
        addAndMakeVisible (exportBtn);

        resetBtn.setButtonText ("Reset");
        resetBtn.onClick = [this] { theme.resetToDefault(); };
        addAndMakeVisible (resetBtn);

        theme.addChangeListener (this);
        setSize (512, 908);
    }

    ~ThemeContent() override
    {
        theme.removeChangeListener (this);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colors::panelBg);

        if (! coloursBoxBounds.isEmpty())
        {
            auto b = coloursBoxBounds.toFloat();
            g.setColour (Colors::panelDark);
            g.fillRoundedRectangle (b, 6.0f);
            g.setColour (Colors::outline);
            g.drawRoundedRectangle (b, 6.0f, 1.0f);
        }
    }

    void lookAndFeelChanged() override { applyThemeStyling(); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);

        presetsHeader.setBounds (r.removeFromTop (20));
        layoutPresetButtons (r.removeFromTop (58));
        r.removeFromTop (6);

        adjustHeader.setBounds (r.removeFromTop (20));
        {
            auto row = r.removeFromTop (92);
            juce::Slider* knobs[] = { &hueKnob, &satKnob, &briKnob, &conKnob, &textKnob };
            juce::Label*  labs [] = { &hueLbl,  &satLbl,  &briLbl,  &conLbl,  &textLbl  };
            int w = row.getWidth() / 5;
            for (int i = 0; i < 5; ++i)
            {
                auto cell = row.removeFromLeft (w);
                labs[i]->setBounds (cell.removeFromTop (16));
                knobs[i]->setBounds (cell.withSizeKeepingCentre (juce::jmin (72, cell.getWidth()), cell.getHeight()));
            }
        }
        r.removeFromTop (6);

        effectsHeader.setBounds (r.removeFromTop (20));
        {
            juce::Slider* sliders[] = { &pianoGlowSlider, &pianoOpacitySlider, &playlistGlowSlider };
            juce::Label*  labs   [] = { &pianoGlowLbl,    &pianoOpacityLbl,    &playlistGlowLbl    };
            for (int i = 0; i < 3; ++i)
            {
                auto row = r.removeFromTop (22);
                labs[i]->setBounds (row.removeFromLeft (150));
                sliders[i]->setBounds (row);
            }
            glowEnableToggle.setBounds (r.removeFromTop (24));
        }
        r.removeFromTop (6);

        coloursHeader.setBounds (r.removeFromTop (20));

        auto buttonRow = r.removeFromBottom (30);
        r.removeFromBottom (8);
        coloursBoxBounds = r;
        coloursViewport.setBounds (r.reduced (4));

        int vw = coloursViewport.getWidth() - coloursViewport.getScrollBarThickness();
        int h  = coloursPanel.computeLayout (vw);
        coloursPanel.setSize (vw, h);

        auto btn = [&buttonRow] (juce::Button& b, int w) { b.setBounds (buttonRow.removeFromLeft (w)); buttonRow.removeFromLeft (6); };
        btn (savePresetBtn, 110);
        btn (importBtn, 84);
        btn (exportBtn, 84);
        resetBtn.setBounds (buttonRow.removeFromRight (70));
    }

private:
    void setupKnob (juce::Slider& k, juce::Label& lbl, const juce::String& name,
                    double lo, double hi, double def, double value)
    {
        lbl.setText (name, juce::dontSendNotification);
        lbl.setFont (juce::Font (juce::FontOptions (11.0f)));
        lbl.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (lbl);

        k.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        k.setRange (lo, hi);
        k.setValue (value, juce::dontSendNotification);
        k.setDoubleClickReturnValue (true, def);
        k.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 54, 15);
        k.onValueChange = [this] { pushAdjustments(); };
        addAndMakeVisible (k);
    }

    void setupEffectSlider (juce::Slider& s, juce::Label& lbl, const juce::String& name,
                            const juce::String& key)
    {
        lbl.setText (name, juce::dontSendNotification);
        lbl.setFont (juce::Font (juce::FontOptions (11.0f)));
        lbl.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (lbl);

        s.setSliderStyle (juce::Slider::LinearHorizontal);
        s.setRange (0.0, 2.0);
        s.setValue (theme.getEffectValue (key), juce::dontSendNotification);
        s.setDoubleClickReturnValue (true, 1.0);
        s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 46, 16);
        s.onValueChange = [this, &s, key] { theme.setEffectValue (key, (float) s.getValue()); };
        addAndMakeVisible (s);
    }

    void pushAdjustments()
    {
        ThemeManager::Adjustments a;
        a.hue        = (float) hueKnob.getValue();
        a.saturation = (float) satKnob.getValue();
        a.brightness = (float) briKnob.getValue();
        a.contrast   = (float) conKnob.getValue();
        a.text       = (float) textKnob.getValue();
        theme.setAdjustments (a);
    }

    void buildPresetButtons()
    {
        presetButtons.clear();

        for (auto& p : ThemeManager::builtInPresets())
        {
            auto var = p.second;
            auto* b = new juce::TextButton (p.first);
            b->onClick = [this, var] { theme.applyPresetVar (var); };
            addAndMakeVisible (b);
            presetButtons.add (b);
        }

        for (auto& f : theme.getPresetDirectory().findChildFiles (juce::File::findFiles, false, "*.odawtheme"))
        {
            auto v = juce::JSON::parse (f);
            juce::String name = (v.isObject() && v.getProperty ("name", {}).toString().isNotEmpty())
                                    ? v.getProperty ("name", {}).toString()
                                    : f.getFileNameWithoutExtension();
            auto file = f;
            auto* b = new juce::TextButton (name);
            b->onClick = [this, file] { theme.importTheme (file); };
            addAndMakeVisible (b);
            presetButtons.add (b);
        }
    }

    void layoutPresetButtons (juce::Rectangle<int> area)
    {
        const int cols = 4;
        const int gap  = 4;
        const int bh   = 24;
        int bw = (area.getWidth() - (cols - 1) * gap) / cols;
        int x = area.getX(), y = area.getY(), col = 0;
        for (auto* b : presetButtons)
        {
            b->setBounds (x, y, bw, bh);
            if (++col == cols) { col = 0; x = area.getX(); y += bh + gap; }
            else               { x += bw + gap; }
        }
    }

    void promptSavePreset()
    {
        saveDialog = std::make_unique<juce::AlertWindow> ("Save preset",
            "Name for this theme preset:", juce::MessageBoxIconType::NoIcon);
        saveDialog->addTextEditor ("name", theme.getName());
        saveDialog->addButton ("Save",   1, juce::KeyPress (juce::KeyPress::returnKey));
        saveDialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        saveDialog->enterModalState (true, juce::ModalCallbackFunction::create ([this] (int r)
        {
            if (r == 1)
            {
                auto name = saveDialog->getTextEditorContents ("name").trim();
                if (name.isNotEmpty())
                {
                    theme.setName (name);
                    auto dest = theme.getPresetDirectory()
                                    .getChildFile (juce::File::createLegalFileName (name) + ".odawtheme");
                    theme.exportTheme (dest);
                    buildPresetButtons();
                    resized();
                }
            }
            saveDialog.reset();
        }), false);
    }

    void importTheme()
    {
        chooser = std::make_unique<juce::FileChooser> ("Import theme",
            theme.getPresetDirectory(), "*.odawtheme");
        chooser->launchAsync (juce::FileBrowserComponent::openMode
                              | juce::FileBrowserComponent::canSelectFiles,
            [this] (const juce::FileChooser& fc)
            {
                auto f = fc.getResult();
                if (f.existsAsFile())
                    theme.importTheme (f);
            });
    }

    void exportTheme()
    {
        chooser = std::make_unique<juce::FileChooser> ("Export theme",
            theme.getPresetDirectory().getChildFile (
                juce::File::createLegalFileName (theme.getName()) + ".odawtheme"),
            "*.odawtheme");
        chooser->launchAsync (juce::FileBrowserComponent::saveMode
                              | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
            [this] (const juce::FileChooser& fc)
            {
                auto f = fc.getResult();
                if (f != juce::File())
                    theme.exportTheme (f.withFileExtension ("odawtheme"));
            });
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        auto a = theme.getAdjustments();
        hueKnob .setValue (a.hue,        juce::dontSendNotification);
        satKnob .setValue (a.saturation, juce::dontSendNotification);
        briKnob .setValue (a.brightness, juce::dontSendNotification);
        conKnob .setValue (a.contrast,   juce::dontSendNotification);
        textKnob.setValue (a.text,       juce::dontSendNotification);
        pianoGlowSlider   .setValue (theme.getEffectValue ("pianoGlowIntensity"),    juce::dontSendNotification);
        pianoOpacitySlider.setValue (theme.getEffectValue ("pianoGlowOpacity"),      juce::dontSendNotification);
        playlistGlowSlider.setValue (theme.getEffectValue ("playlistGlowIntensity"), juce::dontSendNotification);
        glowEnableToggle.setToggleState (theme.isGlowEnabled(), juce::dontSendNotification);
        applyThemeStyling();
    }

    void applyThemeStyling()
    {
        coloursPanel.applyThemeColours();
        repaint();
    }

    ThemeManager& theme;

    juce::Label presetsHeader, adjustHeader, effectsHeader, coloursHeader;
    juce::OwnedArray<juce::TextButton> presetButtons;

    ValueKnob hueKnob, satKnob, briKnob, conKnob, textKnob;
    juce::Label  hueLbl, satLbl, briLbl, conLbl, textLbl;

    ValueSlider pianoGlowSlider, pianoOpacitySlider, playlistGlowSlider;
    juce::Label  pianoGlowLbl, pianoOpacityLbl, playlistGlowLbl;
    juce::ToggleButton glowEnableToggle;

    juce::Viewport coloursViewport;
    ColoursPanel coloursPanel { theme };
    juce::Rectangle<int> coloursBoxBounds;

    juce::TextButton savePresetBtn, importBtn, exportBtn, resetBtn;

    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<juce::AlertWindow> saveDialog;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ThemeContent)
};
