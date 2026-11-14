/**
 * @file CurlShim.cpp
 * @brief The four libcurl functions the game uses, loaded from the TV's own
 *        libcurl when the game first downloads something.
 *
 * A.I.R. downloads with libcurl on Linux (oxygen/download/Downloader.cpp:
 * the remastered soundtrack from the options menu, update checks). Linking
 * libcurl directly would make the app refuse to start on a firmware without
 * it, or with another soname, so the game is linked against these functions
 * instead. They open libcurl.so.4 at the first call; if it is not there,
 * curl_easy_init() returns null and the game reports the download as failed,
 * which it already handles.
 *
 * The port's own HttpGet() (HttpClient.h) uses the same library.
 */
// curl.h otherwise turns curl_easy_setopt into a type-checking macro.
#define CURL_DISABLE_TYPECHECK
#include <curl/curl.h>

#include "HttpClient.h"
#include "Log.h"

#include <dlfcn.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <initializer_list>
#include <mutex>
#include <string>

namespace
{

using EasyInitFn    = CURL *(*)();
using EasySetoptFn  = CURLcode (*)(CURL *, CURLoption, ...);
using EasyPerformFn = CURLcode (*)(CURL *);
using EasyCleanupFn = void (*)(CURL *);
using EasyGetinfoFn = CURLcode (*)(CURL *, CURLINFO, ...);

/** The real library's entry points, or null when it could not be loaded. */
struct RealCurl {
    EasyInitFn init       = nullptr;
    EasySetoptFn setopt   = nullptr;
    EasyPerformFn perform = nullptr;
    EasyCleanupFn cleanup = nullptr;
    EasyGetinfoFn getinfo = nullptr; ///< Only HttpGet() uses it.

    bool Loaded() const { return init && setopt && perform && cleanup; }
};

/** Loads libcurl once (thread-safe: downloads run on their own thread). */
const RealCurl &Real()
{
    static RealCurl real;
    static std::once_flag once;
    std::call_once(once, [] {
        void *library = nullptr;
        for (const char *name : { "libcurl.so.4", "libcurl.so" }) {
            library = dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (library)
                break;
        }
        if (!library) {
            s3w::Log::Printf("libcurl not found: downloads are unavailable");
            return;
        }
        real.init    = reinterpret_cast<EasyInitFn>(dlsym(library, "curl_easy_init"));
        real.setopt  = reinterpret_cast<EasySetoptFn>(dlsym(library, "curl_easy_setopt"));
        real.perform = reinterpret_cast<EasyPerformFn>(dlsym(library, "curl_easy_perform"));
        real.cleanup = reinterpret_cast<EasyCleanupFn>(dlsym(library, "curl_easy_cleanup"));
        real.getinfo = reinterpret_cast<EasyGetinfoFn>(dlsym(library, "curl_easy_getinfo"));
        if (!real.Loaded())
            s3w::Log::Printf("libcurl is incomplete: downloads are unavailable");
    });
    return real;
}

/** Most bytes HttpGet() keeps; anything larger is not what the port asked for. */
constexpr std::size_t kMaxBodyBytes = 1024 * 1024;

/** libcurl write callback: appends to the std::string in @p userData, up to kMaxBodyBytes. */
std::size_t AppendToString(char *data, std::size_t size, std::size_t count, void *userData)
{
    std::string &body        = *static_cast<std::string *>(userData);
    const std::size_t length = size * count;
    if (body.size() + length > kMaxBodyBytes)
        return 0; // makes curl_easy_perform fail
    body.append(data, length);
    return length;
}

/** The CA bundle on the TV, for HTTPS (libcurl's built-in default may not exist there). */
const char *FindCaBundle()
{
    for (const char *path : { "/etc/ssl/certs/ca-certificates.crt", "/etc/ssl/certs/ca-bundle.crt", "/etc/pki/tls/certs/ca-bundle.crt" }) {
        if (access(path, R_OK) == 0)
            return path;
    }
    return nullptr;
}

} // namespace

namespace s3w
{

bool HttpGet(const std::string &url, std::string *body, std::string *error)
{
    const RealCurl &real = Real();
    if (!real.Loaded()) {
        *error = "this TV has no usable libcurl";
        return false;
    }
    CURL *curl = real.init();
    if (!curl) {
        *error = "libcurl could not start";
        return false;
    }
    body->clear();
    real.setopt(curl, CURLOPT_URL, url.c_str());
    real.setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    real.setopt(curl, CURLOPT_TIMEOUT, 20L);
    real.setopt(curl, CURLOPT_NOSIGNAL, 1L);
    // GitHub's API refuses requests without a User-Agent.
    real.setopt(curl, CURLOPT_USERAGENT, "Sonic3AIR-webOS/" S3W_VERSION);
    real.setopt(curl, CURLOPT_WRITEFUNCTION, &AppendToString);
    real.setopt(curl, CURLOPT_WRITEDATA, static_cast<void *>(body));
    if (const char *bundle = FindCaBundle())
        real.setopt(curl, CURLOPT_CAINFO, bundle);

    const CURLcode result = real.perform(curl);
    long status           = 0;
    if (real.getinfo)
        real.getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    real.cleanup(curl);

    if (result != CURLE_OK) {
        *error = "network error (curl " + std::to_string(static_cast<int>(result)) + ")";
        return false;
    }
    if (status != 200) {
        *error = "the server answered " + std::to_string(status);
        return false;
    }
    return true;
}

} // namespace s3w

extern "C" {

CURL *curl_easy_init(void)
{
    const RealCurl &real = Real();
    return real.Loaded() ? real.init() : nullptr;
}

/**
 * The game only passes options whose argument is a pointer (CURLOPT_URL, the
 * write callback and its data), so the variadic argument is forwarded as one.
 */
CURLcode curl_easy_setopt(CURL *curl, CURLoption option, ...)
{
    const RealCurl &real = Real();
    if (!real.Loaded() || !curl)
        return CURLE_FAILED_INIT;
    va_list arguments;
    va_start(arguments, option);
    void *parameter = va_arg(arguments, void *);
    va_end(arguments);
    return real.setopt(curl, option, parameter);
}

CURLcode curl_easy_perform(CURL *curl)
{
    const RealCurl &real = Real();
    if (!real.Loaded() || !curl)
        return CURLE_FAILED_INIT;
    return real.perform(curl);
}

void curl_easy_cleanup(CURL *curl)
{
    const RealCurl &real = Real();
    if (real.Loaded() && curl)
        real.cleanup(curl);
}

} // extern "C"
