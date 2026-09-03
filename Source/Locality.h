#pragma once

#include <string>

namespace http { class Canceller; }

// Turning a postal code into a place a person recognizes.
//
// The point is confirmation, not geography: after typing six characters with no
// visual feedback, hearing "Sorel-Tracy, Québec" is how you know you did not
// mistype your own postal code. Everything else in the app depends on that code
// being right, and a wrong one fails silently — it just returns flyers from
// somewhere else.
//
// Canada Post's own database is proprietary and its lookup service is a paid
// product whose licence does not allow shipping the data inside an application,
// so it is not available to us. OpenStreetMap does not carry Canadian postal
// codes either.
//
// What is available, under CC BY 4.0, is a sector-level open dataset. It is
// embedded in the binary and answers every valid Canadian code offline, which is
// what makes this feature dependable. A rate-limited third-party service is then
// consulted once per postal code, purely to upgrade the sector name to the exact
// municipality; it refuses as often as it answers, and nothing depends on it.
namespace locality
{
    struct Place
    {
        std::string city;        // "Sorel-Tracy"
        std::string province;    // "QC"

        bool empty() const { return city.empty() && province.empty(); }
    };

    // The always-available answer: an embedded table of every Canadian forward
    // sortation area — the first three characters of a postal code — and the
    // place it covers. Offline, instant, no rate limit, and nothing about where
    // the user lives leaves the machine.
    //
    // It is sector-level, so it names the sector rather than the municipality:
    // J3P reads as "Sorel" where the town is Sorel-Tracy. Close enough to
    // confirm a typed postal code, which is the whole job.
    Place fromTable(const std::string& postalCode);

    // Optional refinement, for the exact municipality. Blocking network call —
    // worker threads only; `postalCode` must already be canonical. Returns false
    // on any failure, including the provider's rate limit, which it hits readily:
    // this is a bonus on top of fromTable(), never something to depend on.
    bool lookup(const std::string& postalCode, Place& out,
                http::Canceller* canceller = nullptr);

    // "Sorel-Tracy, Québec" in the UI language, or an empty string for an empty
    // Place. The province code is spelled out because a screen reader reads "QC"
    // as two letters.
    std::string describe(const Place& place);

    // Serialization for the settings cache: "Sorel-Tracy|QC".
    std::string encode(const Place& place);
    Place       decode(const std::string& encoded);
}
