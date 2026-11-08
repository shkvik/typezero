#include "compiler/semantic/library/weakref/weak_ref_checker/weak_ref_checker_errors.h"

namespace tz {

std::string ErrorTextWeakRef::noTarget() {
  return "'WeakRef' needs the type of its object, e.g. 'WeakRef<Node>'";
}

std::string ErrorTextWeakRef::oneObject() {
  return "'new WeakRef' takes the object it refers to, e.g. 'new WeakRef(node)'";
}

std::string ErrorTextWeakRef::typeArgs() {
  return "'WeakRef' takes one type argument, the type of its object";
}

std::string ErrorTextWeakRef::noMethod(llvm::StringRef name, llvm::StringRef type) {
  return "property '" + name.str() + "' does not exist on type '" + type.str() +
         "'; it has 'deref()'";
}

std::string ErrorTextWeakTarget::notYet(llvm::StringRef type) {
  return "a WeakRef of '" + type.str() +
         "' is not supported yet; it refers to objects of the program's types";
}

std::string ErrorTextWeakTarget::notObject(llvm::StringRef type, bool optional) {
  std::string message =
      "a WeakRef refers to an object, not to a value of type '" + type.str() + "'";
  if (optional) {
    message += "; make it of the object and check for undefined before";
  } else {
    message += "; hold the value itself";
  }

  return message;
}

}
