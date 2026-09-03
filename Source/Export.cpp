#include "Export.h"
#include "Format.h"
#include "Localization.h"
#include "Database.h"

#include <wx/ffile.h>
#include <wx/filename.h>

#include <map>

namespace exporter
{
namespace
{
    wxString u8(const std::string& s) { return wxString::FromUTF8(s); }

    // Groups by store while keeping stores in alphabetical order. std::map does
    // both; the list is small enough that nothing smarter is warranted.
    std::map<std::string, std::vector<const model::ListEntry*>>
    groupByStore(const std::vector<model::ListEntry>& entries)
    {
        std::map<std::string, std::vector<const model::ListEntry*>> byStore;

        for (const model::ListEntry& e : entries)
        {
            const std::string key = e.merchantName.empty()
                ? loc::tr("Other", "Autre").utf8_string()
                : e.merchantName;
            byStore[key].push_back(&e);
        }
        return byStore;
    }

    double storeTotal(const std::vector<const model::ListEntry*>& entries)
    {
        double sum = 0.0;
        for (const model::ListEntry* e : entries)
            sum += e->price * e->quantity;
        return sum;
    }

    // A CSV field: quoted, with embedded quotes doubled. Product names routinely
    // contain commas ("bifteck, format familial"), so this is not optional.
    wxString csvField(const wxString& value)
    {
        wxString escaped = value;
        escaped.Replace("\"", "\"\"");
        return "\"" + escaped + "\"";
    }

    wxString buildText(const std::vector<model::ListEntry>& entries, bool markdown)
    {
        const wxString title = loc::tr("Shopping list", "Liste d'épicerie");
        const wxString bullet = markdown ? "- " : "  ";

        // Built through FromUTF8, never written as a bare literal: wxString reads
        // a narrow char* as Latin-1, so the three UTF-8 bytes of an em dash would
        // be re-encoded byte by byte and land in the file as mojibake. Anything
        // non-ASCII in this codebase goes through FromUTF8 or loc::tr.
        const wxString dash = wxString::FromUTF8(" \xE2\x80\x94 ");

        wxString out;
        out << (markdown ? "# " : "") << title << "  (" << u8(db::today()) << ")\n\n";

        double grand = 0.0;

        for (const auto& [store, items] : groupByStore(entries))
        {
            out << (markdown ? "## " : "") << u8(store) << "\n";

            for (const model::ListEntry* e : items)
            {
                out << bullet;

                if (e->quantity > 1)
                    out << e->quantity << " x ";

                out << u8(e->name) << dash << fmt::price(*e);

                const wxString when = fmt::validity(e->validTo);
                if (!when.empty())
                    out << " (" << when << ")";

                out << "\n";
            }

            const double total = storeTotal(items);
            grand += total;

            if (total > 0.0)
                out << (markdown ? "\n**" : "  ")
                    << loc::tr("Subtotal: ", "Sous-total : ") << fmt::money(total)
                    << (markdown ? "**" : "") << "\n";

            out << "\n";
        }

        if (grand > 0.0)
        {
            // Estimated, and said so: items priced "2 for 5$" carry no number and
            // contribute nothing, so a total presented as exact would be wrong.
            out << (markdown ? "**" : "")
                << loc::tr("Estimated total: ", "Total estimé : ") << fmt::money(grand)
                << (markdown ? "**" : "") << "\n";
        }

        return out;
    }

    wxString buildCsv(const std::vector<model::ListEntry>& entries)
    {
        wxString out;
        out << csvField(loc::tr("Store", "Magasin")) << ","
            << csvField(loc::tr("Quantity", "Quantité")) << ","
            << csvField(loc::tr("Item", "Article")) << ","
            << csvField(loc::tr("Price", "Prix")) << ","
            << csvField(loc::tr("Valid until", "Valide jusqu'au")) << "\n";

        for (const model::ListEntry& e : entries)
        {
            // The raw number goes out, not the spoken form: a spreadsheet has to
            // be able to add this column up.
            const wxString priceCell = e.price > 0.0
                ? wxString::Format("%.2f", e.price)
                : u8(e.priceText);

            out << csvField(u8(e.merchantName)) << ","
                << e.quantity << ","
                << csvField(u8(e.name)) << ","
                << csvField(priceCell) << ","
                << csvField(u8(e.validTo)) << "\n";
        }

        return out;
    }
}

Format formatForPath(const wxString& path)
{
    const wxString ext = wxFileName(path).GetExt().Lower();

    if (ext == "csv") return Format::Csv;
    if (ext == "md")  return Format::Markdown;

    return Format::Text;
}

bool write(const wxString& path,
           Format format,
           const std::vector<model::ListEntry>& entries,
           wxString& error)
{
    wxString content;
    switch (format)
    {
        case Format::Csv:      content = buildCsv(entries);          break;
        case Format::Markdown: content = buildText(entries, true);   break;
        case Format::Text:     content = buildText(entries, false);  break;
    }

    wxFFile file(path, "wb");
    if (!file.IsOpened())
    {
        error = loc::tr("Could not create the file.", "Impossible de créer le fichier.");
        return false;
    }

    // UTF-8 with a BOM: without it Excel and Notepad open a French list as
    // mojibake, and the user finds out only once the file is elsewhere.
    static const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
    const std::string utf8 = content.utf8_string();

    const bool ok = file.Write(bom, sizeof(bom)) == sizeof(bom)
                 && file.Write(utf8.data(), utf8.size()) == utf8.size();

    if (!ok)
        error = loc::tr("The file could not be written.", "Le fichier n'a pas pu être écrit.");

    return ok;
}

} // namespace exporter
