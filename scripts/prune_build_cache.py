"""Build-cache size guard (build_cache_dir = .cache).

PlatformIO's content-addressed build cache never evicts: every source/flag/
toolchain change adds new hash entries while stale ones stay forever, so the
cache grows without bound (observed 2.3 GB after one day of multi-env builds).

This pre-script sizes the cache before every build and, past TARGET_CACHE_BYTES,
deletes the least recently used entries until it is back under the target. The
headroom below 3 GB absorbs what the build that follows adds. Removing entries is
always safe -- the cache is a pure compile accelerator; a missing object is just
recompiled and re-cached.
"""

import os

Import("env")  # noqa: F821  (PlatformIO SCons construction environment)

TARGET_CACHE_BYTES = int(2.5 * 1024 * 1024 * 1024)  # 2.5 GB

CACHE_DIR = env.subst("$PROJECT_DIR") + os.sep + ".cache"  # noqa: F821


def cache_entries():
    """(last use, size, path) for each cached object, skipping SCons's own files."""
    entries = []
    for root, _dirs, files in os.walk(CACHE_DIR):
        if root == CACHE_DIR:
            continue  # top level holds .sconsign*.dblite, not cached objects
        for name in files:
            path = os.path.join(root, name)
            try:
                st = os.stat(path)
            except OSError:
                continue
            entries.append((max(st.st_atime, st.st_mtime), st.st_size, path))
    return entries


def main():
    if not os.path.isdir(CACHE_DIR):
        return
    entries = cache_entries()
    total = sum(size for _used, size, _path in entries)
    if total <= TARGET_CACHE_BYTES:
        return
    entries.sort()
    freed = 0
    removed = 0
    for _used, size, path in entries:
        if total - freed <= TARGET_CACHE_BYTES:
            break
        try:
            os.remove(path)
        except OSError:
            continue
        freed += size
        removed += 1
    # Drop bucket directories left empty; keep CACHE_DIR itself so a concurrent
    # PlatformIO process never sees the configured dir vanish.
    for entry in os.listdir(CACHE_DIR):
        path = os.path.join(CACHE_DIR, entry)
        if os.path.isdir(path):
            try:
                os.rmdir(path)
            except OSError:
                pass  # not empty
    print(
        "prune_build_cache: cache %.1f MB > %.0f MB target -- removed %d oldest entries (%.1f MB)"
        % (total / 1e6, TARGET_CACHE_BYTES / 1e6, removed, freed / 1e6)
    )


main()
