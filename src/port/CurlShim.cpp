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
 */
// curl.h otherwise turns curl_easy_setopt into a type-checking macro.
#define CURL_DISABLE_TYPECHECK
#include <curl/curl.h>

#include <dlfcn.h>

#include <cstdarg>
#include <cstdio>
#include <initializer_list>
#include <mutex>

namespace
{

using EasyInitFn    = CURL *(*)();
using EasySetoptFn  = CURLcode (*)(CURL *, CURLoption, ...);
using EasyPerformFn = CURLcode (*)(CURL *);
using EasyCleanupFn = void (*)(CURL *);

/** The real library's entry points, or null when it could not be loaded. */
struct RealCurl {
    EasyInitFn init       = nullptr;
    EasySetoptFn setopt   = nullptr;
    EasyPerformFn perform = nullptr;
    EasyCleanupFn cleanup = nullptr;

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
            std::printf("[S3AIR/webOS] libcurl not found: downloads are unavailable\n");
            return;
        }
        real.init    = reinterpret_cast<EasyInitFn>(dlsym(library, "curl_easy_init"));
        real.setopt  = reinterpret_cast<EasySetoptFn>(dlsym(library, "curl_easy_setopt"));
        real.perform = reinterpret_cast<EasyPerformFn>(dlsym(library, "curl_easy_perform"));
        real.cleanup = reinterpret_cast<EasyCleanupFn>(dlsym(library, "curl_easy_cleanup"));
        if (!real.Loaded())
            std::printf("[S3AIR/webOS] libcurl is incomplete: downloads are unavailable\n");
    });
    return real;
}

} // namespace

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
