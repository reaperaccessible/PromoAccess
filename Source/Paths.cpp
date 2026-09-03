#include "Paths.h"
#include "Version.h"

#include <windows.h>
#include <shlobj.h>        // SHGetKnownFolderPath, FOLDERID_RoamingAppData
#include <wx/filename.h>
#include <wx/stdpaths.h>

namespace paths
{

wxString dataFolder()
{
    static const wxString folder = []
    {
        wxString base;

        PWSTR path = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, nullptr, &path)))
        {
            base = path;
            CoTaskMemFree(path);
        }

        if (base.empty())
            base = wxStandardPaths::Get().GetTempDir();

        const wxString full = base + wxFileName::GetPathSeparator() + PROMO_APP_NAME;
        if (!wxFileName::DirExists(full))
            wxFileName::Mkdir(full, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);

        return full;
    }();

    return folder;
}

wxString databaseFile()
{
    return dataFolder() + wxFileName::GetPathSeparator() + "cache.db";
}

wxString manualFile(bool french)
{
    const wxString name = french ? "manual_fr.html" : "manual_en.html";
    const wxString sep  = wxFileName::GetPathSeparator();

    wxFileName exe(wxStandardPaths::Get().GetExecutablePath());
    const wxString dir = exe.GetPath();

    // Beside the executable is where the installer puts it. The two climbs are
    // for a development build, whose executable sits in build\Release while
    // Docs stays at the root of the source tree; the copy CMake makes after each
    // build normally means the first candidate already wins.
    const wxString candidates[] =
    {
        dir + sep + "Docs" + sep + name,
        dir + sep + ".." + sep + "Docs" + sep + name,
        dir + sep + ".." + sep + ".." + sep + "Docs" + sep + name,
    };

    for (const wxString& candidate : candidates)
        if (wxFileName::FileExists(candidate))
            return candidate;

    return {};
}

} // namespace paths
