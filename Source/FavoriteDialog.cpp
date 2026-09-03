#include "FavoriteDialog.h"
#include "Localization.h"

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/listctrl.h>
#include <wx/checkbox.h>
#include <wx/msgdlg.h>

#include <algorithm>

FavoriteDialog::FavoriteDialog(wxWindow* parent,
                               const std::vector<model::Merchant>& merchants,
                               const model::Favorite* existing)
    : wxDialog(parent, wxID_ANY,
               existing ? loc::tr("Edit favorite", "Modifier le favori")
                        : loc::tr("New favorite", "Nouveau favori"),
               wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , merchants_(merchants)
{
    if (existing != nullptr)
        favorite_ = *existing;

    auto* root = new wxBoxSizer(wxVERTICAL);
    const int border = FromDIP(8);

    // --- Words to watch -------------------------------------------------------
    auto* patternLabel = new wxStaticText(this, wxID_ANY,
        loc::tr("Words to look for:", "Mots à rechercher :"));
    root->Add(patternLabel, 0, wxLEFT | wxRIGHT | wxTOP, border);

    patternCtrl_ = new wxTextCtrl(this, wxID_ANY, wxString::FromUTF8(favorite_.pattern));
    // The name is what a screen reader announces on focus; the static label above
    // is for sighted users and for the ones that do pick it up.
    patternCtrl_->SetName(loc::tr("Words to look for", "Mots à rechercher"));
    root->Add(patternCtrl_, 0, wxEXPAND | wxLEFT | wxRIGHT, border);

    auto* hint = new wxStaticText(this, wxID_ANY,
        loc::tr("Every word must appear. Accents are ignored.",
                "Tous les mots doivent apparaître. Les accents sont ignorés."));
    root->Add(hint, 0, wxLEFT | wxRIGHT | wxBOTTOM, border);

    // --- Banner ---------------------------------------------------------------
    auto* merchantLabel = new wxStaticText(this, wxID_ANY,
        loc::tr("Banners, space to check (none checked = all of them):",
                "Bannières, espace pour cocher (aucune cochée = toutes) :"));
    root->Add(merchantLabel, 0, wxLEFT | wxRIGHT, border);

    // Checkboxes rather than a dropdown: a watch is worth keeping at several
    // stores at once, and a single-choice control forced one favourite per
    // banner. wxListCtrl and not wxCheckListBox — the latter is owner-drawn on
    // Windows and its ticked state never reaches MSAA or UIA, so a screen reader
    // could not tell a checked banner from an unchecked one.
    merchantCtrl_ = new wxListCtrl(this, wxID_ANY, wxDefaultPosition,
                                   FromDIP(wxSize(-1, 140)),
                                   wxLC_REPORT | wxLC_NO_HEADER | wxLC_SINGLE_SEL);
    merchantCtrl_->SetName(loc::tr("Banners", "Bannières"));
    merchantCtrl_->InsertColumn(0, loc::tr("Banner", "Bannière"), wxLIST_FORMAT_LEFT, FromDIP(340));
    merchantCtrl_->EnableCheckBoxes(true);

    for (size_t n = 0; n < merchants_.size(); ++n)
    {
        const long row = merchantCtrl_->InsertItem(static_cast<long>(n),
                                                   wxString::FromUTF8(merchants_[n].name));

        const bool watched = std::find(favorite_.merchantIds.begin(),
                                       favorite_.merchantIds.end(),
                                       merchants_[n].id) != favorite_.merchantIds.end();
        if (watched)
            merchantCtrl_->CheckItem(row, true);
    }

    // Something is selected from the start, so arriving on the list announces a
    // row instead of an empty control.
    if (merchantCtrl_->GetItemCount() > 0)
        merchantCtrl_->SetItemState(0, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                                    wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);

    root->Add(merchantCtrl_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, border);

    // --- Price ceiling --------------------------------------------------------
    auto* priceLabel = new wxStaticText(this, wxID_ANY,
        loc::tr("Only under this price (leave empty for any price):",
                "Seulement sous ce prix (vide pour tout prix) :"));
    root->Add(priceLabel, 0, wxLEFT | wxRIGHT, border);

    wxString priceText;
    if (favorite_.maxPrice > 0.0)
        priceText = wxString::Format("%.2f", favorite_.maxPrice);

    maxPriceCtrl_ = new wxTextCtrl(this, wxID_ANY, priceText);
    maxPriceCtrl_->SetName(loc::tr("Maximum price", "Prix maximum"));
    root->Add(maxPriceCtrl_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, border);

    // --- How the words are matched --------------------------------------------
    wholeWordsCtrl_ = new wxCheckBox(this, wxID_ANY,
        loc::tr("Whole words only", "Mots entiers seulement"));
    wholeWordsCtrl_->SetValue(favorite_.wholeWords);
    root->Add(wholeWordsCtrl_, 0, wxLEFT | wxRIGHT, border);

    auto* wordsHint = new wxStaticText(this, wxID_ANY,
        loc::tr("On: garlic finds garlic, and not chicken wings. "
                "Off: a fragment is found anywhere, so fromag finds fromage.",
                "Coché : « ail » trouve l'ail, et pas les ailes de poulet. "
                "Décoché : un fragment est trouvé partout, « fromag » trouve fromage."));
    root->Add(wordsHint, 0, wxLEFT | wxRIGHT | wxBOTTOM, border);

    // --- Active ---------------------------------------------------------------
    // "Actif", masculine: it qualifies "un favori". The dialog is headed
    // "Modifier le favori", so the feminine form was a plain agreement error.
    enabledCtrl_ = new wxCheckBox(this, wxID_ANY, loc::tr("Active", "Actif"));
    enabledCtrl_->SetValue(favorite_.enabled);
    root->Add(enabledCtrl_, 0, wxLEFT | wxRIGHT | wxBOTTOM, border);

    root->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, border);
    loc::translateStockButtons(this);

    SetSizerAndFit(root);
    SetSize(FromDIP(wxSize(420, -1)));
    CentreOnParent();

    Bind(wxEVT_BUTTON, &FavoriteDialog::onOk, this, wxID_OK);

    patternCtrl_->SetFocus();
}

void FavoriteDialog::onOk(wxCommandEvent& event)
{
    const wxString pattern = patternCtrl_->GetValue().Trim(true).Trim(false);

    // A favourite with no words would match every item in every flyer. Refusing
    // it here beats letting the user discover four thousand "matches" later.
    if (pattern.empty())
    {
        wxMessageBox(loc::tr("Type at least one word to look for.",
                             "Entrez au moins un mot à rechercher."),
                     GetTitle(), wxOK | wxICON_INFORMATION, this);
        patternCtrl_->SetFocus();
        return;
    }

    favorite_.pattern = pattern.utf8_string();

    favorite_.merchantIds.clear();
    favorite_.merchantName.clear();

    for (long row = 0; row < merchantCtrl_->GetItemCount(); ++row)
    {
        if (!merchantCtrl_->IsItemChecked(row) || static_cast<size_t>(row) >= merchants_.size())
            continue;

        favorite_.merchantIds.push_back(merchants_[row].id);

        // Kept as text as well, so the favourites list can name the banners
        // without looking every identifier up again.
        if (!favorite_.merchantName.empty())
            favorite_.merchantName += ", ";
        favorite_.merchantName += merchants_[row].name;
    }

    // Accepts both decimal marks: a French keyboard produces "4,99" and typing
    // it should not silently mean "no ceiling".
    wxString priceText = maxPriceCtrl_->GetValue().Trim(true).Trim(false);
    priceText.Replace(",", ".");

    double parsed = 0.0;
    favorite_.maxPrice = (!priceText.empty() && priceText.ToCDouble(&parsed) && parsed > 0.0)
                       ? parsed : 0.0;

    favorite_.wholeWords = wholeWordsCtrl_->GetValue();
    favorite_.enabled = enabledCtrl_->GetValue();

    event.Skip();   // lets the dialog close with wxID_OK
}

model::Favorite FavoriteDialog::result() const
{
    return favorite_;
}
