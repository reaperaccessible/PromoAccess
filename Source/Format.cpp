#include "Format.h"
#include "Localization.h"
#include "Database.h"
#include "Text.h"

#include <cctype>

#include <wx/tokenzr.h>

#include <cstdlib>

namespace fmt
{
namespace
{
    wxString u8(const std::string& s) { return wxString::FromUTF8(s); }

    // Quebec writes prices as "4,99 $" and English Canada as "$4.99". Getting
    // this wrong is not cosmetic: a screen reader reads "4.99" as "four point
    // ninety-nine" to a French user, which is not how anyone says a price.
    wxString amount(double value)
    {
        if (loc::isFrench())
        {
            wxString s = wxString::Format("%.2f", value);
            s.Replace(".", ",");
            return s + " $";
        }
        return wxString::Format("$%.2f", value);
    }

    const char* kMonthsEn[] = { "January", "February", "March", "April", "May", "June",
                                "July", "August", "September", "October", "November", "December" };
    const char* kMonthsFr[] = { "janvier", "février", "mars", "avril", "mai", "juin",
                                "juillet", "août", "septembre", "octobre", "novembre", "décembre" };

    // "2026-09-09" spoken as a date rather than spelled out as digits and dashes.
    wxString spokenDate(const std::string& iso)
    {
        if (iso.size() < 10)
            return u8(iso);

        const int month = std::atoi(iso.substr(5, 2).c_str());
        const int day   = std::atoi(iso.substr(8, 2).c_str());

        if (month < 1 || month > 12 || day < 1 || day > 31)
            return u8(iso);

        return loc::isFrench()
            ? wxString::Format("%d %s", day, wxString::FromUTF8(kMonthsFr[month - 1]))
            : wxString::Format("%s %d", wxString::FromUTF8(kMonthsEn[month - 1]), day);
    }
}

namespace
{
    std::string trimmed(const std::string& s)
    {
        size_t first = 0;
        while (first < s.size() && std::isspace(static_cast<unsigned char>(s[first])))
            ++first;

        size_t last = s.size();
        while (last > first && std::isspace(static_cast<unsigned char>(s[last - 1])))
            --last;

        return s.substr(first, last - first);
    }

    // A trailing ", 170 G" / ", 2X675 G" / ", 38-120 un." fragment: the last
    // comma-separated chunk, when it carries a digit. Returns npos when there is
    // none. Deliberately loose -- units are written a dozen different ways across
    // four banners, and a digit after the last comma is what they all share.
    size_t trailingSize(const std::string& s)
    {
        const size_t comma = s.rfind(',');
        if (comma == std::string::npos)
            return std::string::npos;

        for (size_t n = comma; n < s.size(); ++n)
            if (std::isdigit(static_cast<unsigned char>(s[n])))
                return comma;

        return std::string::npos;
    }
}

wxString itemName(const std::string& rawName)
{
    const size_t bar = rawName.find('|');
    if (bar == std::string::npos)
        return u8(rawName);

    const std::string french  = trimmed(rawName.substr(0, bar));
    const std::string english = trimmed(rawName.substr(bar + 1));

    // A bar with nothing on one side is not a translation pair; leave it alone
    // rather than hand back an empty row.
    if (french.empty() || english.empty())
        return u8(rawName);

    std::string kept  = loc::isFrench() ? french  : english;
    const std::string& other = loc::isFrench() ? english : french;

    // Carry over the format when it was only written on the other side.
    if (trailingSize(kept) == std::string::npos)
    {
        const size_t size = trailingSize(other);
        if (size != std::string::npos)
            kept += other.substr(size);
    }

    return u8(kept);
}

wxString itemDetail(const model::Item& item)
{
    wxString s = itemName(item.name);

    // Composed rather than reusing price(): the proportion has to sit inside the
    // saving it qualifies. Appended at the end of the sentence it would follow
    // the loyalty mention and read as a percentage of nothing in particular.
    if (item.currentPrice > 0.0)
    {
        s += ", " + money(item.currentPrice);

        if (!item.priceText.empty())
            s += " " + u8(item.priceText);

        const wxString before = savings(item);
        if (!before.empty())
        {
            const int percent = static_cast<int>(
                (item.originalPrice - item.currentPrice) / item.originalPrice * 100.0 + 0.5);

            s += wxString::Format(" (%s, %d %%)", before, percent);
        }

        if (!item.saleStory.empty())
            s += ", " + u8(item.saleStory);
    }
    else
    {
        // No number advertised: price() already says what there is to say,
        // loyalty mention included.
        s += ", " + price(item);
    }

    if (!item.description.empty())
        s += ", " + u8(item.description);

    s += ", " + u8(item.merchantName);

    const wxString when = validity(item.validTo);
    if (!when.empty())
        s += ", " + when;

    if (item.inStoreOnly)
        s += ", " + loc::tr("in store only", "en magasin seulement");

    return s;
}

wxString itemLines(const model::Item& item)
{
    wxString s = itemName(item.name);

    // The price and what it used to be, on one line: they are read together or
    // not at all. The sale story is deliberately NOT folded in here the way
    // price() does it — it earns its own line below, so one Down press reaches
    // the condition without the amount in front of it.
    if (item.currentPrice > 0.0)
    {
        wxString amount = money(item.currentPrice);

        if (!item.priceText.empty())
            amount += " " + u8(item.priceText);

        const wxString before = savings(item);
        if (!before.empty())
        {
            const int percent = static_cast<int>(
                (item.originalPrice - item.currentPrice) / item.originalPrice * 100.0 + 0.5);

            amount += ", " + before + wxString::Format(", %d %%", percent);
        }

        s += "\n" + amount;
    }
    else if (!item.priceText.empty())
    {
        s += "\n" + u8(item.priceText);
    }
    else
    {
        s += "\n" + loc::tr("no price listed", "prix non indiqué");
    }

    if (!item.saleStory.empty())
        s += "\n" + u8(item.saleStory);

    if (!item.description.empty())
    {
        // The description is bilingual too — "format économique | economic pack"
        // — and the feed often puts several of them on their own lines, so the
        // split is applied line by line rather than to the block as a whole.
        wxString block = u8(item.description);
        block.Replace("\r", "");

        wxStringTokenizer lines(block, "\n");
        while (lines.HasMoreTokens())
        {
            const wxString one = lines.GetNextToken().Trim(true).Trim(false);
            if (!one.empty())
                s += "\n" + itemName(one.utf8_string());
        }
    }

    wxString where = u8(item.merchantName);
    const wxString when = validity(item.validTo);
    if (!when.empty())
        where += ", " + when;
    s += "\n" + where;

    if (item.inStoreOnly)
        s += "\n" + loc::tr("in store only", "en magasin seulement");

    // Before the status line, which by contract is always last. The address
    // itself, not a description of it: this pane is caret-readable and
    // Ctrl+C-able, which is what a URL needs — it is deliberately kept out of
    // itemDetail(), where a hundred characters of link would be read aloud.
    if (!item.productUrl.empty())
    {
        s += "\n" + loc::tr("Product page (", "Fiche produit (") + productPageKey() + ")";
        s += "\n" + u8(item.productUrl);
    }

    // The last line always says where the lookup stands. Without it an item the
    // server had nothing to add about is indistinguishable from one the field
    // simply failed to fill — and a failed fetch counts as fetched, so waiting
    // would be waiting for nothing.
    if (!item.detailFetched)
    {
        s += "\n" + loc::tr("Looking up the details...", "Recherche des détails...");
    }
    else
    {
        // Judged on what the field actually SHOWS, not on the raw fields: the
        // feed routinely returns an original price equal to the current one,
        // which savings() rightly ignores. Testing originalPrice > 0 counted
        // such an item as having further details, then printed none — leaving
        // the reader with no closing line at all and no way to know whether
        // anything was still coming.
        const bool nothingMore = item.description.empty()
                              && savings(item).empty()
                              && item.saleStory.empty()
                              && item.productUrl.empty()
                              && !item.inStoreOnly;

        if (nothingMore)
            s += "\n" + loc::tr("No further details.", "Aucun détail supplémentaire.");
    }

    return s;
}

wxString productPageKey()
{
    // Not Ctrl+O: thirty years of Windows have made that "open a file", and a
    // reflex press would throw the user out of the application into a browser —
    // the least recoverable action in the program on the most reflexive key.
    // Localized: Windows calls the key Maj in French and Shift in English, and a
    // screen reader announces whatever it is given.
    return loc::tr("Ctrl+Shift+O", "Ctrl+Maj+O");
}

wxString flyerName(const std::string& rawName)
{
    const std::string name = trimmed(rawName);

    if (name.empty())
        return loc::tr("Flyer", "Circulaire");

    // "Weekly Flyer - Valid Thursday, September 03 - Wednesday, September 09".
    // The dates are already in the column next to it, so the whole title says
    // nothing except which kind of flyer this is.
    const std::string lower = text::normalize(name);
    if (lower.rfind("weekly flyer", 0) == 0)
        return loc::tr("Weekly flyer", "Circulaire hebdomadaire");

    return u8(name);
}

wxString money(double amount_)
{
    return amount(amount_);
}

wxString savings(const model::Item& item)
{
    if (item.originalPrice <= 0.0 || item.currentPrice <= 0.0
        || item.originalPrice <= item.currentPrice)
        return {};

    return loc::tr("was ", "était ") + amount(item.originalPrice) + ", "
         + loc::tr("saving ", "économie ") + amount(item.originalPrice - item.currentPrice);
}

wxString price(const model::Item& item)
{
    if (item.currentPrice > 0.0)
    {
        wxString s = amount(item.currentPrice);

        // "/lb" and "each" change what the number means; dropping them would
        // make a per-pound price look like a package price.
        if (!item.priceText.empty())
            s += " " + u8(item.priceText);

        // Shown right in the price cell rather than kept for a details view:
        // once known, what an item used to cost is the whole point of the row.
        const wxString before = savings(item);
        if (!before.empty())
            s += " (" + before + ")";

        // The condition attached to the price, when the banner states one:
        // "120 Scene+ PTS", "2 for 5$", "with coupon". Without it a price that
        // only applies under a loyalty scheme reads as the shelf price, which is
        // how the same product ends up looking like two contradictory offers.
        if (!item.saleStory.empty())
            s += ", " + u8(item.saleStory);

        return s;
    }

    if (!item.saleStory.empty()) return u8(item.saleStory);
    if (!item.priceText.empty()) return u8(item.priceText);

    return loc::tr("no price listed", "prix non indiqué");
}

wxString price(const model::ListEntry& entry)
{
    if (entry.price > 0.0)
    {
        wxString s = amount(entry.price);
        if (!entry.priceText.empty())
            s += " " + u8(entry.priceText);
        return s;
    }

    if (!entry.priceText.empty()) return u8(entry.priceText);

    return loc::tr("no price listed", "prix non indiqué");
}

wxString validity(const std::string& validTo)
{
    if (validTo.empty())
        return {};

    if (validTo < db::today())
        return loc::tr("expired", "expiré");

    return loc::tr("until ", "jusqu'au ") + spokenDate(validTo);
}

wxString validityDate(const std::string& validTo)
{
    if (validTo.empty())
        return {};

    // Still spelled out when it has passed: under a "Valid until" header, a bare
    // date gives no hint that the deal is over.
    if (validTo < db::today())
        return loc::tr("expired", "expiré");

    return spokenDate(validTo);
}

} // namespace fmt
