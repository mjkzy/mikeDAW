#include "MainComponent.h"
#include "Dialogs.h"
#include "ReactionMenu.h"
#include "DarkTitleBar.h"
#include "../Model/Scales.h"
#include "../Import/FLPImport.h"
#include "../Import/FLPExport.h"
#include "../Engine/Mp3Writer.h"
#include "../Model/Leaderboard.h"
#include "../Net/InviteCode.h"

struct InviteHostResult
{
    InviteCode::HostInvite invite;
};

namespace
{
    juce::File multiplayerFolderForBrowser (Session& session)
    {
        auto folder = session.getMultiplayerFolder();
        if (! folder.isDirectory())
            return {};
        for (auto& child : folder.findChildFiles (juce::File::findFilesAndDirectories, false))
            if (! child.getFileName().equalsIgnoreCase ("cache-index.xml"))
                return folder;
        return {};
    }

    constexpr int rackDefaultVisibleRows = 16;
    constexpr int rackRowHeight = 32;
    constexpr int rackChromeHeight = FloatingPanel::titleHeight + 42;
    constexpr int rackAutoHeightCap = rackChromeHeight + rackDefaultVisibleRows * rackRowHeight;
}

MainComponent::MainComponent()
    : project (Model::createDefaultProject()),
      engine (project, pluginHost),
      session (project, engine, pluginHost),
      transportBar (project, engine, &pluginHost.getSettings())
{
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
    setWantsKeyboardFocus (true);

    perfMonitor = std::make_unique<PerfMonitor>();

    themeManager.onThemeChanged = [this]
    {
        lookAndFeel.refreshColours();
        auto& desktop = juce::Desktop::getInstance();
        for (int i = 0; i < desktop.getNumComponents(); ++i)
            if (auto* c = desktop.getComponent (i))
            {
                c->sendLookAndFeelChange();
                c->repaint();
            }
        if (auto* dw = dynamic_cast<juce::DocumentWindow*> (getTopLevelComponent()))
        {
            dw->setBackgroundColour (Colors::windowBg);
            DarkTitleBar::applyThemeColours (*dw, Colors::header, Colors::text);
        }
    };
    themeManager.apply();

    {
        auto& s = pluginHost.getSettings();
        auto stateXml = s.getXmlValue ("audioDeviceState");
        if (! s.containsKey ("followDefaultOutput"))
            AppSettings::setFollowDefaultOutput (s, stateXml == nullptr);

        if (AppSettings::followDefaultOutput (s))
            selectDefaultOutputDevice();
        else if (stateXml != nullptr)
            engine.deviceManager.initialise (2, 2, stateXml.get(), true);
    }

    deviceChangeRelay.onChange = [this] { applyFollowDefaultOutput(); };
    engine.deviceManager.addChangeListener (&deviceChangeRelay);

    AppSettings::applyProcessPriority (AppSettings::processPriority (pluginHost.getSettings()));
    engine.setSmartDisableEnabled (AppSettings::smartDisable (pluginHost.getSettings()));
    engine.setMulticoreRenderingEnabled (AppSettings::multicoreRendering (pluginHost.getSettings()));

    engine.setRecordingFolder (AppSettings::recordedDir (pluginHost.getSettings()));
    engine.setAudioRecordingEnabled (AppSettings::recAudio (pluginHost.getSettings()));
    engine.onRecordingFinished = [this] (int mixerTrack, juce::File file, double startTicks,
                                         double lengthSeconds, double latencySeconds)
    {
        if (playlist == nullptr)
            return;
        double bpm = juce::jmax (1.0, (double) project.getProperty (IDs::bpm, 140.0));
        double startBeats  = juce::jmax (0.0, startTicks / (double) Model::PPQ - latencySeconds * bpm / 60.0);
        double lengthBeats = juce::jmax (0.25, lengthSeconds * bpm / 60.0);

        int lane = chooseRecordLane (mixerTrack, startBeats);
        recordingLaneCache.erase (mixerTrack);

        bool inSession = session.isActive();
        juce::String recordedBy = inSession ? juce::String (session.getLocalPeerId()) : juce::String();
        playlist->addRecordedClip (file, lane, startBeats, lengthBeats, inSession, recordedBy);

        if (! engine.isSongMode())
            project.setProperty (IDs::songMode, true, nullptr);
    };

    {
        auto& s = pluginHost.getSettings();
        auto fresh = Model::createProjectFromTemplate (AppSettings::startupTemplate (s),
                                                       AppSettings::defaultBpm (s));
        project.copyPropertiesAndChildrenFrom (fresh, nullptr);
        dirty = false;
    }

    engine.onParamChanged = [this] (const juce::String& key, int idx, float value)
    { session.broadcastParam (key, idx, value); };

    engine.onNonParamStateChanged = [this] (const juce::String& key)
    {
        juce::MessageManager::callAsync ([this, key]
        {
            if (session.isActive())
                session.broadcastPluginState (key);
        });
    };

    engine.onTransportBroadcast = [this] (int cmd, double pos)
    { session.broadcastTransport (cmd, pos); };

    engine.onPluginRemoved = [this] (const juce::String& key)
    { pluginWindows.close (key); session.dropCachedState (key); };

    engine.onPluginCreated = [this] (const juce::String& key)
    { session.applyCachedStateOnCreate (key); };

    engine.onPluginLoading = [this] (const juce::String& pluginName)
    { toasts.post ("Loading " + pluginName + "...", ToastLayer::Kind::info, 3000); };

    session.isPluginWindowOpen = [this] (const juce::String& key)
    { return pluginWindows.isOpen (key); };

    session.getPluginInteraction = [this] (const juce::String& key)
    { return pluginWindows.getLastInteraction (key); };

    pluginWindows.onWindowClosed = [this] (const juce::String& key)
    { session.broadcastPluginState (key); };

    pluginWindows.onStateChanged = [this] (const juce::String& key)
    { session.broadcastPluginState (key); };

    session.onStatus = [this] (const juce::String& s)
    {
        transportBar.setSessionText (s);
        transportBar.setSessionActive (session.isActive(), session.getRole() == Session::Role::host);
        transportBar.updatePeerStrips (session.getRoster(), session.getLocalPeerId());
        if (browser != nullptr)
            browser->setMultiplayerFolder (multiplayerFolderForBrowser (session));
    };
    transportBar.onLeaveSession = [this] { leaveSession(); };

    transportBar.getPeerAvatar = [this] (juce::int64 id) { return session.getAvatarFor (id); };
    session.onAvatarChanged = [this] (juce::int64 id, const juce::MemoryBlock& bytes)
    {
        friends.cacheAvatar (id, bytes);
    };

    session.onRosterChanged = [this]
    {
        transportBar.updatePeerStrips (session.getRoster(), session.getLocalPeerId());

        auto local = session.getLocalPeerId();
        std::set<juce::int64> nowAfk;
        for (auto& u : session.getRoster())
        {
            if (u.peerId == local)
                continue;
            if (u.afk)
            {
                nowAfk.insert (u.peerId);
                if (afkPeersSeen.find (u.peerId) == afkPeersSeen.end())
                {
                    logActivity (u.name, "is AFK");
                    toasts.post (u.name + " is now AFK", ToastLayer::Kind::warning);
                }
            }
            else if (afkPeersSeen.find (u.peerId) != afkPeersSeen.end())
            {
                logActivity (u.name, "is back");
                toasts.post (u.name + " is back", ToastLayer::Kind::success);
            }
        }
        afkPeersSeen.swap (nowAfk);
    };
    transportBar.onDeafenToggled = [this] (bool d)
    {
        engine.setDeafened (d);
        session.broadcastUserState (1.0f, d, localAfk);
    };
    transportBar.onInputMuteToggled = [this] (bool m)
    {
        engine.setInputMuted (m);
        AppSettings::setMicMuted (pluginHost.getSettings(), m);
    };
    transportBar.setLocalMicMute (AppSettings::micMuted (pluginHost.getSettings()));
    engine.setInputMuted (AppSettings::micMuted (pluginHost.getSettings()));
    transportBar.updatePeerStrips (session.getRoster(), session.getLocalPeerId());

    engine.onAudition = [this] (int channelId, bool on, int note, float velocity)
    {
        captureRecordedNote (channelId, on, note, velocity);
        if (! session.isActive() || ! transportBar.isSharingAuditions())
            return;
        session.broadcastAudition (channelId, on, note, velocity);
    };
    session.onPeerAudition = [this] (juce::int64 peerId, int channelId, bool on, int note, float velocity)
    {
        if (! transportBar.isListeningToPeer (peerId))
            return;
        if (note < 0 || note > 127)
            return;
        engine.suppressAuditionBroadcast.store (true);
        if (on) engine.auditionNote (channelId, note, juce::jlimit (0, 127, juce::roundToInt (velocity * 127.0f)));
        else    engine.auditionNoteOff (channelId, note);
        engine.suppressAuditionBroadcast.store (false);

        if (pianoRoll != nullptr && pianoRoll->getChannelId() == channelId)
            pianoRoll->setKeyHighlight (note, on);
        else if (! on && pianoRoll != nullptr)
            pianoRoll->setKeyHighlight (note, false);
    };

    localUserId = AppSettings::localUserId (pluginHost.getSettings());
    session.setLocalPeerId (localUserId);
    session.setDisplayName (AppSettings::displayName (pluginHost.getSettings()));
    pushLocalAvatar();

    discord.onActivityJoin = [this] (const juce::String& secret)
    {
        if (! secret.startsWith ("odaw1:"))
            return;
        auto body = secret.fromFirstOccurrenceOf ("odaw1:", false, false);
        auto uid = body.upToFirstOccurrenceOf (":", false, false).getLargeIntValue();
        auto token = body.fromFirstOccurrenceOf (":", false, false);
        if (uid == 0 || uid == localUserId)
            return;
        if (session.isActive())
        {
            toasts.post ("Leave your current session before joining another", ToastLayer::Kind::warning);
            return;
        }
        toasts.post ("Joining a Discord friend's session...", ToastLayer::Kind::info);
        beginRelayJoin (uid, {}, 0, token);
    };
    discord.onJoinRequest = [this] (const juce::String& userId, const juce::String& userName)
    {
        auto who = userName.isNotEmpty() ? userName : juce::String ("A Discord user");
        Dialogs::showOkCancelAsync (juce::MessageBoxIconType::QuestionIcon,
                                    "Discord join request",
                                    who + " wants to join your session.",
                                    "Accept", "Decline",
                                    [this, userId] (bool accept)
                                    {
                                        if (accept) discord.acceptJoinRequest (userId);
                                        else        discord.declineJoinRequest (userId);
                                    });
    };
    discord.start (AppSettings::discordRpc (pluginHost.getSettings()));

    pluginHost.onPluginsChanged = [this] { session.refreshLocalInventory(); };
    session.onPeerCursor = [this] (juce::int64 id, const juce::String& name, float nx, float ny)
    { peerCursors.updatePeer (id, name, nx, ny); };
    peerCursors.getAvatar = [this] (juce::int64 id) { return session.getAvatarFor (id); };

    session.onPeerJoined = [this] (const juce::String& name)
    {
        toasts.post (name + " joined the session", ToastLayer::Kind::success);
        logActivity (name, "joined the session");
        if (localViewport.isNotEmpty())
            session.broadcastViewport (localViewport,
                (localViewport == "Piano Roll" && pianoRoll != nullptr) ? pianoRoll->getPatternId() : -1);
    };
    session.onPeerLeft = [this] (const juce::String& name)
    {
        toasts.post (name + " left the session", ToastLayer::Kind::info);
        logActivity (name, "left the session");
        peerCursors.clear();
        for (auto it = peerViewports.begin(); it != peerViewports.end();)
            (it->second.name == name) ? it = peerViewports.erase (it) : ++it;
        refreshPeerBadges();
    };
    session.onSessionEnded = [this] (const juce::String& hostName)
    {
        auto who = hostName.isNotEmpty() ? hostName : juce::String ("the host");
        toasts.post (who + " ended the session - you keep your copy of the project",
                     ToastLayer::Kind::info);
        logActivity (who, "ended the session");
        resetSessionUiState();
    };
    session.onKicked = [this] (const juce::String& hostName)
    {
        auto who = hostName.isNotEmpty() ? hostName : juce::String ("the host");
        toasts.post ("Kicked from the session by " + who, ToastLayer::Kind::warning);
        logActivity (who, "kicked you from the session");
        resetSessionUiState();
    };
    session.onJoinRejected = [this] (const juce::String& reason)
    {
        toasts.post ("Could not join: " + reason, ToastLayer::Kind::error, 6000);
        resetSessionUiState();
    };
    session.onSnapshotApplied = [this]
    {
        lastWorkFlushMs = juce::Time::currentTimeMillis();
        history.clear();
        updateWindowTitle();

        if (! relayJoinAwaitingSnapshot)
            return;
        relayJoinAwaitingSnapshot = false;
        relayJoinFallbackText.clear();
        relayJoinFallbackPort = 0;
        if (chat != nullptr) { chat->clearChat(); chat->addSystemMessage ("You joined the session (via relay)"); }
        afkPeersSeen.clear();
    };
    session.onProjectSavedByPeer =[this] (const juce::String& saverName, const juce::String& projectName)
    {
        toasts.post ("\"" + projectName + "\" saved by " + saverName, ToastLayer::Kind::success);
        logActivity (saverName, "saved the project");
    };
    session.onRenderedByPeer = [this] (const juce::String& renderName, const juce::String& fileName)
    {
        toasts.post (renderName + " rendered \"" + fileName + "\"", ToastLayer::Kind::success);
        logActivity (renderName, "rendered " + fileName);
    };
    session.onVstAddedByPeer = [this] (const juce::String& senderName, const juce::String& key, const juce::String& pluginName)
    {
        auto where = vstAddedLocation (key);
        toasts.post (senderName + " is adding " + pluginName + where,
                     ToastLayer::Kind::warning);
        logActivity (senderName, "added " + pluginName + where);
        autoOpenPeerVst (key, pluginName, 20);
    };

    session.onResyncing = [this] (const juce::String& msg)
    {
        toasts.post (msg, ToastLayer::Kind::warning, 4000);
        logActivity ("Session", msg);
    };

    session.onSoloChangedByPeer = [this] (const juce::String& byName, const juce::String& target, bool on)
    {
        auto verb = on ? juce::String (" soloed ") : juce::String (" unsoloed ");
        toasts.post (byName + verb + target, ToastLayer::Kind::info);
        logActivity (byName, (on ? "soloed " : "unsoloed ") + target);
    };

    session.onRenderLockChanged = [this] (const juce::String& byName, bool locked)
    {
        if (locked)
            showRenderLock (byName);
        else
            hideRenderLock();
    };

    session.onCompatibilityChanged = [this]
    {
        if (! session.isActive())
            return;
        auto r = session.getCompatReport();
        if (r.peerCount == 0)
            return;
        if (r.localTotal == lastCompatReport.localTotal
            && r.shared == lastCompatReport.shared
            && r.missingFromPeers == lastCompatReport.missingFromPeers
            && r.peerCount == lastCompatReport.peerCount)
            return;
        lastCompatReport = r;

        auto msg = "Smart VST Match: " + juce::String (r.shared) + " of your "
                 + juce::String (r.localTotal) + " plugins shared with everyone";
        if (r.missingFromPeers > 0)
            msg += juce::String (juce::CharPointer_UTF8 (" \xC2\xB7 ")) + "missing " + juce::String (r.missingFromPeers)
                 + (r.missingFromPeers == 1 ? " plugin peers have" : " plugins peers have");
        toasts.post (msg, ToastLayer::Kind::info, 6000);
    };

    session.onPeerViewport = [this] (juce::int64 peerId, const juce::String& name,
                                     const juce::String& panelName, int patternId)
    {
        if (panelName.isEmpty())
            peerViewports.erase (peerId);
        else
            peerViewports[peerId] = { name, panelName, patternId };
        refreshPeerBadges();
    };

    session.onPeerReaction = [this] (juce::int64 peerId, const juce::String& name,
                                     const juce::String& emoji, float nx, float ny)
    {
        reactions.addReaction (peerId, name, emoji, nx, ny);
        logActivity (name, "reacted " + emoji);
    };

    auto chatFromHiddenPeer = [this] (juce::int64 peerId)
    {
        return peerId != session.getLocalPeerId() && (chat == nullptr || ! chat->isVisible());
    };
    auto noteChatActivity = [this, chatFromHiddenPeer] (juce::int64 peerId)
    {
        if (chatFromHiddenPeer (peerId))
            transportBar.setChatUnread (true);
    };
    session.onChatText = [this, noteChatActivity, chatFromHiddenPeer] (juce::int64 peerId, const juce::String& name, const juce::String& text)
    {
        if (chat != nullptr) chat->addTextMessage (peerId, name, text);
        noteChatActivity (peerId);
        if (chatFromHiddenPeer (peerId))
        {
            auto shown = text.length() > 140
                       ? text.substring (0, 140) + juce::String (juce::CharPointer_UTF8 ("\xe2\x80\xa6"))
                       : text;
            toasts.post (name + ": " + shown, ToastLayer::Kind::info);
        }
    };
    session.onChatImageAnnounce = [this, noteChatActivity, chatFromHiddenPeer] (juce::int64 peerId, const juce::String& name, juce::int64 imageId)
    {
        if (chat != nullptr) chat->addImageAnnounce (peerId, name, imageId);
        noteChatActivity (peerId);
        if (chatFromHiddenPeer (peerId))
            toasts.post (name + " sent an image", ToastLayer::Kind::info);
    };
    session.onChatImageData = [this] (juce::int64 imageId, juce::Image image, const juce::MemoryBlock& bytes)
    {
        if (chat != nullptr) chat->addImageData (imageId, image, bytes);
    };
    session.onChatImageRejected = [this]
    { toasts.post ("Image too large to send", ToastLayer::Kind::warning); };
    session.onAudioDownloadStarted = [this] (const juce::String& soundName)
    { toasts.post ("Downloading \"" + soundName + "\"...", ToastLayer::Kind::info, 2500); };
    session.onAudioDownloadFinished = [this] (const juce::String& soundName)
    {
        toasts.post ("Downloaded \"" + soundName + "\"", ToastLayer::Kind::success, 3500);
        if (playlist != nullptr)
            playlist->repaintCanvas();
    };
    session.onMultiplayerFolderChanged = [this] (const juce::File&)
    {
        if (browser != nullptr)
        {
            auto folder = multiplayerFolderForBrowser (session);
            browser->setMultiplayerFolder (folder);
            if (folder.isDirectory())
                browser->refreshMultiplayerFolder();
        }
    };

    engine.onPluginMissing = [this] (int channelId, const juce::String& pluginName)
    {
        if (warnedMissingChannels.insert (channelId).second)
            toasts.post ("Missing plugin \"" + pluginName
                         + "\" - using a placeholder piano so you can still hear it.",
                         ToastLayer::Kind::warning, 6500);
    };

    engine.onEffectMissing = [this] (const juce::String& key, const juce::String& pluginName)
    {
        if (warnedMissingEffects.insert (key).second)
            toasts.post ("Missing effect \"" + pluginName
                         + "\" - it isn't installed on this machine, so its slot is bypassed here.",
                         ToastLayer::Kind::warning, 6500);
    };

    lanDiscovery.peopleProvider = [this] { return presencePeopleCount(); };
    lanDiscovery.presenceNameProvider = [this] { return localDisplayName(); };
    lanDiscovery.presenceStatusProvider = [this] { return presenceStatusString(); };
    lanDiscovery.presenceSessionPortProvider = [this] { return presenceSessionPort(); };
    lanDiscovery.presenceAfkProvider = [this] { return localAfk; };
    lanDiscovery.presenceAfkSinceProvider = [this] { return afkSinceMs; };
    lanDiscovery.onPresence = [this] (juce::int64 id, const juce::String& name,
                                      const juce::String& status, int people, const juce::String& address,
                                      int sessionPort, bool afk, juce::int64 afkSince)
    {
        friends.notePresence (id, name, status, people, address, sessionPort, false, {}, afk, afkSince);
    };
    lanDiscovery.onInviteReceived = [this] (juce::int64 fromId, const juce::String& fromName,
                                            const juce::String& sessionName, const juce::String& address, int port)
    {
        handleFriendInvite (fromId, fromName, sessionName, address, port, true);
    };
    lanDiscovery.onFriendRequest = [this] (juce::int64 fromId, const juce::String& fromName, const juce::String&)
    {
        handleIncomingFriendRequest (fromId, fromName);
    };
    lanDiscovery.onFriendResponse = [this] (juce::int64 fromId, const juce::String& fromName, bool accepted)
    {
        handleIncomingFriendResponse (fromId, fromName, accepted);
    };
    friends.onFriendsChanged = [this]
    {
        relay.notifyFriendsChanged();
        if (friendsUi != nullptr && friendsPanel != nullptr && friendsPanel->isVisible())
            friendsUi->refresh();
    };
    friends.onFriendPresenceChanged = [this] (juce::int64, const juce::String& name, bool online)
    {
        toasts.post (online ? name + " is now online!" : name + " is now offline.",
                     online ? ToastLayer::Kind::success : ToastLayer::Kind::info);
    };
    lanDiscovery.startListening();
    lanDiscovery.startPresence (localUserId);

    {
        auto& settings = pluginHost.getSettings();
        relay.configure (AppSettings::relayHost (settings), AppSettings::relayPort (settings),
                         AppSettings::relayEnabled (settings));
    }
    relay.nameProvider = [this] { return localDisplayName(); };
    relay.statusProvider = [this] { return presenceStatusString(); };
    relay.peopleProvider = [this] { return presencePeopleCount(); };
    relay.sessionPortProvider = [this] { return presenceSessionPort(); };
    relay.inviteCodeProvider = [this] { return lastInviteCode; };
    relay.joinTokenProvider = [this] { return hostJoinToken; };
    relay.afkProvider = [this] { return localAfk; };
    relay.afkSinceProvider = [this] { return afkSinceMs; };
    relay.friendIdsProvider = [this] () -> juce::Array<juce::int64>
    {
        juce::Array<juce::int64> ids;
        for (auto& f : friends.getFriends())
            ids.add (f.info.id);
        return ids;
    };
    relay.watchIdsProvider = [this] () -> juce::Array<juce::int64>
    {
        juce::Array<juce::int64> ids;
        for (auto& f : friends.getFriends())
            ids.add (f.info.id);
        for (auto& r : friends.getIncomingRequests())
            ids.add (r.id);
        for (auto& r : friends.getOutgoingRequests())
            ids.add (r.id);
        return ids;
    };
    relay.onPresence = [this] (juce::int64 id, const juce::String& name,
                               const juce::String& status, int people, const juce::String& address,
                               int sessionPort, const juce::String& inviteCode, const juce::String& avatarHash,
                               bool afk, juce::int64 afkSince)
    {
        friends.notePresence (id, name, status, people, address, sessionPort, true, inviteCode, afk, afkSince);
        if (avatarHash.isNotEmpty() && avatarHash != friends.avatarHashFor (id))
            relay.requestAvatar (id);
    };
    relay.onAvatar = [this] (juce::int64 id, const juce::MemoryBlock& png)
    {
        friends.cacheAvatar (id, png);
    };
    relay.avatarProvider = [this] () -> juce::MemoryBlock
    {
        auto& s = pluginHost.getSettings();
        if (! AppSettings::hasAvatar (s))
            return {};
        juce::MemoryBlock mb;
        AppSettings::avatarFile (s).loadFileAsData (mb);
        return mb;
    };
    relay.onFriendLastSeen = [this] (juce::int64 id, juce::int64 lastSeenMs, const juce::String& name)
    {
        friends.noteServerLastSeen (id, lastSeenMs, name);
    };
    relay.onFriendRequest = [this] (juce::int64 fromId, const juce::String& name)
    {
        handleIncomingFriendRequest (fromId, name);
    };
    relay.onFriendResponse = [this] (juce::int64 fromId, const juce::String& name, bool accepted)
    {
        handleIncomingFriendResponse (fromId, name, accepted);
    };
    relay.onInvite = [this] (juce::int64 fromId, const juce::String& fromName, const juce::String& sessionName,
                             const juce::String& inviteCode, const juce::String& address, int port)
    {
        auto joinText = inviteCode.isNotEmpty() ? inviteCode
                                                : address + ":" + juce::String (port);
        handleFriendInvite (fromId, fromName, sessionName, joinText, port, false);
    };
    relay.onBridgeRequest = [this] (const juce::String& token, juce::int64 fromId,
                                    const juce::String&, const juce::String& joinToken)
    {
        if (session.getRole() != Session::Role::host || hostedPort <= 0 || token.isEmpty())
            return;
        const bool bearer = hostJoinToken.isNotEmpty() && joinToken == hostJoinToken;
        if (! bearer && ! friends.isFriend (fromId))
            return;
        RelayTunnel::startHostBridge (relay.getHost(), relay.getPort(), token, hostedPort);
    };
    relay.onJoinOk = [this] (const juce::String& token)
    {
        if (! relayJoinPending || token.isEmpty())
            return;
        if (session.isActive())
        {
            relayJoinPending = false;
            return;
        }
        relayJoinPending = false;
        relayJoinTunnel = std::make_unique<RelayJoinTunnel> (relay.getHost(), relay.getPort(), token);
        int localPort = relayJoinTunnel->getLocalPort();
        if (localPort <= 0 || ! session.joinHost ("127.0.0.1", localPort))
        {
            relayJoinTunnel.reset();
            runRelayJoinFallback();
            return;
        }
        relayJoinAwaitingSnapshot = true;
        int attempt = ++relayJoinAttempt;
        auto safeThis = juce::Component::SafePointer<MainComponent> (this);
        juce::Timer::callAfterDelay (10000, [safeThis, attempt]
        {
            if (safeThis == nullptr)
                return;
            if (! safeThis->relayJoinAwaitingSnapshot || safeThis->relayJoinAttempt != attempt)
                return;
            safeThis->relayJoinAwaitingSnapshot = false;
            if (safeThis->session.getRole() == Session::Role::client)
                safeThis->session.disconnect();
            safeThis->relayJoinTunnel.reset();
            safeThis->toasts.post ("Relay bridge didn't come up - trying a direct connection...",
                                   ToastLayer::Kind::info);
            safeThis->runRelayJoinFallback();
        });
    };
    relay.onJoinFail = [this] (const juce::String&)
    {
        if (! relayJoinPending)
            return;
        relayJoinPending = false;
        relayJoinAwaitingSnapshot = false;
        runRelayJoinFallback();
    };
    relay.start (localUserId);

   #if ! OPENDAW_PUBLIC_BUILD
    license.onStateChanged = [this] (License::State st)
    {
        if (preferencesWindow != nullptr && preferencesWindow->content != nullptr)
            preferencesWindow->content->setLicenseInfo (license.statusText(), license.hwid());
        if (st == License::State::denied)
            toasts.post ("Unlicensed build: " + license.statusText()
                         + ". Contact the author for access.",
                         ToastLayer::Kind::warning, 8000);
        if (st == License::State::authorized && ! updateChecked)
        {
            updateChecked = true;
            updater.checkForUpdate();
        }
    };
    license.start();

    updater.onUpdateAvailable = [this] (const juce::String& version,
                                        const juce::String& notes,
                                        const juce::String& url)
    {
        auto message = "mikeDAW " + version + " is available (you have "
                     + juce::String (JUCE_APPLICATION_VERSION_STRING) + ").";
        if (notes.isNotEmpty())
        {
            juce::StringArray lines;
            lines.addLines (notes);
            lines.trim();
            lines.removeEmptyStrings();
            message += "\n";
            for (auto& line : lines)
                message += "\n" + (line.startsWith ("-") ? line : "- " + line);
        }
        Dialogs::showOkCancelAsync (juce::MessageBoxIconType::InfoIcon,
                                    "Update available", message,
                                    "Download", "Later",
                                    [this, url] (bool ok)
        {
            if (ok) updater.downloadAndInstall (url);
        });
    };
    updater.onStatus = [this] (const juce::String& m)
    {
        toasts.post (m, ToastLayer::Kind::info, 4000);
    };
    updater.onError = [this] (const juce::String& m)
    {
        toasts.post ("Update: " + m, ToastLayer::Kind::warning, 6000);
    };
   #endif

    transportBar.onPeerRightClick = [this] (juce::int64 id, juce::String name)
    { showPeerFriendMenu (id, name); };

    addAndMakeVisible (transportBar);
    transportBar.onTogglePanel = [this] (const juce::String& p) { togglePanel (p); };
    transportBar.setBrowserOpen (browserVisible);
    transportBar.onNew = [this] { newProject(); };
    transportBar.onSave = [this] { saveProject(); };
    transportBar.onSaveAs = [this] { saveProjectAs(); };
    transportBar.onSaveAsTemplate = [this] { saveAsTemplate(); };
    transportBar.onLoad = [this] { loadProject(); };
    transportBar.onOpenAutosaves = [this]
    {
        auto folder = AppSettings::autoSaveDir (pluginHost.getSettings());
        folder.createDirectory();
        folder.revealToUser();
    };
    transportBar.onOpenRecordings = [this]
    {
        auto folder = AppSettings::recordedDir (pluginHost.getSettings());
        folder.createDirectory();
        folder.revealToUser();
    };
    transportBar.onToggleRecord = [this] { toggleRecord(); };
    transportBar.onOpenRecent = [this] (const juce::File& f) { openRecentProject (f); };
    transportBar.getStartupTypeName = [this]
    { return AppSettings::startupTemplateName (AppSettings::startupTemplate (pluginHost.getSettings())); };
    transportBar.getRecentProjects = [this]
    { return AppSettings::recentProjects (pluginHost.getSettings()); };
    transportBar.onBeforePlay = [this] { syncPlayModeToActivePanel(); };
    transportBar.onRenderAudio = [this] { renderAudioDialog(); };
    transportBar.onExportFlZip = [this] { exportFlZip(); };
    transportBar.onExportMikeZip = [this] { exportProjectZip(); };
    transportBar.onShowPluginManager = [this] { showPluginManager(); };
    transportBar.onAudioSettings = [this] { showPreferences(); };
    transportBar.onSocialMenu = [this] (juce::Component*) { togglePanel ("friends"); };
    transportBar.onEditMenu = [this] (juce::Component* c) { showEditMenu (c); };
    transportBar.onToggleTypingKeyboard = [this] { handleAction (KeyBindings::Action::toggleTypingKeyboard); };
    transportBar.onToggleChat = [this] { togglePanel ("chat"); transportBar.setChatUnread (false); };

    if (pluginHost.getSettings().getBoolValue ("typingKeyboardOn", false))
    {
        typingKeyboardEnabled = true;
        transportBar.setTypingKeyboardActive (true);
    }

    keyBindings.load (pluginHost.getSettings());
    session.remoteApplyFlag = &history.suppress;
    history.onChanged = [this]
    {
        rack->setSelectedPattern (rack->getSelectedPatternId());
        if (rollPanel->isVisible())
            pianoRoll->setTarget (pianoRoll->getPatternId(), pianoRoll->getChannelId());
        repaint();
    };
    history.onConflict = [this] (const juce::String& msg)
    { toasts.post (msg, ToastLayer::Kind::warning, 5000); };

    history.applyParam = [this] (const juce::String& key, int idx, float v)
    {
        if (auto* proc = engine.getProcessorFor (key))
        {
            auto& params = proc->getParameters();
            if (juce::isPositiveAndBelow (idx, params.size()))
                params[idx]->setValueNotifyingHost (v);
        }
    };

    browser = std::make_unique<BrowserPanel> (engine, pluginHost.getSettings());
    browser->onWidthChanged = [this] { resized(); };
    browser->onClose = [this] { if (browserVisible) togglePanel ("browser"); };
    browser->setMultiplayerFolder (multiplayerFolderForBrowser (session));
    addAndMakeVisible (*browser);

    chat = std::make_unique<ChatPanel> (&pluginHost.getSettings());
    chat->getAvatar = [this] (juce::int64 id) { return session.getAvatarFor (id); };
    chat->getLocalPeerId = [this] { return session.getLocalPeerId(); };
    chat->onSendText = [this] (const juce::String& text) { session.sendChatText (text); };
    chat->onSendImageBytes = [this] (const juce::MemoryBlock& bytes) { session.sendChatImage (bytes); };
    chat->onImageClicked = [this] (const juce::Image& img, const juce::MemoryBlock& bytes) { showImageLightbox (img, bytes); };
    chat->onWidthChanged = [this] { resized(); };
    chat->onCloseRequested = [this] { chatVisible = false; chat->setVisible (false); resized(); };
    addAndMakeVisible (*chat);
    chat->setVisible (false);

    addAndMakeVisible (desk);

    desk.addAndMakeVisible (reactions);

    addAndMakeVisible (peerCursors);
    addAndMakeVisible (toasts);
    addMouseListener (this, true);

    middleClickCatcher.onMiddleDown = [this] (const juce::MouseEvent& e) { showReactionPickerAt (e); };
    juce::Desktop::getInstance().addGlobalMouseListener (&middleClickCatcher);

    pluginWindows.setProject (project);

    pluginWindows.onKeyPress = [this] (const juce::KeyPress& kp)
    {
        return keyBindings.match (kp) == KeyBindings::Action::playPause
               && handleAction (KeyBindings::Action::playPause);
    };

    auto addPanel = [this] (const juce::String& title, juce::Component* content,
                            juce::Rectangle<float> fraction, juce::Point<int> minSize) -> FloatingPanel*
    {
        auto* panel = new FloatingPanel (title, content);
        panel->onActivated = [this] (FloatingPanel* p)
        {
            activePanel = p;
            reactions.toFront (false);
            peerCursors.toFront (false);
            toasts.toFront (false);
        };
        panel->setDefaultPlacement (fraction, minSize);
        panels.add (panel);
        desk.addAndMakeVisible (panel);
        return panel;
    };

    playlist = new Playlist (project, engine);
    playlistPanel = addPanel ("ARRANGEMENT", playlist, { 0.015f, 0.014f, 0.637f, 0.376f }, { 480, 240 });

    rack = new ChannelRack (project, engine, pluginHost);
    rackPanel = addPanel ("CHANNEL RACK", rack, { 0.044f, 0.412f, 0.38f, 0.329f }, { 360, 160 });

    pianoRoll = new PianoRoll (project, engine, &pluginHost.getSettings());
    rollPanel = addPanel ("PIANO ROLL", pianoRoll, { 0.111f, 0.082f, 0.919f, 0.871f }, { 560, 360 });
    rollPanel->setVisible (false);

    mixerUi = new MixerPanel (project, engine, pluginHost);
    mixerUi->setSettings (&pluginHost.getSettings());
    mixerPanel = addPanel ("MIXER", mixerUi, { 0.059f, 0.141f, 0.919f, 0.918f }, { 520, 320 });
    mixerPanel->setVisible (false);

    samplerSettings = new SamplerSettingsPanel (engine);
    samplerPanel = addPanel ("CHANNEL SAMPLER", samplerSettings, { 0.14f, 0.10f, 0.62f, 0.72f }, { 680, 540 });
    samplerPanel->setMinimumPanelSize (680, 540);
    samplerSettings->onTitleChanged = [this] (juce::String suffix)
    {
        samplerPanel->setTitleSuffix (std::move (suffix));
    };
    samplerPanel->setVisible (false);
    samplerSettings->onTargetGone = [this]
    {
        samplerHasTarget = false;
        if (! samplerPanel->isDocked())
        {
            samplerPanel->setVisible (false);
            return;
        }
        if (dockTab == samplerPanel)
            dockTab = rackPanel;
        applyDockState();
    };

    friendsUi = new FriendsPanel (friends);
    friendsUi->isHosting = [this] { return session.getRole() == Session::Role::host; };
    friendsUi->isSessionActive = [this] { return session.isActive(); };
    friendsUi->onHostSession = [this] { hostSession(); };
    friendsUi->onJoinSession = [this] { joinSessionDialog(); };
    friendsUi->onLeaveSession = [this] { leaveSession(); };
    friendsUi->onJoinFriend = [this] (juce::int64 id)
    {
        if (session.isActive())
        {
            toasts.post ("Leave your current session before joining another", ToastLayer::Kind::warning);
            return;
        }
        auto code = friends.inviteCodeFor (id);
        if (code.isNotEmpty())
        {
            joinSessionText (code, friends.sessionPortFor (id), id);
            return;
        }
        auto address = friends.addressFor (id);
        auto port = friends.sessionPortFor (id);
        if (address.isEmpty() || port <= 0)
        {
            toasts.post ("That friend is not hosting a joinable session", ToastLayer::Kind::warning);
            return;
        }
        joinSessionText (address, port, id);
    };
    friendsUi->onInvite = [this] (juce::int64 id) { inviteFriend (id); };
    friendsUi->onRemoveFriend = [this] (juce::int64 id) { friends.removeFriend (id); relay.notifyStateChanged(); };
    friendsUi->onAddFriend = [this] (juce::int64 id, juce::String name) { sendFriendRequestTo (id, name); };
    friendsUi->onAcceptRequest = [this] (juce::int64 id, juce::String name) { respondToFriendRequest (id, name, true); };
    friendsUi->onDeclineRequest = [this] (juce::int64 id) { respondToFriendRequest (id, {}, false); };
    friendsUi->getLocalAvatar = [this] { return localAvatarImage; };
    friendsUi->getLocalName = [this] { return localDisplayName(); };
    friendsUi->getLocalStatus = [this] { return presenceStatusString(); };
    friendsUi->isLocalAway = [this] { return localAfk; };
    friendsUi->onSetAway = [this] (bool away) { setManualAway (away); };
    friendsPanel = addPanel ("SOCIAL", friendsUi, { 0.68f, 0.10f, 0.30f, 0.58f }, { 270, 320 });
    friendsPanel->setVisible (false);

    dockedLayout = AppSettings::dockedLayout (pluginHost.getSettings());
    if (dockedLayout)
    {
        auto& settings = pluginHost.getSettings();
        dockHeight = AppSettings::dockHeight (settings);
        dockCollapsed = AppSettings::dockCollapsed (settings);
        dockTab = dockPanelFor (AppSettings::dockTab (settings));

        playlistPanel->setDocked (true, true);
        for (auto* p : { rackPanel, rollPanel, mixerPanel, samplerPanel })
        {
            p->setDocked (true, false);
            p->onTitleChanged = [this] { refreshDockHeader(); };
        }

        dockHeader.onTabClicked = [this] (const juce::String& name)
        {
            auto* p = dockPanelFor (name);
            if (p == rollPanel)
            {
                pianoRoll->setTarget (pianoRoll->getPatternId(), pianoRoll->getChannelId());
                rollPanel->setTitleSuffix (pianoRoll->getTargetDescription());
            }
            showDockTab (p);
        };
        dockHeader.onToggleCollapsed = [this] { setDockCollapsed (! dockCollapsed); };
        dockHeader.onResizeStart = [this]
        {
            dockDragStartHeight = dockCollapsed ? DockHeader::height : dockHeader.getHeight() + rackPanel->getHeight();
        };
        dockHeader.onResizeDrag = [this] (int dy)
        {
            auto h = dockDragStartHeight - dy;
            if (dockCollapsed)
            {
                if (h < DockHeader::height + 60)
                    return;
                dockCollapsed = false;
                applyDockState();
            }
            dockHeight = h;
            resized();
        };
        dockHeader.onResizeEnd = [this]
        {
            if (! dockCollapsed)
                dockHeight = dockHeader.getHeight() + rackPanel->getHeight();
            AppSettings::setDockHeight (pluginHost.getSettings(), dockHeight);
        };
        desk.addAndMakeVisible (dockHeader);

        statusBar.getSessionText = [this]() -> juce::String
        {
            if (session.getRole() == Session::Role::host)
                return "Hosting - " + juce::String (presencePeopleCount()) + " connected";
            if (session.isActive())
                return "In session - " + juce::String (presencePeopleCount()) + " connected";
            return "Offline - host or join a session";
        };
        statusBar.isSessionActive = [this] { return session.isActive(); };
        statusBar.getEngineText = [this]() -> juce::String
        {
            return engine.deviceManager.getCurrentAudioDevice() != nullptr ? "Ready" : "No device";
        };
        statusBar.getFormatText = [this]() -> juce::String
        {
            auto* device = engine.deviceManager.getCurrentAudioDevice();
            if (device == nullptr)
                return {};
            auto khz = device->getCurrentSampleRate() / 1000.0;
            auto rate = std::abs (khz - std::round (khz)) < 0.01 ? juce::String ((int) std::round (khz))
                                                                 : juce::String (khz, 1);
            return rate + " kHz  " + juce::String (device->getCurrentBufferSizeSamples()) + " smp";
        };
        statusBar.getCpuLoad = [this] { return engine.getAudioPerfStats().load.load (std::memory_order_relaxed); };
        statusBar.onSessionClicked = [this] { togglePanel ("friends"); };
        addAndMakeVisible (statusBar);

        applyDockState();
    }

    rack->onOpenPianoRoll = [this] (int pid, int cid)
    {
        openPianoRoll (pid, cid);
        juce::Timer::callAfterDelay (0, [safe = juce::Component::SafePointer<FloatingPanel> (rollPanel)]
        {
            if (safe != nullptr && ! safe->isDocked())
                safe->toFront (true);
        });
    };

    rack->onOpenPluginEditor = [this] (const juce::String& key, const juce::String& title)
    { openPluginEditor (key, title); };

    rack->onLocalVstAdded = [this] (const juce::String& key, const juce::String& name)
    {
        session.broadcastVstAdded (key, name);
        if (session.isActive())
            logActivity (session.getRoster().front().name, "added " + name);
    };

    rack->onChannelRemoved = [this] (const juce::String& name, const juce::String& kind)
    { reportDeletion (kind == "sampler" ? "sound " + name : name); };

    rack->isPluginEnabled = [this] (const juce::PluginDescription& d)
    { return session.isPluginCompatible (d); };

    rack->isPluginNew = [this] (const juce::PluginDescription& d)
    { return pluginHost.newlyFound.contains (d.createIdentifierString()); };

    rack->getBrowserFile = [this] { return browser->getSelectedFile(); };
    pianoRoll->getBrowserFile = [this] { return browser->getSelectedFile(); };

    auto sendToTargetChannel = [this] () -> juce::ValueTree
    {
        auto isSampler = [] (const juce::ValueTree& ch)
        {
            auto type = ch.getProperty (IDs::type).toString();
            return ch.isValid() && type != "plugin" && type != "builtin";
        };
        auto ch = Model::findChannelById (project, lastSoundChannelId);
        if (isSampler (ch))
            return ch;
        ch = Model::findChannelById (project, rack->getLastSelectedChannelId());
        return isSampler (ch) ? ch : juce::ValueTree();
    };
    browser->getSendToChannelLabel = [sendToTargetChannel] () -> juce::String
    {
        auto ch = sendToTargetChannel();
        return ch.isValid() ? ch.getProperty (IDs::name).toString() : juce::String();
    };
    browser->onSendToSelectedChannel = [sendToTargetChannel] (const juce::File& file)
    {
        auto ch = sendToTargetChannel();
        if (ch.isValid())
            ch.setProperty (IDs::samplePath, file.getFullPathName(), nullptr);
    };

    rack->onOpenSamplerSettings = [this] (juce::ValueTree channel)
    {
        lastSoundChannelId = (int) channel.getProperty (IDs::id);
        samplerSettings->setTarget (channel);
        samplerHasTarget = true;
        revealPanel (samplerPanel);
    };

    rack->onPatternChanged = [this]
    {
        auto id = rack->getSelectedPatternId();
        playlist->setSelectedPattern (id);
        if (rollPanel->isVisible())
            retargetPianoRoll (id, pianoRoll->getChannelId());
        playlist->repaint();
        refreshDockHeader();
    };

    playlist->onPatternSelected = [this] (int id)
    {
        rack->setSelectedPattern (id);
        if (rollPanel->isVisible())
            retargetPianoRoll (id, pianoRoll->getChannelId());
    };

    playlist->onPatternCreated = [this] (int id)
    {
        rack->setSelectedPattern (id);
        playlist->repaint();
    };

    pianoRoll->onTargetChanged = [this] (const juce::String& desc)
    { rollPanel->setTitleSuffix (desc); };

    pianoRoll->onPatternSelected = [this] (int id)
    {
        rack->setSelectedPattern (id);
        playlist->setSelectedPattern (id);
        playlist->repaint();
    };

    pianoRoll->onBeginUndoTransaction = [this] { history.beginTransaction(); };
    pianoRoll->onEndUndoTransaction   = [this] { history.endTransaction(); };

    playlist->getCurrentPatternId = [this] { return rack->getSelectedPatternId(); };
    playlist->getBrowserFile = [this] { return browser->getSelectedFile(); };
    playlist->getRecordingLane = [this] (int mixerTrack)
    {
        return chooseRecordLane (mixerTrack, engine.getRecordStartTicks() / (double) Model::PPQ);
    };
    playlist->getLocalPeerIdString = [this]
    {
        return session.isActive() ? juce::String (session.getLocalPeerId()) : juce::String();
    };

    playlist->onOpenPianoRoll = [this] (int patternId)
    {
        engine.setCurrentPatternId (patternId);
        rack->setSelectedPattern (patternId);
        playlist->setSelectedPattern (patternId);
        openPianoRoll (patternId, channelWithContent (patternId, pianoRoll->getChannelId()));
    };

    playlist->onOpenAudioClip = [this] (juce::ValueTree clip)
    {
        samplerSettings->setTarget (clip);
        samplerHasTarget = true;
        revealPanel (samplerPanel);
    };

    mixerUi->onOpenPluginEditor = [this] (const juce::String& key, const juce::String& title)
    { openPluginEditor (key, title); };

    mixerUi->onLocalVstAdded = [this] (const juce::String& key, const juce::String& name)
    {
        session.broadcastVstAdded (key, name);
        if (session.isActive())
            logActivity (session.getRoster().front().name, "added " + name + vstAddedLocation (key));
    };

    mixerUi->onEffectRemoved = [this] (const juce::String& name, int trackIndex)
    {
        reportDeletion (name + (trackIndex >= 0 ? vstAddedLocation ("m" + juce::String (trackIndex) + "s0")
                                                : juce::String()));
    };

    mixerUi->isPluginEnabled = [this] (const juce::PluginDescription& d)
    { return session.isPluginCompatible (d); };

    mixerUi->isPluginNew = [this] (const juce::PluginDescription& d)
    { return pluginHost.newlyFound.contains (d.createIdentifierString()); };

    mixerUi->onParamEdit = [this] (const juce::String& key, int idx, float oldV, float newV)
    {
        history.recordParam (key, idx, oldV, newV);
    };

    mixerUi->getSelectedChannels = [this] { return rack->getSelectedChannelIds(); };
    mixerUi->routeChannelToInsert = [this] (int channelId, int insert)
    {
        auto ch = Model::findChannelById (project, channelId);
        if (ch.isValid())
            ch.setProperty (IDs::mixerTrack,
                            juce::jlimit (1, juce::jmax (1, Model::getMaxMixerTrackIndex (project)), insert),
                            nullptr);
    };

    auto reportSolo = [this] (const juce::String& target, bool on)
    {
        auto actor = session.getRoster().front().name;
        toasts.post (juce::String (on ? "You soloed " : "You unsoloed ") + target,
                     ToastLayer::Kind::info);
        logActivity (actor, (on ? "soloed " : "unsoloed ") + target);
        session.broadcastSoloChanged (target, on);
    };
    rack->onSoloToggled = reportSolo;
    mixerUi->onSoloToggled = reportSolo;
    playlist->onSoloToggled = reportSolo;

    playlist->getPeersOnPattern = [this] (int patternId)
    {
        std::vector<Playlist::PeerAvatar> out;
        if (patternId <= 0)
            return out;
        for (auto& [id, pv] : peerViewports)
            if (pv.panelName == "Piano Roll" && pv.patternId == patternId)
                out.push_back ({ pv.name, session.getAvatarFor (id), Colors::peerColour (id) });
        return out;
    };

    rack->onContentHeightChanged = [this]
    {
        if (rackPanel->isDocked() || rackPanel->isMinimised() || rackPanel->isMaximised())
            return;
        int preferredH = FloatingPanel::titleHeight + 4 + rack->preferredContentHeight();
        int preferredW = 4 + rack->preferredContentWidth();
        auto deskArea = desk.getLocalBounds();
        if (! deskArea.isEmpty())
        {
            preferredH = juce::jmin (preferredH, deskArea.getHeight());
            preferredW = juce::jmin (preferredW, deskArea.getWidth());
        }
        rackPanel->setMaxHeight (preferredH);
        rackPanel->setMaxWidth (preferredW);
        auto b = rackPanel->getBounds();
        int autoW = juce::jmin (preferredW, 4 + rack->compactContentWidth());
        int newW = juce::jmin (preferredW, juce::jmax (autoW, b.getWidth()));
        int newH = juce::jmin (preferredH, juce::jmax (rackAutoHeightCap, b.getHeight()));
        if (b.getHeight() != newH || b.getWidth() != newW)
            rackPanel->setBounds (b.withHeight (newH).withWidth (newW));
        if (! deskArea.isEmpty())
            rackPanel->clampIntoDesk (deskArea);
    };

    setSize (1600, 900);

    activePanel = playlistPanel;
    if (! dockedLayout)
    {
        playlistPanel->toFront (true);
        playlistPanel->toggleMaximised();
    }

    dirty = false;
    project.addListener (this);

    applyAutoSaveSettings();

    lastWorkFlushMs = juce::Time::currentTimeMillis();

    lastActivityMs = juce::Time::currentTimeMillis();
    afkTimer.onTick = [this] { checkAfk(); };
    afkTimer.startTimer (10000);

    manualAway = AppSettings::manualAway (pluginHost.getSettings());
    if (manualAway)
    {
        setLocalAfk (true);
        if (auto since = AppSettings::manualAwaySince (pluginHost.getSettings()); since > 0 && since < afkSinceMs)
            afkSinceMs = since;
    }

    if (! AppSettings::welcomeShown (pluginHost.getSettings()))
        juce::Timer::callAfterDelay (600,
            [safe = juce::Component::SafePointer<MainComponent> (this)]
            {
                if (safe != nullptr)
                    safe->showWelcomeDialog();
            });
}

MainComponent::~MainComponent()
{
    stopTimer();
    pluginHost.clearPedalOnCleanExit();
    afkTimer.stopTimer();
    engine.deviceManager.removeChangeListener (&deviceChangeRelay);
    if (! AppSettings::followDefaultOutput (pluginHost.getSettings()))
        if (auto stateXml = engine.deviceManager.createStateXml())
            pluginHost.getSettings().setValue ("audioDeviceState", stateXml.get());
    pluginHost.getSettings().saveIfNeeded();

    juce::Desktop::getInstance().removeGlobalMouseListener (&middleClickCatcher);
    project.removeListener (this);
    pluginWindows.closeAll();
    session.clearCallbacks();
    session.disconnect();
    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
}

void MainComponent::togglePanel (const juce::String& name)
{
    if (name == "browser")
    {
        browserVisible = ! browserVisible;
        browser->setVisible (browserVisible);
        transportBar.setBrowserOpen (browserVisible);
        resized();
        return;
    }

    if (name == "chat")
    {
        if (chat == nullptr)
            return;
        chatVisible = ! chatVisible;
        chat->setVisible (chatVisible);
        resized();
        if (chatVisible)
            chat->grabInputFocus();
        return;
    }

    if (dockedLayout && (name == "playlist" || dockPanelFor (name) != nullptr))
    {
        if (name == "playlist")
        {
            setDockCollapsed (! dockCollapsed);
            return;
        }
        auto* target = dockPanelFor (name);
        if (target == dockTab && ! dockCollapsed)
        {
            setDockCollapsed (true);
            return;
        }
        if (target == rollPanel)
        {
            pianoRoll->setTarget (pianoRoll->getPatternId(), pianoRoll->getChannelId());
            rollPanel->setTitleSuffix (pianoRoll->getTargetDescription());
        }
        showDockTab (target);
        return;
    }

    FloatingPanel* p = nullptr;
    if (name == "rack")          p = rackPanel;
    else if (name == "roll")     p = rollPanel;
    else if (name == "playlist") p = playlistPanel;
    else if (name == "mixer")    p = mixerPanel;
    else if (name == "friends")  p = friendsPanel;

    if (p == nullptr)
        return;

    auto syncPresenceQuery = [this] { lanDiscovery.setPresenceQuerying (friendsPanel->isVisible()); };

    auto refreshRollTarget = [this]
    {
        pianoRoll->setTarget (pianoRoll->getPatternId(), pianoRoll->getChannelId());
        rollPanel->setTitleSuffix (pianoRoll->getTargetDescription());
    };

    auto capRackHeight = [this, p]
    {
        if (p != rackPanel || rackPanel->isMaximised() || rackPanel->isMinimised())
            return;
        auto b = rackPanel->getBounds();
        if (b.getHeight() > rackAutoHeightCap)
            rackPanel->setBounds (b.withHeight (rackAutoHeightCap));
    };

    if (p->isVisible() && p->isMinimised())
    {
        p->toggleMinimised();
        capRackHeight();
        p->toFront (true);
        activePanel = p;
        syncPresenceQuery();
        return;
    }

    bool justActivated = juce::Time::getMillisecondCounter() - lastWindowActivationMs < 400;
    if (p->isVisible() && p == frontmostChild() && ! justActivated)
    {
        p->setVisible (false);
        syncPresenceQuery();
        return;
    }

    if (! p->isVisible())
    {
        if (p == rollPanel) refreshRollTarget();
        capRackHeight();
        p->setVisible (true);
    }
    p->toFront (true);
    activePanel = p;
    syncPresenceQuery();
}

FloatingPanel* MainComponent::frontmostChild() const
{
    for (int i = desk.getNumChildComponents(); --i >= 0;)
        if (auto* fp = dynamic_cast<FloatingPanel*> (desk.getChildComponent (i)))
            if (fp->isVisible())
                return fp;
    return nullptr;
}

bool MainComponent::isDockPanel (FloatingPanel* p) const
{
    return dockedLayout && p != nullptr
        && (p == rackPanel || p == rollPanel || p == mixerPanel || p == samplerPanel);
}

FloatingPanel* MainComponent::dockPanelFor (const juce::String& name) const
{
    if (name == "rack")    return rackPanel;
    if (name == "roll")    return rollPanel;
    if (name == "mixer")   return mixerPanel;
    if (name == "sampler") return samplerPanel;
    return nullptr;
}

juce::String MainComponent::dockNameFor (FloatingPanel* p) const
{
    if (p == rackPanel)    return "rack";
    if (p == rollPanel)    return "roll";
    if (p == mixerPanel)   return "mixer";
    if (p == samplerPanel) return "sampler";
    return {};
}

void MainComponent::revealPanel (FloatingPanel* p)
{
    if (isDockPanel (p))
    {
        showDockTab (p);
        return;
    }
    p->setVisible (true);
    if (! p->isDocked())
        p->toFront (true);
    activePanel = p;
}

void MainComponent::showDockTab (FloatingPanel* p)
{
    if (p == nullptr)
        return;
    dockTab = p;
    dockCollapsed = false;
    activePanel = p;
    applyDockState();
}

void MainComponent::setDockCollapsed (bool collapsed)
{
    dockCollapsed = collapsed;
    activePanel = collapsed ? playlistPanel : dockTab;
    applyDockState();
}

void MainComponent::applyDockState()
{
    if (dockTab == nullptr || (dockTab == samplerPanel && ! samplerHasTarget))
        dockTab = rackPanel;

    for (auto* p : { rackPanel, rollPanel, mixerPanel, samplerPanel })
        p->setVisible (p == dockTab && ! dockCollapsed);

    auto& settings = pluginHost.getSettings();
    if (dockTab != samplerPanel)
        AppSettings::setDockTab (settings, dockNameFor (dockTab));
    AppSettings::setDockCollapsed (settings, dockCollapsed);

    refreshDockHeader();
    resized();
}

void MainComponent::refreshDockHeader()
{
    if (! dockedLayout)
        return;

    std::vector<DockHeader::Tab> tabs { { "rack",  "Channel Rack", "F6" },
                                        { "roll",  "Piano Roll",   "F7" },
                                        { "mixer", "Mixer",        "F9" } };
    if (samplerHasTarget)
        tabs.push_back ({ "sampler", "Sampler", {} });
    dockHeader.setTabs (std::move (tabs));
    dockHeader.setState (dockNameFor (dockTab), dockCollapsed);

    juce::StringArray parts;
    for (auto rest = dockTab->getTitleSuffix();;)
    {
        auto split = rest.indexOf (" - ");
        parts.add (rest.substring (0, split < 0 ? rest.length() : split).trim());
        if (split < 0)
            break;
        rest = rest.substring (split + 3);
    }
    parts.removeEmptyStrings();

    juce::String primary = panelDisplayName (dockTab), secondary;
    if (dockTab == rollPanel && parts.size() >= 2)
    {
        primary = parts[1];
        secondary = parts[0];
    }
    else if (dockTab == rackPanel)
    {
        auto pattern = Model::findPatternById (project, rack->getSelectedPatternId());
        if (pattern.isValid())
            secondary = pattern.getProperty (IDs::name).toString();
    }
    else if (! parts.isEmpty())
    {
        secondary = parts.joinIntoString (" / ");
    }
    dockHeader.setBreadcrumb (primary, secondary);
}

void MainComponent::layoutDockedDesk (juce::Rectangle<int> area)
{
    const int headerH = DockHeader::height;
    const int maxDock = juce::jmax (headerH, area.getHeight() - 140);
    const int dockH = dockCollapsed ? headerH
                                    : juce::jlimit (juce::jmin (headerH + 120, maxDock), maxDock, dockHeight);

    auto bottom = area.removeFromBottom (juce::jmin (dockH, area.getHeight()));
    playlistPanel->setBounds (area);
    dockHeader.setBounds (bottom.removeFromTop (headerH));
    for (auto* p : { rackPanel, rollPanel, mixerPanel, samplerPanel })
        p->setBounds (bottom);
}

int MainComponent::channelWithContent (int patternId, int currentChannelId) const
{
    auto pattern = Model::findPatternById (project, patternId);
    if (! pattern.isValid())
        return currentChannelId;

    auto hasContent = [] (juce::ValueTree seq)
    {
        if (! seq.isValid())
            return false;
        for (int i = 0; i < seq.getNumChildren(); ++i)
            if (seq.getChild (i).hasType (IDs::Note))
                return true;
        return seq.getProperty (IDs::steps).toString().containsChar ('1');
    };

    if (currentChannelId > 0
        && hasContent (pattern.getChildWithProperty (IDs::channelId, currentChannelId)))
        return currentChannelId;

    for (int i = 0; i < pattern.getNumChildren(); ++i)
    {
        auto seq = pattern.getChild (i);
        if (seq.hasType (IDs::ChannelSeq) && hasContent (seq))
            return (int) seq.getProperty (IDs::channelId);
    }

    return currentChannelId;
}

void MainComponent::retargetPianoRoll (int patternId, int channelId)
{
    if (channelId <= 0)
    {
        auto channels = Model::getChannels (project);
        if (channels.getNumChildren() > 0)
            channelId = (int) channels.getChild (0).getProperty (IDs::id);
    }

    pianoRoll->setTarget (patternId, channelId);
    rollPanel->setTitleSuffix (pianoRoll->getTargetDescription());
    refreshLocalViewportPattern();
}

void MainComponent::openPianoRoll (int patternId, int channelId)
{
    retargetPianoRoll (patternId, channelId);
    revealPanel (rollPanel);
    pianoRoll->grabKeyboardFocus();
}

void MainComponent::openPluginEditor (const juce::String& key, const juce::String& fallbackTitle)
{
    if (engine.getProcessorFor (key) == nullptr)
        pluginWindows.openLoading (key, fallbackTitle);
    openPluginEditorPolling (key, fallbackTitle, 12, false);
}

void MainComponent::openPluginEditorPolling (const juce::String& key, const juce::String& title,
                                             int attemptsLeft, bool respectWarnedGuard)
{
    if (auto* proc = engine.getProcessorFor (key))
    {
        auto name = engine.getPluginNameFor (key);
        pluginWindows.open (key, *proc, name.isNotEmpty() ? name : title);
        return;
    }
    if (engine.isPluginLoading (key))
    {
        attemptsLeft = juce::jmax (attemptsLeft, 2);
        pluginWindows.openLoading (key, title);
    }
    if (attemptsLeft <= 0)
    {
        pluginWindows.close (key);
        if (! respectWarnedGuard || warnedMissingEffects.find (key) == warnedMissingEffects.end())
        {
            auto name = title.isNotEmpty() ? title : juce::String ("This plugin");
            toasts.post (name + " isn't installed on this machine - its editor can't be opened",
                         ToastLayer::Kind::warning, 6500);
        }
        return;
    }

    juce::Timer::callAfterDelay (325,
        [safe = juce::Component::SafePointer<MainComponent> (this), key, title, attemptsLeft, respectWarnedGuard]
        {
            if (safe != nullptr)
                safe->openPluginEditorPolling (key, title, attemptsLeft - 1, respectWarnedGuard);
        });
}

void MainComponent::autoOpenPeerVst (const juce::String& key, const juce::String& pluginName, int attemptsLeft)
{
    openPluginEditorPolling (key, pluginName, attemptsLeft, true);
}

void MainComponent::newProject()
{
    if (session.getRole() == Session::Role::client)
    {
        toasts.post ("Leave the session before replacing the project", ToastLayer::Kind::warning, 4000);
        return;
    }
    confirmSaveIfDirty ([this]
    {
        auto& s = pluginHost.getSettings();
        auto fresh = Model::createProjectFromTemplate (AppSettings::startupTemplate (s),
                                                       AppSettings::defaultBpm (s));
        engine.stop();
        session.replaceProject (fresh);
        history.clear();
        currentProjectFile = juce::File();
        dirty = false;
        warnedMissingChannels.clear();
        warnedMissingEffects.clear();
        lastWorkFlushMs = juce::Time::currentTimeMillis();
        updateWindowTitle();
    });
}

void MainComponent::saveProject()
{
    saveProject (nullptr);
}

void MainComponent::saveProject (std::function<void (bool)> onDone)
{
   #if ! OPENDAW_PUBLIC_BUILD
    if (License::shouldDegrade())
    {
        toasts.post ("This build is not licensed - saving is disabled.",
                     ToastLayer::Kind::warning, 6000);
        if (onDone) onDone (false);
        return;
    }
   #endif

    if (currentProjectFile.existsAsFile())
    {
        flushWorkTime();
        engine.capturePluginStatesToTree();
        auto xml  = project.toXmlString();
        auto file = currentProjectFile;
        dirty = false;

        diskWrites.enqueue (file, xml,
            [safe = juce::Component::SafePointer<MainComponent> (this), file, onDone] (bool ok)
            {
                if (safe != nullptr)
                {
                    if (ok)
                        safe->finishSuccessfulSave (file);
                    else
                    {
                        safe->dirty = true;
                        Dialogs::showInfoAsync (juce::MessageBoxIconType::WarningIcon,
                                                "Save failed", file.getFullPathName());
                    }
                }
                if (onDone) onDone (ok);
            });
        return;
    }
    saveProjectAs (onDone);
}

void MainComponent::finishSuccessfulSave (const juce::File& savedFile)
{
    currentProjectFile = savedFile;
    AppSettings::addRecentProject (pluginHost.getSettings(), savedFile);
    Leaderboard::recordSave (pluginHost.getSettings(), savedFile,
                             (juce::int64) project.getProperty (IDs::secondsWorked, (juce::int64) 0),
                             (juce::int64) project.getProperty (IDs::createdEpoch, (juce::int64) 0));
    updateWindowTitle();
    bundleProjectSounds (savedFile);
    onProjectSavedSuccessfully();
}

void MainComponent::bundleProjectSounds (const juce::File& savedFile)
{
    auto samplesDir = savedFile.getParentDirectory()
                          .getChildFile (savedFile.getFileNameWithoutExtension() + " samples");

    juce::StringArray paths;
    auto addPath = [&paths] (const juce::String& p)
    {
        if (p.isNotEmpty() && ! p.startsWith ("builtin:"))
            paths.addIfNotAlreadyThere (p);
    };

    auto channels = Model::getChannels (project);
    for (int i = 0; i < channels.getNumChildren(); ++i)
        if (channels.getChild (i).hasType (IDs::Channel))
            addPath (channels.getChild (i).getProperty (IDs::samplePath).toString());

    auto audioFiles = Model::getAudioFiles (project);
    for (int i = 0; i < audioFiles.getNumChildren(); ++i)
        if (audioFiles.getChild (i).hasType (IDs::AudioFile))
            addPath (audioFiles.getChild (i).getProperty (IDs::audioPath).toString());

    auto playlist = project.getChildWithName (IDs::Playlist);
    for (int i = 0; i < playlist.getNumChildren(); ++i)
    {
        auto clip = playlist.getChild (i);
        if (clip.hasType (IDs::Clip) && clip.getProperty (IDs::type).toString() == "audio")
            addPath (clip.getProperty (IDs::path).toString());
    }

    std::vector<std::pair<juce::File, juce::File>> copies;
    for (auto& p : paths)
    {
        if (juce::File (p).existsAsFile())
            continue;
        auto source = engine.resolveSampleFile (p);
        if (! source.existsAsFile())
            continue;
        auto dest = samplesDir.getChildFile (source.getFileName());
        if (source == dest || source.isAChildOf (samplesDir))
            continue;
        if (dest.existsAsFile() && dest.getSize() == source.getSize())
            continue;
        copies.emplace_back (source, dest);
    }

    if (copies.empty())
        return;

    samplesDir.createDirectory();

    juce::Component::SafePointer<MainComponent> safe (this);
    juce::Thread::launch ([copies, safe]
    {
        int copied = 0;
        for (auto& c : copies)
            if (c.first.copyFileTo (c.second))
                ++copied;

        juce::MessageManager::callAsync ([safe, copied]
        {
            if (safe != nullptr && copied > 0)
                safe->toasts.post ("Bundled " + juce::String (copied)
                                       + (copied == 1 ? " sound" : " sounds") + " with the project",
                                   ToastLayer::Kind::success);
        });
    });
}

void MainComponent::relinkBundledSamples (juce::ValueTree loaded, const juce::Array<juce::File>& searchDirs)
{
    auto relink = [&searchDirs] (juce::ValueTree node, const juce::Identifier& prop)
    {
        auto path = node.getProperty (prop).toString();
        if (path.isEmpty() || path.startsWith ("builtin:"))
            return;
        if (juce::File (path).existsAsFile())
            return;
        auto fileName = juce::File (path).getFileName();
        for (auto& dir : searchDirs)
        {
            auto bundled = dir.getChildFile (fileName);
            if (bundled.existsAsFile())
            {
                node.setProperty (prop, bundled.getFullPathName(), nullptr);
                return;
            }
        }
    };

    auto channels = loaded.getChildWithName (IDs::Channels);
    for (int i = 0; i < channels.getNumChildren(); ++i)
        if (channels.getChild (i).hasType (IDs::Channel))
            relink (channels.getChild (i), IDs::samplePath);

    auto audioFiles = loaded.getChildWithName (IDs::AudioFiles);
    for (int i = 0; i < audioFiles.getNumChildren(); ++i)
        if (audioFiles.getChild (i).hasType (IDs::AudioFile))
            relink (audioFiles.getChild (i), IDs::audioPath);

    auto playlist = loaded.getChildWithName (IDs::Playlist);
    for (int i = 0; i < playlist.getNumChildren(); ++i)
    {
        auto clip = playlist.getChild (i);
        if (clip.hasType (IDs::Clip) && clip.getProperty (IDs::type).toString() == "audio")
            relink (clip, IDs::path);
    }
}

void MainComponent::checkMissingFiles()
{
    auto missing = MissingFiles::scan (project,
        [this] (const juce::String& p) { return engine.resolveSampleFile (p); });
    if (missing.empty())
        return;

    missingFilesWindow = std::make_unique<MissingFilesWindow> (project, std::move (missing),
        [this] { toasts.post ("Relinked all missing files", ToastLayer::Kind::success); });
    missingFilesWindow->setVisible (true);
    missingFilesWindow->toFront (true);
}

void MainComponent::saveProjectAs()
{
    saveProjectAs (nullptr);
}

void MainComponent::saveProjectAs (std::function<void (bool)> onDone)
{
    chooser = std::make_unique<juce::FileChooser> ("Save project",
        AppSettings::projectsFolder (pluginHost.getSettings()), "*.odaw");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode
                          | juce::FileBrowserComponent::canSelectFiles
                          | juce::FileBrowserComponent::warnAboutOverwriting,
        [this, onDone] (const juce::FileChooser& fc)
        {
            auto f = fc.getResult();
            if (f == juce::File())
            {
                if (onDone) onDone (false);
                return;
            }
            if (! f.hasFileExtension ("odaw"))
                f = f.withFileExtension ("odaw");

            flushWorkTime();
            engine.capturePluginStatesToTree();
            auto xml = project.toXmlString();
            dirty = false;

            juce::Component::SafePointer<MainComponent> safe (this);
            diskWrites.enqueue (f, xml,
                [safe, f, onDone] (bool ok)
                {
                    if (safe != nullptr)
                    {
                        if (ok)
                            safe->finishSuccessfulSave (f);
                        else
                        {
                            safe->dirty = true;
                            Dialogs::showInfoAsync (juce::MessageBoxIconType::WarningIcon,
                                                    "Save failed", f.getFullPathName());
                        }
                    }
                    if (onDone) onDone (ok);
                });
        });
}

juce::String MainComponent::currentProjectName() const
{
    if (currentProjectFile != juce::File())
        return currentProjectFile.getFileNameWithoutExtension();
    auto name = project.getProperty (IDs::name).toString();
    return (name.isEmpty() || name == "Untitled") ? juce::String ("Untitled") : name;
}

void MainComponent::onProjectSavedSuccessfully()
{
    auto name = currentProjectName();

    if (session.isActive())
    {
        auto safeName = juce::File::createLegalFileName (name);
        if (safeName.isEmpty())
            safeName = "Untitled";

        auto folder = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                          .getChildFile ("mikeDAW")
                          .getChildFile ("multiplayer")
                          .getChildFile ("projects")
                          .getChildFile (safeName);
        folder.createDirectory();

        engine.capturePluginStatesToTree();
        diskWrites.enqueue (folder.getChildFile (safeName + ".odaw"), project.toXmlString());

        auto sessionFolder = session.getSessionFolder();
        if (sessionFolder.isDirectory())
        {
            auto samples = folder.getChildFile ("samples");
            samples.createDirectory();
            for (auto& f : sessionFolder.findChildFiles (juce::File::findFiles, false))
            {
                auto source = BrowserPanel::resolveLinkedMediaFile (f);
                if (source.existsAsFile())
                    source.copyFileTo (samples.getChildFile (source.getFileName()));
            }
        }

        updateWindowTitle();
        session.broadcastProjectSaved (name);
        logActivity (session.getRoster().front().name, "saved the project");
    }

    toasts.post ("Project saved", ToastLayer::Kind::success);
}

void MainComponent::saveAsTemplate()
{
    auto folder = AppSettings::templatesFolder (pluginHost.getSettings());
    folder.createDirectory();

    chooser = std::make_unique<juce::FileChooser> ("Save as template", folder, "*.odaw");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode
                          | juce::FileBrowserComponent::canSelectFiles
                          | juce::FileBrowserComponent::warnAboutOverwriting,
        [this] (const juce::FileChooser& fc)
        {
            auto f = fc.getResult();
            if (f == juce::File())
                return;
            if (! f.hasFileExtension ("odaw"))
                f = f.withFileExtension ("odaw");

            engine.capturePluginStatesToTree();
            if (! f.replaceWithText (project.toXmlString()))
                Dialogs::showInfoAsync (juce::MessageBoxIconType::WarningIcon,
                                        "Save template failed", f.getFullPathName());
        });
}

void MainComponent::loadProject()
{
    confirmSaveIfDirty ([this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Open project",
            AppSettings::projectsFolder (pluginHost.getSettings()), "*.odaw;*.flp;*.zip");
        chooser->launchAsync (juce::FileBrowserComponent::openMode
                              | juce::FileBrowserComponent::canSelectFiles,
            [this] (const juce::FileChooser& fc)
            {
                auto f = fc.getResult();
                if (f.existsAsFile())
                    loadFile (f);
            });
    });
}

void MainComponent::loadFile (const juce::File& f)
{
    if (session.getRole() == Session::Role::client)
    {
        toasts.post ("Leave the session before opening another project", ToastLayer::Kind::warning, 4000);
        return;
    }

    if (f.hasFileExtension ("zip"))
    {
        juce::ZipFile zip (f);
        bool hasOdaw = false;
        for (int i = 0; i < zip.getNumEntries(); ++i)
            if (zip.getEntry (i)->filename.endsWithIgnoreCase (".odaw"))
                { hasOdaw = true; break; }

        if (hasOdaw)
        {
            auto extractDir = f.getParentDirectory().getChildFile (f.getFileNameWithoutExtension());
            extractDir.createDirectory();
            zip.uncompressTo (extractDir);

            juce::File odawFile;
            for (auto di = juce::DirectoryIterator (extractDir, true, "*.odaw"); di.next();)
                { odawFile = di.getFile(); break; }

            if (! odawFile.existsAsFile())
            {
                Dialogs::showInfoAsync (juce::MessageBoxIconType::WarningIcon,
                    "Open failed", "No .odaw file found inside " + f.getFileName());
                extractDir.deleteRecursively();
                return;
            }

            auto xml = juce::parseXML (odawFile);
            if (xml == nullptr || ! juce::ValueTree::fromXml (*xml).hasType (IDs::Project))
            {
                Dialogs::showInfoAsync (juce::MessageBoxIconType::WarningIcon,
                    "Open failed", "Invalid project inside " + f.getFileName());
                extractDir.deleteRecursively();
                return;
            }

            auto loaded = juce::ValueTree::fromXml (*xml);
            relinkBundledSamples (loaded, { extractDir.getChildFile ("samples") });

            engine.stop();
            session.replaceProject (loaded);
            history.clear();
            currentProjectFile = juce::File();
            dirty = false;
            warnedMissingChannels.clear();
            warnedMissingEffects.clear();
            lastWorkFlushMs = juce::Time::currentTimeMillis();
            if (! project.hasProperty (IDs::createdEpoch))
                project.setProperty (IDs::createdEpoch, (juce::int64) juce::Time::currentTimeMillis() / 1000, nullptr);
            if (! project.hasProperty (IDs::secondsWorked))
                project.setProperty (IDs::secondsWorked, (juce::int64) 0, nullptr);
            updateWindowTitle();
            AppSettings::addRecentProject (pluginHost.getSettings(), f);
            checkMissingFiles();
            return;
        }
    }

    if (f.hasFileExtension ("flp") || f.hasFileExtension ("zip"))
    {
        auto result = FLPImport::importFile (f, pluginHost);
        if (! result.ok())
        {
            Dialogs::showInfoAsync (juce::MessageBoxIconType::WarningIcon,
                                    "FLP import failed", result.error);
            return;
        }

        engine.stop();
        session.replaceProject (result.project);
        history.clear();
        currentProjectFile = juce::File();
        dirty = false;
        warnedMissingChannels.clear();
        warnedMissingEffects.clear();
        lastWorkFlushMs = juce::Time::currentTimeMillis();
        if (! project.hasProperty (IDs::createdEpoch))
            project.setProperty (IDs::createdEpoch, (juce::int64) juce::Time::currentTimeMillis() / 1000, nullptr);
        if (! project.hasProperty (IDs::secondsWorked))
            project.setProperty (IDs::secondsWorked, (juce::int64) 0, nullptr);
        updateWindowTitle();
        AppSettings::addRecentProject (pluginHost.getSettings(), f);

        auto msg = result.summary;
        if (! result.warnings.isEmpty())
            msg << "\n\n" << result.warnings.joinIntoString ("\n");
        Dialogs::showInfoAsync (juce::MessageBoxIconType::InfoIcon,
                                "FLP imported: " + f.getFileName(), msg);
        checkMissingFiles();
        return;
    }

    if (auto xml = juce::parseXML (f))
    {
        auto loaded = juce::ValueTree::fromXml (*xml);
        if (loaded.hasType (IDs::Project))
        {
            auto dir = f.getParentDirectory();
            relinkBundledSamples (loaded,
                { dir.getChildFile (f.getFileNameWithoutExtension() + " samples"),
                  dir.getChildFile ("samples") });

            engine.stop();
            session.replaceProject (loaded);
            history.clear();
            currentProjectFile = f;
            dirty = false;
            warnedMissingChannels.clear();
            warnedMissingEffects.clear();
            lastWorkFlushMs = juce::Time::currentTimeMillis();
            if (! project.hasProperty (IDs::createdEpoch))
                project.setProperty (IDs::createdEpoch, (juce::int64) juce::Time::currentTimeMillis() / 1000, nullptr);
            if (! project.hasProperty (IDs::secondsWorked))
                project.setProperty (IDs::secondsWorked, (juce::int64) 0, nullptr);
            updateWindowTitle();
            AppSettings::addRecentProject (pluginHost.getSettings(), f);
            checkMissingFiles();
            return;
        }
    }
    Dialogs::showInfoAsync (juce::MessageBoxIconType::WarningIcon,
                            "Open failed",
                            "Not a valid mikeDAW project: " + f.getFileName());
}

void MainComponent::openRecentProject (const juce::File& f)
{
    confirmSaveIfDirty ([this, f]
    {
        if (! f.existsAsFile())
        {
            Dialogs::showInfoAsync (juce::MessageBoxIconType::WarningIcon,
                                    "File not found", f.getFullPathName());
            return;
        }
        loadFile (f);
    });
}

void MainComponent::openProjectFileFromOS (const juce::File& f)
{
    confirmSaveIfDirty ([this, f]
    {
        if (! f.existsAsFile())
        {
            Dialogs::showInfoAsync (juce::MessageBoxIconType::WarningIcon,
                                    "File not found", f.getFullPathName());
            return;
        }
        loadFile (f);
        toasts.post ("Opened " + f.getFileName(), ToastLayer::Kind::success);
    });
}

void MainComponent::confirmSaveIfDirty (std::function<void()> proceed)
{
    if (! dirty)
    {
        if (proceed) proceed();
        return;
    }

    saveAlert = std::make_unique<juce::AlertWindow> (
        "Unsaved changes",
        "You have unsaved changes. Do you want to save this project first?",
        juce::MessageBoxIconType::QuestionIcon);
    saveAlert->addButton ("Save",    1, juce::KeyPress (juce::KeyPress::returnKey));
    saveAlert->addButton ("Discard", 2);
    saveAlert->addButton ("Cancel",  0, juce::KeyPress (juce::KeyPress::escapeKey));

    saveAlert->enterModalState (true,
        juce::ModalCallbackFunction::create ([this, proceed] (int choice)
        {
            saveAlert.reset();
            if (choice == 0)
                return;
            if (choice == 2)
            {
                if (proceed) proceed();
                return;
            }
            saveProject ([proceed] (bool ok)
            {
                if (ok && proceed) proceed();
            });
        }), false);
}

void MainComponent::prepareToQuit()
{
    pluginWindows.closeAll();
}

bool MainComponent::tryClose()
{
    if (! dirty)
        return true;

    confirmSaveIfDirty ([this]
    {
        prepareToQuit();
        juce::JUCEApplication::getInstance()->quit();
    });
    return false;
}

namespace
{
    struct RenderSettings
    {
        juce::String format = "wav";
        int bitDepth = 32;
        int qualityIndex = 6;
        int bitrateKbps = 320;
        double tailSeconds = 0.0;
    };

    class RenderDialog : public juce::Component
    {
    public:
        RenderDialog (bool songMode, std::function<void (RenderSettings)> onStart)
            : onStartCb (std::move (onStart))
        {
            title.setText ("Render Audio", juce::dontSendNotification);
            title.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
            title.setColour (juce::Label::textColourId, Colors::accent);
            addAndMakeVisible (title);

            modeLabel.setText (juce::String ("Rendering the current ")
                                   + (songMode ? "song" : "pattern"),
                               juce::dontSendNotification);
            modeLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
            modeLabel.setColour (juce::Label::textColourId, Colors::textDim);
            addAndMakeVisible (modeLabel);

            const char* fmts[] = { "WAV", "MP3", "FLAC", "OGG" };
            for (int i = 0; i < 4; ++i)
            {
                auto* b = formatBtns.add (new juce::TextButton (fmts[i]));
                b->setClickingTogglesState (true);
                b->setRadioGroupId (5001);
                b->setConnectedEdges (
                    (i > 0 ? juce::Button::ConnectedOnLeft : 0)
                  | (i < 3 ? juce::Button::ConnectedOnRight : 0));
                b->setColour (juce::TextButton::buttonColourId, Colors::raised);
                b->setColour (juce::TextButton::buttonOnColourId, Colors::accent);
                b->setColour (juce::TextButton::textColourOffId, Colors::textDim);
                b->setColour (juce::TextButton::textColourOnId, Colors::text);
                b->onClick = [this, i] { setFormat (i); };
                addAndMakeVisible (b);
            }

            optLabel.setColour (juce::Label::textColourId, Colors::text);
            addAndMakeVisible (optLabel);
            addAndMakeVisible (optBox);

            tailLabel.setText ("Tail", juce::dontSendNotification);
            tailLabel.setColour (juce::Label::textColourId, Colors::text);
            addAndMakeVisible (tailLabel);
            tailBox.addItem ("Cut at end", 1);
            tailBox.addItem ("2 s", 2);
            tailBox.addItem ("5 s", 3);
            tailBox.addItem ("10 s", 4);
            tailBox.setSelectedId (1, juce::dontSendNotification);
            addAndMakeVisible (tailBox);

            startBtn.onClick = [this]
            {
                RenderSettings s;
                s.format = fmt;
                const int sel = optBox.getSelectedId();
                if (fmt == "wav")
                    s.bitDepth = sel == 1 ? 16 : sel == 2 ? 24 : 32;
                else if (fmt == "flac")
                    s.bitDepth = sel == 1 ? 16 : 24;
                else if (fmt == "mp3")
                {
                    const int rates[] = { 128, 160, 192, 256, 320 };
                    s.bitrateKbps = rates[juce::jlimit (0, 4, sel - 1)];
                }
                else
                    s.qualityIndex = sel - 1;

                const double tails[] = { 0.0, 2.0, 5.0, 10.0 };
                s.tailSeconds = tails[juce::jlimit (0, 3, tailBox.getSelectedId() - 1)];

                if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                    dw->exitModalState (0);
                if (onStartCb)
                    onStartCb (s);
            };
            addAndMakeVisible (startBtn);

            cancelBtn.onClick = [this]
            {
                if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                    dw->exitModalState (0);
            };
            addAndMakeVisible (cancelBtn);

            formatBtns[0]->setToggleState (true, juce::dontSendNotification);
            setFormat (0);
            setSize (400, 200);
        }

        void setFormat (int i)
        {
            optBox.clear (juce::dontSendNotification);
            if (i == 0)
            {
                fmt = "wav";
                optLabel.setText ("Bit depth", juce::dontSendNotification);
                optBox.addItem ("16-bit int", 1);
                optBox.addItem ("24-bit int", 2);
                optBox.addItem ("32-bit float", 3);
                optBox.setSelectedId (3, juce::dontSendNotification);
            }
            else if (i == 1)
            {
                fmt = "mp3";
                optLabel.setText ("Bitrate", juce::dontSendNotification);
                const int rates[] = { 128, 160, 192, 256, 320 };
                for (int r = 0; r < (int) std::size (rates); ++r)
                    optBox.addItem (juce::String (rates[r]) + " kbps", r + 1);
                optBox.setSelectedId (5, juce::dontSendNotification);
            }
            else if (i == 2)
            {
                fmt = "flac";
                optLabel.setText ("Bit depth", juce::dontSendNotification);
                optBox.addItem ("16-bit int", 1);
                optBox.addItem ("24-bit int", 2);
                optBox.setSelectedId (2, juce::dontSendNotification);
            }
            else
            {
                fmt = "ogg";
                optLabel.setText ("Quality", juce::dontSendNotification);
                for (int q = 0; q <= 10; ++q)
                    optBox.addItem ("Q" + juce::String (q), q + 1);
                optBox.setSelectedId (7, juce::dontSendNotification);
            }
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (14);
            title.setBounds (r.removeFromTop (22));
            modeLabel.setBounds (r.removeFromTop (18));
            r.removeFromTop (12);

            auto fmtRow = r.removeFromTop (28);
            const int bw = fmtRow.getWidth() / 4;
            for (int i = 0; i < formatBtns.size(); ++i)
                formatBtns[i]->setBounds (fmtRow.removeFromLeft (i == 3 ? fmtRow.getWidth() : bw));
            r.removeFromTop (12);

            auto optRow = r.removeFromTop (26);
            optLabel.setBounds (optRow.removeFromLeft (80));
            optBox.setBounds (optRow.removeFromLeft (170));
            r.removeFromTop (8);

            auto tailRow = r.removeFromTop (26);
            tailLabel.setBounds (tailRow.removeFromLeft (80));
            tailBox.setBounds (tailRow.removeFromLeft (170));

            auto btns = r.removeFromBottom (28);
            startBtn.setBounds (btns.removeFromRight (90));
            btns.removeFromRight (8);
            cancelBtn.setBounds (btns.removeFromRight (90));
        }

        std::function<void (RenderSettings)> onStartCb;
        juce::String fmt { "wav" };
        juce::Label title, modeLabel, optLabel, tailLabel;
        juce::OwnedArray<juce::TextButton> formatBtns;
        juce::ComboBox optBox, tailBox;
        juce::TextButton startBtn { "Start" }, cancelBtn { "Cancel" };
    };
}

void MainComponent::renderAudioDialog()
{
    auto* content = new RenderDialog (engine.isSongMode(),
        [this] (RenderSettings s)
        {
            auto projectName = project.getProperty (IDs::name).toString();
            if (projectName.isEmpty()) projectName = "untitled";
            auto defFile = juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                               .getChildFile (juce::File::createLegalFileName (projectName)
                                              + "." + s.format);

            chooser = std::make_unique<juce::FileChooser> ("Render " + s.format.toUpperCase(),
                defFile, "*." + s.format);
            chooser->launchAsync (juce::FileBrowserComponent::saveMode
                                  | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                [this, s] (const juce::FileChooser& fc)
                {
                    auto dest = fc.getResult();
                    if (dest == juce::File())
                        return;
                    if (! dest.hasFileExtension (s.format))
                        dest = dest.withFileExtension (s.format);
                    runExportRender (dest, s.format, s.bitDepth, s.qualityIndex,
                                     s.bitrateKbps, s.tailSeconds);
                });
        });

    juce::DialogWindow::LaunchOptions opts;
    opts.content.setOwned (content);
    opts.dialogTitle = "Render Audio";
    opts.dialogBackgroundColour = Colors::panelBg;
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar = true;
    opts.resizable = false;
    opts.launchAsync();
}

void MainComponent::exportFlZip()
{
    chooser = std::make_unique<juce::FileChooser> ("Export as FL Studio ZIP",
        AppSettings::projectsFolder (pluginHost.getSettings())
            .getChildFile (juce::File::createLegalFileName (currentProjectName()) + ".zip"),
        "*.zip");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode
                          | juce::FileBrowserComponent::canSelectFiles
                          | juce::FileBrowserComponent::warnAboutOverwriting,
        [this] (const juce::FileChooser& fc)
        {
            auto dest = fc.getResult();
            if (dest == juce::File())
                return;
            if (! dest.hasFileExtension ("zip"))
                dest = dest.withFileExtension ("zip");

            engine.capturePluginStatesToTree();
            auto result = FLPExport::exportAsZip (project, dest,
                [this] (const juce::String& p) { return engine.resolveSampleFile (p); });

            if (! result.success)
            {
                Dialogs::showInfoAsync (juce::MessageBoxIconType::WarningIcon,
                    "Export failed", result.error);
                return;
            }

            Dialogs::showInfoAsync (juce::MessageBoxIconType::InfoIcon,
                "FL Studio ZIP exported", result.summary);
        });
}

void MainComponent::exportProjectZip()
{
    auto projectName = project.getProperty (IDs::name).toString();
    if (projectName.isEmpty()) projectName = "Untitled";
    auto defaultFile = AppSettings::projectsFolder (pluginHost.getSettings())
                           .getChildFile (juce::File::createLegalFileName (projectName) + ".zip");

    chooser = std::make_unique<juce::FileChooser> ("Save Zipped Project",
        defaultFile, "*.zip");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode
                          | juce::FileBrowserComponent::canSelectFiles
                          | juce::FileBrowserComponent::warnAboutOverwriting,
        [this, projectName] (const juce::FileChooser& fc)
        {
            auto dest = fc.getResult();
            if (dest == juce::File())
                return;
            if (! dest.hasFileExtension ("zip"))
                dest = dest.withFileExtension ("zip");

            engine.capturePluginStatesToTree();

            auto tempDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                               .getChildFile ("mikeDAW_mike_" + juce::String (juce::Random::getSystemRandom().nextInt64()));
            tempDir.createDirectory();

            auto odawFile = tempDir.getChildFile (juce::File::createLegalFileName (projectName) + ".odaw");
            odawFile.replaceWithText (project.toXmlString());

            auto samplesDir = tempDir.getChildFile ("samples");
            samplesDir.createDirectory();

            juce::StringArray copied;
            auto channels = Model::getChannels (project);
            for (int i = 0; i < channels.getNumChildren(); ++i)
            {
                auto ch = channels.getChild (i);
                auto path = ch.getProperty (IDs::samplePath).toString();
                if (path.isEmpty() || copied.contains (path))
                    continue;
                copied.add (path);

                if (path.startsWith ("builtin:"))
                {
                    auto leafName = path.fromFirstOccurrenceOf (":", false, false) + ".wav";
                    AudioEngine::renderBuiltinSampleToWav (
                        path.fromFirstOccurrenceOf (":", false, false),
                        samplesDir.getChildFile (leafName));
                }
                else
                {
                    juce::File src (path);
                    if (src.existsAsFile())
                        src.copyFileTo (samplesDir.getChildFile (src.getFileName()));
                }
            }

            if (dest.existsAsFile())
                dest.deleteFile();

            juce::ZipFile::Builder builder;
            for (auto di = juce::DirectoryIterator (tempDir, true); di.next();)
            {
                auto rel = di.getFile().getRelativePathFrom (tempDir);
                builder.addFile (di.getFile(), 9, rel);
            }

            auto outStream = dest.createOutputStream();
            bool wrote = outStream != nullptr && builder.writeToStream (*outStream, nullptr);
            outStream.reset();
            tempDir.deleteRecursively();

            if (! wrote)
            {
                Dialogs::showInfoAsync (juce::MessageBoxIconType::WarningIcon,
                    "Export failed", "Could not write " + dest.getFullPathName());
                return;
            }

            Dialogs::showInfoAsync (juce::MessageBoxIconType::InfoIcon,
                "Zipped Project saved",
                "Saved " + dest.getFileName() + " (" + juce::String (copied.size()) + " samples bundled).");
        });
}

void MainComponent::runExportRender (const juce::File& dest, const juce::String& format,
                                     int bitDepth, int qualityIndex, int bitrateKbps,
                                     double tailSeconds)
{
    engine.capturePluginStatesToTree();
    engine.stop();

    double sampleRate = engine.deviceManager.getCurrentAudioDevice() != nullptr
                          ? engine.deviceManager.getCurrentAudioDevice()->getCurrentSampleRate()
                          : 44100.0;
    if (sampleRate <= 0)
        sampleRate = 44100.0;

    dest.deleteFile();

    std::unique_ptr<juce::AudioFormatWriter> writer;

    if (format == "mp3")
    {
#if JUCE_WINDOWS
        writer = std::make_unique<Mp3Writer> (dest, sampleRate, 2, bitrateKbps);
#else
        auto out = std::make_unique<juce::FileOutputStream> (dest);
        juce::File lame;
       #if defined (OPENDAW_LAME_EXECUTABLE)
        lame = juce::File (OPENDAW_LAME_EXECUTABLE);
       #else
        lame = juce::File ("/usr/bin/lame");
       #endif

        if (! out->openedOk() || ! lame.existsAsFile())
        {
            Dialogs::showInfoAsync (juce::MessageBoxIconType::WarningIcon,
                "Export failed", "Linux MP3 export requires the lame executable.");
            return;
        }

        auto af = std::make_unique<juce::LAMEEncoderAudioFormat> (lame);
        auto options = af->getQualityOptions();
        int quality = juce::jmax (0, options.size() - 1);
        auto requested = juce::String (bitrateKbps) + " Kb/s CBR";
        for (int i = 0; i < options.size(); ++i)
            if (options[i] == requested)
                quality = i;

        auto* rawStream = out.get();
        writer.reset (af->createWriterFor (rawStream, sampleRate, 2, 16, {}, quality));
        if (writer == nullptr)
        {
            Dialogs::showInfoAsync (juce::MessageBoxIconType::WarningIcon,
                "Export failed", "Could not create the Linux MP3 encoder.");
            return;
        }
        out.release();
#endif
    }
    else
    {
        auto out = std::make_unique<juce::FileOutputStream> (dest);
        if (! out->openedOk())
        {
            Dialogs::showInfoAsync (juce::MessageBoxIconType::WarningIcon,
                "Export failed", "Could not open " + dest.getFullPathName() + " for writing.");
            return;
        }

        std::unique_ptr<juce::AudioFormat> af;
        juce::StringPairArray metadata;
        int writerBits = bitDepth;
        int writerQuality = 0;

        if (format == "wav")
            af = std::make_unique<juce::WavAudioFormat>();
        else if (format == "flac")
            af = std::make_unique<juce::FlacAudioFormat>();
        else if (format == "ogg")
        {
            af = std::make_unique<juce::OggVorbisAudioFormat>();
            writerBits = 0;
            writerQuality = qualityIndex;
        }

        if (af == nullptr)
            return;

        auto* rawStream = out.get();
        writer.reset (af->createWriterFor (rawStream, sampleRate, 2,
                          writerBits > 0 ? writerBits : af->getPossibleBitDepths().getLast(),
                          metadata, writerQuality));
        if (writer == nullptr)
        {
            Dialogs::showInfoAsync (juce::MessageBoxIconType::WarningIcon,
                "Export failed", "Could not create a " + format.toUpperCase() + " encoder.");
            return;
        }
        out.release();
    }

    class RenderTask : public juce::ThreadWithProgressWindow
    {
    public:
        RenderTask (AudioEngine& e, std::unique_ptr<juce::AudioFormatWriter> w, juce::File d,
                    double tail)
            : ThreadWithProgressWindow ("Rendering audio...", true, true),
              engine (e), writer (std::move (w)), dest (std::move (d)), tailSeconds (tail) {}

        void run() override
        {
            ok = engine.renderToFile (*writer, tailSeconds, [this] (float p)
            {
                setProgress ((double) p);
                if (threadShouldExit())
                    engine.stop();
            });
            writer.reset();
            if (threadShouldExit())
                dest.deleteFile();
        }

        void threadComplete (bool userCancelled) override
        {
            auto d = dest;
            bool good = ok && ! userCancelled;
            if (onFinished != nullptr)
                onFinished();
            if (good && onRendered != nullptr)
                onRendered (d);
            bool cancelled = userCancelled;
            juce::MessageManager::callAsync ([d, good, cancelled]
            {
                if (! good)
                {
                    if (! cancelled)
                        Dialogs::showInfoAsync (juce::MessageBoxIconType::WarningIcon,
                            "Export failed",
                            "Could not render to " + d.getFullPathName() + ".");
                    return;
                }

                auto* aw = new juce::AlertWindow ("Export complete",
                    "Rendered to " + d.getFullPathName(),
                    juce::MessageBoxIconType::InfoIcon);
                aw->addButton ("OK", 0, juce::KeyPress (juce::KeyPress::returnKey));
                aw->addButton ("Show in folder", 1);
                aw->enterModalState (true, juce::ModalCallbackFunction::create (
                    [aw, d] (int result)
                    {
                        if (result == 1)
                            d.revealToUser();
                        delete aw;
                    }), false);
            });
            delete this;
        }

        std::function<void (juce::File)> onRendered;
        std::function<void()> onFinished;

        AudioEngine& engine;
        std::unique_ptr<juce::AudioFormatWriter> writer;
        juce::File dest;
        double tailSeconds = 2.0;
        bool ok = false;
    };

    auto* task = new RenderTask (engine, std::move (writer), dest, tailSeconds);
    task->onRendered = [safe = juce::Component::SafePointer<MainComponent> (this)] (juce::File d)
    {
        if (safe != nullptr && safe->session.isActive())
            safe->session.shareRenderedFile (d);
    };
    task->onFinished = [safe = juce::Component::SafePointer<MainComponent> (this)]
    {
        if (safe != nullptr)
        {
            safe->session.broadcastRenderLock (false);
            safe->hideRenderLock();
        }
    };

    session.broadcastRenderLock (true);
    showRenderLock (session.getRoster().front().name);
    task->launchThread();
}

void MainComponent::showPluginManager()
{
    if (pluginManager == nullptr)
        pluginManager = std::make_unique<PluginManagerWindow> (pluginHost);

    if (pluginManager->content != nullptr)
        pluginManager->content->refresh();
    pluginManager->setVisible (true);
    pluginManager->toFront (true);
}

namespace
{
    juce::String localLanAddress()
    {
        for (auto& ip : juce::IPAddress::getAllAddresses (false))
            if (! ip.isNull() && ip.toString() != "127.0.0.1")
                return ip.toString();
        return "127.0.0.1";
    }
}

void MainComponent::socialMenu (juce::Component* target)
{
    juce::PopupMenu m;
    bool active = session.isActive();
    m.addItem (5, "Friends", true);
    m.addSeparator();
    m.addItem (1, "Host session", ! active);
    m.addItem (2, "Join session...", ! active);
    if (session.getRole() == Session::Role::host)
        m.addItem (4, lastInviteCode.isNotEmpty() ? "Copy invite code" : "Preparing invite code...", lastInviteCode.isNotEmpty());
    m.addSeparator();
    m.addItem (3, session.getRole() == Session::Role::host ? "End session" : "Leave session", active);

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target),
        [this] (int result)
        {
            if (result == 1)      hostSession();
            else if (result == 2) joinSessionDialog();
            else if (result == 3) leaveSession();
            else if (result == 4) copyInviteCode();
            else if (result == 5) togglePanel ("friends");
        });
}

juce::String MainComponent::localDisplayName()
{
    return AppSettings::displayName (pluginHost.getSettings());
}

int MainComponent::presencePeopleCount()
{
    return session.getNumPeers() + 1;
}

juce::String MainComponent::presenceStatusString()
{
    if (localAfk)
        return "Away";

    auto people = presencePeopleCount();
    auto groupSize = juce::String (people) + (people == 1 ? " person" : " people");
    if (session.getRole() == Session::Role::host)
        return "Hosting a session" + juce::String (juce::CharPointer_UTF8 ("  \xC2\xB7  ")) + groupSize;
    if (session.isActive())
        return "In a session" + juce::String (juce::CharPointer_UTF8 ("  \xC2\xB7  ")) + groupSize;
    if (localViewport.isNotEmpty())
        return "In the " + localViewport;
    return "In the studio";
}

int MainComponent::presenceSessionPort()
{
    return session.getRole() == Session::Role::host ? hostedPort : 0;
}

void MainComponent::inviteFriend (juce::int64 friendId)
{
    if (session.getRole() != Session::Role::host)
    {
        toasts.post ("Only the host can invite friends", ToastLayer::Kind::warning);
        return;
    }
    auto address = friends.lanAddressFor (friendId);
    bool relayReachable = relay.isConnected() && lastInviteCode.isNotEmpty();
    if (address.isEmpty() && ! relayReachable)
    {
        toasts.post ("That friend isn't reachable right now", ToastLayer::Kind::warning);
        return;
    }
    if (address.isNotEmpty())
        lanDiscovery.sendInvite (address, localUserId, localDisplayName(),
                                 hostedSessionName, hostedPort);
    if (lastInviteCode.isNotEmpty())
        relay.sendInvite (friendId, hostedSessionName, lastInviteCode, hostedPort);
    toasts.post ("Invite sent", ToastLayer::Kind::info);
}

void MainComponent::handleFriendInvite (juce::int64 fromId, const juce::String& fromName, const juce::String& sessionName,
                                        const juce::String& joinText, int port, bool localOnly)
{
    auto who = fromName.isNotEmpty() ? fromName : juce::String ("Someone");
    if (session.isActive())
    {
        toasts.post (who + " invited you (you're already in a session)", ToastLayer::Kind::info);
        return;
    }

    juce::ignoreUnused (localOnly);
    auto safeThis = juce::Component::SafePointer<MainComponent> (this);
    toasts.post (who + " invited you to \"" + (sessionName.isNotEmpty() ? sessionName : juce::String ("their session")) + "\"",
                 ToastLayer::Kind::info, 12000, "Join",
                 [safeThis, joinText, port, fromId]
                 {
                     if (safeThis != nullptr)
                         safeThis->joinSessionText (joinText, port, fromId);
                 },
                 "Ignore", {}, Colors::success, Colors::danger);
}

void MainComponent::handleIncomingFriendRequest (juce::int64 fromId, const juce::String& name)
{
    if (fromId == 0 || fromId == localUserId)
        return;
    if (friends.isFriend (fromId))
    {
        sendFriendResponseTo (fromId, true);
        return;
    }
    auto who = name.isNotEmpty() ? name : juce::String ("Someone");
    friends.addIncomingRequest (fromId, name);
    toasts.post (who + " sent you a friend request", ToastLayer::Kind::info);
    relay.notifyStateChanged();
}

void MainComponent::handleIncomingFriendResponse (juce::int64 fromId, const juce::String& name, bool accepted)
{
    if (! friends.hasOutgoingRequest (fromId))
        return;
    auto who = name.isNotEmpty() ? name : juce::String ("Someone");
    if (accepted)
    {
        friends.addFriend (fromId, name);
        toasts.post (who + " accepted your friend request!", ToastLayer::Kind::success);
    }
    else
    {
        friends.clearOutgoingRequest (fromId);
        toasts.post (who + " declined your friend request", ToastLayer::Kind::info);
    }
}

void MainComponent::sendFriendRequestTo (juce::int64 id, const juce::String& name)
{
    if (friends.hasIncomingRequest (id))
    {
        respondToFriendRequest (id, name, true);
        return;
    }
    friends.addOutgoingRequest (id, name);
    auto addr = friends.lanAddressFor (id);
    if (addr.isNotEmpty())
        lanDiscovery.sendFriendRequest (addr, localUserId, localDisplayName());
    relay.sendFriendRequest (id);
    toasts.post ("Friend request sent to " + name, ToastLayer::Kind::info);
    relay.notifyStateChanged();
}

void MainComponent::sendFriendResponseTo (juce::int64 id, bool accept)
{
    auto addr = friends.lanAddressFor (id);
    if (addr.isNotEmpty())
        lanDiscovery.sendFriendResponse (addr, localUserId, localDisplayName(), accept);
    relay.respondFriendRequest (id, accept);
}

void MainComponent::respondToFriendRequest (juce::int64 id, const juce::String& name, bool accept)
{
    if (accept)
    {
        friends.addFriend (id, name);
        sendFriendResponseTo (id, true);
        toasts.post ("You and " + name + " are now friends!", ToastLayer::Kind::success);
    }
    else
    {
        friends.removeIncomingRequest (id);
        sendFriendResponseTo (id, false);
    }
    relay.notifyStateChanged();
}

void MainComponent::showPeerFriendMenu (juce::int64 peerId, const juce::String& name)
{
    if (peerId == 0)
        return;

    if (peerId == localUserId)
    {
        if (! session.isActive())
            return;
        const bool isHost = session.getRole() == Session::Role::host;
        juce::PopupMenu m;
        m.addItem (1, isHost ? "End session for everyone" : "Leave session");
        m.showMenuAsync (juce::PopupMenu::Options(),
            [this] (int r) { if (r == 1) leaveSession(); });
        return;
    }

    bool canKick = false;
    if (session.getRole() == Session::Role::host)
        for (auto& u : session.getRoster())
            if (u.peerId == peerId) { canKick = true; break; }

    juce::PopupMenu m;
    bool already = friends.isFriend (peerId);
    m.addItem (1, already ? "Remove " + name + " from friends"
                          : "Send " + name + " a friend request");
    if (canKick)
        m.addItem (2, "Kick " + name + " from session");
    m.showMenuAsync (juce::PopupMenu::Options(),
        [this, peerId, name, already] (int r)
        {
            if (r == 1)
            {
                if (already)
                {
                    friends.removeFriend (peerId);
                    toasts.post ("Removed " + name + " from friends", ToastLayer::Kind::info);
                    relay.notifyStateChanged();
                }
                else
                {
                    sendFriendRequestTo (peerId, name);
                }
            }
            else if (r == 2)
            {
                session.kickPeer (peerId);
                toasts.post (name + " was kicked", ToastLayer::Kind::info);
            }
        });
}

void MainComponent::leaveSession()
{
    bool wasHost = session.getRole() == Session::Role::host;
    int mappedPort = lastUpnpMappedPort;
    lastUpnpMappedPort = 0;
    hostedPort = 0;
    hostJoinToken.clear();
    lastInviteCode.clear();
    lastInviteStatus.clear();
    if (wasHost)
        session.endSession();
    else
        session.disconnect();
    lanDiscovery.stopAdvertising();
    if (wasHost && mappedPort > 0)
    {
        juce::Thread::launch ([mappedPort]
        {
            juce::String ignored;
            InviteCode::removeUPnPPortMapping (mappedPort, ignored);
        });
    }
    resetSessionUiState();
    relay.notifyStateChanged();
    toasts.post (wasHost ? "You ended the session - everyone keeps their copy"
                         : "You left the session", ToastLayer::Kind::info);
}

void MainComponent::resetSessionUiState()
{
    relayJoinPending = false;
    relayJoinAwaitingSnapshot = false;
    relayJoinFallbackText.clear();
    relayJoinFallbackPort = 0;
    relayJoinTunnel.reset();
    peerCursors.clear();
    reactions.clear();
    if (chat != nullptr) chat->clearChat();
    peerViewports.clear();
    afkPeersSeen.clear();
    localViewport.clear();
    hideRenderLock();
    lastCompatReport = {};
    refreshPeerBadges();
    warnedMissingChannels.clear();
    warnedMissingEffects.clear();
}

void MainComponent::hostSession()
{
    if (session.isActive())
        return;

    const auto port = Session::defaultPort;
    const auto name = juce::SystemStats::getComputerName() + "'s session";
    if (! session.startHost (port))
    {
        toasts.post ("Couldn't start a session on port " + juce::String (port),
                     ToastLayer::Kind::warning);
        return;
    }

    if (chat != nullptr) { chat->clearChat(); chat->addSystemMessage ("You started hosting this session"); }
    afkPeersSeen.clear();
    hostedPort = port;
    hostedSessionName = name;
    hostJoinToken = juce::String::toHexString (juce::Random::getSystemRandom().nextInt64())
                        .paddedLeft ('0', 16);
    lastInviteCode.clear();
    lastInviteStatus = "Preparing invite code...";
    lanDiscovery.startAdvertising (name, port);
    relay.notifyStateChanged();
    toasts.post ("Hosting session - preparing invite code", ToastLayer::Kind::info);

    if (friends.numOnline() > 0)
    {
        if (friendsPanel != nullptr && ! friendsPanel->isVisible())
            togglePanel ("friends");
        toasts.post ("Invite your online friends from the Friends panel", ToastLayer::Kind::info);
    }

    auto safeThis = juce::Component::SafePointer<MainComponent> (this);
    auto lan = localLanAddress();
    juce::Thread::launch ([safeThis, port, lan]
    {
        InviteHostResult result { InviteCode::prepareHostInvite (port, lan) };
        juce::MessageManager::callAsync ([safeThis, result]
        {
            if (safeThis != nullptr)
                safeThis->showHostInviteReady (result);
        });
    });
}

void MainComponent::joinSessionDialog()
{
    if (joinWindow != nullptr)
    {
        joinWindow->toFront (true);
        return;
    }

    joinWindow = std::make_unique<JoinSessionWindow> (
        lanDiscovery,
        [this] (juce::String addr, int port)
        {
            joinSessionText (addr, port);
        },
        [this] { joinWindow.reset(); });
    joinWindow->setVisible (true);
    joinWindow->toFront (true);
}

void MainComponent::joinSessionText (const juce::String& text, int defaultPort, juce::int64 relayFriendId)
{
    if (session.isActive())
    {
        toasts.post ("Already in a session - disconnect first", ToastLayer::Kind::warning);
        return;
    }
    if (joinInProgress || relayJoinPending)
    {
        toasts.post ("Still connecting to a session...", ToastLayer::Kind::info);
        return;
    }

    auto endpoints = InviteCode::parseJoinText (text, defaultPort);
    if (endpoints.empty())
    {
        toasts.post ("That doesn't look like a valid invite code or address", ToastLayer::Kind::warning);
        return;
    }

    joinInProgress = true;

    juce::String tried;
    for (size_t i = 0; i < endpoints.size(); ++i)
        tried << (i ? " or " : "") << endpoints[i].address << ":" << juce::String (endpoints[i].port);

    toasts.post ("Connecting to session...", ToastLayer::Kind::info);

    auto safeThis = juce::Component::SafePointer<MainComponent> (this);
    juce::Thread::launch ([safeThis, endpoints, tried, relayFriendId]
    {
        struct Result { InviteCode::Endpoint ep; std::atomic<bool> reachable { false }; };
        std::vector<std::unique_ptr<Result>> results;
        for (auto& ep : endpoints)
        {
            auto r = std::make_unique<Result>();
            r->ep = ep;
            results.push_back (std::move (r));
        }

        struct Probe : juce::Thread
        {
            Probe (Result* res) : juce::Thread ("odaw-probe"), result (res) {}
            void run() override
            {
                result->reachable = InviteCode::probeEndpoint (result->ep.address, result->ep.port, 4000);
            }
            Result* result;
        };

        juce::OwnedArray<juce::Thread> probes;
        for (auto& r : results)
            probes.add (new Probe (r.get()))->startThread();
        for (auto* p : probes)
            p->waitForThreadToExit (6000);

        InviteCode::Endpoint chosen;
        bool found = false;
        for (auto& r : results)
            if (r->reachable.load())
            {
                chosen = r->ep;
                found = true;
                break;
            }

        juce::MessageManager::callAsync ([safeThis, chosen, found, tried, relayFriendId]
        {
            if (safeThis == nullptr)
                return;

            safeThis->joinInProgress = false;

            if (found && safeThis->session.joinHost (chosen.address, chosen.port))
            {
                if (safeThis->chat != nullptr) { safeThis->chat->clearChat(); safeThis->chat->addSystemMessage ("You joined the session"); }
                safeThis->afkPeersSeen.clear();
                return;
            }

            if (relayFriendId != 0 && safeThis->relay.isConnected() && ! safeThis->session.isActive())
            {
                safeThis->beginRelayJoin (relayFriendId, {}, 0);
                return;
            }

            safeThis->toasts.post ("Couldn't reach the host at " + tried
                                       + " - see multiplayer help",
                                   ToastLayer::Kind::warning, 6000);
        });
    });
}

void MainComponent::beginRelayJoin (juce::int64 relayFriendId, const juce::String& fallbackText, int fallbackPort,
                                    const juce::String& joinToken)
{
    relayJoinPending = true;
    relayJoinFriendId = relayFriendId;
    relayJoinFallbackText = fallbackText;
    relayJoinFallbackPort = fallbackPort;
    int attempt = ++relayJoinAttempt;
    toasts.post ("Connecting via relay...", ToastLayer::Kind::info);
    relay.sendJoinRequest (relayFriendId, joinToken);

    auto safeThis = juce::Component::SafePointer<MainComponent> (this);
    juce::Timer::callAfterDelay (12000, [safeThis, attempt]
    {
        if (safeThis == nullptr)
            return;
        if (safeThis->relayJoinPending && safeThis->relayJoinAttempt == attempt)
        {
            safeThis->relayJoinPending = false;
            safeThis->relayJoinAwaitingSnapshot = false;
            safeThis->runRelayJoinFallback();
        }
    });
}

void MainComponent::runRelayJoinFallback()
{
    auto text = relayJoinFallbackText;
    auto port = relayJoinFallbackPort;
    relayJoinFallbackText.clear();
    relayJoinFallbackPort = 0;
    if (text.isNotEmpty() && ! session.isActive())
    {
        toasts.post ("Relay unavailable - trying a direct connection...", ToastLayer::Kind::info);
        joinSessionText (text, port);
        return;
    }
    toasts.post ("Couldn't reach the host via relay or directly - see multiplayer help",
                 ToastLayer::Kind::warning, 6000);
}

void MainComponent::showHostInviteReady (const InviteHostResult& result)
{
    if (session.getRole() != Session::Role::host || hostedPort != result.invite.port)
    {
        if (result.invite.upnpMapped)
        {
            juce::Thread::launch ([port = result.invite.port]
            {
                juce::String ignored;
                InviteCode::removeUPnPPortMapping (port, ignored);
            });
        }
        return;
    }

    lastInviteCode = result.invite.code;
    lastInviteStatus = result.invite.status;
    lastUpnpMappedPort = result.invite.upnpMapped ? result.invite.port : 0;
    relay.notifyStateChanged();
    juce::SystemClipboard::copyTextToClipboard (lastInviteCode);

    toasts.post ("Invite code copied to clipboard", ToastLayer::Kind::success);
}

void MainComponent::copyInviteCode()
{
    if (lastInviteCode.isEmpty())
        return;

    juce::SystemClipboard::copyTextToClipboard (lastInviteCode);
    toasts.post ("Invite code copied to clipboard", ToastLayer::Kind::success);
}

void MainComponent::showPreferences()
{
    if (preferencesWindow == nullptr)
    {
        preferencesWindow = std::make_unique<PreferencesWindow> (
            engine.deviceManager, pluginHost.getSettings(), themeManager,
            [this]
            {
                if (! AppSettings::followDefaultOutput (pluginHost.getSettings()))
                    if (auto stateXml = engine.deviceManager.createStateXml())
                        pluginHost.getSettings().setValue ("audioDeviceState", stateXml.get());
                pluginHost.getSettings().saveIfNeeded();
            });
        preferencesWindow->content->onFollowDefaultOutputChanged = [this] (bool on)
        {
            if (on)
                selectDefaultOutputDevice();
        };
        preferencesWindow->content->onAutoSaveChanged = [this] { applyAutoSaveSettings(); };
        preferencesWindow->content->onDisplayNameChanged = [this]
        { session.setDisplayName (AppSettings::displayName (pluginHost.getSettings())); };
        preferencesWindow->content->onAvatarChanged = [this] { pushLocalAvatar(); };
        preferencesWindow->content->onSmartDisableChanged = [this] (bool on)
        { engine.setSmartDisableEnabled (on); };
        preferencesWindow->content->onMulticoreChanged = [this] (bool on)
        { engine.setMulticoreRenderingEnabled (on); };
        preferencesWindow->content->onRelayChanged = [this]
        {
            auto& s = pluginHost.getSettings();
            relay.configure (AppSettings::relayHost (s), AppSettings::relayPort (s), AppSettings::relayEnabled (s));
        };
        preferencesWindow->content->getActiveSessionFolder = [this]
        { return session.isActive() ? session.getSessionFolder() : juce::File(); };
    }
   #if ! OPENDAW_PUBLIC_BUILD
    preferencesWindow->content->setLicenseInfo (license.statusText(), license.hwid());
   #endif
    preferencesWindow->setVisible (true);
    preferencesWindow->toFront (true);
}

juce::String MainComponent::defaultOutputDeviceName()
{
    for (auto* type : engine.deviceManager.getAvailableDeviceTypes())
    {
        if (type->getTypeName() == "Windows Audio")
        {
            type->scanForDevices();
            auto names = type->getDeviceNames (false);
            const int idx = type->getDefaultDeviceIndex (false);
            return juce::isPositiveAndBelow (idx, names.size()) ? names[idx] : juce::String();
        }
    }
    return {};
}

void MainComponent::selectDefaultOutputDevice()
{
    auto defName = defaultOutputDeviceName();
    if (defName.isEmpty())
        return;

    if (engine.deviceManager.getCurrentAudioDeviceType() != "Windows Audio")
        engine.deviceManager.setCurrentAudioDeviceType ("Windows Audio", true);

    auto setup = engine.deviceManager.getAudioDeviceSetup();
    if (setup.outputDeviceName != defName)
    {
        setup.outputDeviceName = defName;
        setup.useDefaultOutputChannels = true;
        engine.deviceManager.setAudioDeviceSetup (setup, true);
    }
    followedOutputName = defName;
}

void MainComponent::applyFollowDefaultOutput()
{
    auto& s = pluginHost.getSettings();
    if (! AppSettings::followDefaultOutput (s))
        return;

    const auto defName = defaultOutputDeviceName();
    if (defName.isEmpty())
        return;

    const bool isWasapi = engine.deviceManager.getCurrentAudioDeviceType() == "Windows Audio";
    const auto curOut   = engine.deviceManager.getAudioDeviceSetup().outputDeviceName;

    if (isWasapi && curOut == defName)
    {
        followedOutputName = defName;
        return;
    }

    if (isWasapi && curOut == followedOutputName)
    {
        selectDefaultOutputDevice();
        return;
    }

    AppSettings::setFollowDefaultOutput (s, false);
    s.saveIfNeeded();
    if (preferencesWindow != nullptr && preferencesWindow->content != nullptr)
        preferencesWindow->content->refreshFollowDefaultOutput();
}

void MainComponent::showWelcomeDialog()
{
    auto& s = pluginHost.getSettings();
    AppSettings::setWelcomeShown (s, true);
    s.saveIfNeeded();

    auto* w = new juce::AlertWindow ("Welcome to mDAW!",
        "For your first time, I highly recommend opening up the Preferences tab to "
        "configure everything for your DAW experience, including your Multiplayer "
        "profile data.",
        juce::MessageBoxIconType::InfoIcon);
    w->addButton ("Open Preferences", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Later", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    w->enterModalState (true, juce::ModalCallbackFunction::create (
        [safe = juce::Component::SafePointer<MainComponent> (this)] (int result)
        {
            if (result == 1 && safe != nullptr)
                safe->showPreferences();
        }), true);
}

void MainComponent::pushLocalAvatar()
{
    auto& s = pluginHost.getSettings();
    juce::MemoryBlock bytes;
    if (AppSettings::hasAvatar (s))
        AppSettings::avatarFile (s).loadFileAsData (bytes);
    session.setLocalAvatar (bytes);

    localAvatarImage = bytes.getSize() > 0 ? juce::ImageFileFormat::loadFrom (bytes.getData(), bytes.getSize())
                                           : juce::Image();
    if (friendsUi != nullptr)
        friendsUi->repaint();
}

void MainComponent::applyAutoSaveSettings()
{
    lastAutoSaveMs = juce::Time::currentTimeMillis();
    startTimer (workClockIntervalMs);
}

void MainComponent::flushWorkTime()
{
    auto now = juce::Time::currentTimeMillis();
    if (lastWorkFlushMs == 0)
    {
        lastWorkFlushMs = now;
        return;
    }
    auto elapsedSec = (now - lastWorkFlushMs) / 1000;
    lastWorkFlushMs = now;
    if (elapsedSec <= 0)
        return;

    auto total = (juce::int64) project.getProperty (IDs::secondsWorked, (juce::int64) 0) + elapsedSec;
    bool wasDirty = dirty;
    project.setProperty (IDs::secondsWorked, total, nullptr);
    dirty = wasDirty;
}

void MainComponent::updateWindowTitle()
{
    juce::String base = "mikeDAW v" + juce::String (JUCE_APPLICATION_VERSION_STRING) + " - ";
    if (currentProjectFile.existsAsFile())
        base << currentProjectFile.getFileName();
    else
    {
        auto name = project.getProperty (IDs::name).toString();
        base << ((name.isEmpty() || name == "Untitled") ? juce::String ("Untitled.odaw")
                                                         : name + ".odaw");
    }

    auto secs = (juce::int64) project.getProperty (IDs::secondsWorked, (juce::int64) 0);
    base << "   [" << juce::String (secs / 60) << " min wasted]";

    const bool inSession = session.isActive();
    const int peers = session.getNumPeers();
    if (inSession)
        base << "  [" << peers << (peers == 1 ? " peer]" : " peers]");

    if (auto* top = getTopLevelComponent())
        top->setName (base);

    juce::String details = currentProjectName();

    auto bpmText = juce::String (juce::roundToInt ((double) project.getProperty (IDs::bpm, 140.0)))
                       + " BPM";
    juce::String state;
    if (engine.isRecording())
        state = "Recording a take at " + bpmText;
    else if (engine.isPlaying())
        state = engine.isSongMode() ? "Arranging the song at " + bpmText
                                    : "Cooking a pattern at " + bpmText;
    else
        state = "Producing at " + bpmText;

    const bool hosting = session.getRole() == Session::Role::host;
    const int people = presencePeopleCount();
    if (inSession)
        state << "  \xe2\x80\xa2  " << (hosting ? "Hosting " : "Jamming ")
              << people << "/" << Session::maxPeople;

    if (details != lastPresenceDetails)
    {
        presenceStartEpoch = juce::Time::currentTimeMillis() / 1000;
        lastPresenceDetails = details;
    }

    DiscordPresence::Activity activity;
    activity.details = details;
    activity.state = state;
    activity.startEpochSeconds = presenceStartEpoch;
    if (inSession && hosting)
    {
        activity.partyId = "odaw-" + juce::String (localUserId);
        activity.partySize = people;
        activity.partyMax = Session::maxPeople;
        if (people < Session::maxPeople && hostJoinToken.isNotEmpty()
            && AppSettings::relayEnabled (pluginHost.getSettings()))
            activity.joinSecret = "odaw1:" + juce::String (localUserId) + ":" + hostJoinToken;
    }
    discord.setActivity (activity);
}

void MainComponent::timerCallback()
{
    flushWorkTime();
    updateWindowTitle();

    auto& s = pluginHost.getSettings();
    if (! AppSettings::autoSaveEnabled (s))
        return;

    auto now = juce::Time::currentTimeMillis();
    if (now - lastAutoSaveMs < (juce::int64) AppSettings::autoSaveMinutes (s) * 60 * 1000)
        return;
    lastAutoSaveMs = now;

    if (! dirty)
        return;

    auto dir = AppSettings::autoSaveDir (s);
    dir.createDirectory();

    auto rawName = project.getProperty (IDs::name).toString().trim();
    if (rawName.isEmpty() || rawName == "Untitled")
        rawName = "Untitled";
    auto safeName = juce::File::createLegalFileName (rawName);
    if (safeName.isEmpty())
        safeName = "Untitled";

    auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d_%H-%M-%S");
    auto dest = dir.getChildFile (safeName + "-" + stamp + ".odaw");

    engine.capturePluginStatesToTree();
    diskWrites.enqueue (dest, project.toXmlString(),
        [safe = juce::Component::SafePointer<MainComponent> (this), dir] (bool ok)
        {
            if (safe == nullptr || ! ok)
                return;
            safe->pruneAutoSaves (dir);
            safe->toasts.post ("Auto-saved", ToastLayer::Kind::info, 2500);
        });
}

void MainComponent::pruneAutoSaves (const juce::File& dir)
{
    constexpr int keep = 50;
    auto files = dir.findChildFiles (juce::File::findFiles, false, "*.odaw");
    if (files.size() <= keep)
        return;

    std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b)
        { return a.getLastModificationTime() < b.getLastModificationTime(); });
    for (int i = 0; i < files.size() - keep; ++i)
        files.getReference (i).deleteFile();
}

void MainComponent::revertToAutoSave()
{
    if (session.getRole() == Session::Role::client)
    {
        toasts.post ("Leave the session before replacing the project", ToastLayer::Kind::warning, 4000);
        return;
    }
    auto dir = AppSettings::autoSaveDir (pluginHost.getSettings());
    auto snapshots = dir.findChildFiles (juce::File::findFiles, false, "*.odaw");
    juce::File src;
    for (auto& f : snapshots)
        if (src == juce::File() || f.getLastModificationTime() > src.getLastModificationTime())
            src = f;

    if (src == juce::File())
    {
        auto legacy = AppSettings::autoSaveFile (pluginHost.getSettings());
        if (legacy.existsAsFile())
            src = legacy;
    }

    if (src == juce::File() || ! src.existsAsFile())
    {
        Dialogs::showInfoAsync (juce::MessageBoxIconType::InfoIcon,
            "Revert to auto-save", "No auto-save file was found yet.");
        return;
    }

    confirmSaveIfDirty ([this, src]
    {
        if (auto xml = juce::parseXML (src))
        {
            auto loaded = juce::ValueTree::fromXml (*xml);
            if (loaded.hasType (IDs::Project))
            {
                engine.stop();
                session.replaceProject (loaded);
                history.clear();
                dirty = false;
                lastWorkFlushMs = juce::Time::currentTimeMillis();
                if (! project.hasProperty (IDs::createdEpoch))
                    project.setProperty (IDs::createdEpoch, (juce::int64) juce::Time::currentTimeMillis() / 1000, nullptr);
                if (! project.hasProperty (IDs::secondsWorked))
                    project.setProperty (IDs::secondsWorked, (juce::int64) 0, nullptr);
                updateWindowTitle();
                return;
            }
        }
        Dialogs::showInfoAsync (juce::MessageBoxIconType::WarningIcon,
            "Revert failed", "The auto-save file could not be read.");
    });
}

void MainComponent::showEditMenu (juce::Component* target)
{
    juce::PopupMenu m;
    auto desc = [this] (KeyBindings::Action a)
    {
        auto k = keyBindings.keyFor (a);
        return k.isValid() ? "   (" + k.getTextDescription() + ")" : juce::String();
    };
    m.addItem (1, "Undo" + desc (KeyBindings::Action::undo), history.canUndo());
    m.addItem (2, "Redo" + desc (KeyBindings::Action::redo), history.canRedo());
    m.addSeparator();
    m.addItem (3, "Duplicate" + desc (KeyBindings::Action::duplicate));
    m.addItem (5, "Legato" + desc (KeyBindings::Action::legato));
    m.addItem (8, "Arpeggiate / repeat notes" + desc (KeyBindings::Action::arpeggiate));
    m.addItem (9, "Strum notes (strumizer)" + desc (KeyBindings::Action::strumize));
    m.addItem (6, juce::String (juce::CharPointer_UTF8 ("\xE2\x9A\xA0 "))
                  + "Randomize notes weirdly" + desc (KeyBindings::Action::randomizeNotes));
    m.addSeparator();
    m.addItem (4, "Configure Keyboard Shortcuts...");
    m.addItem (7, "Preferences...");
    m.addSeparator();
    juce::PopupMenu macros;
    macros.addItem (100, "Delete Unused Audio Files");
    m.addSubMenu ("Macros", macros);

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target),
        [this] (int r)
        {
            if (r == 1)      handleAction (KeyBindings::Action::undo);
            else if (r == 2) handleAction (KeyBindings::Action::redo);
            else if (r == 3) handleAction (KeyBindings::Action::duplicate);
            else if (r == 5) handleAction (KeyBindings::Action::legato);
            else if (r == 8) handleAction (KeyBindings::Action::arpeggiate);
            else if (r == 9) handleAction (KeyBindings::Action::strumize);
            else if (r == 6) handleAction (KeyBindings::Action::randomizeNotes);
            else if (r == 4) showKeyBindingsWindow();
            else if (r == 7) showPreferences();
            else if (r == 100) deleteUnusedAudioFiles();
        });
}

void MainComponent::deleteUnusedAudioFiles()
{
    std::set<juce::String> usedPaths;

    auto playlist = Model::getPlaylist (project);
    for (int i = 0; i < playlist.getNumChildren(); ++i)
    {
        auto clip = playlist.getChild (i);
        if (clip.hasType (IDs::Clip) && clip.getProperty (IDs::type).toString() == "audio")
        {
            auto p = clip.getProperty (IDs::path).toString();
            if (p.isNotEmpty())
                usedPaths.insert (p);
        }
    }

    auto patterns = Model::getPatterns (project);
    auto channelHasContent = [&] (int channelId)
    {
        for (int i = 0; i < patterns.getNumChildren(); ++i)
        {
            auto pattern = patterns.getChild (i);
            if (! pattern.hasType (IDs::Pattern))
                continue;
            auto seq = Model::getChannelSeq (pattern, channelId, false);
            if (! seq.isValid())
                continue;
            if (seq.getProperty (IDs::steps).toString().containsChar ('1'))
                return true;
            for (int n = 0; n < seq.getNumChildren(); ++n)
                if (seq.getChild (n).hasType (IDs::Note))
                    return true;
        }
        return false;
    };

    auto channels = Model::getChannels (project);
    for (int i = 0; i < channels.getNumChildren(); ++i)
    {
        auto ch = channels.getChild (i);
        if (! ch.hasType (IDs::Channel) || ch.getProperty (IDs::type).toString() != "sampler")
            continue;
        auto path = ch.getProperty (IDs::samplePath).toString();
        if (path.startsWith ("builtin:") || path.isEmpty())
            continue;
        if (channelHasContent ((int) ch.getProperty (IDs::id)))
            usedPaths.insert (path);
    }

    std::vector<juce::ValueTree> channelsToRemove;
    for (int i = 0; i < channels.getNumChildren(); ++i)
    {
        auto ch = channels.getChild (i);
        if (! ch.hasType (IDs::Channel) || ch.getProperty (IDs::type).toString() != "sampler")
            continue;
        auto path = ch.getProperty (IDs::samplePath).toString();
        if (path.startsWith ("builtin:") || path.isEmpty())
            continue;
        if (usedPaths.find (path) == usedPaths.end())
            channelsToRemove.push_back (ch);
    }

    std::vector<juce::ValueTree> filesToRemove;
    auto audioFiles = Model::getAudioFiles (project);
    for (int i = 0; i < audioFiles.getNumChildren(); ++i)
    {
        auto item = audioFiles.getChild (i);
        if (! item.hasType (IDs::AudioFile))
            continue;
        auto path = item.getProperty (IDs::audioPath).toString();
        if (path.startsWith ("builtin:"))
            continue;
        if (usedPaths.find (path) == usedPaths.end())
            filesToRemove.push_back (item);
    }

    for (auto& ch : channelsToRemove)
    {
        int cid = (int) ch.getProperty (IDs::id);
        for (int i = 0; i < patterns.getNumChildren(); ++i)
        {
            auto pattern = patterns.getChild (i);
            auto seq = Model::getChannelSeq (pattern, cid, false);
            if (seq.isValid())
                pattern.removeChild (seq, nullptr);
        }
        ch.getParent().removeChild (ch, nullptr);
    }

    for (auto& item : filesToRemove)
        item.getParent().removeChild (item, nullptr);

    const int nFiles = (int) filesToRemove.size();
    const int nChannels = (int) channelsToRemove.size();
    if (nFiles == 0 && nChannels == 0)
    {
        toasts.post ("No unused audio files", ToastLayer::Kind::info);
        return;
    }

    juce::String msg = "Removed " + juce::String (nFiles)
                       + (nFiles == 1 ? " audio file" : " audio files");
    if (nChannels > 0)
        msg += " and " + juce::String (nChannels)
               + (nChannels == 1 ? " channel" : " channels");
    toasts.post (msg, ToastLayer::Kind::success);
}

void MainComponent::showKeyBindingsWindow()
{
    if (keyBindingsWindow == nullptr)
        keyBindingsWindow = std::make_unique<KeyBindingsWindow> (
            keyBindings, pluginHost.getSettings(), [this] { grabKeyboardFocus(); });
    keyBindingsWindow->setVisible (true);
    keyBindingsWindow->toFront (true);
}

void MainComponent::toggleDebugOverlay()
{
    if (debugOverlay == nullptr)
    {
        debugOverlayUI = std::make_unique<DebugOverlayUI> (*perfMonitor, engine, project);
        debugOverlay = std::make_unique<DebugOverlayWindow>();
        debugOverlay->canvas().onFrame = [this] { debugOverlayUI->draw(); };
        debugOverlay->onClosed = [this]
        {
            debugOverlayUI->setActive (false);
            engine.setPluginPerfMonitoringEnabled (false);
        };
        engine.setPluginPerfMonitoringEnabled (true);
        debugOverlay->centreWithSize (980, 640);
        debugOverlay->setVisible (true);
        debugOverlay->toFront (true);
        return;
    }

    const bool show = ! debugOverlay->isVisible();
    debugOverlayUI->setActive (show);
    engine.setPluginPerfMonitoringEnabled (show);
    debugOverlay->setVisible (show);
    if (show)
    {
        debugOverlayUI->requestMainTab();
        debugOverlay->toFront (true);
    }
}

bool MainComponent::handleAction (KeyBindings::Action a)
{
    using A = KeyBindings::Action;
    switch (a)
    {
        case A::playPause:
            if (engine.isPlaying())
            {
                engine.stop();
            }
            else
            {
                syncPlayModeToActivePanel();
                engine.play();
            }
            return true;
        case A::stop:
            engine.stop();
            return true;
        case A::saveProject:
            saveProject();
            return true;
        case A::saveProjectAs:
            saveProjectAs();
            return true;
        case A::undo:
            history.undo();
            return true;
        case A::redo:
            history.redo();
            return true;
        case A::duplicate:
            if (activePanel == playlistPanel)
                playlist->duplicateSelection();
            else
                pianoRoll->duplicateSelection();
            return true;
        case A::copy:
            if (rollPanel->isVisible())
                pianoRoll->copySelection();
            return true;
        case A::paste:
            if (rollPanel->isVisible())
                pianoRoll->pasteClipboard();
            return true;
        case A::selectAll:
            if (activePanel == playlistPanel)
                playlist->selectAll();
            else if (rollPanel->isVisible())
                pianoRoll->selectAll();
            return true;
        case A::deselectAll:
            if (rollPanel->isVisible())
                pianoRoll->clearSelection();
            return true;
        case A::deleteSelection:
            if (activePanel == playlistPanel)
                playlist->deleteSelection();
            else if (rollPanel->isVisible())
                pianoRoll->deleteSelection();
            return true;
        case A::legato:
            pianoRoll->legatoSelection();
            return true;
        case A::arpeggiate:
            if (rollPanel->isVisible())
                pianoRoll->showArpeggiatorDialog();
            return true;
        case A::strumize:
            if (rollPanel->isVisible())
                pianoRoll->showStrumizerDialog();
            return true;
        case A::randomizeNotes:
            pianoRoll->randomizeNotesWeirdly();
            return true;
        case A::octaveUp:     pianoRoll->transposeSelection (12);  return true;
        case A::octaveDown:   pianoRoll->transposeSelection (-12); return true;
        case A::semitoneUp:
            if (activePanel == playlistPanel)
                playlist->moveSelectionTracks (-1);
            else
                pianoRoll->transposeSelection (1);
            return true;
        case A::semitoneDown:
            if (activePanel == playlistPanel)
                playlist->moveSelectionTracks (1);
            else
                pianoRoll->transposeSelection (-1);
            return true;
        case A::toggleBrowser:  togglePanel ("browser");  return true;
        case A::toggleRack:     togglePanel ("rack");     return true;
        case A::toggleRoll:     togglePanel ("roll");     return true;
        case A::togglePlaylist: togglePanel ("playlist"); return true;
        case A::toggleMixer:    togglePanel ("mixer");    return true;
        case A::toggleFullscreen:
            if (activePanel != nullptr && activePanel->isVisible())
                activePanel->toggleMaximised();
            return true;
        case A::toggleDebugOverlay:
            toggleDebugOverlay();
            return true;
        case A::toggleTypingKeyboard:
            typingKeyboardEnabled = ! typingKeyboardEnabled;
            transportBar.setTypingKeyboardActive (typingKeyboardEnabled);
            if (typingKeyboardEnabled)
            {
                toasts.post ("Typing keyboard: ON (Z = "
                                 + juce::String (Scales::rootName (Scales::projectRoot (project)))
                                 + juce::String (typingKeyboardOctave + 1) + ")",
                             ToastLayer::Kind::success);
                pluginHost.getSettings().setValue ("typingKeyboardOn", true);
                pluginHost.getSettings().saveIfNeeded();
            }
            else
            {
                releaseAllTypingNotes();
                toasts.post ("Typing keyboard: OFF", ToastLayer::Kind::info);
                pluginHost.getSettings().setValue ("typingKeyboardOn", false);
                pluginHost.getSettings().saveIfNeeded();
            }
            return true;
        default: return false;
    }
}

namespace
{
    struct TypingKey { int code; int row; int index; };

    static const TypingKey kTypingKeys[] =
    {
        { 'Z', 0, 0 }, { 'X', 0, 1 }, { 'C', 0, 2 }, { 'V', 0, 3 }, { 'B', 0, 4 },
        { 'N', 0, 5 }, { 'M', 0, 6 }, { ',', 0, 7 }, { '.', 0, 8 }, { '/', 0, 9 },
        { 'A', 1, 0 }, { 'S', 1, 1 }, { 'D', 1, 2 }, { 'F', 1, 3 }, { 'G', 1, 4 },
        { 'H', 1, 5 }, { 'J', 1, 6 }, { 'K', 1, 7 }, { 'L', 1, 8 }, { ';', 1, 9 }, { '\'', 1, 10 },
        { 'Q', 2, 0 }, { 'W', 2, 1 }, { 'E', 2, 2 }, { 'R', 2, 3 }, { 'T', 2, 4 },
        { 'Y', 2, 5 }, { 'U', 2, 6 }, { 'I', 2, 7 }, { 'O', 2, 8 }, { 'P', 2, 9 },
        { '1', 3, 0 }, { '2', 3, 1 }, { '3', 3, 2 }, { '4', 3, 3 }, { '5', 3, 4 },
        { '6', 3, 5 }, { '7', 3, 6 }, { '8', 3, 7 }, { '9', 3, 8 }, { '0', 3, 9 }
    };
}

int MainComponent::typingNoteForKey (int keyCode) const
{
    const int R = Scales::projectRoot (project);
    const auto& mask = Scales::defForId (Scales::projectTypeId (project)).mask;

    int degreeSemis[12];
    int N = 0;
    for (int s = 0; s < 12; ++s)
        if (mask[(size_t) s] != 0)
            degreeSemis[N++] = s;
    if (N == 0)
    {
        degreeSemis[0] = 0;
        N = 1;
    }

    const int base = R + 12 * (typingKeyboardOctave + 1);

    auto noteForDegree = [&] (int startNote, int d) -> int
    {
        return startNote + 12 * (d / N) + degreeSemis[d % N];
    };

    for (auto& k : kTypingKeys)
    {
        if (k.code != keyCode)
            continue;
        int rowStart = base + 12 * k.row;
        int note = noteForDegree (rowStart, k.index);
        return juce::jlimit (0, 127, note);
    }
    return -1;
}

bool MainComponent::typingNotesConsumeKey (int keyCode) const
{
    for (auto& k : kTypingKeys)
        if (k.code == keyCode)
            return true;
    return false;
}

bool MainComponent::focusIsInTextEditor()
{
    auto* f = juce::Component::getCurrentlyFocusedComponent();
    if (f == nullptr)
        return false;
    if (dynamic_cast<juce::TextEditor*> (f) != nullptr)
        return true;
    return f->findParentComponentOfClass<juce::TextEditor>() != nullptr;
}

int MainComponent::typingTargetChannel() const
{
    if (pianoRoll != nullptr)
    {
        int ch = pianoRoll->getChannelId();
        if (ch > 0)
            return ch;
    }
    auto channels = Model::getChannels (project);
    if (channels.getNumChildren() > 0)
        return (int) channels.getChild (0).getProperty (IDs::id);
    return 0;
}

void MainComponent::releaseAllTypingNotes()
{
    int ch = typingTargetChannel();
    for (auto& held : typingHeldNotes)
        if (ch > 0)
            engine.auditionNoteOff (ch, held.second);
    typingHeldNotes.clear();
    if (pianoRoll != nullptr)
        pianoRoll->clearKeyHighlights();
}

void MainComponent::updateTypingKeyboard()
{
    if (! typingKeyboardEnabled)
        return;

    if (focusIsInTextEditor()
        || juce::ModifierKeys::currentModifiers.isAnyModifierKeyDown())
        return;

    int ch = typingTargetChannel();
    const bool highlight = (pianoRoll != nullptr && ch > 0
                            && pianoRoll->getChannelId() == ch);

    for (auto& k : kTypingKeys)
    {
        bool down = juce::KeyPress::isKeyCurrentlyDown (k.code);
        auto it = typingHeldNotes.find (k.code);
        bool wasDown = (it != typingHeldNotes.end());

        if (down && ! wasDown)
        {
            int note = typingNoteForKey (k.code);
            typingHeldNotes[k.code] = note;
            if (ch > 0)
                engine.auditionNote (ch, note, 100);
            if (highlight)
                pianoRoll->setKeyHighlight (note, true);
        }
        else if (! down && wasDown)
        {
            if (ch > 0)
                engine.auditionNoteOff (ch, it->second);
            if (pianoRoll != nullptr)
                pianoRoll->setKeyHighlight (it->second, false);
            typingHeldNotes.erase (it);
        }
    }
}

bool MainComponent::keyStateChanged (bool)
{
    if (renderLocked || ! typingKeyboardEnabled)
        return false;

    if (focusIsInTextEditor()
        || juce::ModifierKeys::currentModifiers.isAnyModifierKeyDown())
        return false;

    bool anyMappedDown = false;
    for (auto& k : kTypingKeys)
        if (juce::KeyPress::isKeyCurrentlyDown (k.code))
        {
            anyMappedDown = true;
            break;
        }

    size_t before = typingHeldNotes.size();
    updateTypingKeyboard();
    bool changed = (typingHeldNotes.size() != before);

    return changed || anyMappedDown;
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    noteLocalActivity();
    if (renderLocked)
        return true;
    auto action = keyBindings.match (key);
    if (action != KeyBindings::Action::numActions)
        return handleAction (action);

    if (typingKeyboardEnabled && ! key.getModifiers().isAnyModifierKeyDown()
        && ! focusIsInTextEditor())
    {
        if (key.isKeyCode (juce::KeyPress::upKey) || key.isKeyCode (juce::KeyPress::downKey))
        {
            int delta = key.isKeyCode (juce::KeyPress::upKey) ? 1 : -1;
            typingKeyboardOctave = juce::jlimit (0, 6, typingKeyboardOctave + delta);
            toasts.post ("Typing keyboard octave: Z = "
                             + juce::String (Scales::rootName (Scales::projectRoot (project)))
                             + juce::String (typingKeyboardOctave + 1),
                         ToastLayer::Kind::info, 1600);
            return true;
        }

        if (typingNotesConsumeKey (key.getKeyCode()))
        {
            return true;
        }
    }

    if (key.isKeyCode (juce::KeyPress::backspaceKey) && key.getModifiers().isAnyModifierKeyDown() == false)
        return handleAction (KeyBindings::Action::deleteSelection);

    return false;
}

void MainComponent::syncPlayModeToActivePanel()
{
    if (session.isActive())
    {
        const bool wantSong = activePanel == playlistPanel && playlistPanel->isVisible();
        const bool wantPat  = activePanel == rollPanel && rollPanel->isVisible();
        const bool song = engine.isSongMode();
        if ((wantSong && ! song) || (wantPat && song))
        {
            if (modeMismatchWarnedSong != (song ? 1 : 0))
            {
                modeMismatchWarnedSong = song ? 1 : 0;
                toasts.post (song ? "Playing SONG - switch to PAT in the transport bar"
                                  : "Playing PATTERN - switch to SONG in the transport bar",
                             ToastLayer::Kind::info, 2500);
                transportBar.flashModeToggle();
            }
        }
        else
        {
            modeMismatchWarnedSong = -1;
        }
        return;
    }

    modeMismatchWarnedSong = -1;

    if (activePanel == playlistPanel && playlistPanel->isVisible() && ! engine.isSongMode())
        project.setProperty (IDs::songMode, true, nullptr);
    else if (activePanel == rollPanel && rollPanel->isVisible() && engine.isSongMode())
        project.setProperty (IDs::songMode, false, nullptr);
}

void MainComponent::toggleRecord()
{
    if (renderLocked)
        return;

    auto& s = pluginHost.getSettings();
    if (engine.isRecordArmed())
    {
        engine.setRecordArmed (false);
        return;
    }

    if (! AppSettings::recAudio (s) && ! AppSettings::recNotes (s))
        toasts.post ("Both Notes and Audio recording are off (record button menu)",
                     ToastLayer::Kind::warning);

    engine.setAudioRecordingEnabled (AppSettings::recAudio (s));
    engine.setRecordArmed (true);
    if (AppSettings::recStartsPlayback (s) && ! engine.isPlaying())
    {
        syncPlayModeToActivePanel();
        engine.play();
    }
}

void MainComponent::captureRecordedNote (int channelId, bool on, int note, float velocity)
{
    auto& s = pluginHost.getSettings();
    const bool capturing = engine.isRecordArmed() && engine.isPlaying() && AppSettings::recNotes (s);
    if (! capturing)
    {
        if (! engine.isRecordArmed() && ! engine.isRecording())
            recHeldNotes.clear();
        return;
    }
    if (note < 0 || note > 127)
        return;

    auto keyPair = std::make_pair (channelId, note);
    if (on)
    {
        recHeldNotes[keyPair] = { engine.getPositionTicks(),
                                  juce::jlimit (1, 127, juce::roundToInt (velocity * 127.0f)) };
        return;
    }

    auto it = recHeldNotes.find (keyPair);
    if (it == recHeldNotes.end())
        return;
    double startTick = it->second.startTick;
    int vel = it->second.velocity;
    recHeldNotes.erase (it);

    double endTick = engine.getPositionTicks();
    const bool patMode = ! engine.isSongMode();
    const double loopLen = engine.getLoopLengthTicks();
    if (patMode && loopLen > 0.0)
    {
        startTick = std::fmod (juce::jmax (0.0, startTick), loopLen);
        endTick   = std::fmod (juce::jmax (0.0, endTick),   loopLen);
        if (endTick <= startTick)
            endTick += loopLen;
    }

    if (AppSettings::recQuantizeStart (s))
        startTick = std::round (startTick / (double) Model::ticksPerStep) * Model::ticksPerStep;
    double endQ = AppSettings::recQuantizeEnd (s)
                    ? std::round (endTick / (double) Model::ticksPerStep) * Model::ticksPerStep
                    : endTick;
    double lengthTick = juce::jmax (6.0, endQ - startTick);

    auto pattern = Model::findPatternById (project, engine.getCurrentPatternId());
    if (! pattern.isValid())
        return;
    auto seq = Model::getChannelSeq (pattern, channelId, true);
    seq.setProperty (IDs::usePianoRoll, true, nullptr);

    juce::ValueTree n (IDs::Note);
    n.setProperty (IDs::key, note, nullptr);
    n.setProperty (IDs::start, (int) std::llround (startTick), nullptr);
    n.setProperty (IDs::length, (int) std::llround (lengthTick), nullptr);
    n.setProperty (IDs::velocity, vel, nullptr);
    seq.appendChild (n, nullptr);
}

int MainComponent::chooseRecordLane (int mixerTrack, double startBeats)
{
    if (auto cached = recordingLaneCache.find (mixerTrack); cached != recordingLaneCache.end())
        return cached->second;

    Model::ensurePlaylistTracks (project);
    int count = Model::countPlaylistTracks (project);
    auto pl = Model::getPlaylist (project);

    auto laneClaimedByOther = [this, mixerTrack] (int lane)
    {
        for (auto& kv : recordingLaneCache)
            if (kv.first != mixerTrack && kv.second == lane)
                return true;
        return false;
    };

    int chosen = -1;
    for (int lane = 0; lane < count; ++lane)
    {
        if (! Model::isPlaylistTrackEnabled (project, lane) || laneClaimedByOther (lane))
            continue;
        bool occupied = false;
        for (int i = 0; i < pl.getNumChildren(); ++i)
        {
            auto clip = pl.getChild (i);
            if (! clip.hasType (IDs::Clip) || (int) clip.getProperty (IDs::track) != lane)
                continue;
            double cs = clip.getProperty (IDs::startBeats);
            double cl = clip.getProperty (IDs::lengthBeats);
            if (cs + cl > startBeats)
            {
                occupied = true;
                break;
            }
        }
        if (! occupied)
        {
            chosen = lane;
            break;
        }
    }
    if (chosen < 0)
    {
        chosen = count;
        while (laneClaimedByOther (chosen))
            ++chosen;
    }
    recordingLaneCache[mixerTrack] = chosen;
    return chosen;
}

void MainComponent::onPendingClipAdded (const juce::ValueTree& clip)
{
    if (! session.isActive()
        || ! clip.hasType (IDs::Clip)
        || clip.getProperty (IDs::type).toString() != "audio"
        || ! (bool) clip.getProperty (IDs::pending, false))
        return;

    auto recBy = clip.getProperty (IDs::recordedBy).toString();
    auto localId = juce::String (session.getLocalPeerId());
    if (recBy.isEmpty() || recBy == localId)
        return;

    juce::String name = "A peer";
    for (auto& u : session.getRoster())
        if (juce::String (u.peerId) == recBy) { name = u.name; break; }

    toasts.post (name + " recorded a clip - waiting for them to share it",
                 ToastLayer::Kind::info, 5000);
}

void MainComponent::onPendingClipCleared (const juce::ValueTree& clip)
{
    if (! session.isActive()
        || ! clip.hasType (IDs::Clip)
        || clip.getProperty (IDs::type).toString() != "audio"
        || (bool) clip.getProperty (IDs::pending, false))
        return;

    if (clip.getProperty (IDs::recordedBy).toString() == juce::String (session.getLocalPeerId()))
        toasts.post ("Sharing recording with peers...", ToastLayer::Kind::success, 3000);
}

void MainComponent::mouseMove (const juce::MouseEvent& e) { noteLocalActivity(); broadcastLocalCursor (e); }
void MainComponent::mouseDrag (const juce::MouseEvent& e) { noteLocalActivity(); noteLocalViewport (e.eventComponent); broadcastLocalCursor (e); }

void MainComponent::mouseDown (const juce::MouseEvent& e)
{
    noteLocalActivity();
    noteLocalViewport (e.eventComponent);
}

void MainComponent::showReactionPickerAt (const juce::MouseEvent& e)
{
    auto p = desk.getLocalPoint (e.eventComponent, e.position);
    float nx = juce::jlimit (0.0f, 1.0f, (float) p.x / juce::jmax (1.0f, (float) desk.getWidth()));
    float ny = juce::jlimit (0.0f, 1.0f, (float) p.y / juce::jmax (1.0f, (float) desk.getHeight()));

    auto screen = e.getScreenPosition();
    ReactionMenu::show (juce::PopupMenu::Options().withTargetScreenArea (
                            juce::Rectangle<int> (screen.x, screen.y, 1, 1)),
        [this, nx, ny] (const juce::String& emoji)
        {
            reactions.addReaction (session.getLocalPeerId(), session.getDisplayName(),
                                   emoji, nx, ny);
            if (session.isActive())
            {
                session.broadcastReaction (emoji, nx, ny);
                logActivity (session.getDisplayName(), "reacted " + emoji);
            }
        });
}

void MainComponent::noteLocalViewport (juce::Component* eventComponent)
{
    juce::String panelName;
    for (auto* c = eventComponent; c != nullptr; c = c->getParentComponent())
    {
        if (auto* fp = dynamic_cast<FloatingPanel*> (c))
        {
            panelName = panelDisplayName (fp);
            break;
        }
        if (browser != nullptr && c == browser.get())
        {
            panelName = "Browser";
            break;
        }
        if (chat != nullptr && c == chat.get())
        {
            panelName = "Chat";
            break;
        }
    }

    if (panelName == localViewport)
        return;
    localViewport = panelName;

    if (! session.isActive())
        return;
    int patternId = (panelName == "Piano Roll" && pianoRoll != nullptr) ? pianoRoll->getPatternId() : -1;
    session.broadcastViewport (panelName, patternId);
}

void MainComponent::refreshLocalViewportPattern()
{
    if (session.isActive() && localViewport == "Piano Roll" && pianoRoll != nullptr)
        session.broadcastViewport (localViewport, pianoRoll->getPatternId());
}

juce::String MainComponent::panelDisplayName (FloatingPanel* p) const
{
    if (p == rackPanel)     return "Channel Rack";
    if (p == rollPanel)     return "Piano Roll";
    if (p == playlistPanel) return "Playlist";
    if (p == mixerPanel)    return "Mixer";
    if (p == friendsPanel)  return "Social";
    if (p == samplerPanel)  return "Sampler";
    return {};
}

void MainComponent::refreshPeerBadges()
{
    auto badgesFor = [this] (const juce::String& panelName)
    {
        std::vector<std::pair<juce::int64, juce::String>> out;
        for (auto& [id, np] : peerViewports)
            if (np.panelName == panelName)
                out.push_back ({ id, np.name });
        return out;
    };

    if (rackPanel)     rackPanel->setPeerBadges (badgesFor ("Channel Rack"));
    if (rollPanel)     rollPanel->setPeerBadges (badgesFor ("Piano Roll"));
    if (playlistPanel) playlistPanel->setPeerBadges (badgesFor ("Playlist"));
    if (mixerPanel)    mixerPanel->setPeerBadges (badgesFor ("Mixer"));
    if (samplerPanel)  samplerPanel->setPeerBadges (badgesFor ("Sampler"));

    if (playlist != nullptr)
        playlist->repaintCanvas();
}

void MainComponent::logActivity (const juce::String& actor, const juce::String& action)
{
    if (chat == nullptr)
        return;
    juce::Colour col = Colors::text;
    for (auto& u : session.getRoster())
        if (u.name == actor)
        {
            col = Colors::peerColour (u.peerId);
            break;
        }
    chat->addEventMessage (actor, col, action);
}

void MainComponent::reportDeletion (const juce::String& what)
{
    if (history.isApplying())
        return;

    const bool remote = session.remoteApplyFlag != nullptr && session.remoteApplyFlag->load();

    const auto now = juce::Time::currentTimeMillis();
    if (now - deletionBurstMs >= 200)
    {
        deletionBurstMs = now;
        deletionBurstCount = 1;
        deletionBurstRemote = remote;
    }
    else
    {
        ++deletionBurstCount;
        deletionBurstRemote = deletionBurstRemote || remote;
    }

    if (deletionBurstCount > 3)
    {
        if (deletionBurstCount == 4)
            toasts.post (deletionBurstRemote ? juce::String ("A peer deleted several items")
                                             : juce::String ("Several items were deleted"),
                         ToastLayer::Kind::error, deletionBurstRemote ? 6000 : 4200);
        return;
    }

    if (remote)
    {
        toasts.post (what + " was deleted by a peer", ToastLayer::Kind::error, 6000);
        logActivity ("A peer", "deleted " + what);
    }
    else
    {
        toasts.post ("Deleted " + what, ToastLayer::Kind::error);
    }
}

juce::String MainComponent::vstAddedLocation (const juce::String& key) const
{
    if (! key.startsWithChar ('m'))
        return {};
    int s = key.indexOfChar ('s');
    if (s < 2)
        return {};
    int trackIndex = key.substring (1, s).getIntValue();
    auto track = Model::getMixerTrack (project, trackIndex);
    juce::String name = track.isValid() ? track.getProperty (IDs::name).toString() : juce::String();
    if (name.isEmpty())
        name = trackIndex == 0 ? juce::String ("Master") : "Insert " + juce::String (trackIndex);
    return " on " + name;
}

void MainComponent::setLocalAfk (bool afk)
{
    if (afk == localAfk)
        return;

    localAfk = afk;
    afkSinceMs = afk ? juce::Time::currentTimeMillis() : 0;

    if (session.isActive())
        session.broadcastUserState (1.0f, engine.isDeafened(), afk);

    lanDiscovery.sendPresenceNow();
    relay.notifyStateChanged();

    if (friendsUi != nullptr)
        friendsUi->refresh();
}

void MainComponent::setManualAway (bool away)
{
    manualAway = away;
    AppSettings::setManualAway (pluginHost.getSettings(), away);
    lastActivityMs = juce::Time::currentTimeMillis();
    setLocalAfk (away);
    AppSettings::setManualAwaySince (pluginHost.getSettings(), away ? afkSinceMs : 0);
}

void MainComponent::noteLocalActivity()
{
    lastActivityMs = juce::Time::currentTimeMillis();
    if (! manualAway)
        setLocalAfk (false);
}

void MainComponent::checkAfk()
{
    if (localAfk || lastActivityMs == 0)
        return;
    if (juce::Time::currentTimeMillis() - lastActivityMs > 3 * 60 * 1000)
        setLocalAfk (true);
}

void MainComponent::broadcastLocalCursor (const juce::MouseEvent& e)
{
    if (! session.isActive())
        return;

    auto now = juce::Time::getMillisecondCounter();
    if (now - lastCursorSendMs < 25)
        return;
    lastCursorSendMs = now;

    auto p = getLocalPoint (e.eventComponent, e.position);
    float w = juce::jmax (1.0f, (float) getWidth());
    float h = juce::jmax (1.0f, (float) getHeight());
    session.broadcastCursor ((float) p.x / w, (float) p.y / h);
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (Colors::windowBg);
}

void MainComponent::LockOverlay::paint (juce::Graphics& g)
{
    g.fillAll (Colors::windowBg.withAlpha (0.80f));
    g.setColour (Colors::text);
    g.setFont (juce::Font (juce::FontOptions (19.0f, juce::Font::bold)));
    g.drawText (message, getLocalBounds().reduced (24),
                juce::Justification::centred, true);
}

void MainComponent::showRenderLock (const juce::String& byName)
{
    renderLocked = true;
    if (lockOverlay == nullptr)
        lockOverlay = std::make_unique<LockOverlay>();
    lockOverlay->message = byName.isNotEmpty() ? "Locked - " + byName + " is rendering..."
                                               : "Locked - rendering...";
    addAndMakeVisible (*lockOverlay);
    lockOverlay->setBounds (getLocalBounds());
    lockOverlay->toFront (false);
    lockOverlay->repaint();

    pluginWindows.setWindowsRenderLocked (true);
}

void MainComponent::hideRenderLock()
{
    renderLocked = false;
    lockOverlay.reset();
    pluginWindows.setWindowsRenderLocked (false);
}

void MainComponent::showImageLightbox (const juce::Image& img, const juce::MemoryBlock& bytes)
{
    if (! img.isValid())
        return;
    if (lightbox == nullptr)
    {
        lightbox = std::make_unique<ImageLightbox>();
        lightbox->onClose = [this] { hideImageLightbox(); };
        lightbox->onSave  = [this]
        {
            if (chat != nullptr && lightbox != nullptr)
                chat->saveImage (lightbox->image, lightbox->bytes);
        };
    }
    addAndMakeVisible (*lightbox);
    lightbox->setBounds (getLocalBounds());
    lightbox->setContent (img, bytes);
    lightbox->toFront (true);
    lightbox->grabKeyboardFocus();
}

void MainComponent::hideImageLightbox()
{
    lightbox.reset();
}

void MainComponent::parentHierarchyChanged()
{
    updateWindowTitle();
}

void MainComponent::resized()
{
    auto r = getLocalBounds();
    transportBar.setBounds (r.removeFromTop (50));
    if (dockedLayout)
        statusBar.setBounds (r.removeFromBottom (StatusBar::height));

    if (chatVisible && chat != nullptr && ! chat->isDockedRight())
        chat->setBounds (r.removeFromLeft (chat->getPreferredWidth()));
    if (browserVisible)
        browser->setBounds (r.removeFromLeft (browser->getPreferredWidth()));
    if (chatVisible && chat != nullptr && chat->isDockedRight())
        chat->setBounds (r.removeFromRight (chat->getPreferredWidth()));

    desk.setBounds (r);
    auto deskArea = desk.getLocalBounds();

    if (! deskArea.isEmpty())
    {
        if (! panelsLaidOut)
        {
            panelsLaidOut = true;
            for (auto* p : panels)
                if (! p->isDocked())
                    p->materialiseDefault (deskArea);
            if (rack != nullptr)
                rack->onContentHeightChanged();
        }

        for (auto* p : panels)
            if (! p->isDocked())
                p->clampIntoDesk (deskArea);

        if (dockedLayout)
            layoutDockedDesk (deskArea);
    }

    reactions.setBounds (deskArea);
    reactions.toFront (false);
    peerCursors.setBounds (getLocalBounds());
    peerCursors.toFront (false);
    toasts.setBounds (getLocalBounds());
    toasts.toFront (false);

    if (lockOverlay != nullptr)
    {
        lockOverlay->setBounds (getLocalBounds());
        lockOverlay->toFront (false);
    }

    if (lightbox != nullptr)
    {
        lightbox->setBounds (getLocalBounds());
        lightbox->toFront (false);
    }
}
