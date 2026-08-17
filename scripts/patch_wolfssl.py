from pathlib import Path

Import("env")


PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
MARKER = "/* CrossPoint wolfSSL compatibility overrides */"
OVERRIDES = f"""

{MARKER}
#undef NO_DH
#ifndef HAVE_FFDHE_2048
#define HAVE_FFDHE_2048
#endif
/* MEMFIX-PORT: 8192 handles up to RSA-4096 keys (the public-CA maximum,
   ISRG Root X1 included) with half the per-bignum heap of 16384: with
   WOLFSSL_SMALL_STACK each fast-math temp is FP_MAX_BITS/8 * 2 bytes on the
   heap, and TLS cert verification allocates dozens at once. */
#undef FP_MAX_BITS
#define FP_MAX_BITS 8192
/* Client-side TLS 1.3 session tickets (RFC 8446 s4.6.1). A resumed handshake
   sends no certificate chain, so it skips both the chain transfer and the
   signature verification that dominate a full handshake here: FP_MAX_BITS 8192
   with WOLFSSL_SMALL_STACK puts every bignum temp on the heap at 2KB apiece,
   and cert verification wants dozens at once. Device capture (opds_debug.txt):
   53 resume hops of one download cost 100,655ms of handshake against 274,268ms
   wall -- 37% of the transfer -- at 1,634ms each, and the handshake flight is
   also the session's heap peak (~35-43KB) on a device whose largest free block
   sits near 14KB. Costs ~1KB of persistent heap for the cached session plus
   its ticket (staticTicket is SESSION_TICKET_LEN bytes). Both yapaa hosts were
   verified with openssl s_client -sess_out/-sess_in to issue AND accept 1.3
   tickets ("Reused, TLSv1.3"). */
#ifndef HAVE_SESSION_TICKET
#define HAVE_SESSION_TICKET
#endif
"""


def patch_user_settings(path: Path) -> None:
    text = path.read_text()
    if MARKER in text:
        text = text.split(MARKER, 1)[0].rstrip()
    path.write_text(text + OVERRIDES + "\n")
    print(f"Patched wolfSSL settings: {path.relative_to(PROJECT_DIR)}")


for settings in PROJECT_DIR.glob(".pio/libdeps/*/Arduino-wolfSSL/src/user_settings.h"):
    patch_user_settings(settings)
