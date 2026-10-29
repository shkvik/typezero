#pragma once

#include "compiler/program/errors/errors.h"
#include "compiler/program/source_places/source_places.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/SourceMgr.h"
#include <string>
#include <vector>

namespace tz {

struct Module;

class CheckReport {
public:
  CheckReport(const llvm::SourceMgr &sourceMgr, llvm::StringRef entry);

  llvm::json::Value build(const Errors &errors, const std::vector<Module *> &modules) const;

  static llvm::json::Object range(const SourcePlace &place);

private:
  SourcePlaces places_;
  std::string entry_;
};

}
