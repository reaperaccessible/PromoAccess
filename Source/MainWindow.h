#pragma once

#include "Database.h"
#include "Model.h"
#include "Updater.h"
#include "Http.h"
#include "SyncService.h"

#include <wx/frame.h>
#include <wx/timer.h>
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class wxNotebook;
class wxPanel;
class wxListCtrl;
class wxListEvent;
class wxButton;
class wxChoice;
class wxTextCtrl;
class wxStaticText;
class wxCheckBox;
class wxBookCtrlEvent;
class wxCommandEvent;
class wxThreadEvent;
class wxCloseEvent;

//==============================================================================
// The whole user interface: five tabbed pages, reached with Ctrl+1 to Ctrl+5.
//
// Every page is the same shape — a list in report mode, with the controls that
// act on it below. That is not a stylistic choice: wxListCtrl in report mode is
// a native Win32 list-view, which JAWS and NVDA read column by column with no
// custom accessibility code, and which gives sorting and first-letter navigation
// for free. A custom-drawn grid would have to reimplement all of it and would
// still read worse.
//==============================================================================
class MainWindow : public wxFrame
{
public:
    explicit MainWindow(const wxString& title);
    ~MainWindow() override;

    // Opens on Settings with the language dropdown focused. Used by the window
    // that replaces this one after a language change, so the user lands back
    // where they were and hears the confirmation in the language they picked.
    void focusLanguageSetting();

protected:
    // Answer WM_GETOBJECT with our UIA root so screen readers connect over UIA
    // and receive the notification events raised by announce(). A plain wx window
    // is MSAA-only and silently drops them.
    WXLRESULT MSWWindowProc(WXUINT nMsg, WXWPARAM wParam, WXLPARAM lParam) override;

private:
    // --- Construction ---------------------------------------------------------
    void buildLayout();
    wxPanel* buildFlyersPage(wxNotebook* book);
    wxPanel* buildSearchPage(wxNotebook* book);
    wxPanel* buildFavoritesPage(wxNotebook* book);
    wxPanel* buildListPage(wxNotebook* book);
    wxPanel* buildSettingsPage(wxNotebook* book);
    void     buildAccelerators();

    // --- Speech ---------------------------------------------------------------
    // Only for things a screen reader cannot work out on its own: a finished
    // sync, a result count, a confirmation. Selection changes are never
    // announced — the list control already reads the row, and saying it twice is
    // worse than saying nothing.
    void announce(const wxString& text);

    // --- Data refresh ---------------------------------------------------------
    void reloadMerchants();
    void reloadFlyers();
    void reloadFlyerItems();
    void reloadFavorites();
    void reloadFavoriteMatches();
    void reloadList();

    // --- Actions --------------------------------------------------------------
    // `merchantId` 0 syncs every followed banner (F5); a specific id syncs just
    // the one that was ticked.
    void startSync(int merchantId = 0);

    // Ticking several banners in a row must not lose any of them: a sync is
    // already running by the time the second tick lands, so the rest wait here
    // and are started as each run finishes.
    void queueSync(int merchantId);
    void startNextQueuedSync();
    // `moveFocus` false when the search is re-run because a filter changed:
    // stealing focus then would drop the user onto a control of a page the
    // notebook is not even showing.
    void runSearch(bool moveFocus = true);
    void addToList(const model::Item& item);
    void addSelectedToList(wxListCtrl* list, const std::vector<model::Item>& items);
    void exportList();

    // Modal helpers we own, so their buttons and fields carry our labels and our
    // accessible names rather than wxWidgets' untranslated stock ones.
    bool confirm(const wxString& question);
    long askQuantity(const wxString& itemName, int current);   // 0 = cancelled

    // Replaces this window with a freshly built one in `french`.
    //
    // Every label, column header and accessible name is translated at the moment
    // its control is created, so an existing window cannot be retranslated
    // without hunting down each one — and quietly missing some. Rebuilding is a
    // dozen lines and is right by construction.
    void rebuildInLanguage(bool french);

    // --- Event handlers -------------------------------------------------------
    void onSyncProgress(wxThreadEvent& event);
    void onSyncDone(wxThreadEvent& event);
    void onPageChanged(wxBookCtrlEvent& event);
    void onFlyerSelected(wxListEvent& event);
    void onClose(wxCloseEvent& event);

    // --- Helpers --------------------------------------------------------------
    std::string postalCode() const;
    long        selectedRow(wxListCtrl* list) const;

    // Puts the selection back after a list has been refilled.
    //
    // Refilling clears it, and every command in this window acts on the selected
    // row: a sync finishing while the favourites tab is open left the list with
    // focus, rows on screen and nothing selected, so Delete answered "no
    // favorite selected" and appeared to do nothing at all.
    void        restoreSelection(wxListCtrl* list, long wanted) const;
    // Fills a list control from items, in the shared column order.
    void        fillItemList(wxListCtrl* list, const std::vector<model::Item>& items);

    // Rebuilds the read-only location line from what is already known: the
    // cached place name for the current postal code, plus this region coverage.
    // Makes the tab's name the first thing said when a tab is entered.
    //
    // Focus jumps straight into the page, so the screen reader announces the
    // control and never says which tab it belongs to. Raising a separate
    // notification would not fix it: the notification and the focus event are
    // spoken in whatever order they are processed, and the tab name would as
    // often arrive last. The proven remedy is to fold the tab name into the
    // control's accessible NAME for the duration of the switch, so there is one
    // announcement and it begins with the tab.
    // --- Updates --------------------------------------------------------------
    // Same flow as MediaAccess: silent at start-up, spoken only when there is
    // something to say, and the installer does the replacing.
    void startUpdateCheck(bool silent);
    void onUpdateChecked(const updater::Info& info, bool silent);
    void downloadAndApply(const updater::Info& info);

    wxCheckBox*   autoUpdate_      = nullptr;
    wxTimer       updateCheckTimer_;
    std::thread   updateThread_;
    bool          updateInFlight_  = false;

    // Opens the HTML manual of the current language in the default browser.
    void openManual();

    void announce(const wxString& text, int delayMs);
    void announceTabOnFocus(int page);
    void clearTabNameOverrides();

    // The static text in front of each tab's primary control. On Windows a
    // control's announced name comes from that label, not from SetName and not
    // from any accessibility object — see the note in the .cpp — so this is the
    // only handle on what the reader says.
    wxStaticText* primaryLabels_[5] = {};
    wxString      primaryLabelText_[5];

    // The control a tab exists for: the language dropdown on Settings, the flyer
    // list on Flyers, and so on. Focus lands there whenever the tab is entered.
    wxWindow* primaryControl(int page) const;
    // The deepest, last keyboard-focusable control inside .
    static wxWindow* lastFocusable(wxWindow* parent);

    void refreshLocation();

    // The week choice is one value shown by two dropdowns, one on Flyers and one
    // on Search, so it can be changed from wherever the user notices it. These
    // keep the two in step and push the value down to the cache.
    void applyWeek(int selection);
    void syncWeekChoices();

    // Fills a banner dropdown from the banners actually present, keeps the saved
    // choice when that banner is still there, and returns the chosen name —
    // empty for "every banner".
    // Rebuilds the banner dropdown of the favourites tab and narrows the list
    // to the chosen one.
    void applyMatchFilter();

    wxString fillMerchantFilter(wxChoice* choice,
                                const std::vector<wxString>& present,
                                const char* setting);

    // How the three item lists are ordered.
    //
    // One setting for the whole window rather than one per tab: the same
    // question asked in three places should not have three answers to remember,
    // and a screen-reader user pays a real price for having to check.
    enum class Sort { Price = 0, Name, Discount, Brand };

    void   applySort(int selection);
    void   sortItems(std::vector<model::Item>& items) const;
    void   syncSortChoices();
    wxChoice* makeSortChoice(wxWindow* parent);

    // Says what the order means for THIS list — how many items actually carry
    // the value it is sorted on. Half the feed has no percentage and no brand,
    // and silence about that reads as "everything is full price".
    wxString sortSummary(const std::vector<model::Item>& items) const;

    void refreshAllLists();

    // --- Per-item detail ------------------------------------------------------
    // The original price, the format and the SKU live on a per-item endpoint the
    // flyer sync never touches, so they are fetched one item at a time. Requests
    // go through a queue drained by a single worker: an item the user just asked
    // about jumps the line ahead of the favourites being enriched in the
    // background, so an explicit request never waits on a batch.
    void queueDetail(long long itemId, bool speakOnArrival, bool urgent);
    void startDetailWorker();
    void applyDetail(const model::Item& fetched);

    // Ctrl+D, and the Details buttons: acts on whichever item list has focus.
    void announceSelectedDetail();

    // Ctrl+Shift+O, and the product-page buttons. Opens the merchant's own page
    // for the selected item in the default browser.
    void openSelectedProductPage();
    void openProductPage(const std::string& url);
    // True when the item still needs asking about for this parser generation.
    bool needsDetail(const model::Item& item) const;

    // --- The details field ----------------------------------------------------
    // One read-only multi-line field under each item list, holding everything
    // known about the selected item, one fact per line.
    //
    // It exists because a spoken notification is heard once and then gone: it
    // cannot be re-read, cannot be walked word by word, and never reaches a
    // braille display. A text field has a caret, so all three become possible.
    struct DetailPane
    {
        wxTextCtrl*               field = nullptr;
        wxListCtrl*               list  = nullptr;
        std::vector<model::Item>* items = nullptr;
    };

    // Creates the labelled field and registers the pane. Called by each page.
    void addDetailPane(size_t index, wxWindow* parent, wxSizer* sizer,
                       wxListCtrl* list, std::vector<model::Item>* items);

    // Rewrites one pane's field from its list's current selection.
    void refreshDetailPane(size_t index);
    // The pane a list belongs to, or the number of panes when it owns none.
    size_t paneOfList(const wxListCtrl* list) const;

    // Reading a list is walking it row by row. Updating a row after the reader
    // has left it helps nobody — a screen reader does not re-read what changed
    // behind it. So the rows just below the cursor are fetched while the reader
    // is still on the current one, and arrive complete.
    void scheduleDetailPrefetch();
    void prefetchDetailsAroundCursor();
    wxListCtrl* focusedItemList(std::vector<model::Item>*& itemsOut);
    // Resolves a postal code to a place, once, on a worker thread. Does nothing
    // when the code is already cached or a lookup is in flight.
    void requestLocality(const std::string& postalCode);

    db::Database db_;
    SyncService  sync_;

    // Cached rows behind each list, so a row index maps back to real data
    // without re-querying.
    std::vector<model::Merchant> merchants_;
    std::vector<model::Flyer>    flyers_;
    std::vector<model::Item>     flyerItems_;
    std::vector<model::Item>     searchResults_;
    std::vector<model::Favorite> favorites_;
    // What is on screen, and everything there is. The lists show one banner at a
    // time when a banner is chosen, and every command works on the rows the user
    // can actually reach — so the displayed vector is the one the rows index
    // into, and the full one only feeds the dropdown.
    std::vector<model::Item>     favoriteMatches_;
    std::vector<model::Item>     favoriteMatchesAll_;
    std::vector<model::ListEntry> listEntries_;
    std::vector<model::ListEntry> listEntriesAll_;

    wxChoice*     matchMerchant_   = nullptr;
    wxChoice*     listMerchant_    = nullptr;

    wxNotebook*   book_            = nullptr;

    static const size_t kDetailPanes = 3;
    DetailPane    detailPanes_[kDetailPanes];

    wxChoice*     flyerWeek_       = nullptr;
    wxChoice*     flyerMerchant_   = nullptr;
    wxListCtrl*   flyerList_       = nullptr;
    wxListCtrl*   flyerItemList_   = nullptr;

    wxTextCtrl*   searchField_     = nullptr;
    wxChoice*     searchWeek_      = nullptr;

    // One per page that owns an item list: flyers, search, favourites.
    wxChoice*     sortChoices_[3]  = {};
    Sort          sort_            = Sort::Price;
    wxChoice*     searchMerchant_  = nullptr;
    wxTextCtrl*   searchMaxPrice_  = nullptr;
    wxListCtrl*   searchList_      = nullptr;

    wxListCtrl*   favoriteList_    = nullptr;
    wxListCtrl*   favoriteMatchList_ = nullptr;

    wxListCtrl*   shoppingList_    = nullptr;
    // A focusable read-only field, not a wxStaticText: Tab skips static text and
    // nothing reads it aloud, so the item count and the estimated total existed
    // for sighted users only.
    wxTextCtrl*   listTotal_       = nullptr;

    wxChoice*     languageChoice_  = nullptr;
    wxTextCtrl*   postalField_     = nullptr;
    wxTextCtrl*   locationField_   = nullptr;   // read-only, but focusable and readable
    wxCheckBox*   hidePriceless_   = nullptr;
    // A checkbox list-view, not a wxCheckListBox: the latter is owner-drawn on
    // Windows and its ticked state never reaches MSAA or UIA, so a screen-reader
    // user cannot tell which banners are followed. The native list-view exposes
    // the state properly.
    wxListCtrl*   merchantList_    = nullptr;
    wxStaticText* syncStatus_      = nullptr;
    wxButton*     syncButton_      = nullptr;

    // Delays the spoken hint on the postal-code field. The screen reader is
    // already saying the control's own name at the moment focus lands; speaking
    // over it truncates one or the other, so the hint waits a beat.
    wxTimer       postalHintTimer_;
    // Set when the postal field is focused right after something was announced,
    // so the spoken hint does not talk over it.
    bool          suppressPostalHint_ = false;

    // Debounces the look-ahead: arrowing quickly through three hundred rows must
    // not queue three hundred requests.
    wxTimer       detailPrefetchTimer_;

    // Restores the accessible names a moment after the switch, once the reader
    // has read them.
    wxTimer       tabNameTimer_;

    // Holds a notification back until the screen reader has finished saying what
    // the user just did.
    //
    // Picking an entry in a dropdown makes the reader speak the entry. A
    // notification raised in the same handler reaches it first and the count is
    // read BEFORE the choice it describes — which is backwards, and on a long
    // list means hearing a number with nothing yet attached to it. Deferring by
    // a few hundred milliseconds puts the two in the order a person expects.
    wxTimer       announceTimer_;
    wxString      pendingAnnounce_;

    // Page-changed fires while the notebook is still being built, before the
    // controls it would focus exist. Set once the window is fully assembled.
    bool          layoutReady_ = false;

    // One place lookup at a time, off the UI thread. Joined on destruction.
    std::thread       localityThread_;
    std::atomic<bool> localityBusy_{false};
    http::Canceller   localityCanceller_;

    std::thread             detailThread_;
    std::mutex              detailMutex_;
    std::deque<long long>   detailQueue_;
    std::atomic<bool>       detailRunning_{false};
    std::atomic<bool>       detailStop_{false};
    std::atomic<long long>  detailSpeak_{0};   // the one item whose arrival is announced

    // The item whose product page the user asked for before its link had
    // arrived. A slot of its own: detailSpeak_ belongs to Ctrl+D, and sharing it
    // would have one key answer for the other.
    long long               productPageWanted_ = 0;
    http::Canceller         detailCanceller_;

    // Read by the detail worker, written by the UI thread whenever the postal
    // code changes — including in the middle of a running worker. Guarded by
    // detailMutex_ for that reason: an unsynchronised std::string assignment
    // under a concurrent read reallocates the buffer being iterated.
    std::string postalCodeForWorker_;

    // Guards the merchant list against its own repopulation.
    //
    // wxListCtrl::CheckItem DOES emit wxEVT_LIST_ITEM_CHECKED, so ticking rows
    // programmatically runs the very handler that reacts to a user tick. With a
    // sync started on tick, that closed the loop: sync ends, list reloads, rows
    // get re-ticked, each one starts another sync, forever.
    bool populatingMerchants_ = false;

    std::vector<int> pendingSyncs_;       // banners ticked while a sync was running

    // True from the moment a sync is started until its completion event has been
    // handled. SyncService::isRunning() goes false a hair before that event is
    // queued, and a sync started in that gap would let the stale completion
    // announce "sync done" over a run that is still downloading.
    bool             syncPending_ = false;
    wxString         syncingMerchant_;    // name of the banner being fetched, for the announcement

    void* uiaProvider_ = nullptr;   // IRawElementProviderSimple*
};
