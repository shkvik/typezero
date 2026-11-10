#pragma once

#include "compiler/program/diagnostic/diagnostic.h"
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

  llvm::json::Value build(const std::vector<Diagnostic> &found,
                          const std::vector<Module *> &modules);

  static llvm::json::Object range(const SourcePlace &place);

private:
  SourcePlaces places_;
  std::string entry;
};

}
