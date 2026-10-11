#pragma once
#include <JuceHeader.h>
#include <functional>

class Updater : private juce::Thread
{
public:
    explicit Updater (juce::PropertiesFile& settings);
    ~Updater() override;

    void checkForUpdate();
    void downloadAndInstall (const juce::String& url);

    std::function<void (const juce::String& version, const juce::String& notes,
                        const juce::String& url)> onUpdateAvailable;
    std::function<void (const juce::String& message)> onStatus;
    std::function<void (const juce::String& message)> onError;

private:
    void run() override;
    void doCheck();
    void doDownload (const juce::String& url);
    juce::URL authedUrl (const juce::String& base) const;

    void postStatus (const juce::String& message);
    void postError (const juce::String& message);
    void postUpdateAvailable (const juce::String& version,
                              const juce::String& notes,
                              const juce::String& url);

    juce::PropertiesFile& settings;
    juce::CriticalSection lock;
    juce::WaitableEvent jobReady;
    enum class Job { none, check, download };
    Job pending = Job::none;
    juce::String pendingUrl;

    JUCE_DECLARE_WEAK_REFERENCEABLE (Updater)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Updater)
};
