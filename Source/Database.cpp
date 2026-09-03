#include "Database.h"
#include "Text.h"

#include "ThirdParty/sqlite/sqlite3.h"

#include <ctime>
#include <cstdio>

namespace db
{
namespace
{
    // RAII for a prepared statement.
    class Stmt
    {
    public:
        Stmt(sqlite3* handle, const char* sql)
        {
            if (handle != nullptr)
                sqlite3_prepare_v2(handle, sql, -1, &stmt_, nullptr);
        }
        ~Stmt() { if (stmt_) sqlite3_finalize(stmt_); }

        Stmt(const Stmt&) = delete;
        Stmt& operator=(const Stmt&) = delete;

        explicit operator bool() const { return stmt_ != nullptr; }
        sqlite3_stmt* get() const { return stmt_; }

        void bind(int i, const std::string& v)
        {
            // SQLITE_TRANSIENT: the string may be a temporary at the call site.
            sqlite3_bind_text(stmt_, i, v.c_str(), -1, SQLITE_TRANSIENT);
        }
        void bind(int i, long long v) { sqlite3_bind_int64(stmt_, i, v); }
        void bind(int i, int v)       { sqlite3_bind_int(stmt_, i, v); }
        void bind(int i, double v)    { sqlite3_bind_double(stmt_, i, v); }

        bool step() { return sqlite3_step(stmt_) == SQLITE_ROW; }
        void run()  { sqlite3_step(stmt_); }

        std::string text(int col) const
        {
            const unsigned char* p = sqlite3_column_text(stmt_, col);
            return p ? reinterpret_cast<const char*>(p) : std::string{};
        }
        long long i64(int col)  const { return sqlite3_column_int64(stmt_, col); }
        int       i32(int col)  const { return sqlite3_column_int(stmt_, col); }
        double    real(int col) const { return sqlite3_column_double(stmt_, col); }

    private:
        sqlite3_stmt* stmt_ = nullptr;
    };

    // Columns every query depends on. Verified after the migrations have run,
    // because a migration that fails for a real reason — a read-only file, a
    // locked database — is swallowed by the error-ignoring exec() and is
    // otherwise indistinguishable from one that was already applied. The
    // consequence is not a visible error but an application that prepares every
    // item query against a missing column, fails, and shows an empty cache
    // forever.
    struct RequiredColumn { const char* table; const char* column; };
    const RequiredColumn kRequiredColumns[] =
    {
        { "items",  "detail_fetched" },
        { "items",  "product_url" },
        { "items",  "detail_revision" },
        { "items",  "name_tokens" },
        { "items",  "brand" },
        { "items",  "discount" },
        { "favorites", "whole_words" },
        { "favorites", "merchant_ids" },
        { "items",  "name_norm" },
        { "flyers", "item_count" },
    };

    // Schema, applied in order. Adding a migration means appending a statement;
    // every one is written to be safe to re-run.
    const char* kSchema =
        "PRAGMA journal_mode = WAL;"
        "PRAGMA synchronous = NORMAL;"
        "PRAGMA foreign_keys = ON;"

        "CREATE TABLE IF NOT EXISTS settings ("
        "  key   TEXT PRIMARY KEY,"
        "  value TEXT NOT NULL"
        ");"

        "CREATE TABLE IF NOT EXISTS merchants ("
        "  id       INTEGER PRIMARY KEY,"
        "  name     TEXT NOT NULL,"
        "  followed INTEGER NOT NULL DEFAULT 0"
        ");"

        "CREATE TABLE IF NOT EXISTS flyers ("
        "  id            INTEGER PRIMARY KEY,"
        "  merchant_id   INTEGER NOT NULL,"
        "  merchant_name TEXT NOT NULL,"
        "  name          TEXT,"
        "  valid_from    TEXT,"
        "  valid_to      TEXT,"
        "  item_count    INTEGER NOT NULL DEFAULT 0,"
        "  synced_at     TEXT"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_flyers_merchant ON flyers(merchant_id);"
        "CREATE INDEX IF NOT EXISTS idx_flyers_valid    ON flyers(valid_to);"

        // Which postal codes a flyer serves. A separate table rather than a
        // column on flyers, because the relation is many-to-many: one flyer
        // covers a cluster of codes, and a column would let the second sync
        // overwrite the zone recorded by the first.
        "CREATE TABLE IF NOT EXISTS flyer_zones ("
        "  flyer_id    INTEGER NOT NULL,"
        "  postal_code TEXT NOT NULL,"
        "  PRIMARY KEY (flyer_id, postal_code)"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_zones_code ON flyer_zones(postal_code);"

        "CREATE TABLE IF NOT EXISTS items ("
        "  id             INTEGER PRIMARY KEY,"
        "  flyer_id       INTEGER NOT NULL,"
        "  merchant_id    INTEGER NOT NULL,"
        "  merchant_name  TEXT NOT NULL,"
        "  name           TEXT NOT NULL,"
        "  name_norm      TEXT NOT NULL,"
        // The same words, each one padded with spaces, so a LIKE can ask for a
        // WORD rather than for a run of letters: '% ail %' cannot match "ailes".
        "  name_tokens    TEXT NOT NULL DEFAULT '',"
        "  description    TEXT,"
        "  sku            TEXT,"
        "  brand          TEXT NOT NULL DEFAULT '',"
        // -1 = the banner said nothing. Distinct from 0, which would mean "no
        // discount" and would sort a silent item alongside a full-price one.
        "  discount       INTEGER NOT NULL DEFAULT -1,"
        "  current_price  REAL NOT NULL DEFAULT 0,"
        "  original_price REAL NOT NULL DEFAULT 0,"
        "  price_text     TEXT,"
        "  sale_story     TEXT,"
        "  in_store_only  INTEGER NOT NULL DEFAULT 0,"
        "  valid_from     TEXT,"
        "  valid_to       TEXT,"
        "  detail_fetched INTEGER NOT NULL DEFAULT 0,"
        // NOT NULL with a default on both paths on purpose: ADD COLUMN without
        // one fills existing rows with NULL, so a fresh cache would hold '' and
        // an upgraded one NULL, and every predicate would have to remember which.
        "  product_url    TEXT NOT NULL DEFAULT '',"
        "  detail_revision INTEGER NOT NULL DEFAULT 0"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_items_flyer    ON items(flyer_id);"
        "CREATE INDEX IF NOT EXISTS idx_items_merchant ON items(merchant_id);"
        "CREATE INDEX IF NOT EXISTS idx_items_norm     ON items(name_norm);"
        "CREATE INDEX IF NOT EXISTS idx_items_tokens   ON items(name_tokens);"
        "CREATE INDEX IF NOT EXISTS idx_items_valid    ON items(valid_to);"

        "CREATE TABLE IF NOT EXISTS favorites ("
        "  id            INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  pattern       TEXT NOT NULL,"
        "  merchant_id   INTEGER NOT NULL DEFAULT 0,"
        "  merchant_name TEXT,"
        "  max_price     REAL NOT NULL DEFAULT 0,"
        "  enabled       INTEGER NOT NULL DEFAULT 1,"
        "  whole_words   INTEGER NOT NULL DEFAULT 1,"
        // Comma separated, spaces around each one — ",12,34," — so the column
        // reads back with a single split and no ambiguity on an empty list.
        "  merchant_ids  TEXT NOT NULL DEFAULT ''"
        ");"

        "CREATE TABLE IF NOT EXISTS list_entries ("
        "  id            INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name          TEXT NOT NULL,"
        "  merchant_name TEXT,"
        "  price         REAL NOT NULL DEFAULT 0,"
        "  price_text    TEXT,"
        "  quantity      INTEGER NOT NULL DEFAULT 1,"
        "  valid_to      TEXT,"
        "  checked       INTEGER NOT NULL DEFAULT 0,"
        "  added_at      TEXT"
        ");";

    // Migrations for caches created by an earlier version. Each is expected to
    // fail once its column exists, so they run through the error-swallowing
    // exec() rather than the schema string.
    const char* kMigrations[] =
    {
        "ALTER TABLE items ADD COLUMN detail_fetched INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE items ADD COLUMN product_url TEXT NOT NULL DEFAULT ''",
        "ALTER TABLE items ADD COLUMN detail_revision INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE items ADD COLUMN name_tokens TEXT NOT NULL DEFAULT ''",
        "ALTER TABLE favorites ADD COLUMN whole_words INTEGER NOT NULL DEFAULT 1",
        "ALTER TABLE favorites ADD COLUMN merchant_ids TEXT NOT NULL DEFAULT ''",
        "ALTER TABLE items ADD COLUMN brand TEXT NOT NULL DEFAULT ''",
        "ALTER TABLE items ADD COLUMN discount INTEGER NOT NULL DEFAULT -1",
    };

    // "YYYY-MM-DD" for a time_t offset in days from now.
    std::string isoDate(int dayOffset)
    {
        std::time_t t = std::time(nullptr) + static_cast<std::time_t>(dayOffset) * 86400;
        std::tm tm{};
        localtime_s(&tm, &t);

        char buf[16];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        return buf;
    }
}

std::string today() { return isoDate(0); }

namespace
{
    // " ail frais selection " — every word surrounded by spaces, so '% ail %'
    // matches the word and nothing that merely contains its letters.
    // ",12,34," and back. The leading and trailing commas are not decoration:
    // they keep the empty list distinguishable from the list holding one banner.
    std::string packIds(const std::vector<int>& ids)
    {
        if (ids.empty())
            return {};

        std::string out = ",";
        for (int id : ids)
            out += std::to_string(id) + ",";

        return out;
    }

    std::vector<int> unpackIds(const std::string& packed)
    {
        std::vector<int> out;
        std::string digits;

        for (char c : packed)
        {
            if (c >= '0' && c <= '9')
            {
                digits += c;
            }
            else if (!digits.empty())
            {
                out.push_back(std::atoi(digits.c_str()));
                digits.clear();
            }
        }

        if (!digits.empty())
            out.push_back(std::atoi(digits.c_str()));

        return out;
    }

    std::string paddedTokens(const std::string& text)
    {
        std::string out = " ";
        for (const std::string& word : text::tokens(text))
            out += word + " ";

        return out;
    }
}

//==============================================================================
Database::~Database() { close(); }

bool Database::open(const wxString& path, std::string& error)
{
    close();

    const std::string utf8 = path.utf8_string();
    const int rc = sqlite3_open_v2(utf8.c_str(), &handle_,
                                   SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (rc != SQLITE_OK || handle_ == nullptr)
    {
        error = handle_ ? sqlite3_errmsg(handle_) : "Could not open the cache file";
        close();
        return false;
    }

    // A sync writes a few thousand rows; five seconds of patience beats an
    // SQLITE_BUSY surfaced to the user as a failed sync.
    sqlite3_busy_timeout(handle_, 5000);

    // Migrations FIRST, schema second. The schema string creates indexes over
    // columns the migrations add, so running it first on an older cache fails on
    // "no such column" and aborts the whole open. On a new cache the migrations
    // fail instead — harmlessly, the tables do not exist yet — and the schema
    // creates everything.
    for (const char* migration : kMigrations)
        exec(migration);

    if (!exec(kSchema, error))
        return false;

    for (const RequiredColumn& required : kRequiredColumns)
    {
        Stmt probe(handle_, (std::string("SELECT ") + required.column
                             + " FROM " + required.table + " LIMIT 1").c_str());
        if (!probe)
        {
            error = std::string("The cache is missing ") + required.table + "."
                  + required.column + " and could not be upgraded: "
                  + sqlite3_errmsg(handle_);
            close();
            return false;
        }
    }

    return true;
}

std::string Database::weekClause(const char* alias, int todayParam) const
{
    const std::string a = std::string(alias) + ".";
    const std::string p = "?" + std::to_string(todayParam);

    // An undated row must stay visible rather than be hidden on a technicality.
    // Testing for NULL alone was dead code: the feed's missing dates arrive as
    // empty strings and are bound as '', never as SQL NULL, so an undated flyer
    // was invisible in all three modes — cached, counted, and unreachable.
    const std::string to   = "NULLIF(" + a + "valid_to, '')";
    const std::string from = "NULLIF(" + a + "valid_from, '')";

    switch (week_)
    {
        // Valid today: it started on or before today and has not run out.
        case Week::Current:
            return " AND (" + to + " IS NULL OR " + to + " >= " + p + ")"
                   " AND (" + from + " IS NULL OR " + from + " <= " + p + ")";

        // Not yet in force: next week's flyer, already published. An undated row
        // cannot be shown to start later, so it does not belong here.
        case Week::Next:
            return " AND " + from + " > " + p;

        case Week::Both:
        default:
            return " AND (" + to + " IS NULL OR " + to + " >= " + p + ")";
    }
}

// Builds the "IN (?n, ...)" list for the current scope. Empty when the scope is,
// which is how the harness reads across every region at once.
std::string Database::scopeClause(int firstParam) const
{
    std::string list;
    for (size_t n = 0; n < scope_.size(); ++n)
    {
        if (n > 0) list += ", ";
        list += "?" + std::to_string(firstParam + static_cast<int>(n));
    }
    return list;
}

void Database::close()
{
    if (handle_ != nullptr)
    {
        // _v2, not sqlite3_close: the plain form returns SQLITE_BUSY and leaves
        // the connection ALLOCATED when a statement is unfinalized or a
        // transaction still open, and this code nulls the handle either way —
        // which would leak the connection along with the file locks it holds,
        // making every later write more likely to time out. _v2 marks it as a
        // zombie and frees it as soon as the last statement goes.
        sqlite3_close_v2(handle_);
        handle_ = nullptr;
    }
}

bool Database::exec(const char* sql, std::string& error)
{
    char* message = nullptr;
    if (sqlite3_exec(handle_, sql, nullptr, nullptr, &message) != SQLITE_OK)
    {
        error = message ? message : "SQL error";
        sqlite3_free(message);
        return false;
    }
    return true;
}

void Database::exec(const char* sql)
{
    std::string ignored;
    exec(sql, ignored);
}

//==============================================================================
// Settings
//==============================================================================
std::string Database::setting(const char* key, const std::string& fallback) const
{
    Stmt s(handle_, "SELECT value FROM settings WHERE key = ?1");
    if (!s) return fallback;

    s.bind(1, std::string(key));
    return s.step() ? s.text(0) : fallback;
}

void Database::setSetting(const char* key, const std::string& value)
{
    Stmt s(handle_,
           "INSERT INTO settings(key, value) VALUES(?1, ?2) "
           "ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    if (!s) return;

    s.bind(1, std::string(key));
    s.bind(2, value);
    s.run();
}

//==============================================================================
// Merchants
//==============================================================================
void Database::upsertMerchants(const std::vector<model::Merchant>& merchants)
{
    if (merchants.empty()) return;

    exec("BEGIN");
    for (const model::Merchant& m : merchants)
    {
        // Only the name is refreshed on conflict: overwriting `followed` would
        // silently unfollow every banner the user picked, at every sync.
        Stmt s(handle_,
               "INSERT INTO merchants(id, name, followed) VALUES(?1, ?2, 0) "
               "ON CONFLICT(id) DO UPDATE SET name = excluded.name");
        if (!s) break;

        s.bind(1, m.id);
        s.bind(2, m.name);
        s.run();
    }
    exec("COMMIT");
}

std::vector<model::Merchant> Database::merchants(bool followedOnly) const
{
    std::vector<model::Merchant> out;

    // The merchants table is national — it accumulates every banner ever seen,
    // whatever postal code brought it in. Offering all of them would mean a list
    // where most entries do not serve the user's town and ticking them achieves
    // nothing. So it is narrowed to banners that actually run a flyer in the
    // current zone.
    //
    // A banner already followed stays listed even if it no longer serves the
    // zone: it must remain possible to untick what one is following.
    std::string sql = "SELECT m.id, m.name, m.followed FROM merchants m WHERE ";

    if (followedOnly)
    {
        sql += "m.followed = 1";
    }
    else if (!scope_.empty())
    {
        sql += "(m.followed = 1 OR EXISTS (SELECT 1 FROM flyers f"
               " JOIN flyer_zones z ON z.flyer_id = f.id"
               " WHERE f.merchant_id = m.id AND z.postal_code IN (" + scopeClause(1) + ")))";
    }
    else
    {
        sql += "1=1";
    }

    sql += " ORDER BY m.name COLLATE NOCASE";

    Stmt s(handle_, sql.c_str());
    if (!s) return out;

    if (!followedOnly && !scope_.empty())
        for (size_t n = 0; n < scope_.size(); ++n)
            s.bind(1 + static_cast<int>(n), scope_[n]);

    while (s.step())
    {
        model::Merchant m;
        m.id       = s.i32(0);
        m.name     = s.text(1);
        m.followed = s.i32(2) != 0;
        out.push_back(std::move(m));
    }
    return out;
}

void Database::setFollowed(int merchantId, bool followed)
{
    Stmt s(handle_, "UPDATE merchants SET followed = ?2 WHERE id = ?1");
    if (!s) return;

    s.bind(1, merchantId);
    s.bind(2, followed ? 1 : 0);
    s.run();
}

int Database::followedCount() const
{
    Stmt s(handle_, "SELECT COUNT(*) FROM merchants WHERE followed = 1");
    return (s && s.step()) ? s.i32(0) : 0;
}

//==============================================================================
// Flyers
//==============================================================================
void Database::upsertFlyers(const std::vector<model::Flyer>& flyers,
                            const std::string& postalCode)
{
    if (flyers.empty()) return;

    const std::string now = today();

    exec("BEGIN");
    for (const model::Flyer& f : flyers)
    {
        Stmt s(handle_,
               "INSERT INTO flyers(id, merchant_id, merchant_name, name, valid_from, valid_to, item_count, synced_at) "
               "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8) "
               "ON CONFLICT(id) DO UPDATE SET "
               "  merchant_name = excluded.merchant_name,"
               "  name          = excluded.name,"
               "  valid_from    = excluded.valid_from,"
               "  valid_to      = excluded.valid_to,"
               "  item_count    = MAX(excluded.item_count, flyers.item_count),"
               "  synced_at     = excluded.synced_at");
        if (!s) break;

        s.bind(1, f.id);
        s.bind(2, f.merchantId);
        s.bind(3, f.merchantName);
        s.bind(4, f.name);
        s.bind(5, f.validFrom);
        s.bind(6, f.validTo);
        s.bind(7, f.itemCount);
        s.bind(8, now);
        s.run();

        if (!postalCode.empty())
        {
            Stmt z(handle_, "INSERT OR IGNORE INTO flyer_zones(flyer_id, postal_code) VALUES(?1, ?2)");
            if (z)
            {
                z.bind(1, f.id);
                z.bind(2, postalCode);
                z.run();
            }
        }
    }
    exec("COMMIT");
}

std::vector<model::Flyer> Database::flyers(int merchantId, bool currentOnly) const
{
    std::vector<model::Flyer> out;

    std::string sql =
        "SELECT f.id, f.merchant_id, f.merchant_name, f.name, f.valid_from, f.valid_to, f.item_count "
        "FROM flyers f JOIN merchants m ON m.id = f.merchant_id WHERE m.followed = 1";

    if (merchantId > 0) sql += " AND f.merchant_id = ?1";
    if (currentOnly)    sql += weekClause("f", 2);
    if (!scope_.empty())
        sql += " AND EXISTS (SELECT 1 FROM flyer_zones z WHERE z.flyer_id = f.id"
               " AND z.postal_code IN (" + scopeClause(3) + "))";

    sql += " ORDER BY f.merchant_name COLLATE NOCASE, f.valid_to DESC, f.item_count DESC";

    Stmt s(handle_, sql.c_str());
    if (!s) return out;

    if (merchantId > 0) s.bind(1, merchantId);
    if (currentOnly)    s.bind(2, today());
    for (size_t n = 0; n < scope_.size(); ++n)
        s.bind(3 + static_cast<int>(n), scope_[n]);

    while (s.step())
    {
        model::Flyer f;
        f.id           = s.i64(0);
        f.merchantId   = s.i32(1);
        f.merchantName = s.text(2);
        f.name         = s.text(3);
        f.validFrom    = s.text(4);
        f.validTo      = s.text(5);
        f.itemCount    = s.i32(6);
        out.push_back(std::move(f));
    }
    return out;
}

//==============================================================================
// Items
//==============================================================================
void Database::upsertItems(const std::vector<model::Item>& items)
{
    if (items.empty()) return;

    exec("BEGIN");
    for (const model::Item& i : items)
    {
        Stmt s(handle_,
               "INSERT INTO items(id, flyer_id, merchant_id, merchant_name, name, name_norm, name_tokens,"
               "                  description, sku, brand, discount, current_price, original_price,"
               "                  price_text, sale_story, in_store_only, valid_from, valid_to) "
               "VALUES(?1,?2,?3,?4,?5,?6,?16,?7,?8,?17,?18,?9,?10,?11,?12,?13,?14,?15) "
               "ON CONFLICT(id) DO UPDATE SET "
               "  current_price = excluded.current_price,"
               // price_text and sale_story are protected the same way as
               // description and sku below: the flyer endpoint never fills them,
               // so an unconditional assignment overwrote with '' whatever a
               // detail fetch had stored — and detail_fetched stayed 1, so it was
               // never asked again. An item priced "1,99 /lb" silently became
               // "no price listed", and vanished outright under the
               // hide-priceless filter.
               "  price_text    = COALESCE(NULLIF(excluded.price_text, ''), items.price_text),"
               "  sale_story    = COALESCE(NULLIF(excluded.sale_story, ''), items.sale_story),"
               // Detail fields are only ever filled by an on-demand detail fetch;
               // a plain sync must not blank what a detail fetch already stored.
               "  description   = COALESCE(NULLIF(excluded.description, ''), items.description),"
               "  sku           = COALESCE(NULLIF(excluded.sku, ''), items.sku),"
               "  brand         = COALESCE(NULLIF(excluded.brand, ''), items.brand),"
               // Same protection in numeric form: a feed that omits the
               // percentage must not erase one an earlier pass established.
               "  discount      = CASE WHEN excluded.discount > 0 THEN excluded.discount"
               "                       ELSE items.discount END,"
               "  valid_from    = excluded.valid_from,"
               "  valid_to      = excluded.valid_to");
        if (!s) break;

        // The name is indexed in its folded form so an accentless favourite
        // still matches; see text::normalize.
        const std::string norm = text::normalize(i.name + " " + i.description);

        s.bind(1,  i.id);
        s.bind(2,  i.flyerId);
        s.bind(3,  i.merchantId);
        s.bind(4,  i.merchantName);
        s.bind(5,  i.name);
        s.bind(6,  norm);
        s.bind(7,  i.description);
        s.bind(8,  i.sku);
        s.bind(9,  i.currentPrice);
        s.bind(10, i.originalPrice);
        s.bind(11, i.priceText);
        s.bind(12, i.saleStory);
        s.bind(13, i.inStoreOnly ? 1 : 0);
        s.bind(14, i.validFrom);
        s.bind(15, i.validTo);
        s.bind(16, paddedTokens(i.name + " " + i.description));
        s.bind(17, i.brand);
        s.bind(18, i.discountPercent);
        s.run();
    }
    exec("COMMIT");

    // Keep the flyer's advertised count in step with what is actually cached, so
    // the flyer list can say "Super C, 75 items" without a second query.
    Stmt c(handle_,
           "UPDATE flyers SET item_count = "
           "  (SELECT COUNT(*) FROM items WHERE items.flyer_id = flyers.id) "
           "WHERE id = ?1");
    if (c)
    {
        c.bind(1, items.front().flyerId);
        c.run();
    }
}

void Database::updateItemDetail(const model::Item& item)
{
    Stmt s(handle_,
           "UPDATE items SET "
           "  description    = CASE WHEN ?2 <> '' THEN ?2 ELSE description END,"
           "  sku            = CASE WHEN ?3 <> '' THEN ?3 ELSE sku END,"
           "  original_price = CASE WHEN ?4 > 0  THEN ?4 ELSE original_price END,"
           "  price_text     = CASE WHEN ?5 <> '' THEN ?5 ELSE price_text END,"
           "  sale_story     = CASE WHEN ?7 <> '' THEN ?7 ELSE sale_story END,"
           "  in_store_only  = MAX(in_store_only, ?6),"
           "  product_url    = CASE WHEN ?8 <> '' THEN ?8 ELSE product_url END,"
           "  brand          = CASE WHEN ?10 <> '' THEN ?10 ELSE brand END,"
           "  discount       = CASE WHEN ?11 > 0  THEN ?11 ELSE discount END,"
           "  detail_fetched = 1,"
           "  detail_revision = ?9 "
           "WHERE id = ?1");
    if (!s) return;

    s.bind(1, item.id);
    s.bind(2, item.description);
    s.bind(3, item.sku);
    s.bind(4, item.originalPrice);
    s.bind(5, item.priceText);
    s.bind(6, item.inStoreOnly ? 1 : 0);
    s.bind(7, item.saleStory);
    s.bind(8, item.productUrl);
    s.bind(9, detailRevision());
    s.bind(10, item.brand);
    s.bind(11, item.discountPercent);
    s.run();
}

namespace
{
    model::Item readItem(const Stmt& s)
    {
        model::Item i;
        i.id            = s.i64(0);
        i.flyerId       = s.i64(1);
        i.merchantId    = s.i32(2);
        i.merchantName  = s.text(3);
        i.name          = s.text(4);
        i.description   = s.text(5);
        i.sku           = s.text(6);
        i.currentPrice  = s.real(7);
        i.originalPrice = s.real(8);
        i.priceText     = s.text(9);
        i.saleStory     = s.text(10);
        i.inStoreOnly   = s.i32(11) != 0;
        i.validFrom     = s.text(12);
        i.validTo       = s.text(13);
        i.detailFetched = s.i32(14) != 0;
        i.productUrl    = s.text(15);
        i.detailRevision = s.i32(16);
        i.brand         = s.text(17);
        i.discountPercent = s.i32(18);
        return i;
    }

    // Every column is qualified with the "i" alias on purpose: searchItems joins
    // merchants, which also has an "id" and a "name", and an unqualified list
    // makes the whole statement fail to prepare — silently, as an empty result.
    const char* kItemColumns =
        "i.id, i.flyer_id, i.merchant_id, i.merchant_name, i.name, i.description, i.sku,"
        " i.current_price, i.original_price, i.price_text, i.sale_story, i.in_store_only,"
        " i.valid_from, i.valid_to, i.detail_fetched, i.product_url, i.detail_revision,"
        " i.brand, i.discount";
}

std::vector<model::Item> Database::itemsOfFlyer(long long flyerId) const
{
    std::vector<model::Item> out;

    // The week is not applied here: the user picked this flyer from a list that
    // was already filtered, and hiding part of what it contains would be a lie
    // about the flyer. The priceless filter does apply — those rows are noise in
    // any context.
    std::string sql = std::string("SELECT ") + kItemColumns
                    + " FROM items i WHERE i.flyer_id = ?1";

    if (hidePriceless_)
        sql += " AND (i.current_price > 0 OR i.sale_story <> '' OR i.price_text <> '')";

    sql += " ORDER BY i.name COLLATE NOCASE";

    Stmt s(handle_, sql.c_str());
    if (!s) return out;

    s.bind(1, flyerId);
    while (s.step())
        out.push_back(readItem(s));

    return out;
}

std::vector<model::Item> Database::searchItems(const std::string& pattern,
                                               const std::vector<int>& merchantIds,
                                               double maxPrice,
                                               bool currentOnly,
                                               bool followedOnly,
                                               bool wholeWords) const
{
    std::vector<model::Item> out;

    const std::vector<std::string> tokens = text::tokens(pattern);
    if (tokens.empty())
        return out;

    // One LIKE per token, ANDed. LIKE on the normalized column is a scan, but the
    // cache holds a few thousand rows at most and the query runs on a worker
    // thread — an FTS index would buy nothing at this size and cost a migration.
    int next = static_cast<int>(tokens.size()) + 1;

    std::string sql = std::string("SELECT ") + kItemColumns + " FROM items i";
    if (followedOnly)
        sql += " JOIN merchants m ON m.id = i.merchant_id AND m.followed = 1";
    sql += " WHERE 1=1";

    for (size_t n = 0; n < tokens.size(); ++n)
        sql += wholeWords
            ? " AND i.name_tokens LIKE ?" + std::to_string(n + 1)
            : " AND i.name_norm LIKE ?" + std::to_string(n + 1);

    // An item inherits its region from the flyer it came out of. Written as an
    // EXISTS rather than a join so a flyer covering several zones cannot
    // multiply the row it belongs to.
    const int scopeFirst = scope_.empty() ? 0 : next;
    if (scopeFirst)
    {
        sql += " AND EXISTS (SELECT 1 FROM flyer_zones z WHERE z.flyer_id = i.flyer_id"
               " AND z.postal_code IN (" + scopeClause(scopeFirst) + "))";
        next += static_cast<int>(scope_.size());
    }

    // One placeholder per banner. Written out rather than interpolated so the
    // identifiers stay bound values and never reach the SQL as text.
    const int merchantFirst = merchantIds.empty() ? 0 : next;
    if (merchantFirst)
    {
        sql += " AND i.merchant_id IN (";
        for (size_t n = 0; n < merchantIds.size(); ++n)
            sql += (n ? ",?" : "?") + std::to_string(next++);
        sql += ")";
    }

    const int priceParam = (maxPrice > 0.0) ? next++ : 0;
    // A price of 0 means "no number advertised" — those items are kept, because
    // "2 for 5$" can easily beat the threshold and dropping them would hide real
    // deals behind a filter the user meant as a ceiling, not a requirement.
    if (priceParam) sql += " AND (i.current_price = 0 OR i.current_price <= ?" + std::to_string(priceParam) + ")";

    const int dateParam = currentOnly ? next++ : 0;
    if (dateParam) sql += weekClause("i", dateParam);

    // An item with no number, no sale story and no unit text says nothing at all
    // — a section heading or an advertising panel the feed hands over as if it
    // were a product.
    if (hidePriceless_)
        sql += " AND (i.current_price > 0 OR i.sale_story <> '' OR i.price_text <> '')";

    sql += " ORDER BY i.current_price = 0, i.current_price ASC, i.name COLLATE NOCASE";

    Stmt s(handle_, sql.c_str());
    if (!s) return out;

    for (size_t n = 0; n < tokens.size(); ++n)
        s.bind(static_cast<int>(n + 1),
               wholeWords ? "% " + tokens[n] + " %" : "%" + tokens[n] + "%");

    if (scopeFirst)
        for (size_t n = 0; n < scope_.size(); ++n)
            s.bind(scopeFirst + static_cast<int>(n), scope_[n]);

    if (merchantFirst)
        for (size_t n = 0; n < merchantIds.size(); ++n)
            s.bind(merchantFirst + static_cast<int>(n), merchantIds[n]);
    if (priceParam)    s.bind(priceParam, maxPrice);
    if (dateParam)     s.bind(dateParam, today());

    while (s.step())
        out.push_back(readItem(s));

    return out;
}

//==============================================================================
// Favourites
//==============================================================================
long long Database::addFavorite(const model::Favorite& favorite)
{
    Stmt s(handle_,
           "INSERT INTO favorites(pattern, merchant_id, merchant_name, max_price, enabled,"
           " whole_words, merchant_ids) "
           "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7)");
    if (!s) return 0;

    s.bind(1, favorite.pattern);
    // merchant_id is written too: a copy of this cache opened by an older build
    // would otherwise see every favourite as covering all banners.
    s.bind(2, favorite.merchantIds.size() == 1 ? favorite.merchantIds[0] : 0);
    s.bind(3, favorite.merchantName);
    s.bind(4, favorite.maxPrice);
    s.bind(5, favorite.enabled ? 1 : 0);
    s.bind(6, favorite.wholeWords ? 1 : 0);
    s.bind(7, packIds(favorite.merchantIds));
    s.run();

    return sqlite3_last_insert_rowid(handle_);
}

void Database::updateFavorite(const model::Favorite& favorite)
{
    Stmt s(handle_,
           "UPDATE favorites SET pattern = ?2, merchant_id = ?3, merchant_name = ?4,"
           " max_price = ?5, enabled = ?6, whole_words = ?7, merchant_ids = ?8"
           " WHERE id = ?1");
    if (!s) return;

    s.bind(1, favorite.id);
    s.bind(2, favorite.pattern);
    s.bind(3, favorite.merchantIds.size() == 1 ? favorite.merchantIds[0] : 0);
    s.bind(4, favorite.merchantName);
    s.bind(5, favorite.maxPrice);
    s.bind(6, favorite.enabled ? 1 : 0);
    s.bind(7, favorite.wholeWords ? 1 : 0);
    s.bind(8, packIds(favorite.merchantIds));
    s.run();
}

void Database::removeFavorite(long long id)
{
    Stmt s(handle_, "DELETE FROM favorites WHERE id = ?1");
    if (!s) return;

    s.bind(1, id);
    s.run();
}

std::vector<model::Favorite> Database::favorites() const
{
    std::vector<model::Favorite> out;

    Stmt s(handle_,
           "SELECT id, pattern, merchant_id, merchant_name, max_price, enabled, whole_words,"
           " merchant_ids FROM favorites ORDER BY pattern COLLATE NOCASE");
    if (!s) return out;

    while (s.step())
    {
        model::Favorite f;
        f.id           = s.i64(0);
        f.pattern      = s.text(1);
        f.merchantName = s.text(3);
        f.maxPrice     = s.real(4);
        f.enabled      = s.i32(5) != 0;
        f.wholeWords   = s.i32(6) != 0;
        f.merchantIds  = unpackIds(s.text(7));

        // A favourite written before this column existed carries its single
        // banner in merchant_id alone.
        if (f.merchantIds.empty() && s.i32(2) > 0)
            f.merchantIds.push_back(s.i32(2));

        out.push_back(std::move(f));
    }
    return out;
}

//==============================================================================
// Shopping list
//==============================================================================
model::ListEntry Database::findListEntry(const std::string& name,
                                        const std::string& merchantName,
                                        double price) const
{
    model::ListEntry found;

    Stmt s(handle_,
           "SELECT id, name, merchant_name, price, price_text, quantity, valid_to, checked "
           "FROM list_entries "
           "WHERE name = ?1 AND merchant_name = ?2 AND ABS(price - ?3) < 0.005 "
           "LIMIT 1");
    if (!s) return found;

    s.bind(1, name);
    s.bind(2, merchantName);
    s.bind(3, price);

    // ABS(...) rather than "=": prices are stored as doubles, and 1.99 read
    // back from two different paths is not always the same bit pattern.
    if (s.step())
    {
        found.id           = s.i64(0);
        found.name         = s.text(1);
        found.merchantName = s.text(2);
        found.price        = s.real(3);
        found.priceText    = s.text(4);
        found.quantity     = s.i32(5);
        found.validTo      = s.text(6);
        found.checked      = s.i32(7) != 0;
    }

    return found;
}

long long Database::addListEntry(const model::ListEntry& entry)
{
    Stmt s(handle_,
           "INSERT INTO list_entries(name, merchant_name, price, price_text, quantity, valid_to, checked, added_at) "
           "VALUES(?1, ?2, ?3, ?4, ?5, ?6, 0, ?7)");
    if (!s) return 0;

    s.bind(1, entry.name);
    s.bind(2, entry.merchantName);
    s.bind(3, entry.price);
    s.bind(4, entry.priceText);
    s.bind(5, entry.quantity);
    s.bind(6, entry.validTo);
    s.bind(7, today());
    s.run();

    return sqlite3_last_insert_rowid(handle_);
}

void Database::removeListEntry(long long id)
{
    Stmt s(handle_, "DELETE FROM list_entries WHERE id = ?1");
    if (!s) return;

    s.bind(1, id);
    s.run();
}

int Database::removeExpiredListEntries()
{
    // Counted before deleting: sqlite3_changes would do, but counting first
    // keeps the number right even if a future version deletes in several steps.
    int count = 0;

    Stmt tally(handle_,
               "SELECT COUNT(*) FROM list_entries "
               "WHERE valid_to IS NOT NULL AND valid_to <> '' AND valid_to < ?1");
    if (tally)
    {
        tally.bind(1, today());
        if (tally.step())
            count = tally.i32(0);
    }

    if (count == 0)
        return 0;

    Stmt s(handle_,
           "DELETE FROM list_entries "
           "WHERE valid_to IS NOT NULL AND valid_to <> '' AND valid_to < ?1");
    if (!s) return 0;

    s.bind(1, today());
    s.run();

    return count;
}

void Database::clearList()
{
    exec("DELETE FROM list_entries");
}

void Database::setListQuantity(long long id, int quantity)
{
    Stmt s(handle_, "UPDATE list_entries SET quantity = ?2 WHERE id = ?1");
    if (!s) return;

    s.bind(1, id);
    s.bind(2, quantity < 1 ? 1 : quantity);
    s.run();
}

void Database::setListChecked(long long id, bool checked)
{
    Stmt s(handle_, "UPDATE list_entries SET checked = ?2 WHERE id = ?1");
    if (!s) return;

    s.bind(1, id);
    s.bind(2, checked ? 1 : 0);
    s.run();
}

std::vector<model::ListEntry> Database::listEntries() const
{
    std::vector<model::ListEntry> out;

    // Grouped by store, because that is how the list gets walked in the aisles.
    Stmt s(handle_,
           "SELECT id, name, merchant_name, price, price_text, quantity, valid_to, checked "
           "FROM list_entries ORDER BY merchant_name COLLATE NOCASE, name COLLATE NOCASE");
    if (!s) return out;

    while (s.step())
    {
        model::ListEntry e;
        e.id           = s.i64(0);
        e.name         = s.text(1);
        e.merchantName = s.text(2);
        e.price        = s.real(3);
        e.priceText    = s.text(4);
        e.quantity     = s.i32(5);
        e.validTo      = s.text(6);
        e.checked      = s.i32(7) != 0;
        out.push_back(std::move(e));
    }
    return out;
}

//==============================================================================
void Database::rebuildSearchIndex()
{
    struct Row { long long id; std::string text; };
    std::vector<Row> rows;

    {
        Stmt read(handle_, "SELECT id, name, description FROM items");
        if (!read) return;

        while (read.step())
            rows.push_back({ read.i64(0), read.text(1) + " " + read.text(2) });
    }

    exec("BEGIN");
    for (const Row& r : rows)
    {
        Stmt write(handle_, "UPDATE items SET name_norm = ?2, name_tokens = ?3 WHERE id = ?1");
        if (!write) break;

        write.bind(1, r.id);
        write.bind(2, text::normalize(r.text));
        write.bind(3, paddedTokens(r.text));
        write.run();
    }
    exec("COMMIT");
}

void Database::purgeExpired(int keepDays)
{
    const std::string cutoff = isoDate(-keepDays);

    Stmt items(handle_, "DELETE FROM items WHERE valid_to IS NOT NULL AND valid_to < ?1");
    if (items) { items.bind(1, cutoff); items.run(); }

    Stmt flyers(handle_, "DELETE FROM flyers WHERE valid_to IS NOT NULL AND valid_to < ?1");
    if (flyers) { flyers.bind(1, cutoff); flyers.run(); }

    exec("DELETE FROM flyer_zones WHERE flyer_id NOT IN (SELECT id FROM flyers)");
}

} // namespace db
