#pragma once

#include <wx/string.h>
#include <functional>
#include <mutex>
#include <string>

// Minimal HTTPS GET over WinHTTP — the only network operation PromoAccess needs.
//
// Trimmed from the ReaperAccessible Installer Manager's http:: with one
// correction that matters here: WinHttpCrackUrl returns the query string in
// lpszExtraInfo, NOT in lpszUrlPath. The Manager never noticed (its catalog URL
// has no query); every PromoAccess request is query-driven, so the path handed
// to WinHttpOpenRequest must be path + extra info or the server sees a bare
// endpoint with no postal code and no search terms.
namespace http
{
    // Aborts a request that is already in flight, from another thread.
    //
    // A synchronous WinHTTP call cannot be polled and cannot be interrupted by a
    // flag: the thread is inside WinHttpReceiveResponse and stays there until
    // the server answers or the timeout expires. The documented way out is to
    // close the request handle from elsewhere, which makes the pending call fail
    // at once. That is what this does.
    //
    // Why it matters here: closing the window joins three background workers. A
    // worker waiting out a stalled connection kept the message pump stopped for
    // as long as the timeout — the window frozen, nothing for a screen reader to
    // read, and no way to tell whether the application was closing or dead.
    //
    // Ownership of the attached handle passes to the Canceller for the duration:
    // whichever of abort() and release() runs first closes it, under the mutex,
    // and clears it, so it is closed exactly once.
    class Canceller
    {
    public:
        Canceller() = default;
        ~Canceller();

        Canceller(const Canceller&) = delete;
        Canceller& operator=(const Canceller&) = delete;

        // Called from any thread. Every later request is refused as well, until
        // reset() — a worker that is cancelled must not start its next call.
        void abort();

        // Clears the aborted state before a new run. Never call it while a
        // request is in flight.
        void reset();

        bool aborted() const;

        // --- used by getToString only -----------------------------------------
        // Takes ownership of `handle`. Returns false when abort() has already
        // been called, in which case the caller must close the handle itself.
        bool adopt(void* handle);
        // Closes the adopted handle if abort() has not already done so.
        void release();

    private:
        mutable std::mutex mutex_;
        void*              handle_ = nullptr;
        bool               aborted_ = false;
    };

    struct GetResult
    {
        bool     ok = false;
        long     statusCode = 0;
        wxString error;      // human-readable; localized at the call site if shown
    };

    // Blocking GET of a JSON resource into `out` (raw bytes, UTF-8 as sent).
    // Called from worker threads only — never from the UI thread.
    // True when `url` is something safe to hand to the shell to open.
    //
    // Launching a link ends in ShellExecute, which will just as happily open a
    // local path, a UNC share or any registered protocol handler. The feed
    // decides the contents of that string, so it is checked rather than trusted:
    // http or https only, a host with a dot in it, no embedded credentials, no
    // control characters, and a sane length.
    bool isSafeUrl(const std::string& url);

    // Called as the body arrives, with the bytes received so far and the total
    // announced by Content-Length — zero when the server does not announce one.
    // Runs on the calling (worker) thread, so it must not touch the UI directly.
    using Progress = std::function<void(size_t received, size_t total)>;

    // `canceller` may be null. When it is not, the request registers itself with
    // it and can be aborted from another thread.
    GetResult getToString(const wxString& url, std::string& out,
                          Canceller* canceller = nullptr,
                          Progress progress = nullptr);
}
