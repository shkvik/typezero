#include "compiler/semantic/library_v2/result/library_result_registry_errors.h"
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
  auto why = ErrorTextResultUnion::why(before, error);

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
  auto problem = UnionSlots::unionProblem(Type::unionOf({a, b}));

  return problem.empty() ? "" : ": " + problem;
}

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

std::string ErrorTextResultType::typeArgs(llvm::StringRef name) {
  return "'" + name.str() + "' takes two type arguments, the value and the error, e.g. '" +
         name.str() + "<int, string>'";
}

}
