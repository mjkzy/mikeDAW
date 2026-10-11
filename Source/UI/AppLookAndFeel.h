#pragma once
#include <JuceHeader.h>

namespace Fx
{
    inline float pianoGlowIntensity    = 1.0f;
    inline float pianoGlowOpacity      = 1.0f;
    inline float playlistGlowIntensity = 1.0f;
    inline bool  glowEnabled           = true;
}

namespace Colors
{
    inline juce::Colour windowBg   { 0xff111111 };
    inline juce::Colour panelBg    { 0xff181818 };
    inline juce::Colour panelDark  { 0xff141414 };
    inline juce::Colour knobBg     { 0xff0e0e0e };
    inline juce::Colour raised     { 0xff2a2a2a };
    inline juce::Colour header     { 0xff1b1b1b };
    inline juce::Colour card       { 0xff1f1f1f };
    inline juce::Colour outline    { 0xff2a2a2a };
    inline juce::Colour text       { 0xffe6e6e6 };
    inline juce::Colour textDim    { 0xff8b8b8b };
    inline juce::Colour accent     { 0xffa594f9 };
    inline juce::Colour accent2    { 0xfff0a35e };
    inline juce::Colour stepDim    { 0xff2c2c2c };
    inline juce::Colour stepDim2   { 0xff262626 };
    inline juce::Colour gridLine   { 0xff232323 };
    inline juce::Colour gridBar    { 0xff343434 };
    inline juce::Colour noteGreen  { 0xffb3a4ff };
    inline juce::Colour meterMid   { 0xffd6c96a };
    inline juce::Colour meterHot   { 0xffe5484d };
    inline juce::Colour stereoMerged    { 0xffb4a5ff };
    inline juce::Colour stereoSeparated { 0xff6aa9e9 };
    inline juce::Colour toolSelect { 0xfff27aa8 };
    inline juce::Colour toolTrim   { 0xff7f9cf5 };
    inline juce::Colour scaleRow   { 0xff1f1f1f };
    inline juce::Colour ghostNote  { 0xff6b6b6b };

    inline juce::Colour stepOn    { 0xffa594f9 };
    inline juce::Colour stepOff   { 0xff303030 };

    inline juce::Colour danger    { 0xffe5484d };
    inline juce::Colour record    { 0xffe5484d };
    inline juce::Colour success   { 0xff46a758 };
    inline juce::Colour warning   { 0xfff5a524 };

    inline juce::Colour popupBg   { 0xff1e1e1e };

    inline juce::Colour pianoWhiteKey { 0xffd9d9d9 };
    inline juce::Colour pianoBlackKey { 0xff1c1c1c };
    inline juce::Colour pianoKeyText  { 0xff2c2c2c };
    inline juce::Colour pianoRowBlack { 0xff151515 };
    inline juce::Colour pianoRowWhite { 0xff1b1b1b };
    inline juce::Colour pianoRowFlat  { 0xff181818 };

    inline juce::Colour clipDefault    { 0xff66799f };
    inline juce::Colour automationClip { 0xff6fc3a8 };
    inline juce::Colour refreshGreen   { 0xff3f6f4f };
    inline juce::Colour scrollThumb      { 0xff3a3a3a };
    inline juce::Colour scrollThumbHover { 0xff4d4d4d };
    inline juce::Colour eqGraphBg   { 0xff1f2a33 };
    inline juce::Colour eqGraphGrid { 0xff2f3d49 };
    inline juce::Colour eqPanelBg   { 0xff242c35 };
    inline juce::Colour eqCurve     { 0xfff2f5f8 };
    inline juce::Colour eqHeat      { 0xffc8324e };
    inline juce::Colour eqHeatHot   { 0xffff9aa8 };
    inline juce::Colour gbPanel     { 0xff2a2d30 };
    inline juce::Colour gbEnvBg     { 0xff1b1e21 };
    inline juce::Colour gbGrid      { 0xff33383d };
    inline juce::Colour gbTime      { 0xff7ccf4a };
    inline juce::Colour gbVolume    { 0xffe8a33a };

    inline juce::Colour lassoTint  { 0x2ea594f9 };
    inline juce::Colour dangerTint { 0x55ff3030 };

    inline juce::Colour contrastText (juce::Colour bg)
    {
        return bg.getPerceivedBrightness() > 0.56f ? juce::Colour (0xff171a1d)
                                                   : juce::Colour (0xfff4f5f5);
    }

    inline juce::Colour clipTone (juce::Colour c)
    {
        return c.withMultipliedSaturation (0.95f).withMultipliedBrightness (0.85f);
    }

    inline juce::Colour clipHeader (juce::Colour toned) { return toned.darker (0.5f); }

    inline juce::Colour clipHeaderLift (juce::Colour c)
    {
        return juce::Colour::fromRGB ((juce::uint8) juce::jmin (255, c.getRed()   * 3 / 2),
                                      (juce::uint8) juce::jmin (255, c.getGreen() * 3 / 2),
                                      (juce::uint8) juce::jmin (255, c.getBlue()  * 3 / 2))
                   .withMultipliedSaturation (1.0f / 0.95f)
                   .withMultipliedBrightness (1.0f / 0.85f);
    }

    inline juce::Colour peerColour (juce::int64 id)
    {
        auto h = (float) ((juce::uint64) id % 360) / 360.0f;
        return juce::Colour::fromHSV (h, 0.65f, 0.95f, 1.0f);
    }

    inline juce::Font emojiFont (float height)
    {
        return juce::Font (juce::FontOptions (height)
                               .withFallbacks ({ "Segoe UI Emoji", "Noto Color Emoji" }));
    }
}

namespace Metrics
{
    inline constexpr float rS = 4.0f, rM = 7.0f, rL = 11.0f;
    inline constexpr int pad = 8, padS = 4;
}

namespace Typography
{
    inline juce::Font ui (float h)        { return juce::Font (juce::FontOptions (h).withName ("Segoe UI")); }
    inline juce::Font bold (float h)      { return juce::Font (juce::FontOptions (h, juce::Font::bold).withName ("Segoe UI")); }
    inline juce::Font semi (float h)      { return juce::Font (juce::FontOptions (h).withName ("Segoe UI Semibold")); }
    inline juce::Font mono (float h)      { return juce::Font (juce::FontOptions (h, juce::Font::bold).withName ("Consolas")); }
    inline juce::Font caps (float h = 12.0f) { return semi (h).withExtraKerningFactor (0.08f); }
}

class AppLookAndFeel : public juce::LookAndFeel_V4
{
public:
    AppLookAndFeel()
    {
        setDefaultSansSerifTypefaceName ("Segoe UI");
        refreshColours();
    }

    void refreshColours()
    {
        setColour (juce::ResizableWindow::backgroundColourId, Colors::windowBg);
        setColour (juce::DocumentWindow::textColourId, Colors::text);
        setColour (juce::Label::textColourId, Colors::text);
        setColour (juce::TextButton::buttonColourId, Colors::raised);
        setColour (juce::TextButton::buttonOnColourId, Colors::accent.withAlpha (0.75f));
        setColour (juce::TextButton::textColourOffId, Colors::text);
        setColour (juce::TextButton::textColourOnId, Colors::windowBg);
        setColour (juce::ComboBox::backgroundColourId, Colors::card);
        setColour (juce::ComboBox::textColourId, Colors::text);
        setColour (juce::ComboBox::outlineColourId, Colors::outline);
        setColour (juce::ComboBox::arrowColourId, Colors::textDim);
        setColour (juce::PopupMenu::backgroundColourId, Colors::popupBg);
        setColour (juce::PopupMenu::textColourId, Colors::text);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, Colors::raised);
        setColour (juce::PopupMenu::highlightedTextColourId, Colors::text);
        setColour (juce::Slider::backgroundColourId, Colors::panelDark);
        setColour (juce::Slider::trackColourId, Colors::accent);
        setColour (juce::Slider::thumbColourId, Colors::accent);
        setColour (juce::Slider::rotarySliderFillColourId, Colors::accent);
        setColour (juce::Slider::rotarySliderOutlineColourId, Colors::panelDark);
        setColour (juce::TextEditor::backgroundColourId, Colors::panelDark);
        setColour (juce::TextEditor::textColourId, Colors::text);
        setColour (juce::TextEditor::outlineColourId, Colors::outline);
        setColour (juce::TextEditor::focusedOutlineColourId, Colors::accent);
        setColour (juce::TextEditor::highlightColourId, Colors::accent.withAlpha (0.30f));
        setColour (juce::ScrollBar::thumbColourId, Colors::scrollThumb);
        setColour (juce::TreeView::backgroundColourId, Colors::panelDark);
        setColour (juce::TreeView::linesColourId, Colors::gridLine);
        setColour (juce::TreeView::selectedItemBackgroundColourId, Colors::accent.withAlpha (0.16f));
        setColour (juce::DirectoryContentsDisplayComponent::textColourId, Colors::text);
        setColour (juce::DirectoryContentsDisplayComponent::highlightColourId, Colors::accent.withAlpha (0.16f));
        setColour (juce::ListBox::backgroundColourId, Colors::panelDark);
        setColour (juce::AlertWindow::backgroundColourId, Colors::card);
        setColour (juce::AlertWindow::textColourId, Colors::text);
        setColour (juce::AlertWindow::outlineColourId, Colors::outline);
        setColour (juce::ToggleButton::textColourId, Colors::text);
        setColour (juce::ToggleButton::tickColourId, Colors::accent);
        setColour (juce::TooltipWindow::backgroundColourId, Colors::card);
        setColour (juce::TooltipWindow::textColourId, Colors::text);
        setColour (juce::TooltipWindow::outlineColourId, Colors::outline);
    }

    void drawTreeviewPlusMinusBox (juce::Graphics& g, const juce::Rectangle<float>& area,
                                   juce::Colour, bool isOpen, bool isMouseOver) override
    {
        const float s = 4.2f;
        const auto c = area.getCentre();
        juce::Path tri;
        if (isOpen)
            tri.addTriangle (c.x - s, c.y - s * 0.6f, c.x + s, c.y - s * 0.6f, c.x, c.y + s * 0.8f);
        else
            tri.addTriangle (c.x - s * 0.6f, c.y - s, c.x - s * 0.6f, c.y + s, c.x + s * 0.8f, c.y);
        g.setColour (Colors::textDim.withAlpha (isMouseOver ? 1.0f : 0.75f));
        g.fillPath (tri);
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& b,
                               const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted,
                               bool shouldDrawButtonAsDown) override
    {
        auto bounds = b.getLocalBounds().toFloat().reduced (0.5f);
        auto base = backgroundColour;

        if (! b.isEnabled())
            base = base.withMultipliedAlpha (0.5f);
        else if (shouldDrawButtonAsDown)
            base = base.darker (0.08f);
        else if (shouldDrawButtonAsHighlighted)
            base = base.brighter (0.07f);

        g.setColour (base);
        g.fillRoundedRectangle (bounds, 6.0f);
        g.setColour (Colors::outline.brighter (0.12f));
        g.drawRoundedRectangle (bounds, 6.0f, 1.0f);
    }

    void drawComboBox (juce::Graphics& g, int width, int height, bool,
                       int, int, int, int, juce::ComboBox& box) override
    {
        auto bounds = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (0.5f);
        g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
        g.fillRoundedRectangle (bounds, 6.0f);
        g.setColour (box.findColour (juce::ComboBox::outlineColourId));
        g.drawRoundedRectangle (bounds, 6.0f, 1.0f);

        auto cx = (float) width - 12.0f;
        auto cy = (float) height * 0.5f;
        juce::Path chevron;
        chevron.startNewSubPath (cx - 4.0f, cy - 2.0f);
        chevron.lineTo (cx, cy + 2.5f);
        chevron.lineTo (cx + 4.0f, cy - 2.0f);

        g.setColour (box.findColour (juce::ComboBox::arrowColourId)
                        .withAlpha (box.isEnabled() ? 1.0f : 0.4f));
        g.strokePath (chevron, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
    }

    juce::Font getComboBoxFont (juce::ComboBox& box) override
    {
        return Typography::ui (juce::jmin (15.0f, (float) box.getHeight() * 0.65f));
    }

    void drawPopupMenuBackground (juce::Graphics& g, int width, int height) override
    {
        g.fillAll (Colors::popupBg);
        g.setColour (Colors::outline.brighter (0.15f));
        g.drawRect (0, 0, width, height, 1);
    }

    juce::Font getPopupMenuFont() override { return Typography::ui (15.0f); }

    int getPopupMenuBorderSize() override { return 6; }

    void getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator,
                                    int standardMenuItemHeight,
                                    int& idealWidth, int& idealHeight) override
    {
        if (isSeparator)
        {
            idealWidth = 50;
            idealHeight = 13;
            return;
        }

        auto font = getPopupMenuFont();
        idealHeight = standardMenuItemHeight > 0 ? standardMenuItemHeight
                                                 : juce::roundToInt (font.getHeight() * 1.85f);
        idealWidth = juce::GlyphArrangement::getStringWidthInt (font, text) + idealHeight * 2;
    }

    void drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area,
                            bool isSeparator, bool isActive, bool isHighlighted,
                            bool isTicked, bool hasSubMenu, const juce::String& text,
                            const juce::String& shortcutKeyText,
                            const juce::Drawable* icon,
                            const juce::Colour* textColourToUse) override
    {
        if (isSeparator)
        {
            auto r = area.reduced (8, 0).toFloat();
            r.removeFromTop (r.getHeight() * 0.5f - 0.5f);
            g.setColour (Colors::gridLine);
            g.fillRect (r.removeFromTop (1.0f));
            return;
        }

        auto textColour = textColourToUse != nullptr ? *textColourToUse : Colors::text;
        auto r = area.toFloat().reduced (4.0f, 1.0f);

        if (isHighlighted && isActive)
        {
            g.setColour (findColour (juce::PopupMenu::highlightedBackgroundColourId));
            g.fillRoundedRectangle (r, 5.0f);
        }

        g.setColour (isActive ? textColour : textColour.withAlpha (0.4f));

        const float iconCol = juce::jmin (20.0f, r.getHeight() * 0.8f);
        auto iconArea = r.reduced (6.0f, 0.0f).removeFromLeft (iconCol);

        if (icon != nullptr)
        {
            icon->drawWithin (g, iconArea, juce::RectanglePlacement::centred, 1.0f);
        }
        else if (isTicked)
        {
            auto tick = getTickShape (1.0f);
            g.setColour (Colors::accent);
            g.fillPath (tick, tick.getTransformToScaleToFit (iconArea.reduced (2.0f), true));
            g.setColour (isActive ? textColour : textColour.withAlpha (0.4f));
        }

        auto textArea = r.withTrimmedLeft (iconCol + 12.0f).withTrimmedRight (8.0f);

        if (hasSubMenu)
        {
            auto h = r.getHeight();
            auto arrowH = 0.5f * getPopupMenuFont().getAscent();
            auto x = textArea.getRight() - h * 0.5f;

            juce::Path p;
            p.startNewSubPath (x, r.getCentreY() - arrowH * 0.5f);
            p.lineTo (x + arrowH * 0.6f, r.getCentreY());
            p.lineTo (x, r.getCentreY() + arrowH * 0.5f);
            g.strokePath (p, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));

            textArea = textArea.withTrimmedRight (h);
        }

        g.setFont (getPopupMenuFont());
        g.drawFittedText (text, textArea.toNearestInt(), juce::Justification::centredLeft, 1);

        if (shortcutKeyText.isNotEmpty())
        {
            g.setFont (getPopupMenuFont().withHeight (getPopupMenuFont().getHeight() * 0.8f));
            g.setColour (Colors::textDim);
            g.drawText (shortcutKeyText, textArea, juce::Justification::centredRight, true);
        }
    }

    void drawScrollbar (juce::Graphics& g, juce::ScrollBar& bar, int x, int y, int width, int height,
                        bool isScrollbarVertical, int thumbStartPosition, int thumbSize,
                        bool isMouseOver, bool isMouseDown) override
    {
        if (thumbSize <= 0)
            return;

        juce::Rectangle<float> thumb = isScrollbarVertical
            ? juce::Rectangle<float> ((float) x, (float) thumbStartPosition,
                                      (float) width, (float) thumbSize)
            : juce::Rectangle<float> ((float) thumbStartPosition, (float) y,
                                      (float) thumbSize, (float) height);

        const bool hot = isMouseOver || isMouseDown || bar.isMouseButtonDown();
        const float thickness = hot ? 7.0f : 5.0f;
        thumb = isScrollbarVertical ? thumb.withSizeKeepingCentre (juce::jmin (thickness, thumb.getWidth()), thumb.getHeight() - 4.0f)
                                    : thumb.withSizeKeepingCentre (thumb.getWidth() - 4.0f, juce::jmin (thickness, thumb.getHeight()));
        if (thumb.getWidth() <= 0.0f || thumb.getHeight() <= 0.0f)
            return;

        g.setColour (hot ? Colors::scrollThumbHover : Colors::scrollThumb);
        g.fillRoundedRectangle (thumb, juce::jmin (thumb.getWidth(), thumb.getHeight()) * 0.5f);
    }

    void fillTextEditorBackground (juce::Graphics& g, int width, int height,
                                   juce::TextEditor& ed) override
    {
        g.setColour (ed.findColour (juce::TextEditor::backgroundColourId));
        g.fillRoundedRectangle (juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height),
                                6.0f);
    }

    void drawTextEditorOutline (juce::Graphics& g, int width, int height,
                                juce::TextEditor& ed) override
    {
        if (! ed.isEnabled())
            return;

        const auto c = ed.findColour (ed.hasKeyboardFocus (true)
                                          ? juce::TextEditor::focusedOutlineColourId
                                          : juce::TextEditor::outlineColourId);
        if (c.isTransparent())
            return;

        g.setColour (c);
        g.drawRoundedRectangle (juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height)
                                    .reduced (0.5f),
                                6.0f, ed.hasKeyboardFocus (true) ? 1.5f : 1.0f);
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                           juce::Slider& s) override
    {
        auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height)
                          .reduced (1.5f);
        auto radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) / 2.0f;
        auto centre = bounds.getCentre();
        auto angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
        auto fill = s.findColour (juce::Slider::rotarySliderFillColourId);
        auto capR = radius * 0.72f;
        const juce::PathStrokeType arcStroke (2.0f, juce::PathStrokeType::curved,
                                              juce::PathStrokeType::rounded);

        auto wellR = radius + 1.5f;
        g.setColour (Colors::knobBg);
        g.fillEllipse (centre.x - wellR, centre.y - wellR, wellR * 2, wellR * 2);

        {
            juce::Path track;
            track.addCentredArc (centre.x, centre.y, radius, radius, 0.0f,
                                 rotaryStartAngle, rotaryEndAngle, true);
            g.setColour (Colors::raised);
            g.strokePath (track, arcStroke);
        }

        if ((bool) s.getProperties().getWithDefault ("bipolar", false))
        {
            float midAngle = (rotaryStartAngle + rotaryEndAngle) * 0.5f;
            bool atCentre = std::abs (angle - midAngle) < 0.02f;
            bool positive = angle >= midAngle;

            auto sideColour = [&] (bool pos) -> juce::Colour
            {
                const char* key = pos ? "bipolarPosColour" : "bipolarNegColour";
                if (s.getProperties().contains (key))
                    return juce::Colour ((juce::uint32) (int) s.getProperties()[key]);
                return fill;
            };
            juce::Colour active = atCentre ? Colors::textDim : sideColour (positive);

            if (! atCentre)
            {
                juce::Path arc;
                arc.addCentredArc (centre.x, centre.y, radius, radius, 0.0f,
                                   midAngle, angle, true);
                g.setColour (active.withAlpha (0.9f));
                g.strokePath (arc, arcStroke);
            }

            g.setColour (Colors::card);
            g.fillEllipse (centre.x - capR, centre.y - capR, capR * 2, capR * 2);
            g.setColour (Colors::outline.brighter (0.15f));
            g.drawEllipse (centre.x - capR, centre.y - capR, capR * 2, capR * 2, 1.0f);

            juce::Path tick;
            tick.addRectangle (-1.0f, -capR, 2.0f, capR * 0.55f);
            g.setColour (active);
            g.fillPath (tick, juce::AffineTransform::rotation (angle).translated (centre.x, centre.y));
            return;
        }

        juce::Path arc;
        arc.addCentredArc (centre.x, centre.y, radius, radius, 0.0f,
                           rotaryStartAngle, angle, true);
        g.setColour (fill.withAlpha (0.9f));
        g.strokePath (arc, arcStroke);

        g.setColour (Colors::card);
        g.fillEllipse (centre.x - capR, centre.y - capR, capR * 2, capR * 2);
        g.setColour (Colors::outline.brighter (0.15f));
        g.drawEllipse (centre.x - capR, centre.y - capR, capR * 2, capR * 2, 1.0f);

        juce::Path tick;
        tick.addRectangle (-1.0f, -capR, 2.0f, capR * 0.55f);
        g.setColour (Colors::text);
        g.fillPath (tick, juce::AffineTransform::rotation (angle).translated (centre.x, centre.y));
    }

    void drawTabbedButtonBarBackground (juce::TabbedButtonBar& bar, juce::Graphics& g) override
    {
        auto r = bar.getLocalBounds();
        g.setColour (Colors::header);
        g.fillRect (r);
        g.setColour (Colors::outline);
        g.fillRect (r.removeFromBottom (1));
    }

    void drawTabButton (juce::TabBarButton& button, juce::Graphics& g,
                        bool isMouseOver, bool isMouseDown) override
    {
        auto area = button.getActiveArea().toFloat().reduced (3.0f, 4.0f);
        const bool front = button.isFrontTab();

        if (front || isMouseOver)
        {
            g.setColour (front ? Colors::raised : Colors::raised.withAlpha (0.45f));
            g.fillRoundedRectangle (area, 6.0f);
        }

        drawTabButtonText (button, g, isMouseOver, isMouseDown);
    }

    void drawTabButtonText (juce::TabBarButton& button, juce::Graphics& g,
                            bool, bool) override
    {
        const bool front = button.isFrontTab();
        g.setColour (front ? Colors::text : Colors::textDim);
        g.setFont (front ? Typography::semi (14.0f) : Typography::ui (14.0f));
        g.drawText (button.getButtonText(), button.getTextArea(),
                    juce::Justification::centred, false);
    }
};
