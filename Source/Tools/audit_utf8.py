# -*- coding: utf-8 -*-
"""Flags non-ASCII string literals that may reach wxString unconverted.

wxString reads a narrow char* as Latin-1, never as UTF-8, even when the whole
translation unit is compiled with /utf-8. A bare "Français" handed to
wxChoice::Append therefore reaches the screen as "FranÃ§ais". Every non-ASCII
literal must go through loc::tr(), wxString::FromUTF8() or the local u8() helper
— or be a wide literal, or never touch wxString at all.

This is an aid for review, not a gate. It cannot know whether a std::string ends
up in a wxString three calls later, so it errs towards reporting: read each hit
and judge it. What it does guarantee is that nothing is missed by accident.

Run from the project root:
    python Source/Tools/audit_utf8.py
"""
import io
import glob
import os
import re
import sys

sys.stdout.reconfigure(encoding="utf-8")
os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

LITERAL = re.compile(r'"(?:[^"\\]|\\.)*"')

# Calls whose arguments are converted. Matched against the function that
# actually encloses the literal, found by walking back to the unbalanced opening
# parenthesis — not against a fixed window of preceding text, which both missed
# the second argument of a loc::tr split over two lines and whitelisted every
# literal in any statement that mentioned one anywhere.
SAFE_CALLS = ("tr", "FromUTF8", "u8")


def enclosing_call(code, position):
    """Name of the function whose argument list contains `position`, or ""."""
    depth = 0
    i = position - 1

    while i >= 0:
        c = code[i]
        if c == ")":
            depth += 1
        elif c == "(":
            if depth == 0:
                break
            depth -= 1
        i -= 1

    if i < 0:
        return ""

    end = i
    while end > 0 and code[end - 1].isspace():
        end -= 1

    start = end
    while start > 0 and (code[start - 1].isalnum() or code[start - 1] in "_:"):
        start -= 1

    return code[start:end].split(":")[-1]


def strip_comments(source):
    """Blanks out // and /* */ comments, preserving offsets and line breaks."""
    out = []
    i, n = 0, len(source)
    in_string = in_line = in_block = False

    while i < n:
        c = source[i]
        two = source[i:i + 2]

        if in_line:
            if c == "\n":
                in_line = False
                out.append(c)
            else:
                out.append(" ")
        elif in_block:
            if two == "*/":
                in_block = False
                out.append("  ")
                i += 2
                continue
            out.append("\n" if c == "\n" else " ")
        elif in_string:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(source[i + 1])
                i += 2
                continue
            if c == '"':
                in_string = False
        else:
            if two == "//":
                in_line = True
                out.append("  ")
                i += 2
                continue
            if two == "/*":
                in_block = True
                out.append("  ")
                i += 2
                continue
            if c == '"':
                in_string = True
            out.append(c)

        i += 1

    return "".join(out)


def main():
    # Headers too: an earlier version globbed only *.cpp, so a literal in a
    # header was never examined at all.
    files = sorted(glob.glob("Source/*.h") + glob.glob("Source/*.cpp")
                   + glob.glob("Source/Tools/*.cpp"))

    reported = 0
    for path in files:
        raw = io.open(path, encoding="utf-8").read()
        code = strip_comments(raw)

        for match in LITERAL.finditer(code):
            text = match.group(0)
            if all(ord(c) < 128 for c in text):
                continue

            if enclosing_call(code, match.start()) in SAFE_CALLS:
                continue

            # A wide literal is safe whatever surrounds it.
            if match.start() > 0 and code[match.start() - 1] == "L":
                continue

            line = code.count("\n", 0, match.start()) + 1
            reported += 1
            print("%s:%d" % (path, line))
            print("   %s" % raw.split("\n")[line - 1].strip()[:120])

    print()
    print("%d literal(s) to review by hand" % reported)
    return 0


if __name__ == "__main__":
    sys.exit(main())
