#pragma once
#include <JuceHeader.h>
#include <map>
#include <set>
#include <vector>

class FriendsManager : private juce::Timer
{
public:
    explicit FriendsManager (const juce::File& settingsFolder);
    ~FriendsManager() override;

    struct Friend
    {
        juce::int64 id = 0;
        juce::String name;
        juce::int64 lastSeenMs = 0;
        juce::String lastAddress;
    };

    struct Presence
    {
        juce::int64 id = 0;
        juce::String name;
        juce::String status;
        juce::String address;
        juce::String relayAddress;
        juce::String inviteCode;
        int people = 1;
        int sessionPort = 0;
        juce::int64 lastBeaconMs = 0;
        juce::int64 lanBeaconMs = 0;
        juce::int64 onlineSinceMs = 0;
        juce::int64 afkSinceMs = 0;
        bool online = false;
        bool afk = false;
    };

    struct Entry
    {
        Friend info;
        bool online = false;
        juce::String status;
        juce::String address;
        int people = 1;
        int sessionPort = 0;
        juce::int64 onlineSinceMs = 0;
        juce::int64 afkSinceMs = 0;
        bool afk = false;
    };

    struct Request
    {
        juce::int64 id = 0;
        juce::String name;
        juce::int64 sinceMs = 0;
    };

    void addFriend (juce::int64 id, const juce::String& name);
    void removeFriend (juce::int64 id);
    bool isFriend (juce::int64 id) const;

    void addIncomingRequest (juce::int64 id, const juce::String& name);
    void removeIncomingRequest (juce::int64 id);
    bool hasIncomingRequest (juce::int64 id) const;
    void addOutgoingRequest (juce::int64 id, const juce::String& name);
    void clearOutgoingRequest (juce::int64 id);
    bool hasOutgoingRequest (juce::int64 id) const;
    std::vector<Request> getIncomingRequests() const;
    std::vector<Request> getOutgoingRequests() const;

    void noteServerLastSeen (juce::int64 id, juce::int64 lastSeenMs, const juce::String& name);

    void notePresence (juce::int64 id, const juce::String& name,
                       const juce::String& status, int people, const juce::String& address,
                       int sessionPort, bool viaRelay = false, const juce::String& inviteCode = {},
                       bool afk = false, juce::int64 afkSinceMs = 0);

    void cacheAvatar (juce::int64 id, const juce::MemoryBlock& encodedImage);
    juce::Image avatarFor (juce::int64 id) const;
    juce::String avatarHashFor (juce::int64 id) const;

    std::vector<Entry> getFriends() const;

    std::vector<Presence> getNearbyNonFriends() const;

    juce::String inviteCodeFor (juce::int64 id) const;

    juce::String addressFor (juce::int64 id) const;
    juce::String lanAddressFor (juce::int64 id) const;
    int sessionPortFor (juce::int64 id) const;

    int numOnline() const;

    std::function<void()> onFriendsChanged;
    std::function<void (juce::int64 id, const juce::String& name, bool online)> onFriendPresenceChanged;

    static constexpr juce::int64 onlineWindowMs = 7000;
    static constexpr juce::int64 offlineWindowMs = 30000;

private:
    const Friend* findFriend (juce::int64 id) const;
    bool isOnline (juce::int64 id) const;
    void load();
    void save();
    void timerCallback() override;

    juce::File file;
    juce::File avatarFolder;
    std::vector<Friend> friends;
    std::vector<Request> incomingRequests;
    std::vector<Request> outgoingRequests;
    std::map<juce::int64, Presence> presence;
    mutable std::map<juce::int64, juce::Image> avatarCache;
    mutable std::map<juce::int64, juce::String> avatarHashCache;
    std::set<juce::int64> onlineFriendIds;
    juce::int64 startupMs = 0;
    bool dirty = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FriendsManager)
};
