#include "compiler/pipeline/check_program/infer/return_inferrer/return_inferrer_errors.h"
#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/functions/function_checker/function_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextEarlyCall::importBack(llvm::StringRef function) {
  std::string quoted = ErrorTextFunctionName::quoted(function);

  return (llvm::Twine("cannot tell what ") + quoted +
          " returns: it uses a file that imports it back and calls it while loading; declare "
          "its return type, e.g. '): int'")
      .str();
}

std::string ErrorTextCycleType::differ(llvm::StringRef function) {
  std::string quoted = ErrorTextFunctionName::quoted(function);

  return (llvm::Twine(quoted) +
          " returns values of different types; a function returns values of one type")
      .str();
}

std::string ErrorTextCycleType::selfOnly(llvm::StringRef function) {
  std::string quoted = ErrorTextFunctionName::quoted(function);

  return (llvm::Twine("cannot tell what ") + quoted +
          " returns: every value it returns comes from a call to itself; declare its return "
          "type, e.g. '): int'")
      .str();
}

std::string ErrorTextCycleType::calledBack(llvm::StringRef function) {
  std::string quoted = ErrorTextFunctionName::quoted(function);

  return (llvm::Twine("cannot tell what ") + quoted +
          " returns: every value it returns comes from the functions that call it back; "
          "declare its return type, e.g. '): int'")
      .str();
}

std::string ErrorTextCommonReturn::differ(llvm::StringRef function, llvm::StringRef here,
                                          llvm::StringRef elsewhere) {
  std::string quoted = ErrorTextFunctionName::quoted(function);

  return (llvm::Twine(quoted) + " returns '" + here + "' here and '" + elsewhere +
          "' elsewhere; a function returns values of one type")
      .str();
}

std::string ErrorTextDefinedReturn::undefined(llvm::StringRef function) {
  std::string quoted = ErrorTextFunctionName::quoted(function);

  return (llvm::Twine(quoted) +
          " returns only undefined; declare the return type, e.g. '): int | undefined'")
      .str();
}

std::string ErrorTextDefaultType::unknown(llvm::StringRef param) {
  return (llvm::Twine("cannot tell the type of '") + param +
          "' from its default value; add one, e.g. '" + param + ": int | undefined = undefined'")
      .str();
}

}
