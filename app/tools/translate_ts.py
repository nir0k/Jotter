#!/usr/bin/env python3
"""Fill translations of a Qt Linguist file from a JSON map of source text to translation.

Usage: python3 tools/translate_ts.py i18n/jotter_ru.ts translations.json

Only unfinished entries whose source text is in the map are changed; entries with plural forms
take a list of forms. The rest of the file is left as lupdate wrote it.
"""

import json
import sys
import xml.etree.ElementTree as ET


def main() -> None:
    """Apply the map to the file and report entries that remain unfinished."""
    ts_path, map_path = sys.argv[1], sys.argv[2]
    translations = json.loads(open(map_path, encoding="utf-8").read())
    tree = ET.parse(ts_path)
    remaining = []
    for message in tree.getroot().iter("message"):
        source = message.findtext("source")
        translation = message.find("translation")
        if translation is None or translation.get("type") != "unfinished":
            continue
        if source not in translations:
            remaining.append(source)
            continue
        value = translations[source]
        if message.get("numerus") == "yes":
            for form in list(translation):
                translation.remove(form)
            for text in value:
                ET.SubElement(translation, "numerusform").text = text
        else:
            translation.text = value
        del translation.attrib["type"]
    tree.write(ts_path, encoding="utf-8", xml_declaration=True)
    with open(ts_path, "r+", encoding="utf-8") as handle:
        text = handle.read().replace("<?xml version='1.0' encoding='utf-8'?>",
                                     '<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE TS>', 1)
        handle.seek(0)
        handle.write(text if text.endswith("\n") else text + "\n")
        handle.truncate()
    for source in remaining:
        print(f"unfinished: {source}")


if __name__ == "__main__":
    main()
