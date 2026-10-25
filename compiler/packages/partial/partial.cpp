#include "compiler/packages/partial/partial.h"
#include "llvm/ADT/StringExtras.h"

namespace tz::pkg {

namespace {

bool parseNumber(llvm::StringRef text, uint64_t &out) {
  if (text.empty() || text.size() > 15 || !llvm::all_of(text, llvm::isDigit)) {
    return false;
  }

  return !text.getAsInteger(10, out);
}

bool parsePre(llvm::StringRef text, std::vector<std::string> &out) {
  llvm::SmallVector<llvm::StringRef, 4> ids;
  text.split(ids, '.');
  for (llvm::StringRef id : ids) {
    if (id.empty() || !llvm::all_of(id, [](char c) { return llvm::isAlnum(c) || c == '-'; })) {
      return false;
    }
    out.push_back(id.str());
  }

  return true;
}

} // namespace

bool parsePartial(llvm::StringRef text, Partial &out) {
  text.consume_front("=");
  text.consume_front("v");
  text = text.split('+').first;
  auto [core, pre] = text.split('-');
  if (core.size() != text.size() && !parsePre(pre, out.pre)) {
    return false;
  }
  llvm::SmallVector<llvm::StringRef, 3> parts;
  core.split(parts, '.');
  if (parts.size() > 3) {
    return false;
  }
  bool wild = false;
  for (llvm::StringRef part : parts) {
    if (part == "x" || part == "X" || part == "*") {
      wild = true;
      continue;
    }
    if (wild || !parseNumber(part, out.parts[out.count])) {
      return false;
    }
    ++out.count;
  }

  // A prerelease belongs to a full version: "1.2-beta" means nothing.
  return out.pre.empty() || out.count == 3;
}

} // namespace tz::pkg
