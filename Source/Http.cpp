#include "Http.h"

#include <windows.h>
#include <winhttp.h>
#include <vector>
#include <string>

namespace http
{
namespace
{
    // RAII for WinHTTP handles.
    struct Handle
    {
        HINTERNET h = nullptr;
        Handle() = default;
        explicit Handle(HINTERNET x) : h(x) {}
        ~Handle() { if (h) WinHttpCloseHandle(h); }
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
        Handle& operator=(HINTERNET x) { if (h) WinHttpCloseHandle(h); h = x; return *this; }
        operator HINTERNET() const { return h; }
        explicit operator bool() const { return h != nullptr; }
    };

    wxString lastError(const wxString& stage)
    {
        return wxString::Format("%s (WinHTTP error %lu)", stage, ::GetLastError());
    }
}

bool isSafeUrl(const std::string& url)
{
    if (url.size() < 11 || url.size() > 2048)
        return false;

    const bool https = (url.rfind("https://", 0) == 0);
    if (!https && url.rfind("http://", 0) != 0)
        return false;

    for (const unsigned char c : url)
        if (c < 0x20 || c == 0x7F)
            return false;

    const size_t hostStart = https ? 8 : 7;
    const size_t hostEnd   = url.find('/', hostStart);
    const std::string host = url.substr(hostStart,
                                        (hostEnd == std::string::npos) ? std::string::npos
                                                                       : hostEnd - hostStart);

    // No credentials, no empty host, and something that at least looks like a
    // domain rather than a bare word.
    if (host.empty() || host.find('@') != std::string::npos)
        return false;

    return host.find('.') != std::string::npos;
}

Canceller::~Canceller()
{
    release();
}

void Canceller::abort()
{
    std::lock_guard<std::mutex> lock(mutex_);

    aborted_ = true;

    if (handle_ != nullptr)
    {
        // Closing a handle another thread is blocked on is the supported way to
        // cancel a synchronous WinHTTP call; the pending call returns an error
        // immediately. Cleared here so release() does not close it twice.
        WinHttpCloseHandle(static_cast<HINTERNET>(handle_));
        handle_ = nullptr;
    }
}

void Canceller::reset()
{
    std::lock_guard<std::mutex> lock(mutex_);
    aborted_ = false;
}

bool Canceller::aborted() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return aborted_;
}

bool Canceller::adopt(void* handle)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (aborted_)
        return false;

    handle_ = handle;
    return true;
}

void Canceller::release()
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (handle_ != nullptr)
    {
        WinHttpCloseHandle(static_cast<HINTERNET>(handle_));
        handle_ = nullptr;
    }
}

GetResult getToString(const wxString& url, std::string& out, Canceller* canceller)
{
    out.clear();
    GetResult res;

    Handle session(WinHttpOpen(L"PromoAccess/1.0",
                               WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) { res.error = lastError("Session open failed"); return res; }

    // Bounded deliberately. These are not only about failing a request: closing
    // the window joins the background workers, and a worker inside WinHTTP
    // cannot be interrupted — cancellation is only observed between calls. Every
    // second allowed here is a second the window can spend not responding while
    // the user waits for it to close, with nothing for a screen reader to read.
    //
    // Eight seconds to connect and fifteen to receive is ample for a feed that
    // normally answers in a fraction of a second, and caps that wait.
    WinHttpSetTimeouts(session, 8000, 8000, 10000, 15000);

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256]  = {};
    wchar_t path[4096] = {};
    wchar_t extra[4096] = {};
    uc.lpszHostName  = host;  uc.dwHostNameLength  = 255;
    uc.lpszUrlPath   = path;  uc.dwUrlPathLength   = 4095;
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 4095;

    const std::wstring wurl = url.ToStdWstring();
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc))
    {
        res.error = lastError("Invalid URL");
        return res;
    }

    // Path and query recombined — see the note in Http.h.
    const std::wstring target = std::wstring(path) + extra;
    const bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    Handle connect(WinHttpConnect(session, host, uc.nPort, 0));
    if (!connect) { res.error = lastError("Connect failed"); return res; }

    HINTERNET rawRequest = WinHttpOpenRequest(connect, L"GET", target.c_str(), nullptr,
                                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              https ? WINHTTP_FLAG_SECURE : 0);
    if (rawRequest == nullptr) { res.error = lastError("OpenRequest failed"); return res; }

    // The request handle is owned by the canceller rather than by a local RAII
    // guard, so that an abort from another thread and the normal cleanup here
    // cannot both close it. When there is no canceller, a local one plays the
    // same role and simply never aborts.
    Canceller local;
    Canceller& guard = (canceller != nullptr) ? *canceller : local;

    if (!guard.adopt(rawRequest))
    {
        // Already cancelled before this call even reached the network.
        WinHttpCloseHandle(rawRequest);
        res.error = "Cancelled";
        return res;
    }

    // Declared after `session` and `connect` so it runs FIRST: the request must
    // be closed before the connection it belongs to.
    struct Releaser
    {
        Canceller& guard;
        ~Releaser() { guard.release(); }
    } releaser{ guard };

    const HINTERNET request = rawRequest;

    // The feed serves gzip when asked and is several times smaller for it; WinHTTP
    // decompresses transparently once this option is set. Failure is not fatal —
    // without it the server simply replies uncompressed.
    DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(request, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof(decompress));

    static const wchar_t* kHeaders = L"Accept: application/json\r\n";

    if (!WinHttpSendRequest(request, kHeaders, (DWORD)-1L,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
     || !WinHttpReceiveResponse(request, nullptr))
    {
        res.error = lastError("Request failed");
        return res;
    }

    DWORD code = 0, len = sizeof(code);
    WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
    res.statusCode = static_cast<long>(code);

    if (res.statusCode != 200)
    {
        res.error = wxString::Format("HTTP %ld", res.statusCode);
        return res;
    }

    DWORD avail = 0;
    do
    {
        if (!WinHttpQueryDataAvailable(request, &avail)) { res.error = lastError("Read failed"); return res; }
        if (avail == 0) break;

        std::vector<char> buf(avail);
        DWORD got = 0;
        if (!WinHttpReadData(request, buf.data(), avail, &got)) { res.error = lastError("Read failed"); return res; }
        out.append(buf.data(), got);
    }
    while (avail > 0);

    res.ok = true;
    return res;
}

} // namespace http
