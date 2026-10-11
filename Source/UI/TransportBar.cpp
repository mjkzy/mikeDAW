#include "TransportBar.h"
#include "AppLookAndFeel.h"
#include "AppSettings.h"

namespace
{
    void paintTransportSurface (juce::Graphics& g, juce::Rectangle<float> bounds,
                                juce::Colour fill = Colors::panelDark)
    {
        auto r = bounds.reduced (0.5f);
        g.setColour (fill.withMultipliedAlpha (0.72f));
        g.fillRoundedRectangle (r, 5.0f);
        g.setColour (Colors::outline.withAlpha (0.42f));
        g.drawRoundedRectangle (r, 5.0f, 0.75f);
    }
}

IconButton::IconButton (const juce::String& name, Icon i, const juce::String& tip)
    : juce::Button (name), activeColour (Colors::accent), icon (i)
{
    setTooltip (tip);
}

void IconButton::setActive (bool a)
{
    if (active != a)
    {
        active = a;
        repaint();
    }
}

void IconButton::paintButton (juce::Graphics& g, bool, bool down)
{
    auto r = getLocalBounds().toFloat();
    auto side = juce::jmin (r.getWidth(), r.getHeight());
    auto square = r.withSizeKeepingCentre (side, side);

    const juce::Colour idle = idleColour.isTransparent() ? Colors::textDim : idleColour;
    const float h = hover.value();

    if (active && isEnabled() && icon == Icon::record && Fx::glowEnabled)
    {
        const float grow[]  { 3.0f, 2.0f, 1.0f };
        const float alpha[] { 0.05f, 0.09f, 0.16f };
        for (int i = 0; i < 3; ++i)
        {
            g.setColour (activeColour.withAlpha (alpha[i]));
            g.fillRoundedRectangle (square.reduced (3.0f).expanded (grow[i]),
                                    Metrics::rM + grow[i]);
        }
    }

    if (pillBackground && isEnabled())
    {
        auto pill = r.reduced (1.0f, 2.0f);
        if (active)
        {
            g.setColour (activeColour.withAlpha (0.16f));
            g.fillRoundedRectangle (pill, 4.0f);
        }
        else if (h > 0.0f || down)
        {
            g.setColour (Colors::text.withAlpha (down ? 0.12f : 0.07f * h));
            g.fillRoundedRectangle (pill, 4.0f);
        }
    }

    juce::Colour fg;
    if (! isEnabled())
        fg = Colors::textDim.withAlpha (0.45f);
    else if (active || down)
        fg = activeColour;
    else
        fg = idle.interpolatedWith (activeColour, 0.55f * h);

    drawIcon (g, square.reduced (iconInset), fg);
}

void MenuTextButton::paintButton (juce::Graphics& g, bool, bool down)
{
    g.setColour (down ? Colors::accent
                      : Colors::textDim.interpolatedWith (Colors::text, hover.value()));
    g.setFont (Typography::caps (13.5f));
    g.drawText (getButtonText(), getLocalBounds(), juce::Justification::centred);
}

void IconButton::drawIcon (juce::Graphics& g, juce::Rectangle<float> a, juce::Colour c) const
{
    g.setColour (c);
    auto cx = a.getCentreX();
    auto cy = a.getCentreY();

    const float unit = juce::jmin (a.getWidth(), a.getHeight()) / 24.0f;
    const auto grid = juce::AffineTransform::scale (unit)
                          .translated (cx - 12.0f * unit, cy - 12.0f * unit);
    const juce::PathStrokeType gridStroke (1.9f * unit, juce::PathStrokeType::curved,
                                           juce::PathStrokeType::rounded);

    switch (icon)
    {
        case Icon::play:
        {
            juce::Path p;
            p.addTriangle (a.getX() + 1, a.getY(), a.getX() + 1, a.getBottom(), a.getRight(), cy);
            g.fillPath (p);
            break;
        }
        case Icon::stop:
            g.fillRect (a.reduced (1.5f));
            break;

        case Icon::record:
            g.fillEllipse (a.reduced (1.5f));
            break;

        case Icon::metronome:
        {
            juce::Path p;
            p.addTriangle (cx - a.getWidth() * 0.42f, a.getBottom(),
                           cx + a.getWidth() * 0.42f, a.getBottom(),
                           cx, a.getY());
            g.strokePath (p, juce::PathStrokeType (1.6f));
            g.drawLine (cx, a.getBottom() - 2.0f, a.getRight() - 1.0f, a.getY() + 2.0f, 1.6f);
            break;
        }
        case Icon::browser:
        {
            auto f = a.reduced (0.5f, 1.5f);
            g.fillRoundedRectangle (f.getX(), f.getY(), f.getWidth() * 0.45f, 3.5f, 1.0f);
            g.fillRoundedRectangle (f.getX(), f.getY() + 2.5f, f.getWidth(), f.getHeight() - 2.5f, 1.5f);
            break;
        }
        case Icon::rack:
        {
            float rowH = a.getHeight() / 3.0f;
            for (int i = 0; i < 3; ++i)
            {
                float y = a.getY() + i * rowH + 1.0f;
                float h = rowH - 2.0f;
                g.fillRect (a.getX(), y, h, h);
                g.fillRoundedRectangle (a.getX() + h + 2.5f, y, a.getWidth() - h - 2.5f, h, 1.0f);
            }
            break;
        }
        case Icon::piano:
        {
            g.drawRoundedRectangle (a, 1.5f, 1.4f);
            float w3 = a.getWidth() / 3.0f;
            g.fillRect (a.getX() + w3 - 1.8f, a.getY(), 3.6f, a.getHeight() * 0.55f);
            g.fillRect (a.getX() + 2 * w3 - 1.8f, a.getY(), 3.6f, a.getHeight() * 0.55f);
            break;
        }
        case Icon::playlist:
        {
            float rowH = a.getHeight() / 3.0f;
            g.fillRoundedRectangle (a.getX(), a.getY() + 0.5f, a.getWidth() * 0.6f, rowH - 2.0f, 1.0f);
            g.fillRoundedRectangle (a.getX() + a.getWidth() * 0.25f, a.getY() + rowH + 0.5f,
                                    a.getWidth() * 0.7f, rowH - 2.0f, 1.0f);
            g.fillRoundedRectangle (a.getX(), a.getY() + 2 * rowH + 0.5f, a.getWidth() * 0.45f, rowH - 2.0f, 1.0f);
            break;
        }
        case Icon::mixer:
        {
            float x1 = a.getX() + a.getWidth() * 0.18f;
            float x2 = cx;
            float x3 = a.getRight() - a.getWidth() * 0.18f;
            for (float x : { x1, x2, x3 })
                g.drawLine (x, a.getY(), x, a.getBottom(), 1.4f);
            g.fillRoundedRectangle (x1 - 2.6f, cy - 1.0f, 5.2f, 3.0f, 1.0f);
            g.fillRoundedRectangle (x2 - 2.6f, a.getY() + 2.0f, 5.2f, 3.0f, 1.0f);
            g.fillRoundedRectangle (x3 - 2.6f, a.getBottom() - 5.0f, 5.2f, 3.0f, 1.0f);
            break;
        }
        case Icon::pencil:
        {
            juce::Path outline;
            outline.startNewSubPath (2.5f, 12.0f);
            outline.lineTo (7.0f, 8.5f);
            outline.lineTo (19.5f, 8.5f);
            outline.quadraticTo (21.5f, 8.5f, 21.5f, 10.5f);
            outline.lineTo (21.5f, 13.5f);
            outline.quadraticTo (21.5f, 15.5f, 19.5f, 15.5f);
            outline.lineTo (7.0f, 15.5f);
            outline.closeSubPath();
            outline.startNewSubPath (7.0f, 8.5f);
            outline.lineTo (7.0f, 15.5f);
            outline.startNewSubPath (17.0f, 8.5f);
            outline.lineTo (17.0f, 15.5f);

            juce::Path lead;
            lead.addTriangle (2.5f, 12.0f, 4.6f, 10.4f, 4.6f, 13.6f);

            const auto t = juce::AffineTransform::rotation (-juce::MathConstants<float>::pi * 0.25f,
                                                            12.0f, 12.0f).followedBy (grid);
            g.strokePath (outline, gridStroke, t);
            g.fillPath (lead, t);
            break;
        }
        case Icon::cursor:
        {
            juce::Path p;
            p.startNewSubPath (6.0f, 2.5f);
            p.lineTo (6.0f, 19.0f);
            p.lineTo (10.0f, 15.3f);
            p.lineTo (12.8f, 21.3f);
            p.lineTo (15.6f, 20.0f);
            p.lineTo (12.8f, 14.2f);
            p.lineTo (18.2f, 14.2f);
            p.closeSubPath();
            g.setColour (c.withMultipliedAlpha (0.28f));
            g.fillPath (p, grid);
            g.setColour (c);
            g.strokePath (p, gridStroke, grid);
            break;
        }
        case Icon::magnet:
        {
            juce::Path body;
            body.startNewSubPath (5.5f, 15.5f);
            body.lineTo (5.5f, 11.0f);
            body.addCentredArc (12.0f, 11.0f, 6.5f, 6.5f, 0.0f,
                                -juce::MathConstants<float>::halfPi,
                                juce::MathConstants<float>::halfPi, false);
            body.lineTo (18.5f, 15.5f);
            g.strokePath (body, juce::PathStrokeType (4.2f * unit, juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::butt), grid);

            juce::Path poles;
            poles.addRoundedRectangle (3.4f, 17.0f, 4.2f, 4.0f, 0.8f);
            poles.addRoundedRectangle (16.4f, 17.0f, 4.2f, 4.0f, 0.8f);
            g.fillPath (poles, grid);
            break;
        }
        case Icon::blade:
        {
            const juce::Point<float> ringA (6.0f, 6.5f), ringB (6.0f, 17.5f);
            const juce::Point<float> tipA (21.0f, 18.0f), tipB (21.0f, 6.0f);
            const float ringR = 3.2f;

            auto edgeToward = [ringR] (juce::Point<float> centre, juce::Point<float> target)
            {
                auto d = target - centre;
                return centre + d * (ringR / d.getDistanceFromOrigin());
            };

            juce::Path p;
            p.addEllipse (ringA.x - ringR, ringA.y - ringR, ringR * 2.0f, ringR * 2.0f);
            p.addEllipse (ringB.x - ringR, ringB.y - ringR, ringR * 2.0f, ringR * 2.0f);
            auto a0 = edgeToward (ringA, tipA);
            auto b0 = edgeToward (ringB, tipB);
            p.startNewSubPath (a0);
            p.lineTo (tipA);
            p.startNewSubPath (b0);
            p.lineTo (tipB);
            g.strokePath (p, gridStroke, grid);

            juce::Path pivot;
            pivot.addEllipse (11.9f, 10.7f, 2.6f, 2.6f);
            g.fillPath (pivot, grid);
            break;
        }
        case Icon::slide:
        {
            auto b = a.reduced (1.5f);
            g.drawLine (b.getX(), b.getBottom(), b.getRight() - 3.0f, b.getY() + 3.0f, 1.8f);
            juce::Path tri;
            tri.addTriangle (b.getRight() - 5.5f, b.getY() + 0.5f,
                             b.getRight(),        b.getY() + 2.5f,
                             b.getRight() - 4.0f, b.getY() + 5.5f);
            g.fillPath (tri);
            break;
        }
        case Icon::stretch:
        {
            juce::Path p;
            p.startNewSubPath (3.0f, 5.0f);
            p.lineTo (3.0f, 19.0f);
            p.startNewSubPath (21.0f, 5.0f);
            p.lineTo (21.0f, 19.0f);
            p.startNewSubPath (7.0f, 12.0f);
            p.lineTo (17.0f, 12.0f);
            p.startNewSubPath (10.0f, 9.0f);
            p.lineTo (7.0f, 12.0f);
            p.lineTo (10.0f, 15.0f);
            p.startNewSubPath (14.0f, 9.0f);
            p.lineTo (17.0f, 12.0f);
            p.lineTo (14.0f, 15.0f);
            g.strokePath (p, gridStroke, grid);
            break;
        }
        case Icon::typingKeys:
        {
            auto keys = a.removeFromTop (a.getHeight() * 0.55f);
            g.drawRoundedRectangle (keys, 1.2f, 1.2f);
            float kw = keys.getWidth() / 3.0f;
            for (int i = 1; i < 3; ++i)
                g.drawLine (keys.getX() + i * kw, keys.getY(), keys.getX() + i * kw, keys.getBottom(), 1.0f);
            g.fillRect (keys.getX() + kw - 1.6f, keys.getY(), 3.2f, keys.getHeight() * 0.55f);
            g.fillRect (keys.getX() + 2 * kw - 1.6f, keys.getY(), 3.2f, keys.getHeight() * 0.55f);
            auto cap = a.reduced (0.5f, 0.0f).withTrimmedTop (1.5f);
            g.drawRoundedRectangle (cap, 1.6f, 1.2f);
            break;
        }
        case Icon::chat:
        {
            auto b = a.reduced (0.5f, 1.0f).withTrimmedBottom (a.getHeight() * 0.18f);
            g.drawRoundedRectangle (b, 3.0f, 1.6f);
            juce::Path tail;
            float tx = b.getX() + b.getWidth() * 0.24f;
            tail.startNewSubPath (tx, b.getBottom() - 1.0f);
            tail.lineTo (tx - 1.0f, b.getBottom() + a.getHeight() * 0.20f);
            tail.lineTo (tx + b.getWidth() * 0.20f, b.getBottom() - 1.0f);
            g.strokePath (tail, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded));
            float ly = b.getY() + b.getHeight() * 0.38f;
            g.drawLine (b.getX() + 3.0f, ly, b.getRight() - 3.0f, ly, 1.3f);
            g.drawLine (b.getX() + 3.0f, ly + b.getHeight() * 0.24f,
                        b.getRight() - b.getWidth() * 0.30f, ly + b.getHeight() * 0.24f, 1.3f);
            break;
        }
        case Icon::social:
        {
            auto icon = a.withSizeKeepingCentre (20.0f, 20.0f).translated (0.0f, -0.4f);
            auto stroke = juce::PathStrokeType (1.8f, juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded);

            juce::Path back;
            back.addEllipse (icon.getX() + 10.5f, icon.getY() + 1.0f, 6.0f, 6.0f);
            back.startNewSubPath (icon.getX() + 9.5f, icon.getY() + 14.5f);
            back.cubicTo (icon.getX() + 10.1f, icon.getY() + 9.8f,
                          icon.getX() + 17.5f, icon.getY() + 9.0f,
                          icon.getRight() - 0.5f, icon.getY() + 15.5f);
            back.lineTo (icon.getX() + 15.1f, icon.getY() + 15.5f);
            g.strokePath (back, stroke);

            g.setColour (c);
            g.drawEllipse (icon.getX() + 3.0f, icon.getY() + 5.0f, 7.0f, 7.0f, 1.8f);
            juce::Path front;
            front.startNewSubPath (icon.getX() + 0.8f, icon.getBottom() - 0.5f);
            front.cubicTo (icon.getX() + 1.5f, icon.getY() + 13.0f,
                           icon.getX() + 11.5f, icon.getY() + 12.5f,
                           icon.getX() + 14.4f, icon.getBottom() - 0.5f);
            front.closeSubPath();
            g.strokePath (front, stroke);
            break;
        }
    }
}

void TransportBar::BpmBox::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);

    paintTransportSurface (g, getLocalBounds().toFloat());

    auto arrows = r.removeFromRight (arrowW);

    g.setColour (Colors::accent2);
    g.setFont (Typography::bold (22.0f));
    g.drawText (juce::String ((int) engine.getBpm()), r, juce::Justification::centred);

    auto up = arrows.removeFromTop (arrows.getHeight() * 0.5f).reduced (4.0f, 3.0f);
    auto dn = arrows.reduced (4.0f, 3.0f);
    {
        g.setColour (hoverArrow == 1 ? Colors::accent2 : Colors::textDim);
        juce::Path p;
        p.addTriangle (up.getX(), up.getBottom(), up.getRight(), up.getBottom(), up.getCentreX(), up.getY());
        g.fillPath (p);
    }
    {
        g.setColour (hoverArrow == -1 ? Colors::accent2 : Colors::textDim);
        juce::Path p;
        p.addTriangle (dn.getX(), dn.getY(), dn.getRight(), dn.getY(), dn.getCentreX(), dn.getBottom());
        g.fillPath (p);
    }
}

void TransportBar::BpmBox::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isMiddleButtonDown())
        return;

    if (e.mods.isPopupMenu())
    {
        showContextMenu();
        return;
    }

    if (arrowColumn().contains (e.getPosition()))
    {
        bool up = e.position.y < getHeight() * 0.5f;
        engine.setBpm (engine.getBpm() + (up ? 1.0 : -1.0));
        repaint();
        return;
    }

    dragging = true;
    dragStartBpm = (int) engine.getBpm();
    lastDragSteps = 0;
    e.source.enableUnboundedMouseMovement (true, false);
}

void TransportBar::BpmBox::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging)
        return;

    int steps = -e.getDistanceFromDragStartY() / 6;
    if (steps != lastDragSteps)
    {
        lastDragSteps = steps;
        engine.setBpm ((double) (dragStartBpm + steps));
        repaint();
    }
}

void TransportBar::BpmBox::mouseUp (const juce::MouseEvent& e)
{
    dragging = false;
    if (e.source.isUnboundedMouseMovementEnabled())
    {
        auto src = e.source;
        src.enableUnboundedMouseMovement (false);
        src.setScreenPosition (e.getMouseDownScreenPosition().toFloat());
    }
}

void TransportBar::BpmBox::mouseMove (const juce::MouseEvent& e)
{
    int h = 0;
    if (arrowColumn().contains (e.getPosition()))
        h = e.position.y < getHeight() * 0.5f ? 1 : -1;
    if (h != hoverArrow)
    {
        hoverArrow = h;
        repaint();
    }
}

void TransportBar::BpmBox::mouseExit (const juce::MouseEvent&)
{
    if (hoverArrow != 0)
    {
        hoverArrow = 0;
        repaint();
    }
}

void TransportBar::BpmBox::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    engine.setBpm (engine.getBpm() + (wheel.deltaY > 0 ? 1.0 : -1.0));
    repaint();
}

void TransportBar::BpmBox::showContextMenu()
{
    juce::PopupMenu m;
    m.addItem (1, "Copy Value");
    m.addItem (2, "Set Value...");
    m.addItem (3, "Reset to Default");
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
        [this] (int r)
        {
            switch (r)
            {
                case 1:
                    juce::SystemClipboard::copyTextToClipboard (juce::String ((int) engine.getBpm()));
                    break;
                case 2:
                    showSetValueDialog();
                    break;
                case 3:
                    engine.setBpm (settings != nullptr ? AppSettings::defaultBpm (*settings) : 140.0);
                    repaint();
                    break;
                default:
                    break;
            }
        });
}

void TransportBar::BpmBox::showSetValueDialog()
{
    auto* w = new juce::AlertWindow ("Set Tempo",
                                     "Enter a tempo between 20 and 999 BPM:",
                                     juce::MessageBoxIconType::NoIcon);
    w->addTextEditor ("value", juce::String ((int) engine.getBpm()));
    w->addButton ("Set", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    w->enterModalState (true, juce::ModalCallbackFunction::create (
        [safe = juce::Component::SafePointer<BpmBox> (this), w] (int result)
        {
            if (safe != nullptr && result == 1)
            {
                auto txt = w->getTextEditorContents ("value").trim();
                if (txt.isNotEmpty())
                {
                    safe->engine.setBpm (txt.getDoubleValue());
                    safe->repaint();
                }
            }
        }), true);
}

void TransportBar::PatSongSwitch::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    bool song = engine.isSongMode();

    paintTransportSurface (g, getLocalBounds().toFloat());

    auto drawHalf = [&] (juce::Rectangle<float> half, const juce::String& text,
                         juce::Colour glow, bool on)
    {
        auto pad = half.reduced (2.0f, 1.5f);
        if (on)
        {
            g.setColour (glow.withAlpha (0.9f));
            g.fillRoundedRectangle (pad, 2.0f);
        }
        g.setColour (on ? juce::Colours::black : Colors::textDim);
        g.setFont (Typography::bold (10.0f));
        g.drawText (text, half, juce::Justification::centred);
    };

    auto left = r.removeFromLeft (r.getWidth() * 0.5f);
    drawHalf (left, "PAT",  Colors::accent2,   ! song);
    drawHalf (r,    "SONG", Colors::noteGreen, song);

    if (flash)
    {
        g.setColour (Colors::accent);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 3.0f, 2.0f);
    }
}

void TransportBar::PatSongSwitch::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isMiddleButtonDown())
        return;

    bool song = e.position.x > getWidth() * 0.5f;
    if (onModeChosen) onModeChosen (song);
    repaint();
}

juce::String TransportBar::PosReadout::formatText() const
{
    auto ticks = (juce::int64) engine.getPositionTicks();

    if (format == 1)
    {
        auto bar  = ticks / Model::ticksPerBar + 1;
        auto beat = (ticks % Model::ticksPerBar) / Model::PPQ + 1;
        auto step = (ticks % Model::PPQ) / Model::ticksPerStep + 1;
        return juce::String (bar).paddedLeft ('0', 3) + ":"
             + juce::String (beat) + ":" + juce::String (step);
    }

    double bpm = engine.getBpm();
    double seconds = bpm > 0.0 ? (double) ticks / Model::PPQ * (60.0 / bpm) : 0.0;

    if (format == 2)
    {
        int total = (int) seconds;
        int h = total / 3600, m = (total / 60) % 60, s = total % 60;
        return juce::String (h).paddedLeft ('0', 2) + ":"
             + juce::String (m).paddedLeft ('0', 2) + ":"
             + juce::String (s).paddedLeft ('0', 2);
    }

    int total = (int) seconds;
    int m = total / 60, s = total % 60;
    int cs = (int) ((seconds - total) * 100.0);
    return juce::String (m).paddedLeft ('0', 2) + ":"
         + juce::String (s).paddedLeft ('0', 2) + ":"
         + juce::String (cs).paddedLeft ('0', 2);
}

void TransportBar::PosReadout::paint (juce::Graphics& g)
{
    paintTransportSurface (g, getLocalBounds().toFloat(), Colors::windowBg);

    g.setColour (Colors::accent);
    g.setFont (Typography::mono (17.0f));
    g.drawText (formatText(), getLocalBounds().reduced (6, 0), juce::Justification::centred);
}

void TransportBar::PosReadout::mouseDown (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu())
        return;

    juce::PopupMenu m;
    m.addItem (1, "Minutes : Seconds : Centiseconds", true, format == 0);
    m.addItem (2, "Bar : Beat : Step",                true, format == 1);
    m.addItem (3, "Hours : Minutes : Seconds",        true, format == 2);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
        [this] (int r)
        {
            if (r < 1) return;
            format = r - 1;
            if (settings != nullptr)
            {
                settings->setValue ("posReadoutFormat", format);
                settings->saveIfNeeded();
            }
            repaint();
        });
}

void TransportBar::SpectrumBox::updateSpectrum()
{
    int wp = engine.spectrumWritePos.load (std::memory_order_acquire);

    if (mode == Mode::oscilloscope)
    {
        for (int i = 0; i < fftSize; ++i)
            scope[i] = engine.spectrumSnapshot[(wp + i) & (AudioEngine::spectrumSize - 1)];
        return;
    }

    float fftData[fftSize * 2] = {};

    for (int i = 0; i < fftSize; ++i)
        fftData[i] = engine.spectrumSnapshot[(wp + i) & (AudioEngine::spectrumSize - 1)];

    window.multiplyWithWindowingTable (fftData, fftSize);
    fft.performFrequencyOnlyForwardTransform (fftData);

    float sampleRate = 44100.0f;
    if (auto* dev = engine.deviceManager.getCurrentAudioDevice())
        sampleRate = (float) dev->getCurrentSampleRate();

    const float minFreq = 40.0f;
    const float maxFreq = sampleRate * 0.5f;
    const float logMin = std::log2 (minFreq);
    const float logMax = std::log2 (maxFreq);

    for (int b = 0; b < numBands; ++b)
    {
        float f0 = std::pow (2.0f, logMin + (logMax - logMin) * b / numBands);
        float f1 = std::pow (2.0f, logMin + (logMax - logMin) * (b + 1) / numBands);
        int bin0 = juce::jlimit (1, fftSize / 2 - 1, (int) (f0 * fftSize / sampleRate));
        int bin1 = juce::jlimit (bin0 + 1, fftSize / 2, (int) (f1 * fftSize / sampleRate));

        float sum = 0.0f;
        for (int i = bin0; i < bin1; ++i)
            sum = juce::jmax (sum, fftData[i]);

        float dB = juce::jlimit (-60.0f, 0.0f, juce::Decibels::gainToDecibels (sum));
        bands[b] = (dB + 60.0f) / 60.0f;
        smoothed[b] = juce::jmax (bands[b], smoothed[b] * 0.82f);
    }
}

void TransportBar::SpectrumBox::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    paintTransportSurface (g, r);

    auto inner = r.reduced (4.0f, 4.0f);

    if (mode == Mode::oscilloscope)
    {
        const float centre = inner.getCentreY();
        const float halfH  = inner.getHeight() * 0.5f;
        const int   cols   = juce::jmax (1, (int) inner.getWidth());

        for (int c = 0; c < cols; ++c)
        {
            int s0 = (int) ((juce::int64) c       * fftSize / cols);
            int s1 = (int) ((juce::int64) (c + 1) * fftSize / cols);
            s1 = juce::jmax (s1, s0 + 1);

            float mn = scope[s0], mx = scope[s0];
            for (int i = s0 + 1; i < s1; ++i)
            {
                mn = juce::jmin (mn, scope[i]);
                mx = juce::jmax (mx, scope[i]);
            }

            float x  = inner.getX() + (float) c;
            float y0 = centre - juce::jlimit (-1.0f, 1.0f, mx) * halfH;
            float y1 = centre - juce::jlimit (-1.0f, 1.0f, mn) * halfH;

            float peak = juce::jlimit (0.0f, 1.0f, juce::jmax (std::abs (mn), std::abs (mx)));
            g.setColour (Colors::accent.interpolatedWith (Colors::accent2, peak));
            g.drawLine (x, y0, x, y1 + 1.0f, 1.0f);
        }
        return;
    }

    float barW = inner.getWidth() / (float) numBands;

    for (int b = 0; b < numBands; ++b)
    {
        float level = juce::jlimit (0.0f, 1.0f, smoothed[b]);
        float h = smoothed[b] * inner.getHeight();
        float x = inner.getX() + b * barW;
        float y = inner.getBottom() - h;

        g.setColour (Colors::accent.interpolatedWith (Colors::accent2, level)
                        .withAlpha (0.92f));
        g.fillRoundedRectangle (x + 0.5f, y, barW - 1.0f, h, 1.0f);

        if (h > 3.0f)
        {
            g.setColour (Colors::accent2);
            g.fillRoundedRectangle (x + 0.5f, y, barW - 1.0f, 1.6f, 0.8f);
        }
    }
}

void TransportBar::SpectrumBox::mouseDown (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu())
        return;

    juce::PopupMenu m;
    m.addSectionHeader ("Mode");
    m.addItem (1, "Spectrogram",  true, mode == Mode::spectrogram);
    m.addItem (2, "Oscilloscope", true, mode == Mode::oscilloscope);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
        [this] (int r)
        {
            if (r < 1) return;
            mode = (r == 2) ? Mode::oscilloscope : Mode::spectrogram;
            if (settings != nullptr)
            {
                settings->setValue ("spectrumBoxMode", mode == Mode::oscilloscope ? "osc" : "spec");
                settings->saveIfNeeded();
            }
            repaint();
        });
}

TransportBar::TransportBar (juce::ValueTree p, AudioEngine& e, juce::PropertiesFile* s)
    : project (p), engine (e), settings (s)
{
    posReadout.settings = s;
    bpmBox.settings = s;
    if (s != nullptr)
    {
        posReadout.format = juce::jlimit (0, 2, (int) s->getIntValue ("posReadoutFormat", 0));
        spectrumBox.mode = (s->getValue ("spectrumBoxMode", "spec") == "osc")
                             ? SpectrumBox::Mode::oscilloscope
                             : SpectrumBox::Mode::spectrogram;
    }
    fileBtn.setTriggeredOnMouseDown (true);
    fileBtn.onClick = [this] { showFileMenu(); };
    addAndMakeVisible (fileBtn);

    editBtn.setTriggeredOnMouseDown (true);
    editBtn.onClick = [this] { if (onEditMenu) onEditMenu (&editBtn); };
    addAndMakeVisible (editBtn);

    pluginsBtn.setTriggeredOnMouseDown (true);
    pluginsBtn.onClick = [this] { if (onShowPluginManager) onShowPluginManager(); };
    addAndMakeVisible (pluginsBtn);

    playBtn.activeColour = Colors::accent;
    playBtn.idleColour   = Colors::text;
    stopBtn.activeColour = Colors::accent;
    stopBtn.idleColour   = Colors::textDim;
    recBtn.activeColour  = Colors::record;
    recBtn.idleColour    = Colors::record;

    playBtn.onClick = [this]
    {
        if (! engine.isPlaying() && onBeforePlay)
            onBeforePlay();
        engine.togglePlay();
    };
    addAndMakeVisible (playBtn);

    stopBtn.onClick = [this] { engine.stop(); };
    addAndMakeVisible (stopBtn);

    recBtn.setEnabled (true);
    recBtn.onClick = [this] { if (onToggleRecord) onToggleRecord(); };
    recBtn.onRightClick = [this] { showRecordMenu(); };
    addAndMakeVisible (recBtn);

    if (settings != nullptr)
        engine.setPlaybackVolume (settings->getDoubleValue ("playbackVolume", 1.0));
    playbackVolKnob.setRange (0.0, 2.0, 0.0);
    playbackVolKnob.setDoubleClickReturnValue (true, 1.0);
    playbackVolKnob.setValue (engine.getPlaybackVolume(), juce::dontSendNotification);
    playbackVolKnob.setTooltip ("Playback volume (monitoring gain) - separate from the mixer master");
    playbackVolKnob.textFromValueFunction = [] (double v) { return juce::String (juce::roundToInt (v * 100.0)) + "%"; };
    playbackVolKnob.valueFromTextFunction = [] (const juce::String& t) { return t.getDoubleValue() / 100.0; };
    playbackVolKnob.onValueChange = [this]
    {
        engine.setPlaybackVolume (playbackVolKnob.getValue());
        if (settings != nullptr)
        {
            settings->setValue ("playbackVolume", playbackVolKnob.getValue());
            settings->saveIfNeeded();
        }
    };
    addAndMakeVisible (playbackVolKnob);

    playbackVolLabel.setText ("VOL", juce::dontSendNotification);
    playbackVolLabel.setJustificationType (juce::Justification::centred);
    playbackVolLabel.setColour (juce::Label::textColourId, Colors::textDim);
    playbackVolLabel.setFont (Typography::caps (8.0f));
    playbackVolLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (playbackVolLabel);

    addAndMakeVisible (bpmBox);
    addAndMakeVisible (spectrumBox);
    addAndMakeVisible (posReadout);

    metBtn.onClick = [this]
    {
        engine.setMetronomeEnabled (! engine.isMetronomeEnabled());
        metBtn.setActive (engine.isMetronomeEnabled());
        if (settings != nullptr)
        {
            settings->setValue ("metronomeOn", engine.isMetronomeEnabled());
            settings->saveIfNeeded();
        }
    };
    addAndMakeVisible (metBtn);

    if (settings != nullptr && settings->getBoolValue ("metronomeOn", false))
    {
        engine.setMetronomeEnabled (true);
        metBtn.setActive (true);
    }

    patSongSwitch.onModeChosen = [this] (bool song)
    {
        project.setProperty (IDs::songMode, song, nullptr);
        timerCallback();
    };
    addAndMakeVisible (patSongSwitch);

    browserBtn.activeColour = Colors::success;
    browserBtn.onClick  = [this] { if (onTogglePanel) onTogglePanel ("browser"); };
    rackBtn.onClick     = [this] { if (onTogglePanel) onTogglePanel ("rack"); };
    rollBtn.onClick     = [this] { if (onTogglePanel) onTogglePanel ("roll"); };
    playlistBtn.onClick = [this] { if (onTogglePanel) onTogglePanel ("playlist"); };
    mixerBtn.onClick    = [this] { if (onTogglePanel) onTogglePanel ("mixer"); };
    for (auto* b : { (juce::Component*) &browserBtn, (juce::Component*) &rackBtn,
                     (juce::Component*) &rollBtn, (juce::Component*) &playlistBtn,
                     (juce::Component*) &mixerBtn })
        addAndMakeVisible (*b);

    typingKbBtn.activeColour = Colors::accent;
    typingKbBtn.onClick = [this] { if (onToggleTypingKeyboard) onToggleTypingKeyboard(); };
    addAndMakeVisible (typingKbBtn);

    socialBtn.onClick = [this] { if (onSocialMenu) onSocialMenu (&socialBtn); };
    addAndMakeVisible (socialBtn);

    chatBtn.onClick = [this] { if (onToggleChat) onToggleChat(); };
    chatBtn.setVisible (false);
    addAndMakeVisible (chatBtn);

    sessionLabel.setColour (juce::Label::textColourId, Colors::textDim);
    sessionLabel.setFont (Typography::semi (11.0f));
    sessionLabel.setJustificationType (juce::Justification::centredRight);
    sessionLabel.setText ("Offline", juce::dontSendNotification);
    addAndMakeVisible (sessionLabel);

    overflowChip.setJustificationType (juce::Justification::centred);
    overflowChip.setFont (Typography::semi (11.0f));
    overflowChip.setColour (juce::Label::textColourId, Colors::textDim);
    overflowChip.setColour (juce::Label::backgroundColourId, Colors::card);
    overflowChip.setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
    overflowChip.setVisible (false);
    addAndMakeVisible (overflowChip);

    startTimerHz (20);
}

void TransportBar::showFileMenu()
{
    juce::PopupMenu m;

    auto typeName = getStartupTypeName ? getStartupTypeName() : juce::String ("basic drums");
    m.addItem (1, "New (" + typeName + ")");
    m.addItem (2, "Open Project...");
    m.addSeparator();

    m.addItem (3, "Save");
    m.addItem (4, "Save as...");
    m.addItem (5, "Save as template...");
    m.addSeparator();
    m.addItem (6, "Open Autosaves Folder");
    m.addItem (7, "Open Recordings Folder");
    m.addSeparator();

    juce::PopupMenu exportSub;
    exportSub.addItem (10, "Render as Audio...");
    exportSub.addSeparator();
    exportSub.addItem (15, "mikeDAW Project ZIP (.zip)...");
    exportSub.addItem (14, "Zipped Loop Package (.zip)...");
    m.addSubMenu ("Export", exportSub);

    auto recents = getRecentProjects ? getRecentProjects() : juce::StringArray();
    if (recents.size() > 0)
    {
        m.addSeparator();
        for (int i = 0; i < recents.size(); ++i)
            m.addItem (100 + i, juce::File (recents[i]).getFileName());
    }

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&fileBtn),
        [this, recents] (int result)
        {
            if (result == 1 && onNew)            onNew();
            else if (result == 2 && onLoad)      onLoad();
            else if (result == 3 && onSave)      onSave();
            else if (result == 4 && onSaveAs)    onSaveAs();
            else if (result == 5 && onSaveAsTemplate) onSaveAsTemplate();
            else if (result == 6 && onOpenAutosaves) onOpenAutosaves();
            else if (result == 7 && onOpenRecordings) onOpenRecordings();
            else if (result == 10 && onRenderAudio) onRenderAudio();
            else if (result == 14 && onExportFlZip) onExportFlZip();
            else if (result == 15 && onExportMikeZip) onExportMikeZip();
            else if (result >= 100 && result < 100 + recents.size() && onOpenRecent)
                onOpenRecent (juce::File (recents[result - 100]));
        });
}

void TransportBar::showRecordMenu()
{
    if (settings == nullptr)
        return;
    auto& s = *settings;

    juce::PopupMenu m;
    m.addSectionHeader ("Recording filter");
    m.addItem (1, "Automation", false, false);
    m.addItem (2, "Notes", true, AppSettings::recNotes (s));
    m.addItem (3, "Audio", true, AppSettings::recAudio (s));
    m.addItem (4, "Clips", false, false);
    m.addSeparator();
    m.addSectionHeader ("Options");
    m.addItem (5, "Recording starts playback", true, AppSettings::recStartsPlayback (s));
    m.addItem (6, "Disarm on stop", true, AppSettings::recDisarmOnStop (s));
    m.addSeparator();
    m.addSectionHeader ("Quantize");
    m.addItem (7, "Note start time", true, AppSettings::recQuantizeStart (s));
    m.addItem (8, "Note end time", true, AppSettings::recQuantizeEnd (s));
    m.addSeparator();
    m.addItem (9, "Open recordings folder");

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&recBtn),
        [this] (int r)
        {
            if (r < 1 || settings == nullptr)
                return;
            auto& st = *settings;
            switch (r)
            {
                case 2: AppSettings::setRecNotes (st, ! AppSettings::recNotes (st)); break;
                case 3: AppSettings::setRecAudio (st, ! AppSettings::recAudio (st));
                        engine.setAudioRecordingEnabled (AppSettings::recAudio (st)); break;
                case 5: AppSettings::setRecStartsPlayback (st, ! AppSettings::recStartsPlayback (st)); break;
                case 6: AppSettings::setRecDisarmOnStop (st, ! AppSettings::recDisarmOnStop (st)); break;
                case 7: AppSettings::setRecQuantizeStart (st, ! AppSettings::recQuantizeStart (st)); break;
                case 8: AppSettings::setRecQuantizeEnd (st, ! AppSettings::recQuantizeEnd (st)); break;
                case 9: if (onOpenRecordings) onOpenRecordings(); break;
                default: break;
            }
            st.saveIfNeeded();
        });
}

void TransportBar::setSessionText (const juce::String& s)
{
    sessionLabel.setText (s, juce::dontSendNotification);
}

void TransportBar::setSessionActive (bool active, bool isHost)
{
    if (sessionActive == active && sessionIsHost == isHost)
        return;
    sessionActive = active;
    sessionIsHost = isHost;

    chatBtn.setVisible (active);
    if (! active)
        chatBtn.setActive (false);
    sessionLabel.setColour (juce::Label::textColourId, active ? Colors::accent : Colors::textDim);
    sessionLabel.repaint();
    resized();
}

void TransportBar::lookAndFeelChanged()
{
    playbackVolLabel.setColour (juce::Label::textColourId, Colors::textDim);
    sessionLabel.setColour (juce::Label::textColourId, sessionActive ? Colors::accent : Colors::textDim);
    overflowChip.setColour (juce::Label::textColourId, Colors::textDim);
    overflowChip.setColour (juce::Label::backgroundColourId, Colors::panelDark);
    overflowChip.setColour (juce::Label::outlineColourId, Colors::outline);

    playBtn.activeColour = Colors::noteGreen;
    playBtn.idleColour   = Colors::noteGreen.interpolatedWith (Colors::textDim, 0.45f);
    stopBtn.activeColour = Colors::record;
    stopBtn.idleColour   = Colors::textDim;
    recBtn.activeColour  = Colors::record;
    recBtn.idleColour    = Colors::record.interpolatedWith (Colors::textDim, 0.45f);
    typingKbBtn.activeColour = Colors::accent;
    browserBtn.activeColour = Colors::success;
}

void TransportBar::DeafenButton::paintButton (juce::Graphics& g, bool, bool)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    bool on = getToggleState();
    auto base = on ? Colors::danger
                   : (isEnabled() ? Colors::textDim.interpolatedWith (Colors::text, hover.value())
                                  : Colors::textDim);

    auto cx = r.getCentreX();
    auto padW = r.getWidth() * 0.24f;
    auto padH = r.getHeight() * 0.34f;
    auto bandY = r.getY() + r.getHeight() * 0.30f;
    auto padY  = r.getY() + r.getHeight() * 0.42f;
    float bandR = r.getWidth() * 0.34f;

    juce::Path phones;
    phones.addCentredArc (cx, bandY, bandR, bandR * 0.9f, 0.0f,
                          -juce::MathConstants<float>::halfPi * 1.05f,
                           juce::MathConstants<float>::halfPi * 1.05f, true);
    juce::Rectangle<float> leftPad  (cx - bandR - padW * 0.5f, padY, padW, padH);
    juce::Rectangle<float> rightPad (cx + bandR - padW * 0.5f, padY, padW, padH);
    phones.addRoundedRectangle (leftPad, 1.6f);
    phones.addRoundedRectangle (rightPad, 1.6f);

    juce::Path cross;
    cross.startNewSubPath (r.getX() + 1.0f, r.getBottom() - 1.0f);
    cross.lineTo (r.getRight() - 1.0f, r.getY() + 1.0f);

    auto stroke = [&] (const juce::Path& p, float w, float a)
    {
        g.setColour (base.withAlpha (a));
        g.strokePath (p, juce::PathStrokeType (w, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));
    };

    stroke (phones, 1.8f, 1.0f);
    stroke (cross,  1.9f, 1.0f);
}

void TransportBar::ListenButton::paintButton (juce::Graphics& g, bool, bool)
{
    auto r = getLocalBounds().toFloat().reduced (0.8f);
    bool on = getToggleState();
    auto base = on ? onColour
                   : (isEnabled() ? Colors::textDim.interpolatedWith (Colors::text, hover.value())
                                  : Colors::textDim);

    auto cx = r.getCentreX();
    auto cy = r.getCentreY();
    float rad = r.getWidth() * 0.36f;

    juce::Path ear;
    ear.addCentredArc (cx, cy - rad * 0.02f, rad, rad * 1.08f, 0.0f,
                       -juce::MathConstants<float>::pi * 0.82f,
                        juce::MathConstants<float>::pi * 0.72f, true);
    ear.cubicTo (cx + rad * 0.56f, cy + rad * 0.88f,
                 cx - rad * 0.12f, cy + rad * 1.20f,
                 cx - rad * 0.10f, cy + rad * 0.60f);

    juce::Path inner;
    inner.startNewSubPath (cx + rad * 0.22f, cy - rad * 0.34f);
    inner.cubicTo (cx - rad * 0.26f, cy - rad * 0.34f,
                   cx - rad * 0.24f, cy + rad * 0.16f,
                   cx + rad * 0.10f, cy + rad * 0.10f);
    inner.cubicTo (cx + rad * 0.40f, cy + rad * 0.06f,
                   cx + rad * 0.32f, cy + rad * 0.52f,
                   cx + rad * 0.02f, cy + rad * 0.58f);

    juce::Path canal;
    canal.startNewSubPath (cx + rad * 0.16f, cy + rad * 0.22f);
    canal.cubicTo (cx + rad * 0.36f, cy + rad * 0.30f,
                   cx + rad * 0.30f, cy + rad * 0.58f,
                   cx + rad * 0.06f, cy + rad * 0.72f);

    auto stroke = [&] (const juce::Path& path, float w, float a)
    {
        g.setColour (base.withAlpha (a));
        g.strokePath (path, juce::PathStrokeType (w, juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));
    };

    stroke (ear,   1.85f, 1.0f);
    stroke (inner, 1.35f, 0.92f);
    stroke (canal, 1.15f, 0.78f);
}

void TransportBar::MicMuteButton::paintButton (juce::Graphics& g, bool, bool)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    bool muted = getToggleState();
    auto base = muted ? Colors::danger
                      : (isEnabled() ? Colors::textDim.interpolatedWith (Colors::text, hover.value())
                                     : Colors::textDim);

    auto cx = r.getCentreX();
    float capW = r.getWidth() * 0.42f;
    float capH = r.getHeight() * 0.56f;
    auto capY = r.getY() + r.getHeight() * 0.08f;
    float capR = capW * 0.5f;

    juce::Path mic;
    mic.addRoundedRectangle (r.getX() + (r.getWidth() - capW) * 0.5f, capY, capW, capH, capR);
    float stemX = cx - r.getWidth() * 0.05f;
    mic.startNewSubPath (stemX, r.getBottom() - r.getHeight() * 0.16f);
    mic.lineTo (stemX, r.getBottom() - r.getHeight() * 0.34f);
    auto arcR = r.getWidth() * 0.30f;
    mic.addCentredArc (cx, r.getBottom() - r.getHeight() * 0.30f, arcR, arcR, 0.0f,
                       juce::MathConstants<float>::pi, juce::MathConstants<float>::twoPi, true);

    auto stroke = [&] (const juce::Path& p, float w, float a)
    {
        g.setColour (base.withAlpha (a));
        g.strokePath (p, juce::PathStrokeType (w, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));
    };

    stroke (mic, 1.8f, 1.0f);

    if (muted)
    {
        juce::Path cross;
        cross.startNewSubPath (r.getX() + 0.5f, r.getBottom() - 0.5f);
        cross.lineTo (r.getRight() - 0.5f, r.getY() + 0.5f);
        g.setColour (base);
        g.strokePath (cross, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));
    }
}

TransportBar::PeerStrip::PeerStrip()
{
    nameLabel.setJustificationType (juce::Justification::centredLeft);
    nameLabel.setFont (Typography::ui (12.0f));
    nameLabel.setBorderSize (juce::BorderSize<int> (0));
    nameLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (nameLabel);

    deafenBtn.onClick = [this] { if (local && onDeafen) onDeafen (deafenBtn.getToggleState()); };
    addAndMakeVisible (deafenBtn);

    micBtn.onClick = [this] { if (local && onInputMuted) onInputMuted (micBtn.getToggleState()); };
    micBtn.setTooltip ("Mute / unmute your microphone input");
    addAndMakeVisible (micBtn);

    listenBtn.onClick = [this] { if (onListenToggled) onListenToggled (listenBtn.getToggleState()); };
    addAndMakeVisible (listenBtn);
}

void TransportBar::PeerStrip::setState (const Session::UserState& s, bool isLocal,
                                        const juce::Image& avatar)
{
    peerId = s.peerId;
    local = isLocal;
    if (s.afk && ! afk)
        afkSinceMs = juce::Time::currentTimeMillis();
    afk = s.afk;
    peerColour = Colors::peerColour (s.peerId);
    avatarImage = avatar;

    auto name = s.name.isNotEmpty() ? s.name : juce::String ("A Kind Soul");
    peerName = name;
    initial = name.substring (0, 1).toUpperCase();
    nameLabel.setText (name, juce::dontSendNotification);
    nameLabel.setColour (juce::Label::textColourId, afk ? Colors::textDim : Colors::text);

    deafenBtn.setToggleState (s.deafen, juce::dontSendNotification);

    listenBtn.onColour = isLocal ? Colors::accent2 : peerColour;
    listenBtn.setToggleState (getListen && getListen(), juce::dontSendNotification);
    listenBtn.setTooltip (isLocal ? "Share your MIDI note previews with the session"
                                  : "Hear " + name + "'s MIDI note previews");

    deafenBtn.setEnabled (isLocal);
    setInterceptsMouseClicks (true, true);
    deafenBtn.setAlpha (isLocal ? 1.0f : 0.58f);
    nameLabel.setAlpha (isLocal ? 1.0f : 0.58f);
    micBtn.setVisible (isLocal);
    repaint();
}

void TransportBar::PeerStrip::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() && onRightClick)
        onRightClick (peerId, peerName);
}

void TransportBar::PeerStrip::resized()
{
    auto r = getLocalBounds().reduced (4, 1);

    r.removeFromLeft (r.getHeight() + 6);

    auto top = r.removeFromTop (14);
    nameLabel.setBounds (top);

    auto bottom = r;
    const int control = juce::jmin (bottom.getHeight(), 21);
    if (micBtn.isVisible())
    {
        micBtn.setBounds (bottom.removeFromLeft (control).withSizeKeepingCentre (control, control));
        bottom.removeFromLeft (1);
    }
    deafenBtn.setBounds (bottom.removeFromLeft (control).withSizeKeepingCentre (control, control));
    bottom.removeFromLeft (1);
    listenBtn.setBounds (bottom.removeFromLeft (control).withSizeKeepingCentre (control, control));
}

void TransportBar::PeerStrip::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();

    auto body = r.reduced (1.0f);
    paintTransportSurface (g, body, Colors::panelDark.withAlpha (local ? 0.78f : 0.58f));

    auto col = afk ? Colors::textDim : peerColour;

    auto inner = body.reduced (4.0f, 3.0f);
    float d = inner.getHeight();
    juce::Rectangle<float> badge (inner.getX(), inner.getY(), d, d);

    if (avatarImage.isValid())
    {
        juce::Graphics::ScopedSaveState ss (g);
        juce::Path clip;
        clip.addEllipse (badge);
        g.reduceClipRegion (clip);
        g.setOpacity (local ? 1.0f : 0.62f);
        g.drawImage (avatarImage, badge,
                     juce::RectanglePlacement::centred | juce::RectanglePlacement::fillDestination);
    }
    else
    {
        g.setColour (col.withAlpha (local ? 0.32f : 0.20f));
        g.fillEllipse (badge);
        g.setColour (col.withAlpha (local ? 1.0f : 0.62f));
        g.setFont (Typography::semi (d * 0.5f));
        g.drawText (initial, badge, juce::Justification::centred);
    }
    g.setColour (col.withAlpha (local ? 1.0f : 0.62f));
    g.drawEllipse (badge.reduced (0.9f), 1.8f);

    if (afk)
    {
        auto elapsedMs = juce::jmax ((juce::int64) 0, juce::Time::currentTimeMillis() - afkSinceMs);
        auto mins = (int) (elapsedMs / 60000);
        juce::String label = mins < 1 ? "AFK <1m"
                           : mins < 60 ? "AFK " + juce::String (mins) + "m"
                                       : "AFK " + juce::String (mins / 60) + "h" + juce::String (mins % 60) + "m";

        auto pillFont = Typography::semi (9.0f);
        auto textBounds = nameLabel.getBounds().toFloat();
        auto nameW = juce::GlyphArrangement::getStringWidth (Typography::ui (12.0f), nameLabel.getText());
        auto pillW = juce::GlyphArrangement::getStringWidth (pillFont, label) + 10.0f;

        float pillX = textBounds.getX() + juce::jmin (nameW + 4.0f, textBounds.getWidth() - pillW);
        pillX = juce::jmax (textBounds.getX(), pillX);
        float pillH = juce::jmin (13.0f, textBounds.getHeight());
        juce::Rectangle<float> pill (pillX, textBounds.getCentreY() - pillH * 0.5f, pillW, pillH);

        g.setColour (Colors::accent2.withAlpha (local ? 0.9f : 0.62f));
        g.fillRoundedRectangle (pill, pillH * 0.5f);
        g.setColour (Colors::windowBg);
        g.setFont (pillFont);
        g.drawText (label, pill, juce::Justification::centred);
    }
}

void TransportBar::updatePeerStrips (const std::vector<Session::UserState>& roster,
                                     juce::int64 localPeerId)
{
    if (roster.empty())
    {
        if (! peerStrips.isEmpty())
        {
            peerStrips.clear();
            resized();
        }
        return;
    }

    while (peerStrips.size() < (int) roster.size())
    {
        auto* strip = new PeerStrip();
        strip->onDeafen = [this] (bool d)  { if (onDeafenToggled)  onDeafenToggled (d); };
        strip->onInputMuted = [this] (bool m) { if (onInputMuteToggled) onInputMuteToggled (m); };
        strip->onRightClick = [this] (juce::int64 id, juce::String name)
        { if (onPeerRightClick) onPeerRightClick (id, name); };
        addAndMakeVisible (strip);
        peerStrips.add (strip);
    }
    while (peerStrips.size() > (int) roster.size())
        peerStrips.removeLast();

    for (int i = 0; i < (int) roster.size(); ++i)
    {
        auto* strip = peerStrips[i];
        bool isLocal = roster[(size_t) i].peerId == localPeerId;
        auto id = roster[(size_t) i].peerId;

        strip->getListen = [this, id, isLocal] () -> bool
        {
            if (isLocal) return shareAuditions;
            auto it = listenedPeers.find (id);
            return it != listenedPeers.end() && it->second;
        };
        strip->onListenToggled = [this, id, isLocal] (bool on)
        {
            if (isLocal) shareAuditions = on;
            else         listenedPeers[id] = on;
        };

        auto avatar = getPeerAvatar ? getPeerAvatar (id) : juce::Image();
        strip->setState (roster[(size_t) i], isLocal, avatar);
        strip->setListenVisible (sessionActive);
        if (isLocal)
            strip->setMicMute (localMicMuted);
    }

    resized();
}

bool TransportBar::isListeningToPeer (juce::int64 peerId) const
{
    auto it = listenedPeers.find (peerId);
    return it != listenedPeers.end() && it->second;
}

void TransportBar::flashModeToggle()
{
    modeFlashUntilMs = juce::Time::getMillisecondCounter() + 600;
    patSongSwitch.flash = true;
    patSongSwitch.repaint();
}

void TransportBar::timerCallback()
{
    if (patSongSwitch.flash && juce::Time::getMillisecondCounter() >= modeFlashUntilMs)
    {
        patSongSwitch.flash = false;
        patSongSwitch.repaint();
    }

    playBtn.setActive (engine.isPlaying());
    metBtn.setActive (engine.isMetronomeEnabled());

    const bool armed = engine.isRecordArmed();
    const bool recording = engine.isRecording();
    if (recording)
    {
        recBlinkFrame = (recBlinkFrame + 1) % 10;
        recBtn.setActive (recBlinkFrame < 5);
    }
    else
    {
        recBlinkFrame = 0;
        recBtn.setActive (armed);
    }

    const bool playing = engine.isPlaying();
    if (recWasPlaying && ! playing && armed
        && settings != nullptr && AppSettings::recDisarmOnStop (*settings))
        engine.setRecordArmed (false);
    recWasPlaying = playing;

    patSongSwitch.repaint();
    posReadout.repaint();
    bpmBox.repaint();
    spectrumBox.updateSpectrum();
    spectrumBox.repaint();

    for (auto* strip : peerStrips)
        strip->repaintIfAfk();
}

void TransportBar::paint (juce::Graphics& g)
{
    g.fillAll (Colors::header);
    g.setColour (Colors::outline);
    g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
}

void TransportBar::resized()
{
    auto r = getLocalBounds().reduced (6, 5);

    const juce::Font menuFont = Typography::caps (13.5f);
    auto placeMenu = [&] (juce::Button& b)
    {
        int w = juce::GlyphArrangement::getStringWidthInt (menuFont, b.getButtonText()) + 16;
        b.setBounds (r.removeFromLeft (w));
        r.removeFromLeft (8);
    };
    placeMenu (fileBtn);
    placeMenu (editBtn);
    placeMenu (pluginsBtn);

    {
        auto knobArea = r.removeFromLeft (34);
        auto capH = 9;
        playbackVolLabel.setBounds (knobArea.removeFromBottom (capH));
        playbackVolKnob.setBounds (knobArea);
    }
    r.removeFromLeft (8);

    patSongSwitch.setBounds (r.removeFromLeft (76).withSizeKeepingCentre (76, 16));
    r.removeFromLeft (10);

    recBtn.setBounds (r.removeFromLeft (36));
    r.removeFromLeft (2);
    playBtn.setBounds (r.removeFromLeft (36));
    r.removeFromLeft (2);
    stopBtn.setBounds (r.removeFromLeft (36));
    r.removeFromLeft (10);

    metBtn.setBounds (r.removeFromLeft (36));
    r.removeFromLeft (2);
    typingKbBtn.setBounds (r.removeFromLeft (36));
    r.removeFromLeft (10);

    bpmBox.setBounds (r.removeFromLeft (78));
    r.removeFromLeft (6);
    spectrumBox.setBounds (r.removeFromLeft (90));
    r.removeFromLeft (6);
    posReadout.setBounds (r.removeFromLeft (100));
    r.removeFromLeft (12);

    for (auto* b : { &playlistBtn, &rollBtn, &rackBtn, &mixerBtn, &browserBtn })
    {
        b->setBounds (r.removeFromLeft (36));
        r.removeFromLeft (2);
    }

    socialBtn.setBounds (r.removeFromRight (36).reduced (0, 3));
    r.removeFromRight (6);

    if (chatBtn.isVisible())
    {
        chatBtn.setBounds (r.removeFromRight (36).reduced (0, 3));
        r.removeFromRight (6);
    }

    if (! peerStrips.isEmpty())
    {
        const int cardW = 138;
        const int gap = 2;
        const int chipW = 34;

        int budget = juce::jmax (0, r.getWidth() - 120);
        int maxCards = juce::jmax (1, (budget + gap) / (cardW + gap));
        int visible = juce::jmin (peerStrips.size(), maxCards);
        int hidden = peerStrips.size() - visible;

        if (hidden > 0)
            visible = juce::jmax (1, juce::jmin (visible,
                                  (budget - chipW - gap + gap) / (cardW + gap)));
        hidden = peerStrips.size() - visible;

        auto cardArea = r.removeFromRight (visible * cardW + (visible - 1) * gap
                                           + (hidden > 0 ? chipW + gap : 0));

        if (hidden > 0)
        {
            overflowChip.setText ("+" + juce::String (hidden), juce::dontSendNotification);
            overflowChip.setVisible (true);
            overflowChip.setBounds (cardArea.removeFromRight (chipW).reduced (0, 6));
            cardArea.removeFromRight (gap);
        }
        else
        {
            overflowChip.setVisible (false);
        }

        for (int i = 0; i < peerStrips.size(); ++i)
            peerStrips[i]->setVisible (i < visible);

        for (int i = visible; --i >= 0;)
        {
            peerStrips[i]->setBounds (cardArea.removeFromRight (cardW));
            cardArea.removeFromRight (gap);
        }
        r.removeFromRight (6);
    }
    else
    {
        overflowChip.setVisible (false);
    }

    sessionLabel.setBounds (r.reduced (6, 0));
}
