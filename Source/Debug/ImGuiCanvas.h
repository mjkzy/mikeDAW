#pragma once
#include <JuceHeader.h>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

class ImGuiCanvas : public juce::Component,
                    public juce::OpenGLRenderer
{
public:
    ImGuiCanvas();
    ~ImGuiCanvas() override;

    std::function<void()> onFrame;

    const char* clipboardTextForImGui();

    void newOpenGLContextCreated() override;
    void renderOpenGL() override;
    void openGLContextClosing() override;

    void paint (juce::Graphics&) override {}

    void mouseMove (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override;
    bool keyStateChanged (bool isKeyDown) override;
    void modifierKeysChanged (const juce::ModifierKeys&) override;
    void focusGained (FocusChangeType) override;
    void focusLost (FocusChangeType) override;

private:
    enum class EventType
    {
        mousePos, mouseButton, mouseWheel, mouseLeave,
        key, character, modifiers, focus
    };

    struct InputEvent
    {
        EventType type;
        float     x = 0.0f, y = 0.0f;
        int       button = 0;
        bool      down = false;
        int       keyCode = 0;
        unsigned  character = 0;
        bool      ctrl = false, shift = false, alt = false;
    };

    void pushEvent (const InputEvent&);
    void pushMouse (const juce::MouseEvent&);
    void pushModifiers (const juce::ModifierKeys&);
    void releaseMouseButtons();

    juce::OpenGLContext openGLContext;

    std::mutex eventMutex;
    std::vector<InputEvent> eventQueue;

    double lastRenderTime = 0.0;
    std::string clipboardText;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ImGuiCanvas)
};
