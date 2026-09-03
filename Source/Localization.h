#pragma once

#include <wx/string.h>

class wxWindow;

//==============================================================================
// Minimal two-language helper (English / French).
//
// Same contract as DrumAccess's loc:: — the translation lives next to the
// original, where it cannot drift, and no .lang/.mo files need to ship.
//
// Both literals MUST be UTF-8; the build passes /utf-8 on MSVC so the source
// encoding is unambiguous.
//
// The language starts from the operating system and can then be overridden by
// the user, which is persisted. Because tr() is evaluated when a control is
// built, changing the language does not retranslate a window that already
// exists: the caller has to rebuild it. That is deliberate — a rebuild is a
// dozen lines and always correct, where retranslating in place means finding
// every label, column header and accessible name, and quietly missing some.
//==============================================================================
namespace loc
{
    enum class Language { English, French };

    // Overrides the system language. Takes effect for every tr() called after
    // it, so callers must rebuild any window already on screen.
    void setLanguage(Language language);

    Language language();

    // True when the current language is French.
    bool isFrench();

    // Returns the French literal when the language is French, the English one
    // otherwise.
    wxString tr(const char* en, const char* fr);

    // Relabels the stock buttons of a dialog we built (OK, Cancel, Yes, No).
    //
    // wxWidgets supplies those labels from its own message catalogs, and no
    // catalog is installed with this build — so they stay English whatever the
    // interface language. A French user would tab from "Mots à surveiller"
    // straight onto a button announced as "Cancel". Anything wx owns entirely,
    // such as wxGetNumberFromUser, cannot be reached this way and has to be
    // replaced rather than relabelled.
    void translateStockButtons(wxWindow* dialog);
}
