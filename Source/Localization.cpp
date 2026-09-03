#include "Localization.h"

#include <wx/window.h>
#include <wx/intl.h>
#include <wx/uilocale.h>

namespace loc
{
namespace
{
    // Resolved once from the OS, then left alone. The system language cannot
    // change under a running process; only the user's own choice can.
    Language systemLanguage()
    {
        // wxUILocale reports the preferred UI language as a BCP-47 tag ("fr",
        // "fr-FR", "fr-CA", ...). A leading "fr" covers every region.
        const wxString tag = wxUILocale::GetCurrent().GetName();
        if (tag.Lower().StartsWith("fr"))
            return Language::French;

        // Fallback for platforms that only populate the wxLanguageInfo path.
        const wxLanguageInfo* info = wxUILocale::GetLanguageInfo(wxLANGUAGE_DEFAULT);
        if (info != nullptr && info->CanonicalName.Lower().StartsWith("fr"))
            return Language::French;

        return Language::English;
    }

    Language current  = Language::English;
    bool     resolved = false;

    Language& state()
    {
        if (!resolved)
        {
            current  = systemLanguage();
            resolved = true;
        }
        return current;
    }
}

void setLanguage(Language language)
{
    state() = language;   // state() first, so the system default cannot land on top later
}

Language language()
{
    return state();
}

bool isFrench()
{
    return state() == Language::French;
}

wxString tr(const char* en, const char* fr)
{
    // FromUTF8 is explicit on purpose: it states the contract the literals must
    // honour, instead of depending on the compiler's execution charset.
    return wxString::FromUTF8(isFrench() ? fr : en);
}

void translateStockButtons(wxWindow* dialog)
{
    if (dialog == nullptr || !isFrench())
        return;

    struct Stock { wxWindowID id; const char* fr; };
    static const Stock kStock[] =
    {
        { wxID_OK,     "OK" },
        { wxID_CANCEL, "Annuler" },
        { wxID_YES,    "Oui" },
        { wxID_NO,     "Non" },
        { wxID_APPLY,  "Appliquer" },
        { wxID_CLOSE,  "Fermer" },
    };

    for (const Stock& stock : kStock)
        if (wxWindow* button = dialog->FindWindow(stock.id))
            button->SetLabel(wxString::FromUTF8(stock.fr));
}

} // namespace loc
