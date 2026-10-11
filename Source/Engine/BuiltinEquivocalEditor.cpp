#include "BuiltinEquivocal.h"

#include <array>
#include <cmath>
#include <memory>
#include <vector>

namespace
{
    juce::Font uiFont (float height, bool bold = false)
    {
        return juce::Font (juce::FontOptions (height,
                           bold ? juce::Font::bold : juce::Font::plain));
    }

    float clamp01 (float value)
    {
        return juce::jlimit (0.0f, 1.0f, std::isfinite (value) ? value : 0.0f);
    }

    void paintPanel (juce::Graphics& g, juce::Rectangle<int> bounds, float radius = 12.0f)
    {
        auto r = bounds.toFloat();
        g.setColour (Colors::outline.withAlpha (0.48f));
        g.fillRoundedRectangle (r.translated (0.0f, 2.0f), radius);
        g.setColour (Colors::panelDark.withAlpha (0.94f));
        g.fillRoundedRectangle (r, radius);
        g.setColour (Colors::raised.withAlpha (0.55f));
        g.drawRoundedRectangle (r.reduced (0.5f), radius, 1.0f);
    }

    class EquivocalKnobLookAndFeel final : public juce::LookAndFeel_V4
    {
    public:
        void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                               float sliderPosition, float startAngle, float endAngle,
                               juce::Slider& slider) override
        {
            auto area = juce::Rectangle<float> ((float) x, (float) y,
                                                 (float) width, (float) height).reduced (5.0f);
            const float radius = juce::jmin (area.getWidth(), area.getHeight()) * 0.5f;
            const auto centre = area.getCentre();
            const float angle = startAngle + sliderPosition * (endAngle - startAngle);
            const auto accent = slider.findColour (juce::Slider::rotarySliderFillColourId);

            for (int i = 4; i >= 1; --i)
            {
                const float grow = (float) i * 1.8f;
                g.setColour (accent.withAlpha (0.025f * (float) (5 - i)));
                g.fillEllipse (area.expanded (grow));
            }

            juce::Path bed;
            bed.addCentredArc (centre.x, centre.y, radius - 1.0f, radius - 1.0f,
                               0.0f, startAngle, endAngle, true);
            g.setColour (Colors::windowBg.withAlpha (0.86f));
            g.strokePath (bed, juce::PathStrokeType (4.4f,
                          juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            juce::Path valueArc;
            valueArc.addCentredArc (centre.x, centre.y, radius - 1.0f, radius - 1.0f,
                                    0.0f, startAngle, angle, true);
            g.setColour (accent.withAlpha (0.96f));
            g.strokePath (valueArc, juce::PathStrokeType (3.0f,
                          juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            auto cap = area.reduced (radius * 0.27f);
            juce::ColourGradient capGradient (Colors::raised.brighter (0.12f),
                                              cap.getX(), cap.getY(),
                                              Colors::panelDark.darker (0.4f),
                                              cap.getRight(), cap.getBottom(), false);
            g.setGradientFill (capGradient);
            g.fillEllipse (cap);
            g.setColour (Colors::outline.withAlpha (0.9f));
            g.drawEllipse (cap, 1.1f);

            juce::Path indicator;
            indicator.startNewSubPath (centre.x, centre.y - radius * 0.30f);
            indicator.lineTo (centre.x, centre.y - radius * 0.62f);
            g.setColour (Colors::text.withAlpha (0.94f));
            g.strokePath (indicator, juce::PathStrokeType (2.0f,
                          juce::PathStrokeType::curved, juce::PathStrokeType::rounded),
                          juce::AffineTransform::rotation (angle, centre.x, centre.y));

            g.setColour (accent.withAlpha (0.9f));
            g.fillEllipse (centre.x - 1.6f, centre.y - 1.6f, 3.2f, 3.2f);
        }
    };

    class KnobTile final : public juce::Component
    {
    public:
        KnobTile() = default;

        ~KnobTile() override
        {
            knob.setLookAndFeel (nullptr);
        }

        void configure (const juce::String& newName, juce::RangedAudioParameter& parameter,
                        juce::Colour newAccent, EquivocalKnobLookAndFeel& look)
        {
            name = newName;
            accent = newAccent;
            knob.setLookAndFeel (&look);
            knob.setColour (juce::Slider::rotarySliderFillColourId, accent);
            knob.setMouseDragSensitivity (145);
            knob.setRotaryParameters (juce::MathConstants<float>::pi * 1.22f,
                                      juce::MathConstants<float>::pi * 2.78f, true);
            knob.setDoubleClickReturnValue (true,
                parameter.convertFrom0to1 (parameter.getDefaultValue()));
            knob.onValueChange = [this] { repaint(); };
            knob.setTooltip (parameter.getName (64));
            addAndMakeVisible (knob);
        }

        void paint (juce::Graphics& g) override
        {
            auto tile = getLocalBounds().toFloat().reduced (2.0f);
            const float norm = clamp01 ((float) knob.valueToProportionOfLength (knob.getValue()));
            g.setColour (Colors::windowBg.withAlpha (isMouseOver (true) ? 0.58f : 0.35f));
            g.fillRoundedRectangle (tile, 8.0f);
            g.setColour (accent.withAlpha (isMouseOver (true) ? 0.30f : 0.12f));
            g.drawRoundedRectangle (tile, 8.0f, 1.0f);

            auto nameArea = getLocalBounds().removeFromTop (19).reduced (3, 0);
            g.setFont (uiFont (9.0f, true));
            g.setColour (norm > 0.005f ? Colors::text : Colors::textDim);
            g.drawText (name, nameArea, juce::Justification::centred, false);

            auto valueArea = getLocalBounds().removeFromBottom (18).reduced (3, 0);
            g.setFont (uiFont (9.5f));
            g.setColour (accent.withAlpha (0.92f));
            g.drawText (knob.getTextFromValue (knob.getValue()), valueArea,
                        juce::Justification::centred, true);
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (3);
            area.removeFromTop (17);
            area.removeFromBottom (17);
            const int size = juce::jmin (62, juce::jmin (area.getWidth(), area.getHeight()));
            knob.setBounds (area.withSizeKeepingCentre (size, size));
        }

        ValueKnob knob;

    private:
        juce::String name;
        juce::Colour accent { Colors::accent };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KnobTile)
    };

    class ChoiceSelector final : public juce::Component
    {
    public:
        ChoiceSelector (juce::String titleIn, juce::AudioParameterChoice& parameterIn)
            : title (std::move (titleIn)), parameter (parameterIn), choices (parameter.choices),
              attachment (parameter, [this] (float value)
              {
                  current = juce::jlimit (0, juce::jmax (0, choices.size() - 1),
                                          (int) std::round (value));
                  repaint();
              })
        {
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
            attachment.sendInitialUpdate();
        }

        void mouseDown (const juce::MouseEvent& event) override
        {
            if (event.mods.isPopupMenu())
                return;

            juce::PopupMenu menu;
            for (int i = 0; i < choices.size(); ++i)
                menu.addItem (i + 1, choices[i], true, i == current);

            menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                [safe = juce::Component::SafePointer<ChoiceSelector> (this)] (int result)
                {
                    if (safe != nullptr && result > 0)
                        safe->select (result - 1);
                });
        }

        void mouseWheelMove (const juce::MouseEvent&,
                             const juce::MouseWheelDetails& wheel) override
        {
            if (choices.isEmpty() || wheel.deltaY == 0.0f)
                return;
            select ((current + (wheel.deltaY > 0.0f ? -1 : 1) + choices.size()) % choices.size());
        }

        void paint (juce::Graphics& g) override
        {
            auto b = getLocalBounds().toFloat().reduced (1.0f);
            juce::ColourGradient fill (Colors::raised.withAlpha (0.90f), b.getX(), b.getY(),
                                       Colors::panelDark, b.getX(), b.getBottom(), false);
            g.setGradientFill (fill);
            g.fillRoundedRectangle (b, 9.0f);
            g.setColour (Colors::accent.withAlpha (isMouseOver() ? 0.75f : 0.28f));
            g.drawRoundedRectangle (b, 9.0f, 1.0f);

            auto content = getLocalBounds().reduced (10, 4);
            auto titleArea = content.removeFromTop (15);
            g.setFont (uiFont (8.5f, true));
            g.setColour (Colors::textDim);
            g.drawText (title, titleArea, juce::Justification::centredLeft, false);

            g.setFont (uiFont (12.5f, true));
            g.setColour (Colors::text);
            g.drawText (choices[current], content.withTrimmedRight (18),
                        juce::Justification::centredLeft, true);

            auto arrow = juce::Rectangle<float> ((float) getWidth() - 20.0f,
                                                  (float) getHeight() * 0.55f, 8.0f, 5.0f);
            juce::Path triangle;
            triangle.addTriangle (arrow.getX(), arrow.getY(), arrow.getRight(), arrow.getY(),
                                  arrow.getCentreX(), arrow.getBottom());
            g.setColour (Colors::accent);
            g.fillPath (triangle);
        }

    private:
        void select (int index)
        {
            if (choices.isEmpty())
                return;
            attachment.setValueAsCompleteGesture ((float) juce::jlimit (0, choices.size() - 1, index));
        }

        juce::String title;
        juce::AudioParameterChoice& parameter;
        juce::StringArray choices;
        int current = 0;
        juce::ParameterAttachment attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChoiceSelector)
    };

    class FreezeLatch final : public juce::Button
    {
    public:
        FreezeLatch() : juce::Button ("Hold the testimony")
        {
            setClickingTogglesState (true);
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
        }

        void setEnergy (float newEnergy)
        {
            energy += (clamp01 (newEnergy) - energy) * 0.24f;
            repaint();
        }

        void paintButton (juce::Graphics& g, bool hovered, bool down) override
        {
            auto b = getLocalBounds().toFloat().reduced (1.0f);
            const bool active = getToggleState();
            const auto colour = active ? Colors::accent2 : Colors::accent;

            for (int i = 3; i >= 1 && active; --i)
            {
                g.setColour (colour.withAlpha ((0.035f + energy * 0.035f) * (float) i));
                g.fillRoundedRectangle (b.expanded ((float) i * 1.4f), 11.0f);
            }

            g.setColour ((down ? Colors::windowBg : Colors::raised)
                         .withAlpha (active ? 0.98f : 0.78f));
            g.fillRoundedRectangle (b, 10.0f);
            g.setColour (colour.withAlpha (active || hovered ? 0.96f : 0.42f));
            g.drawRoundedRectangle (b, 10.0f, active ? 1.8f : 1.0f);

            auto content = getLocalBounds().reduced (9, 4);
            auto lamp = juce::Rectangle<float> ((float) content.getX(),
                                                (float) content.getCentreY() - 5.0f, 10.0f, 10.0f);
            g.setColour (active ? colour : Colors::outline);
            g.fillEllipse (lamp);
            if (active)
            {
                g.setColour (colour.withAlpha (0.30f + energy * 0.35f));
                g.drawEllipse (lamp.expanded (3.0f), 2.0f);
            }

            auto text = content.withTrimmedLeft (20);
            g.setFont (uiFont (12.0f, true));
            g.setColour (active ? Colors::text : Colors::textDim);
            g.drawText (active ? "TESTIMONY HELD" : "HOLD TESTIMONY", text,
                        juce::Justification::centred, false);
        }

    private:
        float energy = 0.0f;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FreezeLatch)
    };
}

namespace
{
    class RailButton final : public juce::Button
    {
    public:
        enum class Kind { previous, next, preset };

        explicit RailButton (Kind kindIn) : juce::Button ("Preset rail"), kind (kindIn)
        {
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
        }

        void setPreset (juce::String nameIn, juce::String categoryIn)
        {
            presetName = std::move (nameIn);
            category = std::move (categoryIn);
            repaint();
        }

        void paintButton (juce::Graphics& g, bool hovered, bool down) override
        {
            auto b = getLocalBounds().toFloat().reduced (1.0f);
            auto base = down ? Colors::windowBg : Colors::raised;
            g.setColour (base.withAlpha (kind == Kind::preset ? 0.82f : 0.68f));
            g.fillRoundedRectangle (b, kind == Kind::preset ? 9.0f : 7.0f);
            g.setColour ((hovered ? Colors::accent : Colors::gridLine)
                         .withAlpha (hovered ? 0.88f : 0.48f));
            g.drawRoundedRectangle (b, kind == Kind::preset ? 9.0f : 7.0f, 1.0f);

            if (kind != Kind::preset)
            {
                juce::Path arrow;
                const float direction = kind == Kind::previous ? -1.0f : 1.0f;
                const auto c = b.getCentre();
                arrow.startNewSubPath (c.x - direction * 4.0f, c.y - 5.0f);
                arrow.lineTo (c.x + direction * 2.0f, c.y);
                arrow.lineTo (c.x - direction * 4.0f, c.y + 5.0f);
                g.setColour (hovered ? Colors::accent : Colors::textDim);
                g.strokePath (arrow, juce::PathStrokeType (2.0f,
                              juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                return;
            }

            auto content = getLocalBounds().reduced (12, 3);
            auto categoryArea = content.removeFromTop (13);
            g.setFont (uiFont (8.5f, true));
            g.setColour (Colors::accent.withAlpha (0.88f));
            g.drawText (category.toUpperCase(), categoryArea,
                        juce::Justification::centredLeft, true);
            g.setFont (uiFont (13.0f, true));
            g.setColour (Colors::text);
            g.drawText (presetName, content.withTrimmedRight (18),
                        juce::Justification::centredLeft, true);

            auto x = (float) getWidth() - 18.0f;
            auto y = (float) getHeight() * 0.60f;
            juce::Path triangle;
            triangle.addTriangle (x - 4.0f, y - 2.0f, x + 4.0f, y - 2.0f, x, y + 3.0f);
            g.setColour (Colors::accent2);
            g.fillPath (triangle);
        }

    private:
        Kind kind;
        juce::String presetName { "Initial testimony" };
        juce::String category { "Factory" };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RailButton)
    };

    class HeaderTelemetry final : public juce::Component
    {
    public:
        HeaderTelemetry() = default;

        void setState (float witnessIn, float rumorIn, float loopIn, float mercyIn)
        {
            witness += (clamp01 (witnessIn) - witness) * 0.34f;
            rumor += (clamp01 (rumorIn) - rumor) * 0.34f;
            loop += (clamp01 (loopIn) - loop) * 0.28f;
            mercy += (clamp01 (mercyIn) - mercy) * 0.20f;
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            paintPanel (g, getLocalBounds(), 9.0f);
            auto area = getLocalBounds().reduced (9, 5);
            const int rowHeight = juce::jmax (1, area.getHeight() / 3);
            drawMeter (g, area.removeFromTop (rowHeight), "WITNESS", witness, Colors::accent);
            drawMeter (g, area.removeFromTop (rowHeight), "RUMOR", rumor, Colors::accent2);
            drawMeter (g, area, "LOOP", loop * (0.55f + mercy * 0.45f), Colors::warning);
        }

    private:
        static void drawMeter (juce::Graphics& g, juce::Rectangle<int> row,
                               const juce::String& label, float value, juce::Colour colour)
        {
            auto labelArea = row.removeFromLeft (48);
            g.setFont (uiFont (7.5f, true));
            g.setColour (Colors::textDim);
            g.drawText (label, labelArea, juce::Justification::centredLeft, false);

            auto readout = row.removeFromRight (24);
            g.setFont (uiFont (7.5f));
            g.setColour (colour);
            g.drawText (juce::String ((int) std::round (value * 99.0f)), readout,
                        juce::Justification::centredRight, false);

            auto track = row.toFloat().reduced (2.0f, 4.0f);
            g.setColour (Colors::windowBg.withAlpha (0.72f));
            g.fillRoundedRectangle (track, 2.0f);
            auto fill = track.withWidth (track.getWidth() * clamp01 (value));
            g.setColour (colour.withAlpha (0.92f));
            g.fillRoundedRectangle (fill, 2.0f);
        }

        float witness = 0.0f;
        float rumor = 0.0f;
        float loop = 0.0f;
        float mercy = 1.0f;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HeaderTelemetry)
    };

    class VerdictField final : public juce::Component
    {
    public:
        VerdictField (juce::RangedAudioParameter& verdictParameter,
                      juce::RangedAudioParameter& doubtParameter)
            : verdictParam (verdictParameter), doubtParam (doubtParameter),
              verdictAttachment (verdictParam, [this] (float value)
              {
                  verdict = clamp01 (verdictParam.convertTo0to1 (value));
                  repaint();
              }),
              doubtAttachment (doubtParam, [this] (float value)
              {
                  doubt = clamp01 (doubtParam.convertTo0to1 (value));
                  repaint();
              })
        {
            setMouseCursor (juce::MouseCursor::CrosshairCursor);
            juce::Random random (0x4551564f);
            for (auto& particle : particles)
            {
                particle.x = random.nextFloat();
                particle.y = random.nextFloat();
                particle.speed = 0.35f + random.nextFloat() * 0.85f;
                particle.size = 0.7f + random.nextFloat() * 1.8f;
                particle.phase = random.nextFloat() * juce::MathConstants<float>::twoPi;
                particle.rumor = random.nextBool();
            }
            verdictAttachment.sendInitialUpdate();
            doubtAttachment.sendInitialUpdate();
        }

        ~VerdictField() override
        {
            endGesture();
        }

        void setTelemetry (float witnessIn, float rumorIn, float arbiterIn,
                           float featureIn, float feedbackIn, float grainPhaseIn,
                           float delayMsIn)
        {
            witnessTarget = clamp01 (witnessIn);
            rumorTarget = clamp01 (rumorIn);
            arbiterTarget = clamp01 (arbiterIn);
            featureTarget = clamp01 (featureIn);
            feedbackTarget = clamp01 (feedbackIn);
            grainTarget = clamp01 (grainPhaseIn);
            delayMs = juce::jmax (0.0f, std::isfinite (delayMsIn) ? delayMsIn : 0.0f);
        }

        void advance()
        {
            witnessVisual += (witnessTarget - witnessVisual) * 0.22f;
            rumorVisual += (rumorTarget - rumorVisual) * 0.22f;
            arbiterVisual += (arbiterTarget - arbiterVisual) * 0.18f;
            featureVisual += (featureTarget - featureVisual) * 0.16f;
            feedbackVisual += (feedbackTarget - feedbackVisual) * 0.20f;
            grainVisual += (grainTarget - grainVisual) * 0.18f;
            phase += 0.020f + feedbackVisual * 0.060f;

            for (auto& particle : particles)
            {
                particle.x += particle.speed * (0.0012f + feedbackVisual * 0.0048f);
                if (particle.x > 1.04f)
                {
                    particle.x = -0.04f;
                    particle.y = std::fmod (particle.y + 0.37f + grainVisual * 0.31f, 1.0f);
                }
                particle.phase += 0.012f + particle.speed * 0.015f;
            }

            const auto current = getHandlePoint();
            if (trailCount == 0 || current.getDistanceFrom (trail[0]) > 0.4f)
            {
                for (int i = (int) trail.size() - 1; i > 0; --i)
                    trail[(size_t) i] = trail[(size_t) i - 1];
                trail[0] = current;
                trailCount = juce::jmin ((int) trail.size(), trailCount + 1);
            }
            repaint();
        }

        void mouseDown (const juce::MouseEvent& event) override
        {
            if (! event.mods.isLeftButtonDown())
                return;
            dragging = true;
            verdictAttachment.beginGesture();
            doubtAttachment.beginGesture();
            setFromPosition (event.position);
        }

        void mouseDrag (const juce::MouseEvent& event) override
        {
            if (dragging)
                setFromPosition (event.position);
        }

        void mouseUp (const juce::MouseEvent&) override
        {
            endGesture();
        }

        void paint (juce::Graphics& g) override
        {
            auto outer = getLocalBounds().toFloat().reduced (1.0f);
            const auto plot = getPlotBounds();

            g.setColour (Colors::outline.withAlpha (0.78f));
            g.fillRoundedRectangle (outer.translated (0.0f, 2.0f), 16.0f);

            juce::ColourGradient vertical (Colors::accent.withAlpha (0.14f),
                                           plot.getCentreX(), plot.getY(),
                                           Colors::accent2.withAlpha (0.12f),
                                           plot.getCentreX(), plot.getBottom(), false);
            g.setGradientFill (vertical);
            g.fillRoundedRectangle (outer, 16.0f);
            g.setColour (Colors::panelDark.withAlpha (0.86f));
            g.fillRoundedRectangle (outer.reduced (1.0f), 15.0f);

            juce::ColourGradient bloom (Colors::accent.withAlpha (0.12f),
                                        plot.getX(), plot.getCentreY(),
                                        Colors::accent2.withAlpha (0.10f),
                                        plot.getRight(), plot.getCentreY(), false);
            g.setGradientFill (bloom);
            g.fillRoundedRectangle (plot, 10.0f);

            drawGrid (g, plot);
            drawParticles (g, plot);
            drawRibbons (g, plot);
            drawTrail (g);
            drawArbiter (g, plot);
            drawHandle (g);
            drawLabels (g, plot);
        }

    private:
        struct Particle
        {
            float x = 0.0f;
            float y = 0.0f;
            float speed = 0.0f;
            float size = 1.0f;
            float phase = 0.0f;
            bool rumor = false;
        };

        juce::Rectangle<float> getPlotBounds() const
        {
            return getLocalBounds().toFloat().reduced (24.0f, 29.0f);
        }

        juce::Point<float> getHandlePoint() const
        {
            const auto plot = getPlotBounds();
            return { plot.getX() + plot.getWidth() * verdict,
                     plot.getBottom() - plot.getHeight() * doubt };
        }

        void setFromPosition (juce::Point<float> point)
        {
            const auto plot = getPlotBounds();
            const float x = clamp01 ((point.x - plot.getX()) / plot.getWidth());
            const float y = clamp01 (1.0f - (point.y - plot.getY()) / plot.getHeight());
            verdictAttachment.setValueAsPartOfGesture (verdictParam.convertFrom0to1 (x));
            doubtAttachment.setValueAsPartOfGesture (doubtParam.convertFrom0to1 (y));
        }

        void endGesture()
        {
            if (! dragging)
                return;
            verdictAttachment.endGesture();
            doubtAttachment.endGesture();
            dragging = false;
        }

        static void drawGrid (juce::Graphics& g, juce::Rectangle<float> plot)
        {
            g.setColour (Colors::gridLine.withAlpha (0.19f));
            for (int i = 1; i < 8; ++i)
            {
                const float x = plot.getX() + plot.getWidth() * (float) i / 8.0f;
                g.drawVerticalLine ((int) std::round (x), plot.getY(), plot.getBottom());
            }
            for (int i = 1; i < 6; ++i)
            {
                const float y = plot.getY() + plot.getHeight() * (float) i / 6.0f;
                g.drawHorizontalLine ((int) std::round (y), plot.getX(), plot.getRight());
            }

            g.setColour (Colors::gridBar.withAlpha (0.38f));
            g.drawLine (plot.getCentreX(), plot.getY(), plot.getCentreX(), plot.getBottom(), 1.0f);
            g.drawLine (plot.getX(), plot.getCentreY(), plot.getRight(), plot.getCentreY(), 1.0f);
            g.setColour (Colors::outline.withAlpha (0.74f));
            g.drawRoundedRectangle (plot, 10.0f, 1.0f);
        }

        void drawParticles (juce::Graphics& g, juce::Rectangle<float> plot) const
        {
            for (const auto& particle : particles)
            {
                const float wobble = std::sin (particle.phase + grainVisual * 5.0f)
                                     * (3.0f + featureVisual * 7.0f);
                const float px = plot.getX() + particle.x * plot.getWidth();
                const float py = plot.getY() + particle.y * plot.getHeight() + wobble;
                const float intensity = particle.rumor ? rumorVisual : witnessVisual;
                const auto colour = particle.rumor ? Colors::accent2 : Colors::accent;
                g.setColour (colour.withAlpha (0.08f + intensity * 0.38f));
                const float size = particle.size * (0.75f + feedbackVisual * 1.5f);
                g.fillEllipse (px - size, py - size, size * 2.0f, size * 2.0f);
            }
        }

        void drawRibbons (juce::Graphics& g, juce::Rectangle<float> plot) const
        {
            auto makeRibbon = [this, plot] (bool rumorRibbon)
            {
                juce::Path path;
                const float strength = rumorRibbon ? rumorVisual : witnessVisual;
                const float polarity = rumorRibbon ? 1.0f : -1.0f;
                const float baseline = plot.getCentreY()
                                     + polarity * (22.0f + (arbiterVisual - 0.5f) * 28.0f);
                for (int i = 0; i <= 64; ++i)
                {
                    const float t = (float) i / 64.0f;
                    const float x = plot.getX() + t * plot.getWidth();
                    const float wave = std::sin (t * 17.0f + phase * (rumorRibbon ? 1.4f : 1.0f)
                                                 + polarity * featureVisual * 4.0f)
                                     + 0.42f * std::sin (t * 37.0f - phase * 0.7f);
                    const float y = baseline + wave * (3.0f + strength * 13.0f
                                                     + feedbackVisual * 6.0f);
                    if (i == 0) path.startNewSubPath (x, y); else path.lineTo (x, y);
                }
                return path;
            };

            const auto witnessPath = makeRibbon (false);
            const auto rumorPath = makeRibbon (true);
            const juce::PathStrokeType glowStroke (6.0f, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded);
            const juce::PathStrokeType lineStroke (1.8f, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded);
            g.setColour (Colors::accent.withAlpha (0.07f + witnessVisual * 0.14f));
            g.strokePath (witnessPath, glowStroke);
            g.setColour (Colors::accent.withAlpha (0.32f + witnessVisual * 0.58f));
            g.strokePath (witnessPath, lineStroke);
            g.setColour (Colors::accent2.withAlpha (0.07f + rumorVisual * 0.14f));
            g.strokePath (rumorPath, glowStroke);
            g.setColour (Colors::accent2.withAlpha (0.32f + rumorVisual * 0.58f));
            g.strokePath (rumorPath, lineStroke);
        }

        void drawTrail (juce::Graphics& g) const
        {
            for (int i = trailCount - 1; i >= 0; --i)
            {
                const float age = (float) (trailCount - i) / (float) juce::jmax (1, trailCount);
                const float radius = 2.0f + age * 4.5f;
                g.setColour (Colors::accent2.withAlpha (age * 0.16f));
                g.fillEllipse (trail[(size_t) i].x - radius, trail[(size_t) i].y - radius,
                               radius * 2.0f, radius * 2.0f);
            }
        }

        void drawArbiter (juce::Graphics& g, juce::Rectangle<float> plot) const
        {
            const float x = plot.getX() + arbiterVisual * plot.getWidth();
            const float featureY = plot.getBottom() - featureVisual * plot.getHeight();
            g.setColour (Colors::warning.withAlpha (0.20f));
            g.drawLine (x, plot.getY(), x, plot.getBottom(), 1.0f);
            g.setColour (Colors::warning.withAlpha (0.62f));
            g.fillEllipse (x - 3.0f, featureY - 3.0f, 6.0f, 6.0f);
            g.setColour (Colors::warning.withAlpha (0.14f));
            g.drawEllipse (x - 8.0f, featureY - 8.0f, 16.0f, 16.0f, 1.5f);
        }

        void drawHandle (juce::Graphics& g) const
        {
            const auto point = getHandlePoint();
            const auto colour = Colors::accent.interpolatedWith (Colors::accent2, verdict);
            const float pulse = 1.0f + feedbackVisual * 5.0f
                              + 1.5f * std::sin (phase * 2.0f);
            for (int i = 3; i >= 1; --i)
            {
                const float radius = 11.0f + pulse + (float) i * 4.0f;
                g.setColour (colour.withAlpha (0.035f * (float) i));
                g.fillEllipse (point.x - radius, point.y - radius,
                               radius * 2.0f, radius * 2.0f);
            }

            g.setColour (Colors::panelDark.withAlpha (0.92f));
            g.fillEllipse (point.x - 10.0f, point.y - 10.0f, 20.0f, 20.0f);
            g.setColour (colour);
            g.drawEllipse (point.x - 10.0f, point.y - 10.0f, 20.0f, 20.0f, 2.2f);
            g.setColour (Colors::text);
            g.fillEllipse (point.x - 2.3f, point.y - 2.3f, 4.6f, 4.6f);
        }

        void drawLabels (juce::Graphics& g, juce::Rectangle<float> plot) const
        {
            g.setFont (uiFont (9.0f, true));
            g.setColour (Colors::textDim);
            g.drawText ("WITNESS", juce::Rectangle<float> (plot.getX(), 5.0f, 80.0f, 18.0f),
                        juce::Justification::centredLeft, false);
            g.drawText ("RUMOR", juce::Rectangle<float> (plot.getRight() - 80.0f, 5.0f, 80.0f, 18.0f),
                        juce::Justification::centredRight, false);
            g.drawText ("DOUBT", juce::Rectangle<float> (2.0f, plot.getY(), 19.0f, 70.0f),
                        juce::Justification::centred, false);
            g.drawText ("CERTAIN", juce::Rectangle<float> (2.0f, plot.getBottom() - 70.0f, 19.0f, 70.0f),
                        juce::Justification::centred, false);

            auto footer = juce::Rectangle<float> (plot.getX(), plot.getBottom() + 4.0f,
                                                   plot.getWidth(), 18.0f);
            g.setFont (uiFont (8.5f));
            g.setColour (Colors::textDim.withAlpha (0.82f));
            g.drawText ("VERDICT  " + juce::String ((int) std::round (verdict * 100.0f))
                        + "%     DOUBT  " + juce::String ((int) std::round (doubt * 100.0f))
                        + "%     MEMORY  " + juce::String ((int) std::round (delayMs)) + " ms",
                        footer, juce::Justification::centred, true);
        }

        juce::RangedAudioParameter& verdictParam;
        juce::RangedAudioParameter& doubtParam;
        float verdict = 0.5f;
        float doubt = 0.5f;
        float witnessTarget = 0.0f;
        float rumorTarget = 0.0f;
        float arbiterTarget = 0.5f;
        float featureTarget = 0.0f;
        float feedbackTarget = 0.0f;
        float grainTarget = 0.0f;
        float witnessVisual = 0.0f;
        float rumorVisual = 0.0f;
        float arbiterVisual = 0.5f;
        float featureVisual = 0.0f;
        float feedbackVisual = 0.0f;
        float grainVisual = 0.0f;
        float delayMs = 0.0f;
        float phase = 0.0f;
        bool dragging = false;
        std::array<Particle, 46> particles;
        std::array<juce::Point<float>, 22> trail {};
        int trailCount = 0;
        juce::ParameterAttachment verdictAttachment;
        juce::ParameterAttachment doubtAttachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VerdictField)
    };
}

class EquivocalEditor final : public juce::AudioProcessorEditor,
                              private juce::Timer
{
public:
    explicit EquivocalEditor (EquivocalProcessor& processor)
        : juce::AudioProcessorEditor (processor),
          proc (processor),
          verdictField (*proc.verdict, *proc.doubt),
          listenSelector ("LISTENING FOR", *proc.listen),
          clockSelector ("MEMORY CLOCK", *proc.clock),
          previousPreset (RailButton::Kind::previous),
          nextPreset (RailButton::Kind::next),
          presetRail (RailButton::Kind::preset),
          freezeAttachment (*proc.freeze, freezeLatch)
    {
        setOpaque (true);
        setResizable (false, false);

        const std::array<juce::String, 19> names
        {
            "INPUT", "MIX", "OUTPUT",
            "UNCERTAIN", "NERVE", "MEMORY",
            "FEEDBACK", "CONTRADICT", "ORBIT",
            "TONE", "DRIVE", "FOCUS",
            "GRAIN", "REVERSE", "PITCH", "SCATTER",
            "DIFFUSE", "WIDTH", "MERCY"
        };

        const std::array<juce::RangedAudioParameter*, 19> parameters
        {
            proc.input, proc.mix, proc.output,
            proc.uncertainty, proc.nerve, proc.memory,
            proc.feedback, proc.contradiction, proc.orbit,
            proc.tone, proc.drive, proc.focus,
            proc.grain, proc.reverse, proc.pitch, proc.scatter,
            proc.diffuse, proc.width, proc.mercy
        };

        const std::array<juce::Colour, 19> colours
        {
            Colors::accent, Colors::accent2, Colors::accent,
            Colors::warning, Colors::accent2, Colors::accent,
            Colors::accent2, Colors::warning, Colors::accent,
            Colors::accent, Colors::accent2, Colors::warning,
            Colors::accent, Colors::accent2, Colors::warning, Colors::accent2,
            Colors::accent, Colors::accent, Colors::warning
        };

        sliderAttachments.reserve (knobs.size());
        for (size_t i = 0; i < knobs.size(); ++i)
        {
            knobs[i].configure (names[i], *parameters[i], colours[i], knobLookAndFeel);
            addAndMakeVisible (knobs[i]);
            sliderAttachments.push_back (
                std::make_unique<juce::SliderParameterAttachment> (*parameters[i], knobs[i].knob));
        }

        addAndMakeVisible (verdictField);
        addAndMakeVisible (listenSelector);
        addAndMakeVisible (clockSelector);
        addAndMakeVisible (freezeLatch);
        addAndMakeVisible (previousPreset);
        addAndMakeVisible (nextPreset);
        addAndMakeVisible (presetRail);
        addAndMakeVisible (headerTelemetry);

        previousPreset.onClick = [this] { stepProgram (-1); };
        nextPreset.onClick = [this] { stepProgram (1); };
        presetRail.onClick = [this] { showProgramMenu(); };

        displayedProgram = validProgramIndex (proc.getCurrentProgram());
        refreshProgramRail();

        setSize (900, 600);
        startTimerHz (45);
    }

    ~EquivocalEditor() override
    {
        stopTimer();
    }

    void paint (juce::Graphics& g) override
    {
        juce::ColourGradient background (Colors::windowBg.darker (0.28f),
                                         0.0f, 0.0f,
                                         Colors::panelDark.darker (0.40f),
                                         0.0f, (float) getHeight(), false);
        g.setGradientFill (background);
        g.fillAll();

        for (int i = 0; i < 7; ++i)
        {
            const float radius = 34.0f + (float) i * 32.0f;
            const float x = (float) getWidth() * 0.51f
                          + std::sin (backgroundPhase * 0.7f + (float) i) * 18.0f;
            const float y = (float) getHeight() * 0.47f
                          + std::cos (backgroundPhase * 0.5f + (float) i) * 13.0f;
            g.setColour ((i % 2 == 0 ? Colors::accent : Colors::accent2)
                         .withAlpha (0.012f + (float) i * 0.002f));
            g.drawEllipse (x - radius, y - radius, radius * 2.0f, radius * 2.0f, 1.0f);
        }

        paintPanel (g, headerBounds, 12.0f);
        paintPanel (g, leftPanel, 12.0f);
        paintPanel (g, centrePanel, 12.0f);
        paintPanel (g, rightPanel, 12.0f);

        paintBrand (g);
        paintSectionTitle (g, leftPanel, "ARGUMENT", "gain, behavior & disputed memory",
                           Colors::accent);
        paintSectionTitle (g, centrePanel, "VERDICT FIELD", "drag the testimony between truth and doubt",
                           Colors::accent2);
        paintSectionTitle (g, rightPanel, "AFTERIMAGE", "granular material, space & restraint",
                           Colors::warning);
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced (8);
        headerBounds = bounds.removeFromTop (58);
        bounds.removeFromTop (8);

        leftPanel = bounds.removeFromLeft (218);
        bounds.removeFromLeft (8);
        rightPanel = bounds.removeFromRight (218);
        bounds.removeFromRight (8);
        centrePanel = bounds;

        layoutHeader();
        layoutLeftControls();
        layoutCentre();
        layoutRightControls();
    }

private:
    int validProgramIndex (int index) const
    {
        const int count = proc.getNumPrograms();
        return count > 0 ? juce::jlimit (0, count - 1, index) : -1;
    }

    void stepProgram (int delta)
    {
        const int count = proc.getNumPrograms();
        if (count <= 0)
            return;
        int base = validProgramIndex (proc.getCurrentProgram());
        if (base < 0)
            base = 0;
        applyProgram ((base + delta + count) % count);
    }

    void applyProgram (int index)
    {
        const int valid = validProgramIndex (index);
        if (valid < 0)
            return;
        proc.setCurrentProgram (valid);
        displayedProgram = validProgramIndex (proc.getCurrentProgram());
        refreshProgramRail();
    }

    void refreshProgramRail()
    {
        if (displayedProgram < 0)
        {
            presetRail.setPreset ("No testimony", "Empty");
            return;
        }
        presetRail.setPreset (proc.getProgramName (displayedProgram),
                              proc.getProgramCategory (displayedProgram));
    }

    void showProgramMenu()
    {
        juce::StringArray categories = EquivocalProcessor::getProgramCategories();
        for (int i = 0; i < proc.getNumPrograms(); ++i)
        {
            const auto category = proc.getProgramCategory (i);
            if (! categories.contains (category))
                categories.add (category);
        }

        juce::PopupMenu menu;
        for (const auto& category : categories)
        {
            juce::PopupMenu categoryMenu;
            bool populated = false;
            for (int i = 0; i < proc.getNumPrograms(); ++i)
            {
                if (proc.getProgramCategory (i) != category)
                    continue;
                categoryMenu.addItem (i + 1, proc.getProgramName (i), true,
                                      i == validProgramIndex (proc.getCurrentProgram()));
                populated = true;
            }
            if (populated)
                menu.addSubMenu (category, categoryMenu);
        }

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&presetRail),
            [safe = juce::Component::SafePointer<EquivocalEditor> (this)] (int result)
            {
                if (safe != nullptr && result > 0)
                    safe->applyProgram (result - 1);
            });
    }

    void timerCallback() override
    {
        const float witness = clamp01 (proc.visualWitnessRms.load (std::memory_order_relaxed) * 4.0f);
        const float rumor = clamp01 (proc.visualRumorRms.load (std::memory_order_relaxed) * 4.0f);
        const float loop = clamp01 (proc.visualFeedbackRms.load (std::memory_order_relaxed) * 3.0f);
        const float arbiter = clamp01 (proc.visualArbiter.load (std::memory_order_relaxed));
        const float feature = clamp01 (proc.visualFeature.load (std::memory_order_relaxed));
        const float grain = clamp01 (proc.visualGrainPhase.load (std::memory_order_relaxed));
        const float delay = proc.visualDelayMs.load (std::memory_order_relaxed);
        const float mercy = clamp01 (proc.visualMercyGain.load (std::memory_order_relaxed));

        verdictField.setTelemetry (witness, rumor, arbiter, feature, loop, grain, delay);
        verdictField.advance();
        headerTelemetry.setState (witness, rumor, loop, mercy);
        freezeLatch.setEnergy (loop);
        backgroundPhase += 0.018f + loop * 0.028f;

        const int program = validProgramIndex (proc.getCurrentProgram());
        if (program != displayedProgram)
        {
            displayedProgram = program;
            refreshProgramRail();
        }
        repaint();
    }

    void layoutHeader()
    {
        auto header = headerBounds.reduced (8, 6);
        brandBounds = header.removeFromLeft (224);
        header.removeFromLeft (10);
        auto telemetryArea = header.removeFromRight (202);
        header.removeFromRight (10);
        headerTelemetry.setBounds (telemetryArea);

        previousPreset.setBounds (header.removeFromLeft (30).reduced (0, 4));
        header.removeFromLeft (5);
        nextPreset.setBounds (header.removeFromRight (30).reduced (0, 4));
        header.removeFromRight (5);
        presetRail.setBounds (header);
    }

    void layoutLeftControls()
    {
        auto grid = leftPanel.reduced (6);
        grid.removeFromTop (36);
        const int cellWidth = grid.getWidth() / 3;
        const int cellHeight = grid.getHeight() / 4;
        for (int row = 0; row < 4; ++row)
        {
            for (int column = 0; column < 3; ++column)
            {
                const int index = row * 3 + column;
                auto cell = juce::Rectangle<int> (grid.getX() + column * cellWidth,
                                                  grid.getY() + row * cellHeight,
                                                  column == 2 ? grid.getRight() - (grid.getX() + column * cellWidth)
                                                              : cellWidth,
                                                  row == 3 ? grid.getBottom() - (grid.getY() + row * cellHeight)
                                                           : cellHeight);
                knobs[(size_t) index].setBounds (cell.reduced (1));
            }
        }
    }

    void layoutCentre()
    {
        auto area = centrePanel.reduced (7);
        area.removeFromTop (36);
        auto controls = area.removeFromBottom (64);
        area.removeFromBottom (7);
        verdictField.setBounds (area);

        const int selectorWidth = juce::jmax (100, (controls.getWidth() - 12) * 27 / 100);
        listenSelector.setBounds (controls.removeFromLeft (selectorWidth));
        controls.removeFromLeft (6);
        clockSelector.setBounds (controls.removeFromLeft (selectorWidth));
        controls.removeFromLeft (6);
        freezeLatch.setBounds (controls.reduced (0, 4));
    }

    void layoutRightControls()
    {
        auto grid = rightPanel.reduced (6);
        grid.removeFromTop (36);
        const int cellWidth = grid.getWidth() / 2;
        const int cellHeight = grid.getHeight() / 4;
        for (int localIndex = 0; localIndex < 7; ++localIndex)
        {
            const int row = localIndex / 2;
            const int column = localIndex % 2;
            auto cell = juce::Rectangle<int> (grid.getX() + column * cellWidth,
                                              grid.getY() + row * cellHeight,
                                              column == 1 ? grid.getRight() - (grid.getX() + column * cellWidth)
                                                          : cellWidth,
                                              row == 3 ? grid.getBottom() - (grid.getY() + row * cellHeight)
                                                       : cellHeight);
            if (localIndex == 6)
                cell = juce::Rectangle<int> (grid.getCentreX() - cellWidth / 2,
                                              grid.getY() + 3 * cellHeight,
                                              cellWidth, grid.getBottom() - (grid.getY() + 3 * cellHeight));
            knobs[(size_t) (12 + localIndex)].setBounds (cell.reduced (2, 1));
        }
    }

    void paintBrand (juce::Graphics& g) const
    {
        auto area = brandBounds;
        auto mark = area.removeFromLeft (42).toFloat().reduced (2.0f);
        g.setColour (Colors::accent.withAlpha (0.17f));
        g.fillEllipse (mark);
        g.setColour (Colors::accent2.withAlpha (0.86f));
        g.drawEllipse (mark.reduced (4.0f), 2.0f);
        g.setColour (Colors::accent);
        g.fillEllipse (mark.getCentreX() - 3.5f, mark.getCentreY() - 3.5f, 7.0f, 7.0f);
        g.setColour (Colors::accent.withAlpha (0.42f));
        g.drawLine (mark.getX() + 4.0f, mark.getCentreY(), mark.getRight() - 4.0f,
                    mark.getCentreY(), 1.0f);
        g.drawLine (mark.getCentreX(), mark.getY() + 4.0f, mark.getCentreX(),
                    mark.getBottom() - 4.0f, 1.0f);

        area.removeFromLeft (7);
        auto title = area.removeFromTop (27);
        g.setFont (uiFont (20.0f, true));
        g.setColour (Colors::text);
        g.drawText ("EQUIVOCAL", title, juce::Justification::centredLeft, false);
        g.setFont (uiFont (8.5f, true));
        g.setColour (Colors::accent2.withAlpha (0.92f));
        g.drawText ("AUDIO, UNDER QUESTION", area,
                    juce::Justification::centredLeft, false);
    }

    static void paintSectionTitle (juce::Graphics& g, juce::Rectangle<int> panel,
                                   const juce::String& title, const juce::String& subtitle,
                                   juce::Colour colour)
    {
        auto header = panel.reduced (13, 5).removeFromTop (27);
        auto titleArea = header.removeFromLeft (juce::jmin (100, header.getWidth()));
        g.setFont (uiFont (10.0f, true));
        g.setColour (colour);
        g.drawText (title, titleArea, juce::Justification::centredLeft, true);
        g.setFont (uiFont (7.6f));
        g.setColour (Colors::textDim.withAlpha (0.86f));
        g.drawText (subtitle, header, juce::Justification::centredRight, true);
    }

    EquivocalProcessor& proc;
    EquivocalKnobLookAndFeel knobLookAndFeel;
    std::array<KnobTile, 19> knobs;
    VerdictField verdictField;
    ChoiceSelector listenSelector;
    ChoiceSelector clockSelector;
    FreezeLatch freezeLatch;
    RailButton previousPreset;
    RailButton nextPreset;
    RailButton presetRail;
    HeaderTelemetry headerTelemetry;
    std::vector<std::unique_ptr<juce::SliderParameterAttachment>> sliderAttachments;
    juce::ButtonParameterAttachment freezeAttachment;
    int displayedProgram = -1;
    float backgroundPhase = 0.0f;
    juce::Rectangle<int> headerBounds;
    juce::Rectangle<int> brandBounds;
    juce::Rectangle<int> leftPanel;
    juce::Rectangle<int> centrePanel;
    juce::Rectangle<int> rightPanel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EquivocalEditor)
};

juce::AudioProcessorEditor* EquivocalProcessor::createEditor()
{
    return new EquivocalEditor (*this);
}

