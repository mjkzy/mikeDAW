#pragma once
#include <JuceHeader.h>
#include <set>
#include <map>
#include <vector>
#include <functional>
#include "../Model/ProjectModel.h"
#include "../Engine/AudioEngine.h"
#include "../Engine/PluginHost.h"
#include "../Net/Session.h"
#include "../Net/DiscordPresence.h"
#include "../Net/LanDiscovery.h"
#include "../Net/FriendsManager.h"
#include "../Net/RelayClient.h"
#include "../Net/RelayTunnel.h"
#include "../Net/License.h"
#if ! OPENDAW_PUBLIC_BUILD
 #include "../Net/Updater.h"
#endif
#include "../Model/History.h"
#include "AppLookAndFeel.h"
#include "ThemeManager.h"
#include "FloatingPanel.h"
#include "DockShell.h"
#include "PluginManagerWindow.h"
#include "TransportBar.h"
#include "BrowserPanel.h"
#include "ChannelRack.h"
#include "SamplerSettingsPanel.h"
#include "PianoRoll.h"
#include "Playlist.h"
#include "Mixer.h"
#include "KeyBindings.h"
#include "KeyBindingsWindow.h"
#include "PreferencesWindow.h"
#include "JoinSessionWindow.h"
#include "MissingFilesDialog.h"
#include "PeerCursorLayer.h"
#include "ReactionLayer.h"
#include "ToastLayer.h"
#include "ChatPanel.h"
#include "FriendsPanel.h"
#include "AppSettings.h"
#include "../Debug/PerfMonitor.h"
#include "../Debug/DebugOverlayWindow.h"
#include "../Debug/DebugOverlayUI.h"

class DiskWriteQueue : private juce::Thread
{
public:
    DiskWriteQueue() : juce::Thread ("mikeDAW disk save") { startThread(); }
    ~DiskWriteQueue() override
    {
        signalThreadShouldExit();
        notify();
        stopThread (20000);
    }

    void enqueue (juce::File dest, juce::String xml, std::function<void (bool)> onComplete = {})
    {
        {
            const juce::ScopedLock sl (lock);
            jobs.push_back ({ std::move (dest), std::move (xml), std::move (onComplete) });
        }
        notify();
    }

private:
    struct Job { juce::File dest; juce::String xml; std::function<void (bool)> onComplete; };

    void run() override
    {
        for (;;)
        {
            Job job;
            bool haveJob = false;
            {
                const juce::ScopedLock sl (lock);
                if (! jobs.empty())
                {
                    job = std::move (jobs.front());
                    jobs.erase (jobs.begin());
                    haveJob = true;
                }
            }

            if (! haveJob)
            {
                if (threadShouldExit())
                    return;
                wait (-1);
                continue;
            }

            const bool ok = job.dest.replaceWithText (job.xml);
            if (job.onComplete)
                juce::MessageManager::callAsync ([cb = std::move (job.onComplete), ok] { cb (ok); });
        }
    }

    juce::CriticalSection lock;
    std::vector<Job> jobs;
};

class MainComponent : public juce::Component,
                      public juce::DragAndDropContainer,
                      public juce::ApplicationCommandTarget,
                      private juce::ValueTree::Listener,
                      private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void parentHierarchyChanged() override;
    bool keyPressed (const juce::KeyPress&) override;
    bool keyStateChanged (bool isKeyDown) override;

    bool tryClose();

    void prepareToQuit();

    void noteWindowActivated()   { lastWindowActivationMs = juce::Time::getMillisecondCounter(); }

    void openProjectFileFromOS (const juce::File& f);

    ApplicationCommandTarget* getNextCommandTarget() override { return nullptr; }
    void getAllCommands (juce::Array<juce::CommandID>&) override {}
    void getCommandInfo (juce::CommandID, juce::ApplicationCommandInfo&) override {}
    bool perform (const InvocationInfo&) override { return false; }

private:
    void togglePanel (const juce::String& name);
    FloatingPanel* frontmostChild() const;

    bool isDockPanel (FloatingPanel* p) const;
    FloatingPanel* dockPanelFor (const juce::String& name) const;
    juce::String dockNameFor (FloatingPanel* p) const;
    void revealPanel (FloatingPanel* p);
    void showDockTab (FloatingPanel* p);
    void setDockCollapsed (bool collapsed);
    void applyDockState();
    void refreshDockHeader();
    void layoutDockedDesk (juce::Rectangle<int> deskArea);
    void openPluginEditor (const juce::String& key, const juce::String& fallbackTitle);

    void openPluginEditorPolling (const juce::String& key, const juce::String& title,
                                  int attemptsLeft, bool respectWarnedGuard);

    void autoOpenPeerVst (const juce::String& key, const juce::String& pluginName, int attemptsLeft);
    void openPianoRoll (int patternId, int channelId);
    void retargetPianoRoll (int patternId, int channelId);
    int channelWithContent (int patternId, int currentChannelId) const;
    void newProject();
    void saveProject();
    void saveProject (std::function<void (bool)> onDone);
    void saveProjectAs();
    void saveProjectAs (std::function<void (bool)> onDone);
    void finishSuccessfulSave (const juce::File& savedFile);
    void bundleProjectSounds (const juce::File& savedFile);
    void relinkBundledSamples (juce::ValueTree loaded, const juce::Array<juce::File>& searchDirs);
    void checkMissingFiles();
    void saveAsTemplate();
    void loadProject();
    void loadFile (const juce::File& f);
    void openRecentProject (const juce::File& f);
    void confirmSaveIfDirty (std::function<void()> proceed);

    void onProjectSavedSuccessfully();
    juce::String currentProjectName() const;

    void valueTreePropertyChanged (juce::ValueTree& t, const juce::Identifier& p) override
    {
        markDirty();
        if (t.hasType (IDs::Project) && p == IDs::name)
            updateWindowTitle();
        else if (p == IDs::pending && t.hasType (IDs::Clip))
            onPendingClipCleared (t);
    }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& c) override { markDirty(); onPendingClipAdded (c); }

    void onPendingClipAdded (const juce::ValueTree& clip);
    void onPendingClipCleared (const juce::ValueTree& clip);
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override { markDirty(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override { markDirty(); }
    void valueTreeParentChanged (juce::ValueTree&) override {}
    void markDirty() { dirty = true; }
    void renderAudioDialog();
    void exportFlZip();
    void exportProjectZip();
    void runExportRender (const juce::File& dest, const juce::String& format,
                          int bitDepth, int qualityIndex, int bitrateKbps,
                          double tailSeconds);
    void showPluginManager();
    void socialMenu (juce::Component* target);
    void hostSession();
    void joinSessionDialog();
    void joinSessionText (const juce::String& text, int defaultPort, juce::int64 relayFriendId = 0);
    void beginRelayJoin (juce::int64 relayFriendId, const juce::String& fallbackText, int fallbackPort,
                         const juce::String& joinToken = {});
    void runRelayJoinFallback();
    void inviteFriend (juce::int64 friendId);
    void handleFriendInvite (juce::int64 fromId, const juce::String& fromName, const juce::String& sessionName,
                             const juce::String& joinText, int port, bool localOnly);
    void handleIncomingFriendRequest (juce::int64 fromId, const juce::String& name);
    void handleIncomingFriendResponse (juce::int64 fromId, const juce::String& name, bool accepted);
    void sendFriendRequestTo (juce::int64 id, const juce::String& name);
    void respondToFriendRequest (juce::int64 id, const juce::String& name, bool accept);
    void sendFriendResponseTo (juce::int64 id, bool accept);
    void showPeerFriendMenu (juce::int64 peerId, const juce::String& name);
    juce::String localDisplayName();
    juce::String presenceStatusString();
    int presencePeopleCount();
    int presenceSessionPort();
    void showHostInviteReady (const struct InviteHostResult& result);
    void copyInviteCode();
    void leaveSession();
    void resetSessionUiState();
    void showEditMenu (juce::Component* target);
    void showKeyBindingsWindow();
    void showPreferences();
    void deleteUnusedAudioFiles();

    juce::String defaultOutputDeviceName();
    void selectDefaultOutputDevice();
    void applyFollowDefaultOutput();
    juce::String followedOutputName;
    struct DeviceChangeRelay : public juce::ChangeListener
    {
        std::function<void()> onChange;
        void changeListenerCallback (juce::ChangeBroadcaster*) override { if (onChange) onChange(); }
    } deviceChangeRelay;

    void showWelcomeDialog();
    void pushLocalAvatar();
    bool handleAction (KeyBindings::Action);
    void syncPlayModeToActivePanel();
    int modeMismatchWarnedSong = -1;

    void toggleRecord();
    void captureRecordedNote (int channelId, bool on, int note, float velocity);
    int  chooseRecordLane (int mixerTrack, double startBeats);
    std::map<int, int> recordingLaneCache;
    struct RecHeldNote { double startTick; int velocity; };
    std::map<std::pair<int, int>, RecHeldNote> recHeldNotes;

    void updateTypingKeyboard();
    void releaseAllTypingNotes();
    bool typingNotesConsumeKey (int keyCode) const;
    int  typingTargetChannel() const;
    static bool focusIsInTextEditor();
    bool typingKeyboardEnabled = false;
    int  typingKeyboardOctave = 3;
    std::map<int, int> typingHeldNotes;
    int  typingNoteForKey (int keyCode) const;

    void mouseMove (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void broadcastLocalCursor (const juce::MouseEvent&);

    void showReactionPickerAt (const juce::MouseEvent&);
    struct MiddleClickCatcher : juce::MouseListener
    {
        std::function<void (const juce::MouseEvent&)> onMiddleDown;
        void mouseDown (const juce::MouseEvent& e) override
        {
            if (e.mods.isMiddleButtonDown() && onMiddleDown)
                onMiddleDown (e);
        }
    };
    MiddleClickCatcher middleClickCatcher;

    void noteLocalActivity();
    void checkAfk();
    void setLocalAfk (bool afk);
    void setManualAway (bool away);
    juce::int64 lastActivityMs = 0;
    juce::int64 afkSinceMs = 0;
    bool localAfk = false;
    bool manualAway = false;
    struct AfkTimer : juce::Timer
    {
        std::function<void()> onTick;
        void timerCallback() override { if (onTick) onTick(); }
    } afkTimer;

    void timerCallback() override;
    void applyAutoSaveSettings();
    void revertToAutoSave();
    void pruneAutoSaves (const juce::File& dir);

    void flushWorkTime();
    void updateWindowTitle();

    AppLookAndFeel lookAndFeel;
    juce::TooltipWindow tooltipWindow { nullptr, 600 };
    juce::ValueTree project;
    PluginHost pluginHost;
    ThemeManager themeManager { pluginHost.getSettings() };
    AudioEngine engine;
    std::unique_ptr<PerfMonitor> perfMonitor;
    Session session;
    DiscordPresence discord;
    LanDiscovery lanDiscovery;
    FriendsManager friends { pluginHost.getSettings().getFile().getParentDirectory() };
    RelayClient relay;
   #if ! OPENDAW_PUBLIC_BUILD
    LicenseManager license { pluginHost.getSettings() };
    Updater updater { pluginHost.getSettings() };
    bool updateChecked = false;
   #endif
    std::unique_ptr<RelayJoinTunnel> relayJoinTunnel;
    juce::int64 localUserId = 0;
    History history { project };
    KeyBindings keyBindings;
    PluginWindowManager pluginWindows;

    TransportBar transportBar;
    std::unique_ptr<BrowserPanel> browser;
    std::unique_ptr<ChatPanel> chat;
    FloatingPanel* rackPanel = nullptr;
    FloatingPanel* rollPanel = nullptr;
    FloatingPanel* playlistPanel = nullptr;
    FloatingPanel* mixerPanel = nullptr;
    FloatingPanel* samplerPanel = nullptr;
    SamplerSettingsPanel* samplerSettings = nullptr;
    ChannelRack* rack = nullptr;
    PianoRoll* pianoRoll = nullptr;
    Playlist* playlist = nullptr;
    MixerPanel* mixerUi = nullptr;
    FloatingPanel* friendsPanel = nullptr;
    FriendsPanel* friendsUi = nullptr;
    juce::Component desk;
    DockHeader dockHeader;
    StatusBar statusBar;
    bool dockedLayout = true;
    FloatingPanel* dockTab = nullptr;
    bool dockCollapsed = false;
    bool samplerHasTarget = false;
    int dockHeight = 380;
    int dockDragStartHeight = 0;
    PeerCursorLayer peerCursors;
    ReactionLayer reactions;
    ToastLayer toasts;
    juce::uint32 lastCursorSendMs = 0;
    Session::CompatReport lastCompatReport;

    struct PeerViewport { juce::String name; juce::String panelName; int patternId = -1; };
    std::map<juce::int64, PeerViewport> peerViewports;
    juce::String localViewport;
    void refreshLocalViewportPattern();
    std::set<juce::int64> afkPeersSeen;
    void noteLocalViewport (juce::Component* eventComponent);
    void refreshPeerBadges();
    juce::String panelDisplayName (FloatingPanel* p) const;
    void logActivity (const juce::String& actor, const juce::String& action);
    juce::String vstAddedLocation (const juce::String& key) const;
    void reportDeletion (const juce::String& what);
    juce::int64 deletionBurstMs = 0;
    int deletionBurstCount = 0;
    bool deletionBurstRemote = false;
    juce::uint32 lastWindowActivationMs = 0;
    std::set<int> warnedMissingChannels;
    std::set<juce::String> warnedMissingEffects;
    juce::OwnedArray<FloatingPanel> panels;
    FloatingPanel* activePanel = nullptr;
    int lastSoundChannelId = 0;

    struct LockOverlay : juce::Component
    {
        LockOverlay() { setInterceptsMouseClicks (true, true); setWantsKeyboardFocus (false); }
        void paint (juce::Graphics&) override;
        juce::String message;
    };
    std::unique_ptr<LockOverlay> lockOverlay;
    bool renderLocked = false;
    void showRenderLock (const juce::String& byName);
    void hideRenderLock();

    struct ImageLightbox : juce::Component, private juce::Timer
    {
        ImageLightbox() { setInterceptsMouseClicks (true, true); setWantsKeyboardFocus (true); }

        void setContent (const juce::Image& img, const juce::MemoryBlock& b)
        {
            image = img;
            bytes = b;
            gif.reset();
            gifFrame = 0;
            stopTimer();
            if (GifImage::looksLikeGif (b.getData(), b.getSize()))
            {
                auto gg = std::make_shared<GifImage>();
                if (GifImage::decode (b.getData(), b.getSize(), *gg) && gg->isAnimated())
                {
                    gif = gg;
                    nextFrameMs = juce::Time::getMillisecondCounter()
                                    + (juce::uint32) juce::jmax (20, gg->delaysMs[0]);
                    startTimer (33);
                }
            }
            repaint();
        }

        juce::Rectangle<int> imageArea() const
        {
            if (! image.isValid())
                return {};
            const int iw = image.getWidth(), ih = image.getHeight();
            auto avail = getLocalBounds().reduced (48);
            double scale = juce::jmin (1.0, juce::jmin ((double) avail.getWidth()  / juce::jmax (1, iw),
                                                        (double) avail.getHeight() / juce::jmax (1, ih)));
            int w = juce::jmax (1, juce::roundToInt (iw * scale));
            int h = juce::jmax (1, juce::roundToInt (ih * scale));
            return juce::Rectangle<int> (w, h).withCentre (getLocalBounds().getCentre());
        }

        juce::Rectangle<int> closeButton() const { return { getWidth() - 44, 12, 32, 32 }; }
        juce::Rectangle<int> saveButton()  const { return { getWidth() - 88, 12, 32, 32 }; }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colours::black.withAlpha (0.78f));

            auto dst = imageArea();
            if (! dst.isEmpty())
            {
                const juce::Image& frame = (gif != nullptr && gif->isAnimated())
                    ? gif->frames[(size_t) (gifFrame % (int) gif->frames.size())]
                    : image;
                g.drawImage (frame, dst.toFloat(), juce::RectanglePlacement::stretchToFit);
            }

            {
                auto b = saveButton();
                g.setColour (juce::Colours::white.withAlpha (0.12f));
                g.fillRoundedRectangle (b.toFloat(), 5.0f);
                g.setColour (juce::Colours::white.withAlpha (0.9f));
                auto a = b.reduced (9).toFloat();
                const float cx = a.getCentreX();
                juce::Path arrow;
                arrow.startNewSubPath (cx, a.getY());
                arrow.lineTo (cx, a.getBottom() - 2.0f);
                arrow.startNewSubPath (a.getX() + 1.0f, a.getCentreY() + 1.0f);
                arrow.lineTo (cx, a.getBottom() - 2.0f);
                arrow.lineTo (a.getRight() - 1.0f, a.getCentreY() + 1.0f);
                g.strokePath (arrow, juce::PathStrokeType (1.8f));
                g.drawLine (a.getX(), a.getBottom(), a.getRight(), a.getBottom(), 1.8f);
            }
            {
                auto b = closeButton();
                g.setColour (juce::Colours::white.withAlpha (0.12f));
                g.fillRoundedRectangle (b.toFloat(), 5.0f);
                g.setColour (juce::Colours::white.withAlpha (0.9f));
                auto a = b.reduced (10).toFloat();
                g.drawLine (a.getX(), a.getY(), a.getRight(), a.getBottom(), 1.8f);
                g.drawLine (a.getX(), a.getBottom(), a.getRight(), a.getY(), 1.8f);
            }
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            auto p = e.getPosition();
            if (closeButton().contains (p)) { if (onClose) onClose(); return; }
            if (saveButton().contains (p))  { if (onSave)  onSave();  return; }
            if (! imageArea().contains (p)) { if (onClose) onClose(); }
        }

        bool keyPressed (const juce::KeyPress& k) override
        {
            if (k == juce::KeyPress::escapeKey) { if (onClose) onClose(); return true; }
            return false;
        }

        void timerCallback() override
        {
            if (gif == nullptr) { stopTimer(); return; }
            const auto now = juce::Time::getMillisecondCounter();
            if (now >= nextFrameMs)
            {
                gifFrame = (gifFrame + 1) % (int) gif->frames.size();
                nextFrameMs = now + (juce::uint32) juce::jmax (20, gif->delaysMs[(size_t) gifFrame]);
                repaint (imageArea());
            }
        }

        std::function<void()> onClose, onSave;
        juce::Image image;
        juce::MemoryBlock bytes;
        std::shared_ptr<GifImage> gif;
        int gifFrame = 0;
        juce::uint32 nextFrameMs = 0;
    };
    std::unique_ptr<ImageLightbox> lightbox;
    void showImageLightbox (const juce::Image& img, const juce::MemoryBlock& bytes);
    void hideImageLightbox();

    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<PluginManagerWindow> pluginManager;
    std::unique_ptr<KeyBindingsWindow> keyBindingsWindow;
    std::unique_ptr<PreferencesWindow> preferencesWindow;
    std::unique_ptr<JoinSessionWindow> joinWindow;
    std::unique_ptr<MissingFilesWindow> missingFilesWindow;
    bool browserVisible = true;
    bool chatVisible = false;
    bool panelsLaidOut = false;
    juce::String lastInviteCode;
    juce::String lastInviteStatus;
    juce::Image localAvatarImage;
    juce::String hostedSessionName;
    juce::String hostJoinToken;
    int hostedPort = 0;
    int lastUpnpMappedPort = 0;
    bool joinInProgress = false;

    bool relayJoinPending = false;
    bool relayJoinAwaitingSnapshot = false;
    juce::int64 relayJoinFriendId = 0;
    int relayJoinAttempt = 0;
    juce::String relayJoinFallbackText;
    int relayJoinFallbackPort = 0;

    bool dirty = false;
    juce::File currentProjectFile;
    std::unique_ptr<juce::AlertWindow> saveAlert;

    juce::int64 presenceStartEpoch = juce::Time::currentTimeMillis() / 1000;
    juce::String lastPresenceDetails;

    DiskWriteQueue diskWrites;

    juce::int64 lastWorkFlushMs = 0;

    static constexpr int workClockIntervalMs = 60 * 1000;
    juce::int64 lastAutoSaveMs = 0;

    void toggleDebugOverlay();

    std::unique_ptr<DebugOverlayUI> debugOverlayUI;
    std::unique_ptr<DebugOverlayWindow> debugOverlay;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
