#include "InviteCode.h"

#if JUCE_WINDOWS
 #include <windows.h>
 #include <natupnp.h>
 #include <iphlpapi.h>
#endif

namespace
{
    constexpr auto codePrefix = "ODAW-";

    bool looksLikeIPv4 (const juce::String& s)
    {
        auto parts = juce::StringArray::fromTokens (s, ".", "");
        if (parts.size() != 4)
            return false;

        for (auto& p : parts)
        {
            if (p.isEmpty() || p.length() > 3 || p.containsOnly ("0123456789") == false)
                return false;
            auto n = p.getIntValue();
            if (n < 0 || n > 255)
                return false;
        }
        return true;
    }

    bool looksLikeHost (const juce::String& s)
    {
        if (s.isEmpty() || s.containsAnyOf (" \t\r\n/\\\"'"))
            return false;
        return true;
    }

    juce::String base64UrlEncode (const juce::String& payload)
    {
        auto s = juce::Base64::toBase64 (payload).trim();
        s = s.replaceCharacter ('+', '-').replaceCharacter ('/', '_');
        while (s.endsWithChar ('='))
            s = s.dropLastCharacters (1);
        return s;
    }

    bool base64UrlDecode (juce::String encoded, juce::String& payload)
    {
        encoded = encoded.trim().replaceCharacter ('-', '+').replaceCharacter ('_', '/');
        while ((encoded.length() % 4) != 0)
            encoded += "=";

        juce::MemoryOutputStream out;
        if (! juce::Base64::convertFromBase64 (out, encoded))
            return false;

        payload = out.getMemoryBlock().toString();
        return payload.isNotEmpty();
    }

    void addUnique (std::vector<InviteCode::Endpoint>& endpoints, const juce::String& address, int port)
    {
        auto a = address.trim();
        if (! looksLikeHost (a) || port <= 0 || port > 65535)
            return;

        for (auto& e : endpoints)
            if (e.address == a && e.port == port)
                return;

        endpoints.push_back ({ a, port });
    }

    juce::String fetchPublicIPv4()
    {
       #if OPENDAW_PUBLIC_BUILD
        return {};
       #else
        const char* services[] =
        {
            "https://api.ipify.org",
            "https://checkip.amazonaws.com",
            "https://ifconfig.me/ip"
        };

        for (auto* service : services)
        {
            int statusCode = 0;
            juce::URL url (service);
            auto options = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                               .withConnectionTimeoutMs (3500)
                               .withStatusCode (&statusCode);

            if (auto stream = url.createInputStream (options))
            {
                auto text = stream->readEntireStreamAsString().trim();
                if ((statusCode == 0 || (statusCode >= 200 && statusCode < 300)) && looksLikeIPv4 (text))
                    return text;
            }
        }

        return {};
       #endif
    }

    bool isPrivateOrCgnat (const juce::String& ip)
    {
        if (! looksLikeIPv4 (ip))
            return false;
        auto p = juce::StringArray::fromTokens (ip, ".", "");
        int a = p[0].getIntValue(), b = p[1].getIntValue();
        if (a == 10) return true;
        if (a == 192 && b == 168) return true;
        if (a == 172 && b >= 16 && b <= 31) return true;
        if (a == 100 && b >= 64 && b <= 127) return true;
        if (a == 127) return true;
        if (a == 169 && b == 254) return true;
        return false;
    }

    juce::CriticalSection igdCacheLock;
    juce::String cachedIgdControlUrl;
    juce::String cachedIgdServiceType;

    juce::String headerValue (const juce::String& response, const juce::String& name)
    {
        auto lines = juce::StringArray::fromLines (response);
        auto needle = name.toLowerCase();
        for (auto& line : lines)
        {
            auto colon = line.indexOfChar (':');
            if (colon <= 0)
                continue;
            if (line.substring (0, colon).trim().toLowerCase() == needle)
                return line.substring (colon + 1).trim();
        }
        return {};
    }

    juce::String fetchUrlText (const juce::String& url, int timeoutMs)
    {
        int statusCode = 0;
        auto options = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                           .withConnectionTimeoutMs (timeoutMs)
                           .withStatusCode (&statusCode);
        if (auto stream = juce::URL (url).createInputStream (options))
        {
            auto text = stream->readEntireStreamAsString();
            if (statusCode == 0 || (statusCode >= 200 && statusCode < 300))
                return text;
        }
        return {};
    }

    juce::String resolveUrl (const juce::String& base, juce::String rel)
    {
        rel = rel.trim();
        if (rel.startsWithIgnoreCase ("http://") || rel.startsWithIgnoreCase ("https://"))
            return rel;

        auto schemeEnd = base.indexOf ("://");
        if (schemeEnd < 0)
            return rel;
        auto afterScheme = schemeEnd + 3;
        auto pathStart = base.indexOfChar (afterScheme, '/');
        auto origin = pathStart < 0 ? base : base.substring (0, pathStart);

        if (rel.startsWithChar ('/'))
            return origin + rel;
        auto baseNoQuery = base.upToFirstOccurrenceOf ("?", false, false);
        auto dir = baseNoQuery.upToLastOccurrenceOf ("/", true, false);
        return dir + rel;
    }

    void ssdpCollect (const char* searchTarget, juce::StringArray& locations)
    {
        juce::DatagramSocket sock (true);
        if (! sock.bindToPort (0))
            return;

        juce::String req;
        req << "M-SEARCH * HTTP/1.1\r\n"
            << "HOST: 239.255.255.250:1900\r\n"
            << "MAN: \"ssdp:discover\"\r\n"
            << "MX: 2\r\n"
            << "ST: " << searchTarget << "\r\n\r\n";

        auto utf8 = req.toRawUTF8();
        auto bytes = (int) req.getNumBytesAsUTF8();
        sock.write ("239.255.255.250", 1900, utf8, bytes);

        auto deadline = juce::Time::getMillisecondCounter() + 2500;
        char buf[4096];
        while (juce::Time::getMillisecondCounter() < deadline)
        {
            auto ready = sock.waitUntilReady (true, 500);
            if (ready < 0)
                break;
            if (ready == 0)
                continue;

            juce::String senderIP; int senderPort = 0;
            auto n = sock.read (buf, (int) sizeof (buf) - 1, false, senderIP, senderPort);
            if (n <= 0)
                continue;
            buf[n] = 0;
            juce::String resp (juce::CharPointer_UTF8 (buf), (size_t) n);
            auto loc = headerValue (resp, "LOCATION");
            if (loc.isNotEmpty())
                locations.addIfNotAlreadyThere (loc);
        }
    }

    bool discoverIgd (juce::String& controlUrl, juce::String& serviceType)
    {
        {
            const juce::ScopedLock sl (igdCacheLock);
            if (cachedIgdControlUrl.isNotEmpty())
            {
                controlUrl = cachedIgdControlUrl;
                serviceType = cachedIgdServiceType;
                return true;
            }
        }

        juce::StringArray locations;
        ssdpCollect ("urn:schemas-upnp-org:service:WANIPConnection:1", locations);
        ssdpCollect ("urn:schemas-upnp-org:device:InternetGatewayDevice:1", locations);

        const char* wanServices[] =
        {
            "urn:schemas-upnp-org:service:WANIPConnection:1",
            "urn:schemas-upnp-org:service:WANPPPConnection:1"
        };

        for (auto& loc : locations)
        {
            auto xml = fetchUrlText (loc, 4000);
            if (xml.isEmpty())
                continue;

            auto parsed = juce::XmlDocument::parse (xml);
            if (parsed == nullptr)
                continue;

            for (auto* svcType : wanServices)
            {
                juce::String foundControl;
                std::function<void (juce::XmlElement*)> walk = [&] (juce::XmlElement* e)
                {
                    if (e == nullptr || foundControl.isNotEmpty())
                        return;
                    if (e->hasTagName ("service"))
                    {
                        auto* st = e->getChildByName ("serviceType");
                        if (st != nullptr && st->getAllSubText().trim() == svcType)
                        {
                            if (auto* cu = e->getChildByName ("controlURL"))
                                foundControl = cu->getAllSubText().trim();
                        }
                    }
                    for (auto* c : e->getChildIterator())
                        walk (c);
                };
                walk (parsed.get());

                if (foundControl.isNotEmpty())
                {
                    controlUrl = resolveUrl (loc, foundControl);
                    serviceType = svcType;
                    const juce::ScopedLock sl (igdCacheLock);
                    cachedIgdControlUrl = controlUrl;
                    cachedIgdServiceType = serviceType;
                    return true;
                }
            }
        }
        return false;
    }

    juce::String soapCall (const juce::String& controlUrl, const juce::String& serviceType,
                           const juce::String& action, const juce::String& bodyInner, bool& httpOk)
    {
        httpOk = false;

        juce::String body;
        body << "<?xml version=\"1.0\"?>"
             << "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
             << "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
             << "<s:Body>"
             << "<u:" << action << " xmlns:u=\"" << serviceType << "\">"
             << bodyInner
             << "</u:" << action << ">"
             << "</s:Body></s:Envelope>";

        juce::String extraHeaders;
        extraHeaders << "Content-Type: text/xml; charset=\"utf-8\"\r\n"
                     << "SOAPAction: \"" << serviceType << "#" << action << "\"";

        int statusCode = 0;
        auto url = juce::URL (controlUrl).withPOSTData (body);
        auto options = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inPostData)
                           .withConnectionTimeoutMs (4000)
                           .withExtraHeaders (extraHeaders)
                           .withStatusCode (&statusCode);

        if (auto stream = url.createInputStream (options))
        {
            auto resp = stream->readEntireStreamAsString();
            httpOk = (statusCode >= 200 && statusCode < 300);
            return resp;
        }
        return {};
    }

    bool soapAddPortMapping (int port, const juce::String& lanAddress, juce::String& externalIp)
    {
        juce::String controlUrl, serviceType;
        if (! discoverIgd (controlUrl, serviceType))
            return false;

        {
            bool ok = false;
            auto resp = soapCall (controlUrl, serviceType, "GetExternalIPAddress", {}, ok);
            if (ok)
            {
                if (auto xml = juce::XmlDocument::parse (resp))
                {
                    std::function<void (juce::XmlElement*)> find = [&] (juce::XmlElement* e)
                    {
                        if (e == nullptr || externalIp.isNotEmpty())
                            return;
                        if (e->getTagName().endsWith ("NewExternalIPAddress"))
                            externalIp = e->getAllSubText().trim();
                        for (auto* c : e->getChildIterator())
                            find (c);
                    };
                    find (xml.get());
                }
            }
        }

        juce::String inner;
        inner << "<NewRemoteHost></NewRemoteHost>"
              << "<NewExternalPort>" << port << "</NewExternalPort>"
              << "<NewProtocol>TCP</NewProtocol>"
              << "<NewInternalPort>" << port << "</NewInternalPort>"
              << "<NewInternalClient>" << lanAddress << "</NewInternalClient>"
              << "<NewEnabled>1</NewEnabled>"
              << "<NewPortMappingDescription>mikeDAW Multiplayer</NewPortMappingDescription>"
              << "<NewLeaseDuration>0</NewLeaseDuration>";

        bool httpOk = false;
        auto resp = soapCall (controlUrl, serviceType, "AddPortMapping", inner, httpOk);
        if (httpOk)
            return true;

        if (resp.contains ("718") || resp.contains ("ConflictInMapping"))
            return true;

        return false;
    }

    bool soapDeletePortMapping (int port)
    {
        juce::String controlUrl, serviceType;
        if (! discoverIgd (controlUrl, serviceType))
            return false;

        juce::String inner;
        inner << "<NewRemoteHost></NewRemoteHost>"
              << "<NewExternalPort>" << port << "</NewExternalPort>"
              << "<NewProtocol>TCP</NewProtocol>";

        bool httpOk = false;
        soapCall (controlUrl, serviceType, "DeletePortMapping", inner, httpOk);
        return httpOk;
    }

#if JUCE_WINDOWS
    juce::String defaultGatewayIPv4()
    {
        juce::String result;
        ULONG size = 0;
        if (GetIpForwardTable (nullptr, &size, FALSE) != ERROR_INSUFFICIENT_BUFFER)
            return result;

        juce::HeapBlock<char> block (size);
        auto* table = reinterpret_cast<PMIB_IPFORWARDTABLE> (block.getData());
        if (GetIpForwardTable (table, &size, FALSE) != NO_ERROR)
            return result;

        for (DWORD i = 0; i < table->dwNumEntries; ++i)
        {
            if (table->table[i].dwForwardDest == 0 && table->table[i].dwForwardMask == 0)
            {
                auto gw = table->table[i].dwForwardNextHop;
                if (gw != 0)
                {
                    result << (int) (gw & 0xff) << "." << (int) ((gw >> 8) & 0xff)
                           << "." << (int) ((gw >> 16) & 0xff) << "." << (int) ((gw >> 24) & 0xff);
                    return result;
                }
            }
        }
        return result;
    }

    bool natPmpMap (int port, int lifetimeSecs)
    {
        auto gateway = defaultGatewayIPv4();
        if (gateway.isEmpty())
            return false;

        juce::DatagramSocket sock;
        if (! sock.bindToPort (0))
            return false;

        juce::uint8 req[12] = {};
        req[0] = 0;
        req[1] = 2;
        req[2] = 0; req[3] = 0;
        req[4] = (juce::uint8) ((port >> 8) & 0xff);
        req[5] = (juce::uint8) (port & 0xff);
        req[6] = (juce::uint8) ((port >> 8) & 0xff);
        req[7] = (juce::uint8) (port & 0xff);
        req[8]  = (juce::uint8) ((lifetimeSecs >> 24) & 0xff);
        req[9]  = (juce::uint8) ((lifetimeSecs >> 16) & 0xff);
        req[10] = (juce::uint8) ((lifetimeSecs >> 8) & 0xff);
        req[11] = (juce::uint8) (lifetimeSecs & 0xff);

        if (sock.write (gateway, 5351, req, sizeof (req)) != (int) sizeof (req))
            return false;

        if (sock.waitUntilReady (true, 2500) <= 0)
            return false;

        juce::uint8 resp[16] = {};
        juce::String from; int fromPort = 0;
        auto n = sock.read (resp, (int) sizeof (resp), false, from, fromPort);
        if (n < 8)
            return false;

        juce::uint16 resultCode = (juce::uint16) ((resp[2] << 8) | resp[3]);
        return resp[1] == 130 && resultCode == 0;
    }
#endif

#if JUCE_WINDOWS
    struct ComInit
    {
        ComInit()
        {
            hr = CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED);
            ok = SUCCEEDED (hr) || hr == RPC_E_CHANGED_MODE;
        }

        ~ComInit()
        {
            if (SUCCEEDED (hr))
                CoUninitialize();
        }

        HRESULT hr = E_FAIL;
        bool ok = false;
    };

    struct BStr
    {
        explicit BStr (const juce::String& s) : value (SysAllocString (s.toWideCharPointer())) {}
        ~BStr() { SysFreeString (value); }
        operator BSTR() const { return value; }
        BSTR value = nullptr;
    };

    template <typename T>
    struct ComPtr
    {
        ~ComPtr() { if (ptr != nullptr) ptr->Release(); }
        T** operator&() { return &ptr; }
        T* operator->() const { return ptr; }
        operator bool() const { return ptr != nullptr; }
        T* ptr = nullptr;
    };

    bool addUPnPMapping (int port, const juce::String& lanAddress, juce::String& status, bool& mappingCreated)
    {
        ComInit com;
        if (! com.ok)
        {
            status = "UPnP unavailable: COM init failed";
            return false;
        }

        ComPtr<IUPnPNAT> nat;
        auto hr = CoCreateInstance (CLSID_UPnPNAT, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_IUPnPNAT, reinterpret_cast<void**> (&nat.ptr));
        if (FAILED (hr) || ! nat)
        {
            status = "UPnP unavailable on this Windows install/router";
            return false;
        }

        ComPtr<IStaticPortMappingCollection> mappings;
        hr = nat->get_StaticPortMappingCollection (&mappings.ptr);
        if (FAILED (hr) || ! mappings)
        {
            status = "UPnP is disabled or unsupported by the router";
            return false;
        }

        BStr proto ("TCP"), client (lanAddress), desc ("mikeDAW Multiplayer");
        ComPtr<IStaticPortMapping> mapping;
        hr = mappings->Add (port, proto, port, client, VARIANT_TRUE, desc, &mapping.ptr);
        if (SUCCEEDED (hr))
        {
            mappingCreated = true;
            status = "UPnP opened TCP port " + juce::String (port);
            return true;
        }

        ComPtr<IStaticPortMapping> existing;
        if (SUCCEEDED (mappings->get_Item (port, proto, &existing.ptr)) && existing)
        {
            status = "UPnP TCP port " + juce::String (port) + " already has a router mapping";
            return true;
        }

        status = "UPnP could not open TCP port " + juce::String (port)
               + " (router may need manual port forwarding)";
        return false;
    }
#endif
}

namespace InviteCode
{
    juce::String makeCode (const juce::String& publicAddress, const juce::String& lanAddress, int port)
    {
        juce::String payload;
        payload << "1|" << port << "|" << publicAddress.trim() << "|" << lanAddress.trim();
        return codePrefix + base64UrlEncode (payload);
    }

    HostInvite prepareHostInvite (int port, const juce::String& lanAddress)
    {
        HostInvite invite;
        invite.port = port;
        invite.lanAddress = lanAddress.trim();

        juce::String routerExternalIp;
        juce::String mapMethod;

        juce::String comStatus;
#if JUCE_WINDOWS
        bool comMappingCreated = false;
        if (addUPnPMapping (port, invite.lanAddress, comStatus, comMappingCreated))
        {
            invite.upnpPortReady = true;
            invite.upnpMapped = comMappingCreated;
            mapMethod = "UPnP (Windows)";
        }
#else
        comStatus = "UPnP auto-mapping is only implemented on Windows in this build";
#endif

        if (! invite.upnpPortReady)
        {
            if (soapAddPortMapping (port, invite.lanAddress, routerExternalIp))
            {
                invite.upnpPortReady = true;
                invite.upnpMapped = true;
                mapMethod = "UPnP (IGD/SOAP)";
            }
        }
        else
        {
            juce::String controlUrl, serviceType;
            if (discoverIgd (controlUrl, serviceType))
            {
                bool ok = false;
                auto resp = soapCall (controlUrl, serviceType, "GetExternalIPAddress", {}, ok);
                if (ok)
                {
                    auto v = resp.fromFirstOccurrenceOf ("NewExternalIPAddress>", false, true);
                    routerExternalIp = v.upToFirstOccurrenceOf ("<", false, false).trim();
                    if (! looksLikeIPv4 (routerExternalIp))
                        routerExternalIp.clear();
                }
            }
        }

#if JUCE_WINDOWS
        if (! invite.upnpPortReady)
        {
            if (natPmpMap (port, 86400))
            {
                invite.upnpPortReady = true;
                invite.upnpMapped = true;
                mapMethod = "NAT-PMP";
            }
        }
#endif

       #if OPENDAW_PUBLIC_BUILD
        if (routerExternalIp.isNotEmpty() && ! isPrivateOrCgnat (routerExternalIp))
            invite.publicAddress = routerExternalIp;
       #else
        invite.publicAddress = fetchPublicIPv4();
       #endif
        invite.code = makeCode (invite.publicAddress, invite.lanAddress, port);

        juce::String status;
        if (invite.publicAddress.isNotEmpty())
            status << "Internet address: " << invite.publicAddress << ":" << juce::String (port) << "\n";
        else
            status << "Could not auto-detect the public internet address. The code still includes your LAN address.\n";

        bool cgnat = false;
        if (routerExternalIp.isNotEmpty())
        {
            if (isPrivateOrCgnat (routerExternalIp))
                cgnat = true;
            else if (invite.publicAddress.isNotEmpty() && routerExternalIp != invite.publicAddress)
                cgnat = true;
        }

        if (cgnat)
        {
            invite.upnpPortReady = false;
            status << "Your ISP appears to use carrier-grade NAT - internet peers can't connect "
                      "directly. Ask your friend to host instead, or use a free VPN like "
                      "Tailscale / Radmin VPN and share your VPN IP.";
        }
        else if (invite.upnpPortReady)
        {
            status << "Router port opened automatically via " << mapMethod
                   << " (TCP " << juce::String (port) << ").";
        }
        else
        {
            if (comStatus.isNotEmpty())
                status << comStatus << "\n";
            status << "Could not open the port automatically. To let internet peers connect, "
                      "forward TCP port " << juce::String (port) << " to this PC ("
                   << invite.lanAddress << ") in your router settings. LAN discovery / manual IP "
                      "still works without this.";
        }

        invite.status = status;
        return invite;
    }

    std::vector<Endpoint> parseJoinText (const juce::String& text, int defaultPort)
    {
        std::vector<Endpoint> endpoints;
        auto t = text.trim();
        if (t.isEmpty())
            return endpoints;

        auto compact = t.removeCharacters (" \t\r\n");
        if (compact.startsWithIgnoreCase (codePrefix))
        {
            juce::String payload;
            if (base64UrlDecode (compact.substring ((int) std::strlen (codePrefix)), payload))
            {
                auto parts = juce::StringArray::fromTokens (payload, "|", "");
                if (parts.size() >= 4 && parts[0] == "1")
                {
                    auto port = parts[1].getIntValue();
                    addUnique (endpoints, parts[2], port);
                    addUnique (endpoints, parts[3], port);
                    return endpoints;
                }
            }
        }

        juce::String address = t;
        int port = defaultPort;

        if (address.containsChar (':') && ! address.startsWithChar ('['))
        {
            port = address.fromLastOccurrenceOf (":", false, false).getIntValue();
            address = address.upToLastOccurrenceOf (":", false, false);
        }
        else if (address.startsWithChar ('[') && address.contains ("]:"))
        {
            auto after = address.fromFirstOccurrenceOf ("]:", false, false);
            port = after.getIntValue();
            address = address.substring (1, address.indexOfChar (']'));
        }

        if (port <= 0)
            port = defaultPort;

        addUnique (endpoints, address, port);
        return endpoints;
    }

    bool removeUPnPPortMapping (int port, juce::String& status)
    {
#if JUCE_WINDOWS
        {
            ComInit com;
            if (com.ok)
            {
                ComPtr<IUPnPNAT> nat;
                auto hr = CoCreateInstance (CLSID_UPnPNAT, nullptr, CLSCTX_INPROC_SERVER,
                                            IID_IUPnPNAT, reinterpret_cast<void**> (&nat.ptr));
                if (SUCCEEDED (hr) && nat)
                {
                    ComPtr<IStaticPortMappingCollection> mappings;
                    hr = nat->get_StaticPortMappingCollection (&mappings.ptr);
                    if (SUCCEEDED (hr) && mappings)
                    {
                        BStr proto ("TCP");
                        if (SUCCEEDED (mappings->Remove (port, proto)))
                        {
                            status = "UPnP closed TCP port " + juce::String (port);
                            return true;
                        }
                    }
                }
            }
        }
#endif
        if (soapDeletePortMapping (port))
        {
            status = "Closed TCP port " + juce::String (port) + " via IGD";
            return true;
        }

#if JUCE_WINDOWS
        if (natPmpMap (port, 0))
        {
            status = "Removed NAT-PMP mapping for TCP port " + juce::String (port);
            return true;
        }
#endif
        status = "";
        return false;
    }

    bool probeEndpoint (const juce::String& address, int port, int timeoutMs)
    {
        if (address.trim().isEmpty() || port <= 0 || port > 65535)
            return false;

        juce::StreamingSocket sock;
        bool ok = sock.connect (address.trim(), port, timeoutMs);
        sock.close();
        return ok;
    }
}

