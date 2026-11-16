# Development

## Requirements

Fedora 44 or newer with:

```sh
sudo dnf install cmake ninja-build gcc-c++ clang-tools-extra \
  qt6-qtbase-devel qt6-qtsvg-devel qt6-qttools-devel qt6-qtpositioning-devel \
  kf6-syntax-highlighting-devel kf6-karchive-devel sqlite-devel libstemmer-devel libssh-devel md4c-devel
```

Synchronisation talks SFTP through libssh (`libssh-devel`). The sync check of the test stand also needs Docker with the Compose plugin and Python 3.

Search registers its FTS5 tokenizer through the SQLite library that Qt's SQL driver uses, so the build links the system SQLite (`sqlite-devel`); words are stemmed by the Snowball stemmers of `libstemmer-devel`. Pasted Markdown is parsed with `md4c-devel`, the parser Qt embeds, called directly.

Geotags ask GeoClue2 for the position through Qt Positioning; the `geoclue2` service runs on most desktops, and on sway a GeoClue agent must run as well.

WebP images in notes are read by the Qt image formats plugin, a runtime package: `sudo dnf install qt6-qtimageformats`.

## Build and run

All commands run in `app/`.

```sh
cmake --preset dev               # configure a debug build in build/dev
cmake --build --preset dev       # build
ctest --preset dev               # unit tests and translation checks
build/dev/src/jotter             # run
```

`cmake --preset lint` and `cmake --build --preset lint` build once more in `build/lint` with clang-tidy; findings fail the build. `clang-format --dry-run -Werror src/*/*.cpp src/*/*.h tests/*.cpp` checks formatting; `clang-format -i` with the same files applies it.

The `release` preset builds an optimised binary without tests.

## Code style

Warnings are errors in the `dev` preset; formatting follows `app/.clang-format`, static checks `app/.clang-tidy` and naming the Qt coding conventions. Declarations in headers have Doxygen comments (`///` with `@param` and `@return`), as in the existing code.

## Shared fixtures

`tests/fixtures/` holds library files written by Jotter that every client of the format reads and writes back byte for byte; its README describes each file. `tst_fixtures` checks them on every `ctest` run. After changing a fixture by hand, `JOTTER_UPDATE_FIXTURES=1 build/dev/tests/tst_fixtures files` in `app/` rewrites the fixtures in the form the application writes; review the diff, which must change only the form, before committing.

## Sync test stand

`tests/` holds a disposable SFTP server for synchronisation. From `tests/`:

```sh
docker compose up -d     # build and start the server on 127.0.0.1:2222
docker compose down      # stop it; nothing it stored stays behind
```

The server has one account, `jotter` with the password `jotter`, and an empty folder `library` in its home; these fixture credentials exist only in the stand. A public key in `JOTTER_SFTP_AUTHORIZED_KEY` is accepted for the key sign-in method. The host keys are made when the image is built and survive `down` and `up`; `docker compose build --no-cache` makes new ones, which shows the application's changed host key notice.

The sync check runs the sync scenarios of `app/tests/tst_sync.cpp` against the stand:

```sh
tests/sync-check.py            # build tst_sync, start the stand, run, write the report
tests/sync-check.py --keep     # leave the stand running afterwards
tests/sync-check.py --filter hostKeyChanged largeAttachment
```

It makes a key pair for the run, so the key sign-in is checked too, stops the stand again when it started it, and writes `tests/reports/sync-report.html` (scenarios with their descriptions, results and measured times) and the JUnit file it is made from; `tests/reports/` is ignored by git. The same test runs in `ctest` against a local folder; these test-runner variables point it at an SFTP server instead:

| Variable | Meaning |
| --- | --- |
| `JOTTER_TEST_SFTP` | Address of the server and folder, such as `sftp://jotter:jotter@127.0.0.1:2222/library`; the folder is relative to the user's home. Each test works in a folder of its own inside it and removes it afterwards. |
| `JOTTER_TEST_SFTP_KEY` | A private key the server accepts, for the key sign-in check; the test imports its text as the application does. |

## Translations

Interface strings use `tr()` with English source text. After adding or changing strings:

```sh
cmake --build --preset dev --target update_translations
```

This refreshes `i18n/jotter_ru.ts` (every string) and `i18n/jotter_en.ts` (English plural forms only). Translate the new entries in Qt Linguist (`linguist i18n/jotter_ru.ts`), in the file itself, or with `python3 tools/translate_ts.py i18n/jotter_ru.ts <map.json>`, which fills unfinished entries from a JSON map of English source text to translation (a list of forms for plurals). `ctest` fails while any entry is unfinished, obsolete or inconsistent with its source (punctuation, place markers, accelerators).

## Version

The version lives in the repository's `VERSION` file; CMake reads it and stamps the binary.

## Packages

```sh
flatpak install --user flathub org.flatpak.Builder org.kde.Sdk//6.11 org.kde.Platform//6.11
app/packaging/flatpak/build-flatpak.sh
flatpak install --user app/packaging/flatpak/build/jotter-<version>.x86_64.flatpak
```

builds the Flatpak bundle from the working tree into `app/packaging/flatpak/build/` (ignored by git) and installs it. The manifest is generated from `app/packaging/flatpak/io.github.nir0k.Jotter.yml.in` with the version from `VERSION`; it builds libstemmer, libssh and md4c, pinned by version and checksum, then Jotter with its tests, and the script validates the desktop entry and the AppStream metadata. `flatpak-builder` from the host is used when installed, otherwise the `org.flatpak.Builder` Flatpak. A newer Snowball, libssh or md4c release changes the URL and `sha256` of its module; a newer KDE runtime changes `runtime-version` here and in the manifest.
