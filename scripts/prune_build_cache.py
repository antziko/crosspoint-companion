"""Build-cache size guard (build_cache_dir = .cache).

PlatformIO's content-addressed build cache never evicts: every source/flag/
toolchain change adds new hash entries while stale ones stay forever, so the
cache grows without bound (observed 2.3 GB after one day of multi-env builds).

This pre-script sizes the cache before every build; past MAX_CACHE_BYTES it
clears the cache entirely. Clearing is always safe -- the cache is a pure
compile accelerator; the next build of each env recompiles once and re-seeds it.
"""

import os
import shutil

Import("env")  # noqa: F821  (PlatformIO SCons construction environment)

MAX_CACHE_BYTES = 3 * 1024 * 1024 * 1024  # 3 GB

CACHE_DIR = env.subst("$PROJECT_DIR") + os.sep + ".cache"  # noqa: F821


def cache_size_bytes():
    total = 0
    for root, _dirs, files in os.walk(CACHE_DIR):
        for name in files:
            try:
                total += os.stat(os.path.join(root, name)).st_size
            except OSError:
                continue
    return total


def main():
    if not os.path.isdir(CACHE_DIR):
        return
    total = cache_size_bytes()
    if total <= MAX_CACHE_BYTES:
        return
    # Remove the bucket subdirectories rather than CACHE_DIR itself so a
    # concurrent PlatformIO process never sees the configured dir vanish.
    removed = 0
    for entry in os.listdir(CACHE_DIR):
        path = os.path.join(CACHE_DIR, entry)
        try:
            if os.path.isdir(path):
                shutil.rmtree(path, ignore_errors=True)
            else:
                os.remove(path)
            removed += 1
        except OSError:
            continue
    print(
        "prune_build_cache: cache %.1f MB > %.0f MB cap -- cleared (%d top-level entries)"
        % (total / 1e6, MAX_CACHE_BYTES / 1e6, removed)
    )


main()
