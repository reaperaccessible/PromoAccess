#include "FlippSource.h"
#include "Http.h"
#include "Localization.h"
#include "Postal.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

using json = nlohmann::json;

namespace source
{
namespace
{
    const char* kBase = "https://backflipp.wishabi.com/flipp";

    // Percent-encodes everything outside the unreserved set. Postal codes are
    // tame, search terms are not: an accented or spaced query must not be able
    // to produce a malformed request line.
    std::string urlEncode(const std::string& in)
    {
        static const char* hex = "0123456789ABCDEF";
        std::string out;
        out.reserve(in.size() * 3);

        for (unsigned char c : in)
        {
            const bool unreserved = std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
            if (unreserved)
                out += static_cast<char>(c);
            else
            {
                out += '%';
                out += hex[c >> 4];
                out += hex[c & 0x0F];
            }
        }
        return out;
    }

    // Postal codes travel without their space and upper-cased ("h2x 1y4" ->
    // "H2X1Y4"), which is the only form the endpoint answers reliably. Shared
    // with the cache so a flyer is filed under the same key it was fetched with.
    // A code that fails validation is still sent, stripped: letting the server
    // answer "nothing here" beats silently sending no postal code at all.
    std::string normalizePostal(const std::string& in)
    {
        const std::string canonical = postal::canonical(in);
        if (!canonical.empty())
            return canonical;

        std::string out;
        for (unsigned char c : in)
            if (std::isalnum(c))
                out += static_cast<char>(std::toupper(c));
        return out;
    }

    // The locale drives the language of the item names themselves, so it follows
    // the UI language rather than being hard-coded to French: an English user
    // gets "Fresh chicken thighs", a French one "Hauts de cuisse de poulet".
    const char* locale()
    {
        return loc::isFrench() ? "fr-ca" : "en-ca";
    }

    // --- Defensive JSON readers ----------------------------------------------
    // Every field this API returns can be null, absent, or arrive as the other
    // JSON type between one week and the next. None of that may abort a sync.

    std::string str(const json& j, const char* key)
    {
        auto it = j.find(key);
        if (it == j.end() || it->is_null())
            return {};
        if (it->is_string())
            return it->get<std::string>();
        if (it->is_number_integer())
            return std::to_string(it->get<long long>());
        return {};
    }

    long long num(const json& j, const char* key)
    {
        auto it = j.find(key);
        if (it == j.end() || it->is_null())
            return 0;
        if (it->is_number())
            return it->get<long long>();
        if (it->is_string())
        {
            try { return std::stoll(it->get<std::string>()); } catch (...) { return 0; }
        }
        return 0;
    }

    // Prices arrive as a number in one endpoint and as a string in another.
    double price(const json& j, const char* key)
    {
        auto it = j.find(key);
        if (it == j.end() || it->is_null())
            return 0.0;
        if (it->is_number())
            return it->get<double>();
        if (it->is_string())
        {
            try { return std::stod(it->get<std::string>()); } catch (...) { return 0.0; }
        }
        return 0.0;
    }

    // Defensive like its neighbours: this feed has already been seen to change a
    // field's JSON type between one week and the next, and a flag that arrives as
    // 1 or "true" instead of true must not silently read as false.
    bool boolean(const json& j, const char* key)
    {
        auto it = j.find(key);
        if (it == j.end() || it->is_null())
            return false;

        if (it->is_boolean())        return it->get<bool>();
        if (it->is_number())         return it->get<double>() != 0.0;
        if (it->is_string())
        {
            const std::string s = it->get<std::string>();
            return s == "true" || s == "1" || s == "yes";
        }
        return false;
    }

    // "2026-08-27T00:00:00-04:00" becomes "2026-08-27". The time and the zone
    // are noise: a flyer is valid for whole days.
    std::string date(const json& j, const char* key)
    {
        std::string s = str(j, key);
        return s.size() >= 10 ? s.substr(0, 10) : s;
    }

    // Assembles the human price line for items sold by weight or as a multi-buy,
    // where the number alone would be misleading or missing.
    std::string priceLine(const json& j)
    {
        // Three fields, because the two endpoints disagree on where the unit
        // lives. The search reply writes it in post_price_text; the per-item
        // reply writes it in price_text — "/lb - 9,90$/kg" — and reading only
        // pre and post left a fresh whole chicken announced as 4,49 $ flat,
        // which is not its price, it is its price per pound. The banner said
        // so; the parser dropped it.
        const std::string pre  = str(j, "pre_price_text");
        const std::string mid  = str(j, "price_text");
        const std::string post = str(j, "post_price_text");

        std::string line = pre;

        for (const std::string* part : { &mid, &post })
        {
            if (part->empty() || *part == line || (part == &post && *part == mid))
                continue;

            if (!line.empty()) line += ' ';
            line += *part;
        }

        return line;
    }

    // Substrings that identify a click-through rather than a destination. A
    // denylist, not a whitelist of merchants: the user can follow any of the
    // ninety-odd banners their postal code offers, and a whitelist would refuse
    // every one never measured.
    bool looksLikeTracker(const std::string& url)
    {
        static const char* kMarkers[] =
        {
            "doubleclick.net", "/ddm/trackclk/", "adclick.", "/trackclk/",
            "/click?", "/clk?",
        };

        for (const char* marker : kMarkers)
            if (url.find(marker) != std::string::npos)
                return true;

        return false;
    }

    std::string percentDecode(const std::string& in)
    {
        std::string out;
        out.reserve(in.size());

        for (size_t n = 0; n < in.size(); ++n)
        {
            if (in[n] == '%' && n + 2 < in.size()
                && std::isxdigit(static_cast<unsigned char>(in[n + 1]))
                && std::isxdigit(static_cast<unsigned char>(in[n + 2])))
            {
                out += static_cast<char>(std::stoi(in.substr(n + 1, 2), nullptr, 16));
                n += 2;
            }
            else
            {
                out += in[n];
            }
        }
        return out;
    }

    Result getJson(const std::string& url, json& out, http::Canceller* canceller)
    {
        std::string body;
        const http::GetResult r = http::getToString(wxString::FromUTF8(url), body, canceller);
        if (!r.ok)
            return Result::failure(r.error.utf8_string());

        out = json::parse(body, nullptr, /*allow_exceptions=*/false);
        if (out.is_discarded())
            return Result::failure("Malformed JSON in response");

        return Result::success();
    }

    bool cancelled(const CancelFn& fn) { return fn && fn(); }
}

std::string stripTracking(const std::string& url)
{
    std::string current;
    for (const unsigned char c : url)          // trim, without pulling in more headers
        if (!std::isspace(c) || !current.empty())
            current += static_cast<char>(c);
    while (!current.empty() && std::isspace(static_cast<unsigned char>(current.back())))
        current.pop_back();

    if (current.empty())
        return {};

    // Bounded: Super C nests the same redirector twice, and a longer chain is a
    // shape we have never seen and would not want to follow blindly.
    for (int hop = 0; hop < 3 && looksLikeTracker(current); ++hop)
    {
        const size_t question = current.find('?');
        if (question == std::string::npos)
            return {};                          // a tracker with nothing behind it

        std::string tail = current.substr(question + 1);

        if (tail.rfind("http%3A", 0) == 0 || tail.rfind("https%3A", 0) == 0
            || tail.rfind("http%3a", 0) == 0 || tail.rfind("https%3a", 0) == 0)
        {
            tail = percentDecode(tail);
        }

        if (tail.rfind("http://", 0) != 0 && tail.rfind("https://", 0) != 0)
            return {};                          // not a destination we can read

        current = tail;
    }

    // Still a click-through after three hops, or never was a plain URL: refuse.
    if (looksLikeTracker(current) || !http::isSafeUrl(current))
        return {};

    return current;
}

//==============================================================================
Result FlippSource::fetchFlyers(const std::string& postalCode,
                                std::vector<model::Merchant>& merchantsOut,
                                std::vector<model::Flyer>& flyersOut,
                                const CancelFn& isCancelled)
{
    merchantsOut.clear();
    flyersOut.clear();

    char url[512];
    std::snprintf(url, sizeof(url), "%s/flyers?locale=%s&postal_code=%s",
                  kBase, locale(), urlEncode(normalizePostal(postalCode)).c_str());

    json doc;
    const Result got = getJson(url, doc, canceller_);
    if (!got.ok)
        return got;

    auto flyers = doc.find("flyers");
    if (flyers == doc.end() || !flyers->is_array())
        return Result::failure("Response carried no flyer list");

    // A merchant runs several flyers at once (weekly, pharmacy, seasonal); the
    // merchant list is the de-duplicated projection of the flyer list.
    std::vector<int> seen;

    for (const auto& f : *flyers)
    {
        if (cancelled(isCancelled))
            return Result::failure("Cancelled");
        if (!f.is_object())
            continue;

        model::Flyer flyer;
        flyer.id           = num(f, "id");
        flyer.merchantId   = static_cast<int>(num(f, "merchant_id"));
        flyer.merchantName = str(f, "merchant");
        flyer.name         = str(f, "name");
        flyer.validFrom    = date(f, "valid_from");
        flyer.validTo      = date(f, "valid_to");

        if (flyer.id == 0 || flyer.merchantName.empty())
            continue;

        flyersOut.push_back(flyer);

        if (std::find(seen.begin(), seen.end(), flyer.merchantId) == seen.end())
        {
            seen.push_back(flyer.merchantId);
            model::Merchant m;
            m.id   = flyer.merchantId;
            m.name = flyer.merchantName;
            merchantsOut.push_back(m);
        }
    }

    std::sort(merchantsOut.begin(), merchantsOut.end(),
              [](const model::Merchant& a, const model::Merchant& b) { return a.name < b.name; });

    return Result::success();
}

//==============================================================================
Result FlippSource::fetchItems(const model::Flyer& flyer,
                               const std::string& postalCode,
                               std::vector<model::Item>& itemsOut,
                               const CancelFn& isCancelled)
{
    itemsOut.clear();

    char url[512];
    std::snprintf(url, sizeof(url), "%s/flyers/%lld?locale=%s&postal_code=%s",
                  kBase, flyer.id, locale(), urlEncode(normalizePostal(postalCode)).c_str());

    json doc;
    const Result got = getJson(url, doc, canceller_);
    if (!got.ok)
        return got;

    auto items = doc.find("items");
    if (items == doc.end() || !items->is_array())
        return Result::success();   // an empty flyer is normal, not an error

    for (const auto& j : *items)
    {
        if (cancelled(isCancelled))
            return Result::failure("Cancelled");
        if (!j.is_object())
            continue;

        model::Item item;
        item.id           = num(j, "id");
        item.flyerId      = flyer.id;
        item.merchantId   = flyer.merchantId;
        item.merchantName = flyer.merchantName;
        item.name         = str(j, "name");
        item.brand        = str(j, "brand");
        item.currentPrice = price(j, "price");

        // "discount" on the flyer listing is the percentage off, and it agrees
        // with percent_off from the per-item endpoint — verified against the
        // arithmetic on both: 12,99 against 32,19 is reported as 60. Reading it
        // here means the sort works on a plain sync, with no detail fetch.
        const double off = num(j, "discount");
        item.discountPercent = (off > 0.0 && off <= 100.0) ? static_cast<int>(off) : -1;
        item.saleStory    = str(j, "sale_story");
        item.validFrom    = date(j, "valid_from");
        item.validTo      = date(j, "valid_to");

        if (item.validFrom.empty()) item.validFrom = flyer.validFrom;
        if (item.validTo.empty())   item.validTo   = flyer.validTo;

        // Entries with no name are page decorations, not products. Keeping them
        // would pad every list with unreadable blank rows.
        if (item.id == 0 || item.name.empty())
            continue;

        itemsOut.push_back(std::move(item));
    }

    return Result::success();
}

//==============================================================================
Result FlippSource::fetchItemDetail(long long itemId,
                                    const std::string& postalCode,
                                    model::Item& itemInOut)
{
    char url[512];
    std::snprintf(url, sizeof(url), "%s/items/%lld?locale=%s&postal_code=%s",
                  kBase, itemId, locale(), urlEncode(normalizePostal(postalCode)).c_str());

    json doc;
    const Result got = getJson(url, doc, canceller_);
    if (!got.ok)
        return got;

    // The payload is returned bare on this endpoint but wrapped in some
    // responses; accept either shape.
    const json& j = (doc.contains("item") && doc["item"].is_object()) ? doc["item"] : doc;

    // Only fill what the listing could not provide. The listing's own values are
    // the ones already read out to the user and must not shift under them.
    if (itemInOut.description.empty())  itemInOut.description   = str(j, "description");
    if (itemInOut.sku.empty())          itemInOut.sku           = str(j, "sku");
    if (itemInOut.saleStory.empty())    itemInOut.saleStory     = str(j, "sale_story");
    if (itemInOut.currentPrice == 0.0)  itemInOut.currentPrice  = price(j, "current_price");
    if (itemInOut.originalPrice == 0.0) itemInOut.originalPrice = price(j, "original_price");
    if (itemInOut.priceText.empty())    itemInOut.priceText     = priceLine(j);

    // Most banners publish the saving rather than the former price: this feed
    // routinely carries dollars_off with no original_price at all. The two say
    // the same thing, so the missing half is reconstructed. It is what tells two
    // otherwise identical entries apart — the same product at 2,99 and at 12,99
    // differs only by a 13,00 saving against a 3,00 one.
    if (itemInOut.originalPrice == 0.0 && itemInOut.currentPrice > 0.0)
    {
        const double off = price(j, "dollars_off");

        // Bounded on purpose. The field is trusted only as long as it stays
        // plausible: were a percentage ever to arrive in it — the sort of type
        // drift this file exists to survive — an unchecked sum would have the
        // app announce "2,99 $, was 52,99 $, saving 50,00 $, 94 %" with complete
        // confidence. A saving above three times the price is not believed.
        if (off > 0.0 && off <= itemInOut.currentPrice * 3.0)
            itemInOut.originalPrice = itemInOut.currentPrice + off;
    }

    if (itemInOut.productUrl.empty())
        itemInOut.productUrl = stripTracking(str(j, "ttm_url"));

    itemInOut.inStoreOnly = itemInOut.inStoreOnly || boolean(j, "in_store_only");

    return Result::success();
}

//==============================================================================
Result FlippSource::search(const std::string& query,
                           const std::string& postalCode,
                           std::vector<model::Item>& itemsOut,
                           const CancelFn& isCancelled)
{
    itemsOut.clear();

    if (query.empty())
        return Result::failure("Empty search");

    const std::string url = std::string(kBase) + "/items/search?locale=" + locale()
                          + "&postal_code=" + urlEncode(normalizePostal(postalCode))
                          + "&q=" + urlEncode(query);

    json doc;
    const Result got = getJson(url, doc, canceller_);
    if (!got.ok)
        return got;

    // "items" holds flyer items; "ecom_items" holds online-only marketplace
    // listings. Only the former is a real advertised in-store special, which is
    // what the user is shopping for.
    auto items = doc.find("items");
    if (items == doc.end() || !items->is_array())
        return Result::success();

    for (const auto& j : *items)
    {
        if (cancelled(isCancelled))
            return Result::failure("Cancelled");
        if (!j.is_object())
            continue;

        model::Item item;
        item.id            = num(j, "flyer_item_id");
        if (item.id == 0)
            item.id = num(j, "id");

        item.flyerId       = num(j, "flyer_id");
        item.merchantId    = static_cast<int>(num(j, "merchant_id"));
        item.merchantName  = str(j, "merchant_name");
        item.name          = str(j, "name");
        item.brand         = str(j, "brand");

        const double pctOff = num(j, "percent_off");
        if (pctOff > 0.0 && pctOff <= 100.0)
            item.discountPercent = static_cast<int>(pctOff);
        item.currentPrice  = price(j, "current_price");
        item.originalPrice = price(j, "original_price");
        item.priceText     = priceLine(j);
        item.saleStory     = str(j, "sale_story");
        item.validFrom     = date(j, "valid_from");
        item.validTo       = date(j, "valid_to");

        if (item.id == 0 || item.name.empty())
            continue;

        itemsOut.push_back(std::move(item));
    }

    return Result::success();
}

} // namespace source
