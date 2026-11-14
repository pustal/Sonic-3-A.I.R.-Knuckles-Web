/**
 * @file PortUpdateCheck.cpp
 * @brief Implementation of the port's update check (see PortUpdateCheck.h).
 */
#include "PortUpdateCheck.h"

#include "HttpClient.h"
#include "Log.h"
#include "MenuOverlay.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

/** GitHub's description of this project's latest release. */
constexpr const char *kLatestReleaseApi = "https://api.github.com/repos/pustal/Sonic-3-A.I.R.-Knuckles-Web/releases/latest";
/** The releases page people open (also in the Homebrew Channel). */
constexpr const char *kReleasesPage = "github.com/pustal/Sonic-3-A.I.R.-Knuckles-Web/releases";

std::atomic<int> g_state{ S3W_UPDATE_CHECK_IDLE };
std::atomic<bool> g_hasUpdate{ false };
std::mutex g_mutex;
std::string g_latest; ///< Guarded by g_mutex.

/** The string value of "@p key" in a flat JSON object (GitHub's tag_name), or empty. */
std::string JsonString(const std::string &json, const std::string &key)
{
    const std::string needle = "\"" + key + "\"";
    std::size_t at           = json.find(needle);
    if (at == std::string::npos)
        return {};
    at = json.find(':', at + needle.size());
    if (at == std::string::npos)
        return {};
    at = json.find('"', at);
    if (at == std::string::npos)
        return {};
    const std::size_t end = json.find('"', at + 1);
    return end == std::string::npos ? std::string() : json.substr(at + 1, end - at - 1);
}

/** "v1.2.3" or "1.2.3" as numbers; missing parts are 0. */
std::vector<int> VersionParts(const std::string &version)
{
    std::vector<int> parts;
    int current  = 0;
    bool inPart  = false;
    for (const char c : version) {
        if (c >= '0' && c <= '9') {
            current = current * 10 + (c - '0');
            inPart  = true;
        }
        else if (c == '.' && inPart) {
            parts.push_back(current);
            current = 0;
            inPart  = false;
        }
        else if (inPart) {
            break; // "1.2.3-beta": the suffix is not compared
        }
    }
    if (inPart)
        parts.push_back(current);
    while (parts.size() < 3)
        parts.push_back(0);
    return parts;
}

/** The check itself, on its own thread. */
void RunCheck()
{
    std::string body, error;
    if (!s3w::HttpGet(kLatestReleaseApi, &body, &error)) {
        s3w::Log::Printf("update check failed: %s", error.c_str());
        g_state.store(S3W_UPDATE_CHECK_FAILED);
        return;
    }
    std::string tag = JsonString(body, "tag_name");
    if (tag.empty()) {
        s3w::Log::Printf("update check: no release found");
        g_state.store(S3W_UPDATE_CHECK_FAILED);
        return;
    }
    if (tag.front() == 'v' || tag.front() == 'V')
        tag.erase(tag.begin());
    const bool newer = VersionParts(tag) > VersionParts(S3W_VERSION);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_latest = tag;
    }
    g_hasUpdate.store(newer);
    s3w::Log::Printf("update check: latest release %s, this is %s", tag.c_str(), S3W_VERSION);
    g_state.store(S3W_UPDATE_CHECK_DONE);
}

} // namespace

extern "C" {

void s3w_update_check_start(void)
{
    int expected = g_state.load();
    if (expected == S3W_UPDATE_CHECK_RUNNING || !g_state.compare_exchange_strong(expected, S3W_UPDATE_CHECK_RUNNING))
        return;
    std::thread(RunCheck).detach();
}

int s3w_update_check_poll(int *hasUpdate)
{
    if (hasUpdate)
        *hasUpdate = g_hasUpdate.load() ? 1 : 0;
    return g_state.load();
}

const char *s3w_update_check_current_version(void) { return S3W_VERSION; }

const char *s3w_update_check_latest_version(void)
{
    // A copy that stays valid: the game draws it every frame.
    static thread_local std::string copy;
    std::lock_guard<std::mutex> lock(g_mutex);
    copy = g_latest;
    return copy.c_str();
}

void s3w_update_check_show_page(void)
{
    std::string latest;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        latest = g_latest;
    }
    s3w::MenuOverlay::ShowNotice("Version " + latest + " of this app is available. The Homebrew Channel installs it, "
                                 "or download it on a phone or computer: " + kReleasesPage);
}

} // extern "C"
