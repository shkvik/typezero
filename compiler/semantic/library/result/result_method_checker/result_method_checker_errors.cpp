#include "compiler/semantic/library/result/result_method_checker/result_method_checker_errors.h"
#include "compiler/semantic/library/result/result_checker/result_checker_errors.h"

namespace tz {

std::string ErrorTextResultMethod::noMember(llvm::StringRef name, llvm::StringRef type) {
  return "property '" + name.str() + "' does not exist on type '" + type.str() + "'";
}

std::string ErrorTextResultMethod::unwrapOr(llvm::StringRef value, llvm::StringRef other) {
  return "'unwrapOr' gives '" + value.str() + "' for an Ok and '" + other.str() +
         "' otherwise, and no type holds both; say what each gives with 'match'";
}

std::string ErrorTextResultMethod::function(llvm::StringRef name, llvm::StringRef type) {
  return "'" + name.str() + "' needs a function, found '" + type.str() + "'";
}

std::string ErrorTextResultMethod::match(llvm::StringRef a, llvm::StringRef b) {
  return "the functions given to 'match' return '" + a.str() + "' and '" + b.str() +
         "', and no type holds both";
}

std::string ErrorTextResultMethod::result(llvm::StringRef name, llvm::StringRef type,
                                          bool andThen) {
  return "the function given to '" + name.str() +
         "' must return a Result, 'ok(...)' or 'err(...)', found '" + type.str() + "'" +
         (andThen ? "; to change the value, use 'map'" : "");
}

std::string ErrorTextResultMethod::errors(const Type *a, llvm::StringRef name, const Type *b) {
  return "the Result has errors of '" + a->name().str() + "' and the function given to '" +
         name.str() + "' returns errors of '" + b->name().str() + "', which no union holds" +
         ErrorTextResultUnion::why(a, b) + "; convert one with 'mapErr'";
}

std::string ErrorTextResultMethod::values(const Type *value, const Type *other) {
  return "the Result has values of '" + value->name().str() +
         "' and the function given to 'orElse' returns values of '" + other->name().str() +
         "', which no union holds" + ErrorTextResultUnion::why(value, other);
}

}
