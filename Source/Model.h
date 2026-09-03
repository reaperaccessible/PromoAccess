#pragma once

#include <string>
#include <vector>

//==============================================================================
// Plain data the whole application agrees on. No wxWidgets, no SQLite, no HTTP:
// the source adapters fill these, the database stores them, the UI reads them.
//
// Every string is UTF-8. Conversion to wxString happens once, at the UI
// boundary, with wxString::FromUTF8 — never implicitly, which is what turns
// accented French into mojibake.
//==============================================================================
namespace model
{

// A retailer whose flyers the user can follow ("IGA", "Super C", "Maxi"...).
struct Merchant
{
    int         id = 0;         // source-assigned identifier
    std::string name;
    bool        followed = false;
};

// One weekly flyer for one merchant.
struct Flyer
{
    long long   id = 0;
    int         merchantId = 0;
    std::string merchantName;
    std::string name;           // the flyer's own title, often empty
    std::string validFrom;      // ISO-8601 date, "2026-08-27"
    std::string validTo;
    int         itemCount = 0;  // filled once the items are cached
};

// One advertised product inside a flyer. The unit of everything the user
// actually cares about.
struct Item
{
    long long   id = 0;
    long long   flyerId = 0;
    int         merchantId = 0;
    std::string merchantName;

    std::string name;           // "Hauts de cuisse ou pilons de poulet Olymel"
    std::string description;    // size/format, sometimes the other language
    std::string sku;
    std::string brand;          // "Selection", "Irresistible" — half the feeds fill it

    // Advertised discount, in percent. -1 means the banner did not say, which is
    // NOT the same as zero: about half the items carry no percentage at all, and
    // sorting them as if they were full price would bury real bargains.
    int         discountPercent = -1;

    // A price of 0 means "not advertised as a number" — plenty of flyer items
    // are "2 for 5$" or "50% off" and carry only the text below. The UI must
    // never print "0.00 $"; it prints priceText instead.
    double      currentPrice = 0.0;
    double      originalPrice = 0.0;
    std::string priceText;      // free-form price line when there is no number
    std::string saleStory;      // "2 pour 5$", "Achetez-en 2"
    bool        inStoreOnly = false;

    std::string validFrom;
    std::string validTo;

    // The merchant's own page for this product, cleaned of the advertising
    // redirector some banners wrap it in. Empty when the feed offers none.
    std::string productUrl;

    // True once the per-item endpoint has been asked about this item. It is what
    // separates "no original price" from "never looked" — without it the app
    // would re-ask the server forever about items that simply have none.
    bool detailFetched = false;

    // Which generation of the detail parser filled this row. When the parser
    // learns to read a new field, rows written by an older one are re-asked once
    // — WITHOUT clearing detailFetched, which would make every item in a warm
    // cache report itself as incomplete and send Ctrl+D back to the network for
    // data already in SQLite.
    int detailRevision = 0;
};

// A standing watch, not a pinned product. Flyer items get new identifiers every
// week, so a favourite that pointed at one would be dead in seven days; a
// favourite is therefore a matching rule re-evaluated at every sync.
struct Favorite
{
    long long   id = 0;
    std::string pattern;        // words to match, all of them, case/accent-insensitive
    // The banners the watch applies to. Empty = every followed banner.
    //
    // A list rather than a single identifier because one product is worth
    // watching at several stores at once: "cafe" is a deal at Maxi one week and
    // at Super C the next, and forcing one favourite per banner would mean
    // editing four rules whenever the wording changes.
    std::vector<int> merchantIds;
    std::string merchantName;   // cached for display, comma separated
    double      maxPrice = 0.0; // 0 = no price condition
    bool        enabled = true;

    // Match whole words rather than any run of letters.
    //
    // Substring matching is what a search box wants and what a standing watch
    // does not: "ail" as a fragment also matches "AILES DE POULET", and a
    // favourite quietly collects the wrong product week after week. On by
    // default for that reason; turn it off to catch "fromag" in "fromages".
    bool        wholeWords = true;
};

// One line of the shopping list being built.
struct ListEntry
{
    long long   id = 0;
    std::string name;
    std::string merchantName;
    double      price = 0.0;
    std::string priceText;
    int         quantity = 1;
    std::string validTo;
    bool        checked = false;
};

} // namespace model
