#include "PluginHost.h"
#include <atomic>
#include <cstring>
#include <set>
#include <map>
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
 #include <objbase.h>
 #include <malloc.h>
#endif
#include "../UI/DarkTitleBar.h"
#include "../UI/PluginWrapper.h"
#include "../CrashHandler.h"
#include "Bridge32Host.h"

static bool createInstanceUnguarded (juce::AudioPluginFormatManager& formats,
                                     const juce::PluginDescription& desc,
                                     double sr, int bs, juce::String& error,
                                     std::unique_ptr<juce::AudioPluginInstance>& out)
{
    out = formats.createPluginInstance (desc, sr, bs, error);
    return out != nullptr;
}
#if JUCE_WINDOWS
static int pluginSehFilter (DWORD code, DWORD& out) noexcept
{
    if (code == 0xE06D7363)
        return EXCEPTION_CONTINUE_SEARCH;
    out = code;
    return EXCEPTION_EXECUTE_HANDLER;
}

static bool scanFileSeh (juce::AudioPluginFormat& fmt,
                         juce::OwnedArray<juce::PluginDescription>& found,
                         const juce::String& fileId, bool& faulted)
{
    DWORD code = 0;
    __try
    {
        fmt.findAllTypesForFile (found, fileId);
        return true;
    }
    __except (pluginSehFilter (GetExceptionCode(), code))
    {
        if (code == EXCEPTION_STACK_OVERFLOW)
            _resetstkoflw();
        faulted = true;
        return false;
    }
}

static bool scanFileGuarded (juce::AudioPluginFormat& fmt,
                             juce::OwnedArray<juce::PluginDescription>& found,
                             const juce::String& fileId, bool& faulted)
{
    try { return scanFileSeh (fmt, found, fileId, faulted); }
    catch (...) { return false; }
}

static bool createInstanceSeh (juce::AudioPluginFormatManager& formats,
                               const juce::PluginDescription& desc,
                               double sr, int bs, juce::String& error,
                               std::unique_ptr<juce::AudioPluginInstance>& out,
                               bool& faulted)
{
    DWORD code = 0;
    __try
    {
        return createInstanceUnguarded (formats, desc, sr, bs, error, out);
    }
    __except (pluginSehFilter (GetExceptionCode(), code))
    {
        if (code == EXCEPTION_STACK_OVERFLOW)
            _resetstkoflw();
        faulted = true;
        return false;
    }
}

static bool createInstanceGuarded (juce::AudioPluginFormatManager& formats,
                                   const juce::PluginDescription& desc,
                                   double sr, int bs, juce::String& error,
                                   std::unique_ptr<juce::AudioPluginInstance>& out,
                                   bool& faulted)
{
    try
    {
        return createInstanceSeh (formats, desc, sr, bs, error, out, faulted);
    }
    catch (const std::exception& e)
    {
        if (error.isEmpty())
            error = juce::String (e.what());
        return false;
    }
    catch (...)
    {
        if (error.isEmpty())
            error = desc.name + " threw while loading.";
        return false;
    }
}
#else
static bool scanFileGuarded (juce::AudioPluginFormat& fmt,
                             juce::OwnedArray<juce::PluginDescription>& found,
                             const juce::String& fileId, bool& faulted)
{
    juce::ignoreUnused (faulted);
    try { fmt.findAllTypesForFile (found, fileId); return true; }
    catch (...) { return false; }
}

static bool createInstanceGuarded (juce::AudioPluginFormatManager& formats,
                                   const juce::PluginDescription& desc,
                                   double sr, int bs, juce::String& error,
                                   std::unique_ptr<juce::AudioPluginInstance>& out,
                                   bool& faulted)
{
    juce::ignoreUnused (faulted);
    try { return createInstanceUnguarded (formats, desc, sr, bs, error, out); }
    catch (...) { return false; }
}
#endif
enum class DllVerdict { vst2, foreignNative, wrongArch, notAPlugin, vst2x86 };

static DllVerdict inspectDllExports (const juce::File& file)
{
    juce::MemoryMappedFile map (file, juce::MemoryMappedFile::readOnly);
    auto* data = static_cast<const juce::uint8*> (map.getData());
    auto size = (juce::int64) map.getSize();

    auto inRange = [size] (juce::int64 off, juce::int64 n)
    {
        return off >= 0 && n >= 0 && off + n <= size;
    };
    auto u16 = [data] (juce::int64 off) -> juce::uint32
    {
        return data[off] | (juce::uint32 (data[off + 1]) << 8);
    };
    auto u32 = [data] (juce::int64 off) -> juce::uint32
    {
        return data[off] | (juce::uint32 (data[off + 1]) << 8)
             | (juce::uint32 (data[off + 2]) << 16) | (juce::uint32 (data[off + 3]) << 24);
    };

    if (data == nullptr || size < 0x40 || data[0] != 'M' || data[1] != 'Z')
        return DllVerdict::notAPlugin;

    juce::int64 pe = u32 (0x3c);
    if (! inRange (pe, 24) || u32 (pe) != 0x00004550)
        return DllVerdict::notAPlugin;

    const juce::uint32 machine = u16 (pe + 4);
    const bool is64 = machine == 0x8664;
    const bool is32 = machine == 0x14c;
    if (! is64 && ! is32)
        return DllVerdict::wrongArch;

    const int numSections = (int) u16 (pe + 6);
    const juce::int64 optSize = u16 (pe + 20);
    const juce::int64 opt = pe + 24;
    const juce::uint32 optMagic  = is64 ? 0x20bu : 0x10bu;
    const juce::int64  exportDir = is64 ? 112 : 96;
    if (optSize < exportDir + 8 || ! inRange (opt, optSize) || u16 (opt) != optMagic)
        return DllVerdict::notAPlugin;

    const juce::uint32 exportRva = u32 (opt + exportDir);
    if (exportRva == 0)
        return DllVerdict::notAPlugin;

    const juce::int64 sections = opt + optSize;
    if (! inRange (sections, numSections * 40))
        return DllVerdict::notAPlugin;

    auto rvaToOffset = [&] (juce::uint32 rva) -> juce::int64
    {
        for (int i = 0; i < numSections; ++i)
        {
            juce::int64 s = sections + i * 40;
            auto va = u32 (s + 12), vsize = u32 (s + 8);
            auto rawOff = u32 (s + 20), rawSize = u32 (s + 16);
            if (rva >= va && rva < va + juce::jmax (vsize, rawSize))
                return (juce::int64) rawOff + (rva - va);
        }
        return -1;
    };

    auto exp = rvaToOffset (exportRva);
    if (! inRange (exp, 40))
        return DllVerdict::notAPlugin;

    const auto numNames = juce::jmin (u32 (exp + 24), 65536u);
    auto names = rvaToOffset (u32 (exp + 32));
    if (! inRange (names, (juce::int64) numNames * 4))
        return DllVerdict::notAPlugin;

    bool fl = false;
    for (juce::uint32 i = 0; i < numNames; ++i)
    {
        auto nameOff = rvaToOffset (u32 (names + (juce::int64) i * 4));
        if (nameOff < 0)
            continue;
        char buf[20] = {};
        for (int c = 0; c < 19 && inRange (nameOff + c, 1) && data[nameOff + c] != 0; ++c)
            buf[c] = (char) data[nameOff + c];

        if (strcmp (buf, "VSTPluginMain") == 0 || strcmp (buf, "main") == 0)
            return is64 ? DllVerdict::vst2 : DllVerdict::vst2x86;
        if (strcmp (buf, "CreatePlugInstance") == 0)
            fl = true;
    }
    return fl ? DllVerdict::foreignNative : DllVerdict::notAPlugin;
}

static bool applyFLStudioName (juce::PluginDescription& d)
{
    juce::File f (d.fileOrIdentifier);
    if (! f.getFileNameWithoutExtension().equalsIgnoreCase ("engine_x64"))
        return false;
    auto folder = f.getParentDirectory().getFileName();
    if (folder.isEmpty() || d.name == folder)
        return false;
    d.name = folder;
    d.descriptiveName = folder;
    if (d.manufacturerName.isEmpty())
        d.manufacturerName = "Image-Line";
    return true;
}

class PluginHost::ScanThread : public juce::Thread
{
public:
    ScanThread (PluginHost& h,
                std::function<void (const juce::String&)> progress,
                std::function<void (int)> done)
        : juce::Thread ("Plugin Scanner"), host (h),
          onProgress (std::move (progress)), onDone (std::move (done)) {}

    ~ScanThread() override { stopThread (8000); }

    std::atomic<bool> stopFlag { false };

    void run() override
    {
       #if JUCE_WINDOWS
        const bool comOk = SUCCEEDED (CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED));
       #endif

        auto startTime = juce::Time::getMillisecondCounterHiRes();
        juce::StringArray failures;

        {
            auto flFolders = host.isFLStudioScanEnabled() ? host.getFLStudioPluginFolders()
                                                          : juce::Array<juce::File>();
            bool changed = false;
            for (auto d : host.knownPlugins.getTypes())
            {
                juce::File f (d.fileOrIdentifier);
                if (! f.getFileNameWithoutExtension().equalsIgnoreCase ("engine_x64"))
                    continue;

                bool current = false;
                for (auto& folder : flFolders)
                    if (f.isAChildOf (folder))
                        current = true;

                if (! current)
                {
                    host.knownPlugins.removeType (d);
                    changed = true;
                }
                else if (applyFLStudioName (d))
                {
                    host.knownPlugins.removeType (d);
                    host.knownPlugins.addType (d);
                    changed = true;
                }
            }
            if (changed)
                host.saveKnownPlugins();
        }

        {
            bool changed = false;
            for (auto d : host.unverifiedPlugins.getTypes())
            {
                juce::File f (d.fileOrIdentifier);
                if (d.pluginFormatName == "VST" && f.existsAsFile()
                     && inspectDllExports (f) != DllVerdict::vst2)
                {
                    host.unverifiedPlugins.removeType (d);
                    changed = true;
                }
                else if (Bridge32::isBridged (d)
                          && (! f.existsAsFile() || ! Bridge32::isAvailable()
                              || inspectDllExports (f) != DllVerdict::vst2x86))
                {
                    host.unverifiedPlugins.removeType (d);
                    changed = true;
                }
            }
            if (changed)
                host.saveUnverifiedPlugins();
        }

        const bool verify = host.isVerifyEnabled();

        juce::StringArray preExisting;
        for (const auto& d : host.knownPlugins.getTypes())
            preExisting.add (d.createIdentifierString());
        for (const auto& d : host.unverifiedPlugins.getTypes())
            preExisting.add (d.createIdentifierString());

        std::set<juce::String> knownFileIds, unverifiedFileIds;
        for (const auto& d : host.knownPlugins.getTypes())
            knownFileIds.insert (d.fileOrIdentifier);
        for (const auto& d : host.unverifiedPlugins.getTypes())
            unverifiedFileIds.insert (d.fileOrIdentifier);

        auto tsCache = loadTimestampCache();

        if (Bridge32::isAvailable() && ! host.settings->getBoolValue ("bridge32TsPurged", false))
        {
            bool purged = false;
            for (auto it = tsCache.begin(); it != tsCache.end(); )
            {
                juce::File f (it->first);
                if (it->first.endsWithIgnoreCase (".dll") && f.existsAsFile()
                    && inspectDllExports (f) == DllVerdict::vst2x86)
                {
                    it = tsCache.erase (it);
                    purged = true;
                }
                else
                    ++it;
            }
            if (purged)
                saveTimestampCache (tsCache);
            host.settings->setValue ("bridge32TsPurged", true);
            host.settings->saveIfNeeded();
        }

        report ("Enumerating plugin files...");

        struct FileJob { juce::String fileId; juce::AudioPluginFormat* format; };
        std::vector<FileJob> jobs;
        std::set<juce::String> queuedFileIds;
        int skipped = 0, blacklisted = 0;

        for (auto* format : host.formatManager.getFormats())
        {
            if (shouldStop()) break;
            auto searchPath = host.getSearchPathForFormat (*format);
            auto files = format->searchPathsForPlugins (searchPath, true, false);

            for (const auto& f : files)
            {
                if (! queuedFileIds.insert (f).second) { ++skipped; continue; }
                if (knownFileIds.count (f) > 0)  { ++skipped; continue; }

                if (host.isCrashBlacklisted (f))
                {
                    ++blacklisted;
                    continue;
                }

                bool wasUnverified = unverifiedFileIds.count (f) > 0;
                if (! verify && wasUnverified)   { ++skipped; continue; }

                juce::File file (f);
                if (! wasUnverified && file.existsAsFile())
                {
                    auto modTime = file.getLastModificationTime().toMilliseconds();
                    auto it = tsCache.find (f);
                    if (it != tsCache.end() && it->second == modTime)
                    { ++skipped; continue; }
                }

                jobs.push_back ({ f, format });
            }
        }

        host.lastScanBlacklisted = blacklisted;

        int totalJobs = (int) jobs.size();
        if (totalJobs == 0 || shouldStop())
        {
            auto elapsed = (juce::Time::getMillisecondCounterHiRes() - startTime) / 1000.0;
            host.lastScanSeconds = elapsed;
            host.lastScanCancelled = shouldStop();
            if (! shouldStop())
                report ("Nothing new (" + juce::String (skipped) + " skipped"
                        + (blacklisted > 0 ? ", " + juce::String (blacklisted) + " blacklisted" : juce::String())
                        + ", " + juce::String (elapsed, 2) + "s).");
            finish (preExisting, failures);
           #if JUCE_WINDOWS
            if (comOk) CoUninitialize();
           #endif
            return;
        }

        int done = 0, failed = 0;
        juce::uint32 lastReport = 0, lastListSave = juce::Time::getMillisecondCounter();

        std::vector<std::pair<juce::String, juce::int64>> pendingStamps;

        auto flushLists = [&] (juce::uint32 now)
        {
            lastListSave = now;
            host.saveKnownPlugins();
            host.saveUnverifiedPlugins();
            for (const auto& [path, ts] : pendingStamps)
                appendTimestamp (path, ts);
            pendingStamps.clear();
        };

        for (const auto& job : jobs)
        {
            if (shouldStop())
                break;

            ++done;
            auto now = juce::Time::getMillisecondCounter();
            if (now - lastReport >= 100 || done == totalJobs)
            {
                lastReport = now;
                report (job.format->getName() + " (" + juce::String (done) + "/"
                        + juce::String (totalJobs) + "): "
                        + juce::File (job.fileId).getFileNameWithoutExtension());
            }

            juce::File jobFile (job.fileId);
           #if JUCE_WINDOWS
            if (job.format->getName() == "VST" && jobFile.existsAsFile())
            {
                auto verdict = inspectDllExports (jobFile);

                if (verdict == DllVerdict::vst2x86)
                {
                    auto stampTs = [&]
                    {
                        tsCache[job.fileId] = jobFile.getLastModificationTime().toMilliseconds();
                        pendingStamps.push_back ({ job.fileId, tsCache[job.fileId] });
                    };

                    if (! Bridge32::isAvailable())
                    {
                        failures.add (jobFile.getFileName() + " (32-bit plugin - bridge not built)");
                        stampTs();
                        host.removeUnverifiedFile (job.fileId);
                        continue;
                    }

                    if (verify)
                    {
                        juce::PluginDescription d;
                        juce::String scanError;
                        if (Bridge32::scanPlugin (jobFile, d, scanError))
                        {
                            host.knownPlugins.addType (d);
                        }
                        else
                        {
                            host.blacklistPlugin (job.fileId);
                            failures.add (jobFile.getFileName() + " (32-bit bridge scan failed)");
                        }
                        host.removeUnverifiedFile (job.fileId);
                    }
                    else
                    {
                        juce::PluginDescription d;
                        d.name = jobFile.getFileNameWithoutExtension();
                        d.fileOrIdentifier = job.fileId;
                        d.pluginFormatName = Bridge32::formatName;
                        d.lastFileModTime = jobFile.getLastModificationTime();
                        host.unverifiedPlugins.addType (d);
                    }

                    stampTs();
                    if (now - lastListSave >= 2000)
                        flushLists (now);
                    continue;
                }

                if (verdict != DllVerdict::vst2)
                {
                    if (verdict == DllVerdict::wrongArch)
                        failures.add (jobFile.getFileName() + " (unsupported CPU architecture)");

                    tsCache[job.fileId] = jobFile.getLastModificationTime().toMilliseconds();
                    pendingStamps.push_back ({ job.fileId, tsCache[job.fileId] });
                    host.removeUnverifiedFile (job.fileId);
                    continue;
                }
            }
           #endif

            bool addedPlugins = false;

            if (! verify)
            {
                juce::File f (job.fileId);
                juce::PluginDescription d;
                d.name = f.exists() ? f.getFileNameWithoutExtension() : job.fileId;
                d.fileOrIdentifier = job.fileId;
                d.pluginFormatName = job.format->getName();
                d.lastFileModTime = f.getLastModificationTime();
                applyFLStudioName (d);
                host.unverifiedPlugins.addType (d);
                addedPlugins = true;
            }
            else
            {
                host.pedalEnter (job.fileId);

                juce::OwnedArray<juce::PluginDescription> found;
                bool faulted = false;
                if (! scanFileGuarded (*job.format, found, job.fileId, faulted))
                {
                    ++failed;
                    if (faulted)
                        host.blacklistPlugin (job.fileId);
                    failures.add (juce::File (job.fileId).getFileName()
                                  + "  [" + job.format->getName() + "]"
                                  + (faulted ? "" : " (threw)"));
                }

                host.pedalLeave (job.fileId);

                for (auto* d : found)
                {
                    applyFLStudioName (*d);
                    host.knownPlugins.addType (*d);
                }
                addedPlugins = found.size() > 0;

                host.removeUnverifiedFile (job.fileId);
            }

            juce::File file (job.fileId);
            if (file.existsAsFile())
            {
                tsCache[job.fileId] = file.getLastModificationTime().toMilliseconds();
                pendingStamps.push_back ({ job.fileId, tsCache[job.fileId] });
            }

            if (addedPlugins && now - lastListSave >= 2000)
                flushLists (now);
        }

        flushLists (juce::Time::getMillisecondCounter());
        saveTimestampCache (tsCache);

        auto elapsed = (juce::Time::getMillisecondCounterHiRes() - startTime) / 1000.0;
        host.lastScanSeconds = elapsed;
        host.lastScanCancelled = shouldStop();

        finish (preExisting, failures);

       #if JUCE_WINDOWS
        if (comOk) CoUninitialize();
       #endif
    }

private:
    bool shouldStop() const
    {
        return threadShouldExit() || stopFlag.load (std::memory_order_relaxed);
    }

    void report (const juce::String& msg)
    {
        if (onProgress)
            juce::MessageManager::callAsync ([cb = onProgress, msg] { cb (msg); });
    }

    void finish (const juce::StringArray& preExisting, const juce::StringArray& failures)
    {
        auto* h = &host;
        auto cb = onDone;
        juce::MessageManager::callAsync ([h, cb, preExisting, failures]
        {
            h->newlyFound.clear();
            for (const auto& d : h->knownPlugins.getTypes())
                if (! preExisting.contains (d.createIdentifierString()))
                    h->newlyFound.add (d.createIdentifierString());
            for (const auto& d : h->unverifiedPlugins.getTypes())
                if (! preExisting.contains (d.createIdentifierString()))
                    h->newlyFound.add (d.createIdentifierString());

            h->lastScanFailures = h->startupScanNotes;
            h->lastScanFailures.addArray (failures);
            h->saveKnownPlugins();
            h->saveUnverifiedPlugins();
            if (cb)
                cb (h->knownPlugins.getNumTypes() + h->unverifiedPlugins.getNumTypes());
            if (h->onPluginsChanged)
                h->onPluginsChanged();
        });
    }

    std::map<juce::String, juce::int64> loadTimestampCache() const
    {
        std::map<juce::String, juce::int64> cache;
        auto f = host.getTimestampCacheFile();
        if (! f.existsAsFile()) return cache;

        juce::StringArray lines;
        lines.addLines (f.loadFileAsString());
        for (const auto& line : lines)
        {
            auto sep = line.lastIndexOfChar ('|');
            if (sep > 0)
                cache[line.substring (0, sep)] = line.substring (sep + 1).getLargeIntValue();
        }
        return cache;
    }

    void saveTimestampCache (const std::map<juce::String, juce::int64>& cache) const
    {
        juce::String content;
        content.preallocateBytes (cache.size() * 120);
        for (const auto& [path, ts] : cache)
            content += path + "|" + juce::String (ts) + "\n";
        host.getTimestampCacheFile().replaceWithText (content);
    }

    void appendTimestamp (const juce::String& path, juce::int64 ts) const
    {
        juce::FileOutputStream out (host.getTimestampCacheFile());
        if (out.openedOk())
        {
            out.setPosition (out.getFile().getSize());
            out << path << "|" << juce::String (ts) << "\n";
            out.flush();
        }
    }

    PluginHost& host;
    std::function<void (const juce::String&)> onProgress;
    std::function<void (int)> onDone;
};
PluginHost::PluginHost()
{
    formatManager.addDefaultFormats();

    juce::PropertiesFile::Options opts;
    opts.applicationName = "mikeDAW";
    opts.filenameSuffix = ".settings";
    opts.folderName = "mikeDAW";
    opts.osxLibrarySubFolder = "Application Support";
    settings = std::make_unique<juce::PropertiesFile> (opts);

    consumeStartupPedal();

    if (auto xml = juce::parseXML (getCacheFile()))
        knownPlugins.recreateFromXml (*xml);

    if (auto xml = juce::parseXML (getUnverifiedCacheFile()))
        unverifiedPlugins.recreateFromXml (*xml);
}

PluginHost::~PluginHost()
{
    scanThread.reset();
    settings->saveIfNeeded();
}

juce::File PluginHost::getCacheFile() const
{
    return settings->getFile().getSiblingFile ("plugin-cache.xml");
}

juce::File PluginHost::getUnverifiedCacheFile() const
{
    return settings->getFile().getSiblingFile ("plugins-unverified.xml");
}

juce::File PluginHost::getTimestampCacheFile() const
{
    return settings->getFile().getSiblingFile ("scan-timestamps.txt");
}

juce::File PluginHost::pedalFile() const
{
    return settings->getFile().getSiblingFile ("scan-inflight.txt");
}

juce::File PluginHost::crashedListFile() const
{
    return settings->getFile().getSiblingFile ("scan-crashed-plugins.txt");
}

void PluginHost::loadCrashedListLocked()
{
    if (crashedLoaded)
        return;
    crashedLoaded = true;

    juce::StringArray lines;
    lines.addLines (crashedListFile().loadFileAsString());
    auto isNumber = [] (const juce::String& t) { return t.isNotEmpty() && t.containsOnly ("0123456789"); };

    for (const auto& raw : lines)
    {
        auto line = raw.trim();
        if (line.isEmpty())
            continue;

        auto parts = juce::StringArray::fromTokens (line, "|", "");
        CrashEntry entry;
        juce::String path = line;

        if (parts.size() >= 3 && isNumber (parts[parts.size() - 1]) && isNumber (parts[parts.size() - 2]))
        {
            entry.strikes = parts[parts.size() - 1].getIntValue();
            entry.modTime = parts[parts.size() - 2].getLargeIntValue();
            parts.remove (parts.size() - 1);
            parts.remove (parts.size() - 1);
            path = parts.joinIntoString ("|");
        }
        else if (parts.size() >= 2 && isNumber (parts[parts.size() - 1]))
        {
            entry.modTime = parts[parts.size() - 1].getLargeIntValue();
            entry.strikes = 2;
            parts.remove (parts.size() - 1);
            path = parts.joinIntoString ("|");
        }
        else
        {
            entry.strikes = 2;
        }

        if (path.isNotEmpty())
            crashedCache[path] = entry;
    }
}

void PluginHost::saveCrashedListLocked() const
{
    juce::StringArray lines;
    for (const auto& [path, entry] : crashedCache)
        lines.add (path + "|" + juce::String (entry.modTime) + "|" + juce::String (entry.strikes));
    crashedListFile().replaceWithText (lines.joinIntoString ("\n") + "\n");
}

void PluginHost::blacklistPlugin (const juce::String& fileId)
{
    std::lock_guard<std::mutex> lock (crashStateMutex);
    loadCrashedListLocked();

    juce::File f (fileId);
    auto& entry = crashedCache[fileId];
    entry.modTime = f.existsAsFile() ? f.getLastModificationTime().toMilliseconds() : 0;
    entry.strikes = 2;
    saveCrashedListLocked();
}

bool PluginHost::isCrashBlacklisted (const juce::String& fileId)
{
    std::lock_guard<std::mutex> lock (crashStateMutex);
    loadCrashedListLocked();

    auto it = crashedCache.find (fileId);
    if (it == crashedCache.end())
        return false;

    juce::File f (fileId);
    if (it->second.modTime != 0 && f.existsAsFile()
        && f.getLastModificationTime().toMilliseconds() != it->second.modTime)
    {
        crashedCache.erase (it);
        saveCrashedListLocked();
        return false;
    }
    return it->second.strikes >= 2;
}

void PluginHost::writePedalLocked() const
{
    if (inflightPaths.empty())
    {
        pedalFile().deleteFile();
        return;
    }

    juce::String content;
    for (const auto& path : inflightPaths)
        content += path + "\n";
    pedalFile().replaceWithText (content);
}

void PluginHost::pedalEnter (const juce::String& fileId)
{
    std::lock_guard<std::mutex> lock (crashStateMutex);
    inflightPaths.insert (fileId);
    writePedalLocked();
}

void PluginHost::pedalLeave (const juce::String& fileId)
{
    std::lock_guard<std::mutex> lock (crashStateMutex);
    inflightPaths.erase (fileId);
    writePedalLocked();
}

void PluginHost::clearPedalOnCleanExit()
{
    std::lock_guard<std::mutex> lock (crashStateMutex);
    if (inflightPaths.empty())
        pedalFile().deleteFile();
}

void PluginHost::consumeStartupPedal()
{
    juce::StringArray notes;
    {
        std::lock_guard<std::mutex> lock (crashStateMutex);
        loadCrashedListLocked();

        juce::StringArray inflight;
        inflight.addLines (pedalFile().loadFileAsString());
        pedalFile().deleteFile();
        inflightPaths.clear();

        inflight.trim();
        inflight.removeEmptyStrings();
        const bool lone = inflight.size() == 1;

        bool changed = false;
        for (const auto& path : inflight)
        {
            juce::File f (path);
            if (! lone)
            {
                notes.add (f.getFileName() + " (was loading when the app last exited)");
                continue;
            }
            const auto modTime = f.existsAsFile() ? f.getLastModificationTime().toMilliseconds() : 0;

            auto& entry = crashedCache[path];
            if (entry.modTime != modTime)
                entry.strikes = 0;
            entry.modTime = modTime;
            ++entry.strikes;
            changed = true;

            notes.add (f.getFileName()
                       + (entry.strikes >= 2
                              ? juce::String (" (crashed while loading - blacklisted)")
                              : juce::String (" (did not finish loading last launch - one more strike blacklists it)")));
        }

        if (changed)
            saveCrashedListLocked();
    }

    startupScanNotes.addArray (notes);
    lastScanFailures.addArray (notes);
}

void PluginHost::saveKnownPlugins()
{
    if (auto xml = knownPlugins.createXml())
        xml->writeTo (getCacheFile());
}

void PluginHost::saveUnverifiedPlugins()
{
    if (auto xml = unverifiedPlugins.createXml())
        xml->writeTo (getUnverifiedCacheFile());
}

void PluginHost::removeUnverifiedFile (const juce::String& fileId)
{
    for (const auto& d : unverifiedPlugins.getTypes())
        if (d.fileOrIdentifier == fileId)
            unverifiedPlugins.removeType (d);
}

bool PluginHost::isVerifyEnabled() const
{
    return settings->getBoolValue ("verifyPlugins", false);
}

void PluginHost::setVerifyEnabled (bool enabled)
{
    settings->setValue ("verifyPlugins", enabled);
    settings->saveIfNeeded();
}

bool PluginHost::isUnverified (const juce::PluginDescription& desc) const
{
    if (desc.fileOrIdentifier.isEmpty())
        return false;
    for (const auto& d : unverifiedPlugins.getTypes())
        if (d.fileOrIdentifier == desc.fileOrIdentifier)
            return true;
    return false;
}

std::unique_lock<std::mutex> PluginHost::lockOffMessageThread (std::mutex& m)
{
    if (juce::MessageManager::existsAndIsCurrentThread())
        return {};
    return std::unique_lock<std::mutex> (m);
}

bool PluginHost::resolveForLoad (juce::PluginDescription& desc, juce::String& error)
{
    const auto resolveLock = lockOffMessageThread (resolveForLoadMutex);

    if (! isUnverified (desc))
        return true;

    auto firePluginsChanged = [this]
    {
        if (onPluginsChanged)
        {
            if (juce::MessageManager::existsAndIsCurrentThread())
                onPluginsChanged();
            else
                juce::MessageManager::callAsync ([this] { if (onPluginsChanged) onPluginsChanged(); });
        }
    };

    auto promoteBridged = [&] () -> bool
    {
        juce::PluginDescription scanned;
        juce::String scanError;
        if (! Bridge32::scanPlugin (juce::File (desc.fileOrIdentifier), scanned, scanError))
        {
            removeUnverifiedFile (desc.fileOrIdentifier);
            saveUnverifiedPlugins();
            blacklistPlugin (desc.fileOrIdentifier);
            error = scanError.isNotEmpty() ? scanError
                                           : desc.name + " (32-bit bridge scan failed)";
            return false;
        }
        knownPlugins.addType (scanned);
        saveKnownPlugins();
        removeUnverifiedFile (desc.fileOrIdentifier);
        saveUnverifiedPlugins();
        firePluginsChanged();
        desc = scanned;
        return true;
    };

    if (Bridge32::isBridged (desc))
    {
        if (! Bridge32::isAvailable())
        {
            error = desc.name + " is a 32-bit plugin and the 32-bit bridge isn't available.";
            return false;
        }
        return promoteBridged();
    }

    juce::AudioPluginFormat* format = nullptr;
    for (auto* f : formatManager.getFormats())
        if (f->getName() == desc.pluginFormatName)
            format = f;
    if (format == nullptr)
    {
        error = "Unknown plugin format: " + desc.pluginFormatName;
        return false;
    }

   #if JUCE_WINDOWS
    juce::File file (desc.fileOrIdentifier);
    if (desc.pluginFormatName == "VST" && file.existsAsFile())
    {
        auto verdict = inspectDllExports (file);
        if (verdict == DllVerdict::vst2x86 && Bridge32::isAvailable())
        {
            desc.pluginFormatName = Bridge32::formatName;
            return promoteBridged();
        }
        if (verdict != DllVerdict::vst2)
        {
            removeUnverifiedFile (desc.fileOrIdentifier);
            saveUnverifiedPlugins();
            error = verdict == DllVerdict::foreignNative
                        ? desc.name + " is an FL-native plugin, not a VST. FL plugins can only run inside FL Studio."
                        : verdict == DllVerdict::vst2x86
                              ? desc.name + " is a 32-bit plugin and the 32-bit bridge isn't available."
                              : verdict == DllVerdict::wrongArch
                                    ? desc.name + " is not a supported plugin architecture."
                                    : desc.name + " is not a VST plugin.";
            return false;
        }
    }
   #endif

    pedalEnter (desc.fileOrIdentifier);

    juce::OwnedArray<juce::PluginDescription> found;
    bool faulted = false;
    bool ok = scanFileGuarded (*format, found, desc.fileOrIdentifier, faulted);

    pedalLeave (desc.fileOrIdentifier);
    removeUnverifiedFile (desc.fileOrIdentifier);
    saveUnverifiedPlugins();

    if (! ok)
    {
        if (faulted)
        {
            blacklistPlugin (desc.fileOrIdentifier);
            error = desc.name + " crashed while loading and was blacklisted.";
        }
        else
            error = desc.name + " threw while loading.";
        return false;
    }

    if (found.isEmpty())
    {
        error = desc.name + " is not a loadable plugin.";
        return false;
    }

    for (auto* d : found)
    {
        applyFLStudioName (*d);
        knownPlugins.addType (*d);
    }
    saveKnownPlugins();

    firePluginsChanged();

    desc = *found[0];
    return true;
}

void PluginHost::clearPluginList()
{
    if (isScanning())
        return;
    knownPlugins.clear();
    unverifiedPlugins.clear();
    newlyFound.clear();
    lastScanFailures.clear();
    lastScanBlacklisted = 0;
    getCacheFile().deleteFile();
    getUnverifiedCacheFile().deleteFile();
    getTimestampCacheFile().deleteFile();
}

bool PluginHost::isScanning() const
{
    return scanThread != nullptr && scanThread->isThreadRunning();
}

static const char* const vst3Dirs[] = {
    "C:\\Program Files\\Common Files\\VST3",
    "C:\\Program Files (x86)\\Common Files\\VST3",
};

static const char* const vst2Dirs[] = {
    "C:\\Program Files (x86)\\Common Files\\VST2",
    "C:\\Program Files (x86)\\Steinberg\\VstPlugins",
    "C:\\Program Files\\Common Files\\VST2",
    "C:\\Program Files\\VstPlugins",
    "C:\\Program Files\\Common Files\\Steinberg\\VST2",
    "C:\\Program Files\\Steinberg\\VstPlugins",
};

juce::FileSearchPath PluginHost::getSearchPathForFormat (juce::AudioPluginFormat& format,
                                                         bool includeUserFolders) const
{
    juce::FileSearchPath path;

    auto defaults = format.getDefaultLocationsToSearch();
    for (int i = 0; i < defaults.getNumPaths(); ++i)
        if (! defaults.getRawString (i).containsChar ('%')
            && ! defaults[i].getFullPathName().containsChar ('%'))
            path.addIfNotAlreadyThere (defaults[i]);

    auto addIfDir = [&path] (const juce::File& f)
    {
        if (f.isDirectory())
            path.addIfNotAlreadyThere (f);
    };

    if (format.getName() == "VST3")
        for (auto* d : vst3Dirs) addIfDir (juce::File (d));
    else if (format.getName() == "VST")
        for (auto* d : vst2Dirs) addIfDir (juce::File (d));
    else if (format.getName() == "LV2")
    {
        addIfDir (juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                      .getChildFile ("LV2"));
        addIfDir (juce::File (juce::SystemStats::getEnvironmentVariable ("COMMONPROGRAMFILES",
                                                                        "C:\\Program Files\\Common Files"))
                      .getChildFile ("LV2"));
    }

   #if JUCE_LINUX
    auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    if (format.getName() == "VST")
        addIfDir (home.getChildFile (".vst").getChildFile ("yabridge"));
    else if (format.getName() == "VST3")
        addIfDir (home.getChildFile (".vst3").getChildFile ("yabridge"));
   #endif

    if (includeUserFolders)
        for (auto& folder : getUserScanFolders())
        {
            juce::File f (folder);
            if (f.isDirectory())
                path.addIfNotAlreadyThere (f);
        }

    if (isFLStudioScanEnabled())
        for (auto& f : getFLStudioPluginFolders())
            path.addIfNotAlreadyThere (f);

    return path;
}

bool PluginHost::isFLStudioScanEnabled() const
{
    return settings->getBoolValue ("scanFLStudioPlugins", false);
}

void PluginHost::setFLStudioScanEnabled (bool enabled)
{
    settings->setValue ("scanFLStudioPlugins", enabled);
    settings->saveIfNeeded();
}

juce::Array<juce::File> PluginHost::getFLStudioPluginFolders() const
{
    juce::File best;
    int bestVersion = -1;

    // TODO: this most likely will not catch all, figure out a better solution
    for (auto* root : { "C:\\Program Files\\Image-Line", "C:\\Program Files (x86)\\Image-Line" })
    {
        juce::File imageLine (root);
        if (! imageLine.isDirectory())
            continue;
        for (auto& version : imageLine.findChildFiles (juce::File::findDirectories, false, "FL Studio*"))
        {
            auto effects = version.getChildFile ("Plugins").getChildFile ("Fruity").getChildFile ("Effects");
            auto num = version.getFileName().getTrailingIntValue();
            if (effects.isDirectory() && num > bestVersion)
            {
                bestVersion = num;
                best = effects;
            }
        }
    }
    
    juce::Array<juce::File> out;
    if (best.isDirectory())
        out.add (best);
    return out;
}

juce::StringArray PluginHost::getScanLocations() const
{
    juce::StringArray out;
    for (auto* format : formatManager.getFormats())
    {
        auto path = getSearchPathForFormat (*format, false);
        for (int i = 0; i < path.getNumPaths(); ++i)
        {
            auto full = path[i].getFullPathName().trim();
            if (full.isEmpty())
                continue;
            out.addIfNotAlreadyThere (full + "   [" + format->getName() + "]");
        }
    }
    return out;
}

void PluginHost::startScan (std::function<void (const juce::String&)> onProgress,
                            std::function<void (int)> onDone)
{
    if (isScanning())
        return;
    scanThread = std::make_unique<ScanThread> (*this, std::move (onProgress), std::move (onDone));
    scanThread->startThread();
}

void PluginHost::stopScan()
{
    if (scanThread != nullptr)
    {
        scanThread->stopFlag.store (true, std::memory_order_release);
        scanThread->signalThreadShouldExit();
    }
}

void PluginHost::addUserScanFolder (const juce::File& folder)
{
    auto folders = getUserScanFolders();
    folders.addIfNotAlreadyThere (folder.getFullPathName());
    settings->setValue ("userScanFolders", folders.joinIntoString (";"));
    settings->saveIfNeeded();
}

void PluginHost::removeUserScanFolder (const juce::File& folder)
{
    auto folders = getUserScanFolders();
    folders.removeString (folder.getFullPathName(), true);
    settings->setValue ("userScanFolders", folders.joinIntoString (";"));
    settings->saveIfNeeded();
}

juce::StringArray PluginHost::getUserScanFolders() const
{
    juce::StringArray out;
    out.addTokens (settings->getValue ("userScanFolders"), ";", {});
    out.removeEmptyStrings();
    return out;
}

bool PluginHost::isPreferredOver (const juce::PluginDescription& a, const juce::PluginDescription& b)
{
    const bool wantX86 = a.pluginFormatName == Bridge32::formatName;
    auto inX86 = [] (const juce::PluginDescription& d)
    {
        return d.fileOrIdentifier.containsIgnoreCase ("Program Files (x86)");
    };

    const bool aRight = inX86 (a) == wantX86;
    const bool bRight = inX86 (b) == wantX86;
    if (aRight != bRight)
        return aRight;

    if (a.lastFileModTime != b.lastFileModTime)
        return a.lastFileModTime > b.lastFileModTime;

    if (a.fileOrIdentifier.length() != b.fileOrIdentifier.length())
        return a.fileOrIdentifier.length() < b.fileOrIdentifier.length();

    return a.fileOrIdentifier.compare (b.fileOrIdentifier) < 0;
}

static int installUid (const juce::PluginDescription& d)
{
    return d.uniqueId != 0 ? d.uniqueId : d.deprecatedUid;
}

static juce::String installStem (const juce::PluginDescription& d)
{
    return d.pluginFormatName + "|stem:"
         + juce::File::createFileWithoutCheckingPath (d.fileOrIdentifier)
               .getFileNameWithoutExtension().toLowerCase();
}

juce::Array<juce::PluginDescription> PluginHost::dedupeInstalls (const juce::Array<juce::PluginDescription>& in)
{
    std::set<juce::String> verifiedStems;
    for (const auto& d : in)
        if (installUid (d) != 0)
            verifiedStems.insert (installStem (d));

    std::map<juce::String, int> best;
    juce::Array<juce::PluginDescription> out;

    for (const auto& d : in)
    {
        const auto uid = installUid (d);
        if (uid == 0)
        {
            if (verifiedStems.count (installStem (d)) == 0)
                out.add (d);
            continue;
        }

        auto key = d.pluginFormatName + "|" + juce::String::toHexString (uid);
        auto it = best.find (key);
        if (it == best.end())
        {
            best[key] = out.size();
            out.add (d);
        }
        else if (isPreferredOver (d, out.getReference (it->second)))
        {
            out.set (it->second, d);
        }
    }
    return out;
}

bool PluginHost::resolveLocalMatch (const juce::PluginDescription& desc,
                                    juce::PluginDescription& local) const
{
    auto sameFormat = [&] (const juce::PluginDescription& d)
    {
        return Bridge32::formatsInterchangeable (d.pluginFormatName, desc.pluginFormatName);
    };

    bool found = false;
    auto consider = [&] (const juce::PluginDescription& d)
    {
        if (! found || isPreferredOver (d, local))
        {
            local = d;
            found = true;
        }
    };

    for (const auto& d : knownPlugins.getTypes())
        if (sameFormat (d)
            && ((desc.uniqueId != 0 && d.uniqueId == desc.uniqueId)
                || (desc.deprecatedUid != 0 && d.deprecatedUid == desc.deprecatedUid)))
            consider (d);

    if (found)
        return true;

    if (desc.name.isNotEmpty())
        for (const auto& d : knownPlugins.getTypes())
            if (sameFormat (d) && d.name.equalsIgnoreCase (desc.name))
                consider (d);

    return found;
}

bool PluginHost::resolveUnverifiedMatch (const juce::PluginDescription& desc,
                                         juce::PluginDescription& match) const
{
    auto stem = [] (const juce::String& fileOrId)
    {
        return juce::File::createFileWithoutCheckingPath (fileOrId)
                   .getFileNameWithoutExtension().toLowerCase();
    };
    const auto descStem = stem (desc.fileOrIdentifier);

    bool found = false;
    for (const auto& d : unverifiedPlugins.getTypes())
    {
        if (! Bridge32::formatsInterchangeable (d.pluginFormatName, desc.pluginFormatName))
            continue;
        const bool nameMatch = desc.name.isNotEmpty() && d.name.equalsIgnoreCase (desc.name);
        const bool stemMatch = descStem.isNotEmpty() && stem (d.fileOrIdentifier) == descStem;
        if (nameMatch || stemMatch)
        {
            if (! found || isPreferredOver (d, match))
            {
                match = d;
                found = true;
            }
        }
    }
    return found;
}

std::unique_ptr<juce::AudioPluginInstance> PluginHost::instantiateGuarded (const juce::PluginDescription& desc,
                                                                           double sr, int bs,
                                                                           juce::String& error)
{
    if (isCrashBlacklisted (desc.fileOrIdentifier))
    {
        error = desc.name + " is blacklisted: it crashed while loading.";
        return {};
    }

    pedalEnter (desc.fileOrIdentifier);

    std::unique_ptr<juce::AudioPluginInstance> inst;
    bool faulted = false;
    createInstanceGuarded (formatManager, desc, sr, bs, error, inst, faulted);

    pedalLeave (desc.fileOrIdentifier);

    if (faulted)
    {
        blacklistPlugin (desc.fileOrIdentifier);
        error = desc.name + " crashed while loading and was blacklisted.";
        return {};
    }

    if (inst != nullptr)
        CrashHandler::rearm();

    return inst;
}

std::unique_ptr<juce::AudioPluginInstance> PluginHost::createInstance (const juce::PluginDescription& desc,
                                                                       double sampleRate, int blockSize,
                                                                       juce::String& error)
{
    const double sr = sampleRate > 0 ? sampleRate : 44100.0;
    const int    bs = blockSize  > 0 ? blockSize  : 512;

    if (Bridge32::isBridged (desc))
    {
        if (juce::File (desc.fileOrIdentifier).existsAsFile())
            return Bridge32::createInstance (desc, sr, bs, error);

        juce::PluginDescription local;
        if (resolveLocalMatch (desc, local))
        {
            if (Bridge32::isBridged (local))
                return Bridge32::createInstance (local, sr, bs, error);
            const auto formatLock = lockOffMessageThread (formatManagerMutex);
            return instantiateGuarded (local, sr, bs, error);
        }

        error = desc.name + " (32-bit plugin not found on this machine)";
        return {};
    }

    const auto formatLock = lockOffMessageThread (formatManagerMutex);

    const bool fileMissing = juce::File::isAbsolutePath (desc.fileOrIdentifier)
                          && ! juce::File (desc.fileOrIdentifier).exists();

    if (! fileMissing)
    {
        if (auto inst = instantiateGuarded (desc, sr, bs, error))
            return inst;
    }

    juce::PluginDescription local;
    if (resolveLocalMatch (desc, local))
    {
        juce::String localError;
        if (Bridge32::isBridged (local))
        {
            if (auto inst = Bridge32::createInstance (local, sr, bs, localError))
                return inst;
        }
        else if (auto inst = instantiateGuarded (local, sr, bs, localError))
            return inst;
    }

    juce::PluginDescription unverified;
    if (resolveUnverifiedMatch (desc, unverified))
    {
        juce::String resolveError;
        if (resolveForLoad (unverified, resolveError))
        {
            if (Bridge32::isBridged (unverified))
                return Bridge32::createInstance (unverified, sr, bs, error);

            if (auto inst = instantiateGuarded (unverified, sr, bs, resolveError))
                return inst;
        }
    }

    if (! fileMissing)
        return {};

    return instantiateGuarded (desc, sr, bs, error);
}

juce::Array<juce::PluginDescription> PluginHost::getInstruments() const
{
    juce::Array<juce::PluginDescription> out;
    for (const auto& d : knownPlugins.getTypes())
        if (d.isInstrument)
            out.add (d);
    out.addArray (unverifiedPlugins.getTypes());
    out = dedupeInstalls (out);
    std::sort (out.begin(), out.end(),
               [] (const auto& a, const auto& b) { return a.name.compareIgnoreCase (b.name) < 0; });
    return out;
}

juce::Array<juce::PluginDescription> PluginHost::getEffects() const
{
    juce::Array<juce::PluginDescription> out;
    for (const auto& d : knownPlugins.getTypes())
        if (! d.isInstrument)
            out.add (d);
    out.addArray (unverifiedPlugins.getTypes());
    out = dedupeInstalls (out);
    std::sort (out.begin(), out.end(),
               [] (const auto& a, const auto& b) { return a.name.compareIgnoreCase (b.name) < 0; });
    return out;
}
PluginWindow::PluginWindow (const juce::String& k, std::unique_ptr<juce::Component> content,
                            bool resizable, const juce::String& title,
                            std::function<void (const juce::String&)> onClose)
    : juce::DocumentWindow (title, Colors::panelBg,
                            juce::DocumentWindow::minimiseButton
                                | juce::DocumentWindow::closeButton),
      key (k), onCloseFn (std::move (onClose))
{
    setUsingNativeTitleBar (true);
    setContentOwned (content.release(), true);
    setResizable (resizable, false);
    centreWithSize (getWidth(), getHeight());
    setAlwaysOnTop (false);
    setVisible (true);
    DarkTitleBar::apply (*this);
    DarkTitleBar::applyThemeColours (*this, Colors::header, Colors::text);
    toFront (true);
}

PluginWindow::~PluginWindow()
{
    clearContentComponent();
}

void PluginWindow::closeButtonPressed()
{
    setVisible (false);
    if (onCloseFn)
        onCloseFn (key);
}
namespace
{
    class PluginLoadingComponent : public juce::Component, private juce::Timer
    {
    public:
        explicit PluginLoadingComponent (const juce::String& pluginName) : name (pluginName)
        {
            setSize (420, 210);
            startTimerHz (30);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Colors::panelDark);
            auto c = getLocalBounds().getCentre().toFloat();

            juce::Path arc;
            arc.addCentredArc (c.x, c.y - 12.0f, 16.0f, 16.0f, angle,
                               0.0f, juce::MathConstants<float>::pi * 1.5f, true);
            g.setColour (Colors::accent);
            g.strokePath (arc, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));

            g.setColour (Colors::text);
            g.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
            g.drawText ("Loading " + name + "...",
                        getLocalBounds().withTrimmedTop (getHeight() / 2 + 14).withHeight (24),
                        juce::Justification::centredTop);
        }

    private:
        void timerCallback() override { angle += 0.22f; repaint(); }
        juce::String name;
        float angle = 0.0f;
    };
}
struct PluginWindowManager::Entry
{
    struct InteractionListener : juce::MouseListener
    {
        InteractionListener (PluginWindowManager& m, juce::String k) : mgr (m), key (std::move (k)) {}
        void mouseDown (const juce::MouseEvent&) override      { mgr.stampInteraction (key); }
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override
        { mgr.stampInteraction (key); }
        PluginWindowManager& mgr;
        juce::String key;
    };

    ~Entry()
    {
        if (window != nullptr && mouse != nullptr)
            window->removeMouseListener (mouse.get());
    }

    std::unique_ptr<PluginWindow> window;
    PluginWrapperComponent* wrapper = nullptr;
    std::unique_ptr<InteractionListener> mouse;
};

PluginWindowManager::PluginWindowManager() = default;
PluginWindowManager::~PluginWindowManager() = default;

void PluginWindowManager::setProject (juce::ValueTree projectTree)
{
    project = std::move (projectTree);
}

void PluginWindowManager::open (const juce::String& key, juce::AudioProcessor& processor,
                                const juce::String& title)
{
    stampInteraction (key);

    auto it = windows.find (key);
    if (it != windows.end() && it->second->window != nullptr)
    {
        if (it->second->wrapper == nullptr)
        {
            promoteLoadingWindow (key, processor, title);
        }
        else
        {
            auto* w = it->second->window.get();
            w->setVisible (true);
            w->setMinimised (false);
            w->toFront (true);
        }
    }
    else
    {
        createFor (key, processor, title);
    }

    if (auto* entry = (windows.count (key) ? windows[key].get() : nullptr))
    {
        if (entry->window != nullptr)
        {
            juce::Component::SafePointer<PluginWindow> safe (entry->window.get());
            auto raise = [safe]
            {
                if (safe != nullptr && safe->isVisible())
                {
                    safe->setMinimised (false);
                    safe->toFront (true);
                }
            };
            juce::Timer::callAfterDelay (0, raise);
            juce::Timer::callAfterDelay (80, raise);
        }
    }
}

void PluginWindowManager::createFor (const juce::String& key, juce::AudioProcessor& processor,
                                     const juce::String& title)
{
    auto entry = std::make_unique<Entry>();

    auto wrapper = std::make_unique<PluginWrapperComponent> (processor, project, key);
    auto* w = wrapper.get();
    entry->wrapper = w;
    w->onStateChanged = [this, key] { if (onStateChanged) onStateChanged (key); };

    entry->window = std::make_unique<PluginWindow> (key, std::move (wrapper),
        w->isEditorResizable(), title,
        [this] (const juce::String& k)
        {
            if (onWindowClosed)
                onWindowClosed (k);
        });

    entry->window->keyForwarder = [this] (const juce::KeyPress& kp)
    {
        return onKeyPress && onKeyPress (kp);
    };

    entry->mouse = std::make_unique<Entry::InteractionListener> (*this, key);
    entry->window->addMouseListener (entry->mouse.get(), true);

    {
        const int off = (int) (windows.size() % 6) * 28;
        auto* dw = entry->window.get();
        dw->setTopLeftPosition (dw->getX() + off, dw->getY() + off);
    }

    w->onPreferredSizeChanged = [this, key]
    {
        auto it = windows.find (key);
        if (it != windows.end() && it->second->window != nullptr && it->second->wrapper != nullptr)
            it->second->window->setResizable (it->second->wrapper->isEditorResizable(), false);
    };

    windows[key] = std::move (entry);
}

void PluginWindowManager::openLoading (const juce::String& key, const juce::String& title)
{
    auto it = windows.find (key);
    if (it != windows.end() && it->second->window != nullptr)
    {
        auto* w = it->second->window.get();
        w->setVisible (true);
        w->setMinimised (false);
        w->toFront (true);
        return;
    }

    auto entry = std::make_unique<Entry>();
    entry->wrapper = nullptr;
    entry->window = std::make_unique<PluginWindow> (key,
        std::make_unique<PluginLoadingComponent> (title), false, title,
        [this] (const juce::String& k) { if (onWindowClosed) onWindowClosed (k); });

    entry->window->keyForwarder = [this] (const juce::KeyPress& kp)
    { return onKeyPress && onKeyPress (kp); };

    entry->mouse = std::make_unique<Entry::InteractionListener> (*this, key);
    entry->window->addMouseListener (entry->mouse.get(), true);

    {
        const int off = (int) (windows.size() % 6) * 28;
        auto* dw = entry->window.get();
        dw->setTopLeftPosition (dw->getX() + off, dw->getY() + off);
    }

    windows[key] = std::move (entry);
}

void PluginWindowManager::promoteLoadingWindow (const juce::String& key, juce::AudioProcessor& processor,
                                                const juce::String& title)
{
    auto& entry = windows[key];
    auto* window = entry->window.get();
    const auto topLeft = window->getPosition();

    auto wrapper = std::make_unique<PluginWrapperComponent> (processor, project, key);
    auto* w = wrapper.get();
    entry->wrapper = w;
    w->onStateChanged = [this, key] { if (onStateChanged) onStateChanged (key); };
    w->onPreferredSizeChanged = [this, key]
    {
        auto it = windows.find (key);
        if (it != windows.end() && it->second->window != nullptr && it->second->wrapper != nullptr)
            it->second->window->setResizable (it->second->wrapper->isEditorResizable(), false);
    };

    window->setName (title);
    window->setContentOwned (wrapper.release(), true);
    window->setResizable (w->isEditorResizable(), false);
    window->setTopLeftPosition (topLeft);
    window->setVisible (true);
    window->setMinimised (false);
    window->toFront (true);
}

void PluginWindowManager::close (const juce::String& key)
{
    windows.erase (key);
}

void PluginWindowManager::closeAll()
{
    windows.clear();
}
