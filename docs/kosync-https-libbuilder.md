# KOSync HTTPS on X4 — mbedTLS DYNAMIC_BUFFER via lib-builder

## Why

The X4 (ESP32-C3) HTTPS sync handshake fails on a settled heap. Root cause is
hardware-verified via the `FAILALLOC` instrumentation in
`lib/KOReaderSync/KOReaderSyncClient.cpp`:

```
BOOKMARKS_GET fails=1 maxSize=16717 lastSize=16717 caps=0x804   (largest8=32756)
```

- `16717` = one mbedTLS record buffer (16384 content + 333 overhead).
- `fails=1`, not 2 → the handshake allocates **two** 16717 buffers (in + out). The
  first succeeds from the ~32 K largest free block; the second 16717 then has no
  contiguous block and fails → `ssl_setup` fails → `ESP_ERR_HTTP_CONNECT` in ~70 ms.
- So the wall = **2 × 16717 ≈ 33.4 K contiguous**, against a WiFi-settled largest
  free block of ~30–32 K. It only succeeds on a pristine heap (~34–36 K).

The prebuilt `libmbedtls.a` already ships `IN_CONTENT_LEN=8192`,
`OUT_CONTENT_LEN=2048`, `ASYMMETRIC_CONTENT_LEN=y` — but the runtime still allocates
16384-content buffers **up front** at handshake. ASYMMETRIC only shrinks them
*after* the handshake, which is too late. So shrinking `IN_CONTENT_LEN` further does
not help. The only lever that changes handshake-time allocation is
`CONFIG_MBEDTLS_DYNAMIC_BUFFER=y`, which allocates the record buffer to the actual
record size and frees it between flights — peak becomes one ≤16717 block at a time.

`custom_sdkconfig` in `platformio.ini` cannot set this: pioarduino links a **prebuilt**
`libmbedtls.a` (mbedtls is never compiled from source; zero mbedtls `.o` in `.pio`).
The only way to change mbedtls config is to rebuild the Arduino core libs with
`esp32-arduino-lib-builder`.

## Coordinates (must match exactly, or link fails)

From the installed framework (`~/.platformio/packages/framework-arduinoespressif32*`):

- Arduino core: **espressif/arduino-esp32 3.3.7**
- IDF: **pioarduino/esp-idf v5.5.2** (fork, sha `87912cd291`) — *not* vanilla Espressif IDF
- Target: **esp32c3** (RISC-V → `riscv32-esp-elf`, auto-fetched by lib-builder)

A different IDF minor → ABI/symbol mismatch when linking the new `libmbedtls.a`
against the rest of the prebuilt libs. This is the #1 failure mode, not the mbedtls edit.

## 0. Prereqs (Kali / Debian)

```bash
sudo apt update
sudo apt install -y git wget flex bison gperf python3 python3-venv python3-pip \
  cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0
```

~10–14 GB free disk for a **C3-only** build (~25 GB only if you let `build.sh` pull
every chip's toolchain / build all targets — scoping to `-t esp32c3` roughly halves
it). ~1–2 h first build. Lives in the workspace parent
`/home/antz/Documents/github/crosspoint-reader/` (not a git repo — no nesting issue).

> Disk check: `df -h .`. Rough C3-only budget: IDF+submodules ~3 G, riscv toolchain
> ~1.5 G, esp-idf venv ~1 G, arduino src+libs ~1 G, build output ~3–5 G, ccache
> ~1–2 G. A mid-build `ENOSPC` corrupts the tree — restart from clean.

## 1. Clone (master = IDF 5.5 line)

```bash
cd /home/antz/Documents/github/crosspoint-reader
git clone https://github.com/espressif/esp32-arduino-lib-builder.git
cd esp32-arduino-lib-builder
./build.sh -h            # confirm flags for this checkout
```

`master` pins `IDF_BRANCH=release/v5.5` (`tools/config.sh`), matching your linked IDF
5.5.x. There is no separate `release/v5.5` branch (releases jump 5.4 → 6.0), so **stay
on `master`** — no checkout needed.

ABI note: this builds against Espressif's IDF `release/v5.5`; your prebuilt links
pioarduino's IDF fork (`v5.5.2`, sha 87912cd291). Both are 5.5.x — mbedtls/esp-tls ABI
is stable across 5.5 patch levels, so the single-lib swap (Option A) links. A wider
drift would only matter if you swap the whole package (Option B).

## 2. The config edit that actually works — ASYMMETRIC, not DYNAMIC_BUFFER

Append to `configs/defconfig.esp32c3` (applied after `defconfig.common`, so it wins):

```
CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y
CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=8192
CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=4096
```

Why this and not the obvious options (all dead ends, learned the hard way):

- **`MBEDTLS_SSL_MAX_CONTENT_LEN=8192` does NOTHING** — in mbedtls 3.6 it is vestigial.
  `ssl.h` hardcodes `IN/OUT_CONTENT_LEN` to 16384 when undefined, and `esp_config.h`
  only defines the real `IN/OUT` knobs when `ASYMMETRIC_CONTENT_LEN=y`. With asymmetric
  off, the buffers are 16384 regardless of MAX. Verify with objdump (below).
- **`DYNAMIC_BUFFER` can't be used** — `defconfig.common` sets `MBEDTLS_SSL_PROTO_DTLS=y`,
  and Kconfig makes `DYNAMIC_BUFFER depend on !MBEDTLS_SSL_PROTO_DTLS`, so it is silently
  dropped. (Don't disable DTLS to force it — that changes mbedtls struct layout → ABI
  mismatch with the un-rebuilt prebuilt libs.)

ASYMMETRIC + `IN=8192`/`OUT=4096` gives in_buf ~8.5K, out_buf ~4.4K (vs 16.7K each).
The larger single alloc is ~8.5K → fits the WiFi-settled largest free block (~30K) with
room for the second. ABI-safe: the in/out buffers are heap pointers (`mbedtls_calloc` in
`libmbedtls_2.a`), not struct fields, so no struct-layout change vs the other prebuilt libs.

## 3. Build C3 only

```bash
unset PYTHONPATH VIRTUAL_ENV   # see env traps below
export PATH="/usr/bin:$(echo "$PATH" | tr ':' '\n' | grep -v '.platformio/penv' | paste -sd:)"
./build.sh -t esp32c3
```

Output: `out/tools/esp32-arduino-libs/esp32c3/`. ~1–2 h first run.

If you changed the defconfig after a prior build and the result didn't change, ccache
may have served a stale object — force a clean mbedtls recompile:
```bash
export CCACHE_DISABLE=1
find build -path "*mbedtls/mbedtls/library*" -name "*.obj" -delete
rm -f build/esp-idf/mbedtls/mbedtls/library/libmbedtls.a
./build.sh -t esp32c3
```

### Env traps (each one wasted a build cycle)
- `build.sh` needs ESP-IDF already installed; first run *should* do it but its install
  conditional is flaky. If `idf.py: command not found`, install manually:
  `cd esp-idf && ./install.sh esp32c3 && cd ..` (then `build.sh`).
- IDF `install.sh` refuses if any venv is active. **PlatformIO's penv is on `PATH`**, so
  `python3` resolves to a venv → "can not create a virtual environment again". Strip penv
  from `PATH` and use `/usr/bin/python3` (the export line above).
- `~/.zshrc` exports `PYTHONPATH=/usr/lib/python3/dist-packages` → leaks Kali system
  packages into the IDF venv → `pyparsing`/`cryptography` version-check fails →
  `idf.py: command not found`. **`unset PYTHONPATH`** before install AND build.

### Verify the buffer actually shrank (do this before swapping)
```bash
OBJDUMP=$(find ~/.espressif/tools -name riscv32-esp-elf-objdump | head -1)
cd /tmp && ar x <repo>/esp32-arduino-lib-builder/build/esp-idf/mbedtls/mbedtls/library/libmbedtls.a ssl_tls.c.obj
"$OBJDUMP" -d ssl_tls.c.obj | grep -cE 'lui\s+\w+,0x2\b'   # want >0  (0x2 = 8192)
"$OBJDUMP" -d ssl_tls.c.obj | grep -cE 'lui\s+\w+,0x4\b'   # want 0   (0x4 = 16384)
```

## 4. Integrate — swap `libmbedtls_2.a` (NOT `libmbedtls.a`)

The package splits the mbedtls component: `libmbedtls.a` is just the CA-bundle port (2
objects); the real SSL/TLS code (`ssl_tls.c`, `ssl_msg.c` — where the buffers allocate)
is in **`libmbedtls_2.a`**. Swap that one.

```bash
LIBS=~/.platformio/packages/framework-arduinoespressif32-libs/esp32c3/lib
OUT=/home/antz/Documents/github/crosspoint-reader/esp32-arduino-lib-builder/out/tools/esp32-arduino-libs/esp32c3/lib
cp "$LIBS/libmbedtls_2.a" "$LIBS/libmbedtls_2.a.stock"   # backup once
cp "$OUT/libmbedtls_2.a"  "$LIBS/libmbedtls_2.a"          # swap
```

Only `libmbedtls_2.a` is needed — the buffer alloc is internal to it. No need to touch
`libmbedtls.a`, `libesp-tls.a`, or `sdkconfig`.

### Option B — full package override (clean)

```ini
# platformio.local.ini  (gitignored — keeps platformio.ini clean / CI unaffected)
[env:default]
platform_packages =
  framework-arduinoespressif32-libs @ file:///home/antz/Documents/github/crosspoint-reader/esp32-arduino-lib-builder/out/tools/esp32-arduino-libs
```

## 5. Rebuild firmware + verify

```bash
cd ~/Documents/github/crosspoint-reader/mcrosson-crosspoint-reader
pio run -e default -t clean      # force full relink against the new libs
pio run -e default -t upload
```

Acceptance — VERIFIED 2026-06-22 on settled heap (`largest8` 24–33 K), `https-debug.txt`:

- `TLS connected after ~1500ms` at `largest8` as low as **24564** — handshakes that used
  to hard-fail below the 33.4K wall now complete, **and**
- **no** `FAILALLOC` / `abort()` / `ESP_ERR_HTTP_CONNECT` anywhere, **and**
- every leg 200: `BOOKMARKS_GET/PUT`, `STATS_GET/PUT` (incl. `STATS_PUT body=5299`),
  `PROGRESS_GET`.

The `FAILALLOC` instrumentation (commit `46d4e5da`) is the acceptance test — it stays
silent now; leave it in as a regression tripwire or remove once you're satisfied.

## 6. Rollback

```bash
LIBS=~/.platformio/packages/framework-arduinoespressif32-libs/esp32c3/lib
cp "$LIBS/libmbedtls_2.a.stock" "$LIBS/libmbedtls_2.a"
cd ~/Documents/github/crosspoint-reader/mcrosson-crosspoint-reader && pio run -e default -t clean
```

## Durability — the swap does NOT survive a framework reinstall

The swapped `libmbedtls_2.a` lives in `~/.platformio/packages/...`, which PlatformIO
**overwrites** if the platform/framework is reinstalled or updated. To not lose the fix:

- Keep the lib-builder `out/` tree (and `libmbedtls_2.a.stock`) so you can re-swap, OR
- Pin via `platformio.local.ini` (gitignored, machine-specific — never `platformio.ini`,
  CI keeps the stock prebuilt):
  ```ini
  [env:default]
  platform_packages =
    framework-arduinoespressif32-libs @ file:///home/antz/Documents/github/crosspoint-reader/esp32-arduino-lib-builder/out/tools/esp32-arduino-libs
  ```
  (Note: the override needs the whole libs tree to contain the patched `libmbedtls_2.a`.)

## Notes

- If link fails with undefined/duplicate mbedtls symbols → IDF/arduino version drift; try
  also swapping `libesp-tls.a` from the same build, or go full-package via the override.
- Net: in_buf 16717→~8525, out_buf →~4429. Max single contiguous alloc ~8.5K fits the
  WiFi-settled largest free block (~30K) → the 33.4K contiguity wall is gone.
