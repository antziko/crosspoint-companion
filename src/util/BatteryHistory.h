#pragma once

#include <cstddef>
#include <cstdint>

// Battery history: record format, ring layout and the aggregation the Battery screen
// draws from. Pure C++ (no Arduino/HAL) so the host suite covers it; the SD file and the
// sampler live in BatteryLog.
//
// File layout (little-endian, per board): two 32-byte header slots, then RING_CAPACITY
// 20-byte record slots. Record `seq` (1-based, never reused) lives in slot
// (seq - 1) % RING_CAPACITY, so the ring needs no head pointer and a slot whose seq does
// not map back to it is stale. The header is written to slot (nextSeq & 1) after every
// record, so a power cut mid-write leaves the other slot intact.
namespace battery {

constexpr uint32_t RING_CAPACITY = 2048;  // ~30 days at typical use (~50 records/day)
constexpr size_t RECORD_BYTES = 20;
constexpr size_t HEADER_BYTES = 32;
constexpr size_t HEADER_SLOTS = 2;
constexpr size_t RECORDS_OFFSET = HEADER_BYTES * HEADER_SLOTS;
constexpr uint8_t SOC_UNKNOWN = 0xFF;
// Earliest plausible wall-clock epoch (2020-01-01); anything below is "no clock".
constexpr uint32_t MIN_VALID_EPOCH = 1577836800UL;
// A rise this large across a sleep with no USB seen means it was charged while off.
constexpr int OFF_CHARGE_MIN_RISE = 3;

enum class Event : uint8_t { Periodic = 0, Wake = 1, Sleep = 2, PlugIn = 3, Unplug = 4 };

enum Flag : uint8_t {
  FLAG_USB = 1 << 0,         // external power present at the sample
  FLAG_READING = 1 << 1,     // the reader was open since the previous sample (same boot)
  FLAG_WIFI = 1 << 2,        // the radio was on since the previous sample (same boot)
  FLAG_BACKFILLED = 1 << 3,  // epoch derived later in the same boot from uptime
};

struct Record {
  uint32_t seq = 0;
  uint32_t epoch = 0;  // UTC seconds, 0 = no clock at the time
  uint32_t uptimeS = 0;
  uint16_t boot = 0;
  uint16_t millivolts = 0;
  uint8_t soc = SOC_UNKNOWN;
  Event event = Event::Periodic;
  uint8_t flags = 0;

  bool dated() const { return epoch >= MIN_VALID_EPOCH; }
  bool socKnown() const { return soc <= 100; }
  bool usb() const { return (flags & FLAG_USB) != 0; }
};

struct Header {
  uint32_t nextSeq = 1;
  uint32_t chargedPct = 0;      // lifetime % gained
  uint32_t usedPct = 0;         // lifetime % lost
  uint32_t awakeS = 0;          // lifetime awake seconds
  uint32_t firstEpoch = 0;      // first dated sample, 0 = none yet
  uint16_t boot = 0;            // last boot number handed out
  uint8_t level = SOC_UNKNOWN;  // LevelTracker state, persisted across boots
};

uint8_t crc8(const uint8_t* data, size_t len);

void encodeRecord(const Record& r, uint8_t out[RECORD_BYTES]);
// False on a bad CRC or an empty (seq 0) slot.
bool decodeRecord(const uint8_t in[RECORD_BYTES], Record& out);

void encodeHeader(const Header& h, uint8_t out[HEADER_BYTES]);
bool decodeHeader(const uint8_t in[HEADER_BYTES], Header& out);
// The newer of two decoded slots; false when neither is valid.
bool pickHeader(const uint8_t a[HEADER_BYTES], const uint8_t b[HEADER_BYTES], Header& out);

inline uint32_t slotOf(uint32_t seq) { return (seq - 1) % RING_CAPACITY; }
inline size_t headerOffset(uint32_t nextSeq) { return (nextSeq & 1) * HEADER_BYTES; }
inline size_t recordOffset(uint32_t seq) { return RECORDS_OFFSET + static_cast<size_t>(slotOf(seq)) * RECORD_BYTES; }
// Oldest seq still in the ring.
inline uint32_t firstSeq(uint32_t nextSeq) { return nextSeq > RING_CAPACITY ? nextSeq - RING_CAPACITY : 1; }

// True when the step prev -> cur happened on external power, or was a rise across a
// sleep large enough to mean it was charged while off.
bool isChargeStep(const Record& prev, const Record& cur);

// Counts % used and % charged without counting gauge/ADC noise twice: while draining, a
// bounce up does not raise the level, so the next drop is not counted again; while
// charging, a bounce down is ignored.
struct LevelTracker {
  uint8_t level = SOC_UNKNOWN;
  uint32_t charged = 0;
  uint32_t used = 0;
  void step(const Record& prev, const Record& cur);
};

// Totals over consecutive record pairs. Awake time comes from uptime within a boot;
// asleep time only from dated pairs that span a boot.
struct Usage {
  uint32_t awakeS = 0;
  uint32_t readingS = 0;
  uint32_t wifiS = 0;
  uint32_t asleepS = 0;        // dated sleeps only
  uint32_t usedAwakePct = 0;   // drain within a boot, off USB
  uint32_t usedAsleepPct = 0;  // drain across dated sleeps
  uint32_t usedPct = 0;
  uint32_t chargedPct = 0;
  uint32_t samples = 0;
};

class UsageAccumulator {
 public:
  void add(const Record& r);
  const Usage& usage() const { return usage_; }

 private:
  Usage usage_;
  Record prev_;
  bool havePrev_ = false;
  LevelTracker tracker_;
};

// Finds the most recent charge: the sample where the last charge ended (the first
// sample off USB after one on it, or the wake after an off-charge).
class LastChargeFinder {
 public:
  void add(const Record& r);
  bool found() const { return found_; }
  uint32_t endSeq() const { return endSeq_; }
  uint32_t endEpoch() const { return endEpoch_; }
  uint8_t endSoc() const { return endSoc_; }
  // Still on USB at the newest sample.
  bool charging() const { return charging_; }

 private:
  Record prev_;
  bool havePrev_ = false;
  bool found_ = false;
  bool charging_ = false;
  uint32_t endSeq_ = 0;
  uint32_t endEpoch_ = 0;
  uint8_t endSoc_ = SOC_UNKNOWN;
};

// One graph column: SoC range and the endpoints used to join neighbouring columns.
struct Bucket {
  uint8_t minSoc = SOC_UNKNOWN;
  uint8_t maxSoc = SOC_UNKNOWN;
  uint8_t firstSoc = SOC_UNKNOWN;
  uint8_t lastSoc = SOC_UNKNOWN;
  uint8_t flags = 0;  // BUCKET_* below
  bool empty() const { return minSoc == SOC_UNKNOWN; }
};
enum BucketFlag : uint8_t {
  BUCKET_CHARGING = 1 << 0,  // on USB: a USB sample, or between two USB samples of one boot
  BUCKET_WAKE = 1 << 1,      // the first sample is from a new boot (join across a sleep)
  BUCKET_AWAKE = 1 << 2,     // a sample, or between two samples of one boot
};

// Places records into `count` columns over [t0, t1) (dated mode) or over sample index
// [0, total) (session mode, for boards whose samples carry no clock).
class BucketBuilder {
 public:
  BucketBuilder(Bucket* buckets, int count) : buckets_(buckets), count_(count) {}
  void addDated(const Record& r, uint32_t t0, uint32_t t1);
  void addIndexed(const Record& r, uint32_t index, uint32_t total);

 private:
  void place(int col, const Record& r);
  // Fills the awake/charging flags from the previous sample's column to this one, so a
  // column narrower than the sampling interval still shows as awake.
  void markSpan(int col, const Record& r);
  Bucket* buckets_;
  int count_;
  uint16_t lastBoot_ = 0;
  bool haveLast_ = false;
  int spanCol_ = 0;
  uint16_t spanBoot_ = 0;
  bool spanUsb_ = false;
  bool haveSpan_ = false;
};

// "2d 4h", "5h 12m", "12m", "0m".
void formatSpan(uint32_t seconds, char* buf, size_t len);

}  // namespace battery
