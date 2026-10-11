#include "PianoRoll.h"
#include "SmoothValue.h"
#include "BrowserPanel.h"
#include "AppLookAndFeel.h"
#include "Dialogs.h"
#include "AppSettings.h"
#include "ValueKnob.h"
#include "../Model/Scales.h"
#include "../Import/MidiImport.h"
#include <climits>
#include <cmath>

namespace
{
    constexpr int numKeys = 128;
    bool isBlackKey (int key)
    {
        int n = key % 12;
        return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
    }

    juce::String keyName (int key)
    {
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F",
                                       "F#", "G", "G#", "A", "A#", "B" };
        return juce::String (names[key % 12]) + juce::String (key / 12);
    }
}

class PianoRoll::GridViewport : public juce::Viewport
{
public:
    explicit GridViewport (PianoRoll& o) : owner (o) {}
    void visibleAreaChanged (const juce::Rectangle<int>&) override { owner.syncScroll(); }
    PianoRoll& owner;
};

class PianoRoll::Keyboard : public juce::Component
{
public:
    explicit Keyboard (PianoRoll& o) : owner (o) {}

    void paint (juce::Graphics& g) override
    {
        for (int key = 0; key < numKeys; ++key)
        {
            auto r = keyRect (key);
            bool black = isBlackKey (key);
            auto rf = r.toFloat().withTrimmedBottom (1.0f);
            float rad = juce::jmin (Metrics::rS, rf.getHeight() * 0.5f);

            juce::Path shape;
            shape.addRoundedRectangle (rf.getX(), rf.getY(), rf.getWidth(), rf.getHeight(),
                                       rad, rad, false, true, false, true);

            g.setColour (black ? Colors::pianoBlackKey : Colors::pianoWhiteKey);
            g.fillPath (shape);

            if (playGlow[key] > 0.001f)
            {
                g.setColour (Colors::accent.withAlpha (playGlow[key] * 0.25f));
                g.fillPath (shape);
            }
            if (highlight[key])
            {
                g.setColour (Colors::accent.withAlpha (0.5f));
                g.fillPath (shape);
            }

            if (key % 12 == 0)
            {
                g.setColour (black ? Colors::text : Colors::pianoKeyText);
                g.setFont (Typography::semi (9.5f));
                g.drawText ("C" + juce::String (key / 12), r.reduced (5, 0),
                            juce::Justification::centredRight);
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown())
            return;
        heldKey = keyAt (e.getPosition().getY());
        if (heldKey >= 0)
        {
            owner.engine.auditionNote (owner.channelId, heldKey, 100);
            owner.setKeyHighlight (heldKey, true);
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown())
            return;
        if (heldKey >= 0)
        {
            owner.engine.auditionNoteOff (owner.channelId, heldKey);
            owner.setKeyHighlight (heldKey, false);
        }
        heldKey = -1;
    }

    void setHighlight (int midiNote, bool on)
    {
        if (midiNote < 0 || midiNote >= numKeys || highlight[midiNote] == on)
            return;
        highlight[midiNote] = on;
        repaint (keyRect (midiNote));
    }

    void clearHighlights()
    {
        for (int k = 0; k < numKeys; ++k)
            if (highlight[k])
            {
                highlight[k] = false;
                repaint (keyRect (k));
            }
    }

    void setPlayingKeys (const bool* active)
    {
        juce::uint32 now = juce::Time::getMillisecondCounter();
        float dt = (lastPlayMs == 0) ? 1.0f / 60.0f : (now - lastPlayMs) / 1000.0f;
        lastPlayMs = now;
        dt = juce::jlimit (1.0f / 240.0f, 0.1f, dt);

        for (int k = 0; k < numKeys; ++k)
        {
            float prev = playGlow[k];
            float next = active[k] ? juce::jmin (1.0f, prev + dt / 0.06f)
                                   : juce::jmax (0.0f, prev - dt / 0.30f);
            playGlow[k] = next;
            if (next != prev && (std::abs (next - prev) > 0.005f || next <= 0.0f || next >= 1.0f))
                repaint (keyRect (k));
        }
    }

    bool hasActiveGlow() const
    {
        for (int k = 0; k < numKeys; ++k)
            if (playGlow[k] > 0.0001f)
                return true;
        return false;
    }

    juce::Rectangle<int> keyRect (int key) const
    {
        return { 0, (numKeys - 1 - key) * owner.rowHeight, getWidth(), owner.rowHeight };
    }

    int keyAt (int y) const
    {
        return juce::jlimit (0, numKeys - 1, numKeys - 1 - y / owner.rowHeight);
    }

    PianoRoll& owner;
    int heldKey = -1;
    bool highlight[numKeys] = {};
    float playGlow[numKeys] = {};
    juce::uint32 lastPlayMs = 0;
};

class PianoRoll::Grid : public juce::Component
{
public:
    explicit Grid (PianoRoll& o) : owner (o)
    {
        glows.reserve (64);
        sparkles.reserve (48);
        hZoom.onStep = [this] (double v) { applyHZoom (v); };
        vZoom.onStep = [this] (double v)
        {
            int anchorGridY = owner.viewport->getViewPositionY() + vAnchorViewportY;
            owner.setVerticalZoom ((int) std::lround (v), anchorGridY, vAnchorViewportY);
        };
    }

    void applyHZoom (double newPx)
    {
        owner.setHZoom (newPx, hAnchorTick, hAnchorViewX);
    }

    void stopZoomAnim() { hZoom.stop(); vZoom.stop(); }

    void updateSize()
    {
        setSize (juce::jmax (100, (int) (owner.getGridTicks() * owner.pxPerTick),
                             owner.viewport != nullptr ? owner.viewport->getMaximumVisibleWidth() : 0),
                 numKeys * owner.rowHeight);
    }

    juce::Rectangle<int> noteRect (const juce::ValueTree& note) const
    {
        int start = (int) note.getProperty (IDs::start);
        int len = juce::jmax (1, (int) note.getProperty (IDs::length));
        int key = (int) note.getProperty (IDs::key);
        return { (int) (start * owner.pxPerTick),
                 (numKeys - 1 - key) * owner.rowHeight,
                 juce::jmax (3, (int) (len * owner.pxPerTick)),
                 owner.rowHeight };
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colors::panelDark);

        auto clip = g.getClipBounds();

        const bool showScale = owner.settings == nullptr
                            || AppSettings::showScaleHighlight (*owner.settings);
        for (int key = 0; key < numKeys; ++key)
        {
            int y = (numKeys - 1 - key) * owner.rowHeight;
            if (y + owner.rowHeight < clip.getY() || y > clip.getBottom())
                continue;

            if (! showScale)
                g.setColour (Colors::pianoRowFlat);
            else if (Scales::keyInScale (owner.project, key))
                g.setColour (Colors::scaleRow);
            else if (isBlackKey (key))
                g.setColour (Colors::pianoRowBlack);
            else
                g.setColour (Colors::pianoRowWhite);
            g.fillRect (clip.getX(), y, clip.getWidth(), owner.rowHeight);

            if (key % 12 == 11)
            {
                g.setColour (Colors::gridBar.withAlpha (0.55f));
                g.drawHorizontalLine (y + owner.rowHeight - 1, (float) clip.getX(), (float) clip.getRight());
            }
            else if (! showScale)
            {
                g.setColour (Colors::gridLine.withAlpha (0.35f));
                g.drawHorizontalLine (y + owner.rowHeight - 1, (float) clip.getX(), (float) clip.getRight());
            }
        }

        {
            int snapT = owner.getSnapTicks();
            if (snapT < 1) snapT = 1;
            int gridStep = snapT <= 1 ? Model::ticksPerStep : snapT;
            const int lastTick = juce::jmin (owner.getGridTicks(),
                                             (int) ((clip.getRight() + 2) / juce::jmax (1.0e-6, owner.pxPerTick)));
            int firstTick = (int) ((clip.getX() - 2) / juce::jmax (1.0e-6, owner.pxPerTick));
            firstTick = juce::jmax (0, firstTick - (firstTick % gridStep));
            for (int tick = firstTick; tick <= lastTick; tick += gridStep)
            {
                int x = (int) (tick * owner.pxPerTick);
                if (tick % Model::ticksPerBar == 0)
                {
                    g.setColour (Colors::gridBar);
                    g.fillRect (x - 1, clip.getY(), 2, clip.getHeight());
                    continue;
                }
                if (tick % Model::PPQ == 0)               g.setColour (Colors::gridBar.withAlpha (0.6f));
                else if (tick % snapT == 0)               g.setColour (Colors::gridLine.withAlpha (0.8f));
                else                                      g.setColour (Colors::gridLine.withAlpha (0.35f));
                g.drawVerticalLine (x, (float) clip.getY(), (float) clip.getBottom());
            }
        }

        {
            int endX = (int) (owner.getPatternTicks() * owner.pxPerTick);
            if (endX < clip.getRight())
            {
                g.setColour (Colors::windowBg.withAlpha (0.55f));
                g.fillRect (juce::jmax (endX, clip.getX()), clip.getY(),
                            clip.getRight() - juce::jmax (endX, clip.getX()), clip.getHeight());
            }
        }

        if (owner.rangeStartTick >= 0 && owner.rangeEndTick > owner.rangeStartTick)
        {
            int rx0 = (int) (owner.rangeStartTick * owner.pxPerTick);
            int rx1 = (int) (owner.rangeEndTick * owner.pxPerTick);
            int drawX = juce::jmax (rx0, clip.getX());
            int drawR = juce::jmin (rx1, clip.getRight());
            if (drawR > drawX)
            {
                g.setColour (Colors::lassoTint);
                g.fillRect (drawX, clip.getY(), drawR - drawX, clip.getHeight());
            }
        }

        {
            auto pattern = owner.getPattern();
            if (pattern.isValid())
            {
                for (int c = 0; c < pattern.getNumChildren(); ++c)
                {
                    auto other = pattern.getChild (c);
                    if (! other.hasType (IDs::ChannelSeq))
                        continue;
                    int otherChannel = (int) other.getProperty (IDs::channelId, -1);
                    if (otherChannel == owner.channelId)
                        continue;
                    if (! Model::findChannelById (owner.project, otherChannel).isValid())
                        continue;
                    for (int i = 0; i < other.getNumChildren(); ++i)
                    {
                        auto note = other.getChild (i);
                        if (! note.hasType (IDs::Note))
                            continue;
                        auto r = noteRect (note).toFloat().reduced (0.5f, 1.5f);
                        if (! r.getSmallestIntegerContainer().intersects (clip))
                            continue;
                        g.setColour (Colors::ghostNote.withAlpha (0.30f));
                        g.fillRoundedRectangle (r, juce::jmin (Metrics::rS, r.getHeight() * 0.5f));
                    }
                }
            }
        }

        auto seq = owner.getChannelSeq (false);
        auto colour = Colors::noteGreen;
        if (seq.isValid())
        {
            for (int i = 0; i < seq.getNumChildren(); ++i)
            {
                auto note = seq.getChild (i);
                if (! note.hasType (IDs::Note))
                    continue;
                auto r = noteRect (note).toFloat().reduced (0.5f, 1.0f);
                if (! r.getSmallestIntegerContainer().intersects (clip))
                    continue;
                bool selected = owner.selection.contains (note);
                bool slide = (bool) note.getProperty (IDs::slide, false);
                int vel = (int) note.getProperty (IDs::velocity, 100);
                float rad = juce::jmin (Metrics::rS, r.getHeight() * 0.5f);
                auto body = colour.darker (0.55f * (1.0f - vel / 127.0f));
                if (selected)
                    body = body.brighter (0.30f);
                if (slide)
                    body = body.withAlpha (0.55f);
                g.setColour (body);
                g.fillRoundedRectangle (r, rad);

                if (r.getWidth() > rad * 2.0f)
                {
                    auto topEdge = r.withHeight (juce::jmin (1.5f, r.getHeight() * 0.3f))
                                    .reduced (rad * 0.6f, 0.0f);
                    g.setColour (body.brighter (0.5f).withAlpha (slide ? 0.45f : 0.8f));
                    g.fillRoundedRectangle (topEdge, topEdge.getHeight() * 0.5f);
                }

                if (selected)
                {
                    g.setColour (Colors::accent.brighter (0.55f));
                    g.drawRoundedRectangle (r.reduced (0.75f), rad, 1.5f);
                }

                if (slide)
                {
                    float ts = juce::jmin (7.0f, r.getWidth() * 0.6f, r.getHeight());
                    juce::Path tri;
                    tri.addTriangle (r.getRight() - ts, r.getY(), r.getRight(), r.getY(),
                                     r.getRight(), r.getY() + ts);
                    g.setColour (colour.brighter (0.8f));
                    g.fillPath (tri);
                }

                if (r.getWidth() > 10.0f)
                {
                    g.setColour (body.brighter (0.6f).withAlpha (0.85f));
                    g.fillRoundedRectangle (r.getRight() - 3.5f, r.getY() + 1.5f,
                                            2.0f, juce::jmax (1.0f, r.getHeight() - 3.0f), 1.0f);
                }

                if (r.getWidth() > 16.0f && r.getHeight() > 8.0f)
                {
                    g.setColour (Colors::windowBg.withAlpha (0.85f));
                    g.setFont (Typography::semi (juce::jmin (11.0f, r.getHeight() - 2.0f)));
                    g.drawText (keyName ((int) note.getProperty (IDs::key)),
                                r.toNearestInt().reduced (4, 0).withTrimmedRight (4),
                                juce::Justification::centredLeft, false);
                }
            }
        }

        if (! glows.empty())
        {
            const float gi = Fx::pianoGlowIntensity;
            const float go = juce::jmax (0.0f, gi * Fx::pianoGlowOpacity);
            auto glowCol = colour;
            auto sparkCol = colour.brighter (0.5f);
            const float expand[3] = { 7.0f, 4.0f, 2.0f };
            const float haloAlpha[3] = { 0.05f, 0.09f, 0.16f };
            for (auto& e : glows)
            {
                if (e.glow <= 0.0f || ! e.note.isValid())
                    continue;
                auto base = noteRect (e.note).toFloat().reduced (0.5f, 1.0f);
                if (! base.expanded (7.0f).getSmallestIntegerContainer().intersects (clip))
                    continue;
                float rad = juce::jmin (Metrics::rS, base.getHeight() * 0.5f);
                for (int p = 0; p < 3; ++p)
                {
                    g.setColour (glowCol.withAlpha (juce::jmin (1.0f, haloAlpha[p] * go * e.glow)));
                    g.fillRoundedRectangle (base.expanded (expand[p]), rad + expand[p] * 0.5f);
                }
                g.setColour (sparkCol.withAlpha (juce::jmin (1.0f, 0.25f * gi * e.glow)));
                g.fillRoundedRectangle (base, rad);
            }
        }
        if (! sparkles.empty())
        {
            const float gi = Fx::pianoGlowIntensity;
            auto sparkCol = colour.brighter (0.5f);
            for (auto& s : sparkles)
            {
                float t = s.age / juce::jmax (0.001f, s.life);
                float alpha = juce::jmin (1.0f, (1.0f - t) * 0.7f * gi);
                float sz = s.size0 * (1.0f - 0.5f * t);
                g.setColour (sparkCol.withAlpha (alpha));
                g.fillEllipse (s.x - sz, s.y - sz, sz * 2.0f, sz * 2.0f);
            }
        }

        if (mode == Mode::rubber)
        {
            auto rr = rubberRect.toFloat();
            if (rr.getWidth() > 2.0f && rr.getHeight() > 2.0f)
            {
                g.setColour (Colors::lassoTint);
                g.fillRoundedRectangle (rr, Metrics::rS);
                g.setColour (Colors::accent.withAlpha (0.8f));
                g.drawRoundedRectangle (rr.reduced (0.5f), Metrics::rS, 1.0f);
            }
        }

        if (mode == Mode::slice)
        {
            int x = (int) std::round (sliceTick * owner.pxPerTick);
            float yTop = (float) juce::jmin (sliceY0, sliceY1);
            float yBot = (float) juce::jmax (sliceY0, sliceY1);
            g.setColour (Colors::toolTrim);
            g.drawVerticalLine (x, yTop, yBot);
            g.setColour (Colors::toolTrim.withAlpha (0.25f));
            g.drawVerticalLine (x - 1, yTop, yBot);
            g.drawVerticalLine (x + 1, yTop, yBot);
        }

        if (! owner.selection.isEmpty())
        {
            auto handle = getGroupResizeHandle();
            if (! handle.isEmpty())
            {
                auto hf = handle.toFloat();
                g.setColour (Colors::textDim.brighter (0.3f));
                float cy = hf.getCentreY();
                float lx = hf.getX() + 2.0f, rx = hf.getRight() - 2.0f;
                float mx = hf.getCentreX();
                g.drawLine (lx, cy, mx - 1.0f, cy, 1.5f);
                g.drawLine (lx, cy, lx + 3.5f, cy - 3.0f, 1.5f);
                g.drawLine (lx, cy, lx + 3.5f, cy + 3.0f, 1.5f);
                g.drawLine (mx + 1.0f, cy, rx, cy, 1.5f);
                g.drawLine (rx, cy, rx - 3.5f, cy - 3.0f, 1.5f);
                g.drawLine (rx, cy, rx - 3.5f, cy + 3.0f, 1.5f);
            }
        }

        {
            double pos = owner.playheadTicks();
            if (pos < 0.0 && ! owner.engine.isPlaying())
                pos = std::fmod (owner.engine.getPositionTicks(),
                                 (double) juce::jmax (1, owner.getPatternTicks()));
            if (pos >= 0.0)
            {
                int x = (int) (pos * owner.pxPerTick);
                g.setColour (Colors::accent2);
                g.drawVerticalLine (x, 0.0f, (float) getHeight());
            }
        }
    }

    void repaintPlayhead()
    {
        double pos = owner.playheadTicks();
        if (pos < 0.0)
        {
            if (lastPlayheadX >= 0)
            {
                repaint (lastPlayheadX - 3, 0, 6, getHeight());
                lastPlayheadX = -1;
            }
            return;
        }
        int x = (int) (pos * owner.pxPerTick);
        if (lastPlayheadX < 0)
        {
            repaint (x - 3, 0, 6, getHeight());
            lastPlayheadX = x;
            return;
        }
        if (x < lastPlayheadX)
        {
            repaint (lastPlayheadX - 3, 0, 6, getHeight());
            repaint (x - 3, 0, 6, getHeight());
        }
        else
        {
            int lo = lastPlayheadX - 3, hi = x + 3;
            repaint (lo, 0, hi - lo, getHeight());
        }
        lastPlayheadX = x;
    }

    void resetPlayhead() { lastPlayheadX = -1; }

    bool hasGlowActivity() const { return ! glows.empty() || ! sparkles.empty(); }

    void animateGlow()
    {
        juce::uint32 now = juce::Time::getMillisecondCounter();

        if (! Fx::glowEnabled || Fx::pianoGlowIntensity <= 0.001f)
        {
            lastGlowMs = now;
            if (glows.empty() && sparkles.empty())
                return;
            juce::Rectangle<int> dirty;
            for (auto& e : glows)
                if (e.note.isValid())
                    dirty = dirty.getUnion (glowBounds (e.note));
            for (auto& s : sparkles)
            {
                int pad = (int) std::ceil (s.size0) + 2;
                dirty = dirty.getUnion ({ (int) s.x - pad, (int) s.y - pad, pad * 2, pad * 2 });
            }
            glows.clear();
            sparkles.clear();
            if (owner.viewport != nullptr)
                dirty = dirty.getIntersection (owner.viewport->getViewArea());
            if (! dirty.isEmpty())
                repaint (dirty);
            return;
        }

        float dt = (lastGlowMs == 0) ? 1.0f / 60.0f : (now - lastGlowMs) / 1000.0f;
        lastGlowMs = now;
        dt = juce::jlimit (1.0f / 240.0f, 0.1f, dt);

        const double posTicks = owner.playheadTicks();

        if (glows.empty() && sparkles.empty() && posTicks < 0.0)
            return;

        for (auto& e : glows)
            e.active = false;

        auto seq = owner.getChannelSeq (false);
        if (posTicks >= 0.0 && seq.isValid())
        {
            for (int i = 0; i < seq.getNumChildren(); ++i)
            {
                auto note = seq.getChild (i);
                if (! note.hasType (IDs::Note))
                    continue;
                if ((bool) note.getProperty (IDs::slide, false))
                    continue;
                int start = (int) note.getProperty (IDs::start);
                int len = juce::jmax (1, (int) note.getProperty (IDs::length));
                if (posTicks < start || posTicks >= start + len)
                    continue;
                GlowEntry* e = findGlow (note);
                if (e == nullptr)
                {
                    glows.push_back ({ note, 0.0f, true });
                    e = &glows.back();
                }
                e->active = true;
            }
        }

        juce::Rectangle<int> dirty;
        for (size_t i = 0; i < glows.size();)
        {
            auto& e = glows[i];
            if (! e.note.isValid() || ! e.note.getParent().isValid())
            {
                glows[i] = glows.back();
                glows.pop_back();
                continue;
            }
            if (e.active)
            {
                e.glow = juce::jmin (1.0f, e.glow + dt / 0.06f);
                emitSparkles (e.note, dt);
            }
            else
            {
                e.glow = juce::jmax (0.0f, e.glow - dt / 0.30f);
            }
            dirty = dirty.getUnion (glowBounds (e.note));
            if (! e.active && e.glow <= 0.0001f)
            {
                glows[i] = glows.back();
                glows.pop_back();
                continue;
            }
            ++i;
        }

        advanceSparkles (dt, dirty);

        if (owner.viewport != nullptr)
            dirty = dirty.getIntersection (owner.viewport->getViewArea());
        if (! dirty.isEmpty())
            repaint (dirty);
    }

    juce::ValueTree noteAt (juce::Point<int> pos) const
    {
        auto seq = owner.getChannelSeq (false);
        if (! seq.isValid())
            return {};
        for (int i = seq.getNumChildren(); --i >= 0;)
        {
            auto note = seq.getChild (i);
            if (note.hasType (IDs::Note) && noteRect (note).contains (pos))
                return note;
        }
        return {};
    }

    int snapTick (double tick, int snap) const
    {
        if (snap <= 1)
            return juce::jmax (0, (int) tick);
        return juce::jmax (0, (int) (std::round (tick / snap) * snap));
    }

    int snapTick (double tick) const
    {
        return snapTick (tick, owner.getSnapTicks());
    }

    int keyAtY (int y) const
    {
        return juce::jlimit (0, numKeys - 1, numKeys - 1 - y / owner.rowHeight);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown())
            return;
        owner.grabKeyboardFocus();

        if (owner.tool == Tool::slice && ! e.mods.isPopupMenu())
        {
            mode = Mode::slice;
            sliceTick = snapTick (e.getPosition().getX() / owner.pxPerTick);
            sliceY0 = sliceY1 = e.getPosition().getY();
            repaint();
            return;
        }

        auto note = noteAt (e.getPosition());

        if (! e.mods.isPopupMenu() && ! note.isValid() && ! owner.selection.isEmpty())
        {
            auto handle = getGroupResizeHandle();
            if (handle.expanded (4).contains (e.getPosition()))
            {
                mode = Mode::groupResize;
                groupResizeStartX = e.getPosition().getX();
                groupResizeInfos.clearQuick();
                groupAnchor = INT_MAX;
                int maxEnd = 0;
                for (auto& n : owner.selection)
                {
                    int s = (int) n.getProperty (IDs::start);
                    int l = juce::jmax (1, (int) n.getProperty (IDs::length));
                    groupResizeInfos.add ({ n, s, l });
                    groupAnchor = juce::jmin (groupAnchor, s);
                    maxEnd = juce::jmax (maxEnd, s + l);
                }
                groupSpan = juce::jmax (1, maxEnd - groupAnchor);
                repaint();
                return;
            }
        }

        if (e.mods.isPopupMenu() && e.getNumberOfClicks() >= 2 && ! note.isValid())
        {
            auto pattern = owner.getPattern();
            if (pattern.isValid())
            {
                for (int c = pattern.getNumChildren(); --c >= 0;)
                {
                    auto other = pattern.getChild (c);
                    if (! other.hasType (IDs::ChannelSeq))
                        continue;
                    int otherChannel = (int) other.getProperty (IDs::channelId, -1);
                    if (otherChannel == owner.channelId)
                        continue;
                    if (! Model::findChannelById (owner.project, otherChannel).isValid())
                        continue;
                    for (int i = other.getNumChildren(); --i >= 0;)
                    {
                        auto ghost = other.getChild (i);
                        if (ghost.hasType (IDs::Note) && noteRect (ghost).contains (e.getPosition()))
                        {
                            owner.setTarget (owner.patternId, otherChannel);
                            return;
                        }
                    }
                }
            }
        }

        if (e.mods.isPopupMenu())
        {
            mode = Mode::erase;
            if (note.isValid())
                note.getParent().removeChild (note, nullptr);
            owner.selection.clearQuick();
            repaint();
            return;
        }

        if (note.isValid())
        {
            if (owner.tool == Tool::select)
                owner.setTool (Tool::draw);
            auto r = noteRect (note);
            dragNote = note;

            owner.lastNoteLength = juce::jmax (6, (int) note.getProperty (IDs::length));
            owner.lastNoteVelocity = juce::jlimit (1, 127, (int) note.getProperty (IDs::velocity, 100));

            if (! owner.selection.contains (note))
            {
                if (! e.mods.isShiftDown())
                    owner.selection.clearQuick();
                owner.selection.add (note);
            }

            {
                int clickedKey = (int) note.getProperty (IDs::key);
                owner.engine.auditionNote (owner.channelId, clickedKey, 100);
                owner.setKeyHighlight (clickedKey, true);
                lastAuditionKey = clickedKey;
            }

            if (e.getPosition().getX() > r.getRight() - resizeEdge
                || e.mods.isShiftDown())
            {
                mode = Mode::resize;
                startResize();
            }
            else
            {
                mode = Mode::move;
                grabTickOffset = (int) (e.getPosition().getX() / owner.pxPerTick)
                                 - (int) note.getProperty (IDs::start);
                moveInfos.clear();
                for (auto& n : owner.selection)
                    moveInfos.add ({ n, (int) n.getProperty (IDs::start), (int) n.getProperty (IDs::key) });
            }
            repaint();
            return;
        }

        if (e.mods.isCtrlDown())
        {
            mode = Mode::rubber;
            rubberAnchor = e.getPosition();
            rubberRect = {};
            owner.selection.clearQuick();
            repaint();
            return;
        }

        if (e.mods.isShiftDown())
        {
        }
        else if (owner.tool == Tool::select)
        {
            mode = Mode::rubber;
            rubberAnchor = e.getPosition();
            rubberRect = {};
            owner.selection.clearQuick();
            repaint();
            return;
        }

        auto seq = owner.getChannelSeq (true);
        if (! seq.isValid())
            return;

        int tick = snapTick (e.getPosition().getX() / owner.pxPerTick);
        if (tick >= owner.getGridTicks())
            return;
        int key = keyAtY (e.getPosition().getY());

        owner.ensurePatternCovers (tick + owner.lastNoteLength - 1);

        seq.setProperty (IDs::usePianoRoll, true, nullptr);

        juce::ValueTree newNote (IDs::Note);
        newNote.setProperty (IDs::key, key, nullptr);
        newNote.setProperty (IDs::start, tick, nullptr);
        newNote.setProperty (IDs::length, owner.lastNoteLength, nullptr);
        newNote.setProperty (IDs::velocity, owner.lastNoteVelocity, nullptr);
        if (owner.slideMode)
            newNote.setProperty (IDs::slide, true, nullptr);
        seq.appendChild (newNote, nullptr);

        owner.selection.clearQuick();
        owner.selection.add (newNote);
        dragNote = newNote;
        mode = Mode::move;
        grabTickOffset = 0;
        moveInfos.clear();
        moveInfos.add ({ newNote, tick, key });
        owner.engine.auditionNote (owner.channelId, key, owner.lastNoteVelocity);
        owner.setKeyHighlight (key, true);
        lastAuditionKey = key;
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown())
            return;
        if (mode == Mode::slice)
        {
            sliceTick = snapTick (e.getPosition().getX() / owner.pxPerTick);
            sliceY1 = e.getPosition().getY();
            repaint();
            return;
        }

        if (mode == Mode::erase)
        {
            auto note = noteAt (e.getPosition());
            if (note.isValid())
            {
                owner.selection.removeAllInstancesOf (note);
                note.getParent().removeChild (note, nullptr);
                repaint();
            }
            return;
        }

        if (mode == Mode::rubber)
        {
            auto cursorPos = e.getPosition();
            rubberRect = juce::Rectangle<int>::leftTopRightBottom (
                juce::jmin (rubberAnchor.getX(), cursorPos.getX()),
                juce::jmin (rubberAnchor.getY(), cursorPos.getY()),
                juce::jmax (rubberAnchor.getX(), cursorPos.getX()),
                juce::jmax (rubberAnchor.getY(), cursorPos.getY()));
            owner.selection.clearQuick();
            auto seq = owner.getChannelSeq (false);
            if (seq.isValid())
                for (int i = 0; i < seq.getNumChildren(); ++i)
                {
                    auto note = seq.getChild (i);
                    if (note.hasType (IDs::Note) && noteRect (note).intersects (rubberRect))
                        owner.selection.add (note);
                }
            repaint();
            return;
        }

        if (mode == Mode::groupResize)
        {
            int dx = e.getPosition().getX() - groupResizeStartX;
            int deltaTicks = (int) (dx / owner.pxPerTick);
            int snap = owner.getSnapTicks();
            int newSpan = juce::jmax (6, groupSpan + deltaTicks);
            if (e.mods.isAltDown() && snap > 1)
                newSpan = juce::jmax (snap, ((newSpan + snap / 2) / snap) * snap);
            double factor = newSpan / (double) groupSpan;
            int maxEnd = 0;
            for (auto& info : groupResizeInfos)
            {
                int newStart = groupAnchor + (int) std::round ((info.origStart - groupAnchor) * factor);
                int newLen = juce::jmax (1, (int) std::round (info.origLen * factor));
                info.note.setProperty (IDs::start, newStart, nullptr);
                info.note.setProperty (IDs::length, newLen, nullptr);
                maxEnd = juce::jmax (maxEnd, newStart + newLen);
            }
            owner.ensurePatternCovers (maxEnd - 1);
            repaint();
            return;
        }

        if (! dragNote.isValid())
            return;

        if (mode == Mode::move && e.mods.isShiftDown())
        {
            mode = Mode::resize;
            startResize();
        }

        if (mode == Mode::move)
        {
            int newStart = snapTick (e.getPosition().getX() / owner.pxPerTick - grabTickOffset, owner.getSnapTicks());
            int key = keyAtY (e.getPosition().getY());

            int primaryStart = (int) dragNote.getProperty (IDs::start), primaryKey = 0;
            for (auto& info : moveInfos)
                if (info.note == dragNote) { primaryStart = info.start; primaryKey = info.key; }

            newStart = juce::jlimit (0, juce::jmax (0, owner.getGridTicks()
                                       - (int) dragNote.getProperty (IDs::length)), newStart);
            int dTick = newStart - primaryStart;
            int dKey = key - primaryKey;

            int maxEnd = 0;
            for (auto& info : moveInfos)
            {
                int start = juce::jmax (0, info.start + dTick);
                info.note.setProperty (IDs::start, start, nullptr);
                info.note.setProperty (IDs::key, juce::jlimit (0, numKeys - 1, info.key + dKey), nullptr);
                maxEnd = juce::jmax (maxEnd, start + (int) info.note.getProperty (IDs::length));
            }
            owner.ensurePatternCovers (maxEnd - 1);

            if (key != lastAuditionKey)
            {
                if (lastAuditionKey >= 0)
                {
                    owner.engine.auditionNoteOff (owner.channelId, lastAuditionKey);
                    owner.setKeyHighlight (lastAuditionKey, false);
                }
                owner.engine.auditionNote (owner.channelId, key, 100);
                owner.setKeyHighlight (key, true);
                lastAuditionKey = key;
            }
            repaint();
        }
        else if (mode == Mode::resize)
        {
            int noteSnap = owner.getSnapTicks();
            int start = (int) dragNote.getProperty (IDs::start);
            int endTick = snapTick (e.getPosition().getX() / owner.pxPerTick, noteSnap);
            int len = juce::jmax (noteSnap > 1 ? noteSnap : 6, endTick - start);
            len = juce::jmin (len, owner.getGridTicks() - start);
            len = juce::jmax (6, len);

            int dragOrigLen = len;
            for (auto& info : resizeInfos)
                if (info.note == dragNote)
                    dragOrigLen = info.origLen;
            int dLen = len - dragOrigLen;

            int maxEnd = start + len;
            for (auto& info : resizeInfos)
            {
                int nl = juce::jmax (6, info.origLen + dLen);
                nl = juce::jmin (nl, juce::jmax (6, owner.getGridTicks() - info.origStart));
                info.note.setProperty (IDs::length, nl, nullptr);
                maxEnd = juce::jmax (maxEnd, info.origStart + nl);
            }
            if (resizeInfos.isEmpty())
                dragNote.setProperty (IDs::length, len, nullptr);

            owner.lastNoteLength = len;
            owner.ensurePatternCovers (maxEnd - 1);
            repaint();
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown())
            return;
        if (mode == Mode::slice)
            sliceCrossedNotes();
        if (lastAuditionKey >= 0)
        {
            owner.engine.auditionNoteOff (owner.channelId, lastAuditionKey);
            owner.setKeyHighlight (lastAuditionKey, false);
            lastAuditionKey = -1;
        }
        dragNote = {};
        moveInfos.clear();
        groupResizeInfos.clearQuick();
        resizeInfos.clearQuick();
        mode = Mode::none;
        repaint();
        owner.velocityLaneRepaint();
    }

    void sliceCrossedNotes()
    {
        int yTop = juce::jmin (sliceY0, sliceY1);
        int yBot = juce::jmax (sliceY0, sliceY1);

        auto seq = owner.getChannelSeq (false);
        if (! seq.isValid())
            return;

        juce::Array<juce::ValueTree> candidates;
        if (! owner.selection.isEmpty())
            candidates = owner.selection;
        else
            for (int i = 0; i < seq.getNumChildren(); ++i)
                if (seq.getChild (i).hasType (IDs::Note))
                    candidates.add (seq.getChild (i));

        for (auto& note : candidates)
        {
            if (! note.hasType (IDs::Note))
                continue;
            int start = (int) note.getProperty (IDs::start);
            int len   = juce::jmax (1, (int) note.getProperty (IDs::length));
            int end   = start + len;
            if (sliceTick <= start || sliceTick >= end)
                continue;
            auto r = noteRect (note);
            if (yBot < r.getY() || yTop > r.getBottom())
                continue;

            note.setProperty (IDs::length, sliceTick - start, nullptr);

            juce::ValueTree right (IDs::Note);
            right.setProperty (IDs::key, (int) note.getProperty (IDs::key), nullptr);
            right.setProperty (IDs::start, sliceTick, nullptr);
            right.setProperty (IDs::length, end - sliceTick, nullptr);
            right.setProperty (IDs::velocity, (int) note.getProperty (IDs::velocity, 100), nullptr);
            note.getParent().appendChild (right, nullptr);
        }
        repaint();
        owner.velocityLaneRepaint();
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        if (owner.tool == Tool::slice)
        {
            setMouseCursor (juce::MouseCursor::CrosshairCursor);
            return;
        }
        if (! owner.selection.isEmpty())
        {
            auto handle = getGroupResizeHandle();
            if (handle.expanded (4).contains (e.getPosition()))
            {
                setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
                return;
            }
        }
        auto note = noteAt (e.getPosition());
        bool onEdge = note.isValid()
                      && e.getPosition().getX() > noteRect (note).getRight() - resizeEdge;
        setMouseCursor (onEdge ? juce::MouseCursor::LeftRightResizeCursor
                               : juce::MouseCursor::NormalCursor);
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        if (e.mods.isAltDown())
        {
            vAnchorViewportY = e.getEventRelativeTo (owner.viewport.get()).getPosition().getY();
            int base = vZoom.isAnimating() ? (int) std::lround (vZoom.getTarget()) : owner.rowHeight;
            int newTarget = juce::jlimit (6, 60, base + (wheel.deltaY > 0 ? 3 : -3));
            vZoom.animateTo (owner.rowHeight, newTarget);
        }
        else if (e.mods.isCtrlDown())
        {
            hAnchorTick = e.getPosition().getX() / owner.pxPerTick;
            hAnchorViewX = e.getPosition().getX() - owner.viewport->getViewPositionX();
            double base = hZoom.isAnimating() ? hZoom.getTarget() : owner.pxPerTick;
            double newTarget = juce::jlimit (PianoRoll::minPxPerTick, PianoRoll::maxPxPerTick,
                                             base * (wheel.deltaY > 0 ? 1.2 : 1.0 / 1.2));
            hZoom.animateTo (owner.pxPerTick, newTarget);
        }
        else
        {
            Component::mouseWheelMove (e, wheel);
        }
    }

    struct MoveInfo { juce::ValueTree note; int start; int key; };
    struct GroupResizeInfo { juce::ValueTree note; int origStart; int origLen; };

    void startResize()
    {
        resizeInfos.clearQuick();
        for (auto& n : owner.selection)
            resizeInfos.add ({ n, (int) n.getProperty (IDs::start),
                               juce::jmax (1, (int) n.getProperty (IDs::length)) });
    }

    juce::Rectangle<int> getGroupResizeHandle() const
    {
        if (owner.selection.isEmpty())
            return {};
        int maxRight = 0, minTop = getHeight(), maxBottom = 0;
        for (auto& n : owner.selection)
        {
            auto r = noteRect (n);
            maxRight = juce::jmax (maxRight, r.getRight());
            minTop = juce::jmin (minTop, r.getY());
            maxBottom = juce::jmax (maxBottom, r.getBottom());
        }
        int cy = (minTop + maxBottom) / 2;
        return { maxRight + 14, cy - 8, 16, 16 };
    }

    struct GlowEntry { juce::ValueTree note; float glow = 0.0f; bool active = false; };
    struct Sparkle { float x = 0, y = 0, vy = 0, age = 0, life = 0, size0 = 0; };

    GlowEntry* findGlow (const juce::ValueTree& note)
    {
        for (auto& e : glows)
            if (e.note == note)
                return &e;
        return nullptr;
    }

    juce::Rectangle<int> glowBounds (const juce::ValueTree& note) const
    {
        auto r = noteRect (note);
        return { r.getX() - 10, r.getY() - 26, r.getWidth() + 20, r.getHeight() + 36 };
    }

    void emitSparkles (const juce::ValueTree& note, float dt)
    {
        if (sparkles.size() >= 40)
            return;
        auto r = noteRect (note).toFloat();
        if (r.getWidth() < 1.0f)
            return;
        if (glowRandom.nextFloat() > dt * 14.0f * Fx::pianoGlowIntensity)
            return;
        Sparkle s;
        s.x = r.getX() + glowRandom.nextFloat() * r.getWidth();
        s.y = r.getY() - glowRandom.nextFloat() * 2.0f;
        s.life = 0.4f + glowRandom.nextFloat() * 0.3f;
        s.vy = -(10.0f + glowRandom.nextFloat() * 10.0f) / s.life;
        s.size0 = 1.5f + glowRandom.nextFloat() * 1.5f;
        sparkles.push_back (s);
    }

    void advanceSparkles (float dt, juce::Rectangle<int>& dirty)
    {
        for (size_t i = 0; i < sparkles.size();)
        {
            auto& s = sparkles[i];
            s.age += dt;
            s.y += s.vy * dt;
            int pad = (int) std::ceil (s.size0) + 2;
            dirty = dirty.getUnion ({ (int) s.x - pad, (int) s.y - pad, pad * 2, pad * 2 });
            if (s.age >= s.life)
            {
                sparkles[i] = sparkles.back();
                sparkles.pop_back();
                continue;
            }
            ++i;
        }
    }

    enum class Mode { none, move, resize, erase, rubber, groupResize, slice };
    static constexpr int resizeEdge = 9;
    PianoRoll& owner;
    std::vector<GlowEntry> glows;
    std::vector<Sparkle> sparkles;
    juce::Random glowRandom;
    juce::uint32 lastGlowMs = 0;
    int lastPlayheadX = -1;
    SmoothZoomer hZoom, vZoom;
    int vAnchorViewportY = 0;
    double hAnchorTick = 0.0;
    int hAnchorViewX = 0;
    juce::ValueTree dragNote;
    juce::Array<MoveInfo> moveInfos;
    juce::Array<GroupResizeInfo> groupResizeInfos;
    juce::Array<GroupResizeInfo> resizeInfos;
    int groupResizeStartX = 0;
    int groupAnchor = 0;
    int groupSpan = 1;
    Mode mode = Mode::none;
    int grabTickOffset = 0;
    int lastAuditionKey = -1;
    juce::Point<int> rubberAnchor;
    juce::Rectangle<int> rubberRect;
    int sliceTick = 0;
    int sliceY0 = 0, sliceY1 = 0;
};

class PianoRoll::Ruler : public juce::Component
{
public:
    explicit Ruler (PianoRoll& o) : owner (o) {}

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colors::header);
        g.setFont (Typography::semi (10.5f));

        int patTicks  = owner.getPatternTicks();
        int gridTicks = owner.getGridTicks();
        for (int bar = 0; bar * Model::ticksPerBar <= gridTicks; ++bar)
        {
            int x = (int) (bar * Model::ticksPerBar * owner.pxPerTick);
            bool active = bar * Model::ticksPerBar < patTicks;
            g.setColour (Colors::gridBar);
            g.fillRect (x - 1, 3, 2, getHeight() - 3);
            g.setColour (active ? Colors::textDim : Colors::textDim.withAlpha (0.45f));
            g.drawText (juce::String (bar + 1), x + 4, 1, 30, getHeight() - 2,
                        juce::Justification::centredLeft);
        }
        for (int beat = 0; beat * Model::PPQ <= gridTicks; ++beat)
        {
            if (beat % Model::beatsPerBar == 0)
                continue;
            int x = (int) (beat * Model::PPQ * owner.pxPerTick);
            g.setColour (Colors::gridLine.withAlpha (0.7f));
            g.drawVerticalLine (x, (float) getHeight() - 6.0f, (float) getHeight());
        }

        if (owner.rangeStartTick >= 0 && owner.rangeEndTick > owner.rangeStartTick)
        {
            int x0 = (int) (owner.rangeStartTick * owner.pxPerTick);
            int x1 = (int) (owner.rangeEndTick * owner.pxPerTick);
            g.setColour (Colors::lassoTint);
            g.fillRect (x0, 0, x1 - x0, getHeight());
        }

        double beats = owner.playheadTicks();
        if (beats < 0.0 && ! owner.engine.isPlaying())
            beats = std::fmod (owner.engine.getPositionTicks(),
                               (double) juce::jmax (1, patTicks));
        if (beats >= 0.0)
        {
            int px = (int) (beats * owner.pxPerTick);
            g.setColour (Colors::accent2);
            g.drawVerticalLine (px, 0.0f, (float) getHeight());
            juce::Path marker;
            marker.addTriangle ((float) px - 5.0f, 1.0f, (float) px + 5.0f, 1.0f, (float) px, 10.0f);
            g.fillPath (marker);
        }
    }

    void repaintPlayhead()
    {
        double beats = owner.playheadTicks();
        if (beats < 0.0)
        {
            if (lastPlayheadX >= 0)
            {
                repaint (lastPlayheadX - 8, 0, 16, getHeight());
                lastPlayheadX = -1;
            }
            return;
        }
        int x = (int) (beats * owner.pxPerTick);
        if (lastPlayheadX < 0)
        {
            repaint (x - 8, 0, 16, getHeight());
            lastPlayheadX = x;
            return;
        }
        if (x < lastPlayheadX)
        {
            repaint (lastPlayheadX - 8, 0, 16, getHeight());
            repaint (x - 8, 0, 16, getHeight());
        }
        else
        {
            int lo = lastPlayheadX - 8, hi = x + 8;
            repaint (lo, 0, hi - lo, getHeight());
        }
        lastPlayheadX = x;
    }

    void resetPlayhead() { lastPlayheadX = -1; }

    void seekTo (int x)
    {
        double rawTicks = juce::jmax (0.0, x / owner.pxPerTick);
        int snap = owner.getSnapTicks();
        if (snap > 1)
            rawTicks = std::round (rawTicks / snap) * snap;
        scrubPreviewTicks = rawTicks;
        owner.engine.seekTicks (scrubPreviewTicks);
        if (! owner.engine.isPlaying())
            owner.returnOnStopTick = scrubPreviewTicks;
        repaint();
        owner.gridRepaint();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown())
            return;
        dragAnchorX = e.getPosition().getX();
        dragging = false;
        scrubOnly = false;
        rightRange = e.mods.isRightButtonDown();

        bool hadRange = owner.rangeStartTick >= 0;
        owner.rangeStartTick = owner.rangeEndTick = -1;

        if (rightRange)
        {
            if (hadRange)
            {
                owner.gridRepaint();
                repaint();
            }
            return;
        }

        if (owner.tool == PianoRoll::Tool::select)
        {
            if (hadRange)
                owner.gridRepaint();
            return;
        }

        double posTicks = owner.playheadTicks();
        int playheadX = (int) (posTicks * owner.pxPerTick);
        scrubOnly = posTicks >= 0.0 && std::abs (e.getPosition().getX() - playheadX) < 24;

        scrubbing = true;
        seekTo (e.getPosition().getX());

        if (hadRange)
            owner.gridRepaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown())
            return;
        if (! dragging && ! scrubOnly
            && (rightRange || owner.tool == PianoRoll::Tool::select)
            && std::abs (e.getPosition().getX() - dragAnchorX) > 10)
            dragging = true;

        if (dragging)
        {
            scrubbing = false;
            int snap = owner.getSnapTicks();
            if (snap <= 1) snap = Model::ticksPerStep;

            double t0 = dragAnchorX / owner.pxPerTick;
            double t1 = e.getPosition().getX() / owner.pxPerTick;
            if (t0 > t1) std::swap (t0, t1);

            owner.rangeStartTick = juce::jmax (0, ((int) t0 / snap) * snap);
            owner.rangeEndTick = juce::jmax (owner.rangeStartTick + snap,
                                              (((int) t1 + snap - 1) / snap) * snap);

            owner.selection.clearQuick();
            owner.gridRepaint();
            repaint();
        }
        else if (scrubbing)
        {
            seekTo (e.getPosition().getX());
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown())
            return;
        scrubbing = false;
        dragging = false;
        scrubOnly = false;
        rightRange = false;
        repaint();
    }

    PianoRoll& owner;
    bool scrubbing = false;
    bool dragging = false;
    bool scrubOnly = false;
    bool rightRange = false;
    int dragAnchorX = 0;
    int lastPlayheadX = -1;
    double scrubPreviewTicks = 0;
};

class PianoRoll::LaneDivider : public juce::Component
{
public:
    explicit LaneDivider (PianoRoll& o) : owner (o)
    {
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colors::header);
        g.setColour (hovered ? Colors::accent : Colors::textDim.withAlpha (0.6f));
        auto cx = getWidth() / 2;
        g.fillRoundedRectangle ((float) cx - 14.0f, getHeight() / 2.0f - 1.0f, 28.0f, 2.0f, 1.0f);
    }

    void mouseEnter (const juce::MouseEvent&) override { hovered = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { hovered = false; repaint(); }
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown())
            return;
        startHeight = owner.laneHeight;
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown())
            return;
        int maxLane = juce::jmax (60, owner.getHeight() * 6 / 10);
        owner.laneHeight = juce::jlimit (36, maxLane, startHeight - e.getDistanceFromDragStartY());
        owner.resized();
    }

    PianoRoll& owner;
    int startHeight = 70;
    bool hovered = false;
};

class PianoRoll::VelocityLane : public juce::Component
{
public:
    explicit VelocityLane (PianoRoll& o) : owner (o) {}

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colors::panelDark);
        g.setColour (Colors::textDim);
        g.setFont (Typography::caps (9.0f));
        g.drawText ("VELOCITY", Metrics::padS, 0, 80, 12, juce::Justification::centredLeft);

        auto seq = owner.getChannelSeq (false);
        if (! seq.isValid())
            return;
        auto colour = Colors::noteGreen;

        for (int i = 0; i < seq.getNumChildren(); ++i)
        {
            auto note = seq.getChild (i);
            if (! note.hasType (IDs::Note))
                continue;
            int x = (int) ((int) note.getProperty (IDs::start) * owner.pxPerTick);
            int vel = (int) note.getProperty (IDs::velocity, 100);
            int h = juce::jmax (2, vel * (getHeight() - 4) / 127);
            auto bar = juce::Rectangle<float> ((float) x, (float) (getHeight() - h), 3.0f, (float) h);
            g.setColour (owner.selection.contains (note) ? Colors::accent : colour.withAlpha (0.85f));
            g.fillRoundedRectangle (bar, 1.5f);
        }
    }

    void applyAt (juce::Point<int> pos)
    {
        auto seq = owner.getChannelSeq (false);
        if (! seq.isValid())
            return;

        juce::ValueTree best;
        int bestDist = 8;
        for (int i = 0; i < seq.getNumChildren(); ++i)
        {
            auto note = seq.getChild (i);
            if (! note.hasType (IDs::Note))
                continue;
            int x = (int) ((int) note.getProperty (IDs::start) * owner.pxPerTick);
            int dist = std::abs (x - pos.getX());
            if (dist < bestDist)
            {
                bestDist = dist;
                best = note;
            }
        }
        if (best.isValid())
        {
            int vel = juce::jlimit (1, 127, 127 - pos.getY() * 127 / juce::jmax (1, getHeight()));
            best.setProperty (IDs::velocity, vel, nullptr);
            owner.lastNoteVelocity = vel;
            repaint();
            owner.gridRepaint();
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! e.mods.isLeftButtonDown())
            return;
        applyAt (e.getPosition());
        e.source.enableUnboundedMouseMovement (true, false);
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! e.mods.isLeftButtonDown())
            return;
        applyAt (e.getPosition());
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.source.isUnboundedMouseMovementEnabled())
        {
            auto src = e.source;
            src.enableUnboundedMouseMovement (false);
            src.setScreenPosition (e.getMouseDownScreenPosition().toFloat());
        }
    }

    PianoRoll& owner;
};

PianoRoll::PianoRoll (juce::ValueTree p, AudioEngine& e, juce::PropertiesFile* s)
    : project (p), engine (e), settings (s)
{
    project.addListener (this);

    channelBox.setTooltip ("Channel to place notes for");
    channelBox.onChange = [this]
    {
        int id = channelBox.getSelectedId();
        if (id > 0 && id != channelId)
            setTarget (patternId, id);
    };
    channelBox.onPopupAboutToShow = [this] { refreshTargetBoxes(); };
    addAndMakeVisible (channelBox);

    rootBox.setTooltip ("Scale root");
    for (int r = 0; r < 12; ++r)
        rootBox.addItem (Scales::rootName (r), r + 1);
    rootBox.onChange = [this]
    {
        int r = rootBox.getSelectedId() - 1;
        if (r >= 0)
            project.setProperty (IDs::scaleRoot, r, nullptr);
    };
    addAndMakeVisible (rootBox);

    scaleBox.setTooltip ("Scale type (highlights in-scale notes)");
    {
        int id = 1;
        for (auto& d : Scales::all())
            scaleBox.addItem (d.name, id++);
    }
    scaleBox.onChange = [this]
    {
        int idx = scaleBox.getSelectedId() - 1;
        if (idx >= 0 && idx < (int) Scales::all().size())
            project.setProperty (IDs::scaleType, Scales::all()[(size_t) idx].id, nullptr);
    };
    addAndMakeVisible (scaleBox);

    detectBtn.setTooltip ("Detect key/scale from the project's notes");
    detectBtn.onClick = [this]
    {
        auto res = Scales::detectKey (project);
        if (! res.hasNotes)
        {
            detectBtn.setButtonText ("No notes");
            juce::Component::SafePointer<PianoRoll> self (this);
            juce::Timer::callAfterDelay (1500, [self] { if (self) self->detectBtn.setButtonText (self->detectBtnLabel); });
            return;
        }
        project.setProperty (IDs::scaleRoot, res.root, nullptr);
        project.setProperty (IDs::scaleType, res.typeId, nullptr);

        auto name = juce::String (Scales::rootName (res.root)) + " " + Scales::defForId (res.typeId).name;
        detectBtn.setButtonText (name);
        juce::Component::SafePointer<PianoRoll> self (this);
        juce::Timer::callAfterDelay (1500, [self] { if (self) self->detectBtn.setButtonText (self->detectBtnLabel); });
    };
    addAndMakeVisible (detectBtn);

    scaleHiliteBtn.setTooltip ("Highlight in-scale rows (per-user, not saved with the project)");
    scaleHiliteBtn.setClickingTogglesState (true);
    scaleHiliteBtn.setToggleState (settings == nullptr || AppSettings::showScaleHighlight (*settings),
                                   juce::dontSendNotification);
    scaleHiliteBtn.onClick = [this]
    {
        if (settings != nullptr)
        {
            AppSettings::setShowScaleHighlight (*settings, scaleHiliteBtn.getToggleState());
            settings->saveIfNeeded();
        }
        grid->repaint();
    };
    addAndMakeVisible (scaleHiliteBtn);

    refreshScaleBoxes();

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
    snapBox.setSelectedId (6, juce::dontSendNotification);

    snapBtn.setTriggeredOnMouseDown (true);
    snapBtn.onClick = [this]
    {
        juce::PopupMenu m;
        for (int i = 1; i <= snapBox.getNumItems(); ++i)
            m.addItem (i, snapBox.getItemText (i - 1), true, snapBox.getSelectedId() == i);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&snapBtn),
            [this] (int r) { if (r > 0) { snapBox.setSelectedId (r, juce::dontSendNotification); int s = getSnapTicks(); if (s > 1) lastNoteLength = s; grid->repaint(); } });
    };
    addAndMakeVisible (snapBtn);

    menuBtn.setTooltip ("Piano roll options");
    menuBtn.setTriggeredOnMouseDown (true);
    menuBtn.onClick = [this]
    {
        juce::PopupMenu m;
        m.addItem (1, "Export MIDI...");
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&menuBtn),
            [this] (int r) { if (r == 1) exportMidi(); });
    };
    addAndMakeVisible (menuBtn);

    drawBtn.activeColour = Colors::accent2;
    drawBtn.onClick = [this] { setTool (Tool::draw); };
    addAndMakeVisible (drawBtn);
    selectBtn.activeColour = Colors::toolSelect;
    selectBtn.onClick = [this] { setTool (Tool::select); };
    addAndMakeVisible (selectBtn);
    sliceBtn.activeColour = Colors::toolTrim;
    sliceBtn.onClick = [this] { setTool (Tool::slice); };
    addAndMakeVisible (sliceBtn);
    slideBtn.activeColour = Colors::accent;
    slideBtn.onClick = [this] { slideMode = ! slideMode; slideBtn.setActive (slideMode); };
    addAndMakeVisible (slideBtn);

    grid = std::make_unique<Grid> (*this);
    viewport = std::make_unique<GridViewport> (*this);
    viewport->setViewedComponent (grid.get(), false);
    viewport->setScrollBarsShown (true, false);
    addAndMakeVisible (*viewport);

    keyboard = std::make_unique<Keyboard> (*this);
    keyboardClip.addAndMakeVisible (keyboard.get());
    keyboardClip.setInterceptsMouseClicks (false, true);
    addAndMakeVisible (keyboardClip);

    zoomBar.onRangeChanged = [this] (double vStart, double vEnd)
    {
        grid->stopZoomAnim();
        double span = juce::jmax (1.0, vEnd - vStart);
        double visibleW = viewport != nullptr ? viewport->getMaximumVisibleWidth() : getWidth();
        const juce::ScopedValueSetter<bool> barDrives (suppressZoomBarSync, true);
        setHZoom (visibleW / span, vStart, 0);
    };
    zoomBar.onDragEnd = [this] { syncZoomBar(); };
    addAndMakeVisible (zoomBar);

    ruler = std::make_unique<Ruler> (*this);
    rulerClip.addAndMakeVisible (ruler.get());
    rulerClip.setInterceptsMouseClicks (true, true);
    addAndMakeVisible (rulerClip);

    velocityLane = std::make_unique<VelocityLane> (*this);
    laneClip.addAndMakeVisible (velocityLane.get());
    laneClip.setInterceptsMouseClicks (false, true);
    addAndMakeVisible (laneClip);

    laneDivider = std::make_unique<LaneDivider> (*this);
    addAndMakeVisible (*laneDivider);

    setWantsKeyboardFocus (true);
    setTool (Tool::draw);
    refreshTargetBoxes();
    startTimerHz (30);
}

void PianoRoll::setTool (Tool t)
{
    tool = t;
    drawBtn.setActive (t == Tool::draw);
    selectBtn.setActive (t == Tool::select);
    sliceBtn.setActive (t == Tool::slice);
}

void PianoRoll::clearSelection()
{
    selection.clearQuick();
    grid->repaint();
}

std::vector<PianoRoll::ClipNote> PianoRoll::clipboard;
int PianoRoll::clipboardOrigin = 0;

void PianoRoll::setKeyHighlight (int midiNote, bool on)
{
    if (keyboard != nullptr)
        keyboard->setHighlight (midiNote, on);
}

void PianoRoll::clearKeyHighlights()
{
    if (keyboard != nullptr)
        keyboard->clearHighlights();
}

PianoRoll::~PianoRoll()
{
    project.removeListener (this);
}

void PianoRoll::setTarget (int newPatternId, int newChannelId)
{
    if (! Model::findPatternById (project, newPatternId).isValid())
    {
        if (Model::findPatternById (project, patternId).isValid())
            newPatternId = patternId;
        else if (Model::getPatterns (project).getNumChildren() > 0)
            newPatternId = (int) Model::getPatterns (project).getChild (0).getProperty (IDs::id);
    }

    if (! Model::findChannelById (project, newChannelId).isValid())
    {
        if (Model::findChannelById (project, channelId).isValid())
            newChannelId = channelId;
        else
        {
            auto channels = Model::getChannels (project);
            for (int i = 0; i < channels.getNumChildren(); ++i)
                if (channels.getChild (i).hasType (IDs::Channel))
                {
                    newChannelId = (int) channels.getChild (i).getProperty (IDs::id);
                    break;
                }
        }
    }

    const bool sameTarget = (newPatternId == patternId && newChannelId == channelId);

    patternId = newPatternId;
    channelId = newChannelId;

    if (sameTarget)
    {
        for (int i = selection.size(); --i >= 0;)
            if (selection.getReference (i).getRoot() != project)
                selection.remove (i);
    }
    else
        selection.clearQuick();

    if (auto pattern = getPattern(); pattern.isValid())
        Model::convertStepsToNotes (pattern, channelId, Model::middleC);

    refreshTargetBoxes();
    grid->updateSize();
    grid->repaint();
    velocityLane->repaint();

    if (! sameTarget)
    {
        juce::Array<juce::ValueTree> notes;
        if (auto seq = getChannelSeq (false); seq.isValid())
            for (int i = 0; i < seq.getNumChildren(); ++i)
                if (seq.getChild (i).hasType (IDs::Note))
                    notes.add (seq.getChild (i));

        if (! notes.isEmpty())
        {
            juce::int64 keySum = 0;
            for (auto& n : notes)
                keySum += (int) n.getProperty (IDs::key);
            int meanKey = juce::jlimit (0, numKeys - 1, (int) (keySum / notes.size()));

            const int viewH = viewport->getViewHeight();
            int centreY = (numKeys - 1 - meanKey) * rowHeight + rowHeight / 2;
            int y = juce::jlimit (0, juce::jmax (0, numKeys * rowHeight - viewH), centreY - viewH / 2);

            viewport->setViewPosition (0, y);
        }
        else
        {
            const int y = viewport->getViewPositionY() == 0 ? (numKeys - 1 - 72) * rowHeight
                                                            : viewport->getViewPositionY();
            viewport->setViewPosition (0, y);
        }
    }

    if (onTargetChanged)
        onTargetChanged (getTargetDescription());
}

void PianoRoll::selectAll()
{
    auto seq = getChannelSeq (false);
    if (! seq.isValid())
        return;
    selection.clearQuick();
    for (int i = 0; i < seq.getNumChildren(); ++i)
        if (seq.getChild (i).hasType (IDs::Note))
            selection.add (seq.getChild (i));
    gridRepaint();
    velocityLaneRepaint();
}

void PianoRoll::deleteSelection()
{
    if (selection.isEmpty())
        return;
    for (auto& note : selection)
        if (note.getParent().isValid())
            note.getParent().removeChild (note, nullptr);
    selection.clearQuick();
    gridRepaint();
    velocityLaneRepaint();
}

void PianoRoll::duplicateSelection()
{
    auto seq = getChannelSeq (false);
    if (! seq.isValid())
        return;

    if (rangeStartTick >= 0 && rangeEndTick > rangeStartTick)
    {
        int rangeLen = rangeEndTick - rangeStartTick;
        juce::Array<juce::ValueTree> newNotes;

        for (int i = 0; i < seq.getNumChildren(); ++i)
        {
            auto note = seq.getChild (i);
            if (! note.hasType (IDs::Note))
                continue;
            int s = (int) note.getProperty (IDs::start);
            if (s >= rangeStartTick && s < rangeEndTick)
            {
                juce::ValueTree copy (IDs::Note);
                copy.setProperty (IDs::key, (int) note.getProperty (IDs::key), nullptr);
                copy.setProperty (IDs::start, s + rangeLen, nullptr);
                copy.setProperty (IDs::length, (int) note.getProperty (IDs::length), nullptr);
                copy.setProperty (IDs::velocity, (int) note.getProperty (IDs::velocity, 100), nullptr);
                seq.appendChild (copy, nullptr);
                newNotes.add (copy);
            }
        }

        if (! newNotes.isEmpty())
        {
            ensurePatternCovers (rangeEndTick + rangeLen - 1);
            selection = newNotes;
            rangeStartTick += rangeLen;
            rangeEndTick   += rangeLen;
            grid->updateSize();
            grid->repaint();
            velocityLane->repaint();
            ruler->repaint();
            jumpPlaybackToLocalTick (rangeStartTick);
        }
        return;
    }

    juce::Array<juce::ValueTree> src = selection;
    if (src.isEmpty())
        for (int i = 0; i < seq.getNumChildren(); ++i)
            if (seq.getChild (i).hasType (IDs::Note))
                src.add (seq.getChild (i));
    if (src.isEmpty())
        return;

    int minStart = INT_MAX, maxEnd = 0;
    for (auto& n : src)
    {
        int s = (int) n.getProperty (IDs::start);
        int e = s + juce::jmax (1, (int) n.getProperty (IDs::length));
        minStart = juce::jmin (minStart, s);
        maxEnd = juce::jmax (maxEnd, e);
    }
    int span = maxEnd - minStart;
    int shift = ((span + Model::ticksPerBar - 1) / Model::ticksPerBar) * Model::ticksPerBar;
    if (shift <= 0)
        shift = Model::ticksPerBar;

    int needTicks = maxEnd + shift;
    int curSteps = (int) getPattern().getProperty (IDs::lengthSteps, Model::defaultSteps);
    int needSteps = (needTicks + Model::ticksPerStep - 1) / Model::ticksPerStep;
    if (needSteps > curSteps)
        getPattern().setProperty (IDs::lengthSteps, needSteps, nullptr);

    juce::Array<juce::ValueTree> newSel;
    for (auto& n : src)
    {
        juce::ValueTree copy (IDs::Note);
        copy.setProperty (IDs::key, (int) n.getProperty (IDs::key), nullptr);
        copy.setProperty (IDs::start, (int) n.getProperty (IDs::start) + shift, nullptr);
        copy.setProperty (IDs::length, (int) n.getProperty (IDs::length), nullptr);
        copy.setProperty (IDs::velocity, (int) n.getProperty (IDs::velocity, 100), nullptr);
        seq.appendChild (copy, nullptr);
        newSel.add (copy);
    }
    selection = newSel;
    grid->updateSize();
    grid->repaint();
    velocityLane->repaint();
    jumpPlaybackToLocalTick (minStart + shift);
}

void PianoRoll::jumpPlaybackToLocalTick (int tick)
{
    if (! engine.isPlaying())
        return;
    double local = playheadTicks();
    if (local < 0.0)
        return;
    engine.seekTicks (engine.getPositionTicks() - local + tick);
}

void PianoRoll::copySelection()
{
    if (selection.isEmpty())
        return;

    int minStart = INT_MAX;
    for (auto& n : selection)
        minStart = juce::jmin (minStart, (int) n.getProperty (IDs::start));
    int origin = (minStart / Model::ticksPerBar) * Model::ticksPerBar;

    clipboard.clear();
    clipboardOrigin = origin;
    for (auto& n : selection)
        clipboard.push_back ({ (int) n.getProperty (IDs::key),
                               (int) n.getProperty (IDs::start) - origin,
                               juce::jmax (1, (int) n.getProperty (IDs::length)),
                               (int) n.getProperty (IDs::velocity, 100),
                               (bool) n.getProperty (IDs::slide, false) });
}

void PianoRoll::pasteClipboard()
{
    if (clipboard.empty())
        return;
    auto seq = getChannelSeq (true);
    if (! seq.isValid())
        return;

    seq.setProperty (IDs::usePianoRoll, true, nullptr);

    double anchor = (! engine.isPlaying() && returnOnStopTick >= 0.0)
                        ? returnOnStopTick
                        : engine.getPositionTicks();
    anchor = std::fmod (anchor, (double) juce::jmax (1, getPatternTicks()));
    int pasteOrigin = ((int) anchor / Model::ticksPerBar) * Model::ticksPerBar;

    juce::Array<juce::ValueTree> newSel;
    int maxEnd = 0;
    for (auto& c : clipboard)
    {
        int start = pasteOrigin + c.start;
        juce::ValueTree n (IDs::Note);
        n.setProperty (IDs::key, c.key, nullptr);
        n.setProperty (IDs::start, start, nullptr);
        n.setProperty (IDs::length, c.length, nullptr);
        n.setProperty (IDs::velocity, c.velocity, nullptr);
        if (c.slide)
            n.setProperty (IDs::slide, true, nullptr);
        seq.appendChild (n, nullptr);
        newSel.add (n);
        maxEnd = juce::jmax (maxEnd, start + c.length);
    }

    ensurePatternCovers (maxEnd - 1);
    selection = newSel;
    grid->updateSize();
    grid->repaint();
    velocityLane->repaint();
}

void PianoRoll::transposeSelection (int semitones)
{
    auto seq = getChannelSeq (false);
    if (! seq.isValid())
        return;

    juce::Array<juce::ValueTree> src = selection;
    if (src.isEmpty())
        for (int i = 0; i < seq.getNumChildren(); ++i)
            if (seq.getChild (i).hasType (IDs::Note))
                src.add (seq.getChild (i));

    for (auto& n : src)
    {
        int k = juce::jlimit (0, 127, (int) n.getProperty (IDs::key) + semitones);
        n.setProperty (IDs::key, k, nullptr);
    }
    grid->repaint();
}

void PianoRoll::legatoSelection()
{
    auto seq = getChannelSeq (false);
    if (! seq.isValid())
        return;

    juce::Array<juce::ValueTree> src = selection;
    if (src.isEmpty())
        for (int i = 0; i < seq.getNumChildren(); ++i)
            if (seq.getChild (i).hasType (IDs::Note))
                src.add (seq.getChild (i));
    if (src.isEmpty())
        return;

    int latestStart = 0;
    for (auto& n : src)
        latestStart = juce::jmax (latestStart, (int) n.getProperty (IDs::start));
    ensurePatternCovers (latestStart);

    int endTicks = getPatternTicks();

    for (auto& n : src)
    {
        int start = (int) n.getProperty (IDs::start);

        int nextStart = endTicks;
        for (auto& m : src)
        {
            int s = (int) m.getProperty (IDs::start);
            if (s > start && s < nextStart)
                nextStart = s;
        }

        int len = juce::jmax (1, nextStart - start);
        n.setProperty (IDs::length, len, nullptr);
    }

    grid->updateSize();
    grid->repaint();
    velocityLane->repaint();
}

void PianoRoll::randomizeNotesWeirdly()
{
    auto seq = getChannelSeq (false);
    if (! seq.isValid())
        return;

    juce::Array<juce::ValueTree> src = selection;
    if (src.isEmpty())
        for (int i = 0; i < seq.getNumChildren(); ++i)
            if (seq.getChild (i).hasType (IDs::Note))
                src.add (seq.getChild (i));
    if (src.size() < 2)
        return;

    std::sort (src.begin(), src.end(),
               [] (const juce::ValueTree& a, const juce::ValueTree& b)
               { return (int) a.getProperty (IDs::start) < (int) b.getProperty (IDs::start); });

    int firstStart = (int) src.getFirst().getProperty (IDs::start);
    int step = (int) src[1].getProperty (IDs::start) - firstStart;
    if (step <= 0)
        step = juce::jmax (Model::ticksPerStep, getSnapTicks());

    for (int i = 0; i < src.size(); ++i)
    {
        int start = firstStart + i * step;
        src[i].setProperty (IDs::start, start, nullptr);
        src[i].setProperty (IDs::length, step, nullptr);
    }

    int end = firstStart + src.size() * step;
    int curSteps = (int) getPattern().getProperty (IDs::lengthSteps, Model::defaultSteps);
    int needSteps = (end + Model::ticksPerStep - 1) / Model::ticksPerStep;
    if (needSteps > curSteps)
        getPattern().setProperty (IDs::lengthSteps, needSteps, nullptr);

    grid->updateSize();
    grid->repaint();
    velocityLane->repaint();
}

class PianoRoll::ArpDialog : public juce::Component
{
public:
    explicit ArpDialog (PianoRoll& o) : owner (o)
    {
        setWantsKeyboardFocus (true);

        auto setupKnob = [this] (juce::Slider& s)
        {
            s.setSliderStyle (juce::Slider::RotaryVerticalDrag);
            s.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            s.onValueChange = [this] { updateReadouts(); regenerate(); };
            addAndMakeVisible (s);
        };
        auto setupCaption = [this] (juce::Label& l, const juce::String& t, juce::Colour c)
        {
            l.setText (t, juce::dontSendNotification);
            l.setJustificationType (juce::Justification::centred);
            l.setColour (juce::Label::textColourId, c);
            l.setFont (Typography::ui (11.0f));
            addAndMakeVisible (l);
        };

        patternBox.addItem ("Up", 1);
        patternBox.addItem ("Down", 2);
        patternBox.addItem ("Up & down", 3);
        patternBox.addItem ("Repeat", 4);
        patternBox.setSelectedId (1, juce::dontSendNotification);
        patternBox.onChange = [this] { updateReadouts(); regenerate(); };
        addAndMakeVisible (patternBox);
        setupCaption (patternLabel, "Pattern", Colors::textDim);

        setupKnob (timeMulKnob);
        timeMulKnob.setRange (0.0, (double) (timeMulValues.size() - 1), 1.0);
        timeMulKnob.setValue (4.0, juce::dontSendNotification);
        setupCaption (timeMulLabel, "TIME MUL", Colors::text);
        setupCaption (timeMulReadout, "", Colors::accent);

        setupKnob (gateKnob);
        gateKnob.setRange (0.25, 1.0, 0.01);
        gateKnob.setValue (0.9, juce::dontSendNotification);
        setupCaption (gateLabel, "GATE", Colors::text);
        setupCaption (gateReadout, "", Colors::accent);

        setupKnob (velKnob);
        velKnob.setRange (0.0, 2.0, 0.01);
        velKnob.setValue (1.0, juce::dontSendNotification);
        setupCaption (velLabel, "VEL", Colors::text);
        setupCaption (velReadout, "", Colors::accent);

        groupToggle.setButtonText ("Group notes");
        groupToggle.setToggleState (true, juce::dontSendNotification);
        groupToggle.setColour (juce::ToggleButton::textColourId, Colors::text);
        addAndMakeVisible (groupToggle);

        resetBtn.onClick = [this]
        {
            patternBox.setSelectedId (1, juce::dontSendNotification);
            timeMulKnob.setValue (4.0, juce::dontSendNotification);
            gateKnob.setValue (0.9, juce::dontSendNotification);
            velKnob.setValue (1.0, juce::dontSendNotification);
            groupToggle.setToggleState (true, juce::dontSendNotification);
            updateReadouts();
            regenerate();
        };
        addAndMakeVisible (resetBtn);

        cancelBtn.onClick = [this] { if (host != nullptr) host->exitModalState (0); };
        addAndMakeVisible (cancelBtn);

        acceptBtn.onClick = [this] { if (host != nullptr) host->exitModalState (1); };
        addAndMakeVisible (acceptBtn);

        seq = owner.getChannelSeq (true);
        sourceWasSelection = ! owner.selection.isEmpty();

        juce::Array<juce::ValueTree> src = owner.selection;
        if (src.isEmpty() && seq.isValid())
            for (int i = 0; i < seq.getNumChildren(); ++i)
                if (seq.getChild (i).hasType (IDs::Note))
                    src.add (seq.getChild (i));

        for (auto& n : src)
            source.push_back ({ (int) n.getProperty (IDs::key),
                                (int) n.getProperty (IDs::start),
                                juce::jmax (1, (int) n.getProperty (IDs::length)),
                                (int) n.getProperty (IDs::velocity, 100),
                                n.createCopy(), seq.indexOf (n) });

        seqHadUsePR   = seq.hasProperty (IDs::usePianoRoll);
        seqUsePRValue = seq.getProperty (IDs::usePianoRoll);
        auto pat = owner.getPattern();
        patHadLen   = pat.hasProperty (IDs::lengthSteps);
        patLenValue = pat.getProperty (IDs::lengthSteps);

        owner.selection.clearQuick();
        for (auto& s : src)
            if (s.getParent().isValid())
                s.getParent().removeChild (s, nullptr);

        updateReadouts();
        regenerate();
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds();
        g.setColour (Colors::popupBg);
        g.fillRoundedRectangle (b.toFloat(), Metrics::rL);

        g.setColour (Colors::accent);
        g.setFont (Typography::caps (13.0f));
        g.drawText ("ARPEGGIATE", b.reduced (10).removeFromTop (20),
                    juce::Justification::centredLeft);
        g.setColour (Colors::textDim);
        g.setFont (Typography::caps (10.0f));
        g.drawText ("LEVELS", levelsBand, juce::Justification::centredLeft);
    }

    void resized() override
    {
        titleBand = getLocalBounds().removeFromTop (26);

        auto full = getLocalBounds().reduced (10);
        auto r = full;
        r.removeFromTop (20);

        auto patRow = r.removeFromTop (24);
        patternLabel.setBounds (patRow.removeFromLeft (60));
        patternBox.setBounds (patRow.reduced (0, 1));
        r.removeFromTop (10);

        auto knobRow = r.removeFromTop (78);
        layoutKnob (knobRow.removeFromLeft (knobRow.getWidth() / 2),
                    timeMulLabel, timeMulKnob, timeMulReadout);
        layoutKnob (knobRow, gateLabel, gateKnob, gateReadout);

        levelsBand = r.removeFromTop (16);
        auto velRow = r.removeFromTop (78);
        layoutKnob (velRow.removeFromLeft (full.getWidth() / 2), velLabel, velKnob, velReadout);
        groupToggle.setBounds (velRow.withTrimmedTop (26).removeFromTop (24));

        auto btnRow = full.removeFromBottom (24);
        resetBtn.setBounds (btnRow.removeFromLeft (72));
        acceptBtn.setBounds (btnRow.removeFromRight (72));
        cancelBtn.setBounds (btnRow.removeFromRight (72).translated (-6, 0));
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::escapeKey)
        {
            if (host != nullptr) host->exitModalState (0);
            return true;
        }
        return false;
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (titleBand.contains (e.getPosition()))
        {
            dragger.startDraggingComponent (this, e);
            dragging = true;
        }
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragging) return;
        dragger.dragComponent (this, e, nullptr);
        if (auto* p = getParentComponent())
        {
            auto bb = getBounds();
            bb.setX (juce::jlimit (0, juce::jmax (0, p->getWidth()  - getWidth()),  bb.getX()));
            bb.setY (juce::jlimit (0, juce::jmax (0, p->getHeight() - getHeight()), bb.getY()));
            setBounds (bb);
        }
    }
    void mouseUp (const juce::MouseEvent&) override { dragging = false; }

    void accept()
    {
        if (groupToggle.getToggleState())
            owner.selection = applied;
        else
            owner.selection.clearQuick();
        refresh();
    }

    void cancel()
    {
        removeApplied();

        if (seq.isValid())
        {
            if (seqHadUsePR) seq.setProperty (IDs::usePianoRoll, seqUsePRValue, nullptr);
            else             seq.removeProperty (IDs::usePianoRoll, nullptr);
        }
        auto pat = owner.getPattern();
        if (pat.isValid())
        {
            if (patHadLen) pat.setProperty (IDs::lengthSteps, patLenValue, nullptr);
            else           pat.removeProperty (IDs::lengthSteps, nullptr);
        }

        auto ordered = source;
        std::sort (ordered.begin(), ordered.end(),
                   [] (const Snap& a, const Snap& b) { return a.index < b.index; });
        owner.selection.clearQuick();
        for (auto& s : ordered)
        {
            auto copy = s.node.createCopy();
            seq.addChild (copy, juce::jmin (s.index, seq.getNumChildren()), nullptr);
            if (sourceWasSelection)
                owner.selection.add (copy);
        }
        refresh();
    }

private:
    static void layoutKnob (juce::Rectangle<int> area, juce::Label& lab,
                            juce::Slider& knob, juce::Label& readout)
    {
        lab.setBounds (area.removeFromTop (14));
        readout.setBounds (area.removeFromBottom (14));
        knob.setBounds (area.reduced (6, 0));
    }

    static int pingpong (int i, int n)
    {
        if (n <= 1) return 0;
        int period = 2 * n - 2;
        int p = i % period;
        return p < n ? p : period - p;
    }

    void updateReadouts()
    {
        int ti = juce::jlimit (0, (int) timeMulLabels.size() - 1, (int) timeMulKnob.getValue());
        timeMulReadout.setText (juce::String ("x") + timeMulLabels[(size_t) ti], juce::dontSendNotification);
        gateReadout.setText (juce::String (juce::roundToInt (gateKnob.getValue() * 100.0)) + "%", juce::dontSendNotification);
        velReadout.setText (juce::String (juce::roundToInt (velKnob.getValue() * 100.0)) + "%", juce::dontSendNotification);
    }

    void refresh()
    {
        if (owner.grid != nullptr)         owner.grid->updateSize();
        if (owner.grid != nullptr)         owner.grid->repaint();
        if (owner.velocityLane != nullptr) owner.velocityLane->repaint();
    }

    void removeApplied()
    {
        for (auto& n : applied)
            if (n.getParent().isValid())
                n.getParent().removeChild (n, nullptr);
        applied.clearQuick();
    }

    void regenerate()
    {
        if (! seq.isValid())
            return;

        removeApplied();

        std::vector<Snap> notes = source;
        std::sort (notes.begin(), notes.end(),
                   [] (const Snap& a, const Snap& b) { return a.start < b.start; });

        int base = owner.getSnapTicks();
        if (base <= 1)
            base = Model::ticksPerStep;
        const double timeMul   = timeMulValues[(size_t) juce::jlimit (0, (int) timeMulValues.size() - 1, (int) timeMulKnob.getValue())];
        const int    sliceTicks = juce::jmax (4, juce::roundToInt (base * timeMul));
        const double gate      = gateKnob.getValue();
        const double velMul    = velKnob.getValue();
        const int    pat       = patternBox.getSelectedId();
        int lastEnd = 0;

        size_t i = 0;
        while (i < notes.size())
        {
            int gStart = notes[i].start;
            int gEnd   = notes[i].start + notes[i].length;
            size_t j = i + 1;
            while (j < notes.size() && notes[j].start < gEnd)
            {
                gEnd = juce::jmax (gEnd, notes[j].start + notes[j].length);
                ++j;
            }

            std::vector<int> keys, keyVels;
            long velSum = 0; int velCount = 0;
            for (size_t k = i; k < j; ++k)
            {
                if (std::find (keys.begin(), keys.end(), notes[k].key) == keys.end())
                {
                    keys.push_back (notes[k].key);
                    keyVels.push_back (notes[k].velocity);
                }
                velSum += notes[k].velocity;
                ++velCount;
            }
            for (size_t a = 0; a + 1 < keys.size(); ++a)
                for (size_t b = a + 1; b < keys.size(); ++b)
                    if (keys[b] < keys[a])
                    {
                        std::swap (keys[a], keys[b]);
                        std::swap (keyVels[a], keyVels[b]);
                    }
            const int n = (int) keys.size();
            const int avgVel = velCount > 0 ? (int) (velSum / velCount) : 100;

            int sliceIndex = 0;
            for (int t = gStart; t < gEnd; t += sliceTicks, ++sliceIndex)
            {
                const int len = juce::jmax (4, juce::roundToInt (sliceTicks * gate));
                auto emit = [&] (int key, int srcVel)
                {
                    juce::ValueTree note (IDs::Note);
                    note.setProperty (IDs::key, key, nullptr);
                    note.setProperty (IDs::start, t, nullptr);
                    note.setProperty (IDs::length, len, nullptr);
                    note.setProperty (IDs::velocity,
                                      juce::jlimit (1, 127, juce::roundToInt (srcVel * velMul)), nullptr);
                    seq.appendChild (note, nullptr);
                    applied.add (note);
                    lastEnd = juce::jmax (lastEnd, t + len);
                };

                if (pat == 4)
                {
                    for (int k = 0; k < n; ++k)
                        emit (keys[(size_t) k], keyVels[(size_t) k]);
                }
                else
                {
                    int idx = pat == 1 ? sliceIndex % n
                            : pat == 2 ? (n - 1) - (sliceIndex % n)
                                       : pingpong (sliceIndex, n);
                    idx = juce::jlimit (0, n - 1, idx);
                    emit (keys[(size_t) idx],
                          keyVels.empty() ? avgVel : keyVels[(size_t) idx]);
                }
            }

            i = j;
        }

        seq.setProperty (IDs::usePianoRoll, true, nullptr);
        if (lastEnd > 0)
            owner.ensurePatternCovers (lastEnd - 1);

        refresh();
    }

    struct Snap { int key; int start; int length; int velocity; juce::ValueTree node; int index; };

    PianoRoll& owner;
    juce::Component* host = nullptr;
    juce::ValueTree  seq;
    std::vector<Snap> source;
    juce::Array<juce::ValueTree> applied;
    bool sourceWasSelection = false;
    bool seqHadUsePR = false;   juce::var seqUsePRValue;
    bool patHadLen   = false;   juce::var patLenValue;

    juce::ComponentDragger dragger;
    bool dragging = false;

    const std::vector<double>      timeMulValues { 0.25, 1.0 / 3.0, 0.5, 2.0 / 3.0, 1.0, 1.5, 2.0, 3.0, 4.0 };
    const std::vector<juce::String> timeMulLabels { "1/4", "1/3", "1/2", "2/3", "1", "1.5", "2", "3", "4" };

    juce::ComboBox patternBox;
    ValueKnob      timeMulKnob, gateKnob, velKnob;
    juce::Label    patternLabel, timeMulLabel, timeMulReadout,
                   gateLabel, gateReadout, velLabel, velReadout;
    juce::ToggleButton groupToggle;
    juce::TextButton resetBtn { "Reset" }, cancelBtn { "Cancel" }, acceptBtn { "Accept" };
    juce::Rectangle<int> titleBand, levelsBand;

    friend class PianoRoll::ArpBlocker;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ArpDialog)
};

class PianoRoll::ArpBlocker : public juce::Component
{
public:
    explicit ArpBlocker (PianoRoll& o)
    {
        setWantsKeyboardFocus (true);
        dialog = std::make_unique<ArpDialog> (o);
        dialog->host = this;
        dialog->setLookAndFeel (&o.getLookAndFeel());
        dialog->setSize (panelW, panelH);
        addAndMakeVisible (*dialog);
    }

    ArpDialog* getDialog() const noexcept { return dialog.get(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colors::windowBg.withAlpha (0.62f));
    }

    void resized() override
    {
        if (! placed)
        {
            dialog->setBounds (juce::Rectangle<int> (panelW, panelH)
                                   .withCentre (getLocalBounds().getCentre()));
            placed = true;
        }
        else
        {
            auto bb = dialog->getBounds();
            bb.setX (juce::jlimit (0, juce::jmax (0, getWidth()  - panelW), bb.getX()));
            bb.setY (juce::jlimit (0, juce::jmax (0, getHeight() - panelH), bb.getY()));
            dialog->setBounds (bb);
        }
    }

private:
    static constexpr int panelW = 260, panelH = 320;
    std::unique_ptr<ArpDialog> dialog;
    bool placed = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ArpBlocker)
};

void PianoRoll::showArpeggiatorDialog()
{
    bool hasNotes = ! selection.isEmpty();
    if (! hasNotes)
    {
        auto seq = getChannelSeq (false);
        if (seq.isValid())
            for (int i = 0; i < seq.getNumChildren() && ! hasNotes; ++i)
                if (seq.getChild (i).hasType (IDs::Note))
                    hasNotes = true;
    }
    if (! hasNotes)
        return;

    auto* hostComp = getTopLevelComponent();
    if (hostComp == nullptr)
        return;

    if (onBeginUndoTransaction)
        onBeginUndoTransaction();

    auto* blocker = new ArpBlocker (*this);
    hostComp->addAndMakeVisible (blocker);
    blocker->setBounds (hostComp->getLocalBounds());
    blocker->toFront (true);
    if (auto* d = blocker->getDialog())
        d->grabKeyboardFocus();

    juce::Component::SafePointer<ArpBlocker> safe (blocker);
    juce::Component::SafePointer<PianoRoll>  self (this);
    blocker->enterModalState (true,
        juce::ModalCallbackFunction::create (
            [safe, self] (int result) mutable
            {
                if (safe != nullptr)
                    if (auto* d = safe->getDialog())
                    {
                        if (result == 1) d->accept();
                        else             d->cancel();
                    }
                if (self != nullptr && self->onEndUndoTransaction)
                    self->onEndUndoTransaction();
            }),
        true);
}

class PianoRoll::StrumDialog : public juce::Component
{
public:
    explicit StrumDialog (PianoRoll& o) : owner (o)
    {
        setWantsKeyboardFocus (true);

        auto setupKnob = [this] (juce::Slider& s, double lo, double hi, double def)
        {
            s.setSliderStyle (juce::Slider::RotaryVerticalDrag);
            s.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            s.setRange (lo, hi, 0.01);
            s.setValue (def, juce::dontSendNotification);
            s.onValueChange = [this] { updateReadouts(); regenerate(); };
            addAndMakeVisible (s);
        };
        auto setupCaption = [this] (juce::Label& l, const juce::String& t, juce::Colour c)
        {
            l.setText (t, juce::dontSendNotification);
            l.setJustificationType (juce::Justification::centred);
            l.setColour (juce::Label::textColourId, c);
            l.setFont (Typography::ui (11.0f));
            addAndMakeVisible (l);
        };
        auto setupToggle = [this] (juce::ToggleButton& b, const juce::String& t)
        {
            b.setButtonText (t);
            b.setColour (juce::ToggleButton::textColourId, Colors::text);
            b.onClick = [this] { regenerate(); };
            addAndMakeVisible (b);
        };

        setupKnob (startTimeKnob, 0.0, 1.0, 0.5);
        setupCaption (startTimeLabel, "TIME", Colors::text);
        setupCaption (startTimeReadout, "", Colors::accent);
        setupKnob (startTensionKnob, -1.0, 1.0, 0.0);
        setupCaption (startTensionLabel, "TENSION", Colors::text);
        setupCaption (startTensionReadout, "", Colors::accent);

        setupKnob (velKnob, 0.0, 1.0, 0.0);
        setupCaption (velLabel, "VELOCITY", Colors::text);
        setupCaption (velReadout, "", Colors::accent);
        setupKnob (velTensionKnob, -1.0, 1.0, 0.0);
        setupCaption (velTensionLabel, "TENSION", Colors::text);
        setupCaption (velTensionReadout, "", Colors::accent);

        setupToggle (preserveEndToggle, "Preserve end");
        setupToggle (triggerAheadToggle, "Trigger ahead");

        setupKnob (endTimeKnob, 0.0, 1.0, 0.0);
        setupCaption (endTimeLabel, "TIME", Colors::text);
        setupCaption (endTimeReadout, "", Colors::accent);
        setupKnob (endTensionKnob, -1.0, 1.0, 0.0);
        setupCaption (endTensionLabel, "TENSION", Colors::text);
        setupCaption (endTensionReadout, "", Colors::accent);

        setupToggle (alternateToggle, "Alternate direction");

        resetBtn.onClick = [this]
        {
            startTimeKnob.setValue (0.5, juce::dontSendNotification);
            startTensionKnob.setValue (0.0, juce::dontSendNotification);
            velKnob.setValue (0.0, juce::dontSendNotification);
            velTensionKnob.setValue (0.0, juce::dontSendNotification);
            endTimeKnob.setValue (0.0, juce::dontSendNotification);
            endTensionKnob.setValue (0.0, juce::dontSendNotification);
            preserveEndToggle.setToggleState (false, juce::dontSendNotification);
            triggerAheadToggle.setToggleState (false, juce::dontSendNotification);
            alternateToggle.setToggleState (false, juce::dontSendNotification);
            updateReadouts();
            regenerate();
        };
        addAndMakeVisible (resetBtn);

        cancelBtn.onClick = [this] { if (host != nullptr) host->exitModalState (0); };
        addAndMakeVisible (cancelBtn);
        acceptBtn.onClick = [this] { if (host != nullptr) host->exitModalState (1); };
        addAndMakeVisible (acceptBtn);

        seq = owner.getChannelSeq (true);

        juce::Array<juce::ValueTree> src = owner.selection;
        if (src.isEmpty() && seq.isValid())
            for (int i = 0; i < seq.getNumChildren(); ++i)
                if (seq.getChild (i).hasType (IDs::Note))
                    src.add (seq.getChild (i));

        for (auto& n : src)
            source.push_back ({ n,
                                (int) n.getProperty (IDs::key),
                                (int) n.getProperty (IDs::start),
                                juce::jmax (1, (int) n.getProperty (IDs::length)),
                                juce::jlimit (1, 127, (int) n.getProperty (IDs::velocity, 100)) });

        auto pat = owner.getPattern();
        patHadLen   = pat.hasProperty (IDs::lengthSteps);
        patLenValue = pat.getProperty (IDs::lengthSteps);

        updateReadouts();
        regenerate();
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds();
        g.setColour (Colors::popupBg);
        g.fillRoundedRectangle (b.toFloat(), Metrics::rL);

        g.setColour (Colors::accent);
        g.setFont (Typography::caps (13.0f));
        g.drawText ("STRUMIZER", b.reduced (10).removeFromTop (20),
                    juce::Justification::centredLeft);
        g.setColour (Colors::textDim);
        g.setFont (Typography::caps (10.0f));
        g.drawText ("START", startBand, juce::Justification::centredLeft);
        g.drawText ("END",   endBand,   juce::Justification::centredLeft);
    }

    void resized() override
    {
        titleBand = getLocalBounds().removeFromTop (26);

        auto full = getLocalBounds().reduced (10);
        auto btnRow = full.removeFromBottom (24);

        auto r = full;
        r.removeFromTop (20);

        startBand = r.removeFromTop (14);
        auto row1 = r.removeFromTop (74);
        layoutKnob (row1.removeFromLeft (row1.getWidth() / 2), startTimeLabel, startTimeKnob, startTimeReadout);
        layoutKnob (row1, startTensionLabel, startTensionKnob, startTensionReadout);
        auto row2 = r.removeFromTop (74);
        layoutKnob (row2.removeFromLeft (row2.getWidth() / 2), velLabel, velKnob, velReadout);
        layoutKnob (row2, velTensionLabel, velTensionKnob, velTensionReadout);
        auto togRow = r.removeFromTop (22);
        preserveEndToggle.setBounds (togRow.removeFromLeft (togRow.getWidth() / 2));
        triggerAheadToggle.setBounds (togRow);
        r.removeFromTop (6);

        endBand = r.removeFromTop (14);
        auto row3 = r.removeFromTop (74);
        layoutKnob (row3.removeFromLeft (row3.getWidth() / 2), endTimeLabel, endTimeKnob, endTimeReadout);
        layoutKnob (row3, endTensionLabel, endTensionKnob, endTensionReadout);
        r.removeFromTop (4);
        alternateToggle.setBounds (r.removeFromTop (22));

        resetBtn.setBounds (btnRow.removeFromLeft (72));
        acceptBtn.setBounds (btnRow.removeFromRight (72));
        cancelBtn.setBounds (btnRow.removeFromRight (72).translated (-6, 0));
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::escapeKey)
        {
            if (host != nullptr) host->exitModalState (0);
            return true;
        }
        return false;
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (titleBand.contains (e.getPosition()))
        {
            dragger.startDraggingComponent (this, e);
            dragging = true;
        }
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragging) return;
        dragger.dragComponent (this, e, nullptr);
        if (auto* p = getParentComponent())
        {
            auto bb = getBounds();
            bb.setX (juce::jlimit (0, juce::jmax (0, p->getWidth()  - getWidth()),  bb.getX()));
            bb.setY (juce::jlimit (0, juce::jmax (0, p->getHeight() - getHeight()), bb.getY()));
            setBounds (bb);
        }
    }
    void mouseUp (const juce::MouseEvent&) override { dragging = false; }

    void accept()
    {
        refresh();
    }

    void cancel()
    {
        for (auto& s : source)
            if (s.node.isValid())
            {
                s.node.setProperty (IDs::start,    s.start,    nullptr);
                s.node.setProperty (IDs::length,   s.length,   nullptr);
                s.node.setProperty (IDs::velocity, s.velocity, nullptr);
            }

        auto pat = owner.getPattern();
        if (pat.isValid())
        {
            if (patHadLen) pat.setProperty (IDs::lengthSteps, patLenValue, nullptr);
            else           pat.removeProperty (IDs::lengthSteps, nullptr);
        }
        refresh();
    }

private:
    static void layoutKnob (juce::Rectangle<int> area, juce::Label& lab,
                            juce::Slider& knob, juce::Label& readout)
    {
        lab.setBounds (area.removeFromTop (14));
        readout.setBounds (area.removeFromBottom (14));
        knob.setBounds (area.reduced (6, 0));
    }

    static double shape (double x, double t)
    {
        x = juce::jlimit (0.0, 1.0, x);
        return t >= 0.0 ? std::pow (x, 1.0 + 3.0 * t)
                        : 1.0 - std::pow (1.0 - x, 1.0 - 3.0 * t);
    }

    void updateReadouts()
    {
        auto pct  = [] (double v) { return juce::String (juce::roundToInt (v * 100.0)) + "%"; };
        auto tens = [] (double v) { return juce::String (v, 2); };
        startTimeReadout.setText (pct (startTimeKnob.getValue()), juce::dontSendNotification);
        startTensionReadout.setText (tens (startTensionKnob.getValue()), juce::dontSendNotification);
        velReadout.setText (pct (velKnob.getValue()), juce::dontSendNotification);
        velTensionReadout.setText (tens (velTensionKnob.getValue()), juce::dontSendNotification);
        endTimeReadout.setText (pct (endTimeKnob.getValue()), juce::dontSendNotification);
        endTensionReadout.setText (tens (endTensionKnob.getValue()), juce::dontSendNotification);
    }

    void refresh()
    {
        if (owner.grid != nullptr)         owner.grid->updateSize();
        if (owner.grid != nullptr)         owner.grid->repaint();
        if (owner.velocityLane != nullptr) owner.velocityLane->repaint();
    }

    void regenerate()
    {
        if (! seq.isValid())
            return;

        const double startStrength = startTimeKnob.getValue();
        const double startTension  = startTensionKnob.getValue();
        const double velStrength   = velKnob.getValue();
        const double velTension    = velTensionKnob.getValue();
        const double endStrength   = endTimeKnob.getValue();
        const double endTension    = endTensionKnob.getValue();
        const bool   preserveEnd   = preserveEndToggle.getToggleState();
        const bool   triggerAhead  = triggerAheadToggle.getToggleState();
        const bool   alternate     = alternateToggle.getToggleState();
        const double PPQ           = (double) Model::PPQ;

        std::vector<Snap*> ptrs;
        ptrs.reserve (source.size());
        for (auto& s : source) ptrs.push_back (&s);
        std::sort (ptrs.begin(), ptrs.end(), [] (const Snap* a, const Snap* b)
        {
            if (a->start != b->start) return a->start < b->start;
            return a->key < b->key;
        });

        int maxEnd = 0;
        size_t i = 0;
        int chordIndex = 0;
        while (i < ptrs.size())
        {
            size_t j = i + 1;
            while (j < ptrs.size() && ptrs[j]->start == ptrs[i]->start) ++j;

            std::vector<Snap*> chord (ptrs.begin() + (long) i, ptrs.begin() + (long) j);
            if (alternate && (chordIndex % 2 == 1))
                std::reverse (chord.begin(), chord.end());

            const int n = (int) chord.size();

            std::vector<int> offs ((size_t) n, 0);
            if (n > 1 && startStrength > 0.0)
            {
                const double T = startStrength * startStrength * PPQ;
                int maxOff = 0;
                for (int k = 0; k < n; ++k)
                {
                    double x = (double) k / (double) (n - 1);
                    offs[(size_t) k] = juce::roundToInt (T * shape (x, startTension));
                    maxOff = juce::jmax (maxOff, offs[(size_t) k]);
                }
                if (triggerAhead)
                    for (int k = 0; k < n; ++k) offs[(size_t) k] -= maxOff;
            }

            for (int k = 0; k < n; ++k)
            {
                Snap* s = chord[(size_t) k];
                int newStart = s->start;
                int newLen   = s->length;
                int newVel   = s->velocity;

                if (n > 1)
                {
                    const double x = (double) k / (double) (n - 1);

                    if (startStrength > 0.0)
                    {
                        newStart = s->start + offs[(size_t) k];
                        if (preserveEnd)
                            newLen = juce::jmax (1, s->length - offs[(size_t) k]);
                        if (newStart < 0) newStart = 0;
                    }
                    if (velStrength > 0.0)
                        newVel = juce::jlimit (1, 127,
                            juce::roundToInt (s->velocity * (1.0 - velStrength * shape (x, velTension))));
                    if (endStrength > 0.0)
                    {
                        const double E = endStrength * endStrength * PPQ;
                        newLen += juce::roundToInt (E * shape (x, endTension));
                    }
                }
                newLen = juce::jmax (1, newLen);

                s->node.setProperty (IDs::start,    newStart, nullptr);
                s->node.setProperty (IDs::length,   newLen,   nullptr);
                s->node.setProperty (IDs::velocity, newVel,   nullptr);
                maxEnd = juce::jmax (maxEnd, newStart + newLen);
            }

            i = j;
            ++chordIndex;
        }

        if (maxEnd > 0)
            owner.ensurePatternCovers (maxEnd - 1);
        refresh();
    }

    struct Snap { juce::ValueTree node; int key; int start; int length; int velocity; };

    PianoRoll& owner;
    juce::Component* host = nullptr;
    juce::ValueTree  seq;
    std::vector<Snap> source;
    bool patHadLen = false;   juce::var patLenValue;

    juce::ComponentDragger dragger;
    bool dragging = false;

    ValueKnob startTimeKnob, startTensionKnob, velKnob, velTensionKnob, endTimeKnob, endTensionKnob;
    juce::Label  startTimeLabel, startTimeReadout, startTensionLabel, startTensionReadout,
                 velLabel, velReadout, velTensionLabel, velTensionReadout,
                 endTimeLabel, endTimeReadout, endTensionLabel, endTensionReadout;
    juce::ToggleButton preserveEndToggle, triggerAheadToggle, alternateToggle;
    juce::TextButton resetBtn { "Reset" }, cancelBtn { "Cancel" }, acceptBtn { "Accept" };
    juce::Rectangle<int> titleBand, startBand, endBand;

    friend class PianoRoll::StrumBlocker;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StrumDialog)
};

class PianoRoll::StrumBlocker : public juce::Component
{
public:
    explicit StrumBlocker (PianoRoll& o)
    {
        setWantsKeyboardFocus (true);
        dialog = std::make_unique<StrumDialog> (o);
        dialog->host = this;
        dialog->setLookAndFeel (&o.getLookAndFeel());
        dialog->setSize (panelW, panelH);
        addAndMakeVisible (*dialog);
    }

    StrumDialog* getDialog() const noexcept { return dialog.get(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colors::windowBg.withAlpha (0.62f));
    }

    void resized() override
    {
        if (! placed)
        {
            dialog->setBounds (juce::Rectangle<int> (panelW, panelH)
                                   .withCentre (getLocalBounds().getCentre()));
            placed = true;
        }
        else
        {
            auto bb = dialog->getBounds();
            bb.setX (juce::jlimit (0, juce::jmax (0, getWidth()  - panelW), bb.getX()));
            bb.setY (juce::jlimit (0, juce::jmax (0, getHeight() - panelH), bb.getY()));
            dialog->setBounds (bb);
        }
    }

private:
    static constexpr int panelW = 300, panelH = 420;
    std::unique_ptr<StrumDialog> dialog;
    bool placed = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StrumBlocker)
};

void PianoRoll::showStrumizerDialog()
{
    bool hasNotes = ! selection.isEmpty();
    if (! hasNotes)
    {
        auto seq = getChannelSeq (false);
        if (seq.isValid())
            for (int i = 0; i < seq.getNumChildren() && ! hasNotes; ++i)
                if (seq.getChild (i).hasType (IDs::Note))
                    hasNotes = true;
    }
    if (! hasNotes)
        return;

    auto* hostComp = getTopLevelComponent();
    if (hostComp == nullptr)
        return;

    if (onBeginUndoTransaction)
        onBeginUndoTransaction();

    auto* blocker = new StrumBlocker (*this);
    hostComp->addAndMakeVisible (blocker);
    blocker->setBounds (hostComp->getLocalBounds());
    blocker->toFront (true);
    if (auto* d = blocker->getDialog())
        d->grabKeyboardFocus();

    juce::Component::SafePointer<StrumBlocker> safe (blocker);
    juce::Component::SafePointer<PianoRoll>    self (this);
    blocker->enterModalState (true,
        juce::ModalCallbackFunction::create (
            [safe, self] (int result) mutable
            {
                if (safe != nullptr)
                    if (auto* d = safe->getDialog())
                    {
                        if (result == 1) d->accept();
                        else             d->cancel();
                    }
                if (self != nullptr && self->onEndUndoTransaction)
                    self->onEndUndoTransaction();
            }),
        true);
}

void PianoRoll::exportMidi()
{
    auto seq = getChannelSeq (false);
    juce::MidiMessageSequence track;
    auto addNotes = [&track] (const juce::ValueTree& s, int channel1to16)
    {
        for (int i = 0; i < s.getNumChildren(); ++i)
        {
            auto n = s.getChild (i);
            if (! n.hasType (IDs::Note))
                continue;
            int key = juce::jlimit (0, 127, (int) n.getProperty (IDs::key));
            int vel = juce::jlimit (1, 127, (int) n.getProperty (IDs::velocity, 100));
            double start = (double) (int) n.getProperty (IDs::start);
            double len = (double) juce::jmax (1, (int) n.getProperty (IDs::length));
            track.addEvent (juce::MidiMessage::noteOn (channel1to16, key, (juce::uint8) vel), start);
            track.addEvent (juce::MidiMessage::noteOff (channel1to16, key), start + len);
        }
    };

    if (seq.isValid())
        addNotes (seq, 1);

    if (track.getNumEvents() == 0)
    {
        auto pattern = getPattern();
        int ch = 1;
        for (int i = 0; i < pattern.getNumChildren(); ++i)
        {
            auto s = pattern.getChild (i);
            if (s.hasType (IDs::ChannelSeq))
                addNotes (s, juce::jlimit (1, 16, ch++));
        }
    }

    if (track.getNumEvents() == 0)
    {
        Dialogs::showInfoAsync (juce::MessageBoxIconType::InfoIcon,
            "Export MIDI", "This pattern has no piano-roll notes to export.");
        return;
    }

    track.updateMatchedPairs();

    auto defaultName = getPattern().getProperty (IDs::name).toString();
    if (defaultName.isEmpty())
        defaultName = "pattern";

    chooser = std::make_unique<juce::FileChooser> ("Export MIDI",
        juce::File::getSpecialLocation (juce::File::userMusicDirectory)
            .getChildFile (defaultName + ".mid"), "*.mid");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode
                          | juce::FileBrowserComponent::canSelectFiles
                          | juce::FileBrowserComponent::warnAboutOverwriting,
        [track] (const juce::FileChooser& fc)
        {
            auto f = fc.getResult();
            if (f == juce::File())
                return;
            if (! f.hasFileExtension ("mid"))
                f = f.withFileExtension ("mid");

            juce::MidiFile mf;
            mf.setTicksPerQuarterNote (Model::PPQ);
            mf.addTrack (track);

            f.deleteFile();
            juce::FileOutputStream os (f);
            if (os.openedOk())
                mf.writeTo (os);
            else
                Dialogs::showInfoAsync (juce::MessageBoxIconType::WarningIcon,
                    "Export failed", "Could not write " + f.getFullPathName());
        });
}

void PianoRoll::refreshTargetBoxes()
{
    auto pattern = getPattern();
    auto channelHasNotes = [&pattern] (int cid)
    {
        if (! pattern.isValid())
            return false;
        auto seq = pattern.getChildWithProperty (IDs::channelId, cid);
        for (int i = 0; i < seq.getNumChildren(); ++i)
            if (seq.getChild (i).hasType (IDs::Note))
                return true;
        if (seq.getProperty (IDs::steps).toString().containsChar ('1'))
            return true;
        return false;
    };

    channelBox.clear (juce::dontSendNotification);
    auto channels = Model::getChannels (project);
    for (int i = 0; i < channels.getNumChildren(); ++i)
    {
        auto c = channels.getChild (i);
        if (! c.hasType (IDs::Channel))
            continue;
        int id = (int) c.getProperty (IDs::id);
        juce::String label = c.getProperty (IDs::name).toString();
        if (channelHasNotes (id))
            label = juce::String (juce::CharPointer_UTF8 ("\xe2\x9c\x93 ")) + label;
        channelBox.addItem (label, id);
    }
    if (Model::findChannelById (project, channelId).isValid())
        channelBox.setSelectedId (channelId, juce::dontSendNotification);
}

void PianoRoll::refreshScaleBoxes()
{
    rootBox.setSelectedId (Scales::projectRoot (project) + 1, juce::dontSendNotification);

    auto typeId = Scales::projectTypeId (project);
    const auto& defs = Scales::all();
    for (int i = 0; i < (int) defs.size(); ++i)
        if (typeId == defs[(size_t) i].id)
        {
            scaleBox.setSelectedId (i + 1, juce::dontSendNotification);
            break;
        }
}

void PianoRoll::setVerticalZoom (int newRowHeight, int anchorGridY, int anchorViewportY)
{
    newRowHeight = juce::jlimit (6, 60, newRowHeight);
    if (newRowHeight == rowHeight)
        return;

    double keyPos = anchorGridY / (double) rowHeight;
    rowHeight = newRowHeight;

    grid->updateSize();
    keyboard->setSize (keyboardWidth, numKeys * rowHeight);
    resized();

    viewport->setViewPosition (viewport->getViewPositionX(),
                               juce::jmax (0, (int) std::lround (keyPos * rowHeight) - anchorViewportY));
    keyboard->repaint();
    grid->repaint();
}

void PianoRoll::setHZoom (double newPxPerTick, double anchorTick, int anchorViewX)
{
    {
        const juce::ScopedValueSetter<bool> guard (suppressZoomBarSync, true);
        pxPerTick = juce::jlimit (minPxPerTick, maxPxPerTick, newPxPerTick);
        grid->updateSize();
        layoutForGridWidth();
        viewport->setViewPosition (
            juce::jmax (0, (int) std::lround (anchorTick * pxPerTick) - anchorViewX),
            viewport->getViewPositionY());
    }
    syncZoomBar();
    grid->repaint();
    ruler->resetPlayhead();
    ruler->repaint();
    velocityLane->repaint();
}

void PianoRoll::layoutForGridWidth()
{
    ruler->setSize (juce::jmax (grid->getWidth(), rulerClip.getWidth()), rulerHeight);
    velocityLane->setSize (juce::jmax (grid->getWidth(), laneClip.getWidth()), laneHeight);
}

juce::String PianoRoll::getTargetDescription() const
{
    auto pattern = getPattern();
    auto channel = Model::findChannelById (project, channelId);
    juce::String desc;
    if (pattern.isValid())
        desc << " - " << pattern.getProperty (IDs::name).toString();
    if (channel.isValid())
        desc << " - " << channel.getProperty (IDs::name).toString();
    return desc;
}

juce::ValueTree PianoRoll::getPattern() const
{
    return Model::findPatternById (project, patternId);
}

juce::ValueTree PianoRoll::getChannelSeq (bool createIfMissing) const
{
    auto pattern = getPattern();
    if (! pattern.isValid())
        return {};
    return Model::getChannelSeq (pattern, channelId, createIfMissing);
}

int PianoRoll::getPatternTicks() const
{
    auto pattern = getPattern();
    if (! pattern.isValid())
        return Model::PPQ * 4;
    auto contentEnd = Model::patternContentEndTicks (pattern);
    if (contentEnd > 0)
        return (int) (((contentEnd + Model::ticksPerBar - 1) / Model::ticksPerBar) * Model::ticksPerBar);
    return juce::jmin (Model::patternLengthTicks (pattern), Model::ticksPerBar);
}

double PianoRoll::playheadTicks() const
{
    if (! engine.isPlaying())
        return -1.0;

    const double patTicks = (double) juce::jmax (1, getPatternTicks());

    if (! engine.isSongMode())
    {
        if (engine.getCurrentPatternId() != patternId)
            return -1.0;
        return std::fmod (engine.getPositionTicks(), patTicks);
    }

    auto playlist = Model::getPlaylist (project);
    if (! playlist.isValid())
        return -1.0;

    const double posBeats = engine.getPositionTicks() / (double) Model::PPQ;
    for (int i = 0; i < playlist.getNumChildren(); ++i)
    {
        auto clip = playlist.getChild (i);
        if (! clip.hasType (IDs::Clip))
            continue;
        if (clip.getProperty (IDs::type).toString() != "pattern")
            continue;
        if ((int) clip.getProperty (IDs::patternId) != patternId)
            continue;

        const double startB = (double) clip.getProperty (IDs::startBeats);
        const double lenB   = (double) clip.getProperty (IDs::lengthBeats);
        if (posBeats < startB || posBeats >= startB + lenB)
            continue;

        const double offB = (double) clip.getProperty (IDs::offsetBeats, 0.0);
        const double localTicks = (posBeats - startB + offB) * (double) Model::PPQ;
        return (localTicks >= 0.0 && localTicks < patTicks) ? localTicks : -1.0;
    }
    return -1.0;
}

int PianoRoll::getGridTicks() const
{
    auto pattern = getPattern();
    int lenTicks = pattern.isValid() ? Model::patternLengthTicks (pattern) : 0;
    int extent = juce::jmax (getPatternTicks(), lenTicks);
    int bars = juce::jmax (minGridBars,
                           (extent + Model::ticksPerBar - 1) / Model::ticksPerBar);
    return bars * Model::ticksPerBar;
}

void PianoRoll::ensurePatternCovers (int tick)
{
    auto pattern = getPattern();
    if (! pattern.isValid() || tick < getPatternTicks())
        return;
    int needBars  = tick / Model::ticksPerBar + 1;
    int needSteps = needBars * Model::ticksPerBar / Model::ticksPerStep;
    if (needSteps > (int) pattern.getProperty (IDs::lengthSteps, Model::defaultSteps))
        pattern.setProperty (IDs::lengthSteps, needSteps, nullptr);
}

int PianoRoll::getSnapTicks() const
{
    switch (snapBox.getSelectedId())
    {
        case 1:  return 1;
        case 2:  return Model::ticksPerStep / 6;
        case 3:  return Model::ticksPerStep / 4;
        case 4:  return Model::ticksPerStep / 3;
        case 5:  return Model::ticksPerStep / 2;
        case 6:  return Model::ticksPerStep;
        case 7:  return Model::PPQ / 6;
        case 8:  return Model::PPQ / 4;
        case 9:  return Model::PPQ / 3;
        case 10: return Model::PPQ / 2;
        case 11: return Model::PPQ;
        case 12: return Model::ticksPerBar;
        default: return Model::ticksPerStep;
    }
}

void PianoRoll::gridRepaint()      { grid->repaint(); }
void PianoRoll::velocityLaneRepaint() { velocityLane->repaint(); }

bool PianoRoll::isInterestedInDragSource (const SourceDetails& d)
{
    if (d.description.toString() != BrowserPanel::dragDescription)
        return false;
    if (getBrowserFile == nullptr)
        return false;
    return BrowserPanel::isMidiFile (getBrowserFile());
}

void PianoRoll::itemDropped (const SourceDetails&)
{
    if (getBrowserFile == nullptr)
        return;
    auto f = getBrowserFile();
    if (! BrowserPanel::isMidiFile (f))
        return;
    importMidiFile (f);
}

bool PianoRoll::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& path : files)
        if (BrowserPanel::isMidiFile (juce::File (path)))
            return true;
    return false;
}

void PianoRoll::filesDropped (const juce::StringArray& files, int, int)
{
    for (auto& path : files)
    {
        juce::File f (path);
        if (BrowserPanel::isMidiFile (f))
        {
            importMidiFile (f);
            return;
        }
    }
}

void PianoRoll::importMidiFile (const juce::File& file)
{
    auto pattern = getPattern();
    if (! pattern.isValid())
        return;
    auto seq = getChannelSeq (true);
    if (! seq.isValid())
        return;

    for (int i = seq.getNumChildren(); --i >= 0;)
        if (seq.getChild (i).hasType (IDs::Note))
            seq.removeChild (i, nullptr);

    selection.clearQuick();
    if (MidiImport::applyToChannelSeq (file, pattern, seq) > 0)
        Model::addMidiResource (project, file);
    grid->updateSize();
    grid->repaint();
    velocityLane->repaint();
}

void PianoRoll::syncScroll()
{
    keyboard->setTopLeftPosition (0, -viewport->getViewPositionY());
    velocityLane->setTopLeftPosition (-viewport->getViewPositionX(), 0);
    ruler->setTopLeftPosition (-viewport->getViewPositionX(), 0);
    syncZoomBar();
}

void PianoRoll::syncZoomBar()
{
    if (suppressZoomBarSync)
        return;

    double visibleW = viewport->getMaximumVisibleWidth();
    double total = juce::jmax ((double) getGridTicks(), visibleW / pxPerTick);
    double viewStartTick = viewport->getViewPositionX() / pxPerTick;
    double viewEndTick = (viewport->getViewPositionX() + visibleW) / pxPerTick;
    zoomBar.setMaxSpan (visibleW / minPxPerTick);
    zoomBar.setMinSpan (visibleW / maxPxPerTick);
    zoomBar.setRange (total, viewStartTick, viewEndTick);
}

namespace
{
    struct ScopedTransportBroadcastMute
    {
        explicit ScopedTransportBroadcastMute (AudioEngine& e)
            : engine (e), prev (e.suppressTransportBroadcast.exchange (true)) {}
        ~ScopedTransportBroadcastMute() { engine.suppressTransportBroadcast.store (prev); }
        AudioEngine& engine;
        bool prev;
    };
}

void PianoRoll::timerCallback()
{
    bool playing = engine.isPlaying();

    if (wasPlayingForReturn && ! playing && returnOnStopTick >= 0.0 && isShowing())
    {
        const ScopedTransportBroadcastMute mute (engine);
        engine.seekTicks (returnOnStopTick);
        returnOnStopTick = -1.0;
    }
    bool stopped = wasPlayingForReturn && ! playing;
    wasPlayingForReturn = playing;

    bool active[numKeys] = {};
    const double posTicks = playheadTicks();
    if (posTicks >= 0.0)
    {
        auto seq = getChannelSeq (false);
        if (seq.isValid())
        {
            for (int i = 0; i < seq.getNumChildren(); ++i)
            {
                auto note = seq.getChild (i);
                if (! note.hasType (IDs::Note) || (bool) note.getProperty (IDs::slide, false))
                    continue;
                int start = (int) note.getProperty (IDs::start);
                int len = juce::jmax (1, (int) note.getProperty (IDs::length));
                if (posTicks < start || posTicks >= start + len)
                    continue;
                int key = (int) note.getProperty (IDs::key);
                if (key >= 0 && key < numKeys)
                    active[key] = true;
            }
        }
    }
    keyboard->setPlayingKeys (active);

    bool wantFast = playing || grid->hasGlowActivity() || keyboard->hasActiveGlow();
    if (wantFast && getTimerInterval() != 1000 / 60)
        startTimerHz (60);
    else if (! wantFast && getTimerInterval() != 1000 / 30)
        startTimerHz (30);

    if (playing)
    {
        grid->repaintPlayhead();
        ruler->repaintPlayhead();
        grid->animateGlow();
    }
    else if (stopped)
    {
        grid->resetPlayhead();
        ruler->resetPlayhead();
        grid->animateGlow();
        grid->repaint();
        ruler->repaint();
    }
    else if (grid->hasGlowActivity())
    {
        grid->animateGlow();
    }
}

void PianoRoll::valueTreePropertyChanged (juce::ValueTree& t, const juce::Identifier& p)
{
    if (t.hasType (IDs::Note) || t.hasType (IDs::Pattern))
    {
        grid->repaint();
        velocityLane->repaint();
    }
    if (p == IDs::name && (t.hasType (IDs::Pattern) || t.hasType (IDs::Channel)))
        refreshTargetBoxes();
    if (t.hasType (IDs::Project) && (p == IDs::scaleRoot || p == IDs::scaleType))
    {
        refreshScaleBoxes();
        grid->repaint();
    }
}

void PianoRoll::valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& child)
{
    if (child.hasType (IDs::Note) || child.hasType (IDs::ChannelSeq) || child.hasType (IDs::Pattern))
    {
        grid->repaint();
        velocityLane->repaint();
    }
    if (child.hasType (IDs::Pattern) || child.hasType (IDs::Channel))
        refreshTargetBoxes();
}

void PianoRoll::valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& child, int index)
{
    if (child.hasType (IDs::Channel) && (int) child.getProperty (IDs::id) == channelId)
    {
        auto channels = Model::getChannels (project);
        int nearby = -1;
        if (channels.getNumChildren() > 0)
        {
            int i = juce::jlimit (0, channels.getNumChildren() - 1, index);
            nearby = (int) channels.getChild (i).getProperty (IDs::id);
        }
        juce::Component::SafePointer<PianoRoll> self (this);
        juce::MessageManager::callAsync ([self, nearby] { if (self) self->setTarget (self->patternId, nearby); });
    }

    if (child.hasType (IDs::Pattern) && (int) child.getProperty (IDs::id) == patternId)
    {
        auto patterns = Model::getPatterns (project);
        int nearby = -1;
        if (patterns.getNumChildren() > 0)
        {
            int i = juce::jlimit (0, patterns.getNumChildren() - 1, index);
            nearby = (int) patterns.getChild (i).getProperty (IDs::id);
        }
        juce::Component::SafePointer<PianoRoll> self (this);
        juce::MessageManager::callAsync ([self, nearby] { if (self) self->setTarget (nearby, self->channelId); });
    }

    valueTreeChildAdded (project, child);
}

void PianoRoll::paint (juce::Graphics& g)
{
    g.fillAll (Colors::panelBg);
}

void PianoRoll::resized()
{
    auto r = getLocalBounds().reduced (4);

    auto top = r.removeFromTop (toolbarHeight);
    menuBtn.setBounds (top.removeFromLeft (22).reduced (1));
    top.removeFromLeft (2);
    snapBtn.setBounds (top.removeFromLeft (toolbarHeight).reduced (1));
    top.removeFromLeft (3);
    drawBtn.setBounds (top.removeFromLeft (toolbarHeight).reduced (1));
    top.removeFromLeft (1);
    selectBtn.setBounds (top.removeFromLeft (toolbarHeight).reduced (1));
    top.removeFromLeft (1);
    sliceBtn.setBounds (top.removeFromLeft (toolbarHeight).reduced (1));
    top.removeFromLeft (1);
    slideBtn.setBounds (top.removeFromLeft (toolbarHeight).reduced (1));
    top.removeFromLeft (10);
    channelBox.setBounds (top.removeFromLeft (160).reduced (0, 2));
    top.removeFromLeft (10);
    rootBox.setBounds (top.removeFromLeft (52).reduced (0, 2));
    top.removeFromLeft (4);
    scaleBox.setBounds (top.removeFromLeft (130).reduced (0, 2));
    top.removeFromLeft (4);
    detectBtn.setBounds (top.removeFromLeft (58).reduced (0, 2));
    top.removeFromLeft (4);
    scaleHiliteBtn.setBounds (top.removeFromLeft (48).reduced (0, 2));

    auto lane = r.removeFromBottom (laneHeight);
    lane.removeFromLeft (keyboardWidth);
    laneClip.setBounds (lane);
    laneDivider->setBounds (r.removeFromBottom (dividerHeight));

    auto zoomRow = r.removeFromTop (zoomBarHeight);
    zoomRow.removeFromLeft (keyboardWidth);
    zoomBar.setBounds (zoomRow);

    auto rulerRow = r.removeFromTop (rulerHeight);
    rulerRow.removeFromLeft (keyboardWidth);
    rulerClip.setBounds (rulerRow);

    auto keys = r.removeFromLeft (keyboardWidth);
    keyboardClip.setBounds (keys);

    viewport->setBounds (r);

    grid->updateSize();
    keyboard->setSize (keyboardWidth, numKeys * rowHeight);
    layoutForGridWidth();
    syncScroll();
}
