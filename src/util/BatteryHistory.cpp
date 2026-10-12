#include "BatteryHistory.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace battery {

namespace {
constexpr uint32_t HEADER_MAGIC = 0x31425043;  // "CPB1"

void put16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
void put32(uint8_t* p, uint32_t v) {
  put16(p, static_cast<uint16_t>(v));
  put16(p + 2, static_cast<uint16_t>(v >> 16));
}
uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t get32(const uint8_t* p) { return get16(p) | (static_cast<uint32_t>(get16(p + 2)) << 16); }
}  // namespace

uint8_t crc8(const uint8_t* data, size_t len) {
  // CRC-8/ATM (poly 0x07), init 0. Bitwise: records are written every few minutes.
  uint8_t crc = 0;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b) crc = static_cast<uint8_t>((crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1);
  }
  return crc;
}

void encodeRecord(const Record& r, uint8_t out[RECORD_BYTES]) {
  put32(out + 0, r.seq);
  put32(out + 4, r.epoch);
  put32(out + 8, r.uptimeS);
  put16(out + 12, r.boot);
  put16(out + 14, r.millivolts);
  out[16] = r.soc;
  out[17] = static_cast<uint8_t>(r.event);
  out[18] = r.flags;
  out[19] = crc8(out, RECORD_BYTES - 1);
}

bool decodeRecord(const uint8_t in[RECORD_BYTES], Record& out) {
  if (crc8(in, RECORD_BYTES - 1) != in[19]) return false;
  out.seq = get32(in + 0);
  if (out.seq == 0) return false;
  out.epoch = get32(in + 4);
  out.uptimeS = get32(in + 8);
  out.boot = get16(in + 12);
  out.millivolts = get16(in + 14);
  out.soc = in[16];
  out.event = static_cast<Event>(in[17]);
  out.flags = in[18];
  return true;
}

void encodeHeader(const Header& h, uint8_t out[HEADER_BYTES]) {
  put32(out + 0, HEADER_MAGIC);
  put32(out + 4, h.nextSeq);
  put32(out + 8, h.chargedPct);
  put32(out + 12, h.usedPct);
  put32(out + 16, h.awakeS);
  put32(out + 20, h.firstEpoch);
  put16(out + 24, h.boot);
  out[26] = h.level;
  out[27] = 0;
  put16(out + 28, static_cast<uint16_t>(RING_CAPACITY));
  out[30] = 0;
  out[31] = crc8(out, HEADER_BYTES - 1);
}

bool decodeHeader(const uint8_t in[HEADER_BYTES], Header& out) {
  if (crc8(in, HEADER_BYTES - 1) != in[31]) return false;
  if (get32(in) != HEADER_MAGIC || get16(in + 28) != RING_CAPACITY) return false;
  out.nextSeq = get32(in + 4);
  if (out.nextSeq == 0) return false;
  out.chargedPct = get32(in + 8);
  out.usedPct = get32(in + 12);
  out.awakeS = get32(in + 16);
  out.firstEpoch = get32(in + 20);
  out.boot = get16(in + 24);
  out.level = in[26];
  return true;
}

bool pickHeader(const uint8_t a[HEADER_BYTES], const uint8_t b[HEADER_BYTES], Header& out) {
  Header ha;
  Header hb;
  const bool okA = decodeHeader(a, ha);
  const bool okB = decodeHeader(b, hb);
  if (!okA && !okB) return false;
  out = (okA && (!okB || ha.nextSeq >= hb.nextSeq)) ? ha : hb;
  return true;
}

bool isChargeStep(const Record& prev, const Record& cur) {
  if (prev.usb() || cur.usb()) return true;
  return prev.boot != cur.boot && prev.socKnown() && cur.socKnown() && cur.soc >= prev.soc + OFF_CHARGE_MIN_RISE;
}

void LevelTracker::step(const Record& prev, const Record& cur) {
  if (!cur.socKnown()) return;
  if (level == SOC_UNKNOWN) {
    level = cur.soc;
    return;
  }
  if (isChargeStep(prev, cur)) {
    if (cur.soc > level) charged += cur.soc - level;
    level = cur.soc;  // a charge resets the drain reference, up or down
  } else if (cur.soc < level) {
    used += level - cur.soc;
    level = cur.soc;
  }
}

void UsageAccumulator::add(const Record& r) {
  ++usage_.samples;
  if (havePrev_) {
    const uint32_t usedBefore = tracker_.used;
    tracker_.step(prev_, r);
    const uint32_t usedNow = tracker_.used - usedBefore;
    if (r.boot == prev_.boot) {
      if (r.uptimeS >= prev_.uptimeS) {
        const uint32_t dt = r.uptimeS - prev_.uptimeS;
        usage_.awakeS += dt;
        if (r.flags & FLAG_READING) usage_.readingS += dt;
        if (r.flags & FLAG_WIFI) usage_.wifiS += dt;
      }
      usage_.usedAwakePct += usedNow;
    } else if (prev_.dated() && r.dated() && r.epoch > prev_.epoch) {
      // The wake sample lands a few seconds into the boot; those count as asleep, not awake.
      const uint32_t gap = r.epoch - prev_.epoch;
      usage_.asleepS += gap > r.uptimeS ? gap - r.uptimeS : 0;
      usage_.usedAsleepPct += usedNow;
    }
  } else {
    tracker_.step(r, r);
  }
  usage_.usedPct = tracker_.used;
  usage_.chargedPct = tracker_.charged;
  prev_ = r;
  havePrev_ = true;
}

void LastChargeFinder::add(const Record& r) {
  if (havePrev_) {
    const bool offChargeWake = !prev_.usb() && !r.usb() && isChargeStep(prev_, r);
    if ((prev_.usb() && !r.usb()) || offChargeWake) {
      found_ = true;
      endSeq_ = r.seq;
      endEpoch_ = r.dated() ? r.epoch : 0;
      endSoc_ = r.soc;
    }
  }
  charging_ = r.usb();
  prev_ = r;
  havePrev_ = true;
}

void BucketBuilder::markSpan(const int col, const Record& r) {
  int from = col;
  if (haveSpan_ && r.boot == spanBoot_ && spanCol_ <= col) from = spanCol_;
  const bool charging = haveSpan_ && from != col && spanUsb_ && r.usb();
  for (int c = std::max(from, 0); c <= col && c < count_; ++c) {
    buckets_[c].flags |= BUCKET_AWAKE;
    if (charging) buckets_[c].flags |= BUCKET_CHARGING;
  }
  spanCol_ = col;
  spanBoot_ = r.boot;
  spanUsb_ = r.usb();
  haveSpan_ = true;
}

void BucketBuilder::place(const int col, const Record& r) {
  if (col < 0 || col >= count_) return;
  markSpan(col, r);
  if (!r.socKnown()) return;
  Bucket& b = buckets_[col];
  if (b.empty()) {
    b.minSoc = b.maxSoc = b.firstSoc = r.soc;
    if (haveLast_ && r.boot != lastBoot_) b.flags |= BUCKET_WAKE;
  } else {
    if (r.soc < b.minSoc) b.minSoc = r.soc;
    if (r.soc > b.maxSoc) b.maxSoc = r.soc;
  }
  b.lastSoc = r.soc;
  if (r.usb()) b.flags |= BUCKET_CHARGING;
  lastBoot_ = r.boot;
  haveLast_ = true;
}

void BucketBuilder::addDated(const Record& r, const uint32_t t0, const uint32_t t1) {
  if (!r.dated() || r.epoch < t0 || r.epoch >= t1 || t1 <= t0) return;
  const uint64_t col = static_cast<uint64_t>(r.epoch - t0) * static_cast<uint64_t>(count_) / (t1 - t0);
  place(static_cast<int>(col), r);
}

void BucketBuilder::addIndexed(const Record& r, const uint32_t index, const uint32_t total) {
  if (total == 0 || index >= total) return;
  const uint64_t col = static_cast<uint64_t>(index) * static_cast<uint64_t>(count_) / total;
  place(static_cast<int>(col), r);
}

void formatSpan(const uint32_t seconds, char* buf, const size_t len) {
  const uint32_t days = seconds / 86400;
  const uint32_t hours = (seconds % 86400) / 3600;
  const uint32_t minutes = (seconds % 3600) / 60;
  if (days > 0) {
    snprintf(buf, len, "%lud %luh", static_cast<unsigned long>(days), static_cast<unsigned long>(hours));
  } else if (hours > 0) {
    snprintf(buf, len, "%luh %lum", static_cast<unsigned long>(hours), static_cast<unsigned long>(minutes));
  } else {
    snprintf(buf, len, "%lum", static_cast<unsigned long>(minutes));
  }
}

}  // namespace battery
