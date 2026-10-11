#include "Session.h"
#include "SyncDebug.h"
#include "../Engine/Bridge32Host.h"
#include <cmath>

namespace
{
    juce::int64 hashBlobForSession (const juce::MemoryBlock& mb)
    {
        juce::uint64 h = 1469598103934665603ull;
        auto* p = static_cast<const juce::uint8*> (mb.getData());
        for (size_t i = 0; i < mb.getSize(); ++i)
        {
            h ^= p[i];
            h *= 1099511628211ull;
        }
        return (juce::int64) h;
    }

    void logState (const juce::String& msg)
    {
        juce::Logger::writeToLog ("[mp-state] " + msg);
    }

    struct ScopedAtomicFlag
    {
        explicit ScopedAtomicFlag (std::atomic<bool>& f, bool value = true)
            : flag (f), previous (f.load()) { flag.store (value); }
        ~ScopedAtomicFlag() { flag.store (previous); }
        std::atomic<bool>& flag;
        const bool previous;
        JUCE_DECLARE_NON_COPYABLE (ScopedAtomicFlag)
    };

    struct ScopedOptionalFlag
    {
        explicit ScopedOptionalFlag (std::atomic<bool>* f, bool value = true)
            : flag (f), previous (f != nullptr && f->load()) { if (flag) flag->store (value); }
        ~ScopedOptionalFlag() { if (flag) flag->store (previous); }
        std::atomic<bool>* flag;
        const bool previous;
        JUCE_DECLARE_NON_COPYABLE (ScopedOptionalFlag)
    };

    bool looksLikeAudioPath (const juce::String& path)
    {
        if (path.isEmpty() || path.startsWithIgnoreCase ("builtin:"))
            return false;
        return juce::File (path).hasFileExtension ("wav;aif;aiff;flac;ogg;mp3;wma");
    }
}

void Session::Peer::connectionMade()
{
    lastRecvMs = juce::Time::currentTimeMillis();
    if (owner.role == Role::client)
    {
        juce::MemoryOutputStream out;
        out.writeByte ((char) msgHello);
        out.writeInt64 (owner.localPeerId);
        out.writeString (owner.localDisplayName);
        out.writeInt (protocolVersion);
        owner.enqueueTo (this, out.getMemoryBlock());

        owner.sendInventoryTo (this);
    }
    owner.updateStatus();
}

void Session::Peer::connectionLost()
{
    owner.removePeer (this);
}

void Session::Peer::messageReceived (const juce::MemoryBlock& msg)
{
    owner.handleMessage (*this, msg);
}

juce::InterprocessConnection* Session::Server::createConnectionObject()
{
    auto* peer = new Peer (owner);
    owner.registerLink (peer);
    juce::WeakReference<Session> weak (&owner);
    juce::MessageManager::callAsync ([weak, peer]
    {
        auto* s = weak.get();
        if (s == nullptr || s->role != Role::host)
        {
            if (s != nullptr)
                s->unregisterLink (peer);
            delete peer;
            return;
        }
        s->peers.add (peer);
        s->updateStatus();
    });
    return peer;
}

void Session::Sync::stateChanged (const void* data, size_t size)
{
    if (owner.applyingRemote || ! owner.isActive())
        return;

    juce::MemoryOutputStream out;
    out.writeByte ((char) msgDelta);
    out.write (data, size);
    owner.sendToAll (out.getMemoryBlock());
}

Session::Session (juce::ValueTree projectTree, AudioEngine& e, PluginHost& host)
    : project (std::move (projectTree)), engine (e), pluginHost (host)
{
    { juce::WeakReference<Session> primeMaster (this); }

    project.addListener (this);

    sessionFolder = multiplayerCacheFolder();
    sessionFolder.createDirectory();
    loadCacheIndex();

    engine.setSamplePathResolver (
        [this] (const juce::String& originalPath) -> juce::File
        {
            const juce::ScopedLock sl (audioPathsLock);
            auto it = downloadedAudioPaths.find (originalPath);
            if (it != downloadedAudioPaths.end() && it->second.existsAsFile())
                return it->second;
            auto ci = sessionAudioEntries.find (originalPath);
            if (ci != sessionAudioEntries.end() && ci->second.existsAsFile())
                return ci->second;
            return {};
        });
}


Session::~Session()
{
    clearCallbacks();
    project.removeListener (this);
    engine.setSamplePathResolver (nullptr);
    disconnect();
}

bool Session::startHost (int port)
{
    disconnect();
    server = std::make_unique<Server> (*this);
    if (! server->beginWaitingForSocket (port))
    {
        server.reset();
        if (onStatus)
            onStatus ("Failed to open port " + juce::String (port));
        return false;
    }

    role = Role::host;
    sessionEndedByHost = false;
    kickedByHost = false;
    Model::ensureIdentities (project);
    prepareSessionFolder();
    senderThread = std::make_unique<SenderThread> (*this);
    senderThread->startThread();
    sync = std::make_unique<Sync> (project, *this);
    startTimerHz (4);
    updateStatus();
    return true;
}

bool Session::joinHost (const juce::String& address, int port)
{
    disconnect();

    auto peer = std::make_unique<Peer> (*this);
    if (! peer->connectToSocket (address, port, 5000))
    {
        if (onStatus)
            onStatus ("Could not connect to " + address + ":" + juce::String (port));
        return false;
    }

    role = Role::client;
    sessionEndedByHost = false;
    kickedByHost = false;
    prepareSessionFolder();
    senderThread = std::make_unique<SenderThread> (*this);
    senderThread->startThread();
    {
        auto* raw = peer.release();
        peers.add (raw);
        registerLink (raw);
    }
    sync = std::make_unique<Sync> (project, *this);
    startTimerHz (4);
    updateStatus();
    return true;
}

void Session::clearCallbacks()
{
    onStatus = nullptr;
    onPeerCursor = nullptr;
    onPeerJoined = nullptr;
    onPeerLeft = nullptr;
    onSnapshotApplied = nullptr;
    onSessionEnded = nullptr;
    onKicked = nullptr;
    onJoinRejected = nullptr;
    onProjectSavedByPeer = nullptr;
    onRenderedByPeer = nullptr;
    onRenderLockChanged = nullptr;
    onSoloChangedByPeer = nullptr;
    onVstAddedByPeer = nullptr;
    onAudioDownloadStarted = nullptr;
    onAudioDownloadFinished = nullptr;
    onMultiplayerFolderChanged = nullptr;
    onCompatibilityChanged = nullptr;
    onAvatarChanged = nullptr;
    onRosterChanged = nullptr;
    isPluginWindowOpen = nullptr;
    getPluginInteraction = nullptr;
    onPeerAudition = nullptr;
    onChatText = nullptr;
    onChatImageAnnounce = nullptr;
    onChatImageData = nullptr;
    onChatImageRejected = nullptr;
    onPeerViewport = nullptr;
    onPeerReaction = nullptr;
}

void Session::disconnect()
{
    stopTimer();
    cancelPendingUpdate();
    sync.reset();

    if (senderThread != nullptr)
    {
        senderThread->signalThreadShouldExit();
        queueSignal.signal();
        senderThread->stopThread (4000);
        senderThread.reset();
    }
    { const juce::ScopedLock sl (queueLock);  outQueue.clear(); }
    { const juce::ScopedLock sl (linksLock);  peerLinks.clear(); }
    audioWritePool.removeAllJobs (true, 4000);

    if (server != nullptr)
        server->stop();
    peers.clear();
    server.reset();
    role = Role::none;

    if (renderLockHeld)
    {
        renderLockHeld = false;
        renderLockOwner.clear();
        if (onRenderLockChanged)
            onRenderLockChanged ({}, false);
    }
    lastStateHash.clear();
    captureCostMs.clear();
    lastPolledTick.clear();
    latestRemoteState.clear();
    remoteApplyStamp.clear();
    pendingState.clear();
    deferredStateApplies.clear();
    retiredStateApplies.clear();
    lastAppliedStateHash.clear();
    deltasAppliedSinceSnapshot = 0;
    unresolvedDropCount = 0;
    unresolvedWindowStartMs = 0;
    lastResyncMs = 0;
    resyncRequestsUnanswered = 0;
    resyncGaveUp = false;
    pollTick = 0;
    {
        const juce::ScopedLock sl (audioPathsLock);
        downloadedAudioPaths.clear();
    }
    sharedAudioPaths.clear();
    remoteStates.clear();
    peerInventories.clear();
    compatibleSignatures.clear();
    peerAvatarBytes.clear();
    for (auto it = peerAvatars.begin(); it != peerAvatars.end();)
        it = (it->first == localPeerId) ? std::next (it) : peerAvatars.erase (it);
    if (onMultiplayerFolderChanged)
        onMultiplayerFolderChanged (sessionFolder);
    updateStatus();
    if (onRosterChanged)
        onRosterChanged();
}

void Session::endSession()
{
    if (role == Role::host)
    {
        {
            juce::MemoryOutputStream out;
            out.writeByte ((char) msgSessionEnded);
            out.writeString (localDisplayName);
            sendToAll (out.getMemoryBlock());
        }

        const juce::uint32 start = juce::Time::getMillisecondCounter();
        for (;;)
        {
            bool drained;
            { const juce::ScopedLock sl (queueLock); drained = outQueue.empty(); }
            if (drained || juce::Time::getMillisecondCounter() - start >= 500)
                break;
            juce::Thread::sleep (10);
        }
        juce::Thread::sleep (40);
    }

    disconnect();
}

void Session::kickPeer (juce::int64 peerId)
{
    if (role != Role::host || peerId == 0)
        return;

    Peer* target = nullptr;
    for (auto* p : peers)
        if (p->peerId == peerId)
            { target = p; break; }
    if (target == nullptr)
        return;

    {
        juce::MemoryOutputStream out;
        out.writeByte ((char) msgKick);
        out.writeString (localDisplayName);
        enqueueTo (target, out.getMemoryBlock());
    }

    juce::WeakReference<Session> weak (this);
    juce::Timer::callAfterDelay (1500, [weak, peerId]
    {
        if (auto* self = weak.get())
        {
            if (self->role != Role::host || peerId == 0)
                return;
            for (auto* p : self->peers)
                if (p->peerId == peerId)
                    { self->removePeer (p); break; }
        }
    });
}

void Session::removePeer (Peer* peer)
{
    juce::String goneName;
    juce::int64 goneId = 0;
    bool wasHostPeer = (role == Role::host);
    if (peers.contains (peer))
    {
        goneName = peer->displayName;
        goneId = peer->peerId;
    }

    juce::WeakReference<Session> weakSelf (this);
    juce::MessageManager::callAsync ([weakSelf, peer, goneName, goneId, wasHostPeer]
    {
        if (auto* self = weakSelf.get())
            self->finishPeerRemoval (peer, goneName, goneId, wasHostPeer);
    });
}

void Session::finishPeerRemoval (Peer* peer, const juce::String& goneName, juce::int64 goneId, bool wasHostPeer)
{
    unregisterLink (peer);
    if (peers.contains (peer))
        peers.removeObject (peer, true);

    if (renderLockHeld && (renderLockOwner.isEmpty() || goneName == renderLockOwner))
    {
        renderLockHeld = false;
        renderLockOwner.clear();
        if (onRenderLockChanged)
            onRenderLockChanged ({}, false);
    }

    bool rosterChanged = false;
    if (wasHostPeer)
    {
        if (goneId != 0 && remoteStates.erase (goneId) > 0)
            rosterChanged = true;
        if (goneId != 0)
        {
            peerAvatarBytes.erase (goneId);
            peerAvatars.erase (goneId);
        }
        if (goneId != 0 && peerInventories.count (goneId) > 0)
        {
            dropPeerInventory (goneId);
            juce::MemoryOutputStream out;
            out.writeByte ((char) msgPluginInventory);
            out.writeInt64 (goneId);
            out.writeByte ((char) 1);
            sendToAll (out.getMemoryBlock());
        }
        if (goneName.isNotEmpty())
            announcePresence (1, goneName, goneId);
    }

    if (role == Role::client && peers.isEmpty())
    {
        sync.reset();
        role = Role::none;
        if (! remoteStates.empty()) { remoteStates.clear(); rosterChanged = true; }
        if (! peerInventories.empty())
        {
            peerInventories.clear();
            recomputeCompatibility();
            if (onCompatibilityChanged) onCompatibilityChanged();
        }
        peerAvatarBytes.clear();
        peerAvatars.clear();
        if (onPeerLeft && ! sessionEndedByHost && ! kickedByHost)
            onPeerLeft (hostDisplayName.isNotEmpty() ? hostDisplayName : "the host");
        hostDisplayName.clear();
    }
    updateStatus();
    if (rosterChanged && onRosterChanged)
        onRosterChanged();
}

void Session::dropStalePeerFor (Peer& joiner)
{
    std::vector<Peer*> stale;
    for (auto* p : peers)
        if (p != &joiner && joiner.peerId != 0 && p->peerId == joiner.peerId)
            stale.push_back (p);

    for (auto* p : stale)
    {
        p->peerId = 0;
        p->displayName = {};
        p->announced = true;
        removePeer (p);
    }
}

void Session::announcePresence (int kind, const juce::String& name, juce::int64 peerId, Peer* except)
{
    if (kind == 0 && onPeerJoined) onPeerJoined (name);
    else if (kind == 1 && onPeerLeft) onPeerLeft (name);

    if (role == Role::host)
    {
        juce::MemoryOutputStream out;
        out.writeByte ((char) msgPresence);
        out.writeInt (kind);
        out.writeString (name);
        out.writeInt64 (peerId);
        sendToAll (out.getMemoryBlock(), except);
    }
}

void Session::updateStatus()
{
    if (onStatus == nullptr)
        return;

    switch (role)
    {
        case Role::host:
        {
            int n = peers.size();
            onStatus ("Hosting " + juce::String (n) + " peer" + (n == 1 ? "" : "s"));
            break;
        }
        case Role::client:
            onStatus ("Connected");
            break;
        case Role::none:
        default:
            onStatus ("Offline");
            break;
    }
}

void Session::prepareSessionFolder()
{
    sessionFolder = multiplayerCacheFolder();
    sessionFolder.createDirectory();
    {
        const juce::ScopedLock sl (audioPathsLock);
        downloadedAudioPaths.clear();
    }
    sharedAudioPaths.clear();
    if (onMultiplayerFolderChanged)
        onMultiplayerFolderChanged (sessionFolder);
}

juce::File Session::multiplayerCacheFolder()
{
    return juce::File::getSpecialLocation (juce::File::tempDirectory)
               .getChildFile ("mikeDAW")
               .getChildFile ("Multiplayer");
}

void Session::loadCacheIndex()
{
    auto indexFile = sessionFolder.getChildFile ("cache-index.xml");
    if (! indexFile.existsAsFile())
        return;

    auto xml = juce::XmlDocument::parse (indexFile);
    if (xml == nullptr)
        return;

    const juce::ScopedLock sl (audioPathsLock);
    for (auto* e : xml->getChildWithTagNameIterator ("Entry"))
    {
        auto originalPath = e->getStringAttribute ("originalPath");
        auto fileName = e->getStringAttribute ("fileName");
        if (originalPath.isNotEmpty() && fileName.isNotEmpty())
            sessionAudioEntries[originalPath] = sessionFolder.getChildFile (fileName);
    }
}

void Session::saveCacheIndex() const
{
    juce::XmlElement root ("AudioCache");
    {
        const juce::ScopedLock sl (audioPathsLock);
        for (auto& kv : sessionAudioEntries)
        {
            auto* e = root.createNewChildElement ("Entry");
            e->setAttribute ("originalPath", kv.first);
            e->setAttribute ("fileName", kv.second.getRelativePathFrom (sessionFolder)
                                            .replaceCharacter ('\\', '/'));
        }
    }
    root.writeTo (sessionFolder.getChildFile ("cache-index.xml"));
}

bool Session::isShareableAudioPath (const juce::String& path)
{
    if (! looksLikeAudioPath (path))
        return false;
    return juce::File (path).existsAsFile();
}

juce::String Session::fileHashPrefix (const juce::String& text)
{
    juce::uint32 h = 2166136261u;
    auto utf8 = text.toRawUTF8();
    for (auto* p = reinterpret_cast<const unsigned char*> (utf8); *p != 0; ++p)
    {
        h ^= *p;
        h *= 16777619u;
    }
    return juce::String::toHexString ((int) h);
}

juce::File Session::resolveAudioFileForSharing (const juce::String& originalPath) const
{
    if (! looksLikeAudioPath (originalPath))
        return {};

    auto original = juce::File (originalPath);
    if (original.existsAsFile())
        return original;

    const juce::ScopedLock sl (audioPathsLock);
    auto it = downloadedAudioPaths.find (originalPath);
    if (it != downloadedAudioPaths.end() && it->second.existsAsFile())
        return it->second;

    auto cached = sessionAudioEntries.find (originalPath);
    if (cached != sessionAudioEntries.end() && cached->second.existsAsFile())
        return cached->second;

    return {};
}

juce::File Session::getSessionAudioEntryFile (const juce::String& originalPath, const juce::String& fileName)
{
    if (sessionFolder == juce::File())
        return {};

    juce::File candidate;
    {
        const juce::ScopedLock sl (audioPathsLock);
        auto existing = sessionAudioEntries.find (originalPath);
        if (existing != sessionAudioEntries.end())
            return existing->second;

        auto legalName = juce::File::createLegalFileName (fileName);
        if (legalName.isEmpty())
            legalName = "audio";

        auto destDir = sessionFolder;
        if (isRecordingPath (originalPath))
        {
            destDir = sessionFolder.getChildFile ("Recordings");
            destDir.createDirectory();
        }

        candidate = destDir.getChildFile (legalName);
        if (candidate.exists())
        {
            auto ext = candidate.getFileExtension();
            auto stem = ext.isNotEmpty() ? legalName.dropLastCharacters (ext.length())
                                         : legalName;
            candidate = destDir.getChildFile (stem + " (" + fileHashPrefix (originalPath) + ")" + ext);
        }

        sessionAudioEntries[originalPath] = candidate;
    }
    saveCacheIndex();
    return candidate;
}

void Session::exposeLocalAudioInSessionFolder (const juce::File& file, const juce::String& originalPath)
{
    if (! file.existsAsFile() || sessionFolder == juce::File())
        return;

    sessionFolder.createDirectory();
    auto link = getSessionAudioEntryFile (originalPath, file.getFileName());
    if (link == juce::File())
        return;

    bool needsCreate = true;
    if (link.isSymbolicLink())
    {
        if (file.existsAsFile() && link.getLinkedTarget() == file)
            needsCreate = false;
        else
            link.deleteFile();
    }
    else if (link.existsAsFile())
    {
        needsCreate = false;
    }

    if (needsCreate && ! file.createSymbolicLink (link, true))
    {
       #if JUCE_WINDOWS
        file.createShortcut ("mikeDAW multiplayer session audio",
                             link.withFileExtension (link.getFileExtension() + ".lnk"));
       #endif
    }

    if (onMultiplayerFolderChanged)
        onMultiplayerFolderChanged (sessionFolder);
}

void Session::sendAudioFileTo (Peer* peer, const juce::File& file, const juce::String& originalPath)
{
    if (! file.existsAsFile() || ! looksLikeAudioPath (file.getFullPathName()))
        return;

    OutboundItem item;
    item.isFile = true;
    item.file = file;
    item.originalPath = originalPath;
    if (peer != nullptr)
    {
        item.single = linkFor (peer);
        if (item.single == nullptr)
            return;
    }
    enqueue (std::move (item));
}

void Session::shareAudioFileIfNeeded (const juce::String& path)
{
    if (applyingRemote || ! isActive() || peers.isEmpty() || ! isShareableAudioPath (path))
        return;

    if (isPathHeldPending (path))
        return;

    if (! sharedAudioPaths.insert (path).second)
        return;

    auto file = juce::File (path);
    exposeLocalAudioInSessionFolder (file, path);
    sendToAll (makeAudioFileOfferMsg (path, file.getFileName(), file.getSize()));
}

void Session::sendProjectAudioFilesTo (Peer& peer)
{
    std::set<juce::String> paths;
    std::function<void (juce::ValueTree)> collect = [&] (juce::ValueTree t)
    {
        if (t.hasType (IDs::Channel))
        {
            auto p = t.getProperty (IDs::samplePath).toString();
            if (resolveAudioFileForSharing (p) != juce::File())
                paths.insert (p);
        }
        else if (t.hasType (IDs::Clip) && t.getProperty (IDs::type).toString() == "audio")
        {
            auto p = t.getProperty (IDs::path).toString();
            if (resolveAudioFileForSharing (p) != juce::File())
                paths.insert (p);
        }
        else if (t.hasType (IDs::AudioFile))
        {
            auto p = t.getProperty (IDs::audioPath).toString();
            if (resolveAudioFileForSharing (p) != juce::File())
                paths.insert (p);
        }

        for (int i = 0; i < t.getNumChildren(); ++i)
            collect (t.getChild (i));
    };
    collect (project);

    for (auto& path : paths)
    {
        if (isPathHeldPending (path))
            continue;

        auto file = resolveAudioFileForSharing (path);
        if (file != juce::File())
        {
            if (juce::File (path).existsAsFile())
                exposeLocalAudioInSessionFolder (file, path);
            enqueueTo (&peer, makeAudioFileOfferMsg (path, file.getFileName(), file.getSize()));
        }
    }
}

bool Session::isPathHeldPending (const juce::String& path) const
{
    if (path.isEmpty())
        return false;

    auto playlist = Model::getPlaylist (project);
    for (int i = 0; i < playlist.getNumChildren(); ++i)
    {
        auto clip = playlist.getChild (i);
        if (clip.hasType (IDs::Clip)
            && clip.getProperty (IDs::type).toString() == "audio"
            && clip.getProperty (IDs::path).toString() == path
            && (bool) clip.getProperty (IDs::pending, false))
            return true;
    }
    return false;
}

bool Session::isRecordingPath (const juce::String& originalPath) const
{
    if (originalPath.isEmpty())
        return false;

    auto playlist = Model::getPlaylist (project);
    for (int i = 0; i < playlist.getNumChildren(); ++i)
    {
        auto clip = playlist.getChild (i);
        if (clip.hasType (IDs::Clip)
            && clip.getProperty (IDs::type).toString() == "audio"
            && clip.getProperty (IDs::path).toString() == originalPath
            && clip.hasProperty (IDs::recordedBy))
            return true;
    }
    return false;
}

void Session::rememberDownloadedAudio (const juce::String& originalPath, const juce::File& localFile)
{
    {
        const juce::ScopedLock sl (audioPathsLock);
        downloadedAudioPaths[originalPath] = localFile;
    }
    engine.refreshResolvedSampleReferences (originalPath);
}

void Session::valueTreePropertyChanged (juce::ValueTree& t, const juce::Identifier& p)
{
    if (t.hasType (IDs::Channel) && p == IDs::samplePath)
        shareAudioFileIfNeeded (t.getProperty (IDs::samplePath).toString());
    else if (t.hasType (IDs::Clip) && t.getProperty (IDs::type).toString() == "audio")
    {
        if (p == IDs::path)
        {
            if (! (bool) t.getProperty (IDs::pending, false))
                shareAudioFileIfNeeded (t.getProperty (IDs::path).toString());
        }
        else if (p == IDs::pending && ! applyingRemote && ! (bool) t.getProperty (IDs::pending, false))
            shareAudioFileIfNeeded (t.getProperty (IDs::path).toString());
    }
}

void Session::valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& child)
{
    if (child.hasType (IDs::Channel))
        shareAudioFileIfNeeded (child.getProperty (IDs::samplePath).toString());
    else if (child.hasType (IDs::Clip) && child.getProperty (IDs::type).toString() == "audio")
    {
        if (! (bool) child.getProperty (IDs::pending, false))
            shareAudioFileIfNeeded (child.getProperty (IDs::path).toString());
    }
    else if (child.hasType (IDs::AudioFile))
        shareAudioFileIfNeeded (child.getProperty (IDs::audioPath).toString());
}

void Session::sendToAll (const juce::MemoryBlock& msg, Peer* except)
{
    OutboundItem item;
    item.data = msg;
    item.except = except;
    item.hasExcept = (except != nullptr);
    enqueue (std::move (item));
}

void Session::sendSnapshotTo (Peer& peer)
{
    engine.capturePluginStatesToTree();
    dedupeChannelSeqs();
    Model::ensureIdentities (project);

    juce::MemoryOutputStream out;
    out.writeByte ((char) msgSnapshot);
    out.writeString (localDisplayName);
    out.writeString (project.toXmlString());
    enqueueTo (&peer, out.getMemoryBlock());
    sendProjectAudioFilesTo (peer);
}

juce::MemoryBlock Session::buildProjectResetBlock()
{
    engine.capturePluginStatesToTree();
    Model::ensureIdentities (project);
    juce::MemoryOutputStream out;
    out.writeByte ((char) msgProjectReset);
    out.writeString (project.toXmlString());
    return out.getMemoryBlock();
}

void Session::sendProjectResetTo (Peer& peer, const juce::MemoryBlock& block)
{
    peer.lastServedResyncMs = juce::Time::currentTimeMillis();
    enqueueTo (&peer, block);
}

void Session::resetProjectForPeers()
{
    if (role != Role::host || peers.isEmpty())
        return;
    const auto block = buildProjectResetBlock();
    for (auto* p : peers)
    {
        sendProjectResetTo (*p, block);
        sendProjectAudioFilesTo (*p);
    }
}

void Session::replaceProject (const juce::ValueTree& incoming)
{
    {
        const juce::ScopedValueSetter<bool> svs (applyingRemote, true);
        const ScopedOptionalFlag remoteFlag (remoteApplyFlag);
        project.copyPropertiesAndChildrenFrom (incoming, nullptr);
    }
    resetProjectForPeers();
}

void Session::registerLink (Peer* peer)
{
    if (peer == nullptr)
        return;
    auto link = std::make_shared<PeerLink>();
    link->peer.store (peer);
    const juce::ScopedLock sl (linksLock);
    peerLinks[peer] = std::move (link);
}

void Session::unregisterLink (Peer* peer)
{
    std::shared_ptr<PeerLink> link;
    {
        const juce::ScopedLock sl (linksLock);
        auto it = peerLinks.find (peer);
        if (it == peerLinks.end())
            return;
        link = it->second;
        peerLinks.erase (it);
    }
    const juce::ScopedLock sl (link->sendLock);
    link->peer.store (nullptr);
}

std::shared_ptr<Session::PeerLink> Session::linkFor (Peer* peer)
{
    const juce::ScopedLock sl (linksLock);
    auto it = peerLinks.find (peer);
    return it != peerLinks.end() ? it->second : nullptr;
}

void Session::enqueue (OutboundItem&& item)
{
    if (senderThread == nullptr)
        return;
    {
        const juce::ScopedLock sl (queueLock);
        outQueue.push_back (std::move (item));
    }
    queueSignal.signal();
}

void Session::enqueueTo (Peer* peer, juce::MemoryBlock data)
{
    auto link = linkFor (peer);
    if (link == nullptr)
        return;
    OutboundItem item;
    item.data = std::move (data);
    item.single = std::move (link);
    enqueue (std::move (item));
}

void Session::sendVia (const std::shared_ptr<PeerLink>& link, const juce::MemoryBlock& data)
{
    const juce::ScopedLock sl (link->sendLock);
    if (auto* p = link->peer.load())
        p->sendMessage (data);
}

void Session::runSender()
{
    while (! senderThread->threadShouldExit())
    {
        OutboundItem item;
        bool have = false;
        {
            const juce::ScopedLock sl (queueLock);
            if (! outQueue.empty())
            {
                item = std::move (outQueue.front());
                outQueue.pop_front();
                have = true;
            }
        }

        if (! have)
        {
            queueSignal.wait (-1);
            continue;
        }

        if (item.isFile)
        {
            juce::MemoryBlock fileData;
            if (! item.file.existsAsFile() || ! item.file.loadFileAsData (fileData) || fileData.getSize() == 0)
                continue;

            juce::MemoryOutputStream out;
            out.writeByte ((char) msgAudioFile);
            out.writeString (item.originalPath);
            out.writeString (item.file.getFileName());
            out.writeInt64 ((juce::int64) fileData.getSize());
            out.write (fileData.getData(), fileData.getSize());
            item.data = out.getMemoryBlock();
        }

        if (item.data.getSize() == 0)
            continue;

        if (item.single != nullptr)
        {
            sendVia (item.single, item.data);
            continue;
        }

        std::vector<std::shared_ptr<PeerLink>> snapshot;
        {
            const juce::ScopedLock sl (linksLock);
            snapshot.reserve (peerLinks.size());
            for (auto& kv : peerLinks)
                snapshot.push_back (kv.second);
        }
        for (auto& link : snapshot)
        {
            if (item.hasExcept && link->peer.load() == item.except)
                continue;
            sendVia (link, item.data);
        }
    }
}

void Session::applySnapshot (const juce::String& xml)
{
    deltasAppliedSinceSnapshot = 0;
    unresolvedDropCount = 0;
    unresolvedWindowStartMs = 0;
    resyncRequestsUnanswered = 0;

    if (auto parsed = juce::parseXML (xml))
    {
        auto incoming = juce::ValueTree::fromXml (*parsed);
        if (incoming.hasType (IDs::Project))
        {
            const juce::ScopedValueSetter<bool> svs (applyingRemote, true);
            const ScopedOptionalFlag remoteFlag (remoteApplyFlag);
            project.copyPropertiesAndChildrenFrom (incoming, nullptr);
        }
    }

    dedupeChannelSeqs();
}

void Session::dedupeChannelSeqs()
{
    auto patterns = project.getChildWithName (IDs::Patterns);
    if (! patterns.isValid())
        return;

    const ScopedOptionalFlag remoteFlag (remoteApplyFlag);
    for (int i = 0; i < patterns.getNumChildren(); ++i)
    {
        auto pattern = patterns.getChild (i);
        if (pattern.hasType (IDs::Pattern))
            Model::mergeDuplicateChannelSeqs (pattern, nullptr);
    }
}

void Session::disconnectAsync()
{
    juce::WeakReference<Session> weak (this);
    juce::MessageManager::callAsync ([weak] { if (auto* self = weak.get()) self->disconnect(); });
}

void Session::noteUnresolvedDelta (const Model::TreePath& path, bool isRemoveChild, Peer* from)
{
    const auto now = juce::Time::currentTimeMillis();
    if (unresolvedWindowStartMs == 0 || now - unresolvedWindowStartMs > resyncDropWindowMs)
    {
        unresolvedWindowStartMs = now;
        unresolvedDropCount = 0;
    }
    ++unresolvedDropCount;

    const int failingDepth = path.resolvedDepth (project) + 1;
    const bool pathItselfMissing = failingDepth <= (int) path.steps.size();
    const bool containerLevel = pathItselfMissing && ! isRemoveChild
                             && (failingDepth == 2 || failingDepth == 3);

    if (! containerLevel && unresolvedDropCount < resyncDropThreshold)
        return;

    if (role == Role::host)
    {
        if (from == nullptr || ! peers.contains (from))
            return;
        if (now - from->lastServedResyncMs < resyncServeMinIntervalMs)
            return;
        unresolvedDropCount = 0;
        synclog ("resync SERVE peer=" + from->displayName + " - its delta did not resolve here: " + path.toString());
        sendProjectResetTo (*from, buildProjectResetBlock());
        return;
    }

    requestResync ("Lost track of " + path.toString());
}

void Session::requestResync (const juce::String& reason)
{
    if (role != Role::client || peers.isEmpty() || resyncGaveUp)
        return;

    const auto now = juce::Time::currentTimeMillis();
    if (now - lastResyncMs < resyncMinIntervalMs)
        return;
    lastResyncMs = now;
    unresolvedDropCount = 0;

    if (++resyncRequestsUnanswered > resyncMaxRequests)
    {
        resyncGaveUp = true;
        synclog ("resync GIVE-UP after " + juce::String (resyncMaxRequests) + " unanswered requests");
        if (onResyncing)
            onResyncing ("Could not resync - rejoin the session");
        return;
    }

    synclog ("resync REQ   " + reason);
    juce::MemoryOutputStream out;
    out.writeByte ((char) msgResyncRequest);
    enqueueTo (peers.getFirst(), out.getMemoryBlock());

    if (onResyncing)
        onResyncing (reason);
}

void Session::handleMessage (Peer& from, const juce::MemoryBlock& msg)
{
    if (msg.getSize() < 1)
        return;

    from.lastRecvMs = juce::Time::currentTimeMillis();

    juce::MemoryInputStream in (msg, false);
    auto type = (MsgType) (juce::uint8) in.readByte();

    switch (type)
    {
        case msgHello:
        {
            if (role == Role::host)
            {
                from.peerId = in.readInt64();
                auto name = in.readString();
                from.displayName = name.isNotEmpty() ? name : juce::String ("A Kind Soul");

                const int joinerVersion = in.getNumBytesRemaining() >= 4 ? in.readInt() : 1;
                if (joinerVersion != protocolVersion)
                {
                    juce::MemoryOutputStream out;
                    out.writeByte ((char) msgRejected);
                    out.writeString (juce::String (joinerVersion < protocolVersion
                                         ? "Your mikeDAW build is older than the host's - update to join"
                                         : "The host's mikeDAW build is older than yours - they need to update"));
                    enqueueTo (&from, out.getMemoryBlock());
                    from.peerId = 0;
                    from.displayName = {};
                    juce::WeakReference<Session> weak (this);
                    Peer* rejected = &from;
                    juce::Timer::callAfterDelay (1500, [weak, rejected]
                    {
                        if (auto* self = weak.get())
                            if (self->peers.contains (rejected))
                                self->removePeer (rejected);
                    });
                    break;
                }

                dropStalePeerFor (from);

                {
                    int liveClients = 0;
                    for (auto* p : peers)
                    {
                        if (p->peerId == 0)
                            continue;
                        if (p != &from && p->displayName == from.displayName)
                            continue;
                        ++liveClients;
                    }
                    if (liveClients > maxPeople - 1)
                    {
                        from.disconnect();
                        break;
                    }
                }

                std::set<juce::int64> skipIds { from.peerId };
                for (auto& [id, s] : remoteStates)
                    if (id != from.peerId && s.name == from.displayName)
                        skipIds.insert (id);

                sendSnapshotTo (from);

                if (! from.announced)
                {
                    from.announced = true;
                    announcePresence (0, from.displayName, from.peerId, &from);
                }
                updateStatus();

                broadcastUserState (localVolume, localDeafen, localAfk);
                for (auto& [id, s] : remoteStates)
                {
                    if (skipIds.count (id) > 0)
                        continue;
                    juce::MemoryOutputStream out;
                    out.writeByte ((char) msgUserState);
                    out.writeInt64 (s.peerId);
                    out.writeString (s.name);
                    out.writeFloat (s.volume);
                    out.writeByte ((char) (s.deafen ? 1 : 0));
                    out.writeByte ((char) (s.afk ? 1 : 0));
                    enqueueTo (&from, out.getMemoryBlock());
                }

                if (localAvatarBytes.getSize() > 0)
                    sendAvatar (localPeerId, localAvatarBytes, &from);
                for (auto& [id, bytes] : peerAvatarBytes)
                    if (skipIds.count (id) == 0)
                        sendAvatar (id, bytes, &from);

                sendInventoryTo (&from);
                for (auto& [id, inv] : peerInventories)
                    if (skipIds.count (id) == 0)
                        sendStoredInventoryTo (&from, id, inv);
            }
            break;
        }

        case msgSnapshot:
        {
            hostDisplayName = in.readString();
            applySnapshot (in.readString());
            engine.refreshResolvedSampleReferences();
            updateStatus();
            if (onPeerJoined)
                onPeerJoined (hostDisplayName.isNotEmpty() ? hostDisplayName : "the host");
            broadcastUserState (localVolume, localDeafen, localAfk);
            if (localAvatarBytes.getSize() > 0)
                sendAvatar (localPeerId, localAvatarBytes, nullptr);
            if (onSnapshotApplied)
                onSnapshotApplied();
            break;
        }

        case msgPresence:
        {
            auto kind = in.readInt();
            auto name = in.readString();
            juce::int64 goneId = (in.getNumBytesRemaining() >= 8) ? in.readInt64() : 0;
            if (kind == 0 && onPeerJoined) onPeerJoined (name);
            else if (kind == 1)
            {
                if (onPeerLeft) onPeerLeft (name);
                if (goneId != 0)
                {
                    bool rosterChanged = (remoteStates.erase (goneId) > 0);
                    peerAvatarBytes.erase (goneId);
                    peerAvatars.erase (goneId);
                    if (peerInventories.count (goneId) > 0)
                    {
                        dropPeerInventory (goneId);
                        rosterChanged = true;
                    }
                    if (rosterChanged && onRosterChanged)
                        onRosterChanged();
                }
            }
            break;
        }

        case msgDelta:
        {
            auto remaining = (size_t) in.getNumBytesRemaining();
            juce::MemoryBlock delta;
            delta.setSize (remaining);
            in.read (delta.getData(), (int) remaining);

            bool selfEject = false;
            bool unresolved = false;
            {
                const juce::ScopedValueSetter<bool> svs (applyingRemote, true);
                const ScopedOptionalFlag remoteFlag (remoteApplyFlag);
                juce::String why;
                if (TreeSync::applyChange (project, delta.getData(), delta.getSize(), &why))
                {
                    ++deltasAppliedSinceSnapshot;
                }
                else
                {
                    synclog ("delta DROP   " + why);
                    unresolved = why.startsWith ("unresolved");
                    const bool wrongFormat = ! TreeSync::looksLikeDelta (delta.getData(), delta.getSize());
                    selfEject = (role == Role::client && wrongFormat && deltasAppliedSinceSnapshot == 0);
                }
            }

            if (selfEject)
            {
                kickedByHost = true;
                if (onJoinRejected)
                    onJoinRejected ("The host's mikeDAW build is older than yours - they need to update");
                disconnectAsync();
                break;
            }

            if (unresolved)
            {
                Model::TreePath path;
                juce::MemoryInputStream ds (delta.getData(), delta.getSize(), false);
                ds.skipNextBytes (1);
                const auto op = (juce::uint8) ds.readByte();
                if (path.read (ds))
                    noteUnresolvedDelta (path, op == TreeSync::removeChildOp, &from);
            }

            if (role == Role::host)
                sendToAll (msg, &from);

            dedupePending = true;
            triggerAsyncUpdate();
            break;
        }

        case msgParam:
        {
            auto key = in.readString();
            auto idx = in.readInt();
            auto value = in.readFloat();
            engine.setRemoteParam (key, idx, value);
            stampRemoteApply (key);

            if (auto* proc = engine.getProcessorFor (key))
                synclog ("param RECV   key=" + key + " idx=" + juce::String (idx)
                         + " value=" + juce::String (value, 6)
                         + " -> applied (proc has " + juce::String (proc->getParameters().size())
                         + " params)");
            else
                synclog ("param RECV   key=" + key + " idx=" + juce::String (idx)
                         + " value=" + juce::String (value, 6) + " -> NO PROCESSOR for key");

            if (role == Role::host)
                sendToAll (msg, &from);
            break;
        }

        case msgPluginState:
        {
            auto key = in.readString();
            auto remaining = (size_t) in.getNumBytesRemaining();
            juce::MemoryBlock blob;
            blob.setSize (remaining);
            in.read (blob.getData(), (int) remaining);

            latestRemoteState[key] = blob;
            stampRemoteApply (key);
            lastStateHash[key] = hashBlobForSession (blob);

            auto& slot = deferredStateApplies[key];
            if (slot == nullptr)
                slot = std::make_unique<DeferredStateApply> (*this, key);
            slot->blob = blob;
            slot->startTimer (stateApplyDeferMs);

            logState ("received state for " + key + " -> "
                      + juce::String ((int) blob.getSize()) + " bytes, deferred");
            synclog ("state DEFER  key=" + key + " bytes=" + juce::String ((int) blob.getSize())
                     + " sum=" + juce::String::toHexString ((int) SyncDebug::sum (blob.getData(), blob.getSize())));

            if (role == Role::host)
                sendToAll (msg, &from);
            break;
        }

        case msgTransport:
        {
            auto cmd = in.readInt();
            auto pos = in.readDouble();

            {
                const ScopedAtomicFlag noEcho (engine.suppressTransportBroadcast);
                if (cmd == 1)       { engine.seekTicks (pos, true); engine.play(); }
                else if (cmd == 0)  engine.stopAt (pos);
                else if (cmd == 2)  engine.seekTicks (pos);
                else if (cmd == 3)  engine.pause();
            }

            if (role == Role::host)
                sendToAll (msg, &from);
            break;
        }

        case msgCursor:
        {
            auto peerId = in.readInt64();
            auto name = in.readString();
            auto nx = in.readFloat();
            auto ny = in.readFloat();

            if (onPeerCursor)
                onPeerCursor (peerId, name, nx, ny);

            if (role == Role::host)
                sendToAll (msg, &from);
            break;
        }

        case msgUserState:
        {
            UserState s;
            s.peerId = in.readInt64();
            s.name   = in.readString();
            s.volume = in.readFloat();
            s.deafen = in.readByte() != 0;
            s.afk    = in.readByte() != 0;

            if (s.peerId != localPeerId)
            {
                for (auto it = remoteStates.begin(); it != remoteStates.end(); )
                {
                    if (it->first != s.peerId && it->second.name == s.name)
                    {
                        peerAvatarBytes.erase (it->first);
                        peerAvatars.erase (it->first);
                        it = remoteStates.erase (it);
                    }
                    else
                        ++it;
                }
                remoteStates[s.peerId] = s;
                if (onRosterChanged)
                    onRosterChanged();
            }

            if (role == Role::host)
                sendToAll (msg, &from);
            break;
        }

        case msgProjectSaved:
        {
            auto saverName = in.readString();
            auto projectName = in.readString();
            if (onProjectSavedByPeer)
                onProjectSavedByPeer (saverName, projectName);

            if (role == Role::host)
                sendToAll (msg, &from);
            break;
        }

        case msgRendered:
        {
            auto renderName = in.readString();
            auto fileName = in.readString();
            if (onRenderedByPeer)
                onRenderedByPeer (renderName, fileName);

            if (role == Role::host)
                sendToAll (msg, &from);
            break;
        }

        case msgRenderLock:
        {
            auto byName = in.readString();
            bool locked = in.readByte() != 0;
            renderLockHeld  = locked;
            renderLockOwner = locked ? byName : juce::String();
            if (onRenderLockChanged)
                onRenderLockChanged (byName, locked);

            if (role == Role::host)
                sendToAll (msg, &from);
            break;
        }

        case msgSoloChanged:
        {
            auto byName     = in.readString();
            auto targetName = in.readString();
            bool on = in.readByte() != 0;
            if (onSoloChangedByPeer)
                onSoloChangedByPeer (byName, targetName, on);

            if (role == Role::host)
                sendToAll (msg, &from);
            break;
        }

        case msgSessionEnded:
        {
            auto hostName = in.readString();
            sessionEndedByHost = true;
            if (onSessionEnded)
                onSessionEnded (hostName.isNotEmpty() ? hostName : hostDisplayName);
            disconnectAsync();
            break;
        }

        case msgRejected:
        {
            auto reason = in.readString();
            if (role == Role::client)
            {
                kickedByHost = true;
                if (onJoinRejected)
                    onJoinRejected (reason);
                disconnectAsync();
            }
            break;
        }

        case msgKick:
        {
            auto hostName = in.readString();
            kickedByHost = true;
            if (onKicked)
                onKicked (hostName.isNotEmpty() ? hostName : hostDisplayName);
            disconnectAsync();
            break;
        }

        case msgVstAdded:
        {
            auto senderName = in.readString();
            auto pluginKey  = in.readString();
            auto pluginName = in.readString();
            if (onVstAddedByPeer)
                onVstAddedByPeer (senderName, pluginKey, pluginName);

            if (role == Role::host)
                sendToAll (msg, &from);
            break;
        }

        case msgAudition:
        {
            auto peerId = in.readInt64();
            auto channelId = in.readInt();
            bool on = in.readByte() != 0;
            int note = (juce::uint8) in.readByte();
            float velocity = in.readFloat();

            if (peerId != localPeerId && onPeerAudition)
                onPeerAudition (peerId, channelId, on, note, velocity);

            if (role == Role::host)
                sendToAll (msg, &from);
            break;
        }

        case msgViewport:
        {
            auto peerId = in.readInt64();
            auto name = in.readString();
            auto panelName = in.readString();
            auto patternId = in.readInt();

            if (peerId != localPeerId && onPeerViewport)
                onPeerViewport (peerId, name, panelName, patternId);

            if (role == Role::host)
                sendToAll (msg, &from);
            break;
        }

        case msgPing:
            break;

        case msgKeepalive:
            from.peerSendsKeepalives = true;
            break;

        case msgResyncRequest:
        {
            if (role != Role::host)
                break;
            if (juce::Time::currentTimeMillis() - from.lastServedResyncMs < resyncServeMinIntervalMs)
                break;
            synclog ("resync SERVE peer=" + from.displayName);
            sendProjectResetTo (from, buildProjectResetBlock());
            break;
        }

        case msgProjectReset:
        {
            juce::String projectXml;
            projectXml = in.readString();
            if (role == Role::host)
            {
                juce::MemoryOutputStream relay;
                relay.writeByte ((char) msgProjectReset);
                relay.writeString (projectXml);
                sendToAll (relay.getMemoryBlock(), &from);
            }
            else
            {
                applySnapshot (projectXml);
                engine.refreshResolvedSampleReferences();
                updateStatus();
                if (onSnapshotApplied)
                    onSnapshotApplied();
            }
            break;
        }

        case msgReaction:
        {
            auto peerId = in.readInt64();
            auto name = in.readString();
            auto emoji = in.readString();
            auto nx = in.readFloat();
            auto ny = in.readFloat();

            if (peerId != localPeerId && onPeerReaction)
                onPeerReaction (peerId, name, emoji, nx, ny);

            if (role == Role::host)
                sendToAll (msg, &from);
            break;
        }

        case msgChatText:
        {
            auto peerId = in.readInt64();
            auto name = in.readString();
            auto text = in.readString();

            if (peerId != localPeerId && onChatText)
                onChatText (peerId, name, text);

            if (role == Role::host)
                sendToAll (msg, &from);
            break;
        }

        case msgChatImageAnnounce:
        {
            auto peerId = in.readInt64();
            auto name = in.readString();
            auto imageId = in.readInt64();

            if (peerId != localPeerId && onChatImageAnnounce)
                onChatImageAnnounce (peerId, name, imageId);

            if (role == Role::host)
                sendToAll (msg, &from);
            break;
        }

        case msgChatImageData:
        {
            auto imageId = in.readInt64();
            auto size = in.readInt64();
            auto remaining = (size_t) in.getNumBytesRemaining();
            if (size <= 0 || size > chatImageMaxBytes || (size_t) size != remaining)
                break;

            juce::MemoryBlock bytes;
            bytes.setSize (remaining);
            in.read (bytes.getData(), (int) remaining);

            auto img = juce::ImageFileFormat::loadFrom (bytes.getData(), bytes.getSize());
            if (img.isValid() && onChatImageData)
                onChatImageData (imageId, img, bytes);

            if (role == Role::host)
                sendToAll (msg, &from);
            break;
        }

        case msgAvatar:
        {
            auto peerId = in.readInt64();
            auto size = in.readInt64();
            auto remaining = (size_t) in.getNumBytesRemaining();
            if (size <= 0 || size > avatarMaxBytes || (size_t) size != remaining)
                break;

            juce::MemoryBlock bytes;
            bytes.setSize (remaining);
            in.read (bytes.getData(), (int) remaining);

            if (peerId != localPeerId)
            {
                auto img = juce::ImageFileFormat::loadFrom (bytes.getData(), bytes.getSize());
                if (img.isValid())
                {
                    peerAvatarBytes[peerId] = bytes;
                    peerAvatars[peerId] = img;
                    if (onAvatarChanged)
                        onAvatarChanged (peerId, bytes);
                    if (onRosterChanged)
                        onRosterChanged();
                }
            }

            if (role == Role::host)
                sendToAll (msg, &from);
            break;
        }

        case msgPluginInventory:
        {
            auto peerId = in.readInt64();
            const bool removed = in.readByte() != 0;

            if (removed)
            {
                if (peerId != localPeerId)
                    dropPeerInventory (peerId);
            }
            else
            {
                int count = in.readInt();
                count = juce::jlimit (0, 100000, count);
                std::vector<PluginInventoryItem> inv;
                inv.reserve ((size_t) count);
                for (int i = 0; i < count && in.getNumBytesRemaining() > 0; ++i)
                {
                    PluginInventoryItem it;
                    it.format        = in.readString();
                    it.uid           = in.readInt();
                    it.deprecatedUid = in.readInt();
                    it.name          = in.readString();
                    it.fileName      = in.readString();
                    it.isInstrument  = in.readByte() != 0;
                    inv.push_back (std::move (it));
                }

                if (peerId != localPeerId)
                {
                    peerInventories[peerId] = std::move (inv);
                    recomputeCompatibility();
                    if (onCompatibilityChanged)
                        onCompatibilityChanged();
                }
            }

            if (role == Role::host)
                sendToAll (msg, &from);
            break;
        }

        case msgAudioFile:
        {
            auto originalPath = in.readString();
            auto fileName = juce::File::createLegalFileName (in.readString());
            auto expectedSize = in.readInt64();
            auto remaining = (size_t) in.getNumBytesRemaining();
            if (fileName.isEmpty() || expectedSize <= 0 || remaining != (size_t) expectedSize
                || sessionFolder == juce::File())
                break;

            if (onAudioDownloadStarted)
                onAudioDownloadStarted (fileName);

            sessionFolder.createDirectory();
            auto dest = getSessionAudioEntryFile (originalPath, fileName);
            if (dest == juce::File())
                break;

            auto data = std::make_shared<juce::MemoryBlock>();
            data->setSize (remaining);
            in.read (data->getData(), (int) remaining);

            if (role == Role::host)
                sendToAll (msg, &from);

            juce::WeakReference<Session> weak (this);
            audioWritePool.addJob ([weak, data, dest, originalPath, fileName]
            {
                const bool ok = dest.replaceWithData (data->getData(), data->getSize())
                                && dest.existsAsFile();
                juce::MessageManager::callAsync ([weak, ok, dest, originalPath, fileName]
                {
                    if (weak == nullptr)
                        return;
                    auto* self = weak.get();
                    if (ok)
                    {
                        self->rememberDownloadedAudio (originalPath, dest);
                        if (self->onAudioDownloadFinished)
                            self->onAudioDownloadFinished (fileName);
                        if (self->onMultiplayerFolderChanged)
                            self->onMultiplayerFolderChanged (self->sessionFolder);
                    }
                });
            });
            break;
        }

        case msgAudioFileOffer:
        {
            auto originalPath = in.readString();
            auto fileName = in.readString();
            auto offeredSize = in.readInt64();

            if (role == Role::host)
                sendToAll (msg, &from);

            if (originalPath.isEmpty())
                break;

            juce::File localFile;
            bool isOriginal = false;

            auto original = juce::File (originalPath);
            if (original.existsAsFile())
            {
                localFile = original;
                isOriginal = true;
            }
            else
            {
                const juce::ScopedLock sl (audioPathsLock);
                auto matches = [offeredSize] (const juce::File& f)
                {
                    return f.existsAsFile() && f.getSize() == offeredSize;
                };
                auto dit = downloadedAudioPaths.find (originalPath);
                if (dit != downloadedAudioPaths.end() && matches (dit->second))
                    localFile = dit->second;
                else
                {
                    auto cit = sessionAudioEntries.find (originalPath);
                    if (cit != sessionAudioEntries.end() && matches (cit->second))
                        localFile = cit->second;
                }
            }

            if (localFile != juce::File())
            {
                rememberDownloadedAudio (originalPath, localFile);
                if (isOriginal)
                    exposeLocalAudioInSessionFolder (localFile, originalPath);
                if (onMultiplayerFolderChanged)
                    onMultiplayerFolderChanged (sessionFolder);
                break;
            }

            juce::MemoryOutputStream out;
            out.writeByte ((char) msgAudioFileRequest);
            out.writeString (originalPath);
            enqueueTo (&from, out.getMemoryBlock());
            break;
        }

        case msgAudioFileRequest:
        {
            auto originalPath = in.readString();
            if (originalPath.isEmpty())
                break;

            auto file = resolveAudioFileForSharing (originalPath);
            if (file != juce::File())
                sendAudioFileTo (&from, file, originalPath);
            else if (role == Role::host)
                sendToAll (msg, &from);
            break;
        }

        default:
            break;
    }
}

juce::MemoryBlock Session::makeParamMsg (const juce::String& key, int idx, float value)
{
    juce::MemoryOutputStream out;
    out.writeByte ((char) msgParam);
    out.writeString (key);
    out.writeInt (idx);
    out.writeFloat (value);
    return out.getMemoryBlock();
}

juce::MemoryBlock Session::makeAudioFileOfferMsg (const juce::String& originalPath,
                                                  const juce::String& fileName,
                                                  juce::int64 fileSize)
{
    juce::MemoryOutputStream out;
    out.writeByte ((char) msgAudioFileOffer);
    out.writeString (originalPath);
    out.writeString (fileName);
    out.writeInt64 (fileSize);
    return out.getMemoryBlock();
}

void Session::broadcastParam (const juce::String& key, int paramIndex, float value)
{
    if (! isActive())
        return;

    {
        const juce::ScopedLock sl (paramQueueLock);
        for (auto& p : paramQueue)
            if (p.key == key && p.idx == paramIndex)
            {
                p.value = value;
                triggerAsyncUpdate();
                return;
            }
        paramQueue.push_back ({ key, paramIndex, value });
    }
    triggerAsyncUpdate();
}

void Session::handleAsyncUpdate()
{
    std::vector<PendingParam> toSend;
    {
        const juce::ScopedLock sl (paramQueueLock);
        toSend.swap (paramQueue);
    }

    static constexpr int paramBurstThreshold = 8;
    std::map<juce::String, std::set<int>> distinctByKey;
    for (auto& p : toSend)
        distinctByKey[p.key].insert (p.idx);

    for (auto& p : toSend)
    {
        if ((int) distinctByKey[p.key].size() >= paramBurstThreshold)
        {
            synclog ("param BURST-DROP key=" + p.key + " idx=" + juce::String (p.idx)
                     + " (n=" + juce::String ((int) distinctByKey[p.key].size()) + " in batch)");
            continue;
        }
        if (inEchoCooldown (p.key))
        {
            synclog ("param DROP (echo cooldown) key=" + p.key + " idx=" + juce::String (p.idx)
                     + " value=" + juce::String (p.value, 6));
            continue;
        }
        sendToAll (makeParamMsg (p.key, p.idx, p.value));
        synclog ("param SENT   key=" + p.key + " idx=" + juce::String (p.idx)
                 + " value=" + juce::String (p.value, 6)
                 + " -> " + juce::String (peers.size()) + " peer(s)");
    }

    if (dedupePending.exchange (false))
        dedupeChannelSeqs();
}

void Session::timerCallback()
{
    if (! isActive() || peers.isEmpty())
        return;

    ++pollTick;
    tickCaptureMs = 0.0;

    if (pollTick % 8 == 0)
    {
        juce::MemoryOutputStream out;
        out.writeByte ((char) msgKeepalive);
        auto mb = out.getMemoryBlock();
        for (auto* p : peers)
            enqueueTo (p, mb);
    }

    {
        const juce::int64 now = juce::Time::currentTimeMillis();
        std::vector<Peer*> stale;
        for (auto* p : peers)
            if (p->peerSendsKeepalives && p->lastRecvMs != 0 && now - p->lastRecvMs > 60000)
                stale.push_back (p);

        for (auto* p : stale)
            p->disconnect();
    }

    juce::StringArray background;
    for (auto& key : engine.getAllPluginKeys())
    {
        if (isPluginWindowOpen && isPluginWindowOpen (key))
        {
            const auto costIt = captureCostMs.find (key);
            const double cost = costIt != captureCostMs.end() ? costIt->second : 0.0;
            const int period = juce::jlimit (1, 16, (int) std::ceil (cost / 2.0));
            const auto polledIt = lastPolledTick.find (key);
            if (polledIt == lastPolledTick.end() || (pollTick - polledIt->second) >= period)
            {
                lastPolledTick[key] = pollTick;
                pollPluginState (key, true);
            }
        }
        else
            background.add (key);
    }

    const int n = background.size();
    if (n > 0)
    {
        const int budget = juce::jmin (n, maxBackgroundPollsPerTick);
        for (int i = 0; i < budget; ++i)
        {
            if (tickCaptureMs > backgroundTickBudgetMs)
                break;
            if (backgroundPollCursor >= n)
                backgroundPollCursor = 0;
            pollPluginState (background[backgroundPollCursor], false);
            ++backgroundPollCursor;
        }
    }
}

void Session::pollPluginState (const juce::String& key, bool windowOpen)
{
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    auto blob = engine.capturePluginState (key);
    const double cost = juce::Time::getMillisecondCounterHiRes() - t0;
    tickCaptureMs += cost;
    double& smoothed = captureCostMs[key];
    smoothed = smoothed > 0.0 ? 0.5 * smoothed + 0.5 * cost : cost;

    if (blob.getSize() == 0)
        return;

    auto h = hashBlobForSession (blob);
    auto it = lastStateHash.find (key);
    if (it != lastStateHash.end() && it->second == h)
    {
        pendingState.erase (key);
        return;
    }

        if (inEchoCooldown (key))
        {
            lastStateHash[key] = h;
            pendingState.erase (key);
            synclog ("state RESYNC-HASH (remote apply) key=" + key + " bytes="
                     + juce::String ((int) blob.getSize())
                     + " sum=" + juce::String::toHexString ((int) SyncDebug::sum (blob.getData(), blob.getSize())));
            return;
        }

        const bool firstSeen = (it == lastStateHash.end());

        if (firstSeen)
        {
            lastStateHash[key] = h;
            return;
        }

        if (! localInteractionSinceRemoteApply (key))
        {
            lastStateHash[key] = h;
            pendingState.erase (key);
            synclog ("state ABSORB (no local interaction) key=" + key + " sum="
                     + juce::String::toHexString ((int) SyncDebug::sum (blob.getData(), blob.getSize())));
            return;
        }

        auto& pending = pendingState[key];
        const bool sameAsLastTick = (pending.hash == h && pending.totalTicks > 0);
        ++pending.totalTicks;
        const bool forced = pending.totalTicks >= pendingStableMaxTicks;

        if (! sameAsLastTick && ! forced)
        {
            pending.hash = h;
            synclog ("state PENDING (unstable) key=" + key + " sum="
                     + juce::String::toHexString ((int) SyncDebug::sum (blob.getData(), blob.getSize())));
            return;
        }

        lastStateHash[key] = h;
        pendingState.erase (key);
        if (forced)
            synclog ("state BROADCAST (unstable, forced) key=" + key + " bytes="
                     + juce::String ((int) blob.getSize())
                     + " sum=" + juce::String::toHexString ((int) SyncDebug::sum (blob.getData(), blob.getSize())));

        juce::MemoryOutputStream out;
        out.writeByte ((char) msgPluginState);
        out.writeString (key);
        out.write (blob.getData(), blob.getSize());
        sendToAll (out.getMemoryBlock());

        logState ("poll: state changed for " + key + " -> broadcasting "
                  + juce::String ((int) blob.getSize()) + " bytes to "
                  + juce::String (peers.size()) + " peer(s)"
                  + (windowOpen ? " (editor open)" : ""));
        synclog ("state BROADCAST key=" + key + " bytes=" + juce::String ((int) blob.getSize())
                 + " sum=" + juce::String::toHexString ((int) SyncDebug::sum (blob.getData(), blob.getSize()))
                 + " -> " + juce::String (peers.size()) + " peer(s)  (poll"
                 + (windowOpen ? ", editor open)" : ")"));
}

void Session::applyDeferredState (const juce::String& key)
{
    auto it = deferredStateApplies.find (key);
    if (it == deferredStateApplies.end() || it->second == nullptr)
        return;

    auto entry = std::move (it->second);
    deferredStateApplies.erase (it);
    entry->stopTimer();
    auto blob = entry->blob;
    retiredStateApplies.push_back (std::move (entry));
    juce::WeakReference<Session> weak (this);
    juce::MessageManager::callAsync ([weak] { if (auto* self = weak.get()) self->retiredStateApplies.clear(); });

    if (blob.getSize() == 0)
        return;

    auto h = hashBlobForSession (blob);
    auto last = lastAppliedStateHash.find (key);
    if (last != lastAppliedStateHash.end() && last->second == h)
    {
        synclog ("state APPLY (coalesced) key=" + key + " SKIPPED (same as last applied, bytes="
                 + juce::String ((int) blob.getSize()) + ")");
        return;
    }

    const bool haveInstance = engine.getProcessorFor (key) != nullptr;
    stampRemoteApply (key);
    engine.applyPluginStateBlob (key, blob.getData(), (int) blob.getSize());
    if (haveInstance)
        lastAppliedStateHash[key] = h;
    auto* instAfter = engine.getProcessorFor (key);

    logState ("applied deferred state for " + key + " -> "
              + juce::String ((int) blob.getSize()) + " bytes, "
              + (haveInstance ? "applied" : "cached (instance not loaded yet)"));
    synclog ("state RECV   key=" + key + " bytes=" + juce::String ((int) blob.getSize())
             + " sum=" + juce::String::toHexString ((int) SyncDebug::sum (blob.getData(), blob.getSize()))
             + " -> " + (instAfter != nullptr
                    ? "setStateInformation called (coalesced, proc has "
                      + juce::String (instAfter->getParameters().size()) + " params)"
                    : "NO instance yet, cached for late load"));

    auto reHashed = engine.capturePluginState (key);
    if (reHashed.getSize() > 0)
        lastStateHash[key] = hashBlobForSession (reHashed);
}

void Session::applyCachedStateOnCreate (const juce::String& key)
{
    if (! isActive())
        return;

    auto it = latestRemoteState.find (key);
    if (it == latestRemoteState.end() || it->second.getSize() == 0)
        return;

    engine.applyPluginStateBlob (key, it->second.getData(), (int) it->second.getSize());
    logState ("applied cached state to newly-loaded " + key + " -> "
              + juce::String ((int) it->second.getSize()) + " bytes");
    synclog ("state APPLY-ON-CREATE key=" + key + " bytes="
             + juce::String ((int) it->second.getSize())
             + " sum=" + juce::String::toHexString ((int) SyncDebug::sum (it->second.getData(), it->second.getSize())));

    auto reHashed = engine.capturePluginState (key);
    if (reHashed.getSize() > 0)
        lastStateHash[key] = hashBlobForSession (reHashed);
}

void Session::broadcastTransport (int cmd, double posTicks)
{
    if (! isActive())
        return;
    juce::MemoryOutputStream out;
    out.writeByte ((char) msgTransport);
    out.writeInt (cmd);
    out.writeDouble (posTicks);
    sendToAll (out.getMemoryBlock());
}

void Session::broadcastCursor (float nx, float ny)
{
    if (! isActive())
        return;
    juce::MemoryOutputStream out;
    out.writeByte ((char) msgCursor);
    out.writeInt64 (localPeerId);
    out.writeString (localDisplayName);
    out.writeFloat (nx);
    out.writeFloat (ny);
    sendToAll (out.getMemoryBlock());
}

std::vector<Session::UserState> Session::getRoster() const
{
    std::vector<UserState> out;
    out.push_back ({ localPeerId,
                     localDisplayName.isNotEmpty() ? localDisplayName : juce::String ("A Kind Soul"),
                     localVolume, localDeafen, localAfk });
    for (auto& [id, s] : remoteStates)
        out.push_back (s);

    std::map<juce::String, int> seenNames;
    for (auto& s : out)
    {
        auto base = s.name.isNotEmpty() ? s.name : juce::String ("A Kind Soul");
        auto key = base.trim().toLowerCase();
        auto n = ++seenNames[key];
        s.name = n == 1 ? base : base + " (" + juce::String (n) + ")";
    }

    return out;
}

void Session::broadcastUserState (float volume, bool deafen, bool afk)
{
    localVolume = volume;
    localDeafen = deafen;
    localAfk = afk;

    if (isActive())
    {
        juce::MemoryOutputStream out;
        out.writeByte ((char) msgUserState);
        out.writeInt64 (localPeerId);
        out.writeString (localDisplayName);
        out.writeFloat (volume);
        out.writeByte ((char) (deafen ? 1 : 0));
        out.writeByte ((char) (afk ? 1 : 0));
        sendToAll (out.getMemoryBlock());
    }

    if (onRosterChanged)
        onRosterChanged();
}

void Session::broadcastAudition (int channelId, bool on, int note, float velocity)
{
    if (! isActive())
        return;
    juce::MemoryOutputStream out;
    out.writeByte ((char) msgAudition);
    out.writeInt64 (localPeerId);
    out.writeInt (channelId);
    out.writeByte ((char) (on ? 1 : 0));
    out.writeByte ((char) juce::jlimit (0, 127, note));
    out.writeFloat (juce::jlimit (0.0f, 1.0f, velocity));
    sendToAll (out.getMemoryBlock());
}

void Session::broadcastProjectSaved (const juce::String& projectName)
{
    if (! isActive())
        return;
    juce::MemoryOutputStream out;
    out.writeByte ((char) msgProjectSaved);
    out.writeString (localDisplayName);
    out.writeString (projectName);
    sendToAll (out.getMemoryBlock());
}

void Session::broadcastSoloChanged (const juce::String& targetName, bool on)
{
    if (! isActive())
        return;
    juce::MemoryOutputStream out;
    out.writeByte ((char) msgSoloChanged);
    out.writeString (localDisplayName);
    out.writeString (targetName);
    out.writeByte ((char) (on ? 1 : 0));
    sendToAll (out.getMemoryBlock());
}

void Session::broadcastRenderLock (bool locked)
{
    if (! isActive())
        return;
    juce::MemoryOutputStream out;
    out.writeByte ((char) msgRenderLock);
    out.writeString (localDisplayName);
    out.writeByte ((char) (locked ? 1 : 0));
    sendToAll (out.getMemoryBlock());
}

void Session::shareRenderedFile (const juce::File& renderedFile)
{
    if (! isActive() || ! renderedFile.existsAsFile())
        return;

    auto originalPath = renderedFile.getFullPathName();
    if (! looksLikeAudioPath (originalPath))
        return;

    if (sessionFolder != juce::File())
    {
        sessionFolder.createDirectory();
        auto dest = getSessionAudioEntryFile (originalPath, renderedFile.getFileName());
        if (dest != juce::File() && dest.getFullPathName() != originalPath)
        {
            renderedFile.copyFileTo (dest);
            if (dest.existsAsFile())
                rememberDownloadedAudio (originalPath, dest);
        }
        if (onMultiplayerFolderChanged)
            onMultiplayerFolderChanged (sessionFolder);
    }

    sendAudioFileTo (nullptr, renderedFile, originalPath);

    juce::MemoryOutputStream out;
    out.writeByte ((char) msgRendered);
    out.writeString (localDisplayName);
    out.writeString (renderedFile.getFileName());
    sendToAll (out.getMemoryBlock());
}

void Session::broadcastVstAdded (const juce::String& pluginKey, const juce::String& pluginName)
{
    if (! isActive())
        return;
    juce::MemoryOutputStream out;
    out.writeByte ((char) msgVstAdded);
    out.writeString (localDisplayName);
    out.writeString (pluginKey);
    out.writeString (pluginName);
    sendToAll (out.getMemoryBlock());
}

void Session::broadcastViewport (const juce::String& panelName, int patternId)
{
    if (! isActive())
        return;
    juce::MemoryOutputStream out;
    out.writeByte ((char) msgViewport);
    out.writeInt64 (localPeerId);
    out.writeString (localDisplayName);
    out.writeString (panelName);
    out.writeInt (patternId);
    sendToAll (out.getMemoryBlock());
}

void Session::broadcastReaction (const juce::String& emoji, float nx, float ny)
{
    if (! isActive())
        return;
    juce::MemoryOutputStream out;
    out.writeByte ((char) msgReaction);
    out.writeInt64 (localPeerId);
    out.writeString (localDisplayName);
    out.writeString (emoji);
    out.writeFloat (nx);
    out.writeFloat (ny);
    sendToAll (out.getMemoryBlock());
}

void Session::sendChatText (const juce::String& text)
{
    if (text.isEmpty())
        return;

    if (onChatText)
        onChatText (localPeerId, localDisplayName, text);

    if (! isActive())
        return;

    juce::MemoryOutputStream out;
    out.writeByte ((char) msgChatText);
    out.writeInt64 (localPeerId);
    out.writeString (localDisplayName);
    out.writeString (text);
    sendToAll (out.getMemoryBlock());
}

void Session::sendChatImage (const juce::MemoryBlock& encodedBytes)
{
    auto img = juce::ImageFileFormat::loadFrom (encodedBytes.getData(), encodedBytes.getSize());
    if (! img.isValid())
        return;

    juce::MemoryBlock bytes = encodedBytes;

    if (bytes.getSize() > (size_t) chatImageMaxBytes)
    {
        juce::Image scaled = img;
        const int maxSide = 1024;
        const int longest = juce::jmax (scaled.getWidth(), scaled.getHeight());
        if (longest > maxSide)
        {
            double scale = (double) maxSide / (double) longest;
            scaled = scaled.rescaled (juce::jmax (1, juce::roundToInt (scaled.getWidth() * scale)),
                                      juce::jmax (1, juce::roundToInt (scaled.getHeight() * scale)),
                                      juce::Graphics::highResamplingQuality);
        }

        juce::MemoryOutputStream pngOut;
        juce::PNGImageFormat png;
        png.writeImageToStream (scaled, pngOut);
        bytes = pngOut.getMemoryBlock();

        if (bytes.getSize() > (size_t) chatImageMaxBytes)
        {
            juce::MemoryOutputStream jpgOut;
            juce::JPEGImageFormat jpg;
            jpg.setQuality (0.85f);
            jpg.writeImageToStream (scaled, jpgOut);
            bytes = jpgOut.getMemoryBlock();
        }

        img = juce::ImageFileFormat::loadFrom (bytes.getData(), bytes.getSize());
    }

    if (bytes.getSize() == 0 || bytes.getSize() > (size_t) chatImageMaxBytes || ! img.isValid())
    {
        if (onChatImageRejected)
            onChatImageRejected();
        return;
    }

    const juce::int64 imageId = juce::Random::getSystemRandom().nextInt64();

    if (onChatImageAnnounce)
        onChatImageAnnounce (localPeerId, localDisplayName, imageId);
    if (onChatImageData)
        onChatImageData (imageId, img, bytes);

    if (! isActive())
        return;

    {
        juce::MemoryOutputStream out;
        out.writeByte ((char) msgChatImageAnnounce);
        out.writeInt64 (localPeerId);
        out.writeString (localDisplayName);
        out.writeInt64 (imageId);
        sendToAll (out.getMemoryBlock());
    }
    {
        juce::MemoryOutputStream out;
        out.writeByte ((char) msgChatImageData);
        out.writeInt64 (imageId);
        out.writeInt64 ((juce::int64) bytes.getSize());
        out.write (bytes.getData(), bytes.getSize());
        sendToAll (out.getMemoryBlock());
    }
}

void Session::sendAvatar (juce::int64 peerId, const juce::MemoryBlock& bytes, Peer* to)
{
    if (bytes.getSize() == 0 || bytes.getSize() > (size_t) avatarMaxBytes)
        return;

    juce::MemoryOutputStream out;
    out.writeByte ((char) msgAvatar);
    out.writeInt64 (peerId);
    out.writeInt64 ((juce::int64) bytes.getSize());
    out.write (bytes.getData(), bytes.getSize());

    auto mb = out.getMemoryBlock();
    if (to != nullptr)
        enqueueTo (to, mb);
    else
        sendToAll (mb);
}

void Session::setLocalAvatar (const juce::MemoryBlock& pngBytes)
{
    if (pngBytes.getSize() > (size_t) avatarMaxBytes)
        return;

    localAvatarBytes = pngBytes;

    if (localAvatarBytes.getSize() > 0)
        peerAvatars[localPeerId] = juce::ImageFileFormat::loadFrom (
            localAvatarBytes.getData(), localAvatarBytes.getSize());
    else
        peerAvatars.erase (localPeerId);

    if (isActive() && localAvatarBytes.getSize() > 0)
        sendAvatar (localPeerId, localAvatarBytes, nullptr);

    if (onRosterChanged)
        onRosterChanged();
}

juce::Image Session::getAvatarFor (juce::int64 peerId) const
{
    auto it = peerAvatars.find (peerId);
    return it != peerAvatars.end() ? it->second : juce::Image();
}

void Session::broadcastPluginState (const juce::String& key)
{
    if (! isActive() || peers.isEmpty())
        return;

    if (inEchoCooldown (key))
    {
        auto blob = engine.capturePluginState (key);
        if (blob.getSize() > 0)
            lastStateHash[key] = hashBlobForSession (blob);
        synclog ("state RESYNC-HASH (remote apply) key=" + key + " (editor-close, cooldown)");
        return;
    }

    if (! localInteractionSinceRemoteApply (key))
    {
        auto blob = engine.capturePluginState (key);
        if (blob.getSize() > 0)
            lastStateHash[key] = hashBlobForSession (blob);
        synclog ("state ABSORB (no local interaction) key=" + key + " (editor-close/non-param capture)");
        return;
    }

    auto blob = engine.capturePluginState (key);
    if (blob.getSize() == 0)
    {
        logState ("broadcast requested for " + key + " but state is empty (skipped)");
        synclog ("state BROADCAST key=" + key + " SKIPPED (captured 0 bytes)");
        return;
    }

    auto h = hashBlobForSession (blob);
    auto it = lastStateHash.find (key);
    if (it != lastStateHash.end() && it->second == h)
    {
        synclog ("state BROADCAST key=" + key + " SKIPPED (unchanged since last send, bytes="
                 + juce::String ((int) blob.getSize()) + ")");
        return;
    }
    lastStateHash[key] = h;

    juce::MemoryOutputStream out;
    out.writeByte ((char) msgPluginState);
    out.writeString (key);
    out.write (blob.getData(), blob.getSize());
    sendToAll (out.getMemoryBlock());

    logState ("broadcast state for " + key + " -> "
              + juce::String ((int) blob.getSize()) + " bytes to "
              + juce::String (peers.size()) + " peer(s)");
    synclog ("state BROADCAST key=" + key + " bytes=" + juce::String ((int) blob.getSize())
             + " sum=" + juce::String::toHexString ((int) SyncDebug::sum (blob.getData(), blob.getSize()))
             + " -> " + juce::String (peers.size()) + " peer(s)  (editor-close/non-param capture)");
}

juce::String Session::fileStem (const juce::String& fileOrId)
{
    return juce::File::createFileWithoutCheckingPath (fileOrId)
               .getFileNameWithoutExtension().toLowerCase();
}

Session::PluginInventoryItem Session::itemForDesc (const juce::PluginDescription& d)
{
    PluginInventoryItem it;
    it.format        = d.pluginFormatName;
    it.uid           = d.uniqueId;
    it.deprecatedUid = d.deprecatedUid;
    it.name          = d.name;
    it.fileName      = fileStem (d.fileOrIdentifier);
    it.isInstrument  = d.isInstrument;
    return it;
}

void Session::buildLocalInventory()
{
    localInventory.clear();

    auto add = [this] (const juce::PluginDescription& d)
    { localInventory.push_back (itemForDesc (d)); };

    for (const auto& d : pluginHost.knownPlugins.getTypes())
        add (d);
    for (const auto& d : pluginHost.unverifiedPlugins.getTypes())
        add (d);

    localInventoryBuilt = true;
}

bool Session::itemsMatch (const PluginInventoryItem& a, const PluginInventoryItem& b)
{
    if (! Bridge32::formatsInterchangeable (a.format, b.format))
        return false;
    if (a.uid != 0 && a.uid == b.uid)
        return true;
    if (a.deprecatedUid != 0 && a.deprecatedUid == b.deprecatedUid)
        return true;
    if (a.name.isNotEmpty() && a.name.equalsIgnoreCase (b.name))
        return true;
    if (a.fileName.isNotEmpty() && a.fileName == b.fileName)
        return true;
    return false;
}

juce::String Session::sigForItem (const PluginInventoryItem& it) const
{
    return it.format + "|" + juce::String (it.uid) + "|" + juce::String (it.deprecatedUid)
         + "|" + it.name.toLowerCase() + "|" + it.fileName;
}

juce::String Session::sigForDesc (const juce::PluginDescription& d) const
{
    return d.pluginFormatName + "|" + juce::String (d.uniqueId) + "|" + juce::String (d.deprecatedUid)
         + "|" + d.name.toLowerCase() + "|" + fileStem (d.fileOrIdentifier);
}

void Session::recomputeCompatibility()
{
    if (! localInventoryBuilt)
        buildLocalInventory();

    compatibleSignatures.clear();
    if (peerInventories.empty())
        return;

    for (const auto& local : localInventory)
    {
        bool onEveryPeer = true;
        for (const auto& [pid, inv] : peerInventories)
        {
            bool found = false;
            for (const auto& it : inv)
                if (itemsMatch (local, it)) { found = true; break; }
            if (! found) { onEveryPeer = false; break; }
        }
        if (onEveryPeer)
            compatibleSignatures.insert (sigForItem (local));
    }
}

bool Session::isPluginCompatible (const juce::PluginDescription& desc) const
{
    if (! isActive() || peerInventories.empty())
        return true;

    if (compatibleSignatures.count (sigForDesc (desc)) > 0)
        return true;

    const auto item = itemForDesc (desc);
    for (const auto& [pid, inv] : peerInventories)
    {
        bool onPeer = false;
        for (const auto& it : inv)
            if (itemsMatch (item, it)) { onPeer = true; break; }
        if (! onPeer)
            return false;
    }
    return true;
}

Session::CompatReport Session::getCompatReport() const
{
    CompatReport r;
    r.peerCount  = (int) peerInventories.size();
    r.localTotal = (int) localInventory.size();
    r.shared     = (int) compatibleSignatures.size();

    std::set<juce::String> missing;
    for (const auto& [pid, inv] : peerInventories)
        for (const auto& it : inv)
        {
            bool haveLocally = false;
            for (const auto& local : localInventory)
                if (itemsMatch (it, local)) { haveLocally = true; break; }
            if (! haveLocally)
                missing.insert (sigForItem (it));
        }
    r.missingFromPeers = (int) missing.size();
    return r;
}

void Session::writeInventory (juce::MemoryOutputStream& out, juce::int64 peerId,
                              const std::vector<PluginInventoryItem>& inv) const
{
    out.writeByte ((char) msgPluginInventory);
    out.writeInt64 (peerId);
    out.writeByte ((char) 0);
    out.writeInt ((int) inv.size());
    for (const auto& it : inv)
    {
        out.writeString (it.format);
        out.writeInt (it.uid);
        out.writeInt (it.deprecatedUid);
        out.writeString (it.name);
        out.writeString (it.fileName);
        out.writeByte ((char) (it.isInstrument ? 1 : 0));
    }
}

void Session::sendInventoryTo (Peer* to)
{
    if (! isActive())
        return;
    if (! localInventoryBuilt)
        buildLocalInventory();

    juce::MemoryOutputStream out;
    writeInventory (out, localPeerId, localInventory);
    auto mb = out.getMemoryBlock();
    if (to != nullptr)
        enqueueTo (to, mb);
    else
        sendToAll (mb);
}

void Session::sendStoredInventoryTo (Peer* to, juce::int64 peerId,
                                     const std::vector<PluginInventoryItem>& inv)
{
    if (to == nullptr)
        return;
    juce::MemoryOutputStream out;
    writeInventory (out, peerId, inv);
    enqueueTo (to, out.getMemoryBlock());
}

void Session::dropPeerInventory (juce::int64 peerId)
{
    if (peerInventories.erase (peerId) > 0)
    {
        recomputeCompatibility();
        if (onCompatibilityChanged)
            onCompatibilityChanged();
    }
}

void Session::refreshLocalInventory()
{
    buildLocalInventory();
    recomputeCompatibility();
    if (onCompatibilityChanged)
        onCompatibilityChanged();
    if (isActive())
        sendInventoryTo (nullptr);
}
