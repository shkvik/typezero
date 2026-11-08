#include "compiler/semantic/bodies/functions/nested_functions/nested_functions_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextUseOrder::early(llvm::StringRef function, llvm::StringRef variable) {
  return (llvm::Twine("'") + function + "' uses '" + variable +
          "' before it is declared: JavaScript would throw a ReferenceError when it "
          "runs here; move the declaration of '" +
          variable + "' up")
      .str();
}

std::string ErrorTextUseOrder::declared(llvm::StringRef variable) {
  return (llvm::Twine("'") + variable + "' is declared here").str();
}

}
