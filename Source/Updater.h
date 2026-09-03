#pragma once

#include <wx/string.h>
#include <functional>
#include <string>

class wxWindow;

// Automatic update check, the same one MediaAccess ships.
//
// The whole flow, in the order the user meets it:
//
//   1. Three seconds after the window opens, a worker asks GitHub for the
//      releases of this repository. Three seconds, not zero: the first moments
//      belong to the screen reader announcing the window, and a dialog that
//      lands in the middle of that is a dialog nobody heard open.
//   2. It reads the newest release, takes its tag, and compares it with the
//      version built into this executable.
//   3. Nothing newer, or no network: silence. A startup check that announces
//      "you are up to date" every single morning trains people to dismiss it
//      without listening, which is exactly the habit that later makes them
//      dismiss the one that mattered.
//   4. Something newer: a dialog with the version and the release notes, and
//      the choice to install it or not.
//   5. Accepted: the installer is downloaded with a progress dialog, then run
//      with /SILENT /SUPPRESSMSGBOXES /FORCECLOSEAPPLICATIONS /NORESTART
//      /AUTOUPDATE=1, and the application closes so its own file can be
//      replaced. The installer puts it back up when it is done.
//
// The check can be turned off in the settings tab, and is then never made.
namespace updater
{
    struct Info
    {
        bool        available = false;   // a strictly newer version exists
        std::string latestVersion;       // "1.01", tag stripped of any leading v
        std::string installerUrl;        // the Windows installer asset
        std::string releaseNotes;        // the release body, as written
        wxString    error;               // filled when the check could not run
    };

    // Blocking. Worker threads only.
    Info check();

    // True when `remote` is strictly newer than `local`. Shorter versions are
    // zero-padded, so "1.10" beats "1.9" and "1.00" ties with "1".
    bool isNewer(const std::string& remote, const std::string& local);

    // Downloads the installer to the temp folder and returns its path, empty on
    // failure. `progress` is called from the worker thread.
    wxString download(const std::string& url,
                      const std::function<void(size_t, size_t)>& progress);

    // Runs the installer silently and ends this process. Never returns when it
    // succeeds. Shows why it could not and returns when it fails.
    void apply(wxWindow* parent, const wxString& installerPath);

    // True when this copy was put here by the installer rather than unpacked by
    // hand. An update replaces the installed copy through the installer; a copy
    // someone unzipped somewhere is left alone.
    bool isInstalled();
}
