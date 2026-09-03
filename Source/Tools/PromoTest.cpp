// Regression harness for the PromoAccess core: source adapter + cache, no UI.
//
// Usage:
//   promotest                       offline checks only — deterministic, no network
//   promotest <postal code>         offline checks, then a live sync and search
//   promotest <postal code> <query> also runs a live cross-merchant search
//
// Exit code is the number of failed checks, so this is usable from a script.
//
// It writes to a cache of its OWN, never to %APPDATA%\PromoAccess\cache.db: an
// earlier version synced into the real one, rewrote the user's followed banners
// and called clearList() on their shopping list.

#include "Database.h"
#include "FlippSource.h"
#include "Paths.h"
#include "Text.h"
#include "Export.h"
#include "Postal.h"
#include "Format.h"
#include "Http.h"
#include "Updater.h"
#include "Version.h"

#include "sqlite3.h"

#include <wx/file.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/init.h>
#include <wx/string.h>

#include <cstdio>
#include <string>
#include <vector>

namespace
{
    int failures = 0;

    void line() { std::printf("--------------------------------------------------\n"); }

    // The only way a check is recorded. An earlier harness printed "OK" from an
    // unconditional printf next to the value it was supposed to be judging, so a
    // search returning zero hits — the exact shape of three real regressions —
    // reported success and exited 0.
    bool check(bool condition, const char* what)
    {
        std::printf("%s  %s\n", condition ? "OK  " : "FAIL", what);
        if (!condition)
            ++failures;
        return condition;
    }

    void checkEqual(const std::string& got, const std::string& expected, const char* what)
    {
        const bool ok = (got == expected);
        std::printf("%s  %s\n", ok ? "OK  " : "FAIL", what);
        if (!ok)
        {
            std::printf("        expected \"%s\"\n        got      \"%s\"\n",
                        expected.c_str(), got.c_str());
            ++failures;
        }
    }

    // Prices are printed the way the UI speaks them: a number when there is one,
    // the advertised text when there is not, never a bogus "0.00".
    std::string priceOf(const model::Item& i)
    {
        char buf[64];
        if (i.currentPrice > 0.0)
        {
            std::snprintf(buf, sizeof(buf), "%.2f", i.currentPrice);
            std::string s = buf;
            if (!i.priceText.empty())
                s += " " + i.priceText;
            return s;
        }
        if (!i.saleStory.empty()) return i.saleStory;
        if (!i.priceText.empty()) return i.priceText;
        return "-";
    }

    wxString testDatabasePath()
    {
        return paths::dataFolder() + wxFileName::GetPathSeparator() + "promotest.db";
    }
}

//==============================================================================
// Offline checks. Deterministic, no network, and every one of them stands for a
// defect that actually shipped.
//==============================================================================
static void runOfflineChecks()
{
    std::printf("=== offline ===\n");

    // --- Accent folding ------------------------------------------------------
    // Lower case has always worked. Upper case did not: normalize() lower-cased
    // before folding, and towlower in the "C" locale leaves non-ASCII untouched,
    // so "CÉRÉALES" was indexed as "cÉrÉales" and no accentless search could
    // reach it. 37% of a real cache was affected.
    checkEqual(text::normalize("CÉRÉALES"), "cereales", "normalize folds ACCENTED CAPITALS");
    checkEqual(text::normalize("céréales"), "cereales", "normalize folds accented lower case");
    checkEqual(text::normalize("BŒUF HACHÉ"), "boeuf hache", "normalize folds the OE ligature in capitals");
    checkEqual(text::normalize("Bœuf haché"), "boeuf hache", "normalize folds the oe ligature");
    checkEqual(text::normalize("Côte-Nord, Îles"), "cote-nord, iles", "normalize folds circumflex and diaeresis");

    // The two must agree, or a favourite typed in one case misses the other.
    check(text::normalize("CRÈME GLACÉE") == text::normalize("crème glacée"),
          "normalize is case-insensitive across accents");

    // --- Tokenizing ----------------------------------------------------------
    {
        const std::vector<std::string> t = text::tokens("Poulet/Dinde, 50% (frais)");
        check(t.size() == 4, "tokens splits on punctuation");

        // LIKE wildcards must not survive into a pattern, or a search for "50%"
        // silently matches everything.
        bool wildcard = false;
        for (const std::string& one : t)
            if (one.find('%') != std::string::npos || one.find('_') != std::string::npos)
                wildcard = true;
        check(!wildcard, "tokens strips the LIKE wildcards % and _");
    }

    // --- Names that SHOUT ------------------------------------------------------
    // Three banners in six write everything in capitals. Only those names are
    // recased; one written in mixed case was written that way on purpose.
    {
        // wxString::FromUTF8 on every accented input. A wxString built straight
        // from a literal reads those bytes in the machine's ANSI code page, and
        // the test would be measuring mojibake rather than the casing rule.
        auto cased = [](const char* utf8)
        {
            return fmt::properCase(wxString::FromUTF8(utf8)).utf8_string();
        };

        checkEqual(cased("BACON TRANCHÉ MÈRE MICHEL"),
                   "Bacon Tranché Mère Michel", "capitals come down, accents included");
        checkEqual(cased("CREVETTES PANÉES EN PAPILLON"),
                   "Crevettes Panées En Papillon", "and every word gets its capital");
        // An apostrophe and a hyphen keep the word going: French elides, and
        // "C'Est Prêt" is not something anyone writes.
        checkEqual(cased("C'EST PRÊT!"),
                   "C'est Prêt!", "an apostrophe does not start a new word");
        checkEqual(cased("PRÊT-À-MANGER"),
                   "Prêt-à-manger", "and neither does a hyphen");
        checkEqual(cased("SODA ZEVIA, 6X355 ML"),
                   "Soda Zevia, 6X355 Ml", "a letter after a digit is not a new word");

        // Left alone: the banner said something with those capitals.
        checkEqual(cased("biscuits Célébration Leclerc"),
                   "biscuits Célébration Leclerc", "a name with lower case is left alone");
        checkEqual(cased("125 g"), "125 g", "and so is one already lower");
        checkEqual(cased(""), "", "an empty name survives");
    }

    // --- Postal codes --------------------------------------------------------
    checkEqual(postal::canonical("j3p7s7"), "J3P7S7", "postal accepts lower case");
    checkEqual(postal::canonical(" J3P7S7 "), "J3P7S7", "postal tolerates surrounding blanks");
    check(!postal::isValid("J3P 7S7"), "postal refuses an inner space");
    check(!postal::isValid("J3P-7S7"), "postal refuses a hyphen");
    check(!postal::isValid("3JP7S7"),  "postal refuses letter/digit inversion");
    check(!postal::isValid("J3P7S7X"), "postal refuses a seventh character");
    check(!postal::isValid(""),        "postal refuses an empty string");

    // --- Bilingual names -----------------------------------------------------
    // The feed carries one string, "français | english", whatever locale is
    // asked for.
    checkEqual(fmt::itemName("boeuf haché | ground beef").utf8_string(),
               "boeuf haché", "itemName keeps one language");

    // The size is sometimes written only on the English side; dropping it would
    // leave a price with no format.
    checkEqual(fmt::itemName("FRAMBOISES | RASPBERRIES, 170 G").utf8_string(),
               "Framboises, 170 G", "itemName rescues a size left on the other side");

    checkEqual(fmt::itemName("jus de pomme Selection").utf8_string(),
               "jus de pomme Selection", "itemName leaves a single-language name alone");

    // --- Savings -------------------------------------------------------------
    {
        model::Item item;
        item.currentPrice = 4.99;
        item.originalPrice = 6.99;
        check(!fmt::savings(item).empty(), "savings reports a real discount");

        item.originalPrice = 3.99;      // lower than the current price
        check(fmt::savings(item).empty(), "savings ignores an original below the current price");

        item.originalPrice = 0.0;
        check(fmt::savings(item).empty(), "savings is silent with no original price");
    }

    // --- Product links -------------------------------------------------------
    // Two of the four banners wrap their product page in a DoubleClick
    // click-through, and Super C wraps it twice. Opening the raw string would
    // register a click on behalf of a user who only asked to see a product.
    {
        const std::string metro =
            "https://ad.doubleclick.net/ddm/trackclk/N1841780.5269158FLIPP-REEBEE/"
            "B30581383.391396968;dc_trk_aid=597571302;dc_trk_cid=223438268;dc_lat=;"
            "dc_rdid=;tag_for_child_directed_treatment=;tfua=;ltd=;dc_tdv=1"
            "?https://www.metro.ca/epicerie-en-ligne/Allees/p/059749968966";

        checkEqual(source::stripTracking(metro),
                   "https://www.metro.ca/epicerie-en-ligne/Allees/p/059749968966",
                   "stripTracking unwraps a DoubleClick click-through");

        const std::string superc =
            "https://ad.doubleclick.net/ddm/trackclk/A/B;dc_tdv=1"
            "?https://ad.doubleclick.net/ddm/trackclk/A/B;dc_tdv=1"
            "?https://www.superc.ca/Allees/p/059072055296";

        checkEqual(source::stripTracking(superc),
                   "https://www.superc.ca/Allees/p/059072055296",
                   "stripTracking unwraps the doubly nested case");

        // Direct links must come back untouched.
        checkEqual(source::stripTracking("https://commerce.iga.net/fr/produit/00000_123"),
                   "https://commerce.iga.net/fr/produit/00000_123",
                   "stripTracking leaves a direct IGA link alone");
        checkEqual(source::stripTracking("https://www.maxi.ca/p/20972254001_EA"),
                   "https://www.maxi.ca/p/20972254001_EA",
                   "stripTracking leaves a direct Maxi link alone");

        // Failure must return NOTHING, never the tracker. An earlier draft
        // returned the original on failure, which meant the one function written
        // to avoid the click-through would open it in every case it could not
        // parse — and a test asserting that would have enshrined the leak.
        check(source::stripTracking(
                  "https://ad.doubleclick.net/ddm/trackclk/A/B;dc_tdv=1").empty(),
              "stripTracking refuses a tracker with no destination behind it");

        check(source::stripTracking(
                  "https://ad.doubleclick.net/ddm/trackclk/A/B;dc_tdv=1?not-a-url").empty(),
              "stripTracking refuses a destination that is not a URL");

        checkEqual(source::stripTracking(
                       "https://ad.doubleclick.net/ddm/trackclk/A/B;dc_tdv=1"
                       "?https%3A%2F%2Fwww.metro.ca%2Fp%2F123"),
                   "https://www.metro.ca/p/123",
                   "stripTracking decodes a percent-encoded destination");

        checkEqual(source::stripTracking(""), "", "stripTracking handles an empty link");

        // What is eventually handed to the shell.
        check(http::isSafeUrl("https://www.metro.ca/p/123"), "isSafeUrl accepts a plain https link");
        check(!http::isSafeUrl("file:///C:/Windows/System32/cmd.exe"), "isSafeUrl refuses a local file");
        check(!http::isSafeUrl("C:\\Windows\\System32\\cmd.exe"), "isSafeUrl refuses a local path");
        check(!http::isSafeUrl("https://user:pass@evil.example/x"), "isSafeUrl refuses embedded credentials");
        check(!http::isSafeUrl("https://localhost"), "isSafeUrl refuses a host with no dot");
    }

    // --- Opening a cache written by an older version --------------------------
    // The schema string creates indexes over columns the migrations add. Running
    // it before them aborted the open on every existing cache with "no such
    // column: name_tokens", so the user kept a cache that could never be
    // upgraded — and the harness, which always starts from a fresh file, saw
    // nothing. This builds an old-shaped cache on purpose.
    {
        const wxString old = paths::dataFolder()
                           + wxFileName::GetPathSeparator() + "promotest-old.db";
        wxRemoveFile(old);

        sqlite3* raw = nullptr;
        if (sqlite3_open_v2(old.utf8_string().c_str(), &raw,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) == SQLITE_OK)
        {
            // The shape shipped before whole-word matching: no name_tokens on
            // items, no whole_words on favorites.
            sqlite3_exec(raw,
                "CREATE TABLE items(id INTEGER PRIMARY KEY, flyer_id INTEGER,"
                " merchant_id INTEGER, merchant_name TEXT, name TEXT NOT NULL,"
                " name_norm TEXT NOT NULL, valid_to TEXT);"
                "CREATE TABLE favorites(id INTEGER PRIMARY KEY, pattern TEXT NOT NULL,"
                " merchant_id INTEGER, merchant_name TEXT, max_price REAL, enabled INTEGER);",
                nullptr, nullptr, nullptr);
            sqlite3_close(raw);

            db::Database upgraded;
            std::string upgradeError;
            check(upgraded.open(old, upgradeError),
                  "a cache from an older version still opens");
            if (!upgradeError.empty())
                std::printf("      %s\n", upgradeError.c_str());

            // A watch over several banners must survive the round trip, and one
            // written before merchant_ids existed must keep its single banner.
            model::Favorite two;
            two.pattern     = "cafe";
            two.merchantIds = { 1201, 2049 };
            two.wholeWords  = false;
            upgraded.addFavorite(two);

            const std::vector<model::Favorite> back = upgraded.favorites();
            check(back.size() == 1 && back[0].merchantIds.size() == 2,
                  "a favorite keeps every banner it was given");
            check(!back.empty() && !back[0].wholeWords,
                  "a favorite keeps its matching mode");

            model::Favorite all = back.empty() ? model::Favorite() : back[0];
            all.merchantIds.clear();
            upgraded.updateFavorite(all);
            check(upgraded.favorites().size() == 1
                      && upgraded.favorites()[0].merchantIds.empty(),
                  "clearing the banners means every banner, not the last one");

            upgraded.close();
        }
        else
        {
            check(false, "a cache from an older version still opens");
        }

        wxRemoveFile(old);
    }

    // --- Version comparison ---------------------------------------------------
    // The two-digit minor scheme is the trap: "1.10" must beat "1.9", and a
    // string compare would say the opposite.
    check(updater::isNewer("1.01", "1.00"),  "1.01 is newer than 1.00");
    check(updater::isNewer("1.10", "1.09"),  "1.10 is newer than 1.09");
    check(updater::isNewer("1.10", "1.9"),   "1.10 is newer than 1.9");
    check(updater::isNewer("2.00", "1.99"),  "2.00 is newer than 1.99");
    check(!updater::isNewer("1.00", "1.00"), "the same version is not newer");
    check(!updater::isNewer("1.00", "1.01"), "an older version is not newer");
    check(!updater::isNewer("1.00", "1"),    "1.00 and 1 are the same version");
    check(updater::isNewer("v1.01", "1.00"), "a leading v is ignored");
    check(!updater::isNewer("", "1.00"),     "an empty tag is never newer");

    // --- Export --------------------------------------------------------------
    {
        std::vector<model::ListEntry> entries;

        model::ListEntry e;
        e.name = "bifteck, format familial";   // the comma is the point
        e.merchantName = "Metro";
        e.price = 12.99;
        e.quantity = 2;
        e.validTo = "2026-09-09";
        entries.push_back(e);

        const wxString path = paths::dataFolder() + wxFileName::GetPathSeparator() + "promotest.csv";

        wxString error;
        if (check(exporter::write(path, exporter::Format::Csv, entries, error),
                  "export writes a CSV"))
        {
            wxFile file;
            wxString content;
            if (file.Open(path))
            {
                wxString text;
                file.ReadAll(&text);
                content = text;
            }

            check(content.Contains("\"bifteck, format familial\""),
                  "CSV quotes a field containing a comma");
            check(content.StartsWith("\xEF\xBB\xBF") || content.Contains("Metro"),
                  "CSV carries its content");
        }

        wxRemoveFile(path);
    }

    line();
}

//==============================================================================
int main(int argc, char** argv)
{
    // wxUILocale and wxString need the library initialized even without a GUI.
    wxInitializer init;
    if (!init.IsOk())
    {
        std::printf("wxWidgets failed to initialize\n");
        return 1;
    }

    runOfflineChecks();

    if (argc <= 1)
    {
        std::printf("%d failed check(s). Pass a postal code to run the online checks too.\n",
                    failures);
        return failures;
    }

    // Canonical from the first line, like the application.
    const std::string typed  = argv[1];
    const std::string postalCode = postal::isValid(typed) ? postal::canonical(typed) : typed;
    const std::string query  = (argc > 2) ? argv[2] : "";

    std::printf("=== online — postal code %s ===\n", postalCode.c_str());

    // --- Cache ----------------------------------------------------------------
    // A cache of the harness's own. Deleted first, so every run starts from a
    // known state and the checks below can assert on absolute numbers.
    const wxString path = testDatabasePath();
    wxRemoveFile(path);

    db::Database database;
    std::string error;
    if (!check(database.open(path, error), "cache opens"))
    {
        std::printf("      %s\n", error.c_str());
        return failures;
    }
    std::printf("      %s\n", path.utf8_string().c_str());

    database.setScope({ postalCode });

    // --- Flyers ---------------------------------------------------------------
    source::FlippSource feed;
    std::vector<model::Merchant> merchants;
    std::vector<model::Flyer>    flyers;

    source::Result r = feed.fetchFlyers(postalCode, merchants, flyers, {});
    if (!check(r.ok, "fetchFlyers succeeds"))
    {
        std::printf("      %s\n", r.error.c_str());
        return failures;
    }
    check(!merchants.empty(), "fetchFlyers returns merchants");
    check(!flyers.empty(),    "fetchFlyers returns flyers");
    std::printf("      %zu merchants, %zu flyers\n", merchants.size(), flyers.size());

    database.upsertMerchants(merchants);
    database.upsertFlyers(flyers, postalCode);

    static const char* kGrocers[] = { "IGA", "Metro", "Super C", "Maxi", "Provigo" };
    int followed = 0;
    for (const model::Merchant& m : merchants)
        for (const char* g : kGrocers)
            if (m.name == g)
            {
                database.setFollowed(m.id, true);
                ++followed;
            }
    check(followed > 0, "at least one grocery banner is available here");

    // --- Items ----------------------------------------------------------------
    const std::vector<model::Flyer> mine = database.flyers();
    check(!mine.empty(), "the followed banners have current flyers");

    int syncedItems = 0;
    for (const model::Flyer& f : mine)
    {
        std::vector<model::Item> items;
        if (!feed.fetchItems(f, postalCode, items, {}).ok)
            continue;

        database.upsertItems(items);
        syncedItems += static_cast<int>(items.size());
    }
    check(syncedItems > 0, "the sync brought back items");
    std::printf("      %d items over %zu flyers\n", syncedItems, mine.size());

    // --- Brand and discount ---------------------------------------------------
    // Both come from the flyer listing, so a plain sync must fill them without
    // any per-item fetch. If that ever regresses, the sort silently reports
    // every item as "the banner did not say".
    {
        int withBrand = 0, withDiscount = 0, checked = 0;
        for (const model::Flyer& f : database.flyers(0, true))
            for (const model::Item& i : database.itemsOfFlyer(f.id))
            {
                ++checked;
                if (!i.brand.empty())       ++withBrand;
                if (i.discountPercent > 0)  ++withDiscount;
            }

        std::printf("      %d items: %d with a brand, %d with a percentage\n",
                    checked, withBrand, withDiscount);
        check(checked == 0 || withDiscount > 0,
              "a sync stores the advertised discount percentage");
        check(checked == 0 || withBrand > 0,
              "a sync stores the brand");

        // -1 and 0 must stay distinguishable: "not said" is not "no discount".
        int zero = 0;
        for (const model::Flyer& f : database.flyers(0, true))
            for (const model::Item& i : database.itemsOfFlyer(f.id))
                if (i.discountPercent == 0) ++zero;

        check(zero == 0, "an unstated discount is stored as -1, never as 0");
    }

    // --- A line of the list is worth what it contributes -----------------------
    // The row used to show the unit price while contributing the multiple to the
    // total, so the total read as wrong to anyone adding the rows up by ear.
    {
        model::ListEntry e;
        e.name     = "CREVETTES";
        e.price    = 6.99;
        e.quantity = 2;

        const wxString line = fmt::lineTotal(e);
        check(line.Contains(fmt::money(13.98)), "a line of two shows what the two cost");
        check(line.Contains("2 x"),             "and shows the arithmetic behind it");

        e.quantity = 1;
        check(fmt::lineTotal(e) == fmt::price(e), "a single unit reads as its price");

        // No number advertised: nothing to multiply, and nothing invented.
        e.price     = 0.0;
        e.priceText = "2 pour 5$";
        e.quantity  = 3;
        check(fmt::lineTotal(e) == fmt::price(e), "an item with no number is left alone");
    }

    // --- The shopping list merges a repeat ------------------------------------
    // Adding the same product twice must raise the line it is already on. It
    // used to make a second identical row, which reads as a duplicate to
    // anyone walking the list with a screen reader.
    {
        database.clearList();

        model::ListEntry e;
        e.name         = "CAFE SELECTION";
        e.merchantName = "Super C";
        e.price        = 6.99;
        e.quantity     = 1;
        database.addListEntry(e);

        const model::ListEntry again =
            database.findListEntry(e.name, e.merchantName, e.price);
        check(again.id != 0, "the same product is found on the list");
        check(again.quantity == 1, "and it is found with its quantity");

        database.setListQuantity(again.id, again.quantity + 2);
        check(database.listEntries().size() == 1,
              "a repeat raises the line instead of making a second one");
        check(database.listEntries()[0].quantity == 3,
              "and the quantity is the sum");

        // Same name and banner, different price: a banner advertises one
        // product twice a week at two prices, and those are two decisions.
        check(database.findListEntry(e.name, e.merchantName, 4.99).id == 0,
              "a different price is a different line");
        check(database.findListEntry(e.name, "Metro", e.price).id == 0,
              "a different banner is a different line");

        database.clearList();
    }

    // --- Search ---------------------------------------------------------------
    // Non-empty results are asserted, not merely printed. A silently failing
    // statement — the ambiguous-column bug, a missing migration — shows up here
    // as zero hits, which used to print "OK".
    for (const char* pattern : { "poulet", "fromage" })
    {
        const std::vector<model::Item> hits = database.searchItems(pattern);
        check(!hits.empty(), (std::string("cache search finds \"") + pattern + "\"").c_str());

        for (size_t n = 0; n < hits.size() && n < 2; ++n)
            std::printf("      %-12s %-8s %s\n",
                        hits[n].merchantName.c_str(),
                        priceOf(hits[n]).c_str(),
                        hits[n].name.c_str());
    }

    // The accentless pattern must reach the ACCENTED CAPITALS the banners use.
    {
        const std::vector<model::Item> plain = database.searchItems("cereales");
        int accentedCaps = 0;
        for (const model::Item& i : plain)
            if (i.name.find("\xC3\x89") != std::string::npos)   // É in UTF-8
                ++accentedCaps;

        check(plain.empty() || accentedCaps > 0,
              "an accentless search reaches names written in ACCENTED CAPITALS");
        std::printf("      \"cereales\": %zu hits, %d of them ALL-CAPS accented\n",
                    plain.size(), accentedCaps);
    }

    // --- Week filter ----------------------------------------------------------
    {
        database.setWeek(db::Week::Both);
        const size_t both = database.flyers().size();

        database.setWeek(db::Week::Current);
        const size_t current = database.flyers().size();

        database.setWeek(db::Week::Next);
        const size_t next = database.flyers().size();

        database.setWeek(db::Week::Both);

        check(current + next == both, "the two weeks partition the whole set");
        std::printf("      current %zu + next %zu = both %zu\n", current, next, both);
    }

    // --- A detail fetch must survive the next sync ----------------------------
    // price_text and sale_story used to be overwritten with '' by every sync,
    // while detail_fetched stayed 1 — so the item was never asked about again and
    // silently lost its unit price.
    {
        const std::vector<model::Item> some = database.searchItems("poulet");
        if (!some.empty())
        {
            model::Item detailed = some.front();
            if (feed.fetchItemDetail(detailed.id, postalCode, detailed).ok)
            {
                database.updateItemDetail(detailed);

                // Re-run the flyer upsert for that item's flyer, as a sync would.
                for (const model::Flyer& f : mine)
                {
                    if (f.id != detailed.flyerId)
                        continue;

                    std::vector<model::Item> again;
                    if (feed.fetchItems(f, postalCode, again, {}).ok)
                        database.upsertItems(again);
                }

                for (const model::Item& after : database.searchItems("poulet"))
                {
                    if (after.id != detailed.id)
                        continue;

                    check(detailed.description.empty() || !after.description.empty(),
                          "a sync preserves the format stored by a detail fetch");
                    check(detailed.saleStory.empty() || !after.saleStory.empty(),
                          "a sync preserves the sale story stored by a detail fetch");
                    check(detailed.productUrl.empty() || !after.productUrl.empty(),
                          "a sync preserves the product link stored by a detail fetch");
                    check(after.detailRevision == db::Database::detailRevision(),
                          "a detail fetch stamps the parser generation");
                }
            }
        }
    }

    // --- Update check ---------------------------------------------------------
    // The real GitHub call, with the real parser. It must reach the service,
    // read a version out of the newest release and find a Windows installer to
    // download; the shape of that reply is not ours to control.
    {
        const updater::Info info = updater::check();

        check(info.error.empty(), "the update check reaches GitHub");
        if (!info.error.empty())
            std::printf("      %s\n", info.error.utf8_string().c_str());

        check(!info.latestVersion.empty(), "the newest release names a version");
        check(!info.installerUrl.empty(),  "the newest release carries an installer");
        std::printf("      published %s, running %s, update offered: %s\n",
                    info.latestVersion.c_str(), PROMO_VERSION_STR,
                    info.available ? "yes" : "no");
    }

    // --- Live search ----------------------------------------------------------
    if (!query.empty())
    {
        std::vector<model::Item> found;
        r = feed.search(query, postalCode, found, {});
        check(r.ok, "live search succeeds");

        std::printf("      \"%s\": %zu items\n", query.c_str(), found.size());
        for (size_t n = 0; n < found.size() && n < 5; ++n)
            std::printf("      %-16s %-10s %s\n",
                        found[n].merchantName.c_str(),
                        priceOf(found[n]).c_str(),
                        found[n].name.c_str());
    }

    line();
    std::printf("%d failed check(s)\n", failures);
    return failures;
}
