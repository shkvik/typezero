#include "compiler/pipeline/check_program/topics/objects/tuple_checker/tuple_checker.h"
#include "compiler/pipeline/check_program/topics/expr/argument_checker/argument_checker.h"
#include "compiler/pipeline/check_program/topics/expr/call_checker/call_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "compiler/pipeline/check_program/topics/objects/object_check_helpers/object_check_helpers.h"
#include "compiler/pipeline/check_program/types/literal_types/literal_types.h"
#include "compiler/pipeline/check_program/types/type_relations/type_relations.h"
#include "compiler/pipeline/check_program/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

TupleChecker::TupleChecker(CheckState &owner)
    : state(owner) {}

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
  auto *copy = this->state.getAst().make<ArrayLitExpr>(tuple->loc());
  copy->elements.push_back({tuple, true});
  copy->type = Type::array(element);

  return copy;
}

TupleChecker::ResultTupleJoined TupleChecker::checkTupleJoin(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  if (CallChecker(this->state).rejectSpread(call)) {
    return err(TupleCheckerCheckTupleJoinError{
        .code = TupleCheckerCheckTupleJoinErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 0, 1);
  if (arity.isErr()) {
    return err(TupleCheckerCheckTupleJoinError{
        .code = TupleCheckerCheckTupleJoinErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }
  if (!call->args.empty()) {
    call->args[0] = ArgumentChecker(this->state).checkArg(call->args[0], Type::string());
  }
  const Type *tuple = member->object->type;
  for (size_t i = 0; i < tuple->fields().size(); ++i) {
    const Type *element = tuple->fields()[i].type->nonOptional();
    if (element->isNumeric() || element->isDecimal() || element->isBool() || element->isString() ||
        element->isUndefined()) {
      continue;
    }
    std::string message = ErrorTextTupleJoin::notText(i, tuple->name(), element->name());

    return err(TupleCheckerCheckTupleJoinError{
        .code = TupleCheckerCheckTupleJoinErrorCode::ELEMENT_NOT_TEXT,
        .message = message,
    });
  }
  call->target = BuiltinCall{.builtin = Builtin::TupleJoin};
  call->type = Type::string();

  return ok(call);
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

TupleChecker::ResultTupleMember TupleChecker::checkTupleMember(MemberExpr *member) {
  const Type *type = member->object->type;
  if (member->name != "length") {
    member->type = Type::error();
    std::string message = ErrorTextTupleMember::noProperty(member->name, type->name());

    return err(TupleCheckerCheckTupleMemberError{
        .code = TupleCheckerCheckTupleMemberErrorCode::NO_SUCH_PROPERTY,
        .message = message,
    });
  }
  auto *length = this->state.getAst().make<IntLitExpr>(member->loc(), type->fields().size());
  length->type = Type::intTy();
  if (isa<NameExpr>(member->object)) {
    return ok(length);
  }
  auto *seq = this->state.getAst().make<SequenceExpr>(member->loc(),
                                                      std::vector<Expr *>{member->object, length});
  seq->type = Type::intTy();

  return ok(seq);
}

TupleChecker::ResultTupleMethod TupleChecker::checkTupleMethod(CallExpr *call, MemberExpr *member) {
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  std::string text = "t";
  if (auto *object = dyn_cast<NameExpr>(unnarrowed(member->object))) {
    text = object->name.str();
  }
  static const char *const resizing[] = {"push", "pop", "shift", "unshift", "splice"};
  if (name == "join") {
    ResultTupleJoined joined = this->checkTupleJoin(call, member);
    bool reported = joined.isErr() &&
                    joined.error().code == TupleCheckerCheckTupleJoinErrorCode::ALREADY_REPORTED;
    if (reported) {
      return err(TupleCheckerCheckTupleMethodError{
          .code = TupleCheckerCheckTupleMethodErrorCode::ALREADY_REPORTED,
          .message = "",
      });
    }
    if (joined.isErr()) {
      return err(TupleCheckerCheckTupleMethodError{
          .code = TupleCheckerCheckTupleMethodErrorCode::JOIN_FAILED,
          .message = joined.error().message,
      });
    }
    return ok(joined.value());
  }
  ArgumentChecker(this->state).checkArgsLoosely(call);
  call->type = Type::error();
  if (llvm::is_contained(resizing, name)) {
    std::string message = ErrorTextTupleMethod::fixedLen(name, text);

    return err(TupleCheckerCheckTupleMethodError{
        .code = TupleCheckerCheckTupleMethodErrorCode::FIXED_LENGTH,
        .message = message,
    });
  }
  if (TupleChecker::tupleElement(type)) {
    std::string message = ErrorTextTupleMethod::copyFirst(name, text);

    return err(TupleCheckerCheckTupleMethodError{
        .code = TupleCheckerCheckTupleMethodErrorCode::COPY_FIRST,
        .message = message,
    });
  }

  std::string message = ErrorTextTupleMethod::destruct(name, text);

  return err(TupleCheckerCheckTupleMethodError{
      .code = TupleCheckerCheckTupleMethodErrorCode::DESTRUCTURE_INSTEAD,
      .message = message,
  });
}

TupleChecker::ResultTupleFilled TupleChecker::checkTupleLit(ArrayLitExpr *lit, const Type *tuple) {
  llvm::ArrayRef<Field> fields = tuple->fields();
  lit->type = Type::error();
  auto spreads = [](const ArrayLitExpr::Element &e) { return e.spread; };
  bool spread = llvm::any_of(lit->elements, spreads);
  if (spread || lit->elements.size() != fields.size()) {
    for (ArrayLitExpr::Element &e : lit->elements) {
      e.expr = ExprChecker(this->state).checkExpr(e.expr);
    }
  }
  if (spread) {
    std::string message = ErrorTextTupleLit::spread(tuple->name());

    return err(TupleCheckerCheckTupleLitError{
        .code = TupleCheckerCheckTupleLitErrorCode::SPREAD_IN_TUPLE,
        .message = message,
    });
  }
  if (lit->elements.size() != fields.size()) {
    std::string message =
        ErrorTextTupleLit::length(tuple->name(), fields.size(), lit->elements.size());

    return err(TupleCheckerCheckTupleLitError{
        .code = TupleCheckerCheckTupleLitErrorCode::WRONG_LENGTH,
        .message = message,
    });
  }

  for (size_t i = 0; i < fields.size(); ++i) {
    Expr *&e = lit->elements[i].expr;
    std::string what = ErrorTextTupleLit::element(i);
    e = TypeRelations(this->state)
            .coerce(ExprChecker(this->state).checkExpr(e, fields[i].type), fields[i].type, what);
  }
  lit->type = tuple;

  return ok(lit);
}

}
