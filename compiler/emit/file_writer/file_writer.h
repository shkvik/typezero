#pragma once

#include "compiler/program/errors/errors.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

namespace tz {

class FileWriter {
public:
  Errors write(llvm::StringRef path,
               llvm::function_ref<void(llvm::raw_pwrite_stream &)> contents) const;
};

}
