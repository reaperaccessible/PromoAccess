#pragma once

#include "Model.h"

#include <wx/string.h>
#include <vector>

// Writing the shopping list out to a file.
//
// The target reader is the user standing in an aisle with a phone, so the output
// is grouped by store, one short line per item, and plain enough that any reader
// on any device speaks it correctly. No PDF, no layout, no columns to get lost
// in — the format is the accessibility feature.
namespace exporter
{
    enum class Format { Text, Markdown, Csv };

    // Picks the format from the file's extension (.md, .csv, anything else text).
    Format formatForPath(const wxString& path);

    // Writes `entries` to `path` as UTF-8. Returns false and fills `error` if the
    // file cannot be written.
    bool write(const wxString& path,
               Format format,
               const std::vector<model::ListEntry>& entries,
               wxString& error);
}
