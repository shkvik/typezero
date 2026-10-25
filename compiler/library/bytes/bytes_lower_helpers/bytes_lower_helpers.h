#pragma once

#include "compiler/lower/lowering/lowering.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ErrorHandling.h"
#include <numeric>

namespace tz {

// Functions that several classes made from codegen_bytes.cpp use.

using Bytes = Type::Bytes;

} // namespace tz
