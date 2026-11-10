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
  this->byteProofs = &all.getByteProofs();
  this->objectChecker = &all.getObjectChecker();
  this->constantFolder = &all.getConstantFolder();
  this->genericChecker = &all.getGenericChecker();
  this->reflectChecker = &all.getReflectChecker();
  this->unknownChecker = &all.getUnknownChecker();
  this->iterableChecker = &all.getIterableChecker();
  this->conditionalTypes = &all.getConditionalTypes();
  this->tzModulesChecker = &all.getTzModulesChecker();
  this->wideningRelations = &all.getWideningRelations();
  this->functionValueChecker = &all.getFunctionValueChecker();
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
  if (to->isUnknown()) {
    return TypeRelations::stepDecided(!from->isVoid());
  }
  if (to->isAnyObject()) {
    if (UnknownChecker::isObjectReference(from)) {
      return TypeRelations::stepHolds();
    }
    bool constrained = from->isParam() && from->constraint();
    return constrained ? TypeRelations::stepNeeds({{from->constraint(), to}})
                       : TypeRelations::stepDecided(false);
  }
  if (to->isConstructor() && from->isClassRef()) {
    if (from->classRef() && from->classRef()->isGeneric()) {
      return TypeRelations::stepDecided(false);
    }
    const Type *made = ClassValueChecker::constructedBy(from);
    return made ? TypeRelations::stepNeeds({{made, to->constructed()}})
                : TypeRelations::stepDecided(to->constructed()->isAnyObject());
  }
  if (to->isJson()) {
    return TypeRelations::jsonStep(from, to);
  }
  if (to->isOptional()) {
    return from->isUndefined() ? TypeRelations::stepHolds()
                               : TypeRelations::stepNeeds({{from->nonOptional(), to->inner()}});
  }
  if (to->isUnion()) {
    return TypeRelations::memberGoals(from, to);
  }
  if (from->isUnion()) {
    if (from->isTagged() || UnionSlots::hasParamMember(from)) {
      return TypeRelations::stepDecided(false);
    }
    std::vector<ConversionGoal> members;
    for (const Type *m : from->members()) {
      members.push_back(ConversionGoal{m, to});
    }
    return TypeRelations::stepNeeds(std::move(members));
  }
  if (to->isPattern()) {
    return TypeRelations::stepDecided(from->isStringLiteral() &&
                                      TemplatePatterns::matchesPattern(from->literalText(), to));
  }
  if (from->isPattern()) {
    return TypeRelations::stepDecided(to == Type::string());
  }
  if (from->isLiteral()) {
    return to->isLiteral() ? TypeRelations::stepDecided(false)
                           : TypeRelations::stepNeeds({{from->literalBase(), to}});
  }
  if (from->isParam() && from->constraint()) {
    return TypeRelations::stepNeeds({{from->constraint(), to}});
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
  if (to->nonOptional()->isIterator() && from->isObject()) {
    Expr *iterator = this->iterableChecker->iteratorOf(expr);
    if (!iterator) {
      iterator = this->iterableChecker->userIterator(expr);
    }
    if (iterator && iterator->type->isIterator() &&
        TypeRelations::isImplicitlyConvertible(iterator->type, to)) {
      return this->convert(iterator, to);
    }
  }
  if (to->nonOptional()->isAsyncIterator() && from->isObject() && !from->isAsyncIterator()) {
    if (Expr *iterator = this->iterableChecker->asyncUserIterator(expr);
        iterator && iterator->type->isAsyncIterator() &&
        TypeRelations::isImplicitlyConvertible(iterator->type, to)) {
      return this->convert(iterator, to);
    }
  }
  const Type *function = to->isOptional() && to->inner()->isFunction() ? to->inner() : to;
  if (from->isFunction() && function->isFunction()) {
    if (Expr *adapted = this->functionValueChecker->adaptFunction(expr, function)) {
      return function == to ? adapted : this->convert(adapted, to);
    }
    if (Expr *adapted = this->tzModulesChecker->adaptResult(expr, function)) {
      return function == to ? adapted : this->convert(adapted, to);
    }
  }

  const Type *target = to->nonOptional();
  std::vector<std::pair<int64_t, int64_t>> intervals;
  if (std::optional<std::pair<int64_t, int64_t>> range = LiteralTypes::intRange(target)) {
    intervals.push_back(*range);
  } else if (target->isUnion() && target->members().size() >= 64) {
    intervals = LiteralTypes::intIntervals(target);
  }
  if (from->isInt() && llvm::any_of(intervals, [&](const std::pair<int64_t, int64_t> &interval) {
        return this->byteProofs->provesFits(expr, interval.first, interval.second);
      })) {
    return this->convert(expr, to);
  }

  ResultRangeWasProven proven = TypeRelations::rangeProven(expr, to, what, intervals);
  if (proven.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), proven.error().message);
    return expr;
  }

  ResultTypeAssignable assigned = this->assignable(expr, to, what);
  const Type *other = this->otherPlace(expr, to);
  if (assigned.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), assigned.error().message);
  }
  if (assigned.isErr() && other) {
    llvm::SMLoc otherLoc = this->placeLoc(expr, other);
    std::string alsoHere = ErrorTextAssignable::alsoHere(other->name());
    this->state.getDiagnostics().note(otherLoc, alsoHere);
  }

  return expr;
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
  const Type *from = expr->type;
  const Type *fromObject = TypeRelations::innerObject(from, to);
  const Type *toObject = TypeRelations::innerObject(to, from);
  const Type *function = to->isOptional() && to->inner()->isFunction() ? to->inner() : to;
  auto stream = [](const Type *t) -> const ClassDecl * {
    const ClassDecl *cls = t->classDecl();
    return cls && cls->library() && cls->generic() && cls->generic()->name == "ReadableStream" &&
                   cls->typeArgs.size() == 2
               ? cls
               : nullptr;
  };
  auto body = [](const Type *m) {
    const ClassDecl *cls = m->classDecl();
    return cls && cls->library() && cls->name == "FormData";
  };
  auto isStream = [&](const Type *m) { return stream(m) != nullptr; };
  llvm::ArrayRef<const Type *> wanted =
      toObject->isUnion() ? toObject->members() : llvm::ArrayRef<const Type *>(toObject);
  bool streamBody =
      stream(fromObject) && llvm::any_of(wanted, body) && llvm::none_of(wanted, isStream);
  bool requestInit = llvm::StringRef(fromObject->name()).starts_with("RequestInit") &&
                     toObject->name() == "RequestOptions";

  std::string prefix = ErrorTextAssignable::notTo(from->name(), to->name(), what);
  if (from->isFunction() && function->isFunction()) {
    prefix += ErrorTextAssignable::functions();
  }
  if (streamBody) {
    prefix += ErrorTextAssignable::stream();
  }
  if (requestInit) {
    prefix += ErrorTextAssignable::request();
  }
  if (to->isNever()) {
    prefix += ErrorTextAssignable::never();
  }

  const Type *other = this->otherPlace(expr, to);
  const ClassDecl *fromClass = fromObject->classDecl();
  if (from->classRef() && from->classRef()->isGeneric() && toObject->isConstructor()) {
    const ClassDecl *generic = from->classRef();
    std::string name = generic->name.str();
    std::string example = generic->library() ? ErrorTextAssignable::extendsBus(name)
                                             : ErrorTextAssignable::extendsInt(name);
    std::string message = prefix + ErrorTextAssignable::generic(name, example);

    return err(TypeRelationsAssignableError{
        .code = TypeRelationsAssignableErrorCode::GENERIC_CLASS_VALUE,
        .message = message,
    });
  }
  if (from->isUnknown() || from->isAnyObject()) {
    std::string hint = this->unknownChecker->unknownHint(expr, to);
    std::string message = prefix + hint;

    return err(TypeRelationsAssignableError{
        .code = TypeRelationsAssignableErrorCode::UNKNOWN_VALUE,
        .message = message,
    });
  }
  if (fromObject->isObject() && toObject->isClass() && (!fromClass || fromClass->literal())) {
    std::string message = prefix + ErrorTextAssignable::literal(toObject->name());

    return err(TypeRelationsAssignableError{
        .code = TypeRelationsAssignableErrorCode::LITERAL_NOT_INSTANCE,
        .message = message,
    });
  }
  if (fromClass && fromClass->literal() && toObject->isObject()) {
    std::string message = prefix + ErrorTextAssignable::methods();

    return err(TypeRelationsAssignableError{
        .code = TypeRelationsAssignableErrorCode::LITERAL_WITH_METHODS,
        .message = message,
    });
  }

  std::string why = GenericWidening::whyNot(fromObject, toObject);
  if (!why.empty()) {
    std::string message = prefix + why;

    return err(TypeRelationsAssignableError{
        .code = TypeRelationsAssignableErrorCode::GENERIC_WIDENING,
        .message = message,
    });
  }

  bool classValue = fromObject->isClass() && toObject->isObject() && fromObject != toObject;
  if (classValue && toObject->isClass()) {
    std::string message =
        prefix + ErrorTextAssignable::classes(fromObject->name(), toObject->name());

    return err(TypeRelationsAssignableError{
        .code = TypeRelationsAssignableErrorCode::CLASS_NOT_EXTENDED,
        .message = message,
    });
  }
  if (classValue) {
    std::string message = prefix + ErrorTextAssignable::implements(toObject->name());

    return err(TypeRelationsAssignableError{
        .code = TypeRelationsAssignableErrorCode::CLASS_NOT_IMPLEMENTED,
        .message = message,
    });
  }

  auto isObject = [](const Type *m) { return m->isObject(); };
  bool objectIntoUnion = fromObject->isObject() && !fromObject->isClass() && toObject->isUnion() &&
                         llvm::any_of(toObject->members(), isObject);
  if (objectIntoUnion) {
    std::string message = prefix + ErrorTextAssignable::inUnion();
    const Type *front = toObject->members().front();
    for (const Field &f : front->isObject() ? front->fields() : llvm::ArrayRef<Field>()) {
      auto literalField = [&](const Type *m) {
        return m->isObject() && m->field(f.name) && m->field(f.name)->type->isLiteral();
      };
      if (llvm::all_of(toObject->members(), literalField)) {
        message += ErrorTextAssignable::withValue(f.name);
        break;
      }
    }

    return err(TypeRelationsAssignableError{
        .code = TypeRelationsAssignableErrorCode::OBJECT_INTO_UNION,
        .message = message,
    });
  }
  if (fromObject->isTuple() && toObject->isArray()) {
    std::string message = prefix + ErrorTextAssignable::tuple();

    return err(TypeRelationsAssignableError{
        .code = TypeRelationsAssignableErrorCode::TUPLE_NOT_ARRAY,
        .message = message,
    });
  }

  bool objectsDiffer = fromObject->isObject() && toObject->isObject() &&
                       !TypeRelations::isImplicitlyConvertible(fromObject, toObject);
  if (objectsDiffer) {
    std::string mismatch =
        this->objectChecker->objectMismatch(fromObject, toObject, other != nullptr);
    std::string message = prefix + mismatch;
    if (other) {
      message += ErrorTextAssignable::both(other->name());
    }

    return err(TypeRelationsAssignableError{
        .code = TypeRelationsAssignableErrorCode::OBJECT_MISMATCH,
        .message = message,
    });
  }
  if (from->isOptional() && TypeRelations::isImplicitlyConvertible(from->inner(), to)) {
    std::string message = prefix + ErrorTextAssignable::undefined();

    return err(TypeRelationsAssignableError{
        .code = TypeRelationsAssignableErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  if (from->nonOptional()->isJson()) {
    std::string message = prefix + ErrorTextAssignable::json();

    return err(TypeRelationsAssignableError{
        .code = TypeRelationsAssignableErrorCode::JSON_UNCHECKED,
        .message = message,
    });
  }
  if (to->nonOptional()->isAnyObject()) {
    std::string message = prefix + ErrorTextAssignable::object();

    return err(TypeRelationsAssignableError{
        .code = TypeRelationsAssignableErrorCode::NOT_AN_OBJECT,
        .message = message,
    });
  }

  bool intoJson = to->nonOptional()->isJson();
  if (intoJson && (fromObject->isObject() || from->nonOptional()->isArray())) {
    std::string message = prefix + ErrorTextAssignable::jsonObject();

    return err(TypeRelationsAssignableError{
        .code = TypeRelationsAssignableErrorCode::OBJECT_INTO_JSON,
        .message = message,
    });
  }
  if (intoJson && from->nonOptional()->isRecord() &&
      TypeRelations::holdsWideInteger(from->nonOptional()->valueType())) {
    std::string message = prefix + ErrorTextAssignable::wideJson();

    return err(TypeRelationsAssignableError{
        .code = TypeRelationsAssignableErrorCode::WIDE_INTEGERS_INTO_JSON,
        .message = message,
    });
  }
  if (intoJson && from->nonOptional()->isDecimal()) {
    std::string message = prefix + ErrorTextAssignable::noDecimal();

    return err(TypeRelationsAssignableError{
        .code = TypeRelationsAssignableErrorCode::DECIMAL_INTO_JSON,
        .message = message,
    });
  }
  if (from->isFloat() && to->isDecimal()) {
    std::string message = prefix + ErrorTextAssignable::toDecimal();

    return err(TypeRelationsAssignableError{
        .code = TypeRelationsAssignableErrorCode::DOUBLE_INTO_DECIMAL,
        .message = message,
    });
  }
  if (from->isDecimal() && (to->isNumeric() || to->nonOptional()->isNumeric())) {
    std::string message = prefix + ErrorTextAssignable::toNumber();

    return err(TypeRelationsAssignableError{
        .code = TypeRelationsAssignableErrorCode::DECIMAL_INTO_NUMBER,
        .message = message,
    });
  }

  std::string hint = TypeRelations::conversionHint(expr, to);
  std::string message = prefix + hint;

  return err(TypeRelationsAssignableError{
      .code = TypeRelationsAssignableErrorCode::NOT_ASSIGNABLE,
      .message = message,
  });
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
