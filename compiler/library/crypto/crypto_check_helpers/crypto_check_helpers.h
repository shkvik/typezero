#pragma once

#include "compiler/check/sema/sema.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"

namespace tz {

// Functions that several classes made from sema_crypto.cpp use.

using Crypto = Type::Crypto;

} // namespace tz
