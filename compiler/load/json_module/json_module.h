#pragma once

#include "llvm/ADT/StringRef.h"
#include <cstddef>
#include <string>

namespace tz {

inline constexpr llvm::StringLiteral kJsonExport = "json";

bool jsonModuleText(llvm::StringRef json, llvm::StringRef exported, std::string &out,
                    size_t &errorOffset, std::string &error);

}
