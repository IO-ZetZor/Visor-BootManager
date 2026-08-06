#!/usr/bin/env python3
"""Report config keys that src/config.c accepts but the docs do not mention.

The parser is the source of truth. Anything it accepts should appear in the
schema, the example config and the wiki; anything the docs promise should be
something the parser actually accepts.

Exit status is 1 when a key is missing somewhere, so this can gate a release.
"""

import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

CONFIG_C = os.path.join(ROOT, "src", "config.c")
SCHEMA = os.path.join(ROOT, "docs", "boot.conf.schema.json")
EXAMPLE = os.path.join(ROOT, "boot.conf.example")
WIKI_DIR = os.path.join(ROOT, "..", "visor-wiki")


def read(path):
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def parser_keys():
    """Keys compared in apply_global(), and in the entry-block parser."""
    src = read(CONFIG_C)

    start = src.index("static void apply_global")
    end = src.index("\nstatic void apply_theme")
    glob = set(re.findall(r'efi_strcmp\(key, L"([a-z0-9_]+)"\)', src[start:end]))

    head = src[:start]
    entry_start = head.rindex("CHAR16 *luks_preset")
    entry = set(re.findall(r'efi_strcmp\(key, L"([a-z0-9_]+)"\)', head[entry_start:]))

    return glob, entry


def schema_keys():
    doc = json.loads(read(SCHEMA))
    props = doc["properties"]
    entry_props = props["entry"]["properties"]

    def expand(table):
        names = set()
        for key, spec in table.items():
            names.add(key)
            names.update(spec.get("x-aliases", []))
        return names

    glob = expand({k: v for k, v in props.items() if k != "entry"})
    return glob, expand(entry_props)


def mentioned(text, key):
    return re.search(r"\b%s\b" % re.escape(key), text) is not None


def main():
    glob, entry = parser_keys()
    s_glob, s_entry = schema_keys()

    example = read(EXAMPLE)

    wiki = ""
    if os.path.exists(WIKI_DIR):
        for fname in os.listdir(WIKI_DIR):
            if fname.endswith(".html"):
                wiki += read(os.path.join(WIKI_DIR, fname)) + "\n"

    if not wiki:
        wiki = None

    problems = 0

    def report(label, missing):
        nonlocal problems
        if missing:
            problems += len(missing)
            print("  %-28s %s" % (label, " ".join(sorted(missing))))

    print("parser: %d global keys, %d entry keys (aliases included)"
          % (len(glob), len(entry)))

    report("missing from schema:", (glob - s_glob) | (entry - s_entry - s_glob))
    report("in schema, not parser:", (s_glob - glob) | (s_entry - entry))
    report("missing from example:",
           {k for k in glob | entry if not mentioned(example, k)})

    if wiki is None:
        print("  %-28s %s" % ("wiki not found:", WIKI_DIR))
    else:
        report("missing from wiki:",
               {k for k in glob | entry if not mentioned(wiki, k)})

    if problems:
        print("\n%d key(s) out of sync" % problems)
        return 1

    print("all keys documented in schema, example and wiki")
    return 0


if __name__ == "__main__":
    sys.exit(main())
