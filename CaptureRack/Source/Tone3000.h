#pragma once
// Minimal TONE3000 API v1 client: device sign-in, tone lists, model lists, downloads.
// All network calls run on a background thread; callbacks arrive on the message thread.
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_data_structures/juce_data_structures.h>

namespace rack
{
    struct T3kTone
    {
        juce::String id, title, author, gear, format, license, url;
        int a1 = 0, a2 = 0, irs = 0, favourites = 0, downloads = 0;
    };
    struct T3kModel
    {
        juce::String id, name, url, size, arch;
    };

    class Tone3000
    {
    public:
        static inline const juce::String base = "https://www.tone3000.com/api/v1";

        Tone3000()
        {
            juce::PropertiesFile::Options o;
            o.applicationName = "Capture Rack";
            o.folderName = "CaptureRack";
            o.filenameSuffix = ".settings";
            o.osxLibrarySubFolder = "Application Support";
            settings = std::make_unique<juce::PropertiesFile> (o);
        }
        ~Tone3000() { *alive = false; pool.removeAllJobs (true, 5000); }

        juce::String clientId() const { return settings->getValue ("t3k_client_id"); }
        void setClientId (const juce::String& id) { settings->setValue ("t3k_client_id", id.trim()); settings->saveIfNeeded(); }
        bool signedIn() const { return settings->getValue ("t3k_access").isNotEmpty(); }
        juce::String userName() const { return settings->getValue ("t3k_user"); }
        void signOut()
        {
            for (auto k : { "t3k_access", "t3k_refresh", "t3k_user" }) settings->removeValue (k);
            settings->saveIfNeeded();
        }

        // ---- device sign-in (RFC 8628) ----
        // onCode(userCode, verifyUrl) fires once; onDone(ok, message) when finished.
        void signIn (std::function<void (juce::String, juce::String)> onCode, std::function<void (bool, juce::String)> onDone)
        {
            const auto cid = clientId();
            if (cid.isEmpty()) { onDone (false, "Enter your TONE3000 publishable key (client_id) first."); return; }
            cancelSignIn = false;
            run ([this, cid, onCode, onDone] (std::shared_ptr<bool> ok)
            {
                int status = 0; juce::String err;
                auto r = post ("/oauth/device_authorization", { { "client_id", cid }, { "scope", "read" } }, {}, status, err);
                auto* o = r.getDynamicObject();
                if (o == nullptr || ! o->hasProperty ("device_code"))
                {
                    post2 (ok, [onDone, err, r] { onDone (false, "Sign-in failed: " + (err.isNotEmpty() ? err : juce::JSON::toString (r, true))); });
                    return;
                }
                const auto device = o->getProperty ("device_code").toString();
                const auto code = o->getProperty ("user_code").toString();
                auto verify = o->getProperty ("verification_uri_complete").toString();
                if (verify.isEmpty()) verify = o->getProperty ("verification_uri").toString();
                int interval = juce::jmax (2, (int) o->getProperty ("interval"));
                const int expires = juce::jmax (60, (int) o->getProperty ("expires_in"));
                post2 (ok, [onCode, code, verify] { onCode (code, verify); });

                const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) expires * 1000;
                while (*ok && ! cancelSignIn && juce::Time::getMillisecondCounter() < deadline)
                {
                    for (int w = 0; w < interval * 10 && *ok && ! cancelSignIn; ++w) juce::Thread::sleep (100);
                    if (! *ok || cancelSignIn) break;
                    auto t = post ("/oauth/token", { { "grant_type", "urn:ietf:params:oauth:grant-type:device_code" },
                                                     { "device_code", device }, { "client_id", cid } }, {}, status, err);
                    auto* to = t.getDynamicObject();
                    if (to == nullptr) continue;
                    if (to->hasProperty ("access_token"))
                    {
                        storeTokens (*to);
                        int s2 = 0; juce::String e2;
                        auto user = get ("/user", s2, e2);
                        if (auto* u = user.getDynamicObject())
                        {
                            auto name = u->getProperty ("display_name").toString();
                            if (name.isEmpty()) name = u->getProperty ("username").toString();
                            settings->setValue ("t3k_user", name);
                            settings->saveIfNeeded();
                        }
                        post2 (ok, [onDone] { onDone (true, "Signed in."); });
                        return;
                    }
                    const auto e = to->getProperty ("error").toString();
                    if (e == "slow_down") interval += 5;
                    else if (e != "authorization_pending")
                    {
                        post2 (ok, [onDone, e] { onDone (false, "Sign-in stopped: " + e); });
                        return;
                    }
                }
                post2 (ok, [onDone] { onDone (false, "Sign-in timed out or was cancelled."); });
            });
        }
        void cancel() { cancelSignIn = true; }

        // ---- tones ----
        // list: favorited, created, downloaded, trending, latest, search
        void tones (const juce::String& list, const juce::String& query, int page,
                    std::function<void (juce::Array<T3kTone>, int totalPages, juce::String error)> done)
        {
            run ([this, list, query, page, done] (std::shared_ptr<bool> ok)
            {
                juce::String path = "/tones/" + list;
                if (list == "favorited" || list == "created" || list == "downloaded" || list == "search")
                    path << "?page=" << page << "&page_size=" << (list == "search" ? 25 : 50);
                if (list == "search") path << "&query=" << juce::URL::addEscapeChars (query, true);
                int status = 0; juce::String err;
                auto r = get (path, status, err);
                juce::Array<T3kTone> out;
                int pages = 1;
                if (auto* o = r.getDynamicObject())
                {
                    pages = juce::jmax (1, (int) o->getProperty ("total_pages"));
                    if (auto* arr = o->getProperty ("data").getArray())
                        for (auto& t : *arr) out.add (parseTone (t));
                }
                if (status >= 400) err = "TONE3000 said " + juce::String (status) + (status == 403 ? " (this list may need partner access)" : "");
                post2 (ok, [done, out, pages, err] { done (out, pages, err); });
            });
        }

        // Models for a tone, both A1/custom and A2.
        void models (const juce::String& toneId, std::function<void (juce::Array<T3kModel>, juce::String)> done)
        {
            run ([this, toneId, done] (std::shared_ptr<bool> ok)
            {
                juce::Array<T3kModel> out; juce::String err;
                for (auto arch : { juce::String(), juce::String ("2") })
                {
                    int status = 0;
                    auto r = get ("/models?tone_id=" + toneId + "&page_size=100" + (arch.isNotEmpty() ? "&architecture=" + arch : juce::String()), status, err);
                    if (auto* arr = r.getProperty ("data", {}).getArray())
                        for (auto& m : *arr)
                        {
                            T3kModel tm;
                            tm.id = m.getProperty ("id", {}).toString();
                            tm.name = m.getProperty ("name", {}).toString();
                            tm.url = m.getProperty ("model_url", {}).toString();
                            tm.size = m.getProperty ("size", {}).toString();
                            const auto a = m.getProperty ("architecture_version", {}).toString();
                            tm.arch = a == "2" ? "A2" : a == "1" ? "A1" : a.isNotEmpty() ? a : "IR";
                            bool dup = false;
                            for (auto& e : out) dup = dup || e.id == tm.id;
                            if (! dup) out.add (tm);
                        }
                }
                post2 (ok, [done, out, err] { done (out, err); });
            });
        }

        // Downloads one model file to dest. Calls done(ok, message).
        void download (const T3kModel& m, const juce::File& dest, std::function<void (bool, juce::String)> done)
        {
            run ([this, m, dest, done] (std::shared_ptr<bool> ok)
            {
                int status = 0;
                auto stream = open (juce::URL (m.url), false, status);
                if (stream == nullptr || status == 401)
                {
                    if (refresh()) stream = open (juce::URL (m.url), false, status);
                }
                bool good = false;
                if (stream != nullptr && status < 400)
                {
                    dest.getParentDirectory().createDirectory();
                    juce::FileOutputStream fos (dest);
                    if (fos.openedOk())
                    {
                        fos.setPosition (0); fos.truncate();
                        good = fos.writeFromInputStream (*stream, -1) > 0;
                    }
                }
                if (! good) dest.deleteFile();
                post2 (ok, [done, good, status, dest] { done (good, good ? dest.getFileName() : "Download failed (" + juce::String (status) + ")"); });
            });
        }

    private:
        using Params = std::initializer_list<std::pair<juce::String, juce::String>>;

        void run (std::function<void (std::shared_ptr<bool>)> job)
        {
            auto ok = alive;
            pool.addJob ([job, ok] { job (ok); });
        }
        static void post2 (std::shared_ptr<bool> ok, std::function<void()> fn)
        {
            juce::MessageManager::callAsync ([ok, fn] { if (*ok) fn(); });
        }

        static T3kTone parseTone (const juce::var& t)
        {
            T3kTone x;
            x.id = t.getProperty ("id", {}).toString();
            x.title = t.getProperty ("title", {}).toString();
            const auto u = t.getProperty ("user", {});
            x.author = u.getProperty ("display_name", {}).toString();
            if (x.author.isEmpty()) x.author = u.getProperty ("username", {}).toString();
            x.gear = t.getProperty ("gear", {}).toString();
            x.format = t.getProperty ("format", {}).toString();
            x.license = t.getProperty ("license", {}).toString();
            x.url = t.getProperty ("url", {}).toString();
            x.a1 = t.getProperty ("a1_models_count", {});
            x.a2 = t.getProperty ("a2_models_count", {});
            x.irs = t.getProperty ("irs_count", {});
            x.favourites = t.getProperty ("favorites_count", {});
            x.downloads = t.getProperty ("downloads_count", {});
            return x;
        }

        std::unique_ptr<juce::InputStream> open (const juce::URL& url, bool postData, int& status, bool auth = true)
        {
            juce::String headers;
            const auto token = settings->getValue ("t3k_access");
            if (auth && token.isNotEmpty()) headers << "Authorization: Bearer " << token << "\r\n";
            if (postData) headers << "Content-Type: application/x-www-form-urlencoded\r\n";
            auto opts = juce::URL::InputStreamOptions (postData ? juce::URL::ParameterHandling::inPostData : juce::URL::ParameterHandling::inAddress)
                            .withExtraHeaders (headers)
                            .withConnectionTimeoutMs (20000)
                            .withStatusCode (&status)
                            .withNumRedirectsToFollow (5);
            return url.createInputStream (opts);
        }

        juce::var post (const juce::String& path, Params params, const juce::String&, int& status, juce::String& err)
        {
            juce::URL url (base + path);
            for (auto& p : params) url = url.withParameter (p.first, p.second);
            auto s = open (url, true, status, false);
            if (s == nullptr) { err = "No connection to TONE3000"; return {}; }
            return juce::JSON::parse (s->readEntireStreamAsString());
        }

        juce::var get (const juce::String& path, int& status, juce::String& err)
        {
            auto s = open (juce::URL (base + path), false, status);
            if (s != nullptr && status == 401 && refresh()) s = open (juce::URL (base + path), false, status);
            if (s == nullptr) { err = "No connection to TONE3000"; return {}; }
            return juce::JSON::parse (s->readEntireStreamAsString());
        }

        bool refresh()
        {
            const auto rt = settings->getValue ("t3k_refresh");
            if (rt.isEmpty()) return false;
            int status = 0; juce::String err;
            auto r = post ("/oauth/token", { { "grant_type", "refresh_token" }, { "refresh_token", rt }, { "client_id", clientId() } }, {}, status, err);
            if (auto* o = r.getDynamicObject(); o != nullptr && o->hasProperty ("access_token")) { storeTokens (*o); return true; }
            return false;
        }

        void storeTokens (juce::DynamicObject& o)
        {
            settings->setValue ("t3k_access", o.getProperty ("access_token").toString());
            if (o.hasProperty ("refresh_token")) settings->setValue ("t3k_refresh", o.getProperty ("refresh_token").toString());
            settings->saveIfNeeded();
        }

        std::unique_ptr<juce::PropertiesFile> settings;
        juce::ThreadPool pool { 2 };
        std::shared_ptr<bool> alive = std::make_shared<bool> (true);
        std::atomic<bool> cancelSignIn { false };
    };
}
