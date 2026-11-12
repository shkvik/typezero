#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/bodies/classes/class_links/class_links.h"
#include "compiler/semantic/bodies/classes/class_value_checker/class_value_checker.h"
#include "compiler/semantic/bodies/functions/function_value_checker/function_value_checker.h"
#include "compiler/semantic/bodies/iterators/iterable_checker/iterable_checker.h"
#include "compiler/semantic/bodies/objects/object_checker/object_checker.h"
#include "compiler/semantic/declare/constant_folder/constant_folder.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/generics/generic_widening/generic_widening.h"
#include "compiler/semantic/generics/widening_relations/widening_relations.h"
#include "compiler/semantic/library/bytes/byte_proofs/byte_proofs.h"
#include "compiler/semantic/library/reflect/reflect_checker/reflect_checker.h"
#include "compiler/semantic/library/result/result_types/result_types.h"
#include "compiler/semantic/library/tz_modules/tz_modules_checker/tz_modules_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/conditional_types/conditional_types.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/semantic/types/type_parts/type_routes.h"
#include "compiler/semantic/types/unknown_checker/unknown_checker.h"
#include "llvm/ADT/STLExtras.h"
#include <cctype>
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr double kMaxExactInteger = 9007199254740992.0;
constexpr size_t kManyLiteralMembers = 64;
constexpr llvm::StringLiteral kMatchIndices = "indices";
constexpr llvm::StringLiteral kHiddenNamePrefix = "%";
constexpr llvm::StringLiteral kHttpStatusType = "HttpStatus";
constexpr llvm::StringLiteral kWebStreamClass = "ReadableStream";
constexpr llvm::StringLiteral kFormDataClass = "FormData";
constexpr llvm::StringLiteral kRequestInitType = "RequestInit";
constexpr llvm::StringLiteral kRequestOptionsType = "RequestOptions";

}

TypeRelations::TypeRelations(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->byteProofs = &all.byteProofs;
  this->literalTypes = &all.literalTypes;
  this->objectChecker = &all.objectChecker;
  this->constantFolder = &all.constantFolder;
  this->genericChecker = &all.genericChecker;
  this->reflectChecker = &all.reflectChecker;
  this->unknownChecker = &all.unknownChecker;
  this->iterableChecker = &all.iterableChecker;
  this->genericWidening = &all.genericWidening;
  this->conditionalTypes = &all.conditionalTypes;
  this->tzModulesChecker = &all.tzModulesChecker;
  this->wideningRelations = &all.wideningRelations;
  this->functionValueChecker = &all.functionValueChecker;
}

bool TypeRelations::isHoldingWideInteger(const Type *type) {
  auto parts = TypeParts::collect(type, kWideIntegerRoute);
  for (const Type *part : parts.getTypes()) {
    if (TypeRelations::isWideInteger(part)) {
      return true;
    }
  }

  return false;
}

bool TypeRelations::isWideInteger(const Type *type) {
  if (!type->isInteger()) {
    return false;
  }

  return double(type->intMax()) > kMaxExactInteger || double(type->intMin()) < -kMaxExactInteger;
}

bool TypeRelations::isCopyingIntoRecord(const Type *from, const Type *to) {
  if (!TypeRelations::hasRecordCopyShape(from, to)) {
    return false;
  }
  const Type *value = to->valueType();
  for (const Field &field : from->fields()) {
    const auto *type = TypeRelations::copiedFieldType(field, value);
    if (type->isFunction() || !TypeRelations::isImplicitlyConvertible(type, value)) {
      return false;
    }
  }

  return true;
}

bool TypeRelations::hasRecordCopyShape(const Type *from, const Type *to) {
  return to->isRecord() && to->keyType()->isString() && from->isObject() && !from->isClass() &&
         !from->isTuple() && !from->isAnyObject() && !from->isIncomplete() &&
         from->bases().empty() && from->overloads().empty() && !from->isRegExp() &&
         !from->isMatch() && !from->isDate() && !from->isIntl() && !from->isCrypto() &&
         !from->isWeakRef() && !from->isBytes() && !from->isAsyncIterator();
}

const Type *TypeRelations::copiedFieldType(const Field &field, const Type *value) {
  if (field.type->isOptional() && !value->isOptional()) {
    return field.type->inner();
  }

  return field.type;
}

ConversionStep TypeRelations::recordCopyStep(const Type *from, const Type *to) {
  const Type *value = to->valueType();
  auto fields = std::vector<ConversionGoal>();
  for (const Field &field : from->fields()) {
    const auto *type = TypeRelations::copiedFieldType(field, value);
    if (type->isFunction()) {
      return TypeRelations::stepFails();
    }
    fields.push_back(ConversionGoal{.from = type, .to = value});
  }

  return TypeRelations::stepNeeds(std::move(fields));
}

bool TypeRelations::isImplicitlyConvertible(const Type *from, const Type *to) {
  auto first = ConversionGoal{.from = from, .to = to};
  auto tasks = std::vector<ConversionTask>{first};
  while (!tasks.empty()) {
    ConversionTask task = std::move(tasks.back());
    tasks.pop_back();
    auto step = TypeRelations::taskStep(task);
    if (step.isPossible) {
      tasks.insert(tasks.end(), step.needs.rbegin(), step.needs.rend());
    } else if (!TypeRelations::canRetryMember(tasks)) {
      return false;
    }
  }

  return true;
}

ConversionStep TypeRelations::taskStep(const ConversionTask &task) {
  switch (task.kind()) {
    case ConversionTaskKind::GOAL:
      return TypeRelations::conversionStep(task.as<ConversionGoal>()->from,
                                           task.as<ConversionGoal>()->to);
    case ConversionTaskKind::MEMBER:
      return TypeRelations::memberStep(*task.as<MemberGoal>());
    case ConversionTaskKind::CHOICE:
      return TypeRelations::stepHolds();
  }
  llvm_unreachable("unknown conversion task");
}

ConversionStep TypeRelations::memberGoals(const Type *from, const Type *to) {
  const Type *source = from;
  llvm::ArrayRef<const Type *> members =
      source->isUnion() ? source->members() : llvm::ArrayRef<const Type *>(source);
  auto needs = std::vector<ConversionTask>();
  for (const Type *member : members) {
    needs.push_back(MemberGoal{.from = member, .to = to});
  }

  return ConversionStep{.isPossible = true, .needs = std::move(needs)};
}

ConversionStep TypeRelations::memberStep(const MemberGoal &goal) {
  if (LiteralTypes::exactMemberFor(goal.from, goal.to) != nullptr) {
    return TypeRelations::stepHolds();
  }

  return TypeRelations::memberTrial(MemberChoice{.from = goal.from, .to = goal.to, .next = 0});
}

ConversionStep TypeRelations::memberTrial(MemberChoice choice) {
  llvm::ArrayRef<const Type *> members = choice.to->members();
  while (choice.next < members.size()) {
    const Type *member = members[choice.next];
    choice.next += 1;
    if (!member->isParam() && !choice.from->isParam()) {
      auto goal = ConversionGoal{.from = choice.from, .to = member};

      return ConversionStep{.isPossible = true, .needs = {goal, choice}};
    }
  }

  return TypeRelations::stepFails();
}

bool TypeRelations::canRetryMember(std::vector<ConversionTask> &tasks) {
  while (!tasks.empty()) {
    ConversionTask task = std::move(tasks.back());
    tasks.pop_back();
    const MemberChoice *choice = task.as<MemberChoice>();
    if (choice != nullptr && TypeRelations::canTryNextMember(*choice, tasks)) {
      return true;
    }
  }

  return false;
}

bool TypeRelations::canTryNextMember(const MemberChoice &choice,
                                     std::vector<ConversionTask> &tasks) {
  auto trial = TypeRelations::memberTrial(choice);
  if (!trial.isPossible) {
    return false;
  }
  tasks.insert(tasks.end(), trial.needs.rbegin(), trial.needs.rend());

  return true;
}

ConversionStep TypeRelations::conversionStep(const Type *from, const Type *to) {
  if (from == to || from->isNever()) {
    return TypeRelations::stepHolds();
  }
  auto byTarget = TypeRelations::targetStep(from, to);
  if (byTarget.has_value()) {
    return *byTarget;
  }
  auto bySource = TypeRelations::sourceStep(from, to);
  if (bySource.has_value()) {
    return *bySource;
  }
  auto plain = TypeRelations::plainConvertible(from, to);
  if (plain.has_value()) {
    return TypeRelations::stepDecided(*plain);
  }
  if (TypeRelations::hasRecordCopyShape(from, to)) {
    return TypeRelations::recordCopyStep(from, to);
  }
  if (from->isResult() && to->isResult()) {
    return TypeRelations::resultStep(from, to);
  }

  return TypeRelations::stepFails();
}

std::optional<ConversionStep> TypeRelations::targetStep(const Type *from, const Type *to) {
  if (to->isUnknown()) {
    return TypeRelations::stepDecided(!from->isVoid());
  }
  if (to->isAnyObject()) {
    return TypeRelations::anyObjectStep(from, to);
  }
  if (to->isConstructor() && from->isClassRef()) {
    return TypeRelations::classRefStep(from, to);
  }
  if (to->isJson()) {
    return TypeRelations::jsonStep(from, to);
  }
  if (to->isOptional() && from->isUndefined()) {
    return TypeRelations::stepHolds();
  }
  if (to->isOptional()) {
    return TypeRelations::stepNeeds({{from->nonOptional(), to->inner()}});
  }
  if (to->isUnion()) {
    return TypeRelations::memberGoals(from, to);
  }

  return std::nullopt;
}

ConversionStep TypeRelations::anyObjectStep(const Type *from, const Type *to) {
  if (UnknownChecker::isObjectReference(from)) {
    return TypeRelations::stepHolds();
  }
  if (from->isParam() && from->constraint() != nullptr) {
    return TypeRelations::stepNeeds({{from->constraint(), to}});
  }

  return TypeRelations::stepFails();
}

ConversionStep TypeRelations::classRefStep(const Type *from, const Type *to) {
  if (from->classRef() != nullptr && from->classRef()->isGeneric()) {
    return TypeRelations::stepFails();
  }
  const auto *made = ClassValueChecker::constructedBy(from);
  if (made != nullptr) {
    return TypeRelations::stepNeeds({{made, to->constructed()}});
  }

  return TypeRelations::stepDecided(to->constructed()->isAnyObject());
}

std::optional<ConversionStep> TypeRelations::sourceStep(const Type *from, const Type *to) {
  if (from->isUnion()) {
    return TypeRelations::unionSourceStep(from, to);
  }
  if (to->isPattern()) {
    bool isMatching =
        from->isStringLiteral() && TemplatePatterns::matchesPattern(from->literalText(), to);
    return TypeRelations::stepDecided(isMatching);
  }
  if (from->isPattern()) {
    return TypeRelations::stepDecided(to == Type::string());
  }
  if (from->isLiteral() && to->isLiteral()) {
    return TypeRelations::stepFails();
  }
  if (from->isLiteral()) {
    return TypeRelations::stepNeeds({{from->literalBase(), to}});
  }
  if (from->isParam() && from->constraint() != nullptr) {
    return TypeRelations::stepNeeds({{from->constraint(), to}});
  }

  return std::nullopt;
}

ConversionStep TypeRelations::unionSourceStep(const Type *from, const Type *to) {
  if (from->isTagged() || UnionSlots::hasParamMember(from)) {
    return TypeRelations::stepFails();
  }
  auto members = std::vector<ConversionGoal>();
  for (const Type *member : from->members()) {
    members.push_back(ConversionGoal{.from = member, .to = to});
  }

  return TypeRelations::stepNeeds(std::move(members));
}

ConversionStep TypeRelations::stepHolds() {
  return ConversionStep{.isPossible = true, .needs = {}};
}

ConversionStep TypeRelations::stepFails() {
  return ConversionStep{.isPossible = false, .needs = {}};
}

ConversionStep TypeRelations::stepDecided(bool isPossible) {
  return ConversionStep{.isPossible = isPossible, .needs = {}};
}

ConversionStep TypeRelations::stepNeeds(std::vector<ConversionGoal> goals) {
  auto needs = std::vector<ConversionTask>(goals.begin(), goals.end());

  return ConversionStep{.isPossible = true, .needs = std::move(needs)};
}

ConversionStep TypeRelations::jsonStep(const Type *from, const Type *to) {
  bool isPlain = from->isString() || from->isNumeric() || from->isBool() || from->isLiteral() ||
                 (from->isArray() && from->inner()->isJson());
  if (isPlain) {
    return TypeRelations::stepHolds();
  }
  bool isRecord = from->isRecord() && from->keyType()->isString() &&
                  !TypeRelations::isHoldingWideInteger(from->valueType());
  if (!isRecord) {
    return TypeRelations::stepFails();
  }

  return TypeRelations::stepNeeds({{from->valueType()->nonOptional(), to}});
}

ConversionStep TypeRelations::resultStep(const Type *from, const Type *to) {
  auto halves = std::vector<ConversionGoal>{
      ConversionGoal{.from = from->okType(), .to = to->okType()},
      ConversionGoal{.from = from->errType(), .to = to->errType()},
  };
  auto needs = std::vector<ConversionGoal>();
  for (const ConversionGoal &half : halves) {
    bool isSettled = half.from == half.to || half.from->isNever();
    bool hasVoid = half.from->isVoid() || half.to->isVoid();
    if (!isSettled && hasVoid) {
      return TypeRelations::stepFails();
    }
    if (!isSettled) {
      needs.push_back(half);
    }
  }

  return TypeRelations::stepNeeds(std::move(needs));
}

std::optional<bool> TypeRelations::plainConvertible(const Type *from, const Type *to) {
  if (from->isArray() && to->isArray()) {
    return WideningRelations::isSameReference(from->inner(), to->inner());
  }
  if (from->isIterator() && to->isIterator()) {
    return from->yielded() == to->yielded() &&
           (from->returned() == to->returned() || to->returned()->isVoid()) &&
           (from->sent() == to->sent() || to->sent()->isVoid());
  }
  if (from->isPromise() || to->isPromise()) {
    return from->isPromise() && to->isPromise() && from->promised()->isNever();
  }
  if (from->isBytes() || to->isBytes()) {
    return from->isBuffer() && to->bytes() == Type::Bytes::Uint8;
  }
  if (TypeRelations::isRuntimeObject(from) || TypeRelations::isRuntimeObject(to)) {
    return from->isMatch() && to->isMatch() && to->matchGroups() < 0 &&
           from->field(kMatchIndices) == nullptr;
  }
  if (from->isObject() && to->isObject() && from->isSubtypeOf(to)) {
    return true;
  }
  if (GenericWidening::isWidening(from, to)) {
    return true;
  }
  if (TypeRelations::isSameKindOfShape(from, to)) {
    return TypeRelations::hasSameFields(from, to);
  }

  return std::nullopt;
}

bool TypeRelations::isRuntimeObject(const Type *type) {
  return type->isMatch() || type->isRegExp() || type->isDate() || type->isIntl() ||
         type->isCrypto() || type->isWeakRef();
}

bool TypeRelations::isSameKindOfShape(const Type *from, const Type *to) {
  bool isPlainPair = from->isObject() && to->isObject() && !from->isClass() && !to->isClass() &&
                     !from->isAnyObject() && !to->isAnyObject();
  bool isSameLayout =
      from->isTuple() == to->isTuple() && from->fields().size() == to->fields().size();

  return isPlainPair && isSameLayout && from->overloads().empty() && to->overloads().empty();
}

bool TypeRelations::hasSameFields(const Type *from, const Type *to) {
  for (const Field &field : from->fields()) {
    const Field *same = to->field(field.name);
    if (same == nullptr || same->type != field.type) {
      return false;
    }
  }

  return true;
}

const Type *TypeRelations::commonNumericType(const Type *first, const Type *second) {
  if (TypeRelations::isImplicitlyConvertible(first, second)) {
    return second;
  }
  if (TypeRelations::isImplicitlyConvertible(second, first)) {
    return first;
  }

  return nullptr;
}

bool TypeRelations::isIntLiteralTree(const Expr *expr) const {
  for (const Expr *node : ExprParts::operatorTree(expr)) {
    if (!this->isIntLiteralNode(node)) {
      return false;
    }
  }

  return true;
}

bool TypeRelations::isIntLiteralNode(const Expr *expr) const {
  auto *name = dyn_cast<NameExpr>(expr);
  auto *unary = dyn_cast<UnaryExpr>(expr);
  auto *binary = dyn_cast<BinaryExpr>(expr);

  if (isa<IntLitExpr>(expr)) {
    return true;
  }
  if (name != nullptr) {
    return name->var() != nullptr && name->var()->literalConst;
  }
  if (unary != nullptr) {
    return unary->op == UnaryOp::Neg || unary->op == UnaryOp::Plus;
  }
  if (binary != nullptr) {
    return binary->op == BinaryOp::Add || binary->op == BinaryOp::Sub ||
           binary->op == BinaryOp::Mul || binary->op == BinaryOp::Rem ||
           binary->op == BinaryOp::Pow;
  }

  return false;
}

const Type *TypeRelations::literalTarget(const Expr *expr, const Type *to) {
  const Type *target = this->numberTarget(to);
  bool isLiteral = target != nullptr && expr->type != nullptr && expr->type->isInteger() &&
                   expr->type != target && this->isIntLiteralTree(expr);
  if (!isLiteral) {
    return nullptr;
  }
  if (target->isFloat() || target->isDecimal()) {
    return target;
  }
  if (!target->isInteger()) {
    return nullptr;
  }
  auto value = this->constantFolder->intConstant(expr);
  if (!value.has_value() || !this->isValueFitting(*value, expr->type, target)) {
    return nullptr;
  }

  return target;
}

const Type *TypeRelations::numberTarget(const Type *to) const {
  const Type *target = to->nonOptional();
  if (!target->isUnion()) {
    return target;
  }

  return this->literalTypes->numberMemberOf(target, NumberSpelling::WHOLE);
}

bool TypeRelations::isValueFitting(int64_t value, const Type *from, const Type *target) const {
  if (value < 0 && !from->isUnsigned()) {
    return !target->isUnsigned() && value >= target->intMin();
  }

  return uint64_t(value) <= target->intMax();
}

bool TypeRelations::isLiteralAdapting(const Expr *expr, const Type *to) {
  return this->literalTarget(expr, to) != nullptr;
}

Expr *TypeRelations::adaptLiteral(Expr *expr, const Type *to) {
  const auto *target = this->literalTarget(expr, to);
  if (target == nullptr) {
    return expr;
  }
  auto *adapted = this->convert(expr, target);
  if (target == to) {
    return adapted;
  }

  return this->convert(adapted, to);
}

std::string TypeRelations::operandText(llvm::StringRef text) {
  bool isLiteral = !text.empty() && std::isdigit(static_cast<unsigned char>(text.front()));
  bool isPlain = TypeRelations::isPlainOperand(text) || TypeRelations::isWrappedInParens(text);
  if (isPlain && !isLiteral) {
    return text.str();
  }

  return "(" + text.str() + ")";
}

bool TypeRelations::isPlainOperand(llvm::StringRef text) {
  if (text.empty() || text.front() == '-' || text.front() == '+') {
    return false;
  }
  int depth = 0;
  for (char unit : text) {
    bool isOpening = unit == '(' || unit == '[';
    bool isClosing = unit == ')' || unit == ']';
    bool isBracket = isOpening || isClosing;
    if (isOpening) {
      depth += 1;
    }
    if (isClosing) {
      depth -= 1;
    }
    if (!isBracket && depth == 0 && !TypeRelations::isNameUnit(unit)) {
      return false;
    }
  }

  return true;
}

bool TypeRelations::isNameUnit(char unit) {
  bool isWordUnit = std::isalnum(static_cast<unsigned char>(unit)) != 0 || unit == '_';

  return isWordUnit || unit == '.' || unit == '#' || unit == '$' || unit == '"' || unit == '\'';
}

bool TypeRelations::isWrappedInParens(llvm::StringRef text) {
  if (text.size() < 2 || text.front() != '(' || text.back() != ')') {
    return false;
  }
  int level = 0;
  for (size_t index = 0; index < text.size(); ++index) {
    if (text[index] == '(') {
      level += 1;
    }
    if (text[index] == ')') {
      level -= 1;
    }
    if (level == 0 && index + 1 < text.size()) {
      return false;
    }
  }

  return true;
}

std::string TypeRelations::conversionHint(const Expr *expr, const Type *to) const {
  if (expr->type == nullptr) {
    return "";
  }
  const Type *from = expr->type->nonOptional();
  const Type *target = to->nonOptional();
  if (!TypeRelations::isNumberLike(from) || !TypeRelations::isNumberLike(target)) {
    return "";
  }
  auto text = this->shownOperand(expr);
  auto method = this->conversionMethod(target);
  bool isKnownTarget = target->isInteger() || target->isFloat() || target->isDecimal();
  if (from->isInteger() && isKnownTarget) {
    return ErrorTextConvert::direct(text, method);
  }
  if (from->isFloat() && target->isInteger()) {
    return ErrorTextConvert::rounded(text, method);
  }

  return "";
}

bool TypeRelations::isNumberLike(const Type *type) {
  return type->isNumeric() || type->isDecimal();
}

std::string TypeRelations::shownOperand(const Expr *expr) const {
  auto *name = dyn_cast<NameExpr>(expr);
  auto *member = dyn_cast<MemberExpr>(expr);

  if (name != nullptr && !name->name.starts_with(kHiddenNamePrefix)) {
    return name->name.str();
  }
  if (name == nullptr && member != nullptr) {
    return this->shownMember(member);
  }

  return "x";
}

std::string TypeRelations::shownMember(const MemberExpr *member) const {
  auto *owner = dyn_cast<NameExpr>(member->object);
  if (owner == nullptr || owner->name.starts_with(kHiddenNamePrefix)) {
    return "x";
  }

  return owner->name.str() + "." + member->name.str();
}

std::string TypeRelations::conversionMethod(const Type *to) const {
  if (to->isFloat()) {
    return "toDouble";
  }
  if (to->isDecimal()) {
    return "toDecimal";
  }
  if (to->isInt()) {
    return "toInt";
  }
  std::string name = to->name().str();
  name[0] = char(std::toupper(name[0]));

  return "to" + name;
}

const Type *TypeRelations::unify(const Type *first, const Type *second) const {
  return TypeRelations::unifyAs(UnifyForm::WHOLE, first, second);
}

const Type *TypeRelations::unifyAs(UnifyForm form, const Type *first, const Type *second) {
  auto start = EnterUnify{.first = first, .second = second, .form = form};
  auto tasks = std::vector<UnifyTask>{start};
  auto answers = std::vector<const Type *>();
  while (!tasks.empty()) {
    UnifyTask task = tasks.back();
    tasks.pop_back();
    switch (task.kind()) {
      case UnifyTaskKind::ENTER:
        TypeRelations::enterUnify(*task.as<EnterUnify>(), tasks, answers);
        break;
      case UnifyTaskKind::COLLECT:
        TypeRelations::collectUnify(*task.as<CollectUnify>(), answers);
        break;
    }
  }

  return answers.back();
}

void TypeRelations::enterUnify(const EnterUnify &enter, std::vector<UnifyTask> &tasks,
                               std::vector<const Type *> &answers) {
  switch (enter.form) {
    case UnifyForm::WHOLE:
      return TypeRelations::enterWholeUnify(enter.first, enter.second, tasks, answers);
    case UnifyForm::HALF:
      return TypeRelations::enterHalfUnify(enter.first, enter.second, tasks, answers);
    case UnifyForm::UNION:
      return TypeRelations::pushUnify(tasks, enter.first, enter.second, UnifyForm::UNION,
                                      UnifyForm::WHOLE);
  }
}

void TypeRelations::pushUnify(std::vector<UnifyTask> &tasks, const Type *first, const Type *second,
                              UnifyForm collected, UnifyForm entered) {
  tasks.push_back(CollectUnify{.first = first, .second = second, .form = collected});
  tasks.push_back(EnterUnify{.first = first, .second = second, .form = entered});
}

void TypeRelations::enterHalfUnify(const Type *first, const Type *second,
                                   std::vector<UnifyTask> &tasks,
                                   std::vector<const Type *> &answers) {
  auto shortcut = ResultTypes::halfShortcut(first, second);
  if (shortcut.isDecided) {
    return answers.push_back(shortcut.type);
  }

  return TypeRelations::pushUnify(tasks, first, second, UnifyForm::HALF, UnifyForm::UNION);
}

void TypeRelations::enterWholeUnify(const Type *first, const Type *second,
                                    std::vector<UnifyTask> &tasks,
                                    std::vector<const Type *> &answers) {
  auto shortcut = TypeRelations::shortUnify(first, second);
  if (shortcut.isDecided) {
    return answers.push_back(shortcut.type);
  }
  const Type *plainFirst = first->nonOptional();
  const Type *plainSecond = second->nonOptional();
  bool isResultPair =
      plainFirst->isResult() && plainSecond->isResult() && plainFirst != plainSecond;
  if (isResultPair) {
    return TypeRelations::pushResultUnify(tasks, first, second);
  }
  const Type *base = TypeRelations::baseUnify(plainFirst, plainSecond);
  const Type *unified = TypeRelations::optionalLike(first, second, base);

  return answers.push_back(unified);
}

void TypeRelations::pushResultUnify(std::vector<UnifyTask> &tasks, const Type *first,
                                    const Type *second) {
  const Type *firstResult = first->nonOptional();
  const Type *secondResult = second->nonOptional();
  tasks.push_back(CollectUnify{.first = first, .second = second, .form = UnifyForm::WHOLE});
  tasks.push_back(EnterUnify{
      .first = firstResult->errType(),
      .second = secondResult->errType(),
      .form = UnifyForm::HALF,
  });
  tasks.push_back(EnterUnify{
      .first = firstResult->okType(),
      .second = secondResult->okType(),
      .form = UnifyForm::HALF,
  });
}

void TypeRelations::collectUnify(const CollectUnify &collect, std::vector<const Type *> &answers) {
  const Type *collected = TypeRelations::collectedType(collect, answers);
  answers.push_back(collected);
}

const Type *TypeRelations::collectedType(const CollectUnify &collect,
                                         std::vector<const Type *> &answers) {
  switch (collect.form) {
    case UnifyForm::WHOLE:
      return TypeRelations::collectedResult(collect, answers);
    case UnifyForm::HALF:
      return TypeRelations::collectedHalf(answers);
    case UnifyForm::UNION:
      return TypeRelations::collectedUnion(collect, answers);
  }

  return nullptr;
}

const Type *TypeRelations::collectedResult(const CollectUnify &collect,
                                           std::vector<const Type *> &answers) {
  const Type *error = ChildrenFirst::takeAnswer(answers);
  const Type *value = ChildrenFirst::takeAnswer(answers);
  if (value == nullptr || error == nullptr) {
    return nullptr;
  }
  const auto *result = Type::result(value, error);

  return TypeRelations::optionalLike(collect.first, collect.second, result);
}

const Type *TypeRelations::collectedHalf(std::vector<const Type *> &answers) {
  const Type *common = ChildrenFirst::takeAnswer(answers);
  if (common == nullptr || common->isError()) {
    return nullptr;
  }

  return common;
}

const Type *TypeRelations::collectedUnion(const CollectUnify &collect,
                                          std::vector<const Type *> &answers) {
  const Type *common = ChildrenFirst::takeAnswer(answers);
  if (common != nullptr) {
    return common;
  }

  return LiteralTypes::unionFallback(collect.first, collect.second);
}

const Type *TypeRelations::optionalLike(const Type *first, const Type *second, const Type *base) {
  if (base == nullptr) {
    return nullptr;
  }
  if (first->isOptional() || second->isOptional()) {
    return Type::optional(base);
  }

  return base;
}

UnifyShortcut TypeRelations::shortUnify(const Type *first, const Type *second) {
  if (first == second) {
    return TypeRelations::decided(first);
  }
  if (first->isError() || second->isError()) {
    return TypeRelations::decided(Type::error());
  }
  if (first->isNever()) {
    return TypeRelations::decided(second);
  }
  if (second->isNever()) {
    return TypeRelations::decided(first);
  }
  if (first->isVoid() || second->isVoid()) {
    return TypeRelations::decided(nullptr);
  }
  if (first->isUndefined()) {
    return TypeRelations::decided(Type::optional(second));
  }
  if (second->isUndefined()) {
    return TypeRelations::decided(Type::optional(first));
  }
  if (first->isUnknown() || second->isUnknown()) {
    return TypeRelations::decided(Type::unknown());
  }
  const Type *plainFirst = first->nonOptional();
  const Type *plainSecond = second->nonOptional();
  bool hasChoice = plainFirst->isLiteral() || plainSecond->isLiteral() || plainFirst->isUnion() ||
                   plainSecond->isUnion();
  if (hasChoice) {
    const Type *wider = TypeRelations::commonNumericType(plainFirst, plainSecond);
    const Type *unified = TypeRelations::optionalLike(first, second, wider);

    return TypeRelations::decided(unified);
  }
  if (plainFirst->isString() && plainSecond->isString()) {
    const Type *unified = TypeRelations::optionalLike(first, second, Type::string());

    return TypeRelations::decided(unified);
  }

  return UnifyShortcut{.isDecided = false, .type = nullptr};
}

UnifyShortcut TypeRelations::decided(const Type *type) {
  return UnifyShortcut{.isDecided = true, .type = type};
}

const Type *TypeRelations::baseUnify(const Type *first, const Type *second) {
  if (first == second) {
    return first;
  }
  const Type *related = TypeRelations::widerOfRelated(first, second);
  if (related != nullptr) {
    return related;
  }
  const Type *json = TypeRelations::widerJson(first, second);
  if (json != nullptr) {
    return json;
  }
  const Type *parent = TypeRelations::commonParent(first, second);
  if (parent != nullptr) {
    return parent;
  }
  const Type *array = TypeRelations::widerArray(first, second);
  if (array != nullptr) {
    return array;
  }
  const Type *promise = TypeRelations::settledPromise(first, second);
  if (promise != nullptr) {
    return promise;
  }
  const Type *constructor = TypeRelations::commonConstructor(first, second);
  if (constructor != nullptr) {
    return constructor;
  }

  return TypeRelations::commonAnyObject(first, second);
}

const Type *TypeRelations::widerOfRelated(const Type *first, const Type *second) {
  bool isNumbers = TypeRelations::isNumberLike(first) && TypeRelations::isNumberLike(second);
  bool isObjects = first->isObject() && second->isObject();
  bool isIterators = first->isIterator() && second->isIterator();
  if (!isNumbers && !isObjects && !isIterators) {
    return nullptr;
  }

  return TypeRelations::commonNumericType(first, second);
}

const Type *TypeRelations::widerJson(const Type *first, const Type *second) {
  if (!first->isJson() && !second->isJson()) {
    return nullptr;
  }

  return TypeRelations::commonNumericType(second, first);
}

const Type *TypeRelations::commonParent(const Type *first, const Type *second) {
  if (!first->isClass() || !second->isClass()) {
    return nullptr;
  }

  return ClassLinks::commonClass(first, second);
}

const Type *TypeRelations::widerArray(const Type *first, const Type *second) {
  if (!first->isArray() || !second->isArray()) {
    return nullptr;
  }

  return TypeRelations::commonNumericType(first, second);
}

const Type *TypeRelations::settledPromise(const Type *first, const Type *second) {
  if (!first->isPromise() || !second->isPromise()) {
    return nullptr;
  }
  if (first->promised()->isNever()) {
    return second;
  }
  if (second->promised()->isNever()) {
    return first;
  }

  return nullptr;
}

const Type *TypeRelations::commonConstructor(const Type *first, const Type *second) {
  if (!first->isClassRef() || !second->isClassRef()) {
    return nullptr;
  }
  const Type *firstMade = ClassValueChecker::constructedBy(first);
  const Type *secondMade = ClassValueChecker::constructedBy(second);
  const Type *made = TypeRelations::commonInstance(firstMade, secondMade);
  if (made == nullptr) {
    return Type::constructorOf(Type::anyObject());
  }

  return Type::constructorOf(made);
}

const Type *TypeRelations::commonInstance(const Type *first, const Type *second) {
  if (first == nullptr || second == nullptr) {
    return nullptr;
  }
  const Type *wider = TypeRelations::commonNumericType(first, second);
  if (wider != nullptr) {
    return wider;
  }

  return ClassLinks::commonClass(first, second);
}

const Type *TypeRelations::commonAnyObject(const Type *first, const Type *second) {
  bool hasAnyObject = first->isAnyObject() || second->isAnyObject();
  bool isReferences =
      UnknownChecker::isObjectReference(first) && UnknownChecker::isObjectReference(second);
  if (hasAnyObject && isReferences) {
    return Type::anyObject();
  }

  return nullptr;
}

Expr *TypeRelations::convert(Expr *expr, const Type *to) {
  if (expr->type == to || expr->type->isError() || to->isError()) {
    return expr;
  }
  if (expr->type->isParam() && !TypeQueries::mentionsParam(to)) {
    this->genericChecker->noteForced(expr, to);
  }
  IntLitExpr *literal = this->retypedLiteral(expr, to);
  if (literal != nullptr) {
    literal->type = to;
    expr->type = to;

    return expr;
  }
  this->rememberView(expr->type, to);
  auto *castExpr = this->state.getAst().make<CastExpr>(expr->loc(), expr, std::nullopt);
  castExpr->type = to;
  if (this->wideningRelations->isWideningWrites(expr->type, to)) {
    this->state.getWideningStore().rememberWideningCast(castExpr);
  }
  if (this->isReflectingTarget(to)) {
    this->reflectChecker->noteReflectedConversion(expr->type, to, castExpr);
  }
  this->reflectChecker->noteConstructorFlow(expr->type, to);

  return castExpr;
}

bool TypeRelations::isReflectingTarget(const Type *to) const {
  const Type *reflected = to->nonOptional();
  bool isUnknownArray = reflected->isArray() && reflected->inner()->isUnknown();

  return reflected->isConstructor() || reflected->isAnyObject() || reflected->isUnknown() ||
         isUnknownArray;
}

IntLitExpr *TypeRelations::retypedLiteral(Expr *expr, const Type *to) const {
  bool isNumberChange = TypeRelations::isNumberLike(to) && expr->type->isInteger();
  if (!isNumberChange) {
    return nullptr;
  }
  auto *unary = dyn_cast<UnaryExpr>(expr);
  bool isNegated = unary != nullptr && unary->op == UnaryOp::Neg;
  Expr *written = isNegated ? unary->operand : expr;

  return dyn_cast<IntLitExpr>(written);
}

void TypeRelations::rememberView(const Type *from, const Type *to) {
  const Type *fromObject = this->innerObject(from, to);
  const Type *toObject = this->innerObject(to, from);
  bool isPlainPair = fromObject->isObject() && toObject->isObject() && !fromObject->isClass() &&
                     !toObject->isClass() && !fromObject->isAnyObject() &&
                     !toObject->isAnyObject() && !fromObject->isBytes();
  bool isViewed = isPlainPair && fromObject != toObject && !fromObject->isSubtypeOf(toObject);
  if (isViewed) {
    this->state.getInventoryStore().addView(fromObject, toObject);
  }
}

Expr *TypeRelations::coerce(Expr *expr, const Type *to, llvm::StringRef what) {
  const Type *from = expr->type;
  if (from == to || from->isError() || to->isError()) {
    return expr;
  }
  if (TypeRelations::isImplicitlyConvertible(from, to)) {
    return this->convert(expr, to);
  }
  if (this->isLiteralAdapting(expr, to)) {
    return this->adaptLiteral(expr, to);
  }
  if (this->conditionalTypes->isFittingConditional(from, to)) {
    return this->convert(expr, to);
  }
  auto *adapted = this->adaptedValue(expr, to);
  if (adapted != nullptr) {
    return adapted;
  }
  auto intervals = this->targetIntervals(to);
  if (from->isInt() && this->canProveSomeInterval(expr, intervals)) {
    return this->convert(expr, to);
  }
  auto proven = this->rangeProven(expr, to, what, intervals);
  if (proven.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), proven.error().message);
    return expr;
  }
  this->walkAssignable(expr, to, what);

  return expr;
}

Expr *TypeRelations::adaptedValue(Expr *expr, const Type *to) {
  const Type *from = expr->type;
  if (to->nonOptional()->isIterator() && from->isObject()) {
    auto *iterator = this->iteratorValue(expr, to);
    if (iterator != nullptr) {
      return iterator;
    }
  }
  if (to->nonOptional()->isAsyncIterator() && from->isObject() && !from->isAsyncIterator()) {
    auto *iterator = this->asyncIteratorValue(expr, to);
    if (iterator != nullptr) {
      return iterator;
    }
  }
  const Type *function = this->functionTarget(to);
  if (from->isFunction() && function->isFunction()) {
    return this->adaptedFunction(expr, function, to);
  }

  return nullptr;
}

const Type *TypeRelations::functionTarget(const Type *to) const {
  if (to->isOptional() && to->inner()->isFunction()) {
    return to->inner();
  }

  return to;
}

Expr *TypeRelations::iteratorValue(Expr *expr, const Type *to) {
  Expr *iterator = this->iteratorOf(expr);
  bool isFitting = iterator != nullptr && iterator->type->isIterator() &&
                   TypeRelations::isImplicitlyConvertible(iterator->type, to);
  if (!isFitting) {
    return nullptr;
  }

  return this->convert(iterator, to);
}

Expr *TypeRelations::iteratorOf(Expr *expr) {
  auto *builtin = this->iterableChecker->iteratorOf(expr);
  if (builtin != nullptr) {
    return builtin;
  }

  return this->iterableChecker->userIterator(expr);
}

Expr *TypeRelations::asyncIteratorValue(Expr *expr, const Type *to) {
  auto *iterator = this->iterableChecker->asyncUserIterator(expr);
  bool isFitting = iterator != nullptr && iterator->type->isAsyncIterator() &&
                   TypeRelations::isImplicitlyConvertible(iterator->type, to);
  if (!isFitting) {
    return nullptr;
  }

  return this->convert(iterator, to);
}

Expr *TypeRelations::adaptedFunction(Expr *expr, const Type *function, const Type *to) {
  Expr *adapted = this->adaptedTo(expr, function);
  if (adapted == nullptr || function == to) {
    return adapted;
  }

  return this->convert(adapted, to);
}

Expr *TypeRelations::adaptedTo(Expr *expr, const Type *function) {
  auto *adapted = this->functionValueChecker->adaptFunction(expr, function);
  if (adapted != nullptr) {
    return adapted;
  }

  return this->tzModulesChecker->adaptResult(expr, function);
}

std::vector<std::pair<int64_t, int64_t>> TypeRelations::targetIntervals(const Type *to) const {
  const Type *target = to->nonOptional();
  auto range = this->literalTypes->intRange(target);
  if (range.has_value()) {
    return {*range};
  }
  if (target->isUnion() && target->members().size() >= kManyLiteralMembers) {
    return this->literalTypes->intIntervals(target);
  }

  return {};
}

bool TypeRelations::canProveSomeInterval(const Expr *expr,
                                         llvm::ArrayRef<std::pair<int64_t, int64_t>> intervals) {
  for (const std::pair<int64_t, int64_t> &interval : intervals) {
    if (this->byteProofs->provesFits(expr, interval.first, interval.second)) {
      return true;
    }
  }

  return false;
}

void TypeRelations::walkAssignable(const Expr *expr, const Type *to, llvm::StringRef what) {
  auto assigned = this->assignable(expr, to, what);
  const auto *other = this->otherPlace(expr, to);
  if (assigned.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), assigned.error().message);
  }
  if (assigned.isErr() && other != nullptr) {
    auto otherLoc = this->placeLoc(expr, other);
    auto alsoHere = ErrorTextAssignable::alsoHere(other->name());
    this->state.getDiagnostics().note(otherLoc, alsoHere);
  }
}

TypeRelations::ResultRangeWasProven
TypeRelations::rangeProven(const Expr *expr, const Type *to, llvm::StringRef what,
                           llvm::ArrayRef<std::pair<int64_t, int64_t>> intervals) const {
  const Type *from = expr->type;
  bool isInteger = from->isInt() || (from->isLiteral() && from->literalBase()->isInt());
  bool isOutside = !intervals.empty() && isInteger;
  auto prefix = ErrorTextAssignable::notTo(from->name(), to->name(), what);
  auto holds = ErrorTextRange::holds(intervals);
  if (isOutside && !from->isInt()) {
    std::string message = prefix + holds;

    return err(TypeRelationsRangeProvenError{
        .code = TypeRelationsRangeProvenErrorCode::LITERAL_OUT_OF_RANGE,
        .message = message,
    });
  }
  if (isOutside) {
    auto check = this->rangeCheck(expr, to->nonOptional(), intervals);
    std::string message = prefix + holds + ErrorTextRange::outside(check);

    return err(TypeRelationsRangeProvenError{
        .code = TypeRelationsRangeProvenErrorCode::MAY_BE_OUT_OF_RANGE,
        .message = message,
    });
  }

  return ok(expr);
}

std::string TypeRelations::rangeCheck(const Expr *expr, const Type *target,
                                      llvm::ArrayRef<std::pair<int64_t, int64_t>> intervals) const {
  auto widest = this->widestInterval(intervals);
  auto low = std::to_string(widest.first);
  auto high = std::to_string(widest.second);
  auto *name = dyn_cast<NameExpr>(expr);
  auto text = name != nullptr ? name->name.str() : std::string("x");
  if (target->name() == kHttpStatusType) {
    return ErrorTextRange::status(text);
  }

  return ErrorTextRange::between(text, low, high);
}

std::pair<int64_t, int64_t>
TypeRelations::widestInterval(llvm::ArrayRef<std::pair<int64_t, int64_t>> intervals) const {
  std::pair<int64_t, int64_t> widest = intervals.front();
  for (const std::pair<int64_t, int64_t> &interval : intervals.drop_front()) {
    bool isWider = widest.second - widest.first < interval.second - interval.first;
    if (isWider) {
      widest = interval;
    }
  }

  return widest;
}

const Type *TypeRelations::innerObject(const Type *type, const Type *other) const {
  const Type *inner = type->nonOptional();
  const Type *otherInner = other->nonOptional();
  while (inner->isArray() && otherInner->isArray()) {
    inner = inner->inner()->nonOptional();
    otherInner = otherInner->inner()->nonOptional();
  }

  return inner;
}

const Type *TypeRelations::otherPlace(const Expr *expr, const Type *to) const {
  const auto *toObject = this->innerObject(to, expr->type);
  auto *name = dyn_cast<NameExpr>(expr);
  if (name == nullptr || name->var() == nullptr) {
    return nullptr;
  }
  for (auto [type, loc] : this->state.getTypeContext().getDestinations(name->var())) {
    if (type->nonOptional() != toObject) {
      return type->nonOptional();
    }
  }

  return nullptr;
}

llvm::SMLoc TypeRelations::placeLoc(const Expr *expr, const Type *place) const {
  auto *name = dyn_cast<NameExpr>(expr);
  for (auto [type, loc] : this->state.getTypeContext().getDestinations(name->var())) {
    if (type->nonOptional() == place) {
      return loc;
    }
  }

  return llvm::SMLoc();
}

TypeRelations::ResultTypeAssignable TypeRelations::assignable(const Expr *expr, const Type *to,
                                                              llvm::StringRef what) const {
  auto attempt = this->describeAttempt(expr, to);
  auto code = this->mismatchOf(attempt);
  auto prefix = this->mismatchPrefix(attempt, what);
  auto reason = this->mismatchReason(attempt, code);
  std::string message = prefix + reason;

  return err(TypeRelationsAssignableError{
      .code = code,
      .message = message,
  });
}

TypeRelations::AssignAttempt TypeRelations::describeAttempt(const Expr *expr,
                                                            const Type *to) const {
  const Type *from = expr->type;

  return AssignAttempt{
      .expr = expr,
      .from = from,
      .to = to,
      .fromObject = this->innerObject(from, to),
      .toObject = this->innerObject(to, from),
  };
}

TypeRelationsAssignableErrorCode TypeRelations::mismatchOf(const AssignAttempt &attempt) const {
  const Type *from = attempt.from;
  const Type *to = attempt.to;
  const Type *fromObject = attempt.fromObject;
  const Type *toObject = attempt.toObject;
  const ClassDecl *fromClass = fromObject->classDecl();
  const ClassDecl *genericClass = from->classRef();
  bool isGenericValue =
      genericClass != nullptr && genericClass->isGeneric() && toObject->isConstructor();
  if (isGenericValue) {
    return TypeRelationsAssignableErrorCode::GENERIC_CLASS_VALUE;
  }
  if (from->isUnknown() || from->isAnyObject()) {
    return TypeRelationsAssignableErrorCode::UNKNOWN_VALUE;
  }
  bool isLiteralObject = fromClass == nullptr || fromClass->literal() != nullptr;
  if (fromObject->isObject() && toObject->isClass() && isLiteralObject) {
    return TypeRelationsAssignableErrorCode::LITERAL_NOT_INSTANCE;
  }
  if (fromClass != nullptr && fromClass->literal() != nullptr && toObject->isObject()) {
    return TypeRelationsAssignableErrorCode::LITERAL_WITH_METHODS;
  }
  auto widening = this->genericWidening->whyNot(fromObject, toObject);
  if (!widening.empty()) {
    return TypeRelationsAssignableErrorCode::GENERIC_WIDENING;
  }
  bool isClassValue = fromObject->isClass() && toObject->isObject() && fromObject != toObject;
  if (isClassValue && toObject->isClass()) {
    return TypeRelationsAssignableErrorCode::CLASS_NOT_EXTENDED;
  }
  if (isClassValue) {
    return TypeRelationsAssignableErrorCode::CLASS_NOT_IMPLEMENTED;
  }
  bool isObjectIntoUnion = fromObject->isObject() && !fromObject->isClass() &&
                           toObject->isUnion() && this->hasObjectMember(toObject);
  if (isObjectIntoUnion) {
    return TypeRelationsAssignableErrorCode::OBJECT_INTO_UNION;
  }
  if (fromObject->isTuple() && toObject->isArray()) {
    return TypeRelationsAssignableErrorCode::TUPLE_NOT_ARRAY;
  }
  bool hasDifferentObjects = fromObject->isObject() && toObject->isObject() &&
                             !TypeRelations::isImplicitlyConvertible(fromObject, toObject);
  if (hasDifferentObjects) {
    return TypeRelationsAssignableErrorCode::OBJECT_MISMATCH;
  }
  if (from->isOptional() && TypeRelations::isImplicitlyConvertible(from->inner(), to)) {
    return TypeRelationsAssignableErrorCode::MAY_BE_UNDEFINED;
  }
  if (from->nonOptional()->isJson()) {
    return TypeRelationsAssignableErrorCode::JSON_UNCHECKED;
  }
  if (to->nonOptional()->isAnyObject()) {
    return TypeRelationsAssignableErrorCode::NOT_AN_OBJECT;
  }
  const Type *value = from->nonOptional();
  bool isIntoJson = to->nonOptional()->isJson();
  if (isIntoJson && (fromObject->isObject() || value->isArray())) {
    return TypeRelationsAssignableErrorCode::OBJECT_INTO_JSON;
  }
  if (isIntoJson && value->isRecord() && TypeRelations::isHoldingWideInteger(value->valueType())) {
    return TypeRelationsAssignableErrorCode::WIDE_INTEGERS_INTO_JSON;
  }
  if (isIntoJson && value->isDecimal()) {
    return TypeRelationsAssignableErrorCode::DECIMAL_INTO_JSON;
  }
  if (from->isFloat() && to->isDecimal()) {
    return TypeRelationsAssignableErrorCode::DOUBLE_INTO_DECIMAL;
  }
  if (from->isDecimal() && (to->isNumeric() || to->nonOptional()->isNumeric())) {
    return TypeRelationsAssignableErrorCode::DECIMAL_INTO_NUMBER;
  }

  return TypeRelationsAssignableErrorCode::NOT_ASSIGNABLE;
}

std::string TypeRelations::mismatchPrefix(const AssignAttempt &attempt,
                                          llvm::StringRef what) const {
  const Type *from = attempt.from;
  const Type *to = attempt.to;
  const Type *function = to->isOptional() && to->inner()->isFunction() ? to->inner() : to;
  auto prefix = ErrorTextAssignable::notTo(from->name(), to->name(), what);
  if (from->isFunction() && function->isFunction()) {
    prefix += ErrorTextAssignable::functions();
  }
  if (this->isStreamIntoBody(attempt)) {
    prefix += ErrorTextAssignable::stream();
  }
  if (this->isRequestInit(attempt)) {
    prefix += ErrorTextAssignable::request();
  }
  if (to->isNever()) {
    prefix += ErrorTextAssignable::never();
  }

  return prefix;
}

bool TypeRelations::hasObjectMember(const Type *unionType) const {
  for (const Type *member : unionType->members()) {
    if (member->isObject()) {
      return true;
    }
  }

  return false;
}

bool TypeRelations::isStreamIntoBody(const AssignAttempt &attempt) const {
  if (!this->isWebStream(attempt.fromObject)) {
    return false;
  }
  const Type *toObject = attempt.toObject;
  llvm::ArrayRef<const Type *> wanted =
      toObject->isUnion() ? toObject->members() : llvm::ArrayRef<const Type *>(toObject);
  bool hasFormData = false;
  bool hasWebStream = false;
  for (const Type *member : wanted) {
    hasFormData = hasFormData || this->isFormData(member);
    hasWebStream = hasWebStream || this->isWebStream(member);
  }

  return hasFormData && !hasWebStream;
}

bool TypeRelations::isWebStream(const Type *type) const {
  const ClassDecl *classDecl = type->classDecl();
  bool isLibraryGeneric =
      classDecl != nullptr && classDecl->library() && classDecl->generic() != nullptr;

  return isLibraryGeneric && classDecl->generic()->name == kWebStreamClass &&
         classDecl->typeArgs.size() == 2;
}

bool TypeRelations::isFormData(const Type *type) const {
  const ClassDecl *classDecl = type->classDecl();

  return classDecl != nullptr && classDecl->library() && classDecl->name == kFormDataClass;
}

bool TypeRelations::isRequestInit(const AssignAttempt &attempt) const {
  llvm::StringRef fromName = attempt.fromObject->name();

  return fromName.starts_with(kRequestInitType) && attempt.toObject->name() == kRequestOptionsType;
}

std::string TypeRelations::mismatchReason(const AssignAttempt &attempt,
                                          TypeRelationsAssignableErrorCode code) const {
  const Type *fromObject = attempt.fromObject;
  const Type *toObject = attempt.toObject;
  switch (code) {
    case TypeRelationsAssignableErrorCode::GENERIC_CLASS_VALUE:
      return this->genericClassText(attempt.from->classRef());
    case TypeRelationsAssignableErrorCode::UNKNOWN_VALUE:
      return this->unknownChecker->unknownHint(attempt.expr, attempt.to);
    case TypeRelationsAssignableErrorCode::LITERAL_NOT_INSTANCE:
      return ErrorTextAssignable::literal(toObject->name());
    case TypeRelationsAssignableErrorCode::LITERAL_WITH_METHODS:
      return ErrorTextAssignable::methods();
    case TypeRelationsAssignableErrorCode::GENERIC_WIDENING:
      return this->genericWidening->whyNot(fromObject, toObject);
    case TypeRelationsAssignableErrorCode::CLASS_NOT_EXTENDED:
      return ErrorTextAssignable::classes(fromObject->name(), toObject->name());
    case TypeRelationsAssignableErrorCode::CLASS_NOT_IMPLEMENTED:
      return ErrorTextAssignable::implements(toObject->name());
    case TypeRelationsAssignableErrorCode::OBJECT_INTO_UNION:
      return this->unionText(toObject);
    case TypeRelationsAssignableErrorCode::TUPLE_NOT_ARRAY:
      return ErrorTextAssignable::tuple();
    case TypeRelationsAssignableErrorCode::OBJECT_MISMATCH:
      return this->objectMismatchText(attempt);
    case TypeRelationsAssignableErrorCode::MAY_BE_UNDEFINED:
      return ErrorTextAssignable::undefined();
    case TypeRelationsAssignableErrorCode::JSON_UNCHECKED:
      return ErrorTextAssignable::json();
    case TypeRelationsAssignableErrorCode::NOT_AN_OBJECT:
      return ErrorTextAssignable::object();
    case TypeRelationsAssignableErrorCode::OBJECT_INTO_JSON:
      return ErrorTextAssignable::jsonObject();
    case TypeRelationsAssignableErrorCode::WIDE_INTEGERS_INTO_JSON:
      return ErrorTextAssignable::wideJson();
    case TypeRelationsAssignableErrorCode::DECIMAL_INTO_JSON:
      return ErrorTextAssignable::noDecimal();
    case TypeRelationsAssignableErrorCode::DOUBLE_INTO_DECIMAL:
      return ErrorTextAssignable::toDecimal();
    case TypeRelationsAssignableErrorCode::DECIMAL_INTO_NUMBER:
      return ErrorTextAssignable::toNumber();
    case TypeRelationsAssignableErrorCode::NOT_ASSIGNABLE:
      return this->conversionHint(attempt.expr, attempt.to);
  }

  return this->conversionHint(attempt.expr, attempt.to);
}

std::string TypeRelations::genericClassText(const ClassDecl *generic) const {
  auto name = generic->name.str();
  auto example = generic->library() ? ErrorTextAssignable::extendsBus(name)
                                    : ErrorTextAssignable::extendsInt(name);

  return ErrorTextAssignable::generic(name, example);
}

std::string TypeRelations::unionText(const Type *toObject) const {
  auto text = ErrorTextAssignable::inUnion();
  const Type *front = toObject->members().front();
  if (!front->isObject()) {
    return text;
  }
  const Field *tag = this->sharedLiteralField(toObject);
  if (tag != nullptr) {
    text += ErrorTextAssignable::withValue(tag->name);
  }

  return text;
}

const Field *TypeRelations::sharedLiteralField(const Type *unionType) const {
  for (const Field &field : unionType->members().front()->fields()) {
    if (this->hasLiteralFieldInAll(unionType->members(), field.name)) {
      return &field;
    }
  }

  return nullptr;
}

bool TypeRelations::hasLiteralFieldInAll(llvm::ArrayRef<const Type *> members,
                                         llvm::StringRef name) const {
  for (const Type *member : members) {
    const Field *field = member->isObject() ? member->field(name) : nullptr;
    bool isLiteral = field != nullptr && field->type->isLiteral();
    if (!isLiteral) {
      return false;
    }
  }

  return true;
}

std::string TypeRelations::objectMismatchText(const AssignAttempt &attempt) const {
  const auto *other = this->otherPlace(attempt.expr, attempt.to);
  auto text =
      this->objectChecker->objectMismatch(attempt.fromObject, attempt.toObject, other != nullptr);
  if (other != nullptr) {
    text += ErrorTextAssignable::both(other->name());
  }

  return text;
}

const Type *TypeRelations::comparisonType(const Type *first, const Type *second) const {
  if (first->isVoid() || second->isVoid()) {
    return nullptr;
  }
  const Type *common = this->unify(first, second);
  if (common == nullptr || common->isError()) {
    return common;
  }
  if (!this->isComparable(common->nonOptional())) {
    return nullptr;
  }

  return common;
}

bool TypeRelations::isComparable(const Type *base) const {
  bool isPlainUnion = base->isUnion() && !UnionSlots::hasParamMember(base);

  return base->isNumeric() || base->isDecimal() || base->isBool() || base->isString() ||
         base->isArray() || base->isFunction() || base->isObject() || base->isIterator() ||
         base->isJson() || base->isDict() || base->isUnknown() || base->isClassRef() ||
         base->isLiteral() || isPlainUnion;
}

}
