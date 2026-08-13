#pragma once

#include <cstddef>
#include <cstdint>

struct JsonCallbacks {
  void* ctx;
  void (*onKey)(void* ctx, const char* key, size_t len);
  void (*onString)(void* ctx, const char* value, size_t len);
  void (*onNumber)(void* ctx, const char* value, size_t len);
  void (*onBool)(void* ctx, bool value);
  void (*onNull)(void* ctx);
  void (*onObjectStart)(void* ctx);
  void (*onObjectEnd)(void* ctx);
  void (*onArrayStart)(void* ctx);
  void (*onArrayEnd)(void* ctx);
  // Optional, and LAST on purpose: both existing consumers build this struct with positional
  // aggregate init, so anywhere but the end would silently re-seat their callbacks.
  //
  // When set, string VALUES are delivered through this instead of onString, split into pieces
  // of at most TOKEN_BUF_SIZE-1 bytes: `first` marks the opening piece, `last` the closing one,
  // and a value that fits in one buffer arrives as a single call with both set.
  //
  // Exists because without it a value longer than the token buffer is silently DISCARDED
  // (appendToken flags overflow, emitToken then skips the callback) — fine for the fixed-size
  // fields ReleaseJsonParser reads, fatal for KOReader stats, whose per-device blobs carry
  // multi-KB base64 and would vanish with no error anywhere. Consumers that leave this null
  // keep the original truncate-and-drop behaviour exactly.
  //
  // Pieces arrive already unescaped (handleStringChar resolves \" \\ \n etc. before this), so
  // an embedded JSON document can be reassembled or re-parsed directly. \uXXXX is still passed
  // through literally, as everywhere else in this parser.
  void (*onStringChunk)(void* ctx, const char* data, size_t len, bool first, bool last) = nullptr;
};

class StreamingJsonParser {
 public:
  static constexpr size_t TOKEN_BUF_SIZE = 512;
  static constexpr size_t MAX_NESTING = 32;

  explicit StreamingJsonParser(const JsonCallbacks& callbacks);

  void reset();
  void feed(const char* data, size_t len);

  bool hasError() const { return error; }

 private:
  enum class State : uint8_t {
    SCANNING,
    IN_STRING_KEY,
    IN_STRING_VALUE,
    IN_NUMBER,
    IN_LITERAL,
    SKIP_STRING,
  };

  enum class Container : uint8_t {
    NONE,
    OBJECT,
    ARRAY,
  };

  void handleScanning(char c);
  void handleStringChar(char c);
  void handleNumber(char c);
  void handleLiteral(char c);
  void handleSkipString(char c);

  void appendToken(char c);
  void emitToken();

  bool inArray() const { return nestingDepth > 0 && nestingStack[nestingDepth - 1] == Container::ARRAY; }

  JsonCallbacks cb;
  char tokenBuf[TOKEN_BUF_SIZE];
  size_t tokenLen;
  State state;
  bool expectingValue;
  bool escaped;
  bool tokenOverflow;
  // True once a chunk has been handed out for the string value currently being read, so
  // emitToken knows whether its terminal piece is also the `first` one.
  bool chunkEmitted;
  bool error;

  Container nestingStack[MAX_NESTING];
  uint8_t nestingDepth;

  char literalExpected[6];
  uint8_t literalLen;
  uint8_t literalPos;
};
