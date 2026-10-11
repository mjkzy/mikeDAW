#include "ImGuiCanvas.h"
#include <imgui.h>
#include <backends/imgui_impl_opengl3.h>

namespace
{
    int juceKeyToImGui (const juce::KeyPress& key)
    {
        const int code = key.getKeyCode();
        const juce_wchar tc = juce::CharacterFunctions::toLowerCase ((juce_wchar) key.getTextCharacter());

        if (tc >= 'a' && tc <= 'z') return ImGuiKey_A + (int) (tc - 'a');
        if (code >= '0' && code <= '9') return ImGuiKey_0 + (code - '0');

        using KP = juce::KeyPress;
        if (code == KP::leftKey)      return ImGuiKey_LeftArrow;
        if (code == KP::rightKey)     return ImGuiKey_RightArrow;
        if (code == KP::upKey)        return ImGuiKey_UpArrow;
        if (code == KP::downKey)      return ImGuiKey_DownArrow;
        if (code == KP::homeKey)      return ImGuiKey_Home;
        if (code == KP::endKey)       return ImGuiKey_End;
        if (code == KP::pageUpKey)    return ImGuiKey_PageUp;
        if (code == KP::pageDownKey)  return ImGuiKey_PageDown;
        if (code == KP::returnKey)    return ImGuiKey_Enter;
        if (code == KP::escapeKey)    return ImGuiKey_Escape;
        if (code == KP::backspaceKey) return ImGuiKey_Backspace;
        if (code == KP::deleteKey)    return ImGuiKey_Delete;
        if (code == KP::tabKey)       return ImGuiKey_Tab;
        if (code == KP::spaceKey)     return ImGuiKey_Space;
        if (code == KP::insertKey)    return ImGuiKey_Insert;

        for (int f = 0; f < 12; ++f)
            if (code == KP::F1Key + f)
                return ImGuiKey_F1 + f;

        return ImGuiKey_None;
    }

    const char* clipboardGet (ImGuiContext*)
    {
        auto* self = static_cast<ImGuiCanvas*> (ImGui::GetPlatformIO().Platform_ClipboardUserData);
        return self != nullptr ? self->clipboardTextForImGui() : "";
    }

    void clipboardSet (ImGuiContext*, const char* text)
    {
        juce::SystemClipboard::copyTextToClipboard (juce::String::fromUTF8 (text));
    }
}

const char* ImGuiCanvas::clipboardTextForImGui()
{
    clipboardText = juce::SystemClipboard::getTextFromClipboard().toStdString();
    return clipboardText.c_str();
}

ImGuiCanvas::ImGuiCanvas()
{
    setWantsKeyboardFocus (true);
    setOpaque (true);

    openGLContext.setOpenGLVersionRequired (juce::OpenGLContext::openGL3_2);
    openGLContext.setRenderer (this);
    openGLContext.setComponentPaintingEnabled (false);
    openGLContext.setContinuousRepainting (true);
    openGLContext.setSwapInterval (1);
    openGLContext.attachTo (*this);
}

ImGuiCanvas::~ImGuiCanvas()
{
    openGLContext.detach();
}

void ImGuiCanvas::newOpenGLContextCreated()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    const ImVec4 accent (0.937f, 0.612f, 0.302f, 1.0f);
    style.Colors[ImGuiCol_CheckMark]        = accent;
    style.Colors[ImGuiCol_SliderGrab]       = accent;
    style.Colors[ImGuiCol_SliderGrabActive] = accent;
    style.Colors[ImGuiCol_HeaderHovered]    = ImVec4 (accent.x, accent.y, accent.z, 0.6f);
    style.Colors[ImGuiCol_TabHovered]       = accent;

    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    pio.Platform_ClipboardUserData = this;
    pio.Platform_GetClipboardTextFn = &clipboardGet;
    pio.Platform_SetClipboardTextFn = &clipboardSet;

    ImGui_ImplOpenGL3_Init ("#version 150");

    lastRenderTime = 0.0;
}

void ImGuiCanvas::renderOpenGL()
{
    juce::OpenGLHelpers::clear (juce::Colour (0xff141414));

    ImGuiIO& io = ImGui::GetIO();

    const double scale = openGLContext.getRenderingScale();
    io.DisplaySize = ImVec2 ((float) (getWidth() * scale), (float) (getHeight() * scale));
    io.DisplayFramebufferScale = ImVec2 (1.0f, 1.0f);

    const double now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    double dt = (lastRenderTime > 0.0) ? (now - lastRenderTime) : (1.0 / 60.0);
    dt = juce::jlimit (1.0 / 1000.0, 1.0 / 15.0, dt);
    io.DeltaTime = (float) dt;
    lastRenderTime = now;

    {
        std::vector<InputEvent> events;
        {
            std::lock_guard<std::mutex> lock (eventMutex);
            events.swap (eventQueue);
        }

        for (const auto& e : events)
        {
            switch (e.type)
            {
                case EventType::mousePos:
                    io.AddMousePosEvent ((float) (e.x * scale), (float) (e.y * scale));
                    break;
                case EventType::mouseButton:
                    io.AddMouseButtonEvent (e.button, e.down);
                    break;
                case EventType::mouseWheel:
                    io.AddMouseWheelEvent (e.x, e.y);
                    break;
                case EventType::mouseLeave:
                    io.AddMousePosEvent (-FLT_MAX, -FLT_MAX);
                    break;
                case EventType::key:
                    io.AddKeyEvent ((ImGuiKey) e.keyCode, e.down);
                    break;
                case EventType::character:
                    io.AddInputCharacter (e.character);
                    break;
                case EventType::modifiers:
                    io.AddKeyEvent (ImGuiMod_Ctrl,  e.ctrl);
                    io.AddKeyEvent (ImGuiMod_Shift, e.shift);
                    io.AddKeyEvent (ImGuiMod_Alt,   e.alt);
                    break;
                case EventType::focus:
                    io.AddFocusEvent (e.down);
                    break;
            }
        }
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();

    if (onFrame)
        onFrame();

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData (ImGui::GetDrawData());
}

void ImGuiCanvas::openGLContextClosing()
{
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
}

void ImGuiCanvas::pushEvent (const InputEvent& e)
{
    std::lock_guard<std::mutex> lock (eventMutex);
    eventQueue.push_back (e);
}

void ImGuiCanvas::pushMouse (const juce::MouseEvent& e)
{
    const auto position = e.position;
    InputEvent ev;
    ev.type = EventType::mousePos;
    ev.x = position.x;
    ev.y = position.y;
    pushEvent (ev);
}

void ImGuiCanvas::pushModifiers (const juce::ModifierKeys& m)
{
    InputEvent ev;
    ev.type = EventType::modifiers;
    ev.ctrl = m.isCtrlDown();
    ev.shift = m.isShiftDown();
    ev.alt = m.isAltDown();
    pushEvent (ev);
}

void ImGuiCanvas::releaseMouseButtons()
{
    for (int button = 0; button < 3; ++button)
    {
        InputEvent ev;
        ev.type = EventType::mouseButton;
        ev.button = button;
        ev.down = false;
        pushEvent (ev);
    }
}

void ImGuiCanvas::mouseMove (const juce::MouseEvent& e) { pushMouse (e); }

void ImGuiCanvas::mouseEnter (const juce::MouseEvent& e) { pushMouse (e); }

void ImGuiCanvas::mouseExit (const juce::MouseEvent&)
{
    InputEvent ev;
    ev.type = EventType::mouseLeave;
    pushEvent (ev);
}

void ImGuiCanvas::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    pushMouse (e);

    const auto& m = e.mods;
    InputEvent leftButton; leftButton.type = EventType::mouseButton; leftButton.button = 0; leftButton.down = m.isLeftButtonDown();   pushEvent (leftButton);
    InputEvent rightButton; rightButton.type = EventType::mouseButton; rightButton.button = 1; rightButton.down = m.isRightButtonDown();  pushEvent (rightButton);
    InputEvent middleButton; middleButton.type = EventType::mouseButton; middleButton.button = 2; middleButton.down = m.isMiddleButtonDown(); pushEvent (middleButton);
}

void ImGuiCanvas::mouseDrag (const juce::MouseEvent& e) { pushMouse (e); }

void ImGuiCanvas::mouseUp (const juce::MouseEvent& e)
{
    pushMouse (e);
    releaseMouseButtons();
}

void ImGuiCanvas::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    InputEvent ev;
    ev.type = EventType::mouseWheel;
    ev.x = w.deltaX * 5.0f;
    ev.y = w.deltaY * 5.0f;
    pushEvent (ev);
}

bool ImGuiCanvas::keyPressed (const juce::KeyPress& key)
{
    pushModifiers (key.getModifiers());

    const int imKey = juceKeyToImGui (key);
    if (imKey != ImGuiKey_None)
    {
        InputEvent down; down.type = EventType::key; down.keyCode = imKey; down.down = true;  pushEvent (down);
        InputEvent up;   up.type   = EventType::key; up.keyCode   = imKey; up.down   = false; pushEvent (up);
    }

    const juce_wchar tc = key.getTextCharacter();
    if (tc != 0 && tc >= 32 && tc != 127)
    {
        InputEvent ch; ch.type = EventType::character; ch.character = (unsigned) tc; pushEvent (ch);
    }

    return true;
}

bool ImGuiCanvas::keyStateChanged (bool) { return false; }

void ImGuiCanvas::modifierKeysChanged (const juce::ModifierKeys& m) { pushModifiers (m); }

void ImGuiCanvas::focusGained (FocusChangeType)
{
    InputEvent ev; ev.type = EventType::focus; ev.down = true; pushEvent (ev);
}

void ImGuiCanvas::focusLost (FocusChangeType)
{
    releaseMouseButtons();
    InputEvent ev; ev.type = EventType::focus; ev.down = false; pushEvent (ev);
}
