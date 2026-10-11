#include "RelayTunnel.h"
#include <vector>

struct RelayBridgeState
{
    std::unique_ptr<juce::StreamingSocket> relay;
    std::unique_ptr<juce::StreamingSocket> local;
    std::atomic<bool> closed { false };
    std::atomic<bool> relaySwapPending { false };

    juce::String relayHost;
    int relayPort = 0;
    juce::String token;
    juce::String role;

    void setDead() { closed.store (true); }

    bool dialRelay()
    {
        auto s = std::make_unique<juce::StreamingSocket>();
        if (! s->connect (relayHost, relayPort, 8000))
            return false;
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty ("t", "bridge");
        o->setProperty ("token", token);
        o->setProperty ("role", role);
        juce::String line = juce::JSON::toString (juce::var (o.get()), true) + "\n";
        int len = (int) line.getNumBytesAsUTF8();
        if (s->write (line.toRawUTF8(), len) != len)
            return false;
        relay = std::move (s);
        return true;
    }

    void shutdown()
    {
        if (closed.exchange (true))
            return;
        if (relay != nullptr) relay->close();
        if (local != nullptr) local->close();
    }
};

namespace
{
    void pumpRelayToLocal (const std::shared_ptr<RelayBridgeState>& state)
    {
        std::vector<char> buf (65536);
        while (! state->closed.load() && ! state->relaySwapPending.load())
        {
            auto* relay = state->relay.get();
            if (relay == nullptr)
            {
                state->relaySwapPending.store (true);
                break;
            }
            int ready = relay->waitUntilReady (true, 200);
            if (ready < 0)
            {
                state->relaySwapPending.store (true);
                break;
            }
            if (ready == 0)
                continue;
            int n = relay->read (buf.data(), (int) buf.size(), false);
            if (n <= 0)
            {
                state->relaySwapPending.store (true);
                break;
            }
            int off = 0;
            while (off < n)
            {
                if (state->closed.load()) return;
                int w = state->local->write (buf.data() + off, n - off);
                if (w <= 0)
                {
                    state->setDead();
                    return;
                }
                off += w;
            }
        }
    }

    void pumpLocalToRelay (const std::shared_ptr<RelayBridgeState>& state)
    {
        std::vector<char> buf (65536);
        while (! state->closed.load() && ! state->relaySwapPending.load())
        {
            auto* relay = state->relay.get();
            if (relay == nullptr)
            {
                state->relaySwapPending.store (true);
                break;
            }
            int ready = state->local->waitUntilReady (true, 200);
            if (ready < 0)
            {
                state->setDead();
                break;
            }
            if (ready == 0)
                continue;
            int n = state->local->read (buf.data(), (int) buf.size(), false);
            if (n <= 0)
            {
                state->setDead();
                break;
            }
            int off = 0;
            while (off < n)
            {
                if (state->closed.load()) return;
                int w = relay->write (buf.data() + off, n - off);
                if (w <= 0)
                {
                    state->relaySwapPending.store (true);
                    return;
                }
                off += w;
            }
        }
    }

    void runBridge (const std::shared_ptr<RelayBridgeState>& state)
    {
        while (! state->closed.load())
        {
            state->relaySwapPending.store (false);
            if (! state->dialRelay())
            {
                if (! state->closed.load())
                    juce::Thread::sleep (1000);
                continue;
            }

            std::thread other ([state] { pumpLocalToRelay (state); });
            pumpRelayToLocal (state);
            if (other.joinable())
                other.join();

            state->shutdown();
            break;
        }
    }
}

void RelayTunnel::startHostBridge (const juce::String& relayHost, int relayPort,
                                   const juce::String& token, int localSessionPort)
{
   #if OPENDAW_PUBLIC_BUILD
    juce::ignoreUnused (relayHost, relayPort, token, localSessionPort);
    return;
   #endif
    juce::Thread::launch ([relayHost, relayPort, token, localSessionPort]
    {
        auto state = std::make_shared<RelayBridgeState>();
        state->relayHost = relayHost;
        state->relayPort = relayPort;
        state->token = token;
        state->role = "host";

        state->local = std::make_unique<juce::StreamingSocket>();
        if (! state->local->connect ("127.0.0.1", localSessionPort, 4000))
            return;

        runBridge (state);
    });
}

RelayJoinTunnel::RelayJoinTunnel (const juce::String& host, int port, const juce::String& tok)
    : relayHost (host), relayPort (port), token (tok)
{
    if (listener.createListener (0, "127.0.0.1"))
    {
        localPort = listener.getBoundPort();
        thread = std::thread ([this] { acceptLoop(); });
    }
}

RelayJoinTunnel::~RelayJoinTunnel()
{
    stop = true;
    listener.close();
    {
        const juce::ScopedLock sl (bridgeLock);
        if (bridge != nullptr)
            bridge->shutdown();
    }
    if (thread.joinable())
        thread.join();
}

void RelayJoinTunnel::acceptLoop()
{
    std::unique_ptr<juce::StreamingSocket> incoming (listener.waitForNextConnection());
    if (stop.load() || incoming == nullptr)
        return;

    auto state = std::make_shared<RelayBridgeState>();
    state->local = std::move (incoming);
    state->relayHost = relayHost;
    state->relayPort = relayPort;
    state->token = token;
    state->role = "join";

    {
        const juce::ScopedLock sl (bridgeLock);
        if (stop.load())
        {
            state->shutdown();
            return;
        }
        bridge = state;
    }

    if (state->closed.load())
        return;

    runBridge (state);
}
