"""Report config keys that the parser accepts but the docs do not mention.

The parser is the source of truth. Anything it accepts should appear in the
schema and the example config; anything the schema promises should be something
the parser actually accepts.

Exit status is 1 when a key is missing somewhere, so this can gate a release.
"""

import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

CONFIG_KEYS_C = os.path.join(ROOT, "src", "config_keys.c")
CONFIG_ENTRY_C = os.path.join(ROOT, "src", "config_entry.c")
SCHEMA = os.path.join(ROOT, "docs", "boot.conf.schema.json")
EXAMPLE = os.path.join(ROOT, "boot.conf.example")

def read(path):
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read()

def slice_between(src, path, start_pat, end_pat):
    """The body of one function, by its opening line and the one after it."""
    try:
        start = src.index(start_pat)
    except ValueError:
        sys.exit("%s: could not find %r - did the parser move again?"
                 % (os.path.basename(path), start_pat))
    try:
        end = src.index(end_pat, start)
    except ValueError:
        sys.exit("%s: could not find %r after %r"
                 % (os.path.basename(path), end_pat, start_pat))
    return src[start:end]

def parser_keys():
    """Keys compared in apply_global(), and in the entry-block parser."""
    keys_src = read(CONFIG_KEYS_C)
    entry_src = read(CONFIG_ENTRY_C)

    glob_body = slice_between(keys_src, CONFIG_KEYS_C,
                              "void apply_global", "\nvoid apply_theme")
    glob = set(re.findall(r'efi_strcmp\(key, L"([a-z0-9_]+)"\)', glob_body))

    entry_body = slice_between(entry_src, CONFIG_ENTRY_C,
                               "EFI_STATUS parse_entry", "\nvoid bls_decrement")
    entry = set(re.findall(r'efi_strcmp\(key, L"([a-z0-9_]+)"\)', entry_body))

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

    if problems:
        print("\n%d key(s) out of sync" % problems)
        return 1

    print("all keys documented in the schema and the example config")
    return 0

if __name__ == "__main__":
    sys.exit(main())
