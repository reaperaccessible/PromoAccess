cmake_minimum_required(VERSION 3.22)
project(PromoAccess VERSION 1.0.7 LANGUAGES C CXX)   # display form is 1.07, see Source/Version.h

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- wxWidgets (static, via vcpkg x64-windows-static) -------------------------
# Configure with:
#   -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
#   -DVCPKG_TARGET_TRIPLET=x64-windows-static
set(wxWidgets_USE_STATIC ON)
find_package(wxWidgets 3.2 REQUIRED COMPONENTS core base)
include(${wxWidgets_USE_FILE})

find_package(nlohmann_json CONFIG REQUIRED)

# Match wxWidgets' static-CRT build (x64-windows-static uses /MT).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# --- SQLite -------------------------------------------------------------------
# Vendored as the official amalgamation rather than pulled from vcpkg: it keeps
# the build self-contained (single .c file, no extra port to install on a fresh
# machine) and pins the exact version the schema was written against.
add_library(promo_sqlite STATIC Source/ThirdParty/sqlite/sqlite3.c)
target_include_directories(promo_sqlite PUBLIC Source/ThirdParty/sqlite)
target_compile_definitions(promo_sqlite PRIVATE
    SQLITE_THREADSAFE=1           # the sync worker and the UI both touch it
    SQLITE_DEFAULT_FOREIGN_KEYS=1
    SQLITE_OMIT_LOAD_EXTENSION=1  # nothing loads extensions; drops attack surface
    SQLITE_DQS=0                  # double-quoted strings are errors, not silent identifiers
)
if(MSVC)
    # The amalgamation trips a pile of MSVC's C4996/C4244 noise that is not ours
    # to fix; silencing it keeps a real warning in our own code visible.
    target_compile_options(promo_sqlite PRIVATE /w)
endif()

# --- Core: everything that is not the UI --------------------------------------
# Kept as its own library so the console harness and the GUI link the exact same
# code, and so a UI change can never quietly alter sync or matching behaviour.
add_library(promo_core STATIC
    Source/Http.cpp
    Source/Text.cpp
    Source/Postal.cpp
    Source/Locality.cpp
    Source/Paths.cpp
    Source/Localization.cpp
    Source/Database.cpp
    Source/FlippSource.cpp
    Source/Format.cpp
    Source/Export.cpp
    Source/Updater.cpp
)
target_include_directories(promo_core PUBLIC Source)
target_link_libraries(promo_core PUBLIC
    promo_sqlite
    nlohmann_json::nlohmann_json
    ${wxWidgets_LIBRARIES}
    winhttp
)
if(MSVC)
    # UTF-8 source + execution charset for the accented EN/FR string literals.
    target_compile_options(promo_core PRIVATE /utf-8)
endif()

# --- Console harness ----------------------------------------------------------
# Exercises the source adapter and the cache without any UI in the way. This is
# what proves a feed change broke the sync rather than the window.
add_executable(promotest Source/Tools/PromoTest.cpp)
target_link_libraries(promotest PRIVATE promo_core)
if(MSVC)
    target_compile_options(promotest PRIVATE /utf-8)
endif()

# --- The application ----------------------------------------------------------
add_executable(PromoAccess WIN32
    Source/App.cpp
    Source/MainWindow.cpp
    Source/FavoriteDialog.cpp
    Source/SyncService.cpp
    Source/PromoAccess.rc      # manifest (common controls v6 + DPI) + version info
)
target_link_libraries(PromoAccess PRIVATE
    promo_core
    shell32           # SHGetKnownFolderPath
    ole32             # CoTaskMemFree
    oleaut32          # SysAllocString / SysFreeString for UIA notification text
    uiautomationcore  # UiaRaiseNotificationEvent — how NVDA and JAWS hear us
)
if(MSVC)
    target_compile_options(PromoAccess PRIVATE /utf-8)
endif()

# The manifest is embedded through the .rc file; suppress the linker's own so
# there is not a second, conflicting one.
set_target_properties(PromoAccess PROPERTIES LINK_FLAGS "/MANIFEST:NO")

# The manual is a document read at run time, not a resource compiled in, so it
# has to sit beside the executable exactly as the installer will place it.
# Without this a development build finds nothing under Docs and F1 says so.
add_custom_command(TARGET PromoAccess POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/Docs"
            "$<TARGET_FILE_DIR:PromoAccess>/Docs"
    COMMENT "Copying Docs beside PromoAccess.exe")
