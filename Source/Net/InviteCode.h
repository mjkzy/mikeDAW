#pragma once
#include <JuceHeader.h>
#include <vector>

namespace InviteCode
{
    struct Endpoint
    {
        juce::String address;
        int port = 0;
    };

    struct HostInvite
    {
        juce::String code;
        juce::String publicAddress;
        juce::String lanAddress;
        int port = 0;
        bool upnpMapped = false;
        bool upnpPortReady = false;
        juce::String status;
    };

    HostInvite prepareHostInvite (int port, const juce::String& lanAddress);
    std::vector<Endpoint> parseJoinText (const juce::String& text, int defaultPort);
    juce::String makeCode (const juce::String& publicAddress, const juce::String& lanAddress, int port);
    bool removeUPnPPortMapping (int port, juce::String& status);

    bool probeEndpoint (const juce::String& address, int port, int timeoutMs);
}
