#include "compiler/semantic/library/result/result_checker/result_checker_errors.h"
#include "compiler/program/types/union_slots/union_slots.h"

namespace tz {

std::string ErrorTextOkErr::typeArgs(llvm::StringRef name) {
  return "'" + name.str() + "' takes two type arguments: '" + name.str() + "<T, E>(...)'";
}

std::string ErrorTextOkErr::voidValue(llvm::StringRef name) {
  return "an expression of type 'void' has no value to keep; write '" + name.str() + "()'";
}

std::string ErrorTextCombine::noStatic(llvm::StringRef name, llvm::StringRef method) {
  return "'" + name.str() + "." + method.str() +
         "' does not exist; Result has 'combine' and "
         "'combineWithAllErrors'";
}

std::string ErrorTextCombine::holds(llvm::StringRef what, bool values, llvm::StringRef type) {
  return "'" + what.str() + "' needs Results that hold " + (values ? "errors" : "values") +
         ", found '" + type.str() + "'; check them one by one, e.g. with 'every(r => r.isOk())'";
}

std::string ErrorTextCombine::notResult(llvm::StringRef what, llvm::StringRef type) {
  return "'" + what.str() + "' takes Results, found '" + type.str() + "'";
}

std::string ErrorTextCombine::noUnion(llvm::StringRef what, const Type *before, const Type *error) {
  std::string why = ErrorTextResultUnion::why(before, error);

  return "the Results given to '" + what.str() + "' have errors that no union holds" + why +
         "; convert them with 'mapErr'";
}

std::string ErrorTextCombine::notArray(llvm::StringRef what, llvm::StringRef type) {
  return "'" + what.str() + "' takes an array of Results, found '" + type.str() + "'";
}

std::string ErrorTextResultMember::value(llvm::StringRef r) {
  return "'value' is there only when the Result is an Ok: check it first, 'if (" + r.str() +
         ".isOk())', or use 'match' or 'unwrapOr'";
}

std::string ErrorTextResultMember::error(llvm::StringRef r) {
  return "'error' is there only when the Result is an Err: check it first, 'if (" + r.str() +
         ".isErr())', or use 'match'";
}

std::string ErrorTextResultMember::method(llvm::StringRef name, llvm::StringRef r) {
  return "'" + name.str() + "' is a method of Result; call it: '" + r.str() + "." + name.str() +
         "(...)'";
}

std::string ErrorTextResultMember::noMember(llvm::StringRef name, llvm::StringRef type) {
  return "property '" + name.str() + "' does not exist on type '" + type.str() + "'";
}

std::string ErrorTextUnusedResult::results() {
  return "the Results are not used, so errors in them would go unchecked; "
         "handle them, e.g. with 'Result.combine(...)', or keep them in a "
         "variable";
}

std::string ErrorTextUnusedResult::callback() {
  return "the function returns a Result that its caller drops, so an error in it "
         "would go unchecked; handle it here, e.g. with '.match(...)'";
}

std::string ErrorTextUnusedResult::result() {
  return "the Result is not used, so an error in it would go unchecked; handle "
         "it, e.g. with '.match(...)', or keep it in a variable and check it";
}

std::string ErrorTextResultUnion::why(const Type *a, const Type *b) {
  if (a->isVoid() || b->isVoid()) {
    return ": one of them is nothing, 'ok()'";
  }
  std::string problem = UnionSlots::unionProblem(Type::unionOf({a, b}));

  return problem.empty() ? "" : ": " + problem;
}

}
