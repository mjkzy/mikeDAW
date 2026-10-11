#include "ChannelRack.h"
#include "BrowserPanel.h"
#include "AppLookAndFeel.h"
#include "Dialogs.h"
#include "ValueKnob.h"
#include "HoverFade.h"
#include "PluginPickerPopup.h"
#include "../Import/MidiImport.h"
#include "../Model/Automation.h"

namespace
{
    juce::PopupMenu::Options menuAtMouse()
    {
        auto p = juce::Desktop::getInstance().getMainMouseSource().getScreenPosition().roundToInt();
        return juce::PopupMenu::Options().withTargetScreenArea ({ p.x, p.y, 1, 1 });
    }

    struct FlatNameLookAndFeel : public AppLookAndFeel
    {
        void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&,
                                   bool highlighted, bool down) override
        {
            auto r = b.getLocalBounds().toFloat();
            g.setColour (Colors::raised);
            g.fillRoundedRectangle (r, 3.0f);

            if (highlighted || down)
            {
                g.setColour (juce::Colours::white.withAlpha (down ? 0.07f : 0.04f));
                g.fillRoundedRectangle (r, 3.0f);
            }

            g.setColour (Colors::outline);
            g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 1.0f);
        }

        juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override
        {
            return Typography::semi (juce::jmin (16.0f, (float) buttonHeight * 0.7f));
        }

        void drawButtonText (juce::Graphics& g, juce::TextButton& b, bool highlighted, bool) override
        {
            g.setFont (getTextButtonFont (b, b.getHeight()));
            g.setColour (b.isEnabled() ? (highlighted ? Colors::text : Colors::text.withAlpha (0.9f))
                                       : Colors::textDim);
            g.drawFittedText (b.getButtonText(),
                              b.getLocalBounds().reduced (Metrics::pad, 0),
                              juce::Justification::centredLeft, 1);
        }
    };
}

ChannelRack::SeqClipboard ChannelRack::seqClipboard;

class ChannelRack::Row : public juce::Component
{
public:
    static constexpr int xSwitch = 8,  wSwitch = 12;
    static constexpr int xPan = 22,   wKnob = 18;
    static constexpr int xVol = 42;
    static constexpr int xRoute = 66, wRoute = 34;
    static constexpr int xName = 102, wName = 200;
    static constexpr int xSelect = 306, wSelect = 14;
    static constexpr int xSteps = 326;
    static constexpr int stepW = 14;
    static constexpr int groupGap = 8;

    Row (ChannelRack& r, juce::ValueTree channelTree)
        : rack (r), channel (channelTree)
    {
        nameBtn.setButtonText (channel.getProperty (IDs::name).toString());
        nameBtn.onClick = [this]
        {
            if (isPluginChannel())
            {
                if (rack.onOpenPluginEditor)
                    rack.onOpenPluginEditor ("c" + juce::String (channelId()),
                                             channel.getProperty (IDs::name).toString());
            }
            else if (rack.onOpenSamplerSettings)
                rack.onOpenSamplerSettings (channel);
        };
        nameBtn.setLookAndFeel (&*nameLnf);
        nameBtn.addMouseListener (this, false);
        addAndMakeVisible (nameBtn);

        auto setupKnob = [this] (ValueKnob& k, const juce::Identifier& prop,
                                 double min, double max, double def)
        {
            k.setSliderStyle (juce::Slider::RotaryVerticalDrag);
            k.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            k.setRange (min, max, 0.0);
            k.setDoubleClickReturnValue (true, def);
            k.setValue ((double) channel.getProperty (prop, def), juce::dontSendNotification);
            k.onValueChange = [this, &k, prop] { channel.setProperty (prop, k.getValue(), nullptr); };
            k.addMouseListener (this, false);
            addAndMakeVisible (k);
        };
        setupKnob (panKnob, IDs::pan, -1.0, 1.0, 0.0);
        setupKnob (volKnob, IDs::volume, 0.0, 1.25, 0.8);
        panKnob.getProperties().set ("bipolar", true);
        panKnob.setRotaryParameters (juce::MathConstants<float>::pi,
                                     juce::MathConstants<float>::pi * 3.0f, true);
        panKnob.setTooltip ("Pan");
        volKnob.setTooltip ("Volume");
        volKnob.onCreateAutomationClip = [this]
            { Automation::createClip (rack.project, "channel." + juce::String (channelId()) + ".volume"); };
        panKnob.onCreateAutomationClip = [this]
            { Automation::createClip (rack.project, "channel." + juce::String (channelId()) + ".pan"); };

        refreshFromTree();
    }

    ~Row() override { nameBtn.setLookAndFeel (nullptr); }

    int channelId() const        { return (int) channel.getProperty (IDs::id); }
    bool isPluginChannel() const
    {
        auto t = channel.getProperty (IDs::type).toString();
        return t == "plugin" || t == "builtin";
    }
    bool isOn() const            { return ! (bool) channel.getProperty (IDs::mute, false); }

    bool showsPianoRollPreview() const
    {
        auto seq = rack.currentPattern().getChildWithProperty (IDs::channelId, channelId());
        if (! seq.isValid())
            return false;
        if ((bool) seq.getProperty (IDs::usePianoRoll, false))
            return true;
        for (int i = 0; i < seq.getNumChildren(); ++i)
            if (seq.getChild (i).hasType (IDs::Note))
                return true;
        return false;
    }

    void refreshFromTree()
    {
        nameBtn.setButtonText (channel.getProperty (IDs::name).toString());
        panKnob.setValue ((double) channel.getProperty (IDs::pan, 0.0), juce::dontSendNotification);
        volKnob.setValue ((double) channel.getProperty (IDs::volume, 0.8), juce::dontSendNotification);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        const bool selected = rack.isChannelSelected (channelId());

        g.setColour (Colors::panelDark);
        g.fillRoundedRectangle (r.reduced (1).toFloat(), 3.0f);

        g.setColour (Model::channelColour (channelId()));
        g.fillRoundedRectangle (r.removeFromLeft (5).reduced (1, 4).toFloat(), 2.0f);

        {
            auto circle = getSwitchArea().toFloat().withSizeKeepingCentre (8.0f, 8.0f);
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

        {
            auto routeArea = getRouteArea().toFloat();
            const float arrowW = 8.0f;

            g.setColour (Colors::panelDark);
            g.fillRoundedRectangle (routeArea, 3.0f);
            g.setColour (Colors::outline);
            g.drawRoundedRectangle (routeArea, 3.0f, 1.0f);

            auto arrows = routeArea.removeFromRight (arrowW);

            g.setColour (Colors::accent);
            g.setFont (Typography::bold (14.0f));
            g.drawText (juce::String ((int) channel.getProperty (IDs::mixerTrack, 1)),
                        routeArea.toNearestInt(), juce::Justification::centred);

            g.setColour (Colors::textDim);
            auto up = arrows.removeFromTop (arrows.getHeight() * 0.5f).reduced (1.5f, 3.0f);
            auto dn = arrows.reduced (1.5f, 3.0f);
            {
                juce::Path p;
                p.addTriangle (up.getX(), up.getBottom(), up.getRight(), up.getBottom(), up.getCentreX(), up.getY());
                g.fillPath (p);
            }
            {
                juce::Path p;
                p.addTriangle (dn.getX(), dn.getY(), dn.getRight(), dn.getY(), dn.getCentreX(), dn.getBottom());
                g.fillPath (p);
            }
        }

        {
            auto box = getSelectArea().toFloat().reduced (1.0f, 8.0f);
            auto now = juce::Time::getMillisecondCounter();

            bool last = rack.lastSelectedChannel == channelId();
            juce::Colour idleFill = selected ? Colors::accent.withAlpha (last ? 0.9f : 0.5f)
                                             : Colors::stepDim2;
            juce::Colour idleBorder = selected ? Colors::accent : Colors::outline;

            float env = 0.0f;
            if (now < flashUntil)
            {
                float t = juce::jlimit (0.0f, 1.0f, (float) (flashUntil - now) / (float) flashMs);
                env = t * t;
            }

            if (env > 0.0f)
            {
                g.setColour (Colors::accent2.withAlpha (0.30f * env));
                g.fillRoundedRectangle (box.expanded (2.5f), 3.0f);
            }
            g.setColour (idleFill.interpolatedWith (Colors::accent2, env));
            g.fillRoundedRectangle (box, 2.0f);
            g.setColour (idleBorder.interpolatedWith (Colors::accent2.brighter (0.4f), env));
            g.drawRoundedRectangle (box, 2.0f, 1.0f + 0.2f * env);
        }

        if (showsPianoRollPreview())
            paintNotePreview (g);
        else
            paintSteps (g);

        paintPlayhead (g);

        if (rack.dragHoverChannelId == channelId())
        {
            auto frame = getLocalBounds().reduced (1).toFloat();
            g.setColour (Colors::accent.withAlpha (0.12f));
            g.fillRoundedRectangle (frame, 3.0f);
            g.setColour (Colors::accent.withAlpha (0.75f));
            g.drawRoundedRectangle (frame.reduced (0.7f), 3.0f, 1.4f);
        }
    }

    void paintPlayhead (juce::Graphics& g)
    {
        auto& engine = rack.engine;
        if (! engine.isPlaying())
            return;

        if (lastPlayheadLineX < 0)
            return;

        auto area = getPreviewArea();
        const int x = lastPlayheadLineX;
        const float top = (float) area.getY() + 2.0f, bottom = (float) area.getBottom() - 2.0f;
        g.setColour (Colors::accent2.withAlpha (0.25f));
        g.fillRect ((float) x - 2.0f, top, 5.0f, bottom - top);
        g.setColour (Colors::accent2);
        g.fillRect ((float) x - 0.5f, top, 2.0f, bottom - top);
    }

    int activeNoteHash (double tick) const
    {
        if (tick < 0.0)
            return 0;
        auto pattern = rack.currentPattern();
        if (! pattern.isValid())
            return 0;
        auto seq = pattern.getChildWithProperty (IDs::channelId, channelId());
        int h = 0;
        for (int i = 0; i < seq.getNumChildren(); ++i)
        {
            auto note = seq.getChild (i);
            if (! note.hasType (IDs::Note))
                continue;
            int start = (int) note.getProperty (IDs::start);
            int len = juce::jmax (1, (int) note.getProperty (IDs::length));
            if (tick >= start && tick < start + len)
                h = h * 31 + i + 1;
        }
        return h;
    }

    bool advanceStepGlow (float dt, double playTick)
    {
        stepGlowDirty = {};

        if (! Fx::glowEnabled)
        {
            if (stepGlow.empty())
                return false;
            stepGlow.clear();
            stepGlowDirty = getPreviewArea();
            return true;
        }

        auto steps = getStepString();
        int n = steps.length();
        if ((int) stepGlow.size() != n)
        {
            stepGlow.assign ((size_t) juce::jmax (0, n), 0.0f);
            stepGlowDirty = getPreviewArea();
        }

        int cur = playTick >= 0.0 ? juce::jlimit (0, n - 1, (int) (playTick / Model::ticksPerStep)) : -1;
        bool changed = false;
        for (int i = 0; i < n; ++i)
        {
            float target = (i == cur && steps[i] == '1') ? 1.0f : 0.0f;
            float& v = stepGlow[(size_t) i];
            float nv = target > v ? juce::jmin (target, v + dt / 0.06f)
                                  : juce::jmax (target, v - dt / 0.30f);
            if (std::abs (nv - v) > 0.002f)
            {
                changed = true;
                stepGlowDirty = stepGlowDirty.getUnion (getStepRect (i, n).expanded (5));
            }
            v = nv;
        }
        return changed;
    }

    void repaintStepGlow()
    {
        if (! stepGlowDirty.isEmpty())
            repaint (stepGlowDirty);
    }

    void repaintPlayheadLine (float playFrac, double tick)
    {
        auto area = getPreviewArea();
        const int x = playFrac >= 0.0f ? area.getX() + (int) (playFrac * (float) area.getWidth()) : -1;
        playTick = tick;
        if (showsPianoRollPreview())
        {
            const int hot = activeNoteHash (tick);
            if (hot != lastActiveNoteHash)
            {
                lastActiveNoteHash = hot;
                repaint (area);
            }
        }
        if (x == lastPlayheadLineX)
            return;

        if (lastPlayheadLineX >= 0 && x >= 0 && std::abs (x - lastPlayheadLineX) <= 64)
        {
            const int lo = juce::jmin (x, lastPlayheadLineX) - 2;
            const int hi = juce::jmax (x, lastPlayheadLineX) + 2;
            repaint (lo, 0, hi - lo + 1, getHeight());
        }
        else
        {
            if (lastPlayheadLineX >= 0)
                repaint (lastPlayheadLineX - 2, 0, 5, getHeight());
            if (x >= 0)
                repaint (x - 2, 0, 5, getHeight());
        }
        lastPlayheadLineX = x;
    }

    bool stepGlowActive() const
    {
        for (float v : stepGlow)
            if (v > 0.002f)
                return true;
        return false;
    }

    void paintSteps (juce::Graphics& g)
    {
        auto steps = getStepString();
        int n = steps.length();

        for (int i = 0; i < n; ++i)
        {
            auto cell = getStepRect (i, n).toFloat().reduced (1.0f, 4.0f);
            bool on = steps[i] == '1';
            bool groupA = (i / 4) % 2 == 0;
            auto baseColour = on ? Model::channelColour (channelId())
                                 : (groupA ? Colors::stepDim : Colors::stepDim2);
            g.setColour (baseColour);
            g.fillRoundedRectangle (cell, 2.0f);

            if (on)
            {
                g.setColour (juce::Colours::white.withAlpha (0.25f));
                g.fillRoundedRectangle (cell.withHeight (cell.getHeight() * 0.4f), 2.0f);
            }

            if (on && Fx::glowEnabled && i < (int) stepGlow.size())
            {
                float glow = stepGlow[(size_t) i];
                if (glow > 0.002f)
                {
                    auto tint = Model::channelColour (channelId()).interpolatedWith (juce::Colours::white, 0.6f);
                    g.setColour (tint.withAlpha (glow * 0.16f));
                    g.fillRoundedRectangle (cell.expanded (3.0f), 3.0f);
                    g.setColour (tint.withAlpha (glow * 0.28f));
                    g.fillRoundedRectangle (cell.expanded (1.5f), 2.5f);
                    g.setColour (juce::Colours::white.withAlpha (glow * 0.5f));
                    g.fillRoundedRectangle (cell, 2.0f);
                }
            }
        }
    }

    void paintNotePreview (juce::Graphics& g)
    {
        auto pattern = rack.currentPattern();
        auto area = getPreviewArea().toFloat().reduced (0.0f, 4.0f);
        auto colour = Colors::noteGreen;

        g.setColour (Colors::panelDark);
        g.fillRoundedRectangle (area, Metrics::rS);

        if (! pattern.isValid())
            return;
        auto seq = pattern.getChildWithProperty (IDs::channelId, channelId());
        if (! seq.isValid())
            return;

        int minKey = 127, maxKey = 0;
        for (int i = 0; i < seq.getNumChildren(); ++i)
        {
            auto note = seq.getChild (i);
            if (! note.hasType (IDs::Note))
                continue;
            int k = (int) note.getProperty (IDs::key);
            minKey = juce::jmin (minKey, k);
            maxKey = juce::jmax (maxKey, k);
        }
        if (minKey > maxKey)
            return;

        double patTicks = juce::jmax (1, Model::patternLengthTicks (pattern));
        auto inner = area.reduced (2.0f);
        float noteH = juce::jlimit (2.0f, 5.0f, (inner.getHeight()) / (float) (maxKey - minKey + 1));

        for (int i = 0; i < seq.getNumChildren(); ++i)
        {
            auto note = seq.getChild (i);
            if (! note.hasType (IDs::Note))
                continue;
            double start = (double) (int) note.getProperty (IDs::start) / patTicks;
            double len = (double) juce::jmax (1, (int) note.getProperty (IDs::length)) / patTicks;
            int key = (int) note.getProperty (IDs::key);
            float y = maxKey == minKey
                ? inner.getCentreY() - noteH * 0.5f
                : inner.getY() + (float) (maxKey - key) / (float) (maxKey - minKey)
                                 * (inner.getHeight() - noteH);
            int s0 = (int) note.getProperty (IDs::start);
            bool hot = playTick >= 0.0 && playTick >= s0
                    && playTick < s0 + juce::jmax (1, (int) note.getProperty (IDs::length));
            auto rect = juce::Rectangle<float> (inner.getX() + (float) (start * inner.getWidth()), y,
                                                juce::jmax (2.0f, (float) (len * inner.getWidth())), noteH);
            if (hot)
            {
                g.setColour (juce::Colours::white.withAlpha (0.35f));
                g.fillRoundedRectangle (rect.expanded (2.0f, 1.5f), Metrics::rS);
                g.setColour (juce::Colours::white);
            }
            else
                g.setColour (colour);
            g.fillRoundedRectangle (rect, juce::jmin (Metrics::rS, noteH * 0.5f));
        }
    }

    void resized() override
    {
        panKnob.setBounds (xPan, (getHeight() - wKnob) / 2, wKnob, wKnob);
        volKnob.setBounds (xVol, (getHeight() - wKnob) / 2, wKnob, wKnob);
        nameBtn.setBounds (xName, 3, wName, getHeight() - 6);
    }

    void showChannelMenu()
    {
        juce::Array<int> transposeTargets = rack.isChannelSelected (channelId())
                                              ? rack.getSelectedChannelIds()
                                              : juce::Array<int> { channelId() };

        if (! rack.isChannelSelected (channelId()))
            rack.selectChannel (channelId(), false);

        juce::PopupMenu m;
        m.addItem (1, "Piano roll");
        m.addItem (6, "Solo", true, rack.soloedChannelId == channelId());
        m.addItem (4, "Cut itself", true, (bool) channel.getProperty (IDs::cutItself, false));
        m.addSeparator();

        m.addItem (30, "Fill each 2 steps");
        m.addItem (31, "Fill each 4 steps");
        m.addItem (32, "Fill each 8 steps");
        m.addSeparator();

        m.addItem (8, "Cut");
        m.addItem (9, "Copy");
        m.addItem (10, "Paste", ChannelRack::seqClipboard.valid);
        m.addSeparator();

        juce::PopupMenu transpose;
        transpose.addItem (20, "+12 (octave up)");
        transpose.addItem (21, "+2");
        transpose.addItem (22, "+1");
        transpose.addItem (23, "-1");
        transpose.addItem (24, "-2");
        transpose.addItem (25, "-12 (octave down)");
        m.addSubMenu ("Transpose selected", transpose);
        m.addItem (7, "Clone channel");
        m.addSeparator();
        m.addItem (5, "Replace Instrument...");
        m.addItem (2, "Load sample into channel...", ! isPluginChannel());
        m.addSeparator();
        m.addItem (3, "Delete channel");

        m.showMenuAsync (menuAtMouse(),
            [this, transposeTargets] (int result)
            {
                if (result == 1 && rack.onOpenPianoRoll)
                    rack.onOpenPianoRoll (rack.getSelectedPatternId(), channelId());
                else if (result == 2)
                    rack.pickSampleFor (channel);
                else if (result == 3)
                {
                    Model::removeChannelSequences (rack.project, channelId());
                    channel.getParent().removeChild (channel, nullptr);
                }
                else if (result == 4)
                    channel.setProperty (IDs::cutItself,
                                         ! (bool) channel.getProperty (IDs::cutItself, false), nullptr);
                else if (result == 6)
                    rack.toggleChannelSolo (channelId());
                else if (result == 7)
                    rack.cloneChannel (channel);
                else if (result == 8)
                    cutSeq();
                else if (result == 9)
                    copySeq();
                else if (result == 10)
                    pasteSeq();
                else if (result >= 30 && result <= 32)
                {
                    static const int spacing[] = { 2, 4, 8 };
                    fillEverySteps (spacing[result - 30]);
                }
                else if (result >= 20 && result <= 25)
                {
                    static const int deltas[] = { 12, 2, 1, -1, -2, -12 };
                    rack.transposeChannels (transposeTargets, deltas[result - 20]);
                }
                else if (result == 5)
                {
                    auto instruments = rack.host.getInstruments();
                    auto safeRow = juce::Component::SafePointer<Row> (this);
                    PluginPickerPopup::show (*this, instruments,
                        std::vector<std::pair<int, juce::String>>{},
                        [safeRow] (const juce::PluginDescription& desc)
                        {
                            if (safeRow == nullptr)
                                return;
                            auto resolved = desc;
                            juce::String err;
                            if (! safeRow->rack.host.resolveForLoad (resolved, err))
                            {
                                Dialogs::showInfoAsync (juce::MessageBoxIconType::WarningIcon,
                                    "Plugin failed to load", err);
                                return;
                            }
                            safeRow->rack.replaceInstrument (safeRow->channel, resolved);
                        },
                        {}, rack.isPluginEnabled, rack.isPluginNew);
                }
            });
    }

    void showRouteMenu()
    {
        juce::PopupMenu m;
        m.addItem (1, "Reset to Default");
        m.showMenuAsync (menuAtMouse(),
            [safe = juce::Component::SafePointer<Row> (this)] (int r)
            {
                if (safe != nullptr && r == 1)
                    safe->channel.setProperty (IDs::mixerTrack, 1, nullptr);
            });
    }

    bool fromKnob (const juce::MouseEvent& e) const
    {
        return e.eventComponent == &panKnob || e.eventComponent == &volKnob;
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown() || fromKnob (e))
            return;
        if (e.eventComponent == &nameBtn)
        {
            if (e.mods.isPopupMenu())
                showChannelMenu();
            return;
        }

        if (getSelectArea().contains (e.getPosition()) && ! e.mods.isPopupMenu())
        {
            selectDragging = true;
            bool additive = e.mods.isShiftDown() || e.mods.isCommandDown()
                            || rack.isChannelSelected (channelId());
            rack.selectChannel (channelId(), additive);
            return;
        }

        if (! e.mods.isPopupMenu())
            rack.selectChannel (channelId(), e.mods.isShiftDown() || e.mods.isCommandDown());

        if (getSwitchArea().contains (e.getPosition()))
        {
            if (e.mods.isPopupMenu())
                rack.toggleChannelSolo (channelId());
            else
                channel.setProperty (IDs::mute, isOn(), nullptr);
            repaint();
            return;
        }

        if (getRouteArea().contains (e.getPosition()))
        {
            if (e.mods.isPopupMenu())
            {
                showRouteMenu();
                return;
            }
            routeDragging = true;
            routeDragStart = (int) channel.getProperty (IDs::mixerTrack, 1);
            e.source.enableUnboundedMouseMovement (true, false);
            return;
        }

        bool preview = showsPianoRollPreview();
        int stepIdx = preview ? -1 : stepIndexAt (e.getPosition());

        if (e.mods.isPopupMenu() && (preview || stepIdx < 0))
        {
            showChannelMenu();
            return;
        }

        if (preview && getPreviewArea().contains (e.getPosition()))
        {
            if (rack.onOpenPianoRoll)
                rack.onOpenPianoRoll (rack.getSelectedPatternId(), channelId());
            return;
        }

        if (stepIdx >= 0)
        {
            auto steps = getStepString();
            if (stepIdx < steps.length())
            {
                paintValue = ! e.mods.isPopupMenu();
                setStep (stepIdx, paintValue);
                if (paintValue && ! rack.engine.isPlaying())
                    rack.engine.auditionNote (channelId(), 60, 100);
            }
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown() || fromKnob (e))
            return;
        if (selectDragging)
        {
            rack.selectAtContainerPoint (e.getEventRelativeTo (&rack.rowContainer).getPosition());
            return;
        }

        if (routeDragging)
        {
            int delta = -e.getDistanceFromDragStartY() / 8;
            int next = juce::jlimit (1, Model::getMaxMixerTrackIndex (channel.getParent().getParent()),
                                     routeDragStart + delta);
            if (next != (int) channel.getProperty (IDs::mixerTrack, 1))
            {
                channel.setProperty (IDs::mixerTrack, next, nullptr);
                repaint();
            }
            return;
        }

        if (showsPianoRollPreview())
            return;
        int idx = stepIndexAt (e.getPosition());
        if (idx >= 0)
        {
            auto steps = getStepString();
            if (idx < steps.length() && (steps[idx] == '1') != paintValue)
                setStep (idx, paintValue);
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown() || fromKnob (e))
            return;
        if (routeDragging && e.source.isUnboundedMouseMovementEnabled())
        {
            auto src = e.source;
            src.enableUnboundedMouseMovement (false);
            src.setScreenPosition (e.getMouseDownScreenPosition().toFloat());
        }
        routeDragging = false;
        selectDragging = false;
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        if (getRouteArea().contains (e.getPosition()))
        {
            int cur = (int) channel.getProperty (IDs::mixerTrack, 1);
            int next = juce::jlimit (1, Model::getMaxMixerTrackIndex (channel.getParent().getParent()),
                                     cur + (wheel.deltaY > 0 ? 1 : -1));
            channel.setProperty (IDs::mixerTrack, next, nullptr);
            repaint();
            return;
        }
        juce::Component::mouseWheelMove (e, wheel);
    }

    static int stepsWidth (int numSteps)
    {
        return numSteps * stepW + (juce::jmax (0, numSteps - 1) / 4) * groupGap;
    }

    juce::ValueTree channel;

private:
    juce::Rectangle<int> getSwitchArea() const { return { xSwitch, 0, wSwitch, getHeight() }; }
    juce::Rectangle<int> getRouteArea() const  { return { xRoute, 5, wRoute, getHeight() - 10 }; }
    juce::Rectangle<int> getSelectArea() const { return { xSelect, 0, wSelect, getHeight() }; }

    int stepCellW (int numSteps) const
    {
        int gaps = (juce::jmax (0, numSteps - 1) / 4) * groupGap;
        return juce::jmax (stepW, (getWidth() - xSteps - 6 - gaps) / juce::jmax (1, numSteps));
    }

    juce::Rectangle<int> getStepRect (int i, int numSteps) const
    {
        int w = stepCellW (numSteps);
        return { xSteps + i * w + (i / 4) * groupGap, 0, w, getHeight() };
    }

    juce::Rectangle<int> getPreviewArea() const
    {
        return { xSteps, 0, juce::jmax (0, getWidth() - xSteps - 6), getHeight() };
    }

    int stepIndexAt (juce::Point<int> pos) const
    {
        if (pos.getX() < xSteps)
            return -1;
        int n = getStepString().length();
        for (int i = 0; i < n; ++i)
            if (getStepRect (i, n).contains (pos))
                return i;
        return -1;
    }

    juce::String getStepString() const
    {
        auto pattern = rack.currentPattern();
        if (! pattern.isValid())
            return Model::emptySteps (Model::defaultSteps);
        auto seq = pattern.getChildWithProperty (IDs::channelId, channelId());
        int len = (int) pattern.getProperty (IDs::lengthSteps, Model::defaultSteps);
        auto s = seq.isValid() ? seq.getProperty (IDs::steps).toString() : juce::String();
        if (s.length() < len)
            s += Model::emptySteps (len - s.length());
        return s.substring (0, len);
    }

    void setStep (int idx, bool on)
    {
        auto pattern = rack.currentPattern();
        if (! pattern.isValid())
            return;
        auto seq = Model::getChannelSeq (pattern, channelId(), true);
        auto steps = getStepString();
        steps = steps.substring (0, idx) + (on ? "1" : "0") + steps.substring (idx + 1);
        seq.setProperty (IDs::steps, steps, nullptr);
        repaint();
    }

    void fillEverySteps (int n)
    {
        auto pattern = rack.currentPattern();
        if (! pattern.isValid())
            return;
        auto seq = Model::getChannelSeq (pattern, channelId(), true);
        int len = (int) pattern.getProperty (IDs::lengthSteps, Model::defaultSteps);
        auto contentEnd = Model::patternContentEndTicks (pattern);
        if (contentEnd > 0)
        {
            auto loopTicks = ((contentEnd + Model::ticksPerBar - 1) / Model::ticksPerBar) * Model::ticksPerBar;
            len = juce::jmin (len, (int) (loopTicks / Model::ticksPerStep));
        }
        for (int i = seq.getNumChildren() - 1; i >= 0; --i)
            if (seq.getChild (i).hasType (IDs::Note))
                seq.removeChild (i, nullptr);
        juce::String steps;
        for (int i = 0; i < len; ++i)
            steps += (i % n == 0) ? "1" : "0";
        seq.setProperty (IDs::steps, steps, nullptr);
        repaint();
    }

    void copySeq()
    {
        auto pattern = rack.currentPattern();
        if (! pattern.isValid())
            return;
        auto& clip = ChannelRack::seqClipboard;
        clip = {};
        auto seq = Model::getChannelSeq (pattern, channelId(), false);
        if (seq.isValid())
        {
            clip.steps = seq.getProperty (IDs::steps).toString();
            clip.usePianoRoll = (bool) seq.getProperty (IDs::usePianoRoll, false);
            for (int i = 0; i < seq.getNumChildren(); ++i)
                if (seq.getChild (i).hasType (IDs::Note))
                    clip.notes.appendChild (seq.getChild (i).createCopy(), nullptr);
        }
        else
        {
            clip.steps = Model::emptySteps ((int) pattern.getProperty (IDs::lengthSteps, Model::defaultSteps));
        }
        clip.valid = true;
    }

    void cutSeq()
    {
        copySeq();
        auto pattern = rack.currentPattern();
        if (! pattern.isValid())
            return;
        auto seq = Model::getChannelSeq (pattern, channelId(), false);
        if (! seq.isValid())
            return;
        seq.setProperty (IDs::steps,
                         Model::emptySteps ((int) pattern.getProperty (IDs::lengthSteps, Model::defaultSteps)),
                         nullptr);
        repaint();
    }

    void pasteSeq()
    {
        auto& clip = ChannelRack::seqClipboard;
        if (! clip.valid)
            return;
        auto pattern = rack.currentPattern();
        if (! pattern.isValid())
            return;
        auto seq = Model::getChannelSeq (pattern, channelId(), true);
        for (int i = seq.getNumChildren() - 1; i >= 0; --i)
            if (seq.getChild (i).hasType (IDs::Note))
                seq.removeChild (i, nullptr);
        int len = (int) pattern.getProperty (IDs::lengthSteps, Model::defaultSteps);
        auto steps = clip.steps;
        if (steps.length() < len)
            steps += Model::emptySteps (len - steps.length());
        seq.setProperty (IDs::steps, steps.substring (0, len), nullptr);
        seq.setProperty (IDs::usePianoRoll, clip.usePianoRoll, nullptr);
        for (int i = 0; i < clip.notes.getNumChildren(); ++i)
            seq.appendChild (clip.notes.getChild (i).createCopy(), nullptr);
        repaint();
    }

    ChannelRack& rack;
    juce::SharedResourcePointer<FlatNameLookAndFeel> nameLnf;
    juce::TextButton nameBtn;
    ValueKnob panKnob, volKnob;
    bool paintValue = true;
    bool routeDragging = false;
    bool selectDragging = false;
    int routeDragStart = 1;
    juce::uint32 flashUntil = 0;
    static constexpr int flashMs = 280;
    std::vector<float> stepGlow;
    juce::Rectangle<int> stepGlowDirty;
    int lastPlayheadLineX = -1;
    int lastActiveNoteHash = 0;
    double playTick = -1.0;

    friend class ChannelRack;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Row)
};

bool ChannelRack::SmoothViewport::wheelScroll (const juce::MouseWheelDetails& wheel)
{
    auto* content = getViewedComponent();
    if (content == nullptr || wheel.deltaY == 0.0f || wheel.isSmooth)
        return false;

    const int maxY = juce::jmax (0, content->getHeight() - getMaximumVisibleHeight());
    if (maxY <= 0)
        return false;

    const double step = juce::jmax (1.0, (double) getVerticalScrollBar().getSingleStepSize()) * 14.0;
    targetY = juce::jlimit (0, maxY, targetY - juce::roundToInt (wheel.deltaY * step));
    if (! isTimerRunning())
        startTimerHz (60);
    return true;
}

void ChannelRack::SmoothViewport::mouseWheelMove (const juce::MouseEvent& e,
                                                  const juce::MouseWheelDetails& wheel)
{
    if (! wheelScroll (wheel))
        juce::Viewport::mouseWheelMove (e, wheel);
}

void ChannelRack::SmoothViewport::visibleAreaChanged (const juce::Rectangle<int>&)
{
    if (! animating)
    {
        targetY = getViewPositionY();
        stopTimer();
    }
}

void ChannelRack::SmoothViewport::timerCallback()
{
    const juce::ScopedValueSetter<bool> guard (animating, true);
    const float cur = (float) getViewPositionY();
    const float delta = (float) targetY - cur;

    if (std::abs (delta) < 1.0f)
    {
        stopTimer();
        setViewPosition (getViewPositionX(), targetY);
        return;
    }
    setViewPosition (getViewPositionX(), juce::roundToInt (cur + delta * 0.3f));
}

void ChannelRack::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (! viewport.wheelScroll (wheel))
        juce::Component::mouseWheelMove (e, wheel);
}

ChannelRack::ChannelRack (juce::ValueTree p, AudioEngine& e, PluginHost& h)
    : project (p), engine (e), host (h)
{
    project.addListener (this);

    addChannelBtn.setTooltip ("Add channel");
    addChannelBtn.onClick = [this] { showAddChannelMenu(); };
    addAndMakeVisible (addChannelBtn);

    rowContainer.addChildComponent (insertLine);
    insertLine.setInterceptsMouseClicks (false, false);
    insertLine.setAlwaysOnTop (true);

    viewport.setViewedComponent (&rowContainer, false);
    viewport.setScrollBarsShown (true, true);
    viewport.setScrollBarThickness (10);
    addAndMakeVisible (viewport);

    selectedPatternId = engine.getCurrentPatternId();
    if (! Model::findPatternById (project, selectedPatternId).isValid())
    {
        auto patterns = Model::getPatterns (project);
        if (patterns.getNumChildren() > 0)
            selectedPatternId = (int) patterns.getChild (0).getProperty (IDs::id);
    }
    rebuildRows();

    startTimerHz (30);
}

void ChannelRack::timerCallback()
{
    int buf[64];
    int n = engine.drainNoteOnChannelIds (buf, 64);
    auto now = juce::Time::getMillisecondCounter();
    if (n > 0)
    {
        auto deadline = now + (juce::uint32) Row::flashMs;
        for (int i = 0; i < n; ++i)
            for (auto* row : rows)
                if (row->channelId() == buf[i])
                    row->flashUntil = deadline;
    }

    bool playing = engine.isPlaying();

    float playFrac = -1.0f;
    double playTick = -1.0;
    auto pattern = currentPattern();
    if (playing && pattern.isValid())
    {
        double gridTicks = juce::jmax (1, Model::patternLengthTicks (pattern));
        auto contentEnd = Model::patternContentEndTicks (pattern);
        double loopTicks = contentEnd > 0
            ? (double) (((contentEnd + Model::ticksPerBar - 1) / Model::ticksPerBar) * Model::ticksPerBar)
            : juce::jmin (gridTicks, (double) Model::ticksPerBar);

        if (! engine.isSongMode())
        {
            if (engine.getCurrentPatternId() == getSelectedPatternId())
                playTick = std::fmod (engine.getPositionTicks(), juce::jmax (1.0, loopTicks));
        }
        else
        {
            const double posBeats = engine.getPositionTicks() / (double) Model::PPQ;
            auto playlist = Model::getPlaylist (project);
            for (int i = 0; playlist.isValid() && i < playlist.getNumChildren(); ++i)
            {
                auto clip = playlist.getChild (i);
                if (! clip.hasType (IDs::Clip)
                    || clip.getProperty (IDs::type).toString() != "pattern"
                    || (int) clip.getProperty (IDs::patternId) != getSelectedPatternId())
                    continue;
                const double startB = (double) clip.getProperty (IDs::startBeats);
                const double lenB   = (double) clip.getProperty (IDs::lengthBeats);
                if (posBeats < startB || posBeats >= startB + lenB)
                    continue;
                const double offB = (double) clip.getProperty (IDs::offsetBeats, 0.0);
                const double local = (posBeats - startB + offB) * (double) Model::PPQ;
                if (local >= 0.0 && local < loopTicks)
                {
                    playTick = local;
                    break;
                }
            }
        }

        if (playTick >= 0.0)
            playFrac = (float) (playTick / juce::jmax (gridTicks, loopTicks));
    }

    float dt = lastGlowMs == 0 ? 1.0f / (float) timerRate
                               : juce::jlimit (0.001f, 0.25f, (float) (now - lastGlowMs) * 0.001f);
    lastGlowMs = now;

    bool glowing = false;
    for (auto* row : rows)
    {
        bool flashing = row->flashUntil != 0;
        if (flashing && now >= row->flashUntil)
            row->flashUntil = 0;

        row->advanceStepGlow (dt, row->showsPianoRollPreview() ? -1.0 : playTick);
        glowing = glowing || row->stepGlowActive();

        row->repaintStepGlow();
        row->repaintPlayheadLine (playFrac, playTick);
        if (flashing)
            row->repaint (row->getSelectArea().expanded (4));
    }

    int rate = (playing || glowing) ? 60 : 30;
    if (rate != timerRate)
    {
        timerRate = rate;
        startTimerHz (rate);
    }
}

ChannelRack::~ChannelRack()
{
    project.removeListener (this);
}

int ChannelRack::getSelectedPatternId() const
{
    return selectedPatternId > 0 ? selectedPatternId : engine.getCurrentPatternId();
}

void ChannelRack::setSelectedPattern (int patternId)
{
    if (selectedPatternId == patternId)
        return;
    selectedPatternId = patternId;
    for (auto* row : rows)
        row->repaint();
    if (onContentHeightChanged)
        onContentHeightChanged();
}

juce::ValueTree ChannelRack::currentPattern() const
{
    return Model::findPatternById (project, getSelectedPatternId());
}

void ChannelRack::transposeChannels (const juce::Array<int>& channelIds, int semitones)
{
    auto pattern = currentPattern();
    if (! pattern.isValid())
        return;
    for (int cid : channelIds)
    {
        auto seq = Model::getChannelSeq (pattern, cid, false);
        if (! seq.isValid())
            continue;
        for (int i = 0; i < seq.getNumChildren(); ++i)
        {
            auto note = seq.getChild (i);
            if (! note.hasType (IDs::Note))
                continue;
            int k = (int) note.getProperty (IDs::key);
            note.setProperty (IDs::key, juce::jlimit (0, 127, k + semitones), nullptr);
        }
    }
}

void ChannelRack::cloneChannel (juce::ValueTree channel)
{
    engine.capturePluginStatesToTree();
    auto copy = channel.createCopy();
    copy.setProperty (IDs::id, Model::newId(), nullptr);
    copy.setProperty (IDs::name,
                      Model::uniqueChannelName (project, channel.getProperty (IDs::name).toString()),
                      nullptr);
    auto parent = channel.getParent();
    parent.addChild (copy, parent.indexOf (channel) + 1, nullptr);
}

juce::Array<int> ChannelRack::getSelectedChannelIds() const
{
    juce::Array<int> ordered;
    for (auto* row : rows)
        if (selectedChannels.contains (row->channelId()))
            ordered.add (row->channelId());
    return ordered;
}

bool ChannelRack::isChannelSelected (int channelId) const
{
    return selectedChannels.contains (channelId);
}

void ChannelRack::selectChannel (int channelId, bool additive)
{
    if (! additive)
        selectedChannels.clearQuick();
    selectedChannels.addIfNotAlreadyThere (channelId);
    lastSelectedChannel = channelId;
    for (auto* row : rows)
        row->repaint();
}

void ChannelRack::selectAtContainerPoint (juce::Point<int> p)
{
    for (auto* row : rows)
    {
        if (! row->getBounds().contains (p))
            continue;
        auto local = p - row->getPosition();
        if (row->getSelectArea().contains (local))
            selectChannel (row->channelId(), true);
        return;
    }
}

void ChannelRack::refreshPatternBox()
{
    if (! Model::findPatternById (project, selectedPatternId).isValid())
    {
        auto patterns = Model::getPatterns (project);
        selectedPatternId = patterns.getNumChildren() > 0
                                ? (int) patterns.getChild (0).getProperty (IDs::id)
                                : engine.getCurrentPatternId();
    }
    for (auto* row : rows)
        row->repaint();
}

void ChannelRack::rebuildRows()
{
    int prevCount = rows.size();
    rows.clear();
    auto channels = Model::getChannels (project);
    for (int i = 0; i < channels.getNumChildren(); ++i)
    {
        auto c = channels.getChild (i);
        if (! c.hasType (IDs::Channel))
            continue;
        auto* row = new Row (*this, c);
        rows.add (row);
        rowContainer.addAndMakeVisible (row);
    }
    resized();
    rowContainer.repaint();
    if (rows.size() != prevCount && onContentHeightChanged)
        onContentHeightChanged();
}

void ChannelRack::showAddChannelMenu()
{
    auto instruments = host.getInstruments();

    std::vector<std::pair<int, juce::String>> extras;
    extras.push_back ({ 1, "Load sample..." });
    extras.push_back ({ 2, "Built-in Piano" });

    PluginPickerPopup::show (addChannelBtn, instruments, extras,
        [safe = juce::Component::SafePointer<ChannelRack> (this)] (const juce::PluginDescription& desc)
        {
            if (safe == nullptr)
                return;
            auto resolved = desc;
            juce::String err;
            if (! safe->host.resolveForLoad (resolved, err))
            {
                Dialogs::showInfoAsync (juce::MessageBoxIconType::WarningIcon,
                    "Plugin failed to load", err);
                return;
            }
            juce::String xml;
            if (auto x = resolved.createXml())
                xml = x->toString();
            auto c = Model::addPluginChannel (safe->project,
                Model::uniqueChannelName (safe->project, resolved.name), xml);
            auto key = "c" + juce::String ((int) c.getProperty (IDs::id));
            if (safe->onLocalVstAdded)
                safe->onLocalVstAdded (key, resolved.name);
            if (safe->onOpenPluginEditor)
                safe->onOpenPluginEditor (key, resolved.name);
        },
        [safe = juce::Component::SafePointer<ChannelRack> (this)] (int id)
        {
            if (safe == nullptr)
                return;
            if (id == 2)
            {
                Model::addBuiltinChannel (safe->project, "Piano");
                return;
            }
            if (id != 1)
                return;
            safe->chooser = std::make_unique<juce::FileChooser> ("Load sample",
                juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
            safe->chooser->launchAsync (juce::FileBrowserComponent::openMode
                                        | juce::FileBrowserComponent::canSelectFiles,
                [safe] (const juce::FileChooser& fc)
                {
                    if (safe != nullptr && fc.getResult().existsAsFile())
                        safe->addSamplerFromFile (fc.getResult());
                });
        },
        isPluginEnabled, isPluginNew);
}

void ChannelRack::addSamplerFromFile (const juce::File& f)
{
    Model::addSamplerChannel (project, f.getFileNameWithoutExtension(), f.getFullPathName());
}

void ChannelRack::insertSamplerAt (const juce::File& f, int index)
{
    auto c = Model::addSamplerChannel (project, f.getFileNameWithoutExtension(), f.getFullPathName());
    auto channels = Model::getChannels (project);
    channels.moveChild (channels.indexOf (c),
                        juce::jlimit (0, channels.getNumChildren() - 1, index), nullptr);
}

void ChannelRack::importMidiToChannel (const juce::File& midi, juce::ValueTree channel)
{
    bool isNewChannel = ! channel.isValid();
    if (isNewChannel)
        channel = Model::addSamplerChannel (project, midi.getFileNameWithoutExtension(), {});

    auto pattern = currentPattern();
    if (! pattern.isValid())
        return;

    int channelId = (int) channel.getProperty (IDs::id);
    auto seq = Model::getChannelSeq (pattern, channelId, true);

    for (int i = seq.getNumChildren(); --i >= 0;)
        if (seq.getChild (i).hasType (IDs::Note))
            seq.removeChild (i, nullptr);

    int imported = MidiImport::applyToChannelSeq (midi, pattern, seq);
    if (imported > 0)
    {
        Model::addMidiResource (project, midi);
        if (isNewChannel && onOpenPianoRoll)
            onOpenPianoRoll ((int) pattern.getProperty (IDs::id), channelId);
    }
    else
        Dialogs::showInfoAsync (juce::MessageBoxIconType::WarningIcon,
            "Import MIDI", "No notes found in " + midi.getFileName());
}

void ChannelRack::replaceInstrument (juce::ValueTree channel, const juce::PluginDescription& desc)
{
    if (! channel.isValid())
        return;

    juce::String xml;
    if (auto x = desc.createXml())
        xml = x->toString();

    channel.setProperty (IDs::type, "plugin", nullptr);
    channel.setProperty (IDs::pluginDesc, xml, nullptr);
    channel.removeProperty (IDs::pluginState, nullptr);
    channel.removeProperty (IDs::samplePath, nullptr);
    channel.setProperty (IDs::name,
        channel.getProperty (IDs::name).toString() == desc.name
            ? desc.name
            : Model::uniqueChannelName (project, desc.name), nullptr);

    auto key = "c" + juce::String ((int) channel.getProperty (IDs::id));
    auto name = desc.name;
    if (onLocalVstAdded)
        onLocalVstAdded (key, name);
    juce::Timer::callAfterDelay (450,
        [safe = juce::Component::SafePointer<ChannelRack> (this), key, name]
        {
            if (safe != nullptr && safe->onOpenPluginEditor)
                safe->onOpenPluginEditor (key, name);
        });
}

void ChannelRack::pickSampleFor (juce::ValueTree channel)
{
    chooser = std::make_unique<juce::FileChooser> ("Load sample",
        juce::File::getSpecialLocation (juce::File::userMusicDirectory),
        "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
    chooser->launchAsync (juce::FileBrowserComponent::openMode
                          | juce::FileBrowserComponent::canSelectFiles,
        [channel] (const juce::FileChooser& fc) mutable
        {
            auto f = fc.getResult();
            if (f.existsAsFile())
            {
                channel.setProperty (IDs::samplePath, f.getFullPathName(), nullptr);
                channel.setProperty (IDs::name, f.getFileNameWithoutExtension(), nullptr);
            }
        });
}

void ChannelRack::toggleChannelSolo (int channelId)
{
    auto channels = Model::getChannels (project);
    auto target = Model::findChannelById (project, channelId);
    if (! target.isValid())
        return;

    juce::String name = target.getProperty (IDs::name).toString();
    if (name.isEmpty())
        name = "Channel";
    juce::String label = "Channel Rack channel \"" + name + "\"";

    if (soloedChannelId == channelId)
    {
        for (int i = 0; i < channels.getNumChildren(); ++i)
        {
            auto ch = channels.getChild (i);
            if (ch.hasType (IDs::Channel))
                ch.setProperty (IDs::mute, false, nullptr);
        }
        soloedChannelId = 0;
        if (onSoloToggled)
            onSoloToggled (label, false);
        return;
    }

    for (int i = 0; i < channels.getNumChildren(); ++i)
    {
        auto ch = channels.getChild (i);
        if (ch.hasType (IDs::Channel))
            ch.setProperty (IDs::mute, (int) ch.getProperty (IDs::id) != channelId, nullptr);
    }
    soloedChannelId = channelId;
    if (onSoloToggled)
        onSoloToggled (label, true);
}

bool ChannelRack::isInterestedInDragSource (const SourceDetails& d)
{
    return d.description.toString() == "browser-file";
}

void ChannelRack::itemDropped (const SourceDetails& d)
{
    const int insertAt = dragInsertIndex;
    clearDragHover();
    if (getBrowserFile == nullptr)
        return;
    auto f = getBrowserFile();
    if (! f.existsAsFile())
        return;

    auto posInContainer = rowContainer.getLocalPoint (this, d.localPosition);
    Row* hitRow = nullptr;
    for (auto* row : rows)
        if (row->getBounds().contains (posInContainer))
            { hitRow = row; break; }

    if (BrowserPanel::isMidiFile (f))
    {
        importMidiToChannel (f, hitRow != nullptr ? hitRow->channel : juce::ValueTree());
        return;
    }

    if (insertAt >= 0)
    {
        insertSamplerAt (f, insertAt);
        return;
    }

    if (hitRow != nullptr && ! hitRow->isPluginChannel())
    {
        hitRow->channel.setProperty (IDs::samplePath, f.getFullPathName(), nullptr);
        hitRow->channel.setProperty (IDs::name, f.getFileNameWithoutExtension(), nullptr);
        return;
    }
    addSamplerFromFile (f);
}

void ChannelRack::InsertLine::paint (juce::Graphics& g)
{
    g.fillAll (Colors::accent);
}

void ChannelRack::updateDragHover (juce::Point<int> posInRack, bool isMidi, bool isAudio)
{
    int hoverId = 0;
    bool wouldCreate = false;
    int insertIdx = -1;
    const int rowH = 32;

    auto posInContainer = rowContainer.getLocalPoint (this, posInRack);

    if (isAudio && rows.size() > 0
        && posInContainer.y >= -5 && posInContainer.y <= rows.size() * rowH + 5)
    {
        int nearest = juce::jlimit (0, rows.size(), (posInContainer.y + rowH / 2) / rowH);
        if (std::abs (posInContainer.y - nearest * rowH) <= 5)
            insertIdx = nearest;
    }

    Row* hitRow = nullptr;
    if (insertIdx < 0)
        for (auto* row : rows)
            if (row->getBounds().contains (posInContainer))
                { hitRow = row; break; }

    if (insertIdx < 0)
    {
        if (hitRow != nullptr && (isMidi || (isAudio && ! hitRow->isPluginChannel())))
            hoverId = hitRow->channelId();
        else if (isMidi || isAudio)
            wouldCreate = true;
    }

    if (hoverId != dragHoverChannelId || wouldCreate != dragHoverNewChannel
        || insertIdx != dragInsertIndex)
    {
        dragHoverChannelId = hoverId;
        dragHoverNewChannel = wouldCreate;
        dragInsertIndex = insertIdx;
        if (insertIdx >= 0)
            insertLine.setBounds (0, juce::jmax (0, insertIdx * rowH - 1),
                                  rowContainer.getWidth(), 3);
        insertLine.setVisible (insertIdx >= 0);
        addChannelBtn.setColour (juce::TextButton::buttonColourId,
                                 wouldCreate ? Colors::accent.withAlpha (0.55f) : Colors::raised);
        rowContainer.repaint();
    }
}

void ChannelRack::clearDragHover()
{
    if (dragHoverChannelId != 0 || dragHoverNewChannel || dragInsertIndex >= 0)
    {
        dragHoverChannelId = 0;
        dragHoverNewChannel = false;
        dragInsertIndex = -1;
        insertLine.setVisible (false);
        addChannelBtn.setColour (juce::TextButton::buttonColourId, Colors::raised);
        rowContainer.repaint();
    }
}

void ChannelRack::itemDragEnter (const SourceDetails& d) { itemDragMove (d); }

void ChannelRack::itemDragMove (const SourceDetails& d)
{
    auto f = getBrowserFile != nullptr ? getBrowserFile() : juce::File();
    updateDragHover (d.localPosition, BrowserPanel::isMidiFile (f), BrowserPanel::isAudioFile (f));
}

void ChannelRack::itemDragExit (const SourceDetails&) { clearDragHover(); }

void ChannelRack::fileDragEnter (const juce::StringArray& files, int x, int y) { fileDragMove (files, x, y); }

void ChannelRack::fileDragMove (const juce::StringArray& files, int x, int y)
{
    bool midi = false, audio = false;
    for (auto& path : files)
    {
        juce::File f (path);
        midi = midi || BrowserPanel::isMidiFile (f);
        audio = audio || BrowserPanel::isAudioFile (f);
    }
    updateDragHover ({ x, y }, midi, audio);
}

void ChannelRack::fileDragExit (const juce::StringArray&) { clearDragHover(); }

bool ChannelRack::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (BrowserPanel::isAudioFile (juce::File (f)) || BrowserPanel::isMidiFile (juce::File (f)))
            return true;
    return false;
}

void ChannelRack::filesDropped (const juce::StringArray& files, int x, int y)
{
    int insertAt = dragInsertIndex;
    clearDragHover();
    auto posInContainer = rowContainer.getLocalPoint (this, juce::Point<int> (x, y));
    Row* hitRow = nullptr;
    for (auto* row : rows)
        if (row->getBounds().contains (posInContainer))
            { hitRow = row; break; }

    for (auto& path : files)
    {
        juce::File f (path);
        if (BrowserPanel::isMidiFile (f))
            importMidiToChannel (f, hitRow != nullptr ? hitRow->channel : juce::ValueTree());
        else if (BrowserPanel::isAudioFile (f))
        {
            if (insertAt >= 0)
                insertSamplerAt (f, insertAt++);
            else if (hitRow != nullptr && ! hitRow->isPluginChannel())
            {
                hitRow->channel.setProperty (IDs::samplePath, f.getFullPathName(), nullptr);
                hitRow->channel.setProperty (IDs::name, f.getFileNameWithoutExtension(), nullptr);
            }
            else
                addSamplerFromFile (f);
        }
    }
}

void ChannelRack::valueTreePropertyChanged (juce::ValueTree& t, const juce::Identifier& p)
{
    if (t.hasType (IDs::Channel))
    {
        for (auto* row : rows)
            if (row->channel == t)
                row->refreshFromTree();
    }
    else if (t.hasType (IDs::ChannelSeq) || t.hasType (IDs::Note))
    {
        for (auto* row : rows)
            row->repaint();
    }
    else if (t.hasType (IDs::Pattern) && p == IDs::name)
    {
        refreshPatternBox();
    }
    else if (t.hasType (IDs::Pattern) && p == IDs::lengthSteps)
    {
        for (auto* row : rows)
            row->repaint();
        if (onContentHeightChanged)
            onContentHeightChanged();
    }
}

void ChannelRack::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child)
{
    if (child.hasType (IDs::Channel) || parent.hasType (IDs::Channels)
        || child.hasType (IDs::Channels) || child.hasType (IDs::Patterns))
    {
        queueRebuild();
    }
    else if (child.hasType (IDs::Pattern) || parent.hasType (IDs::Patterns))
    {
        refreshPatternBox();
    }
    else if (child.hasType (IDs::Note) || child.hasType (IDs::ChannelSeq))
    {
        for (auto* row : rows)
            row->repaint();
    }
}

void ChannelRack::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int)
{
    if (child.hasType (IDs::Channel) && onChannelRemoved)
    {
        auto name = child.getProperty (IDs::name).toString();
        auto type = child.getProperty (IDs::type).toString();
        if (name.isEmpty() && type == "sampler")
            name = juce::File (child.getProperty (IDs::samplePath).toString()).getFileNameWithoutExtension();
        if (name.isEmpty())
            name = "channel";
        onChannelRemoved (name, type);
    }

    valueTreeChildAdded (parent, child);
}

void ChannelRack::valueTreeRedirected (juce::ValueTree&)
{
    refreshPatternBox();
    queueRebuild();
}

void ChannelRack::queueRebuild()
{
    if (rebuildQueued)
        return;
    rebuildQueued = true;
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<ChannelRack> (this)]
    {
        if (safe != nullptr)
        {
            safe->rebuildQueued = false;
            safe->refreshPatternBox();
            safe->rebuildRows();
        }
    });
}

void ChannelRack::paint (juce::Graphics& g)
{
    g.fillAll (Colors::panelBg);

    auto footer = getLocalBounds().removeFromBottom (footerH + 4);
    g.setColour (Colors::header);
    g.fillRect (footer);
    g.setColour (Colors::outline);
    g.drawHorizontalLine (footer.getY(), (float) footer.getX(), (float) footer.getRight());
}

int ChannelRack::preferredContentHeight() const
{
    const int rowH = 32;
    int h = 4 + rows.size() * rowH + footerH + 4;
    if (preferredContentWidth() > getWidth())
        h += viewport.getScrollBarThickness();
    return h;
}

int ChannelRack::widthForSteps (int numSteps) const
{
    return Row::xSteps + Row::stepsWidth (numSteps) + viewport.getScrollBarThickness() + 16;
}

int ChannelRack::preferredContentWidth() const
{
    int numSteps = Model::defaultSteps;
    auto pattern = currentPattern();
    if (pattern.isValid())
        numSteps = (int) pattern.getProperty (IDs::lengthSteps, Model::defaultSteps);
    return widthForSteps (numSteps);
}

int ChannelRack::compactContentWidth() const { return widthForSteps (Model::defaultSteps); }

void ChannelRack::resized()
{
    auto r = getLocalBounds().reduced (4);
    auto footer = r.removeFromBottom (footerH);
    viewport.setBounds (r);

    int bw = juce::jlimit (juce::jmin (160, footer.getWidth()), footer.getWidth(),
                           (int) ((float) getWidth() * 0.4f));
    addChannelBtn.setBounds (footer.withSizeKeepingCentre (bw, 21));

    int numSteps = Model::defaultSteps;
    auto pattern = currentPattern();
    if (pattern.isValid())
        numSteps = (int) pattern.getProperty (IDs::lengthSteps, Model::defaultSteps);

    const int rowH = 32;
    const int neededW  = Row::xSteps + Row::stepsWidth (numSteps) + 8;
    const int contentH = juce::jmax (rows.size() * rowH, 1);

    const int sbThick = viewport.getScrollBarThickness();
    const bool needV = contentH > r.getHeight();
    const int visW = r.getWidth() - (needV ? sbThick : 0);
    const int containerW = juce::jmax (neededW, visW);
    rowContainer.setSize (containerW, contentH);

    for (int i = 0; i < rows.size(); ++i)
        rows[i]->setBounds (0, i * rowH, containerW, rowH);
}
