#pragma once
#include <JuceHeader.h>
#include <vector>
#include <algorithm>
#include <cmath>
#include <limits>
#include "../Model/ProjectModel.h"
#include "../Engine/AudioEngine.h"
#include "../Engine/PitchDetect.h"
#include "AppLookAndFeel.h"
#include "ValueKnob.h"
#include "WheelComboBox.h"
#include "ZoomScrollBar.h"
#include "Dialogs.h"

class SamplerSettingsPanel : public juce::Component,
                             private juce::ValueTree::Listener
{
    struct LedToggle : public juce::Component,
                       public juce::SettableTooltipClient
    {
        std::function<bool()> getState;
        std::function<void (bool)> setState;
        juce::String caption;

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (e.mods.isMiddleButtonDown() || ! isEnabled())
                return;
            if (getState && setState)
                setState (! getState());
        }

        void paint (juce::Graphics& g) override
        {
            bool on = getState && getState();
            float enabledAlpha = isEnabled() ? 1.0f : 0.35f;
            auto led = juce::Rectangle<float> (0.0f, 0.0f, 12.0f, 12.0f)
                           .withCentre ({ caption.isNotEmpty() ? 6.0f : getWidth() * 0.5f,
                                          getHeight() * 0.5f });
            if (on)
            {
                g.setColour (Colors::stepOn.withAlpha (0.35f * enabledAlpha));
                g.fillEllipse (led.expanded (2.0f));
                g.setColour (Colors::stepOn.withMultipliedAlpha (enabledAlpha));
            }
            else
            {
                g.setColour (Colors::stepOff.withMultipliedAlpha (enabledAlpha));
            }
            g.fillEllipse (led);
            g.setColour (Colors::outline.withMultipliedAlpha (enabledAlpha));
            g.drawEllipse (led, 1.0f);

            if (caption.isNotEmpty())
            {
                g.setColour ((on ? Colors::text : Colors::textDim)
                                 .withMultipliedAlpha (enabledAlpha));
                g.setFont (juce::Font (juce::FontOptions (10.5f)));
                g.drawText (caption, 16, 0, getWidth() - 16, getHeight(),
                            juce::Justification::centredLeft);
            }
        }

        void enablementChanged() override { repaint(); }
    };

    struct IntDragBox : public juce::Component
    {
        int minV = 1, maxV = 48, value = 1;
        std::function<void (int)> onChange;
        std::function<void()> onPopup;
        std::function<juce::String (int)> valueText;

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (e.mods.isMiddleButtonDown() || ! isEnabled())
                return;
            if (e.mods.isPopupMenu())
            {
                if (onPopup)
                    onPopup();
                return;
            }
            dragStart = value;
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (e.mods.isMiddleButtonDown() || ! isEnabled())
                return;
            int next = juce::jlimit (minV, maxV, dragStart - e.getDistanceFromDragStartY() / 6);
            if (next != value && onChange)
                onChange (next);
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            if (! e.mods.isMiddleButtonDown() && ! e.mods.isPopupMenu() && isEnabled()
                && e.getDistanceFromDragStart() < 3 && onPopup)
                onPopup();
        }

        void mouseWheelMove (const juce::MouseEvent&,
                             const juce::MouseWheelDetails& wheel) override
        {
            if (! isEnabled() || wheel.deltaY == 0.0f)
                return;
            int next = juce::jlimit (minV, maxV, value + (wheel.deltaY > 0.0f ? 1 : -1));
            if (next != value && onChange)
                onChange (next);
        }

        void paint (juce::Graphics& g) override
        {
            auto b = getLocalBounds().toFloat().reduced (0.5f);
            float alpha = isEnabled() ? 1.0f : 0.35f;
            g.setColour (Colors::panelDark.withMultipliedAlpha (alpha));
            g.fillRoundedRectangle (b, 3.0f);
            g.setColour (Colors::outline.withMultipliedAlpha (alpha));
            g.drawRoundedRectangle (b, 3.0f, 1.0f);
            g.setColour (Colors::accent.withMultipliedAlpha (alpha));
            g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
            g.drawText (valueText ? valueText (value) : juce::String (value),
                        getLocalBounds(), juce::Justification::centred);
        }

        int dragStart = 1;
    };

    struct TimeKnob : public ValueKnob
    {
        std::function<void()> showPresetMenu;

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (e.mods.isPopupMenu())
            {
                if (showPresetMenu)
                    showPresetMenu();
                return;
            }
            ValueKnob::mouseDown (e);
        }
    };

    struct FolderIconButton : public juce::Button
    {
        FolderIconButton() : juce::Button ("reveal") {}
        void paintButton (juce::Graphics& g, bool over, bool down) override
        {
            auto b = getLocalBounds().toFloat().reduced (getWidth() * 0.14f);
            float alpha = isEnabled() ? (down ? 1.0f : over ? 0.95f : 0.8f) : 0.35f;
            auto col = (over && isEnabled() ? Colors::accent : Colors::textDim)
                           .withMultipliedAlpha (alpha);
            float lw = juce::jmax (1.5f, b.getWidth() * 0.08f);
            auto body = b.withTrimmedTop (b.getHeight() * 0.22f);
            juce::Path tab;
            tab.startNewSubPath (body.getX(), body.getY());
            tab.lineTo (body.getX() + body.getWidth() * 0.14f, b.getY() + b.getHeight() * 0.08f);
            tab.lineTo (body.getX() + body.getWidth() * 0.46f, b.getY() + b.getHeight() * 0.08f);
            tab.lineTo (body.getX() + body.getWidth() * 0.56f, body.getY());
            g.setColour (col);
            g.strokePath (tab, juce::PathStrokeType (lw, juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
            g.drawRoundedRectangle (body, body.getWidth() * 0.08f, lw);
        }
    };

    struct GlyphButton : public juce::Button
    {
        enum class Glyph { open, clear };
        explicit GlyphButton (Glyph glyphType) : juce::Button ("glyph"), glyph (glyphType) {}
        void paintButton (juce::Graphics& g, bool over, bool down) override
        {
            auto b = getLocalBounds().toFloat().reduced (getWidth() * 0.2f);
            float alpha = isEnabled() ? (down ? 1.0f : over ? 0.95f : 0.75f) : 0.3f;
            auto col = (over && isEnabled() ? Colors::accent : Colors::textDim)
                           .withMultipliedAlpha (alpha);
            g.setColour (col);
            float lw = juce::jmax (1.4f, b.getWidth() * 0.13f);
            if (glyph == Glyph::open)
            {
                auto a = b.reduced (b.getWidth() * 0.08f);
                g.drawLine (a.getCentreX(), a.getY(), a.getCentreX(), a.getBottom() - a.getHeight() * 0.3f, lw);
                juce::Path head;
                float hy = a.getBottom() - a.getHeight() * 0.3f;
                head.startNewSubPath (a.getCentreX() - a.getWidth() * 0.22f, hy - a.getHeight() * 0.22f);
                head.lineTo (a.getCentreX(), hy);
                head.lineTo (a.getCentreX() + a.getWidth() * 0.22f, hy - a.getHeight() * 0.22f);
                g.strokePath (head, juce::PathStrokeType (lw, juce::PathStrokeType::curved,
                                                          juce::PathStrokeType::rounded));
                g.drawLine (a.getX(), a.getBottom(), a.getRight(), a.getBottom(), lw);
            }
            else
            {
                g.drawLine (b.getX(), b.getY(), b.getRight(), b.getBottom(), lw);
                g.drawLine (b.getRight(), b.getY(), b.getX(), b.getBottom(), lw);
            }
        }
        Glyph glyph;
    };

    struct RootKeyboard : public juce::Component
    {
        static constexpr int firstKey = 24;
        static constexpr int lastKey  = 107;

        std::function<int()> getRoot;
        std::function<void (int)> setRoot;
        std::function<void (int, bool)> playKey;

        static bool isBlack (int key)
        {
            int n = key % 12;
            return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
        }

        int numWhite() const
        {
            int c = 0;
            for (int k = firstKey; k <= lastKey; ++k)
                if (! isBlack (k)) ++c;
            return c;
        }

        juce::Rectangle<float> whiteRect (int key) const
        {
            float w = (float) getWidth() / (float) juce::jmax (1, numWhite());
            int idx = 0;
            for (int k = firstKey; k < key; ++k)
                if (! isBlack (k)) ++idx;
            return { idx * w, 0.0f, w, (float) getHeight() };
        }

        juce::Rectangle<float> blackRect (int key) const
        {
            float w = (float) getWidth() / (float) juce::jmax (1, numWhite());
            int whitesBelow = 0;
            for (int k = firstKey; k < key; ++k)
                if (! isBlack (k)) ++whitesBelow;
            float cx = whitesBelow * w;
            float bw = w * 0.62f;
            return { cx - bw * 0.5f, 0.0f, bw, getHeight() * 0.6f };
        }

        int keyAt (juce::Point<int> p) const
        {
            for (int k = firstKey; k <= lastKey; ++k)
                if (isBlack (k) && blackRect (k).contains (p.toFloat()))
                    return k;
            float w = (float) getWidth() / (float) juce::jmax (1, numWhite());
            int wi = juce::jlimit (0, numWhite() - 1, (int) ((float) p.x / w));
            int idx = 0;
            for (int k = firstKey; k <= lastKey; ++k)
                if (! isBlack (k))
                {
                    if (idx == wi) return k;
                    ++idx;
                }
            return firstKey;
        }

        void paint (juce::Graphics& g) override
        {
            int root = getRoot ? getRoot() : 60;

            for (int k = firstKey; k <= lastKey; ++k)
            {
                if (isBlack (k)) continue;
                auto r = whiteRect (k);
                g.setColour (k == root ? Colors::accent : Colors::pianoWhiteKey);
                g.fillRect (r.reduced (0.5f, 0.0f));
                g.setColour (Colors::outline);
                g.drawRect (r, 1.0f);
                if (k % 12 == 0)
                {
                    g.setColour (k == root ? Colors::text : Colors::pianoKeyText);
                    g.setFont (juce::Font (juce::FontOptions (8.5f)));
                    g.drawText ("C" + juce::String (k / 12),
                                r.toNearestInt().reduced (1).withTrimmedTop (r.getHeight() * 0.55f),
                                juce::Justification::centredBottom, false);
                }
            }
            for (int k = firstKey; k <= lastKey; ++k)
            {
                if (! isBlack (k)) continue;
                auto r = blackRect (k);
                g.setColour (k == root ? Colors::accent : Colors::pianoBlackKey);
                g.fillRect (r);
                g.setColour (Colors::outline);
                g.drawRect (r, 1.0f);
            }
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (e.mods.isMiddleButtonDown())
                return;
            int k = keyAt (e.getPosition());
            heldKey = k;
            if (setRoot) setRoot (k);
            if (playKey) playKey (k, true);
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (e.mods.isMiddleButtonDown())
                return;
            int k = keyAt (e.getPosition());
            if (k != heldKey)
            {
                if (playKey && heldKey >= 0) playKey (heldKey, false);
                heldKey = k;
                if (setRoot) setRoot (k);
                if (playKey) playKey (k, true);
            }
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            if (e.mods.isMiddleButtonDown())
                return;
            if (playKey && heldKey >= 0) playKey (heldKey, false);
            heldKey = -1;
        }

        int heldKey = -1;
    };

public:
    explicit SamplerSettingsPanel (AudioEngine& e) : engine (e)
    {
        waveScroll.setMinSpan (1.0 / 128.0);
        waveScroll.setMaxSpan (1.0);
        waveScroll.setRange (1.0, 0.0, 1.0);
        waveScroll.onRangeChanged = [this] (double start, double end)
        {
            waveViewStart = juce::jlimit (0.0, 1.0, start);
            waveViewEnd = juce::jlimit (waveViewStart + 1.0 / 128.0, 1.0, end);
            repaint (waveR);
        };
        addAndMakeVisible (waveScroll);

        onLed.getState  = [this] { return target.isValid() && ! (bool) target.getProperty (IDs::mute, false); };
        onLed.setState  = [this] (bool v) { if (target.isValid()) target.setProperty (IDs::mute, ! v, nullptr); };
        addAndMakeVisible (onLed);

        addAndMakeVisible (rangeBox);
        rangeBox.minV = 1; rangeBox.maxV = 48;
        rangeBox.onChange = [this] (int v)
        {
            if (target.isValid())
                target.setProperty (IDs::pitchRange, v, nullptr);
        };

        addChildComponent (trackBox);
        trackBox.minV = 0; trackBox.maxV = Model::maxMixerTracks;
        trackBox.valueText = [] (int v) { return v == 0 ? juce::String ("M") : juce::String (v); };
        trackBox.onChange = [this] (int v)
        {
            if (target.isValid())
                setRoutedMixerTrack (v);
        };
        trackBox.onPopup = [this] { showTrackMenu(); };

        openBtn.setTooltip ("Load a different sample");
        openBtn.onClick = [this] { chooseFile(); };
        addAndMakeVisible (openBtn);

        revealBtn.setTooltip ("Show sample in Explorer");
        revealBtn.onClick = [this] { auto f = sampleFile(); if (f.existsAsFile()) f.revealToUser(); };
        addAndMakeVisible (revealBtn);

        clearBtn.setTooltip ("Clear sample");
        clearBtn.onClick = [this]
        {
            if (isChannel && target.isValid())
                target.setProperty (IDs::samplePath, "", nullptr);
        };
        addChildComponent (clearBtn);

        declickBox.addItem ("Out only (no bleeding)", 1);
        declickBox.addItem ("Transient (no bleeding)", 2);
        declickBox.addItem ("Transient (bleeding)", 3);
        declickBox.addItem ("Generic (bleeding)", 4);
        declickBox.addItem ("Smooth (bleeding)", 5);
        declickBox.addItem ("Crossfade (bleeding)", 6);
        declickBox.addItem ("No declicking", 7);
        declickBox.onChange = [this]
        {
            if (target.isValid() && declickBox.getSelectedId() > 0)
                target.setProperty (IDs::declickMode, declickBox.getSelectedId() - 1, nullptr);
        };
        addAndMakeVisible (declickBox);

        stretchBox.addSectionHeading ("Realtime");
        stretchBox.addItem ("Auto", 12);
        stretchBox.addItem ("Resample", 1);
        stretchBox.addItem ("Stretch", 2);
        stretchBox.addItem ("Stretch Pro", 3);
        stretchBox.addSectionHeading ("High quality");
        stretchBox.addItem ("e3 Generic", 4);
        stretchBox.addItem ("e3 Mono", 5);
        stretchBox.addSectionHeading ("Legacy");
        stretchBox.addItem ("e2 Generic", 6);
        stretchBox.addItem ("e2 Transient", 7);
        stretchBox.addItem ("e2 Mono", 8);
        stretchBox.addItem ("e2 Speech", 9);
        stretchBox.addSectionHeading ("Slicing");
        stretchBox.addItem ("Slice Stretch", 10);
        stretchBox.addItem ("Slice Map", 11);
        stretchBox.setSelectedId (1, juce::dontSendNotification);
        stretchBox.onChange = [this]
        {
            if (target.isValid() && stretchBox.getSelectedId() > 0)
            {
                target.setProperty (IDs::stretchMode,
                                    stretchModeForId (stretchBox.getSelectedId()), nullptr);
                updateStretchAvailability();
            }
        };
        addAndMakeVisible (stretchBox);

        makePropertyLed (keepDiskLed, IDs::sampleKeepOnDisk, "Keep on disk",
                         [this] { updatePrecomputedAvailability(); });
        makePropertyLed (resampleLed, IDs::sampleResample, "Resample");
        resampleLed.setTooltip ("High-quality (windowed-sinc) interpolation with anti-aliasing");
        makePropertyLed (regionsLed, IDs::loadRegions, "Load regions");
        regionsLed.setTooltip ("Apply the sample's embedded loop points");
        makePropertyLed (slicesLed, IDs::loadSliceMarkers, "Load slice markers");
        slicesLed.setTooltip ("Show the sample's embedded cue markers");

        makePropertyLed (useLoopLed, IDs::useLoopPoints, "Use loop points",
                         [this] { updatePlaybackAvailability(); });
        makePropertyLed (pingPongLed, IDs::pingPongLoop, "Ping pong loop");

        makeEffectLed (fxDcLed,       IDs::fxRemoveDC,   "Remove DC offset");
        makeEffectLed (fxNormLed,     IDs::fxNormalize,  "Normalize");
        makeEffectLed (fxRevLed,      IDs::fxReverse,    "Reverse");
        makeEffectLed (fxPolLed,      IDs::fxRevPolarity,"Reverse polarity");
        makeEffectLed (fxFadeStLed,   IDs::fxFadeStereo, "Fade stereo");
        makeEffectLed (fxSwapStLed,   IDs::fxSwapStereo, "Swap stereo");

        rootKb.getRoot = [this] { return juce::jlimit (0, 127, (int) target.getProperty (IDs::rootNote, 60)); };
        rootKb.setRoot = [this] (int k) { if (target.isValid()) target.setProperty (IDs::rootNote, k, nullptr); };
        rootKb.playKey = [this] (int k, bool on)
        {
            if (! target.isValid()) return;
            int cid = (int) target.getProperty (IDs::id, 0);
            if (on) engine.auditionNote (cid, k, 100);
            else    engine.auditionNoteOff (cid, k);
        };
        addChildComponent (rootKb);

        resetBtn.setTooltip ("Reset root note to C5 and clear fine tune");
        resetBtn.onClick = [this]
        {
            if (! target.isValid()) return;
            target.setProperty (IDs::rootNote, 60, nullptr);
            target.setProperty (IDs::fineTune, 0.0, nullptr);
        };
        addChildComponent (resetBtn);

        detectBtn.setTooltip ("Detect the sample's pitch (Edison-style) and tune the root note to it");
        detectBtn.onClick = [this] { runDetection(); };
        addChildComponent (detectBtn);

        fineKnob = std::make_unique<ValueKnob>();
        fineKnob->setRange (-100.0, 100.0, 0.0);
        fineKnob->setDoubleClickReturnValue (true, 0.0);
        fineKnob->setTooltip ("Fine tune (cents)");
        fineKnob->onValueChange = [this]
        {
            if (target.isValid()) target.setProperty (IDs::fineTune, fineKnob->getValue(), nullptr);
        };
        addChildComponent (*fineKnob);
    }

    ~SamplerSettingsPanel() override
    {
        ++targetGeneration;
        ++detectionGeneration;
        ++waveformGeneration;
        waveformPool.removeAllJobs (true, 5000);
        analysisPool.removeAllJobs (true, 5000);
        target.removeListener (this);
        routeChannel.removeListener (this);
        routeTrack.removeListener (this);
    }

    std::function<void()> onTargetGone;
    std::function<void (juce::String)> onTitleChanged;

    void setTarget (juce::ValueTree node)
    {
        ++targetGeneration;
        ++detectionGeneration;
        target.removeListener (this);
        routeChannel.removeListener (this);
        routeTrack.removeListener (this);
        target = node;
        isChannel = target.hasType (IDs::Channel);
        if (target.isValid())
            target.addListener (this);
        bindRoutingNodes();

        rebuildKnobs();

        trackBox.setVisible (target.isValid());
        clearBtn.setVisible (isChannel);

        detectedRegions.clear();
        detectStatus = {};
        rootKb.setVisible (isChannel);
        resetBtn.setVisible (isChannel);
        detectBtn.setVisible (isChannel);
        fineKnob->setVisible (isChannel);

        syncControls();
        loadWaveform();
        emitTitle();
        resized();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colors::panelBg);

        g.setColour (Colors::header);
        g.fillRect (headerR);
        g.setColour (Colors::outline);
        g.fillRect (headerR.withTrimmedTop (headerR.getHeight() - 1));

        g.setColour (Colors::text);
        g.setFont (juce::Font (juce::FontOptions (19.0f, juce::Font::bold)));
        g.drawFittedText (target.isValid() ? displayName() : "(no sample)",
                          headerR.reduced (12, 0).withTrimmedRight (clusterWidth + 12),
                          juce::Justification::centredLeft, 1);

        paintCapsLabels (g, headerLabels);

        g.setColour (Colors::textDim);
        g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
        g.drawText ("FILE", fileLabelR, juce::Justification::centredLeft);
        {
            auto box = fileNameR.toFloat();
            g.setColour (Colors::panelDark);
            g.fillRoundedRectangle (box, 3.0f);
            g.setColour (Colors::outline);
            g.drawRoundedRectangle (box, 3.0f, 1.0f);
            g.setColour (Colors::text);
            g.setFont (juce::Font (juce::FontOptions (12.0f)));
            g.drawText (fileDisplay(), fileNameR.reduced (7, 0), juce::Justification::centredLeft, true);
        }

        drawGroup (g, contentR, "Content");
        drawGroup (g, playbackR, "Playback");
        drawGroup (g, stretchR, "Time stretching");
        drawGroup (g, effectsR, "Precomputed effects");

        g.setColour (Colors::textDim);
        g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
        g.drawText ("DECLICKING", declickLabelR, juce::Justification::centredLeft);
        g.drawText ("MODE", stretchModeLabelR, juce::Justification::centredLeft);

        paintCapsLabels (g, knobLabels);

        if (isChannel)
        {
            int root = juce::jlimit (0, 127, (int) target.getProperty (IDs::rootNote, 60));
            g.setColour (Colors::text);
            g.setFont (juce::Font (juce::FontOptions (11.5f, juce::Font::bold)));
            g.drawText ("Root note: " + keyName (root), rootCaptionR,
                        juce::Justification::centredLeft, true);

            g.setColour (Colors::textDim);
            g.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::bold)));
            g.drawText ("FINE TUNE", fineLabelR, juce::Justification::centredLeft, false);

            if (detectStatus.isNotEmpty())
            {
                g.setColour (Colors::accent);
                g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
                g.drawText (detectStatus, statusR, juce::Justification::centredLeft, true);
            }
        }

        drawWaveform (g);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        headerLabels.clear();
        knobLabels.clear();

        layoutHeader (r.removeFromTop (headerH));
        layoutFileRow (r.removeFromTop (fileRowH));
        r.removeFromTop (4);

        int rootStripH = isChannel ? (rootRowH + 4 + keyboardH + 6) : 0;
        int waveMinH = 72 + ZoomScrollBar::barHeight + 21;
        int bodyH = juce::jlimit (0, juce::jmax (0, r.getHeight() - rootStripH - waveMinH),
                                  topGroupH + groupGapV + bottomGroupH);
        layoutBody (r.removeFromTop (bodyH).reduced (10, 0));
        r.removeFromTop (6);

        if (isChannel)
        {
            auto strip = r.removeFromTop (juce::jmin (r.getHeight(), rootRowH + 4 + keyboardH));
            auto ctrl = strip.removeFromTop (juce::jmin (strip.getHeight(), rootRowH))
                             .reduced (10, 0);
            rootCaptionR = ctrl.removeFromLeft (juce::jmin (128, ctrl.getWidth() / 3));
            resetBtn.setBounds (ctrl.removeFromLeft (52).reduced (1, 2));
            detectBtn.setBounds (ctrl.removeFromLeft (56).reduced (1, 2));
            ctrl.removeFromLeft (8);
            fineKnob->setBounds (ctrl.removeFromLeft (22).withSizeKeepingCentre (22, 22));
            ctrl.removeFromLeft (4);
            fineLabelR = ctrl.removeFromLeft (juce::jmin (56, ctrl.getWidth()));
            statusR = ctrl;
            strip.removeFromTop (4);
            rootKb.setBounds (strip.reduced (10, 0));
            r.removeFromTop (6);
        }

        auto waveArea = r.reduced (10, 0).withTrimmedBottom (8);
        waveScroll.setBounds (waveArea.removeFromBottom (ZoomScrollBar::barHeight));
        waveArea.removeFromBottom (3);
        waveR = waveArea;
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown())
            return;
        if (! waveR.contains (e.getPosition()))
            return;
        if (e.mods.isPopupMenu())
        {
            showWaveMenu();
            return;
        }

        waveDrag = markerAt (e.position.x, e.position.y);
        if (waveDrag == WaveDrag::none)
            waveDrag = WaveDrag::preview;
        waveDragStarted = false;
        dragStartSampleStart = property01 (IDs::smpStart, 0.0);
        dragStartSampleLength = sampleWindowLength();
        dragStartOffset = property01 (IDs::startOffset, 0.0);
        dragStartLoopStart = property01 (IDs::loopStart, 0.0);
        dragStartLoopEnd = property01 (IDs::loopEnd, 1.0);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown() || waveDrag == WaveDrag::none)
            return;
        if (waveDrag == WaveDrag::preview)
        {
            if (! waveDragStarted && e.getDistanceFromDragStart() >= 4)
                beginWaveDrag();
            return;
        }
        dragWaveMarker (e);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! e.mods.isMiddleButtonDown() && waveDrag == WaveDrag::preview && ! waveDragStarted
            && waveR.contains (e.getPosition()))
        {
            auto f = sampleFile();
            if (f.existsAsFile())
                engine.previewFile (f, normalizedAtX ((float) e.x));
        }
        if (waveDrag != WaveDrag::none && waveDrag != WaveDrag::preview)
        {
            waveStatus = idleWaveStatus();
            repaint (waveR);
        }
        waveDrag = WaveDrag::none;
        waveDragStarted = false;
        updateWaveCursor ((float) e.x, (float) e.y);
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        updateWaveCursor ((float) e.x, (float) e.y);
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        setMouseCursor (juce::MouseCursor::NormalCursor);
    }

    void mouseWheelMove (const juce::MouseEvent& e,
                         const juce::MouseWheelDetails& wheel) override
    {
        if (! waveR.contains (e.getPosition()) || wheel.deltaY == 0.0f)
            return;

        double span = waveViewEnd - waveViewStart;
        double anchor = normalizedAtX ((float) e.x);
        double anchorFraction = span > 0.0 ? (anchor - waveViewStart) / span : 0.5;
        double nextSpan = juce::jlimit (1.0 / 128.0, 1.0,
                                        span * std::exp (-wheel.deltaY * 2.0));
        double nextStart = anchor - anchorFraction * nextSpan;
        nextStart = juce::jlimit (0.0, 1.0 - nextSpan, nextStart);
        setWaveView (nextStart, nextStart + nextSpan);
    }

private:
    enum class WaveDrag
    {
        none,
        preview,
        sampleStart,
        sampleEnd,
        startOffset,
        loopStart,
        loopEnd
    };

    double property01 (const juce::Identifier& property, double fallback) const
    {
        return target.isValid()
                   ? juce::jlimit (0.0, 1.0, (double) target.getProperty (property, fallback))
                   : fallback;
    }

    double sampleWindowLength() const
    {
        double start = property01 (IDs::smpStart, 0.0);
        return juce::jlimit (0.000001, juce::jmax (0.000001, 1.0 - start),
                            property01 (IDs::smpLength, 1.0));
    }

    double sampleWindowEnd() const
    {
        return juce::jmin (1.0, property01 (IDs::smpStart, 0.0) + sampleWindowLength());
    }

    double logicalToRaw (double logical) const
    {
        logical = juce::jlimit (0.0, 1.0, logical);
        if ((bool) target.getProperty (IDs::fxReverse, false))
            logical = 1.0 - logical;
        return property01 (IDs::smpStart, 0.0) + logical * sampleWindowLength();
    }

    double rawToLogical (double raw) const
    {
        double logical = (raw - property01 (IDs::smpStart, 0.0)) / sampleWindowLength();
        logical = juce::jlimit (0.0, 1.0, logical);
        return (bool) target.getProperty (IDs::fxReverse, false) ? 1.0 - logical : logical;
    }

    double normalizedAtX (float x) const
    {
        if (waveR.getWidth() <= 0)
            return waveViewStart;
        double fraction = juce::jlimit (0.0, 1.0,
                                        (x - waveR.getX()) / (double) waveR.getWidth());
        return waveViewStart + fraction * (waveViewEnd - waveViewStart);
    }

    float xForNormalized (double normalized) const
    {
        double span = juce::jmax (1.0 / 128.0, waveViewEnd - waveViewStart);
        return waveR.getX() + (float) ((normalized - waveViewStart) / span * waveR.getWidth());
    }

    void setWaveView (double start, double end)
    {
        double span = juce::jlimit (1.0 / 128.0, 1.0, end - start);
        waveViewStart = juce::jlimit (0.0, 1.0 - span, start);
        waveViewEnd = waveViewStart + span;
        waveScroll.setRange (1.0, waveViewStart, waveViewEnd);
        repaint (waveR);
    }

    WaveDrag markerAt (float x, float y) const
    {
        if (! waveR.contains (juce::roundToInt (x), juce::roundToInt (y))
            || ! target.isValid() || peaks.empty())
            return WaveDrag::none;

        constexpr float hit = 8.0f;
        bool top = y < waveR.getY() + 15.0f;
        bool bottom = y > waveR.getBottom() - 15.0f;
        bool keepOnDisk = (bool) target.getProperty (IDs::sampleKeepOnDisk, false);
        auto isNearMarker = [&] (double normalized)
        {
            return std::abs (x - xForNormalized (normalized)) <= hit;
        };

        if (! keepOnDisk && top && isNearMarker (property01 (IDs::smpStart, 0.0)))
            return WaveDrag::sampleStart;
        if (! keepOnDisk && top && isNearMarker (sampleWindowEnd()))
            return WaveDrag::sampleEnd;
        if (bottom && isNearMarker (logicalToRaw (property01 (IDs::startOffset, 0.0))))
            return WaveDrag::startOffset;

        bool editableLoop = (bool) target.getProperty (IDs::useLoopPoints, false)
                            && ! (bool) target.getProperty (IDs::pingPongLoop, false);
        if (editableLoop && ! bottom
            && isNearMarker (logicalToRaw (property01 (IDs::loopStart, 0.0))))
            return WaveDrag::loopStart;
        if (editableLoop
            && isNearMarker (logicalToRaw (property01 (IDs::loopEnd, 1.0))))
            return WaveDrag::loopEnd;

        if (! keepOnDisk && isNearMarker (property01 (IDs::smpStart, 0.0)))
            return WaveDrag::sampleStart;
        if (! keepOnDisk && isNearMarker (sampleWindowEnd()))
            return WaveDrag::sampleEnd;
        if (isNearMarker (logicalToRaw (property01 (IDs::startOffset, 0.0))))
            return WaveDrag::startOffset;
        return WaveDrag::none;
    }

    void updateWaveCursor (float x, float y)
    {
        if (! waveR.contains (juce::roundToInt (x), juce::roundToInt (y)))
        {
            setMouseCursor (juce::MouseCursor::NormalCursor);
            return;
        }
        setMouseCursor (markerAt (x, y) == WaveDrag::none
                            ? juce::MouseCursor::PointingHandCursor
                            : juce::MouseCursor::LeftRightResizeCursor);
    }

    void dragWaveMarker (const juce::MouseEvent& event)
    {
        if (! target.isValid())
            return;

        double raw = normalizedAtX ((float) event.x);
        constexpr double minimum = 0.000001;
        if (waveDrag == WaveDrag::sampleStart)
        {
            double oldEnd = juce::jmin (1.0, dragStartSampleStart + dragStartSampleLength);
            if (event.mods.isShiftDown())
            {
                double length = juce::jmin (dragStartSampleLength, 1.0);
                double start = juce::jlimit (0.0, 1.0 - length, raw);
                target.setProperty (IDs::smpStart, start, nullptr);
                target.setProperty (IDs::smpLength, length, nullptr);
            }
            else
            {
                double start = juce::jlimit (0.0, oldEnd - minimum, raw);
                target.setProperty (IDs::smpStart, start, nullptr);
                target.setProperty (IDs::smpLength, oldEnd - start, nullptr);
            }
        }
        else if (waveDrag == WaveDrag::sampleEnd)
        {
            double start = dragStartSampleStart;
            target.setProperty (
                IDs::smpLength,
                juce::jlimit (minimum, juce::jmax (minimum, 1.0 - start), raw - start),
                nullptr);
        }
        else if (waveDrag == WaveDrag::startOffset)
        {
            target.setProperty (IDs::startOffset, rawToLogical (raw), nullptr);
        }
        else if (waveDrag == WaveDrag::loopStart || waveDrag == WaveDrag::loopEnd)
        {
            double logical = rawToLogical (raw);
            double span = dragStartLoopEnd - dragStartLoopStart;
            if (event.mods.isShiftDown())
            {
                double anchor = waveDrag == WaveDrag::loopStart
                                    ? dragStartLoopStart : dragStartLoopEnd;
                double start = juce::jlimit (0.0, 1.0 - span,
                                             dragStartLoopStart + logical - anchor);
                target.setProperty (IDs::loopStart, start, nullptr);
                target.setProperty (IDs::loopEnd, start + span, nullptr);
            }
            else if (waveDrag == WaveDrag::loopStart)
            {
                target.setProperty (IDs::loopStart,
                                    juce::jlimit (0.0, dragStartLoopEnd - minimum, logical), nullptr);
            }
            else
            {
                target.setProperty (IDs::loopEnd,
                                    juce::jlimit (dragStartLoopStart + minimum, 1.0, logical), nullptr);
            }
        }
        waveStatus = markerReadout (waveDrag);
        repaint (waveR);
    }

    void beginWaveDrag()
    {
        auto file = sampleFile();
        if (! file.existsAsFile())
            return;
        auto* dragContainer = juce::DragAndDropContainer::findParentDragContainerFor (this);
        if (dragContainer == nullptr)
            return;

        waveDragStarted = true;
        juce::Image image (juce::Image::ARGB, 190, 32, true);
        juce::Graphics graphics (image);
        graphics.setColour (Colors::accent.withAlpha (0.92f));
        graphics.fillRoundedRectangle (1.0f, 1.0f, 188.0f, 30.0f, 4.0f);
        graphics.setColour (Colors::text);
        graphics.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        graphics.drawText (displayName(), 9, 0, 172, 32,
                           juce::Justification::centredLeft, true);
        dragContainer->startDragging ("playlist-audio:" + file.getFullPathName(),
                                      this, image);
    }

    void showWaveMenu()
    {
        auto file = sampleFile();
        if (! target.isValid() || ! file.existsAsFile())
            return;

        juce::PopupMenu menu;
        menu.addItem (1, "Preview");
        menu.addItem (2, "Reload from disk");
        menu.addItem (3, "Detect tempo");
        if (isChannel)
            menu.addItem (4, "Detect pitch");
        menu.addSeparator();
        menu.addItem (5, "Save processed sample as...");
        menu.addItem (6, "Show in Explorer");
        menu.addItem (7, "Copy sample path");
        menu.addSeparator();
        menu.addItem (8, "Reset zoom");
        menu.addItem (9, "Reset playback markers");

        auto capturedTarget = target;
        int generation = targetGeneration;
        menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition(),
            [safe = juce::Component::SafePointer<SamplerSettingsPanel> (this),
             capturedTarget, generation, file] (int result)
            {
                if (safe == nullptr || result == 0 || safe->target != capturedTarget
                    || safe->targetGeneration != generation)
                    return;
                if (result == 1)
                    safe->engine.previewFile (file, safe->waveViewStart);
                else if (result == 2)
                {
                    safe->engine.invalidateSampleCache (safe->rawPath());
                    safe->engine.refreshResolvedSampleReferences (safe->rawPath());
                    safe->loadWaveform();
                }
                else if (result == 3)
                    safe->detectTempo (capturedTarget);
                else if (result == 4 && safe->isChannel)
                    safe->runDetection();
                else if (result == 5)
                    safe->saveProcessedSample();
                else if (result == 6)
                    file.revealToUser();
                else if (result == 7)
                    juce::SystemClipboard::copyTextToClipboard (file.getFullPathName());
                else if (result == 8)
                    safe->setWaveView (0.0, 1.0);
                else if (result == 9)
                    safe->resetPlaybackMarkers();
            });
    }

    void saveProcessedSample()
    {
        auto source = sampleFile();
        if (! target.isValid() || ! source.existsAsFile())
            return;
        auto suggested = source.getSiblingFile (
            source.getFileNameWithoutExtension() + " processed.wav");
        saveChooser = std::make_unique<juce::FileChooser> (
            "Save processed sample", suggested, "*.wav");
        auto capturedTarget = target;
        int generation = targetGeneration;
        auto settings = target.createCopy();
        auto sourcePath = rawPath();
        saveChooser->launchAsync (
            juce::FileBrowserComponent::saveMode
                | juce::FileBrowserComponent::canSelectFiles
                | juce::FileBrowserComponent::warnAboutOverwriting,
            [safe = juce::Component::SafePointer<SamplerSettingsPanel> (this),
             capturedTarget, generation, settings, sourcePath] (const juce::FileChooser& chooserResult)
            {
                auto destination = chooserResult.getResult();
                if (safe == nullptr || destination == juce::File()
                    || safe->target != capturedTarget
                    || safe->targetGeneration != generation)
                    return;
                if (! destination.hasFileExtension ("wav"))
                    destination = destination.withFileExtension ("wav");
                safe->waveStatus = "Saving...";
                safe->repaint (safe->waveR);
                safe->analysisPool.addJob (
                    [safe, capturedTarget, generation, settings, sourcePath, destination,
                     &eng = safe->engine]
                    {
                        bool success = eng.saveProcessedSampleToWav (
                            sourcePath, settings, destination, 24);
                        juce::MessageManager::callAsync (
                            [safe, capturedTarget, generation, destination, success]
                            {
                                if (safe == nullptr || safe->target != capturedTarget
                                    || safe->targetGeneration != generation)
                                    return;
                                safe->waveStatus = success
                                    ? "Saved " + destination.getFileName()
                                    : "Save failed";
                                safe->repaint (safe->waveR);
                                if (! success)
                                    Dialogs::showInfoAsync (
                                        juce::MessageBoxIconType::WarningIcon,
                                        "Could not save sample",
                                        "The processed WAV could not be written.");
                            });
                    });
            });
    }

    void resetPlaybackMarkers()
    {
        if (! target.isValid())
            return;
        target.setProperty (IDs::startOffset, 0.0, nullptr);
        target.setProperty (IDs::loopStart, 0.0, nullptr);
        target.setProperty (IDs::loopEnd, 1.0, nullptr);
        target.setProperty (IDs::loopCrossfade, 0.0, nullptr);
    }

    void applyEmbeddedLoopIfRequested()
    {
        if (! target.isValid()
            || ! (bool) target.getProperty (IDs::loadRegions, false)
            || embeddedLoopStart < 0.0 || embeddedLoopEnd <= embeddedLoopStart)
            return;
        double start = rawToLogical (embeddedLoopStart);
        double end = rawToLogical (embeddedLoopEnd);
        if (end < start)
            std::swap (start, end);
        if (end - start < 0.0001)
            return;
        target.setProperty (IDs::loopStart, start, nullptr);
        target.setProperty (IDs::loopEnd, end, nullptr);
        target.setProperty (IDs::useLoopPoints, true, nullptr);
    }

    static juce::String formatDuration (double seconds)
    {
        int wholeSeconds = juce::jmax (0, (int) seconds);
        int minutes = wholeSeconds / 60;
        int remaining = wholeSeconds % 60;
        int millis = juce::jlimit (0, 999,
                                   (int) std::round ((seconds - wholeSeconds) * 1000.0));
        return juce::String::formatted ("%d:%02d.%03d", minutes, remaining, millis);
    }

    juce::String idleWaveStatus() const
    {
        if (waveformLoading)
            return "Loading...";
        if (! target.isValid() || ! sampleFile().existsAsFile())
            return {};
        return peaks.empty() ? "Could not read sample" : "Click to preview / drag to playlist";
    }

    juce::String markerReadout (WaveDrag which) const
    {
        juce::String name;
        double raw = 0.0;
        switch (which)
        {
            case WaveDrag::sampleStart: name = "Sample start"; raw = property01 (IDs::smpStart, 0.0); break;
            case WaveDrag::sampleEnd:   name = "Sample end";   raw = sampleWindowEnd(); break;
            case WaveDrag::startOffset: name = "Start offset"; raw = logicalToRaw (property01 (IDs::startOffset, 0.0)); break;
            case WaveDrag::loopStart:   name = "Loop start";   raw = logicalToRaw (property01 (IDs::loopStart, 0.0)); break;
            case WaveDrag::loopEnd:     name = "Loop end";     raw = logicalToRaw (property01 (IDs::loopEnd, 1.0)); break;
            default: return idleWaveStatus();
        }
        return waveDuration > 0.0
                   ? name + " " + formatDuration (raw * waveDuration)
                   : name + " " + juce::String (raw * 100.0, 1) + "%";
    }

    struct KnobBind { ValueKnob* knob; juce::Identifier prop; double def; };

    void rebuildKnobs()
    {
        knobBinds.clear();
        panKnob.reset();   volKnob.reset();   pitchKnob.reset();
        stretchPitchKnob.reset(); startOffsetKnob.reset(); formantKnob.reset();
        smpStartKnob.reset(); lengthKnob.reset(); inKnob.reset(); outKnob.reset();
        mulKnob.reset(); timeKnob.reset(); crossfadeKnob.reset(); trimKnob.reset();

        double volDef = isChannel ? 0.8 : 1.0;
        double range = juce::jmax (1.0, (double) target.getProperty (IDs::pitchRange, 2));

        panKnob      = makeKnob (IDs::pan,      -1.0, 1.0, 0.0, "Pan", true);
        volKnob      = makeKnob (IDs::volume,    0.0, 1.25, volDef, "Volume", true);
        pitchKnob    = makeKnob (IDs::pitchSemis, -range, range, 0.0, "Pitch (semitones)", true);
        startOffsetKnob = makeKnob (IDs::startOffset, 0.0, 1.0, 0.0,
                                    "Non-destructive playback start offset", true);
        stretchPitchKnob = makeKnob (IDs::stretchPitchSemis, -24.0, 24.0, 0.0,
                                     "Time-stretch pitch (semitones)", true);
        mulKnob = makeKnob (IDs::stretchMultiplier, 0.25, 4.0, 1.0,
                            "Time multiplier", true);
        formantKnob = makeKnob (IDs::formantShift, -1.0, 1.0, 0.0,
                                "Stretch Pro formant shift", true);

        timeKnob = std::make_unique<TimeKnob>();
        configureKnob (*timeKnob, IDs::stretchTime, 0.25, 4.0, 1.0,
                       "Time factor; right-click for tempo presets", true);
        timeKnob->showPresetMenu = [this] { showTimePresetMenu(); };
        {
            auto previous = timeKnob->onValueChange;
            timeKnob->onValueChange = [this, previous]
            {
                if (previous)
                    previous();
                if (target.isValid())
                    target.setProperty (IDs::stretchTempoMode, 0, nullptr);
            };
        }
        addAndMakeVisible (*timeKnob);

        smpStartKnob = makeKnob (IDs::smpStart,  0.0, 1.0, 0.0, "Sample start", true);
        lengthKnob   = makeKnob (IDs::smpLength, 0.0, 1.0, 1.0, "Sample length", true);
        inKnob       = makeKnob (IDs::fadeIn,    0.0, 1.0, 0.0, "Fade in", true);
        outKnob      = makeKnob (IDs::fadeOut,   0.0, 1.0, 0.0, "Fade out", true);
        trimKnob     = makeKnob (IDs::smpTrim,   0.0, 1.0, 0.0,
                                 "Trim: silence threshold for the sample's head and tail "
                                 "(0 = dead-zero only, max = -24 dBFS)", true);
        crossfadeKnob = makeKnob (IDs::loopCrossfade, 0.0, 1.0, 0.0,
                                  "Loop crossfade", true);

        auto repaintOnChange = [this] (ValueKnob* k)
        {
            auto prev = k->onValueChange;
            k->onValueChange = [this, prev] { if (prev) prev(); repaint(); };
        };
        repaintOnChange (smpStartKnob.get());
        repaintOnChange (lengthKnob.get());
        repaintOnChange (inKnob.get());
        repaintOnChange (outKnob.get());
        repaintOnChange (trimKnob.get());
        repaintOnChange (startOffsetKnob.get());

        updateStretchAvailability();
        updatePrecomputedAvailability();
        updatePlaybackAvailability();
    }

    std::unique_ptr<ValueKnob> makeKnob (juce::Identifier prop, double min, double max,
                                         double def, const juce::String& tip, bool live)
    {
        auto k = std::make_unique<ValueKnob>();
        configureKnob (*k, prop, min, max, def, tip, live);
        addAndMakeVisible (*k);
        return k;
    }

    void configureKnob (ValueKnob& knob, juce::Identifier prop, double min, double max,
                        double def, const juce::String& tip, bool live)
    {
        auto* k = &knob;
        k->setRange (min, max, 0.0);
        k->setDoubleClickReturnValue (true, def);
        k->setTooltip (tip);
        if (live && prop.isValid() && target.isValid())
        {
            k->setValue ((double) target.getProperty (prop, def), juce::dontSendNotification);
            auto* kp = k;
            k->onValueChange = [this, kp, prop] { if (target.isValid()) target.setProperty (prop, kp->getValue(), nullptr); };
            knobBinds.push_back ({ kp, prop, def });
        }
        else
        {
            k->setValue (def, juce::dontSendNotification);
            k->setEnabled (false);
            k->setAlpha (0.4f);
        }
    }

    void makeEffectLed (LedToggle& led, const juce::Identifier prop, const juce::String& caption)
    {
        led.caption = caption;
        led.getState = [this, prop] { return target.isValid() && (bool) target.getProperty (prop, false); };
        led.setState = [this, prop] (bool v) { if (target.isValid()) target.setProperty (prop, v, nullptr); };
        addAndMakeVisible (led);
    }

    void makePropertyLed (LedToggle& led, const juce::Identifier prop,
                          const juce::String& caption, std::function<void()> after = {})
    {
        led.caption = caption;
        led.getState = [this, prop]
        {
            return target.isValid() && (bool) target.getProperty (prop, false);
        };
        led.setState = [this, prop, after = std::move (after)] (bool v)
        {
            if (! target.isValid())
                return;
            target.setProperty (prop, v, nullptr);
            if (after)
                after();
        };
        addAndMakeVisible (led);
    }

    void syncControls()
    {
        declickBox.setSelectedId (
            juce::jlimit (0, 6, (int) target.getProperty (IDs::declickMode, 0)) + 1,
            juce::dontSendNotification);
        stretchBox.setSelectedId (
            stretchIdForMode (target.getProperty (IDs::stretchMode, "resample").toString()),
            juce::dontSendNotification);
        rangeBox.value = juce::jlimit (rangeBox.minV, rangeBox.maxV,
                                       (int) target.getProperty (IDs::pitchRange, 2));
        trackBox.maxV = target.isValid()
                            ? juce::jmax (0, Model::getMaxMixerTrackIndex (target.getRoot()))
                            : Model::maxMixerTracks;
        if (target.isValid())
            trackBox.value = juce::jlimit (trackBox.minV, trackBox.maxV, routedMixerTrack());
        for (auto& b : knobBinds)
            b.knob->setValue ((double) target.getProperty (b.prop, b.def), juce::dontSendNotification);

        auto f = sampleFile();
        revealBtn.setEnabled (f.existsAsFile());
        if (isChannel)
        {
            fineKnob->setValue (juce::jlimit (-100.0, 100.0, (double) target.getProperty (IDs::fineTune, 0.0)),
                                juce::dontSendNotification);
            detectBtn.setEnabled (f.existsAsFile());
            rootKb.repaint();
        }
        onLed.repaint();
        rangeBox.repaint();
        trackBox.repaint();
        keepDiskLed.repaint(); resampleLed.repaint(); regionsLed.repaint(); slicesLed.repaint();
        useLoopLed.repaint(); pingPongLed.repaint();
        fxDcLed.repaint(); fxNormLed.repaint(); fxRevLed.repaint();
        fxPolLed.repaint(); fxFadeStLed.repaint(); fxSwapStLed.repaint();
        updateContentAvailability();
        updateStretchAvailability();
        updatePrecomputedAvailability();
        updatePlaybackAvailability();
    }

    void applyPitchRange()
    {
        double range = juce::jmax (1.0, (double) target.getProperty (IDs::pitchRange, 2));
        if (pitchKnob != nullptr)
        {
            double v = juce::jlimit (-range, range, pitchKnob->getValue());
            pitchKnob->setRange (-range, range, 0.0);
            pitchKnob->setValue (v, juce::dontSendNotification);
        }
    }

    static juce::String stretchModeForId (int id)
    {
        static const char* modes[] = {
            "", "resample", "stretch", "stretchPro", "e3Generic", "e3Mono",
            "e2Generic", "e2Transient", "e2Mono", "e2Speech", "sliceStretch",
            "sliceMap", "auto" };
        return modes[juce::jlimit (1, 12, id)];
    }

    static int stretchIdForMode (const juce::String& mode)
    {
        for (int id = 1; id <= 12; ++id)
            if (mode == stretchModeForId (id))
                return id;
        return 1;
    }

    void updateStretchAvailability()
    {
        bool valid = target.isValid();
        auto mode = target.getProperty (IDs::stretchMode, "resample").toString();
        if (stretchPitchKnob) stretchPitchKnob->setEnabled (valid);
        if (mulKnob)          mulKnob->setEnabled (valid);
        if (timeKnob)         timeKnob->setEnabled (valid);
        if (formantKnob)
        {
            bool enabled = valid && mode == "stretchPro";
            formantKnob->setEnabled (enabled);
            formantKnob->setAlpha (enabled ? 1.0f : 0.4f);
        }
    }

    void updatePlaybackAvailability()
    {
        bool loops = target.isValid() && (bool) target.getProperty (IDs::useLoopPoints, false);
        pingPongLed.setEnabled (loops);
        updateCrossfadeAvailability();
    }

    void updateCrossfadeAvailability()
    {
        if (! crossfadeKnob)
            return;
        bool enabled = target.isValid()
                       && ! (bool) target.getProperty (IDs::sampleKeepOnDisk, false)
                       && (bool) target.getProperty (IDs::useLoopPoints, false);
        crossfadeKnob->setEnabled (enabled);
        crossfadeKnob->setAlpha (enabled ? 1.0f : 0.4f);
    }

    void updatePrecomputedAvailability()
    {
        bool enabled = target.isValid()
                       && ! (bool) target.getProperty (IDs::sampleKeepOnDisk, false);
        for (auto* led : { &fxDcLed, &fxNormLed, &fxRevLed, &fxPolLed, &fxFadeStLed, &fxSwapStLed })
            led->setEnabled (enabled);
        for (auto* knob : { smpStartKnob.get(), lengthKnob.get(), inKnob.get(),
                            outKnob.get(), trimKnob.get() })
            if (knob != nullptr)
            {
                knob->setEnabled (enabled);
                knob->setAlpha (enabled ? 1.0f : 0.4f);
            }
        updateCrossfadeAvailability();
    }

    void updateContentAvailability()
    {
        auto f = sampleFile();
        bool readable = f.existsAsFile();
        keepDiskLed.setEnabled (false);
        keepDiskLed.setTooltip ("Disk streaming is not supported yet");
        resampleLed.setEnabled (readable);
        regionsLed.setEnabled (readable);
        slicesLed.setEnabled (readable);
    }

    void showTimePresetMenu()
    {
        if (! target.isValid())
            return;
        juce::PopupMenu menu;
        menu.addItem (1, "None");
        menu.addItem (2, "Autodetect");
        menu.addItem (3, "Project tempo");
        menu.addSeparator();
        menu.addItem (11, "1 beat");
        menu.addItem (12, "2 beats");
        menu.addItem (14, "4 beats");
        menu.addItem (18, "8 beats");
        menu.addItem (26, "16 beats");

        auto capturedTarget = target;
        auto generation = targetGeneration;
        menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition(),
            [safe = juce::Component::SafePointer<SamplerSettingsPanel> (this),
             capturedTarget, generation] (int result) mutable
            {
                if (safe == nullptr || result == 0 || safe->target != capturedTarget
                    || safe->targetGeneration != generation)
                    return;
                if (result == 1)
                {
                    capturedTarget.setProperty (IDs::stretchTempoMode, 0, nullptr);
                    capturedTarget.setProperty (IDs::stretchTime, 1.0, nullptr);
                    return;
                }
                if (result == 2)
                {
                    safe->detectTempo (capturedTarget);
                    return;
                }
                if (result == 3)
                {
                    capturedTarget.setProperty (IDs::stretchTempoMode, 2, nullptr);
                    capturedTarget.setProperty (IDs::stretchSourceBpm, safe->engine.getBpm(), nullptr);
                    return;
                }
                int beats = result - 10;
                capturedTarget.setProperty (IDs::stretchTempoMode, 3, nullptr);
                capturedTarget.setProperty (IDs::stretchBeats, beats, nullptr);
            });
    }

    void detectTempo (juce::ValueTree node)
    {
        auto path = node.getProperty (node.hasType (IDs::Channel) ? IDs::samplePath : IDs::path).toString();
        auto f = engine.resolveSampleFile (path);
        if (! f.existsAsFile())
            return;
        auto reader = std::unique_ptr<juce::AudioFormatReader> (engine.audioFormats.createReaderFor (f));
        if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
            return;

        double seconds = (double) reader->lengthInSamples / reader->sampleRate;
        double projectBpm = engine.getBpm();
        int bestBeats = 4;
        double bestBpm = projectBpm;
        double bestDistance = std::numeric_limits<double>::max();
        for (int beats : { 1, 2, 4, 8, 16 })
        {
            double candidate = beats * 60.0 / seconds;
            if (candidate < 20.0 || candidate > 400.0)
                continue;
            double distance = std::abs (std::log (candidate / juce::jmax (1.0, projectBpm)));
            if (distance < bestDistance)
            {
                bestDistance = distance;
                bestBpm = candidate;
                bestBeats = beats;
            }
        }
        node.setProperty (IDs::stretchSourceBpm, bestBpm, nullptr);
        node.setProperty (IDs::stretchBeats, bestBeats, nullptr);
        node.setProperty (IDs::stretchTempoMode, 1, nullptr);
    }

    void bindRoutingNodes()
    {
        routeChannel.removeListener (this);
        routeTrack.removeListener (this);
        routeChannel = {};
        routeTrack = {};

        if (! isChannel && target.isValid())
        {
            auto path = target.getProperty (IDs::path).toString();
            auto channels = Model::getChannels (target.getRoot());
            for (int i = 0; i < channels.getNumChildren(); ++i)
            {
                auto channel = channels.getChild (i);
                if (channel.hasType (IDs::Channel)
                    && channel.getProperty (IDs::type).toString() == "sampler"
                    && channel.getProperty (IDs::samplePath).toString() == path)
                {
                    routeChannel = channel;
                    break;
                }
            }
        }
        if (routeChannel.isValid())
            routeChannel.addListener (this);
        bindRouteTrack();
    }

    void bindRouteTrack()
    {
        routeTrack.removeListener (this);
        routeTrack = {};
        if (target.isValid())
        {
            int index = juce::jlimit (0, Model::maxMixerTracks, routedMixerTrack());
            routeTrack = Model::getMixerTrack (target.getRoot(), index);
            if (routeTrack.isValid())
                routeTrack.addListener (this);
        }
    }

    void showTrackMenu()
    {
        if (! target.isValid())
            return;

        juce::PopupMenu menu;
        auto mixer = Model::getMixer (target.getRoot());
        int selected = routedMixerTrack();
        for (int i = 0; i < mixer.getNumChildren(); ++i)
        {
            auto track = mixer.getChild (i);
            if (! track.hasType (IDs::Track))
                continue;
            int index = (int) track.getProperty (IDs::index, 0);
            auto name = track.getProperty (IDs::name,
                                          index == 0 ? juce::String ("Master")
                                                     : "Insert " + juce::String (index)).toString();
            menu.addItem (index + 1, index == 0 ? "Master" : juce::String (index) + "  " + name,
                          true, index == selected);
        }

        auto capturedTarget = target;
        auto generation = targetGeneration;
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&trackBox),
            [safe = juce::Component::SafePointer<SamplerSettingsPanel> (this),
             capturedTarget, generation] (int result) mutable
            {
                if (safe == nullptr || result <= 0 || safe->target != capturedTarget
                    || safe->targetGeneration != generation)
                    return;
                safe->setRoutedMixerTrack (result - 1);
            });
    }

    void emitTitle()
    {
        if (! onTitleChanged)
            return;
        int trackIndex = routedMixerTrack();
        auto track = Model::getMixerTrack (target.getRoot(), trackIndex);
        auto trackName = track.isValid()
                             ? track.getProperty (IDs::name).toString()
                             : (trackIndex == 0 ? juce::String ("Master")
                                                : "Insert " + juce::String (trackIndex));
        auto code = trackIndex == 0 ? juce::String ("M")
                                    : juce::String (trackIndex).paddedLeft ('0', 2);
        onTitleChanged (" - [" + code + "] " + fileDisplay() + " (" + trackName + ")");
    }

    int routedMixerTrack() const
    {
        if (! target.isValid())
            return 0;
        if (isChannel)
            return (int) target.getProperty (IDs::mixerTrack, 1);
        if (target.hasProperty (IDs::mixerTrack))
            return (int) target.getProperty (IDs::mixerTrack, 0);
        if (routeChannel.isValid())
            return (int) routeChannel.getProperty (IDs::mixerTrack, 1);
        return 0;
    }
    void setRoutedMixerTrack (int v)
    {
        if (target.isValid())
            target.setProperty (IDs::mixerTrack, juce::jlimit (0, Model::maxMixerTracks, v), nullptr);
    }

    juce::Identifier pathProp() const { return isChannel ? IDs::samplePath : IDs::path; }

    juce::String rawPath() const { return target.getProperty (pathProp()).toString(); }

    juce::File sampleFile() const
    {
        auto p = rawPath();
        return (p.isEmpty() || p.startsWith ("builtin:")) ? juce::File()
                                                          : engine.resolveSampleFile (p);
    }

    juce::String fileDisplay() const
    {
        auto p = rawPath();
        if (p.isEmpty())
            return "(no sample)";
        if (p.startsWith ("builtin:"))
            return p;
        return juce::File (p).getFileName();
    }

    juce::String displayName() const
    {
        if (isChannel)
            return target.getProperty (IDs::name).toString();
        auto p = rawPath();
        auto files = Model::getAudioFiles (target.getRoot());
        auto node = files.getChildWithProperty (IDs::audioPath, p);
        auto n = node.isValid() ? node.getProperty (IDs::name).toString() : juce::String();
        return n.isNotEmpty() ? n : juce::File (p).getFileNameWithoutExtension();
    }

    void chooseFile()
    {
        auto start = sampleFile().existsAsFile() ? sampleFile().getParentDirectory() : juce::File();
        auto capturedTarget = target;
        auto generation = targetGeneration;
        bool capturedIsChannel = isChannel;
        int previousRoute = routedMixerTrack();
        chooser = std::make_unique<juce::FileChooser> ("Load sample", start,
                                                       "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
        chooser->launchAsync (juce::FileBrowserComponent::openMode
                                  | juce::FileBrowserComponent::canSelectFiles,
            [safe = juce::Component::SafePointer<SamplerSettingsPanel> (this),
             capturedTarget, generation, capturedIsChannel, previousRoute] (const juce::FileChooser& fc)
            {
                auto f = fc.getResult();
                if (safe == nullptr || f == juce::File() || safe->target != capturedTarget
                    || safe->targetGeneration != generation)
                    return;
                safe->replaceSample (capturedTarget, capturedIsChannel, f, previousRoute);
            });
    }

    void replaceSample (juce::ValueTree node, bool nodeIsChannel,
                        const juce::File& file, int previousRoute)
    {
        if (! node.isValid() || ! file.existsAsFile()
            || ! node.getRoot().hasType (IDs::Project))
            return;

        auto project = node.getRoot();
        Model::addAudioResource (project, file);
        if (nodeIsChannel)
        {
            node.setProperty (IDs::samplePath, file.getFullPathName(), nullptr);
            return;
        }

        auto channels = Model::getChannels (project);
        auto newPath = file.getFullPathName();
        juce::ValueTree companion;
        for (int i = 0; i < channels.getNumChildren(); ++i)
        {
            auto channel = channels.getChild (i);
            if (channel.hasType (IDs::Channel)
                && channel.getProperty (IDs::type).toString() == "sampler"
                && channel.getProperty (IDs::samplePath).toString() == newPath)
            {
                companion = channel;
                break;
            }
        }
        if (! companion.isValid())
        {
            companion = Model::addSamplerChannel (project, file.getFileNameWithoutExtension(), newPath);
            companion.setProperty (IDs::mixerTrack, previousRoute, nullptr);
        }
        node.setProperty (IDs::path, newPath, nullptr);
        bindRoutingNodes();
        syncControls();
        emitTitle();
    }

    static juce::String keyName (int key)
    {
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F",
                                       "F#", "G", "G#", "A", "A#", "B" };
        return juce::String (names[((key % 12) + 12) % 12]) + juce::String (key / 12);
    }

    void runDetection()
    {
        auto f = sampleFile();
        if (! isChannel || ! f.existsAsFile())
            return;

        detectStatus = "detecting...";
        repaint();

        auto capturedTarget = target;
        int generation = ++detectionGeneration;
        analysisPool.addJob (
            [safe = juce::Component::SafePointer<SamplerSettingsPanel> (this), f, &eng = engine,
             capturedTarget, generation]
            {
                PitchDetect::Result res;
                juce::int64 fileSamples = 0;
                if (auto reader = std::unique_ptr<juce::AudioFormatReader> (eng.audioFormats.createReaderFor (f)))
                {
                    auto len = std::min<juce::int64> (reader->lengthInSamples,
                                                      (juce::int64) (reader->sampleRate * 12));
                    if (len > 0)
                    {
                        fileSamples = reader->lengthInSamples;
                        juce::AudioBuffer<float> buf ((int) juce::jmax (1u, reader->numChannels), (int) len);
                        reader->read (&buf, 0, (int) len, 0, true, true);

                        std::vector<float> mono ((size_t) len, 0.0f);
                        int nc = buf.getNumChannels();
                        for (int c = 0; c < nc; ++c)
                        {
                            const float* src = buf.getReadPointer (c);
                            for (juce::int64 i = 0; i < len; ++i)
                                mono[(size_t) i] += src[i] / (float) nc;
                        }
                        res = PitchDetect::analyze (mono.data(), len, reader->sampleRate);
                    }
                }

                juce::MessageManager::callAsync ([safe, res, fileSamples, capturedTarget, generation]
                {
                    if (safe == nullptr || safe->target != capturedTarget
                        || safe->detectionGeneration != generation)
                        return;
                    safe->onDetectionDone (capturedTarget, res, fileSamples);
                });
            });
    }

    void onDetectionDone (juce::ValueTree detectedTarget, const PitchDetect::Result& res,
                          juce::int64 fileSamples)
    {
        if (! isChannel || ! target.isValid() || target != detectedTarget)
            return;

        detectedRegions = res.regions;
        detectFileSamples = fileSamples;

        if (! res.valid)
        {
            detectStatus = "no pitch detected";
            repaint();
            return;
        }

        int cents = (int) std::lround (res.cents);
        detectStatus = keyName (res.midiNote)
                         + "  " + (cents >= 0 ? "+" : "") + juce::String (cents) + " ct";

        detectedTarget.setProperty (IDs::rootNote, res.midiNote, nullptr);
        double fine = juce::jlimit (-100.0, 100.0, std::round (-res.cents * 10.0) / 10.0);
        detectedTarget.setProperty (IDs::fineTune, fine, nullptr);
        repaint();
    }

    struct KnobSlot { juce::Component* knob; const char* caption; };

    static juce::Rectangle<int> groupInner (juce::Rectangle<int> group)
    {
        return group.withTrimmedTop (18).reduced (10, 4);
    }

    void placeKnob (juce::Rectangle<int> column, juce::Component* knob,
                    const juce::String& caption, int knobSize = 28)
    {
        if (knob == nullptr)
            return;
        constexpr int labelH = 12, gap = 2;
        int top = column.getY()
                  + juce::jmax (0, (column.getHeight() - (knobSize + gap + labelH)) / 2);
        knob->setBounds (column.getCentreX() - knobSize / 2, top, knobSize, knobSize);
        knobLabels.push_back ({ juce::Rectangle<int> (column.getX(), top + knobSize + gap,
                                                      column.getWidth(), labelH), caption });
    }

    void layoutKnobRow (juce::Rectangle<int> row, const KnobSlot* slots, int count)
    {
        int slotW = juce::jmax (1, row.getWidth() / count);
        for (int i = 0; i < count; ++i)
            placeKnob (row.removeFromLeft (i == count - 1 ? row.getWidth() : slotW),
                       slots[i].knob, slots[i].caption);
    }

    void layoutHeader (juce::Rectangle<int> area)
    {
        headerR = area;
        auto cluster = area.withTrimmedRight (8).removeFromRight (clusterWidth);
        constexpr int ctrlH = 26, labelH = 11, gap = 2;
        int bandTop = area.getY()
                      + juce::jmax (0, (area.getHeight() - 1 - (ctrlH + gap + labelH)) / 2);

        auto place = [&] (juce::Component* c, int w, int h, const juce::String& caption)
        {
            auto col = cluster.removeFromLeft (headerSlotW);
            if (c != nullptr)
                c->setBounds (col.getCentreX() - w / 2, bandTop + (ctrlH - h) / 2, w, h);
            headerLabels.push_back ({ juce::Rectangle<int> (col.getX(), bandTop + ctrlH + gap,
                                                            col.getWidth(), labelH), caption });
        };

        place (&onLed,          18, 18, "ON");
        place (panKnob.get(),   26, 26, "PAN");
        place (volKnob.get(),   26, 26, "VOL");
        place (pitchKnob.get(), 26, 26, "PITCH");
        place (&rangeBox,       30, 20, "RANGE");
        place (&trackBox,       30, 20, "TRACK");
    }

    void layoutFileRow (juce::Rectangle<int> area)
    {
        auto row = area.reduced (10, 3);
        fileLabelR = row.removeFromLeft (32);
        clearBtn.setBounds  (row.removeFromRight (22).reduced (1));
        revealBtn.setBounds (row.removeFromRight (24).reduced (1));
        openBtn.setBounds   (row.removeFromRight (24).reduced (1));
        fileNameR = row.withTrimmedRight (6);
    }

    void layoutBody (juce::Rectangle<int> area)
    {
        int usable = juce::jmax (0, area.getHeight() - groupGapV);
        int topH = juce::roundToInt (usable * (double) topGroupH
                                     / (double) (topGroupH + bottomGroupH));

        auto left = area.removeFromLeft (juce::roundToInt (area.getWidth() * 0.42f));
        area.removeFromLeft (10);
        auto right = area;

        contentR = left.removeFromTop (topH);
        left.removeFromTop (groupGapV);
        playbackR = left;

        stretchR = right.removeFromTop (topH);
        right.removeFromTop (groupGapV);
        effectsR = right;

        layoutContentGroup();
        layoutPlaybackGroup();
        layoutStretchGroup();
        layoutEffectsGroup();
    }

    void layoutContentGroup()
    {
        auto inner = groupInner (contentR);
        auto declick = inner.removeFromBottom (juce::jmin (24, inner.getHeight()));
        declickLabelR = declick.removeFromLeft (juce::jmin (74, declick.getWidth()));
        declickBox.setBounds (declick);
        inner.removeFromBottom (4);

        LedToggle* leds[4] = { &keepDiskLed, &regionsLed, &resampleLed, &slicesLed };
        int colW = juce::jmax (1, inner.getWidth() / 2);
        int rowH = juce::jmax (1, inner.getHeight() / 2);
        for (int i = 0; i < 4; ++i)
            leds[i]->setBounds (inner.getX() + (i % 2) * colW,
                                inner.getY() + (i / 2) * rowH,
                                juce::jmax (1, colW - 6), rowH);
    }

    void layoutPlaybackGroup()
    {
        auto inner = groupInner (playbackR);
        placeKnob (inner.removeFromLeft (juce::jmin (88, inner.getWidth() / 2)),
                   startOffsetKnob.get(), "START OFFSET");
        inner.removeFromLeft (8);

        constexpr int ledH = 22;
        int top = inner.getY() + juce::jmax (0, (inner.getHeight() - ledH * 2) / 2);
        useLoopLed.setBounds  (inner.getX(), top,        inner.getWidth(), ledH);
        pingPongLed.setBounds (inner.getX(), top + ledH, inner.getWidth(), ledH);
    }

    void layoutStretchGroup()
    {
        auto inner = groupInner (stretchR);
        auto modeRow = inner.removeFromTop (juce::jmin (24, inner.getHeight()));
        stretchModeLabelR = modeRow.removeFromLeft (juce::jmin (40, modeRow.getWidth()));
        stretchBox.setBounds (modeRow);
        inner.removeFromTop (4);

        const KnobSlot slots[] = { { stretchPitchKnob.get(), "PITCH" },
                                   { mulKnob.get(),          "MUL" },
                                   { timeKnob.get(),         "TIME" },
                                   { formantKnob.get(),      "FORMANT" } };
        layoutKnobRow (inner, slots, 4);
    }

    void layoutEffectsGroup()
    {
        auto inner = groupInner (effectsR);
        auto knobRow = inner.removeFromBottom (juce::jmin (42, inner.getHeight()));
        inner.removeFromBottom (4);

        LedToggle* leds[6] = { &fxDcLed, &fxNormLed, &fxRevLed,
                               &fxPolLed, &fxFadeStLed, &fxSwapStLed };
        int colW = juce::jmax (1, inner.getWidth() / 3);
        int rowH = juce::jmax (1, inner.getHeight() / 2);
        for (int i = 0; i < 6; ++i)
            leds[i]->setBounds (inner.getX() + (i % 3) * colW,
                                inner.getY() + (i / 3) * rowH,
                                juce::jmax (1, colW - 6), rowH);

        const KnobSlot slots[] = { { smpStartKnob.get(),  "SMP START" },
                                   { lengthKnob.get(),    "LENGTH" },
                                   { inKnob.get(),        "IN" },
                                   { outKnob.get(),       "OUT" },
                                   { crossfadeKnob.get(), "CROSSFADE" },
                                   { trimKnob.get(),      "TRIM" } };
        layoutKnobRow (knobRow, slots, 6);
    }

    struct CapLabel { juce::Rectangle<int> area; juce::String text; };

    void paintCapsLabels (juce::Graphics& g, const std::vector<CapLabel>& labels)
    {
        g.setColour (Colors::textDim);
        g.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::bold)));
        for (auto& l : labels)
            g.drawText (l.text, l.area, juce::Justification::centred, false);
    }

    void drawGroup (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title)
    {
        auto b = area.toFloat().reduced (0.5f);
        g.setColour (Colors::panelDark.withAlpha (0.5f));
        g.fillRoundedRectangle (b, 4.0f);
        g.setColour (Colors::outline);
        g.drawRoundedRectangle (b, 4.0f, 1.0f);
        g.setColour (Colors::textDim);
        g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
        g.drawText (title, area.reduced (10, 0).withHeight (18), juce::Justification::centredLeft);
    }

    struct WaveformData
    {
        std::vector<float> peaks;
        std::vector<double> cues;
        double loopStart = -1.0;
        double loopEnd = -1.0;
        double sampleRate = 0.0;
        double duration = 0.0;
        juce::int64 samples = 0;
        int bits = 0;
        int channels = 0;
    };

    void drawWaveform (juce::Graphics& g)
    {
        g.setColour (Colors::panelDark.darker (0.4f));
        g.fillRoundedRectangle (waveR.toFloat(), 3.0f);

        if (target.isValid())
        {
            g.setColour (Colors::textDim.withAlpha (0.20f));
            g.setFont (juce::Font (juce::FontOptions (22.0f, juce::Font::bold)));
            g.drawText (displayName(), waveR.reduced (10, 6),
                        juce::Justification::topRight, true);
        }

        if (peaks.empty())
        {
            g.setColour (Colors::textDim);
            g.setFont (juce::Font (juce::FontOptions (11.0f)));
            g.drawText (waveformLoading ? "Analysing waveform..."
                                       : target.isValid() ? "(click to preview)" : "(no sample)",
                        waveR, juce::Justification::centred);
            return;
        }

        auto inner = waveR.toFloat().reduced (4.0f, 4.0f);
        float sampleX0 = xForNormalized (property01 (IDs::smpStart, 0.0));
        float sampleX1 = xForNormalized (sampleWindowEnd());
        auto selected = inner.withX (juce::jmax (inner.getX(), sampleX0))
                             .withRight (juce::jmin (inner.getRight(), sampleX1));
        if (! selected.isEmpty())
        {
            g.setColour (Colors::accent.withAlpha (0.07f));
            g.fillRect (selected);
        }

        g.setColour (Colors::outline.withAlpha (0.55f));
        g.drawHorizontalLine ((int) inner.getCentreY(), inner.getX(), inner.getRight());

        float cy = inner.getCentreY();
        float maxH = inner.getHeight() * 0.46f;
        int width = juce::jmax (1, (int) inner.getWidth());
        int count = (int) peaks.size();
        float gain = 1.0f;
        if ((bool) target.getProperty (IDs::fxNormalize, false))
        {
            float maximum = 0.0f;
            for (float peak : peaks)
                maximum = juce::jmax (maximum, peak);
            if (maximum > 1.0e-5f)
                gain = 1.0f / maximum;
        }

        g.setColour (Colors::accent.withAlpha (0.88f));
        for (int x = 0; x < width; ++x)
        {
            double n0 = waveViewStart + (double) x / width * (waveViewEnd - waveViewStart);
            double n1 = waveViewStart + (double) (x + 1) / width * (waveViewEnd - waveViewStart);
            int i0 = juce::jlimit (0, count - 1, (int) std::floor (n0 * count));
            int i1 = juce::jlimit (i0 + 1, count, (int) std::ceil (n1 * count));
            float peak = 0.0f;
            for (int i = i0; i < i1; ++i)
                peak = juce::jmax (peak, peaks[(size_t) i]);
            float h = juce::jlimit (0.5f, maxH, peak * gain * maxH);
            g.drawVerticalLine ((int) inner.getX() + x, cy - h, cy + h);
        }

        if (sampleX0 > inner.getX())
        {
            g.setColour (Colors::panelDark.withAlpha (0.68f));
            g.fillRect (inner.withRight (juce::jmin (inner.getRight(), sampleX0)));
        }
        if (sampleX1 < inner.getRight())
        {
            g.setColour (Colors::panelDark.withAlpha (0.68f));
            g.fillRect (inner.withLeft (juce::jmax (inner.getX(), sampleX1)));
        }

        drawPitchRegions (g, inner);
        drawCueMarkers (g, inner);
        drawWaveMarkers (g, inner);

        juce::String info;
        if (waveBits > 0)
            info << waveBits << "-bit  ";
        if (waveChannels > 0)
            info << (waveChannels == 1 ? "Mono" : waveChannels == 2 ? "Stereo"
                                                                    : juce::String (waveChannels) + " ch")
                 << "  ";
        if (waveSampleRate > 0.0)
            info << juce::String (waveSampleRate / 1000.0, 1) << " kHz  ";
        if (waveDuration > 0.0)
            info << formatDuration (waveDuration);

        g.setColour (Colors::textDim.withAlpha (0.82f));
        g.setFont (juce::Font (juce::FontOptions (9.5f)));
        g.drawText (waveStatus, waveR.reduced (7, 3), juce::Justification::bottomLeft, true);
        g.drawText (info, waveR.reduced (7, 3), juce::Justification::bottomRight, true);
    }

    void drawPitchRegions (juce::Graphics& g, juce::Rectangle<float> inner)
    {
        if (detectedRegions.empty() || detectFileSamples <= 0)
            return;

        g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
        for (size_t i = 0; i < detectedRegions.size(); ++i)
        {
            const auto& region = detectedRegions[i];
            double n0 = (double) region.startSample / (double) detectFileSamples;
            double n1 = (double) region.endSample / (double) detectFileSamples;
            if (n1 < waveViewStart || n0 > waveViewEnd)
                continue;
            float x0 = juce::jmax (inner.getX(), xForNormalized (n0));
            float x1 = juce::jmin (inner.getRight(), xForNormalized (n1));
            auto band = juce::Rectangle<float> (x0, inner.getY(),
                                                 juce::jmax (1.0f, x1 - x0), inner.getHeight());
            g.setColour (Colors::accent2.withAlpha (i % 2 == 0 ? 0.13f : 0.07f));
            g.fillRect (band);
            g.setColour (Colors::accent2.withAlpha (0.48f));
            g.drawVerticalLine ((int) x0, band.getY(), band.getBottom());
            if (band.getWidth() > 22.0f)
            {
                g.setColour (Colors::text.withAlpha (0.82f));
                g.drawText (keyName (region.midiNote), band.toNearestInt(),
                            juce::Justification::centredTop, false);
            }
        }
    }

    void drawCueMarkers (juce::Graphics& g, juce::Rectangle<float> inner)
    {
        if (! (bool) target.getProperty (IDs::loadSliceMarkers, false))
            return;
        g.setColour (Colors::warning.withAlpha (0.9f));
        for (double cue : embeddedCues)
        {
            if (cue < waveViewStart || cue > waveViewEnd)
                continue;
            float x = xForNormalized (cue);
            juce::Path marker;
            marker.addTriangle (x - 4.0f, inner.getY(), x + 4.0f, inner.getY(),
                                x, inner.getY() + 6.0f);
            g.fillPath (marker);
            g.drawVerticalLine ((int) x, inner.getY() + 6.0f, inner.getBottom());
        }
    }

    void drawWaveMarker (juce::Graphics& g, juce::Rectangle<float> inner, double position,
                         juce::Colour colour, bool atTop)
    {
        if (position < waveViewStart || position > waveViewEnd)
            return;
        float x = xForNormalized (position);
        g.setColour (colour.withAlpha (0.92f));
        g.drawVerticalLine ((int) x, inner.getY(), inner.getBottom());
        juce::Path handle;
        if (atTop)
            handle.addTriangle (x - 5.0f, inner.getY(), x + 5.0f, inner.getY(),
                                x, inner.getY() + 7.0f);
        else
            handle.addTriangle (x - 5.0f, inner.getBottom(), x + 5.0f, inner.getBottom(),
                                x, inner.getBottom() - 7.0f);
        g.fillPath (handle);
    }

    void drawWaveMarkers (juce::Graphics& g, juce::Rectangle<float> inner)
    {
        drawWaveMarker (g, inner, property01 (IDs::smpStart, 0.0), Colors::danger, true);
        drawWaveMarker (g, inner, sampleWindowEnd(), Colors::danger, true);
        drawWaveMarker (g, inner, logicalToRaw (property01 (IDs::startOffset, 0.0)),
                        Colors::success, false);
        if ((bool) target.getProperty (IDs::useLoopPoints, false))
        {
            drawWaveMarker (g, inner, logicalToRaw (property01 (IDs::loopStart, 0.0)),
                            Colors::warning, true);
            drawWaveMarker (g, inner, logicalToRaw (property01 (IDs::loopEnd, 1.0)),
                            Colors::accent2, false);
        }
    }

    void loadWaveform()
    {
        auto capturedTarget = target;
        auto file = sampleFile();
        int generation = ++waveformGeneration;
        peaks.clear();
        embeddedCues.clear();
        embeddedLoopStart = embeddedLoopEnd = -1.0;
        waveBits = waveChannels = 0;
        waveSampleRate = waveDuration = 0.0;
        waveSampleCount = 0;
        waveformLoading = file.existsAsFile();
        waveStatus = waveformLoading ? "Loading..." : juce::String();
        setWaveView (0.0, 1.0);
        repaint (waveR);
        if (! waveformLoading)
            return;

        waveformPool.addJob (
            [safe = juce::Component::SafePointer<SamplerSettingsPanel> (this),
             capturedTarget, file, generation, &eng = engine]
            {
                auto result = std::make_shared<WaveformData>();
                auto reader = std::unique_ptr<juce::AudioFormatReader> (
                    eng.audioFormats.createReaderFor (file));
                if (reader != nullptr && reader->lengthInSamples > 0 && reader->sampleRate > 0.0)
                {
                    result->samples = reader->lengthInSamples;
                    result->sampleRate = reader->sampleRate;
                    result->duration = (double) result->samples / result->sampleRate;
                    result->bits = (int) reader->bitsPerSample;
                    result->channels = (int) reader->numChannels;

                    int peakCount = juce::jlimit (
                        1024, 32768, (int) std::min<juce::int64> (
                            32768, std::max<juce::int64> (1024, result->samples / 128)));
                    result->peaks.assign ((size_t) peakCount, 0.0f);
                    juce::AudioBuffer<float> buffer (
                        (int) juce::jmax (1u, reader->numChannels), 1 << 14);
                    for (juce::int64 position = 0; position < result->samples;)
                    {
                        int chunk = (int) std::min<juce::int64> (
                            buffer.getNumSamples(), result->samples - position);
                        reader->read (&buffer, 0, chunk, position, true, true);
                        for (int i = 0; i < chunk; ++i)
                        {
                            int bin = (int) ((position + i) * peakCount / result->samples);
                            auto& peak = result->peaks[(size_t) juce::jlimit (0, peakCount - 1, bin)];
                            for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
                                peak = juce::jmax (peak, std::abs (buffer.getSample (channel, i)));
                        }
                        position += chunk;
                    }

                    auto keys = reader->metadataValues.getAllKeys();
                    for (auto& key : keys)
                    {
                        auto lower = key.toLowerCase();
                        auto value = reader->metadataValues[key].getLargeIntValue();
                        if (value < 0 || value > result->samples)
                            continue;
                        double normalized = (double) value / (double) result->samples;
                        if (lower.contains ("loop") && lower.contains ("start")
                            && result->loopStart < 0.0)
                            result->loopStart = normalized;
                        else if (lower.contains ("loop") && lower.contains ("end")
                                 && result->loopEnd < 0.0)
                            result->loopEnd = normalized;
                        else if (lower.contains ("cue")
                                 && (lower.contains ("offset") || lower.contains ("position")))
                            result->cues.push_back (normalized);
                    }
                    std::sort (result->cues.begin(), result->cues.end());
                    result->cues.erase (
                        std::unique (result->cues.begin(), result->cues.end()),
                        result->cues.end());
                }

                juce::MessageManager::callAsync ([safe, capturedTarget, generation, result]
                {
                    if (safe == nullptr || safe->target != capturedTarget
                        || safe->waveformGeneration != generation)
                        return;
                    safe->waveformLoading = false;
                    safe->peaks = std::move (result->peaks);
                    safe->embeddedCues = std::move (result->cues);
                    safe->embeddedLoopStart = result->loopStart;
                    safe->embeddedLoopEnd = result->loopEnd;
                    safe->waveBits = result->bits;
                    safe->waveChannels = result->channels;
                    safe->waveSampleRate = result->sampleRate;
                    safe->waveDuration = result->duration;
                    safe->waveSampleCount = result->samples;
                    safe->waveStatus = safe->idleWaveStatus();
                    safe->applyEmbeddedLoopIfRequested();
                    safe->repaint (safe->waveR);
                });
            });
    }

    void valueTreeParentChanged (juce::ValueTree& t) override
    {
        if (t == target && target.isValid() && ! target.getRoot().hasType (IDs::Project))
        {
            if (onTargetGone)
                onTargetGone();
            return;
        }
        if (t == routeChannel || t == routeTrack)
        {
            bindRoutingNodes();
            syncControls();
            emitTitle();
        }
    }

    void valueTreePropertyChanged (juce::ValueTree& t, const juce::Identifier& p) override
    {
        if (t == routeTrack)
        {
            if (p == IDs::name)
                emitTitle();
            return;
        }
        if (t == routeChannel && ! isChannel)
        {
            if (p == IDs::mixerTrack)
            {
                bindRouteTrack();
                trackBox.value = juce::jlimit (trackBox.minV, trackBox.maxV, routedMixerTrack());
                trackBox.repaint();
                emitTitle();
            }
            else if (p == IDs::samplePath)
            {
                bindRoutingNodes();
                syncControls();
                emitTitle();
            }
            return;
        }
        if (t != target)
            return;

        if (p == pathProp())
        {
            ++detectionGeneration;
            if (! isChannel)
                bindRoutingNodes();
            loadWaveform();
            revealBtn.setEnabled (sampleFile().existsAsFile());
            updateContentAvailability();
            emitTitle();
            if (isChannel)
            {
                detectedRegions.clear();
                detectStatus = {};
                detectBtn.setEnabled (sampleFile().existsAsFile());
            }
            repaint();
            return;
        }
        if (p == IDs::rootNote)
        {
            rootKb.repaint();
            repaint (rootCaptionR);
            return;
        }
        if (p == IDs::fineTune)
        {
            fineKnob->setValue (juce::jlimit (-100.0, 100.0, (double) target.getProperty (IDs::fineTune, 0.0)),
                                juce::dontSendNotification);
            return;
        }
        if (p == IDs::declickMode)
            declickBox.setSelectedId (juce::jlimit (0, 6, (int) target.getProperty (IDs::declickMode, 0)) + 1,
                                      juce::dontSendNotification);
        else if (p == IDs::pitchRange)
        {
            rangeBox.value = juce::jlimit (rangeBox.minV, rangeBox.maxV,
                                           (int) target.getProperty (IDs::pitchRange, 2));
            rangeBox.repaint();
            applyPitchRange();
        }
        else if (p == IDs::mixerTrack)
        {
            bindRouteTrack();
            trackBox.value = juce::jlimit (trackBox.minV, trackBox.maxV, routedMixerTrack());
            trackBox.repaint();
            emitTitle();
        }
        else if (p == IDs::mute)
            onLed.repaint();
        else if (p == IDs::name)
        {
            repaint();
            emitTitle();
        }
        else
        {
            for (auto& b : knobBinds)
                if (p == b.prop)
                    b.knob->setValue ((double) target.getProperty (b.prop, b.def), juce::dontSendNotification);

            if (p == IDs::stretchMode)
            {
                stretchBox.setSelectedId (
                    stretchIdForMode (target.getProperty (IDs::stretchMode, "resample").toString()),
                    juce::dontSendNotification);
                updateStretchAvailability();
            }
            if (p == IDs::sampleKeepOnDisk)
            {
                keepDiskLed.repaint();
                updatePrecomputedAvailability();
            }
            if (p == IDs::sampleResample)       resampleLed.repaint();
            if (p == IDs::loadRegions)
            {
                regionsLed.repaint();
                applyEmbeddedLoopIfRequested();
                repaint (waveR);
            }
            if (p == IDs::loadSliceMarkers)
            {
                slicesLed.repaint();
                repaint (waveR);
            }
            if (p == IDs::useLoopPoints)
            {
                useLoopLed.repaint();
                updatePlaybackAvailability();
                repaint (waveR);
            }
            if (p == IDs::pingPongLoop)
            {
                pingPongLed.repaint();
                repaint (waveR);
            }

            if (p == IDs::smpStart || p == IDs::smpLength || p == IDs::smpTrim
                || p == IDs::fadeIn || p == IDs::fadeOut
                || p == IDs::fxReverse || p == IDs::fxNormalize
                || p == IDs::startOffset || p == IDs::loopStart || p == IDs::loopEnd
                || p == IDs::loopCrossfade)
                repaint();
            else if (p == IDs::fxRemoveDC || p == IDs::fxRevPolarity
                     || p == IDs::fxFadeStereo || p == IDs::fxSwapStereo)
            {
                fxDcLed.repaint(); fxPolLed.repaint(); fxFadeStLed.repaint(); fxSwapStLed.repaint();
            }
            if (p == IDs::fxReverse)      fxRevLed.repaint();
            if (p == IDs::fxNormalize)    fxNormLed.repaint();
        }
    }

    AudioEngine& engine;
    juce::ValueTree target;
    juce::ValueTree routeChannel;
    juce::ValueTree routeTrack;
    bool isChannel = false;
    int targetGeneration = 0;
    int detectionGeneration = 0;

    LedToggle onLed;
    IntDragBox rangeBox, trackBox;

    GlyphButton openBtn { GlyphButton::Glyph::open };
    FolderIconButton revealBtn;
    GlyphButton clearBtn { GlyphButton::Glyph::clear };

    WheelComboBox declickBox, stretchBox;

    LedToggle keepDiskLed, resampleLed, regionsLed, slicesLed;
    LedToggle useLoopLed, pingPongLed;

    std::unique_ptr<ValueKnob> panKnob, volKnob, pitchKnob;
    std::unique_ptr<ValueKnob> startOffsetKnob, stretchPitchKnob, mulKnob, formantKnob;
    std::unique_ptr<TimeKnob> timeKnob;
    std::unique_ptr<ValueKnob> smpStartKnob, lengthKnob, inKnob, outKnob;
    std::unique_ptr<ValueKnob> crossfadeKnob, trimKnob;
    std::vector<KnobBind> knobBinds;

    LedToggle fxDcLed, fxNormLed, fxRevLed, fxPolLed, fxFadeStLed, fxSwapStLed;

    RootKeyboard rootKb;
    juce::TextButton resetBtn { "Reset" }, detectBtn { "Detect" };
    std::unique_ptr<ValueKnob> fineKnob;
    juce::String detectStatus;
    std::vector<PitchDetect::Region> detectedRegions;
    juce::int64 detectFileSamples = 0;

    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<juce::FileChooser> saveChooser;
    juce::ThreadPool waveformPool { 1 };
    juce::ThreadPool analysisPool { 1 };
    std::vector<float> peaks;
    std::vector<double> embeddedCues;
    double embeddedLoopStart = -1.0;
    double embeddedLoopEnd = -1.0;
    double waveSampleRate = 0.0;
    double waveDuration = 0.0;
    double waveViewStart = 0.0;
    double waveViewEnd = 1.0;
    juce::int64 waveSampleCount = 0;
    int waveBits = 0;
    int waveChannels = 0;
    int waveformGeneration = 0;
    bool waveformLoading = false;
    juce::String waveStatus;
    ZoomScrollBar waveScroll;

    WaveDrag waveDrag = WaveDrag::none;
    bool waveDragStarted = false;
    double dragStartSampleStart = 0.0;
    double dragStartSampleLength = 1.0;
    double dragStartOffset = 0.0;
    double dragStartLoopStart = 0.0;
    double dragStartLoopEnd = 1.0;

    juce::Rectangle<int> headerR, fileLabelR, fileNameR, contentR, playbackR, effectsR,
                         stretchR, declickLabelR, stretchModeLabelR, waveR,
                         rootCaptionR, fineLabelR, statusR;
    std::vector<CapLabel> headerLabels, knobLabels;

    static constexpr int headerH = 48;
    static constexpr int fileRowH = 28;
    static constexpr int headerSlotW = 42;
    static constexpr int clusterWidth = headerSlotW * 6;
    static constexpr int topGroupH = 96;
    static constexpr int bottomGroupH = 114;
    static constexpr int groupGapV = 8;
    static constexpr int rootRowH = 24;
    static constexpr int keyboardH = 44;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SamplerSettingsPanel)
};
