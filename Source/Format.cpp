#include "Format.h"

#include <windows.h>

#include <vector>
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

namespace
{
    // Case conversion through Windows rather than through towlower.
    //
    // std::towlower leaves accented capitals exactly as they are under the "C"
    // locale — É stays É — so a name came back as "Bacon TranchÉ MÈre Michel".
    // The same trap Text.cpp documents for the search index, met again here.
    // CharLowerBuffW and CharUpperBuffW use the system's own tables and get the
    // whole Latin range right.
    wxUniChar lowered(wxUniChar c)
    {
        wchar_t w = static_cast<wchar_t>(c.GetValue());
        ::CharLowerBuffW(&w, 1);
        return wxUniChar(w);
    }

    wxUniChar raised(wxUniChar c)
    {
        wchar_t w = static_cast<wchar_t>(c.GetValue());
        ::CharUpperBuffW(&w, 1);
        return wxUniChar(w);
    }

    // A character that has a case is a letter. Asked this way rather than with
    // iswalpha, which answers for the "C" locale and not for the text at hand.
    bool hasCase(wxUniChar c)
    {
        return lowered(c) != c || raised(c) != c;
    }
}

namespace
{
    // Units written in capitals are simply wrong, and the right form is not a
    // matter of taste: 890 ML is 890 ml. Lowered whatever the rest of the name
    // does, which is also what keeps "890 mL" from becoming "890 Ml".
    bool isUnit(const wxString& word)
    {
        static const wxString kUnits[] =
        {
            "g", "kg", "mg", "ml", "l", "cl", "lb", "oz", "cm", "mm", "m", "po"
        };

        const wxString lower = word.Lower();

        for (const wxString& unit : kUnits)
            if (lower == unit)
                return true;

        return false;
    }

    bool allUpper(const wxString& word)
    {
        bool anyCased = false;

        for (const wxUniChar c : word)
        {
            if (!hasCase(c))
                continue;

            anyCased = true;

            if (lowered(c) == c)
                return false;
        }

        return anyCased;
    }

    // Words, for this purpose: runs of letters, with apostrophes and hyphens
    // holding a word together. A digit ends one, so "6X355" is "X" between two
    // numbers rather than one long word.
    std::vector<wxString> words(const wxString& name)
    {
        std::vector<wxString> out;
        wxString current;

        for (const wxUniChar c : name)
        {
            const bool joiner = (c == wxUniChar('\'')
                              || c == wxUniChar(0x2019)
                              || c == wxUniChar('-'));

            if (hasCase(c) || (joiner && !current.empty()))
            {
                current += c;
                continue;
            }

            if (!current.empty())
            {
                out.push_back(current);
                current.clear();
            }
        }

        if (!current.empty())
            out.push_back(current);

        return out;
    }
}

namespace
{
    // A name written in mixed case keeps its capitals — "Irrésistible",
    // "Kellogg's", "San Daniele" were put there on purpose — except the very
    // first letter. Super C opens 95 % of its names in lower case, "fromage
    // feta Irrésistible", and a product name is a title, not the middle of a
    // sentence. A name that begins with a digit or a sign is left whole: "500 g
    // de beurre" must not become "500 G de beurre".
    wxString sentenceStart(const wxString& name)
    {
        if (name.empty())
            return name;

        const wxUniChar first = name[0];

        if (!hasCase(first) || raised(first) == first)
            return name;

        wxString out = name;
        out[0] = raised(first);
        return out;
    }
}

wxString properCase(const wxString& name)
{
    // Does this name SHOUT?
    //
    // Asked of the words of three letters or more, and of them only. The first
    // rule looked for a single lower-case letter anywhere, and "MAYONNAISE MAG,
    // 890 mL" therefore escaped untouched: the banner had shouted the product
    // and written the unit correctly. Short words are exactly where the
    // exceptions live — units, sigles — so they get no vote.
    bool anyLongWord = false;

    for (const wxString& word : words(name))
    {
        if (word.length() < 3)
            continue;

        anyLongWord = true;

        if (!allUpper(word))
            return sentenceStart(name);   // mixed case: the banner meant it
    }

    if (!anyLongWord)
        return sentenceStart(name);

    // Word by word. One already carrying a lower-case letter is left exactly as
    // it is — that is how "mL" survives — and a unit is lowered whatever it
    // looked like.
    wxString out;
    out.reserve(name.length());

    wxString current;

    auto flush = [&out, &current]
    {
        if (current.empty())
            return;

        if (isUnit(current))
        {
            out += current.Lower();
        }
        else if (!allUpper(current))
        {
            out += current;                    // written properly already
        }
        else
        {
            bool first = true;
            for (const wxUniChar c : current)
            {
                out += (first && hasCase(c)) ? raised(c) : lowered(c);
                if (hasCase(c))
                    first = false;
            }
        }

        current.clear();
    };

    for (const wxUniChar c : name)
    {
        const bool joiner = (c == wxUniChar('\'')
                          || c == wxUniChar(0x2019)
                          || c == wxUniChar('-'));

        if (hasCase(c) || (joiner && !current.empty()))
        {
            current += c;
            continue;
        }

        flush();
        out += c;
    }

    flush();
    return out;
}

wxString itemName(const std::string& rawName)
{
    const size_t bar = rawName.find('|');
    if (bar == std::string::npos)
        return properCase(u8(rawName));

    const std::string french  = trimmed(rawName.substr(0, bar));
    const std::string english = trimmed(rawName.substr(bar + 1));

    // A bar with nothing on one side is not a translation pair; leave it alone
    // rather than hand back an empty row.
    if (french.empty() || english.empty())
        return properCase(u8(rawName));

    std::string kept  = loc::isFrench() ? french  : english;
    const std::string& other = loc::isFrench() ? english : french;

    // Carry over the format when it was only written on the other side.
    if (trailingSize(kept) == std::string::npos)
    {
        const size_t size = trailingSize(other);
        if (size != std::string::npos)
            kept += other.substr(size);
    }

    return properCase(u8(kept));
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

wxString lineTotal(const model::ListEntry& entry)
{
    if (entry.quantity <= 1 || entry.price <= 0.0)
        return price(entry);

    wxString s = amount(entry.price * entry.quantity)
               + wxString::Format(" (%d x %s)", entry.quantity, amount(entry.price));

    // The banner's own wording is kept at the end, as it is for a single unit:
    // "RABAIS DE 4$" is the reason the price is what it is.
    if (!entry.priceText.empty())
        s += " " + u8(entry.priceText);

    return s;
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

bool isExpired(const std::string& validTo)
{
    // ISO dates compare correctly as text, which is why they are stored that
    // way: no parsing, no time zone, no ambiguity about 03/09.
    return !validTo.empty() && validTo < db::today();
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
