#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <memory>
#include <vector>

class DiscordPresence : private juce::Thread
{
public:
    DiscordPresence();
    ~DiscordPresence() override;

    static constexpr const char* clientAppId = "1525718276162719814";

    struct Activity
    {
        juce::String details;
        juce::String state;
        juce::int64  startEpochSeconds = 0;

        juce::String partyId;
        int          partySize = 0;
        int          partyMax = 0;
        juce::String joinSecret;
    };

    void start (bool enabled);

    void setActivity (const Activity& activity);

    std::function<void (const juce::String& secret)> onActivityJoin;
    std::function<void (const juce::String& userId, const juce::String& userName)> onJoinRequest;

    void acceptJoinRequest (const juce::String& userId);
    void declineJoinRequest (const juce::String& userId);

private:
    void run() override;

    bool connectPipe();
    void closePipe();
    bool handshake();
    void subscribeJoinEvents();
    bool sendFrame (int opcode, const juce::String& json);
    bool readFrame (int& opcode, juce::String& json, int timeoutMs);
    void sendActivity (const Activity& a);
    void handleDispatch (const juce::String& json);
    void queueCommand (const juce::String& cmd, const juce::String& userId);
    void flushCommands();

    static bool sameActivity (const Activity& a, const Activity& b);

    juce::CriticalSection stateLock;
    Activity pending;
    bool     hasPending = false;
    std::vector<juce::String> outCommands;

    Activity last;
    bool     lastValid = false;

    std::shared_ptr<std::atomic<bool>> alive { std::make_shared<std::atomic<bool>> (true) };

    juce::WaitableEvent wake;
    void* pipe = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DiscordPresence)
};
