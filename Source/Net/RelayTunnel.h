#pragma once
#include <JuceHeader.h>
#include <memory>
#include <thread>

struct RelayBridgeState;

class RelayTunnel
{
public:
    static void startHostBridge (const juce::String& relayHost, int relayPort,
                                 const juce::String& token, int localSessionPort);
};

class RelayJoinTunnel
{
public:
    RelayJoinTunnel (const juce::String& relayHost, int relayPort, const juce::String& token);
    ~RelayJoinTunnel();

    int getLocalPort() const noexcept { return localPort; }

private:
    void acceptLoop();

    juce::String relayHost;
    int relayPort;
    juce::String token;
    juce::StreamingSocket listener;
    int localPort = 0;
    std::atomic<bool> stop { false };
    juce::CriticalSection bridgeLock;
    std::shared_ptr<RelayBridgeState> bridge;
    std::thread thread;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RelayJoinTunnel)
};
