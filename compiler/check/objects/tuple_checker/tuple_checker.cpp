#include "compiler/check/objects/tuple_checker/tuple_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/objects/object_check_helpers/object_check_helpers.h"
#include "compiler/check/types/literal_types/literal_types.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

TupleChecker::TupleChecker(Checker &checker) : checker_(checker) {}

Expr *TupleChecker::checkTupleJoin(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  if (checker_.callChecker().rejectSpread(call) ||
      !checker_.argumentChecker().checkArity(call, member->loc(), 0, 1)) {
    return call;
  }
  if (!call->args.empty()) {
    call->args[0] = checker_.argumentChecker().checkArg(call->args[0], Type::string());
  }
  const Type *tuple = member->object->type;
  for (size_t i = 0; i < tuple->fields().size(); ++i) {
    const Type *element = tuple->fields()[i].type->nonOptional();
    if (element->isNumeric() || element->isDecimal() || element->isBool() || element->isString() ||
        element->isUndefined()) {
      continue;
    }
    checker_.diags_.error(
        member->loc(), "'join' turns each element into a string, and element " + std::to_string(i) +
                           " of '" + tuple->name() + "' is '" + element->name() +
                           "', which cannot be one; build the text from the elements");

    return call;
  }
  call->builtin = Builtin::TupleJoin;
  call->type = Type::string();

  return call;
}

const Type *TupleChecker::tupleElement(const Type *tuple) {
  const Type *element = nullptr;
  for (const Field &f : tuple->fields()) {
    element = element ? LiteralTypes::unionUnify(element, f.type) : f.type;
    if (!element || element->isError()) {
      return nullptr;
    }
  }

  return element;
}

Expr *TupleChecker::checkTupleMember(MemberExpr *member) {
  const Type *type = member->object->type;
  if (member->name != "length") {
    checker_.diags_.error(member->loc(), "property '" + member->name +
                                             "' does not exist on type '" + type->name() + "'");
    member->type = Type::error();
    return member;
  }
  auto *length = checker_.ctx_.make<IntLitExpr>(member->loc(), type->fields().size());
  length->type = Type::intTy();
  if (isa<NameExpr>(member->object)) {
    return length;
  }
  auto *seq =
      checker_.ctx_.make<SequenceExpr>(member->loc(), std::vector<Expr *>{member->object, length});
  seq->type = Type::intTy();

  return seq;
}

Expr *TupleChecker::checkTupleMethod(CallExpr *call, MemberExpr *member) {
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  std::string text = "t";
  if (auto *object = dyn_cast<NameExpr>(unnarrowed(member->object))) {
    text = object->name.str();
  }
  static const char *const resizing[] = {"push", "pop", "shift", "unshift", "splice"};
  if (name == "join") {
    return checkTupleJoin(call, member);
  }
  if (llvm::is_contained(resizing, name)) {
    checker_.diags_.error(
        member->loc(), "a tuple has a fixed length, and '" + name +
                           "' would change it; to change the length, copy it into an array: '[..." +
                           text + "]'");
  } else if (tupleElement(type)) {
    checker_.diags_.error(
        member->loc(),
        "'" + name + "' is not supported on tuples; copy the tuple into an array first: '[..." +
            text + "]." + name + "(...)'");
  } else {
    checker_.diags_.error(
        member->loc(),
        "'" + name + "' is not supported on tuples; destructure it instead, e.g. 'const [a, b] = " +
            text + "'");
  }
  for (Expr *&arg : call->args) {
    arg = checker_.exprChecker().checkExpr(arg);
  }
  call->type = Type::error();

  return call;
}

Expr *TupleChecker::checkTupleLit(ArrayLitExpr *lit, const Type *tuple) {
  llvm::ArrayRef<Field> fields = tuple->fields();
  lit->type = Type::error();
  auto spread =
      llvm::find_if(lit->elements, [](const ArrayLitExpr::Element &e) { return e.spread; });
  if (spread != lit->elements.end() || lit->elements.size() != fields.size()) {
    if (spread != lit->elements.end()) {
      checker_.diags_.error(spread->expr->loc(), "a tuple cannot take spread values: type '" +
                                                     tuple->name() +
                                                     "' has a fixed length; write each element");
    } else {
      checker_.diags_.error(lit->loc(), "type '" + tuple->name() + "' has " +
                                            std::to_string(fields.size()) +
                                            " elements, and this literal has " +
                                            std::to_string(lit->elements.size()));
    }
    for (ArrayLitExpr::Element &e : lit->elements) {
      e.expr = checker_.exprChecker().checkExpr(e.expr);
    }
    return lit;
  }
  for (size_t i = 0; i < fields.size(); ++i) {
    Expr *&e = lit->elements[i].expr;
    e = checker_.typeRelations().coerce(checker_.exprChecker().checkExpr(e, fields[i].type),
                                        fields[i].type, "element " + std::to_string(i));
  }
  lit->type = tuple;

  return lit;
}

}
