#include "Postal.h"

#include <cctype>

namespace postal
{
namespace
{
    // Removes leading and trailing whitespace only. Everything inside is left
    // alone on purpose: an inner space or a hyphen has to fail validation, not be
    // silently swallowed, or the field would once again accept several spellings
    // of the same code.
    std::string trim(const std::string& input)
    {
        size_t first = 0;
        while (first < input.size() && std::isspace(static_cast<unsigned char>(input[first])))
            ++first;

        size_t last = input.size();
        while (last > first && std::isspace(static_cast<unsigned char>(input[last - 1])))
            --last;

        return input.substr(first, last - first);
    }
}

std::string canonical(const std::string& input)
{
    const std::string block = trim(input);

    if (block.size() != 6)
        return {};

    // A Canadian postal code alternates letter and digit: A1A1A1. Checking the
    // shape catches the mistakes that actually happen -- a missing character, a
    // transposition, something pasted from the wrong field.
    //
    // Deliberately NOT enforced: the rule that D, F, I, O, Q and U never appear.
    // It would catch a typed "O" for a zero, but a validator that rejects a real
    // address is worse than one that lets a wrong code through -- in that case
    // the sync simply comes back empty and the user tries again.
    std::string out;
    out.reserve(6);

    for (size_t n = 0; n < 6; ++n)
    {
        const unsigned char c = static_cast<unsigned char>(block[n]);
        const bool wantLetter = (n % 2 == 0);

        if (wantLetter)
        {
            if (!std::isalpha(c))
                return {};
            out += static_cast<char>(std::toupper(c));
        }
        else
        {
            if (!std::isdigit(c))
                return {};
            out += static_cast<char>(c);
        }
    }

    return out;
}

bool isValid(const std::string& input)
{
    return !canonical(input).empty();
}

} // namespace postal
