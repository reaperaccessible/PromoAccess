#pragma once

#include "Model.h"

#include <wx/event.h>
#include <wx/string.h>

#include "Http.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

//==============================================================================
// Background refresh of the local cache.
//
// A full sync is one request for the flyer list plus one per followed flyer —
// seventeen flyers and four thousand items on a typical Quebec postal code, a
// good ten seconds of network. That never runs on the UI thread: a frozen window
// is bad for everyone and unusable with a screen reader, which cannot read a
// window that is not pumping messages.
//
// The worker opens its own database connection. SQLite is built serialized here,
// so sharing one would be legal, but a second connection over WAL lets the UI
// keep reading the cache while the sync writes to it.
//==============================================================================

// Carries a step of the sync. `GetInt` is the completed flyer count, `GetExtraLong`
// the total, `GetString` the merchant just finished.
wxDECLARE_EVENT(EVT_SYNC_PROGRESS, wxThreadEvent);

// Fires once, whatever the outcome. `GetInt` is 1 on success, `GetString` holds
// the error when it is 0, and `GetExtraLong` the number of items written.
wxDECLARE_EVENT(EVT_SYNC_DONE, wxThreadEvent);

class SyncService
{
public:
    explicit SyncService(wxEvtHandler* sink) : sink_(sink) {}
    ~SyncService();

    SyncService(const SyncService&) = delete;
    SyncService& operator=(const SyncService&) = delete;

    // Starts a sync for `postalCode` against the cache at `databasePath`.
    // `merchantId` 0 covers every followed banner; a specific id fetches just
    // that one, which is what ticking a banner needs — downloading thirteen
    // flyers to obtain two would make the tick feel broken.
    //
    // Returns false if one is already running: the caller queues instead, since
    // a second run would only fight the first for the same rows.
    bool start(const wxString& databasePath, const std::string& postalCode,
               int merchantId = 0);

    // Asks the worker to stop and waits for it. Safe to call when idle; called
    // from the window's close handler, so it must never hang.
    void stop();

    bool isRunning() const { return running_.load(); }

private:
    void run(wxString databasePath, std::string postalCode, int merchantId);

    // Owned by the service, so it outlives every thread it hands it to and can
    // be aborted from the UI thread without a lifetime question.
    http::Canceller   canceller_;

    wxEvtHandler*     sink_ = nullptr;
    std::thread       thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_{false};
};
