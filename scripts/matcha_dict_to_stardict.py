#!/usr/bin/env python3
"""Convert a matcha-reader dictionary (vocab.idx + vocab.dat) to a StarDict dictionary.

matcha-reader's Japanese/Chinese dictionaries (written by its tools/dict_convert/convert_jmdict.py)
are a sorted array of 40-byte records -- headword (32 bytes, NUL padded) + offset (u32) +
length (u16) + priority (u8) + POS flags (u8) -- pointing into a plain-text blob. This firmware
reads StarDict, so this writes <stem>.ifo / .idx / .dict with sametypesequence=m (plain text,
which also enables the inline gloss box).

Several records can share a headword (a kanji form and its kana reading both point at the same
entry, and homographs have one entry each); their distinct definitions are joined under one
headword, highest priority first.

Usage:
    python3 scripts/matcha_dict_to_stardict.py <matcha-folder> <output-folder> [--name vocab]
        [--title "JMdict (Japanese-English)"]
    python3 scripts/dictionary_tools.py prep <output-folder>

Copy the output folder into /dictionaries/ (or /.dictionaries/) on the SD card.
"""

import argparse
import struct
import sys
from pathlib import Path

RECORD = struct.Struct("<32sIHBB")


def read_records(idx_path: Path, dat_path: Path):
    """Yields (headword, definition, priority) for every record."""
    idx = idx_path.read_bytes()
    dat = dat_path.read_bytes()
    if len(idx) % RECORD.size:
        sys.exit(f"{idx_path}: size {len(idx)} is not a multiple of {RECORD.size}; not a matcha index")
    for i in range(0, len(idx), RECORD.size):
        raw, offset, length, priority, _pos = RECORD.unpack_from(idx, i)
        headword = raw.split(b"\0", 1)[0].decode("utf-8", errors="ignore").strip()
        if not headword or offset + length > len(dat):
            continue
        definition = dat[offset : offset + length].decode("utf-8", errors="replace").strip()
        if definition:
            yield headword, definition, priority


def convert(src: Path, out: Path, name: str, title: str) -> None:
    idx_path = src / f"{name}.idx"
    dat_path = src / f"{name}.dat"
    if not idx_path.is_file() or not dat_path.is_file():
        sys.exit(f"Missing {idx_path.name} / {dat_path.name} in {src}")
    if not title:
        title_path = src / f"{name}.title"
        title = title_path.read_text("utf-8").strip() if title_path.is_file() else f"{src.name}-{name}"

    # headword -> list of (priority, definition), deduplicated per headword.
    entries: dict[str, list[tuple[int, str]]] = {}
    for headword, definition, priority in read_records(idx_path, dat_path):
        bucket = entries.setdefault(headword, [])
        if all(definition != d for _, d in bucket):
            bucket.append((priority, definition))

    out.mkdir(parents=True, exist_ok=True)
    stem = out.name
    # The same order dictionary_tools.py merge writes, which the firmware's lookup expects.
    words = sorted(entries, key=lambda w: (w.lower(), w))
    with open(out / f"{stem}.dict", "wb") as dict_f, open(out / f"{stem}.idx", "wb") as idx_f:
        offset = 0
        for word in words:
            defs = sorted(entries[word], key=lambda e: -e[0])
            body = "\n\n".join(d for _, d in defs).encode("utf-8")
            dict_f.write(body)
            idx_f.write(word.encode("utf-8") + b"\0" + struct.pack(">II", offset, len(body)))
            offset += len(body)
    idx_size = (out / f"{stem}.idx").stat().st_size
    (out / f"{stem}.ifo").write_text(
        "StarDict's dict ifo file\n"
        "version=2.4.2\n"
        f"wordcount={len(words)}\n"
        f"idxfilesize={idx_size}\n"
        f"bookname={title}\n"
        "sametypesequence=m\n"
        # Which matcha dictionary this is: the reader shows Words / Names / Grammar tabs from it.
        f"matchaslot={name}\n",
        "utf-8",
    )
    print(f"{out}: {len(words)} headwords, title '{title}'")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("source", type=Path, help="matcha dictionary folder (holds vocab.idx / vocab.dat)")
    parser.add_argument("output", type=Path, help="StarDict folder to write; its name becomes the file stem")
    parser.add_argument("--name", default="vocab", help="matcha file stem: vocab (default) or names")
    parser.add_argument("--title", default="", help="display name (default: <name>.title, else folder name)")
    args = parser.parse_args()
    convert(args.source, args.output, args.name, args.title)


if __name__ == "__main__":
    main()
