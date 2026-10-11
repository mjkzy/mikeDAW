#include "Updater.h"
#include "../UI/AppSettings.h"

#ifndef JUCE_APPLICATION_VERSION_STRING
 #define JUCE_APPLICATION_VERSION_STRING "0.0.0"
#endif

namespace
{
    constexpr int kUpdatePort = 41238;

    bool versionNewer (const juce::String& a, const juce::String& b)
    {
        auto parts = [] (const juce::String& s)
        {
            juce::Array<int> out;
            for (auto& p : juce::StringArray::fromTokens (s, ".", {}))
                out.add (p.getIntValue());
            while (out.size() < 3)
                out.add (0);
            return out;
        };
        auto pa = parts (a);
        auto pb = parts (b);
        for (int i = 0; i < 3; ++i)
            if (pa[i] != pb[i])
                return pa[i] > pb[i];
        return false;
    }
}

Updater::Updater (juce::PropertiesFile& s)
    : juce::Thread ("updater"), settings (s)
{
    startThread (juce::Thread::Priority::low);
}

Updater::~Updater()
{
    signalThreadShouldExit();
    jobReady.signal();
    stopThread (4000);
}

void Updater::checkForUpdate()
{
    {
        const juce::ScopedLock sl (lock);
        pending = Job::check;
    }
    jobReady.signal();
}

void Updater::downloadAndInstall (const juce::String& url)
{
    {
        const juce::ScopedLock sl (lock);
        pending = Job::download;
        pendingUrl = url;
    }
    jobReady.signal();
}

void Updater::run()
{
    for (;;)
    {
        jobReady.wait (-1);
        if (threadShouldExit())
            break;

        Job job;
        juce::String url;
        {
            const juce::ScopedLock sl (lock);
            job = pending;
            url = pendingUrl;
            pending = Job::none;
        }

        if (job == Job::check)
            doCheck();
        else if (job == Job::download)
            doDownload (url);
    }
}

juce::URL Updater::authedUrl (const juce::String& base) const
{
    juce::URL url (base);
    auto token = settings.getValue ("licenseToken");
    auto sig = settings.getValue ("licenseSig");
    if (token.isNotEmpty() && sig.isNotEmpty())
        url = url.withParameter ("token", token).withParameter ("sig", sig);
    return url;
}

void Updater::doCheck()
{
    auto host = AppSettings::relayHost (settings);
    auto url = authedUrl ("http://" + host + ":" + juce::String (kUpdatePort)
                          + "/latest.json");

    auto stream = url.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                              .withConnectionTimeoutMs (8000));
    if (stream == nullptr)
        return;

    auto json = juce::JSON::parse (stream->readEntireStreamAsString());
    if (! json.isObject())
        return;

    auto latest = json.getProperty ("version", "").toString();
    auto installer = json.getProperty ("installer", "").toString();
    auto notes = json.getProperty ("notes", "").toString();

    if (latest.isEmpty() || installer.isEmpty())
        return;

    if (! versionNewer (latest, JUCE_APPLICATION_VERSION_STRING))
        return;

    postUpdateAvailable (latest, notes,
                         "http://" + host + ":" + juce::String (kUpdatePort)
                             + "/" + installer);
}

void Updater::doDownload (const juce::String& url)
{
    postStatus ("Downloading update...");

    auto target = juce::File::getSpecialLocation (juce::File::tempDirectory)
                      .getChildFile ("mikeDAW-update-setup.exe");
    target.deleteFile();

    auto u = authedUrl (url);
    int statusCode = 0;
    auto stream = u.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                            .withConnectionTimeoutMs (8000)
                                            .withStatusCode (&statusCode));
    if (stream == nullptr || statusCode != 200)
    {
        postError ("Download failed (HTTP " + juce::String (statusCode) + ").");
        return;
    }

    {
        juce::FileOutputStream out (target);
        if (out.failedToOpen())
        {
            postError ("Download failed: could not write to the temp folder.");
            return;
        }
        out.writeFromInputStream (*stream, -1);
        out.flush();
    }
    stream.reset();

    if (target.getSize() <= 0)
    {
        postError ("Download failed.");
        return;
    }

    postStatus ("Installing update...");
    if (! target.startAsProcess ("/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /NORESTARTAPPLICATIONS /CLOSEAPPLICATIONS"))
    {
        postError ("Could not launch the installer.");
        return;
    }

    juce::MessageManager::callAsync ([]
    {
        if (auto* app = juce::JUCEApplication::getInstance())
            app->systemRequestedQuit();
    });
}

void Updater::postStatus (const juce::String& message)
{
    juce::WeakReference<Updater> safe (this);
    juce::MessageManager::callAsync ([safe, message]
    {
        if (auto* self = safe.get())
            if (self->onStatus) self->onStatus (message);
    });
}

void Updater::postError (const juce::String& message)
{
    juce::WeakReference<Updater> safe (this);
    juce::MessageManager::callAsync ([safe, message]
    {
        if (auto* self = safe.get())
            if (self->onError) self->onError (message);
    });
}

void Updater::postUpdateAvailable (const juce::String& version,
                                   const juce::String& notes,
                                   const juce::String& url)
{
    juce::WeakReference<Updater> safe (this);
    juce::MessageManager::callAsync ([safe, version, notes, url]
    {
        if (auto* self = safe.get())
            if (self->onUpdateAvailable) self->onUpdateAvailable (version, notes, url);
    });
}
