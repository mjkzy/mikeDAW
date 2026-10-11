#include "RelayClient.h"

RelayClient::RelayClient()
    : juce::Thread ("Relay Client")
{
}

RelayClient::~RelayClient()
{
    stop();
}

void RelayClient::configure (const juce::String& h, int p, bool en)
{
    bool changed = false;
    {
        const juce::ScopedLock sl (lock);
        if (h != host || p != port || en != enabled)
            changed = true;
        host = h;
        port = p;
        enabled = en;
        if (changed)
            configDirty = true;
    }
    if (changed)
        notify();
}

void RelayClient::start (juce::int64 uid)
{
   #if OPENDAW_PUBLIC_BUILD
    juce::ignoreUnused (uid);
    return;
   #endif
    {
        const juce::ScopedLock sl (lock);
        userId = uid;
        running = true;
        configDirty = true;
    }
    startTimer (1000);
    if (! isThreadRunning())
        startThread();
    notify();
}

void RelayClient::stop()
{
    {
        const juce::ScopedLock sl (lock);
        running = false;
    }
    stopTimer();
    signalThreadShouldExit();
    notify();
    socket.close();
    stopThread (3000);
    connected = false;
    {
        const juce::ScopedLock sl (lock);
        outQueue.clear();
        inQueue.clear();
    }
    lastReportedConnected = false;
}

bool RelayClient::isConnected() const
{
    return connected.load();
}

juce::String RelayClient::getHost() const
{
    const juce::ScopedLock sl (lock);
    return host;
}

int RelayClient::getPort() const
{
    const juce::ScopedLock sl (lock);
    return port;
}

void RelayClient::run()
{
    std::vector<char> backlog;
    int backoffMs = reconnectMinMs;

    while (! threadShouldExit())
    {
        juce::String h;
        int p;
        bool en, run_, dirty;
        {
            const juce::ScopedLock sl (lock);
            h = host;
            p = port;
            en = enabled;
            run_ = running;
            dirty = configDirty;
            configDirty = false;
        }

        if (dirty && socket.isConnected())
        {
            socket.close();
            connected = false;
            backlog.clear();
        }

        if (! (en && run_))
        {
            if (socket.isConnected())
            {
                socket.close();
                connected = false;
                backlog.clear();
            }
            wait (500);
            continue;
        }

        if (! socket.isConnected())
        {
            connected = false;
            if (! socket.connect (h, p, 4000))
            {
                wait (backoffMs);
                backoffMs = juce::jmin (backoffMs * 2, reconnectMaxMs);
                continue;
            }
            backoffMs = reconnectMinMs;
            backlog.clear();
            {
                const juce::ScopedLock sl (lock);
                handshakeNeeded = true;
                awaitingHello = true;
            }
            connected = true;
        }

        {
            bool skipFlush;
            std::vector<juce::String> lines;
            {
                const juce::ScopedLock sl (lock);
                skipFlush = awaitingHello;
                if (! skipFlush)
                    lines.swap (outQueue);
            }
            if (! skipFlush)
            {
                bool writeFailed = false;
                for (auto& l : lines)
                {
                    auto s = l + "\n";
                    auto len = (int) s.getNumBytesAsUTF8();
                    if (socket.write (s.toRawUTF8(), len) != len)
                    {
                        writeFailed = true;
                        break;
                    }
                }
                if (writeFailed)
                {
                    socket.close();
                    connected = false;
                    backlog.clear();
                    continue;
                }
            }
        }

        int ready = socket.waitUntilReady (true, 200);
        if (ready < 0)
        {
            socket.close();
            connected = false;
            backlog.clear();
            continue;
        }
        if (ready == 0)
            continue;

        char buf[2048];
        int n = socket.read (buf, sizeof (buf), false);
        if (n <= 0)
        {
            socket.close();
            connected = false;
            backlog.clear();
            continue;
        }

        backlog.insert (backlog.end(), buf, buf + n);
        size_t start = 0;
        for (size_t i = 0; i < backlog.size(); ++i)
        {
            if (backlog[i] != '\n')
                continue;
            auto line = juce::String::fromUTF8 (backlog.data() + start, (int) (i - start)).trim();
            start = i + 1;
            if (line.isNotEmpty())
            {
                const juce::ScopedLock sl (lock);
                inQueue.push_back (line);
            }
        }
        backlog.erase (backlog.begin(), backlog.begin() + (std::ptrdiff_t) start);
        if (backlog.size() > 65536)
            backlog.clear();
    }
}

void RelayClient::timerCallback()
{
    bool conn = connected.load();
    if (conn != lastReportedConnected)
    {
        lastReportedConnected = conn;
        if (! conn)
            presenceTick = 0;
        if (onConnectionChanged)
            onConnectionChanged (conn);
    }

    bool doHandshake = false;
    {
        const juce::ScopedLock sl (lock);
        if (handshakeNeeded)
        {
            handshakeNeeded = false;
            doHandshake = true;
        }
    }

    if (doHandshake)
    {
        pushHandshake();
        presenceTick = 0;
        pendingStateChange = false;
        pendingFriendsPush = false;
    }
    else if (conn && (pendingStateChange || pendingFriendsPush
                      || ++presenceTick >= presenceIntervalMs / 1000))
    {
        presenceTick = 0;
        pendingStateChange = false;
        enqueue (makePresence());
        enqueue (makeWatch());
        if (pendingFriendsPush)
        {
            pendingFriendsPush = false;
            enqueue (makeFriends());
        }
    }

    drainInbound();
}

void RelayClient::pushHandshake()
{
    juce::String hello = makeHello();
    juce::String presence = makePresence();
    juce::String watch = makeWatch();
    juce::String friendList = makeFriends();
    {
        const juce::ScopedLock sl (lock);
        outQueue.insert (outQueue.begin(), { hello, presence, watch, friendList });
        awaitingHello = false;
    }
    notify();

    if (avatarProvider)
    {
        auto bytes = avatarProvider();
        if (bytes.getSize() > 0 && (int) bytes.getSize() <= maxAvatarBytes)
        {
            juce::DynamicObject::Ptr o = new juce::DynamicObject();
            o->setProperty ("t", "avatar");
            o->setProperty ("data", juce::Base64::toBase64 (bytes.getData(), bytes.getSize()));
            enqueue (juce::JSON::toString (juce::var (o.get()), true));
        }
    }
}

void RelayClient::enqueue (const juce::String& line)
{
   #if OPENDAW_PUBLIC_BUILD
    juce::ignoreUnused (line);
    return;
   #endif
    {
        const juce::ScopedLock sl (lock);
        outQueue.push_back (line);
    }
    notify();
}

juce::String RelayClient::makeHello() const
{
    juce::int64 id;
    {
        const juce::ScopedLock sl (lock);
        id = userId;
    }
    juce::String name = nameProvider ? nameProvider() : juce::String ("A Kind Soul");

    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty ("t", "hello");
    o->setProperty ("v", 1);
    o->setProperty ("id", id);
    o->setProperty ("name", name);
    return juce::JSON::toString (juce::var (o.get()), true);
}

juce::String RelayClient::makePresence() const
{
    juce::String status = statusProvider ? statusProvider() : juce::String ("In the studio");
    int people = peopleProvider ? juce::jmax (1, peopleProvider()) : 1;
    int sessionPort = sessionPortProvider ? juce::jlimit (0, 65535, sessionPortProvider()) : 0;
    juce::String code = inviteCodeProvider ? inviteCodeProvider() : juce::String();
    juce::String joinToken = joinTokenProvider ? joinTokenProvider() : juce::String();
    bool afk = afkProvider && afkProvider();
    juce::int64 afkSince = afk && afkSinceProvider ? afkSinceProvider() : 0;

    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty ("t", "presence");
    o->setProperty ("status", status);
    o->setProperty ("people", people);
    o->setProperty ("sessionPort", sessionPort);
    o->setProperty ("code", code);
    o->setProperty ("afk", afk);
    o->setProperty ("afkSince", afkSince);
    o->setProperty ("joinToken", joinToken);
    return juce::JSON::toString (juce::var (o.get()), true);
}

juce::String RelayClient::makeWatch() const
{
    juce::Array<juce::var> ids;
    if (watchIdsProvider)
        for (auto id : watchIdsProvider())
            ids.add (juce::var (id));

    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty ("t", "watch");
    o->setProperty ("ids", juce::var (ids));
    return juce::JSON::toString (juce::var (o.get()), true);
}

juce::String RelayClient::makeFriends() const
{
    juce::Array<juce::var> ids;
    if (friendIdsProvider)
        for (auto id : friendIdsProvider())
            ids.add (juce::var (id));

    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty ("t", "friends");
    o->setProperty ("ids", juce::var (ids));
    return juce::JSON::toString (juce::var (o.get()), true);
}

void RelayClient::sendFriendRequest (juce::int64 toId)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty ("t", "freq");
    o->setProperty ("to", toId);
    enqueue (juce::JSON::toString (juce::var (o.get()), true));
}

void RelayClient::respondFriendRequest (juce::int64 toId, bool accept)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty ("t", "fresp");
    o->setProperty ("to", toId);
    o->setProperty ("accept", accept);
    enqueue (juce::JSON::toString (juce::var (o.get()), true));
}

void RelayClient::sendInvite (juce::int64 toId, const juce::String& sessionName,
                              const juce::String& inviteCode, int port_)
{
    if (! connected.load())
        return;
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty ("t", "invite");
    o->setProperty ("to", toId);
    o->setProperty ("session", sessionName);
    o->setProperty ("code", inviteCode);
    o->setProperty ("port", port_);
    enqueue (juce::JSON::toString (juce::var (o.get()), true));
}

void RelayClient::sendJoinRequest (juce::int64 hostId, const juce::String& joinToken)
{
    if (! connected.load())
        return;
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty ("t", "joinreq");
    o->setProperty ("to", hostId);
    o->setProperty ("token", joinToken);
    enqueue (juce::JSON::toString (juce::var (o.get()), true));
}

void RelayClient::requestAvatar (juce::int64 id)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty ("t", "getavatar");
    o->setProperty ("id", id);
    enqueue (juce::JSON::toString (juce::var (o.get()), true));
}

void RelayClient::notifyStateChanged()
{
    pendingStateChange = true;
}

void RelayClient::notifyFriendsChanged()
{
    pendingFriendsPush = true;
}

void RelayClient::drainInbound()
{
    std::vector<juce::String> lines;
    {
        const juce::ScopedLock sl (lock);
        lines.swap (inQueue);
    }
    for (auto& l : lines)
        handleLine (l);
}

void RelayClient::handleLine (const juce::String& line)
{
    auto v = juce::JSON::parse (line);
    if (! v.isObject())
        return;

    auto t = v.getProperty ("t", juce::var()).toString();

    if (t == "presence")
    {
        auto id = (juce::int64) v.getProperty ("id", (juce::int64) 0);
        auto name = v.getProperty ("name", juce::var()).toString();
        bool online = (bool) v.getProperty ("online", false);
        if (online)
        {
            auto status = v.getProperty ("status", juce::var()).toString();
            int people = (int) v.getProperty ("people", 1);
            auto addr = v.getProperty ("addr", juce::var()).toString();
            int sp = (int) v.getProperty ("sessionPort", 0);
            auto code = v.getProperty ("code", juce::var()).toString();
            auto avatarHash = v.getProperty ("avatarHash", juce::var()).toString();
            bool afk = (bool) v.getProperty ("afk", false);
            auto afkSince = (juce::int64) v.getProperty ("afkSince", (juce::int64) 0);
            if (onPresence)
                onPresence (id, name, status, people, addr, sp, code, avatarHash, afk, afkSince);
        }
        else
        {
            auto lastSeen = (juce::int64) v.getProperty ("lastSeen", (juce::int64) 0);
            if (onFriendLastSeen)
                onFriendLastSeen (id, lastSeen, name);
        }
    }
    else if (t == "freq")
    {
        auto from = (juce::int64) v.getProperty ("from", (juce::int64) 0);
        auto name = v.getProperty ("name", juce::var()).toString();
        if (onFriendRequest)
            onFriendRequest (from, name);
    }
    else if (t == "fresp")
    {
        auto from = (juce::int64) v.getProperty ("from", (juce::int64) 0);
        auto name = v.getProperty ("name", juce::var()).toString();
        bool accept = (bool) v.getProperty ("accept", false);
        if (onFriendResponse)
            onFriendResponse (from, name, accept);
    }
    else if (t == "invite")
    {
        auto from = (juce::int64) v.getProperty ("from", (juce::int64) 0);
        auto name = v.getProperty ("name", juce::var()).toString();
        auto session = v.getProperty ("session", juce::var()).toString();
        auto code = v.getProperty ("code", juce::var()).toString();
        auto addr = v.getProperty ("addr", juce::var()).toString();
        int p = (int) v.getProperty ("port", 0);
        if (onInvite)
            onInvite (from, name, session, code, addr, p);
    }
    else if (t == "joinok")
    {
        if (onJoinOk)
            onJoinOk (v.getProperty ("token", juce::var()).toString());
    }
    else if (t == "joinfail")
    {
        if (onJoinFail)
            onJoinFail (v.getProperty ("reason", juce::var()).toString());
    }
    else if (t == "bridgereq")
    {
        auto token = v.getProperty ("token", juce::var()).toString();
        auto from = (juce::int64) v.getProperty ("from", (juce::int64) 0);
        auto name = v.getProperty ("name", juce::var()).toString();
        auto jt = v.getProperty ("jt", juce::var()).toString();
        if (onBridgeRequest && token.isNotEmpty())
            onBridgeRequest (token, from, name, jt);
    }
    else if (t == "avatar")
    {
        auto id = (juce::int64) v.getProperty ("id", (juce::int64) 0);
        auto data = v.getProperty ("data", juce::var()).toString();
        juce::MemoryOutputStream mos;
        if (id != 0 && juce::Base64::convertFromBase64 (mos, data)
            && mos.getDataSize() > 0 && (int) mos.getDataSize() <= maxAvatarBytes)
        {
            juce::MemoryBlock png (mos.getData(), mos.getDataSize());
            if (onAvatar)
                onAvatar (id, png);
        }
    }
}
