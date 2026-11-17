# Jotter — library format and synchronisation

Jotter keeps notes as ordinary HTML files in a library folder that stays readable without the application, and synchronises libraries between devices through the person's own SFTP storage. This specification defines the library format (section 3) and the synchronisation model (section 12) that every client of a Jotter library follows. The other section numbers belong to the desktop application's own description and are not part of this document.

## 1. Principles

- The library is a folder of HTML and JSON files that any browser and text editor can read; nothing in it requires Jotter.
- Notes are stored as HTML, not Markdown, because Markdown cannot hold fonts, colours, highlights and list numbering styles (section 3.3).
- A library belongs to one person. There is no sharing between people and no real-time co-editing; devices merge concurrent changes while they synchronise.
- Encryption of the remote copy is not part of the model.

## 2. Terminology

| Term | Meaning |
| --- | --- |
| Library | A folder holding a person's notes, collections, rules and attachments in the format of section 3. |
| Client | An application that reads and writes a library, such as the Jotter desktop application. |
| Note | A titled rich text document with metadata, stored as one HTML file. |
| Body | The rich text content of a note, without its title and metadata. |
| Collection | A named, nestable group of notes with an icon and a colour. A note belongs to zero, one or several collections. |
| Tag | A label of a note, stored in its metadata. Tags nest by `/` (`work/meetings`). |
| Rule | A condition on a note's tags with an action that files the note into collections. |
| Favourite | A collection or note marked for quick access, listed in `library.json`. |
| Archive | A state of a note that removes it from ordinary views and search while keeping it intact. |
| Trash | A state of a note awaiting permanent deletion after the library's retention period. |
| Geotag | Coordinates attached to a note when it was created. |
| Attachment | A file or image inserted into a note and stored in the note's attachment folder. |
| Note link | A link in a body that opens another note of the library. |
| Numbering style | How a numbered list counts on each nesting level: `1.`, `A.`, `I.` or multilevel (`1.1.`). |
| Sync storage | A remote place a library is synchronised with: an SFTP folder. |

## 3. Library and storage format

The library is the only source of truth; everything else a client keeps is device data (section 3.8).

### 3.2 Layout

```
Jotter/
  library.json          Library id, format version, favourites, tag colours, library settings
  collections.json      The collection tree
  rules.json            Rules in their order
  index.html            Generated table of contents for reading without Jotter
  notes/
    <note-id>.html      One file per note
  attachments/
    <note-id>/          Images and files of that note
```

- Identifiers are lowercase UUID strings: version 7 for everything a client creates, so they sort by creation time; identifiers of other UUID versions found in a library are accepted as they are. Timestamps are RFC 3339 in UTC with milliseconds.
- All text files are UTF-8 with LF line endings. JSON files are written with two-space indentation and keys in alphabetical order so that changes are readable in a diff; the examples in this section show the keys in reading order. Unknown fields are kept and written back unchanged in `library.json` and its `settings`, in the entries of `collections.json` and `rules.json` and in note metadata; elsewhere (the top level of `collections.json` and `rules.json`, favourites, tombstones, locations, rule actions and the conditions of supported rules) they are dropped.
- The shared fixtures in `tests/fixtures/` are libraries written by the desktop application that cover every form of this section. They are the reference for the exact bytes of each file, including the generated style sheets and colour classes, and every client of the format reads and writes them back unchanged.
- `library.json`:

```json
{
  "format": 1,
  "id": "0192f3a4-5b6c-7d8e-9f01-23456789abcd",
  "created": "2026-10-23T10:15:00.000Z",
  "favorites": [
    { "kind": "collection", "id": "…" },
    { "kind": "note", "id": "…" }
  ],
  "tagColors": { "work": "sky", "work/urgent": "red" },
  "settings": { "trashRetentionDays": 30 },
  "tombstones": [
    { "id": "…", "deleted": "2026-10-23T12:40:00.000Z" }
  ]
}
```

- `tagColors` maps a tag, case-folded, to its colour: a tag colour key (section 3.9) or a custom colour as `#rrggbb`; an unknown key is kept and shown without colour.
- `settings.trashRetentionDays` is the number of days a trashed note is kept before it is deleted permanently: a positive integer, or `null` to keep trashed notes until the trash is emptied; absent means 30. The desktop application offers 7, 30, 90 days and never.
- `tombstones` lists the notes deleted permanently within the last 90 days, with the time of deletion, so a device that still has such a note does not bring it back (section 12.4). Every permanent deletion records one, whether or not the library is synchronised.
- A client opens a library whose `format` is newer than it understands read-only. Format 1 is the only format so far; a later format will upgrade an older library in place only after the person confirms the change.

### 3.3 Note files

A note is a complete HTML document that a browser shows readably. The metadata is a JSON block that browsers do not execute; the visible header repeats the title and tags for a human reader and is regenerated on every save.

```html
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<title>Weekly planning</title>
<script type="application/json" id="jotter-meta">
{
  "format": 1,
  "id": "0192f3a4-…",
  "title": "Weekly planning",
  "icon": "Calendar",
  "color": "blue",
  "created": "2026-10-23T10:15:00.000Z",
  "updated": "2026-10-23T11:02:31.250Z",
  "collections": ["0192f3a0-…"],
  "tags": ["work/planning"],
  "archived": null,
  "trashed": null,
  "location": { "lat": 55.75583, "lon": 37.6173, "accuracy": 35, "captured": "2026-10-23T10:15:02.000Z" }
}
</script>
<style>/* Generated: readable typography, list numbering, checklist and colour classes */</style>
</head>
<body>
<header class="jotter-header"><h1>Weekly planning</h1><p>#work/planning</p></header>
<article class="jotter-body">
  …
</article>
</body>
</html>
```

Metadata fields:

| Field | Meaning |
| --- | --- |
| `format` | Note format version. |
| `id` | Note identifier; equals the file name. |
| `title` | Plain text title, may be empty. |
| `icon` | Glyph name (section 3.9) or `null`. |
| `color` | Title colour as a tag colour key, a custom colour as `#rrggbb` or `null`. |
| `created`, `updated` | Creation time and time of the last change of the title, body or metadata. |
| `collections` | Identifiers of the collections the note belongs to. |
| `tags` | The note's tags, in their order. |
| `archived`, `trashed` | Time the note was archived or moved to the trash, or `null`. A trashed note keeps its `archived` value for restoring. |
| `location` | Geotag: latitude and longitude with five decimals, accuracy in metres, capture time; or `null`. |

Earlier versions also wrote a `quick` field. It no longer means anything: new notes are written without it, and one found in a file is kept as an unknown field.

The body is a restricted HTML vocabulary. Clients write only these forms and read them back without loss.

| Content | HTML |
| --- | --- |
| Paragraph, headings 1–3, quote | `<p>`, `<h1>`–`<h3>`, `<blockquote><p>` |
| Bold, italic, underline, strikethrough | `<strong>`, `<em>`, `<u>`, `<s>` |
| Superscript, subscript | `<sup>`, `<sub>` |
| Font family | `<span style="font-family: 'Name'">` |
| Text colour, highlight | `<span data-color="red">`, `<mark data-color="yellow">`, with a generated class giving browsers the light palette's colour; a custom colour carries it in its own `style` (`<span data-color="#ff8800" style="color: #ff8800">`), which clients ignore when reading |
| Link | `<a href="https://…">` (schemes `http`, `https`, `mailto`) |
| Note link | `<a class="note-link" href="<note-id>.html">text</a>`; the relative address also works when the file is opened in a browser |
| Inline code | `<code>` |
| Bulleted list | `<ul>` with `<li>`; nested lists inside `<li>` |
| Numbered list | `<ol data-style="<style-id>" start="n">`; the generated style sheet draws the style's markers. A list without `data-style` uses `1.`; a style the library does not know is kept in the file and drawn as `1.` |
| Checklist | `<ul class="checklist">` with `<li data-checked="true">` |
| Code block | `<pre data-language="go" data-title="main.go"><code>…</code></pre>`; plain text, highlighting is applied when shown. The language is the lower-case name of a KDE SyntaxHighlighting definition and is absent for plain text; an unknown language is kept and shown as plain text. The title is absent when the block has none |
| Table | `<table data-width="60">` (width from 10 to 99 percent of the text column, absent for full width) with an optional `<colgroup>` of column widths (`<col style="width: 30%">`, whole percents adding up to 100, written only when the columns are not equal), an optional `<thead>` header row and `<tbody>`, one row per line; cells hold paragraphs, inline formatting, lists, images and attachments, written as blocks like the body (`<td><p>…</p></td>`) |
| Image | `<img src="../attachments/<note-id>/<file>" data-width="50" data-wrap="square">` inside a paragraph (width from 10 to 99 percent of the text column or of the table column, absent for full width). `data-wrap` is `square` or `top-bottom` for an image that text wraps around, absent for an image in line with text; the base style sheet floats a square-wrapped image left and gives a top-and-bottom one a line of its own |
| Attachment | `<a class="attachment" href="../attachments/<note-id>/<file>" data-size="12345">file name</a>` inside a paragraph; the text is the name the chip shows |
| Divider | `<hr>` |

- The generated style sheet holds a rule for each table and image width the note uses.
- Lists nest up to nine levels, and each nested list has its own type: a checklist inside a numbered list inside a bulleted list is allowed.
- In the addresses of images and attachments, the characters of a file name that would end or change a URL (`%`, `#`, `?`, space, `"`, `<`, `>`, `\`, `^`, `` ` ``, `{`, `|`, `}`, control characters and DEL) are percent-encoded; other characters, Cyrillic letters included, stay as they are.
- Numbering styles have fixed identifiers: `decimal` (`1.`, written as a list without `data-style`), `upper-alpha`, `upper-roman` and `multilevel`. Any other value, such as the styles of earlier versions (`decimal-paren`, `lower-alpha`, `lower-roman`, `outline`) or the identifier of a custom style, is kept in the file and drawn as `1.` until the list's style is changed.
- A file whose metadata is missing or malformed, or whose body contains forms outside the vocabulary, is never changed by a client: it stays byte-for-byte unchanged.
- Files in `notes/` that are not named `<uuid>.html` are ignored.
- Known JSON fields must have their declared types. A present scalar in place of an array or object is malformed, never an empty list or default settings. Collection and rule documents require an integer `format` of at least 1 and their named arrays.

### 3.4 Collections and rules files

- `collections.json` holds a flat list ordered by position within each parent: `id`, `parent` (identifier or `null`), `name`, `icon` (glyph name, default `Folder`), `color` (tag colour key, custom colour or `null`), `created`, `updated`. Membership lives in the notes, so a note file describes itself completely.
- `rules.json` holds the ordered rule list as a list `rules`, each with `id`, `name`, `enabled`, `match` (`all` or `any`), `conditions`, `action`, `stop`, `created` and `updated`. A condition has `field` and `op`: `tag` with `equals` and a `value`, or `tags` with `none`. The action has `kind` (`add` or `move`) and `collections`, the target collection identifiers (one for `move`). A rule with an unknown condition or action makes the file unreadable, like other malformed library files.
- Earlier versions also wrote conditions on the title (`title` with `matches`, `contains` or `equals`) and with a regular expression (`tag` with `matches`), and `caseSensitive` for them. A rule with such a condition is read as unsupported and written back with its conditions unchanged; `caseSensitive` is kept as an unknown field and no longer written to other rules.
- Earlier versions kept custom numbering styles in `styles.json`; clients no longer read or write it and leave an existing file in place.

### 3.5 Attachments

- Images and files of a note are stored in `attachments/<note-id>/` under their original file names; a clashing name gets a suffix (`photo (2).jpg`).
- Accepted image kinds, detected from their bytes: PNG, JPEG, GIF and WebP. Other files, including SVG, are inserted as attachments.
- Size limit: 50 MB per file, image or attachment, applied when a file is inserted; files already in the library, or put there by other means, are shown as they are.
- An attachment file that its note no longer references (its reference was removed from the body) is removed no earlier than 30 days later, so undo and an accidental deletion can still bring it back within that time. Restoring the reference within the period cancels the removal. Folders of notes that cannot be read are left alone, as their references are unknown.
- Content copied from another note brings its files along: they are copied into the folder of the note it is pasted into, so no note depends on another note's folder.
- Moving a note to the trash keeps its attachments; deleting it permanently deletes its attachment folder.

### 3.6 Generated table of contents

`index.html` lists all notes that are not in the trash: grouped by collection (a note appears under each of its collections, nested collections as nested headings), then notes without a collection, then the archive. Each entry links to the note file and shows its updated date and tags; notes are ordered by title, and a collection is listed only when it or a descendant holds notes. Headings and dates follow the interface language. Clients regenerate it after changes and write it only when its contents changed (never in a read-only library); it is never read back.

### 3.7 Saving and outside changes

- Writes are atomic: a crash never leaves a half-written file. Below the library root, file operations do not follow symbolic links and refuse a `..` component.
- When a note with unsaved local changes was changed outside the client (by another program or by synchronisation), the outside version is kept and the local text is saved as a new note titled "<title> (conflict, <date>)" in the same collections; the words and the short date follow the interface language.

### 3.8 Device data

Indexes, window state, sync state, connection settings, credentials and accepted host keys are device data: they are kept outside the library, never reach a sync storage or another device, and losing them loses no note. Losing sync state makes the next run a first connection to the same library (section 12.6), which loses no change.

### 3.9 Library semantics

Rules that every client applies to the data, so that two devices treat one library alike.

- **Tags.** A tag is stored without a leading `#`, without empty path parts (`a//b`, `/a`, `a/`) and without spaces around each part: `#work / meetings` is stored as `work/meetings`. Tags compare case-insensitively (case-folded); a note never holds the same tag twice, and the first spelling of a tag is kept. `work/meetings` is a child of `work`: a parent tag's view includes the notes of its children. `#word` in a body is ordinary text, not a tag.
- **Tag colours.** The tag colour keys are `red`, `orange`, `amber`, `yellow`, `lime`, `green`, `emerald`, `teal`, `cyan`, `sky`, `blue`, `indigo`, `violet`, `purple`, `fuchsia`, `pink`, `rose`, `brown`, `gray`, `maroon`, `coral`, `gold`, `beige`, `olive`, `mint`, `navy`, `lavender`, `slate` and `graphite`; a custom colour is `#rrggbb`. The same keys colour note and collection titles, text and highlights. A tag without a colour of its own takes the colour of its nearest coloured parent (`work/meetings` takes the colour of `work`); a tag without any colour is neutral. Renaming a tag moves its colour, and the colours of its children, with it.
- **Icons.** Note and collection icons are glyph names listed in the desktop application's icon manifest, `app/resources/icons/manifest.json`; collections default to `Folder`. A stored name is kept unchanged, and no client rewrites it: a name a client has no glyph of, such as one a later release removed, shows no glyph, and a collection then shows `Folder`.
- **Rules.** Rules are evaluated in their order when a note is created and when its tags change, by any means. A rule acts only when the note starts matching it (it did not match before the change), so a person's own change of collections is not undone by the next edit. `match` joins the conditions with all or any; `tag` `equals` matches the tag and its children, case-insensitively; `tags` `none` matches a note without tags. `add` adds the note to the target collections, `move` puts it into its one target and removes it from all other collections; targets that no longer exist are skipped. A rule with `stop` whose conditions hold stops the rules after it for that note, whether or not it changed the note. Disabled and unsupported rules are skipped, and so are archived and trashed notes.
- **Trash.** A trashed note is read-only and is deleted permanently once it has been in the trash longer than `trashRetentionDays`.

## 12. Synchronisation

A library may be connected to one sync storage, a folder on an SFTP server. The remote folder holds the same layout as the library (section 3.2), so it is readable there too. The server only stores files: every device merges concurrent changes itself while it syncs.

### 12.1 Connection

- A connection names an SFTP server, its port, a user, the remote folder and a sign-in method: a private key or a password. The connection and its credentials belong to the device and are never written to the library or the sync storage.
- Host keys are verified. An unknown host key is trusted only when the person accepts its fingerprint; a changed host key stops synchronisation and is never accepted by a sync run.
- Saving a connection first checks it: connect, verify the host key, sign in, then write and remove a probe file in the remote folder through a temporary file and a rename, as uploads do. The remote folder must exist. A failed check saves nothing.
- Disconnect removes the library's connection, credentials and sync state; the files stay on both sides.

### 12.2 Files and sync state

- Synchronised are the files of the library layout: `library.json`, `collections.json`, `rules.json`, `notes/<note-id>.html` and `attachments/<note-id>/`. Other files on either side are left alone. `index.html` is uploaded after it changes, so the remote folder stays readable, and is never downloaded: every device generates its own (section 3.6).
- Each device keeps sync state outside the library: its device identifier and, for every synced file, its modification time and size on both sides, its content hash and, for note and JSON files, a copy of the last synced content, the base for three-way merges.
- A file counts as changed when its modification time or size differs from sync state; it is then read (a remote file is downloaded) and compared by hash, so a file whose time changed without its content is not a change. File system times are coarse, so a file could change again within the same tick and keep its size: a local file modified less than two seconds before it is recorded is recorded without its time and read again by the next run, and a run waits for the precision of the storage's times (a second for SFTP) after its last remote change before it releases the lock, so later changes by other devices carry later times. An attachment folder is listed again only when the folder's own modification time changed, which every upload changes (below); a full run lists every folder.
- Uploads are written to a temporary file `.jotter-tmp-<random>` in the target folder and renamed into place (the SFTP `posix-rename@openssh.com` extension), so another device never reads half a file. Temporary files older than a day are removed. A name in a remote listing that is not a single entry (a `/`, `.` or `..`), which only a broken or hostile server sends, is skipped.

### 12.3 A sync run

1. Take the remote lock: `.jotter-lock` in the root of the remote folder, created exclusively, holding the device identifier and an expiry ten minutes ahead, renewed while a long run lasts. While another device holds an unexpired lock the run waits and tries again every 30 seconds. An expired lock, or one the device itself left behind, is taken over: it is first renamed to a temporary name and checked to be the lock that was read, so that of two devices taking it over at once only one succeeds. Without the lock two devices syncing at once could each upload a merge that misses the other's change. Every client of the format follows this lock.
2. Read the remote `library.json`. A format newer than the client understands stops synchronisation; a read-only library is not synchronised. A remote folder without `library.json` is a first connection (section 12.6) whatever the sync state remembers, so a folder emptied on the server receives the library again instead of deleting it on the device.
3. List remote and local changes since the last run. A file changed on one side is copied to the other; a file changed on both sides is merged (section 12.4), written locally and uploaded.
4. Update sync state and release the lock.

- Losing the lock during a run stops it.

### 12.4 Merging and deletions

- Note metadata merges field by field against the base: a field changed on one side takes that side; both sides changing one field keeps the later `updated`; `collections` and `tags` merge as sets.
- Note bodies merge three ways by top-level blocks. Changes to different blocks combine, also when they are next to each other. When both sides changed the same block, or inserted different blocks at the same place, the remote body wins and the local version is saved as a conflict copy, as in section 3.7. Each conflict copy owns duplicates of its referenced attachments and rewrites their addresses to its new identifier; deleting the original cannot break the copy.
- `collections.json` and `rules.json` merge by entry identifier: the same entry changed on both sides keeps the later `updated`, and an entry changed on one side and deleted on the other is kept. The order is the remote one unless only the local side reordered; entries missing from it follow their predecessor on their own side. A collection whose parent is gone, or that ended up in a cycle of parents, moves to the top level.
- `library.json` merges favourites by entry, tag colours by tag and settings by key; these values carry no time of change, so a value changed on both sides keeps the remote one. Tombstones of both sides are kept until they expire, and favourites of notes with a tombstone are dropped.
- Attachments are not merged. An attachment changed on both sides keeps the remote file; the local one is kept beside it as `<name> (conflict).<ext>`, or `<name> (conflict 2).<ext>` and so on when that name is taken. Nothing references the copy, so it is removed after the period of section 3.5 unless it is inserted again.
- A device deletes a note that has a tombstone (section 3.2).
- A file in sync state that is missing on one side without a tombstone was removed outside the client. If the other side did not change it since the last run, it is deleted there too; if it did, it is copied back, so no change is lost.
- The shared fixtures in `tests/fixtures/merge/` hold merge cases of note, collection, rule and library files: the versions of both sides, the base and the merged result. Every client of the format merges them to the same bytes.

### 12.6 First connection and another device

- Connecting a library to a remote folder:
  - an empty folder receives the library;
  - a folder holding the library with the same identifier is synchronised with an empty base: identical files match, notes with the same body keep the later version, notes with different bodies keep the remote version with the local one as a conflict copy, and JSON files merge by entry as in section 12.4;
  - a folder holding another library, or other files without a `library.json`, is refused with the reason: two libraries are never mixed.
- A library can also be downloaded from a sync storage into an empty or new local folder. Only a fully successful download is published there; the library then opens already connected, and the first run transfers nothing again.
