#pragma once
#include <JuceHeader.h>

class ValueSliderBase : public juce::Slider
{
public:
    std::function<void()> onCreateAutomationClip;

    void setDoubleClickReturnValue (bool isEnabled, double value,
                                    juce::ModifierKeys mods = juce::ModifierKeys())
    {
        hasDefaultValue = isEnabled;
        defaultValue = value;
        juce::Slider::setDoubleClickReturnValue (isEnabled, value, mods);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown())
            return;
        if (e.mods.isPopupMenu())
        {
            juce::PopupMenu m;
            auto clip = juce::SystemClipboard::getTextFromClipboard().trim();
            m.addItem (1, "Copy Value");
            m.addItem (2, "Paste Value", clip.isNotEmpty());
            m.addItem (3, "Set Value...");
            m.addItem (4, "Reset Value to Default", hasDefaultValue);
            if (onCreateAutomationClip)
            {
                m.addSeparator();
                m.addItem (5, "Create automation clip");
            }
            m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(),
                [safe = juce::Component::SafePointer<ValueSliderBase> (this)] (int r)
                {
                    if (safe == nullptr)
                        return;
                    if (r == 1)
                        juce::SystemClipboard::copyTextToClipboard (juce::String (safe->getValue(), 4));
                    else if (r == 2)
                        safe->pasteValueFromClipboard();
                    else if (r == 3)
                        safe->showSetValueDialog();
                    else if (r == 4)
                        safe->setValue (juce::jlimit (safe->getMinimum(), safe->getMaximum(), safe->defaultValue),
                                        juce::sendNotificationSync);
                    else if (r == 5 && safe->onCreateAutomationClip)
                        safe->onCreateAutomationClip();
                });
            return;
        }
        juce::Slider::mouseDown (e);
        if (isEnabled() && ! e.source.isUnboundedMouseMovementEnabled())
            e.source.enableUnboundedMouseMovement (true, false);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        const bool wasUnbounded = e.source.isUnboundedMouseMovementEnabled();
        juce::Slider::mouseUp (e);
        if (wasUnbounded)
        {
            auto src = e.source;
            src.enableUnboundedMouseMovement (false);

            auto target = e.getMouseDownScreenPosition().toFloat();
            if (isVertical() || getSliderStyle() == juce::Slider::LinearBarVertical)
                target = localPointToGlobal (juce::Point<float> ((float) getWidth() * 0.5f,
                                                                 (float) getPositionOfValue (getValue())));
            else if (isHorizontal() || getSliderStyle() == juce::Slider::LinearBar)
                target = localPointToGlobal (juce::Point<float> ((float) getPositionOfValue (getValue()),
                                                                 (float) getHeight() * 0.5f));

            src.setScreenPosition (target);
        }
    }

protected:
    void pasteValueFromClipboard()
    {
        auto txt = juce::SystemClipboard::getTextFromClipboard().trim();
        if (txt.isNotEmpty())
            setValueFromText (txt);
    }

    void setValueFromText (const juce::String& txt)
    {
        setValue (juce::jlimit (getMinimum(), getMaximum(), getValueFromText (txt)),
                  juce::sendNotificationSync);
    }

    void showSetValueDialog()
    {
        auto* w = new juce::AlertWindow ("Set Value",
                                         "Enter a value between " + juce::String (getMinimum(), 2)
                                         + " and " + juce::String (getMaximum(), 2) + ":",
                                         juce::MessageBoxIconType::NoIcon);
        w->addTextEditor ("value", juce::String (getValue(), 4));
        w->addButton ("Set", 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

        w->enterModalState (true, juce::ModalCallbackFunction::create (
            [safe = juce::Component::SafePointer<ValueSliderBase> (this), w] (int result)
            {
                if (safe != nullptr && result == 1)
                {
                    auto txt = w->getTextEditorContents ("value").trim();
                    if (txt.isNotEmpty())
                        safe->setValueFromText (txt);
                }
            }), true);
    }

    bool hasDefaultValue = false;
    double defaultValue = 0.0;
};

class ValueKnob : public ValueSliderBase
{
public:
    ValueKnob()
    {
        setSliderStyle (juce::Slider::RotaryVerticalDrag);
        setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ValueKnob)
};

class ValueSlider : public ValueSliderBase
{
public:
    ValueSlider()
    {
        setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ValueSlider)
};
