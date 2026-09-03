#include "App.h"
#include "MainWindow.h"
#include "Database.h"
#include "Localization.h"
#include "Paths.h"
#include "Version.h"

#include <wx/cmdline.h>
#include <wx/uilocale.h>

#include <windows.h>

wxIMPLEMENT_APP(PromoApp);

void PromoApp::OnInitCmdLine(wxCmdLineParser& parser)
{
    wxApp::OnInitCmdLine(parser);

    // Nothing on the command line may ever stop this program from opening.
    //
    // wxApp parses the command line before OnInit runs and refuses what it was
    // not told about: the installer relaunches us with "/fromupdate" at the end
    // of an automatic update, and wx answered "Unknown option 'fromupdate'" and
    // quit — the update finished with the application never coming back.
    //
    // Declaring the switch is not enough. wx wants "--name" for a long option;
    // with a single slash it reads the word as a cluster of short switches and
    // refuses it just the same. So the slash stops being a switch character at
    // all, and anything left over is swallowed as an optional parameter. The
    // one flag we care about is then read from the raw arguments below.
    //
    // A slashed word and a bare path are therefore harmless now; an undeclared
    // "--word" still stops the program, which is what a command line is
    // supposed to do and is not a shape anything in this chain produces.
    parser.SetSwitchChars("-");
    parser.AddParam(wxEmptyString, wxCMD_LINE_VAL_STRING,
                    wxCMD_LINE_PARAM_OPTIONAL | wxCMD_LINE_PARAM_MULTIPLE);

    // And the dashed spelling as a real switch, so "--fromupdate" is understood
    // too rather than becoming an undeclared long option, which is refused.
    parser.AddSwitch(wxEmptyString, "fromupdate",
                     "relaunched by the installer after an automatic update");
}

bool PromoApp::OnCmdLineParsed(wxCmdLineParser& parser)
{
    // Read from argv rather than from the parser, so every spelling the
    // installer might use is understood: /fromupdate, -fromupdate, --fromupdate.
    for (int n = 1; n < argc; ++n)
    {
        wxString arg(argv[n]);
        arg.MakeLower();

        while (!arg.empty() && (arg[0] == '/' || arg[0] == '-'))
            arg.Remove(0, 1);

        if (arg == "fromupdate")
        {
            fromUpdate_ = true;
            break;
        }
    }

    return wxApp::OnCmdLineParsed(parser);
}

bool PromoApp::OnInit()
{
    if (!wxApp::OnInit())
        return false;

    // Adopt the system UI language before the first loc:: call, which resolves
    // and caches it. Without this, wxUILocale reports the neutral "C" locale and
    // a French Windows would get an English interface.
    wxUILocale::UseDefault();

    SetAppName(PROMO_APP_NAME);
    SetVendorName(PROMO_PUBLISHER);

    // The user's own language choice, if they made one, wins over the system.
    // Read here rather than in the window, because every label in that window is
    // translated while it is being built — by the time the window can read its
    // own settings, it has already been built in the wrong language.
    {
        db::Database settings;
        std::string error;
        if (settings.open(paths::databaseFile(), error))
        {
            const std::string saved = settings.setting("language");
            if (saved == "fr")
                loc::setLanguage(loc::Language::French);
            else if (saved == "en")
                loc::setLanguage(loc::Language::English);
        }
    }

    // The title goes straight to the HWND caption, which is what a screen reader
    // reads when the window is activated or reached with Alt+Tab.
    auto* frame = new MainWindow(PROMO_WINDOW_TITLE);
    frame->Show(true);
    SetTopWindow(frame);

    // Relaunched by the installer at the end of an automatic update. Windows
    // gives a process started by another one no right to the foreground, so the
    // window would come up behind the installer's last screen and the screen
    // reader would be reading something the user is no longer in. Asked for
    // explicitly here, and only on that path.
    if (fromUpdate_)
    {
        frame->Raise();

        if (WXWidget handle = frame->GetHandle())
            ::SetForegroundWindow(static_cast<HWND>(handle));

        frame->SetFocus();
    }

    return true;
}
