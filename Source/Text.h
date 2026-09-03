#pragma once

#include <string>
#include <vector>

// Text handling shared by the matcher, the search box and the database index.
namespace text
{
    // Lower-cases and strips diacritics, returning UTF-8.
    //
    // This is the single most important function for a French-language flyer
    // app: the same product is advertised as "Boeuf hache", "boeuf hache",
    // "Boeuf Hache mi-maigre" and "BOEUF HACHE" in the same week, across four
    // banners, with and without accents. Every stored name gets normalized once
    // at insert time and every query gets normalized the same way, so a
    // favourite typed without accents still matches.
    std::string normalize(const std::string& utf8);

    // Splits on whitespace after normalizing. Empty tokens are dropped.
    std::vector<std::string> tokens(const std::string& utf8);

    // True when every token of `pattern` appears somewhere in `haystack`.
    // Deliberately AND-of-substrings rather than a regex or a phrase match:
    // "poulet olymel" must find "Hauts de cuisse de poulet Olymel desosses",
    // where neither a phrase search nor a strict word order would.
    bool matchesAllTokens(const std::string& haystackNormalized,
                          const std::vector<std::string>& patternTokens);
}
