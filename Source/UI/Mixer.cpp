#include "Mixer.h"
#include "AppLookAndFeel.h"
#include "Dialogs.h"
#include "ValueKnob.h"
#include "PluginPickerPopup.h"
#include "AppSettings.h"
#include "../Model/Automation.h"

namespace
{
    juce::PopupMenu::Options menuAtMouse()
    {
        auto p = juce::Desktop::getInstance().getMainMouseSource().getScreenPosition().roundToInt();
        return juce::PopupMenu::Options().withTargetScreenArea ({ p.x, p.y, 1, 1 });
    }

    class MixerStripLnF : public AppLookAndFeel
    {
    public:
        void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                               float sliderPos, float minSliderPos, float maxSliderPos,
                               juce::Slider::SliderStyle style, juce::Slider& s) override
        {
            if (style != juce::Slider::LinearVertical)
            {
                AppLookAndFeel::drawLinearSlider (g, x, y, width, height, sliderPos,
                                                 minSliderPos, maxSliderPos, style, s);
                return;
            }

            auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();
            float cx = bounds.getCentreX();

            g.setColour (Colors::outline);
            g.fillRoundedRectangle (cx - 2.0f, bounds.getY(), 4.0f, bounds.getHeight(), 2.0f);

            g.setColour (Colors::stepDim.withAlpha (0.5f));
            for (int i = 1; i < 4; ++i)
            {
                float ty = bounds.getY() + bounds.getHeight() * (float) i / 4.0f;
                g.fillRect (cx - 7.0f, ty, 4.0f, 1.0f);
                g.fillRect (cx + 3.0f, ty, 4.0f, 1.0f);
            }

            const bool master = (bool) s.getProperties().getWithDefault ("masterFader", false);
            juce::Rectangle<float> handle (cx - 13.0f, sliderPos - 9.0f, 26.0f, 18.0f);
            g.setColour (master ? Colors::accent2 : Colors::text.darker (0.15f));
            g.fillRoundedRectangle (handle, 3.0f);
            g.setColour (Colors::outline);
            g.drawRoundedRectangle (handle, 3.0f, 1.0f);
            g.setColour (master ? Colors::accent2.darker (0.45f) : Colors::panelDark);
            g.fillRect (handle.getX() + 3.0f, sliderPos - 1.0f, handle.getWidth() - 6.0f, 2.0f);
        }
    };

    const juce::Identifier mixerStateType ("MixerState");
    const juce::Identifier mixerStateVersion ("version");

    juce::String effectDisplayName (const juce::ValueTree& slot)
    {
        auto type = slot.getProperty (IDs::type).toString();
        if (type == "gain") return "Gain";
        if (type == "eq")   return "EQ (3-band)";
        if (type == "softclip") return "Soft Clipper";
        if (type == "delay")  return "Delay";
        if (type == "delaybank") return "Delay Bank";
        if (type == "reverb") return "Reverb";
        if (type == "limiter") return "Limiter";
        if (type == "multiband") return "Multiband Compressor";
        if (type == "soundbeast") return "Soundbeastifier";
        if (type == "void") return "Void";
        if (type == "lowlifter") return "Low Lifter";
        if (type == "parameq") return "Parametric EQ 2";
        if (type == "convolver") return "Convolver";
        if (type == "parterre") return "Parterre";
        if (type == "equivocal") return "Equivocal";
        if (type == "grossbeat") return "Gross Beat";
        if (auto xml = juce::parseXML (slot.getProperty (IDs::pluginDesc).toString()))
        {
            juce::PluginDescription desc;
            if (desc.loadFromXml (*xml))
                return desc.name;
        }
        return "FX";
    }

    void slotMoveOneStep (juce::ValueTree track, juce::ValueTree slot, int delta)
    {
        if (! track.isValid() || ! slot.isValid())
            return;
        int cur = (int) slot.getProperty (IDs::slotIndex);
        int dest = cur + delta;
        if (! juce::isPositiveAndBelow (dest, Model::numSlotsPerTrack))
            return;
        for (int i = 0; i < track.getNumChildren(); ++i)
        {
            auto o = track.getChild (i);
            if (o.hasType (IDs::Slot) && (int) o.getProperty (IDs::slotIndex) == dest)
            { o.setProperty (IDs::slotIndex, cur, nullptr); break; }
        }
        slot.setProperty (IDs::slotIndex, dest, nullptr);
    }

    void slotToggleEnabled (juce::ValueTree slot)
    {
        if (slot.isValid())
            slot.setProperty (IDs::enabled, ! (bool) slot.getProperty (IDs::enabled, true), nullptr);
    }

    void slotRemove (juce::ValueTree slot)
    {
        if (slot.isValid())
            slot.getParent().removeChild (slot, nullptr);
    }

    int firstFreeSlotIndex (juce::ValueTree track)
    {
        for (int idx = 0; idx < Model::numSlotsPerTrack; ++idx)
        {
            bool used = false;
            for (int i = 0; i < track.getNumChildren() && ! used; ++i)
            {
                auto s = track.getChild (i);
                used = s.hasType (IDs::Slot) && (int) s.getProperty (IDs::slotIndex) == idx;
            }
            if (! used)
                return idx;
        }
        return Model::numSlotsPerTrack;
    }
}

class MixerPanel::Strip : public juce::Component
{
public:
    Strip (MixerPanel& m, int trackIdx) : mixer (m), trackIndex (trackIdx)
    {
        auto select = [this] { mixer.selectTrack (trackIndex); };

        panKnob.setRange (-1.0, 1.0, 0.0);
        panKnob.setDoubleClickReturnValue (true, 0.0);
        panKnob.setMouseDragSensitivity (350);
        panKnob.getProperties().set ("bipolar", true);
        panKnob.setRotaryParameters (juce::MathConstants<float>::pi,
                                     juce::MathConstants<float>::pi * 3.0f, true);
        panKnob.setTooltip ("Pan");
        panKnob.onDragStart = select;
        panKnob.onValueChange = [this] { getTrackTree().setProperty (IDs::pan, panKnob.getValue(), nullptr); };
        panKnob.onCreateAutomationClip = [this] { createAutomation ("pan"); };
        addAndMakeVisible (panKnob);

        sepKnob.setRange (-1.0, 1.0, 0.0);
        sepKnob.setDoubleClickReturnValue (true, 0.0);
        sepKnob.setMouseDragSensitivity (350);
        sepKnob.getProperties().set ("bipolar", true);
        sepKnob.setRotaryParameters (juce::MathConstants<float>::pi,
                                     juce::MathConstants<float>::pi * 3.0f, true);
        sepKnob.getProperties().set ("bipolarNegColour", (int) Colors::stereoSeparated.getARGB());
        sepKnob.getProperties().set ("bipolarPosColour", (int) Colors::stereoMerged.getARGB());
        sepKnob.setTooltip ("Stereo separation (left = separated, right = merged)");
        sepKnob.onDragStart = select;
        sepKnob.onValueChange = [this] { getTrackTree().setProperty (IDs::stereoSep, sepKnob.getValue(), nullptr); };
        addAndMakeVisible (sepKnob);

        fader.setSliderStyle (juce::Slider::LinearVertical);
        fader.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        fader.setRange (0.0, 1.25, 0.0);
        fader.setDoubleClickReturnValue (true, 1.0);
        fader.setTooltip ("Volume");
        fader.getProperties().set ("masterFader", trackIndex == 0);
        fader.setLookAndFeel (mixer.stripLnf.get());
        fader.onDragStart = select;
        fader.onValueChange = [this] { getTrackTree().setProperty (IDs::volume, fader.getValue(), nullptr); };
        fader.onCreateAutomationClip = [this] { createAutomation ("volume"); };
        addAndMakeVisible (fader);

        refreshFromTree();
    }

    void createAutomation (const juce::String& param)
    {
        Automation::createClip (mixer.project,
            "track." + juce::String (trackIndex) + "." + param);
    }

    void updateAutoFollow()
    {
        float av = mixer.engine.getTrackAutoVolume (trackIndex);
        float ap = mixer.engine.getTrackAutoPan (trackIndex);
        bool following = av >= 0.0f || ap >= -1.5f;

        if (av >= 0.0f && ! fader.isMouseButtonDown())
            fader.setValue (av, juce::dontSendNotification);
        if (ap >= -1.5f && ! panKnob.isMouseButtonDown())
            panKnob.setValue (ap, juce::dontSendNotification);

        if (! following && wasFollowingAuto)
            refreshFromTree();
        wasFollowingAuto = following;
    }

    ~Strip() override
    {
        fader.setLookAndFeel (nullptr);
    }

    juce::ValueTree getTrackTree() const  { return mixer.trackTree (trackIndex); }

    void refreshFromTree()
    {
        auto t = getTrackTree();
        if (! t.isValid())
            return;
        fader.setValue   ((double) t.getProperty (IDs::volume, 1.0), juce::dontSendNotification);
        panKnob.setValue ((double) t.getProperty (IDs::pan, 0.0), juce::dontSendNotification);
        sepKnob.setValue ((double) t.getProperty (IDs::stereoSep, 0.0), juce::dontSendNotification);

        bool off = (bool) t.getProperty (IDs::mute, false);
        float a = off ? 0.45f : 1.0f;
        panKnob.setAlpha (a);
        sepKnob.setAlpha (a);
        fader.setAlpha (a);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().reduced (1);
        bool selected = mixer.selectedTrack == trackIndex;
        auto t = getTrackTree();

        auto colStr = t.getProperty (IDs::colour).toString();
        juce::Colour tint = colStr.isNotEmpty() ? juce::Colour::fromString (colStr) : juce::Colour();
        juce::Colour base = trackIndex == 0 ? Colors::raised.darker (0.1f) : Colors::panelDark;

        g.setColour (colStr.isNotEmpty() ? base.interpolatedWith (tint.darker (0.2f), 0.30f) : base);
        g.fillRoundedRectangle (r.toFloat(), 3.0f);
        if (selected)
        {
            g.setColour (Colors::accent.withAlpha (0.9f));
            g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 3.0f, 1.4f);
        }

        auto header = getHeaderArea();
        g.setColour (colStr.isNotEmpty() ? tint.withAlpha (selected ? 0.55f : 0.4f)
                                         : (selected ? Colors::accent.withAlpha (0.25f) : Colors::header));
        g.fillRoundedRectangle (header.toFloat().reduced (1.0f), 2.0f);
        if (colStr.isNotEmpty())
        {
            g.setColour (tint);
            g.fillRoundedRectangle (r.toFloat().withHeight (3.0f), 1.5f);
        }
        g.setColour (selected ? Colors::accent : Colors::textDim);
        g.setFont (Typography::bold (10.0f));
        g.drawText (trackIndex == 0 ? juce::String ("M") : juce::String (trackIndex),
                    header, juce::Justification::centred);

        auto meterRegion = getMeterRegion();
        g.setColour (Colors::outline);
        g.fillRoundedRectangle (meterRegion.toFloat(), 2.0f);

        {
            auto nameArea = meterRegion.withTrimmedRight (meterBarW + 2);
            juce::Graphics::ScopedSaveState ss (g);
            float cx = (float) nameArea.getCentreX(), cy = (float) nameArea.getCentreY();
            g.addTransform (juce::AffineTransform::rotation (-juce::MathConstants<float>::halfPi, cx, cy));
            g.setColour (Colors::text);
            g.setFont (Typography::semi (13.0f));
            juce::Rectangle<int> rot ((int) (cx - nameArea.getHeight() / 2.0f), (int) (cy - nameArea.getWidth() / 2.0f),
                                      nameArea.getHeight(), nameArea.getWidth());
            g.drawText (t.getProperty (IDs::name).toString(), rot.reduced (4, 0),
                        juce::Justification::centredLeft);
        }

        {
            auto dot = getToggleArea().withSizeKeepingCentre (10, 10).toFloat();
            bool on = ! (bool) t.getProperty (IDs::mute, false);
            g.setColour (on ? Colors::noteGreen : Colors::stepDim);
            g.fillEllipse (dot);
            if (on)
            {
                g.setColour (Colors::noteGreen.withAlpha (0.35f));
                g.drawEllipse (dot.expanded (2.0f), 1.5f);
            }
            g.setColour (Colors::outline);
            g.drawEllipse (dot, 1.0f);
        }

        if (trackIndex > 0)
        {
            auto dot = getRecArmArea().withSizeKeepingCentre (12, 12).toFloat();
            bool armed = (bool) t.getProperty (IDs::recArm, false);
            bool hasInput = t.getProperty (IDs::audioInput).toString().trim().isNotEmpty();
            if (armed)
            {
                g.setColour (hasInput ? Colors::record : Colors::record.withAlpha (0.5f));
                g.fillEllipse (dot);
            }
            else
            {
                g.setColour (Colors::record.withAlpha (hasInput ? 0.4f : 0.18f));
                g.fillEllipse (dot);
            }
            if (hasInput)
            {
                g.setColour (Colors::record.withAlpha (armed ? 0.5f : 0.35f));
                g.drawEllipse (dot.expanded (2.0f), 1.4f);
            }
            g.setColour (Colors::outline);
            g.drawEllipse (dot, 1.0f);
        }

        {
            auto route = getRouteArea();
            int src = mixer.selectedTrack;
            bool amSelected = (src == trackIndex);
            bool socket = isRouteTarget (trackIndex, src);

            g.setColour (selected ? Colors::raised.brighter (0.1f) : Colors::raised);
            g.fillRoundedRectangle (route.toFloat().reduced (2.0f), 3.0f);

            if (socket)
            {
                bool lit = (int) mixer.trackTree (src).getProperty (IDs::routeTo, 0) == trackIndex;
                juce::Colour c = lit ? Colors::accent
                                     : (routeHover ? Colors::accent.withAlpha (0.85f)
                                                   : Colors::textDim.withAlpha (0.65f));
                float cx = (float) route.getCentreX(), cy = (float) route.getCentreY();

                juce::Path plug;
                plug.addRoundedRectangle (cx - 1.8f, cy - 8.0f, 3.6f, 6.0f, 1.2f);
                plug.addTriangle (cx - 5.0f, cy - 3.0f, cx + 5.0f, cy - 3.0f, cx, cy + 4.0f);
                g.setColour (c);
                g.fillPath (plug);

                juce::Path cup;
                cup.addCentredArc (cx, cy + 4.5f, 7.0f, 5.5f, 0.0f,
                                   juce::MathConstants<float>::pi * 0.5f,
                                   juce::MathConstants<float>::pi * 1.5f, true);
                g.setColour (lit ? c : c.withAlpha (0.75f));
                g.strokePath (cup, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved,
                                                         juce::PathStrokeType::rounded));
                if (lit)
                {
                    g.setColour (Colors::accent.withAlpha (0.35f));
                    g.drawRoundedRectangle (route.toFloat().reduced (3.0f), 3.0f, 1.4f);
                }
            }
            else if (trackIndex == 0)
            {
                g.setColour (Colors::textDim);
                g.setFont (Typography::caps (9.0f));
                g.drawText ("OUT", route, juce::Justification::centred);
            }
            else if (amSelected)
            {
                int dest = (int) t.getProperty (IDs::routeTo, 0);
                if (dest < 0)
                {
                    g.setColour (Colors::textDim.withAlpha (0.6f));
                    g.fillRect ((float) route.getCentreX() - 5.0f,
                                (float) route.getCentreY() - 1.0f, 10.0f, 2.0f);
                }
                else
                {
                    float cx = (float) route.getCentreX() - 12.0f, cy = (float) route.getCentreY();
                    juce::Path plug;
                    plug.addRoundedRectangle (cx - 1.3f, cy - 6.0f, 2.6f, 4.5f, 1.0f);
                    plug.addTriangle (cx - 4.0f, cy - 2.0f, cx + 4.0f, cy - 2.0f, cx, cy + 3.5f);
                    g.setColour (Colors::textDim);
                    g.fillPath (plug);
                    juce::Path cup;
                    cup.addCentredArc (cx, cy + 3.5f, 5.5f, 4.0f, 0.0f,
                                       juce::MathConstants<float>::pi * 0.5f,
                                       juce::MathConstants<float>::pi * 1.5f, true);
                    g.strokePath (cup, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved,
                                                             juce::PathStrokeType::rounded));
                    g.setColour (Colors::text);
                    g.setFont (Typography::semi (9.0f));
                    g.drawText (mixer.routeLabelFor (trackIndex),
                                route.withTrimmedLeft (route.getWidth() / 2 - 4),
                                juce::Justification::centred);
                }
            }
        }

        if ((bool) t.getProperty (IDs::mute, false))
        {
            g.setColour (Colors::panelDark.withAlpha (0.55f));
            g.fillRoundedRectangle (r.toFloat(), 3.0f);
        }

        {
            auto meter = getMeterArea();
            float peak = mixer.engine.getTrackPeak (trackIndex);
            constexpr float minDb = -48.0f, maxDb = 6.0f;
            auto dbToY = [&] (float db)
            {
                float frac = juce::jlimit (0.0f, 1.0f, (db - minDb) / (maxDb - minDb));
                return meter.getBottom() - (int) (frac * (float) meter.getHeight());
            };
            int yMid = dbToY (-12.0f);
            int yHot = dbToY (0.0f);

            g.setColour (Colors::meterHot.withAlpha (0.10f));
            g.fillRect (meter.withBottom (yHot));
            g.setColour (Colors::stepDim.withAlpha (0.6f));
            g.fillRect (meter.getX(), dbToY (-24.0f), meter.getWidth(), 1);
            g.fillRect (meter.getX(), yMid, meter.getWidth(), 1);
            g.setColour (Colors::textDim.withAlpha (0.7f));
            g.fillRect (meter.getX(), yHot, meter.getWidth(), 1);

            float curDb = juce::Decibels::gainToDecibels (peak, minDb);
            int top = dbToY (curDb);
            if (top < meter.getBottom())
            {
                g.setColour (Colors::noteGreen);
                g.fillRect (meter.withTop (juce::jmax (top, yMid)));
                if (top < yMid)
                {
                    g.setColour (Colors::meterMid);
                    g.fillRect (meter.withTop (juce::jmax (top, yHot)).withBottom (yMid));
                }
                if (top < yHot)
                {
                    g.setColour (Colors::meterHot);
                    g.fillRect (meter.withTop (top).withBottom (yHot));
                }
            }

            auto now = juce::Time::getMillisecondCounter();
            float dt = lastMeterMs == 0 ? 0.0f : (float) (now - lastMeterMs) / 1000.0f;
            lastMeterMs = now;
            if (curDb >= heldDb)
            {
                heldDb = curDb;
                heldSinceMs = now;
            }
            else if (now - heldSinceMs > 1500)
            {
                heldDb = juce::jmax (curDb, heldDb - 30.0f * dt);
            }
            if (heldDb > minDb + 0.5f)
            {
                int hy = juce::jlimit (meter.getY(), meter.getBottom() - 2, dbToY (heldDb) - 1);
                g.setColour (heldDb >= 0.0f ? Colors::meterHot
                             : heldDb >= -12.0f ? Colors::meterMid : Colors::noteGreen);
                g.fillRect (meter.getX(), hy, meter.getWidth(), 2);
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown())
            return;

        if (isRouteTarget (trackIndex, mixer.selectedTrack)
            && getRouteArea().contains (e.getPosition()))
        {
            int src = mixer.selectedTrack;
            if (e.mods.isPopupMenu())
            {
                mixer.showRoutingMenuFor (src, this);
                return;
            }
            auto s = mixer.trackTree (src);
            int current = (int) s.getProperty (IDs::routeTo, 0);
            int target = (current == trackIndex) ? -1 : trackIndex;
            s.setProperty (IDs::routeTo, target, nullptr);
            return;
        }

        mixer.selectTrack (trackIndex);

        if (trackIndex > 0 && getRecArmArea().contains (e.getPosition()))
        {
            auto t = getTrackTree();
            if (e.mods.isPopupMenu())
            {
                showInputMenu (false);
            }
            else
            {
                bool armed = (bool) t.getProperty (IDs::recArm, false);
                bool hasInput = t.getProperty (IDs::audioInput).toString().trim().isNotEmpty();
                if (! armed && ! hasInput)
                    showInputMenu (true);
                else
                    t.setProperty (IDs::recArm, ! armed, nullptr);
            }
            return;
        }

        if (getToggleArea().expanded (2).contains (e.getPosition()))
        {
            auto t = getTrackTree();
            if (e.mods.isPopupMenu() && trackIndex > 0)
                toggleSolo();
            else
                t.setProperty (IDs::mute, ! (bool) t.getProperty (IDs::mute, false), nullptr);
            return;
        }

        if (trackIndex > 0 && getRouteArea().contains (e.getPosition()))
        {
            mixer.showRoutingMenuFor (trackIndex, this);
            return;
        }

        if (e.mods.isPopupMenu() && trackIndex > 0)
            showChannelRoutingMenu();
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        bool over = isRouteTarget (trackIndex, mixer.selectedTrack)
                    && getRouteArea().contains (e.getPosition());
        if (over != routeHover)
        {
            routeHover = over;
            repaint (getRouteArea());
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (routeHover)
        {
            routeHover = false;
            repaint (getRouteArea());
        }
    }

    static bool isRouteTarget (int targetIdx, int src)
    {
        return src > 0 && targetIdx != src && (targetIdx == 0 || targetIdx > src);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (3);
        r.removeFromTop (headerH);

        int fixed = toggleH + panH + sepH + recArmH + routeH;
        int flexible = juce::jmax (0, r.getHeight() - fixed);
        int meterH = juce::jmax (60, (int) (flexible * 0.42f));

        r.removeFromTop (meterH);
        r.removeFromTop (toggleH);
        panKnob.setBounds (r.removeFromTop (panH).withSizeKeepingCentre (30, 30));
        sepKnob.setBounds (r.removeFromTop (sepH).withSizeKeepingCentre (20, 20));
        r.removeFromTop (recArmH);
        r.removeFromBottom (routeH);
        fader.setBounds (r.reduced (0, 2));
    }

    juce::Rectangle<int> getHeaderArea() const
    {
        return { 3, 3, getWidth() - 6, headerH - 2 };
    }

    juce::Rectangle<int> getMeterRegion() const
    {
        auto r = getLocalBounds().reduced (3);
        r.removeFromTop (headerH);
        int fixed = toggleH + panH + sepH + recArmH + routeH;
        int flexible = juce::jmax (0, r.getHeight() - fixed);
        return r.removeFromTop (juce::jmax (60, (int) (flexible * 0.42f))).reduced (1, 0);
    }

    juce::Rectangle<int> getMeterArea() const
    {
        return getMeterRegion().removeFromRight (meterBarW).reduced (1, 2);
    }

    juce::Rectangle<int> getToggleArea() const
    {
        auto r = getMeterRegion();
        return { 3, r.getBottom(), getWidth() - 6, toggleH };
    }

    juce::Rectangle<int> getRouteArea() const
    {
        return { 3, getHeight() - 3 - routeH, getWidth() - 6, routeH };
    }

    juce::Rectangle<int> getRecArmArea() const
    {
        auto r = getToggleArea();
        return { 3, r.getBottom() + panH + sepH, getWidth() - 6, recArmH };
    }

    bool isMeterActive() const
    {
        float peak = mixer.engine.getTrackPeak (trackIndex);
        float curDb = juce::Decibels::gainToDecibels (peak, -48.0f);
        return curDb > -47.5f || heldDb > -47.5f;
    }

    static constexpr int headerH = 16, toggleH = 16, panH = 34,
                         sepH = 26, routeH = 26, recArmH = 16, meterBarW = 16;

    const int trackIndex;
    bool wasMeterActive = false;

private:
    float heldDb = -100.0f;
    juce::uint32 heldSinceMs = 0, lastMeterMs = 0;
    bool routeHover = false;
    bool wasFollowingAuto = false;

    void toggleSolo()
    {
        auto current = getTrackTree();
        bool alreadySoloed = (bool) current.getProperty (IDs::solo, false);
        int maxIdx = Model::getMaxMixerTrackIndex (mixer.project);

        juce::String label = trackIndex == 0 ? juce::String ("Mixer Master")
                                             : "Mixer Insert " + juce::String (trackIndex);
        juce::String nm = mixer.trackName (trackIndex);
        juce::String def = trackIndex == 0 ? juce::String ("Master")
                                           : "Insert " + juce::String (trackIndex);
        if (nm.isNotEmpty() && nm != def)
            label += " \"" + nm + "\"";

        if (alreadySoloed)
        {
            for (int i = 0; i <= maxIdx; ++i)
            {
                auto t = mixer.trackTree (i);
                if (t.isValid())
                {
                    t.setProperty (IDs::solo, false, nullptr);
                    if (i > 0)
                        t.setProperty (IDs::mute, false, nullptr);
                }
            }
            if (mixer.onSoloToggled)
                mixer.onSoloToggled (label, false);
            return;
        }

        for (int i = 0; i <= maxIdx; ++i)
        {
            auto t = mixer.trackTree (i);
            if (t.isValid())
            {
                bool isTarget = i == trackIndex;
                t.setProperty (IDs::solo, isTarget, nullptr);
                if (i > 0)
                    t.setProperty (IDs::mute, ! isTarget, nullptr);
            }
        }
        if (mixer.onSoloToggled)
            mixer.onSoloToggled (label, true);
    }

    void showChannelRoutingMenu()
    {
        juce::Array<int> sel;
        if (mixer.getSelectedChannels)
            sel = mixer.getSelectedChannels();

        juce::PopupMenu m;
        m.addSectionHeader (getTrackTree().getProperty (IDs::name).toString());
        m.addItem (1, "Route Selected Channel to Insert", sel.size() >= 1);
        m.addItem (2, "Route Selected Channels to own Inserts", sel.size() >= 2);
        m.addSeparator();
        m.addItem (3, "Save mixer state as...");
        m.addItem (4, "Load mixer state...");

        m.showMenuAsync (menuAtMouse(),
            [safe = juce::Component::SafePointer<Strip> (this), sel] (int result)
            {
                if (safe == nullptr || result == 0)
                    return;

                if (result == 3) { safe->mixer.exportMixerState (safe->trackIndex); return; }
                if (result == 4) { safe->mixer.importMixerState (safe->trackIndex); return; }

                if (safe->mixer.routeChannelToInsert == nullptr)
                    return;

                auto nameInsert = [] (MixerPanel& mx, int channelId, int insertIndex)
                {
                    auto tt = mx.trackTree (insertIndex);
                    if (! tt.isValid()
                        || tt.getProperty (IDs::name).toString() != "Insert " + juce::String (insertIndex))
                        return;
                    if (auto ch = Model::findChannelById (mx.project, channelId); ch.isValid())
                        tt.setProperty (IDs::name, ch.getProperty (IDs::name), nullptr);
                    tt.setProperty (IDs::colour, Model::channelColour (channelId).toString(), nullptr);
                };

                if (result == 1)
                {
                    for (int id : sel)
                    {
                        safe->mixer.routeChannelToInsert (id, safe->trackIndex);
                        nameInsert (safe->mixer, id, safe->trackIndex);
                    }
                }
                else if (result == 2)
                {
                    int maxIdx = Model::getMaxMixerTrackIndex (safe->mixer.project);
                    for (int i = 0; i < sel.size(); ++i)
                    {
                        int insert = safe->trackIndex + i;
                        if (insert > maxIdx)
                            break;
                        safe->mixer.routeChannelToInsert (sel[i], insert);
                        nameInsert (safe->mixer, sel[i], insert);
                    }
                }
            });
    }

    void showInputMenu (bool armAfterSelection)
    {
        auto t = getTrackTree();
        auto* dev = mixer.engine.deviceManager.getCurrentAudioDevice();
        juce::StringArray names = dev != nullptr ? dev->getInputChannelNames() : juce::StringArray();
        auto current = t.getProperty (IDs::audioInput).toString().trim();

        juce::PopupMenu m;
        m.addSectionHeader (dev != nullptr ? dev->getName() : juce::String ("No audio device"));

        if (names.isEmpty())
        {
            m.addItem (1, "No audio inputs", false, false);
            m.showMenuAsync (menuAtMouse(), [] (int) {});
            return;
        }

        m.addItem (1000, "(none)", true, current.isEmpty());
        for (int i = 0; i < names.size(); ++i)
            m.addItem (1 + i, "In " + juce::String (i + 1), true, current == juce::String (i));
        if (names.size() >= 2)
        {
            m.addSeparator();
            for (int i = 0; i + 1 < names.size(); ++i)
            {
                auto val = juce::String (i) + "," + juce::String (i + 1);
                m.addItem (500 + i, "In " + juce::String (i + 1) + " - In " + juce::String (i + 2),
                           true, current == val);
            }
        }

        m.showMenuAsync (menuAtMouse(),
            [safe = juce::Component::SafePointer<Strip> (this), armAfterSelection] (int r)
            {
                if (safe == nullptr || r == 0)
                    return;
                auto tt = safe->getTrackTree();
                juce::String val;
                if (r == 1000)      val = "";
                else if (r >= 500)  { int i = r - 500; val = juce::String (i) + "," + juce::String (i + 1); }
                else                { int i = r - 1;   val = juce::String (i); }
                tt.setProperty (IDs::audioInput, val, nullptr);
                if (armAfterSelection && val.isNotEmpty())
                    tt.setProperty (IDs::recArm, true, nullptr);
            });
    }

    MixerPanel& mixer;
    ValueKnob panKnob, sepKnob;
    ValueSlider fader;
};

class MixerPanel::AddStripButton : public juce::Component
{
public:
    explicit AddStripButton (MixerPanel& m) : mixer (m) {}

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().reduced (1);
        g.setColour (hover ? Colors::raised : Colors::card.withAlpha (0.6f));
        g.fillRoundedRectangle (r.toFloat(), Metrics::rM);
        g.setColour (hover ? Colors::accent : Colors::textDim);
        g.setFont (Typography::bold (18.0f));
        g.drawText ("+", r.removeFromTop (60), juce::Justification::centred);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown())
            return;
        auto t = Model::addMixerTrack (mixer.project);
        if (t.isValid())
            mixer.selectTrack ((int) t.getProperty (IDs::index));
    }

    void mouseEnter (const juce::MouseEvent&) override { hover = true;  repaint(); }
    void mouseExit  (const juce::MouseEvent&) override { hover = false; repaint(); }

private:
    MixerPanel& mixer;
    bool hover = false;
};

class MixerPanel::EffectRack : public juce::Component
{
public:
    static constexpr int rackWidth = 226;

    explicit EffectRack (MixerPanel& m) : mixer (m)
    {
        slotViewport.setViewedComponent (&slotList, false);
        slotViewport.setScrollBarsShown (true, false);
        slotViewport.setScrollBarThickness (8);
        addAndMakeVisible (slotViewport);

        for (int i = 0; i < Model::numSlotsPerTrack; ++i)
            slotList.addAndMakeVisible (rows.add (new SlotRow (*this, i)));
    }

    juce::ValueTree currentTrack() const   { return mixer.trackTree (mixer.selectedTrack); }

    void refresh()
    {
        for (auto* r : rows)
            r->refresh();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        g.setColour (Colors::panelDark);
        g.fillRoundedRectangle (r.toFloat(), Metrics::rL);

        auto header = r.removeFromTop (headerH);
        g.setColour (Colors::header);
        g.fillRoundedRectangle (header.toFloat().reduced (2.0f), Metrics::rS);
        g.setColour (Colors::text);
        g.setFont (Typography::semi (13.5f));
        g.drawText (mixer.trackName (mixer.selectedTrack), header.reduced (8, 0),
                    juce::Justification::centredLeft);

        g.setColour (Colors::windowBg);
        g.fillRoundedRectangle (slotBox.toFloat(), Metrics::rM);

        auto out = getOutputArea();
        g.setColour (Colors::raised);
        g.fillRoundedRectangle (out.toFloat().reduced (2.0f), Metrics::rM);
        g.setColour (Colors::textDim);
        g.setFont (Typography::caps (9.5f));
        g.drawText ("OUT", out.removeFromLeft (40).reduced (8, 0), juce::Justification::centredLeft);
        g.setColour (Colors::text);
        g.setFont (Typography::ui (11.0f));
        int outDest = (int) currentTrack().getProperty (IDs::routeTo, 0);
        juce::String target = mixer.selectedTrack == 0
            ? juce::String ("Out 1 - Out 2")
            : (outDest < 0 ? juce::String ("(none)")
                           : outDest == 0 ? juce::String ("Master")
                                          : mixer.trackName (outDest));
        g.drawText (target, out.reduced (4, 0), juce::Justification::centredLeft);
        if (mixer.selectedTrack > 0)
        {
            g.setColour (Colors::textDim);
            g.drawText ("...", getOutputArea().removeFromRight (22), juce::Justification::centred);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        endWheelTrack();
        if (e.mods.isMiddleButtonDown())
            return;
        if (mixer.selectedTrack > 0 && getOutputArea().contains (e.getPosition()))
            mixer.showRoutingMenuFor (mixer.selectedTrack, this);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (4);
        r.removeFromTop (headerH + 2);
        r.removeFromBottom (30);
        slotBox = r;

        auto inner = r.reduced (4);
        slotViewport.setBounds (inner);

        int rowH = juce::jlimit (22, 30, inner.getHeight() / juce::jmax (1, rows.size()));
        int contentH = juce::jmax (inner.getHeight(), rowH * rows.size());
        int contentW = inner.getWidth()
                     - (contentH > inner.getHeight() ? slotViewport.getScrollBarThickness() : 0);
        slotList.setSize (contentW, contentH);

        juce::Rectangle<int> lane (0, 0, contentW, contentH);
        for (auto* row : rows)
            row->setBounds (lane.removeFromTop (rowH).reduced (0, 1));
    }

private:
    juce::Rectangle<int> getOutputArea() const
    {
        return getLocalBounds().reduced (4).removeFromBottom (26);
    }

    void moveSlotOneStep (juce::ValueTree s, int delta)
    {
        slotMoveOneStep (currentTrack(), s, delta);
    }

    void setAllRowCursors (const juce::MouseCursor& c)
    {
        setMouseCursor (c);
        slotViewport.setMouseCursor (c);
        slotList.setMouseCursor (c);
        for (auto* r : rows)
            r->setMouseCursor (c);
        juce::Desktop::getInstance().getMainMouseSource().forceMouseCursorUpdate();
    }

    void beginWheelTrack (juce::ValueTree s, juce::Point<int> screenPos)
    {
        wheelTrackedSlot   = s;
        wheelTrackScreenPos = screenPos;
        wheelTracking      = true;
        setAllRowCursors (juce::MouseCursor::NoCursor);
    }

    void endWheelTrack()
    {
        if (! wheelTracking)
            return;
        wheelTracking    = false;
        wheelTrackedSlot = {};
        setAllRowCursors (juce::MouseCursor::NormalCursor);
    }

    void maybeEndWheelTrack (juce::Point<int> screenPos)
    {
        if (! wheelTracking)
            return;
        if (std::abs (screenPos.x - wheelTrackScreenPos.x) > 3
            || std::abs (screenPos.y - wheelTrackScreenPos.y) > 3)
            endWheelTrack();
    }

    class SlotRow : public juce::Component
    {
    public:
        SlotRow (EffectRack& r, int idx) : rack (r), slotIndex (idx)
        {
            wetKnob.setRange (0.0, 1.0, 0.0);
            wetKnob.setDoubleClickReturnValue (true, 1.0);
            wetKnob.setMouseDragSensitivity (300);
            wetKnob.onValueChange = [this]
            {
                if (slot.isValid())
                    slot.setProperty (IDs::mix, wetKnob.getValue(), nullptr);
            };
            addChildComponent (wetKnob);
        }

        void refresh()
        {
            slot = {};
            auto t = rack.currentTrack();
            for (int i = 0; i < t.getNumChildren(); ++i)
            {
                auto s = t.getChild (i);
                if (s.hasType (IDs::Slot) && (int) s.getProperty (IDs::slotIndex) == slotIndex)
                { slot = s; break; }
            }

            wetKnob.setVisible (slot.isValid());
            if (slot.isValid())
                wetKnob.setValue ((double) slot.getProperty (IDs::mix, 1.0), juce::dontSendNotification);
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds();
            bool filled = slot.isValid();
            bool enabled = filled && (bool) slot.getProperty (IDs::enabled, true);

            auto rf = r.toFloat();
            if (filled)
            {
                g.setColour (Colors::raised);
                g.fillRoundedRectangle (rf, 3.0f);
                g.setColour (Colors::outline);
                g.drawRoundedRectangle (rf.reduced (0.5f), 3.0f, 1.0f);
            }
            else
            {
                g.setColour (Colors::stepDim2.withAlpha (0.4f));
                g.fillRoundedRectangle (rf, 3.0f);
            }
            if (rowHover)
            {
                g.setColour (juce::Colours::white.withAlpha (0.04f));
                g.fillRoundedRectangle (rf, 3.0f);
            }

            {
                auto arr = getArrowArea().toFloat();
                float cx = arr.getCentreX() - 1.0f, cy = arr.getCentreY();
                g.setColour (arrowHover ? Colors::accent
                                        : (filled ? Colors::textDim : Colors::textDim.withAlpha (0.5f)));
                juce::Path tri;
                tri.addTriangle (cx - 1.5f, cy - 4.0f, cx - 1.5f, cy + 4.0f, cx + 3.5f, cy);
                g.fillPath (tri);
            }

            auto nameArea = getNameArea();
            if (filled)
            {
                g.setColour (enabled ? Colors::text : Colors::textDim);
                g.setFont (Typography::ui (13.0f));
                g.drawText (effectDisplayName (slot), nameArea, juce::Justification::centredLeft);
            }
            else
            {
                g.setColour (Colors::textDim.withAlpha (0.45f));
                g.setFont (Typography::ui (12.0f));
                g.drawText ("Slot " + juce::String (slotIndex + 1), nameArea,
                            juce::Justification::centredLeft);
            }

            if (filled)
            {
                auto led = getLedArea().withSizeKeepingCentre (9, 9).toFloat();
                g.setColour (enabled ? Colors::noteGreen : Colors::stepDim);
                g.fillEllipse (led);
                if (enabled)
                {
                    g.setColour (Colors::noteGreen.withAlpha (0.35f));
                    g.drawEllipse (led.expanded (1.5f), 1.2f);
                }
            }
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            rack.endWheelTrack();
            if (e.mods.isMiddleButtonDown())
                return;
            bool filled = slot.isValid();

            if (getArrowArea().contains (e.getPosition()))
            {
                showContextMenu();
                return;
            }

            if (filled && getLedArea().contains (e.getPosition()))
            {
                slotToggleEnabled (slot);
                return;
            }

            if (e.mods.isPopupMenu())
            {
                showContextMenu();
                return;
            }

            if (filled)
                rack.mixer.openSlotEditor (rack.mixer.selectedTrack, slot);
            else
                rack.mixer.showAddEffectMenu (rack.mixer.selectedTrack, slotIndex, this);
        }

        void mouseMove (const juce::MouseEvent& e) override
        {
            rack.maybeEndWheelTrack (e.getScreenPosition());
            bool over = getArrowArea().contains (e.getPosition());
            if (over != arrowHover)
            {
                arrowHover = over;
                repaint();
            }
        }

        void mouseEnter (const juce::MouseEvent& e) override
        {
            rack.maybeEndWheelTrack (e.getScreenPosition());
            rowHover = true;
            repaint();
        }

        void mouseExit (const juce::MouseEvent&) override
        {
            arrowHover = false;
            rowHover = false;
            repaint();
        }

        void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
        {
            if (wheel.deltaY == 0.0f)
            {
                Component::mouseWheelMove (e, wheel);
                return;
            }
            int delta = wheel.deltaY > 0 ? -1 : 1;

            if (rack.wheelTracking)
            {
                if (rack.wheelTrackedSlot.isValid()
                    && rack.wheelTrackedSlot.getParent() == rack.currentTrack())
                {
                    rack.moveSlotOneStep (rack.wheelTrackedSlot, delta);
                    return;
                }
                rack.endWheelTrack();
            }

            if (! slot.isValid())
            {
                Component::mouseWheelMove (e, wheel);
                return;
            }

            auto tracked = slot;
            rack.moveSlotOneStep (tracked, delta);
            rack.beginWheelTrack (tracked, e.getScreenPosition());
        }

        void resized() override
        {
            auto knobArea = getLocalBounds().removeFromRight (40).removeFromLeft (22);
            wetKnob.setBounds (knobArea.withSizeKeepingCentre (20, 20));
        }

    private:
        juce::Rectangle<int> getArrowArea() const { return getLocalBounds().removeFromLeft (18); }
        juce::Rectangle<int> getLedArea() const    { return getLocalBounds().removeFromRight (16); }
        juce::Rectangle<int> getNameArea() const
        {
            return getLocalBounds().withTrimmedLeft (18).withTrimmedRight (42);
        }

        void showContextMenu()
        {
            juce::PopupMenu m;
            bool filled = slot.isValid();
            if (filled)
            {
                m.addItem (1, "Open editor");
                m.addItem (2, (bool) slot.getProperty (IDs::enabled, true) ? "Disable" : "Enable");
                m.addSeparator();
                m.addItem (3, "Remove");
            }
            else
            {
                m.addItem (4, "Add effect...");
            }

            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                [safe = juce::Component::SafePointer<SlotRow> (this), s = slot] (int result) mutable
                {
                    if (safe == nullptr || result == 0)
                        return;
                    if (result == 1)
                        safe->rack.mixer.openSlotEditor (safe->rack.mixer.selectedTrack, s);
                    else if (result == 2)
                        slotToggleEnabled (s);
                    else if (result == 3)
                        slotRemove (s);
                    else if (result == 4)
                        safe->rack.mixer.showAddEffectMenu (safe->rack.mixer.selectedTrack, safe->slotIndex, safe.getComponent());
                });
        }

        EffectRack& rack;
        const int slotIndex;
        juce::ValueTree slot;
        ValueKnob wetKnob;
        bool arrowHover = false, rowHover = false;
    };

    static constexpr int headerH = 24;

    MixerPanel& mixer;
    juce::Viewport slotViewport;
    juce::Component slotList;
    juce::OwnedArray<SlotRow> rows;
    juce::Rectangle<int> slotBox;

    juce::ValueTree wheelTrackedSlot;
    juce::Point<int> wheelTrackScreenPos;
    bool wheelTracking = false;
};

class MixerPanel::ModeChip : public juce::Component,
                             public juce::SettableTooltipClient
{
public:
    explicit ModeChip (MixerPanel& m) : mixer (m)
    {
        setTooltip ("Effect rack view: slot rack or device chain");
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        float pill = r.getHeight() * 0.5f;
        g.setColour (Colors::panelDark);
        g.fillRoundedRectangle (r, pill);

        bool chain = mixer.rackMode == RackMode::deviceChain;
        auto inner = r.reduced (1.5f);
        auto left = inner.removeFromLeft (inner.getWidth() * 0.5f);
        auto& right = inner;

        juce::Rectangle<float> active = chain ? right : left;
        g.setColour (Colors::accent.withAlpha (0.22f));
        g.fillRoundedRectangle (active, inner.getHeight() * 0.5f);

        g.setFont (Typography::caps (10.5f));
        g.setColour (! chain ? Colors::accent : Colors::textDim);
        g.drawText ("RACK", left, juce::Justification::centred);
        g.setColour (chain ? Colors::accent : Colors::textDim);
        g.drawText ("CHAIN", right, juce::Justification::centred);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! e.mods.isMiddleButtonDown())
            mixer.toggleRackMode();
    }

private:
    MixerPanel& mixer;
};

class MixerPanel::DeviceChain : public juce::Component,
                                private juce::Timer
{
public:
    explicit DeviceChain (MixerPanel& m) : mixer (m)
    {
        header = std::make_unique<HeaderCell> (*this);
        addAndMakeVisible (*header);

        cardsViewport.setViewedComponent (&cardsContainer, false);
        cardsViewport.setScrollBarsShown (false, true);
        addAndMakeVisible (cardsViewport);

        addCard = std::make_unique<AddCard> (*this);
        cardsContainer.addAndMakeVisible (*addCard);

        rebuild();
        startTimerHz (15);
    }

    int selectedTrack() const              { return mixer.selectedTrack; }
    juce::ValueTree track() const          { return mixer.trackTree (mixer.selectedTrack); }
    juce::String trackName() const         { return mixer.trackName (mixer.selectedTrack); }
    juce::String nameOf (int idx) const    { return mixer.trackName (idx); }
    void showRouting (juce::Component* t)  { mixer.showRoutingMenuFor (mixer.selectedTrack, t); }
    void openEditor (juce::ValueTree s)    { mixer.openSlotEditor (mixer.selectedTrack, s); }
    void addEffect (int slotIdx, juce::Component* t) { mixer.showAddEffectMenu (mixer.selectedTrack, slotIdx, t); }

    juce::AudioProcessor* processorForUid (int uid) const
    {
        return mixer.engine.getProcessorFor ("m" + juce::String (mixer.selectedTrack)
                                              + "s" + juce::String (uid));
    }

    void recordParamEdit (int uid, int paramIndex, float oldV, float newV)
    {
        if (mixer.onParamEdit)
            mixer.onParamEdit ("m" + juce::String (mixer.selectedTrack)
                                 + "s" + juce::String (uid), paramIndex, oldV, newV);
    }

    juce::ValueTree findSlotByUid (int uid) const
    {
        auto t = track();
        for (int i = 0; i < t.getNumChildren(); ++i)
        {
            auto s = t.getChild (i);
            if (s.hasType (IDs::Slot) && (int) s.getProperty (IDs::uid) == uid)
                return s;
        }
        return {};
    }

    void refresh()
    {
        bool needLayout = false;
        for (auto* c : cards)
            needLayout |= c->refresh();
        if (needLayout)
            layoutCards();
        header->repaint();
    }

    void rebuild()
    {
        cards.clear();
        auto t = track();
        for (int idx = 0; idx < Model::numSlotsPerTrack; ++idx)
            for (int i = 0; i < t.getNumChildren(); ++i)
            {
                auto s = t.getChild (i);
                if (s.hasType (IDs::Slot) && (int) s.getProperty (IDs::slotIndex) == idx)
                {
                    auto* c = new DeviceCard (*this, (int) s.getProperty (IDs::uid));
                    cards.add (c);
                    cardsContainer.addAndMakeVisible (c);
                    break;
                }
            }
        addCard->toFront (false);
        header->repaint();
        layoutCards();
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (Colors::panelDark);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), Metrics::rL);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (3);
        header->setBounds (r.removeFromLeft (headerW));
        r.removeFromLeft (4);
        cardsViewport.setBounds (r);
        layoutCards();
    }

private:
    static constexpr int headerW = 96, addCardW = 40, gap = 4;

    void timerCallback() override
    {
        if (! isShowing())
            return;
        bool needLayout = false;
        for (auto* c : cards)
            needLayout |= c->refresh();
        if (needLayout)
            layoutCards();
    }

    void layoutCards()
    {
        int h = juce::jmax (10, cardsViewport.getMaximumVisibleHeight());
        int x = 0;
        for (auto* c : cards)
        {
            int w = c->preferredWidth();
            c->setBounds (x, 0, w, h);
            x += w + gap;
        }
        addCard->setBounds (x, 0, addCardW, h);
        x += addCardW;
        cardsContainer.setSize (juce::jmax (cardsViewport.getWidth(), x), h);
    }

    class HeaderCell : public juce::Component
    {
    public:
        explicit HeaderCell (DeviceChain& c) : chain (c) {}

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds();
            g.setColour (Colors::header);
            g.fillRoundedRectangle (r.toFloat().reduced (1.0f), Metrics::rM);

            auto top = r.reduced (5);
            g.setColour (Colors::text);
            g.setFont (Typography::semi (12.0f));
            g.drawText (chain.trackName(), top.removeFromTop (top.getHeight() - 32),
                        juce::Justification::topLeft, true);

            auto out = getOutputArea();
            g.setColour (Colors::raised);
            g.fillRoundedRectangle (out.toFloat(), Metrics::rS);
            g.setColour (Colors::textDim);
            g.setFont (Typography::caps (8.5f));
            g.drawText ("OUT", out.removeFromTop (13).reduced (5, 0), juce::Justification::bottomLeft);

            int sel = chain.selectedTrack();
            int dest = (int) chain.track().getProperty (IDs::routeTo, 0);
            juce::String target = sel == 0 ? juce::String ("Out 1-2")
                : (dest < 0 ? juce::String ("(none)")
                            : dest == 0 ? juce::String ("Master")
                                        : chain.nameOf (dest));
            g.setColour (Colors::text);
            g.setFont (Typography::ui (10.5f));
            g.drawText (target, out.reduced (5, 0), juce::Justification::centredLeft, true);
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (! e.mods.isMiddleButtonDown()
                && chain.selectedTrack() > 0 && getOutputArea().contains (e.getPosition()))
                chain.showRouting (this);
        }

    private:
        juce::Rectangle<int> getOutputArea() const
        {
            return getLocalBounds().reduced (4).removeFromBottom (32);
        }
        DeviceChain& chain;
    };

    class DeviceCard : public juce::Component
    {
    public:
        DeviceCard (DeviceChain& c, int uid) : chain (c), slotUid (uid)
        {
            mixKnob.setRange (0.0, 1.0, 0.0);
            mixKnob.setDoubleClickReturnValue (true, 1.0);
            mixKnob.setMouseDragSensitivity (300);
            mixKnob.setTooltip ("Wet / dry mix");
            mixKnob.onValueChange = [this]
            {
                if (auto s = slotTree(); s.isValid())
                    s.setProperty (IDs::mix, mixKnob.getValue(), nullptr);
            };
            addAndMakeVisible (mixKnob);
            refresh();
        }

        juce::ValueTree slotTree() const { return chain.findSlotByUid (slotUid); }

        bool refresh()
        {
            bool enabled = true;
            if (auto s = slotTree(); s.isValid())
            {
                mixKnob.setValue ((double) s.getProperty (IDs::mix, 1.0), juce::dontSendNotification);
                enabled = (bool) s.getProperty (IDs::enabled, true);
            }

            int oldW = preferredWidth();
            auto params = visibleParams (chain.processorForUid (slotUid));

            bool changed = ((int) cachedVals.size() != params.size()) || enabled != cachedEnabled;
            cachedEnabled = enabled;
            cachedVals.resize ((size_t) params.size());
            for (int i = 0; i < params.size(); ++i)
            {
                float v = params[i]->getValue();
                if (! juce::approximatelyEqual (v, cachedVals[(size_t) i]))
                { cachedVals[(size_t) i] = v; changed = true; }
            }
            paramCount = params.size();

            if (changed)
                repaint();
            return preferredWidth() != oldW;
        }

        int preferredWidth() const
        {
            if (paramCount <= 0)
                return cardMinW;
            int cols = juce::jlimit (1, maxCols, (paramCount + rowsPerCol - 1) / rowsPerCol);
            return juce::jmax (cardMinW, mixColW + cols * cellW + bodyPad * 2);
        }

        void paint (juce::Graphics& g) override
        {
            auto s = slotTree();
            bool enabled = s.isValid() && (bool) s.getProperty (IDs::enabled, true);

            auto rf = getLocalBounds().toFloat().reduced (1.0f);
            g.setColour (Colors::card);
            g.fillRoundedRectangle (rf, Metrics::rM);

            auto title = getTitleBar();
            auto tf = title.toFloat();
            g.setColour (Colors::header);
            g.fillRoundedRectangle (tf, Metrics::rM);
            g.fillRect (tf.withTop (tf.getCentreY()));

            auto led = getLedArea().withSizeKeepingCentre (9, 9).toFloat();
            g.setColour (enabled ? Colors::accent : Colors::stepOff);
            g.fillEllipse (led);
            if (enabled)
            {
                g.setColour (Colors::accent.withAlpha (0.30f));
                g.drawEllipse (led.expanded (1.5f), 1.2f);
            }

            g.setColour (enabled ? Colors::text : Colors::textDim);
            g.setFont (Typography::semi (12.0f));
            g.drawText (effectDisplayName (s), getNameArea(), juce::Justification::centredLeft, true);

            g.setFont (Typography::semi (11.0f));
            g.setColour (moveLeftHover ? Colors::accent : Colors::textDim);
            g.drawText ("<", getMoveLeftArea(), juce::Justification::centred);
            g.setColour (moveRightHover ? Colors::accent : Colors::textDim);
            g.drawText (">", getMoveRightArea(), juce::Justification::centred);
            g.setColour (deleteHover ? Colors::record : Colors::textDim);
            g.drawText ("x", getDeleteArea(), juce::Justification::centred);

            g.setColour (Colors::textDim);
            g.setFont (Typography::caps (9.0f));

            if (paramCount > 0)
            {
                g.drawText ("MIX", getMixColArea().removeFromBottom (14), juce::Justification::centred);

                auto params = visibleParams (chain.processorForUid (slotUid));
                int n = juce::jmin (params.size(), paramCount);
                for (int i = 0; i < n; ++i)
                    paintCell (g, i, params[i], enabled);
            }
            else
            {
                g.drawText ("MIX", getMixLabelArea(), juce::Justification::centred);
            }
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (e.mods.isMiddleButtonDown())
                return;
            auto s = slotTree();
            if (! s.isValid())
                return;
            auto p = e.getPosition();
            if (getLedArea().contains (p))        { slotToggleEnabled (s); return; }
            if (getDeleteArea().contains (p))     { slotRemove (s); return; }
            if (getMoveLeftArea().contains (p))   { slotMoveOneStep (s.getParent(), s, -1); return; }
            if (getMoveRightArea().contains (p))  { slotMoveOneStep (s.getParent(), s, +1); return; }
            if (e.mods.isPopupMenu())             { showMenu (s); return; }
            if (getTitleBar().contains (p))       { chain.openEditor (s); return; }

            int idx = paramIndexAt (p);
            if (idx >= 0)
            {
                if (auto* pr = paramAt (idx); pr != nullptr && isTwoState (pr))
                {
                    float target = p.x < cellBounds (idx).reduced (3, 2).getCentreX() ? 0.0f : 1.0f;
                    float oldV = pr->getValue();
                    pr->beginChangeGesture();
                    pr->setValueNotifyingHost (target);
                    pr->endChangeGesture();
                    chain.recordParamEdit (slotUid, pr->getParameterIndex(), oldV, target);
                    refresh();
                    return;
                }
                dragIndex = idx;
                dragStartPos = p;
                if (auto* pr = paramAt (idx))
                {
                    dragStartValue = pr->getValue();
                    pr->beginChangeGesture();
                    gestureActive = true;
                    if (! e.source.isUnboundedMouseMovementEnabled())
                        e.source.enableUnboundedMouseMovement (true, false);
                }
            }
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (dragIndex < 0)
                return;
            auto* pr = paramAt (dragIndex);
            if (pr == nullptr)
                return;
            bool fine = e.mods.isShiftDown() || e.mods.isCtrlDown();
            float sens = fine ? 1200.0f : 300.0f;
            float dy = (float) (dragStartPos.y - e.y);
            float dx = (float) (e.x - dragStartPos.x);
            float delta = (dy + dx * 0.25f) / sens;
            pr->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, dragStartValue + delta));
            refresh();
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            if (e.source.isUnboundedMouseMovementEnabled())
            {
                auto src = e.source;
                src.enableUnboundedMouseMovement (false);
                src.setScreenPosition (e.getMouseDownScreenPosition().toFloat());
            }
            if (gestureActive)
                if (auto* pr = paramAt (dragIndex))
                {
                    pr->endChangeGesture();
                    chain.recordParamEdit (slotUid, pr->getParameterIndex(), dragStartValue, pr->getValue());
                }
            gestureActive = false;
            dragIndex = -1;
        }

        void mouseDoubleClick (const juce::MouseEvent& e) override
        {
            int idx = paramIndexAt (e.getPosition());
            if (idx < 0)
                return;
            if (auto* pr = paramAt (idx))
            {
                float oldV = pr->getValue();
                pr->setValueNotifyingHost (pr->getDefaultValue());
                chain.recordParamEdit (slotUid, pr->getParameterIndex(), oldV, pr->getValue());
            }
            refresh();
        }

        void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
        {
            int idx = paramIndexAt (e.getPosition());
            if (idx < 0)
            {
                juce::Component::mouseWheelMove (e, w);
                return;
            }
            auto* pr = paramAt (idx);
            if (pr == nullptr)
                return;
            if (isTwoState (pr))
            {
                float oldV = pr->getValue();
                float target = pr->getValue() >= 0.5f ? 0.0f : 1.0f;
                pr->beginChangeGesture();
                pr->setValueNotifyingHost (target);
                pr->endChangeGesture();
                chain.recordParamEdit (slotUid, pr->getParameterIndex(), oldV, target);
                refresh();
                return;
            }
            float step = (e.mods.isShiftDown() || e.mods.isCtrlDown()) ? 0.01f : 0.03f;
            float oldV = pr->getValue();
            float v = juce::jlimit (0.0f, 1.0f, pr->getValue() + (w.deltaY > 0.0f ? step : -step));
            pr->beginChangeGesture();
            pr->setValueNotifyingHost (v);
            pr->endChangeGesture();
            chain.recordParamEdit (slotUid, pr->getParameterIndex(), oldV, v);
            refresh();
        }

        void mouseMove (const juce::MouseEvent& e) override
        {
            auto p = e.getPosition();
            bool l = getMoveLeftArea().contains (p);
            bool rr = getMoveRightArea().contains (p);
            bool d = getDeleteArea().contains (p);
            if (l != moveLeftHover || rr != moveRightHover || d != deleteHover)
            {
                moveLeftHover = l; moveRightHover = rr; deleteHover = d;
                repaint (getTitleBar());
            }
            int hc = paramIndexAt (p);
            if (hc != hoverCell)
            {
                hoverCell = hc;
                repaint (getBodyArea());
            }
        }

        void mouseExit (const juce::MouseEvent&) override
        {
            if (moveLeftHover || moveRightHover || deleteHover)
            {
                moveLeftHover = moveRightHover = deleteHover = false;
                repaint (getTitleBar());
            }
            if (hoverCell != -1)
            {
                hoverCell = -1;
                repaint (getBodyArea());
            }
        }

        void resized() override
        {
            if (paramCount > 0)
                mixKnob.setBounds (getMixColArea().withSizeKeepingCentre (32, 32).translated (0, -6));
            else
                mixKnob.setBounds (getBodyArea().withSizeKeepingCentre (38, 38).translated (0, -4));
        }

    private:
        static juce::Array<juce::AudioProcessorParameter*> visibleParams (juce::AudioProcessor* proc)
        {
            juce::Array<juce::AudioProcessorParameter*> out;
            if (proc == nullptr)
                return out;
            auto* bypass = proc->getBypassParameter();
            for (auto* p : proc->getParameters())
            {
                if (p == bypass || ! p->isAutomatable())
                    continue;
                out.add (p);
                if (out.size() >= maxParams)
                    break;
            }
            return out;
        }

        juce::AudioProcessorParameter* paramAt (int idx) const
        {
            auto params = visibleParams (chain.processorForUid (slotUid));
            return juce::isPositiveAndBelow (idx, params.size()) ? params[idx] : nullptr;
        }

        static bool isTwoState (juce::AudioProcessorParameter* p)
        {
            return p->isBoolean() || (p->isDiscrete() && p->getNumSteps() == 2);
        }

        void paintCell (juce::Graphics& g, int i, juce::AudioProcessorParameter* pr, bool enabled) const
        {
            auto cell = cellBounds (i).reduced (3, 2);
            if (cell.getHeight() < 8)
                return;

            auto bed = cell.toFloat().expanded (2.0f, 1.0f);
            g.setColour (Colors::panelDark);
            g.fillRoundedRectangle (bed, Metrics::rS);
            if (i == hoverCell)
            {
                g.setColour (juce::Colours::white.withAlpha (0.04f));
                g.fillRoundedRectangle (bed, Metrics::rS);
            }

            auto nameR = cell.removeFromTop (juce::jmin (13, cell.getHeight() / 3));
            g.setColour (enabled ? Colors::text : Colors::stepDim);
            g.setFont (Typography::semi (10.5f));
            g.drawText (pr->getName (20), nameR, juce::Justification::centredLeft, true);

            if (isTwoState (pr))
            {
                int segH = juce::jmin (18, cell.getHeight());
                auto seg = cell.withSizeKeepingCentre (cell.getWidth(), segH);
                auto left = seg.removeFromLeft (seg.getWidth() / 2);
                left.removeFromRight (1);
                auto right = seg;
                right.removeFromLeft (1);

                bool onActive = pr->getValue() >= 0.5f;
                auto offText = pr->getText (0.0f, 8);
                auto onText = pr->getText (1.0f, 8);
                if (offText.isEmpty()) offText = "OFF";
                if (onText.isEmpty())  onText = "ON";

                auto segFont = Typography::semi (10.0f);
                auto drawSeg = [&] (juce::Rectangle<int> r, const juce::String& txt, bool active)
                {
                    if (active)
                    {
                        g.setColour (enabled ? Colors::accent : Colors::stepDim);
                        g.fillRoundedRectangle (r.toFloat(), Metrics::rS);
                        g.setColour (enabled ? Colors::windowBg : Colors::textDim);
                    }
                    else
                    {
                        g.setColour (Colors::raised);
                        g.fillRoundedRectangle (r.toFloat(), Metrics::rS);
                        g.setColour (Colors::textDim);
                    }
                    g.setFont (segFont);
                    g.drawText (txt, r, juce::Justification::centred, true);
                };
                drawSeg (left, offText, ! onActive);
                drawSeg (right, onText, onActive);
                return;
            }

            auto valueR = cell.removeFromBottom (juce::jmin (14, cell.getHeight() / 2));

            float norm = juce::jlimit (0.0f, 1.0f, pr->getValue());
            auto trackR = cell.reduced (2, 0);
            auto tr = trackR.withSizeKeepingCentre (trackR.getWidth(), 7).toFloat();
            g.setColour (Colors::windowBg);
            g.fillRoundedRectangle (tr, 3.5f);
            g.setColour (enabled ? Colors::accent : Colors::stepDim);
            g.fillRoundedRectangle (tr.withWidth (juce::jmax (3.0f, tr.getWidth() * norm)), 3.5f);

            float mx = tr.getX() + tr.getWidth() * norm;
            juce::Path tri;
            tri.addTriangle (mx - 4.0f, tr.getY() - 5.0f, mx + 4.0f, tr.getY() - 5.0f, mx, tr.getY() + 1.0f);
            g.setColour (enabled ? Colors::accent.brighter (0.25f) : Colors::stepDim);
            g.fillPath (tri);

            juce::String vt = pr->getCurrentValueAsText();
            if (vt.isEmpty())
                vt = pr->getText (pr->getValue(), 16);
            g.setColour (enabled ? Colors::text : Colors::textDim);
            g.setFont (Typography::semi (11.0f));
            g.drawText (vt, valueR, juce::Justification::centred, true);
        }

        juce::Rectangle<int> getTitleBar() const   { return getLocalBounds().reduced (2).removeFromTop (titleH); }
        juce::Rectangle<int> getBodyArea() const    { return getLocalBounds().reduced (2).withTrimmedTop (titleH + 2); }
        juce::Rectangle<int> getMixColArea() const  { return getBodyArea().removeFromLeft (mixColW); }
        juce::Rectangle<int> getGridArea() const
        {
            auto body = getBodyArea();
            body.removeFromLeft (mixColW);
            return body.reduced (bodyPad, bodyPad);
        }
        juce::Rectangle<int> getLedArea() const     { return getTitleBar().removeFromLeft (16); }
        juce::Rectangle<int> getDeleteArea() const  { return getTitleBar().removeFromRight (16); }
        juce::Rectangle<int> getMoveRightArea() const { auto t = getTitleBar(); t.removeFromRight (16); return t.removeFromRight (14); }
        juce::Rectangle<int> getMoveLeftArea() const  { auto t = getTitleBar(); t.removeFromRight (30); return t.removeFromRight (14); }
        juce::Rectangle<int> getNameArea() const
        {
            return getTitleBar().withTrimmedLeft (16).withTrimmedRight (44);
        }
        juce::Rectangle<int> getMixLabelArea() const
        {
            return getBodyArea().removeFromBottom (16);
        }

        juce::Rectangle<int> cellBounds (int i) const
        {
            auto grid = getGridArea();
            int cellH = grid.getHeight() / rowsPerCol;
            int col = i / rowsPerCol, row = i % rowsPerCol;
            return { grid.getX() + col * cellW, grid.getY() + row * cellH, cellW, cellH };
        }

        int paramIndexAt (juce::Point<int> p) const
        {
            if (paramCount <= 0 || ! getGridArea().contains (p))
                return -1;
            for (int i = 0; i < paramCount; ++i)
                if (cellBounds (i).contains (p))
                    return i;
            return -1;
        }

        void showMenu (juce::ValueTree s)
        {
            juce::PopupMenu m;
            m.addItem (1, "Open editor");
            m.addItem (2, (bool) s.getProperty (IDs::enabled, true) ? "Disable" : "Enable");
            m.addSeparator();
            m.addItem (3, "Remove");
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                [safe = juce::Component::SafePointer<DeviceCard> (this), s] (int result) mutable
                {
                    if (safe == nullptr || result == 0)
                        return;
                    if (result == 1)      safe->chain.openEditor (s);
                    else if (result == 2) slotToggleEnabled (s);
                    else if (result == 3) slotRemove (s);
                });
        }

        static constexpr int titleH = 22;
        static constexpr int rowsPerCol = 4, maxCols = 4, maxParams = 16;
        static constexpr int mixColW = 54, cellW = 88, cardMinW = 170, bodyPad = 4;

        DeviceChain& chain;
        const int slotUid;
        ValueKnob mixKnob;
        bool moveLeftHover = false, moveRightHover = false, deleteHover = false;
        int paramCount = 0, hoverCell = -1, dragIndex = -1;
        juce::Point<int> dragStartPos;
        float dragStartValue = 0.0f;
        bool gestureActive = false, cachedEnabled = true;
        std::vector<float> cachedVals;
    };

    class AddCard : public juce::Component,
                    public juce::SettableTooltipClient
    {
    public:
        explicit AddCard (DeviceChain& c) : chain (c)
        {
            setTooltip ("Add effect");
        }

        void paint (juce::Graphics& g) override
        {
            auto rf = getLocalBounds().toFloat().reduced (1.0f);
            g.setColour (hover ? Colors::raised : Colors::panelDark);
            g.fillRoundedRectangle (rf, 4.0f);
            g.setColour (Colors::outline);
            g.drawRoundedRectangle (rf.reduced (0.5f), 4.0f, 1.0f);
            g.setColour (hover ? Colors::accent : Colors::textDim);
            g.setFont (Typography::bold (20.0f));
            g.drawText ("+", getLocalBounds(), juce::Justification::centred);
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (e.mods.isMiddleButtonDown())
                return;
            int free = firstFreeSlotIndex (chain.track());
            if (free < Model::numSlotsPerTrack)
                chain.addEffect (free, this);
        }

        void mouseEnter (const juce::MouseEvent&) override { hover = true;  repaint(); }
        void mouseExit  (const juce::MouseEvent&) override { hover = false; repaint(); }

    private:
        DeviceChain& chain;
        bool hover = false;
    };

    MixerPanel& mixer;
    std::unique_ptr<HeaderCell> header;
    juce::Viewport cardsViewport;
    juce::Component cardsContainer;
    juce::OwnedArray<DeviceCard> cards;
    std::unique_ptr<AddCard> addCard;
};

class MixerPanel::TopScrollBar : public juce::Component
{
public:
    std::function<void (int)> onScroll;

    void setMetrics (int content, int view, int pos)
    {
        contentW = content; viewW = view; viewX = pos;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        float rad = r.getHeight() * 0.5f;
        g.setColour (Colors::panelDark);
        g.fillRoundedRectangle (r, rad);

        auto thumb = thumbRect().toFloat().reduced (1.0f);
        g.setColour ((dragging || hover) ? Colors::scrollThumbHover : Colors::scrollThumb);
        g.fillRoundedRectangle (thumb, thumb.getHeight() * 0.5f);
        g.setColour (Colors::textDim.withAlpha (0.55f));
        float cx = thumb.getCentreX(), cy = thumb.getCentreY();
        for (int i = -1; i <= 1; ++i)
            g.fillEllipse (cx + (float) i * 4.0f - 1.0f, cy - 1.0f, 2.0f, 2.0f);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        auto th = thumbRect();
        if (th.contains (e.getPosition()))
        {
            dragging = true;
            dragOffset = e.x - th.getX();
        }
        else
        {
            int page = juce::jmax (1, viewW * 9 / 10);
            request (viewX + (e.x < th.getCentreX() ? -page : page));
        }
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragging)
            return;
        int range = juce::jmax (1, getWidth() - thumbRect().getWidth());
        int tx = juce::jlimit (0, range, e.x - dragOffset);
        request ((int) ((float) tx / (float) range * (float) juce::jmax (0, contentW - viewW)));
    }

    void mouseUp (const juce::MouseEvent&) override      { dragging = false; repaint(); }
    void mouseEnter (const juce::MouseEvent&) override   { hover = true;  repaint(); }
    void mouseExit  (const juce::MouseEvent&) override   { hover = false; repaint(); }

private:
    void request (int x)
    {
        if (onScroll)
            onScroll (juce::jlimit (0, juce::jmax (0, contentW - viewW), x));
    }

    juce::Rectangle<int> thumbRect() const
    {
        int barW = getWidth();
        if (contentW <= viewW || contentW <= 0)
            return getLocalBounds();
        int tw = juce::jlimit (minThumb, barW, (int) ((float) barW * (float) viewW / (float) contentW));
        int range = juce::jmax (1, barW - tw);
        int tx = juce::jlimit (0, range, (int) ((float) range * (float) viewX / (float) (contentW - viewW)));
        return { tx, 0, tw, getHeight() };
    }

    static constexpr int minThumb = 36;
    int contentW = 0, viewW = 0, viewX = 0, dragOffset = 0;
    bool dragging = false, hover = false;
};

class MixerPanel::TimecodeBar : public juce::Component
{
public:
    TimecodeBar (AudioEngine& e, juce::ValueTree p) : engine (e), project (p) {}

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        const float h = r.getHeight();
        const float w = r.getWidth();

        g.setColour (Colors::panelDark);
        g.fillRect (r);

        const double totalBeats = arrangementBeats();
        const double totalBars  = totalBeats / 4.0;
        const float pxPerBar = w / (float) totalBars;

        const double posBeats = juce::jmax (0.0, engine.getPositionTicks() / (double) Model::PPQ);
        const float playX = juce::jlimit (0.0f, w, (float) (posBeats / totalBeats * w));

        g.setColour (Colors::accent.withAlpha (0.14f));
        g.fillRect (0.0f, 0.0f, playX, h);

        int labelEvery = pxPerBar < 8.0f ? 8 : (pxPerBar < 16.0f ? 4 : 1);
        g.setFont (Typography::semi (8.5f));
        for (int bar = 0; bar <= (int) totalBars; ++bar)
        {
            const int x = (int) (bar * pxPerBar);
            g.setColour (Colors::gridLine);
            g.drawVerticalLine (x, 0.0f, h);
            if (bar < (int) totalBars && bar % labelEvery == 0)
            {
                g.setColour (Colors::textDim);
                g.drawText (juce::String (bar + 1), x + 2, 0,
                            (int) (pxPerBar * labelEvery) - 2, (int) h,
                            juce::Justification::centredLeft);
            }
        }

        g.setColour (Colors::accent2);
        g.fillRect (playX - 1.0f, 0.0f, 2.0f, h);
        juce::Path cap;
        cap.addTriangle (playX - 4.0f, 0.0f, playX + 4.0f, 0.0f, playX, 5.0f);
        g.fillPath (cap);

        const int barNum  = (int) (posBeats / 4.0) + 1;
        const int beatNum = (int) std::fmod (posBeats, 4.0) + 1;
        g.setColour (Colors::text);
        g.setFont (Typography::mono (9.5f));
        g.drawText (juce::String (barNum) + ":" + juce::String (beatNum),
                    getLocalBounds().reduced (5, 0), juce::Justification::centredRight);
    }

    void mouseDown (const juce::MouseEvent& e) override { scrub (e); }
    void mouseDrag (const juce::MouseEvent& e) override { scrub (e); }

private:
    void scrub (const juce::MouseEvent& e)
    {
        const double total = arrangementBeats();
        double beats = juce::jlimit (0.0, total,
                                     (double) e.x / (double) juce::jmax (1, getWidth()) * total);
        if (! e.mods.isShiftDown())
            beats = std::round (beats);
        engine.seekTicks (beats * (double) Model::PPQ);
    }

    double arrangementBeats() const
    {
        double maxEnd = 0.0;
        auto pl = Model::getPlaylist (project);
        for (int i = 0; i < pl.getNumChildren(); ++i)
        {
            auto c = pl.getChild (i);
            if (! c.hasType (IDs::Clip))
                continue;
            const double end = (double) c.getProperty (IDs::startBeats, 0.0)
                             + (double) c.getProperty (IDs::lengthBeats, 0.0);
            maxEnd = juce::jmax (maxEnd, end);
        }
        return juce::jmax (16.0, std::ceil (maxEnd / 4.0)) * 4.0;
    }

    AudioEngine& engine;
    juce::ValueTree project;
};

MixerPanel::MixerPanel (juce::ValueTree p, AudioEngine& e, PluginHost& h)
    : project (p), engine (e), host (h)
{
    stripLnf = std::make_unique<MixerStripLnF>();

    project.addListener (this);

    addStripBtn = std::make_unique<AddStripButton> (*this);
    stripContainer.addAndMakeVisible (*addStripBtn);

    viewport.setViewedComponent (&stripContainer, false);
    viewport.setScrollBarsShown (false, false);
    viewport.onVisibleAreaChanged = [this] { updateScrollBar(); };
    addAndMakeVisible (viewport);

    scrollBar = std::make_unique<TopScrollBar>();
    scrollBar->onScroll = [this] (int x) { viewport.setViewPosition (x, 0); };
    addAndMakeVisible (*scrollBar);

    timecode = std::make_unique<TimecodeBar> (engine, project);
    addAndMakeVisible (*timecode);

    rack = std::make_unique<EffectRack> (*this);
    addAndMakeVisible (*rack);

    deviceChain = std::make_unique<DeviceChain> (*this);
    addChildComponent (*deviceChain);

    modeChip = std::make_unique<ModeChip> (*this);
    addAndMakeVisible (*modeChip);

    selectedTrack = 1;
    rebuildStrips();

    startTimerHz (30);
}

MixerPanel::~MixerPanel()
{
    project.removeListener (this);
}

int MixerPanel::numTracksInTree() const
{
    return Model::getMaxMixerTrackIndex (project) + 1;
}

juce::ValueTree MixerPanel::trackTree (int index) const
{
    return Model::getMixerTrack (project, index);
}

juce::String MixerPanel::trackName (int index) const
{
    return trackTree (index).getProperty (IDs::name).toString();
}

juce::String MixerPanel::routeLabelFor (int index) const
{
    int dest = (int) trackTree (index).getProperty (IDs::routeTo, 0);
    return dest < 0 ? juce::String ("-")
                    : dest == 0 ? juce::String ("M") : juce::String (dest);
}

void MixerPanel::rebuildStrips()
{
    int wanted = numTracksInTree();
    if (strips.size() != wanted)
    {
        strips.clear();
        for (int i = 0; i < wanted; ++i)
        {
            auto* strip = new Strip (*this, i);
            strips.add (strip);
            stripContainer.addAndMakeVisible (strip);
        }
        addStripBtn->setVisible (wanted - 1 < Model::maxMixerTracks);
        addStripBtn->toFront (false);
        selectedTrack = juce::jlimit (0, wanted - 1, selectedTrack);
        resized();
    }
    refreshAll();
}

void MixerPanel::refreshAll()
{
    for (auto* s : strips)
        s->refreshFromTree();
    rack->refresh();
    if (rackMode == RackMode::deviceChain && deviceChain != nullptr)
        deviceChain->rebuild();
}

void MixerPanel::selectTrack (int index)
{
    index = juce::jlimit (0, juce::jmax (0, strips.size() - 1), index);
    if (selectedTrack == index)
        return;
    selectedTrack = index;
    for (auto* s : strips)
        s->repaint();
    rack->refresh();
    if (rackMode == RackMode::deviceChain)
        deviceChain->rebuild();

    if (auto* s = strips[index]; s != nullptr && viewport.getWidth() > 0)
    {
        int vx = viewport.getViewPositionX();
        if (s->getX() < vx)
            vx = s->getX();
        else if (s->getRight() > vx + viewport.getWidth())
            vx = s->getRight() - viewport.getWidth();
        viewport.setViewPosition (vx, 0);
    }
}

void MixerPanel::showRoutingMenuFor (int index, juce::Component* target)
{
    if (index <= 0)
        return;

    int current = (int) trackTree (index).getProperty (IDs::routeTo, 0);
    int maxIdx = Model::getMaxMixerTrackIndex (project);

    constexpr int noneId = 9999;
    juce::PopupMenu m;
    m.addSectionHeader (trackName (index) + " output");
    m.addItem (1, "Master", true, current == 0);
    m.addItem (noneId, "(none)", true, current == -1);
    if (index + 1 <= maxIdx)
    {
        m.addSeparator();
        for (int k = index + 1; k <= maxIdx; ++k)
            m.addItem (2 + k, trackName (k), true, current == k);
    }

    m.showMenuAsync (menuAtMouse(),
        [safe = juce::Component::SafePointer<MixerPanel> (this), index, noneId] (int result)
        {
            if (safe == nullptr || result == 0)
                return;
            int target = result == 1 ? 0 : result == noneId ? -1 : result - 2;
            safe->trackTree (index).setProperty (IDs::routeTo, target, nullptr);
        });
}

void MixerPanel::showAddEffectMenu (int trackIndex, int slotIndex, juce::Component* target)
{
    auto effects = host.getEffects();

    std::vector<std::pair<int, juce::String>> extras;
    extras.push_back ({ 1, "Add Gain" });
    extras.push_back ({ 2, "Add EQ (3-band)" });
    extras.push_back ({ 3, "Add Soft Clipper" });
    extras.push_back ({ 4, "Add Delay" });
    extras.push_back ({ 5, "Add Reverb" });
    extras.push_back ({ 6, "Add Delay Bank" });
    extras.push_back ({ 7, "Add Limiter" });
    extras.push_back ({ 8, "Add Multiband Compressor" });
    extras.push_back ({ 9, "Add Soundbeastifier" });
    extras.push_back ({ 10, "Add Void" });
    extras.push_back ({ 11, "Add Low Lifter" });
    extras.push_back ({ 12, "Add Parametric EQ 2" });
    extras.push_back ({ 13, "Add Convolver" });
    extras.push_back ({ 14, "Add Parterre" });
    extras.push_back ({ 15, "Add Equivocal" });
    extras.push_back ({ 16, "Add Gross Beat" });

    PluginPickerPopup::show (*target, effects, extras,
        [safe = juce::Component::SafePointer<MixerPanel> (this), trackIndex, slotIndex] (const juce::PluginDescription& desc)
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
            juce::ValueTree s (IDs::Slot);
            s.setProperty (IDs::uid, Model::newId(), nullptr);
            s.setProperty (IDs::slotIndex, slotIndex, nullptr);
            s.setProperty (IDs::enabled, true, nullptr);
            s.setProperty (IDs::mix, 1.0, nullptr);
            s.setProperty (IDs::type, "plugin", nullptr);
            if (auto xml = resolved.createXml())
                s.setProperty (IDs::pluginDesc, xml->toString(), nullptr);
            safe->trackTree (trackIndex).appendChild (s, nullptr);
            if (safe->onLocalVstAdded)
                safe->onLocalVstAdded ("m" + juce::String (trackIndex) + "s" + s.getProperty (IDs::uid).toString(),
                                       resolved.name);
            juce::MessageManager::callAsync ([safe, trackIndex, s]
            {
                if (safe != nullptr)
                    safe->openSlotEditor (trackIndex, s);
            });
        },
        [safe = juce::Component::SafePointer<MixerPanel> (this), trackIndex, slotIndex] (int id)
        {
            if (safe == nullptr)
                return;
            juce::ValueTree s (IDs::Slot);
            s.setProperty (IDs::uid, Model::newId(), nullptr);
            s.setProperty (IDs::slotIndex, slotIndex, nullptr);
            s.setProperty (IDs::enabled, true, nullptr);
            s.setProperty (IDs::mix, 1.0, nullptr);
            if (id == 1)
                s.setProperty (IDs::type, "gain", nullptr);
            else if (id == 2)
                s.setProperty (IDs::type, "eq", nullptr);
            else if (id == 3)
                s.setProperty (IDs::type, "softclip", nullptr);
            else if (id == 4)
                s.setProperty (IDs::type, "delay", nullptr);
            else if (id == 5)
                s.setProperty (IDs::type, "reverb", nullptr);
            else if (id == 6)
                s.setProperty (IDs::type, "delaybank", nullptr);
            else if (id == 7)
                s.setProperty (IDs::type, "limiter", nullptr);
            else if (id == 8)
                s.setProperty (IDs::type, "multiband", nullptr);
            else if (id == 9)
                s.setProperty (IDs::type, "soundbeast", nullptr);
            else if (id == 10)
                s.setProperty (IDs::type, "void", nullptr);
            else if (id == 11)
                s.setProperty (IDs::type, "lowlifter", nullptr);
            else if (id == 12)
                s.setProperty (IDs::type, "parameq", nullptr);
            else if (id == 13)
                s.setProperty (IDs::type, "convolver", nullptr);
            else if (id == 14)
                s.setProperty (IDs::type, "parterre", nullptr);
            else if (id == 15)
                s.setProperty (IDs::type, "equivocal", nullptr);
            else if (id == 16)
                s.setProperty (IDs::type, "grossbeat", nullptr);
            safe->trackTree (trackIndex).appendChild (s, nullptr);
            juce::MessageManager::callAsync ([safe, trackIndex, s]
            {
                if (safe != nullptr)
                    safe->openSlotEditor (trackIndex, s);
            });
        },
        isPluginEnabled, isPluginNew);
}

void MixerPanel::openSlotEditor (int trackIndex, juce::ValueTree slot)
{
    if (! slot.isValid() || onOpenPluginEditor == nullptr)
        return;
    auto key = "m" + juce::String (trackIndex) + "s" + slot.getProperty (IDs::uid).toString();
    onOpenPluginEditor (key, trackName (trackIndex));
}

void MixerPanel::setSettings (juce::PropertiesFile* s)
{
    settings = s;
    auto mode = (s != nullptr && AppSettings::mixerRackMode (*s) == "chain")
                    ? RackMode::deviceChain : RackMode::slotRack;
    setRackMode (mode);
}

void MixerPanel::setRackMode (RackMode m)
{
    rackMode = m;
    bool chain = (m == RackMode::deviceChain);
    rack->setVisible (! chain);
    deviceChain->setVisible (chain);
    if (chain)
        deviceChain->rebuild();
    else
        rack->refresh();
    if (modeChip != nullptr)
        modeChip->repaint();
    resized();
}

void MixerPanel::toggleRackMode()
{
    auto next = rackMode == RackMode::slotRack ? RackMode::deviceChain : RackMode::slotRack;
    if (settings != nullptr)
    {
        AppSettings::setMixerRackMode (*settings, next == RackMode::deviceChain ? "chain" : "rack");
        settings->saveIfNeeded();
    }
    setRackMode (next);
}

void MixerPanel::exportMixerState (int trackIndex)
{
    auto track = trackTree (trackIndex);
    if (! track.isValid())
        return;

    engine.capturePluginStatesToTree();

    juce::ValueTree state (mixerStateType);
    state.setProperty (mixerStateVersion, 1, nullptr);
    state.setProperty (IDs::name,   track.getProperty (IDs::name), nullptr);
    if (track.hasProperty (IDs::colour))
        state.setProperty (IDs::colour, track.getProperty (IDs::colour), nullptr);
    state.setProperty (IDs::volume,    track.getProperty (IDs::volume, 1.0), nullptr);
    state.setProperty (IDs::pan,       track.getProperty (IDs::pan, 0.0), nullptr);
    state.setProperty (IDs::stereoSep, track.getProperty (IDs::stereoSep, 0.0), nullptr);
    for (int i = 0; i < track.getNumChildren(); ++i)
        if (auto c = track.getChild (i); c.hasType (IDs::Slot))
            state.appendChild (c.createCopy(), nullptr);

    auto defName = juce::File::createLegalFileName (track.getProperty (IDs::name).toString());
    if (defName.isEmpty())
        defName = "Mixer State";
    auto seed = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                    .getChildFile (defName + ".odms");

    fileChooser = std::make_unique<juce::FileChooser> ("Save mixer state", seed, "*.odms");
    fileChooser->launchAsync (juce::FileBrowserComponent::saveMode
                              | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
        [state] (const juce::FileChooser& fc)
        {
            auto f = fc.getResult();
            if (f == juce::File())
                return;
            if (! f.hasFileExtension ("odms"))
                f = f.withFileExtension ("odms");
            if (! f.replaceWithText (state.toXmlString()))
                Dialogs::showInfoAsync (juce::MessageBoxIconType::WarningIcon,
                                        "Save failed", f.getFullPathName());
        });
}

void MixerPanel::importMixerState (int trackIndex)
{
    fileChooser = std::make_unique<juce::FileChooser> ("Load mixer state",
        juce::File::getSpecialLocation (juce::File::userDocumentsDirectory), "*.odms");
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                              | juce::FileBrowserComponent::canSelectFiles,
        [safe = juce::Component::SafePointer<MixerPanel> (this), trackIndex] (const juce::FileChooser& fc)
        {
            if (safe == nullptr)
                return;
            auto f = fc.getResult();
            if (f == juce::File())
                return;

            auto xml = juce::parseXML (f);
            auto state = xml != nullptr ? juce::ValueTree::fromXml (*xml) : juce::ValueTree();
            if (! state.isValid() || ! state.hasType (mixerStateType))
            {
                Dialogs::showInfoAsync (juce::MessageBoxIconType::WarningIcon,
                                        "Cannot load mixer state",
                                        "Not a valid mikeDAW Mixer State file.");
                return;
            }
            safe->applyMixerState (trackIndex, state);
        });
}

void MixerPanel::applyMixerState (int trackIndex, const juce::ValueTree& state)
{
    auto track = trackTree (trackIndex);
    if (! track.isValid())
        return;

    {
        const juce::ScopedValueSetter<bool> svs (replacingChain, true);
        for (int i = track.getNumChildren() - 1; i >= 0; --i)
            if (track.getChild (i).hasType (IDs::Slot))
                track.removeChild (i, nullptr);
    }

    for (int i = 0; i < state.getNumChildren(); ++i)
    {
        auto src = state.getChild (i);
        if (! src.hasType (IDs::Slot))
            continue;
        auto s = src.createCopy();
        s.setProperty (IDs::uid, Model::newId(), nullptr);
        track.appendChild (s, nullptr);
        if (onLocalVstAdded && s.getProperty (IDs::type).toString() == "plugin")
            onLocalVstAdded ("m" + juce::String (trackIndex) + "s" + s.getProperty (IDs::uid).toString(),
                             effectDisplayName (s));
    }

    track.setProperty (IDs::volume,    state.getProperty (IDs::volume, 1.0), nullptr);
    track.setProperty (IDs::pan,       state.getProperty (IDs::pan, 0.0), nullptr);
    track.setProperty (IDs::stereoSep, state.getProperty (IDs::stereoSep, 0.0), nullptr);
    if (state.hasProperty (IDs::name))
        track.setProperty (IDs::name, state.getProperty (IDs::name), nullptr);
    if (state.hasProperty (IDs::colour))
        track.setProperty (IDs::colour, state.getProperty (IDs::colour), nullptr);
}

void MixerPanel::timerCallback()
{
    const int viewX = viewport.getViewPositionX();
    const int viewRight = viewX + viewport.getWidth();
    for (auto* s : strips)
    {
        if (s->getRight() <= viewX || s->getX() >= viewRight)
            continue;

        s->updateAutoFollow();

        bool active = s->isMeterActive();
        if (active || s->wasMeterActive)
            s->repaint (s->getMeterArea());
        s->wasMeterActive = active;
    }

    const double ticks = engine.getPositionTicks();
    if (engine.isPlaying() || ticks != lastTimecodeTicks)
    {
        lastTimecodeTicks = ticks;
        if (timecode != nullptr)
            timecode->repaint();
    }
}

void MixerPanel::valueTreePropertyChanged (juce::ValueTree& t, const juce::Identifier& id)
{
    if (t.hasType (IDs::Track))
    {
        int idx = (int) t.getProperty (IDs::index, -1);
        if (juce::isPositiveAndBelow (idx, strips.size()))
            strips[idx]->refreshFromTree();
        if (idx == selectedTrack)
        {
            if (rackMode == RackMode::deviceChain)
                deviceChain->refresh();
            else
                rack->refresh();
        }
        if (id == IDs::routeTo)
            for (auto* s : strips)
                s->repaint();
    }
    else if (t.hasType (IDs::Slot))
    {
        if (rackMode == RackMode::deviceChain)
        {
            if ((int) t.getParent().getProperty (IDs::index, -1) == selectedTrack)
            {
                if (id == IDs::slotIndex)
                    deviceChain->rebuild();
                else
                    deviceChain->refresh();
            }
        }
        else
            rack->refresh();
    }
}

void MixerPanel::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child)
{
    if (child.hasType (IDs::Slot))
    {
        if (rackMode == RackMode::deviceChain)
        {
            if ((int) parent.getProperty (IDs::index, -1) == selectedTrack)
                deviceChain->rebuild();
        }
        else
            rack->refresh();
    }
    else if (child.hasType (IDs::Track) || child.hasType (IDs::Mixer))
    {
        if (rebuildPending)
            return;
        rebuildPending = true;
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MixerPanel> (this)]
        {
            if (safe == nullptr)
                return;
            safe->rebuildPending = false;
            safe->rebuildStrips();
        });
    }
}

void MixerPanel::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int)
{
    if (child.hasType (IDs::Slot) && onEffectRemoved && ! replacingChain)
        onEffectRemoved (effectDisplayName (child), (int) parent.getProperty (IDs::index, -1));

    valueTreeChildAdded (parent, child);
}

void MixerPanel::paint (juce::Graphics& g)
{
    g.fillAll (Colors::panelBg);
}

void MixerPanel::resized()
{
    auto full = getLocalBounds().reduced (4);

    constexpr int chipW = 72, chipH = 18;
    if (modeChip != nullptr)
        modeChip->setBounds (full.getRight() - chipW, full.getY(), chipW, chipH);

    auto r = full;
    bool chain = rackMode == RackMode::deviceChain;

    if (! chain)
    {
        rack->setBounds (r.removeFromRight (EffectRack::rackWidth));
        r.removeFromRight (4);
    }
    else
    {
        constexpr int chainH = 190;
        deviceChain->setBounds (r.removeFromBottom (chainH));
        r.removeFromBottom (4);
    }

    constexpr int barH = 15;
    auto topBar = r.removeFromTop (barH);
    if (chain)
        topBar.removeFromRight (chipW + 4);
    scrollBar->setBounds (topBar);
    r.removeFromTop (3);

    constexpr int tcH = 16;
    timecode->setBounds (r.removeFromTop (tcH));
    r.removeFromTop (4);
    viewport.setBounds (r);

    const int stripW = 66, addW = 26;
    stripContainer.setSize (strips.size() * stripW + addW + 2,
                            juce::jmax (240, viewport.getMaximumVisibleHeight()));
    for (int i = 0; i < strips.size(); ++i)
        strips[i]->setBounds (i * stripW, 0, stripW, stripContainer.getHeight());
    addStripBtn->setBounds (strips.size() * stripW + 2, 0, addW - 2, stripContainer.getHeight());

    updateScrollBar();
    if (modeChip != nullptr)
        modeChip->toFront (false);
}

void MixerPanel::updateScrollBar()
{
    if (scrollBar == nullptr)
        return;
    int contentW = stripContainer.getWidth();
    int viewW = viewport.getWidth();
    scrollBar->setVisible (contentW > viewW);
    scrollBar->setMetrics (contentW, viewW, viewport.getViewPositionX());
}
