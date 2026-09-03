#pragma once

// PromoAccess — version constants. Single source of truth, consumed by the
// window title, the manual, the resource file and the CMake project version.
//
// Numbering matches the other ReaperAccessible products: MAJOR.MINOR with the
// minor always on two digits — 1.00, 1.01, 1.02 … 1.99, and only then 2.00.
// PROMO_VERSION_STR is therefore written out rather than assembled from the
// numbers, which would print 1.0 where the catalog and the installer expect
// 1.00.

#define PROMO_VERSION_MAJOR 1
#define PROMO_VERSION_MINOR 12
#define PROMO_VERSION_PATCH 0

#define PROMO_VERSION_STR "1.12"

#define PROMO_APP_NAME    "PromoAccess"
#define PROMO_PUBLISHER   "ReaperAccessible"
#define PROMO_COPYRIGHT   "Copyright (C) 2026 ReaperAccessible"

// The window caption, in one place: the application builds it at startup and the
// window rebuilds itself with it when the interface language changes.
#define PROMO_WINDOW_TITLE PROMO_APP_NAME " " PROMO_VERSION_STR
