#pragma once
#include <JuceHeader.h>
#include <atomic>

#if OPENDAW_PUBLIC_BUILD
namespace License
{
    constexpr bool shouldDegrade() noexcept { return false; }
}
#else
namespace License
{
    enum class State { checking = 0, authorized = 1, denied = 2 };

    inline std::atomic<int>          gState { 0 };
    inline std::atomic<juce::int64>  gStartMs { 0 };

    inline bool shouldDegrade() noexcept
    {
        const int s = gState.load (std::memory_order_relaxed);
        if (s == (int) State::authorized) return false;
        if (s == (int) State::denied)     return true;
        const auto start = gStartMs.load (std::memory_order_relaxed);
        return start != 0 && juce::Time::currentTimeMillis() - start > 90000;
    }
}

class LicenseManager : private juce::Thread
{
public:
    explicit LicenseManager (juce::PropertiesFile& settings);
    ~LicenseManager() override;

    void start();

    License::State state() const noexcept { return (License::State) License::gState.load(); }
    bool  authorized()  const noexcept    { return state() == License::State::authorized; }
    juce::String licensedName() const;
    juce::String hwid() const             { return machineId; }
    juce::String statusText() const;

    std::function<void (License::State)> onStateChanged;

private:
    void run() override;
    bool doAuthOnce();
    void applyState (License::State, const juce::String& reason, const juce::String& name);

    bool verifyToken (const juce::MemoryBlock& tokenBytes,
                      const juce::MemoryBlock& sig,
                      const juce::String& expectNonce,
                      juce::String& outName);
    bool loadCachedToken();

    juce::PropertiesFile& settings;
    juce::String machineId;
    juce::int64  localUid = 0;
    juce::String displayName;

    mutable juce::CriticalSection infoLock;
    juce::String lastReason;
    juce::String cachedName;
    std::atomic<juce::int64> cachedExp { 0 };

    static juce::String computeHwid();

    JUCE_DECLARE_WEAK_REFERENCEABLE (LicenseManager)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LicenseManager)
};
#endif
