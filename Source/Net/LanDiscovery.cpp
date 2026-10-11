#include "LanDiscovery.h"

namespace
{
    const juce::String beaconMagic   = "OPENDAW1";
    const juce::String queryMagic    = "OPENDAW1?";
    const juce::String presenceMagic = "ODPRES1";
    const juce::String presenceQueryMagic = "ODPRES1?";
    const juce::String inviteMagic   = "ODINV1";
    const juce::String friendReqMagic = "ODFREQ1";
    const juce::String friendRspMagic = "ODFRSP1";

    juce::String sanitizeField (const juce::String& s)
    {
        return s.replaceCharacter ('|', ' ').substring (0, 64);
    }
}

LanDiscovery::LanDiscovery()
    : juce::Thread ("LAN Discovery")
{
}

LanDiscovery::~LanDiscovery()
{
    stopAdvertising();
    stopListening();
}

void LanDiscovery::startAdvertising (const juce::String& sessionName, int tcpPort)
{
    {
        const juce::ScopedLock sl (lock);
        advertName = sessionName;
        advertPort = tcpPort;
        advertising = true;
    }
    refreshLocalAddresses();
    sendBeacon();
    updateTimerState();
}

void LanDiscovery::stopAdvertising()
{
    {
        const juce::ScopedLock sl (lock);
        advertising = false;
    }
    updateTimerState();
}

void LanDiscovery::startListening()
{
    if (listening)
        return;

    listenSocket.bindToPort (discoveryPort, "0.0.0.0");
    refreshLocalAddresses();
    listening = true;
    startThread();
}

void LanDiscovery::stopListening()
{
    listening = false;
    setQuerying (false);
    signalThreadShouldExit();
    listenSocket.shutdown();
    stopThread (2000);
}

void LanDiscovery::setQuerying (bool shouldQuery)
{
    if (querying == shouldQuery)
        return;
    querying = shouldQuery;
    if (querying)
        sendQuery();
    updateTimerState();
}

void LanDiscovery::setPresenceQuerying (bool shouldQuery)
{
    if (presenceQuerying == shouldQuery)
        return;
    presenceQuerying = shouldQuery;
    if (presenceQuerying)
        sendPresenceQuery();
    updateTimerState();
}

void LanDiscovery::startPresence (juce::int64 userId)
{
    {
        const juce::ScopedLock sl (lock);
        myUserId = userId;
        presenceActive = true;
    }
    refreshLocalAddresses();
    sendPresence();
    updateTimerState();
}

void LanDiscovery::updateTimerState()
{
    bool needTimer;
    {
        const juce::ScopedLock sl (lock);
        needTimer = advertising || querying || presenceActive || presenceQuerying;
    }
    if (needTimer)
        startTimer (beaconIntervalMs);
    else
        stopTimer();
}

void LanDiscovery::refreshLocalAddresses()
{
    juce::StringArray addrs;
    for (auto& ip : juce::IPAddress::getAllAddresses (false))
        addrs.add (ip.toString());

    const juce::ScopedLock sl (lock);
    localAddresses = std::move (addrs);
}

void LanDiscovery::run()
{
    char buffer[512];
    while (! threadShouldExit())
    {
        auto ready = listenSocket.waitUntilReady (true, 500);
        if (ready < 0)
            break;
        if (ready == 0)
        {
            const juce::ScopedLock sl (lock);
            expireOldLocked();
            continue;
        }

        juce::String senderIp;
        int senderPort = 0;
        int n = listenSocket.read (buffer, sizeof (buffer) - 1, false, senderIp, senderPort);
        if (n <= 0)
            continue;

        buffer[n] = 0;
        auto text = juce::String::fromUTF8 (buffer, n);

        if (text.startsWith (queryMagic))
        {
            juce::String reply;
            {
                const juce::ScopedLock sl (lock);
                if (! advertising)
                    continue;
                reply = makeBeaconPayload();
            }
            writeTo (listenSocket, senderIp, senderPort, reply.toRawUTF8(),
                     (int) reply.getNumBytesAsUTF8());
            continue;
        }

        if (text.startsWith (presenceQueryMagic))
        {
            juce::String reply;
            {
                const juce::ScopedLock sl (lock);
                if (! presenceActive || lastPresencePayload.isEmpty()
                    || localAddresses.contains (senderIp))
                    continue;
                reply = lastPresencePayload;
            }
            writeTo (listenSocket, senderIp, senderPort, reply.toRawUTF8(),
                     (int) reply.getNumBytesAsUTF8());
            continue;
        }

        auto parts = juce::StringArray::fromTokens (text, "|", "");

        if (parts.size() >= 5 && parts[0] == presenceMagic)
        {
            InPresence ip;
            ip.id = parts[1].getLargeIntValue();
            ip.name = sanitizeField (parts[2]);
            ip.status = sanitizeField (parts[3]);
            ip.people = juce::jlimit (1, 64, parts[4].getIntValue());
            ip.sessionPort = parts.size() >= 6 ? juce::jlimit (0, 65535, parts[5].getIntValue()) : 0;
            ip.afk = parts.size() >= 7 && parts[6].getIntValue() != 0;
            ip.afkSinceMs = parts.size() >= 8 ? parts[7].getLargeIntValue() : 0;
            ip.address = senderIp;
            const juce::ScopedLock sl (lock);
            if (ip.id == 0 || ip.id == myUserId)
                continue;
            pendingPresence.push_back (std::move (ip));
            continue;
        }

        if (parts.size() >= 5 && parts[0] == inviteMagic)
        {
            InInvite iv;
            iv.fromId = parts[1].getLargeIntValue();
            iv.name = sanitizeField (parts[2]);
            iv.sessionName = sanitizeField (parts[3]);
            iv.port = parts[4].getIntValue();
            iv.address = senderIp;
            if (iv.fromId == 0 || iv.port <= 0 || iv.port > 65535)
                continue;
            const juce::ScopedLock sl (lock);
            if (iv.fromId == myUserId)
                continue;
            pendingInvites.push_back (std::move (iv));
            continue;
        }

        if (parts.size() >= 3 && parts[0] == friendReqMagic)
        {
            InFriendReq fr;
            fr.fromId = parts[1].getLargeIntValue();
            fr.name = sanitizeField (parts[2]);
            fr.address = senderIp;
            const juce::ScopedLock sl (lock);
            if (fr.fromId == 0 || fr.fromId == myUserId)
                continue;
            pendingFriendReq.push_back (std::move (fr));
            continue;
        }

        if (parts.size() >= 4 && parts[0] == friendRspMagic)
        {
            InFriendRsp fr;
            fr.fromId = parts[1].getLargeIntValue();
            fr.name = sanitizeField (parts[2]);
            fr.accepted = parts[3].getIntValue() != 0;
            const juce::ScopedLock sl (lock);
            if (fr.fromId == 0 || fr.fromId == myUserId)
                continue;
            pendingFriendRsp.push_back (std::move (fr));
            continue;
        }

        if (parts.size() < 3 || parts[0] != beaconMagic)
            continue;

        Entry e;
        e.name = parts[1];
        e.address = senderIp;
        e.port = parts[2].getIntValue();
        e.people = parts.size() >= 4 ? juce::jmax (1, parts[3].getIntValue()) : 1;
        e.lastSeenMs = juce::Time::getMillisecondCounter();
        if (e.port <= 0)
            continue;

        const juce::ScopedLock sl (lock);

        if (advertising && e.port == advertPort && localAddresses.contains (e.address))
            continue;

        bool merged = false;
        for (auto& existing : sessions)
            if (existing.address == e.address && existing.port == e.port)
            {
                existing = e;
                merged = true;
                break;
            }
        if (! merged)
            sessions.add (e);

        expireOldLocked();
    }
}

void LanDiscovery::expireOldLocked()
{
    auto now = juce::Time::getMillisecondCounter();
    for (int i = sessions.size(); --i >= 0;)
        if (now - sessions[i].lastSeenMs > (juce::uint32) entryTimeoutMs)
            sessions.remove (i);
}

void LanDiscovery::timerCallback()
{
    bool advertise, presence;
    {
        const juce::ScopedLock sl (lock);
        advertise = advertising;
        presence = presenceActive;
    }
    if (advertise)
        sendBeacon();
    if (querying)
        sendQuery();
    if (presence && (++presenceTick % (presenceIntervalMs / beaconIntervalMs)) == 0)
    {
        sendPresence();
        if (presenceQuerying)
            sendPresenceQuery();
    }

    drainInbound();
}

void LanDiscovery::drainInbound()
{
    std::vector<InPresence> pres;
    std::vector<InInvite> inv;
    std::vector<InFriendReq> freqs;
    std::vector<InFriendRsp> frsps;
    {
        const juce::ScopedLock sl (lock);
        pres.swap (pendingPresence);
        inv.swap (pendingInvites);
        freqs.swap (pendingFriendReq);
        frsps.swap (pendingFriendRsp);
    }
    if (onPresence)
        for (auto& p : pres)
            onPresence (p.id, p.name, p.status, p.people, p.address, p.sessionPort, p.afk, p.afkSinceMs);
    if (onInviteReceived)
        for (auto& i : inv)
            onInviteReceived (i.fromId, i.name, i.sessionName, i.address, i.port);
    if (onFriendRequest)
        for (auto& f : freqs)
            onFriendRequest (f.fromId, f.name, f.address);
    if (onFriendResponse)
        for (auto& f : frsps)
            onFriendResponse (f.fromId, f.name, f.accepted);
}

void LanDiscovery::sendPresenceNow()
{
    sendPresence();
}

void LanDiscovery::sendPresence()
{
    juce::int64 uid;
    {
        const juce::ScopedLock sl (lock);
        if (! presenceActive)
            return;
        uid = myUserId;
    }
    int people = peopleProvider != nullptr ? juce::jmax (1, peopleProvider()) : 1;
    juce::String name   = presenceNameProvider   != nullptr ? sanitizeField (presenceNameProvider())   : juce::String ("A Kind Soul");
    juce::String status = presenceStatusProvider != nullptr ? sanitizeField (presenceStatusProvider()) : juce::String ("In the studio");
    int sessionPort = presenceSessionPortProvider != nullptr
                    ? juce::jlimit (0, 65535, presenceSessionPortProvider()) : 0;
    bool afk = presenceAfkProvider != nullptr && presenceAfkProvider();
    juce::int64 afkSince = afk && presenceAfkSinceProvider != nullptr ? presenceAfkSinceProvider() : 0;

    juce::String payload = presenceMagic + "|" + juce::String (uid) + "|" + name
                         + "|" + status + "|" + juce::String (people)
                         + "|" + juce::String (sessionPort)
                         + "|" + juce::String (afk ? 1 : 0)
                         + "|" + juce::String (afkSince);

    {
        const juce::ScopedLock sl (lock);
        lastPresencePayload = payload;
    }

    if (! sendSocketBound)
        sendSocketBound = sendSocket.bindToPort (0);
    broadcastTo (sendSocket, payload);
}

void LanDiscovery::sendInvite (const juce::String& address, juce::int64 fromUserId,
                               const juce::String& fromName, const juce::String& sessionName, int tcpPort)
{
    if (address.isEmpty() || tcpPort <= 0)
        return;
    juce::String payload = inviteMagic + "|" + juce::String (fromUserId) + "|"
                         + sanitizeField (fromName) + "|" + sanitizeField (sessionName)
                         + "|" + juce::String (tcpPort);

    if (! sendSocketBound)
        sendSocketBound = sendSocket.bindToPort (0);
    sendSocket.write (address, discoveryPort, payload.toRawUTF8(),
                      (int) payload.getNumBytesAsUTF8());
}

void LanDiscovery::sendFriendRequest (const juce::String& address, juce::int64 fromUserId,
                                      const juce::String& fromName)
{
    if (address.isEmpty() || fromUserId == 0)
        return;
    juce::String payload = friendReqMagic + "|" + juce::String (fromUserId)
                         + "|" + sanitizeField (fromName);

    if (! sendSocketBound)
        sendSocketBound = sendSocket.bindToPort (0);
    sendSocket.write (address, discoveryPort, payload.toRawUTF8(),
                      (int) payload.getNumBytesAsUTF8());
}

void LanDiscovery::sendFriendResponse (const juce::String& address, juce::int64 fromUserId,
                                       const juce::String& fromName, bool accepted)
{
    if (address.isEmpty() || fromUserId == 0)
        return;
    juce::String payload = friendRspMagic + "|" + juce::String (fromUserId)
                         + "|" + sanitizeField (fromName)
                         + "|" + juce::String (accepted ? 1 : 0);

    if (! sendSocketBound)
        sendSocketBound = sendSocket.bindToPort (0);
    sendSocket.write (address, discoveryPort, payload.toRawUTF8(),
                      (int) payload.getNumBytesAsUTF8());
}

juce::String LanDiscovery::makeBeaconPayload() const
{
    int people = peopleProvider != nullptr ? juce::jmax (1, peopleProvider()) : 1;
    return beaconMagic + "|" + advertName + "|" + juce::String (advertPort)
                       + "|" + juce::String (people);
}

void LanDiscovery::broadcastTo (juce::DatagramSocket& socket, const juce::String& payload)
{
    auto bytes = payload.toRawUTF8();
    auto len = (int) payload.getNumBytesAsUTF8();

    juce::StringArray targets;
    for (auto& ip : juce::IPAddress::getAllAddresses (false))
    {
        if (ip.isNull() || ip.toString() == "127.0.0.1")
            continue;
        auto bcast = juce::IPAddress::getInterfaceBroadcastAddress (ip);
        if (! bcast.isNull())
            targets.addIfNotAlreadyThere (bcast.toString());
    }
    targets.addIfNotAlreadyThere ("255.255.255.255");

    for (auto& t : targets)
        writeTo (socket, t, discoveryPort, bytes, len);
}

void LanDiscovery::writeTo (juce::DatagramSocket& socket, const juce::String& address, int port,
                            const void* bytes, int len)
{
    const juce::ScopedLock sl (socketWriteLock);
    socket.write (address, port, bytes, len);
}

void LanDiscovery::sendBeacon()
{
    juce::String payload;
    {
        const juce::ScopedLock sl (lock);
        if (! advertising)
            return;
        payload = makeBeaconPayload();
    }

    if (! sendSocketBound)
        sendSocketBound = sendSocket.bindToPort (0);

    broadcastTo (sendSocket, payload);
}

void LanDiscovery::sendQuery()
{
    broadcastTo (listenSocket, queryMagic);
    sweepSubnetsWith (queryMagic);
}

void LanDiscovery::sendPresenceQuery()
{
    broadcastTo (listenSocket, presenceQueryMagic);
    sweepSubnetsWith (presenceQueryMagic);
}

void LanDiscovery::sweepSubnetsWith (const juce::String& payload)
{
    auto bytes = payload.toRawUTF8();
    auto len = (int) payload.getNumBytesAsUTF8();

    for (auto& ip : juce::IPAddress::getAllAddresses (false))
    {
        auto s = ip.toString();
        if (ip.isNull() || s == "127.0.0.1")
            continue;

        auto octets = juce::StringArray::fromTokens (s, ".", "");
        if (octets.size() != 4)
            continue;

        auto prefix = octets[0] + "." + octets[1] + "." + octets[2] + ".";
        int selfHost = octets[3].getIntValue();
        for (int host = 1; host <= 254; ++host)
        {
            if (host == selfHost)
                continue;
            writeTo (listenSocket, prefix + juce::String (host), discoveryPort, bytes, len);
        }
    }
}

juce::Array<LanDiscovery::Entry> LanDiscovery::getSessions() const
{
    const juce::ScopedLock sl (lock);
    auto now = juce::Time::getMillisecondCounter();
    juce::Array<Entry> live;
    for (auto& e : sessions)
        if (now - e.lastSeenMs <= (juce::uint32) entryTimeoutMs)
            live.add (e);
    return live;
}
