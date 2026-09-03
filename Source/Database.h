#pragma once

#include "Model.h"

#include <wx/string.h>
#include <string>
#include <vector>

struct sqlite3;

//==============================================================================
// The local cache: everything the user can browse without a network.
//
// Two reasons this is a real database and not an in-memory list. First,
// PromoAccess must be usable offline and instantly — sync once, then browse at
// keyboard speed, which matters far more with a screen reader than with eyes.
// Second, keeping expired flyers is what makes price history possible later:
// "is 3.99 actually a good price for this?" is the question a flyer app should
// answer and none of them do.
//==============================================================================
namespace db
{

// Today, as "YYYY-MM-DD" in local time — the form every date in the schema uses,
// which makes validity a plain string comparison.
std::string today();

// Which week's deals to show.
//
// Banners publish next week's flyer before this week's expires, so the cache
// always holds two weeks at once — 2293 items against 1774 on a typical Quebec
// postal code. Read together they nearly double what the user has to walk
// through, for deals that are not yet valid.
enum class Week
{
    Current,   // valid today
    Next,      // starts after today
    Both
};

class Database
{
public:
    Database() = default;
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Opens (creating if needed) and migrates the schema. `error` is filled on
    // failure; the caller decides whether that is fatal.
    bool open(const wxString& path, std::string& error);
    void close();
    bool isOpen() const { return handle_ != nullptr; }

    // --- Settings -------------------------------------------------------------
    std::string setting(const char* key, const std::string& fallback = {}) const;
    void        setSetting(const char* key, const std::string& value);

    // --- Scope ----------------------------------------------------------------
    // Flyers are regional, and the feed decides that region by the delivery zone
    // a store declares -- not by distance. A supermarket a few minutes away can
    // be absent from one postal code and present in the neighbouring one, and
    // the same flyer is usually shared by a whole cluster of codes. So the scope
    // is a set of postal codes, and a flyer belongs to every code it was seen
    // under (see the flyer_zones table).
    //
    // An empty scope reads everything, which is what the console harness wants.
    void setScope(const std::vector<std::string>& postalCodes) { scope_ = postalCodes; }
    const std::vector<std::string>& scope() const { return scope_; }

    // --- Filters --------------------------------------------------------------
    // Applied by every read that takes `currentOnly`, so one setting covers the
    // flyer list, the item search and the favourite matches alike.
    void setWeek(Week week) { week_ = week; }
    Week week() const { return week_; }

    // Drops items advertised with no price at all — no number, no "2 for 5$", no
    // per-pound text. 7% of a typical sync: section headings and call-to-action
    // panels the feed hands over as if they were products.
    void setHidePriceless(bool hide) { hidePriceless_ = hide; }
    bool hidePriceless() const { return hidePriceless_; }

    // --- Merchants ------------------------------------------------------------
    // Inserts new merchants and refreshes names, preserving the followed flag —
    // the user's choice of banners must survive every sync.
    void upsertMerchants(const std::vector<model::Merchant>& merchants);
    std::vector<model::Merchant> merchants(bool followedOnly = false) const;
    void setFollowed(int merchantId, bool followed);
    int  followedCount() const;

    // --- Flyers ---------------------------------------------------------------
    // `postalCode` is the zone this batch was fetched for; the flyers are
    // recorded as belonging to it, in addition to any zone they already had.
    void upsertFlyers(const std::vector<model::Flyer>& flyers,
                      const std::string& postalCode);
    // `merchantId` 0 means every followed merchant. `currentOnly` drops flyers
    // whose validity has passed.
    std::vector<model::Flyer> flyers(int merchantId = 0, bool currentOnly = true) const;

    // --- Items ----------------------------------------------------------------
    void upsertItems(const std::vector<model::Item>& items);

    // The generation of the detail parser. Bumped when fetchItemDetail learns to
    // read a field the stored rows cannot have; rows below it are re-asked once,
    // as they are walked past, and nothing the user perceives regresses.
    // 4: the detail parser learnt to read the unit out of price_text.
    // 5: and then out of the description, where Walmart writes it. Each bump
    // makes rows fetched by the older parser be asked about once more.
    static int detailRevision() { return 5; }

    // Writes what only the per-item endpoint knows: the original price, the
    // format, the SKU, the product page, the in-store-only flag. Never touches the current price —
    // that comes from the flyer, which is the authority on what is advertised.
    // Marks the item as looked up either way, including when it carried nothing.
    void updateItemDetail(const model::Item& item);
    std::vector<model::Item> itemsOfFlyer(long long flyerId) const;

    // Local search over the cache. `pattern` is tokenized and every token must
    // appear; `merchantId` 0 means all; `maxPrice` 0 means no price condition.
    // `wholeWords` matches each token as a word rather than as any run of
    // letters — "ail" then finds "ail frais" and not "ailes de poulet".
    std::vector<model::Item> searchItems(const std::string& pattern,
                                         const std::vector<int>& merchantIds = {},
                                         double maxPrice = 0.0,
                                         bool currentOnly = true,
                                         bool followedOnly = true,
                                         bool wholeWords = false) const;

    // --- Favourites -----------------------------------------------------------
    long long addFavorite(const model::Favorite& favorite);
    void      updateFavorite(const model::Favorite& favorite);
    void      removeFavorite(long long id);
    std::vector<model::Favorite> favorites() const;

    // --- Shopping list --------------------------------------------------------
    long long addListEntry(const model::ListEntry& entry);
    void      removeListEntry(long long id);
    void      clearList();

    // Removes the lines whose deal has ended, and says how many went. The list
    // is a snapshot: nothing else ever expires it, so this is the only way a
    // line from three weeks ago leaves.
    int       removeExpiredListEntries();
    void      setListQuantity(long long id, int quantity);

    // The line already holding this product, or an empty entry when there is
    // none. Matched on name, banner AND price, not on name alone: a banner
    // advertises the same product twice in one week at two prices — a format,
    // a variety, a members-only deal — and folding those into one line would
    // total the wrong amount and hide a choice the user was making.
    model::ListEntry findListEntry(const std::string& name,
                                   const std::string& merchantName,
                                   double price) const;
    void      setListChecked(long long id, bool checked);
    std::vector<model::ListEntry> listEntries() const;

    // Recomputes name_norm for every cached item.
    //
    // The search index is written once, at insert time, by text::normalize. Any
    // change to that function leaves every existing row indexed by the old
    // rules — invisible to the new ones — so a corrected normalizer has to be
    // paired with a rebuild. Cheap: a few thousand rows in one transaction.
    void rebuildSearchIndex();

    // --- Housekeeping ---------------------------------------------------------
    // Drops flyers and items that expired more than `keepDays` ago. Called at the
    // end of a sync, not at startup — a cache nobody syncs is never purged, which
    // is the right behaviour since nothing new arrived either. A generous window
    // is what feeds the future price history.
    void purgeExpired(int keepDays);

private:
    bool exec(const char* sql, std::string& error);
    void exec(const char* sql);          // fire-and-forget for statements that cannot fail meaningfully

    // Builds "IN (?a, ?b, ...)" for the scope and returns the first parameter
    // index it used, so callers can carry on numbering after it.
    std::string scopeClause(int firstParam) const;

    // The validity condition for the selected week, over `alias`.valid_from and
    // `alias`.valid_to, using `todayParam` for today's date.
    std::string weekClause(const char* alias, int todayParam) const;

    sqlite3*                 handle_ = nullptr;
    std::vector<std::string> scope_;   // postal codes every flyer read is limited to
    Week                     week_ = Week::Both;
    bool                     hidePriceless_ = false;
};

} // namespace db
