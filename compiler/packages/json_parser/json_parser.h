#pragma once

#include "compiler/packages/json_value/json_value.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz::pkg {

// Parses JSON, also with the comments and trailing commas that tsconfig.json
// and deno.jsonc allow. On failure, `errorOffset` and `error` say where and
// what.
bool parseJson(llvm::StringRef text, JsonValue &out, size_t &errorOffset, std::string &error);

} // namespace tz::pkg
