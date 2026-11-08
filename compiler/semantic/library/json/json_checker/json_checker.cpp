#include "compiler/semantic/library/json/json_checker/json_checker.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/binary_checker/binary_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/expr/truth_checker/truth_checker_errors.h"
#include "compiler/semantic/bodies/values/array_literal_checker/array_literal_checker.h"
#include "compiler/semantic/flow/field_facts/field_facts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;

namespace tz {

const VarSymbol *JsonChecker::jsonVariable(const Expr *expr) {
  if (auto *conv = dyn_cast<CastExpr>(expr); conv && conv->isImplicit()) {
    expr = conv->operand;
  }
  auto *name = dyn_cast<NameExpr>(expr);

  return name && name->var() && name->var()->type->nonOptional()->isJson() ? name->var() : nullptr;
}

bool JsonChecker::nullName(const Expr *expr) {
  auto *name = dyn_cast<NameExpr>(expr);

  return name && name->name == "null" && !name->var() && !name->func();
}

bool JsonChecker::isNothing(const Expr *expr) {
  return JsonChecker::nullName(expr) || (expr->type && expr->type->isUndefined());
}

const Expr *JsonChecker::bare(const Expr *expr) {
  while (auto *conv = dyn_cast_or_null<CastExpr>(expr)) {
    if (!conv->isImplicit()) {
      break;
    }
    expr = conv->operand;
  }

  return expr;
}

bool JsonChecker::sameRef(const Expr *a, const Expr *b) {
  a = JsonChecker::bare(a);
  b = JsonChecker::bare(b);
  auto *na = dyn_cast_or_null<NameExpr>(a);
  auto *nb = dyn_cast_or_null<NameExpr>(b);
  if (na && nb) {
    return na->var() && na->var() == nb->var();
  }
  auto *ma = dyn_cast_or_null<MemberExpr>(a);
  auto *mb = dyn_cast_or_null<MemberExpr>(b);

  return ma && mb && ma->name == mb->name && JsonChecker::sameRef(ma->object, mb->object);
}

std::string JsonChecker::refText(const Expr *expr) {
  expr = JsonChecker::bare(expr);
  if (auto *name = dyn_cast<NameExpr>(expr)) {
    return name->name.str();
  }
  if (auto *member = dyn_cast<MemberExpr>(expr)) {
    return JsonChecker::refText(member->object) + "." + member->name.str();
  }

  return "x";
}

void JsonChecker::checkRedundantTests(const BinaryExpr *bin) {
  bool isAnd = bin->op == BinaryOp::LogAnd;
  std::vector<const Expr *> terms;
  std::function<void(const Expr *)> collect = [&](const Expr *e) {
    if (auto *b = dyn_cast<BinaryExpr>(e); b && b->op == bin->op) {
      collect(b->lhs);
      collect(b->rhs);
      return;
    }
    terms.push_back(e);
  };
  collect(bin->lhs);
  const Expr *tested = nullptr;
  bool nullTest = false;
  const Expr *rhs = JsonChecker::bare(bin->rhs);
  if (auto *eq = dyn_cast<BinaryExpr>(rhs); eq &&
                                            (eq->op == BinaryOp::Eq || eq->op == BinaryOp::Ne) &&
                                            (JsonChecker::nullName(JsonChecker::bare(eq->lhs)) ||
                                             JsonChecker::nullName(JsonChecker::bare(eq->rhs)))) {
    tested = JsonChecker::nullName(JsonChecker::bare(eq->lhs)) ? eq->rhs : eq->lhs;
    nullTest = true;
  } else {
    const Expr *inner = rhs;
    if (auto *unary = dyn_cast<UnaryExpr>(rhs); unary && unary->op == UnaryOp::Not) {
      inner = JsonChecker::bare(unary->operand);
    }
    if (auto *call = dyn_cast<CallExpr>(inner); call && call->builtin() == Builtin::ArrayIsArray) {
      tested = call->args[0];
    }
  }
  if (!tested || !JsonChecker::bare(tested)->type ||
      !JsonChecker::bare(tested)->type->nonOptional()->isJson()) {
    return;
  }
  for (const Expr *term : terms) {
    auto *cmp = dyn_cast<BinaryExpr>(JsonChecker::bare(term));
    if (!cmp || cmp->op != (isAnd ? BinaryOp::Eq : BinaryOp::Ne)) {
      continue;
    }
    auto *typeOf = dyn_cast<UnaryExpr>(JsonChecker::bare(cmp->lhs));
    auto *text = dyn_cast<StringLitExpr>(JsonChecker::bare(cmp->rhs));
    if (!typeOf || !text) {
      typeOf = dyn_cast<UnaryExpr>(JsonChecker::bare(cmp->rhs));
      text = dyn_cast<StringLitExpr>(JsonChecker::bare(cmp->lhs));
    }
    if (!typeOf || typeOf->op != UnaryOp::TypeOf || !text ||
        !JsonChecker::sameRef(typeOf->operand, tested)) {
      continue;
    }
    std::string name = ToUtf8::toUtf8(text->value);
    ResultTypeOfAgrees agrees = JsonChecker::typeOfAgrees(tested, name, nullTest, isAnd);
    if (agrees.isErr()) {
      this->state.getDiagnostics().error(rhs->loc(), agrees.error().message);
      return;
    }
  }
}

JsonChecker::ResultTypeOfAgrees JsonChecker::typeOfAgrees(const Expr *tested, llvm::StringRef name,
                                                          bool nullTest, bool isAnd) {
  if (nullTest && name != "null" && name != "undefined") {
    std::string message = ErrorTextJsonTest::neverNull(JsonChecker::refText(tested), isAnd, name);

    return err(JsonCheckerTypeOfAgreesError{
        .code = JsonCheckerTypeOfAgreesErrorCode::NEVER_NULL,
        .message = message,
    });
  }
  if (!nullTest && name != "array") {
    std::string message = ErrorTextJsonTest::neverArray(JsonChecker::refText(tested), isAnd, name);

    return err(JsonCheckerTypeOfAgreesError{
        .code = JsonCheckerTypeOfAgreesErrorCode::NEVER_ARRAY,
        .message = message,
    });
  }

  return ok(tested);
}

JsonChecker::ResultNullPossible JsonChecker::nullPossible(const Type *type, llvm::StringRef op) {
  if (!type->isError() && !type->nonOptional()->isJson()) {
    std::string message = ErrorTextJsonNull::never(type->name(), op);

    return err(JsonCheckerNullPossibleError{
        .code = JsonCheckerNullPossibleErrorCode::NEVER_NULL,
        .message = message,
    });
  }

  return ok(type);
}

JsonChecker::ResultTestsUnknown JsonChecker::testsUnknown(const Type *type) {
  if (!type->isUnknown() && !type->nonOptional()->isParam() &&
      !this->state.getGenericStore().isCheckingInstance()) {
    std::string message = ErrorTextJsonIsJson::known(type->name(), type->nonOptional()->isJson());

    return err(JsonCheckerTestsUnknownError{
        .code = JsonCheckerTestsUnknownErrorCode::KNOWN_TYPE,
        .message = message,
    });
  }

  return ok(type);
}

JsonChecker::JsonChecker(SemanticState &owner)
    : state(owner) {}

bool JsonChecker::holdsJson(const Type *type) {
  while (type->isOptional() || type->isArray()) {
    type = type->inner();
  }

  return type->isJson();
}

bool JsonChecker::isNullName(const Expr *expr) {
  return JsonChecker::nullName(expr);
}

Expr *JsonChecker::checkNull(NameExpr *name, const Type *expected) {
  if (!JsonChecker::isNullName(name) || !expected || !expected->nonOptional()->isJson()) {
    return nullptr;
  }
  name->type = Type::json();

  return name;
}

Expr *JsonChecker::checkJsonMember(MemberExpr *member) {
  member->type = Type::optional(Type::json());
  if (Expr *narrowed = FieldFacts(this->state).narrowedField(member); narrowed != member) {
    return narrowed;
  }
  if (std::optional<FieldPath> path = FieldFacts(this->state).fieldPath(member);
      path && FieldFacts(this->state).isFieldDefined(*path)) {
    return TypeRelations(this->state).convert(member, Type::json());
  }

  return member;
}

Expr *JsonChecker::checkJsonIndex(IndexExpr *index) {
  index->index = ExprChecker(this->state).checkExpr(index->index);
  index->type = Type::optional(Type::json());
  const Type *key = index->index->type;
  if (key->isError() || key->isString()) {
    return index;
  }
  if (key->isNumeric()) {
    index->index = TypeRelations(this->state).convert(index->index, Type::doubleTy());
    return index;
  }
  std::string message = ErrorTextJsonKey::index(key->name());
  this->state.getDiagnostics().error(index->index->loc(), message);
  index->type = Type::error();

  return index;
}

const Type *JsonChecker::checkJsonTarget(Expr *target) {
  if (auto *index = dyn_cast<IndexExpr>(target)) {
    if (this->checkJsonIndex(index)->type->isError()) {
      return Type::error();
    }
  } else {
    this->checkJsonMember(cast<MemberExpr>(target));
  }

  return Type::optional(Type::json());
}

Expr *JsonChecker::reportJsonMethod(CallExpr *call, MemberExpr *member) {
  const Expr *object = member->object;
  if (auto *conv = dyn_cast<CastExpr>(object); conv && conv->isImplicit()) {
    object = conv->operand;
  }
  llvm::StringRef variable;
  if (auto *name = dyn_cast<NameExpr>(object); name && name->var()) {
    variable = name->name;
  }
  std::string message = ErrorTextJsonMethod::none(member->name, variable);
  this->state.getDiagnostics().error(member->loc(), message);
  for (Expr *&arg : call->args) {
    arg = ExprChecker(this->state).checkExpr(arg);
  }

  return call;
}

Expr *JsonChecker::checkJsonObjectLit(ObjectLitExpr *lit) {
  const Type *json = Type::json();
  for (ObjectLitExpr::Property &prop : lit->props) {
    if (prop.spread() || prop.method()) {
      std::string message =
          prop.spread() ? ErrorTextJsonLiteral::spread() : ErrorTextJsonLiteral::function();
      this->state.getDiagnostics().error(prop.loc, message);
      prop.value = ExprChecker(this->state).checkExpr(prop.value);
      continue;
    }
    if (prop.defaulted()) {
      std::string message = ErrorTextJsonLiteral::defaulted(prop.name);
      this->state.getDiagnostics().error(prop.loc, message);
      prop.value = ExprChecker(this->state).checkExpr(cast<AssignExpr>(prop.value)->value);
      prop.form = ValueProperty{};
      continue;
    }
    if (ComputedProperty *computed = prop.form.as<ComputedProperty>()) {
      computed->key = ExprChecker(this->state).checkExpr(computed->key);
      if (!prop.key()->type->isString() && !prop.key()->type->isError()) {
        std::string message = ErrorTextJsonKey::literal(prop.key()->type->name());
        this->state.getDiagnostics().error(prop.key()->loc(), message);
      }
    }
    Expr *value = ExprChecker(this->state).checkExpr(prop.value, json);
    const Type *type = value->type;
    std::string label = ErrorTextJsonKey::label(prop.name);
    prop.value =
        TypeRelations(this->state)
            .coerce(value, type->isOptional() || type->isUndefined() ? Type::optional(json) : json,
                    label);
  }
  lit->type = json;

  return lit;
}

Expr *JsonChecker::checkJsonArrayLit(ArrayLitExpr *lit) {
  Expr *array = ArrayLiteralChecker(this->state).checkArrayLit(lit, Type::array(Type::json()));
  if (array->type->isError()) {
    return array;
  }

  return TypeRelations(this->state).coerce(array, Type::json());
}

Expr *JsonChecker::checkNullEquality(BinaryExpr *bin) {
  bool lhsNull = JsonChecker::isNullName(bin->lhs);
  bool rhsNull = JsonChecker::isNullName(bin->rhs);
  if (!lhsNull && !rhsNull) {
    return nullptr;
  }
  bin->type = Type::boolTy();
  Expr *&null = lhsNull ? bin->lhs : bin->rhs;
  Expr *&other = lhsNull ? bin->rhs : bin->lhs;
  const Type *type = Type::undefinedTy();
  if (!(lhsNull && rhsNull)) {
    other = ExprChecker(this->state).checkExpr(other);
    type = other->type;
  }
  null->type = Type::json();
  ResultNullPossible possible = JsonChecker::nullPossible(type, Operators::opSpelling(bin->op));
  if (possible.isOk()) {
    return bin;
  }
  this->state.getDiagnostics().error(null->loc(), possible.error().message);
  null->type = Type::error();

  return bin;
}

Expr *JsonChecker::checkJsonEquality(BinaryExpr *bin) {
  const Type *l = bin->lhs->type;
  const Type *r = bin->rhs->type;
  bool lhsJson = l->nonOptional()->isJson();
  bool rhsJson = r->nonOptional()->isJson();
  const Type *json =
      l->isOptional() || r->isOptional() ? Type::optional(Type::json()) : Type::json();
  if (lhsJson && rhsJson) {
    bin->lhs = TypeRelations(this->state).convert(bin->lhs, json);
    bin->rhs = TypeRelations(this->state).convert(bin->rhs, json);
    return bin;
  }
  Expr *&other = lhsJson ? bin->rhs : bin->lhs;
  const Type *type = other->type;
  if (type->isString() || type->isBool()) {
    return bin;
  }
  if (type->isNumeric()) {
    other = TypeRelations(this->state).convert(other, Type::doubleTy());
    return bin;
  }
  if (type->isOptional() && TypeRelations::isImplicitlyConvertible(type->inner(), Type::json())) {
    bin->lhs = TypeRelations(this->state).convert(bin->lhs, json);
    bin->rhs = TypeRelations(this->state).convert(bin->rhs, json);
    return bin;
  }
  std::string message = BinaryChecker::operandTypesText(Operators::opSpelling(bin->op), l, r);
  this->state.getDiagnostics().error(bin->loc(), message);

  return bin;
}

std::optional<FieldPath> JsonChecker::jsonField(const Expr *expr) const {
  if (auto *conv = dyn_cast<CastExpr>(expr); conv && conv->isImplicit()) {
    expr = conv->operand;
  }
  auto *member = dyn_cast<MemberExpr>(expr);
  if (!member || !member->object->type || !member->object->type->nonOptional()->isJson()) {
    return std::nullopt;
  }

  return FieldFacts(this->state).fieldPath(member);
}

void JsonChecker::narrowJsonField(Flow &flow, const FieldPath &path, const Type *type) {
  llvm::erase_if(flow.narrowedFields, [&](const auto &entry) { return entry.first == path; });
  flow.narrowedFields.push_back({path, type});
  FieldFacts::defineField(flow, path);
}

void JsonChecker::addJsonFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const {
  if (auto *call = dyn_cast<CallExpr>(cond); call && call->builtin() == Builtin::ArrayIsArray) {
    if (const VarSymbol *var = JsonChecker::jsonVariable(call->args[0])) {
      whenTrue.narrowed.push_back({var, Type::array(Type::json())});
      if (var->type->isOptional()) {
        whenTrue.defined.insert(var);
      }
    } else if (std::optional<FieldPath> path = this->jsonField(call->args[0])) {
      JsonChecker::narrowJsonField(whenTrue, *path, Type::array(Type::json()));
    }
    return;
  }
  auto *bin = dyn_cast<BinaryExpr>(cond);
  if (!bin || (bin->op != BinaryOp::Eq && bin->op != BinaryOp::Ne)) {
    return;
  }
  Flow &equal = bin->op == BinaryOp::Eq ? whenTrue : whenFalse;
  Flow &different = bin->op == BinaryOp::Eq ? whenFalse : whenTrue;
  for (auto [side, other] : {std::pair{bin->lhs, bin->rhs}, std::pair{bin->rhs, bin->lhs}}) {
    if (JsonChecker::isNothing(other)) {
      bool null = JsonChecker::nullName(other);
      if (const VarSymbol *var = JsonChecker::jsonVariable(side); var && var->type->isOptional()) {
        (null ? equal : different).defined.insert(var);
      } else if (std::optional<FieldPath> path = this->jsonField(side)) {
        FieldFacts::defineField(null ? equal : different, *path);
      }
      continue;
    }
    auto *typeOf = dyn_cast<UnaryExpr>(side);
    auto *text = dyn_cast<StringLitExpr>(other);
    if (!typeOf || typeOf->op != UnaryOp::TypeOf || !text) {
      continue;
    }
    const Type *narrowed = text->value == u"string"    ? Type::string()
                           : text->value == u"double"  ? Type::doubleTy()
                           : text->value == u"boolean" ? Type::boolTy()
                           : text->value == u"array"   ? Type::array(Type::json())
                                                       : nullptr;
    bool present = narrowed || text->value == u"object" || text->value == u"null";
    const VarSymbol *var = JsonChecker::jsonVariable(typeOf->operand);
    if (!var) {
      std::optional<FieldPath> path = this->jsonField(typeOf->operand);
      if (!path) {
        continue;
      }
      if (narrowed) {
        JsonChecker::narrowJsonField(equal, *path, narrowed);
      } else if (present) {
        FieldFacts::defineField(equal, *path);
      } else if (text->value == u"undefined") {
        FieldFacts::defineField(different, *path);
      }
      continue;
    }
    if (narrowed) {
      equal.narrowed.push_back({var, narrowed});
    }
    if (var->type->isOptional()) {
      if (present) {
        equal.defined.insert(var);
      } else if (text->value == u"undefined") {
        different.defined.insert(var);
      }
    }
  }
}

Expr *JsonChecker::checkJsonIsJson(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  bool rejected = CallChecker(this->state).rejectSpread(call);
  ArgumentChecker::ResultArityMatches arity =
      rejected ? ok(call) : ArgumentChecker(this->state).checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
  }
  if (rejected || arity.isErr()) {
    return call;
  }
  call->args[0] = ExprChecker(this->state).checkExpr(call->args[0]);
  const Type *type = call->args[0]->type;
  if (type->isError()) {
    return call;
  }
  ResultTestsUnknown tests = this->testsUnknown(type);
  if (tests.isErr()) {
    this->state.getDiagnostics().error(call->args[0]->loc(), tests.error().message);
    return call;
  }
  call->target = BuiltinCall{.builtin = Builtin::JsonIsJson};
  call->type = Type::boolTy();

  return call;
}

Expr *JsonChecker::checkArrayIsArray(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  bool rejected = CallChecker(this->state).rejectSpread(call);
  ArgumentChecker::ResultArityMatches arity =
      rejected ? ok(call) : ArgumentChecker(this->state).checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
  }
  if (rejected || arity.isErr()) {
    return call;
  }
  call->args[0] = ExprChecker(this->state).checkExpr(call->args[0]);
  if (call->args[0]->type->isVoid()) {
    std::string message = ErrorTextTruthy::voidValue();
    this->state.getDiagnostics().error(call->args[0]->loc(), message);
    return call;
  }
  call->target = BuiltinCall{.builtin = Builtin::ArrayIsArray};
  call->type = Type::boolTy();

  return call;
}

}
