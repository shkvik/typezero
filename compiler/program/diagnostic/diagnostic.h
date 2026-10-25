#pragma once

#include "llvm/Support/SMLoc.h"
#include "llvm/Support/SourceMgr.h"
#include <string>

namespace tz {

struct Diagnostic {
  enum class Form {
    Located,
    Tool,
    Raw,
  };

  llvm::SourceMgr::DiagKind kind = llvm::SourceMgr::DK_Error;
  Form form = Form::Located;
  llvm::SMLoc loc;
  std::string message;
};

} // namespace tz
