#include "compiler/semantic/bodies/objects/tuple_checker/tuple_checker.h"
#include "compiler/program/library/array/array_method_table/array_method_table.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/objects/object_check_helpers/object_check_helpers.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr const char *kTuplePlaceholder = "t";

}

TupleChecker::TupleChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->typeRelations = &all.typeRelations;
  this->argumentChecker = &all.argumentChecker;
}

Expr *TupleChecker::arrayCopy(Expr *tuple) {
  const Type *type = tuple->type;
  if (type == nullptr || !type->isTuple() || type->tupleRest() != nullptr ||
      type->fields().empty() || type->tupleMin() != type->fields().size()) {
    return nullptr;
  }
  const Type *element = type->fields().front().type;
  for (const Field &field : type->fields()) {
    if (field.type != element) {
      return nullptr;
    }
  }
  auto *copy = this->state.getAst().make<ArrayLitExpr>(tuple->loc());
  copy->elements.push_back(ArrayLitExpr::Element{.expr = tuple, .spread = true});
  copy->type = Type::array(element);

  return copy;
}

TupleChecker::ResultTupleJoined TupleChecker::checkTupleJoin(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  if (this->callChecker->rejectSpread(call)) {
    return err(TupleCheckerCheckTupleJoinError{
        .code = TupleCheckerCheckTupleJoinErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  auto arity = this->argumentChecker->checkArity(call, 0, 1);
  if (arity.isErr()) {
    return err(TupleCheckerCheckTupleJoinError{
        .code = TupleCheckerCheckTupleJoinErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }
  if (!call->args.empty()) {
    call->args[0] = this->argumentChecker->checkArg(call->args[0], Type::string());
  }
  const Type *tuple = member->object->type;
  for (size_t index = 0; index < tuple->fields().size(); ++index) {
    const Type *element = tuple->fields()[index].type->nonOptional();
    if (!this->isJoinableElement(element)) {
      auto message = ErrorTextTupleJoin::notText(index, tuple->name(), element->name());

      return err(TupleCheckerCheckTupleJoinError{
          .code = TupleCheckerCheckTupleJoinErrorCode::ELEMENT_NOT_TEXT,
          .message = message,
      });
    }
  }
  call->target = BuiltinCall{.builtin = Builtin::TupleJoin};
  call->type = Type::string();

  return ok(call);
}

bool TupleChecker::isJoinableElement(const Type *element) const {
  return element->isNumeric() || element->isDecimal() || element->isBool() || element->isString() ||
         element->isUndefined();
}

const Type *TupleChecker::tupleElement(const Type *tuple) const {
  const Type *element = nullptr;
  for (const Field &field : tuple->fields()) {
    element = element != nullptr ? LiteralTypes::unionUnify(element, field.type) : field.type;
    if (element == nullptr || element->isError()) {
      return nullptr;
    }
  }

  return element;
}

TupleChecker::ResultTupleMember TupleChecker::checkTupleMember(MemberExpr *member) {
  const Type *type = member->object->type;
  if (!BuiltinPropertyTable::isName(member->name, BuiltinProperty::LENGTH)) {
    member->type = Type::error();
    auto message = ErrorTextTupleMember::noProperty(member->name, type->name());

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
  auto *sequence = this->state.getAst().make<SequenceExpr>(
      member->loc(), std::vector<Expr *>{member->object, length});
  sequence->type = Type::intTy();

  return ok(sequence);
}

TupleChecker::ResultTupleMethod TupleChecker::checkTupleMethod(CallExpr *call, MemberExpr *member) {
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  const auto *method = ArrayMethodTable::find(name);
  bool isJoining = method != nullptr && method->method == ArrayMethod::JOIN;
  if (isJoining) {
    return this->checkJoinMethod(call, member);
  }
  bool isResizing = method != nullptr && method->form.is<ResizingArrayMethod>();
  auto text = this->tupleText(member);
  this->argumentChecker->walkArgsLoosely(call);
  call->type = Type::error();
  if (isResizing) {
    auto message = ErrorTextTupleMethod::fixedLen(name, text);

    return err(TupleCheckerCheckTupleMethodError{
        .code = TupleCheckerCheckTupleMethodErrorCode::FIXED_LENGTH,
        .message = message,
    });
  }
  if (this->tupleElement(type) != nullptr) {
    auto message = ErrorTextTupleMethod::copyFirst(name, text);

    return err(TupleCheckerCheckTupleMethodError{
        .code = TupleCheckerCheckTupleMethodErrorCode::COPY_FIRST,
        .message = message,
    });
  }
  auto message = ErrorTextTupleMethod::destruct(name, text);

  return err(TupleCheckerCheckTupleMethodError{
      .code = TupleCheckerCheckTupleMethodErrorCode::DESTRUCTURE_INSTEAD,
      .message = message,
  });
}

TupleChecker::ResultTupleMethod TupleChecker::checkJoinMethod(CallExpr *call, MemberExpr *member) {
  auto joined = this->checkTupleJoin(call, member);
  if (joined.isOk()) {
    return ok(joined.value());
  }
  auto code = this->joinMethodCode(joined.error().code);

  return err(TupleCheckerCheckTupleMethodError{
      .code = code,
      .message = joined.error().message,
  });
}

TupleCheckerCheckTupleMethodErrorCode
TupleChecker::joinMethodCode(TupleCheckerCheckTupleJoinErrorCode code) const {
  switch (code) {
    case TupleCheckerCheckTupleJoinErrorCode::ALREADY_REPORTED:
      return TupleCheckerCheckTupleMethodErrorCode::ALREADY_REPORTED;
    case TupleCheckerCheckTupleJoinErrorCode::WRONG_ARITY:
    case TupleCheckerCheckTupleJoinErrorCode::ELEMENT_NOT_TEXT:
      return TupleCheckerCheckTupleMethodErrorCode::JOIN_FAILED;
  }

  return TupleCheckerCheckTupleMethodErrorCode::JOIN_FAILED;
}

std::string TupleChecker::tupleText(const MemberExpr *member) const {
  const Expr *unnarrowed = ObjectCheckHelpers::unnarrowed(member->object);
  auto *object = dyn_cast<NameExpr>(unnarrowed);
  if (object == nullptr) {
    return kTuplePlaceholder;
  }

  return object->name.str();
}

TupleChecker::ResultTupleFilled TupleChecker::checkTupleLit(ArrayLitExpr *literal,
                                                            const Type *tuple) {
  llvm::ArrayRef<Field> fields = tuple->fields();
  literal->type = Type::error();
  bool isSpread =
      llvm::any_of(literal->elements, std::bind_front(&TupleChecker::isSpreadElement, this));
  if (isSpread || literal->elements.size() != fields.size()) {
    this->walkLooseElements(literal);
  }
  if (isSpread) {
    auto message = ErrorTextTupleLit::spread(tuple->name());

    return err(TupleCheckerCheckTupleLitError{
        .code = TupleCheckerCheckTupleLitErrorCode::SPREAD_IN_TUPLE,
        .message = message,
    });
  }
  if (literal->elements.size() != fields.size()) {
    auto message =
        ErrorTextTupleLit::length(tuple->name(), fields.size(), literal->elements.size());

    return err(TupleCheckerCheckTupleLitError{
        .code = TupleCheckerCheckTupleLitErrorCode::WRONG_LENGTH,
        .message = message,
    });
  }
  for (size_t index = 0; index < fields.size(); ++index) {
    Expr *&expr = literal->elements[index].expr;
    auto what = ErrorTextTupleLit::element(index);
    auto *checked = this->exprChecker->checkExpr(expr, fields[index].type);
    expr = this->typeRelations->coerce(checked, fields[index].type, what);
  }
  literal->type = tuple;

  return ok(literal);
}

bool TupleChecker::isSpreadElement(const ArrayLitExpr::Element &element) const {
  return element.spread;
}

void TupleChecker::walkLooseElements(ArrayLitExpr *literal) {
  for (ArrayLitExpr::Element &element : literal->elements) {
    element.expr = this->exprChecker->checkExpr(element.expr);
  }
}

}
