#pragma once
#include <JuceHeader.h>
#include "AppLookAndFeel.h"
#include "ValueKnob.h"
#include "../Model/ProjectModel.h"

class PluginWrapperComponent : public juce::Component,
                               private juce::ValueTree::Listener
{
public:
    PluginWrapperComponent (juce::AudioProcessor& proc, juce::ValueTree projectTree,
                            juce::String pluginKey)
        : processor (proc), project (std::move (projectTree)), key (std::move (pluginKey))
    {
        setWantsKeyboardFocus (true);

        resolveNode();
        buildMixControls();

        if (kind == Kind::mixerSlot && node.getProperty (IDs::type).toString() == "plugin")
            showingParams = (bool) node.getProperty (IDs::genericUI, true);

        if (node.isValid())
            project.addListener (this);

        if (processor.hasEditor())
        {
            loading = std::make_unique<LoadingView> (processor.getName());
            addAndMakeVisible (*loading);
            setSize (420, 180 + barH);
            scheduleEditorBuild (0);
        }
        else
        {
            rebuildEditor();
            auto b = getPreferredBounds();
            setSize (b.getWidth(), b.getHeight());
        }
    }

    ~PluginWrapperComponent() override
    {
        if (node.isValid())
            project.removeListener (this);
    }

    std::function<void()> onStateChanged;
    std::function<void()> onPreferredSizeChanged;

    bool isEditorResizable() const { return ! showingParams && editor != nullptr && editor->isResizable(); }

    juce::Rectangle<int> getPreferredBounds() const
    {
        if (editor == nullptr)
            return { 0, 0, 420, 180 + barH };
        return { 0, 0, juce::jmax (320, editor->getWidth()), editor->getHeight() + barH };
    }

    static constexpr int barH = 30;

    void paint (juce::Graphics& g) override
    {
        auto bar = getLocalBounds().withHeight (barH);
        g.setColour (Colors::header);
        g.fillRect (bar);
        g.setColour (Colors::outline);
        g.fillRect (bar.removeFromBottom (1));

        const int numProgs = processor.getNumPrograms();

        {
            auto a = menuArea().toFloat();
            g.setColour (menuHover ? Colors::accent : Colors::textDim);
            juce::Path p;
            p.addTriangle (a.getCentreX() - 4.5f, a.getCentreY() - 2.5f,
                           a.getCentreX() + 4.5f, a.getCentreY() - 2.5f,
                           a.getCentreX(),        a.getCentreY() + 3.5f);
            g.fillPath (p);
        }

        {
            auto a = paramsArea().toFloat();
            g.setColour (showingParams ? Colors::accent
                                       : paramsHover ? Colors::text : Colors::textDim);
            const float knobX[3] = { -4.0f, 3.0f, -1.0f };
            for (int i = 0; i < 3; ++i)
            {
                float y = a.getCentreY() + (float) (i - 1) * 5.0f;
                g.fillRoundedRectangle (a.getCentreX() - 7.0f, y - 0.8f, 14.0f, 1.6f, 0.8f);
                g.fillEllipse (a.getCentreX() + knobX[i] - 1.8f, y - 1.8f, 3.6f, 3.6f);
            }
        }

        paintMixCluster (g);

        auto arrow = [&g] (juce::Rectangle<int> area, bool left, bool enabled, bool hover)
        {
            auto a = area.toFloat();
            g.setColour (! enabled ? Colors::stepDim : hover ? Colors::accent : Colors::text);
            juce::Path p;
            float d = left ? -1.0f : 1.0f;
            p.addTriangle (a.getCentreX() + 3.5f * d, a.getCentreY(),
                           a.getCentreX() - 2.5f * d, a.getCentreY() - 4.5f,
                           a.getCentreX() - 2.5f * d, a.getCentreY() + 4.5f);
            g.fillPath (p);
        };
        arrow (prevArea(), true,  numProgs > 1, prevHover);
        arrow (nextArea(), false, numProgs > 1, nextHover);

        auto na = nameArea().reduced (0, 6);
        g.setColour (Colors::panelDark);
        g.fillRoundedRectangle (na.toFloat(), 3.0f);
        if (nameHover)
        {
            g.setColour (Colors::accent.withAlpha (0.5f));
            g.drawRoundedRectangle (na.toFloat(), 3.0f, 1.0f);
        }
        g.setColour (numProgs > 1 ? Colors::text : Colors::textDim);
        g.setFont (juce::Font (juce::FontOptions (11.0f)));
        g.drawText (numProgs > 0 ? programName (processor.getCurrentProgram()) : "-",
                    na.reduced (6, 0), juce::Justification::centred);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();
        if (e.mods.isMiddleButtonDown())
            return;
        if (e.getPosition().getY() >= barH)
            return;
        const int numProgs = processor.getNumPrograms();
        if (menuArea().contains (e.getPosition()))
            showWrapperMenu();
        else if (paramsArea().contains (e.getPosition()))
            setParamsView (! showingParams);
        else if (onLedArea().contains (e.getPosition()))
            toggleOn();
        else if (trackArea().contains (e.getPosition()))
        {
            trackDragging = true;
            trackDragStart = channelTrack();
        }
        else if (prevArea().contains (e.getPosition()) && numProgs > 1)
            stepProgram (-1);
        else if (nextArea().contains (e.getPosition()) && numProgs > 1)
            stepProgram (1);
        else if (nameArea().contains (e.getPosition()))
            showPresetMenu();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown())
            return;
        if (! trackDragging)
            return;
        int delta = -e.getDistanceFromDragStartY() / 8;
        int next = juce::jlimit (1, Model::getMaxMixerTrackIndex (project), trackDragStart + delta);
        if (next != channelTrack())
            node.setProperty (IDs::mixerTrack, next, nullptr);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown())
            return;
        trackDragging = false;
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        if (kind == Kind::channel && e.getPosition().getY() < barH && trackArea().contains (e.getPosition()))
        {
            int next = juce::jlimit (1, Model::getMaxMixerTrackIndex (project),
                                     channelTrack() + (wheel.deltaY > 0 ? 1 : -1));
            node.setProperty (IDs::mixerTrack, next, nullptr);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        bool menu = menuArea().contains (e.getPosition());
        bool params = paramsArea().contains (e.getPosition());
        bool prev = prevArea().contains (e.getPosition());
        bool next = nextArea().contains (e.getPosition());
        bool name = nameArea().contains (e.getPosition());
        if (menu != menuHover || params != paramsHover || prev != prevHover
            || next != nextHover || name != nameHover)
        {
            menuHover = menu; paramsHover = params;
            prevHover = prev; nextHover = next; nameHover = name;
            repaint (0, 0, getWidth(), barH);
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        menuHover = paramsHover = prevHover = nextHover = nameHover = false;
        repaint (0, 0, getWidth(), barH);
    }

    void childBoundsChanged (juce::Component* c) override
    {
        if (editor != nullptr && c == editor.get())
            applyPreferredSize();
    }

    void resized() override
    {
        layoutMixControls();
        if (loading != nullptr)
            loading->setBounds (getLocalBounds().withTrimmedTop (barH));
        positionEditor();
    }

private:
    enum class Kind { none, channel, mixerSlot };

    class LoadingView : public juce::Component, private juce::Timer
    {
    public:
        explicit LoadingView (const juce::String& pluginName) : name (pluginName)
        {
            startTimerHz (30);
        }

        bool hasPainted() const { return painted; }

        void paint (juce::Graphics& g) override
        {
            painted = true;
            g.fillAll (Colors::panelDark);
            auto c = getLocalBounds().getCentre().toFloat();

            juce::Path arc;
            arc.addCentredArc (c.x, c.y - 12.0f, 16.0f, 16.0f, angle,
                               0.0f, juce::MathConstants<float>::pi * 1.5f, true);
            g.setColour (Colors::accent);
            g.strokePath (arc, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));

            g.setColour (Colors::text);
            g.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
            g.drawText ("Loading " + name + "...",
                        getLocalBounds().withTrimmedTop (getHeight() / 2 + 14).withHeight (24),
                        juce::Justification::centredTop);
        }

    private:
        void timerCallback() override
        {
            angle += 0.22f;
            repaint();
        }

        juce::String name;
        float angle = 0.0f;
        bool painted = false;
    };

    void scheduleEditorBuild (int attempt)
    {
        juce::Timer::callAfterDelay (attempt == 0 ? 50 : 30,
            [safe = juce::Component::SafePointer<PluginWrapperComponent> (this), attempt]
            {
                if (safe == nullptr || safe->loading == nullptr)
                    return;
                if (! safe->loading->hasPainted() && attempt < 40)
                {
                    safe->loading->repaint();
                    safe->scheduleEditorBuild (attempt + 1);
                    return;
                }
                safe->rebuildEditor();
                safe->applyPreferredSize();
            });
    }

    juce::Rectangle<int> menuArea() const   { return { 4, 0, 24, barH }; }
    juce::Rectangle<int> paramsArea() const { return { 30, 0, 24, barH }; }

    static constexpr int clusterX = 56;
    juce::Rectangle<int> onLedArea() const  { return { clusterX, 0, 22, barH }; }
    juce::Rectangle<int> knobSlot (int i) const
    { return { clusterX + 22 + i * 26, 0, 24, barH }; }
    juce::Rectangle<int> trackArea() const
    {
        return { clusterX + 22 + 2 * 26 + 2, 0, 26, barH };
    }

    juce::Rectangle<int> nextArea() const   { return { getWidth() - 24, 0, 20, barH }; }
    juce::Rectangle<int> nameArea() const   { return { getWidth() - 24 - 132, 0, 130, barH }; }
    juce::Rectangle<int> prevArea() const   { return { getWidth() - 24 - 132 - 22, 0, 20, barH }; }

    void paintMixCluster (juce::Graphics& g)
    {
        if (kind == Kind::none)
            return;

        {
            auto led = onLedArea().toFloat();
            auto circle = led.withSizeKeepingCentre (10.0f, 10.0f);
            if (isOn())
            {
                g.setColour (Colors::stepOn.withAlpha (0.35f));
                g.fillEllipse (circle.expanded (2.0f));
                g.setColour (Colors::stepOn);
            }
            else
            {
                g.setColour (Colors::stepOff);
            }
            g.fillEllipse (circle);
            g.setColour (Colors::outline);
            g.drawEllipse (circle, 1.0f);
        }

        if (kind == Kind::channel)
        {
            auto ta = trackArea().toFloat().reduced (1.0f, 7.0f);
            g.setColour (Colors::panelDark);
            g.fillRoundedRectangle (ta, 3.0f);
            g.setColour (Colors::outline);
            g.drawRoundedRectangle (ta, 3.0f, 1.0f);
            g.setColour (Colors::accent);
            g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
            g.drawText (juce::String (channelTrack()), ta.toNearestInt(), juce::Justification::centred);
        }
    }

    void buildMixControls()
    {
        if (kind == Kind::channel)
        {
            makeKnob (panKnob, IDs::pan, -1.0, 1.0, 0.0, "Pan");
            makeKnob (volKnob, IDs::volume, 0.0, 1.25, 0.8, "Volume");
        }
        else if (kind == Kind::mixerSlot)
        {
            makeKnob (mixKnob, IDs::mix, 0.0, 1.0, 1.0, "Mix (dry/wet)");
        }
    }

    void makeKnob (std::unique_ptr<ValueKnob>& k, const juce::Identifier& prop,
                   double min, double max, double def, const juce::String& tip)
    {
        k = std::make_unique<ValueKnob>();
        k->setRange (min, max, 0.0);
        k->setDoubleClickReturnValue (true, def);
        k->setValue ((double) node.getProperty (prop, def), juce::dontSendNotification);
        k->setTooltip (tip);
        auto* kp = k.get();
        k->onValueChange = [this, kp, prop] { node.setProperty (prop, kp->getValue(), nullptr); };
        addAndMakeVisible (*k);
    }

    void layoutMixControls()
    {
        const int wKnob = 20;
        const int y = (barH - wKnob) / 2;
        if (kind == Kind::channel)
        {
            if (panKnob != nullptr) panKnob->setBounds (knobSlot (0).getX() + 2, y, wKnob, wKnob);
            if (volKnob != nullptr) volKnob->setBounds (knobSlot (1).getX() + 2, y, wKnob, wKnob);
        }
        else if (kind == Kind::mixerSlot)
        {
            if (mixKnob != nullptr) mixKnob->setBounds (knobSlot (0).getX() + 2, y, wKnob, wKnob);
        }
    }

    void resolveNode()
    {
        if (key.startsWithChar ('c'))
        {
            int id = key.substring (1).getIntValue();
            node = Model::findChannelById (project, id);
            if (node.isValid())
                kind = Kind::channel;
        }
        else if (key.startsWithChar ('m'))
        {
            int sPos = key.indexOfChar ('s');
            if (sPos > 1)
            {
                int trackIndex = key.substring (1, sPos).getIntValue();
                int uid = key.substring (sPos + 1).getIntValue();
                auto track = Model::getMixerTrack (project, trackIndex);
                if (track.isValid())
                {
                    node = track.getChildWithProperty (IDs::uid, uid);
                    if (node.isValid())
                        kind = Kind::mixerSlot;
                }
            }
        }
    }

    bool isOn() const
    {
        if (kind == Kind::channel)
            return ! (bool) node.getProperty (IDs::mute, false);
        if (kind == Kind::mixerSlot)
            return (bool) node.getProperty (IDs::enabled, true);
        return true;
    }

    void toggleOn()
    {
        if (kind == Kind::channel)
            node.setProperty (IDs::mute, isOn(), nullptr);
        else if (kind == Kind::mixerSlot)
            node.setProperty (IDs::enabled, ! isOn(), nullptr);
        repaint (0, 0, getWidth(), barH);
    }

    int channelTrack() const { return (int) node.getProperty (IDs::mixerTrack, 1); }

    void valueTreePropertyChanged (juce::ValueTree& t, const juce::Identifier& p) override
    {
        if (t != node)
            return;
        if (kind == Kind::channel)
        {
            if (p == IDs::pan && panKnob != nullptr)
                panKnob->setValue ((double) node.getProperty (IDs::pan, 0.0), juce::dontSendNotification);
            else if (p == IDs::volume && volKnob != nullptr)
                volKnob->setValue ((double) node.getProperty (IDs::volume, 0.8), juce::dontSendNotification);
            else if (p == IDs::mute || p == IDs::mixerTrack)
                repaint (0, 0, getWidth(), barH);
        }
        else if (kind == Kind::mixerSlot)
        {
            if (p == IDs::mix && mixKnob != nullptr)
                mixKnob->setValue ((double) node.getProperty (IDs::mix, 1.0), juce::dontSendNotification);
            else if (p == IDs::enabled)
                repaint (0, 0, getWidth(), barH);
        }
    }

    void showWrapperMenu()
    {
        const int numProgs = processor.getNumPrograms();
        juce::PopupMenu presets;
        presets.addItem (1, "Next preset", numProgs > 1);
        presets.addItem (2, "Previous preset", numProgs > 1);
        if (numProgs > 1)
        {
            juce::PopupMenu progs;
            const int shown = juce::jmin (numProgs, 200);
            for (int i = 0; i < shown; ++i)
                progs.addItem (1000 + i, programName (i), true,
                               i == processor.getCurrentProgram());
            presets.addSubMenu ("Browse presets", progs);
        }
        presets.addSeparator();
        presets.addItem (3, "Save preset as...");
        presets.addItem (4, "Load preset...");

        juce::PopupMenu m;
        m.addSubMenu ("Presets", presets);
        m.addSeparator();
        m.addItem (5, "Browse parameters", processor.hasEditor(), showingParams);

        m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (
                             localAreaToGlobal (menuArea())),
            [safe = juce::Component::SafePointer<PluginWrapperComponent> (this)] (int r)
            {
                if (safe == nullptr || r == 0)
                    return;
                if (r == 1)       safe->stepProgram (1);
                else if (r == 2)  safe->stepProgram (-1);
                else if (r == 3)  safe->savePreset();
                else if (r == 4)  safe->loadPreset();
                else if (r == 5)  safe->setParamsView (! safe->showingParams);
                else if (r >= 1000)
                    safe->setProgram (r - 1000);
            });
    }

    void showPresetMenu()
    {
        juce::PopupMenu m;
        const int shown = juce::jmin (processor.getNumPrograms(), 200);
        for (int i = 0; i < shown; ++i)
            m.addItem (i + 1, programName (i), true, i == processor.getCurrentProgram());
        if (shown > 0)
            m.addSeparator();
        m.addItem (900001, "Save preset as...");
        m.addItem (900002, "Load preset...");
        m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (
                             localAreaToGlobal (nameArea())),
            [safe = juce::Component::SafePointer<PluginWrapperComponent> (this)] (int r)
            {
                if (safe == nullptr || r == 0)
                    return;
                if (r == 900001)      safe->savePreset();
                else if (r == 900002) safe->loadPreset();
                else if (r > 0)       safe->setProgram (r - 1);
            });
    }

    void stepProgram (int delta)
    {
        const int n = processor.getNumPrograms();
        if (n < 2)
            return;
        setProgram ((processor.getCurrentProgram() + delta + n) % n);
    }

    void setProgram (int index)
    {
        processor.setCurrentProgram (index);
        repaint (0, 0, getWidth(), barH);
        if (onStateChanged)
            onStateChanged();
    }

    void setParamsView (bool generic)
    {
        if (! processor.hasEditor())
            generic = true;
        if (generic == showingParams && editor != nullptr)
            return;
        showingParams = generic;
        if (kind == Kind::mixerSlot && node.isValid())
            node.setProperty (IDs::genericUI, generic, nullptr);
        rebuildEditor();
        applyPreferredSize();
    }

    void rebuildEditor()
    {
        loading.reset();
        editor.reset();
        if (! showingParams && processor.hasEditor())
        {
            processor.suspendProcessing (true);
            editor.reset (processor.createEditorIfNeeded());
            processor.suspendProcessing (false);
        }
        if (editor == nullptr)
        {
            showingParams = true;
            editor = std::make_unique<juce::GenericAudioProcessorEditor> (processor);
        }
        addAndMakeVisible (*editor);
        positionEditor();
    }

    void positionEditor()
    {
        if (editor != nullptr)
            editor->setTopLeftPosition (juce::jmax (0, (getWidth() - editor->getWidth()) / 2), barH);
    }

    void applyPreferredSize()
    {
        auto b = getPreferredBounds();
        if (b.getWidth() != getWidth() || b.getHeight() != getHeight())
            setSize (b.getWidth(), b.getHeight());
        if (onPreferredSizeChanged)
            onPreferredSizeChanged();
    }

    void savePreset()
    {
        juce::MemoryBlock state;
        processor.getStateInformation (state);
        auto suggested = processor.getNumPrograms() > 1
                             ? programName (processor.getCurrentProgram())
                             : processor.getName();
        chooser = std::make_unique<juce::FileChooser> (
            "Save preset",
            presetDir().getChildFile (juce::File::createLegalFileName (suggested + ".odawpreset")),
            "*.odawpreset");
        chooser->launchAsync (juce::FileBrowserComponent::saveMode
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
            [state] (const juce::FileChooser& fc)
            {
                auto f = fc.getResult();
                if (f != juce::File())
                    f.withFileExtension ("odawpreset").replaceWithData (state.getData(), state.getSize());
            });
    }

    void loadPreset()
    {
        chooser = std::make_unique<juce::FileChooser> ("Load preset", presetDir(), "*.odawpreset");
        chooser->launchAsync (juce::FileBrowserComponent::openMode
                                  | juce::FileBrowserComponent::canSelectFiles,
            [safe = juce::Component::SafePointer<PluginWrapperComponent> (this)] (const juce::FileChooser& fc)
            {
                auto f = fc.getResult();
                if (safe == nullptr || f == juce::File())
                    return;
                juce::MemoryBlock mb;
                if (! f.loadFileAsData (mb) || mb.isEmpty())
                    return;
                {
                    const juce::ScopedLock sl (safe->processor.getCallbackLock());
                    safe->processor.setStateInformation (mb.getData(), (int) mb.getSize());
                }
                safe->repaint();
                if (safe->onStateChanged)
                    safe->onStateChanged();
            });
    }

    juce::String programName (int index) const
    {
        auto s = processor.getProgramName (index).trim();
        return s.isNotEmpty() ? s : "Program " + juce::String (index + 1);
    }

    juce::File presetDir() const
    {
        auto dir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                       .getChildFile ("mikeDAW").getChildFile ("Presets")
                       .getChildFile (juce::File::createLegalFileName (processor.getName()));
        dir.createDirectory();
        return dir;
    }

    juce::AudioProcessor& processor;
    juce::ValueTree project;
    juce::String key;
    juce::ValueTree node;
    Kind kind = Kind::none;

    std::unique_ptr<ValueKnob> panKnob, volKnob, mixKnob;

    std::unique_ptr<juce::AudioProcessorEditor> editor;
    std::unique_ptr<LoadingView> loading;
    std::unique_ptr<juce::FileChooser> chooser;
    bool showingParams = false;
    bool menuHover = false, paramsHover = false, prevHover = false,
         nextHover = false, nameHover = false;
    bool trackDragging = false;
    int trackDragStart = 1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginWrapperComponent)
};
