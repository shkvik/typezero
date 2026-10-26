#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz::pkg {

bool splitPackageSpecifier(llvm::StringRef specifier, std::string &name, std::string &subpath);

bool splitJsrImport(llvm::StringRef specifier, std::string &package, std::string &source,
                    std::string &subpath);

}
