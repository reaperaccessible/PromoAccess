#include "Updater.h"
#include "Http.h"
#include "Localization.h"
#include "Version.h"

#include <nlohmann/json.hpp>

#include <wx/filename.h>
#include <wx/file.h>
#include <wx/msgdlg.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <vector>

namespace
{
    // The releases of this repository, newest first.
    const wxString kReleasesUrl =
        "https://api.github.com/repos/reaperaccessible/PromoAccess/releases";

    // "1.02" -> {1, 2}. A leading v is dropped and trailing text is ignored, so
    // a tag written "v1.02" or "1.02-beta" still compares as {1, 2}.
    std::vector<unsigned> parts(const std::string& version)
    {
        std::vector<unsigned> out;
        size_t i = 0;

        if (i < version.size() && (version[i] == 'v' || version[i] == 'V'))
            ++i;

        while (i < version.size())
        {
            unsigned n = 0;
            bool any = false;

            while (i < version.size() && version[i] >= '0' && version[i] <= '9')
            {
                n = n * 10 + static_cast<unsigned>(version[i] - '0');
                ++i;
                any = true;
            }

            if (!any)
                break;

            out.push_back(n);

            if (i < version.size() && version[i] == '.')
            {
                ++i;
                continue;
            }

            break;
        }

        return out;
    }

    // Reads a string field, tolerating a missing or wrongly typed one: the reply
    // comes from a server, so every field is treated as optional.
    std::string str(const nlohmann::json& j, const char* key)
    {
        auto it = j.find(key);
        return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
    }
}

namespace updater
{

bool isNewer(const std::string& remote, const std::string& local)
{
    std::vector<unsigned> r = parts(remote);
    std::vector<unsigned> l = parts(local);

    // Zero-padded so "1.1" and "1.1.0" are the same version rather than one
    // being mysteriously newer than the other.
    const size_t n = std::max(r.size(), l.size());
    r.resize(n, 0);
    l.resize(n, 0);

    for (size_t i = 0; i < n; ++i)
    {
        if (r[i] > l[i]) return true;
        if (r[i] < l[i]) return false;
    }

    return false;   // equal is not newer
}

Info check()
{
    Info info;

    std::string body;
    const http::GetResult result = http::getToString(kReleasesUrl, body);

    if (!result.ok)
    {
        info.error = loc::tr("Could not reach GitHub to check for updates.",
                             "Impossible de joindre GitHub pour vérifier les mises à jour.");
        return info;
    }

    nlohmann::json releases = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (!releases.is_array() || releases.empty())
    {
        info.error = loc::tr("No release was found.", "Aucune version n'a été trouvée.");
        return info;
    }

    // The first entry is the newest release. Drafts and prereleases are skipped:
    // an unfinished release is not something to push onto everyone's machine.
    const nlohmann::json* release = nullptr;
    for (const auto& candidate : releases)
    {
        if (!candidate.is_object())
            continue;

        const auto draft = candidate.find("draft");
        const auto pre   = candidate.find("prerelease");

        const bool isDraft = draft != candidate.end() && draft->is_boolean() && draft->get<bool>();
        const bool isPre   = pre   != candidate.end() && pre->is_boolean()   && pre->get<bool>();

        if (!isDraft && !isPre)
        {
            release = &candidate;
            break;
        }
    }

    if (release == nullptr)
    {
        info.error = loc::tr("No release was found.", "Aucune version n'a été trouvée.");
        return info;
    }

    info.latestVersion = str(*release, "tag_name");
    if (!info.latestVersion.empty()
        && (info.latestVersion[0] == 'v' || info.latestVersion[0] == 'V'))
    {
        info.latestVersion.erase(0, 1);
    }

    info.releaseNotes = str(*release, "body");

    // The Windows installer among the assets. The version-less name is the one
    // the permanent download link uses, so it is preferred; a versioned name is
    // accepted as a fallback in case a release ever ships only that.
    const auto assets = release->find("assets");
    if (assets != release->end() && assets->is_array())
    {
        for (const auto& asset : *assets)
        {
            if (!asset.is_object())
                continue;

            const std::string name = str(asset, "name");
            const std::string url  = str(asset, "browser_download_url");

            if (name.size() < 4 || url.empty())
                continue;

            const bool isExe = name.compare(name.size() - 4, 4, ".exe") == 0;
            if (!isExe || !http::isSafeUrl(url))
                continue;

            // The version-less asset, which is also what the permanent download
            // link serves. The former name is still accepted: a release could
            // be built by an older script, and a client that finds nothing it
            // recognises would silently stop offering updates.
            if (name == "ReaperAccessible-PromoAccess-Installer.exe"
                || name == "PromoAccessInstaller.exe")
            {
                info.installerUrl = url;
                break;
            }

            if (info.installerUrl.empty())
                info.installerUrl = url;
        }
    }

    if (info.installerUrl.empty())
    {
        info.error = loc::tr("This release has no Windows installer.",
                             "Cette version n'a pas d'installeur Windows.");
        return info;
    }

    info.available = isNewer(info.latestVersion, PROMO_VERSION_STR);
    return info;
}

wxString download(const std::string& url,
                  const std::function<void(size_t, size_t)>& progress)
{
    if (!http::isSafeUrl(url))
        return {};

    std::string bytes;
    const http::GetResult result =
        http::getToString(wxString::FromUTF8(url), bytes, nullptr, progress);

    if (!result.ok || bytes.empty())
        return {};

    // An installer is a few megabytes; anything wildly outside that is not the
    // file we asked for and must not be run.
    if (bytes.size() < 256 * 1024 || bytes.size() > 200 * 1024 * 1024)
        return {};

    // "MZ" — refuse to hand the shell something that is not an executable, such
    // as an error page served with a 200.
    if (bytes.compare(0, 2, "MZ") != 0)
        return {};

    const wxString path = wxFileName::CreateTempFileName(
        wxStandardPaths::Get().GetTempDir() + wxFileName::GetPathSeparator()
        + "PromoAccess-Setup-");

    if (path.empty())
        return {};

    // Renamed to .exe: CreateTempFileName makes a .tmp, and the shell refuses to
    // run that as a program.
    wxFileName named(path);
    named.SetExt("exe");
    const wxString exePath = named.GetFullPath();

    wxFile file;
    if (!file.Create(exePath, /*overwrite=*/true)
        || file.Write(bytes.data(), bytes.size()) != bytes.size())
    {
        wxRemoveFile(path);
        return {};
    }

    file.Close();
    wxRemoveFile(path);   // the empty .tmp CreateTempFileName reserved
    return exePath;
}

bool isInstalled()
{
    // Written by the installer beside the executable. Its absence means somebody
    // unpacked this copy by hand, and replacing it with an installer run would
    // put the program somewhere they did not choose.
    wxFileName marker(wxStandardPaths::Get().GetExecutablePath());
    marker.SetFullName("installed.txt");
    return marker.FileExists();
}

void apply(wxWindow* parent, const wxString& installerPath)
{
    // SEE_MASK_NOASYNC so the installer has genuinely started before this
    // process leaves. When it needs elevation the consent prompt appears here;
    // if the user declines, the call fails and the application simply stays
    // open instead of quitting into nothing.
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask  = SEE_MASK_NOASYNC;
    sei.lpVerb = L"open";
    sei.lpFile = installerPath.wc_str();

    // /SILENT                 no wizard, but a progress window a reader can see
    // /SUPPRESSMSGBOXES       answer prompts rather than block on a dialog
    // /FORCECLOSEAPPLICATIONS let the Restart Manager close this program
    // /NORESTART              never reboot the machine
    // /AUTOUPDATE=1           our own flag; the script relaunches us with it
    sei.lpParameters = L"/SILENT /SUPPRESSMSGBOXES /FORCECLOSEAPPLICATIONS /NORESTART /AUTOUPDATE=1";
    sei.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei))
    {
        wxMessageBox(loc::tr("The installer could not be started. The update was not applied.",
                             "L'installeur n'a pas pu être lancé. La mise à jour n'a pas été faite."),
                     loc::tr("Update", "Mise à jour"), wxOK | wxICON_ERROR, parent);
        return;
    }

    // Close so the file being replaced is no longer locked. Destroying the
    // window runs the normal shutdown — the workers are joined and the cache is
    // closed — which is why this is not an ExitProcess.
    if (parent != nullptr)
        parent->Close(/*force=*/true);
}

} // namespace updater
