// Replays an op log (docs/EVENT_FORMAT.md) through lob::OrderBook and prints every emitted event,
// one per line, to stdout. Deterministic: same log in, byte-identical output.
//   lob_replay LOGFILE
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

#include "lob/event_log.hpp"
#include "lob/order_book.hpp"

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: lob_replay LOGFILE\n");
    return 2;
  }
  std::ifstream in(argv[1]);
  if (!in) {
    std::fprintf(stderr, "cannot open %s\n", argv[1]);
    return 2;
  }
  lob::OrderBook book([](const lob::Event& e) { std::cout << lob::to_string(e) << '\n'; });
  std::string line;
  std::size_t lineno = 0;
  while (std::getline(in, line)) {
    ++lineno;
    const auto first = line.find_first_not_of(" \t\r");
    if (first == std::string::npos || line[first] == '#') continue;
    const auto op = lob::parse_op(line);
    if (!op) {
      std::fprintf(stderr, "%s:%zu: malformed op: %s\n", argv[1], lineno, line.c_str());
      return 1;
    }
    lob::apply(book, *op);
  }
  return 0;
}
