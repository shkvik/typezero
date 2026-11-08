#pragma once

#include "compiler/program/ast/ast.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class ChunkTypesHookParamsErrorCode {
  NO_LIBRARY_HOOK,
  PARAMS_NOT_TYPED,
  OTHER_PARAMETERS,
};

struct ChunkTypesHookParamsError {
  ChunkTypesHookParamsErrorCode code;
  std::string message;
};

class ErrorTextStreamHook {
public:
  static std::string params(const FuncSymbol *hook);
  static std::string other(llvm::StringRef name, llvm::StringRef params, bool chunks);
};

}
