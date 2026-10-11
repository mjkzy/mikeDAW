#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

class PerfMonitor;
class AudioEngine;

class DebugOverlayUI : private juce::Timer
{
public:
    DebugOverlayUI (PerfMonitor&, AudioEngine&, juce::ValueTree project);
    void draw();
    void requestMainTab() noexcept { selectMainTab.store (true, std::memory_order_release); }
    void setActive (bool);

private:
    struct PluginInfo
    {
        std::string key;
        std::string name;
        std::string location;
        std::string format;
        bool loaded = false;
        bool loading = false;
        float load = 0.0f;
        float peak = 0.0f;
    };

    struct ProjectSnapshot
    {
        std::string name;
        double bpm = 0.0;
        bool songMode = false;
        int channels = 0;
        int samplers = 0;
        int builtinInstruments = 0;
        int instrumentPlugins = 0;
        int patterns = 0;
        int playlistClips = 0;
        int audioClips = 0;
        int mixerTracks = 0;
        int builtinEffects = 0;
        int pluginTotal = 0;
        int pluginLoaded = 0;
        int pluginLoading = 0;
        int pluginMissing = 0;
        int missingMedia = 0;
        std::vector<PluginInfo> plugins;
    };

    void drawOverview();
    void drawAudio();
    void drawThreads();
    void drawHitches();
    void drawMemory();
    void drawFlags();
    void drawReport();
    void timerCallback() override;
    void refreshProjectSnapshot();
    ProjectSnapshot getProjectSnapshot();

    juce::String audioExtraSection();
    juce::String projectExtraSection();

    PerfMonitor& perf;
    AudioEngine& engine;
    juce::ValueTree project;

    std::map<int, std::vector<std::string>> hitchStacks;
    unsigned int xrunBaseline = 0;
    std::string lastReportPath;
    std::atomic<bool> selectMainTab { true };
    std::mutex projectSnapshotMutex;
    ProjectSnapshot cachedProject;
    std::map<std::string, std::pair<std::string, std::string>> pluginIdentityCache;
};
