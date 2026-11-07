#include "compiler/check/stmt/stmt_checker/stmt_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextFunctionStmt::nested() {
  return "a function can be declared only directly in a block; in a "
         "'switch', declare it before the 'switch'";
}

std::string ErrorTextBody::declared() {
  return "a declaration cannot be the whole body here; wrap it in '{ }'";
}

}
