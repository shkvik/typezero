#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class PredicateCheckerNotAssertingErrorCode {
  ASSERTS_PREDICATE,
};

struct PredicateCheckerNotAssertingError {
  PredicateCheckerNotAssertingErrorCode code;
  std::string message;
};

class ErrorTextAsserts {
public:
  static std::string notAllowed(llvm::StringRef shown, bool typed, llvm::StringRef call);
};

enum class PredicateCheckerPredicateArgErrorCode {
  THIS_OUTSIDE_METHOD,
  NOT_A_PARAMETER,
};

struct PredicateCheckerPredicateArgError {
  PredicateCheckerPredicateArgErrorCode code;
  std::string message;
};

class ErrorTextPredicateArg {
public:
  static std::string thisOutside();
  static std::string notParam(llvm::StringRef name);
};

enum class PredicateCheckerNotePredicateExitErrorCode {
  NOT_NARROWED,
};

struct PredicateCheckerNotePredicateExitError {
  PredicateCheckerNotePredicateExitErrorCode code;
  std::string message;
};

class ErrorTextPredicateExit {
public:
  static std::string mayLie(llvm::StringRef function, llvm::StringRef var, llvm::StringRef type,
                            llvm::StringRef shown);
};

}
