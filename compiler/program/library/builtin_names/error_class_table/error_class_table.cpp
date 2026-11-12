#include "compiler/program/library/builtin_names/error_class_table/error_class_table.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

namespace {

const llvm::StringLiteral kErrorClasses[] = {"Error",       "TypeError",      "RangeError",
                                             "SyntaxError", "ReferenceError", "EvalError",
                                             "URIError",    "AggregateError"};

}

bool ErrorClassTable::isErrorClass(llvm::StringRef name) {
  return llvm::is_contained(kErrorClasses, name);
}

}
