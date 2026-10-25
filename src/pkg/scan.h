#pragma once

#include "lex/token.h"

#include "llvm/ADT/StringRef.h"

#include <string>
#include <vector>

namespace tz::pkg {

// The tokens of a TypeScript file, without a parser: tz's lexer, with a
// regex literal wherever the token before a '/' lets an expression start.
// Characters tz does not know (a decorator's '@') are skipped. For tools that
// read code tz may not accept yet: the dependencies of a package, tz port.
std::vector<Token> tokenize(llvm::StringRef text);

// The value of a string literal token whose text has no escapes, or of one
// with only simple escapes.
std::string stringValue(const Token &tok);

struct ImportRef {
  std::string specifier;
  size_t offset; // of the string literal
};
// The modules a file imports or re-exports from, in order: `import ... from
// "x"`, `import "x"`, `export ... from "x"`.
std::vector<ImportRef> importSpecifiers(const std::vector<Token> &tokens, llvm::StringRef text);

// The .ts file a relative import of `fromFile` names, as the loader finds it
// ("./a" is a.ts or a/index.ts, "./a.js" is a.ts); "" if there is none.
std::string resolveRelative(llvm::StringRef fromFile, llvm::StringRef specifier);

// The files reachable from `entries` through relative imports, and the
// specifiers of everything else they import, without duplicates.
void reachableFiles(const std::vector<std::string> &entries, std::vector<std::string> &files,
                    std::vector<std::string> &external);

} // namespace tz::pkg
