#pragma once

#include <string>

// Canadian postal codes, in the one form PromoAccess accepts.
//
// The rule is deliberately strict: six characters in a single block, no space
// and no hyphen, upper or lower case as the user prefers. Two reasons.
//
// First, the postal code is not merely a search parameter here, it is the key
// flyers are filed under in the cache. One spelling means one key; the day the
// same code arrives written differently, the same flyers get filed under a
// second zone and the app looks empty until a full resync.
//
// Second, a field that quietly accepts several shapes cannot tell the user what
// it expects. One documented form can be announced on focus, in one short
// sentence, and either matches or does not.
namespace postal
{
    // "j3p7s7" and "J3P7S7" both become "J3P7S7". Surrounding whitespace is
    // tolerated because it is invisible and never intended; anything else -- an
    // inner space, a hyphen, a seventh character -- makes this return an empty
    // string, which callers treat the same as "nothing typed".
    std::string canonical(const std::string& input);

    // True when canonical() would return something.
    bool isValid(const std::string& input);
}
