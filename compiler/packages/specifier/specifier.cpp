#include "compiler/packages/specifier/specifier.h"

namespace tz::pkg {

bool splitPackageSpecifier(llvm::StringRef specifier, std::string &name, std::string &subpath) {
  llvm::StringRef rest = specifier;
  size_t end = rest.find('/');
  if (rest.starts_with("@")) {
    if (end == llvm::StringRef::npos) {
      return false;
    }
    end = rest.find('/', end + 1);
  }
  name = rest.substr(0, end).str();
  subpath = end == llvm::StringRef::npos ? "." : "./" + rest.drop_front(end + 1).str();

  return !name.empty();
}

bool splitJsrImport(llvm::StringRef specifier, std::string &package, std::string &source,
                    std::string &subpath) {
  llvm::StringRef rest = specifier;
  if (!rest.consume_front("jsr:")) {
    return false;
  }
  rest.consume_front("/");
  if (!rest.starts_with("@")) {
    return false;
  }
  size_t slash = rest.find('/');
  size_t end = slash == llvm::StringRef::npos ? slash : rest.find('/', slash + 1);
  llvm::StringRef full = rest.substr(0, end); // "@std/path@^1"
  package = full.substr(0, full.find('@', 1)).str();
  source = "jsr:" + full.str();
  subpath = end == llvm::StringRef::npos ? "" : rest.drop_front(end).str();

  return true;
}

} // namespace tz::pkg
