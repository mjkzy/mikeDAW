#pragma once
#include <JuceHeader.h>
#include "../Engine/AudioEngine.h"
#include "../Engine/PluginHost.h"
#include "TreeSync.h"
#include <map>
#include <set>
#include <vector>
#include <deque>
#include <memory>
#include <atomic>

class Session : private juce::AsyncUpdater,
                private juce::Timer,
                private juce::ValueTree::Listener
{
public:
    Session (juce::ValueTree projectTree, AudioEngine& engine, PluginHost& host);
    ~Session() override;

    enum class Role { none, host, client };

    bool startHost (int port);
    bool joinHost (const juce::String& address, int port);
    void disconnect();

    void clearCallbacks();

    void endSession();

    void resetProjectForPeers();

    void replaceProject (const juce::ValueTree& incoming);

    Role getRole() const noexcept       { return role; }
    int getNumPeers() const noexcept    { return peers.size(); }
    bool isActive() const noexcept      { return role != Role::none; }

    std::function<void (const juce::String&)> onStatus;

    std::function<void (juce::int64 peerId, const juce::String& name, float nx, float ny)> onPeerCursor;

    std::function<void (const juce::String& name)> onPeerJoined;
    std::function<void (const juce::String& name)> onPeerLeft;

    std::function<void()> onSnapshotApplied;

    std::function<void (const juce::String& hostName)> onSessionEnded;

    std::function<void (const juce::String& hostName)> onKicked;

    std::function<void (const juce::String& reason)> onResyncing;

    std::function<void (const juce::String& reason)> onJoinRejected;

    void kickPeer (juce::int64 peerId);

    std::function<void (const juce::String& saverName, const juce::String& projectName)> onProjectSavedByPeer;

    std::function<void (const juce::String& renderName, const juce::String& fileName)> onRenderedByPeer;

    std::function<void (const juce::String& byName, bool locked)> onRenderLockChanged;

    std::function<void (const juce::String& byName, const juce::String& targetName, bool on)> onSoloChangedByPeer;

    std::function<void (const juce::String& senderName, const juce::String& pluginKey, const juce::String& pluginName)> onVstAddedByPeer;

    std::function<void (const juce::String& soundName)> onAudioDownloadStarted;
    std::function<void (const juce::String& soundName)> onAudioDownloadFinished;
    std::function<void (const juce::File& folder)> onMultiplayerFolderChanged;

    void setDisplayName (const juce::String& name) { localDisplayName = name; }
    const juce::String& getDisplayName() const noexcept { return localDisplayName; }
    juce::int64 getLocalPeerId() const noexcept { return localPeerId; }

    void setLocalPeerId (juce::int64 id) noexcept { if (id != 0) localPeerId = id; }

    struct PluginInventoryItem
    {
        juce::String format;
        int uid = 0;
        int deprecatedUid = 0;
        juce::String name;
        juce::String fileName;
        bool isInstrument = false;
    };

    void refreshLocalInventory();

    bool isPluginCompatible (const juce::PluginDescription& desc) const;

    struct CompatReport
    {
        int peerCount = 0;
        int localTotal = 0;
        int shared = 0;
        int missingFromPeers = 0;
    };
    CompatReport getCompatReport() const;

    std::function<void()> onCompatibilityChanged;

    void setLocalAvatar (const juce::MemoryBlock& pngBytes);

    juce::Image getAvatarFor (juce::int64 peerId) const;
    std::function<void (juce::int64 peerId, const juce::MemoryBlock& encodedImage)> onAvatarChanged;

    juce::String getHostDisplayName() const { return hostDisplayName; }
    juce::File getMultiplayerFolder() const { return sessionFolder; }
    juce::File getSessionFolder() const { return sessionFolder; }

    std::atomic<bool>* remoteApplyFlag = nullptr;

    struct UserState
    {
        juce::int64 peerId = 0;
        juce::String name;
        float volume = 1.0f;
        bool deafen = false;
        bool afk = false;
    };

    std::vector<UserState> getRoster() const;

    std::function<void()> onRosterChanged;

    void broadcastUserState (float volume, bool deafen, bool afk);

    void broadcastParam (const juce::String& key, int paramIndex, float value);
    void broadcastTransport (int cmd, double posTicks);
    void broadcastPluginState (const juce::String& key);
    void broadcastCursor (float nx, float ny);

    void applyCachedStateOnCreate (const juce::String& key);

    std::function<bool (const juce::String& key)> isPluginWindowOpen;

    std::function<juce::uint32 (const juce::String& key)> getPluginInteraction;

    void broadcastProjectSaved (const juce::String& projectName);

    void broadcastSoloChanged (const juce::String& targetName, bool on);

    void shareRenderedFile (const juce::File& renderedFile);

    void broadcastRenderLock (bool locked);

    void broadcastVstAdded (const juce::String& pluginKey, const juce::String& pluginName);

    void broadcastAudition (int channelId, bool on, int note, float velocity);

    std::function<void (juce::int64 peerId, int channelId, bool on, int note, float velocity)> onPeerAudition;

    void broadcastViewport (const juce::String& panelName, int patternId = -1);
    void broadcastReaction (const juce::String& emoji, float nx = -1.0f, float ny = -1.0f);
    void sendChatText (const juce::String& text);
    void sendChatImage (const juce::MemoryBlock& encodedBytes);
    std::function<void (juce::int64 peerId, const juce::String& name, const juce::String& text)> onChatText;
    std::function<void (juce::int64 peerId, const juce::String& name, juce::int64 imageId)> onChatImageAnnounce;
    std::function<void (juce::int64 imageId, juce::Image image, const juce::MemoryBlock& bytes)> onChatImageData;
    std::function<void()> onChatImageRejected;
    static constexpr int chatImageMaxBytes = 4 * 1024 * 1024;
    std::function<void (juce::int64 peerId, const juce::String& name, const juce::String& panelName, int patternId)> onPeerViewport;
    std::function<void (juce::int64 peerId, const juce::String& name, const juce::String& emoji, float nx, float ny)> onPeerReaction;

    static constexpr int defaultPort = 41234;
    static constexpr int maxPeople = 4;
    static constexpr int avatarMaxBytes = 256 * 1024;

private:
    enum MsgType : juce::uint8
    {
        msgHello = 1,
        msgSnapshot,
        msgDelta,
        msgParam,
        msgPluginState,
        msgTransport,
        msgCursor,
        msgPresence,
        msgAudioFile,
        msgUserState,
        msgProjectSaved,
        msgVstAdded,
        msgAudition,
        msgViewport,
        msgPing,
        msgReaction,
        msgAvatar,
        msgPluginInventory,
        msgChatText,
        msgChatImageAnnounce,
        msgChatImageData,
        msgRendered,
        msgRenderLock,
        msgSoloChanged,
        msgSessionEnded,
        msgAudioFileOffer,
        msgAudioFileRequest,
        msgKick,
        msgKeepalive,
        msgProjectReset,
        msgRejected,
        msgResyncRequest
    };

    static constexpr int protocolVersion = 3;

    struct Peer : juce::InterprocessConnection
    {
        Peer (Session& s) : juce::InterprocessConnection (true, 0xda3b71c4), owner (s) {}
        ~Peer() override { disconnect(); }
        void connectionMade() override;
        void connectionLost() override;
        void messageReceived (const juce::MemoryBlock&) override;
        Session& owner;
        juce::String displayName;
        juce::int64 peerId = 0;
        bool announced = false;
        juce::int64 lastRecvMs = 0;
        bool peerSendsKeepalives = false;
        juce::int64 lastServedResyncMs = 0;
    };

    struct Server : juce::InterprocessConnectionServer
    {
        Server (Session& s) : owner (s) {}
        juce::InterprocessConnection* createConnectionObject() override;
        Session& owner;
    };

    struct Sync : TreeSync
    {
        Sync (const juce::ValueTree& tree, Session& s) : TreeSync (tree), owner (s) {}
        void stateChanged (const void* data, size_t size) override;
        Session& owner;
    };

    struct PeerLink
    {
        std::atomic<Peer*> peer { nullptr };
        juce::CriticalSection sendLock;
    };

    struct OutboundItem
    {
        juce::MemoryBlock data;
        std::shared_ptr<PeerLink> single;
        Peer* except = nullptr;
        bool hasExcept = false;
        bool isFile = false;
        juce::File file;
        juce::String originalPath;
    };

    struct SenderThread : juce::Thread
    {
        SenderThread (Session& s) : juce::Thread ("mikeDAW net sender"), owner (s) {}
        void run() override { owner.runSender(); }
        Session& owner;
    };

    std::unique_ptr<SenderThread> senderThread;
    juce::CriticalSection queueLock;
    std::deque<OutboundItem> outQueue;
    juce::WaitableEvent queueSignal;

    juce::CriticalSection linksLock;
    std::map<Peer*, std::shared_ptr<PeerLink>> peerLinks;

    void runSender();
    void enqueue (OutboundItem&& item);
    void enqueueTo (Peer* peer, juce::MemoryBlock data);
    void sendVia (const std::shared_ptr<PeerLink>& link, const juce::MemoryBlock& data);
    void registerLink (Peer* peer);
    void unregisterLink (Peer* peer);
    std::shared_ptr<PeerLink> linkFor (Peer* peer);

    juce::ThreadPool audioWritePool { 1 };

    void handleMessage (Peer&, const juce::MemoryBlock&);
    void sendToAll (const juce::MemoryBlock&, Peer* except = nullptr);
    void sendSnapshotTo (Peer&);
    juce::MemoryBlock buildProjectResetBlock();
    void sendProjectResetTo (Peer&, const juce::MemoryBlock& block);
    void applySnapshot (const juce::String& xml);
    void dedupeChannelSeqs();
    void disconnectAsync();

    void noteUnresolvedDelta (const Model::TreePath& path, bool isRemoveChild, Peer* from);
    void requestResync (const juce::String& reason);
    static constexpr int resyncDropThreshold = 3;
    static constexpr juce::int64 resyncDropWindowMs = 30000;
    static constexpr juce::int64 resyncMinIntervalMs = 15000;
    static constexpr juce::int64 resyncServeMinIntervalMs = 10000;
    static constexpr int resyncMaxRequests = 3;
    int deltasAppliedSinceSnapshot = 0;
    int unresolvedDropCount = 0;
    juce::int64 unresolvedWindowStartMs = 0;
    juce::int64 lastResyncMs = 0;
    int resyncRequestsUnanswered = 0;
    bool resyncGaveUp = false;

    void removePeer (Peer*);
    void finishPeerRemoval (Peer*, const juce::String& goneName, juce::int64 goneId, bool wasHostPeer);
    void dropStalePeerFor (Peer& joiner);
    void updateStatus();
    void prepareSessionFolder();
    void shareAudioFileIfNeeded (const juce::String& path);
    void sendAudioFileTo (Peer* peer, const juce::File& file, const juce::String& originalPath);
    void sendProjectAudioFilesTo (Peer& peer);
    bool isPathHeldPending (const juce::String& path) const;
    bool isRecordingPath (const juce::String& originalPath) const;
    void rememberDownloadedAudio (const juce::String& originalPath, const juce::File& localFile);
    juce::File resolveAudioFileForSharing (const juce::String& originalPath) const;
    juce::File getSessionAudioEntryFile (const juce::String& originalPath, const juce::String& fileName);
    static juce::File multiplayerCacheFolder();
    void loadCacheIndex();
    void saveCacheIndex() const;
    void exposeLocalAudioInSessionFolder (const juce::File& file, const juce::String& originalPath);
    static bool isShareableAudioPath (const juce::String& path);
    static juce::String fileHashPrefix (const juce::String& text);
    void handleAsyncUpdate() override;
    void timerCallback() override;

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override {}
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override {}
    void valueTreeParentChanged (juce::ValueTree&) override {}
    void valueTreeRedirected (juce::ValueTree&) override {}

    static juce::MemoryBlock makeParamMsg (const juce::String& key, int idx, float value);
    static juce::MemoryBlock makeAudioFileOfferMsg (const juce::String& originalPath,
                                                    const juce::String& fileName,
                                                    juce::int64 fileSize);

    juce::ValueTree project;
    AudioEngine& engine;
    PluginHost& pluginHost;
    Role role = Role::none;

    juce::int64 localPeerId = (juce::int64) juce::Random::getSystemRandom().nextInt64();
    juce::String localDisplayName { "A Kind Soul" };
    juce::String hostDisplayName;

    float localVolume = 1.0f;
    bool localDeafen = false;
    bool localAfk = false;
    std::map<juce::int64, UserState> remoteStates;

    juce::MemoryBlock localAvatarBytes;
    std::map<juce::int64, juce::MemoryBlock> peerAvatarBytes;
    std::map<juce::int64, juce::Image> peerAvatars;
    void sendAvatar (juce::int64 peerId, const juce::MemoryBlock& bytes, Peer* to);

    void announcePresence (int kind, const juce::String& name, juce::int64 peerId, Peer* except = nullptr);

    std::unique_ptr<Server> server;
    juce::OwnedArray<Peer> peers;
    std::unique_ptr<Sync> sync;
    bool applyingRemote = false;
    std::atomic<bool> dedupePending { false };
    bool sessionEndedByHost = false;
    bool kickedByHost = false;
    juce::File sessionFolder;
    std::map<juce::String, juce::File> downloadedAudioPaths;
    std::map<juce::String, juce::File> sessionAudioEntries;
    std::set<juce::String> sharedAudioPaths;
    mutable juce::CriticalSection audioPathsLock;

    juce::CriticalSection paramQueueLock;
    struct PendingParam { juce::String key; int idx; float value; };
    std::vector<PendingParam> paramQueue;

    std::map<juce::String, juce::int64> lastStateHash;

    static constexpr int echoCooldownMs = 1000;
    std::map<juce::String, juce::uint32> remoteApplyStamp;

    static constexpr int pendingStableMaxTicks = 6;
    struct PendingState { juce::int64 hash = 0; int totalTicks = 0; };
    std::map<juce::String, PendingState> pendingState;

    static constexpr int stateApplyDeferMs = 350;
    struct DeferredStateApply : juce::Timer
    {
        DeferredStateApply (Session& s, juce::String k) : owner (s), key (std::move (k)) {}
        void timerCallback() override { owner.applyDeferredState (key); }
        Session& owner;
        juce::String key;
        juce::MemoryBlock blob;
    };
    std::map<juce::String, std::unique_ptr<DeferredStateApply>> deferredStateApplies;
    std::vector<std::unique_ptr<DeferredStateApply>> retiredStateApplies;
    std::map<juce::String, juce::int64> lastAppliedStateHash;
    void applyDeferredState (const juce::String& key);

    void stampRemoteApply (const juce::String& key) { remoteApplyStamp[key] = juce::Time::getMillisecondCounter(); }
    std::vector<PluginInventoryItem> localInventory;
    bool localInventoryBuilt = false;
    std::map<juce::int64, std::vector<PluginInventoryItem>> peerInventories;
    std::set<juce::String> compatibleSignatures;

    void buildLocalInventory();
    void recomputeCompatibility();
    void sendInventoryTo (Peer* to);
    void sendStoredInventoryTo (Peer* to, juce::int64 peerId,
                                const std::vector<PluginInventoryItem>& inv);
    void writeInventory (juce::MemoryOutputStream& out, juce::int64 peerId,
                         const std::vector<PluginInventoryItem>& inv) const;
    void dropPeerInventory (juce::int64 peerId);

    static bool itemsMatch (const PluginInventoryItem& a, const PluginInventoryItem& b);
    juce::String sigForItem (const PluginInventoryItem& it) const;
    juce::String sigForDesc (const juce::PluginDescription& d) const;
    static juce::String fileStem (const juce::String& fileOrId);
    static PluginInventoryItem itemForDesc (const juce::PluginDescription& d);

    bool localInteractionSinceRemoteApply (const juce::String& key) const
    {
        auto stampIt = remoteApplyStamp.find (key);
        if (stampIt == remoteApplyStamp.end())
            return true;
        const juce::uint32 lastRemote = stampIt->second;
        const juce::uint32 lastLocal = getPluginInteraction ? getPluginInteraction (key) : 0;
        return lastLocal != 0 && (juce::int32) (lastLocal - lastRemote) > 0;
    }

    bool inEchoCooldown (const juce::String& key) const
    {
        auto it = remoteApplyStamp.find (key);
        return it != remoteApplyStamp.end()
               && (juce::Time::getMillisecondCounter() - it->second) < (juce::uint32) echoCooldownMs;
    }

    std::map<juce::String, juce::MemoryBlock> latestRemoteState;

    int pollTick = 0;
    int backgroundPollCursor = 0;
    static constexpr int maxBackgroundPollsPerTick = 4;

    std::map<juce::String, double> captureCostMs;
    std::map<juce::String, int> lastPolledTick;
    double tickCaptureMs = 0.0;
    static constexpr double backgroundTickBudgetMs = 4.0;

    void pollPluginState (const juce::String& key, bool windowOpen);

public:
    void dropCachedState (const juce::String& key) { latestRemoteState.erase (key); }

private:

    bool renderLockHeld = false;
    juce::String renderLockOwner;

    JUCE_DECLARE_WEAK_REFERENCEABLE (Session)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Session)
};
