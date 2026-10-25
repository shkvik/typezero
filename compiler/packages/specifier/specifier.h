#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz::pkg {

// "@scope/name" or "name", as an import names a package; the rest of the
// specifier is the path inside it ("./mini" for "zod/mini", "." for "zod").
bool splitPackageSpecifier(llvm::StringRef specifier, std::string &name, std::string &subpath);

// An import "jsr:@std/path@^1/join", as JSR writes the imports of the
// packages it publishes: `package` "@std/path", `source` "jsr:@std/path@^1"
// (what tz.lock records), `subpath` "/join" ("" for none). False if it is not
// one.
bool splitJsrImport(llvm::StringRef specifier, std::string &package, std::string &source,
                    std::string &subpath);

} // namespace tz::pkg
