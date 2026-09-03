#include "App.h"
#include "MainWindow.h"
#include "Database.h"
#include "Localization.h"
#include "Paths.h"
#include "Version.h"

#include <wx/uilocale.h>

wxIMPLEMENT_APP(PromoApp);

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

    return true;
}
