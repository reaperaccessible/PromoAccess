#include "Text.h"

#include <wx/string.h>
#include <algorithm>
#include <cwctype>

namespace text
{
namespace
{
    // Folds one code point to its unaccented LOWER-CASE base letter.
    //
    // Both cases are listed on purpose. An earlier version lower-cased first and
    // folded afterwards, which looked equivalent and was not: std::towlower runs
    // in the process's "C" locale, where it leaves every non-ASCII character
    // untouched. "CÉRÉALES" therefore came out of the normalizer as "cÉrÉales",
    // accents intact, and no accentless search could ever match it — 37% of a
    // real cache was unreachable, precisely the ALL-CAPS names Metro, IGA and
    // Maxi publish. Fold first, and never rely on towlower for anything but
    // ASCII.
    //
    // Covers Latin-1 and Latin Extended-A, which is every accent French uses
    // plus the Spanish and Portuguese forms that appear in ethnic-grocery
    // flyers.
    wchar_t fold(wchar_t c)
    {
        switch (c)
        {
            case L'À': case L'Á': case L'Â': case L'Ã':
            case L'Ä': case L'Å': case L'Ā': case L'Ă': case L'Ą':
            case L'à': case L'á': case L'â': case L'ã':
            case L'ä': case L'å': case L'ā': case L'ă': case L'ą':
                return L'a';

            case L'Ç': case L'Ć': case L'Ĉ': case L'Č':
            case L'ç': case L'ć': case L'ĉ': case L'č':
                return L'c';

            case L'È': case L'É': case L'Ê': case L'Ë':
            case L'Ē': case L'Ĕ': case L'Ė': case L'Ę': case L'Ě':
            case L'è': case L'é': case L'ê': case L'ë':
            case L'ē': case L'ĕ': case L'ė': case L'ę': case L'ě':
                return L'e';

            case L'Ì': case L'Í': case L'Î': case L'Ï':
            case L'Ī': case L'Į':
            case L'ì': case L'í': case L'î': case L'ï':
            case L'ī': case L'į':
                return L'i';

            case L'Ñ': case L'Ń': case L'Ň':
            case L'ñ': case L'ń': case L'ň':
                return L'n';

            case L'Ò': case L'Ó': case L'Ô': case L'Õ':
            case L'Ö': case L'Ō': case L'Ŏ': case L'Ő':
            case L'ò': case L'ó': case L'ô': case L'õ':
            case L'ö': case L'ō': case L'ŏ': case L'ő':
                return L'o';

            case L'Ù': case L'Ú': case L'Û': case L'Ü':
            case L'Ū': case L'Ŭ': case L'Ů': case L'Ű':
            case L'ù': case L'ú': case L'û': case L'ü':
            case L'ū': case L'ŭ': case L'ů': case L'ű':
                return L'u';

            case L'Ý': case L'Ÿ':
            case L'ý': case L'ÿ':
                return L'y';

            default:
                // ASCII only past this point, where towlower is dependable.
                return static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c)));
        }
    }
}

std::string normalize(const std::string& utf8)
{
    const wxString ws = wxString::FromUTF8(utf8);

    std::wstring out;
    out.reserve(ws.length());

    for (const wxUniChar ch : ws)
    {
        const wchar_t c = static_cast<wchar_t>(ch.GetValue());

        // The two ligatures decompose into two letters, so they cannot go
        // through the one-to-one fold table: "coeur de boeuf" and its ligatured
        // spelling have to normalize to the same string. Both cases again.
        if (c == L'œ' || c == L'Œ') { out += L"oe"; continue; }
        if (c == L'æ' || c == L'Æ') { out += L"ae"; continue; }

        out += fold(c);
    }

    return wxString(out).utf8_string();
}

std::vector<std::string> tokens(const std::string& utf8)
{
    const std::string norm = normalize(utf8);

    std::vector<std::string> result;
    std::string current;

    for (const char c : norm)
    {
        // Punctuation splits like whitespace: a favourite typed as "boeuf,
        // hache" or a flyer name written "Poulet/Dinde" must still tokenize.
        //
        // '%' and '_' are separators too, and not for readability: the tokens
        // become LIKE patterns, where those two are wildcards. Leaving them in
        // would turn a search for "50% de rabais" into a pattern matching
        // anything at all.
        const bool separator = (static_cast<unsigned char>(c) <= ' ')
                            || c == ',' || c == ';' || c == '/' || c == '|'
                            || c == '(' || c == ')' || c == '.'
                            || c == '%' || c == '_';
        if (separator)
        {
            if (!current.empty())
            {
                result.push_back(current);
                current.clear();
            }
        }
        else
        {
            current += c;
        }
    }

    if (!current.empty())
        result.push_back(current);

    return result;
}

bool matchesAllTokens(const std::string& haystackNormalized,
                      const std::vector<std::string>& patternTokens)
{
    if (patternTokens.empty())
        return false;

    for (const std::string& t : patternTokens)
        if (haystackNormalized.find(t) == std::string::npos)
            return false;

    return true;
}

} // namespace text
