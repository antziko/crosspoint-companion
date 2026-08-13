#pragma once

#include <Logging.h>
#include <StreamingJsonParser.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "KOReaderSyncClient.h"

// Streaming decoder for the KOSync stats response.
//
// Split out of KOReaderSyncClient.cpp so it can be exercised on the host: the file it came from
// pulls in Arduino, wolfSSL and ArduinoJson, none of which a unit test can link, while everything
// here is plain C++ over StreamingJsonParser. See test/koreader-stats-stream/.
//
// Why it exists at all: the previous decode buffered the whole response and then built an
// ArduinoJson document over it (~3x the body), peaking near 36 KB on a device reporting
// free=36256 largest=6388. It failed as a *silent* half-sync — every leg logged code=200 while
// `others` read 0. Nothing here allocates per response.

namespace kostats {

// Upper bound on a decoded dated-history blob (ReadingTimeHistory::BLOB_MAX_BYTES is 876; round
// up for headroom). Bounds the decode buffer — a malformed/oversized "h" is rejected, not grown.
constexpr size_t kStatsDatedMaxBytes = 1024;

// Upper bound on a decoded per-book dictionary-history blob ("dh"). Matches the 4 KB
// serializeBlob cap on the sender.
constexpr size_t kStatsDictMaxBytes = 4096;

// Upper bound on a decoded per-book flashcard blob ("fc"). Matches the sender's
// FlashcardDeck::FC_SLICE_CEIL adaptive cap.
constexpr size_t kStatsFcMaxBytes = 6144;

// --- Streaming base64, for the "h"/"dh"/"fc" payloads -------------------------------------
//
// mbedtls_base64_decode needs the whole input at once, which is exactly what we are trying to
// stop holding: those payloads run to ~5.5 KB each and the encoded form is another third on top.
// Decoding 4 characters at a time straight into the caller's fold buffer removes the encoded
// copy entirely — the only memory left is the decoded buffer the fold was always going to need.
//
// The carry is the whole point: chunk boundaries fall wherever the token buffer happened to
// fill, so a quantum is routinely split across two feeds.
class Base64Streamer {
 public:
  void begin(uint8_t* out, size_t cap) {
    out_ = out;
    cap_ = cap;
    len_ = 0;
    carry_ = 0;
    bad_ = false;
  }
  void feed(const char* data, size_t len) {
    if (!out_ || bad_) return;
    for (size_t i = 0; i < len; i++) {
      const char c = data[i];
      if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
      if (c == '=') {  // padding: no further output, and nothing after it matters
        flushCarry();
        return;
      }
      const int v = valueOf(c);
      if (v < 0) {
        bad_ = true;
        return;
      }
      quantum_[carry_++] = static_cast<uint8_t>(v);
      if (carry_ == 4) emit(3);
    }
  }
  // Returns decoded length, or 0 if the payload was malformed or overran the buffer.
  size_t finish() {
    if (bad_) return 0;
    flushCarry();
    return bad_ ? 0 : len_;
  }

 private:
  static int valueOf(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  }
  // A trailing group of 2 or 3 characters carries 1 or 2 bytes; a lone character is malformed.
  void flushCarry() {
    if (carry_ == 0) return;
    if (carry_ == 1) {
      bad_ = true;
      carry_ = 0;
      return;
    }
    const uint8_t produced = static_cast<uint8_t>(carry_ - 1);
    while (carry_ < 4) quantum_[carry_++] = 0;
    emit(produced);
  }
  void emit(uint8_t count) {
    const uint32_t triple = (static_cast<uint32_t>(quantum_[0]) << 18) | (static_cast<uint32_t>(quantum_[1]) << 12) |
                            (static_cast<uint32_t>(quantum_[2]) << 6) | static_cast<uint32_t>(quantum_[3]);
    for (uint8_t i = 0; i < count; i++) {
      if (len_ >= cap_) {  // oversized payload: fail rather than truncate into a fold
        bad_ = true;
        carry_ = 0;
        return;
      }
      out_[len_++] = static_cast<uint8_t>((triple >> (16 - 8 * i)) & 0xFF);
    }
    carry_ = 0;
  }

  uint8_t* out_ = nullptr;
  size_t cap_ = 0;
  size_t len_ = 0;
  uint8_t quantum_[4] = {};
  uint8_t carry_ = 0;
  bool bad_ = false;
};

// --- Streaming stats decode ----------------------------------------------------------------
//
// Shape on the wire: {"stats":{"<deviceId>":"<a JSON document, as a STRING>", ...}}
// Each device blob is {"s":300,"lr":9650,"lh":21,"lm":15,"h":"<b64>","dh":"<b64>","fc":"<b64>"}.
//
// Two parsers, nested: the outer walks the envelope and hands each device's blob text to the
// inner one, which is reset per device. The outer's chunks arrive already unescaped, so they
// are valid JSON for the inner parser with no intermediate copy. Peak cost is the two parsers
// (~600 B each) plus the fold buffers that were always required — the response body and the
// ~3x ArduinoJson document built over it are both gone.

// Per-device blob consumer, driven by the inner parser.
struct BlobDecoder {
  KOReaderStatsEntry* entry = nullptr;
  bool isOther = false;
  const StatsDatedFold* fold = nullptr;
  const StatsDatedFold* dictFold = nullptr;
  const StatsDatedFold* fcFold = nullptr;
  uint8_t* datedBuf = nullptr;
  uint8_t* dictBuf = nullptr;
  uint8_t* fcBuf = nullptr;

  char key[8] = {};  // longest key we act on is "dh"/"fc"/"lr" — 8 is ample
  Base64Streamer b64;
  // Both set together while streaming a payload we actually want, cleared otherwise. Holding
  // the buffer here rather than re-deriving it from activeFold keeps the fold call unambiguous.
  const StatsDatedFold* activeFold = nullptr;
  uint8_t* activeBuf = nullptr;

  void reset(KOReaderStatsEntry* e, bool other) {
    entry = e;
    isOther = other;
    key[0] = '\0';
    activeFold = nullptr;
    activeBuf = nullptr;
  }
};

void blobOnKey(void* ctx, const char* key, size_t len) {
  auto* d = static_cast<BlobDecoder*>(ctx);
  if (len >= sizeof(d->key)) len = sizeof(d->key) - 1;
  memcpy(d->key, key, len);
  d->key[len] = '\0';
}

void blobOnNumber(void* ctx, const char* value, size_t len) {
  auto* d = static_cast<BlobDecoder*>(ctx);
  (void)len;
  if (!d->entry) return;
  const uint32_t v = static_cast<uint32_t>(strtoul(value, nullptr, 10));
  if (strcmp(d->key, "s") == 0) {
    d->entry->seconds = v;
  } else if (strcmp(d->key, "lr") == 0) {
    d->entry->lastReadDayIndex = v;
  } else if (strcmp(d->key, "lh") == 0) {
    d->entry->lastReadHour = static_cast<uint8_t>(v);
  } else if (strcmp(d->key, "lm") == 0) {
    d->entry->lastReadMinute = static_cast<uint8_t>(v);
  }
}

void blobOnStringChunk(void* ctx, const char* data, size_t len, bool first, bool last) {
  auto* d = static_cast<BlobDecoder*>(ctx);
  if (first) {
    // Local history is the source of truth and is uploaded, not merged back in, so only OTHER
    // devices' payloads are folded. A payload with no fold requested is skipped outright rather
    // than decoded and discarded — that is the whole saving over the filtered-document version.
    d->activeFold = nullptr;
    d->activeBuf = nullptr;
    if (d->isOther) {
      if (strcmp(d->key, "h") == 0 && d->fold && d->fold->fn && d->datedBuf) {
        d->activeFold = d->fold;
        d->activeBuf = d->datedBuf;
        d->b64.begin(d->datedBuf, kStatsDatedMaxBytes);
      } else if (strcmp(d->key, "dh") == 0 && d->dictFold && d->dictFold->fn && d->dictBuf) {
        d->activeFold = d->dictFold;
        d->activeBuf = d->dictBuf;
        d->b64.begin(d->dictBuf, kStatsDictMaxBytes);
      } else if (strcmp(d->key, "fc") == 0 && d->fcFold && d->fcFold->fn && d->fcBuf) {
        d->activeFold = d->fcFold;
        d->activeBuf = d->fcBuf;
        d->b64.begin(d->fcBuf, kStatsFcMaxBytes);
      }
    }
  }
  if (!d->activeFold) return;
  d->b64.feed(data, len);
  if (last) {
    const size_t dlen = d->b64.finish();
    if (dlen > 0) {
      d->activeFold->fn(d->activeFold->ctx, d->activeBuf, dlen);
    } else {
      LOG_DBG("KOSync", "Skipping bad \"%s\" payload (base64 decode failed or overran)", d->key);
    }
    d->activeFold = nullptr;
    d->activeBuf = nullptr;
  }
}

// Envelope consumer, driven by the outer parser.
struct StatsDecoder {
  StreamingJsonParser* inner = nullptr;
  BlobDecoder blob;

  KOReaderStatsEntry* outEntries = nullptr;
  size_t maxEntries = 0;
  size_t count = 0;
  // This device's id, injected rather than read from KOReaderSyncClient::deviceId(): that
  // reads the eFuse MAC, which no host test can provide, and a decoder that has to be told
  // who it is has no hidden state to get wrong.
  const char* selfDeviceId = nullptr;

  int depth = 0;
  bool pendingStatsKey = false;
  bool inStats = false;
  bool sawStats = false;
  bool skippingBlob = false;  // past MAX_STATS_DEVICES: parse nothing, just let it flow past
  size_t bytes = 0;

  // Every skip drops one device's seconds from the caller's `others` sum with no error return,
  // so a fully-skipped decode is reported as others=0 alongside fetch=1. Counted so the log can
  // tell that apart from a server that genuinely holds one device.
  uint16_t skipBad = 0;   // inner parser rejected the blob
  uint16_t skipOver = 0;  // past MAX_STATS_DEVICES
};

void statsOnKey(void* ctx, const char* key, size_t len) {
  auto* d = static_cast<StatsDecoder*>(ctx);
  if (d->depth == 1) {
    d->pendingStatsKey = (len == 5 && strcmp(key, "stats") == 0);
    return;
  }
  if (d->depth == 2 && d->inStats && d->count < d->maxEntries) {
    // Device id doubles as the "is this us?" test, so it is captured before the blob arrives.
    snprintf(d->outEntries[d->count].deviceId, sizeof(d->outEntries[d->count].deviceId), "%.*s", (int)len, key);
  }
}

void statsOnObjectStart(void* ctx) {
  auto* d = static_cast<StatsDecoder*>(ctx);
  d->depth++;
  if (d->depth == 2 && d->pendingStatsKey) {
    d->inStats = true;
    d->sawStats = true;
  }
}

void statsOnObjectEnd(void* ctx) {
  auto* d = static_cast<StatsDecoder*>(ctx);
  if (d->depth == 2 && d->inStats) d->inStats = false;
  if (d->depth > 0) d->depth--;
}

void statsOnStringChunk(void* ctx, const char* data, size_t len, bool first, bool last) {
  auto* d = static_cast<StatsDecoder*>(ctx);
  if (!d->inStats || d->depth != 2) return;  // some other string in the envelope

  if (first) {
    d->skippingBlob = (d->count >= d->maxEntries);
    if (d->skippingBlob) {
      d->skipOver++;
      LOG_DBG("KOSync", "More than %u stats devices; extras dropped", (unsigned)d->maxEntries);
    } else {
      KOReaderStatsEntry& e = d->outEntries[d->count];
      const bool isOther = strcmp(e.deviceId, d->selfDeviceId ? d->selfDeviceId : "") != 0;
      e.seconds = 0;
      e.lastReadDayIndex = 0;
      e.lastReadHour = 0;
      e.lastReadMinute = 0;
      d->blob.reset(&e, isOther);
      d->inner->reset();
    }
  }
  if (d->skippingBlob) return;

  d->inner->feed(data, len);

  if (last) {
    if (d->inner->hasError()) {
      d->skipBad++;
      LOG_DBG("KOSync", "Skipping malformed stats blob for %s", d->outEntries[d->count].deviceId);
      d->outEntries[d->count].deviceId[0] = '\0';
    } else {
      d->count++;
    }
  }
}

}  // namespace kostats
