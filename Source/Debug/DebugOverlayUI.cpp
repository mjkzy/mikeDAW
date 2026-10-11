#include "DebugOverlayUI.h"
#include "PerfMonitor.h"
#include "DebugFlags.h"
#include "../Engine/AudioEngine.h"
#include "../Model/ProjectModel.h"
#include <imgui.h>
#include <algorithm>
#include <cfloat>
#include <cstdio>

namespace
{
    constexpr double kMB = 1024.0 * 1024.0;

    void plotFromHistory (const char* label, const std::vector<float>& v,
                          float lo = FLT_MAX, float hi = FLT_MAX)
    {
        ImGui::PlotLines (label, v.empty() ? nullptr : v.data(), (int) v.size(),
                          0, nullptr, lo, hi, ImVec2 (0.0f, 90.0f));
    }

    std::pair<std::string, std::string> pluginIdentity (const juce::String& xmlText,
                                                         const juce::String& fallback)
    {
        juce::PluginDescription desc;
        if (auto xml = juce::parseXML (xmlText); xml != nullptr && desc.loadFromXml (*xml))
            return { desc.name.toStdString(), desc.pluginFormatName.toStdString() };
        return { fallback.toStdString(), {} };
    }

    const char* pluginStatus (bool loaded, bool loading)
    {
        if (loading)
            return "LOADING";
        return loaded ? "LOADED" : "MISSING";
    }
}

DebugOverlayUI::DebugOverlayUI (PerfMonitor& p, AudioEngine& e, juce::ValueTree projectTree)
    : perf (p), engine (e), project (std::move (projectTree))
{
    refreshProjectSnapshot();
    startTimer (2000);
}

void DebugOverlayUI::draw()
{
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos (ImVec2 (0.0f, 0.0f));
    ImGui::SetNextWindowSize (io.DisplaySize);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                                 | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse
                                 | ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::Begin ("##debugoverlay", nullptr, flags);
    if (ImGui::BeginTabBar ("##debugtabs"))
    {
        const auto mainFlags = selectMainTab.exchange (false, std::memory_order_acq_rel)
                             ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
        if (ImGui::BeginTabItem ("Main", nullptr, mainFlags))
        {
            drawOverview();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem ("Audio"))    { drawAudio();    ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem ("Threads"))  { drawThreads();  ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem ("Hitches"))  { drawHitches();  ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem ("Memory"))   { drawMemory();   ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem ("Flags"))    { drawFlags();    ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem ("Report"))   { drawReport();   ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void DebugOverlayUI::timerCallback()
{
    refreshProjectSnapshot();
}

void DebugOverlayUI::setActive (bool active)
{
    if (! active)
    {
        stopTimer();
        return;
    }

    refreshProjectSnapshot();
    startTimer (2000);
}

void DebugOverlayUI::refreshProjectSnapshot()
{
    ProjectSnapshot next;
    std::map<std::string, std::pair<float, float>> pluginPerf;
    for (const auto& stats : engine.getPluginPerfStats())
        pluginPerf.emplace (stats.key.toStdString(), std::make_pair (stats.load, stats.peak));

    auto applyPerf = [&] (PluginInfo& info, const juce::String& key)
    {
        info.key = key.toStdString();
        if (const auto found = pluginPerf.find (info.key); found != pluginPerf.end())
        {
            info.load = found->second.first;
            info.peak = found->second.second;
        }
    };
    auto identityFor = [&] (const juce::String& xml, const juce::String& fallback)
    {
        if (xml.isEmpty())
            return std::make_pair (fallback.toStdString(), std::string {});
        const auto cacheKey = xml.toStdString();
        if (const auto found = pluginIdentityCache.find (cacheKey);
            found != pluginIdentityCache.end())
            return found->second;
        return pluginIdentityCache.emplace (cacheKey, pluginIdentity (xml, fallback)).first->second;
    };

    next.name = project.getProperty (IDs::name, "Untitled").toString().toStdString();
    next.bpm = (double) project.getProperty (IDs::bpm, 140.0);
    next.songMode = (bool) project.getProperty (IDs::songMode, false);

    auto channels = Model::getChannels (project);
    next.channels = channels.getNumChildren();
    for (int i = 0; i < channels.getNumChildren(); ++i)
    {
        auto channel = channels.getChild (i);
        if (! channel.hasType (IDs::Channel))
            continue;

        const auto type = channel.getProperty (IDs::type).toString();
        if (type == "sampler")
        {
            ++next.samplers;
            const auto path = channel.getProperty (IDs::samplePath).toString();
            if (path.isNotEmpty() && ! path.startsWith ("builtin:")
                && ! engine.resolveSampleFile (path).existsAsFile())
                ++next.missingMedia;
            continue;
        }
        if (type == "builtin")
        {
            ++next.builtinInstruments;
            continue;
        }
        if (type != "plugin")
            continue;

        ++next.instrumentPlugins;
        const int channelId = (int) channel.getProperty (IDs::id);
        const juce::String key = "c" + juce::String (channelId);
        const bool loading = engine.isPluginLoading (key);
        const bool loaded = engine.getProcessorFor (key) != nullptr
                         && ! engine.isChannelPlaceholder (channelId);
        const auto identity = identityFor (channel.getProperty (IDs::pluginDesc).toString(),
                                           channel.getProperty (IDs::name, "Instrument").toString());

        PluginInfo info;
        applyPerf (info, key);
        info.name = identity.first;
        info.format = identity.second;
        info.location = "Channel: " + channel.getProperty (IDs::name, "Instrument").toString().toStdString();
        info.loaded = loaded;
        info.loading = loading;
        next.plugins.push_back (std::move (info));
    }

    auto patterns = Model::getPatterns (project);
    for (int i = 0; i < patterns.getNumChildren(); ++i)
        if (patterns.getChild (i).hasType (IDs::Pattern))
            ++next.patterns;

    auto playlist = Model::getPlaylist (project);
    for (int i = 0; i < playlist.getNumChildren(); ++i)
    {
        auto clip = playlist.getChild (i);
        if (! clip.hasType (IDs::Clip))
            continue;
        ++next.playlistClips;
        if (clip.getProperty (IDs::type).toString() == "audio")
        {
            ++next.audioClips;
            const auto path = clip.getProperty (IDs::path).toString();
            if (path.isNotEmpty() && ! engine.resolveSampleFile (path).existsAsFile())
                ++next.missingMedia;
        }
    }

    auto mixer = Model::getMixer (project);
    for (int i = 0; i < mixer.getNumChildren(); ++i)
    {
        auto track = mixer.getChild (i);
        if (! track.hasType (IDs::Track))
            continue;
        ++next.mixerTracks;
        const int trackIndex = (int) track.getProperty (IDs::index);
        for (int slotIndex = 0; slotIndex < track.getNumChildren(); ++slotIndex)
        {
            auto slot = track.getChild (slotIndex);
            if (! slot.hasType (IDs::Slot))
                continue;
            if (slot.getProperty (IDs::type).toString() != "plugin")
            {
                ++next.builtinEffects;
                continue;
            }

            const juce::String key = "m" + juce::String (trackIndex)
                                   + "s" + slot.getProperty (IDs::uid).toString();
            const bool loading = engine.isPluginLoading (key);
            const bool loaded = engine.getProcessorFor (key) != nullptr;
            const auto identity = identityFor (slot.getProperty (IDs::pluginDesc).toString(), "Effect");

            PluginInfo info;
            applyPerf (info, key);
            info.name = identity.first;
            info.format = identity.second;
            info.location = "Mixer " + std::to_string (trackIndex)
                          + ", slot " + std::to_string ((int) slot.getProperty (IDs::slotIndex) + 1);
            info.loaded = loaded;
            info.loading = loading;
            next.plugins.push_back (std::move (info));
        }
    }

    std::stable_sort (next.plugins.begin(), next.plugins.end(),
                      [] (const PluginInfo& a, const PluginInfo& b)
                      {
                          const int aRank = ! a.loaded && ! a.loading ? 0 : (a.loading ? 1 : 2);
                          const int bRank = ! b.loaded && ! b.loading ? 0 : (b.loading ? 1 : 2);
                          if (aRank != bRank)
                              return aRank < bRank;
                          if (a.peak != b.peak)
                              return a.peak > b.peak;
                          return a.location < b.location;
                      });

    next.pluginTotal = (int) next.plugins.size();
    for (const auto& plugin : next.plugins)
    {
        if (plugin.loading)
            ++next.pluginLoading;
        else if (plugin.loaded)
            ++next.pluginLoaded;
        else
            ++next.pluginMissing;
    }

    std::lock_guard<std::mutex> lock (projectSnapshotMutex);
    cachedProject = std::move (next);
}

DebugOverlayUI::ProjectSnapshot DebugOverlayUI::getProjectSnapshot()
{
    std::lock_guard<std::mutex> lock (projectSnapshotMutex);
    return cachedProject;
}

void DebugOverlayUI::drawOverview()
{
    const auto s = perf.latest();
    const auto hist = perf.history();
    const auto& ap = engine.getAudioPerfStats();
    const auto projectInfo = getProjectSnapshot();
    const float audioLoad = ap.load.load (std::memory_order_relaxed);
    const float audioPeak = ap.loadPeak.load (std::memory_order_relaxed);
    const float lockWaitMs = ap.lockWaitMs.load (std::memory_order_relaxed);
    const unsigned int xruns = ap.xruns.load (std::memory_order_relaxed);
    const unsigned int newXruns = xruns >= xrunBaseline ? xruns - xrunBaseline : xruns;
    const double slopeMB = perf.memorySlopeBytesPerMin (300) / kMB;
    const int majorHitches = perf.majorHitches.load (std::memory_order_relaxed);
    const auto ui = perf.uiResponsiveness();
    const PluginInfo* hottestPlugin = nullptr;
    for (const auto& plugin : projectInfo.plugins)
        if (plugin.loaded && (hottestPlugin == nullptr || plugin.peak > hottestPlugin->peak))
            hottestPlugin = &plugin;
    const auto good = ImGui::GetStyleColorVec4 (ImGuiCol_CheckMark);
    const auto warning = ImGui::GetStyleColorVec4 (ImGuiCol_PlotHistogram);
    const auto critical = ImGui::GetStyleColorVec4 (ImGuiCol_PlotLinesHovered);

    int criticalCount = 0;
    int warningCount = 0;
    if (audioLoad >= 1.0f || newXruns > 0)
        ++criticalCount;
    else if (audioPeak >= 0.85f)
        ++warningCount;
    if (projectInfo.pluginMissing > 0 || projectInfo.missingMedia > 0)
        ++criticalCount;
    if (lockWaitMs >= 1.0f || majorHitches > 0 || projectInfo.pluginLoading > 0)
        ++warningCount;
    if (ui.samples > 0 && ui.p95Ms >= 60.0)
        ++warningCount;
    if (s.timeSeconds >= 120.0 && slopeMB >= 1.0)
        ++warningCount;
    if (hottestPlugin != nullptr && hottestPlugin->peak >= 0.4f)
        ++warningCount;

    const auto headlineColour = criticalCount > 0 ? critical : (warningCount > 0 ? warning : good);
    const char* headline = criticalCount > 0 ? "ACTION REQUIRED"
                         : (warningCount > 0 ? "CHECK WARNINGS" : "NO IMMEDIATE FAULT DETECTED");
    ImGui::TextColored (headlineColour, "%s", headline);
    ImGui::SameLine();
    ImGui::TextDisabled ("Project and live engine assessment");

    if (audioLoad >= 1.0f)
        ImGui::BulletText ("Audio callback is over budget (%.1f%%); dropouts are expected.", audioLoad * 100.0f);
    else if (newXruns > 0)
        ImGui::BulletText ("%u audio overruns recorded since reset.", newXruns);
    else if (audioPeak >= 0.85f)
        ImGui::BulletText ("Audio peak is close to the callback deadline (%.1f%%).", audioPeak * 100.0f);
    if (lockWaitMs >= 1.0f)
        ImGui::BulletText ("Audio thread waits %.2f ms for the engine lock.", lockWaitMs);
    if (projectInfo.pluginMissing > 0)
        ImGui::BulletText ("%d VST instance%s missing or failed to load.",
                           projectInfo.pluginMissing, projectInfo.pluginMissing == 1 ? " is" : "s are");
    if (projectInfo.pluginLoading > 0)
        ImGui::BulletText ("%d VST instance%s still loading.",
                           projectInfo.pluginLoading, projectInfo.pluginLoading == 1 ? " is" : "s are");
    if (projectInfo.missingMedia > 0)
        ImGui::BulletText ("%d audio file reference%s unresolved.",
                           projectInfo.missingMedia, projectInfo.missingMedia == 1 ? " is" : "s are");
    if (majorHitches > 0)
        ImGui::BulletText ("%d major message-thread hitch%s captured.",
                           majorHitches, majorHitches == 1 ? " was" : "es were");
    if (ui.samples > 0 && ui.p95Ms >= 60.0)
        ImGui::BulletText ("UI thread is oversubscribed: 25 ms tick runs %.0f ms median, %.0f ms p95 (%d/%d late).",
                           ui.medianMs, ui.p95Ms, ui.lateTicks, ui.samples);
    if (s.timeSeconds >= 120.0 && slopeMB >= 1.0)
        ImGui::BulletText ("Private memory is growing %.2f MB/min over the recent window.", slopeMB);
    if (hottestPlugin != nullptr && hottestPlugin->peak >= 0.25f)
        ImGui::BulletText ("%s peaked at %.1f%% of one audio-buffer deadline (current %.1f%%).",
                           hottestPlugin->name.c_str(), hottestPlugin->peak * 100.0f,
                           hottestPlugin->load * 100.0f);
    if (criticalCount == 0 && warningCount == 0)
        ImGui::BulletText ("Audio timing, project resources, VST loading, and memory trend are currently healthy.");

    ImGui::Separator();

    ImGui::Text ("Project: %s   |   %.0f BPM   |   %s",
                 projectInfo.name.c_str(), projectInfo.bpm,
                 projectInfo.songMode ? "SONG" : "PATTERN");
    ImGui::Text ("Channels %d (%d VST, %d sampler, %d built-in)   |   Patterns %d   |   Clips %d (%d audio)",
                 projectInfo.channels, projectInfo.instrumentPlugins,
                 projectInfo.samplers, projectInfo.builtinInstruments, projectInfo.patterns,
                 projectInfo.playlistClips, projectInfo.audioClips);
    ImGui::Text ("Mixer tracks %d   |   VSTs %d: %d loaded, %d loading, %d missing   |   Built-in FX %d",
                 projectInfo.mixerTracks, projectInfo.pluginTotal, projectInfo.pluginLoaded,
                 projectInfo.pluginLoading, projectInfo.pluginMissing, projectInfo.builtinEffects);
    ImGui::Text ("UI tick (25 ms target): median %.1f ms   |   p95 %.1f ms   |   worst %.1f ms   |   late %d/%d",
                 ui.medianMs, ui.p95Ms, ui.worstMs, ui.lateTicks, ui.samples);

    if (ImGui::BeginTable ("##mainmetrics", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn ("Audio load");
        ImGui::TableSetupColumn ("Audio peak");
        ImGui::TableSetupColumn ("Process CPU");
        ImGui::TableSetupColumn ("Memory");
        ImGui::TableHeadersRow();
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex (0); ImGui::Text ("%.1f%%", audioLoad * 100.0f);
        ImGui::TableSetColumnIndex (1); ImGui::Text ("%.1f%%", audioPeak * 100.0f);
        ImGui::TableSetColumnIndex (2); ImGui::Text ("%.1f%%", s.processCpu);
        ImGui::TableSetColumnIndex (3); ImGui::Text ("%.1f MB", s.privateBytes / kMB);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex (0); ImGui::Text ("%u xruns", newXruns);
        ImGui::TableSetColumnIndex (1); ImGui::Text ("%.2f ms lock wait", lockWaitMs);
        ImGui::TableSetColumnIndex (2); ImGui::Text ("%d major hitches", majorHitches);
        ImGui::TableSetColumnIndex (3); ImGui::Text ("%+.2f MB/min", slopeMB);
        ImGui::EndTable();
    }

    if (! projectInfo.plugins.empty())
    {
        ImGui::TextUnformatted ("VST instances");
        const auto tableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                              | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
        if (ImGui::BeginTable ("##mainplugins", 5, tableFlags, ImVec2 (0.0f, 150.0f)))
        {
            ImGui::TableSetupColumn ("Status", ImGuiTableColumnFlags_WidthFixed, 75.0f);
            ImGui::TableSetupColumn ("Plugin");
            ImGui::TableSetupColumn ("Format", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn ("Buffer CPU", ImGuiTableColumnFlags_WidthFixed, 105.0f);
            ImGui::TableSetupColumn ("Location");
            ImGui::TableHeadersRow();
            for (const auto& plugin : projectInfo.plugins)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex (0);
                ImGui::TextUnformatted (pluginStatus (plugin.loaded, plugin.loading));
                ImGui::TableSetColumnIndex (1);
                ImGui::TextUnformatted (plugin.name.c_str());
                ImGui::TableSetColumnIndex (2);
                ImGui::TextUnformatted (plugin.format.c_str());
                ImGui::TableSetColumnIndex (3);
                ImGui::Text ("%.1f%% / %.1f%%", plugin.load * 100.0f, plugin.peak * 100.0f);
                ImGui::TableSetColumnIndex (4);
                ImGui::TextUnformatted (plugin.location.c_str());
            }
            ImGui::EndTable();
        }
    }

    std::vector<float> cpu, priv;
    cpu.reserve (hist.size());
    priv.reserve (hist.size());
    for (const auto& h : hist)
    {
        cpu.push_back (h.processCpu);
        priv.push_back ((float) (h.privateBytes / kMB));
    }
    ImGui::PlotLines ("Process CPU", cpu.empty() ? nullptr : cpu.data(), (int) cpu.size(),
                      0, nullptr, 0.0f, 100.0f, ImVec2 (0.0f, 60.0f));

    const int n = AudioEngine::AudioPerfStats::historySize;
    std::vector<float> loadRing ((size_t) n, 0.0f);
    const int wp = ap.historyWritePos.load (std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        loadRing[(size_t) i] = ap.loadHistory[(size_t) ((wp + i) & (n - 1))]
                                   .load (std::memory_order_relaxed);
    ImGui::PlotLines ("Audio load", loadRing.data(), n, 0, nullptr, 0.0f, 1.0f,
                      ImVec2 (0.0f, 60.0f));
}

void DebugOverlayUI::drawAudio()
{
    const auto& ap = engine.getAudioPerfStats();
    const unsigned int xr = ap.xruns.load (std::memory_order_relaxed);

    ImGui::Text ("Load        %6.1f %%", ap.load.load (std::memory_order_relaxed) * 100.0f);
    ImGui::Text ("Peak        %6.1f %%", ap.loadPeak.load (std::memory_order_relaxed) * 100.0f);
    ImGui::Text ("Lock wait   %6.2f ms", ap.lockWaitMs.load (std::memory_order_relaxed));
    ImGui::Text ("Xruns       %u  (%u since reset)", xr, xr - xrunBaseline);
    ImGui::Text ("Clip voices dropped %u", ap.droppedClipVoices.load (std::memory_order_relaxed));
    ImGui::Text ("Callbacks   %u", ap.callbacks.load (std::memory_order_relaxed));
    ImGui::Text ("Buffer      %d samples", ap.bufferSamples.load (std::memory_order_relaxed));
    ImGui::Text ("Sample rate %.0f Hz", ap.sampleRate.load (std::memory_order_relaxed));

    if (ImGui::Button ("Reset peaks/xruns"))
        xrunBaseline = xr;

    ImGui::Separator();

    const int n = AudioEngine::AudioPerfStats::historySize;
    std::vector<float> loadRing ((size_t) n, 0.0f);
    const int wp = ap.historyWritePos.load (std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        loadRing[(size_t) i] = ap.loadHistory[(size_t) ((wp + i) & (n - 1))]
                                   .load (std::memory_order_relaxed);
    ImGui::PlotLines ("Audio load", loadRing.data(), n, 0, nullptr, 0.0f, 1.0f,
                      ImVec2 (0.0f, 160.0f));
}

void DebugOverlayUI::drawThreads()
{
    auto th = perf.threads();
    std::sort (th.begin(), th.end(),
               [] (const PerfMonitor::ThreadInfo& a, const PerfMonitor::ThreadInfo& b)
               { return a.cpuPercent > b.cpuPercent; });

    ImGui::Text ("%d threads   /   %d logical cores",
                 (int) th.size(), perf.numLogicalCores());

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                                | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable ("##threads", 3, flags))
    {
        ImGui::TableSetupColumn ("TID");
        ImGui::TableSetupColumn ("Name");
        ImGui::TableSetupColumn ("CPU %");
        ImGui::TableHeadersRow();
        for (const auto& t : th)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex (0); ImGui::Text ("%u", t.tid);
            ImGui::TableSetColumnIndex (1); ImGui::TextUnformatted (t.name.toRawUTF8());
            ImGui::TableSetColumnIndex (2); ImGui::Text ("%.1f", t.cpuPercent);
        }
        ImGui::EndTable();
    }
}

void DebugOverlayUI::drawHitches()
{
    float thr = (float) perf.hitchThresholdMs.load();
    if (ImGui::SliderFloat ("Threshold (ms)", &thr, 100.0f, 2000.0f, "%.0f"))
        perf.hitchThresholdMs.store ((double) thr);

    ImGui::Text ("Total %d   Major %d", perf.totalHitches.load(), perf.majorHitches.load());

    if (ImGui::Button ("Clear"))
    {
        perf.clearHitches();
        hitchStacks.clear();
    }

    bool logToFile = DebugFlags::logHitchesToFile.load();
    if (ImGui::Checkbox ("Log hitches to file", &logToFile))
        DebugFlags::logHitchesToFile.store (logToFile);

    ImGui::Separator();

    const auto hs = perf.hitches();
    for (auto it = hs.rbegin(); it != hs.rend(); ++it)
    {
        const auto& h = *it;
        ImGui::PushID (h.index);
        char label[160];
        std::snprintf (label, sizeof label, "%s   %.1f ms###hitch",
                       h.wallTime.toRawUTF8(), h.durationMs);
        if (ImGui::TreeNode (label))
        {
            auto found = hitchStacks.find (h.index);
            if (found == hitchStacks.end())
            {
                std::vector<std::string> lines;
                for (const auto& l : perf.symbolizeHitch (h.index))
                    lines.push_back (l.toStdString());
                found = hitchStacks.emplace (h.index, std::move (lines)).first;
            }

            for (const auto& line : found->second)
                ImGui::TextUnformatted (line.c_str());

            if (ImGui::Button ("Copy"))
            {
                std::string all;
                for (const auto& line : found->second) { all += line; all += '\n'; }
                ImGui::SetClipboardText (all.c_str());
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

void DebugOverlayUI::drawMemory()
{
    const auto s = perf.latest();
    const auto hist = perf.history();

    std::vector<float> priv, ws, handles, gdi, usr;
    priv.reserve (hist.size());
    ws.reserve (hist.size());
    handles.reserve (hist.size());
    gdi.reserve (hist.size());
    usr.reserve (hist.size());
    for (const auto& h : hist)
    {
        priv.push_back ((float) (h.privateBytes / kMB));
        ws.push_back ((float) (h.workingSet / kMB));
        handles.push_back ((float) h.handles);
        gdi.push_back ((float) h.gdiObjects);
        usr.push_back ((float) h.userObjects);
    }

    plotFromHistory ("Private MB", priv);
    plotFromHistory ("Working set MB", ws);

    ImGui::Text ("Handles %u   GDI %u   USER %u", s.handles, s.gdiObjects, s.userObjects);
    plotFromHistory ("Handles", handles);
    plotFromHistory ("GDI objects", gdi);
    plotFromHistory ("USER objects", usr);

    const double slopeMB = perf.memorySlopeBytesPerMin (300) / kMB;
    const ImVec4 col = slopeMB < 0.1 ? ImVec4 (0.40f, 0.90f, 0.40f, 1.0f)
                     : slopeMB < 1.0 ? ImVec4 (0.95f, 0.85f, 0.30f, 1.0f)
                                     : ImVec4 (0.95f, 0.35f, 0.35f, 1.0f);
    ImGui::TextColored (col, "Leak slope  %.3f MB/min (5 min window)", slopeMB);

    ImGui::Separator();

    if (ImGui::Button ("Mark baseline"))
        perf.markBaseline();

    const auto d = perf.baselineDiff();
    ImGui::Text ("Baseline diff:");
    ImGui::Text ("  Private  %+.1f MB", d.privateDelta / kMB);
    ImGui::Text ("  Working  %+.1f MB", d.workingSetDelta / kMB);
    ImGui::Text ("  Handles  %+d", d.handleDelta);
    ImGui::Text ("  GDI      %+d", d.gdiDelta);
    ImGui::Text ("  Elapsed  %.0f s", d.seconds);
}

void DebugOverlayUI::drawFlags()
{
    bool spectrum = DebugFlags::disableSpectrum.load();
    if (ImGui::Checkbox ("Disable spectrum analyzer", &spectrum))
        DebugFlags::disableSpectrum.store (spectrum);

    bool thumbs = DebugFlags::disableWaveformThumbnails.load();
    if (ImGui::Checkbox ("Disable waveform thumbnails", &thumbs))
        DebugFlags::disableWaveformThumbnails.store (thumbs);

    bool logHitches = DebugFlags::logHitchesToFile.load();
    if (ImGui::Checkbox ("Log hitches to file", &logHitches))
        DebugFlags::logHitchesToFile.store (logHitches);
}

juce::String DebugOverlayUI::projectExtraSection()
{
    const auto snapshot = getProjectSnapshot();
    juce::String s;
    s << "\n[Project]\n"
      << "name            " << snapshot.name.c_str() << "\n"
      << "mode            " << (snapshot.songMode ? "SONG" : "PATTERN") << "\n"
      << "bpm             " << juce::String (snapshot.bpm, 1) << "\n"
      << "channels        " << snapshot.channels << "\n"
      << "patterns        " << snapshot.patterns << "\n"
      << "playlistClips   " << snapshot.playlistClips << "\n"
      << "audioClips      " << snapshot.audioClips << "\n"
      << "mixerTracks     " << snapshot.mixerTracks << "\n"
      << "vstTotal        " << snapshot.pluginTotal << "\n"
      << "vstLoaded       " << snapshot.pluginLoaded << "\n"
      << "vstLoading      " << snapshot.pluginLoading << "\n"
      << "vstMissing      " << snapshot.pluginMissing << "\n"
      << "builtinEffects  " << snapshot.builtinEffects << "\n"
      << "missingMedia    " << snapshot.missingMedia << "\n\n"
      << "[VST Instances]\n";
    for (const auto& plugin : snapshot.plugins)
        s << pluginStatus (plugin.loaded, plugin.loading) << "\t"
          << plugin.name.c_str() << "\t" << plugin.format.c_str()
          << "\tcurrent=" << juce::String (plugin.load * 100.0f, 1)
          << "%\tpeak=" << juce::String (plugin.peak * 100.0f, 1)
          << "%\t" << plugin.location.c_str() << "\n";
    return s;
}

juce::String DebugOverlayUI::audioExtraSection()
{
    const auto& ap = engine.getAudioPerfStats();
    juce::String s;
    s << "\n[Audio]\n"
      << "load        " << juce::String (ap.load.load() * 100.0f, 1) << " %\n"
      << "peak        " << juce::String (ap.loadPeak.load() * 100.0f, 1) << " %\n"
      << "lockWaitMs  " << juce::String (ap.lockWaitMs.load(), 2) << " ms\n"
      << "xruns       " << (int) ap.xruns.load() << "\n"
      << "callbacks   " << (int) ap.callbacks.load() << "\n"
      << "buffer      " << ap.bufferSamples.load() << " samples\n"
      << "sampleRate  " << juce::String (ap.sampleRate.load(), 0) << " Hz\n";
    return s;
}

void DebugOverlayUI::drawReport()
{
    if (ImGui::Button ("Write report file"))
    {
        const juce::File f = perf.writeReport (projectExtraSection() + audioExtraSection());
        lastReportPath = f.getFullPathName().toStdString();
    }
    if (! lastReportPath.empty())
        ImGui::Text ("Wrote: %s", lastReportPath.c_str());

    if (ImGui::Button ("Copy report to clipboard"))
    {
        const juce::String txt = perf.buildReportText() + projectExtraSection() + audioExtraSection();
        ImGui::SetClipboardText (txt.toRawUTF8());
    }
}
