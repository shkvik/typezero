#include "compiler/check/expr/literal_checker/literal_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextIntLit::tooBig(llvm::StringRef type) {
  return (llvm::Twine("integer literal does not fit in type '") + type + "'").str();
}

}
