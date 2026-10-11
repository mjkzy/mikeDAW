#pragma once
#include <JuceHeader.h>

class LanDiscovery : private juce::Thread,
                     private juce::Timer
{
public:
    LanDiscovery();
    ~LanDiscovery() override;

    struct Entry
    {
        juce::String name;
        juce::String address;
        int port = 0;
        int people = 1;
        juce::uint32 lastSeenMs = 0;
    };

    void startAdvertising (const juce::String& sessionName, int tcpPort);
    void stopAdvertising();

    std::function<int()> peopleProvider;

    void startListening();
    void stopListening();

    void setQuerying (bool shouldQuery);

    juce::Array<Entry> getSessions() const;

    void startPresence (juce::int64 userId);

    void sendPresenceNow();

    void setPresenceQuerying (bool shouldQuery);

    std::function<juce::String()> presenceNameProvider;
    std::function<juce::String()> presenceStatusProvider;
    std::function<int()> presenceSessionPortProvider;
    std::function<bool()> presenceAfkProvider;
    std::function<juce::int64()> presenceAfkSinceProvider;

    std::function<void (juce::int64 id, const juce::String& name,
                        const juce::String& status, int people,
                        const juce::String& address, int sessionPort,
                        bool afk, juce::int64 afkSinceMs)> onPresence;

    std::function<void (juce::int64 fromId, const juce::String& fromName,
                        const juce::String& sessionName,
                        const juce::String& address, int port)> onInviteReceived;

    void sendInvite (const juce::String& address, juce::int64 fromUserId,
                     const juce::String& fromName, const juce::String& sessionName, int tcpPort);

    std::function<void (juce::int64 fromId, const juce::String& fromName,
                        const juce::String& address)> onFriendRequest;
    std::function<void (juce::int64 fromId, const juce::String& fromName,
                        bool accepted)> onFriendResponse;

    void sendFriendRequest (const juce::String& address, juce::int64 fromUserId,
                            const juce::String& fromName);
    void sendFriendResponse (const juce::String& address, juce::int64 fromUserId,
                             const juce::String& fromName, bool accepted);

    static constexpr int discoveryPort = 41235;
    static constexpr int beaconIntervalMs = 1000;
    static constexpr int presenceIntervalMs = 2000;
    static constexpr int entryTimeoutMs = 4000;

private:
    void run() override;
    void timerCallback() override;
    void sendBeacon();
    void sendQuery();
    void sendPresenceQuery();
    void sendPresence();
    void sweepSubnetsWith (const juce::String& payload);
    void broadcastTo (juce::DatagramSocket&, const juce::String& payload);
    void writeTo (juce::DatagramSocket&, const juce::String& address, int port,
                  const void* bytes, int len);
    juce::String makeBeaconPayload() const;
    void drainInbound();
    void updateTimerState();
    void refreshLocalAddresses();
    void expireOldLocked();

    juce::DatagramSocket listenSocket { true };
    juce::DatagramSocket sendSocket { true };
    bool sendSocketBound = false;
    bool listening = false;
    bool querying = false;
    bool presenceQuerying = false;

    bool advertising = false;
    juce::String advertName;
    int advertPort = 0;

    bool presenceActive = false;
    juce::String lastPresencePayload;
    juce::int64 myUserId = 0;
    int presenceTick = 0;

    struct InPresence { juce::int64 id; juce::String name, status, address; int people, sessionPort;
                        bool afk; juce::int64 afkSinceMs; };
    struct InInvite   { juce::int64 fromId; juce::String name, sessionName, address; int port; };
    struct InFriendReq { juce::int64 fromId; juce::String name, address; };
    struct InFriendRsp { juce::int64 fromId; juce::String name; bool accepted; };
    std::vector<InPresence> pendingPresence;
    std::vector<InInvite>   pendingInvites;
    std::vector<InFriendReq> pendingFriendReq;
    std::vector<InFriendRsp> pendingFriendRsp;

    mutable juce::CriticalSection lock;
    juce::CriticalSection socketWriteLock;
    juce::Array<Entry> sessions;
    juce::StringArray localAddresses;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LanDiscovery)
};
