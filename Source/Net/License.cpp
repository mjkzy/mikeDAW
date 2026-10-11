#include "License.h"
#include "../UI/AppSettings.h"

extern "C" {
 #include "monocypher.h"
 #include "monocypher-ed25519.h"
}

#if JUCE_WINDOWS
 #include <windows.h>
 #include <intrin.h>
#endif

#ifndef JUCE_APPLICATION_VERSION_STRING
 #define JUCE_APPLICATION_VERSION_STRING "0.0.0"
#endif

#if __has_include("BuildStamp.h")
 #include "BuildStamp.h"
#endif
#ifndef MIKEDAW_BUILD_ID
 #define MIKEDAW_BUILD_ID "DEV-UNSTAMPED"
#endif

namespace
{
    const uint8_t kPubKey[32] = {
        0x7c,0x3c,0xa2,0x45,0x0a,0x79,0x0f,0x0f,0xcc,0xcb,0x0a,0xc0,0x13,0x56,0xfb,0x9f,
        0x20,0x40,0x3b,0xc4,0xc3,0xa0,0x3b,0x8f,0xdf,0xce,0x30,0x00,0x6b,0x6f,0xc1,0x46 };

    constexpr int   kLicensePort   = 41237;
    constexpr int   kHeartbeatMs   = 15 * 60 * 1000;
    constexpr int   kSocketTimeout = 6000;
}

extern "C" volatile const char mikedaw_build_marker[] =
    "bpmStateVault\0MIKEDAW::BUILD::" MIKEDAW_BUILD_ID "::" __DATE__ " " __TIME__ "::LIC::mjkzy";

LicenseManager::LicenseManager (juce::PropertiesFile& s)
    : juce::Thread ("license"), settings (s)
{
    machineId    = computeHwid();
    localUid     = AppSettings::localUserId (settings);
    displayName  = AppSettings::displayName (settings);
    License::gStartMs.store (juce::Time::currentTimeMillis());
}

LicenseManager::~LicenseManager()
{
    signalThreadShouldExit();
    notify();
    stopThread (4000);
}

void LicenseManager::start()
{
    if (loadCachedToken())
        applyState (License::State::authorized, "cached", cachedName);
    startThread (juce::Thread::Priority::low);
}

juce::String LicenseManager::licensedName() const
{
    const juce::ScopedLock sl (infoLock);
    return cachedName.isNotEmpty() ? cachedName : displayName;
}

juce::String LicenseManager::statusText() const
{
    const juce::ScopedLock sl (infoLock);
    switch (state())
    {
        case License::State::authorized:
            return "Access granted to: " + (cachedName.isNotEmpty() ? cachedName : displayName);
        case License::State::denied:
            return "Access denied (" + lastReason + ")";
        case License::State::checking:
        default:
            return "Checking license...";
    }
}

void LicenseManager::applyState (License::State st, const juce::String& reason,
                                 const juce::String& name)
{
    License::gState.store ((int) st);
    {
        const juce::ScopedLock sl (infoLock);
        lastReason = reason;
        if (name.isNotEmpty())
            cachedName = name;
    }

    juce::WeakReference<LicenseManager> safe (this);
    juce::MessageManager::callAsync ([safe, st]
    {
        if (auto* self = safe.get())
            if (self->onStateChanged) self->onStateChanged (st);
    });
}

void LicenseManager::run()
{
    for (;;)
    {
        doAuthOnce();
        wait (kHeartbeatMs);
        if (threadShouldExit())
            break;
    }
}

bool LicenseManager::doAuthOnce()
{
    juce::String host = AppSettings::relayHost (settings);

    juce::StreamingSocket sock;
    if (! sock.connect (host, kLicensePort, kSocketTimeout))
    {
        if (! (cachedExp.load() > juce::Time::currentTimeMillis()))
            applyState (License::State::denied, "offline", {});
        return false;
    }

    auto nonce = juce::String::toHexString (juce::Random::getSystemRandom().nextInt64());

    juce::DynamicObject::Ptr req (new juce::DynamicObject());
    req->setProperty ("t", "auth");
    req->setProperty ("uid", localUid);
    req->setProperty ("name", displayName);
    req->setProperty ("hwid", machineId);
    req->setProperty ("ver", JUCE_APPLICATION_VERSION_STRING);
    req->setProperty ("build", MIKEDAW_BUILD_ID);
    req->setProperty ("nonce", nonce);
    auto line = juce::JSON::toString (juce::var (req.get()), true) + "\n";
    auto utf8 = line.toRawUTF8();
    if (sock.write (utf8, (int) strlen (utf8)) < 0)
        return false;

    juce::String resp;
    auto deadline = juce::Time::getMillisecondCounter() + (uint32_t) kSocketTimeout;
    while (! resp.containsChar ('\n') && juce::Time::getMillisecondCounter() < deadline)
    {
        if (sock.waitUntilReady (true, 500) == 1)
        {
            char buf[2048];
            int n = sock.read (buf, sizeof (buf) - 1, false);
            if (n <= 0) break;
            buf[n] = 0;
            resp += juce::String::fromUTF8 (buf, n);
        }
        if (threadShouldExit()) return false;
    }
    sock.close();

    auto msg = juce::JSON::parse (resp.upToFirstOccurrenceOf ("\n", false, false));
    auto t = msg.getProperty ("t", "").toString();

    if (t == "authno")
    {
        auto reason = msg.getProperty ("reason", "denied").toString();
        settings.removeValue ("licenseToken");
        settings.removeValue ("licenseSig");
        settings.saveIfNeeded();
        cachedExp.store (0);
        applyState (License::State::denied, reason, {});
        return true;
    }

    if (t == "authok")
    {
        juce::MemoryOutputStream tokBytes, sigBytes;
        if (! juce::Base64::convertFromBase64 (tokBytes, msg.getProperty ("token", "").toString())
            || ! juce::Base64::convertFromBase64 (sigBytes, msg.getProperty ("sig", "").toString()))
        {
            applyState (License::State::denied, "corrupt", {});
            return true;
        }
        juce::MemoryBlock tok (tokBytes.getData(), tokBytes.getDataSize());
        juce::MemoryBlock sig (sigBytes.getData(), sigBytes.getDataSize());
        juce::String name;
        if (verifyToken (tok, sig, nonce, name))
        {
            settings.setValue ("licenseToken", msg.getProperty ("token", "").toString());
            settings.setValue ("licenseSig",   msg.getProperty ("sig", "").toString());
            settings.setValue ("licensedName", name);
            settings.saveIfNeeded();
            applyState (License::State::authorized, "server", name);
            return true;
        }
        applyState (License::State::denied, "badsig", {});
        return true;
    }

    return false;
}

bool LicenseManager::verifyToken (const juce::MemoryBlock& tok,
                                  const juce::MemoryBlock& sig,
                                  const juce::String& expectNonce,
                                  juce::String& outName)
{
    if (sig.getSize() != 64 || tok.getSize() == 0)
        return false;

    if (crypto_ed25519_check ((const uint8_t*) sig.getData(),
                              kPubKey,
                              (const uint8_t*) tok.getData(),
                              tok.getSize()) != 0)
        return false;

    auto payload = juce::JSON::parse (tok.toString());
    if (! payload.isObject())
        return false;

    if ((juce::int64) payload.getProperty ("uid", 0) != localUid)
        return false;
    if (payload.getProperty ("hwid", "").toString() != machineId)
        return false;

    auto exp = (juce::int64) payload.getProperty ("exp", 0);
    if (exp <= juce::Time::currentTimeMillis())
        return false;

    if (expectNonce.isNotEmpty()
        && payload.getProperty ("nonce", "").toString() != expectNonce)
        return false;

    cachedExp.store (exp);
    outName = payload.getProperty ("name", "").toString();
    return true;
}

bool LicenseManager::loadCachedToken()
{
    auto tokB64 = settings.getValue ("licenseToken");
    auto sigB64 = settings.getValue ("licenseSig");
    if (tokB64.isEmpty() || sigB64.isEmpty())
        return false;

    juce::MemoryOutputStream tokBytes, sigBytes;
    if (! juce::Base64::convertFromBase64 (tokBytes, tokB64)
        || ! juce::Base64::convertFromBase64 (sigBytes, sigB64))
        return false;

    juce::MemoryBlock tok (tokBytes.getData(), tokBytes.getDataSize());
    juce::MemoryBlock sig (sigBytes.getData(), sigBytes.getDataSize());
    juce::String name;
    if (verifyToken (tok, sig, {}, name))
    {
        cachedName = name;
        return true;
    }
    return false;
}

juce::String LicenseManager::computeHwid()
{
    juce::String raw;

#if JUCE_WINDOWS
    {
        char guid[128] = {0};
        DWORD sz = sizeof (guid);
        if (RegGetValueA (HKEY_LOCAL_MACHINE,
                          "SOFTWARE\\Microsoft\\Cryptography", "MachineGuid",
                          RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr,
                          guid, &sz) == ERROR_SUCCESS)
            raw << "mg:" << juce::String (guid) << "|";
    }
    {
        DWORD serial = 0;
        if (GetVolumeInformationA ("C:\\", nullptr, 0, &serial, nullptr, nullptr, nullptr, 0))
            raw << "vs:" << juce::String::toHexString ((int) serial) << "|";
    }
    {
        int regs[4] = {0};
        __cpuid (regs, 0);
        char vendor[13] = {0};
        memcpy (vendor + 0, &regs[1], 4);
        memcpy (vendor + 4, &regs[3], 4);
        memcpy (vendor + 8, &regs[2], 4);
        __cpuid (regs, 1);
        raw << "cpu:" << juce::String (vendor) << ":" << juce::String::toHexString (regs[0]) << "|";
    }
#else
    raw << "nonwin:" << juce::SystemStats::getUniqueDeviceID();
#endif

    if (raw.isEmpty())
        raw = juce::SystemStats::getUniqueDeviceID();

    return juce::SHA256 (raw.toRawUTF8(), (size_t) raw.getNumBytesAsUTF8()).toHexString();
}
