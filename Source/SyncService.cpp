#include "SyncService.h"
#include "Database.h"
#include "FlippSource.h"

wxDEFINE_EVENT(EVT_SYNC_PROGRESS, wxThreadEvent);
wxDEFINE_EVENT(EVT_SYNC_DONE, wxThreadEvent);

SyncService::~SyncService()
{
    stop();
}

bool SyncService::start(const wxString& databasePath, const std::string& postalCode,
                        int merchantId)
{
    if (running_.load())
        return false;

    stop();                 // joins a finished-but-unjoined thread

    cancel_.store(false);
    canceller_.reset();
    running_.store(true);

    thread_ = std::thread(&SyncService::run, this, databasePath, postalCode, merchantId);
    return true;
}

void SyncService::stop()
{
    cancel_.store(true);

    // Not only a flag: the worker may be inside a network call that no flag can
    // interrupt. This closes the request out from under it so the join below
    // returns at once instead of waiting out the timeout.
    canceller_.abort();

    if (thread_.joinable())
        thread_.join();

    running_.store(false);
}

void SyncService::run(wxString databasePath, std::string postalCode, int merchantId)
{
    auto finish = [this](bool ok, const wxString& error, long items)
    {
        // The flag is cleared before the event is queued, so isRunning() reads
        // false for the moment the completion spends in the queue. The window
        // covers that gap with its own syncPending_; see MainWindow::startSync.
        running_.store(false);

        auto* event = new wxThreadEvent(EVT_SYNC_DONE);
        event->SetInt(ok ? 1 : 0);
        event->SetString(error);
        event->SetExtraLong(items);
        wxQueueEvent(sink_, event);
    };

    db::Database database;
    std::string error;
    if (!database.open(databasePath, error))
    {
        finish(false, wxString::FromUTF8(error), 0);
        return;
    }

    // The worker connection is separate from the window's, so it carries its own
    // copy of the scope. Everything it writes and reads back belongs to this run.
    database.setScope({ postalCode });

    const source::CancelFn cancelled = [this] { return cancel_.load(); };

    source::FlippSource feed;
    feed.setCanceller(&canceller_);

    std::vector<model::Merchant> merchants;
    std::vector<model::Flyer>    flyers;

    source::Result r = feed.fetchFlyers(postalCode, merchants, flyers, cancelled);
    if (!r.ok)
    {
        finish(false, wxString::FromUTF8(r.error), 0);
        return;
    }

    // The merchant list is written before anything else, so a first run on a new
    // postal code populates the Settings tab even if the item pass then fails.
    database.upsertMerchants(merchants);
    database.upsertFlyers(flyers, postalCode);

    // Only followed merchants get their items pulled. Fetching all ninety-six
    // would be a hundred requests for flyers the user will never open.
    const std::vector<model::Flyer> mine = database.flyers(merchantId, true);

    const int total = static_cast<int>(mine.size());

    long itemsWritten = 0;
    int  done = 0;

    for (const model::Flyer& f : mine)
    {
        if (cancel_.load())
        {
            finish(false, wxString(), itemsWritten);
            return;
        }

        std::vector<model::Item> items;
        r = feed.fetchItems(f, postalCode, items, cancelled);

        // One unavailable flyer must not abandon the other sixteen; the user
        // still gets a usable cache, and the missing one returns next sync.
        if (r.ok)
        {
            database.upsertItems(items);
            itemsWritten += static_cast<long>(items.size());
        }

        ++done;

        auto* event = new wxThreadEvent(EVT_SYNC_PROGRESS);
        event->SetInt(done);
        event->SetExtraLong(total);
        event->SetString(wxString::FromUTF8(f.merchantName));
        wxQueueEvent(sink_, event);
    }

    // Expired flyers are kept for a season: they cost almost nothing and they are
    // the raw material for answering "is this price actually good?".
    database.purgeExpired(120);

    finish(true, wxString(), itemsWritten);
}
