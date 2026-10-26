#pragma once

#include "compiler/load/token/token.h"
#include "compiler/packages/import_ref/import_ref.h"
#include "llvm/ADT/StringRef.h"
#include <string>
#include <vector>

namespace tz::pkg {

std::vector<Token> tokenize(llvm::StringRef text);

std::string stringValue(const Token &tok);

std::vector<ImportRef> importSpecifiers(const std::vector<Token> &tokens, llvm::StringRef text);

enum class Sources { Tz, TypeScript };

std::string resolveRelative(llvm::StringRef fromFile, llvm::StringRef specifier, Sources sources);

void reachableFiles(const std::vector<std::string> &entries, std::vector<std::string> &files,
                    std::vector<std::string> &external, Sources sources);

}
