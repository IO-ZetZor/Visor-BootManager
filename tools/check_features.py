"""Check that the two halves of the feature registry agree, and that they
agree with the schema and the source tree.

features.mk is the build half; features.json is the human/UI half. Nothing
generates one from the other on purpose - a generated-and-committed file is a
file contributors forget to regenerate. So they are cross-checked here instead,
the same way tools/check_config_keys.py gates the config keys.

Exit status is 1 on any mismatch, so this can gate a release.

  --sizes   re-measure per-feature sizes from a built tree and print the
            numbers to paste into features.json
"""

import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

FEATURES_MK = os.path.join(ROOT, "features", "features.mk")
FEATURES_JSON = os.path.join(ROOT, "features", "features.json")
SCHEMA = os.path.join(ROOT, "docs", "boot.conf.schema.json")
EXAMPLE = os.path.join(ROOT, "boot.conf.example")
SRC = os.path.join(ROOT, "src")
STUBS = os.path.join(SRC, "stubs")

ARCHES = ("x86_64", "aarch64")

def read(path):
    with open(path, encoding="utf-8") as fh:
        return fh.read()

def parse_mk(text):
    """Every `NAME := a b c` in the file, with line continuations folded.

    The whitespace classes are horizontal-only on purpose: \\s would let an
    empty assignment (`FEAT_gui_DEPS :=`) swallow the line after it.
    """
    folded = text.replace("\\\n", " ")
    out = {}
    for m in re.finditer(r"^([A-Za-z_][A-Za-z0-9_]*)[ \t]*:=[ \t]*(.*)$",
                         folded, re.M):
        out[m.group(1)] = m.group(2).split()
    return out

def mk_registry():
    mk = parse_mk(read(FEATURES_MK))
    feats = mk.get("FEAT_ALL", [])

    reg = {}
    for f in feats:
        reg[f] = {
            "src": mk.get("FEAT_%s_SRC" % f, []),
            "src_gui": mk.get("FEAT_%s_SRC_GUI" % f, []),
            "deps": mk.get("FEAT_%s_DEPS" % f, []),
            "stub": mk.get("FEAT_%s_STUB" % f, []),
            "arch": mk.get("FEAT_%s_ARCH" % f, []),
            "profiles": mk.get("FEAT_%s_PROFILES" % f, []),
        }

    profiles = {}
    for name in mk.get("PROFILE_ALL", []):
        raw = mk.get("PROFILE_%s" % name)
        if raw is None:
            profiles[name] = None
            continue
        profiles[name] = feats if raw == ["$(FEAT_ALL)"] else raw

    return feats, reg, profiles, mk.get("CORE_SRC", [])

class Checker:
    def __init__(self):
        self.problems = 0

    def fail(self, msg):
        self.problems += 1
        print("  %s" % msg)

    def section(self, title):
        print("%s" % title)

def check_registry(c, feats, reg, profiles, core):
    c.section("registry")

    if len(feats) != len(set(feats)):
        dup = sorted({f for f in feats if feats.count(f) > 1})
        c.fail("FEAT_ALL has duplicates: %s" % " ".join(dup))

    known = set(feats)

    for f in feats:
        if not reg[f]["src"] and not reg[f]["src_gui"]:
            c.fail("%s: no sources" % f)

    for f in feats:

        for d in reg[f]["deps"]:
            if d not in known:
                c.fail("%s: depends on unknown feature '%s'" % (f, d))

    colour = {}
    def visit(f, stack):
        if colour.get(f) == 2:
            return
        if colour.get(f) == 1:
            c.fail("dependency cycle: %s" % " -> ".join(stack + [f]))
            return
        colour[f] = 1
        for d in reg[f]["deps"]:
            if d in known:
                visit(d, stack + [f])
        colour[f] = 2
    for f in feats:
        visit(f, [])

    for f in feats:
        fa = set(reg[f]["arch"]) or set(ARCHES)
        for d in reg[f]["deps"]:
            if d not in known:
                continue
            da = set(reg[d]["arch"]) or set(ARCHES)
            if not fa <= da:
                c.fail("%s supports %s but depends on %s which supports only %s"
                       % (f, " ".join(sorted(fa)), d, " ".join(sorted(da))))
        for a in reg[f]["arch"]:
            if a not in ARCHES:
                c.fail("%s: unknown arch '%s'" % (f, a))

    coreset = set(core)
    seen = {}
    for f in feats:
        for s in reg[f]["src"] + reg[f]["src_gui"]:
            if not os.path.exists(os.path.join(SRC, s)):
                c.fail("%s: src/%s does not exist" % (f, s))
            if s in coreset:
                c.fail("%s: src/%s is also in CORE_SRC" % (f, s))
            if s in seen:
                c.fail("src/%s claimed by both %s and %s" % (s, seen[s], f))
            seen[s] = f
    for s in core:
        if not os.path.exists(os.path.join(SRC, s)):
            c.fail("CORE_SRC: src/%s does not exist" % s)

    for f in feats:

        if reg[f]["src_gui"] and f != "gui":
            reach = set()
            stack = list(reg[f]["deps"])
            while stack:
                d = stack.pop()
                if d in reach or d not in known:
                    continue
                reach.add(d)
                stack.extend(reg[d]["deps"])
            if "gui" not in reach and "gui" not in reg[f]["deps"]:
                pass

    for name, want in profiles.items():
        if want is None:
            continue
        for f in want:
            if f not in known:
                c.fail("profile %s: unknown feature '%s'" % (name, f))
        have = set(want)
        for f in want:
            if f not in known:
                continue
            for d in reg[f]["deps"]:
                if d not in have:
                    c.fail("profile %s: has %s but not its dependency %s"
                           % (name, f, d))

        for f in want:
            if f not in known:
                continue
            restricted = reg[f]["profiles"]
            if restricted and name not in restricted:
                c.fail("profile %s lists %s, which is restricted to: %s"
                       % (name, f, " ".join(restricted)))

        for f in want:
            if f not in known:
                continue
            for d in reg[f]["deps"]:
                if d not in known:
                    continue
                restricted = reg[d]["profiles"]
                if restricted and name not in restricted:
                    c.fail("profile %s: %s needs %s, restricted to: %s"
                           % (name, f, d, " ".join(restricted)))

    return known

def check_json_agrees(c, feats, reg, profiles, doc):
    c.section("features.json")

    jfeats = doc["features"]
    mk_set, js_set = set(feats), set(jfeats)

    for f in sorted(mk_set - js_set):
        c.fail("%s is in features.mk but not features.json" % f)
    for f in sorted(js_set - mk_set):
        c.fail("%s is in features.json but not features.mk" % f)

    for f in sorted(mk_set & js_set):
        spec = jfeats[f]
        for field in ("summary", "detail", "keys", "entrykeys", "hotkeys",
                      "cli", "size_kb"):
            if field not in spec:
                c.fail("%s: features.json is missing '%s'" % (f, field))

        jarch = spec.get("arch")
        marchs = reg[f]["arch"]
        if bool(jarch) != bool(marchs) or (jarch and sorted(jarch) != sorted(marchs)):
            c.fail("%s: arch disagrees - mk=%s json=%s"
                   % (f, marchs or "all", jarch or "all"))

        jprofs = spec.get("profiles")
        mprofs = reg[f]["profiles"]
        if bool(jprofs) != bool(mprofs) or (jprofs and sorted(jprofs) != sorted(mprofs)):
            c.fail("%s: profiles disagree - mk=%s json=%s"
                   % (f, " ".join(mprofs) or "any", " ".join(jprofs or []) or "any"))
        for p in mprofs:
            if p not in profiles:
                c.fail("%s: restricted to unknown profile '%s'" % (f, p))

        want_arch = set(marchs) or set(ARCHES)
        got_arch = set(spec.get("size_kb", {}))
        if got_arch != want_arch:
            c.fail("%s: size_kb covers %s, expected %s"
                   % (f, " ".join(sorted(got_arch)) or "nothing",
                      " ".join(sorted(want_arch))))

    jprof, mprof = set(doc["profiles"]), set(profiles)
    for p in sorted(mprof - jprof):
        c.fail("profile %s is in features.mk but not features.json" % p)
    for p in sorted(jprof - mprof):
        c.fail("profile %s is in features.json but not features.mk" % p)

    defaults = [p for p, s in doc["profiles"].items() if s.get("default")]
    if len(defaults) != 1:
        c.fail("exactly one profile must be marked default, found %d %s"
               % (len(defaults), defaults))

def check_keys(c, doc):
    """Every key a feature claims must exist in the schema, and no key may be
    claimed twice - otherwise filtering the example config is ambiguous.

    Features declare canonical keys only. Aliases inherit their canonical
    key's owner, the same way tools/check_config_keys.py expands them, so
    adding an alias to the schema never means editing this registry too."""
    c.section("config keys")

    schema = json.loads(read(SCHEMA))
    props = schema["properties"]
    entry_props = props["entry"]["properties"]

    def canon_and_aliases(table):
        """canonical name -> its aliases, and alias -> canonical."""
        aliases = {}
        for key, spec in table.items():
            for a in spec.get("x-aliases", []):
                aliases[a] = key
        return set(table), aliases

    gcanon, galias = canon_and_aliases({k: v for k, v in props.items()
                                        if k != "entry"})
    ecanon, ealias = canon_and_aliases(entry_props)

    def check_side(kind, claimed_by, canon, alias):
        owner = {}
        for f, keys in claimed_by:
            for k in keys:
                if k in alias:
                    c.fail("%s claims %s key '%s', which is an alias of '%s' - "
                           "claim the canonical key; aliases are inherited"
                           % (f, kind, k, alias[k]))
                    continue
                if k not in canon:
                    c.fail("%s claims %s key '%s', which the schema does not have"
                           % (f, kind, k))
                    continue
                if k in owner:
                    c.fail("%s key '%s' claimed by both %s and %s"
                           % (kind, k, owner[k], f))
                owner[k] = f
        for a, canonical in alias.items():
            if canonical in owner:
                owner[a] = owner[canonical]
        total = len(canon) + len(alias)
        print("  %d of %d %s keys owned by a feature (%d always present)"
              % (len(owner), total, kind, total - len(owner)))
        return owner

    owner_g = check_side("global",
                         [(f, s.get("keys", [])) for f, s in doc["features"].items()],
                         gcanon, galias)
    owner_e = check_side("entry",
                         [(f, s.get("entrykeys", [])) for f, s in doc["features"].items()],
                         ecanon, ealias)

    return owner_g, owner_e

def check_sections(c, doc):
    """Sections a feature claims must exist in boot.conf.example, so the
    installer's awk filter has something to match on."""
    c.section("boot.conf.example sections")

    text = read(EXAMPLE)
    have = set(re.findall(r"^# --- (.+?) -*\s*$", text, re.M))

    for f, spec in sorted(doc["features"].items()):
        sec = spec.get("section")
        if sec and sec not in have:
            c.fail("%s claims section %r, which is not in boot.conf.example"
                   % (f, sec))

def check_stubs(c, feats, reg):
    """Every removable feature needs its stub to exist once the stubs land."""
    c.section("stubs")

    if not os.path.isdir(STUBS):
        print("  src/stubs/ does not exist yet - skipping")
        return

    for f in feats:
        for s in reg[f]["stub"]:
            p = os.path.join(STUBS, s)
            if not os.path.exists(p):
                c.fail("%s: src/stubs/%s does not exist" % (f, s))

def check_cli(c, doc):
    """A feature that guards a `visor` subcommand must name one that exists."""
    c.section("visor subcommands")

    visor = os.path.join(ROOT, "visor")
    if not os.path.exists(visor):
        print("  ./visor not found - skipping")
        return
    text = read(visor)

    for f, spec in sorted(doc["features"].items()):
        for cmd in spec.get("cli", []):
            if not re.search(r"^\s+%s\b" % re.escape(cmd), text, re.M):
                c.fail("%s guards `visor %s`, which is not in the usage text"
                       % (f, cmd))

def measure(build_dir="build"):
    """Allocated size of each feature's objects, per arch, from a built tree."""
    feats, reg, _, core = mk_registry()
    keep = (".text", ".rodata", ".data", ".sdata")

    def alloc(objs):
        objs = [o for o in objs if os.path.exists(o)]
        if not objs:
            return None
        out = subprocess.run(["size", "-A"] + objs,
                             capture_output=True, text=True).stdout
        total = 0
        for line in out.splitlines():
            p = line.split()
            if len(p) >= 2 and p[0] in keep:
                try:
                    total += int(p[1])
                except ValueError:
                    pass
        return total

    print('  "size_kb" values, rounded as features.json stores them:\n')
    for f in feats:
        srcs = reg[f]["src"] + reg[f]["src_gui"]
        parts = []
        for a in ARCHES:
            if reg[f]["arch"] and a not in reg[f]["arch"]:
                continue
            n = alloc([os.path.join(ROOT, build_dir, a, s[:-2] + ".o")
                       for s in srcs])
            if n is None:
                parts.append('"%s": null' % a)
            else:
                parts.append('"%s": %d' % (a, round(n / 1024.0)))
        print('    %-14s { %s }' % (f, ", ".join(parts)))

def main():
    if "--sizes" in sys.argv:
        i = sys.argv.index("--sizes")
        build = sys.argv[i + 1] if len(sys.argv) > i + 1 else "build"
        measure(build)
        return 0

    feats, reg, profiles, core = mk_registry()
    doc = json.loads(read(FEATURES_JSON))
    c = Checker()

    print("%d features, %d profiles, %d core sources\n"
          % (len(feats), len(profiles), len(core)))

    check_registry(c, feats, reg, profiles, core)
    check_json_agrees(c, feats, reg, profiles, doc)
    check_keys(c, doc)
    check_sections(c, doc)
    check_stubs(c, feats, reg)
    check_cli(c, doc)

    if c.problems:
        print("\n%d problem(s)" % c.problems)
        return 1
    print("\nregistry is consistent")
    return 0

if __name__ == "__main__":
    sys.exit(main())
