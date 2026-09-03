#include "Locality.h"
#include "Http.h"
#include "Localization.h"
#include "Text.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>

using json = nlohmann::json;

namespace locality
{
namespace
{
    // The provider returns city names in a flattened case ("Sorel-tracy",
    // "Saint-jean-sur-richelieu"), which a screen reader renders as written and
    // which simply looks wrong. Rebuilt here word by word.
    //
    // French particles stay lower-case: "Saint-Jean-sur-Richelieu", not
    // "Saint-Jean-Sur-Richelieu". A particle is never the first word.
    bool isParticle(const std::string& word)
    {
        static const char* kParticles[] =
        { "sur", "sous", "de", "du", "des", "la", "le", "les", "aux", "au", "et", "en", "a" };

        return std::any_of(std::begin(kParticles), std::end(kParticles),
                           [&word](const char* p) { return word == p; });
    }

    std::string titleCase(const std::string& input)
    {
        std::string out;
        out.reserve(input.size());

        std::string word;
        bool first = true;

        // Flushes the word just read, capitalizing it unless it is a particle in
        // a position other than the first.
        auto flush = [&]
        {
            if (word.empty())
                return;

            if (!(isParticle(word) && !first))
                word[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(word[0])));

            out += word;
            word.clear();
            first = false;
        };

        for (const char c : input)
        {
            if (c == '-' || c == ' ' || c == '\'')
            {
                flush();
                out += c;
            }
            else
            {
                word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
        }
        flush();

        return out;
    }

    // Province and territory codes spelled out. A screen reader says "QC" as two
    // letters, which tells a Quebecer nothing they did not already assume.
    std::string provinceName(const std::string& code)
    {
        struct Entry { const char* code; const char* en; const char* fr; };
        static const Entry kProvinces[] =
        {
            { "AB", "Alberta",                   "Alberta" },
            { "BC", "British Columbia",          "Colombie-Britannique" },
            { "MB", "Manitoba",                  "Manitoba" },
            { "NB", "New Brunswick",             "Nouveau-Brunswick" },
            { "NL", "Newfoundland and Labrador", "Terre-Neuve-et-Labrador" },
            { "NS", "Nova Scotia",               "Nouvelle-Écosse" },
            { "NT", "Northwest Territories",     "Territoires du Nord-Ouest" },
            { "NU", "Nunavut",                   "Nunavut" },
            { "ON", "Ontario",                   "Ontario" },
            { "PE", "Prince Edward Island",      "Île-du-Prince-Édouard" },
            { "QC", "Quebec",                    "Québec" },
            { "SK", "Saskatchewan",              "Saskatchewan" },
            { "YT", "Yukon",                     "Yukon" },
        };

        for (const Entry& e : kProvinces)
            if (code == e.code)
                return loc::tr(e.en, e.fr).utf8_string();

        return code;   // an unknown code is still better said than dropped
    }
}

namespace
{
    struct TableEntry
    {
        const char* fsa;
        const char* place;
        const char* province;
    };

    // Sorted by code; see the generator named in the file header.
    const TableEntry kTable[] =
    {
#include "PostalPlaces.inc"
    };
}

Place fromTable(const std::string& postalCode)
{
    Place place;

    if (postalCode.size() < 3)
        return place;

    const std::string fsa = postalCode.substr(0, 3);

    const auto found = std::lower_bound(std::begin(kTable), std::end(kTable), fsa,
        [](const TableEntry& entry, const std::string& key) { return entry.fsa < key; });

    if (found != std::end(kTable) && fsa == found->fsa)
    {
        place.city     = found->place;
        place.province = found->province;
    }

    return place;
}

bool lookup(const std::string& postalCode, Place& out, http::Canceller* canceller)
{
    out = {};

    if (postalCode.size() != 6)
        return false;

    const wxString url =
        wxString::Format("https://geocoder.ca/?postal=%s&json=1",
                         wxString::FromUTF8(postalCode));

    std::string body;
    if (!http::getToString(url, body, canceller).ok)
        return false;

    const json doc = json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object())
        return false;

    // The provider answers a rate-limited request with success:false rather than
    // an HTTP error. Treated as "not now" — the caller keeps no cache entry, so a
    // later attempt is free to succeed.
    if (doc.contains("success") && doc["success"].is_boolean() && !doc["success"].get<bool>())
        return false;

    const auto standard = doc.find("standard");
    if (standard == doc.end() || !standard->is_object())
        return false;

    const auto city = standard->find("city");
    const auto prov = standard->find("prov");

    if (city != standard->end() && city->is_string())
        out.city = titleCase(city->get<std::string>());

    if (prov != standard->end() && prov->is_string())
        out.province = prov->get<std::string>();

    return !out.empty();
}

std::string describe(const Place& place)
{
    if (place.empty())
        return {};

    if (place.city.empty())
        return provinceName(place.province);

    if (place.province.empty())
        return place.city;

    // Quebec City sits in Quebec: saying it twice is noise, not precision.
    //
    // Compared through the accent fold, not literally: the provider returns the
    // city as "Quebec" while the French province name is "Québec", so a literal
    // comparison matched in English and never in French — which is the one
    // language where the repetition would actually be heard.
    const std::string province = provinceName(place.province);
    if (text::normalize(place.city) == text::normalize(province))
        return province;

    return place.city + ", " + province;
}

std::string encode(const Place& place)
{
    return place.city + "|" + place.province;
}

Place decode(const std::string& encoded)
{
    Place place;

    const size_t bar = encoded.find('|');
    if (bar == std::string::npos)
        return place;

    place.city     = encoded.substr(0, bar);
    place.province = encoded.substr(bar + 1);
    return place;
}

} // namespace locality
