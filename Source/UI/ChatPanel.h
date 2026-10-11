#pragma once
#include <JuceHeader.h>
#include "AppLookAndFeel.h"
#include "GifImage.h"
#include <deque>
#include <memory>

class ChatPanel : public juce::Component,
                  public juce::FileDragAndDropTarget,
                  private juce::Timer
{
public:
    explicit ChatPanel (juce::PropertiesFile* settingsFile = nullptr)
        : settings (settingsFile)
    {
        if (settings != nullptr)
        {
            chatWidth = juce::jlimit (minWidth, maxWidth, settings->getIntValue ("chatWidth", 340));
            dockRight = settings->getBoolValue ("chatDockRight", true);
        }

        viewport.setViewedComponent (&content, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);
        content.owner = this;
        viewport.addMouseListener (this, true);

        input.setMultiLine (true, true);
        input.setReturnKeyStartsNewLine (false);
        input.setFont (Typography::ui (14.0f));
        applyInputColours();
        input.onReturnKey = [this] { sendCurrentText(); };
        addAndMakeVisible (input);

        attachBtn.setButtonText ("+");
        attachBtn.setTooltip ("Attach an image");
        attachBtn.onClick = [this] { chooseImage(); };
        addAndMakeVisible (attachBtn);

        sendBtn.setButtonText ("Send");
        sendBtn.onClick = [this] { sendCurrentText(); };
        addAndMakeVisible (sendBtn);

        swapBtn.setTooltip ("Dock the chat on the other side");
        swapBtn.onClick = [this] { setDockedRight (! dockRight); };
        addAndMakeVisible (swapBtn);

        closeBtn.setTooltip ("Close the chat");
        closeBtn.onClick = [this] { if (onCloseRequested) onCloseRequested(); };
        addAndMakeVisible (closeBtn);
    }

    void lookAndFeelChanged() override { applyInputColours(); }

    std::function<juce::Image (juce::int64)> getAvatar;

    std::function<void (const juce::String& text)> onSendText;
    std::function<void (const juce::MemoryBlock& encodedBytes)> onSendImageBytes;

    std::function<void (const juce::Image& image, const juce::MemoryBlock& bytes)> onImageClicked;

    std::function<juce::int64()> getLocalPeerId;

    std::function<void()> onWidthChanged;
    std::function<void()> onCloseRequested;

    int  getPreferredWidth() const noexcept { return chatWidth; }

    void setPreferredWidth (int w)
    {
        w = juce::jlimit (minWidth, maxWidth, w);
        if (w == chatWidth)
            return;
        chatWidth = w;
        if (settings != nullptr)
        {
            settings->setValue ("chatWidth", chatWidth);
            settings->saveIfNeeded();
        }
        if (onWidthChanged)
            onWidthChanged();
    }

    bool isDockedRight() const noexcept { return dockRight; }

    void setDockedRight (bool shouldDockRight)
    {
        if (shouldDockRight == dockRight)
            return;
        dockRight = shouldDockRight;
        if (settings != nullptr)
        {
            settings->setValue ("chatDockRight", dockRight);
            settings->saveIfNeeded();
        }
        resized();
        repaint();
        if (onWidthChanged)
            onWidthChanged();
    }

    void grabInputFocus() { input.grabKeyboardFocus(); }

    void addTextMessage (juce::int64 peerId, const juce::String& name, const juce::String& text)
    {
        Message m;
        m.peerId = peerId;
        m.name = name.isNotEmpty() ? name : juce::String ("A Kind Soul");
        m.colour = Colors::peerColour (peerId);
        m.timeMs = juce::Time::currentTimeMillis();
        m.time = juce::Time::getCurrentTime().toString (false, true, false, true);
        m.text = text;
        pushMessage (std::move (m));
    }

    void addImageAnnounce (juce::int64 peerId, const juce::String& name, juce::int64 imageId)
    {
        Message m;
        m.peerId = peerId;
        m.name = name.isNotEmpty() ? name : juce::String ("A Kind Soul");
        m.colour = Colors::peerColour (peerId);
        m.timeMs = juce::Time::currentTimeMillis();
        m.time = juce::Time::getCurrentTime().toString (false, true, false, true);
        m.isImage = true;
        m.imageId = imageId;
        m.loading = true;
        pushMessage (std::move (m));
    }

    void addSystemMessage (const juce::String& text)
    {
        Message m;
        m.isSystem = true;
        m.timeMs = juce::Time::currentTimeMillis();
        m.time = juce::Time::getCurrentTime().toString (false, true, false, true);
        m.text = text;
        pushMessage (std::move (m));
    }

    void addEventMessage (const juce::String& actor, juce::Colour actorColour, const juce::String& action)
    {
        Message m;
        m.isEvent = true;
        m.name = actor;
        m.colour = actorColour;
        m.timeMs = juce::Time::currentTimeMillis();
        m.time = juce::Time::getCurrentTime().toString (false, true, false, true);
        m.text = action;
        pushMessage (std::move (m));
    }

    void addImageData (juce::int64 imageId, const juce::Image& image, const juce::MemoryBlock& bytes)
    {
        const bool nearBottom = isNearBottom();
        for (auto& m : messages)
            if (m.isImage && m.imageId == imageId)
            {
                m.image = image;
                m.bytes = bytes;
                m.loading = false;
            }
        content.relayout();
        if (nearBottom)
            scrollToBottom();
        updateGifTimer();
        repaint();
    }

    void clearChat()
    {
        messages.clear();
        content.relayout();
        updateGifTimer();
        repaint();
    }

    void saveImage (const juce::Image& image, const juce::MemoryBlock& bytes)
    {
        const juce::String ext = bytes.getSize() > 0 ? extForBytes (bytes) : juce::String (".png");
        auto suggested = juce::File::getSpecialLocation (juce::File::userPicturesDirectory)
                             .getChildFile ("chat-image" + ext);
        saveChooser = std::make_unique<juce::FileChooser> ("Save image", suggested, "*" + ext);
        saveChooser->launchAsync (juce::FileBrowserComponent::saveMode
                                      | juce::FileBrowserComponent::canSelectFiles
                                      | juce::FileBrowserComponent::warnAboutOverwriting,
            [image, bytes, ext] (const juce::FileChooser& fc)
            {
                auto f = fc.getResult();
                if (f == juce::File())
                    return;
                if (f.getFileExtension().isEmpty())
                    f = f.withFileExtension (ext);
                if (bytes.getSize() > 0)
                {
                    f.replaceWithData (bytes.getData(), bytes.getSize());
                }
                else if (image.isValid())
                {
                    f.deleteFile();
                    juce::FileOutputStream os (f);
                    if (os.openedOk())
                    {
                        juce::PNGImageFormat png;
                        png.writeImageToStream (image, os);
                    }
                }
            });
    }

    void visibilityChanged() override { updateGifTimer(); }

    void resized() override
    {
        auto r = dockRight ? getLocalBounds().withTrimmedLeft (resizeEdgeW)
                           : getLocalBounds().withTrimmedRight (resizeEdgeW);

        auto head = r.removeFromTop (headerBarH).reduced (6, 3);
        closeBtn.setBounds (head.removeFromRight (24));
        head.removeFromRight (2);
        swapBtn.setBounds (head.removeFromRight (24));

        auto inputRow = r.removeFromBottom (inputRowH).reduced (7, 8);
        attachBtn.setBounds (inputRow.removeFromLeft (32));
        inputRow.removeFromLeft (5);
        sendBtn.setBounds (inputRow.removeFromRight (58));
        inputRow.removeFromRight (5);
        input.setBounds (inputRow);

        viewport.setBounds (r);
        content.relayout();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colors::panelBg);

        auto body = dockRight ? getLocalBounds().withTrimmedLeft (resizeEdgeW)
                              : getLocalBounds().withTrimmedRight (resizeEdgeW);

        auto head = body.removeFromTop (headerBarH);
        g.setColour (Colors::header);
        g.fillRect (head);

        g.setColour (Colors::text);
        g.setFont (Typography::caps (13.5f));
        g.drawText ("CHAT", head.getX() + Metrics::pad + 2, head.getY(), 120, head.getHeight(),
                    juce::Justification::centredLeft);

        if (! input.getBounds().isEmpty())
        {
            auto well = input.getBounds().toFloat().expanded ((float) Metrics::padS, 3.0f);
            g.setColour (input.hasKeyboardFocus (true) ? Colors::panelDark.brighter (0.06f)
                                                       : Colors::panelDark);
            g.fillRoundedRectangle (well, 8.0f);
        }

        paintResizeEdge (g);

        if (dragHover)
        {
            g.setColour (Colors::accent.withAlpha (0.8f));
            g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (2.0f), Metrics::rL, 2.0f);
        }
    }

    void focusOfChildComponentChanged (FocusChangeType) override { repaint(); }

    void mouseMove (const juce::MouseEvent& e) override
    {
        setMouseCursor (overResizeEdge (e.getEventRelativeTo (this).getPosition())
                        ? juce::MouseCursor::LeftRightResizeCursor
                        : juce::MouseCursor::NormalCursor);
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (! resizing)
            setMouseCursor (juce::MouseCursor::NormalCursor);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown())
            return;
        if (! overResizeEdge (e.getEventRelativeTo (this).getPosition()))
            return;
        resizing = true;
        dragStartWidth = chatWidth;
        dragStartX = e.getScreenX();
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! resizing || e.mods.isMiddleButtonDown())
            return;
        const int delta = e.getScreenX() - dragStartX;
        setPreferredWidth (dockRight ? dragStartWidth - delta : dragStartWidth + delta);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! resizing)
            return;
        resizing = false;
        setMouseCursor (overResizeEdge (e.getEventRelativeTo (this).getPosition())
                        ? juce::MouseCursor::LeftRightResizeCursor
                        : juce::MouseCursor::NormalCursor);
        repaint();
    }

    bool isInterestedInFileDrag (const juce::StringArray& files) override
    {
        for (auto& f : files)
            if (isImageFile (f))
                return true;
        return false;
    }

    void fileDragEnter (const juce::StringArray&, int, int) override { dragHover = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override            { dragHover = false; repaint(); }

    void filesDropped (const juce::StringArray& files, int, int) override
    {
        dragHover = false;
        repaint();
        for (auto& f : files)
            if (isImageFile (f))
                sendImageFile (juce::File (f));
    }

private:
    class HeaderButton : public juce::Button
    {
    public:
        enum class Glyph { swapSide, close };

        HeaderButton (const juce::String& name, Glyph g) : juce::Button (name), glyph (g) {}

        void paintButton (juce::Graphics& g, bool highlighted, bool down) override
        {
            auto a = getLocalBounds().toFloat().reduced (7.0f);
            g.setColour (highlighted || down ? Colors::text : Colors::textDim);

            if (glyph == Glyph::close)
            {
                g.drawLine (a.getX(), a.getY(), a.getRight(), a.getBottom(), 1.6f);
                g.drawLine (a.getX(), a.getBottom(), a.getRight(), a.getY(), 1.6f);
                return;
            }

            const float y1 = a.getCentreY() - 3.0f;
            const float y2 = a.getCentreY() + 3.0f;
            juce::Path p;
            p.startNewSubPath (a.getRight(), y1);
            p.lineTo (a.getX(), y1);
            p.lineTo (a.getX() + 3.0f, y1 - 3.0f);
            p.startNewSubPath (a.getX(), y2);
            p.lineTo (a.getRight(), y2);
            p.lineTo (a.getRight() - 3.0f, y2 + 3.0f);
            g.strokePath (p, juce::PathStrokeType (1.5f));
        }

    private:
        Glyph glyph;
    };

    void applyInputColours()
    {
        input.setTextToShowWhenEmpty ("Message the session", Colors::textDim);
        input.setColour (juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
        input.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        input.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
        input.setColour (juce::TextEditor::textColourId, Colors::text);
        input.setColour (juce::TextEditor::highlightColourId, Colors::accent.withAlpha (0.3f));
        input.setColour (juce::TextEditor::highlightedTextColourId, Colors::windowBg);
        input.setColour (juce::CaretComponent::caretColourId, Colors::accent);

        attachBtn.setColour (juce::TextButton::buttonColourId, Colors::raised);
        attachBtn.setColour (juce::TextButton::textColourOffId, Colors::textDim);
        sendBtn.setColour (juce::TextButton::buttonColourId, Colors::accent);
        sendBtn.setColour (juce::TextButton::textColourOffId, Colors::windowBg);
    }

    struct Message
    {
        juce::int64 peerId = 0;
        juce::int64 timeMs = 0;
        juce::String name, time, text;
        juce::Colour colour { Colors::text };
        bool isSystem = false;
        bool isEvent = false;
        bool isImage = false;
        juce::int64 imageId = 0;
        juce::Image image;
        juce::MemoryBlock bytes;
        bool loading = false;

        std::shared_ptr<GifImage> gif;
        bool triedGif = false;
        int gifFrame = 0;
        juce::uint32 nextFrameMs = 0;

        bool isAnimated() const noexcept { return gif != nullptr && gif->isAnimated(); }
    };

    static constexpr int resizeEdgeW = 6;
    static constexpr int minWidth = 260, maxWidth = 720;
    static constexpr int headerBarH = 30;
    static constexpr int inputRowH = 54;

    static constexpr int avatarD = 40;
    static constexpr int gutter = 56;
    static constexpr int headerH = 20;
    static constexpr int rowPad = 16;
    static constexpr int groupGap = 18;
    static constexpr int contGap = 3;
    static constexpr int sysGap = 5;
    static constexpr int maxImageH = 200;
    static constexpr size_t maxMessages = 500;

    static constexpr juce::int64 groupWindowMs = 5 * 60 * 1000;

    bool overResizeEdge (juce::Point<int> p) const
    {
        return dockRight ? (p.x >= 0 && p.x <= resizeEdgeW)
                         : (p.x >= getWidth() - resizeEdgeW && p.x <= getWidth());
    }

    void paintResizeEdge (juce::Graphics& g)
    {
        const float edgeX = dockRight ? resizeEdgeW * 0.5f
                                      : (float) getWidth() - resizeEdgeW * 0.5f;
        if (resizing)
        {
            g.setColour (Colors::accent.withAlpha (0.9f));
            g.fillRoundedRectangle (dockRight ? 0.0f : (float) getWidth() - 2.0f,
                                    0.0f, 2.0f, (float) getHeight(), 1.0f);
        }
        g.setColour (Colors::textDim.withAlpha (resizing ? 0.9f : 0.35f));
        const float cy = getHeight() * 0.5f;
        for (int i = -2; i <= 2; ++i)
            g.fillEllipse (edgeX - 1.0f, cy + i * 5.0f - 1.0f, 2.0f, 2.0f);
    }

    bool isContinuation (size_t idx) const
    {
        if (idx == 0 || idx >= messages.size())
            return false;
        const auto& cur = messages[idx];
        const auto& prev = messages[idx - 1];
        if (cur.isSystem || prev.isSystem || cur.isEvent || prev.isEvent)
            return false;
        if (cur.peerId != prev.peerId)
            return false;
        return (cur.timeMs - prev.timeMs) <= groupWindowMs;
    }

    struct RowMetrics
    {
        int rowH = 0;
        int bodyY = 0;
        int bodyH = 0;
        bool grouped = false;
        bool system = false;
        bool event = false;
    };

    static juce::Font sysFont()   { return Typography::ui (12.5f).italicised(); }
    static juce::Font bodyFont()  { return Typography::ui (14.0f); }

    juce::String systemLine (const Message& m) const { return m.text + "   " + m.time; }

    RowMetrics rowMetrics (size_t idx, int bw) const
    {
        const auto& m = messages[idx];
        RowMetrics r;
        r.system = m.isSystem;
        r.event = m.isEvent;
        r.grouped = isContinuation (idx);

        if (r.event)
        {
            const bool afterEvent = idx > 0 && messages[idx - 1].isEvent;
            r.bodyH = 14;
            r.bodyY = afterEvent ? 2 : 8;
            r.rowH = r.bodyY + r.bodyH + 8;
            return r;
        }

        if (r.system)
        {
            r.bodyH = juce::jmax (16, textHeight (systemLine (m), bw, sysFont()));
            r.bodyY = sysGap;
            r.rowH = sysGap + r.bodyH + sysGap;
            return r;
        }

        r.bodyH = bodyHeight (m, bw);
        if (r.grouped)
        {
            r.bodyY = contGap;
            r.rowH = contGap + r.bodyH;
        }
        else
        {
            r.bodyY = groupGap + headerH + 2;
            r.rowH = r.bodyY + r.bodyH + 5;
        }
        return r;
    }

    static bool isImageFile (const juce::String& path)
    {
        auto ext = juce::File (path).getFileExtension().toLowerCase();
        return ext == ".png" || ext == ".jpg" || ext == ".jpeg"
            || ext == ".gif" || ext == ".bmp" || ext == ".webp";
    }

    static juce::String extForBytes (const juce::MemoryBlock& b)
    {
        auto* d = static_cast<const unsigned char*> (b.getData());
        auto n = b.getSize();
        if (n >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') return ".png";
        if (n >= 2 && d[0] == 0xFF && d[1] == 0xD8)                              return ".jpg";
        if (n >= 4 && std::memcmp (d, "GIF8", 4) == 0)                           return ".gif";
        return ".png";
    }

    void sendCurrentText()
    {
        auto t = input.getText().trim();
        if (t.isEmpty())
            return;
        input.clear();
        if (onSendText)
            onSendText (t);
    }

    void sendImageFile (const juce::File& f)
    {
        juce::MemoryBlock block;
        if (! f.loadFileAsData (block) || block.getSize() == 0)
            return;
        auto img = juce::ImageFileFormat::loadFrom (block.getData(), block.getSize());
        if (img.isValid() && onSendImageBytes)
            onSendImageBytes (block);
    }

    void chooseImage()
    {
        chooser = std::make_unique<juce::FileChooser> (
            "Choose an image to send", juce::File(),
            "*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.webp");
        chooser->launchAsync (juce::FileBrowserComponent::openMode
                                  | juce::FileBrowserComponent::canSelectFiles,
            [this] (const juce::FileChooser& fc)
            {
                auto f = fc.getResult();
                if (f != juce::File())
                    sendImageFile (f);
            });
    }

    void pushMessage (Message&& m)
    {
        const bool own = (! m.isSystem) && (! m.isEvent) && getLocalPeerId && m.peerId == getLocalPeerId();
        const bool nearBottom = isNearBottom();
        messages.push_back (std::move (m));
        while (messages.size() > maxMessages)
            messages.pop_front();
        content.relayout();
        if (own || nearBottom)
            scrollToBottom();
        updateGifTimer();
        repaint();
    }

    void scrollToBottom() { viewport.setViewPositionProportionately (0.0, 1.0); }

    bool isNearBottom() const
    {
        const int viewBottom = viewport.getViewPositionY() + viewport.getViewHeight();
        return viewBottom >= content.getHeight() - 40;
    }

    int bodyWidth() const
    {
        const bool barVisible = const_cast<juce::Viewport&> (viewport).getVerticalScrollBar().isVisible();
        const int sb = barVisible ? viewport.getScrollBarThickness() : 0;
        return juce::jmax (40, viewport.getWidth() - sb - gutter - rowPad * 2);
    }

    static int textHeight (const juce::String& text, int width, juce::Font font)
    {
        juce::AttributedString s;
        s.append (text, font, Colors::text);
        juce::TextLayout layout;
        layout.createLayout (s, (float) width);
        return (int) std::ceil (layout.getHeight());
    }

    int bodyHeight (const Message& m, int width) const
    {
        if (! m.isImage)
            return juce::jmax (18, textHeight (m.text, width, bodyFont()));

        if (m.loading || ! m.image.isValid())
            return 80;

        double scale = juce::jmin (1.0, (double) width / (double) juce::jmax (1, m.image.getWidth()));
        int h = juce::roundToInt (m.image.getHeight() * scale);
        return juce::jlimit (24, maxImageH, h);
    }

    juce::Rectangle<int> imageRect (size_t idx, int rowY, int bw) const
    {
        const auto& m = messages[idx];
        const auto rm = rowMetrics (idx, bw);
        const int bx = gutter + rowPad;
        const int by = rowY + rm.bodyY;
        double scale = juce::jmin (1.0, (double) bw / (double) juce::jmax (1, m.image.getWidth()));
        int iw = juce::jlimit (1, bw, juce::roundToInt (m.image.getWidth() * scale));
        return { bx, by, iw, rm.bodyH };
    }

    int messageHeight (size_t idx, int width) const { return rowMetrics (idx, width).rowH; }

    void updateGifTimer()
    {
        bool anyAnimated = false;
        for (auto& m : messages)
            if (m.isAnimated())
            {
                anyAnimated = true;
                break;
            }

        if (anyAnimated && isShowing())
        {
            if (! isTimerRunning())
                startTimer (33);
        }
        else
        {
            stopTimer();
        }
    }

    void timerCallback() override
    {
        if (! isShowing())
        {
            stopTimer();
            return;
        }

        const auto now = juce::Time::getMillisecondCounter();
        const int bw = bodyWidth();
        const int viewTop = viewport.getViewPositionY();
        const int viewBot = viewTop + viewport.getViewHeight();

        bool anyAnimated = false;
        int y = 4;
        for (size_t idx = 0; idx < messages.size(); ++idx)
        {
            auto& m = messages[idx];
            const int rowH = messageHeight (idx, bw);
            if (m.isAnimated())
            {
                anyAnimated = true;
                const bool visible = (y + rowH > viewTop) && (y < viewBot);
                if (visible && now >= m.nextFrameMs)
                {
                    const int n = (int) m.gif->frames.size();
                    m.gifFrame = (m.gifFrame + 1) % n;
                    int delay = m.gif->delaysMs[(size_t) m.gifFrame];
                    m.nextFrameMs = now + (juce::uint32) juce::jmax (20, delay);
                    content.repaint ({ 0, y, content.getWidth(), rowH });
                }
            }
            y += rowH;
        }

        if (! anyAnimated)
            stopTimer();
    }

    struct Content : juce::Component
    {
        void relayout()
        {
            if (owner == nullptr)
                return;
            int w = juce::jmax (10, owner->viewport.getWidth() - owner->viewport.getScrollBarThickness());
            int bw = owner->bodyWidth();
            int total = 4;
            for (size_t idx = 0; idx < owner->messages.size(); ++idx)
                total += owner->messageHeight (idx, bw);
            setSize (w, juce::jmax (owner->viewport.getHeight(), total + 4));
        }

        int imageRowAt (juce::Point<int> p, juce::Rectangle<int>& imgOut) const
        {
            if (owner == nullptr)
                return -1;
            const int bw = owner->bodyWidth();
            int y = 4;
            for (size_t idx = 0; idx < owner->messages.size(); ++idx)
            {
                auto& m = owner->messages[idx];
                const int rowH = owner->messageHeight (idx, bw);
                if (m.isImage && ! m.loading && m.image.isValid())
                {
                    auto r = owner->imageRect (idx, y, bw);
                    if (r.contains (p))
                    {
                        imgOut = r;
                        return (int) idx;
                    }
                }
                y += rowH;
            }
            return -1;
        }

        int rowAt (juce::Point<int> p) const
        {
            if (owner == nullptr)
                return -1;
            const int bw = owner->bodyWidth();
            int y = 4;
            for (size_t idx = 0; idx < owner->messages.size(); ++idx)
            {
                const int rowH = owner->messageHeight (idx, bw);
                if (p.y >= y && p.y < y + rowH)
                    return (int) idx;
                y += rowH;
            }
            return -1;
        }

        void setHoverRow (int r)
        {
            if (r == hoverRow || owner == nullptr)
                return;

            const int previous = hoverRow;
            hoverRow = r;

            const int bw = owner->bodyWidth();
            int y = 4;
            for (size_t i = 0; i < owner->messages.size(); ++i)
            {
                const int rowH = owner->messageHeight (i, bw);
                if ((int) i == previous || (int) i == hoverRow)
                    repaint ({ 0, y, getWidth(), rowH });
                y += rowH;
            }
        }

        void mouseMove (const juce::MouseEvent& e) override { setHoverRow (rowAt (e.getPosition())); }

        void mouseExit (const juce::MouseEvent&) override { setHoverRow (-1); }

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (owner == nullptr)
                return;
            juce::Rectangle<int> imgR;
            const int idx = imageRowAt (e.getPosition(), imgR);
            if (idx < 0)
                return;

            auto& m = owner->messages[(size_t) idx];
            const auto imageCopy = m.image;
            const auto bytesCopy = m.bytes;

            if (e.mods.isPopupMenu())
            {
                juce::PopupMenu menu;
                menu.addItem (1, "Save image...");
                menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition(),
                    [this, imageCopy, bytesCopy] (int r)
                    {
                        if (r == 1 && owner != nullptr)
                            owner->saveImage (imageCopy, bytesCopy);
                    });
                return;
            }

            if (owner->onImageClicked)
                owner->onImageClicked (imageCopy, bytesCopy);
        }

        void paintEventRow (juce::Graphics& g, const Message& m, int by, int bh, int bw, int bx) const
        {
            g.setColour (m.colour.withAlpha (0.85f));
            g.fillEllipse ((float) (gutter - 12), (float) by + bh * 0.5f - 2.0f, 4.0f, 4.0f);

            juce::AttributedString s;
            s.append (m.name + " ", Typography::semi (11.5f), m.colour.withAlpha (0.85f));
            s.append (m.text, Colors::emojiFont (11.5f), Colors::textDim);
            s.append (juce::String (juce::CharPointer_UTF8 ("  \xC2\xB7  ")) + m.time,
                      Typography::ui (10.5f), Colors::textDim.withAlpha (0.6f));
            juce::TextLayout layout;
            layout.createLayout (s, (float) bw);
            layout.draw (g, juce::Rectangle<float> ((float) bx, (float) by, (float) bw, (float) bh));
        }

        void paint (juce::Graphics& g) override
        {
            if (owner == nullptr)
                return;

            if (owner->messages.empty())
            {
                g.setColour (Colors::textDim);
                g.setFont (Typography::ui (13.0f));
                g.drawText ("No messages yet - say hi",
                            getLocalBounds().removeFromBottom (44).withTrimmedLeft (rowPad).withTrimmedRight (rowPad),
                            juce::Justification::centredLeft);
                return;
            }

            const int bw = owner->bodyWidth();
            const juce::Font nameFont = Typography::semi (15.0f);
            const juce::Font timeFont = Typography::ui (11.0f);
            const juce::Font textFont = bodyFont();

            const int bx = gutter + rowPad;
            const auto clip = g.getClipBounds();
            int y = 4;
            for (size_t idx = 0; idx < owner->messages.size(); ++idx)
            {
                auto& m = owner->messages[idx];
                const auto rm = owner->rowMetrics (idx, bw);
                const int by = y + rm.bodyY;

                if (y >= clip.getBottom() || y + rm.rowH <= clip.getY())
                {
                    y += rm.rowH;
                    continue;
                }

                if ((int) idx == hoverRow && ! rm.system && ! rm.event)
                {
                    const int lead = rm.grouped ? contGap : groupGap;
                    g.setColour (juce::Colours::white.withAlpha (0.025f));
                    g.fillRect (0, y + lead, getWidth(), rm.rowH - lead);
                }

                if (rm.event)
                {
                    paintEventRow (g, m, by, rm.bodyH, bw, bx);
                    y += rm.rowH;
                    continue;
                }

                if (rm.system)
                {
                    g.setColour (Colors::textDim.withAlpha (0.5f));
                    g.fillEllipse ((float) (gutter - 14), (float) (by + rm.bodyH * 0.5f - 2.0f), 4.0f, 4.0f);

                    juce::AttributedString s;
                    s.append (m.text, sysFont(), Colors::textDim);
                    s.append ("   " + m.time, sysFont(), Colors::textDim.withAlpha (0.7f));
                    juce::TextLayout layout;
                    layout.createLayout (s, (float) bw);
                    layout.draw (g, juce::Rectangle<float> ((float) bx, (float) by, (float) bw, (float) rm.bodyH));
                    y += rm.rowH;
                    continue;
                }

                if (! rm.grouped)
                {
                    const int headTop = y + groupGap;
                    juce::Rectangle<float> badge ((float) rowPad, (float) headTop, (float) avatarD, (float) avatarD);
                    auto avatar = owner->getAvatar ? owner->getAvatar (m.peerId) : juce::Image();
                    if (avatar.isValid())
                    {
                        juce::Graphics::ScopedSaveState ss (g);
                        juce::Path clipPath; clipPath.addEllipse (badge);
                        g.reduceClipRegion (clipPath);
                        g.setColour (juce::Colours::white);
                        g.drawImage (avatar, badge, juce::RectanglePlacement::centred
                                                        | juce::RectanglePlacement::fillDestination);
                    }
                    else
                    {
                        g.setColour (m.colour.withAlpha (0.25f));
                        g.fillEllipse (badge);
                        g.setColour (m.colour);
                        g.setFont (Typography::semi (16.0f));
                        g.drawText (m.name.substring (0, 1).toUpperCase(), badge, juce::Justification::centred);
                    }

                    const float baseline = (float) headTop + nameFont.getAscent()
                                             + ((float) headerH - nameFont.getHeight()) * 0.5f;
                    const int nameW = juce::GlyphArrangement::getStringWidthInt (nameFont, m.name);

                    juce::GlyphArrangement ga;
                    ga.addLineOfText (nameFont, m.name, (float) bx, baseline);
                    g.setColour (m.colour);
                    ga.draw (g);

                    juce::GlyphArrangement ta;
                    ta.addLineOfText (timeFont, m.time, (float) (bx + nameW + 6), baseline);
                    g.setColour (Colors::textDim);
                    ta.draw (g);
                }
                else if ((int) idx == hoverRow)
                {
                    const juce::Font stampFont = Typography::ui (10.5f);
                    const int stampW = juce::GlyphArrangement::getStringWidthInt (stampFont, m.time);

                    juce::GlyphArrangement ta;
                    ta.addLineOfText (stampFont, m.time,
                                      (float) (gutter + rowPad - 8 - stampW),
                                      (float) by + textFont.getAscent());
                    g.setColour (Colors::textDim.withAlpha (0.6f));
                    ta.draw (g);
                }

                if (! m.isImage)
                {
                    juce::AttributedString s;
                    s.append (m.text, textFont, Colors::text);
                    juce::TextLayout layout;
                    layout.createLayout (s, (float) bw);
                    layout.draw (g, juce::Rectangle<float> ((float) bx, (float) by, (float) bw, (float) rm.bodyH));
                }
                else if (m.loading || ! m.image.isValid())
                {
                    juce::Rectangle<float> box ((float) bx, (float) by, juce::jmin (160.0f, (float) bw), (float) rm.bodyH);
                    g.setColour (Colors::card);
                    g.fillRoundedRectangle (box, Metrics::rM);
                    g.setColour (Colors::textDim);
                    g.setFont (Typography::ui (12.5f));
                    g.drawText ("loading image...", box, juce::Justification::centred);
                }
                else
                {
                    owner->ensureGifDecoded (m);
                    auto imgR = owner->imageRect (idx, y, bw);
                    const juce::Image& frame = m.isAnimated()
                                                 ? m.gif->frames[(size_t) (m.gifFrame % (int) m.gif->frames.size())]
                                                 : m.image;
                    juce::Graphics::ScopedSaveState ss (g);
                    juce::Path clipPath;
                    clipPath.addRoundedRectangle (imgR.toFloat(), Metrics::rM);
                    g.reduceClipRegion (clipPath);
                    g.setColour (juce::Colours::white);
                    g.drawImage (frame, imgR.toFloat(), juce::RectanglePlacement::xLeft
                                                            | juce::RectanglePlacement::yTop
                                                            | juce::RectanglePlacement::onlyReduceInSize);
                }

                y += rm.rowH;
            }
        }

        int hoverRow = -1;
        ChatPanel* owner = nullptr;
    };

    void ensureGifDecoded (Message& m)
    {
        if (m.triedGif || m.bytes.getSize() == 0)
            return;
        m.triedGif = true;
        if (! GifImage::looksLikeGif (m.bytes.getData(), m.bytes.getSize()))
            return;

        auto gif = std::make_shared<GifImage>();
        if (GifImage::decode (m.bytes.getData(), m.bytes.getSize(), *gif) && gif->isAnimated())
        {
            m.gif = gif;
            m.gifFrame = 0;
            m.nextFrameMs = juce::Time::getMillisecondCounter() + (juce::uint32) juce::jmax (20, gif->delaysMs[0]);
            updateGifTimer();
        }
    }

    juce::PropertiesFile* settings = nullptr;
    int chatWidth = 340;
    bool dockRight = true;
    bool resizing = false;
    int dragStartWidth = 0, dragStartX = 0;

    juce::Viewport viewport;
    Content content;
    std::deque<Message> messages;
    juce::TextEditor input;
    juce::TextButton attachBtn, sendBtn;
    HeaderButton swapBtn { "chat-swap-side", HeaderButton::Glyph::swapSide };
    HeaderButton closeBtn { "chat-close", HeaderButton::Glyph::close };
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<juce::FileChooser> saveChooser;
    bool dragHover = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChatPanel)
};
