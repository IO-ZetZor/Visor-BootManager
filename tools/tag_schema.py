#!/usr/bin/env python3
"""Tag every config key in docs/boot.conf.schema.json with its owning feature.

x-feature on a schema property lets Studio (and the visor CLI) grey out or
sanity-check keys belonging to features that are not compiled into a given
build, instead of silently ignoring them.

  "x-feature": "gui"              - this one feature must be on
  "x-feature": ["clock", "blur"]  - ALL of these must be on (and-ed)
  "x-feature": "core"             - always present, no dependency

The source of truth for ownership is features/features.json (its "keys" and
"entrykeys" arrays).  Alias properties in the schema (x-alias-of) inherit the
canonical key's tag.  A few keys genuinely need more than one feature - those
live in MULTI here, mirroring the registry's comments.

Running with --check verifies the on-disk schema matches this derivation and
exits 1 otherwise; without it, the schema is rewritten in place.
"""

import argparse
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FEATURES = os.path.join(ROOT, "features", "features.json")
SCHEMA = os.path.join(ROOT, "docs", "boot.conf.schema.json")

MULTI = {
    "clock_blur": ["clock", "blur"],
}


def load(path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def owner_map(doc):
    """canonical key -> sorted list of owning features."""
    own = {}
    for feat, spec in doc["features"].items():
        for key in spec.get("keys", []) or []:
            own.setdefault(key, []).append(feat)
        for key in spec.get("entrykeys", []) or []:
            own.setdefault(key, []).append(feat)
    for key in own:
        own[key] = sorted(set(own[key]))
    return own


def feature_of(name, own, alias_to):
    if name in MULTI:
        return MULTI[name]
    canonical = alias_to.get(name, name)
    if canonical in own:
        return own[canonical]
    return ["core"]


def alias_map(schema):
    alias = {}
    tables = [schema["properties"], schema["properties"]["entry"]["properties"]]
    for table in tables:
        for key, spec in table.items():
            for other in spec.get("x-aliases", []):
                alias[other] = key
            if spec.get("x-alias-of"):
                alias[key] = spec["x-alias-of"]
    return alias


def tag_dict(props, own, alias):
    for name, spec in props.items():
        feat = feature_of(name, own, alias)
        value = feat[0] if len(feat) == 1 else feat
        ordered = {"x-feature": value}
        ordered.update(spec)
        props[name] = ordered


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="verify the schema has correct x-feature tags and exit")
    args = ap.parse_args()

    doc = load(FEATURES)
    schema = load(SCHEMA)
    own = owner_map(doc)
    alias = alias_map(schema)

    import copy
    expected = copy.deepcopy(schema)
    tag_dict(expected["properties"], own, alias)
    tag_dict(expected["properties"]["entry"]["properties"], own, alias)

    if args.check:
        tagged = {k: v.get("x-feature") for k, v in schema["properties"].items() if k != "entry"}
        tagged.update({k: v.get("x-feature")
                       for k, v in schema["properties"]["entry"]["properties"].items()})
        want = {k: v.get("x-feature")
                for k, v in expected["properties"].items() if k != "entry"}
        want.update({k: v.get("x-feature")
                     for k, v in expected["properties"]["entry"]["properties"].items()})
        if tagged != want:
            diff = set(tagged.items()) ^ set(want.items())
            print("x-feature tags out of sync with features/features.json:")
            for key, val in sorted(diff):
                print("  %-24s schema=%r expected=%r" % (key, tagged.get(key), want.get(key)))
            missing = [k for k in tagged if tagged[k] is None]
            if missing:
                print("  %d untagged: %s" % (len(missing), ", ".join(missing)))
            return 1
        core = [k for k, v in sorted(want.items()) if v == "core"]
        print("schema x-feature tags consistent "
              "(%d global + %d entry keys; %d core)"
              % (len([k for k in want if k in schema["properties"] and k != "entry"]),
                 len([k for k in want if k in schema["properties"]["entry"]["properties"]]),
                 len(core)))
        return 0

    text = json.dumps(expected, indent=2, ensure_ascii=False) + "\n"
    with open(SCHEMA, "w", encoding="utf-8") as fh:
        fh.write(text)
    print("tagged %d schema keys with x-feature" % len(want_props(expected)))
    return 0


def want_props(expected):
    return expected["properties"] | expected["properties"]["entry"]["properties"]


if __name__ == "__main__":
    sys.exit(main())