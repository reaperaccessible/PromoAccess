#pragma once

#include <wx/string.h>

// Filesystem locations PromoAccess writes to.
namespace paths
{
    // %APPDATA%\PromoAccess, created if missing. Holds the SQLite cache and the
    // settings file. Falls back to the temp folder if the known folder lookup
    // fails, so the app still runs (with a throwaway cache) rather than refusing
    // to start.
    wxString dataFolder();

    // Full path of the SQLite cache inside dataFolder().
    wxString databaseFile();

    // The HTML manual for the current interface language, as shipped in Docs.
    //
    // A document on disk opened in the browser rather than a window built in
    // C++: the browser gives a screen reader its whole navigation vocabulary for
    // free — headings, links, a table of contents, find-on-page — which no
    // hand-built dialog matches. It is also the same arrangement as the other
    // ReaperAccessible products, so the manual is one habit and not five.
    //
    // Returns an empty string when no copy can be found; the caller says which
    // file is missing rather than opening nothing.
    wxString manualFile(bool french);
}
