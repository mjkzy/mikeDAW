#pragma once
#include <JuceHeader.h>
#include "KeyBindings.h"
#include "AppLookAndFeel.h"
#include "Dialogs.h"

class KeyBindingsContent : public juce::Component
{
public:
    KeyBindingsContent (KeyBindings& kb, juce::PropertiesFile& props, std::function<void()> onApplied)
        : bindings (kb), settings (props), applied (std::move (onApplied))
    {
        staged = kb.all();

        title.setText ("Configure Keyboard Shortcuts", juce::dontSendNotification);
        title.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
        addAndMakeVisible (title);

        hint.setText ("Click a shortcut, then press the key combo. Esc clears it.",
                      juce::dontSendNotification);
        hint.setFont (juce::Font (juce::FontOptions (11.0f)));
        hint.setColour (juce::Label::textColourId, Colors::textDim);
        addAndMakeVisible (hint);

        for (size_t i = 0; i < staged.size(); ++i)
        {
            auto* row = new Row (*this, (int) i);
            rows.add (row);
            addAndMakeVisible (row);
        }

        applyBtn.setButtonText ("Apply");
        applyBtn.setColour (juce::TextButton::buttonColourId, Colors::success);
        applyBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::black);
        applyBtn.onClick = [this] { apply(); };
        addAndMakeVisible (applyBtn);

        resetBtn.setButtonText ("Reset to defaults");
        resetBtn.onClick = [this]
        {
            for (auto& e : staged) e.key = e.defaultKey;
            for (auto* r : rows) r->refresh();
        };
        addAndMakeVisible (resetBtn);

        setSize (440, 60 + (int) staged.size() * 30 + 50);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        title.setBounds (r.removeFromTop (24));
        hint.setBounds (r.removeFromTop (18));
        r.removeFromTop (6);

        auto bottom = r.removeFromBottom (30);
        applyBtn.setBounds (bottom.removeFromRight (90));
        bottom.removeFromRight (8);
        resetBtn.setBounds (bottom.removeFromRight (130));

        for (auto* row : rows)
            row->setBounds (r.removeFromTop (30));
    }

    void paint (juce::Graphics& g) override { g.fillAll (Colors::panelBg); }

private:
    struct Row : public juce::Component
    {
        Row (KeyBindingsContent& o, int idx) : owner (o), index (idx)
        {
            label.setText (owner.staged[(size_t) index].label, juce::dontSendNotification);
            addAndMakeVisible (label);

            keyBtn.onClick = [this] { startCapture(); };
            addAndMakeVisible (keyBtn);
            refresh();
        }

        void refresh()
        {
            auto& e = owner.staged[(size_t) index];
            keyBtn.setButtonText (capturing ? "Press a key..."
                                            : (e.key.isValid() ? e.key.getTextDescription() : "(none)"));
        }

        void startCapture()
        {
            capturing = true;
            refresh();
            keyBtn.setWantsKeyboardFocus (true);
            keyBtn.grabKeyboardFocus();
            keyBtn.addKeyListener (&owner.captureListener);
            owner.capturingRow = this;
        }

        void applyCaptured (const juce::KeyPress& kp)
        {
            capturing = false;
            keyBtn.removeKeyListener (&owner.captureListener);
            owner.capturingRow = nullptr;
            if (kp.isValid() && kp.getKeyCode() != juce::KeyPress::escapeKey)
                owner.staged[(size_t) index].key = kp;
            else if (kp.getKeyCode() == juce::KeyPress::escapeKey)
                owner.staged[(size_t) index].key = juce::KeyPress();
            refresh();
        }

        void paint (juce::Graphics& g) override
        {
            if (! owner.staged[(size_t) index].warn)
                return;
            auto a = juce::Rectangle<float> (2.0f, (getHeight() - 14.0f) * 0.5f, 14.0f, 14.0f);
            juce::Path tri;
            tri.addTriangle (a.getCentreX(), a.getY(),
                             a.getX(), a.getBottom(), a.getRight(), a.getBottom());
            g.setColour (Colors::warning);
            g.fillPath (tri);
            g.setColour (juce::Colours::black);
            g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
            g.drawText ("!", a.withTrimmedTop (2.0f), juce::Justification::centred);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (0, 2);
            r.removeFromLeft (owner.staged[(size_t) index].warn ? 20 : 0);
            label.setBounds (r.removeFromLeft (owner.staged[(size_t) index].warn ? 180 : 200));
            keyBtn.setBounds (r.removeFromLeft (200));
        }

        KeyBindingsContent& owner;
        int index;
        juce::Label label;
        juce::TextButton keyBtn;
        bool capturing = false;
    };

    struct CaptureListener : public juce::KeyListener
    {
        explicit CaptureListener (KeyBindingsContent& o) : owner (o) {}
        bool keyPressed (const juce::KeyPress& kp, juce::Component*) override
        {
            if (owner.capturingRow != nullptr)
                owner.capturingRow->applyCaptured (kp);
            return true;
        }
        KeyBindingsContent& owner;
    };

    void apply()
    {
        auto& live = bindings.all();
        for (size_t i = 0; i < live.size() && i < staged.size(); ++i)
            live[i].key = staged[i].key;
        bindings.save (settings);
        if (applied)
            applied();
    }

    KeyBindings& bindings;
    juce::PropertiesFile& settings;
    std::function<void()> applied;
    std::vector<KeyBindings::Entry> staged;

    juce::Label title, hint;
    juce::OwnedArray<Row> rows;
    juce::TextButton applyBtn, resetBtn;
    CaptureListener captureListener { *this };
    Row* capturingRow = nullptr;

    friend struct Row;
    friend struct CaptureListener;
};

class KeyBindingsWindow : public juce::DocumentWindow
{
public:
    KeyBindingsWindow (KeyBindings& kb, juce::PropertiesFile& props, std::function<void()> onApplied)
        : juce::DocumentWindow ("Keyboard Shortcuts", Colors::panelBg,
                                juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        content = new KeyBindingsContent (kb, props, std::move (onApplied));
        Dialogs::setContentFitted (*this, content, content->getWidth(), content->getHeight());
        setResizable (false, false);
    }

    void closeButtonPressed() override { setVisible (false); }

    KeyBindingsContent* content = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KeyBindingsWindow)
};
