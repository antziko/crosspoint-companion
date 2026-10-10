"""
PlatformIO pre-build script: move PNGdec's 32 KB zlib window out of the PNG object.

Upstream PNGdec embeds the inflate dictionary in PNGIMAGE, which makes `new PNG()`
one ~58 KB contiguous allocation. On the X3 the largest free block is ~61 KB at
boot and fragments below that within a few page turns, so every PNG then fails to
decode. With the window caller-supplied (PngToFramebufferConverter borrows the
boot-time InflateReader window), the object shrinks to ~26 KB.

PNGdec is a registry install (no .git), so the edit is an exact-text replacement
rather than `git apply`. Idempotent: a file already carrying MARKER is skipped; a
file with neither the marker nor the expected upstream text aborts the build.
"""

Import("env")  # noqa: F821 (SCons-injected global)
import os
import sys

MARKER = "CrossPoint: caller-supplied zlib window"

EDITS = {
    "PNGdec.h": [
        (
            "    uint8_t ucZLIB[32768 + sizeof(struct inflate_state)]; // put this here to avoid needing malloc/free\n",
            "    uint8_t ucZLIB[sizeof(struct inflate_state)]; // " + MARKER + "\n"
            "    uint8_t *pZlibWindow; // 32768 bytes, set by setZlibWindow() after open()\n",
        ),
        (
            "    int decode(void *pUser, int iOptions);\n",
            "    int decode(void *pUser, int iOptions);\n"
            "    void setZlibWindow(uint8_t *pWindow) { _png.pZlibWindow = pWindow; } // " + MARKER + "\n",
        ),
    ],
    "png.inl": [
        (
            "    state->window = &pPage->ucZLIB[sizeof(struct inflate_state)]; // point to 32k dictionary buffer\n",
            "    if (pPage->pZlibWindow == NULL) { // " + MARKER + "\n"
            "        pPage->iError = PNG_MEM_ERROR;\n"
            "        return PNG_MEM_ERROR;\n"
            "    }\n"
            "    state->window = pPage->pZlibWindow;\n",
        ),
    ],
}


def patch_pngdec(env):
    libdeps_dir = os.path.join(env["PROJECT_DIR"], ".pio", "libdeps")
    if not os.path.isdir(libdeps_dir):
        return
    for env_dir in os.listdir(libdeps_dir):
        src_dir = os.path.join(libdeps_dir, env_dir, "PNGdec", "src")
        if not os.path.isdir(src_dir):
            continue
        for name, edits in EDITS.items():
            _patch_file(os.path.join(src_dir, name), edits)


def _patch_file(path, edits):
    with open(path, encoding="utf-8") as f:
        text = f.read()
    if MARKER in text:
        return
    for old, new in edits:
        if text.count(old) != 1:
            sys.stderr.write(
                "ERROR: PNGdec patch does not apply to %s (expected text not found once):\n%s\n"
                % (path, old)
            )
            raise SystemExit(1)
        text = text.replace(old, new)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    print("Applied PNGdec patch: %s" % path)


patch_pngdec(env)  # noqa: F821
