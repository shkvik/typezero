#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <string>
#include <utility>
#include <vector>

namespace tz::pkg {

struct Manifest;
struct PortContext;

class PortCommand {
public:
  int run(llvm::ArrayRef<const char *> args) const;

private:
  static bool readFile(llvm::StringRef file, std::string &text);
  static bool writeFile(llvm::StringRef file, llvm::StringRef text);
  static void findDefaultNames(const std::vector<std::string> &files, llvm::StringRef packageDir,
                               PortContext &ctx);
  static std::vector<std::pair<std::string, std::string>> packageEntries(const std::string &dir,
                                                                         const Manifest &manifest,
                                                                         std::string &name,
                                                                         std::string &version);
  static bool fetchSource(llvm::StringRef arg, std::string &dir, std::string &name,
                          std::string &version,
                          std::vector<std::pair<std::string, std::string>> &exports, bool &local);
  static std::string copyName(llvm::StringRef file);
  static std::string displayPath(llvm::StringRef base, llvm::StringRef relative);
};

}
