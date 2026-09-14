#!/usr/bin/env python3
"""Filter boot.conf.example and the JSON schema down to one build's features.

A user who installs the 'minimal' profile should not receive an 823-line
example config two thirds of which documents keys their binary will silently
ignore.  This takes the resolved feature set and writes out the subset that
actually applies.

  filter_config.py --features gui,clock,verify --config OUT --schema OUT

Ownership comes from the same place tag_schema.py reads - features.json's
"keys"/"entrykeys" arrays, via the x-feature tags already in the schema - so
there is one source of truth and no second mapping to drift.

Two granularities, because the example config is prose-heavy:

  * a section whose every key belongs to absent features is dropped whole,
    prose and all.  This is where the size comes off - Clock, Screensaver,
    Snapshots and friends are self-contained.
  * a mixed section keeps its prose and loses the individual key lines, plus
    the indented "#   key = value   what it does" lines that document them.

Nothing here can make a config invalid: every removed line was either a
comment or a commented-out default, and Visor ignores unknown keys anyway.
The removals are reported on stderr so the installer can summarise them.
"""

import argparse
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCHEMA_IN = os.path.join(ROOT, "docs", "boot.conf.schema.json")
CONFIG_IN = os.path.join(ROOT, "boot.conf.example")

SECTION_RE = re.compile(r"^#\s*-{2,}\s*(.+?)\s*-{2,}\s*$")
DIVIDER_RE = re.compile(r"^#\s*={4,}\s*$")
SETTING_RE = re.compile(r"^(#?)\s*([a-z][a-z0-9_]*)\s*=")
DOCKEY_RE = re.compile(r"^#\s{2,}([a-z][a-z0-9_]*)\s*=")
DOCCONT_RE = re.compile(r"^#\s{8,}\S")


def load_owners():
    """key -> list of features that must all be present, from the schema tags."""
    with open(SCHEMA_IN, encoding="utf-8") as fh:
        schema = json.load(fh)
    owners = {}
    tables = [schema["properties"],
              schema["properties"]["entry"]["properties"]]
    for table in tables:
        for name, spec in table.items():
            if not isinstance(spec, dict):
                continue
            feat = spec.get("x-feature")
            if feat is None:
                continue
            need = [feat] if isinstance(feat, str) else list(feat)
            owners[name] = need
            for alias in spec.get("x-aliases", []) or []:
                owners[alias] = need
    return owners, schema


def present(key, owners, on):
    """Is every feature this key needs compiled in?

    An unknown key is kept: the registry not knowing about it is not evidence
    that the user's build lacks it, and silently dropping a key we cannot
    attribute would be the one failure mode with no recovery path."""
    need = owners.get(key)
    if need is None:
        return True
    return all(f == "core" or f in on for f in need)


def split_sections(lines):
    """[(header_or_None, [lines])] - the preamble comes back with header None."""
    out, cur, head = [], [], None
    for line in lines:
        if SECTION_RE.match(line):
            out.append((head, cur))
            head, cur = line, [line]
            continue
        cur.append(line)
    out.append((head, cur))
    return out


def filter_section(body, owners, on):
    """Drop key lines for absent features, and the doc lines describing them."""
    kept, dropped, skipping = [], [], False
    for line in body:
        m = SETTING_RE.match(line)
        if m:
            skipping = False
            key = m.group(2)
            if present(key, owners, on):
                kept.append(line)
            else:
                dropped.append(key)
            continue
        d = DOCKEY_RE.match(line)
        if d:
            key = d.group(1)
            skipping = not present(key, owners, on)
            if not skipping:
                kept.append(line)
            continue
        if skipping and DOCCONT_RE.match(line):
            continue
        skipping = False
        kept.append(line)
    return kept, dropped


def section_keys(body):
    keys = []
    for line in body:
        m = SETTING_RE.match(line)
        if m:
            keys.append(m.group(2))
    return keys


def trim_blanks(lines):
    """Collapse the runs of blank/bare-# lines a removal leaves behind."""
    out = []
    for line in lines:
        bare = line.strip() in ("", "#")
        if bare and out and out[-1].strip() in ("", "#"):
            continue
        out.append(line)
    return out


def filter_config(on, owners, profile):
    with open(CONFIG_IN, encoding="utf-8") as fh:
        lines = fh.read().splitlines()

    sections = split_sections(lines)
    out, dropped_sections, dropped_keys = [], [], []

    for head, body in sections:
        if head is None:
            out.extend(body)
            continue
        keys = section_keys(body)
        if keys and not any(present(k, owners, on) for k in keys):
            name = SECTION_RE.match(head).group(1)
            dropped_sections.append(name)
            dropped_keys.extend(keys)
            continue
        kept, gone = filter_section(body, owners, on)
        dropped_keys.extend(gone)
        out.extend(trim_blanks(kept))

    if not dropped_keys and not dropped_sections:
        return lines, [], []

    banner = [
        "# ----------------------------------------------------------------------------",
        "#  Filtered for the '%s' profile of this Visor build." % profile,
        "#",
        "#  Keys for features this binary was not compiled with have been removed, so",
        "#  everything documented below is something your build can actually do.",
        "#  Run 'visor features' to see the feature set, and see the full reference at",
        "#  https://visor-bootmanager.vercel.app  if you later rebuild with more.",
        "# ----------------------------------------------------------------------------",
        "",
    ]
    return banner + trim_blanks(out), dropped_sections, dropped_keys


def filter_schema(on, schema, profile, features):
    dropped = []

    def prune(table):
        for name in list(table.keys()):
            spec = table[name]
            if not isinstance(spec, dict):
                continue
            feat = spec.get("x-feature")
            if feat is None:
                continue
            need = [feat] if isinstance(feat, str) else list(feat)
            if not all(f == "core" or f in on for f in need):
                del table[name]
                dropped.append(name)

    prune(schema["properties"]["entry"]["properties"])
    prune(schema["properties"])

    schema["x-visor-profile"] = profile
    schema["x-visor-features"] = sorted(features)
    schema["x-visor-filtered"] = bool(dropped)
    if dropped:
        schema["description"] = (schema.get("description", "").rstrip() +
                                 "  Filtered to the '%s' profile: keys for features "
                                 "absent from this build have been removed." % profile)
    return schema, dropped


def short_name(section):
    """A section header trimmed to something that fits on a summary line.

    Headers carry their own explanations - "Font sizes (in pixels; 0 / absent
    = sensible default)" - which are useful in the config and far too long
    once seventeen of them are joined into one sentence."""
    return section.split("(")[0].strip().rstrip(":").strip()


def summarise(dropped_keys, dropped_sections, limit=6):
    names = [short_name(s) for s in dropped_sections]
    names = [n for n in names if n]
    text = "config: %d keys and %d sections removed" % (len(dropped_keys),
                                                        len(dropped_sections))
    if names:
        shown = names[:limit]
        rest = len(names) - len(shown)
        text += " (" + ", ".join(shown)
        text += " and %d more)" % rest if rest else ")"
    return text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--features", required=True,
                    help="comma/space separated list of features that are ON")
    ap.add_argument("--profile", default="custom", help="profile name, for the banner")
    ap.add_argument("--config", help="write the filtered boot.conf.example here")
    ap.add_argument("--schema", help="write the filtered JSON schema here")
    ap.add_argument("--quiet", action="store_true", help="no summary on stderr")
    args = ap.parse_args()

    on = set(re.split(r"[,\s]+", args.features.strip())) - {""}
    if not on:
        print("filter_config: empty feature list", file=sys.stderr)
        return 2

    owners, schema = load_owners()
    notes = []

    if args.config:
        lines, ds, dk = filter_config(on, owners, args.profile)
        with open(args.config, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines).rstrip() + "\n")
        if dk or ds:
            notes.append(summarise(dk, ds))

    if args.schema:
        doc, dropped = filter_schema(on, schema, args.profile, on)
        with open(args.schema, "w", encoding="utf-8") as fh:
            json.dump(doc, fh, indent=2)
            fh.write("\n")
        if dropped:
            notes.append("schema: %d properties removed" % len(dropped))

    if notes and not args.quiet:
        print("; ".join(notes), file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
