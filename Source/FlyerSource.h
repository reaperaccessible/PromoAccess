#pragma once

#include "Model.h"

#include <functional>
#include <string>
#include <vector>

//==============================================================================
// The seam between PromoAccess and wherever flyer data happens to come from.
//
// Everything that knows a URL or a JSON field name lives behind this interface
// and nowhere else. That is deliberate: the current backend is an undocumented
// vendor API that can change or close without notice, and some banners may one
// day have to be served by their own adapter. When that happens we add a second
// implementation — the database, the matching logic and the whole UI are
// untouched.
//==============================================================================
namespace http { class Canceller; }

namespace source
{

struct Result
{
    bool        ok = false;
    std::string error;      // English, technical; the UI localizes its own wording
    static Result success()                    { return { true, {} }; }
    static Result failure(std::string message) { return { false, std::move(message) }; }
};

// Polled between network requests so a long sync can be abandoned when the user
// closes the window. Returning true aborts; the partial result is discarded.
using CancelFn = std::function<bool()>;

class IFlyerSource
{
public:
    virtual ~IFlyerSource() = default;

    // Stable identifier stored alongside cached rows, so a future second source
    // can coexist with this one in the same database.
    virtual std::string id() const = 0;

    // Hands the adapter a token another thread can use to abort a request that
    // is already in flight. The polling CancelFn below is checked between items;
    // this is what interrupts the network call itself. The token must outlive
    // every call made through this adapter.
    virtual void setCanceller(http::Canceller* canceller) = 0;

    // Every flyer currently available for `postalCode`, with the merchant list
    // it implies. One request.
    virtual Result fetchFlyers(const std::string& postalCode,
                               std::vector<model::Merchant>& merchantsOut,
                               std::vector<model::Flyer>& flyersOut,
                               const CancelFn& isCancelled) = 0;

    // Every advertised item in one flyer, with prices. One request per flyer.
    virtual Result fetchItems(const model::Flyer& flyer,
                              const std::string& postalCode,
                              std::vector<model::Item>& itemsOut,
                              const CancelFn& isCancelled) = 0;

    // Format, bilingual name, SKU and disclaimer for a single item. Not part of
    // the flyer listing, so it is fetched on demand when the user opens an item
    // rather than 75 times per flyer during a sync.
    virtual Result fetchItemDetail(long long itemId,
                                   const std::string& postalCode,
                                   model::Item& itemInOut) = 0;

    // Advertised items matching `query` across every merchant at once. One
    // request, roughly 150 results — the fastest answer to "who has chicken on
    // sale this week" and the engine behind favourite matching.
    virtual Result search(const std::string& query,
                          const std::string& postalCode,
                          std::vector<model::Item>& itemsOut,
                          const CancelFn& isCancelled) = 0;
};

} // namespace source
