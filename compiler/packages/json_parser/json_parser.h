#pragma once

#include "compiler/packages/json_value/json_value.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz::pkg {

bool parseJson(llvm::StringRef text, JsonValue &out, size_t &errorOffset, std::string &error);

}
