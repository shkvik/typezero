#include "compiler/packages/version/version.h"
#include "compiler/packages/partial/partial.h"
#include "llvm/ADT/StringExtras.h"

namespace tz::pkg {

namespace {

bool isNumeric(llvm::StringRef id) { return !id.empty() && llvm::all_of(id, llvm::isDigit); }

}

bool Version::parse(llvm::StringRef text, Version &out) {
  Partial p;
  if (!parsePartial(text.trim(), p) || p.count != 3) {
    return false;
  }
  out = p.version();

  return true;
}

int Version::compare(const Version &o) const {
  if (major != o.major) {
    return major < o.major ? -1 : 1;
  }
  if (minor != o.minor) {
    return minor < o.minor ? -1 : 1;
  }
  if (patch != o.patch) {
    return patch < o.patch ? -1 : 1;
  }
  if (pre.empty() || o.pre.empty()) {
    return pre.empty() == o.pre.empty() ? 0 : (pre.empty() ? 1 : -1);
  }
  for (size_t i = 0; i < pre.size() && i < o.pre.size(); ++i) {
    const std::string &a = pre[i], &b = o.pre[i];
    if (a == b) {
      continue;
    }
    bool an = isNumeric(a), bn = isNumeric(b);
    if (an && bn) {
      if (a.size() != b.size()) {
        return a.size() < b.size() ? -1 : 1;
      }
      return a < b ? -1 : 1;
    }
    if (an != bn) {
      return an ? -1 : 1;
    }

    return a < b ? -1 : 1;
  }
  if (pre.size() == o.pre.size()) {
    return 0;
  }

  return pre.size() < o.pre.size() ? -1 : 1;
}

std::string Version::str() const {
  std::string s = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
  for (size_t i = 0; i < pre.size(); ++i) {
    s += (i ? "." : "-") + pre[i];
  }

  return s;
}

}
