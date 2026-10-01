"""
PlatformIO pre-build script: choose which hyphenation pattern sets are compiled in.

Reads `custom_hyphenation_langs` from the environment (comma-separated primary tags,
e.g. "en,de,fr", or "all") and defines CP_HYPH_<TAG>=0 for every language left out.
LanguageRegistry.cpp defaults each undefined tag to 1, so the source still builds every
language when this script does not run.

The patterns are the single largest block of data after the fonts -- German alone is
206 KB of the 351 KB total -- and a language left out only loses its line breaking, so
a device that never opens books in it pays nothing for the omission.

English is always built: it is the fallback, and an empty registry would leave nothing
for getLanguageEntries() to return.
"""

import sys

# Primary tags with a generated trie under lib/Epub/Epub/hyphenation/generated/.
SUPPORTED = ("en", "fr", "de", "ru", "es", "it", "pl", "sv", "uk", "fi", "pt")
ALWAYS_BUILT = "en"


def warn(msg):
    print(f"WARNING [select_hyphenation.py]: {msg}", file=sys.stderr)


def parse_langs(value):
    """The set of tags to build. `None`/"all"/empty means every supported language."""
    if value is None:
        return set(SUPPORTED)
    text = value.strip().lower()
    if text in ("", "all"):
        return set(SUPPORTED)

    chosen = set()
    for raw in text.split(","):
        tag = raw.strip()
        if not tag:
            continue
        if tag not in SUPPORTED:
            warn(f'unknown hyphenation language "{tag}"; supported: {", ".join(SUPPORTED)}')
            continue
        chosen.add(tag)

    if not chosen:
        warn("no recognised hyphenation languages; building all of them")
        return set(SUPPORTED)
    chosen.add(ALWAYS_BUILT)
    return chosen


def apply(env):
    chosen = parse_langs(env.GetProjectOption("custom_hyphenation_langs", "all"))
    omitted = [tag for tag in SUPPORTED if tag not in chosen]
    if not omitted:
        return
    env.Append(CPPDEFINES=[(f"CP_HYPH_{tag.upper()}", 0) for tag in omitted])
    print(f"Hyphenation: building {len(chosen)} of {len(SUPPORTED)} languages "
          f"(omitted: {', '.join(omitted)})")


if "Import" in globals():
    Import("env")  # noqa: F821  # type: ignore[name-defined]
    apply(env)     # noqa: F821  # type: ignore[name-defined]
