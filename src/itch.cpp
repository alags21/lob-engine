#include "lob/itch.hpp"

namespace lob::itch {

namespace {

std::uint64_t be(const std::uint8_t* p, int n) {
  std::uint64_t v = 0;
  for (int i = 0; i < n; ++i) v = (v << 8) | p[i];
  return v;
}

void put_be(std::vector<std::uint8_t>& out, std::uint64_t v, int n) {
  for (int i = n - 1; i >= 0; --i) out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xff));
}

void get_stock(const std::uint8_t* p, char* dst) {
  std::memcpy(dst, p, 8);
  dst[8] = 0;
  for (int i = 7; i >= 0 && (dst[i] == ' ' || dst[i] == 0); --i) dst[i] = 0;
}

void put_stock(std::vector<std::uint8_t>& out, const char* s) {
  std::size_t n = 0;
  while (n < 8 && s[n]) {
    out.push_back(static_cast<std::uint8_t>(s[n]));
    ++n;
  }
  for (; n < 8; ++n) out.push_back(' ');
}

}  // namespace

std::size_t body_length(char type) {
  switch (type) {
    case 'S': return 12;
    case 'R': return 39;
    case 'H': return 25;
    case 'A': return 36;
    case 'F': return 40;
    case 'E': return 31;
    case 'C': return 36;
    case 'X': return 23;
    case 'D': return 19;
    case 'U': return 35;
    case 'P': return 44;
    case 'Q': return 40;
    default: return 0;
  }
}

ParseStatus parse(const std::uint8_t* p, std::size_t len, Message& m) {
  m = Message{};
  if (len < 1) return ParseStatus::Malformed;
  const char t = static_cast<char>(p[0]);
  const std::size_t want = body_length(t);
  if (!want) return ParseStatus::Unknown;
  if (len != want) return ParseStatus::Malformed;
  m.type = t;
  m.locate = static_cast<std::uint16_t>(be(p + 1, 2));
  m.ts_ns = be(p + 5, 6);
  const std::uint8_t* b = p + 11;  // body after the common 11-byte header
  switch (t) {
    case 'S': m.code = static_cast<char>(b[0]); break;
    case 'R':
      get_stock(b, m.stock);
      m.category = static_cast<char>(b[8]);
      m.round_lot = static_cast<std::uint32_t>(be(b + 10, 4));
      m.etp = static_cast<char>(b[22]);
      break;
    case 'H':
      get_stock(b, m.stock);
      m.code = static_cast<char>(b[8]);
      break;
    case 'A':
    case 'F':
      m.ref = be(b, 8);
      m.side = static_cast<char>(b[8]);
      m.qty = be(b + 9, 4);
      get_stock(b + 13, m.stock);
      m.price = static_cast<std::int64_t>(be(b + 21, 4));
      break;
    case 'E':
      m.ref = be(b, 8);
      m.qty = be(b + 8, 4);
      m.match = be(b + 12, 8);
      break;
    case 'C':
      m.ref = be(b, 8);
      m.qty = be(b + 8, 4);
      m.match = be(b + 12, 8);
      m.printable = b[20] == 'Y';
      m.price = static_cast<std::int64_t>(be(b + 21, 4));
      break;
    case 'X':
      m.ref = be(b, 8);
      m.qty = be(b + 8, 4);
      break;
    case 'D': m.ref = be(b, 8); break;
    case 'U':
      m.ref = be(b, 8);
      m.new_ref = be(b + 8, 8);
      m.qty = be(b + 16, 4);
      m.price = static_cast<std::int64_t>(be(b + 20, 4));
      break;
    case 'P':
      m.ref = be(b, 8);
      m.side = static_cast<char>(b[8]);
      m.qty = be(b + 9, 4);
      get_stock(b + 13, m.stock);
      m.price = static_cast<std::int64_t>(be(b + 21, 4));
      m.match = be(b + 25, 8);
      break;
    case 'Q':
      m.qty = be(b, 8);
      get_stock(b + 8, m.stock);
      m.price = static_cast<std::int64_t>(be(b + 16, 4));
      m.match = be(b + 20, 8);
      m.code = static_cast<char>(b[28]);
      break;
    default: return ParseStatus::Unknown;
  }
  return ParseStatus::Ok;
}

bool append_framed(std::vector<std::uint8_t>& out, const Message& m) {
  const std::size_t n = body_length(m.type);
  if (!n) return false;
  put_be(out, n, 2);
  out.push_back(static_cast<std::uint8_t>(m.type));
  put_be(out, m.locate, 2);
  put_be(out, 0, 2);  // tracking number (unused)
  put_be(out, m.ts_ns, 6);
  switch (m.type) {
    case 'S': out.push_back(static_cast<std::uint8_t>(m.code)); break;
    case 'R':
      put_stock(out, m.stock);
      out.push_back('Q');                // market category
      out.push_back('N');                // financial status
      put_be(out, 100, 4);               // round lot size
      out.push_back('N');                // round lots only
      out.push_back('C');                // issue classification
      put_be(out, ('Z' << 8) | ' ', 2);  // issue sub-type
      out.push_back('P');                // authenticity
      out.push_back(' ');                // short sale threshold
      out.push_back(' ');                // IPO flag
      out.push_back('1');                // LULD tier
      out.push_back('N');                // ETP flag
      put_be(out, 0, 4);                 // ETP leverage
      out.push_back('N');                // inverse
      break;
    case 'H':
      put_stock(out, m.stock);
      out.push_back(static_cast<std::uint8_t>(m.code));
      out.push_back(' ');
      put_be(out, 0x20202020, 4);
      break;
    case 'A':
    case 'F':
      put_be(out, m.ref, 8);
      out.push_back(static_cast<std::uint8_t>(m.side));
      put_be(out, m.qty, 4);
      put_stock(out, m.stock);
      put_be(out, static_cast<std::uint64_t>(m.price), 4);
      if (m.type == 'F') put_be(out, 0x4e534451, 4);  // attribution "NSDQ"
      break;
    case 'E':
      put_be(out, m.ref, 8);
      put_be(out, m.qty, 4);
      put_be(out, m.match, 8);
      break;
    case 'C':
      put_be(out, m.ref, 8);
      put_be(out, m.qty, 4);
      put_be(out, m.match, 8);
      out.push_back(m.printable ? 'Y' : 'N');
      put_be(out, static_cast<std::uint64_t>(m.price), 4);
      break;
    case 'X':
      put_be(out, m.ref, 8);
      put_be(out, m.qty, 4);
      break;
    case 'D': put_be(out, m.ref, 8); break;
    case 'U':
      put_be(out, m.ref, 8);
      put_be(out, m.new_ref, 8);
      put_be(out, m.qty, 4);
      put_be(out, static_cast<std::uint64_t>(m.price), 4);
      break;
    case 'P':
      put_be(out, m.ref, 8);
      out.push_back(static_cast<std::uint8_t>(m.side));
      put_be(out, m.qty, 4);
      put_stock(out, m.stock);
      put_be(out, static_cast<std::uint64_t>(m.price), 4);
      put_be(out, m.match, 8);
      break;
    case 'Q':
      put_be(out, m.qty, 8);
      put_stock(out, m.stock);
      put_be(out, static_cast<std::uint64_t>(m.price), 4);
      put_be(out, m.match, 8);
      out.push_back(static_cast<std::uint8_t>(m.code));
      break;
    default: return false;
  }
  return true;
}

}  // namespace lob::itch
