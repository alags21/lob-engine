// Placeholder CLI; replay/export tooling arrives in later stages.
#include <cstdio>

#include "lob/version.hpp"

int main() {
  std::printf("lob-engine %s\n", lob::version());
  return 0;
}
