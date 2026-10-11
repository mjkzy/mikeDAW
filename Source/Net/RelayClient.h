#pragma once
#include <JuceHeader.h>

class RelayClient : private juce::Thread,
                    private juce::Timer
{
public:
    RelayClient();
    ~RelayClient() override;

    void configure (const juce::String& host, int port, bool enabled);
    void start (juce::int64 userId);
    void stop();
    bool isConnected() const;

    juce::String getHost() const;
    int getPort() const;

    std::function<juce::String()> nameProvider;
    std::function<juce::String()> statusProvider;
    std::function<int()> peopleProvider;
    std::function<int()> sessionPortProvider;
    std::function<juce::String()> inviteCodeProvider;
    std::function<juce::String()> joinTokenProvider;
    std::function<bool()> afkProvider;
    std::function<juce::int64()> afkSinceProvider;
    std::function<juce::Array<juce::int64>()> watchIdsProvider;
    std::function<juce::Array<juce::int64>()> friendIdsProvider;
    std::function<juce::MemoryBlock()> avatarProvider;

    std::function<void (juce::int64 id, const juce::String& name, const juce::String& status,
                        int people, const juce::String& address, int sessionPort,
                        const juce::String& inviteCode, const juce::String& avatarHash,
                        bool afk, juce::int64 afkSinceMs)> onPresence;
    std::function<void (juce::int64 id, juce::int64 lastSeenMs, const juce::String& name)> onFriendLastSeen;
    std::function<void (juce::int64 fromId, const juce::String& name)> onFriendRequest;
    std::function<void (juce::int64 fromId, const juce::String& name, bool accepted)> onFriendResponse;
    std::function<void (juce::int64 fromId, const juce::String& fromName, const juce::String& sessionName,
                        const juce::String& inviteCode, const juce::String& address, int port)> onInvite;
    std::function<void (juce::int64 id, const juce::MemoryBlock& pngBytes)> onAvatar;
    std::function<void (bool connected)> onConnectionChanged;
    std::function<void (const juce::String& token)> onJoinOk;
    std::function<void (const juce::String& reason)> onJoinFail;
    std::function<void (const juce::String& token, juce::int64 fromId,
                        const juce::String& fromName, const juce::String& joinToken)> onBridgeRequest;

    void sendFriendRequest (juce::int64 toId);
    void respondFriendRequest (juce::int64 toId, bool accept);
    void sendInvite (juce::int64 toId, const juce::String& sessionName, const juce::String& inviteCode, int port);
    void sendJoinRequest (juce::int64 hostId, const juce::String& joinToken = {});
    void requestAvatar (juce::int64 id);
    void notifyStateChanged();
    void notifyFriendsChanged();

    static constexpr int maxAvatarBytes = 256 * 1024;

    static constexpr int presenceIntervalMs = 5000;
    static constexpr int reconnectMinMs = 5000;
    static constexpr int reconnectMaxMs = 60000;

private:
    void run() override;
    void timerCallback() override;
    void drainInbound();
    void handleLine (const juce::String& line);
    void enqueue (const juce::String& line);
    juce::String makeHello() const;
    juce::String makePresence() const;
    juce::String makeWatch() const;
    juce::String makeFriends() const;
    void pushHandshake();

    juce::StreamingSocket socket;

    mutable juce::CriticalSection lock;
   #if OPENDAW_PUBLIC_BUILD
    juce::String host;
   #else
    juce::String host { "148.113.175.195" };
   #endif
    int port = 41236;
    bool enabled = true;
    bool running = false;
    bool configDirty = false;
    bool handshakeNeeded = false;
    bool awaitingHello = false;
    juce::int64 userId = 0;
    std::vector<juce::String> outQueue;
    std::vector<juce::String> inQueue;

    std::atomic<bool> connected { false };

    bool lastReportedConnected = false;
    bool pendingStateChange = false;
    bool pendingFriendsPush = false;
    int presenceTick = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RelayClient)
};
