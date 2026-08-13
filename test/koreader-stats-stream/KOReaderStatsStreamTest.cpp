#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "StatsStreamDecode.h"

// Exercises the streaming replacement for the buffer-then-ArduinoJson stats decode. The old path
// peaked near 36 KB on a device reporting free=36256 largest=6388 and failed as a SILENT half
// sync: every leg logged code=200 while `others` read 0. These tests pin the two things that
// made it silent — devices being dropped, and payloads being dropped — plus the base64 carry
// across chunk boundaries, which is the genuinely new logic.

namespace {

// Minimal base64 encoder, so fixtures are written as the bytes they should decode to.
std::string b64(const std::vector<uint8_t>& in) {
  static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    const uint32_t t = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
    out += T[(t >> 18) & 63];
    out += T[(t >> 12) & 63];
    out += T[(t >> 6) & 63];
    out += T[t & 63];
  }
  if (i + 1 == in.size()) {
    const uint32_t t = in[i] << 16;
    out += T[(t >> 18) & 63];
    out += T[(t >> 12) & 63];
    out += "==";
  } else if (i + 2 == in.size()) {
    const uint32_t t = (in[i] << 16) | (in[i + 1] << 8);
    out += T[(t >> 18) & 63];
    out += T[(t >> 12) & 63];
    out += T[(t >> 6) & 63];
    out += '=';
  }
  return out;
}

// A JSON string value containing a JSON document — the shape the server actually stores.
std::string embed(const std::string& inner) {
  std::string out = "\"";
  for (const char c : inner) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  out += '"';
  return out;
}

struct FoldCapture {
  std::vector<std::vector<uint8_t>> blobs;
};
void captureFold(void* ctx, const uint8_t* blob, size_t len) {
  static_cast<FoldCapture*>(ctx)->blobs.push_back(std::vector<uint8_t>(blob, blob + len));
}

struct Harness {
  KOReaderStatsEntry entries[KOReaderSyncClient::MAX_STATS_DEVICES];
  uint8_t datedBuf[kostats::kStatsDatedMaxBytes];
  uint8_t dictBuf[kostats::kStatsDictMaxBytes];
  uint8_t fcBuf[kostats::kStatsFcMaxBytes];
  FoldCapture dated, dict, fc;
  StatsDatedFold datedFold{&dated, captureFold};
  StatsDatedFold dictFold{&dict, captureFold};
  StatsDatedFold fcFold{&fc, captureFold};
  kostats::StatsDecoder dec;

  // feedSize 0 == one shot; otherwise the body is fed in fixed slices, which is what the TLS
  // read loop actually does and what the chunk carry has to survive.
  void run(const std::string& body, const char* self = "crosspoint-me", size_t feedSize = 0) {
    dec.outEntries = entries;
    dec.maxEntries = KOReaderSyncClient::MAX_STATS_DEVICES;
    dec.selfDeviceId = self;
    dec.blob.fold = &datedFold;
    dec.blob.dictFold = &dictFold;
    dec.blob.fcFold = &fcFold;
    dec.blob.datedBuf = datedBuf;
    dec.blob.dictBuf = dictBuf;
    dec.blob.fcBuf = fcBuf;

    JsonCallbacks blobCbs = {};
    blobCbs.ctx = &dec.blob;
    blobCbs.onKey = kostats::blobOnKey;
    blobCbs.onNumber = kostats::blobOnNumber;
    blobCbs.onStringChunk = kostats::blobOnStringChunk;
    StreamingJsonParser inner(blobCbs);
    dec.inner = &inner;

    JsonCallbacks envCbs = {};
    envCbs.ctx = &dec;
    envCbs.onKey = kostats::statsOnKey;
    envCbs.onObjectStart = kostats::statsOnObjectStart;
    envCbs.onObjectEnd = kostats::statsOnObjectEnd;
    envCbs.onStringChunk = kostats::statsOnStringChunk;
    StreamingJsonParser outer(envCbs);

    if (feedSize == 0) {
      outer.feed(body.data(), body.size());
    } else {
      for (size_t i = 0; i < body.size(); i += feedSize) {
        outer.feed(body.data() + i, std::min(feedSize, body.size() - i));
      }
    }
    dec.bytes = body.size();
  }
};

}  // namespace

TEST(KOReaderStatsStream, DecodesScalarsForEachDevice) {
  Harness h;
  const std::string body = R"({"stats":{"crosspoint-aaa":)" + embed(R"({"s":300,"lr":9650,"lh":21,"lm":15})") +
                           R"(,"crosspoint-bbb":)" + embed(R"({"s":1200,"lr":9651,"lh":7,"lm":3})") + R"(}})";
  h.run(body);

  ASSERT_EQ(h.dec.count, 2u);
  EXPECT_TRUE(h.dec.sawStats);
  EXPECT_STREQ(h.entries[0].deviceId, "crosspoint-aaa");
  EXPECT_EQ(h.entries[0].seconds, 300u);
  EXPECT_EQ(h.entries[0].lastReadDayIndex, 9650u);
  EXPECT_EQ(h.entries[0].lastReadHour, 21);
  EXPECT_EQ(h.entries[0].lastReadMinute, 15);
  EXPECT_STREQ(h.entries[1].deviceId, "crosspoint-bbb");
  EXPECT_EQ(h.entries[1].seconds, 1200u);
}

TEST(KOReaderStatsStream, MissingStatsObjectIsReportedNotGuessed) {
  Harness h;
  h.run("{}");
  EXPECT_FALSE(h.dec.sawStats);  // caller turns this into NOT_FOUND, not "0 seconds"
  EXPECT_EQ(h.dec.count, 0u);
}

TEST(KOReaderStatsStream, FoldsOtherDevicesPayloads) {
  Harness h;
  const std::vector<uint8_t> dated{1, 2, 3, 4, 5};
  const std::string body =
      R"({"stats":{"crosspoint-other":)" + embed(R"({"s":10,"h":")" + b64(dated) + R"("})") + R"(}})";
  h.run(body, "crosspoint-me");

  ASSERT_EQ(h.dec.count, 1u);
  ASSERT_EQ(h.dated.blobs.size(), 1u);
  EXPECT_EQ(h.dated.blobs[0], dated);
}

TEST(KOReaderStatsStream, SkipsOwnDevicePayload) {
  Harness h;
  const std::vector<uint8_t> dated{9, 9, 9};
  const std::string body = R"({"stats":{"crosspoint-me":)" + embed(R"({"s":10,"h":")" + b64(dated) + R"("})") + R"(}})";
  h.run(body, "crosspoint-me");

  ASSERT_EQ(h.dec.count, 1u);
  EXPECT_EQ(h.entries[0].seconds, 10u);
  EXPECT_TRUE(h.dated.blobs.empty()) << "local history is uploaded, never merged back in";
}

TEST(KOReaderStatsStream, LargePayloadSurvivesChunkBoundaries) {
  // The regression that matters: a payload far longer than the parser's 512-byte token buffer,
  // fed in slices that fall at arbitrary points inside base64 quanta.
  std::vector<uint8_t> big(900);
  for (size_t i = 0; i < big.size(); i++) big[i] = static_cast<uint8_t>((i * 7 + 13) & 0xFF);
  const std::string body = R"({"stats":{"crosspoint-other":)" + embed(R"({"s":5,"h":")" + b64(big) + R"("})") + R"(}})";

  for (const size_t slice : {size_t{0}, size_t{1}, size_t{3}, size_t{64}, size_t{511}, size_t{512}}) {
    Harness h;
    h.run(body, "crosspoint-me", slice);
    ASSERT_EQ(h.dec.count, 1u) << "slice=" << slice;
    ASSERT_EQ(h.dated.blobs.size(), 1u) << "slice=" << slice;
    EXPECT_EQ(h.dated.blobs[0], big) << "slice=" << slice;
    EXPECT_EQ(h.entries[0].seconds, 5u) << "slice=" << slice;
  }
}

TEST(KOReaderStatsStream, AllThreePayloadKindsRouteToTheirOwnFold) {
  Harness h;
  const std::vector<uint8_t> dated{1, 1}, dict{2, 2, 2}, fc{3, 3, 3, 3};
  const std::string blob =
      R"({"s":1,"h":")" + b64(dated) + R"(","dh":")" + b64(dict) + R"(","fc":")" + b64(fc) + R"("})";
  h.run(R"({"stats":{"crosspoint-other":)" + embed(blob) + R"(}})", "crosspoint-me");

  ASSERT_EQ(h.dec.count, 1u);
  ASSERT_EQ(h.dated.blobs.size(), 1u);
  ASSERT_EQ(h.dict.blobs.size(), 1u);
  ASSERT_EQ(h.fc.blobs.size(), 1u);
  EXPECT_EQ(h.dated.blobs[0], dated);
  EXPECT_EQ(h.dict.blobs[0], dict);
  EXPECT_EQ(h.fc.blobs[0], fc);
}

TEST(KOReaderStatsStream, OversizedPayloadIsRejectedNotTruncated) {
  // A truncated fold would corrupt the merge silently; the decoder must drop it instead.
  std::vector<uint8_t> huge(kostats::kStatsDatedMaxBytes + 64, 0xAB);
  const std::string body =
      R"({"stats":{"crosspoint-other":)" + embed(R"({"s":1,"h":")" + b64(huge) + R"("})") + R"(}})";
  Harness h;
  h.run(body, "crosspoint-me");

  EXPECT_EQ(h.dec.count, 1u);          // the device's scalars still count
  EXPECT_TRUE(h.dated.blobs.empty());  // but the payload is refused outright
}

TEST(KOReaderStatsStream, MalformedBase64IsDropped) {
  Harness h;
  const std::string body = R"({"stats":{"crosspoint-other":)" + embed(R"({"s":1,"h":"!!!not base64!!!"})") + R"(}})";
  h.run(body, "crosspoint-me");
  EXPECT_TRUE(h.dated.blobs.empty());
}

TEST(KOReaderStatsStream, ExtraDevicesBeyondTheCapAreCounted) {
  Harness h;
  std::string body = R"({"stats":{)";
  for (size_t i = 0; i < KOReaderSyncClient::MAX_STATS_DEVICES + 3; i++) {
    if (i) body += ",";
    body += "\"dev" + std::to_string(i) + "\":" + embed(R"({"s":1})");
  }
  body += "}}";
  h.run(body, "crosspoint-me");

  EXPECT_EQ(h.dec.count, KOReaderSyncClient::MAX_STATS_DEVICES);
  EXPECT_EQ(h.dec.skipOver, 3);  // dropped devices are visible, not silent
}

TEST(KOReaderStatsStream, PayloadWithoutAFoldRequestedIsIgnored) {
  // Nothing should be decoded when the caller asked for no fold — that skip is the saving.
  Harness h;
  const std::vector<uint8_t> dated{4, 5, 6};
  h.dec.outEntries = h.entries;

  const std::string body =
      R"({"stats":{"crosspoint-other":)" + embed(R"({"s":2,"h":")" + b64(dated) + R"("})") + R"(}})";
  h.run(body, "crosspoint-me");
  ASSERT_EQ(h.dated.blobs.size(), 1u);  // sanity: with a fold requested it does decode

  // Same body, no fold registered: the base64 is walked past without being decoded anywhere.
  Harness noFold;
  noFold.dec.outEntries = noFold.entries;
  noFold.dec.maxEntries = KOReaderSyncClient::MAX_STATS_DEVICES;
  noFold.dec.selfDeviceId = "crosspoint-me";
  noFold.dec.blob.datedBuf = noFold.datedBuf;  // buffer present, fold absent
  JsonCallbacks blobCbs = {};
  blobCbs.ctx = &noFold.dec.blob;
  blobCbs.onKey = kostats::blobOnKey;
  blobCbs.onNumber = kostats::blobOnNumber;
  blobCbs.onStringChunk = kostats::blobOnStringChunk;
  StreamingJsonParser inner(blobCbs);
  noFold.dec.inner = &inner;
  JsonCallbacks envCbs = {};
  envCbs.ctx = &noFold.dec;
  envCbs.onKey = kostats::statsOnKey;
  envCbs.onObjectStart = kostats::statsOnObjectStart;
  envCbs.onObjectEnd = kostats::statsOnObjectEnd;
  envCbs.onStringChunk = kostats::statsOnStringChunk;
  StreamingJsonParser outer(envCbs);
  outer.feed(body.data(), body.size());

  EXPECT_EQ(noFold.dec.count, 1u);
  EXPECT_EQ(noFold.entries[0].seconds, 2u);
  EXPECT_TRUE(noFold.dated.blobs.empty());
}
