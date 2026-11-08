#pragma once

#include "compiler/load/source_overlay/source_overlay.h"
#include "compiler/lsp/lsp_check/lsp_check_errors.h"
#include "compiler/result/result/result.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"
#include <set>
#include <string>

namespace tz {

class LspCheck {
public:
  using ResultResponseFile = Result<llvm::StringRef, LspCheckRunError>;

  explicit LspCheck(const char *executable);

  ResultResponseFile run(llvm::StringRef requestPath, llvm::StringRef responsePath) const;

private:
  using ResultRequestValue = Result<llvm::json::Value, LspCheckReadRequestError>;

  static SourceOverlay overlayOf(const llvm::json::Object &request);
  static ResultRequestValue readRequest(llvm::StringRef requestPath);
  static std::set<std::string> detailedFiles(const llvm::json::Object &request);

  const char *argv0;
};

}
