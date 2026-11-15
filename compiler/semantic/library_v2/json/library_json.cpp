#include "compiler/semantic/library_v2/json/library_json.h"
#include "compiler/program/library/builtin_names/keyword_table/keyword_table.h"
#include "compiler/program/library/builtin_names/type_of_name_table/type_of_name_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>

using llvm::dyn_cast;

namespace tz {

LibraryJson::LibraryJson(SemanticState &owner)
    : state(owner) {}

const VarSymbol *LibraryJson::jsonVariable(const Expr *expr) const {
  auto *conversion = dyn_cast<CastExpr>(expr);
  if (conversion != nullptr && conversion->isImplicit()) {
    expr = conversion->operand;
  }
  auto *name = dyn_cast<NameExpr>(expr);

  return name != nullptr && name->var() != nullptr && name->var()->type->nonOptional()->isJson()
             ? name->var()
             : nullptr;
}

bool LibraryJson::isNullName(const Expr *expr) const {
  const auto *name = dyn_cast<NameExpr>(expr);

  return name != nullptr && KeywordTable::find(name->name) == Keyword::NULL_KEYWORD &&
         name->var() == nullptr && name->func() == nullptr;
}

bool LibraryJson::isNothing(const Expr *expr) const {
  return this->isNullName(expr) || (expr->type != nullptr && expr->type->isUndefined());
}

void LibraryJson::addJsonFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const {
  const auto *call = dyn_cast<CallExpr>(cond);
  if (call != nullptr && call->builtin() == Builtin::ArrayIsArray) {
    this->addArrayFacts(call->args[0], whenTrue);
    return;
  }
  const auto *binary = dyn_cast<BinaryExpr>(cond);
  if (binary == nullptr || (binary->op != BinaryOp::Eq && binary->op != BinaryOp::Ne)) {
    return;
  }
  Flow &equal = binary->op == BinaryOp::Eq ? whenTrue : whenFalse;
  Flow &different = binary->op == BinaryOp::Eq ? whenFalse : whenTrue;
  this->addSideFacts(binary->lhs, binary->rhs, equal, different);
  this->addSideFacts(binary->rhs, binary->lhs, equal, different);
}

void LibraryJson::addSideFacts(const Expr *side, const Expr *other, Flow &equal,
                               Flow &different) const {
  if (this->isNothing(other)) {
    return this->addNothingFacts(side, other, equal, different);
  }
  const auto *typeOf = dyn_cast<UnaryExpr>(side);
  const auto *text = dyn_cast<StringLitExpr>(other);
  if (typeOf != nullptr && typeOf->op == UnaryOp::TypeOf && text != nullptr) {
    this->addTypeOfFacts(typeOf->operand, text->value, equal, different);
  }
}

void LibraryJson::addArrayFacts(const Expr *arg, Flow &whenTrue) const {
  const auto *variable = this->jsonVariable(arg);
  if (variable != nullptr) {
    whenTrue.narrowed.push_back({variable, Type::array(Type::json())});
    if (variable->type->isOptional()) {
      whenTrue.defined.insert(variable);
    }
    return;
  }
  auto path = this->jsonField(arg);
  if (path.has_value()) {
    this->narrowJsonField(whenTrue, *path, Type::array(Type::json()));
  }
}

void LibraryJson::addNothingFacts(const Expr *side, const Expr *other, Flow &equal,
                                  Flow &different) const {
  bool isNull = this->isNullName(other);
  Flow &defined = isNull ? equal : different;
  const auto *variable = this->jsonVariable(side);
  if (variable != nullptr && variable->type->isOptional()) {
    defined.defined.insert(variable);
    return;
  }
  auto path = this->jsonField(side);
  if (path.has_value()) {
    this->state.flow.tracked.defineField(defined, *path);
  }
}

void LibraryJson::addTypeOfFacts(const Expr *operand, const std::u16string &text, Flow &equal,
                                 Flow &different) const {
  const auto *narrowed = this->typeOfJson(text);
  auto typeOf = TypeOfNameTable::find(ToUtf8::toUtf8(text));
  bool isPresent =
      narrowed != nullptr || typeOf == TypeOfName::OBJECT || typeOf == TypeOfName::NULL_TYPE;
  const auto *variable = this->jsonVariable(operand);
  if (variable == nullptr) {
    auto path = this->jsonField(operand);
    if (!path.has_value()) {
      return;
    }
    if (narrowed != nullptr) {
      this->narrowJsonField(equal, *path, narrowed);
    } else if (isPresent) {
      this->state.flow.tracked.defineField(equal, *path);
    } else if (typeOf == TypeOfName::UNDEFINED_TYPE) {
      this->state.flow.tracked.defineField(different, *path);
    }
    return;
  }
  if (narrowed != nullptr) {
    equal.narrowed.push_back({variable, narrowed});
  }
  if (!variable->type->isOptional()) {
    return;
  }
  if (isPresent) {
    equal.defined.insert(variable);
  } else if (typeOf == TypeOfName::UNDEFINED_TYPE) {
    different.defined.insert(variable);
  }
}

const Type *LibraryJson::typeOfJson(const std::u16string &text) const {
  auto typeOf = TypeOfNameTable::find(ToUtf8::toUtf8(text));
  if (typeOf == TypeOfName::STRING) {
    return Type::string();
  }
  if (typeOf == TypeOfName::DOUBLE) {
    return Type::doubleTy();
  }
  if (typeOf == TypeOfName::BOOLEAN) {
    return Type::boolTy();
  }
  if (typeOf == TypeOfName::ARRAY) {
    return Type::array(Type::json());
  }

  return nullptr;
}

std::optional<FieldPath> LibraryJson::jsonField(const Expr *expr) const {
  const auto *conversion = dyn_cast<CastExpr>(expr);
  bool isImplicit = conversion != nullptr && conversion->isImplicit();
  const Expr *field = isImplicit ? conversion->operand : expr;
  const auto *member = dyn_cast<MemberExpr>(field);
  if (member == nullptr || member->object->type == nullptr ||
      !member->object->type->nonOptional()->isJson()) {
    return std::nullopt;
  }

  return this->state.flow.tracked.fieldPath(member);
}

void LibraryJson::narrowJsonField(Flow &flow, const FieldPath &path, const Type *type) const {
  auto isSamePath = std::bind_front(&LibraryJson::isNarrowedPath, this, std::cref(path));
  llvm::erase_if(flow.narrowedFields, isSamePath);
  flow.narrowedFields.push_back({path, type});
  this->state.flow.tracked.defineField(flow, path);
}

bool LibraryJson::isNarrowedPath(const FieldPath &path,
                                 const std::pair<FieldPath, const Type *> &entry) const {
  return entry.first == path;
}

}
