#pragma once

#include <cstdio>
#include <string>

#ifdef LOB_HAVE_ZLIB
#include <zlib.h>
#endif

namespace tool {

struct FileSource {
  std::FILE* f = nullptr;
#ifdef LOB_HAVE_ZLIB
  gzFile gz = nullptr;
#endif
  bool open(const std::string& path) {
    const bool is_gz = path.size() > 3 && path.compare(path.size() - 3, 3, ".gz") == 0;
    if (is_gz) {
#ifdef LOB_HAVE_ZLIB
      gz = gzopen(path.c_str(), "rb");
      if (gz) gzbuffer(gz, 1 << 20);
      return gz != nullptr;
#else
      std::fprintf(stderr, "built without zlib: cannot read .gz\n");
      return false;
#endif
    }
    f = std::fopen(path.c_str(), "rb");
    return f != nullptr;
  }
  std::size_t read(void* dst, std::size_t n) {
#ifdef LOB_HAVE_ZLIB
    if (gz) {
      const int k = gzread(gz, dst, static_cast<unsigned>(n));
      return k < 0 ? 0 : static_cast<std::size_t>(k);
    }
#endif
    return std::fread(dst, 1, n, f);
  }
  ~FileSource() {
    if (f) std::fclose(f);
#ifdef LOB_HAVE_ZLIB
    if (gz) gzclose(gz);
#endif
  }
};

}  // namespace tool
