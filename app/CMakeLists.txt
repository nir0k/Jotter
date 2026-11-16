cmake_minimum_required(VERSION 4.3)

# The product version lives only in the repository's VERSION file.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" JOTTER_VERSION LIMIT_COUNT 1)
string(STRIP "${JOTTER_VERSION}" JOTTER_VERSION)

project(Jotter VERSION ${JOTTER_VERSION} LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(JOTTER_WERROR "Treat compiler warnings as errors" OFF)
option(JOTTER_BUILD_TESTS "Build the unit tests" ON)

# Qt 6.9 is the minimum: it brings UUID version 7 and matches the KDE Flatpak runtime. Geotags
# ask GeoClue2 for the position through Qt Positioning.
find_package(Qt6 6.9 REQUIRED COMPONENTS
    Core Concurrent Gui Network Positioning Widgets Sql Svg Test LinguistTools)
# KDE Frameworks 6.13 came with the KDE Flatpak runtime 6.9; code blocks highlight with it and
# exports of several files are packed into ZIP archives with KArchive.
find_package(KF6SyntaxHighlighting 6.13 REQUIRED)
find_package(KF6Archive 6.13 REQUIRED)
# Search registers its FTS5 tokenizer through the SQLite library Qt's driver uses; 3.20 brought
# sqlite3_bind_pointer(), which hands out the FTS5 interface. CMake 4.3 named its target
# SQLite3::SQLite3, which is why it is the minimum.
find_package(SQLite3 3.20 REQUIRED)
# Synchronisation talks SFTP through libssh; 0.11 brought the asynchronous reads and writes and
# the server limits that large files are transferred with.
find_package(libssh 0.11 REQUIRED CONFIG)
# Pasted Markdown is parsed with md4c, the parser Qt embeds, called directly so notes keep what
# Qt's importer drops: formatting around inline code and line breaks. 0.5.1 is Fedora 44's
# release; the library installs no CMake version file, so pkg-config checks the version.
find_package(PkgConfig REQUIRED)
pkg_check_modules(MD4C REQUIRED IMPORTED_TARGET md4c>=0.5.1)
# The Snowball stemmers (libstemmer) reduce Russian and English words to their stems; the
# library installs no version information or CMake package.
find_path(STEMMER_INCLUDE_DIR libstemmer.h REQUIRED)
find_library(STEMMER_LIBRARY stemmer REQUIRED)
add_library(Stemmer::Stemmer UNKNOWN IMPORTED)
set_target_properties(Stemmer::Stemmer PROPERTIES
    IMPORTED_LOCATION "${STEMMER_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${STEMMER_INCLUDE_DIR}")

qt_standard_project_setup(I18N_SOURCE_LANGUAGE en I18N_TRANSLATED_LANGUAGES ru)

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
if(JOTTER_WERROR)
    add_compile_options(-Werror)
endif()
add_compile_definitions(
    QT_NO_CAST_FROM_ASCII
    QT_NO_CAST_TO_ASCII
    QT_NO_URL_CAST_FROM_STRING
    QT_NO_KEYWORDS
    QT_DISABLE_DEPRECATED_UP_TO=0x060900
    JOTTER_VERSION="${PROJECT_VERSION}")

# Generated sources (moc, rcc, translations) live in the build tree; this configuration keeps
# clang-tidy from reporting on them while the project's own .clang-tidy covers src/ and tests/.
file(WRITE "${CMAKE_BINARY_DIR}/.clang-tidy"
    "Checks: '-*,bugprone-assert-side-effect'\nInheritParentConfig: false\n")

add_subdirectory(src)

qt_add_translations(TARGETS jotter jotter_app jotter_core jotter_editor jotter_ui
    TS_FILE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/i18n"
    TS_FILE_BASE jotter
    LUPDATE_OPTIONS -no-obsolete -locations none)

if(JOTTER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
