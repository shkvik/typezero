#include "compiler/semantic/library_v2/json/library_json_registry.h"
#include "compiler/packages/json_text/json_text.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/builtin_names/type_of_name_table/type_of_name_table.h"
#include "compiler/program/library/json/json_member_table/json_member_table.h"
#include "compiler/program/library/json/json_static_table/json_static_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/code/logic/code_logic_registry_errors.h"
#include "compiler/semantic/code/operators/code_operators_registry_errors.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/types/type_parts/type_routes.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

constexpr llvm::StringLiteral kValuePlaceholder = "x";

}

LibraryJsonRegistry::LibraryJsonRegistry(SemanticState &owner)
    : state(owner) {}

const Expr *LibraryJsonRegistry::bare(const Expr *expr) const {
  if (expr == nullptr) {
    return nullptr;
  }

  return ExprParts::withoutImplicitCasts(expr);
}

bool LibraryJsonRegistry::isSameRef(const Expr *first, const Expr *second) const {
  const auto *left = this->bare(first);
  const auto *right = this->bare(second);
  const auto *leftMember = dyn_cast_or_null<MemberExpr>(left);
  const auto *rightMember = dyn_cast_or_null<MemberExpr>(right);
  while (leftMember != nullptr && rightMember != nullptr) {
    if (leftMember->name != rightMember->name) {
      return false;
    }
    left = this->bare(leftMember->object);
    right = this->bare(rightMember->object);
    leftMember = dyn_cast_or_null<MemberExpr>(left);
    rightMember = dyn_cast_or_null<MemberExpr>(right);
  }
  const auto *leftName = dyn_cast_or_null<NameExpr>(left);
  const auto *rightName = dyn_cast_or_null<NameExpr>(right);
  if (leftName == nullptr || rightName == nullptr) {
    return false;
  }

  return leftName->var() != nullptr && leftName->var() == rightName->var();
}

std::string LibraryJsonRegistry::refText(const Expr *expr) const {
  auto names = std::vector<llvm::StringRef>();
  const auto *link = this->bare(expr);
  const auto *member = dyn_cast<MemberExpr>(link);
  while (member != nullptr) {
    names.push_back(member->name);
    link = this->bare(member->object);
    member = dyn_cast<MemberExpr>(link);
  }
  const auto *root = dyn_cast<NameExpr>(link);
  std::string text = root != nullptr ? root->name.str() : kValuePlaceholder.str();
  for (llvm::StringRef name : llvm::reverse(names)) {
    text += "." + name.str();
  }

  return text;
}

void LibraryJsonRegistry::walkRedundantTests(const WalkRedundantTests &task) {
  const BinaryExpr *binary = task.binary;
  bool isAnd = binary->op == BinaryOp::LogAnd;
  auto terms = ExprParts::sameOperatorParts(binary->lhs, binary->op);
  const auto *rhs = this->bare(binary->rhs);
  auto test = this->jsonTest(rhs);
  if (!test.has_value()) {
    return;
  }
  for (const Expr *term : terms) {
    if (this->reportRedundantTest(term, test.value(), isAnd, rhs)) {
      return;
    }
  }
}

std::optional<LibraryJsonRegistry::JsonTest> LibraryJsonRegistry::jsonTest(const Expr *rhs) const {
  auto found = this->testedSide(rhs);
  if (!found.has_value()) {
    return std::nullopt;
  }
  const auto *tested = this->bare(found->tested);
  if (tested->type == nullptr || !tested->type->nonOptional()->isJson()) {
    return std::nullopt;
  }

  return found;
}

std::optional<LibraryJsonRegistry::JsonTest>
LibraryJsonRegistry::testedSide(const Expr *rhs) const {
  const auto *equality = dyn_cast<BinaryExpr>(rhs);
  bool isEquality =
      equality != nullptr && (equality->op == BinaryOp::Eq || equality->op == BinaryOp::Ne);
  if (isEquality && this->isNullComparison(equality)) {
    bool isLeftNull = this->state.library.json.isNullName(this->bare(equality->lhs));
    const Expr *tested = isLeftNull ? equality->rhs : equality->lhs;
    return JsonTest{.tested = tested, .isNullTest = true};
  }
  const auto *inner = this->withoutNot(rhs);
  const auto *call = dyn_cast<CallExpr>(inner);
  if (call != nullptr && call->builtin() == Builtin::ArrayIsArray) {
    return JsonTest{.tested = call->args[0], .isNullTest = false};
  }

  return std::nullopt;
}

bool LibraryJsonRegistry::isNullComparison(const BinaryExpr *equality) const {
  return this->state.library.json.isNullName(this->bare(equality->lhs)) ||
         this->state.library.json.isNullName(this->bare(equality->rhs));
}

const Expr *LibraryJsonRegistry::withoutNot(const Expr *expr) const {
  const auto *unary = dyn_cast<UnaryExpr>(expr);
  if (unary != nullptr && unary->op == UnaryOp::Not) {
    return this->bare(unary->operand);
  }

  return expr;
}

bool LibraryJsonRegistry::reportRedundantTest(const Expr *term, const JsonTest &test, bool isAnd,
                                              const Expr *rhs) {
  const auto *text = this->typeOfTextFor(term, test.tested, isAnd);
  if (text == nullptr) {
    return false;
  }
  auto name = ToUtf8::toUtf8(text->value);
  auto agrees = this->checkTypeOfAgrees(test.tested, name, test.isNullTest, isAnd);
  if (agrees.isOk()) {
    return false;
  }
  this->state.diagnostics.error(rhs->loc(), agrees.error().message);

  return true;
}

const StringLitExpr *LibraryJsonRegistry::typeOfTextFor(const Expr *term, const Expr *tested,
                                                        bool isAnd) const {
  const auto *comparison = dyn_cast<BinaryExpr>(this->bare(term));
  BinaryOp wanted = isAnd ? BinaryOp::Eq : BinaryOp::Ne;
  if (comparison == nullptr || comparison->op != wanted) {
    return nullptr;
  }
  auto pair = this->typeOfPair(comparison);
  bool isTypeOfTest = pair.typeOf != nullptr && pair.typeOf->op == UnaryOp::TypeOf &&
                      pair.text != nullptr && this->isSameRef(pair.typeOf->operand, tested);

  return isTypeOfTest ? pair.text : nullptr;
}

LibraryJsonRegistry::TypeOfPair
LibraryJsonRegistry::typeOfPair(const BinaryExpr *comparison) const {
  const auto *left = this->bare(comparison->lhs);
  const auto *right = this->bare(comparison->rhs);
  auto straight = TypeOfPair{
      .typeOf = dyn_cast<UnaryExpr>(left),
      .text = dyn_cast<StringLitExpr>(right),
  };
  if (straight.typeOf != nullptr && straight.text != nullptr) {
    return straight;
  }

  return TypeOfPair{
      .typeOf = dyn_cast<UnaryExpr>(right),
      .text = dyn_cast<StringLitExpr>(left),
  };
}

LibraryJsonRegistry::ResultTypeOfAgrees LibraryJsonRegistry::checkTypeOfAgrees(const Expr *tested,
                                                                               llvm::StringRef name,
                                                                               bool nullTest,
                                                                               bool isAnd) const {
  auto typeOf = TypeOfNameTable::find(name);
  bool isNullName = typeOf == TypeOfName::NULL_TYPE || typeOf == TypeOfName::UNDEFINED_TYPE;
  if (nullTest && !isNullName) {
    auto message = ErrorTextJsonTest::neverNull(this->refText(tested), isAnd, name);

    return err(LibraryJsonRegistryTypeOfAgreesError{
        .code = LibraryJsonRegistryTypeOfAgreesErrorCode::NEVER_NULL,
        .message = message,
    });
  }
  if (!nullTest && typeOf != TypeOfName::ARRAY) {
    auto message = ErrorTextJsonTest::neverArray(this->refText(tested), isAnd, name);

    return err(LibraryJsonRegistryTypeOfAgreesError{
        .code = LibraryJsonRegistryTypeOfAgreesErrorCode::NEVER_ARRAY,
        .message = message,
    });
  }

  return ok(tested);
}

LibraryJsonRegistry::ResultNullPossible
LibraryJsonRegistry::checkNullPossible(const Type *type, llvm::StringRef op) const {
  if (!type->isError() && !type->nonOptional()->isJson()) {
    auto message = ErrorTextJsonNull::never(type->name(), op);

    return err(LibraryJsonRegistryNullPossibleError{
        .code = LibraryJsonRegistryNullPossibleErrorCode::NEVER_NULL,
        .message = message,
    });
  }

  return ok(type);
}

LibraryJsonRegistry::ResultTestsUnknown LibraryJsonRegistry::checkTestsUnknown(const Type *type) {
  if (!type->isUnknown() && !type->nonOptional()->isParam() &&
      !this->state.generics.copies.isCheckingInstance()) {
    auto message = ErrorTextJsonIsJson::known(type->name(), type->nonOptional()->isJson());

    return err(LibraryJsonRegistryTestsUnknownError{
        .code = LibraryJsonRegistryTestsUnknownErrorCode::KNOWN_TYPE,
        .message = message,
    });
  }

  return ok(type);
}

void LibraryJsonRegistry::checkJsonMember(const CheckJsonMember &task) {
  return this->startJsonMember(task.member);
}

void LibraryJsonRegistry::startJsonMember(MemberExpr *member) {
  auto &tasks = this->state.walk.tasks;
  member->type = Type::optional(Type::json());
  tasks.pushChecked(member);
  const auto *narrowed = this->state.flow.tracked.narrowedFieldType(member);
  if (narrowed != nullptr) {
    return tasks.pushTask(Convert{.to = narrowed});
  }
  auto path = this->state.flow.tracked.fieldPath(member);
  bool isDefined = path.has_value() && this->state.walk.flow.isFieldDefined(*path);
  if (isDefined) {
    return tasks.pushTask(Convert{.to = Type::json()});
  }
}

void LibraryJsonRegistry::checkJsonIndex(const CheckJsonIndex &task) {
  this->state.walk.tasks.pushChecked(task.index);

  return this->startJsonIndex(task.index);
}

void LibraryJsonRegistry::startJsonIndex(IndexExpr *index) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterJsonIndexKey{.index = index});

  return tasks.pushTask(CheckExprSlot{.slot = &index->index, .expected = nullptr});
}

void LibraryJsonRegistry::afterJsonIndexKey(const AfterJsonIndexKey &task) {
  IndexExpr *index = task.index;
  index->type = Type::optional(Type::json());
  const Type *key = index->index->type;
  if (key->isError() || key->isString()) {
    return;
  }
  if (key->isNumeric()) {
    return this->state.walk.tasks.pushTask(
        ConvertSlot{.slot = &index->index, .to = Type::doubleTy()});
  }
  auto message = ErrorTextJsonKey::index(key->name());
  this->state.diagnostics.error(index->index->loc(), message);
  index->type = Type::error();
}

void LibraryJsonRegistry::checkJsonTarget(const CheckJsonTarget &task) {
  auto &tasks = this->state.walk.tasks;
  auto *index = dyn_cast<IndexExpr>(task.target);
  if (index != nullptr) {
    tasks.pushTask(AfterJsonTargetIndex{.index = index});

    return this->startJsonIndex(index);
  }
  tasks.pushTask(AfterJsonTargetMember{});

  return this->startJsonMember(cast<MemberExpr>(task.target));
}

void LibraryJsonRegistry::afterJsonTargetIndex(const AfterJsonTargetIndex &task) {
  bool isFailed = task.index->type->isError();
  const Type *type = isFailed ? Type::error() : Type::optional(Type::json());

  return this->state.walk.tasks.pushAnswer(type);
}

void LibraryJsonRegistry::afterJsonTargetMember() {
  auto &tasks = this->state.walk.tasks;
  tasks.takeChecked();

  return tasks.pushAnswer(Type::optional(Type::json()));
}

void LibraryJsonRegistry::checkJsonMethod(const CheckJsonMethod &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  const Expr *object = member->object;
  auto *conversion = dyn_cast<CastExpr>(object);
  if (conversion != nullptr && conversion->isImplicit()) {
    object = conversion->operand;
  }
  llvm::StringRef variable;
  auto *name = dyn_cast<NameExpr>(object);
  if (name != nullptr && name->var() != nullptr) {
    variable = name->name;
  }
  auto message = ErrorTextJsonMethod::none(member->name, variable);
  this->state.diagnostics.error(member->loc(), message);
  tasks.pushChecked(call);

  return tasks.pushTask(WalkArgsLoosely{.call = call});
}

void LibraryJsonRegistry::checkJsonObjectLit(const CheckJsonObjectLit &task) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(FinishJsonObjectLit{.literal = task.literal});

  return tasks.pushTask(NextJsonProperty{.literal = task.literal, .index = 0});
}

void LibraryJsonRegistry::finishJsonObjectLit(const FinishJsonObjectLit &task) {
  task.literal->type = Type::json();

  return this->state.walk.tasks.pushChecked(task.literal);
}

void LibraryJsonRegistry::nextJsonProperty(const NextJsonProperty &task) {
  ObjectLitExpr *literal = task.literal;
  size_t index = task.index;
  auto &tasks = this->state.walk.tasks;
  if (index == literal->props.size()) {
    return;
  }
  tasks.pushTask(NextJsonProperty{.literal = literal, .index = index + 1});
  ObjectLitExpr::Property &property = literal->props[index];
  if (property.spread() || property.method()) {
    std::string message =
        property.spread() ? ErrorTextJsonLiteral::spread() : ErrorTextJsonLiteral::function();
    this->state.diagnostics.error(property.loc, message);

    return tasks.pushTask(CheckExprSlot{.slot = &property.value, .expected = nullptr});
  }
  if (property.defaulted()) {
    auto message = ErrorTextJsonLiteral::defaulted(property.name);
    this->state.diagnostics.error(property.loc, message);
    property.value = cast<AssignExpr>(property.value)->value;
    property.form = ValueProperty{};

    return tasks.pushTask(CheckExprSlot{.slot = &property.value, .expected = nullptr});
  }
  tasks.pushTask(AfterJsonPropertyValue{.literal = literal, .index = index});
  tasks.pushTask(CheckExprSlot{.slot = &property.value, .expected = Type::json()});
  auto *computed = property.form.as<ComputedProperty>();
  if (computed == nullptr) {
    return;
  }
  tasks.pushTask(AfterJsonKey{.literal = literal, .index = index});

  return tasks.pushTask(CheckExprSlot{.slot = &computed->key, .expected = nullptr});
}

void LibraryJsonRegistry::afterJsonKey(const AfterJsonKey &task) {
  const ObjectLitExpr::Property &property = task.literal->props[task.index];
  const Type *keyType = property.key()->type;
  if (!keyType->isString() && !keyType->isError()) {
    auto message = ErrorTextJsonKey::literal(keyType->name());
    this->state.diagnostics.error(property.key()->loc(), message);
  }
}

void LibraryJsonRegistry::afterJsonPropertyValue(const AfterJsonPropertyValue &task) {
  ObjectLitExpr::Property &property = task.literal->props[task.index];
  const auto *json = Type::json();
  const Type *type = property.value->type;
  auto label = ErrorTextJsonKey::label(property.name);
  const Type *target = type->isOptional() || type->isUndefined() ? Type::optional(json) : json;

  return this->state.walk.tasks.pushTask(
      CoerceSlot{.slot = &property.value, .to = target, .what = label});
}

void LibraryJsonRegistry::checkJsonArrayLit(const CheckJsonArrayLit &task) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterJsonArrayLit{});

  return tasks.pushTask(
      CheckArrayLiteral{.literal = task.literal, .expected = Type::array(Type::json())});
}

void LibraryJsonRegistry::afterJsonArrayLit() {
  auto &tasks = this->state.walk.tasks;
  Expr *array = tasks.takeChecked();
  tasks.pushChecked(array);
  if (array->type->isError()) {
    return;
  }

  return tasks.pushTask(Coerce{.to = Type::json(), .what = ""});
}

void LibraryJsonRegistry::checkNullEquality(const CheckNullEquality &task) {
  BinaryExpr *binary = task.binary;
  auto &tasks = this->state.walk.tasks;
  auto &json = this->state.library.json;
  bool isLhsNull = json.isNullName(binary->lhs);
  bool isRhsNull = json.isNullName(binary->rhs);
  binary->type = Type::boolTy();
  tasks.pushChecked(binary);
  tasks.pushTask(AfterNullEquality{.binary = binary});
  if (isLhsNull && isRhsNull) {
    return;
  }
  Expr *&other = isLhsNull ? binary->rhs : binary->lhs;

  return tasks.pushTask(CheckExprSlot{.slot = &other, .expected = nullptr});
}

void LibraryJsonRegistry::afterNullEquality(const AfterNullEquality &task) {
  BinaryExpr *binary = task.binary;
  auto &json = this->state.library.json;
  bool isLhsNull = json.isNullName(binary->lhs);
  bool isRhsNull = json.isNullName(binary->rhs);
  Expr *null = isLhsNull ? binary->lhs : binary->rhs;
  const Expr *other = isLhsNull ? binary->rhs : binary->lhs;
  const Type *type = isLhsNull && isRhsNull ? Type::undefinedTy() : other->type;
  null->type = Type::json();
  auto possible = this->checkNullPossible(type, Operators::opSpelling(binary->op));
  if (possible.isErr()) {
    this->state.diagnostics.error(null->loc(), possible.error().message);
    null->type = Type::error();
  }
}

void LibraryJsonRegistry::checkJsonEquality(const CheckJsonEquality &task) {
  BinaryExpr *binary = task.binary;
  auto &tasks = this->state.walk.tasks;
  tasks.pushChecked(binary);
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  bool isLhsJson = leftType->nonOptional()->isJson();
  bool isRhsJson = rightType->nonOptional()->isJson();
  bool isOptional = leftType->isOptional() || rightType->isOptional();
  const Type *json = isOptional ? Type::optional(Type::json()) : Type::json();
  Expr *&other = isLhsJson ? binary->rhs : binary->lhs;
  const Type *type = other->type;
  bool isOptionalJson = type->isOptional() && this->state.types.relations.isImplicitlyConvertible(
                                                  type->inner(), Type::json());
  if ((isLhsJson && isRhsJson) || isOptionalJson) {
    tasks.pushTask(ConvertSlot{.slot = &binary->rhs, .to = json});

    return tasks.pushTask(ConvertSlot{.slot = &binary->lhs, .to = json});
  }
  if (type->isString() || type->isBool()) {
    return;
  }
  if (type->isNumeric()) {
    return tasks.pushTask(ConvertSlot{.slot = &other, .to = Type::doubleTy()});
  }
  const char *spelling = Operators::opSpelling(binary->op);
  bool isUndefined = leftType->isOptional() || rightType->isOptional() || leftType->isUndefined() ||
                     rightType->isUndefined();
  auto message =
      ErrorTextOperands::types(spelling, leftType->name(), rightType->name(), isUndefined);
  this->state.diagnostics.error(binary->loc(), message);
}

void LibraryJsonRegistry::checkJsonIsJson(const CheckJsonIsJson &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  call->type = Type::error();
  tasks.pushChecked(call);
  if (this->state.library.common.hasSpread(call)) {
    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  auto arity = this->state.library.common.checkArity(call, 1, 1);
  if (arity.isErr()) {
    return this->state.diagnostics.error(task.member->loc(), arity.error().message);
  }
  tasks.pushTask(AfterJsonIsJsonArg{.call = call});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = nullptr});
}

void LibraryJsonRegistry::afterJsonIsJsonArg(const AfterJsonIsJsonArg &task) {
  CallExpr *call = task.call;
  const Type *type = call->args[0]->type;
  if (type->isError()) {
    return;
  }
  auto tests = this->checkTestsUnknown(type);
  if (tests.isErr()) {
    return this->state.diagnostics.error(call->args[0]->loc(), tests.error().message);
  }
  call->target = BuiltinCall{.builtin = Builtin::JsonIsJson};
  call->type = Type::boolTy();
}

void LibraryJsonRegistry::checkArrayIsArray(const CheckArrayIsArray &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  call->type = Type::error();
  tasks.pushChecked(call);
  if (this->state.library.common.hasSpread(call)) {
    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  auto arity = this->state.library.common.checkArity(call, 1, 1);
  if (arity.isErr()) {
    return this->state.diagnostics.error(task.member->loc(), arity.error().message);
  }
  tasks.pushTask(AfterArrayIsArrayArg{.call = call});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = nullptr});
}

void LibraryJsonRegistry::afterArrayIsArrayArg(const AfterArrayIsArrayArg &task) {
  CallExpr *call = task.call;
  if (call->args[0]->type->isVoid()) {
    auto message = ErrorTextTruthy::voidValue();

    return this->state.diagnostics.error(call->args[0]->loc(), message);
  }
  call->target = BuiltinCall{.builtin = Builtin::ArrayIsArray};
  call->type = Type::boolTy();
}

bool LibraryJsonRegistry::canHoldItself(const Type *type) const {
  auto open = std::vector<const Type *>();
  std::vector<HeldTypeStep> steps = {EnterHeldType{type}};
  while (!steps.empty()) {
    HeldTypeStep step = steps.back();
    steps.pop_back();
    if (step.is<LeaveHeldType>()) {
      open.pop_back();
    } else if (this->enterHeldType(step.as<EnterHeldType>()->type, open, steps)) {
      return true;
    }
  }

  return false;
}

bool LibraryJsonRegistry::enterHeldType(const Type *type, std::vector<const Type *> &open,
                                        std::vector<HeldTypeStep> &steps) const {
  const Type *plain = type->nonOptional();
  if (plain->isParam() || plain->isAnyObject()) {
    return false;
  }
  if (plain->isJson() || (plain->isBase() && plain->classDecl() == nullptr) || plain->isUnknown() ||
      llvm::is_contained(open, plain)) {
    return true;
  }
  open.push_back(plain);
  steps.push_back(LeaveHeldType{});
  auto parts = this->heldParts(plain);
  for (const Type *part : llvm::reverse(parts)) {
    steps.push_back(EnterHeldType{part});
  }

  return false;
}

std::vector<const Type *> LibraryJsonRegistry::heldParts(const Type *type) const {
  const ClassDecl *classDecl = type->classDecl();
  if (classDecl != nullptr) {
    return this->familyFields(classDecl);
  }
  if (type->isArray()) {
    return {type->inner()};
  }
  if (type->isResult()) {
    return {type->okType(), type->errType()};
  }
  if (type->isDict()) {
    return {type->valueType()};
  }
  if (type->isObject()) {
    return this->fieldTypes(type);
  }
  if (type->isUnion()) {
    return std::vector<const Type *>(type->members().begin(), type->members().end());
  }

  return {};
}

std::vector<const Type *> LibraryJsonRegistry::familyFields(const ClassDecl *classDecl) const {
  std::vector<const ClassDecl *> family{classDecl};
  for (size_t index = 0; index < family.size(); ++index) {
    family.insert(family.end(), family[index]->children.begin(), family[index]->children.end());
  }
  auto parts = std::vector<const Type *>();
  for (const ClassDecl *member : family) {
    if (member->type != nullptr) {
      auto fields = this->fieldTypes(member->type);
      parts.insert(parts.end(), fields.begin(), fields.end());
    }
  }

  return parts;
}

std::vector<const Type *> LibraryJsonRegistry::fieldTypes(const Type *type) const {
  auto types = std::vector<const Type *>();
  for (const Field &field : type->fields()) {
    types.push_back(field.type);
  }

  return types;
}

const ClassDecl *LibraryJsonRegistry::classWithToJson(const Type *type) const {
  auto parts = TypeParts::collect(type, kToJsonRoute);
  for (const Type *part : parts.getTypes()) {
    const ClassDecl *classDecl = part->classDecl();
    if (classDecl != nullptr && this->hasToJson(classDecl)) {
      return classDecl;
    }
  }

  return nullptr;
}

bool LibraryJsonRegistry::hasToJson(const ClassDecl *classDecl) const {
  for (const FunctionDecl *functionDecl : classDecl->methods) {
    if (JsonMemberTable::find(functionDecl->name) == JsonMember::TO_JSON &&
        !functionDecl->isStatic()) {
      return true;
    }
  }

  return false;
}

bool LibraryJsonRegistry::canStringifyToUndefined(const Type *type) const {
  return type->isUndefined() || type->isOptional() || type->isFunction() || type->isNever() ||
         type->isUnknown();
}

void LibraryJsonRegistry::checkJsonStatic(const CheckJsonStatic &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  call->type = Type::error();
  tasks.pushChecked(call);
  if (this->state.library.common.hasSpread(call)) {
    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  auto function = JsonStaticTable::find(member->name);
  if (function == JsonStatic::PARSE) {
    return this->checkJsonParse(call, member);
  }
  if (!member->typeArgs.empty()) {
    auto message = ErrorTextJsonStatic::typeArgs(member->name);
    this->state.diagnostics.error(member->loc(), message);
  }
  if (function != JsonStatic::STRINGIFY) {
    auto message = ErrorTextJsonStatic::noMember(member->name);
    this->state.diagnostics.error(member->loc(), message);

    return this->walkNonNullArgs(call);
  }

  return this->checkStringify(call, member);
}

void LibraryJsonRegistry::walkNonNullArgs(CallExpr *call) {
  for (Expr *&arg : llvm::reverse(call->args)) {
    if (!this->state.library.json.isNullName(arg)) {
      this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &arg, .expected = nullptr});
    }
  }
}

void LibraryJsonRegistry::checkJsonParse(CallExpr *call, const MemberExpr *member) {
  auto &tasks = this->state.walk.tasks;
  if (call->args.size() == 2) {
    auto message = ErrorTextJsonStatic::reviver();
    this->state.diagnostics.error(call->args[1]->loc(), message);

    return this->walkNonNullArgs(call);
  }
  if (!member->typeArgs.empty()) {
    auto message = ErrorTextJsonStatic::parseTypes();
    this->state.diagnostics.error(member->loc(), message);

    return this->walkNonNullArgs(call);
  }
  auto arity = this->state.library.common.checkArity(call, 1, 1);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  tasks.pushTask(FinishJsonParse{.call = call});
  tasks.pushTask(PreludeTypeOf{.type = PreludeType::JSON_ERROR});

  return tasks.pushTask(CheckArgSlot{.slot = &call->args[0], .to = Type::string()});
}

void LibraryJsonRegistry::finishJsonParse(const FinishJsonParse &task) {
  CallExpr *call = task.call;
  const Type *failure = this->state.walk.tasks.takeAnswer();
  this->walkConstantJson(call->args[0]);
  call->target = BuiltinCall{.builtin = Builtin::JsonParse};
  call->type = Type::result(Type::json(), failure);
}

void LibraryJsonRegistry::checkStringify(CallExpr *call, const MemberExpr *member) {
  auto &tasks = this->state.walk.tasks;
  auto arity = this->state.library.common.checkArity(call, 1, 3);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  tasks.pushTask(AfterStringifyValue{.call = call});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = nullptr});
}

void LibraryJsonRegistry::afterStringifyValue(const AfterStringifyValue &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  bool isOk = this->matchStringifiedValue(call->args[0]);
  Expr *replacer = call->args.size() < 2 ? nullptr : call->args[1];
  if (!this->isGivenOption(replacer)) {
    return this->checkStringifySpace(call, isOk);
  }
  tasks.pushTask(AfterStringifyReplacer{.call = call, .isOk = isOk});

  return tasks.pushTask(CheckExpr{.expr = replacer, .expected = nullptr});
}

void LibraryJsonRegistry::afterStringifyReplacer(const AfterStringifyReplacer &task) {
  Expr *checked = this->state.walk.tasks.takeChecked();
  if (!checked->type->isError()) {
    auto message = ErrorTextJsonStatic::replacer();
    this->state.diagnostics.error(checked->loc(), message);
  }

  return this->checkStringifySpace(task.call, false);
}

void LibraryJsonRegistry::checkStringifySpace(CallExpr *call, bool isOk) {
  auto &tasks = this->state.walk.tasks;
  Expr *space = call->args.size() != 3 ? nullptr : call->args[2];
  if (!this->isGivenOption(space)) {
    return this->finishStringify(FinishStringify{.call = call, .isOk = isOk, .hasSpace = false});
  }
  tasks.pushTask(AfterStringifySpace{.call = call, .isOk = isOk});

  return tasks.pushTask(CheckExpr{.expr = space, .expected = nullptr});
}

void LibraryJsonRegistry::afterStringifySpace(const AfterStringifySpace &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  Expr *checked = tasks.takeChecked();
  const Type *type = checked->type;
  if (type->isNumeric()) {
    tasks.pushChecked(checked);
    tasks.pushTask(FinishStringify{.call = call, .isOk = task.isOk, .hasSpace = true});

    return tasks.pushTask(Convert{.to = Type::doubleTy()});
  }
  if (type->isString()) {
    tasks.pushChecked(checked);

    return tasks.pushTask(FinishStringify{.call = call, .isOk = task.isOk, .hasSpace = true});
  }
  if (!type->isError()) {
    auto message = ErrorTextJsonStatic::space(type->name());
    this->state.diagnostics.error(checked->loc(), message);
  }
  bool isOk = task.isOk && type->isError();

  return this->finishStringify(FinishStringify{.call = call, .isOk = isOk, .hasSpace = false});
}

void LibraryJsonRegistry::finishStringify(const FinishStringify &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  Expr *space = task.hasSpace ? tasks.takeChecked() : nullptr;
  if (!task.isOk) {
    return;
  }
  Expr *value = call->args[0];
  call->args = {value};
  if (space != nullptr) {
    call->args.push_back(space);
  }
  call->target = BuiltinCall{.builtin = Builtin::JsonStringify};
  call->type =
      this->canStringifyToUndefined(value->type) ? Type::optional(Type::string()) : Type::string();
  bool isCyclic = !this->state.library.declarations.isInsideLibrary(call->loc()) &&
                  this->canHoldItself(value->type);
  if (!isCyclic) {
    return;
  }
  tasks.pushTask(AfterStringifyFailure{.call = call});

  return tasks.pushTask(PreludeTypeOf{.type = PreludeType::JSON_ERROR});
}

void LibraryJsonRegistry::afterStringifyFailure(const AfterStringifyFailure &task) {
  const Type *failure = this->state.walk.tasks.takeAnswer();
  task.call->type = Type::result(task.call->type, failure);
}

bool LibraryJsonRegistry::isGivenOption(const Expr *option) const {
  if (option == nullptr || isa<UndefinedExpr>(option)) {
    return false;
  }

  return !this->state.library.json.isNullName(option);
}

bool LibraryJsonRegistry::matchStringifiedValue(const Expr *value) {
  bool isOk = !value->type->isError();
  if (value->type->isVoid()) {
    auto message = ErrorTextJsonStatic::voidValue();
    this->state.diagnostics.error(value->loc(), message);
    isOk = false;
  }
  const ClassDecl *classDecl = isOk ? this->classWithToJson(value->type) : nullptr;
  if (classDecl != nullptr) {
    auto message = ErrorTextJsonStatic::toJson(classDecl->name);
    this->state.diagnostics.error(value->loc(), message);
    isOk = false;
  }

  return isOk;
}

std::optional<std::u16string> LibraryJsonRegistry::constantPath(const Expr *arg) const {
  const auto *literal = dyn_cast<StringLitExpr>(arg);
  if (literal != nullptr) {
    return literal->value;
  }
  const auto *templateExpr = dyn_cast<TemplateExpr>(arg);
  if (templateExpr != nullptr && templateExpr->exprs.empty()) {
    return templateExpr->quasis[0];
  }

  return std::nullopt;
}

std::string LibraryJsonRegistry::embeddedFullPath(const CallExpr *call,
                                                  llvm::StringRef shown) const {
  llvm::SourceMgr &sources = this->state.diagnostics.sourceMgr();
  unsigned id = sources.FindBufferContainingLoc(call->loc());
  llvm::StringRef source = id != 0 ? sources.getMemoryBuffer(id)->getBufferIdentifier() : "";
  llvm::SmallString<256> nativePath(llvm::sys::path::parent_path(source));
  llvm::sys::path::append(nativePath, shown);
  llvm::sys::path::remove_dots(nativePath, true);
  llvm::sys::path::native(nativePath);

  return std::string(nativePath);
}

void LibraryJsonRegistry::walkConstantJson(const Expr *arg) {
  const auto *bare = ExprParts::withoutImplicitCasts(arg);
  const auto *literal = llvm::dyn_cast<StringLitExpr>(bare);
  if (literal == nullptr) {
    return;
  }
  auto text = ToUtf8::toUtf8(literal->value);
  auto json = this->checkConstantJson(text);
  if (json.isErr()) {
    this->state.diagnostics.error(literal->loc(), json.error().message);
  }
}

LibraryJsonRegistry::ResultConstantJson
LibraryJsonRegistry::checkConstantJson(llvm::StringRef text) const {
  if (text.starts_with("\xEF\xBB\xBF")) {
    auto message = ErrorTextConstantJson::bom();

    return err(LibraryJsonRegistryConstantJsonError{
        .code = LibraryJsonRegistryConstantJsonErrorCode::BYTE_ORDER_MARK,
        .message = message,
    });
  }
  pkg::JsonProblem problem = pkg::JsonText::checkJsonText(text);
  if (problem.message.empty()) {
    return ok(text);
  }
  size_t line = 1;
  size_t column = 1;
  for (size_t index = 0; index < problem.offset && index < text.size(); ++index) {
    if (text[index] == '\n') {
      ++line;
      column = 1;
    } else if ((static_cast<unsigned char>(text[index]) & 0xC0) != 0x80) {
      ++column;
    }
  }
  auto message = ErrorTextConstantJson::invalid(problem.message, line, column);

  return err(LibraryJsonRegistryConstantJsonError{
      .code = LibraryJsonRegistryConstantJsonErrorCode::INVALID,
      .message = message,
  });
}

void LibraryJsonRegistry::checkJsonGlobal(const CheckJsonGlobal &task) {
  CallExpr *call = task.call;
  NameExpr *name = task.name;
  auto &tasks = this->state.walk.tasks;
  bool isEmbedingText = this->state.isGlobalName(name, GlobalName::EMBED_TEXT);
  bool isEmbedingBytes = this->state.isGlobalName(name, GlobalName::EMBED_BYTES);
  if (!isEmbedingText && !isEmbedingBytes) {
    return tasks.pushChecked(nullptr);
  }
  llvm::StringRef functionName = name->name;
  call->type = Type::error();
  if (this->state.library.common.hasSpread(call)) {
    tasks.pushChecked(call);

    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  if (!name->typeArgs.empty()) {
    auto message = ErrorTextEmbed::typeArgs(functionName);
    this->state.diagnostics.error(name->loc(), message);
  }
  auto arity = this->state.library.common.checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.diagnostics.error(name->loc(), arity.error().message);

    return tasks.pushChecked(call);
  }
  auto embedded = this->checkEmbeddedFile(call, functionName);
  if (embedded.isErr()) {
    tasks.pushChecked(call);

    return this->reportEmbedFailure(call, embedded.error());
  }
  if (isEmbedingBytes) {
    auto *bytes = this->embedBytes(call, embedded.value());

    return tasks.pushChecked(bytes);
  }
  auto *text = this->embedText(call, embedded.value());

  return tasks.pushChecked(text);
}

void LibraryJsonRegistry::reportEmbedFailure(CallExpr *call,
                                             const LibraryJsonRegistryEmbeddedFileError &error) {
  auto &tasks = this->state.walk.tasks;
  llvm::SMLoc loc = call->args[0]->loc();
  if (error.code != LibraryJsonRegistryEmbeddedFileErrorCode::NOT_CONSTANT) {
    return this->state.diagnostics.error(loc, error.message);
  }
  tasks.pushTask(ReportError{.loc = loc, .message = error.message});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = nullptr});
}

Expr *LibraryJsonRegistry::embedText(CallExpr *call, const EmbeddedFile &embedded) {
  llvm::StringRef text(embedded.bytes);
  auto units = llvm::SmallVector<llvm::UTF16, 64>();
  if (!llvm::convertUTF8ToUTF16String(text, units)) {
    auto message = ErrorTextEmbed::notUtf8(embedded.shown);
    this->state.diagnostics.error(call->args[0]->loc(), message);

    return call;
  }
  std::u16string content(units.begin(), units.end());
  auto *literal = this->state.ast.make<StringLitExpr>(call->loc(), std::move(content));
  literal->type = Type::string();

  return literal;
}

Expr *LibraryJsonRegistry::embedBytes(CallExpr *call, const EmbeddedFile &embedded) {
  const std::string &bytes = embedded.bytes;
  if (int64_t(bytes.size()) > Type::kMaxLength) {
    auto message = ErrorTextEmbed::tooLarge(embedded.shown);
    this->state.diagnostics.error(call->args[0]->loc(), message);

    return call;
  }
  llvm::SMLoc loc = call->args[0]->loc();
  std::u16string units(bytes.size(), u'\0');
  for (size_t index = 0; index < bytes.size(); ++index) {
    units[index] = char16_t(static_cast<unsigned char>(bytes[index]));
  }
  auto *literal = this->state.ast.make<StringLitExpr>(loc, std::move(units));
  literal->type = Type::string();
  auto path = llvm::SmallVector<llvm::UTF16, 64>();
  llvm::convertUTF8ToUTF16String(embedded.full, path);
  auto *file = this->state.ast.make<StringLitExpr>(loc, std::u16string(path.begin(), path.end()));
  file->type = Type::string();
  call->args = {literal, file};
  call->target = BuiltinCall{.builtin = Builtin::EmbedBytes};
  call->type = Type::bytesType(Type::Bytes::Uint8);

  return call;
}

LibraryJsonRegistry::ResultEmbeddedFile
LibraryJsonRegistry::checkEmbeddedFile(const CallExpr *call, llvm::StringRef functionName) const {
  const Expr *arg = call->args[0];
  auto path = this->constantPath(arg);
  if (!path.has_value()) {
    auto example = ErrorTextEmbed::example(functionName);
    auto message = ErrorTextEmbed::literal(functionName, example);

    return err(LibraryJsonRegistryEmbeddedFileError{
        .code = LibraryJsonRegistryEmbeddedFileErrorCode::NOT_CONSTANT,
        .message = message,
    });
  }
  auto shown = ToUtf8::toUtf8(path.value());
  if (llvm::StringRef(shown).contains('\\')) {
    auto message = ErrorTextEmbed::backslash();

    return err(LibraryJsonRegistryEmbeddedFileError{
        .code = LibraryJsonRegistryEmbeddedFileErrorCode::BACKSLASH,
        .message = message,
    });
  }
  bool isRelative = !shown.empty() && !llvm::sys::path::is_absolute(shown) && shown[0] != '/';
  if (!isRelative) {
    auto example = ErrorTextEmbed::example(functionName);
    auto message = ErrorTextEmbed::relative(functionName, example);

    return err(LibraryJsonRegistryEmbeddedFileError{
        .code = LibraryJsonRegistryEmbeddedFileErrorCode::NOT_RELATIVE,
        .message = message,
    });
  }
  auto full = this->embeddedFullPath(call, shown);
  auto buffer = llvm::MemoryBuffer::getFile(full, false, false);
  if (!buffer) {
    std::string why = buffer.getError().message();
    auto message = ErrorTextEmbed::unread(shown, why);

    return err(LibraryJsonRegistryEmbeddedFileError{
        .code = LibraryJsonRegistryEmbeddedFileErrorCode::UNREADABLE,
        .message = message,
    });
  }
  auto bytes = (*buffer)->getBuffer().str();

  return ok(EmbeddedFile{.bytes = bytes, .shown = shown, .full = full});
}

}
