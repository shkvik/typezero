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

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

TupleChecker::TupleChecker(Checker &owner)
    : checker(owner) {}

Expr *TupleChecker::arrayCopy(Expr *tuple) {
  const Type *type = tuple->type;
  if (!type || !type->isTuple() || type->tupleRest() || type->fields().empty() ||
      type->tupleMin() != type->fields().size()) {
    return nullptr;
  }
  const Type *element = type->fields().front().type;
  for (const Field &f : type->fields()) {
    if (f.type != element) {
      return nullptr;
    }
  }
  auto *copy = this->checker.ctx_.make<ArrayLitExpr>(tuple->loc());
  copy->elements.push_back({tuple, true});
  copy->type = Type::array(element);

  return copy;
}

Expr *TupleChecker::checkTupleJoin(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  bool rejected = this->checker.callChecker().rejectSpread(call);
  ArgumentChecker::ResultArityMatches arity =
      rejected ? ok(call) : this->checker.argumentChecker().checkArity(call, 0, 1);
  if (arity.isErr()) {
    this->checker.diags_.error(member->loc(), arity.error().message);
  }
  if (rejected || arity.isErr()) {
    return call;
  }
  if (!call->args.empty()) {
    call->args[0] = this->checker.argumentChecker().checkArg(call->args[0], Type::string());
  }
  const Type *tuple = member->object->type;
  for (size_t i = 0; i < tuple->fields().size(); ++i) {
    const Type *element = tuple->fields()[i].type->nonOptional();
    if (element->isNumeric() || element->isDecimal() || element->isBool() || element->isString() ||
        element->isUndefined()) {
      continue;
    }
    this->checker.diags_.error(
        member->loc(), "'join' turns each element into a string, and element " + std::to_string(i) +
                           " of '" + tuple->name() + "' is '" + element->name() +
                           "', which cannot be one; build the text from the elements");

    return call;
  }
  call->target = BuiltinCall{.builtin = Builtin::TupleJoin};
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
    this->checker.diags_.error(member->loc(), "property '" + member->name +
                                                  "' does not exist on type '" + type->name() +
                                                  "'");
    member->type = Type::error();
    return member;
  }
  auto *length = this->checker.ctx_.make<IntLitExpr>(member->loc(), type->fields().size());
  length->type = Type::intTy();
  if (isa<NameExpr>(member->object)) {
    return length;
  }
  auto *seq = this->checker.ctx_.make<SequenceExpr>(member->loc(),
                                                    std::vector<Expr *>{member->object, length});
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
    return this->checkTupleJoin(call, member);
  }
  if (llvm::is_contained(resizing, name)) {
    this->checker.diags_.error(
        member->loc(), "a tuple has a fixed length, and '" + name +
                           "' would change it; to change the length, copy it into an array: '[..." +
                           text + "]'");
  } else if (TupleChecker::tupleElement(type)) {
    this->checker.diags_.error(
        member->loc(),
        "'" + name + "' is not supported on tuples; copy the tuple into an array first: '[..." +
            text + "]." + name + "(...)'");
  } else {
    this->checker.diags_.error(
        member->loc(),
        "'" + name + "' is not supported on tuples; destructure it instead, e.g. 'const [a, b] = " +
            text + "'");
  }
  for (Expr *&arg : call->args) {
    arg = this->checker.exprChecker().checkExpr(arg);
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
      this->checker.diags_.error(spread->expr->loc(),
                                 "a tuple cannot take spread values: type '" + tuple->name() +
                                     "' has a fixed length; write each element");
    } else {
      this->checker.diags_.error(lit->loc(), "type '" + tuple->name() + "' has " +
                                                 std::to_string(fields.size()) +
                                                 " elements, and this literal has " +
                                                 std::to_string(lit->elements.size()));
    }
    for (ArrayLitExpr::Element &e : lit->elements) {
      e.expr = this->checker.exprChecker().checkExpr(e.expr);
    }
    return lit;
  }
  for (size_t i = 0; i < fields.size(); ++i) {
    Expr *&e = lit->elements[i].expr;
    e = this->checker.typeRelations().coerce(
        this->checker.exprChecker().checkExpr(e, fields[i].type), fields[i].type,
        "element " + std::to_string(i));
  }
  lit->type = tuple;

  return lit;
}

}
