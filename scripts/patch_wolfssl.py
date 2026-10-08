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
/* MEMFIX-PORT: fast math covers up to 2048-bit operands (RSA-1024/2048 and
   FFDHE-2048); RSA-2048/3072/4096 run on the SP code (WOLFSSL_HAVE_SP_RSA plus
   WOLFSSL_SP_4096 here), which uses fixed arrays, so RSA-4096 keys (the
   public-CA maximum, ISRG Root X1 included) still verify. Every fp_int is sized
   by FP_MAX_BITS, so 4096 keeps an RsaKey to ~4KB and halves every SMALL_STACK
   temp: a full handshake must fit beside the File Transfer server. */
#undef FP_MAX_BITS
#define FP_MAX_BITS 4096
#ifndef WOLFSSL_SP_4096
#define WOLFSSL_SP_4096
#endif
/* Client-side TLS 1.3 session tickets (RFC 8446 s4.6.1). A resumed handshake
   sends no certificate chain, so it skips both the chain transfer and the
   signature verification that dominate a full handshake here: WOLFSSL_SMALL_STACK
   puts every bignum temp on the heap, and cert verification wants dozens at once. Device capture (opds_debug.txt):
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
