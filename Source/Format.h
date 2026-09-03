#pragma once

#include "Model.h"

#include <wx/string.h>

// Turning model data into the exact words the screen reader will say.
//
// This lives in one place because the same item is spoken in four contexts — a
// flyer row, a search result, a favourite match and a list line — and they must
// agree. If a price reads "2 for 5$" in one and "0,00 $" in another, the user
// has no way to tell which one lied.
namespace fmt
{
    // The price as a person would say it, never a bare zero. Falls back to the
    // advertised text ("2 pour 5$", "1,99 /lb") when there is no plain number,
    // and to a spoken "no price" when the flyer advertised none at all.
    wxString price(const model::Item& item);

    // Same rule for a saved list line.
    wxString price(const model::ListEntry& entry);

    // "until September 9" / "jusqu'au 9 septembre", or an explicit "expired".
    // For running text — the export file — where nothing else supplies the
    // preposition.
    wxString validity(const std::string& validTo);

    // The same date with no preposition: "September 9" / "9 septembre".
    //
    // For a column already headed "Valid until". A screen reader announces the
    // header before the cell, so the full form there comes out as "Valid until:
    // until September 9" — the reader hears the word twice and has to work out
    // that nothing is missing.
    wxString validityDate(const std::string& validTo);

    // A monetary total, e.g. "48,35 $".
    wxString money(double amount);

    // "was 6,99 $, saving 2,00 $", or empty when no original price is known.
    //
    // Both figures, never a percentage on its own: the feed carries the same
    // product at two prices against one original (a per-pound line and a package
    // line), so a percentage presented alone would state a discount that does not
    // exist. Two numbers let the reader judge.
    //
    // A percentage IS added by itemDetail() and itemLines(), and only there: it
    // sits beside the two amounts it derives from, computed from that one item's
    // own pair, so it cannot be mistaken for a claim about a different entry. It
    // is also the figure that flags the implausible ones — an 81 % saving on
    // chicken wings is how the reader learns the price is conditional.
    wxString savings(const model::Item& item);

    // The item name in the reader's language alone.
    //
    // Quebec banners publish one bilingual string per item, French then English,
    // separated by a vertical bar: "boeuf hache mi-maigre | medium ground beef".
    // The feed returns it that way whatever locale is asked for, so the split is
    // ours to make. Half the items in a typical flyer are like this, and reading
    // both halves aloud doubles the length of every row for no gain.
    //
    // The size sometimes sits only on the English side ("FRAMBOISES | RASPBERRIES,
    // 170 G") -- about one bilingual item in fourteen. That trailing fragment is
    // carried over, because a raspberry price without the format is useless.
    //
    // Applied at display time, never on the way into the cache: the stored name
    // keeps both languages, so search works in either one and switching the
    // interface language needs no resync.
    wxString itemName(const std::string& rawName);

    // Everything worth hearing about one item, as one sentence: name, price,
    // what it was, the format, the banner, the validity.
    wxString itemDetail(const model::Item& item);

    // The same facts, one per line, for the read-only field under each item list.
    //
    // A spoken notification is heard once and cannot be reviewed; a text field
    // has a caret, so a screen reader walks it line by line and a braille
    // display shows it. One fact per line is what makes a single Down press
    // move from the price to the format.
    //
    // Deliberately self-contained — it repeats the name, the price, the banner
    // and the validity that the list row also carries — so that the field can be
    // read on its own without having heard the row.
    wxString itemLines(const model::Item& item);

    // The keystroke the details field advertises for opening a product page.
    // Kept here so the field, the button label and the handler cannot drift.
    wxString productPageKey();

    // Same idea for a flyer title. Loblaw banners title theirs "Weekly Flyer -
    // Valid Thursday, September 03 - Wednesday, September 09": English, and a
    // restatement of the validity column right beside it. Replaced by a plain
    // localized label.
    wxString flyerName(const std::string& rawName);
}
