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

TypeRelations::TypeRelations(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->byteProofs = &all.byteProofs;
  this->objectChecker = &all.objectChecker;
  this->constantFolder = &all.constantFolder;
  this->genericChecker = &all.genericChecker;
  this->reflectChecker = &all.reflectChecker;
  this->unknownChecker = &all.unknownChecker;
  this->iterableChecker = &all.iterableChecker;
  this->conditionalTypes = &all.conditionalTypes;
  this->tzModulesChecker = &all.tzModulesChecker;
  this->wideningRelations = &all.wideningRelations;
  this->functionValueChecker = &all.functionValueChecker;
}

bool TypeRelations::holdsWideInteger(const Type *type) {
  TypeParts parts = TypeParts::collect(type, kWideIntegerRoute);
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
  constexpr double kExact = 9007199254740992.0;

  return double(type->intMax()) > kExact || double(type->intMin()) < -kExact;
}

bool TypeRelations::copiesIntoRecord(const Type *from, const Type *to) {
  if (!TypeRelations::recordCopyShape(from, to)) {
    return false;
  }
  const Type *value = to->valueType();
  for (const Field &f : from->fields()) {
    const Type *type = TypeRelations::copiedFieldType(f, value);
    if (type->isFunction() || !TypeRelations::isImplicitlyConvertible(type, value)) {
      return false;
    }
  }

  return true;
}

bool TypeRelations::recordCopyShape(const Type *from, const Type *to) {
  return to->isRecord() && to->keyType()->isString() && from->isObject() && !from->isClass() &&
         !from->isTuple() && !from->isAnyObject() && !from->isIncomplete() &&
         from->bases().empty() && from->overloads().empty() && !from->isRegExp() &&
         !from->isMatch() && !from->isDate() && !from->isIntl() && !from->isCrypto() &&
         !from->isWeakRef() && !from->isBytes() && !from->isAsyncIterator();
}

const Type *TypeRelations::copiedFieldType(const Field &field, const Type *value) {
  return field.type->isOptional() && !value->isOptional() ? field.type->inner() : field.type;
}

ConversionStep TypeRelations::recordCopyStep(const Type *from, const Type *to) {
  const Type *value = to->valueType();
  std::vector<ConversionGoal> fields;
  for (const Field &f : from->fields()) {
    const Type *type = TypeRelations::copiedFieldType(f, value);
    if (type->isFunction()) {
      return TypeRelations::stepDecided(false);
    }
    fields.push_back(ConversionGoal{type, value});
  }

  return TypeRelations::stepNeeds(std::move(fields));
}

bool TypeRelations::isImplicitlyConvertible(const Type *from, const Type *to) {
  std::vector<ConversionTask> tasks = {ConversionGoal{from, to}};
  while (!tasks.empty()) {
    ConversionTask task = std::move(tasks.back());
    tasks.pop_back();
    ConversionStep step = TypeRelations::taskStep(task);
    if (step.possible) {
      tasks.insert(tasks.end(), step.needs.rbegin(), step.needs.rend());
    } else if (!TypeRelations::retryMember(tasks)) {
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
  std::vector<ConversionTask> needs;
  if (from->isUnion()) {
    for (const Type *m : from->members()) {
      needs.push_back(MemberGoal{m, to});
    }
  } else {
    needs.push_back(MemberGoal{from, to});
  }

  return ConversionStep{true, std::move(needs)};
}

ConversionStep TypeRelations::memberStep(const MemberGoal &goal) {
  if (LiteralTypes::exactMemberFor(goal.from, goal.to) != nullptr) {
    return TypeRelations::stepHolds();
  }

  return TypeRelations::memberTrial(MemberChoice{goal.from, goal.to, 0});
}

ConversionStep TypeRelations::memberTrial(MemberChoice choice) {
  llvm::ArrayRef<const Type *> members = choice.to->members();
  while (choice.next < members.size()) {
    const Type *member = members[choice.next];
    choice.next += 1;
    if (!member->isParam() && !choice.from->isParam()) {
      const Type *from = choice.from;
      return ConversionStep{true, {ConversionGoal{from, member}, choice}};
    }
  }

  return TypeRelations::stepDecided(false);
}

bool TypeRelations::retryMember(std::vector<ConversionTask> &tasks) {
  while (!tasks.empty()) {
    ConversionTask task = std::move(tasks.back());
    tasks.pop_back();
    const MemberChoice *choice = task.as<MemberChoice>();
    if (choice == nullptr) {
      continue;
    }
    ConversionStep trial = TypeRelations::memberTrial(*choice);
    if (trial.possible) {
      tasks.insert(tasks.end(), trial.needs.rbegin(), trial.needs.rend());
      return true;
    }
  }

  return false;
}

ConversionStep TypeRelations::conversionStep(const Type *from, const Type *to) {
  if (from == to || from->isNever()) {
    return TypeRelations::stepHolds();
  }
  std::optional<ConversionStep> byTarget = TypeRelations::targetStep(from, to);
  if (byTarget.has_value()) {
    return *byTarget;
  }
  std::optional<ConversionStep> bySource = TypeRelations::sourceStep(from, to);
  if (bySource.has_value()) {
    return *bySource;
  }
  std::optional<bool> plain = TypeRelations::plainConvertible(from, to);
  if (plain.has_value()) {
    return TypeRelations::stepDecided(*plain);
  }
  if (TypeRelations::recordCopyShape(from, to)) {
    return TypeRelations::recordCopyStep(from, to);
  }
  if (from->isResult() && to->isResult()) {
    return TypeRelations::resultStep(from, to);
  }

  return TypeRelations::stepDecided(false);
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

  return TypeRelations::stepDecided(false);
}

ConversionStep TypeRelations::classRefStep(const Type *from, const Type *to) {
  if (from->classRef() != nullptr && from->classRef()->isGeneric()) {
    return TypeRelations::stepDecided(false);
  }
  const Type *made = ClassValueChecker::constructedBy(from);
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
    bool matches =
        from->isStringLiteral() && TemplatePatterns::matchesPattern(from->literalText(), to);
    return TypeRelations::stepDecided(matches);
  }
  if (from->isPattern()) {
    return TypeRelations::stepDecided(to == Type::string());
  }
  if (from->isLiteral() && to->isLiteral()) {
    return TypeRelations::stepDecided(false);
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
    return TypeRelations::stepDecided(false);
  }
  std::vector<ConversionGoal> members;
  for (const Type *m : from->members()) {
    members.push_back(ConversionGoal{m, to});
  }

  return TypeRelations::stepNeeds(std::move(members));
}

ConversionStep TypeRelations::stepHolds() {
  return ConversionStep{true, {}};
}

ConversionStep TypeRelations::stepDecided(bool possible) {
  return ConversionStep{possible, {}};
}

ConversionStep TypeRelations::stepNeeds(std::vector<ConversionGoal> goals) {
  std::vector<ConversionTask> needs(goals.begin(), goals.end());

  return ConversionStep{true, std::move(needs)};
}

ConversionStep TypeRelations::jsonStep(const Type *from, const Type *to) {
  bool plain = from->isString() || from->isNumeric() || from->isBool() || from->isLiteral() ||
               (from->isArray() && from->inner()->isJson());
  if (plain) {
    return TypeRelations::stepHolds();
  }
  bool record = from->isRecord() && from->keyType()->isString() &&
                !TypeRelations::holdsWideInteger(from->valueType());
  return record ? TypeRelations::stepNeeds({{from->valueType()->nonOptional(), to}})
                : TypeRelations::stepDecided(false);
}

ConversionStep TypeRelations::resultStep(const Type *from, const Type *to) {
  std::vector<ConversionGoal> needs;
  for (auto [a, b] :
       {std::pair{from->okType(), to->okType()}, std::pair{from->errType(), to->errType()}}) {
    if (a == b || a->isNever()) {
      continue;
    }
    if (a->isVoid() || b->isVoid()) {
      return TypeRelations::stepDecided(false);
    }
    needs.push_back(ConversionGoal{a, b});
  }

  return TypeRelations::stepNeeds(std::move(needs));
}

std::optional<bool> TypeRelations::plainConvertible(const Type *from, const Type *to) {
  if (from->isArray() && to->isArray()) {
    return WideningRelations::sameReference(from->inner(), to->inner());
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
  if (from->isMatch() || to->isMatch() || from->isRegExp() || to->isRegExp() || from->isDate() ||
      to->isDate() || from->isIntl() || to->isIntl() || from->isCrypto() || to->isCrypto() ||
      from->isWeakRef() || to->isWeakRef()) {
    return from->isMatch() && to->isMatch() && to->matchGroups() < 0 && !from->field("indices");
  }
  if (from->isObject() && to->isObject() && from->isSubtypeOf(to)) {
    return true;
  }
  if (GenericWidening::widens(from, to)) {
    return true;
  }
  if (from->isObject() && to->isObject() && !from->isClass() && !to->isClass() &&
      from->isTuple() == to->isTuple() && from->fields().size() == to->fields().size() &&
      !from->isAnyObject() && !to->isAnyObject() && from->overloads().empty() &&
      to->overloads().empty()) {
    for (const Field &f : from->fields()) {
      const Field *t = to->field(f.name);
      if (!t || t->type != f.type) {
        return false;
      }
    }
    return true;
  }

  return std::nullopt;
}

const Type *TypeRelations::commonNumericType(const Type *a, const Type *b) {
  if (TypeRelations::isImplicitlyConvertible(a, b)) {
    return b;
  }
  if (TypeRelations::isImplicitlyConvertible(b, a)) {
    return a;
  }

  return nullptr;
}

bool TypeRelations::isIntLiteralTree(const Expr *expr) {
  for (const Expr *node : ExprParts::operatorTree(expr)) {
    if (!TypeRelations::isIntLiteralNode(node)) {
      return false;
    }
  }

  return true;
}

bool TypeRelations::isIntLiteralNode(const Expr *expr) {
  if (isa<IntLitExpr>(expr)) {
    return true;
  }
  if (auto *name = dyn_cast<NameExpr>(expr)) {
    return name->var() && name->var()->literalConst;
  }
  if (auto *unary = dyn_cast<UnaryExpr>(expr)) {
    return unary->op == UnaryOp::Neg || unary->op == UnaryOp::Plus;
  }
  if (auto *bin = dyn_cast<BinaryExpr>(expr)) {
    return bin->op == BinaryOp::Add || bin->op == BinaryOp::Sub || bin->op == BinaryOp::Mul ||
           bin->op == BinaryOp::Rem || bin->op == BinaryOp::Pow;
  }

  return false;
}

const Type *TypeRelations::literalTarget(const Expr *expr, const Type *to) {
  const Type *target = to->nonOptional();
  if (target->isUnion()) {
    target = LiteralTypes::numberMemberOf(target, false);
  }
  if (!target || !expr->type || !expr->type->isInteger() || expr->type == target ||
      !TypeRelations::isIntLiteralTree(expr)) {
    return nullptr;
  }
  if (target->isFloat() || target->isDecimal()) {
    return target;
  }
  if (!target->isInteger()) {
    return nullptr;
  }
  std::optional<int64_t> value = this->constantFolder->intConstant(expr);
  if (!value) {
    return nullptr;
  }
  if (*value < 0 && !expr->type->isUnsigned()) {
    return !target->isUnsigned() && *value >= target->intMin() ? target : nullptr;
  }

  return uint64_t(*value) <= target->intMax() ? target : nullptr;
}

bool TypeRelations::literalAdapts(const Expr *expr, const Type *to) {
  return this->literalTarget(expr, to) != nullptr;
}

Expr *TypeRelations::adaptLiteral(Expr *expr, const Type *to) {
  const Type *target = this->literalTarget(expr, to);
  if (!target) {
    return expr;
  }
  Expr *adapted = this->convert(expr, target);

  return target == to ? adapted : this->convert(adapted, to);
}

std::string TypeRelations::operandText(llvm::StringRef text) {
  int depth = 0;
  bool plain = !text.empty() && text.front() != '-' && text.front() != '+';
  for (char c : text) {
    if (c == '(' || c == '[') {
      ++depth;
    } else if (c == ')' || c == ']') {
      --depth;
    } else if (depth == 0 && !std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '.' &&
               c != '#' && c != '$' && c != '"' && c != '\'') {
      plain = false;
    }
  }
  if (text.size() >= 2 && text.front() == '(' && text.back() == ')') {
    int level = 0;
    bool outer = true;
    for (size_t i = 0; i < text.size(); ++i) {
      level += text[i] == '(' ? 1 : text[i] == ')' ? -1 : 0;
      if (level == 0 && i + 1 < text.size()) {
        outer = false;
      }
    }
    plain |= outer;
  }
  bool literal = !text.empty() && std::isdigit(static_cast<unsigned char>(text.front()));

  return plain && !literal ? text.str() : "(" + text.str() + ")";
}

std::string TypeRelations::conversionHint(const Expr *expr, const Type *to) {
  const Type *from = expr->type ? expr->type->nonOptional() : nullptr;
  const Type *target = to->nonOptional();
  if (!from || !(from->isNumeric() || from->isDecimal()) ||
      !(target->isNumeric() || target->isDecimal())) {
    return "";
  }
  std::string text = "x";
  if (auto *name = dyn_cast<NameExpr>(expr); name && !name->name.starts_with("%")) {
    text = name->name.str();
  } else if (auto *member = dyn_cast<MemberExpr>(expr);
             member && isa<NameExpr>(member->object) &&
             !cast<NameExpr>(member->object)->name.starts_with("%")) {
    text = cast<NameExpr>(member->object)->name.str() + "." + member->name.str();
  }
  if (from->isInteger() && target->isInteger()) {
    std::string method = TypeRelations::conversionMethod(target);
    return ErrorTextConvert::direct(text, method);
  }
  if (from->isInteger() && target->isFloat()) {
    return ErrorTextConvert::direct(text, "toDouble");
  }
  if (from->isInteger() && target->isDecimal()) {
    return ErrorTextConvert::direct(text, "toDecimal");
  }
  if (from->isFloat() && target->isInteger()) {
    std::string method = TypeRelations::conversionMethod(target);
    return ErrorTextConvert::rounded(text, method);
  }

  return "";
}

std::string TypeRelations::conversionMethod(const Type *to) {
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

const Type *TypeRelations::unify(const Type *a, const Type *b) {
  return TypeRelations::unifyAs(UnifyForm::WHOLE, a, b);
}

const Type *TypeRelations::unifyAs(UnifyForm form, const Type *a, const Type *b) {
  std::vector<UnifyTask> tasks = {EnterUnify{a, b, form}};
  std::vector<const Type *> answers;
  while (!tasks.empty()) {
    UnifyTask task = tasks.back();
    tasks.pop_back();
    if (const EnterUnify *enter = task.as<EnterUnify>()) {
      TypeRelations::enterUnify(*enter, tasks, answers);
    } else {
      TypeRelations::collectUnify(*task.as<CollectUnify>(), answers);
    }
  }

  return answers.back();
}

void TypeRelations::enterUnify(const EnterUnify &enter, std::vector<UnifyTask> &tasks,
                               std::vector<const Type *> &answers) {
  const Type *a = enter.a;
  const Type *b = enter.b;
  switch (enter.form) {
    case UnifyForm::WHOLE:
      return TypeRelations::enterWholeUnify(a, b, tasks, answers);
    case UnifyForm::HALF: {
      UnifyShortcut shortcut = ResultTypes::halfShortcut(a, b);
      if (shortcut.decided) {
        return answers.push_back(shortcut.type);
      }
      tasks.push_back(CollectUnify{a, b, UnifyForm::HALF});
      return tasks.push_back(EnterUnify{a, b, UnifyForm::UNION});
    }
    case UnifyForm::UNION:
      tasks.push_back(CollectUnify{a, b, UnifyForm::UNION});
      return tasks.push_back(EnterUnify{a, b, UnifyForm::WHOLE});
  }
}

void TypeRelations::enterWholeUnify(const Type *a, const Type *b, std::vector<UnifyTask> &tasks,
                                    std::vector<const Type *> &answers) {
  UnifyShortcut shortcut = TypeRelations::shortUnify(a, b);
  if (shortcut.decided) {
    return answers.push_back(shortcut.type);
  }
  const Type *x = a->nonOptional();
  const Type *y = b->nonOptional();
  if (x->isResult() && y->isResult() && x != y) {
    tasks.push_back(CollectUnify{a, b, UnifyForm::WHOLE});
    tasks.push_back(EnterUnify{x->errType(), y->errType(), UnifyForm::HALF});
    return tasks.push_back(EnterUnify{x->okType(), y->okType(), UnifyForm::HALF});
  }
  const Type *base = TypeRelations::baseUnify(x, y);

  return answers.push_back(TypeRelations::optionalLike(a, b, base));
}

void TypeRelations::collectUnify(const CollectUnify &collect, std::vector<const Type *> &answers) {
  const Type *a = collect.a;
  const Type *b = collect.b;
  switch (collect.form) {
    case UnifyForm::WHOLE: {
      const Type *error = ChildrenFirst::takeAnswer(answers);
      const Type *value = ChildrenFirst::takeAnswer(answers);
      const Type *base = value && error ? Type::result(value, error) : nullptr;
      return answers.push_back(TypeRelations::optionalLike(a, b, base));
    }
    case UnifyForm::HALF: {
      const Type *common = ChildrenFirst::takeAnswer(answers);
      return answers.push_back(common && !common->isError() ? common : nullptr);
    }
    case UnifyForm::UNION: {
      const Type *common = ChildrenFirst::takeAnswer(answers);
      return answers.push_back(common ? common : LiteralTypes::unionFallback(a, b));
    }
  }
}

const Type *TypeRelations::optionalLike(const Type *a, const Type *b, const Type *base) {
  if (!base) {
    return nullptr;
  }

  return a->isOptional() || b->isOptional() ? Type::optional(base) : base;
}

UnifyShortcut TypeRelations::shortUnify(const Type *a, const Type *b) {
  if (a == b) {
    return {true, a};
  }
  if (a->isError() || b->isError()) {
    return {true, Type::error()};
  }
  if (a->isNever()) {
    return {true, b};
  }
  if (b->isNever()) {
    return {true, a};
  }
  if (a->isVoid() || b->isVoid()) {
    return {true, nullptr};
  }
  if (a->isUndefined()) {
    return {true, Type::optional(b)};
  }
  if (b->isUndefined()) {
    return {true, Type::optional(a)};
  }
  if (a->isUnknown() || b->isUnknown()) {
    return {true, Type::unknown()};
  }
  const Type *x = a->nonOptional();
  const Type *y = b->nonOptional();
  if (x->isLiteral() || y->isLiteral() || x->isUnion() || y->isUnion()) {
    const Type *wider = TypeRelations::isImplicitlyConvertible(x, y)   ? y
                        : TypeRelations::isImplicitlyConvertible(y, x) ? x
                                                                       : nullptr;
    return {true, TypeRelations::optionalLike(a, b, wider)};
  }
  if (x->isString() && y->isString()) {
    return {true, TypeRelations::optionalLike(a, b, Type::string())};
  }

  return {false, nullptr};
}

// TODO: говнокод
const Type *TypeRelations::baseUnify(const Type *x, const Type *y) {
  auto number = [](const Type *t) { return t->isNumeric() || t->isDecimal(); };
  bool related = (number(x) && number(y)) || (x->isObject() && y->isObject()) ||
                 (x->isIterator() && y->isIterator());
  const Type *base = x == y ? x : related ? TypeRelations::commonNumericType(x, y) : nullptr;
  if (!base && (x->isJson() || y->isJson())) {
    base = TypeRelations::isImplicitlyConvertible(y, x)   ? x
           : TypeRelations::isImplicitlyConvertible(x, y) ? y
                                                          : nullptr;
  }
  if (!base && x->isClass() && y->isClass()) {
    base = ClassLinks::commonClass(x, y);
  }
  if (!base && x->isArray() && y->isArray()) {
    base = TypeRelations::isImplicitlyConvertible(x, y)   ? y
           : TypeRelations::isImplicitlyConvertible(y, x) ? x
                                                          : nullptr;
  }
  if (!base && x->isPromise() && y->isPromise()) {
    base = x->promised()->isNever() ? y : y->promised()->isNever() ? x : nullptr;
  }
  if (!base && x->isClassRef() && y->isClassRef()) {
    const Type *p = ClassValueChecker::constructedBy(x);
    const Type *q = ClassValueChecker::constructedBy(y);
    const Type *made = !p || !q                                       ? nullptr
                       : TypeRelations::isImplicitlyConvertible(p, q) ? q
                       : TypeRelations::isImplicitlyConvertible(q, p)
                           ? p
                           : ClassLinks::commonClass(p, q);
    base = Type::constructorOf(made ? made : Type::anyObject());
  }
  if (!base && (x->isAnyObject() || y->isAnyObject()) && UnknownChecker::isObjectReference(x) &&
      UnknownChecker::isObjectReference(y)) {
    base = Type::anyObject();
  }

  return base;
}

Expr *TypeRelations::convert(Expr *expr, const Type *to) {
  if (expr->type == to || expr->type->isError() || to->isError()) {
    return expr;
  }
  const Type *reflected = to->nonOptional();
  bool reflects = reflected->isConstructor() || reflected->isAnyObject() ||
                  reflected->isUnknown() ||
                  (reflected->isArray() && reflected->inner()->isUnknown());
  if (expr->type->isParam() && !TypeQueries::mentionsParam(to)) {
    this->genericChecker->noteForced(expr, to);
  }
  if ((to->isNumeric() || to->isDecimal()) && expr->type->isInteger()) {
    auto *unary = dyn_cast<UnaryExpr>(expr);
    auto *lit = dyn_cast<IntLitExpr>(unary && unary->op == UnaryOp::Neg ? unary->operand : expr);
    if (lit) {
      lit->type = to;
      if (unary) {
        unary->type = to;
      }
      return expr;
    }
  }
  const Type *from = expr->type->nonOptional();
  const Type *target = to->nonOptional();
  while (from->isArray() && target->isArray()) {
    from = from->inner()->nonOptional();
    target = target->inner()->nonOptional();
  }
  if (from->isObject() && target->isObject() && from != target && !from->isClass() &&
      !target->isClass() && !from->isAnyObject() && !target->isAnyObject() && !from->isBytes() &&
      !from->isSubtypeOf(target)) {
    this->state.getInventoryStore().addView(from, target);
  }
  auto *cast = this->state.getAst().make<CastExpr>(expr->loc(), expr, std::nullopt);
  cast->type = to;
  if (this->wideningRelations->widensWrites(expr->type, to)) {
    this->state.getWideningStore().rememberWideningCast(cast);
  }
  if (reflects) {
    this->reflectChecker->noteReflectedConversion(expr->type, to, cast);
  }
  this->reflectChecker->noteConstructorFlow(expr->type, to);

  return cast;
}

Expr *TypeRelations::coerce(Expr *expr, const Type *to, llvm::StringRef what) {
  const Type *from = expr->type;
  if (from == to || from->isError() || to->isError()) {
    return expr;
  }
  if (TypeRelations::isImplicitlyConvertible(from, to)) {
    return this->convert(expr, to);
  }
  if (this->literalAdapts(expr, to)) {
    return this->adaptLiteral(expr, to);
  }
  if (this->conditionalTypes->fitsConditional(from, to)) {
    return this->convert(expr, to);
  }
  Expr *adapted = this->adaptedValue(expr, to);
  if (adapted != nullptr) {
    return adapted;
  }
  std::vector<std::pair<int64_t, int64_t>> intervals = TypeRelations::targetIntervals(to);
  if (from->isInt() && this->provesSomeInterval(expr, intervals)) {
    return this->convert(expr, to);
  }
  ResultRangeWasProven proven = TypeRelations::rangeProven(expr, to, what, intervals);
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
    Expr *iterator = this->iteratorValue(expr, to);
    if (iterator != nullptr) {
      return iterator;
    }
  }
  if (to->nonOptional()->isAsyncIterator() && from->isObject() && !from->isAsyncIterator()) {
    Expr *iterator = this->asyncIteratorValue(expr, to);
    if (iterator != nullptr) {
      return iterator;
    }
  }
  const Type *function = to->isOptional() && to->inner()->isFunction() ? to->inner() : to;
  if (from->isFunction() && function->isFunction()) {
    return this->adaptedFunction(expr, function, to);
  }

  return nullptr;
}

Expr *TypeRelations::iteratorValue(Expr *expr, const Type *to) {
  Expr *iterator = this->iterableChecker->iteratorOf(expr);
  if (iterator == nullptr) {
    iterator = this->iterableChecker->userIterator(expr);
  }
  bool fits = iterator != nullptr && iterator->type->isIterator() &&
              TypeRelations::isImplicitlyConvertible(iterator->type, to);

  return fits ? this->convert(iterator, to) : nullptr;
}

Expr *TypeRelations::asyncIteratorValue(Expr *expr, const Type *to) {
  Expr *iterator = this->iterableChecker->asyncUserIterator(expr);
  bool fits = iterator != nullptr && iterator->type->isAsyncIterator() &&
              TypeRelations::isImplicitlyConvertible(iterator->type, to);

  return fits ? this->convert(iterator, to) : nullptr;
}

Expr *TypeRelations::adaptedFunction(Expr *expr, const Type *function, const Type *to) {
  Expr *adapted = this->functionValueChecker->adaptFunction(expr, function);
  if (adapted == nullptr) {
    adapted = this->tzModulesChecker->adaptResult(expr, function);
  }
  if (adapted == nullptr) {
    return nullptr;
  }

  return function == to ? adapted : this->convert(adapted, to);
}

std::vector<std::pair<int64_t, int64_t>> TypeRelations::targetIntervals(const Type *to) {
  const Type *target = to->nonOptional();
  std::optional<std::pair<int64_t, int64_t>> range = LiteralTypes::intRange(target);
  if (range.has_value()) {
    return {*range};
  }
  if (target->isUnion() && target->members().size() >= 64) {
    return LiteralTypes::intIntervals(target);
  }

  return {};
}

bool TypeRelations::provesSomeInterval(const Expr *expr,
                                       llvm::ArrayRef<std::pair<int64_t, int64_t>> intervals) {
  for (const std::pair<int64_t, int64_t> &interval : intervals) {
    if (this->byteProofs->provesFits(expr, interval.first, interval.second)) {
      return true;
    }
  }

  return false;
}

void TypeRelations::walkAssignable(const Expr *expr, const Type *to, llvm::StringRef what) {
  ResultTypeAssignable assigned = this->assignable(expr, to, what);
  const Type *other = this->otherPlace(expr, to);
  if (assigned.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), assigned.error().message);
  }
  if (assigned.isErr() && other != nullptr) {
    llvm::SMLoc otherLoc = this->placeLoc(expr, other);
    std::string alsoHere = ErrorTextAssignable::alsoHere(other->name());
    this->state.getDiagnostics().note(otherLoc, alsoHere);
  }
}

TypeRelations::ResultRangeWasProven
TypeRelations::rangeProven(const Expr *expr, const Type *to, llvm::StringRef what,
                           llvm::ArrayRef<std::pair<int64_t, int64_t>> intervals) {
  const Type *from = expr->type;
  bool integer = from->isInt() || (from->isLiteral() && from->literalBase()->isInt());
  bool outside = !intervals.empty() && integer;
  std::string prefix = ErrorTextAssignable::notTo(from->name(), to->name(), what);
  std::string holds = ErrorTextRange::holds(intervals);
  if (outside && !from->isInt()) {
    std::string message = prefix + holds;

    return err(TypeRelationsRangeProvenError{
        .code = TypeRelationsRangeProvenErrorCode::LITERAL_OUT_OF_RANGE,
        .message = message,
    });
  }
  if (outside) {
    std::string check = TypeRelations::rangeCheck(expr, to->nonOptional(), intervals);
    std::string message = prefix + holds + ErrorTextRange::outside(check);

    return err(TypeRelationsRangeProvenError{
        .code = TypeRelationsRangeProvenErrorCode::MAY_BE_OUT_OF_RANGE,
        .message = message,
    });
  }

  return ok(expr);
}

std::string TypeRelations::rangeCheck(const Expr *expr, const Type *target,
                                      llvm::ArrayRef<std::pair<int64_t, int64_t>> intervals) {
  auto narrower = [](const std::pair<int64_t, int64_t> &a, const std::pair<int64_t, int64_t> &b) {
    return a.second - a.first < b.second - b.first;
  };
  std::pair<int64_t, int64_t> widest = *llvm::max_element(intervals, narrower);
  std::string lo = std::to_string(widest.first);
  std::string hi = std::to_string(widest.second);
  std::string text = "x";
  if (auto *name = dyn_cast<NameExpr>(expr)) {
    text = name->name.str();
  }
  if (target->name() == "HttpStatus") {
    return ErrorTextRange::status(text);
  }

  return ErrorTextRange::between(text, lo, hi);
}

const Type *TypeRelations::innerObject(const Type *type, const Type *other) {
  const Type *inner = type->nonOptional();
  const Type *otherInner = other->nonOptional();
  while (inner->isArray() && otherInner->isArray()) {
    inner = inner->inner()->nonOptional();
    otherInner = otherInner->inner()->nonOptional();
  }

  return inner;
}

const Type *TypeRelations::otherPlace(const Expr *expr, const Type *to) const {
  const Type *toObject = TypeRelations::innerObject(to, expr->type);
  auto *name = dyn_cast<NameExpr>(expr);
  if (!name || !name->var()) {
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
  AssignAttempt attempt = TypeRelations::describeAttempt(expr, to);
  TypeRelationsAssignableErrorCode code = this->mismatchOf(attempt);
  std::string prefix = TypeRelations::mismatchPrefix(attempt, what);
  std::string reason = this->mismatchReason(attempt, code);
  std::string message = prefix + reason;

  return err(TypeRelationsAssignableError{
      .code = code,
      .message = message,
  });
}

TypeRelations::AssignAttempt TypeRelations::describeAttempt(const Expr *expr, const Type *to) {
  const Type *from = expr->type;

  return AssignAttempt{
      .expr = expr,
      .from = from,
      .to = to,
      .fromObject = TypeRelations::innerObject(from, to),
      .toObject = TypeRelations::innerObject(to, from),
  };
}

TypeRelationsAssignableErrorCode TypeRelations::mismatchOf(const AssignAttempt &attempt) const {
  const Type *from = attempt.from;
  const Type *to = attempt.to;
  const Type *fromObject = attempt.fromObject;
  const Type *toObject = attempt.toObject;
  const ClassDecl *fromClass = fromObject->classDecl();
  const ClassDecl *genericClass = from->classRef();
  bool genericValue =
      genericClass != nullptr && genericClass->isGeneric() && toObject->isConstructor();
  if (genericValue) {
    return TypeRelationsAssignableErrorCode::GENERIC_CLASS_VALUE;
  }
  if (from->isUnknown() || from->isAnyObject()) {
    return TypeRelationsAssignableErrorCode::UNKNOWN_VALUE;
  }
  bool literalObject = fromClass == nullptr || fromClass->literal();
  if (fromObject->isObject() && toObject->isClass() && literalObject) {
    return TypeRelationsAssignableErrorCode::LITERAL_NOT_INSTANCE;
  }
  if (fromClass != nullptr && fromClass->literal() && toObject->isObject()) {
    return TypeRelationsAssignableErrorCode::LITERAL_WITH_METHODS;
  }
  std::string widening = GenericWidening::whyNot(fromObject, toObject);
  if (!widening.empty()) {
    return TypeRelationsAssignableErrorCode::GENERIC_WIDENING;
  }
  bool classValue = fromObject->isClass() && toObject->isObject() && fromObject != toObject;
  if (classValue && toObject->isClass()) {
    return TypeRelationsAssignableErrorCode::CLASS_NOT_EXTENDED;
  }
  if (classValue) {
    return TypeRelationsAssignableErrorCode::CLASS_NOT_IMPLEMENTED;
  }
  bool objectIntoUnion = fromObject->isObject() && !fromObject->isClass() && toObject->isUnion() &&
                         llvm::any_of(toObject->members(), TypeRelations::isObjectType);
  if (objectIntoUnion) {
    return TypeRelationsAssignableErrorCode::OBJECT_INTO_UNION;
  }
  if (fromObject->isTuple() && toObject->isArray()) {
    return TypeRelationsAssignableErrorCode::TUPLE_NOT_ARRAY;
  }
  bool objectsDiffer = fromObject->isObject() && toObject->isObject() &&
                       !TypeRelations::isImplicitlyConvertible(fromObject, toObject);
  if (objectsDiffer) {
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
  bool intoJson = to->nonOptional()->isJson();
  if (intoJson && (fromObject->isObject() || value->isArray())) {
    return TypeRelationsAssignableErrorCode::OBJECT_INTO_JSON;
  }
  if (intoJson && value->isRecord() && TypeRelations::holdsWideInteger(value->valueType())) {
    return TypeRelationsAssignableErrorCode::WIDE_INTEGERS_INTO_JSON;
  }
  if (intoJson && value->isDecimal()) {
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

std::string TypeRelations::mismatchPrefix(const AssignAttempt &attempt, llvm::StringRef what) {
  const Type *from = attempt.from;
  const Type *to = attempt.to;
  const Type *function = to->isOptional() && to->inner()->isFunction() ? to->inner() : to;
  std::string prefix = ErrorTextAssignable::notTo(from->name(), to->name(), what);
  if (from->isFunction() && function->isFunction()) {
    prefix += ErrorTextAssignable::functions();
  }
  if (TypeRelations::isStreamIntoBody(attempt)) {
    prefix += ErrorTextAssignable::stream();
  }
  if (TypeRelations::isRequestInit(attempt)) {
    prefix += ErrorTextAssignable::request();
  }
  if (to->isNever()) {
    prefix += ErrorTextAssignable::never();
  }

  return prefix;
}

bool TypeRelations::isStreamIntoBody(const AssignAttempt &attempt) {
  const Type *toObject = attempt.toObject;
  llvm::ArrayRef<const Type *> wanted =
      toObject->isUnion() ? toObject->members() : llvm::ArrayRef<const Type *>(toObject);

  return TypeRelations::isWebStream(attempt.fromObject) &&
         llvm::any_of(wanted, TypeRelations::isFormData) &&
         llvm::none_of(wanted, TypeRelations::isWebStream);
}

bool TypeRelations::isWebStream(const Type *type) {
  const ClassDecl *cls = type->classDecl();

  return cls != nullptr && cls->library() && cls->generic() != nullptr &&
         cls->generic()->name == "ReadableStream" && cls->typeArgs.size() == 2;
}

bool TypeRelations::isFormData(const Type *type) {
  const ClassDecl *cls = type->classDecl();

  return cls != nullptr && cls->library() && cls->name == "FormData";
}

bool TypeRelations::isObjectType(const Type *type) {
  return type->isObject();
}

bool TypeRelations::isRequestInit(const AssignAttempt &attempt) {
  llvm::StringRef fromName = attempt.fromObject->name();

  return fromName.starts_with("RequestInit") && attempt.toObject->name() == "RequestOptions";
}

std::string TypeRelations::mismatchReason(const AssignAttempt &attempt,
                                          TypeRelationsAssignableErrorCode code) const {
  const Type *fromObject = attempt.fromObject;
  const Type *toObject = attempt.toObject;
  switch (code) {
    case TypeRelationsAssignableErrorCode::GENERIC_CLASS_VALUE:
      return TypeRelations::genericClassText(attempt.from->classRef());
    case TypeRelationsAssignableErrorCode::UNKNOWN_VALUE:
      return this->unknownChecker->unknownHint(attempt.expr, attempt.to);
    case TypeRelationsAssignableErrorCode::LITERAL_NOT_INSTANCE:
      return ErrorTextAssignable::literal(toObject->name());
    case TypeRelationsAssignableErrorCode::LITERAL_WITH_METHODS:
      return ErrorTextAssignable::methods();
    case TypeRelationsAssignableErrorCode::GENERIC_WIDENING:
      return GenericWidening::whyNot(fromObject, toObject);
    case TypeRelationsAssignableErrorCode::CLASS_NOT_EXTENDED:
      return ErrorTextAssignable::classes(fromObject->name(), toObject->name());
    case TypeRelationsAssignableErrorCode::CLASS_NOT_IMPLEMENTED:
      return ErrorTextAssignable::implements(toObject->name());
    case TypeRelationsAssignableErrorCode::OBJECT_INTO_UNION:
      return TypeRelations::unionText(toObject);
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
      return TypeRelations::conversionHint(attempt.expr, attempt.to);
  }

  return TypeRelations::conversionHint(attempt.expr, attempt.to);
}

std::string TypeRelations::genericClassText(const ClassDecl *generic) {
  std::string name = generic->name.str();
  std::string example = generic->library() ? ErrorTextAssignable::extendsBus(name)
                                           : ErrorTextAssignable::extendsInt(name);

  return ErrorTextAssignable::generic(name, example);
}

std::string TypeRelations::unionText(const Type *toObject) {
  std::string text = ErrorTextAssignable::inUnion();
  const Type *front = toObject->members().front();
  if (!front->isObject()) {
    return text;
  }
  for (const Field &field : front->fields()) {
    if (TypeRelations::allHaveLiteralField(toObject->members(), field.name)) {
      text += ErrorTextAssignable::withValue(field.name);
      break;
    }
  }

  return text;
}

bool TypeRelations::allHaveLiteralField(llvm::ArrayRef<const Type *> members,
                                        llvm::StringRef name) {
  for (const Type *member : members) {
    const Field *field = member->isObject() ? member->field(name) : nullptr;
    bool literal = field != nullptr && field->type->isLiteral();
    if (!literal) {
      return false;
    }
  }

  return true;
}

std::string TypeRelations::objectMismatchText(const AssignAttempt &attempt) const {
  const Type *other = this->otherPlace(attempt.expr, attempt.to);
  std::string text =
      this->objectChecker->objectMismatch(attempt.fromObject, attempt.toObject, other != nullptr);
  if (other != nullptr) {
    text += ErrorTextAssignable::both(other->name());
  }

  return text;
}

const Type *TypeRelations::comparisonType(const Type *a, const Type *b) {
  const Type *common = a->isVoid() || b->isVoid() ? nullptr : TypeRelations::unify(a, b);
  if (common && common->isError()) {
    return common;
  }
  const Type *base = common ? common->nonOptional() : nullptr;
  if (base && (base->isNumeric() || base->isDecimal() || base->isBool() || base->isString() ||
               base->isArray() || base->isFunction() || base->isObject() || base->isIterator() ||
               base->isJson() || base->isDict() || base->isUnknown() || base->isClassRef() ||
               base->isLiteral() || (base->isUnion() && !UnionSlots::hasParamMember(base)))) {
    return common;
  }

  return nullptr;
}

}
