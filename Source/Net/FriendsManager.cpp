#include "FriendsManager.h"
#include <algorithm>

FriendsManager::FriendsManager (const juce::File& settingsFolder)
    : file (settingsFolder.getChildFile ("friends.xml")),
      avatarFolder (settingsFolder.getChildFile ("Friend Avatars"))
{
    load();
    startupMs = juce::Time::currentTimeMillis();
    startTimer (1000);
}

FriendsManager::~FriendsManager()
{
    stopTimer();
    if (dirty)
        save();
}

void FriendsManager::load()
{
    friends.clear();
    incomingRequests.clear();
    outgoingRequests.clear();
    if (! file.existsAsFile())
        return;

    if (auto xml = juce::parseXML (file))
        for (auto* e : xml->getChildIterator())
        {
            if (e->hasTagName ("Friend"))
            {
                Friend f;
                f.id = e->getStringAttribute ("id").getLargeIntValue();
                if (f.id == 0)
                    continue;
                f.name = e->getStringAttribute ("name");
                f.lastSeenMs = e->getStringAttribute ("lastSeenMs").getLargeIntValue();
                f.lastAddress = e->getStringAttribute ("lastAddress");
                friends.push_back (std::move (f));
            }
            else if (e->hasTagName ("IncomingRequest") || e->hasTagName ("OutgoingRequest"))
            {
                Request r;
                r.id = e->getStringAttribute ("id").getLargeIntValue();
                if (r.id == 0)
                    continue;
                r.name = e->getStringAttribute ("name");
                r.sinceMs = e->getStringAttribute ("sinceMs").getLargeIntValue();
                (e->hasTagName ("IncomingRequest") ? incomingRequests : outgoingRequests)
                    .push_back (std::move (r));
            }
        }
}

void FriendsManager::save()
{
    juce::XmlElement xml ("Friends");
    for (auto& f : friends)
    {
        auto* e = xml.createNewChildElement ("Friend");
        e->setAttribute ("id", juce::String (f.id));
        e->setAttribute ("name", f.name);
        e->setAttribute ("lastSeenMs", juce::String (f.lastSeenMs));
        e->setAttribute ("lastAddress", f.lastAddress);
    }
    for (auto& r : incomingRequests)
    {
        auto* e = xml.createNewChildElement ("IncomingRequest");
        e->setAttribute ("id", juce::String (r.id));
        e->setAttribute ("name", r.name);
        e->setAttribute ("sinceMs", juce::String (r.sinceMs));
    }
    for (auto& r : outgoingRequests)
    {
        auto* e = xml.createNewChildElement ("OutgoingRequest");
        e->setAttribute ("id", juce::String (r.id));
        e->setAttribute ("name", r.name);
        e->setAttribute ("sinceMs", juce::String (r.sinceMs));
    }
    file.getParentDirectory().createDirectory();
    xml.writeTo (file);
    dirty = false;
}

const FriendsManager::Friend* FriendsManager::findFriend (juce::int64 id) const
{
    for (auto& f : friends)
        if (f.id == id)
            return &f;
    return nullptr;
}

bool FriendsManager::isFriend (juce::int64 id) const
{
    return findFriend (id) != nullptr;
}

bool FriendsManager::isOnline (juce::int64 id) const
{
    auto it = presence.find (id);
    return it != presence.end() && it->second.online;
}

void FriendsManager::addFriend (juce::int64 id, const juce::String& name)
{
    if (id == 0)
        return;

    bool clearedRequest = false;
    auto eraseRequest = [id, &clearedRequest] (std::vector<Request>& v)
    {
        auto before = v.size();
        v.erase (std::remove_if (v.begin(), v.end(),
                                 [id] (const Request& r) { return r.id == id; }), v.end());
        if (v.size() != before)
            clearedRequest = true;
    };
    eraseRequest (incomingRequests);
    eraseRequest (outgoingRequests);

    if (isFriend (id))
    {
        if (clearedRequest)
        {
            save();
            if (onFriendsChanged) onFriendsChanged();
        }
        return;
    }

    Friend f;
    f.id = id;
    f.name = name;
    if (auto it = presence.find (id); it != presence.end())
    {
        f.lastSeenMs = it->second.lastBeaconMs;
        f.lastAddress = it->second.address;
        if (f.name.isEmpty())
            f.name = it->second.name;
    }
    friends.push_back (std::move (f));
    save();
    if (onFriendsChanged) onFriendsChanged();
}

void FriendsManager::addIncomingRequest (juce::int64 id, const juce::String& name)
{
    if (id == 0 || isFriend (id) || hasIncomingRequest (id))
        return;

    Request r;
    r.id = id;
    r.name = name;
    r.sinceMs = juce::Time::currentTimeMillis();
    incomingRequests.push_back (std::move (r));
    save();
    if (onFriendsChanged) onFriendsChanged();
}

void FriendsManager::removeIncomingRequest (juce::int64 id)
{
    auto before = incomingRequests.size();
    incomingRequests.erase (std::remove_if (incomingRequests.begin(), incomingRequests.end(),
                                            [id] (const Request& r) { return r.id == id; }),
                            incomingRequests.end());
    if (incomingRequests.size() != before)
    {
        save();
        if (onFriendsChanged) onFriendsChanged();
    }
}

bool FriendsManager::hasIncomingRequest (juce::int64 id) const
{
    for (auto& r : incomingRequests)
        if (r.id == id)
            return true;
    return false;
}

void FriendsManager::addOutgoingRequest (juce::int64 id, const juce::String& name)
{
    if (id == 0 || isFriend (id) || hasOutgoingRequest (id))
        return;

    Request r;
    r.id = id;
    r.name = name;
    r.sinceMs = juce::Time::currentTimeMillis();
    outgoingRequests.push_back (std::move (r));
    save();
    if (onFriendsChanged) onFriendsChanged();
}

void FriendsManager::clearOutgoingRequest (juce::int64 id)
{
    auto before = outgoingRequests.size();
    outgoingRequests.erase (std::remove_if (outgoingRequests.begin(), outgoingRequests.end(),
                                            [id] (const Request& r) { return r.id == id; }),
                            outgoingRequests.end());
    if (outgoingRequests.size() != before)
    {
        save();
        if (onFriendsChanged) onFriendsChanged();
    }
}

bool FriendsManager::hasOutgoingRequest (juce::int64 id) const
{
    for (auto& r : outgoingRequests)
        if (r.id == id)
            return true;
    return false;
}

std::vector<FriendsManager::Request> FriendsManager::getIncomingRequests() const
{
    return incomingRequests;
}

std::vector<FriendsManager::Request> FriendsManager::getOutgoingRequests() const
{
    return outgoingRequests;
}

void FriendsManager::noteServerLastSeen (juce::int64 id, juce::int64 lastSeenMs,
                                         const juce::String& name)
{
    for (auto& f : friends)
        if (f.id == id)
        {
            if (lastSeenMs > f.lastSeenMs)
            {
                f.lastSeenMs = lastSeenMs;
                if (f.name.isEmpty() && name.isNotEmpty())
                    f.name = name;
                dirty = true;
                if (onFriendsChanged) onFriendsChanged();
            }
            return;
        }
}

void FriendsManager::timerCallback()
{
    auto now = juce::Time::currentTimeMillis();
    for (auto& [id, p] : presence)
    {
        auto age = now - p.lastBeaconMs;
        p.online = age <= onlineWindowMs ? true : (p.online && age <= offlineWindowMs);
    }

    std::set<juce::int64> nowOnline;
    for (auto& f : friends)
        if (isOnline (f.id))
            nowOnline.insert (f.id);

    bool suppress = juce::Time::currentTimeMillis() - startupMs < 5000;
    bool changed = false;

    if (! suppress)
    {
        auto fire = [this] (juce::int64 id, bool online)
        {
            if (! onFriendPresenceChanged)
                return;
            juce::String name;
            if (auto* f = findFriend (id))
                name = f->name;
            onFriendPresenceChanged (id, name, online);
        };

        for (auto id : nowOnline)
            if (onlineFriendIds.find (id) == onlineFriendIds.end())
            {
                changed = true;
                fire (id, true);
            }

        for (auto id : onlineFriendIds)
            if (nowOnline.find (id) == nowOnline.end())
            {
                changed = true;
                fire (id, false);
            }
    }

    onlineFriendIds = std::move (nowOnline);

    if (changed && onFriendsChanged)
        onFriendsChanged();
}

void FriendsManager::removeFriend (juce::int64 id)
{
    auto before = friends.size();
    friends.erase (std::remove_if (friends.begin(), friends.end(),
                                   [id] (const Friend& f) { return f.id == id; }),
                   friends.end());
    if (friends.size() != before)
    {
        save();
        if (onFriendsChanged) onFriendsChanged();
    }
}

void FriendsManager::notePresence (juce::int64 id, const juce::String& name,
                                   const juce::String& status, int people,
                                   const juce::String& address, int sessionPort, bool viaRelay,
                                   const juce::String& inviteCode, bool afk, juce::int64 afkSinceMs)
{
    if (id == 0)
        return;

    auto now = juce::Time::currentTimeMillis();
    auto existing = presence.find (id);
    bool wasOnline = existing != presence.end() && existing->second.online;
    auto& p = presence[id];
    p.id = id;
    p.name = name;
    p.status = status;
    p.people = juce::jmax (1, people);
    p.sessionPort = juce::jlimit (0, 65535, sessionPort);
    p.lastBeaconMs = now;
    p.online = true;
    if (! wasOnline)
        p.onlineSinceMs = now;

    if (! afk)
    {
        p.afkSinceMs = 0;
    }
    else
    {
        if (afkSinceMs <= 0)
            afkSinceMs = p.afk && p.afkSinceMs > 0 ? p.afkSinceMs : now;
        p.afkSinceMs = afkSinceMs < now - 24 * 60 * 60 * 1000 ? now : juce::jmin (afkSinceMs, now);
    }
    p.afk = afk;

    if (viaRelay)
    {
        p.relayAddress = address;
        p.inviteCode = inviteCode;
    }
    else
    {
        p.address = address;
        p.lanBeaconMs = now;
    }

    for (auto& f : friends)
        if (f.id == id)
        {
            if (name.isNotEmpty() && f.name != name)   { f.name = name; dirty = true; }
            if (! viaRelay && address.isNotEmpty() && f.lastAddress != address) { f.lastAddress = address; dirty = true; }
            f.lastSeenMs = now;
            dirty = true;
            break;
        }

    if (onFriendsChanged) onFriendsChanged();
}

std::vector<FriendsManager::Entry> FriendsManager::getFriends() const
{
    std::vector<Entry> out;
    out.reserve (friends.size());
    for (auto& f : friends)
    {
        Entry e;
        e.info = f;
        if (auto it = presence.find (f.id); it != presence.end() && isOnline (f.id))
        {
            e.online = true;
            e.status = it->second.status;
            e.address = it->second.address;
            e.people = it->second.people;
            e.sessionPort = it->second.sessionPort;
            e.onlineSinceMs = it->second.onlineSinceMs;
            e.afk = it->second.afk;
            e.afkSinceMs = it->second.afkSinceMs;
        }
        out.push_back (std::move (e));
    }

    std::stable_sort (out.begin(), out.end(), [] (const Entry& a, const Entry& b)
    {
        if (a.online != b.online)
            return a.online;
        return a.info.name.compareIgnoreCase (b.info.name) < 0;
    });
    return out;
}

std::vector<FriendsManager::Presence> FriendsManager::getNearbyNonFriends() const
{
    std::vector<Presence> out;
    for (auto& [id, p] : presence)
        if (! isFriend (id) && isOnline (id))
            out.push_back (p);

    std::stable_sort (out.begin(), out.end(), [] (const Presence& a, const Presence& b)
    {
        return a.name.compareIgnoreCase (b.name) < 0;
    });
    return out;
}

juce::String FriendsManager::inviteCodeFor (juce::int64 id) const
{
    if (auto it = presence.find (id); it != presence.end() && isOnline (id))
        return it->second.inviteCode;
    return {};
}

juce::String FriendsManager::addressFor (juce::int64 id) const
{
    if (auto it = presence.find (id); it != presence.end() && isOnline (id))
    {
        if (juce::Time::currentTimeMillis() - it->second.lanBeaconMs <= offlineWindowMs)
            return it->second.address;
        if (it->second.relayAddress.isNotEmpty())
            return it->second.relayAddress;
    }
    if (auto* f = findFriend (id))
        return f->lastAddress;
    return {};
}

juce::String FriendsManager::lanAddressFor (juce::int64 id) const
{
    if (auto it = presence.find (id); it != presence.end() && isOnline (id)
        && juce::Time::currentTimeMillis() - it->second.lanBeaconMs <= offlineWindowMs)
        return it->second.address;
    if (auto* f = findFriend (id))
        return f->lastAddress;
    return {};
}

int FriendsManager::sessionPortFor (juce::int64 id) const
{
    if (auto it = presence.find (id); it != presence.end() && isOnline (id))
        return it->second.sessionPort;
    return 0;
}

void FriendsManager::cacheAvatar (juce::int64 id, const juce::MemoryBlock& encodedImage)
{
    if (id == 0 || encodedImage.getSize() == 0)
        return;

    auto image = juce::ImageFileFormat::loadFrom (encodedImage.getData(), encodedImage.getSize());
    if (! image.isValid())
        return;

    avatarFolder.createDirectory();
    auto target = avatarFolder.getChildFile (juce::String (id) + ".png");
    if (! target.replaceWithData (encodedImage.getData(), encodedImage.getSize()))
        return;
    avatarCache[id] = image;
    avatarHashCache[id] = juce::MD5 (encodedImage.getData(), encodedImage.getSize()).toHexString();
    if (onFriendsChanged)
        onFriendsChanged();
}

juce::String FriendsManager::avatarHashFor (juce::int64 id) const
{
    if (auto it = avatarHashCache.find (id); it != avatarHashCache.end())
        return it->second;

    auto fileToLoad = avatarFolder.getChildFile (juce::String (id) + ".png");
    juce::MemoryBlock bytes;
    if (! fileToLoad.loadFileAsData (bytes) || bytes.getSize() == 0)
        return {};
    auto hash = juce::MD5 (bytes.getData(), bytes.getSize()).toHexString();
    avatarHashCache[id] = hash;
    return hash;
}

juce::Image FriendsManager::avatarFor (juce::int64 id) const
{
    if (auto it = avatarCache.find (id); it != avatarCache.end())
        return it->second;

    auto fileToLoad = avatarFolder.getChildFile (juce::String (id) + ".png");
    auto image = juce::ImageFileFormat::loadFrom (fileToLoad);
    if (image.isValid())
        avatarCache[id] = image;
    return image;
}

int FriendsManager::numOnline() const
{
    int n = 0;
    for (auto& f : friends)
        if (isOnline (f.id))
            ++n;
    return n;
}
