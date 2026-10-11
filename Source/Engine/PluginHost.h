#pragma once
#include <JuceHeader.h>
#include <map>
#include <mutex>
#include <set>
#include "../UI/AppLookAndFeel.h"

class PluginHost
{
public:
    PluginHost();
    ~PluginHost();

    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPlugins;

    juce::KnownPluginList unverifiedPlugins;

    void startScan (std::function<void (const juce::String&)> onProgress,
                    std::function<void (int numFound)> onDone);
    bool isScanning() const;

    void addUserScanFolder (const juce::File& folder);
    void removeUserScanFolder (const juce::File& folder);
    juce::StringArray getUserScanFolders() const;

    bool isVerifyEnabled() const;
    void setVerifyEnabled (bool enabled);

    bool isUnverified (const juce::PluginDescription& desc) const;

    bool resolveForLoad (juce::PluginDescription& desc, juce::String& error);

    void clearPluginList();

    bool isFLStudioScanEnabled() const;
    void setFLStudioScanEnabled (bool enabled);
    juce::Array<juce::File> getFLStudioPluginFolders() const;

    juce::FileSearchPath getSearchPathForFormat (juce::AudioPluginFormat&,
                                                 bool includeUserFolders = true) const;
    juce::StringArray getScanLocations() const;

    std::function<void()> onPluginsChanged;

    juce::StringArray newlyFound;

    juce::StringArray lastScanFailures;

    juce::StringArray startupScanNotes;
    void clearPedalOnCleanExit();

    double lastScanSeconds = 0.0;
    bool lastScanCancelled = false;

    int lastScanBlacklisted = 0;

    void stopScan();

    std::unique_ptr<juce::AudioPluginInstance> createInstance (const juce::PluginDescription& desc,
                                                               double sampleRate, int blockSize,
                                                               juce::String& error);

    juce::Array<juce::PluginDescription> getInstruments() const;
    juce::Array<juce::PluginDescription> getEffects() const;

    static bool isPreferredOver (const juce::PluginDescription& a, const juce::PluginDescription& b);

    juce::PropertiesFile& getSettings() { return *settings; }

private:
    bool resolveLocalMatch (const juce::PluginDescription& desc, juce::PluginDescription& local) const;

    bool resolveUnverifiedMatch (const juce::PluginDescription& desc, juce::PluginDescription& match) const;

    static juce::Array<juce::PluginDescription> dedupeInstalls (const juce::Array<juce::PluginDescription>&);

    std::unique_ptr<juce::AudioPluginInstance> instantiateGuarded (const juce::PluginDescription& desc,
                                                                   double sampleRate, int blockSize,
                                                                   juce::String& error);
    bool isCrashBlacklisted (const juce::String& fileId);

    void saveKnownPlugins();
    void saveUnverifiedPlugins();
    juce::File getCacheFile() const;
    juce::File getUnverifiedCacheFile() const;
    juce::File getTimestampCacheFile() const;

    juce::File pedalFile() const;
    juce::File crashedListFile() const;

    struct CrashEntry
    {
        juce::int64 modTime = 0;
        int strikes = 0;
    };

    std::mutex crashStateMutex;
    std::map<juce::String, CrashEntry> crashedCache;
    bool crashedLoaded = false;
    std::set<juce::String> inflightPaths;

    void loadCrashedListLocked();
    void saveCrashedListLocked() const;
    void blacklistPlugin (const juce::String& fileId);
    void consumeStartupPedal();
    void pedalEnter (const juce::String& fileId);
    void pedalLeave (const juce::String& fileId);
    void writePedalLocked() const;
    void removeUnverifiedFile (const juce::String& fileId);

    std::mutex formatManagerMutex;
    std::mutex resolveForLoadMutex;
    static std::unique_lock<std::mutex> lockOffMessageThread (std::mutex&);

    class ScanThread;
    std::unique_ptr<ScanThread> scanThread;
    std::unique_ptr<juce::PropertiesFile> settings;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginHost)
};
class PluginWindow : public juce::DocumentWindow
{
public:
    PluginWindow (const juce::String& key, std::unique_ptr<juce::Component> content,
                  bool resizable, const juce::String& title,
                  std::function<void (const juce::String&)> onClose);
    ~PluginWindow() override;

    void closeButtonPressed() override;

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (keyForwarder && keyForwarder (key))
            return true;
        return juce::DocumentWindow::keyPressed (key);
    }

    std::function<bool (const juce::KeyPress&)> keyForwarder;

    void setRenderLocked (bool locked)
    {
        if (locked)
        {
            if (lockVeil == nullptr)
            {
                lockVeil = std::make_unique<LockVeil>();
                addAndMakeVisible (*lockVeil);
            }
            lockVeil->setBounds (getLocalBounds());
            lockVeil->toFront (false);
        }
        else
        {
            lockVeil.reset();
        }
    }

    const juce::String key;

private:
    struct LockVeil : juce::Component
    {
        LockVeil() { setInterceptsMouseClicks (true, true); setWantsKeyboardFocus (false); }
        void paint (juce::Graphics& g) override { g.fillAll (Colors::windowBg.withAlpha (0.80f)); }
    };

    void resized() override
    {
        juce::DocumentWindow::resized();
        if (lockVeil != nullptr)
            lockVeil->setBounds (getLocalBounds());
    }

    std::unique_ptr<LockVeil> lockVeil;
    std::function<void (const juce::String&)> onCloseFn;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginWindow)
};

class PluginWindowManager
{
public:
    PluginWindowManager();
    ~PluginWindowManager();

    void setProject (juce::ValueTree projectTree);

    void open (const juce::String& key, juce::AudioProcessor& processor, const juce::String& title);
    void openLoading (const juce::String& key, const juce::String& title);
    void close (const juce::String& key);
    void closeAll();

    void setWindowsRenderLocked (bool locked)
    {
        auto& desktop = juce::Desktop::getInstance();
        for (int i = desktop.getNumComponents(); --i >= 0;)
            if (auto* w = dynamic_cast<PluginWindow*> (desktop.getComponent (i)))
                w->setRenderLocked (locked);
    }

    bool isOpen (const juce::String& key) const { return windows.find (key) != windows.end(); }

    juce::uint32 getLastInteraction (const juce::String& key) const
    {
        auto it = lastInteraction.find (key);
        return it != lastInteraction.end() ? it->second : 0;
    }

    std::function<void (const juce::String&)> onWindowClosed;
    std::function<void (const juce::String&)> onStateChanged;
    std::function<bool (const juce::KeyPress&)> onKeyPress;

private:
    struct Entry;
    void createFor (const juce::String& key, juce::AudioProcessor& processor,
                    const juce::String& title);
    void promoteLoadingWindow (const juce::String& key, juce::AudioProcessor& processor,
                               const juce::String& title);
    void stampInteraction (const juce::String& key)
    { lastInteraction[key] = juce::Time::getMillisecondCounter(); }

    juce::ValueTree project;
    std::map<juce::String, std::unique_ptr<Entry>> windows;
    std::map<juce::String, juce::uint32> lastInteraction;
};
