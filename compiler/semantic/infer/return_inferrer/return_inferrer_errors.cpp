#include "compiler/semantic/infer/return_inferrer/return_inferrer_errors.h"
#include "compiler/semantic/bodies/functions/function_checker/function_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextEarlyCall::importBack(llvm::StringRef function) {
  auto quoted = ErrorTextFunctionName::quoted(function);

  return (llvm::Twine("cannot tell what ") + quoted +
          " returns: it uses a file that imports it back and calls it while loading; declare "
          "its return type, e.g. '): int'")
      .str();
}

std::string ErrorTextCycleType::differ(llvm::StringRef function) {
  auto quoted = ErrorTextFunctionName::quoted(function);

  return (llvm::Twine(quoted) +
          " returns values of different types; a function returns values of one type")
      .str();
}

std::string ErrorTextCycleType::selfOnly(llvm::StringRef function) {
  auto quoted = ErrorTextFunctionName::quoted(function);

  return (llvm::Twine("cannot tell what ") + quoted +
          " returns: every value it returns comes from a call to itself; declare its return "
          "type, e.g. '): int'")
      .str();
}

std::string ErrorTextCycleType::calledBack(llvm::StringRef function) {
  auto quoted = ErrorTextFunctionName::quoted(function);

  return (llvm::Twine("cannot tell what ") + quoted +
          " returns: every value it returns comes from the functions that call it back; "
          "declare its return type, e.g. '): int'")
      .str();
}

std::string ErrorTextCommonReturn::differ(llvm::StringRef function, llvm::StringRef here,
                                          llvm::StringRef elsewhere) {
  auto quoted = ErrorTextFunctionName::quoted(function);

  return (llvm::Twine(quoted) + " returns '" + here + "' here and '" + elsewhere +
          "' elsewhere; a function returns values of one type")
      .str();
}

std::string ErrorTextDefinedReturn::undefined(llvm::StringRef function) {
  auto quoted = ErrorTextFunctionName::quoted(function);

  return (llvm::Twine(quoted) +
          " returns only undefined; declare the return type, e.g. '): int | undefined'")
      .str();
}

}
