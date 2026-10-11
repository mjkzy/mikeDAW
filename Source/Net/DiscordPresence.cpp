#include "DiscordPresence.h"

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

namespace
{
    enum Op { opHandshake = 0, opFrame = 1, opClose = 2, opPing = 3, opPong = 4 };

    constexpr int minSendGapMs   = 5000;
    constexpr int reconnectMs    = 15000;
    constexpr int badClientIdMs  = 60000;
}

DiscordPresence::DiscordPresence() : juce::Thread ("mikeDAW Discord RPC") {}

DiscordPresence::~DiscordPresence()
{
    alive->store (false);
    signalThreadShouldExit();
    wake.signal();
    stopThread (2000);
    closePipe();
}

void DiscordPresence::start (bool enabled)
{
    if (! enabled)
    {
        DBG ("DiscordPresence: dormant (disabled)");
        return;
    }
    startThread (juce::Thread::Priority::low);
}

void DiscordPresence::setActivity (const Activity& activity)
{
    if (! isThreadRunning())
        return;
    {
        const juce::ScopedLock sl (stateLock);
        pending    = activity;
        hasPending = true;
    }
    wake.signal();
}

void DiscordPresence::acceptJoinRequest (const juce::String& userId)
{
    queueCommand ("SEND_ACTIVITY_JOIN_INVITE", userId);
}

void DiscordPresence::declineJoinRequest (const juce::String& userId)
{
    queueCommand ("CLOSE_ACTIVITY_JOIN_REQUEST", userId);
}

void DiscordPresence::queueCommand (const juce::String& cmd, const juce::String& userId)
{
    if (! isThreadRunning() || userId.isEmpty())
        return;

    juce::DynamicObject::Ptr args = new juce::DynamicObject();
    args->setProperty ("user_id", userId);

    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty ("cmd", cmd);
    root->setProperty ("args", juce::var (args.get()));
    root->setProperty ("nonce", juce::Uuid().toString());

    {
        const juce::ScopedLock sl (stateLock);
        outCommands.push_back (juce::JSON::toString (juce::var (root.get()), true));
    }
    wake.signal();
}

void DiscordPresence::flushCommands()
{
    std::vector<juce::String> cmds;
    {
        const juce::ScopedLock sl (stateLock);
        cmds.swap (outCommands);
    }
    for (auto& c : cmds)
        if (! sendFrame (opFrame, c))
            break;
}

bool DiscordPresence::sameActivity (const Activity& a, const Activity& b)
{
    return a.details == b.details && a.state == b.state
        && a.startEpochSeconds == b.startEpochSeconds
        && a.partyId == b.partyId && a.partySize == b.partySize && a.partyMax == b.partyMax
        && a.joinSecret == b.joinSecret;
}

void DiscordPresence::run()
{
    juce::uint32 lastSendMs = 0;

    while (! threadShouldExit())
    {
        if (pipe == nullptr)
        {
            if (! connectPipe())
            {
                wake.wait (reconnectMs);
                continue;
            }
            if (! handshake())
            {
                closePipe();
                wake.wait (badClientIdMs);
                continue;
            }
            subscribeJoinEvents();
            DBG ("DiscordPresence: connected + READY");
            lastValid = false;
        }

        wake.wait (1000);
        if (threadShouldExit())
            break;

        int op; juce::String js;
        while (readFrame (op, js, 0))
        {
            if (op == opPing)
                sendFrame (opPong, js);
            else if (op == opFrame)
                handleDispatch (js);
            else if (op == opClose)
                closePipe();

            if (pipe == nullptr)
                break;
        }
        if (pipe == nullptr)
            continue;

        flushCommands();
        if (pipe == nullptr)
            continue;

        Activity a; bool have;
        {
            const juce::ScopedLock sl (stateLock);
            a = pending; have = hasPending;
        }
        if (! have)
            continue;

        if (lastValid && sameActivity (a, last))
            continue;

        const juce::uint32 nowMs = juce::Time::getMillisecondCounter();
        const juce::uint32 since = nowMs - lastSendMs;
        if (lastSendMs != 0 && since < (juce::uint32) minSendGapMs)
        {
            wake.wait ((int) (minSendGapMs - since));
            continue;
        }

        sendActivity (a);
        if (pipe == nullptr)
            continue;

        last = a; lastValid = true;
        lastSendMs = juce::Time::getMillisecondCounter();
        DBG ("DiscordPresence: activity '" << a.details << "' / '" << a.state << "'");
    }

    closePipe();
}

bool DiscordPresence::connectPipe()
{
   #if JUCE_WINDOWS
    for (int i = 0; i < 10; ++i)
    {
        auto name = juce::String ("\\\\.\\pipe\\discord-ipc-") + juce::String (i);
        HANDLE h = CreateFileW (name.toWideCharPointer(), GENERIC_READ | GENERIC_WRITE,
                                0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            pipe = (void*) h;
            return true;
        }
    }
   #endif
    return false;
}

void DiscordPresence::closePipe()
{
   #if JUCE_WINDOWS
    if (pipe != nullptr)
    {
        CloseHandle ((HANDLE) pipe);
        pipe = nullptr;
    }
   #endif
}

bool DiscordPresence::handshake()
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty ("v", 1);
    o->setProperty ("client_id", juce::String (clientAppId));
    if (! sendFrame (opHandshake, juce::JSON::toString (juce::var (o.get()), true)))
        return false;

    int op; juce::String js;
    if (! readFrame (op, js, 3000))
        return false;
    if (op == opClose)
        return false;

    auto evt = juce::JSON::parse (js) ["evt"].toString();
    return evt.isEmpty() || evt == "READY";
}

void DiscordPresence::subscribeJoinEvents()
{
    for (auto* evt : { "ACTIVITY_JOIN", "ACTIVITY_JOIN_REQUEST" })
    {
        juce::DynamicObject::Ptr root = new juce::DynamicObject();
        root->setProperty ("cmd", "SUBSCRIBE");
        root->setProperty ("evt", evt);
        root->setProperty ("args", juce::var (new juce::DynamicObject()));
        root->setProperty ("nonce", juce::Uuid().toString());
        if (! sendFrame (opFrame, juce::JSON::toString (juce::var (root.get()), true)))
            return;
    }
}

void DiscordPresence::handleDispatch (const juce::String& json)
{
    auto v = juce::JSON::parse (json);
    if (! v.isObject())
        return;

    auto evt = v.getProperty ("evt", juce::var()).toString();
    if (evt != "ACTIVITY_JOIN" && evt != "ACTIVITY_JOIN_REQUEST")
        return;

    auto data = v.getProperty ("data", juce::var());
    if (! data.isObject())
        return;

    auto flag = alive;

    if (evt == "ACTIVITY_JOIN")
    {
        auto secret = data.getProperty ("secret", juce::var()).toString();
        if (secret.isEmpty())
            return;
        juce::MessageManager::callAsync ([this, flag, secret]
        {
            if (flag->load() && onActivityJoin)
                onActivityJoin (secret);
        });
        return;
    }

    auto user = data.getProperty ("user", juce::var());
    if (! user.isObject())
        return;
    auto userId = user.getProperty ("id", juce::var()).toString();
    if (userId.isEmpty())
        return;
    auto name = user.getProperty ("global_name", juce::var()).toString();
    if (name.isEmpty())
        name = user.getProperty ("username", juce::var()).toString();

    juce::MessageManager::callAsync ([this, flag, userId, name]
    {
        if (flag->load() && onJoinRequest)
            onJoinRequest (userId, name);
    });
}

bool DiscordPresence::sendFrame (int opcode, const juce::String& json)
{
   #if JUCE_WINDOWS
    if (pipe == nullptr)
        return false;

    auto utf8 = json.toRawUTF8();
    const juce::uint32 len = (juce::uint32) juce::CharPointer_UTF8 (utf8).sizeInBytes() - 1;

    juce::MemoryBlock frame;
    const juce::int32 op = (juce::int32) opcode;
    const juce::int32 ln = (juce::int32) len;
    frame.append (&op, 4);
    frame.append (&ln, 4);
    frame.append (utf8, len);

    DWORD written = 0;
    if (! WriteFile ((HANDLE) pipe, frame.getData(), (DWORD) frame.getSize(), &written, nullptr)
        || written != frame.getSize())
    {
        closePipe();
        return false;
    }
    return true;
   #else
    juce::ignoreUnused (opcode, json);
    return false;
   #endif
}

bool DiscordPresence::readFrame (int& opcode, juce::String& json, int timeoutMs)
{
   #if JUCE_WINDOWS
    if (pipe == nullptr)
        return false;

    const juce::uint32 deadline = juce::Time::getMillisecondCounter() + (juce::uint32) juce::jmax (0, timeoutMs);
    DWORD avail = 0;

    for (;;)
    {
        if (! PeekNamedPipe ((HANDLE) pipe, nullptr, 0, nullptr, &avail, nullptr))
        {
            closePipe();
            return false;
        }
        if (avail >= 8)
            break;
        if (timeoutMs <= 0 || juce::Time::getMillisecondCounter() >= deadline || threadShouldExit())
            return false;
        wait (15);
    }

    juce::int32 header[2] = { 0, 0 };
    DWORD got = 0;
    if (! ReadFile ((HANDLE) pipe, header, 8, &got, nullptr) || got != 8)
    {
        closePipe();
        return false;
    }
    opcode = (int) header[0];
    const int len = (int) header[1];
    if (len <= 0)
    {
        json = {};
        return true;
    }

    juce::HeapBlock<char> buf ((size_t) len + 1);
    DWORD readTotal = 0;
    while ((int) readTotal < len)
    {
        DWORD n = 0;
        if (! ReadFile ((HANDLE) pipe, buf + readTotal, (DWORD) (len - (int) readTotal), &n, nullptr) || n == 0)
        {
            closePipe();
            return false;
        }
        readTotal += n;
    }
    buf[len] = 0;
    json = juce::String::fromUTF8 (buf, len);
    return true;
   #else
    juce::ignoreUnused (opcode, json, timeoutMs);
    return false;
   #endif
}

void DiscordPresence::sendActivity (const Activity& a)
{
    juce::DynamicObject::Ptr assets = new juce::DynamicObject();
    assets->setProperty ("large_image", "opendaw");
    assets->setProperty ("large_text", "mikeDAW");

    juce::DynamicObject::Ptr activity = new juce::DynamicObject();
    if (a.details.length() >= 2)
        activity->setProperty ("details", a.details);
    if (a.state.length() >= 2)
        activity->setProperty ("state", a.state);
    if (a.startEpochSeconds > 0)
    {
        juce::DynamicObject::Ptr ts = new juce::DynamicObject();
        ts->setProperty ("start", a.startEpochSeconds);
        activity->setProperty ("timestamps", juce::var (ts.get()));
    }
    activity->setProperty ("assets", juce::var (assets.get()));

    if (a.partyId.isNotEmpty() && a.partySize > 0 && a.partyMax >= a.partySize)
    {
        juce::Array<juce::var> size;
        size.add (a.partySize);
        size.add (a.partyMax);

        juce::DynamicObject::Ptr party = new juce::DynamicObject();
        party->setProperty ("id", a.partyId);
        party->setProperty ("size", juce::var (size));
        activity->setProperty ("party", juce::var (party.get()));

        if (a.joinSecret.isNotEmpty())
        {
            juce::DynamicObject::Ptr secrets = new juce::DynamicObject();
            secrets->setProperty ("join", a.joinSecret);
            activity->setProperty ("secrets", juce::var (secrets.get()));
            activity->setProperty ("instance", true);
        }
    }

    juce::DynamicObject::Ptr args = new juce::DynamicObject();
   #if JUCE_WINDOWS
    args->setProperty ("pid", (juce::int64) GetCurrentProcessId());
   #endif
    args->setProperty ("activity", juce::var (activity.get()));

    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty ("cmd", "SET_ACTIVITY");
    root->setProperty ("args", juce::var (args.get()));
    root->setProperty ("nonce", juce::Uuid().toString());

    sendFrame (opFrame, juce::JSON::toString (juce::var (root.get()), true));
}
