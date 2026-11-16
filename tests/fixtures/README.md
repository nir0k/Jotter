# Shared library fixtures

Library files that every client of the format reads and writes back byte for byte (spec section 3), and the three-way merges of synchronisation that every client computes alike (spec section 12.4). They are exactly what Jotter writes: the contents were authored by hand and then rewritten by the application's own writers, so the bytes are the reference for anything the specification leaves open, such as the order of JSON keys (alphabetical), how Qt writes empty arrays and objects and numbers, and where the editor puts an empty paragraph. Other clients of the format copy this folder and record the commit it comes from; a note form missing here is added here first.

`app/tests/tst_fixtures.cpp` checks them on every `ctest` run.

## Layout

| Path | Content |
| --- | --- |
| `library/` | A readable library; every file reads and writes back unchanged, and every note body survives the editor unchanged |
| `library/library.json` | Favourites of both kinds, tag colours (a tag colour key, a custom `#rrggbb`, an unknown key, a Cyrillic tag), a tombstone, unknown fields at the top level and in `settings` |
| `library/collections.json` | A three-level tree, icons, colour keys, a custom and an unknown colour, an unknown field |
| `library/rules.json` | Both condition kinds, `add` with two targets and `move`, `any`, `stop`, a disabled rule, an unknown field; a rule of an earlier version with conditions on the title and a regular expression and `caseSensitive`, unsupported and written back unchanged |
| `library/notes/` | Notes grouped by subject, listed below |
| `library/attachments/` | The files the notes show: PNG, JPEG, GIF and WebP images, and attachments that are not images by their bytes (PDF, SVG, a text file named `.png`) |
| `problems/` | A library whose note files cannot be read; `expected.json` lists the problem code each one gives (`malformed` or `newerFormat`). `01a1a316-0000-…` has valid metadata and unsupported body HTML that must remain untouched until explicit normalisation. `readme.html` is not named after an identifier and is not a note, so it is not listed |
| `newer-library/` | `library.json` of a newer format version; the library opens read-only |
| `merge/` | Merge cases, listed below |

Notes of `library/notes/`:

| Note | Content |
| --- | --- |
| `01a1872b-97ba-…` Текст и форматирование | Headings, inline formatting, font, tag and custom colours, a line break, escaping, Cyrillic and emoji, a quote, an empty paragraph, a divider; icon, colour, collection, nested tags and a geotag |
| `01a1872b-97bc-…` Lists | Bulleted, numbered with `start`, checklists, every numbering style, a style of an earlier version and an unknown style identifier, nine levels of mixed kinds, adjacent lists; archived |
| `01a1872b-97be-…` Code blocks | A language and a title with characters to escape, tabs, plain, unknown language, empty, between lists; the empty paragraphs the editor puts between adjacent blocks |
| `01a1872b-97c0-…` Tables | Width, column widths, header row, a list, an image and two paragraphs in cells, a full-width table; the empty paragraph between adjacent tables |
| `01a1872b-97c3-…` Изображения и вложения | Image widths and both wrap modes, every image kind, percent-encoded and Cyrillic file names, attachment chips |
| `01a1872b-97c5-…` Links | Web and mail links, note links; trashed with its archived time kept |
| `01a1872b-97c7-…` (untitled) | Empty title and body, a `quick` field from an earlier version, kept as an unknown field, a geotag without accuracy |
| `1889d8c9-…` План… | UUID version 4, a title that could end the metadata script and needs HTML escaping, an unknown icon, a custom colour, unknown fields of every JSON type |

## Merge cases

Each folder of `merge/` is one case. `local/` and `remote/` hold the two versions of library files, named as in a library (`library.json`, `collections.json`, `rules.json`, `notes/<id>.html`); `base/` holds the version both sides started from, and is missing for a first connection (spec section 12.6). `merged/` holds what the merge writes to both sides. `case.json` gives the time of the merge (`now`, which expires tombstones) and the files whose note bodies conflict (`conflicts`), whose local version becomes a conflict copy.

| Case | Content |
| --- | --- |
| `note-blocks-combine` | Changes to different blocks combine, blocks added on both sides at different places included |
| `note-adjacent-blocks` | Changes to blocks next to each other combine |
| `note-same-block` | The same block changed on both sides conflicts; the remote body wins |
| `note-metadata` | Metadata by field: one-sided changes combine, the later note wins a field changed on both sides, tags merge as case-folded sets |
| `note-first-connection-same-body` | Without a base, equal bodies keep the later note |
| `note-first-connection-different-body` | Without a base, different bodies conflict and keep the remote note |
| `collections-by-entry` | Additions on both sides, a change against a deletion, the later of two changes, a child of a deleted parent moved to the top level |
| `collections-local-reorder` | A reorder made only locally is kept |
| `collections-parent-cycle` | Each side moving a collection under the other leaves both at the top level; a child below keeps its parent |
| `rules-by-entry` | The later of two changes, a change against a deletion, an addition following its predecessor |
| `library-first-connection` | Without a base: tombstones of both sides until they expire, favourites of deleted notes dropped, tag colours by tag, remote settings |
| `library-with-base` | Favourites by entry with a local reorder kept, tag colours by tag, a setting changed on one side |

## Updating

Change a fixture by hand, then rewrite the fixtures in the form the application writes and check them again. In `app/`:

```sh
cmake --build --preset dev --target tst_fixtures
JOTTER_UPDATE_FIXTURES=1 build/dev/tests/tst_fixtures files merges
ctest --preset dev -R fixtures
```

Review the diff before committing: the rewrite must change only the form, never the content, and a merged version is written by the merge itself, so it is checked against the case's intent. The damaged notes of `problems/` and the newer library are not rewritten.
