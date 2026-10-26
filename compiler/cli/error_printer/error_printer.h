#pragma once

#include "compiler/program/errors/errors.h"
#include "llvm/Support/SourceMgr.h"

namespace tz {

class ErrorPrinter {
public:
  explicit ErrorPrinter(const llvm::SourceMgr &sourceMgr);

  void print(const Errors &errors) const;

private:
  const llvm::SourceMgr &sourceMgr_;
};

}
