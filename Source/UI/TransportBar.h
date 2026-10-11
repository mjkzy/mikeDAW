#pragma once
#include <JuceHeader.h>
#include "../Model/ProjectModel.h"
#include "../Engine/AudioEngine.h"
#include "../Net/Session.h"
#include "AppLookAndFeel.h"
#include "ValueKnob.h"
#include "HoverFade.h"
#include <map>
#include <vector>

class IconButton : public juce::Button
{
public:
    enum class Icon { play, stop, record, metronome, browser, rack, piano, playlist, mixer,
                      pencil, cursor, magnet, blade, slide, stretch, typingKeys, chat, social };

    IconButton (const juce::String& name, Icon i, const juce::String& tooltip);

    void setActive (bool shouldBeActive);
    juce::Colour activeColour;
    juce::Colour idleColour;
    float iconInset = 5.5f;
    bool pillBackground = false;

    void paintButton (juce::Graphics&, bool highlighted, bool down) override;
    void buttonStateChanged() override { hover.setHovered (isOver()); }

private:
    void drawIcon (juce::Graphics&, juce::Rectangle<float>, juce::Colour) const;
    Icon icon;
    bool active = false;
    HoverFade hover { *this };
};

class RecordButton : public IconButton
{
public:
    RecordButton() : IconButton ("rec", Icon::record, "Record (right-click for options)") {}
    std::function<void()> onRightClick;
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            if (onRightClick) onRightClick();
            return;
        }
        IconButton::mouseDown (e);
    }
};

class MenuTextButton : public juce::Button
{
public:
    explicit MenuTextButton (const juce::String& text) : juce::Button (text)
    {
        setTriggeredOnMouseDown (true);
    }
    void paintButton (juce::Graphics&, bool highlighted, bool down) override;
    void buttonStateChanged() override { hover.setHovered (isOver()); }
private:
    HoverFade hover { *this };
};

class TransportBar : public juce::Component,
                     private juce::Timer
{
public:
    TransportBar (juce::ValueTree project, AudioEngine&, juce::PropertiesFile* settings = nullptr);

    std::function<void (const juce::String& panel)> onTogglePanel;
    std::function<void()> onNew, onSave, onSaveAs, onSaveAsTemplate, onLoad;
    std::function<void()> onOpenAutosaves;
    std::function<void()> onOpenRecordings;
    std::function<void()> onToggleRecord;
    std::function<void()> onShowPluginManager, onAudioSettings;
    std::function<void (const juce::File&)> onOpenRecent;
    std::function<juce::String()> getStartupTypeName;
    std::function<juce::StringArray()> getRecentProjects;
    std::function<void()> onBeforePlay;
    std::function<void()> onRenderAudio;
    std::function<void()> onExportFlZip;
    std::function<void()> onExportMikeZip;
    std::function<void (juce::Component*)> onSocialMenu;
    std::function<void (juce::Component*)> onEditMenu;
    std::function<void()> onLeaveSession;
    std::function<void()> onToggleTypingKeyboard;
    std::function<void()> onToggleChat;

    void setSessionText (const juce::String&);
    void setSessionActive (bool active, bool isHost);
    void setTypingKeyboardActive (bool active) { typingKbBtn.setActive (active); }
    void setBrowserOpen (bool open) { browserBtn.setActive (open); }
    void setLocalMicMute (bool m) { localMicMuted = m; for (auto* s : peerStrips) if (s->isLocalStrip()) s->setMicMute (m); }

    void setChatUnread (bool unread) { chatBtn.setActive (unread); }

    void flashModeToggle();

    void updatePeerStrips (const std::vector<Session::UserState>& roster, juce::int64 localPeerId);

    std::function<juce::Image (juce::int64)> getPeerAvatar;

    std::function<void (bool)>  onDeafenToggled;
    std::function<void (bool)>  onInputMuteToggled;

    std::function<void (juce::int64, juce::String)> onPeerRightClick;

    bool isListeningToPeer (juce::int64 peerId) const;
    bool isSharingAuditions() const { return shareAuditions; }

    void paint (juce::Graphics&) override;
    void resized() override;
    void lookAndFeelChanged() override;

private:
    class BpmBox : public juce::Component
    {
    public:
        explicit BpmBox (AudioEngine& e, juce::PropertiesFile* s = nullptr) : engine (e), settings (s) {}
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
        AudioEngine& engine;
        juce::PropertiesFile* settings;
        int dragStartBpm = 0;
        int lastDragSteps = 0;
    private:
        static constexpr float arrowW = 18.0f;
        juce::Rectangle<int> arrowColumn() const { return getLocalBounds().removeFromRight ((int) arrowW); }
        void showContextMenu();
        void showSetValueDialog();
        bool dragging = false;
        int hoverArrow = 0;
    };

    class PatSongSwitch : public juce::Component
    {
    public:
        explicit PatSongSwitch (AudioEngine& e) : engine (e) {}
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        std::function<void (bool songMode)> onModeChosen;
        AudioEngine& engine;
        bool flash = false;
    };

    class SpectrumBox : public juce::Component
    {
    public:
        enum class Mode { spectrogram, oscilloscope };

        explicit SpectrumBox (AudioEngine& e, juce::PropertiesFile* s = nullptr)
            : engine (e), settings (s),
              fft (fftOrder), window (fftSize, juce::dsp::WindowingFunction<float>::hann) {}

        void updateSpectrum();
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;

        AudioEngine& engine;
        juce::PropertiesFile* settings;
        Mode mode = Mode::spectrogram;

        static constexpr int fftOrder = 10;
        static constexpr int fftSize = 1 << fftOrder;
        static constexpr int numBands = 24;
        juce::dsp::FFT fft;
        juce::dsp::WindowingFunction<float> window;
        float bands[numBands] {};
        float smoothed[numBands] {};

        float scope[fftSize] {};
    };

    class PosReadout : public juce::Component
    {
    public:
        PosReadout (AudioEngine& e, juce::PropertiesFile* s) : engine (e), settings (s) {}
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        juce::String formatText() const;
        AudioEngine& engine;
        juce::PropertiesFile* settings;
        int format = 0;
    };

    class DeafenButton : public juce::Button
    {
    public:
        DeafenButton() : juce::Button ("deafen") { setClickingTogglesState (true); }
        void paintButton (juce::Graphics&, bool highlighted, bool down) override;
        void buttonStateChanged() override { hover.setHovered (isOver()); }
    private:
        HoverFade hover { *this };
    };

    class ListenButton : public juce::Button
    {
    public:
        ListenButton() : juce::Button ("listen") { setClickingTogglesState (true); }
        void paintButton (juce::Graphics&, bool highlighted, bool down) override;
        void buttonStateChanged() override { hover.setHovered (isOver()); }
        juce::Colour onColour { Colors::accent2 };
    private:
        HoverFade hover { *this };
    };

    class MicMuteButton : public juce::Button
    {
    public:
        MicMuteButton() : juce::Button ("mic mute") { setClickingTogglesState (true); }
        void paintButton (juce::Graphics&, bool highlighted, bool down) override;
        void buttonStateChanged() override { hover.setHovered (isOver()); }
    private:
        HoverFade hover { *this };
    };

    class PeerStrip : public juce::Component
    {
    public:
        PeerStrip();
        void setState (const Session::UserState& s, bool isLocal, const juce::Image& avatar);
        void setListenVisible (bool v) { listenBtn.setVisible (v); }
        void setMicMute (bool m) { micBtn.setToggleState (m, juce::dontSendNotification); micBtn.repaint(); }
        void resized() override;
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;

        std::function<void (bool)>  onDeafen;
        std::function<bool()>       getListen;
        std::function<void (bool)>  onListenToggled;
        std::function<void (bool)>  onInputMuted;
        std::function<void (juce::int64, juce::String)> onRightClick;

        juce::int64 peerId = 0;
        juce::String peerName;

        bool isAfk() const { return afk; }
        bool isLocalStrip() const { return local; }
        void repaintIfAfk() { if (afk) repaint(); }

    private:
        juce::Label nameLabel;
        DeafenButton deafenBtn;
        ListenButton listenBtn;
        MicMuteButton micBtn;
        juce::Colour peerColour { Colors::textDim };
        juce::String initial;
        juce::Image avatarImage;
        bool local = false;
        bool afk = false;
        juce::int64 afkSinceMs = 0;
    };

    void timerCallback() override;
    void showFileMenu();
    void showRecordMenu();
    int  recBlinkFrame = 0;
    juce::uint32 modeFlashUntilMs = 0;
    bool recWasPlaying = false;

    juce::ValueTree project;
    AudioEngine& engine;
    juce::PropertiesFile* settings = nullptr;

    MenuTextButton fileBtn { "FILE" };
    MenuTextButton editBtn { "EDIT" };
    MenuTextButton pluginsBtn { "PLUGINS" };

    IconButton playBtn { "play", IconButton::Icon::play, "Play / pause (Space)" };
    IconButton stopBtn { "stop", IconButton::Icon::stop, "Stop and return to start" };
    RecordButton recBtn;
    BpmBox bpmBox { engine, settings };
    SpectrumBox spectrumBox { engine, settings };
    PosReadout posReadout { engine, settings };
    IconButton metBtn { "met", IconButton::Icon::metronome, "Metronome" };
    PatSongSwitch patSongSwitch { engine };

    ValueKnob playbackVolKnob;
    juce::Label playbackVolLabel;

    IconButton browserBtn  { "browser",  IconButton::Icon::browser,  "Browser" };
    IconButton rackBtn     { "rack",     IconButton::Icon::rack,     "Channel Rack" };
    IconButton rollBtn     { "roll",     IconButton::Icon::piano,    "Piano Roll" };
    IconButton playlistBtn { "playlist", IconButton::Icon::playlist, "Playlist" };
    IconButton mixerBtn    { "mixer",    IconButton::Icon::mixer,    "Mixer" };

    IconButton typingKbBtn { "typingkb", IconButton::Icon::typingKeys,
                             "Typing keyboard to piano keyboard (Ctrl+T)" };

    IconButton socialBtn { "social", IconButton::Icon::social, "Social and friends" };
    IconButton chatBtn     { "chat",     IconButton::Icon::chat,     "Session chat" };
    juce::Label sessionLabel;
    bool sessionActive = false;
    bool sessionIsHost = false;

    juce::OwnedArray<PeerStrip> peerStrips;
    juce::Label overflowChip;

    std::map<juce::int64, bool> listenedPeers;
    bool shareAuditions = true;
    bool localMicMuted = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TransportBar)
};
