"""
PlatformIO pre-build script: pin CrossPoint's shrunk mbedTLS lib.

WHY: HTTPS (KOSync / OPDS / OTA) needs the per-connection mbedTLS record
buffers shrunk from the stock 16384-content size to 8192. The stock buffers
demand ~33KB of *contiguous* heap for a fresh TLS handshake; after WiFi + a
prior sync leg the largest free block sits below that, so the handshake fails
(ESP_ERR_HTTP_CONNECT) on the constrained X3/X4. The shrunk build (record bufs
~8525) needs only ~17KB contiguous, which fits the fragmented heap. Full story:
docs/kosync-https-libbuilder.md.

HOW the fix ships: a prebuilt shrunk `libmbedtls_2.a` lives in the repo at
`scripts/mbedtls/libmbedtls_2.a.shrunk`. The framework package's stock copy is
a raw file in `~/.platformio/packages/...` that PlatformIO OVERWRITES on any
framework reinstall/update — so the swap does NOT survive on its own (it has
silently reverted to stock at least once, bringing the HTTPS wall back). This
script re-applies the swap on every build, so a reinstall self-heals on the
next `pio run` and CI gets it too.

Idempotent: if the installed lib already byte-matches the shrunk copy, do
nothing. Otherwise back up the stock lib once (`.stock`) and copy the shrunk
one over it. Only touches the esp32c3 lib (the only target this firmware
builds for); other arches in the package are left alone.
"""

Import("env")  # noqa: F821 (SCons-injected global)
import filecmp
import os
import shutil
import sys

# The shrunk lib lives under esp32c3/ in the framework-libs package layout.
ARCH = "esp32c3"
LIB_NAME = "libmbedtls_2.a"

SHRUNK = os.path.join(env["PROJECT_DIR"], "scripts", "mbedtls", LIB_NAME + ".shrunk")  # noqa: F821


def _package_lib_path(env):
    """Resolve <framework-arduinoespressif32-libs>/esp32c3/lib/libmbedtls_2.a."""
    try:
        pkg_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32-libs")
    except Exception as exc:  # pragma: no cover - platform API shape varies
        sys.stderr.write("patch_mbedtls: cannot locate framework-libs package: %s\n" % exc)
        return None
    if not pkg_dir:
        return None
    return os.path.join(pkg_dir, ARCH, "lib", LIB_NAME)


def patch_mbedtls(env):
    if not os.path.isfile(SHRUNK):
        # The pin is load-bearing for HTTPS; a missing source is a build-config
        # error, not something to silently skip.
        raise SystemExit(
            "patch_mbedtls: shrunk lib missing at %s -- HTTPS handshake will use "
            "stock 16KB record buffers and fail on fragmented heap" % SHRUNK
        )

    target = _package_lib_path(env)
    if not target or not os.path.isfile(target):
        sys.stderr.write(
            "patch_mbedtls: package lib not found (%s); skipping pin\n" % target
        )
        return

    if filecmp.cmp(SHRUNK, target, shallow=False):
        return  # already pinned this build's package

    stock_backup = target + ".stock"
    if not os.path.exists(stock_backup):
        shutil.copy2(target, stock_backup)  # preserve the original once
    shutil.copy2(SHRUNK, target)
    print("patch_mbedtls: pinned shrunk %s over stock (HTTPS record bufs 8192)" % LIB_NAME)


patch_mbedtls(env)  # noqa: F821
