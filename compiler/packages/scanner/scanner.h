#pragma once

#include "compiler/load/token/token.h"
#include "compiler/packages/import_ref/import_ref.h"
#include "llvm/ADT/StringRef.h"
#include <string>
#include <vector>

namespace tz::pkg {

enum class Sources { Tz, TypeScript };

class Scanner {
public:
  static std::string stringValue(const Token &tok);
  static std::vector<Token> tokenize(llvm::StringRef text);
  static void reachableFiles(const std::vector<std::string> &entries,
                             std::vector<std::string> &files, std::vector<std::string> &external,
                             Sources sources);
  static std::vector<ImportRef> importSpecifiers(const std::vector<Token> &tokens,
                                                 llvm::StringRef text);
  static std::string resolveRelative(llvm::StringRef fromFile, llvm::StringRef specifier,
                                     Sources sources);

private:
  static bool regexMayFollow(const Token *prev);
};

}
