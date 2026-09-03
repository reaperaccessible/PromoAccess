#pragma once

#include "Model.h"

#include <wx/dialog.h>
#include <vector>

class wxTextCtrl;
class wxListCtrl;
class wxCheckBox;

// Creates or edits one favourite.
//
// A favourite is a rule, not a pinned product: flyer items get new identifiers
// every week, so a favourite pointing at one would be dead in seven days. What
// is stored is the words to look for, optionally narrowed to a set of banners
// and a price ceiling, re-evaluated against the cache at every sync.
//
// Deliberately four plain controls in a vertical column, each with a real label
// control associated to it, and nothing else. wxWidgets gives every one of them
// a native MSAA/UIA identity for free, so JAWS and NVDA read the label, the type
// and the value on Tab with no code at all — which no custom-drawn form ever
// manages.
class FavoriteDialog : public wxDialog
{
public:
    // `merchants` seeds the banner list; pass the followed ones. `existing`
    // is nullptr for a new watch.
    FavoriteDialog(wxWindow* parent,
                   const std::vector<model::Merchant>& merchants,
                   const model::Favorite* existing);

    // Valid once the dialog returned wxID_OK.
    model::Favorite result() const;

private:
    void onOk(wxCommandEvent& event);

    wxTextCtrl* patternCtrl_  = nullptr;
    wxListCtrl* merchantCtrl_ = nullptr;
    wxTextCtrl* maxPriceCtrl_ = nullptr;
    wxCheckBox* wholeWordsCtrl_ = nullptr;
    wxCheckBox* enabledCtrl_  = nullptr;

    std::vector<model::Merchant> merchants_;
    model::Favorite              favorite_;
};
