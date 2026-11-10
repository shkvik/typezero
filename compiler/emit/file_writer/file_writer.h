#pragma once

#include "compiler/program/diagnostic/diagnostic.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"
#include <vector>

namespace tz {

class FileWriter {
public:
  std::vector<Diagnostic> write(llvm::StringRef path,
                                llvm::function_ref<void(llvm::raw_pwrite_stream &)> contents) const;
};

}
