#include "compiler/semantic/code/classes/dispose_rules/dispose_rules_errors.h"

namespace tz {

std::string ErrorTextDispose::shape() {
  return "'[Symbol.dispose]' is an instance method with no parameters that gives nothing: tz "
         "calls it when the last reference to the object is gone";
}

std::string ErrorTextDispose::noSuper(llvm::StringRef parent) {
  return "this '[Symbol.dispose]' replaces the one of '" + parent.str() +
         "': call 'super[Symbol.dispose]()' in it, or what '" + parent.str() + "' holds leaks";
}

std::string ErrorTextDispose::thisLeaves() {
  return "'this' cannot leave '[Symbol.dispose]': the object is dying; use its fields";
}

std::string ErrorTextDispose::manual() {
  return "tz calls '[Symbol.dispose]' itself when the last reference to the object is gone; a "
         "second call would free what it holds twice; give the class a method of its own to "
         "close it early";
}

std::string ErrorTextDispose::notUsable(llvm::StringRef type) {
  return "'using' takes an object whose class has '[Symbol.dispose]'; '" + type.str() +
         "' has none";
}

std::string ErrorTextDispose::outlives(llvm::StringRef name) {
  return "'" + name.str() +
         "' of 'using' cannot outlive its block: '[Symbol.dispose]' runs when the block ends";
}

}
