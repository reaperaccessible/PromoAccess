#include "MainWindow.h"
#include "FavoriteDialog.h"
#include "Format.h"
#include "Export.h"
#include "Localization.h"
#include "Paths.h"
#include "Updater.h"
#include "FlippSource.h"
#include "Http.h"
#include "Locality.h"
#include "Postal.h"
#include "Text.h"
#include "Version.h"

#include <wx/app.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/listctrl.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/textctrl.h>
#include <wx/stattext.h>
#include <wx/sizer.h>
#include <wx/msgdlg.h>
#include <wx/progdlg.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/accel.h>
#include <wx/dialog.h>
#include <wx/utils.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <set>

#define NOMINMAX
#include <windows.h>
#include <uiautomation.h>
#include <oleauto.h>

namespace
{
    // Minimal UIA provider for the frame's HWND. Its only job is to exist, so
    // WM_GETOBJECT can hand back a real UIA root and the notifications we raise
    // reach NVDA and JAWS. Content itself is still read through the MSAA bridge.
    class FrameUiaProvider : public IRawElementProviderSimple
    {
    public:
        explicit FrameUiaProvider(HWND hwnd) : hwnd_(hwnd) {}

        ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
        ULONG STDMETHODCALLTYPE Release() override
        {
            const LONG r = InterlockedDecrement(&ref_);
            if (r == 0) delete this;
            return r;
        }
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
        {
            if (ppv == nullptr) return E_INVALIDARG;
            if (riid == __uuidof(IUnknown) || riid == __uuidof(IRawElementProviderSimple))
            {
                *ppv = static_cast<IRawElementProviderSimple*>(this);
                AddRef();
                return S_OK;
            }
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* p) override
        {
            *p = static_cast<ProviderOptions>(ProviderOptions_ServerSideProvider
                                            | ProviderOptions_UseComThreading);
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID, IUnknown** p) override
        {
            *p = nullptr;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID id, VARIANT* p) override
        {
            p->vt = VT_EMPTY;
            if (id == UIA_ControlTypePropertyId) { p->vt = VT_I4; p->lVal = UIA_PaneControlTypeId; }
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** p) override
        {
            return UiaHostProviderFromHwnd(hwnd_, p);
        }

    private:
        LONG ref_ = 1;
        HWND hwnd_;
    };

    enum
    {
        ID_SYNC = wxID_HIGHEST + 1,
        ID_SEARCH,
        ID_ADD_TO_LIST,
        ID_FAVORITE_NEW, ID_FAVORITE_EDIT, ID_FAVORITE_DELETE, ID_FAVORITE_FROM_ITEM,
        ID_LIST_REMOVE, ID_LIST_CLEAR, ID_LIST_QUANTITY, ID_LIST_EXPORT,
        ID_DETAIL, ID_PRODUCT_PAGE,
        ID_TAB_1, ID_TAB_2, ID_TAB_3, ID_TAB_4, ID_TAB_5,
        ID_MERCHANT_TOGGLE, ID_HELP
    };

    // Tab order. Settings comes first because it is the tab that has to be
    // visited before any other one can show anything, and because a first run
    // starts there. The enum exists so a reorder is one edit here instead of a
    // hunt for bare indices scattered through the event handlers.
    enum Page { PageSettings = 0, PageFlyers, PageSearch, PageFavorites, PageList };

    const char* kSettingPostal    = "postal_code";
    const char* kSettingLanguage  = "language";
    const char* kSettingWeek      = "week";
    const char* kSettingNormRev   = "norm_revision";

    // Bumped whenever text::normalize changes. The stored index was written by
    // the old rules and is unreachable by the new ones until it is rebuilt.
    const char* kNormRevision     = "3";
    const char* kSettingPriceless = "hide_priceless";
    const char* kSettingSort      = "sort_order";
    const char* kSettingUpdates   = "check_updates";
    const char* kSettingMatchBan  = "match_merchant";
    const char* kSettingListBan   = "list_merchant";

    // Dropdown order, matching db::Week.
    wxChoice* makeWeekChoice(wxWindow* parent)
    {
        auto* choice = new wxChoice(parent, wxID_ANY);
        choice->SetName(loc::tr("Week", "Semaine"));
        choice->Append(loc::tr("This week", "Cette semaine"));
        choice->Append(loc::tr("Next week", "Semaine prochaine"));
        choice->Append(loc::tr("Both weeks", "Les deux semaines"));
        return choice;
    }

    int weekIndex(db::Week week)
    {
        switch (week)
        {
            case db::Week::Current: return 0;
            case db::Week::Next:    return 1;
            default:                return 2;
        }
    }

    db::Week weekFromIndex(int index)
    {
        switch (index)
        {
            case 0:  return db::Week::Current;
            case 1:  return db::Week::Next;
            default: return db::Week::Both;
        }
    }

    // One cache entry per postal code, kept for good: the place a postal code
    // sits in does not change, and the lookup provider is rate-limited and may
    // not outlive the app. Looked up once, then read from here forever.
    std::string localityKey(const std::string& postalCode)
    {
        return "locality_" + postalCode;
    }

    wxString u8(const std::string& s) { return wxString::FromUTF8(s); }

    // Report-mode list, full-row selection, always with a name: the name is what
    // a screen reader says when focus lands on the control, before it starts
    // reading rows.
    wxListCtrl* makeList(wxWindow* parent, const wxString& name)
    {
        auto* list = new wxListCtrl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                    wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES);
        list->SetName(name);

        // Put the cursor on the first row when focus arrives with nothing
        // selected.
        //
        // A Win32 list-view does not select anything on its own: focus lands on
        // the control, no row is current, and a screen reader announces the list
        // name and then falls silent. The user has to guess that pressing Down
        // will produce content — and on a checkbox list, that no row is announced
        // does not mean there are no checkboxes. A sighted user sees the rows
        // immediately; this is what makes them audible.
        list->Bind(wxEVT_SET_FOCUS, [list](wxFocusEvent& event)
        {
            if (list->GetItemCount() > 0
                && list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED) == -1)
            {
                list->SetItemState(0, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                                      wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
            }
            event.Skip();
        });

        return list;
    }

    void addColumn(wxListCtrl* list, const wxString& title, int width)
    {
        list->AppendColumn(title, wxLIST_FORMAT_LEFT, width);
    }

    // A read-only text field that Tab still stops on.
    //
    // wxTextCtrl::AcceptsFocusFromKeyboard() returns false once the control is
    // read-only, so a plain wxTE_READONLY field is skipped by Tab entirely. That
    // is a reasonable default for a field nobody can change — and exactly wrong
    // for one whose whole purpose is to be read, since a screen-reader user
    // reaches text by tabbing to it. A sighted user gets this information by
    // glancing at it; this override is how everyone else gets it.
    class ReadOnlyField : public wxTextCtrl
    {
    public:
        ReadOnlyField(wxWindow* parent, wxWindowID id,
                      const wxString& value = wxEmptyString,
                      const wxPoint& pos = wxDefaultPosition,
                      const wxSize& size = wxDefaultSize,
                      long style = 0)
            : wxTextCtrl(parent, id, value, pos, size, style | wxTE_READONLY)
        {
            // Windows selects the whole content of an edit field reached with
            // Tab, and a screen reader then says "selected" before the value —
            // a word that means nothing here, since there is nothing to cut,
            // copy or replace in a field you cannot type into.
            //
            // Undone after the default handler rather than instead of it:
            // the selection is made by that handler, so clearing it first would
            // clear nothing.
            Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& e)
            {
                e.Skip();
                CallAfter([this] { SetSelection(0, 0); });
            });
        }

        bool AcceptsFocusFromKeyboard() const override { return true; }
    };

    // Adds the caption for the control that is created NEXT.
    //
    // Order matters and is not a style question: Windows derives a control's
    // accessible name from the static text that precedes it in z-order, which is
    // creation order. Creating the label after its control shifts every name by
    // one, so the flyer list ends up announced as "Banner" and the item list as
    // "Flyers" - each wearing its neighbour's label. Always: label, then control.
    wxStaticText* addLabel(wxWindow* parent, wxSizer* sizer, const wxString& text)
    {
        auto* label = new wxStaticText(parent, wxID_ANY, text);
        sizer->Add(label, 0, wxLEFT | wxRIGHT | wxTOP, parent->FromDIP(6));
        return label;
    }
}

//==============================================================================
MainWindow::MainWindow(const wxString& title)
    : wxFrame(nullptr, wxID_ANY, title, wxDefaultPosition, wxSize(900, 620))
    , sync_(this)
{
    // The HWND exists by now (the base constructor made it), so the provider can
    // bind to it immediately.
    uiaProvider_ = new FrameUiaProvider(reinterpret_cast<HWND>(GetHandle()));

    std::string error;
    if (!db_.open(paths::databaseFile(), error))
    {
        wxMessageBox(loc::tr("The local cache could not be opened. PromoAccess will run, "
                             "but nothing will be remembered between sessions.",
                             "Le cache local n'a pas pu être ouvert. PromoAccess fonctionnera, "
                             "mais rien ne sera conservé entre les sessions.")
                     + "\n\n" + u8(error),
                     title, wxOK | wxICON_WARNING, this);
    }

    // Every read below is limited to the saved postal code, set unconditionally,
    // empty code included. An empty scope means "read
    // every region" — the right default for the console harness, and exactly the
    // wrong one here: before the user has entered a postal code, it would show
    // whatever region happened to be in the cache as though it were theirs. A
    // scope of one empty code matches nothing, so the lists stay empty and the
    // welcome message below says what to do.
    db_.setScope({ postal::canonical(db_.setting(kSettingPostal)) });
    {
        std::lock_guard<std::mutex> lock(detailMutex_);
        postalCodeForWorker_ = postal::canonical(db_.setting(kSettingPostal));
    }

    // A corrected normalizer only reaches rows written after it, so the index is
    // rebuilt once when the rules change.
    if (db_.setting(kSettingNormRev) != kNormRevision)
    {
        db_.rebuildSearchIndex();
        db_.setSetting(kSettingNormRev, kNormRevision);
    }

    // Filters are preferences: they survive the session that set them.
    db_.setWeek(weekFromIndex(std::atoi(db_.setting(kSettingWeek, "2").c_str())));

    // Read before the pages are built, so every dropdown is created already
    // showing the stored order instead of flickering to it afterwards.
    {
        const int stored = std::atoi(db_.setting(kSettingSort, "0").c_str());
        sort_ = (stored >= 0 && stored <= 3) ? static_cast<Sort>(stored) : Sort::Price;
    }
    db_.setHidePriceless(db_.setting(kSettingPriceless) == "1");

    buildLayout();
    // The taskbar, Alt+Tab and the window corner take their icon from the frame,
    // not from the executable's resource table, so it has to be set explicitly.
    // wxICON names the resource declared in PromoAccess.rc.
    SetIcon(wxICON(AppIcon));

    buildAccelerators();

    Bind(EVT_SYNC_PROGRESS, &MainWindow::onSyncProgress, this);
    Bind(EVT_SYNC_DONE, &MainWindow::onSyncDone, this);
    Bind(wxEVT_CLOSE_WINDOW, &MainWindow::onClose, this);

    syncWeekChoices();
    reloadMerchants();
    reloadFlyers();
    reloadFavorites();
    reloadList();
    refreshLocation();

    // Resolve the saved postal code on the way in, so the location line is
    // already right the first time the user tabs to it rather than only after a
    // sync. No-op when it is already cached, which it is after the first run.
    requestLocality(postalCode());

    // A first run has no cache and no followed banners; say what to do rather
    // than presenting five empty lists with no explanation.
    if (db_.followedCount() == 0)
    {
        book_->SetSelection(PageSettings);
        suppressPostalHint_ = true;
        postalField_->SetFocus();
        announce(loc::tr("Welcome. Enter your postal code, choose your banners, then press F5 to sync.",
                         "Bienvenue. Entrez votre code postal, choisissez vos bannières, puis F5 pour synchroniser."));
    }

    for (int page = 0; page < 5; ++page)
        if (primaryLabels_[page] != nullptr)
            primaryLabelText_[page] = primaryLabels_[page]->GetLabel();

    tabNameTimer_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { clearTabNameOverrides(); });

    announceTimer_.Bind(wxEVT_TIMER, [this](wxTimerEvent&)
    {
        announce(pendingAnnounce_);
        pendingAnnounce_.clear();
    });

    // Three seconds after the window is up, exactly as MediaAccess does it: the
    // first moments belong to the screen reader announcing the window, and a
    // dialog that lands in the middle of that is a dialog nobody heard open.
    updateCheckTimer_.Bind(wxEVT_TIMER, [this](wxTimerEvent&)
    {
        startUpdateCheck(/*silent=*/true);
    });

    if (db_.setting(kSettingUpdates, "1") == "1")
        updateCheckTimer_.StartOnce(3000);

    layoutReady_ = true;
}

void MainWindow::announceTabOnFocus(int page)
{
    if (book_ == nullptr || page < 0 || page >= 5)
        return;

    wxStaticText* label = primaryLabels_[page];
    if (label == nullptr)
        return;

    // Windows builds a control's accessible name from the static text in front
    // of it. Not from wxWindow::SetName, and not from wxWindow::SetAccessible
    // either: wxWidgets 3.3 never handles WM_GETOBJECT, so an accessibility
    // object handed to it is stored and never asked. Rewriting the label is
    // therefore the only way to change what the reader says.
    //
    // Folded into the name rather than spoken as a separate notification,
    // because two announcements race: the tab name would arrive after the
    // control as often as before it. One name, read once, in the right order.
    const wxString tab = book_->GetPageText(static_cast<size_t>(page));

    // Only when it adds something. Three of the five labels already begin with
    // their tab's own word, and "Favoris. Favoris" read aloud is noise: the tab
    // name is the first thing said either way, which is the whole point.
    const std::string tabNorm   = text::normalize(tab.utf8_string());
    const std::string labelNorm = text::normalize(primaryLabelText_[page].utf8_string());

    if (labelNorm.rfind(tabNorm, 0) == 0)
        label->SetLabel(primaryLabelText_[page]);
    else
        label->SetLabel(tab + ". " + primaryLabelText_[page]);

    // Restored a moment later, so the label is its plain self again by the time
    // the user tabs back to the control by hand.
    tabNameTimer_.StartOnce(800);
}

void MainWindow::clearTabNameOverrides()
{
    for (int page = 0; page < 5; ++page)
        if (primaryLabels_[page] != nullptr)
            primaryLabels_[page]->SetLabel(primaryLabelText_[page]);
}

MainWindow::~MainWindow()
{
    sync_.stop();

    if (updateThread_.joinable())
        updateThread_.join();

    if (localityThread_.joinable())
        localityThread_.join();

    detailStop_.store(true);

    // The flags stop the loops; these interrupt whatever request is already in
    // flight, so the joins that follow return at once rather than waiting out a
    // stalled connection with the message pump stopped.
    detailCanceller_.abort();
    localityCanceller_.abort();
    if (detailThread_.joinable())
        detailThread_.join();

    if (uiaProvider_ != nullptr)
    {
        static_cast<IRawElementProviderSimple*>(uiaProvider_)->Release();
        uiaProvider_ = nullptr;
    }
}

WXLRESULT MainWindow::MSWWindowProc(WXUINT nMsg, WXWPARAM wParam, WXLPARAM lParam)
{
    if (nMsg == WM_GETOBJECT && uiaProvider_ != nullptr
        && static_cast<long>(lParam) == static_cast<long>(UiaRootObjectId))
    {
        return UiaReturnRawElementProvider(
            reinterpret_cast<HWND>(GetHandle()), wParam, lParam,
            static_cast<IRawElementProviderSimple*>(uiaProvider_));
    }
    return wxFrame::MSWWindowProc(nMsg, wParam, lParam);
}

long MainWindow::askQuantity(const wxString& itemName, int current)
{
    wxDialog dialog(this, wxID_ANY, loc::tr("Quantity", "Quantité"));

    auto* root = new wxBoxSizer(wxVERTICAL);
    const int border = FromDIP(8);

    root->Add(new wxStaticText(&dialog, wxID_ANY, itemName), 0, wxALL, border);

    root->Add(new wxStaticText(&dialog, wxID_ANY,
                               loc::tr("How many? (1 to 99)", "Combien ? (1 à 99)")),
              0, wxLEFT | wxRIGHT, border);

    auto* field = new wxTextCtrl(&dialog, wxID_ANY, wxString::Format("%d", current),
                                 wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    field->SetName(loc::tr("Quantity", "Quantité"));
    field->SetMaxLength(2);
    root->Add(field, 0, wxEXPAND | wxALL, border);

    root->Add(dialog.CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, border);
    loc::translateStockButtons(&dialog);

    field->Bind(wxEVT_TEXT_ENTER, [&dialog](wxCommandEvent&) { dialog.EndModal(wxID_OK); });

    dialog.SetSizerAndFit(root);
    dialog.CentreOnParent();
    field->SetFocus();
    field->SelectAll();

    if (dialog.ShowModal() != wxID_OK)
        return 0;

    long value = 0;
    if (!field->GetValue().Trim(true).Trim(false).ToLong(&value) || value < 1 || value > 99)
    {
        announce(loc::tr("That is not a quantity between 1 and 99.",
                         "Ce n'est pas une quantité entre 1 et 99."));
        return 0;
    }

    return value;
}

bool MainWindow::confirm(const wxString& question)
{
    // wxMessageDialog rather than wxMessageBox: only the dialog object exposes
    // SetYesNoLabels, and without it the buttons of a French question are
    // announced as "Yes" and "No".
    wxMessageDialog dialog(this, question, GetTitle(), wxYES_NO | wxICON_QUESTION);
    dialog.SetYesNoLabels(loc::tr("Yes", "Oui"), loc::tr("No", "Non"));

    return dialog.ShowModal() == wxID_YES;
}

void MainWindow::announce(const wxString& text)
{
    if (text.empty() || uiaProvider_ == nullptr)
        return;



    BSTR message = SysAllocString(text.wc_str());
    if (message == nullptr)
        return;

    UiaRaiseNotificationEvent(static_cast<IRawElementProviderSimple*>(uiaProvider_),
                              NotificationKind_Other,
                              NotificationProcessing_All,
                              message, /*activityId=*/nullptr);
    SysFreeString(message);
}

//==============================================================================
// Layout
//==============================================================================
void MainWindow::buildLayout()
{
    book_ = new wxNotebook(this, wxID_ANY);

    book_->AddPage(buildSettingsPage(book_),  loc::tr("Settings", "Réglages"));
    book_->AddPage(buildFlyersPage(book_),    loc::tr("Flyers", "Circulaires"), true);
    book_->AddPage(buildSearchPage(book_),    loc::tr("Search", "Recherche"));
    book_->AddPage(buildFavoritesPage(book_), loc::tr("Favorites", "Favoris"));
    book_->AddPage(buildListPage(book_),      loc::tr("Shopping list", "Liste d'épicerie"));

    book_->Bind(wxEVT_NOTEBOOK_PAGE_CHANGED, &MainWindow::onPageChanged, this);

    // Shift+Tab off the tab strip.
    //
    // The strip is a legitimate stop — it is where a reader arrows between tabs
    // — but it is the FIRST control of the window, so backward navigation from
    // it has nowhere to go and wxWidgets simply leaves the focus where it is.
    // Pressing Shift+Tab then does nothing at all, repeatedly, which to someone
    // who cannot see the screen is indistinguishable from a frozen application.
    // Sent instead to the last control of the current page, which is where
    // "backwards" means anything.
    book_->Bind(wxEVT_NAVIGATION_KEY, [this](wxNavigationKeyEvent& e)
    {
        if (e.GetDirection() || FindFocus() != book_)
        {
            e.Skip();
            return;
        }

        wxWindow* page = book_->GetCurrentPage();
        if (page == nullptr)
        {
            e.Skip();
            return;
        }

        if (wxWindow* last = lastFocusable(page))
            last->SetFocus();
        else
            e.Skip();
    });

    auto* root = new wxBoxSizer(wxVERTICAL);
    root->Add(book_, 1, wxEXPAND | wxALL, FromDIP(4));
    SetSizer(root);

    CreateStatusBar();
    SetStatusText(loc::tr("F5 syncs, Ctrl+1 to Ctrl+5 switch tabs.",
                          "F5 synchronise, Ctrl+1 à Ctrl+5 changent d'onglet."));

    // Settings is built first but Flyers is what opens: the tab a returning user
    // wants is the one with this week's deals, not the one they configured once.
    book_->SetSelection(PageFlyers);
}

wxPanel* MainWindow::buildFlyersPage(wxNotebook* book)
{
    auto* page = new wxPanel(book);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    const int border = FromDIP(6);

    addLabel(page, sizer, loc::tr("Week:", "Semaine :"));
    flyerWeek_ = makeWeekChoice(page);
    sizer->Add(flyerWeek_, 0, wxEXPAND | wxLEFT | wxRIGHT, border);
    flyerWeek_->Bind(wxEVT_CHOICE, [this](wxCommandEvent& e) { applyWeek(e.GetSelection()); });

    addLabel(page, sizer, loc::tr("Banner:", "Bannière :"));
    flyerMerchant_ = new wxChoice(page, wxID_ANY);
    flyerMerchant_->SetName(loc::tr("Banner", "Bannière"));
    sizer->Add(flyerMerchant_, 0, wxEXPAND | wxLEFT | wxRIGHT, border);
    flyerMerchant_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { reloadFlyers(); });

    primaryLabels_[PageFlyers] = addLabel(page, sizer, loc::tr("Flyers:", "Circulaires :"));
    flyerList_ = makeList(page, loc::tr("Flyers", "Circulaires"));
    addColumn(flyerList_, loc::tr("Banner", "Bannière"), FromDIP(140));
    // A banner publishes several flyers at once — next week's is out before this
    // week's expires, the main circular comes with smaller inserts, and chains
    // like Metro and IGA run a separate prepared-meals book. Without its title,
    // two rows would read as the identical "Super C, 9 September" and there
    // would be no way to tell which one to open.
    addColumn(flyerList_, loc::tr("Title", "Titre"), FromDIP(190));
    addColumn(flyerList_, loc::tr("Items", "Articles"), FromDIP(80));
    addColumn(flyerList_, loc::tr("Valid until", "Valide jusqu'au"), FromDIP(180));
    sizer->Add(flyerList_, 1, wxEXPAND | wxLEFT | wxRIGHT, border);
    flyerList_->Bind(wxEVT_LIST_ITEM_SELECTED, &MainWindow::onFlyerSelected, this);

    // The sort control always sits immediately above the list it reorders, on
    // all three pages, so there is one place to look for it and never a doubt
    // about which list it governs.
    addLabel(page, sizer, loc::tr("Sort by:", "Trier par :"));
    sortChoices_[0] = makeSortChoice(page);
    sizer->Add(sortChoices_[0], 0, wxEXPAND | wxLEFT | wxRIGHT, border);

    addLabel(page, sizer, loc::tr("Items in the selected flyer:",
                                  "Articles de la circulaire choisie :"));
    flyerItemList_ = makeList(page, loc::tr("Items", "Articles"));
    addColumn(flyerItemList_, loc::tr("Item", "Article"), FromDIP(360));
    addColumn(flyerItemList_, loc::tr("Price", "Prix"), FromDIP(120));
    addColumn(flyerItemList_, loc::tr("Banner", "Bannière"), FromDIP(140));
    addColumn(flyerItemList_, loc::tr("Valid until", "Valide jusqu'au"), FromDIP(160));
    sizer->Add(flyerItemList_, 2, wxEXPAND | wxLEFT | wxRIGHT, border);

    // Enter on an item is the fast path: it is the one action anybody wants on a
    // product they just heard. Note it does NOT work from the details field
    // below: a multi-line edit swallows Return.
    flyerItemList_->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent&)
    {
        addSelectedToList(flyerItemList_, flyerItems_);
    });

    flyerItemList_->Bind(wxEVT_LIST_ITEM_SELECTED,
                         [this](wxListEvent& e) { scheduleDetailPrefetch(); e.Skip(); });

    addDetailPane(0, page, sizer, flyerItemList_, &flyerItems_);

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* add = new wxButton(page, ID_ADD_TO_LIST,
                             loc::tr("Add to list", "Ajouter à la liste"));
    auto* watch = new wxButton(page, ID_FAVORITE_FROM_ITEM,
                               loc::tr("Add to favorites", "Ajouter aux favoris"));
    auto* detail = new wxButton(page, ID_DETAIL,
                                loc::tr("Details (Ctrl+D)", "Détails (Ctrl+D)"));
    // Appended last, so it costs no extra tab stop on the way from the item list
    // to "Add to list" — the action taxed least should be the one used most.
    auto* product = new wxButton(page, ID_PRODUCT_PAGE,
                                 loc::tr("Product page (", "Fiche produit (")
                                     + fmt::productPageKey() + ")");
    buttons->Add(add, 0, wxRIGHT, border);
    buttons->Add(watch, 0, wxRIGHT, border);
    buttons->Add(detail, 0, wxRIGHT, border);
    buttons->Add(product, 0);
    sizer->Add(buttons, 0, wxALL, border);

    add->Bind(wxEVT_BUTTON, [this](wxCommandEvent&)
    {
        addSelectedToList(flyerItemList_, flyerItems_);
    });

    page->SetSizer(sizer);
    return page;
}

wxPanel* MainWindow::buildSearchPage(wxNotebook* book)
{
    auto* page = new wxPanel(book);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    const int border = FromDIP(6);

    primaryLabels_[PageSearch] = addLabel(page, sizer, loc::tr("Search for:", "Rechercher :"));
    searchField_ = new wxTextCtrl(page, wxID_ANY, wxEmptyString,
                                  wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    searchField_->SetName(loc::tr("Search for", "Rechercher"));
    sizer->Add(searchField_, 0, wxEXPAND | wxLEFT | wxRIGHT, border);
    searchField_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { runSearch(); });

    addLabel(page, sizer, loc::tr("Banner:", "Bannière :"));
    searchMerchant_ = new wxChoice(page, wxID_ANY);
    searchMerchant_->SetName(loc::tr("Banner", "Bannière"));
    sizer->Add(searchMerchant_, 0, wxEXPAND | wxLEFT | wxRIGHT, border);

    addLabel(page, sizer, loc::tr("Week:", "Semaine :"));
    searchWeek_ = makeWeekChoice(page);
    sizer->Add(searchWeek_, 0, wxEXPAND | wxLEFT | wxRIGHT, border);
    searchWeek_->Bind(wxEVT_CHOICE, [this](wxCommandEvent& e) { applyWeek(e.GetSelection()); });

    addLabel(page, sizer, loc::tr("Maximum price (leave empty for any price):",
                                  "Prix maximum (vide pour tout prix) :"));
    searchMaxPrice_ = new wxTextCtrl(page, wxID_ANY, wxEmptyString,
                                     wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    searchMaxPrice_->SetName(loc::tr("Maximum price", "Prix maximum"));
    sizer->Add(searchMaxPrice_, 0, wxEXPAND | wxLEFT | wxRIGHT, border);
    searchMaxPrice_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { runSearch(); });

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* go = new wxButton(page, ID_SEARCH, loc::tr("Search", "Rechercher"));
    auto* add = new wxButton(page, wxID_ANY, loc::tr("Add to list", "Ajouter à la liste"));
    auto* watch = new wxButton(page, wxID_ANY, loc::tr("Add to favorites", "Ajouter aux favoris"));
    auto* detail = new wxButton(page, ID_DETAIL,
                                loc::tr("Details (Ctrl+D)", "Détails (Ctrl+D)"));
    auto* product = new wxButton(page, ID_PRODUCT_PAGE,
                                 loc::tr("Product page (", "Fiche produit (")
                                     + fmt::productPageKey() + ")");
    buttons->Add(go, 0, wxRIGHT, border);
    buttons->Add(add, 0, wxRIGHT, border);
    buttons->Add(watch, 0, wxRIGHT, border);
    buttons->Add(detail, 0, wxRIGHT, border);
    buttons->Add(product, 0);
    sizer->Add(buttons, 0, wxALL, border);

    go->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { runSearch(); });
    add->Bind(wxEVT_BUTTON, [this](wxCommandEvent&)
    {
        addSelectedToList(searchList_, searchResults_);
    });
    watch->Bind(wxEVT_BUTTON, [this](wxCommandEvent&)
    {
        wxCommandEvent forward(wxEVT_BUTTON, ID_FAVORITE_FROM_ITEM);
        ProcessWindowEvent(forward);
    });

    addLabel(page, sizer, loc::tr("Sort by:", "Trier par :"));
    sortChoices_[1] = makeSortChoice(page);
    sizer->Add(sortChoices_[1], 0, wxEXPAND | wxLEFT | wxRIGHT, border);

    addLabel(page, sizer, loc::tr("Results:", "Résultats :"));
    searchList_ = makeList(page, loc::tr("Results", "Résultats"));
    addColumn(searchList_, loc::tr("Item", "Article"), FromDIP(360));
    addColumn(searchList_, loc::tr("Price", "Prix"), FromDIP(120));
    addColumn(searchList_, loc::tr("Banner", "Bannière"), FromDIP(140));
    addColumn(searchList_, loc::tr("Valid until", "Valide jusqu'au"), FromDIP(160));
    sizer->Add(searchList_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, border);

    searchList_->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent&)
    {
        addSelectedToList(searchList_, searchResults_);
    });

    searchList_->Bind(wxEVT_LIST_ITEM_SELECTED,
                      [this](wxListEvent& e) { scheduleDetailPrefetch(); e.Skip(); });

    // Last on this page: the buttons were added above the results list, so there
    // is no trailing button row to sit in front of.
    addDetailPane(1, page, sizer, searchList_, &searchResults_);

    // The buttons are drawn above the results but come AFTER them in the tab
    // chain, exactly as on the flyers tab. Reading order: what you searched for,
    // the filters, the results, the details — and only then the actions. Before
    // this, reaching "Add to favorites" from a result meant four Shift+Tabs
    // backwards past the sort and two other buttons.
    go->MoveAfterInTabOrder(detailPanes_[1].field);
    add->MoveAfterInTabOrder(go);
    watch->MoveAfterInTabOrder(add);
    detail->MoveAfterInTabOrder(watch);
    product->MoveAfterInTabOrder(detail);

    page->SetSizer(sizer);
    return page;
}

wxPanel* MainWindow::buildFavoritesPage(wxNotebook* book)
{
    auto* page = new wxPanel(book);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    const int border = FromDIP(6);

    addLabel(page, sizer, loc::tr("Favorites:", "Favoris :"));
    favoriteList_ = makeList(page, loc::tr("Favorites", "Favoris"));
    addColumn(favoriteList_, loc::tr("Words", "Mots"), FromDIP(260));
    addColumn(favoriteList_, loc::tr("Banner", "Bannière"), FromDIP(150));
    addColumn(favoriteList_, loc::tr("Under", "Sous"), FromDIP(100));
    addColumn(favoriteList_, loc::tr("Matches", "Trouvés"), FromDIP(90));
    sizer->Add(favoriteList_, 1, wxEXPAND | wxLEFT | wxRIGHT, border);

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* add    = new wxButton(page, ID_FAVORITE_NEW, loc::tr("New", "Nouveau"));
    auto* edit   = new wxButton(page, ID_FAVORITE_EDIT, loc::tr("Edit", "Modifier"));
    auto* remove = new wxButton(page, ID_FAVORITE_DELETE, loc::tr("Delete", "Supprimer"));
    buttons->Add(add, 0, wxRIGHT, border);
    buttons->Add(edit, 0, wxRIGHT, border);
    buttons->Add(remove, 0);
    sizer->Add(buttons, 0, wxALL, border);

    addLabel(page, sizer, loc::tr("Sort by:", "Trier par :"));
    sortChoices_[2] = makeSortChoice(page);
    sizer->Add(sortChoices_[2], 0, wxEXPAND | wxLEFT | wxRIGHT, border);

    // The banner comes immediately before the items it filters, so Tab reads as
    // a sentence: this store, then what it has. It is also where Ctrl+4 lands,
    // so this is the label the tab name is folded into.
    primaryLabels_[PageFavorites] = addLabel(page, sizer, loc::tr("Banner:", "Bannière :"));
    matchMerchant_ = new wxChoice(page, wxID_ANY);
    matchMerchant_->SetName(loc::tr("Banner", "Bannière"));
    sizer->Add(matchMerchant_, 0, wxEXPAND | wxLEFT | wxRIGHT, border);

    matchMerchant_->Bind(wxEVT_CHOICE, [this](wxCommandEvent& e)
    {
        const wxString chosen = e.GetSelection() <= 0
            ? wxString()
            : matchMerchant_->GetString(static_cast<unsigned>(e.GetSelection()));

        db_.setSetting(kSettingMatchBan, chosen.utf8_string());
        applyMatchFilter();
    });

    addLabel(page, sizer, loc::tr("On sale right now:", "En rabais en ce moment :"));
    favoriteMatchList_ = makeList(page, loc::tr("Matches", "Articles trouvés"));
    addColumn(favoriteMatchList_, loc::tr("Item", "Article"), FromDIP(360));
    addColumn(favoriteMatchList_, loc::tr("Price", "Prix"), FromDIP(120));
    addColumn(favoriteMatchList_, loc::tr("Banner", "Bannière"), FromDIP(140));
    addColumn(favoriteMatchList_, loc::tr("Valid until", "Valide jusqu'au"), FromDIP(160));
    sizer->Add(favoriteMatchList_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, border);

    favoriteMatchList_->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent&)
    {
        addSelectedToList(favoriteMatchList_, favoriteMatches_);
    });

    favoriteMatchList_->Bind(wxEVT_LIST_ITEM_SELECTED,
                             [this](wxListEvent& e) { scheduleDetailPrefetch(); e.Skip(); });

    favoriteList_->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent& e)
    {
        wxCommandEvent edit(wxEVT_BUTTON, ID_FAVORITE_EDIT);
        ProcessWindowEvent(edit);
        e.Skip();
    });

    // Last on this page too: New/Edit/Delete belong to the watches list above,
    // not to the matches list.
    addDetailPane(2, page, sizer, favoriteMatchList_, &favoriteMatches_);

    // Tab goes down the page and not through the buttons first.
    //
    // Reading order, not creation order: from a watch, the next thing anyone
    // wants is what that watch found, then the details of the item standing on
    // it. New, Edit and Delete are things you go looking for, so they are moved
    // to the end of the chain even though they sit between the two lists on
    // screen. MoveAfterInTabOrder rewrites the chain without touching the
    // layout.
    // The sort dropdown is drawn above the matches list but comes AFTER the
    // details field in the chain: Lee asked for the matches and their details
    // first, and a control between the two lists would sit right in the way.
    sortChoices_[2]->MoveAfterInTabOrder(detailPanes_[2].field);
    add->MoveAfterInTabOrder(sortChoices_[2]);
    edit->MoveAfterInTabOrder(add);
    remove->MoveAfterInTabOrder(edit);

    page->SetSizer(sizer);
    return page;
}

wxPanel* MainWindow::buildListPage(wxNotebook* book)
{
    auto* page = new wxPanel(book);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    const int border = FromDIP(6);

    primaryLabels_[PageList] = addLabel(page, sizer, loc::tr("Banner:", "Bannière :"));
    listMerchant_ = new wxChoice(page, wxID_ANY);
    listMerchant_->SetName(loc::tr("Banner", "Bannière"));
    sizer->Add(listMerchant_, 0, wxEXPAND | wxLEFT | wxRIGHT, border);

    listMerchant_->Bind(wxEVT_CHOICE, [this](wxCommandEvent& e)
    {
        const wxString chosen = e.GetSelection() <= 0
            ? wxString()
            : listMerchant_->GetString(static_cast<unsigned>(e.GetSelection()));

        db_.setSetting(kSettingListBan, chosen.utf8_string());
        reloadList();

        // Spoken after the banner the reader is already saying, like the sort.
        announce(listTotal_->GetValue(), 500);
    });

    addLabel(page, sizer, loc::tr("Shopping list:", "Liste d'épicerie :"));
    shoppingList_ = makeList(page, loc::tr("Shopping list", "Liste d'épicerie"));
    addColumn(shoppingList_, loc::tr("Item", "Article"), FromDIP(320));
    addColumn(shoppingList_, loc::tr("Quantity", "Quantité"), FromDIP(90));
    addColumn(shoppingList_, loc::tr("Price", "Prix"), FromDIP(120));
    addColumn(shoppingList_, loc::tr("Banner", "Bannière"), FromDIP(140));
    addColumn(shoppingList_, loc::tr("Valid until", "Valide jusqu'au"), FromDIP(160));
    sizer->Add(shoppingList_, 1, wxEXPAND | wxLEFT | wxRIGHT, border);

    // An EMPTY label, and deliberately so. On Windows a control takes its
    // announced name from the static text in front of it, so removing the label
    // outright would not leave the field nameless — it would let the list's own
    // label leak onto it, and the reader would say "Shopping list" over the
    // total. An empty one holds the place and says nothing.
    //
    // Nothing is lost: the value reads "3 items, estimated total 16.86 $",
    // which already says what it is. A caption saying "Total:" in front of it
    // was one more word before every reading of a line that is read often.
    sizer->Add(new wxStaticText(page, wxID_ANY, wxEmptyString), 0, wxLEFT | wxRIGHT, border);

    listTotal_ = new ReadOnlyField(page, wxID_ANY, wxEmptyString,
                                   wxDefaultPosition, wxDefaultSize);
    sizer->Add(listTotal_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, border);

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* quantity = new wxButton(page, ID_LIST_QUANTITY, loc::tr("Quantity", "Quantité"));
    auto* remove   = new wxButton(page, ID_LIST_REMOVE, loc::tr("Remove", "Retirer"));
    auto* clear    = new wxButton(page, ID_LIST_CLEAR, loc::tr("Clear list", "Vider la liste"));
    auto* save     = new wxButton(page, ID_LIST_EXPORT, loc::tr("Save to file", "Enregistrer"));
    buttons->Add(quantity, 0, wxRIGHT, border);
    buttons->Add(remove, 0, wxRIGHT, border);
    buttons->Add(clear, 0, wxRIGHT, border);
    buttons->Add(save, 0);
    sizer->Add(buttons, 0, wxALL, border);

    page->SetSizer(sizer);
    return page;
}

wxPanel* MainWindow::buildSettingsPage(wxNotebook* book)
{
    auto* page = new wxPanel(book);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    const int border = FromDIP(6);

    addLabel(page, sizer, loc::tr("Interface language:", "Langue de l'interface :"));

    languageChoice_ = new wxChoice(page, wxID_ANY);
    languageChoice_->SetName(loc::tr("Interface language", "Langue de l'interface"));
    // Each language is written in itself, never translated: someone who has
    // landed in the wrong language has to be able to recognize their own.
    //
    // Through u8(), like every other non-ASCII literal in this codebase. A bare
    // "Français" handed to wxString is read as Latin-1, and its two UTF-8 bytes
    // for the cedilla come out the other side as two characters.
    languageChoice_->Append(u8("Français"));
    languageChoice_->Append("English");
    languageChoice_->SetSelection(loc::isFrench() ? 0 : 1);
    sizer->Add(languageChoice_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, border);

    languageChoice_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&)
    {
        const bool french = (languageChoice_->GetSelection() == 0);
        if (french == loc::isFrench())
            return;

        db_.setSetting(kSettingLanguage, french ? "fr" : "en");

        // Deferred: the rebuild destroys the control whose event is still being
        // dispatched, which is a use-after-free if done inline.
        CallAfter([this, french] { rebuildInLanguage(french); });
    });

    primaryLabels_[PageSettings] =
        addLabel(page, sizer, loc::tr("Postal code, six characters in one block:",
                                      "Code postal, six caractères en un seul bloc :"));

    // wxTE_PROCESS_ENTER so Return reaches us instead of being eaten as the
    // dialog's default action: typing a postal code and pressing Enter has to
    // start the sync, which is what anyone will try first.
    postalField_ = new wxTextCtrl(page, wxID_ANY,
                                  wxString::FromUTF8(db_.setting(kSettingPostal)),
                                  wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    postalField_->SetName(loc::tr("Postal code", "Code postal"));
    postalField_->SetMaxLength(6);
    sizer->Add(postalField_, 0, wxEXPAND | wxLEFT | wxRIGHT, border);

    postalField_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { startSync(); });

    // Leaving the field is the moment to confirm what was typed, before any sync.
    postalField_->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e)
    {
        // Leaving before the hint fired: it would otherwise land while the
        // screen reader is announcing the control the user has just reached.
        postalHintTimer_.Stop();

        requestLocality(postalCode());
        refreshLocation();
        e.Skip();
    });

    // The expected shape is spoken on arrival rather than left in a label a
    // screen-reader user would have to go looking for. Deferred by a moment so it
    // does not collide with the reader announcing the field itself.
    postalField_->Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& e)
    {
        // Not while something has just been said. Both places that focus this
        // field announce first — the welcome message and the invalid-code
        // message — and both already state the rule, so the hint would repeat it
        // over the top of them.
        if (!suppressPostalHint_)
            postalHintTimer_.StartOnce(150);

        suppressPostalHint_ = false;
        e.Skip();
    });

    detailPrefetchTimer_.Bind(wxEVT_TIMER,
                              [this](wxTimerEvent&) { prefetchDetailsAroundCursor(); });

    postalHintTimer_.Bind(wxEVT_TIMER, [this](wxTimerEvent&)
    {
        // Only when the field does not already hold a valid code. Now that
        // Ctrl+1 lands here, the hint would otherwise be recited on every single
        // visit to Settings — help the first time, nagging the tenth. Someone
        // whose postal code is already entered does not need the rule restated.
        if (postal::isValid(postalField_->GetValue().utf8_string()))
            return;

        announce(loc::tr("Six characters in one block, no space, for example J3P7S7. "
                         "Press Enter or F5 to sync.",
                         "Six caractères en un seul bloc, sans espace, par exemple J3P7S7. "
                         "Entrée ou F5 pour synchroniser."));
    });

    addLabel(page, sizer, loc::tr("Location:", "Emplacement :"));

    // Read-only rather than a static text: wxTE_READONLY keeps the control in the
    // tab order and readable, so Tab from the postal code lands on it and the
    // screen reader says it. A wxStaticText would be skipped by Tab entirely,
    // which is exactly the information a sighted user gets for free by looking.
    locationField_ = new ReadOnlyField(page, wxID_ANY, wxEmptyString,
                                       wxDefaultPosition, wxDefaultSize, wxTE_READONLY);
    locationField_->SetName(loc::tr("Location", "Emplacement"));
    sizer->Add(locationField_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, border);

    addLabel(page, sizer, loc::tr("Banners to follow (space to check):",
                                  "Bannières à suivre (espace pour cocher) :"));
    merchantList_ = makeList(page, loc::tr("Banners to follow", "Bannières à suivre"));
    addColumn(merchantList_, loc::tr("Banner", "Bannière"), FromDIP(280));
    // Native list-view checkboxes. wxCheckListBox would look identical and be
    // silent: it is owner-drawn on Windows and its ticked state is invisible to
    // MSAA and UIA, which would leave a screen-reader user unable to tell which
    // banners are already followed.
    merchantList_->EnableCheckBoxes(true);
    sizer->Add(merchantList_, 1, wxEXPAND | wxLEFT | wxRIGHT, border);

    // Written through immediately: a checkbox that only takes effect on some
    // later "Apply" is a trap when you cannot see whether it is still pending.
    auto onToggle = [this](wxListEvent& e)
    {
        // Only a real tick by the user counts; the ones CheckItem emits while
        // the list is being refilled must be ignored. See populatingMerchants_.
        if (populatingMerchants_)
            return;

        const long index = e.GetIndex();
        if (index < 0 || static_cast<size_t>(index) >= merchants_.size())
            return;

        const bool followed = (e.GetEventType() == wxEVT_LIST_ITEM_CHECKED);
        const int  merchantId = merchants_[index].id;
        const wxString name = u8(merchants_[index].name);

        db_.setFollowed(merchantId, followed);
        merchants_[index].followed = followed;

        reloadFlyers();

        if (followed)
        {
            // Its flyers are already known — the flyer list is fetched whole at
            // every sync — but they hold no items yet. Showing a banner with
            // "0 items" and waiting for the user to guess that F5 is needed makes
            // the tick look broken, so the download starts here.
            announce(name + ", " + loc::tr("followed, downloading...",
                                           "suivie, téléchargement..."));
            queueSync(merchantId);
        }
        else
        {
            // Nothing is deleted: the items stay cached and simply stop being
            // read, so ticking the banner again brings them back with no network.
            announce(name + ", " + loc::tr("not followed", "non suivie"));
            refreshAllLists();
        }
    };
    merchantList_->Bind(wxEVT_LIST_ITEM_CHECKED, onToggle);
    merchantList_->Bind(wxEVT_LIST_ITEM_UNCHECKED, onToggle);

    hidePriceless_ = new wxCheckBox(page, wxID_ANY,
        loc::tr("Hide items with no price (headings and ads)",
                "Masquer les articles sans prix (titres et encarts)"));
    hidePriceless_->SetValue(db_.setting(kSettingPriceless) == "1");
    sizer->Add(hidePriceless_, 0, wxALL, border);

    hidePriceless_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&)
    {
        const bool hide = hidePriceless_->GetValue();
        db_.setSetting(kSettingPriceless, hide ? "1" : "0");
        db_.setHidePriceless(hide);

        announce(hide ? loc::tr("Items with no price hidden.", "Articles sans prix masqués.")
                      : loc::tr("Items with no price shown.", "Articles sans prix affichés."));
        refreshAllLists();
    });

    autoUpdate_ = new wxCheckBox(page, wxID_ANY,
        loc::tr("Check for updates when PromoAccess starts",
                "Vérifier les mises à jour au démarrage de PromoAccess"));
    // On unless it was turned off: a program nobody can see the version of is a
    // program that stays on an old version for years.
    autoUpdate_->SetValue(db_.setting(kSettingUpdates, "1") == "1");
    sizer->Add(autoUpdate_, 0, wxALL, border);

    autoUpdate_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&)
    {
        const bool on = autoUpdate_->GetValue();
        db_.setSetting(kSettingUpdates, on ? "1" : "0");
        announce(on ? loc::tr("Update check enabled.", "Vérification des mises à jour activée.")
                    : loc::tr("Update check disabled.", "Vérification des mises à jour désactivée."));
    });

    auto* updateButton = new wxButton(page, wxID_ANY,
        loc::tr("Check for updates now", "Vérifier les mises à jour maintenant"));
    sizer->Add(updateButton, 0, wxLEFT | wxRIGHT | wxBOTTOM, border);

    // Asked for on purpose, so it answers even when there is nothing new.
    updateButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&)
    {
        announce(loc::tr("Checking...", "Vérification..."));
        startUpdateCheck(/*silent=*/false);
    });

    syncStatus_ = new wxStaticText(page, wxID_ANY,
        loc::tr("Never synced.", "Jamais synchronisé."));
    sizer->Add(syncStatus_, 0, wxALL, border);

    syncButton_ = new wxButton(page, ID_SYNC, loc::tr("Sync now (F5)", "Synchroniser (F5)"));
    sizer->Add(syncButton_, 0, wxLEFT | wxRIGHT | wxBOTTOM, border);

    page->SetSizer(sizer);
    return page;
}

void MainWindow::buildAccelerators()
{
    const wxAcceleratorEntry entries[] =
    {
        { wxACCEL_NORMAL, WXK_F1,  ID_HELP },
        { wxACCEL_NORMAL, WXK_F5,  ID_SYNC },
        { wxACCEL_CTRL,   '1',     ID_TAB_1 },
        { wxACCEL_CTRL,   '2',     ID_TAB_2 },
        { wxACCEL_CTRL,   '3',     ID_TAB_3 },
        { wxACCEL_CTRL,   '4',     ID_TAB_4 },
        { wxACCEL_CTRL,   '5',     ID_TAB_5 },
        { wxACCEL_CTRL,   'F',     ID_SEARCH },
        { wxACCEL_CTRL,   'S',     ID_LIST_EXPORT },
        { wxACCEL_CTRL,   'D',     ID_DETAIL },
        { wxACCEL_CTRL | wxACCEL_SHIFT, 'O', ID_PRODUCT_PAGE },
        // Add the selected item to the favourites, from any list that holds
        // items. The button for it sits several stops away on two of the three
        // tabs, and this is an action taken on the item under the cursor.
        { wxACCEL_CTRL | wxACCEL_SHIFT, 'F', ID_FAVORITE_FROM_ITEM },
    };
    SetAcceleratorTable(wxAcceleratorTable(WXSIZEOF(entries), entries));

    // Selecting the page is all these do: onPageChanged then moves the focus, so
    // the keyboard shortcut, a mouse click on the tab and Ctrl+Tab all behave the
    // same way instead of each having its own idea of where focus should go.
    for (int n = 0; n < 5; ++n)
        Bind(wxEVT_MENU, [this, n](wxCommandEvent&)
        {
            // Pressing the shortcut for the page already showing changes nothing,
            // so onPageChanged never runs and the focus stays wherever it was —
            // on the tab strip at start-up, which is exactly when someone reaches
            // for Ctrl+2. The shortcut must land in the same place every time.
            if (book_->GetSelection() == n)
            {
                announceTabOnFocus(n);
                if (wxWindow* target = primaryControl(n))
                    target->SetFocus();
                return;
            }

            book_->SetSelection(n);
        }, ID_TAB_1 + n);

    Bind(wxEVT_MENU, [this](wxCommandEvent&) { openManual(); }, ID_HELP);

    Bind(wxEVT_MENU,   [this](wxCommandEvent&) { startSync(); }, ID_SYNC);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { startSync(); }, ID_SYNC);

    Bind(wxEVT_MENU, [this](wxCommandEvent&)
    {
        book_->SetSelection(PageSearch);
        searchField_->SetFocus();
        searchField_->SelectAll();
    }, ID_SEARCH);

    Bind(wxEVT_MENU,   [this](wxCommandEvent&) { announceSelectedDetail(); }, ID_DETAIL);

    Bind(wxEVT_MENU, [this](wxCommandEvent&)
    {
        wxCommandEvent watch(wxEVT_BUTTON, ID_FAVORITE_FROM_ITEM);
        ProcessWindowEvent(watch);
    }, ID_FAVORITE_FROM_ITEM);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { announceSelectedDetail(); }, ID_DETAIL);

    Bind(wxEVT_MENU,   [this](wxCommandEvent&) { openSelectedProductPage(); }, ID_PRODUCT_PAGE);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { openSelectedProductPage(); }, ID_PRODUCT_PAGE);

    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { exportList(); }, ID_LIST_EXPORT);
    Bind(wxEVT_MENU,   [this](wxCommandEvent&)
    {
        // Ctrl+S only means "save the list" while the list tab is up; elsewhere
        // it would fire on a tab where the user has no idea what got saved.
        if (book_->GetSelection() == PageList)
            exportList();
    }, ID_LIST_EXPORT);

    // --- Favourites -----------------------------------------------------------
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&)
    {
        FavoriteDialog dialog(this, db_.merchants(true), nullptr);
        if (dialog.ShowModal() == wxID_OK)
        {
            db_.addFavorite(dialog.result());
            reloadFavorites();
            announce(loc::tr("Favorite added.", "Favori ajouté."));
        }
        favoriteList_->SetFocus();
    }, ID_FAVORITE_NEW);

    Bind(wxEVT_BUTTON, [this](wxCommandEvent&)
    {
        const long row = selectedRow(favoriteList_);
        if (row < 0 || static_cast<size_t>(row) >= favorites_.size())
        {
            announce(loc::tr("No favorite selected.", "Aucun favori sélectionné."));
            return;
        }

        FavoriteDialog dialog(this, db_.merchants(true), &favorites_[row]);
        if (dialog.ShowModal() == wxID_OK)
        {
            db_.updateFavorite(dialog.result());
            reloadFavorites();
            announce(loc::tr("Favorite updated.", "Favori modifié."));
        }
        favoriteList_->SetFocus();
    }, ID_FAVORITE_EDIT);

    Bind(wxEVT_BUTTON, [this](wxCommandEvent&)
    {
        const long row = selectedRow(favoriteList_);
        if (row < 0 || static_cast<size_t>(row) >= favorites_.size())
        {
            announce(loc::tr("No favorite selected.", "Aucun favori sélectionné."));
            return;
        }

        const wxString name = u8(favorites_[row].pattern);
        if (!confirm(loc::tr("Delete the favorite for ", "Supprimer le favori ")
                     + name + " ?"))
            return;

        db_.removeFavorite(favorites_[row].id);
        reloadFavorites();
        announce(loc::tr("Favorite deleted.", "Favori supprimé."));
        favoriteList_->SetFocus();
    }, ID_FAVORITE_DELETE);

    // Turning the selected item into a watch: the product name is offered as the
    // starting pattern, which the user then trims to the words that will still be
    // true next week.
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&)
    {
        // Whichever item list the user is standing on, and the one this tab is
        // about when they are standing on a button. Chosen by tab before, which
        // meant the favourites tab fell through to the search results — and a
        // shortcut pressed there watched the wrong item.
        std::vector<model::Item>* itemsPtr = nullptr;
        wxListCtrl* list = focusedItemList(itemsPtr);

        if (list == nullptr || itemsPtr == nullptr)
        {
            announce(loc::tr("No item selected.", "Aucun article sélectionné."));
            return;
        }

        const std::vector<model::Item>& items = *itemsPtr;

        const long row = selectedRow(list);
        if (row < 0 || static_cast<size_t>(row) >= items.size())
        {
            announce(loc::tr("No item selected.", "Aucun article sélectionné."));
            return;
        }

        model::Favorite seed;
        // One language only: a favourite requires every word to appear, and a
        // bilingual seed would demand the English half match as well.
        seed.pattern      = fmt::itemName(items[row].name).utf8_string();
        // Pre-checked on the banner the product came from, which is what the
        // user was looking at; unchecking it there widens the watch to all.
        seed.merchantIds  = { items[row].merchantId };
        seed.merchantName = items[row].merchantName;

        FavoriteDialog dialog(this, db_.merchants(true), &seed);
        if (dialog.ShowModal() == wxID_OK)
        {
            db_.addFavorite(dialog.result());
            reloadFavorites();
            announce(loc::tr("Favorite added.", "Favori ajouté."));
        }
    }, ID_FAVORITE_FROM_ITEM);

    // --- Shopping list --------------------------------------------------------
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&)
    {
        const long row = selectedRow(shoppingList_);
        if (row < 0 || static_cast<size_t>(row) >= listEntries_.size())
        {
            announce(loc::tr("Nothing selected.", "Rien de sélectionné."));
            return;
        }

        const wxString name = u8(listEntries_[row].name);
        db_.removeListEntry(listEntries_[row].id);
        reloadList();
        announce(loc::tr("Removed: ", "Retiré : ") + name);
    }, ID_LIST_REMOVE);

    // Delete arrives through a key hook and not through the accelerator table.
    // An accelerator entry for a plain, unmodified key is never translated once
    // the focus sits in a control that claims the key for itself — a list view
    // and an edit field both do — so the entry silently did nothing, in the
    // shopping list as much as in the favourites. CHAR_HOOK sees the key before
    // any of them and hands back everything it does not act on.
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e)
    {
        // Both spellings of the key. The Delete above the arrows and the Del on
        // the numeric pad reach wxWidgets as two different codes, and a user who
        // presses the one on the pad means exactly the same thing.
        const int key = e.GetKeyCode();
        if ((key != WXK_DELETE && key != WXK_NUMPAD_DELETE) || e.HasAnyModifiers())
        {
            e.Skip();
            return;
        }

        wxCommandEvent remove(wxEVT_BUTTON, ID_LIST_REMOVE);
        if (FindFocus() == favoriteList_)
            remove.SetId(ID_FAVORITE_DELETE);
        else if (FindFocus() != shoppingList_)
        {
            e.Skip();   // an edit field deletes a character, as it should
            return;
        }

        ProcessWindowEvent(remove);
    });

    Bind(wxEVT_BUTTON, [this](wxCommandEvent&)
    {
        const long row = selectedRow(shoppingList_);
        if (row < 0 || static_cast<size_t>(row) >= listEntries_.size())
        {
            announce(loc::tr("Nothing selected.", "Rien de sélectionné."));
            return;
        }

        // Not wxGetNumberFromUser: it builds its own dialog, whose OK and Cancel
        // come from a wx catalog this build does not ship, and whose entry field
        // carries no accessible name of ours.
        const long value = askQuantity(u8(listEntries_[row].name),
                                       listEntries_[row].quantity);
        if (value < 1)
            return;     // cancelled

        db_.setListQuantity(listEntries_[row].id, static_cast<int>(value));
        reloadList();
        announce(loc::tr("Quantity set.", "Quantité modifiée."));
    }, ID_LIST_QUANTITY);

    Bind(wxEVT_BUTTON, [this](wxCommandEvent&)
    {
        if (listEntries_.empty())
            return;

        if (!confirm(loc::tr("Remove every item from the list?",
                             "Retirer tous les articles de la liste ?")))
            return;

        db_.clearList();
        reloadList();
        announce(loc::tr("List cleared.", "Liste vidée."));
    }, ID_LIST_CLEAR);
}

//==============================================================================
// Data refresh
//==============================================================================
std::string MainWindow::postalCode() const
{
    // Canonical, always: "j3p 7s7", "J3P-7S7" and "J3P7S7" have to reach the
    // cache as one key, or the same flyers get filed under several zones and the
    // app looks empty after the user retypes their own postal code differently.
    // Empty when what is typed is not a postal code at all.
    const std::string typed = postalField_
        ? postalField_->GetValue().utf8_string()
        : db_.setting(kSettingPostal);

    return postal::canonical(typed);
}

long MainWindow::selectedRow(wxListCtrl* list) const
{
    return list ? list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED) : -1;
}

void MainWindow::restoreSelection(wxListCtrl* list, long wanted) const
{
    if (list == nullptr || list->GetItemCount() == 0)
        return;

    // Nothing was selected and the list is not where the user is: leave it
    // alone, so a background refresh cannot make a list announce a row the user
    // never asked for.
    if (wanted < 0 && wxWindow::FindFocus() != list)
        return;

    const long last = list->GetItemCount() - 1;
    const long row  = wanted < 0 ? 0 : (wanted > last ? last : wanted);

    list->SetItemState(row, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                            wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
    list->EnsureVisible(row);
}

void MainWindow::applyWeek(int selection)
{
    db_.setWeek(weekFromIndex(selection));
    db_.setSetting(kSettingWeek, std::to_string(selection));

    syncWeekChoices();
    refreshAllLists();

    // Say the count, not the setting: the dropdown already announced itself, and
    // what the user actually wants to know is how much is left to walk through.
    announce(wxString::Format("%zu %s", flyers_.size(),
                              flyers_.size() == 1 ? loc::tr("flyer", "circulaire")
                                                  : loc::tr("flyers", "circulaires")));
}

wxChoice* MainWindow::makeSortChoice(wxWindow* parent)
{
    auto* choice = new wxChoice(parent, wxID_ANY);
    choice->SetName(loc::tr("Sort by", "Trier par"));
    choice->Append(loc::tr("Price, lowest first",     "Prix, du plus bas"));
    choice->Append(loc::tr("Name, A to Z",           "Nom, de A à Z"));
    choice->Append(loc::tr("Discount, highest first", "Rabais, du plus élevé"));
    choice->Append(loc::tr("Brand",                   "Marque"));
    choice->SetSelection(static_cast<int>(sort_));
    choice->Bind(wxEVT_CHOICE, [this](wxCommandEvent& e) { applySort(e.GetSelection()); });
    return choice;
}

void MainWindow::syncSortChoices()
{
    for (wxChoice* choice : sortChoices_)
        if (choice != nullptr)
            choice->SetSelection(static_cast<int>(sort_));
}

void MainWindow::sortItems(std::vector<model::Item>& items) const
{
    // std::stable_sort throughout: two items that tie keep the order they came
    // in with, which is the flyer's own. An unstable sort would reshuffle a list
    // on every refresh and move a row out from under the cursor.
    switch (sort_)
    {
        case Sort::Name:
            std::stable_sort(items.begin(), items.end(),
                [](const model::Item& a, const model::Item& b)
                {
                    // Folded, so "Émincé" files under E and not after Z.
                    return text::normalize(fmt::itemName(a.name).utf8_string())
                         < text::normalize(fmt::itemName(b.name).utf8_string());
                });
            break;

        case Sort::Discount:
            std::stable_sort(items.begin(), items.end(),
                [](const model::Item& a, const model::Item& b)
                {
                    // Items the banner said nothing about go last, never among
                    // the zeroes: "no percentage given" is not "no discount".
                    if ((a.discountPercent < 0) != (b.discountPercent < 0))
                        return b.discountPercent < 0;

                    return a.discountPercent > b.discountPercent;
                });
            break;

        case Sort::Brand:
            std::stable_sort(items.begin(), items.end(),
                [](const model::Item& a, const model::Item& b)
                {
                    if (a.brand.empty() != b.brand.empty())
                        return b.brand.empty();   // nameless brands last

                    // Folded as well: the same feed writes "Irresistible" and
                    // "IRRESISTIBLES", and a raw compare files them apart.
                    return text::normalize(a.brand) < text::normalize(b.brand);
                });
            break;

        case Sort::Price:
        default:
            std::stable_sort(items.begin(), items.end(),
                [](const model::Item& a, const model::Item& b)
                {
                    // A price of 0 means "no number advertised", so those sit at
                    // the end rather than pretending to be the best deal going.
                    if ((a.currentPrice <= 0.0) != (b.currentPrice <= 0.0))
                        return b.currentPrice <= 0.0;

                    return a.currentPrice < b.currentPrice;
                });
            break;
    }
}

wxString MainWindow::sortSummary(const std::vector<model::Item>& items) const
{
    if (sort_ != Sort::Discount && sort_ != Sort::Brand)
        return wxString::Format(loc::tr("%zu items.", "%zu articles."), items.size());

    size_t known = 0;
    for (const model::Item& i : items)
        known += (sort_ == Sort::Discount ? i.discountPercent >= 0 : !i.brand.empty()) ? 1 : 0;

    const wxString what = (sort_ == Sort::Discount)
        ? loc::tr("with a percentage", "avec un pourcentage")
        : loc::tr("with a brand", "avec une marque");

    // The count of what is MISSING is the useful half. Half the feed carries no
    // percentage at all, and a list that simply looked short would be read as
    // "there are no deals" instead of "the banner did not say".
    return wxString::Format("%zu %s, %zu %s.",
                            known, what,
                            items.size() - known,
                            loc::tr("without", "sans"));
}

void MainWindow::applySort(int selection)
{
    if (selection < 0 || selection > 3)
        return;

    sort_ = static_cast<Sort>(selection);
    db_.setSetting(kSettingSort, std::to_string(selection));

    syncSortChoices();

    // Re-sorted in place rather than through refreshAllLists(): that one rebuilds
    // the flyer list, which clears the chosen flyer and empties its items, so
    // changing the order would have emptied the very list it was meant to
    // reorder. Nothing here touches the cache, only the order of what is already
    // on screen — and every selection survives.
    sortItems(flyerItems_);
    fillItemList(flyerItemList_, flyerItems_);

    sortItems(searchResults_);
    fillItemList(searchList_, searchResults_);

    sortItems(favoriteMatches_);
    fillItemList(favoriteMatchList_, favoriteMatches_);

    // Which list the count refers to is the one the user is looking at.
    const std::vector<model::Item>* shown = nullptr;
    switch (book_ != nullptr ? book_->GetSelection() : -1)
    {
        case PageFlyers:    shown = &flyerItems_;      break;
        case PageSearch:    shown = &searchResults_;   break;
        case PageFavorites: shown = &favoriteMatches_; break;
        default: break;
    }

    // Spoken after the dropdown entry itself: the order is "Rabais, du plus
    // élevé" and THEN what that gives, which is the order a person asks the
    // question in.
    if (shown != nullptr)
        announce(sortSummary(*shown), 500);
}

wxString MainWindow::fillMerchantFilter(wxChoice* choice,
                                       const std::vector<wxString>& present,
                                       const char* setting)
{
    if (choice == nullptr)
        return {};

    const wxString wanted = u8(db_.setting(setting));

    // Rebuilt from what is actually there rather than from the followed
    // banners: walking past four empty stores to reach the one you are standing
    // in is exactly the kind of trip this filter exists to remove.
    choice->Clear();
    choice->Append(loc::tr("All banners", "Toutes les bannières"));

    int selection = 0;
    for (size_t n = 0; n < present.size(); ++n)
    {
        choice->Append(present[n]);

        if (present[n] == wanted)
            selection = static_cast<int>(n) + 1;
    }

    // SetSelection emits no event, so restoring the choice cannot bounce back
    // into the handler that saved it.
    choice->SetSelection(selection);

    // The saved banner may have gone — its flyer expired, or it stopped being
    // followed. The filter then falls back to everything rather than showing an
    // empty list with no way to tell why.
    return selection == 0 ? wxString() : present[static_cast<size_t>(selection - 1)];
}

void MainWindow::syncWeekChoices()
{
    const int index = weekIndex(db_.week());

    // wxChoice::SetSelection emits no command event, so setting one dropdown from
    // the other cannot bounce back and send the pair into a loop.
    if (flyerWeek_  != nullptr) flyerWeek_->SetSelection(index);
    if (searchWeek_ != nullptr) searchWeek_->SetSelection(index);
}

void MainWindow::refreshAllLists()
{
    reloadFlyers();
    reloadFavorites();

    // A search already on screen is re-run so its results obey the new filter
    // rather than sitting there stale — but without moving the focus, since the
    // user is standing on the Flyers week dropdown or the Settings checkbox that
    // triggered this, not on the Search page.
    if (searchField_ != nullptr && !searchField_->GetValue().Trim().empty())
        runSearch(/*moveFocus=*/false);
}

void MainWindow::queueDetail(long long itemId, bool speakOnArrival, bool urgent)
{
    if (itemId == 0)
        return;

    if (speakOnArrival)
        detailSpeak_.store(itemId);

    {
        std::lock_guard<std::mutex> lock(detailMutex_);

        const auto already = std::find(detailQueue_.begin(), detailQueue_.end(), itemId);
        if (already != detailQueue_.end())
        {
            // Already waiting: move it to the front if the user just asked for it.
            if (urgent)
            {
                detailQueue_.erase(already);
                detailQueue_.push_front(itemId);
            }
            return;
        }

        if (urgent)
            detailQueue_.push_front(itemId);
        else
            detailQueue_.push_back(itemId);
    }

    startDetailWorker();
}

void MainWindow::startDetailWorker()
{
    if (detailRunning_.exchange(true))
        return;     // one worker is enough; it will drain whatever was just added

    detailCanceller_.reset();

    if (detailThread_.joinable())
        detailThread_.join();

    detailThread_ = std::thread([this]
    {
        source::FlippSource feed;
        feed.setCanceller(&detailCanceller_);

        // The worker's own connection. WAL lets it write while the UI thread
        // reads; it never touches db_, which belongs to the UI thread.
        db::Database cache;
        std::string error;
        cache.open(paths::databaseFile(), error);

        for (;;)
        {
            long long id = 0;
            {
                std::lock_guard<std::mutex> lock(detailMutex_);
                if (detailQueue_.empty())
                {
                    // Cleared while the lock is still held. Clearing it after
                    // releasing leaves a window where a push sees the worker as
                    // running and does not start one, and the worker then exits:
                    // the item is orphaned, and a user who pressed Ctrl+D hears
                    // "looking up" followed by silence forever.
                    detailRunning_.store(false);
                    break;
                }

                id = detailQueue_.front();
                detailQueue_.pop_front();
            }

            if (detailStop_.load())
                break;

            std::string postalCode;
            {
                std::lock_guard<std::mutex> lock(detailMutex_);
                postalCode = postalCodeForWorker_;   // copied, never held by reference
            }

            model::Item fetched;
            fetched.id = id;

            // A failure still counts as looked at: the item is marked fetched so
            // the app does not ask again on every refresh for something the feed
            // simply has nothing to say about.
            feed.fetchItemDetail(id, postalCode, fetched);

            // Written here, on the worker's own connection, rather than on the
            // UI thread: SQLite serialises writers, so a UI-thread UPDATE that
            // collides with the sync worker's transaction blocks the message
            // pump for up to the five-second busy timeout — repeatedly, while
            // forty favourites are being enriched.
            if (cache.isOpen())
                cache.updateItemDetail(fetched);

            CallAfter([this, fetched] { applyDetail(fetched); });
        }

        // Also cleared on the detailStop_ path, which breaks with the flag set.
        detailRunning_.store(false);
    });
}

void MainWindow::applyDetail(const model::Item& fetched)
{
    // The worker already persisted this on its own connection; here we only
    // refresh what is on screen.
    // Update the copies the lists are drawn from, and repaint just the price
    // cell of the row concerned rather than rebuilding the whole list under the
    // user's cursor.
    struct Bound { std::vector<model::Item>* items; wxListCtrl* list; };
    const Bound bound[] =
    {
        { &flyerItems_,      flyerItemList_ },
        { &searchResults_,   searchList_ },
        { &favoriteMatches_, favoriteMatchList_ },
    };

    bool spoken = false;

    for (const Bound& b : bound)
    {
        if (b.list == nullptr)
            continue;

        for (size_t n = 0; n < b.items->size(); ++n)
        {
            model::Item& item = (*b.items)[n];
            if (item.id != fetched.id)
                continue;

            if (item.description.empty())   item.description   = fetched.description;
            if (item.sku.empty())           item.sku           = fetched.sku;
            if (item.originalPrice <= 0.0)  item.originalPrice = fetched.originalPrice;
            if (item.priceText.empty())     item.priceText     = fetched.priceText;
            if (item.saleStory.empty())     item.saleStory     = fetched.saleStory;
            if (item.productUrl.empty())    item.productUrl    = fetched.productUrl;
            item.detailRevision = db::Database::detailRevision();
            item.inStoreOnly  = item.inStoreOnly || fetched.inStoreOnly;
            item.detailFetched = true;

            b.list->SetItem(static_cast<long>(n), 1, fmt::price(item));

            // The field is rewritten even when it holds the focus. Holding the
            // update back until focus leaves would strand a reader who is
            // standing in the field precisely because it said "looking up" —
            // which is the very complaint this feature answers. The caret moving
            // back to the top is announced, so the jump is explained rather than
            // merely felt.
            const size_t pane = paneOfList(b.list);
            if (pane < kDetailPanes && selectedRow(b.list) == static_cast<long>(n))
            {
                const bool wasFocused = (FindFocus() == detailPanes_[pane].field);
                refreshDetailPane(pane);

                if (wasFocused)
                    announce(loc::tr("Details received.", "Détails reçus."));
            }

            if (detailSpeak_.load() == fetched.id)
            {
                detailSpeak_.store(0);
                announce(fmt::itemDetail(item));
            }

            // The page the user asked for before it was known. Only if they are
            // still standing on that item: a browser opening under a cursor that
            // has moved on would be a surprise, not a service.
            if (productPageWanted_ == fetched.id)
            {
                productPageWanted_ = 0;

                if (!item.productUrl.empty() && selectedRow(b.list) == static_cast<long>(n))
                    openProductPage(item.productUrl);
                else
                    announce(loc::tr("No product page for this item.",
                                     "Aucune fiche produit pour cet article."));
            }

            spoken = true;
        }
    }

    // The row is gone — another flyer was opened, a sync finished, the week
    // changed — so there is nothing to update. Someone who pressed Ctrl+D is
    // still waiting on the "looking up" they were told, and would otherwise wait
    // for ever; and the pending id has to be cleared or the next background
    // prefetch of that same item would announce a detail nobody asked for.
    if (!spoken && detailSpeak_.load() == fetched.id)
    {
        detailSpeak_.store(0);
        announce(loc::tr("That item is no longer on screen.",
                         "Cet article n'est plus affiché."));
    }
}

wxListCtrl* MainWindow::focusedItemList(std::vector<model::Item>*& itemsOut)
{
    wxWindow* focus = FindFocus();

    if (focus == flyerItemList_)     { itemsOut = &flyerItems_;      return flyerItemList_; }
    if (focus == searchList_)        { itemsOut = &searchResults_;   return searchList_; }
    if (focus == favoriteMatchList_) { itemsOut = &favoriteMatches_; return favoriteMatchList_; }

    // Not standing on a list: fall back to the one the current tab is about, so
    // the Details button works after clicking it.
    switch (book_->GetSelection())
    {
        case PageFlyers:    itemsOut = &flyerItems_;      return flyerItemList_;
        case PageSearch:    itemsOut = &searchResults_;   return searchList_;
        case PageFavorites: itemsOut = &favoriteMatches_; return favoriteMatchList_;
        default:            itemsOut = nullptr;           return nullptr;
    }
}

void MainWindow::scheduleDetailPrefetch()
{
    detailPrefetchTimer_.StartOnce(250);
}

void MainWindow::prefetchDetailsAroundCursor()
{
    std::vector<model::Item>* items = nullptr;
    wxListCtrl* list = focusedItemList(items);

    if (list == nullptr || items == nullptr || items->empty())
        return;

    const long row = selectedRow(list);
    if (row < 0)
        return;

    // A short window around the cursor: the row itself, one behind for a reader
    // coming back up, and four ahead — about four seconds of reading at the pace
    // one goes through a list, which is more than a request needs.
    const long first = std::max<long>(0, row - 1);
    const long last  = std::min<long>(static_cast<long>(items->size()) - 1, row + 4);

    for (long n = first; n <= last; ++n)
    {
        const model::Item& item = (*items)[n];
        if (needsDetail(item))
            queueDetail(item.id, /*speakOnArrival=*/false, /*urgent=*/(n == row));
    }
}

bool MainWindow::needsDetail(const model::Item& item) const
{
    // Never asked, or asked by a parser that did not yet know how to read every
    // field this one does. The second arm is what a new field costs: one re-ask
    // per row the user actually walks past, instead of clearing detailFetched
    // across the table and making a warm cache report itself as incomplete.
    return !item.detailFetched || item.detailRevision < db::Database::detailRevision();
}

void MainWindow::openSelectedProductPage()
{
    std::vector<model::Item>* items = nullptr;
    wxListCtrl* list = focusedItemList(items);

    // Settings and the shopping list own no item list. Saying so beats a key
    // that does nothing at all, which to someone who cannot see the screen is
    // indistinguishable from a broken build.
    if (list == nullptr || items == nullptr)
    {
        announce(loc::tr("No product page here.", "Pas de fiche produit ici."));
        return;
    }

    const long row = selectedRow(list);
    if (row < 0 || static_cast<size_t>(row) >= items->size())
    {
        announce(loc::tr("No item selected.", "Aucun article sélectionné."));
        return;
    }

    const model::Item& item = (*items)[row];

    if (!item.productUrl.empty())
    {
        openProductPage(item.productUrl);
        return;
    }

    if (needsDetail(item))
    {
        // Asked for before the link had arrived: remembered, so the page opens
        // by itself the moment it does. Leaving the user to press again and
        // guess when is the worst possible shape for someone with no spinner to
        // watch.
        productPageWanted_ = item.id;
        announce(loc::tr("Looking up the product page...",
                         "Recherche de la fiche produit..."));
        queueDetail(item.id, /*speakOnArrival=*/false, /*urgent=*/true);
        return;
    }

    announce(loc::tr("No product page for this item.",
                     "Aucune fiche produit pour cet article."));
}

void MainWindow::openProductPage(const std::string& url)
{
    // Checked here as well as when it was parsed. Opening ends in ShellExecute,
    // which would as happily run a local path or a protocol handler, and the
    // string came from the feed.
    if (!http::isSafeUrl(url))
    {
        announce(loc::tr("That product link is unusable.",
                         "Ce lien de fiche produit est inutilisable."));
        return;
    }

    announce(loc::tr("Opening the product page in your browser.",
                     "Ouverture de la fiche produit dans le navigateur."));

    // Deferred: the browser takes the foreground almost at once, and a screen
    // reader drops a pending notification when it starts describing the window
    // that just arrived. The announcement needs a moment of its own first.
    const wxString target = wxString::FromUTF8(url);
    CallAfter([target]
    {
        wxLaunchDefaultBrowser(target);
    });
}

void MainWindow::startUpdateCheck(bool silent)
{
    // One at a time. A second check while the first is in flight would race two
    // dialogs onto the screen, and the user hears them fight.
    if (updateInFlight_)
        return;

    updateInFlight_ = true;

    if (updateThread_.joinable())
        updateThread_.join();

    updateThread_ = std::thread([this, silent]
    {
        const updater::Info info = updater::check();
        CallAfter([this, info, silent] { onUpdateChecked(info, silent); });
    });
}

void MainWindow::onUpdateChecked(const updater::Info& info, bool silent)
{
    updateInFlight_ = false;

    if (!info.error.empty())
    {
        // Silence on start-up. No network on a laptop that has not joined the
        // wifi yet is not an error worth interrupting anyone for.
        if (!silent)
            announce(info.error);

        return;
    }

    if (!info.available)
    {
        if (!silent)
            announce(loc::tr("PromoAccess is up to date.", "PromoAccess est à jour."));

        return;
    }

    // A copy someone unpacked by hand is left where it is: running the installer
    // would move the program somewhere they did not choose.
    if (!updater::isInstalled())
    {
        if (!silent)
        {
            wxMessageBox(loc::tr("A newer version is available, but this copy was not installed "
                                 "by the installer. Download it again from the website.",
                                 "Une version plus récente existe, mais cette copie n'a pas été "
                                 "posée par l'installeur. Téléchargez-la de nouveau depuis le site."),
                         loc::tr("Update", "Mise à jour"), wxOK | wxICON_INFORMATION, this);
        }

        return;
    }

    wxString message = wxString::Format(
        loc::tr("PromoAccess %s is available. You have %s.",
                "PromoAccess %s est disponible. Vous avez la %s."),
        wxString::FromUTF8(info.latestVersion),
        wxString::FromUTF8(PROMO_VERSION_STR));

    if (!info.releaseNotes.empty())
        message += "\n\n" + wxString::FromUTF8(info.releaseNotes);

    message += "\n\n"
             + loc::tr("Install it now? PromoAccess will close and reopen itself.",
                       "L'installer maintenant ? PromoAccess se fermera et se rouvrira.");

    wxMessageDialog dialog(this, message,
                           loc::tr("Update available", "Mise à jour disponible"),
                           wxYES_NO | wxICON_QUESTION);
    dialog.SetYesNoLabels(loc::tr("Install", "Installer"),
                          loc::tr("Later", "Plus tard"));

    if (dialog.ShowModal() == wxID_YES)
        downloadAndApply(info);
}

void MainWindow::downloadAndApply(const updater::Info& info)
{
    // Determinate when the server announces a size, pulsing when it does not.
    wxProgressDialog progress(loc::tr("Update", "Mise à jour"),
                              loc::tr("Downloading the installer...",
                                      "Téléchargement de l'installeur..."),
                              100, this,
                              wxPD_APP_MODAL | wxPD_AUTO_HIDE);

    std::atomic<bool> done{ false };
    std::atomic<int>  percent{ -1 };
    wxString path;

    std::thread worker([&]
    {
        path = updater::download(info.installerUrl, [&](size_t got, size_t total)
        {
            percent = (total > 0) ? static_cast<int>((got * 100) / total) : -1;
        });

        done = true;
    });

    // The dialog only repaints, and only answers the screen reader, while the
    // main loop runs — so the wait yields to it instead of blocking on join().
    while (!done)
    {
        const int shown = percent.load();

        if (shown >= 0)
            progress.Update(std::min(shown, 99));
        else
            progress.Pulse();

        wxYield();
        wxMilliSleep(80);
    }

    worker.join();
    progress.Update(100);

    if (path.empty())
    {
        wxMessageBox(loc::tr("The installer could not be downloaded.",
                             "L'installeur n'a pas pu être téléchargé."),
                     loc::tr("Update", "Mise à jour"), wxOK | wxICON_ERROR, this);
        return;
    }

    updater::apply(this, path);
}

void MainWindow::openManual()
{
    const wxString path = paths::manualFile(loc::isFrench());

    // Naming the file that is missing rather than failing in silence: the manual
    // is a document shipped beside the program, and a document can be absent
    // from a copy that was moved by hand.
    // wxLaunchDefaultApplication and not wxLaunchDefaultBrowser: the browser call
    // wants a URL, and turning a Windows path into a file:// URL by hand is where
    // spaces and backslashes go wrong. Handing the shell the file lets it pick
    // whatever the user reads HTML with.
    if (path.empty() || !wxLaunchDefaultApplication(path))
    {
        wxMessageBox(loc::tr("The manual could not be opened. Check that the Docs folder "
                             "is present beside PromoAccess.exe.",
                             "Le manuel n'a pas pu être ouvert. Vérifiez que le dossier "
                             "Docs se trouve à côté de PromoAccess.exe."),
                     loc::tr("Manual", "Manuel"), wxOK | wxICON_WARNING, this);
        return;
    }

    // The browser takes the foreground, so the reader is already saying the page
    // title; saying anything here would only talk over it.
}

void MainWindow::announce(const wxString& text, int delayMs)
{
    pendingAnnounce_ = text;
    announceTimer_.StartOnce(delayMs);
}

void MainWindow::announceSelectedDetail()
{
    std::vector<model::Item>* items = nullptr;
    wxListCtrl* list = focusedItemList(items);

    if (list == nullptr || items == nullptr)
        return;

    const long row = selectedRow(list);
    if (row < 0 || static_cast<size_t>(row) >= items->size())
    {
        announce(loc::tr("No item selected.", "Aucun article sélectionné."));
        return;
    }

    const model::Item& item = (*items)[row];

    // Already known: answer at once instead of going back to the network for
    // something the cache holds.
    if (item.detailFetched)
    {
        announce(fmt::itemDetail(item));
        return;
    }

    announce(loc::tr("Looking up...", "Recherche en cours..."));
    queueDetail(item.id, /*speakOnArrival=*/true, /*urgent=*/true);
}

void MainWindow::refreshLocation()
{
    if (locationField_ == nullptr)
        return;

    const std::string code = postalCode();
    if (code.empty())
    {
        locationField_->ChangeValue(loc::tr("No postal code yet.",
                                            "Aucun code postal pour l'instant."));
        return;
    }

    wxString text;

    // The refined municipality if the online lookup ever succeeded for this code,
    // otherwise the embedded sector table — which answers every valid Canadian
    // code, offline, so this line is never blank while the user waits on a
    // service that may simply refuse.
    locality::Place place = locality::decode(db_.setting(localityKey(code).c_str()));
    if (place.empty())
        place = locality::fromTable(code);

    const std::string described = locality::describe(place);

    text = described.empty()
        ? loc::tr("Place unknown", "Lieu inconnu")
        : u8(described);

    // The coverage figures describe the region that was actually synced. When the
    // user has typed a different postal code and not synced it yet, quoting them
    // beside the new place name would attribute one region's flyers to another.
    if (code != db_.setting(kSettingPostal))
    {
        text += ", " + loc::tr("not synced yet — press F5",
                                "pas encore synchronisé — appuyez sur F5");
        locationField_->ChangeValue(text);
        return;
    }

    const int followed = db_.followedCount();

    text += wxString::Format(", %d %s, %zu %s",
        followed,
        followed == 1 ? loc::tr("banner followed", "bannière suivie")
                      : loc::tr("banners followed", "bannières suivies"),
        flyers_.size(),
        flyers_.size() == 1 ? loc::tr("current flyer", "circulaire en cours")
                            : loc::tr("current flyers", "circulaires en cours"));

    locationField_->ChangeValue(text);
}

void MainWindow::requestLocality(const std::string& postalCode)
{
    if (postalCode.empty() || localityBusy_.load())
        return;

    // Already known: never ask twice for the same code.
    if (!db_.setting(localityKey(postalCode).c_str()).empty())
        return;

    if (localityThread_.joinable())
        localityThread_.join();

    localityBusy_.store(true);
    localityCanceller_.reset();

    localityThread_ = std::thread([this, postalCode]
    {
        locality::Place place;
        const bool ok = locality::lookup(postalCode, place, &localityCanceller_);

        CallAfter([this, postalCode, place, ok]
        {
            localityBusy_.store(false);

            // A failed lookup writes nothing, so the next attempt is free to
            // succeed — the provider refuses a request as often as it answers it.
            if (ok)
                db_.setSetting(localityKey(postalCode).c_str(), locality::encode(place));

            refreshLocation();

            // Only speak when the user is standing on the field waiting for it.
            // Anywhere else this would interrupt whatever they are reading.
            if (ok && FindFocus() == locationField_)
                announce(locationField_->GetValue());
        });
    });
}

void MainWindow::reloadMerchants()
{
    merchants_ = db_.merchants(false);

    // CheckItem below emits the same event a user tick does, so the handler is
    // muted for the duration; without this the list refill re-triggers whatever
    // ticking a banner is wired to do.
    populatingMerchants_ = true;

    merchantList_->DeleteAllItems();
    for (size_t n = 0; n < merchants_.size(); ++n)
    {
        const long row = merchantList_->InsertItem(static_cast<long>(n), u8(merchants_[n].name));
        if (merchants_[n].followed)
            merchantList_->CheckItem(row, true);
    }

    populatingMerchants_ = false;

    // The two banner dropdowns only ever offer followed banners: filtering by a
    // store whose flyers were never downloaded would just show nothing.
    const std::vector<model::Merchant> followed = db_.merchants(true);

    for (wxChoice* choice : { flyerMerchant_, searchMerchant_ })
    {
        if (choice == nullptr)
            continue;

        // What the user had chosen, so it can be restored. Rebuilding these
        // dropdowns after every sync used to reset them to "All banners"
        // silently — wxChoice::SetSelection emits no event, so nothing was
        // announced and the flyer list simply widened again under the user.
        const int previous = choice->GetSelection();
        const wxString chosen = (previous > 0) ? choice->GetString(previous) : wxString();

        choice->Clear();
        choice->Append(loc::tr("All banners", "Toutes les bannières"));
        for (const model::Merchant& m : followed)
            choice->Append(u8(m.name));

        const int restored = chosen.empty() ? wxNOT_FOUND : choice->FindString(chosen);
        choice->SetSelection(restored == wxNOT_FOUND ? 0 : restored);
    }
}

void MainWindow::reloadFlyers()
{
    int merchantId = 0;
    const int index = flyerMerchant_->GetSelection();
    if (index > 0)
    {
        const std::vector<model::Merchant> followed = db_.merchants(true);
        if (static_cast<size_t>(index - 1) < followed.size())
            merchantId = followed[index - 1].id;
    }

    flyers_ = db_.flyers(merchantId, true);

    flyerList_->DeleteAllItems();
    for (size_t n = 0; n < flyers_.size(); ++n)
    {
        const long row = flyerList_->InsertItem(static_cast<long>(n), u8(flyers_[n].merchantName));

        flyerList_->SetItem(row, 1, fmt::flyerName(flyers_[n].name));

        flyerList_->SetItem(row, 2, wxString::Format("%d", flyers_[n].itemCount));
        flyerList_->SetItem(row, 3, fmt::validityDate(flyers_[n].validTo));
    }

    flyerItems_.clear();
    flyerItemList_->DeleteAllItems();
    refreshDetailPane(paneOfList(flyerItemList_));
}

void MainWindow::reloadFlyerItems()
{
    const long row = selectedRow(flyerList_);
    if (row < 0 || static_cast<size_t>(row) >= flyers_.size())
        return;

    flyerItems_ = db_.itemsOfFlyer(flyers_[row].id);
    sortItems(flyerItems_);
    fillItemList(flyerItemList_, flyerItems_);
}

void MainWindow::fillItemList(wxListCtrl* list, const std::vector<model::Item>& items)
{
    // Freeze/Thaw around a few hundred inserts: without it the control repaints
    // per row, and a screen reader can start reading a half-built list.
    const long wasSelected = selectedRow(list);

    list->Freeze();
    list->DeleteAllItems();

    for (size_t n = 0; n < items.size(); ++n)
    {
        const long row = list->InsertItem(static_cast<long>(n), fmt::itemName(items[n].name));
        list->SetItem(row, 1, fmt::price(items[n]));
        list->SetItem(row, 2, u8(items[n].merchantName));
        list->SetItem(row, 3, fmt::validityDate(items[n].validTo));
    }

    list->Thaw();
    restoreSelection(list, wasSelected);

    // The field is bound to the selection, and emptying a list fires no
    // selection event — so without this it would go on describing a product that
    // is no longer on screen, in a control the user can quote from.
    refreshDetailPane(paneOfList(list));
}

void MainWindow::reloadFavorites()
{
    favorites_ = db_.favorites();

    const long wasSelected = selectedRow(favoriteList_);
    favoriteList_->DeleteAllItems();
    for (size_t n = 0; n < favorites_.size(); ++n)
    {
        const model::Favorite& f = favorites_[n];

        const long row = favoriteList_->InsertItem(static_cast<long>(n), u8(f.pattern));
        favoriteList_->SetItem(row, 1, f.merchantIds.empty()
            ? loc::tr("All banners", "Toutes les bannières")
            : u8(f.merchantName));
        favoriteList_->SetItem(row, 2, f.maxPrice > 0.0 ? fmt::money(f.maxPrice) : "-");

        // The count is filled by reloadFavoriteMatches() below, which runs the
        // very same query. Running it here as well doubled the number of full
        // table scans on the UI thread, once per favourite, every time this tab
        // was opened or a sync finished.
        favoriteList_->SetItem(row, 3, f.enabled
            ? wxString("...")
            : loc::tr("paused", "en pause"));
    }

    restoreSelection(favoriteList_, wasSelected);

    reloadFavoriteMatches();
}

void MainWindow::applyMatchFilter()
{
    std::vector<wxString> present;
    for (const model::Item& item : favoriteMatchesAll_)
    {
        const wxString name = u8(item.merchantName);
        if (!name.empty()
            && std::find(present.begin(), present.end(), name) == present.end())
        {
            present.push_back(name);
        }
    }

    std::sort(present.begin(), present.end());

    const wxString chosen = fillMerchantFilter(matchMerchant_, present, kSettingMatchBan);

    favoriteMatches_.clear();
    for (const model::Item& item : favoriteMatchesAll_)
        if (chosen.empty() || u8(item.merchantName) == chosen)
            favoriteMatches_.push_back(item);

    fillItemList(favoriteMatchList_, favoriteMatches_);
}

void MainWindow::reloadFavoriteMatches()
{
    favoriteMatches_.clear();

    // The same product can satisfy two watches ("poulet" and "cuisses poulet");
    // it must appear once, or the user hears it twice while arrowing down.
    std::set<long long> seen;

    for (size_t n = 0; n < favorites_.size(); ++n)
    {
        const model::Favorite& f = favorites_[n];
        if (!f.enabled)
            continue;

        // Queried once, and the per-favourite count taken from the same result
        // rather than from a second identical scan.
        std::vector<model::Item> hits = db_.searchItems(f.pattern, f.merchantIds, f.maxPrice,
                                                        /*currentOnly=*/true,
                                                        /*followedOnly=*/true,
                                                        f.wholeWords);

        if (favoriteList_ != nullptr)
            favoriteList_->SetItem(static_cast<long>(n), 3,
                                   wxString::Format("%zu", hits.size()));

        for (model::Item& item : hits)
            if (seen.insert(item.id).second)
                favoriteMatches_.push_back(std::move(item));
    }

    // The banner is the tiebreak, not the grouping: the order the user picked
    // governs here exactly as it does in the other two lists, and two items that
    // tie on it then fall together by store. Sorting this first and relying on
    // stability is what makes that hold.
    std::stable_sort(favoriteMatches_.begin(), favoriteMatches_.end(),
                     [](const model::Item& a, const model::Item& b)
                     {
                         return a.merchantName < b.merchantName;
                     });
    sortItems(favoriteMatches_);

    // Everything found, kept aside: the dropdown is built from it, and the list
    // shows the chosen banner's share of it.
    favoriteMatchesAll_ = favoriteMatches_;
    applyMatchFilter();

    // Favourites are the short list the user actually watches, so their original
    // prices are fetched without being asked for. Bounded: a loose pattern can
    // match hundreds of items, and this is one request each. Asked of everything
    // found, not of what is on screen — the price is wanted whichever banner is
    // showing.
    int queued = 0;
    for (const model::Item& item : favoriteMatchesAll_)
    {
        if (!needsDetail(item) || queued >= 40)
            continue;

        queueDetail(item.id, /*speakOnArrival=*/false, /*urgent=*/false);
        ++queued;
    }
}

void MainWindow::reloadList()
{
    listEntriesAll_ = db_.listEntries();

    // The banners actually on the list, in order, so the dropdown never offers a
    // store with nothing in it.
    std::vector<wxString> present;
    for (const model::ListEntry& e : listEntriesAll_)
    {
        const wxString name = u8(e.merchantName);
        if (!name.empty()
            && std::find(present.begin(), present.end(), name) == present.end())
        {
            present.push_back(name);
        }
    }

    std::sort(present.begin(), present.end());

    const wxString chosen = fillMerchantFilter(listMerchant_, present, kSettingListBan);

    listEntries_.clear();
    for (const model::ListEntry& e : listEntriesAll_)
        if (chosen.empty() || u8(e.merchantName) == chosen)
            listEntries_.push_back(e);

    const long wasSelected = selectedRow(shoppingList_);
    shoppingList_->DeleteAllItems();
    double total = 0.0;
    int    items = 0;

    for (size_t n = 0; n < listEntries_.size(); ++n)
    {
        const model::ListEntry& e = listEntries_[n];

        // Through the same recasing as everywhere else: a line added before the
        // rule existed is still stored SHOUTING, and the list would otherwise
        // read half one way and half the other.
        const long row = shoppingList_->InsertItem(static_cast<long>(n),
                                                   fmt::properCase(u8(e.name)));
        shoppingList_->SetItem(row, 1, wxString::Format("%d", e.quantity));
        shoppingList_->SetItem(row, 2, fmt::lineTotal(e));
        shoppingList_->SetItem(row, 3, u8(e.merchantName));
        shoppingList_->SetItem(row, 4, fmt::validityDate(e.validTo));

        total += e.price * e.quantity;
        items += e.quantity;
    }

    restoreSelection(shoppingList_, wasSelected);

    // Articles counted, not rows. Two tins of the same thing are two articles on
    // one row, and a field that answered "1" while the total charged for two is
    // the field that made the total look wrong.
    //
    // The row count is gone. It described how the data is laid out on screen,
    // not the shopping — and anyone can hear how many rows there are by walking
    // them. A word nobody asked for, in a line that is read many times a trip.
    if (listEntries_.empty())
    {
        listTotal_->ChangeValue(listEntriesAll_.empty()
            ? loc::tr("Empty list.", "Liste vide.")
            : loc::tr("Nothing on the list for this banner.",
                      "Rien sur la liste pour cette bannière."));
        return;
    }

    // The total follows the filter, and names the banner it belongs to. That is
    // the point of filtering at all: standing in one store, what matters is what
    // will be spent in THAT store, not the whole week's shopping.
    const wxString banner = chosen.empty() ? wxString() : chosen + " : ";

    listTotal_->ChangeValue(wxString::Format("%s%d %s, %s%s",
        banner, items,
        items == 1 ? loc::tr("item", "article") : loc::tr("items", "articles"),
        loc::tr("estimated total ", "total estimé "), fmt::money(total)));
}

//==============================================================================
// Actions
//==============================================================================
void MainWindow::queueSync(int merchantId)
{
    if (std::find(pendingSyncs_.begin(), pendingSyncs_.end(), merchantId) == pendingSyncs_.end())
        pendingSyncs_.push_back(merchantId);

    startNextQueuedSync();
}

void MainWindow::startNextQueuedSync()
{
    if (pendingSyncs_.empty() || sync_.isRunning())
        return;

    const int merchantId = pendingSyncs_.front();
    pendingSyncs_.erase(pendingSyncs_.begin());

    startSync(merchantId);
}

void MainWindow::startSync(int merchantId)
{
    if (sync_.isRunning() || syncPending_)
    {
        // syncPending_ as well as isRunning(): the worker clears its own flag
        // just before queueing the completion event, so for a moment a sync
        // reads as finished while its result has not been handled yet. Starting
        // another run in that gap would let the old completion announce "sync
        // done" over the new one.
        if (merchantId == 0)
            announce(loc::tr("A sync is already running.", "Une synchronisation est déjà en cours."));
        return;
    }

    // F5 covers every followed banner, so anything queued is already included.
    if (merchantId == 0)
        pendingSyncs_.clear();

    syncingMerchant_.clear();
    for (const model::Merchant& m : merchants_)
        if (m.id == merchantId)
            syncingMerchant_ = u8(m.name);

    const std::string code = postalCode();
    if (code.empty())
    {
        book_->SetSelection(PageSettings);
        suppressPostalHint_ = true;
        postalField_->SetFocus();
        postalField_->SelectAll();
        announce(loc::tr("Not a valid postal code. Six characters in one block, no space, "
                         "letter digit letter digit letter digit.",
                         "Code postal invalide. Six caractères en un seul bloc, sans espace, "
                         "lettre chiffre lettre chiffre lettre chiffre."));
        return;
    }

    // Echo back the upper-case form, so what the field shows is exactly the key
    // the cache will use.
    postalField_->ChangeValue(wxString::FromUTF8(code));

    db_.setSetting(kSettingPostal, code);
    db_.setScope({ code });
    {
        std::lock_guard<std::mutex> lock(detailMutex_);
        postalCodeForWorker_ = code;
    }
    requestLocality(code);

    if (!sync_.start(paths::databaseFile(), code, merchantId))
        return;

    syncPending_ = true;
    syncButton_->Disable();
    syncStatus_->SetLabel(loc::tr("Syncing...", "Synchronisation en cours..."));

    // A banner just ticked already announced itself; saying "syncing" again on
    // top of it would only talk over the confirmation.
    if (merchantId == 0)
        announce(loc::tr("Syncing.", "Synchronisation."));
}

void MainWindow::runSearch(bool moveFocus)
{
    const wxString query = searchField_->GetValue().Trim(true).Trim(false);
    if (query.empty())
    {
        if (moveFocus)
        {
            announce(loc::tr("Type something to search for.", "Entrez un mot à rechercher."));
            searchField_->SetFocus();
        }
        return;
    }

    int merchantId = 0;
    const int index = searchMerchant_->GetSelection();
    if (index > 0)
    {
        const std::vector<model::Merchant> followed = db_.merchants(true);
        if (static_cast<size_t>(index - 1) < followed.size())
            merchantId = followed[index - 1].id;
    }

    // Accepts both decimal marks: a French keyboard produces "4,99", and typing
    // it must not silently mean "no ceiling".
    wxString priceText = searchMaxPrice_->GetValue().Trim(true).Trim(false);
    priceText.Replace(",", ".");

    double maxPrice = 0.0;
    if (!priceText.empty() && (!priceText.ToCDouble(&maxPrice) || maxPrice <= 0.0))
    {
        if (moveFocus)
        {
            announce(loc::tr("That maximum price is not a number.",
                             "Ce prix maximum n'est pas un nombre."));
            searchMaxPrice_->SetFocus();
            searchMaxPrice_->SelectAll();
        }
        return;
    }

    // The cache, not the network: results are instant and work offline. The
    // network is only ever touched by a sync, which the user starts on purpose.
    searchResults_ = db_.searchItems(query.utf8_string(),
                                     merchantId > 0 ? std::vector<int>{ merchantId }
                                                    : std::vector<int>{},
                                     maxPrice);
    sortItems(searchResults_);
    fillItemList(searchList_, searchResults_);

    if (searchResults_.empty())
    {
        if (moveFocus)
        {
            announce(loc::tr("No match. Try fewer words, or sync with F5.",
                             "Aucun résultat. Essayez moins de mots, ou synchronisez avec F5."));
            searchField_->SetFocus();
        }
        return;
    }

    if (!moveFocus)
        return;

    announce(wxString::Format("%zu %s", searchResults_.size(),
                              loc::tr("results", "résultats")));
    searchList_->SetFocus();

    // Land on the first row so the arrow keys work immediately and the cheapest
    // result is read out without the user hunting for it.
    searchList_->SetItemState(0, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                              wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
}

void MainWindow::addToList(const model::Item& item)
{
    const wxString shown = fmt::itemName(item.name);

    // Asked before anything is written. Two of the same tin is the normal case
    // at a grocery, and making the user add the line and then go hunting for the
    // Quantity button on another tab is a trip nobody should have to make.
    const long wanted = askQuantity(shown, 1);
    if (wanted <= 0)
        return;   // cancelled, or not a number — askQuantity has already said so

    model::ListEntry entry;
    entry.name         = shown.utf8_string();
    entry.merchantName = item.merchantName;
    entry.price        = item.currentPrice;
    entry.priceText    = item.priceText.empty() ? item.saleStory : item.priceText;
    entry.quantity     = static_cast<int>(wanted);
    entry.validTo      = item.validTo;

    // The same product added again raises the line it is already on. Adding it
    // twice used to make two identical rows, which read as a duplicate to
    // anyone walking the list and totalled correctly only by accident.
    const model::ListEntry existing =
        db_.findListEntry(entry.name, entry.merchantName, entry.price);

    if (existing.id != 0)
    {
        const int total = std::min(existing.quantity + entry.quantity, 99);
        db_.setListQuantity(existing.id, total);
        reloadList();

        announce(wxString::Format(loc::tr("%s, quantity now %d.", "%s, quantité portée à %d."),
                                  shown, total));
        return;
    }

    db_.addListEntry(entry);
    reloadList();

    // Through the same formatting the row used: announcing the raw feed string
    // would read the bar aloud and then the English half, so the confirmation
    // would not match the line it confirms.
    announce(wxString::Format(loc::tr("Added: %s, quantity %d.", "Ajouté : %s, quantité %d."),
                              shown, static_cast<int>(wanted)));
}

void MainWindow::addSelectedToList(wxListCtrl* list, const std::vector<model::Item>& items)
{
    const long row = selectedRow(list);
    if (row < 0 || static_cast<size_t>(row) >= items.size())
    {
        announce(loc::tr("No item selected.", "Aucun article sélectionné."));
        return;
    }

    addToList(items[row]);
}

void MainWindow::rebuildInLanguage(bool french)
{
    // The workers are stopped BEFORE the language is switched. loc:: keeps the
    // current language in a plain global, and FlippSource reads it on every
    // request to choose the feed's locale: flipping it first would be an
    // unsynchronised write under a concurrent read, and would leave a running
    // sync writing the second half of its items in the other language.
    sync_.stop();
    if (localityThread_.joinable())
        localityThread_.join();

    detailStop_.store(true);

    // The flags stop the loops; these interrupt whatever request is already in
    // flight, so the joins that follow return at once rather than waiting out a
    // stalled connection with the message pump stopped.
    detailCanceller_.abort();
    localityCanceller_.abort();
    if (detailThread_.joinable())
        detailThread_.join();

    db_.close();

    loc::setLanguage(french ? loc::Language::French : loc::Language::English);

    // Built and shown before this one goes away. wxWidgets ends the application
    // when the last top-level window is destroyed, so the order matters.
    auto* replacement = new MainWindow(PROMO_WINDOW_TITLE);
    replacement->Show(true);
    wxTheApp->SetTopWindow(replacement);
    replacement->focusLanguageSetting();

    Destroy();
}

void MainWindow::focusLanguageSetting()
{
    book_->SetSelection(PageSettings);

    if (languageChoice_ != nullptr)
        languageChoice_->SetFocus();

    announce(loc::tr("Interface language: English.",
                     "Langue de l'interface : français."));
}

void MainWindow::exportList()
{
    // Everything, whatever banner is showing. A file per store is the point of
    // the text export, and exporting only the store on screen would quietly
    // produce a list missing the rest of the week.
    if (listEntriesAll_.empty())
    {
        announce(loc::tr("The list is empty.", "La liste est vide."));
        return;
    }

    wxFileDialog dialog(this,
        loc::tr("Save the shopping list", "Enregistrer la liste d'épicerie"),
        wxEmptyString,
        loc::tr("shopping-list.txt", "liste-épicerie.txt"),
        loc::tr("Text file (*.txt)|*.txt|Markdown (*.md)|*.md|Spreadsheet (*.csv)|*.csv",
                "Fichier texte (*.txt)|*.txt|Markdown (*.md)|*.md|Tableur (*.csv)|*.csv"),
        wxFD_SAVE | wxFD_OVERWRITE_PROMPT);

    if (dialog.ShowModal() != wxID_OK)
        return;

    const wxString path = dialog.GetPath();
    const exporter::Format format = exporter::formatForPath(path);

    // Markdown and CSV stay one file. A spreadsheet wants one table, and a
    // Markdown list is read on a screen where scrolling costs nothing; it is the
    // text file that gets carried into a store, one store at a time.
    if (format != exporter::Format::Text)
    {
        wxString error;
        if (exporter::write(path, format, listEntriesAll_, error))
            announce(loc::tr("List saved.", "Liste enregistrée."));
        else
            wxMessageBox(error, GetTitle(), wxOK | wxICON_ERROR, this);

        return;
    }

    // --- one text file per banner -------------------------------------------
    std::vector<wxString> banners;
    for (const model::ListEntry& e : listEntriesAll_)
    {
        const wxString name = u8(e.merchantName);
        if (std::find(banners.begin(), banners.end(), name) == banners.end())
            banners.push_back(name);
    }

    std::sort(banners.begin(), banners.end());

    wxFileName base(path);
    const wxString stem = base.GetName();

    int written = 0;
    wxString firstError;

    for (const wxString& banner : banners)
    {
        std::vector<model::ListEntry> ofBanner;
        for (const model::ListEntry& e : listEntriesAll_)
            if (u8(e.merchantName) == banner)
                ofBanner.push_back(e);

        if (ofBanner.empty())
            continue;

        // A banner name is not a file name: a slash or a colon in one would
        // send the file somewhere else entirely, or fail to open at all.
        wxString safe = banner.empty() ? loc::tr("no banner", "sans bannière") : banner;
        for (const wxString& forbidden : { "\\", "/", ":", "*", "?", "\"", "<", ">", "|" })
            safe.Replace(forbidden, "-");

        wxFileName target(base);
        target.SetName(stem + " - " + safe);

        wxString error;
        if (exporter::write(target.GetFullPath(), exporter::Format::Text, ofBanner, error))
        {
            ++written;
        }
        else if (firstError.empty())
        {
            firstError = error;
        }
    }

    if (written == 0)
    {
        wxMessageBox(firstError.empty()
                         ? loc::tr("The list could not be saved.",
                                   "La liste n'a pas pu être enregistrée.")
                         : firstError,
                     GetTitle(), wxOK | wxICON_ERROR, this);
        return;
    }

    // The count is the useful part: it says how many stores the trip covers, and
    // confirms a file was made for each of them.
    announce(wxString::Format(
        written == 1 ? loc::tr("One file saved, in %s.", "Un fichier enregistré, dans %s.")
                     : loc::tr("%d files saved, one per banner, in %s.",
                               "%d fichiers enregistrés, un par bannière, dans %s."),
        written, base.GetPath()));

    if (!firstError.empty())
        wxMessageBox(firstError, GetTitle(), wxOK | wxICON_WARNING, this);
}


//==============================================================================
// Events
//==============================================================================
void MainWindow::onSyncProgress(wxThreadEvent& event)
{
    const int done  = event.GetInt();
    const int total = static_cast<int>(event.GetExtraLong());

    const wxString text = wxString::Format("%s %d/%d", event.GetString(), done, total);
    syncStatus_->SetLabel(text);
    SetStatusText(text);

    // Speaking every flyer would be seventeen interruptions. Every fifth, plus
    // the last, is enough to know it is alive without drowning out the UI.
    if (done % 5 == 0 || done == total)
        announce(text);
}

void MainWindow::onSyncDone(wxThreadEvent& event)
{
    syncPending_ = false;
    syncButton_->Enable();

    if (event.GetInt() == 0)
    {
        const wxString error = event.GetString();
        const wxString text = error.empty()
            ? loc::tr("Sync cancelled.", "Synchronisation annulée.")
            : loc::tr("Sync failed: ", "Échec de la synchronisation : ") + error;

        syncStatus_->SetLabel(text);
        announce(text);
        startNextQueuedSync();
        return;
    }

    const long items = event.GetExtraLong();

    reloadMerchants();
    reloadFlyers();
    reloadFavorites();
    refreshLocation();

    // Name the banner when only one was fetched, so a tick is confirmed by what
    // it actually brought back rather than by a bare total.
    const wxString text = syncingMerchant_.empty()
        ? wxString::Format("%s%ld %s, %zu %s",
              loc::tr("Sync done. ", "Synchronisation terminée. "),
              items, loc::tr("items", "articles"),
              flyers_.size(), loc::tr("flyers", "circulaires"))
        : wxString::Format("%s, %ld %s",
              syncingMerchant_, items,
              items == 1 ? loc::tr("item", "article") : loc::tr("items", "articles"));

    syncStatus_->SetLabel(text);
    SetStatusText(text);
    announce(text);

    startNextQueuedSync();
}

void MainWindow::addDetailPane(size_t index, wxWindow* parent, wxSizer* sizer,
                               wxListCtrl* list, std::vector<model::Item>* items)
{
    const int border = FromDIP(6);

    // Named apart from the "Details (Ctrl+D)" button, which sits a couple of tab
    // stops away: two controls announced by the same word on one page is the
    // vocabulary split this codebase has already been bitten by.
    addLabel(parent, sizer, loc::tr("Item details:", "Détails de l'article :"));

    // A plain wxTextCtrl, not ReadOnlyField: wxTextCtrl::AcceptsFocusFromKeyboard
    // already returns true for a multi-line control, read-only or not, so the
    // override that single-line fields need would be a no-op here.
    //
    // wxTE_DONTWRAP matters and is not cosmetic. A wrapped Win32 edit reports
    // DISPLAY lines to the screen reader, so one Down press would give half a
    // fact; unwrapped, one logical line is one fact. The horizontal scrollbar
    // that comes with it costs nothing to someone reading by caret.
    //
    // wxTE_PROCESS_TAB must never be added: without it Tab leaves the field
    // through Win32's own dialog navigation, with it the field would swallow Tab
    // and trap the user in a control they cannot even type into.
    auto* field = new wxTextCtrl(parent, wxID_ANY, wxEmptyString,
                                 wxDefaultPosition, wxSize(-1, FromDIP(96)),
                                 wxTE_READONLY | wxTE_MULTILINE | wxTE_DONTWRAP);
    field->SetName(loc::tr("Item details", "Détails de l'article"));
    sizer->Add(field, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, border);

    detailPanes_[index] = { field, list, items };

    // Entering the field is an explicit request, so the item jumps the queue
    // ahead of whatever batch of favourites is being enriched in the background.
    field->Bind(wxEVT_SET_FOCUS, [this, index](wxFocusEvent& e)
    {
        const DetailPane& pane = detailPanes_[index];
        const long row = selectedRow(pane.list);

        if (pane.items != nullptr && row >= 0
            && static_cast<size_t>(row) < pane.items->size()
            && needsDetail((*pane.items)[row]))
        {
            queueDetail((*pane.items)[row].id, /*speakOnArrival=*/false, /*urgent=*/true);
        }

        e.Skip();
    });

    // Written on every selection change, and reset whenever the list is emptied.
    list->Bind(wxEVT_LIST_ITEM_SELECTED, [this, index](wxListEvent& e)
    {
        refreshDetailPane(index);
        e.Skip();
    });

    refreshDetailPane(index);
}

size_t MainWindow::paneOfList(const wxListCtrl* list) const
{
    for (size_t n = 0; n < kDetailPanes; ++n)
        if (detailPanes_[n].list == list)
            return n;

    return kDetailPanes;
}

void MainWindow::refreshDetailPane(size_t index)
{
    if (index >= kDetailPanes)
        return;

    const DetailPane& pane = detailPanes_[index];
    if (pane.field == nullptr || pane.list == nullptr || pane.items == nullptr)
        return;

    const long row = selectedRow(pane.list);

    const wxString text = (row < 0 || static_cast<size_t>(row) >= pane.items->size())
        ? loc::tr("No item selected.", "Aucun article sélectionné.")
        : fmt::itemLines((*pane.items)[row]);

    if (pane.field->GetValue() == text)
        return;

    // ChangeValue, never SetValue: the latter fires wxEVT_TEXT, and this field is
    // rewritten on every arrow key.
    pane.field->ChangeValue(text);

    // The caret goes back to the top, or a reader who had walked down to the
    // format line would find themselves somewhere arbitrary in the new text.
    pane.field->SetInsertionPoint(0);
}

wxWindow* MainWindow::lastFocusable(wxWindow* parent)
{
    // Depth first, from the end: the last thing a sighted user would see at the
    // bottom of the page is the last thing Shift+Tab should reach.
    const wxWindowList& children = parent->GetChildren();

    for (auto node = children.GetLast(); node != nullptr; node = node->GetPrevious())
    {
        wxWindow* child = node->GetData();

        if (wxWindow* deeper = lastFocusable(child))
            return deeper;

        if (child->IsShown() && child->IsEnabled() && child->AcceptsFocusFromKeyboard())
            return child;
    }

    return nullptr;
}

wxWindow* MainWindow::primaryControl(int page) const
{
    switch (page)
    {
        // The postal code, not the language dropdown above it. Settings is
        // entered to change the postal code or the banners, essentially never to
        // change the language — landing on the setting one touches once in the
        // life of the application taxed every visit for the sake of the rarest.
        // The dropdown stays one Shift+Tab away.
        case PageSettings:  return postalField_;
        case PageFlyers:    return flyerList_;
        case PageSearch:    return searchField_;
        // The banner, not the list. Shopping is done store by store, so the
        // first decision on arriving is which store — and the list is one Tab
        // away, already narrowed to it. The favourites themselves stay one
        // Shift+Tab back, where the rules are edited rather than read.
        case PageFavorites: return matchMerchant_;
        case PageList:      return listMerchant_;
        default:            return nullptr;
    }
}

void MainWindow::onPageChanged(wxBookCtrlEvent& event)
{
    // Favourite matches are recomputed on entry rather than kept live: they
    // depend on every favourite and on the whole cache, and the user only needs
    // them to be right at the moment they look.
    if (event.GetSelection() == PageFavorites)
        reloadFavorites();

    // Land on the control the tab is for. Leaving focus on the tab strip means
    // arrow keys move between tabs instead of through the content, and the first
    // thing announced is the tab name the user just chose — telling them nothing
    // they did not already know.
    if (layoutReady_)
    {
        if (wxWindow* target = primaryControl(event.GetSelection()))
        {
            // The name is prepared BEFORE the focus moves: the reader reads the
            // name it finds at the moment the focus event arrives.
            announceTabOnFocus(event.GetSelection());
            target->SetFocus();
        }
    }

    event.Skip();
}

void MainWindow::onFlyerSelected(wxListEvent& event)
{
    reloadFlyerItems();
    event.Skip();
}

void MainWindow::onClose(wxCloseEvent& event)
{
    // Stops the workers and waits for them, so no queued event lands on a window
    // that is already gone.
    sync_.stop();

    detailStop_.store(true);

    // The flags stop the loops; these interrupt whatever request is already in
    // flight, so the joins that follow return at once rather than waiting out a
    // stalled connection with the message pump stopped.
    detailCanceller_.abort();
    localityCanceller_.abort();

    if (localityThread_.joinable())
        localityThread_.join();
    if (detailThread_.joinable())
        detailThread_.join();
    event.Skip();
}
