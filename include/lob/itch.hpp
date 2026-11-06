#pragma once

// Nasdaq TotalView-ITCH 5.0 message parsing and encoding (the subset this project uses).
//
// Wire format: every message starts with a 1-byte type; all integers are big-endian; timestamps are
// 6-byte nanoseconds since midnight; prices are unsigned 32-bit integers in 1/10000 dollar, which is
// used directly as integer ticks. In a BinaryFILE the messages are framed with a 2-byte big-endian
// length prefix. Spec: Nasdaq "NQTV-ITCH Specification 5.0". Only these types are decoded: S, R, H,
// A, F, E, C, X, D, U, P, Q. Every other type is reported as Unknown (and skipped by its length).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace lob::itch {

struct Message {
  char type = 0;
  std::uint16_t locate = 0;     ///< stock locate code
  std::uint64_t ts_ns = 0;      ///< nanoseconds since midnight
  std::uint64_t ref = 0;        ///< order reference (A F E C X D P); original reference for U
  std::uint64_t new_ref = 0;    ///< U: new order reference
  std::uint64_t qty = 0;        ///< shares: A F P new shares of U executed(E C) cancelled(X); cross shares (Q)
  std::int64_t price = 0;       ///< 1/10000 dollar (A F C U P Q)
  std::uint64_t match = 0;      ///< match number (E C P Q)
  char side = 0;                ///< 'B' or 'S' (A F P)
  char code = 0;                ///< S: event code; H: trading state; Q: cross type
  bool printable = true;        ///< C only
  char stock[9] = {0};          ///< symbol, right-trimmed (R H A F P Q)

  friend bool operator==(const Message& a, const Message& b) {
    return a.type == b.type && a.locate == b.locate && a.ts_ns == b.ts_ns && a.ref == b.ref && a.new_ref == b.new_ref &&
           a.qty == b.qty && a.price == b.price && a.match == b.match && a.side == b.side && a.code == b.code &&
           a.printable == b.printable && std::strncmp(a.stock, b.stock, 8) == 0;
  }
};

enum class ParseStatus { Ok, Unknown, Malformed };

/// Parses one message body (without the 2-byte length prefix). Unknown: a type we do not decode.
/// Malformed: a known type whose length does not match the specification.
ParseStatus parse(const std::uint8_t* p, std::size_t len, Message& out);

/// Appends one framed message (2-byte length prefix + body) for the supported types. Used by tests
/// and by the synthetic ITCH generator so that no Nasdaq data is needed to exercise the pipeline.
bool append_framed(std::vector<std::uint8_t>& out, const Message& m);

/// Expected body length for a type, or 0 if the type is not one we decode.
std::size_t body_length(char type);

/// Reads length-prefixed messages from any source with `std::size_t read(void*, std::size_t)`.
template <class Source>
class FrameReader {
 public:
  explicit FrameReader(Source& src, std::size_t buf_size = 1 << 20) : src_(src), buf_(buf_size) {}

  /// Returns false at end of stream (or on a truncated final frame, see truncated()). `body` points
  /// into an internal buffer valid until the next call.
  bool next(const std::uint8_t*& body, std::size_t& len) {
    if (!ensure(2)) return false;
    const std::size_t n = (static_cast<std::size_t>(buf_[pos_]) << 8) | buf_[pos_ + 1];
    if (!ensure(2 + n)) {
      truncated_ = true;
      return false;
    }
    body = buf_.data() + pos_ + 2;
    len = n;
    pos_ += 2 + n;
    ++frames_;
    return true;
  }
  bool truncated() const { return truncated_; }
  std::uint64_t frames() const { return frames_; }

 private:
  // Makes at least `need` bytes available at buf_[pos_..end_).
  bool ensure(std::size_t need) {
    if (end_ - pos_ >= need) return true;
    if (pos_ > 0) {
      std::memmove(buf_.data(), buf_.data() + pos_, end_ - pos_);
      end_ -= pos_;
      pos_ = 0;
    }
    while (end_ < need) {
      const std::size_t got = src_.read(buf_.data() + end_, buf_.size() - end_);
      if (got == 0) return false;
      end_ += got;
    }
    return true;
  }
  Source& src_;
  std::vector<std::uint8_t> buf_;
  std::size_t pos_ = 0, end_ = 0;
  bool truncated_ = false;
  std::uint64_t frames_ = 0;
};

/// A source reading from a memory buffer (tests, synthetic files).
class MemorySource {
 public:
  MemorySource(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}
  explicit MemorySource(const std::vector<std::uint8_t>& v) : MemorySource(v.data(), v.size()) {}
  std::size_t read(void* dst, std::size_t n) {
    const std::size_t k = n < size_ - pos_ ? n : size_ - pos_;
    std::memcpy(dst, data_ + pos_, k);
    pos_ += k;
    return k;
  }

 private:
  const std::uint8_t* data_;
  std::size_t size_, pos_ = 0;
};

}  // namespace lob::itch
