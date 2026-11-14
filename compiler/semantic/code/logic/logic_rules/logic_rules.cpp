#include "compiler/semantic/code/logic/logic_rules/logic_rules.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/program/library/builtin_names/keyword_table/keyword_table.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "llvm/ADT/StringExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr llvm::StringLiteral kHiddenPrefix = "%";
constexpr const char *kEllipsis = "...";
constexpr const char *kArgumentSeparator = ", ";
constexpr char16_t kFirstPrintable = 0x20;
constexpr char16_t kFirstNonAscii = 0x7f;

}

LogicRules::LogicRules(SemanticState &owner)
    : state(owner) {}

void LogicRules::pushValueFacts(const Expr *value) {
  auto flows = ConditionFlows{
      .whenTrue = this->state.walk.flow.getFlow(),
      .whenFalse = this->state.walk.flow.getFlow(),
      .constant = std::nullopt,
  };
  const auto *literal = dyn_cast<BoolLitExpr>(value);
  if (literal != nullptr) {
    Flow &impossible = literal->value ? flows.whenFalse : flows.whenTrue;
    impossible.unreachable = true;
    return this->state.walk.tasks.pushConditionFlows(std::move(flows));
  }
  this->state.walk.tasks.pushConditionFlows(std::move(flows));

  return this->state.walk.tasks.pushTask(AddConditionFacts{.cond = value});
}

std::string LogicRules::sourceText(const Expr *expr) const {
  auto answers = std::vector<std::string>();
  for (const Expr *node : ChildrenFirst::order(expr, this, &LogicRules::sourceParts)) {
    auto text = this->nodeSource(node, answers);
    if (text.empty()) {
      return "";
    }
    answers.push_back(std::move(text));
  }

  return answers.back();
}

NodeChildren<const Expr *> LogicRules::sourceParts(const Expr *expr) const {
  auto *conversion = dyn_cast<CastExpr>(expr);
  auto *chain = dyn_cast<OptionalChainExpr>(expr);
  auto *member = dyn_cast<MemberExpr>(expr);
  auto *index = dyn_cast<IndexExpr>(expr);
  auto *call = dyn_cast<CallExpr>(expr);
  if (conversion != nullptr && conversion->isImplicit()) {
    return {conversion->operand};
  }
  if (chain != nullptr) {
    return {chain->chain};
  }
  if (member != nullptr) {
    return {member->object};
  }
  if (index != nullptr) {
    return {index->object, index->index};
  }
  if (call == nullptr || call->hasSpread()) {
    return {};
  }
  auto parts = NodeChildren<const Expr *>{call->callee};
  parts.append(call->args.begin(), call->args.end());

  return parts;
}

std::string LogicRules::nodeSource(const Expr *expr, std::vector<std::string> &answers) const {
  auto *conversion = dyn_cast<CastExpr>(expr);
  auto *name = dyn_cast<NameExpr>(expr);
  auto *integer = dyn_cast<IntLitExpr>(expr);
  auto *boolean = dyn_cast<BoolLitExpr>(expr);
  auto *text = dyn_cast<StringLitExpr>(expr);
  auto *list = dyn_cast<ArrayLitExpr>(expr);
  auto *member = dyn_cast<MemberExpr>(expr);
  auto *index = dyn_cast<IndexExpr>(expr);
  auto *call = dyn_cast<CallExpr>(expr);
  bool isPassing =
      (conversion != nullptr && conversion->isImplicit()) || isa<OptionalChainExpr>(expr);
  if (isPassing) {
    return ChildrenFirst::takeAnswer(answers);
  }
  if (name != nullptr) {
    return name->name.starts_with(kHiddenPrefix) ? "" : name->name.str();
  }
  if (integer != nullptr) {
    return std::to_string(integer->value);
  }
  if (boolean != nullptr) {
    Keyword written = boolean->value ? Keyword::TRUE_KEYWORD : Keyword::FALSE_KEYWORD;
    return KeywordTable::getName(written).str();
  }
  if (isa<UndefinedExpr>(expr)) {
    return KeywordTable::getName(Keyword::UNDEFINED_KEYWORD).str();
  }
  if (text != nullptr) {
    return this->stringSource(text);
  }
  if (list != nullptr) {
    return list->elements.empty() ? "[]" : "";
  }
  if (member != nullptr) {
    return this->memberSource(member, answers);
  }
  if (index != nullptr) {
    return this->indexSource(index, answers);
  }
  if (call != nullptr) {
    return this->callSource(call, answers);
  }

  return "";
}

std::string LogicRules::memberSource(const MemberExpr *member,
                                     std::vector<std::string> &answers) const {
  auto object = ChildrenFirst::takeAnswer(answers);
  const char *dot = member->optional ? "?." : ".";

  return object + dot + member->name.str();
}

std::string LogicRules::indexSource(const IndexExpr *index,
                                    std::vector<std::string> &answers) const {
  auto position = ChildrenFirst::takeAnswer(answers);
  auto object = ChildrenFirst::takeAnswer(answers);
  const char *open = index->optional ? "?.[" : "[";

  return object + open + position + "]";
}

std::string LogicRules::stringSource(const StringLitExpr *literal) const {
  auto text = std::string("\"");
  for (char16_t unit : literal->value) {
    bool isPlain = unit >= kFirstPrintable && unit < kFirstNonAscii && unit != '"' && unit != '\\';
    if (!isPlain) {
      return "";
    }
    text += static_cast<char>(unit);
  }

  return text + "\"";
}

std::string LogicRules::callSource(const CallExpr *call, std::vector<std::string> &answers) const {
  if (call->hasSpread()) {
    return "";
  }
  auto args = ChildrenFirst::takeAnswers(answers, call->args.size());
  auto callee = ChildrenFirst::takeAnswer(answers);
  const char *open = call->optional ? "?.(" : "(";
  auto joined = llvm::join(args, kArgumentSeparator);

  return callee + open + joined + ")";
}

std::string LogicRules::hintText(const Expr *expr, const char *placeholder) const {
  std::string text = expr != nullptr ? this->sourceText(expr) : "";

  return text.empty() ? placeholder : text;
}

const NameExpr *LogicRules::narrowedVariable(const Expr *expr) const {
  auto *conversion = dyn_cast<CastExpr>(expr);
  if (conversion == nullptr) {
    return nullptr;
  }
  const Type *from = conversion->operand->type;
  bool isUnwrapping = conversion->isImplicit() && !conversion->shortCircuit && from->isOptional() &&
                      conversion->type == from->inner();
  if (!isUnwrapping) {
    return nullptr;
  }

  return dyn_cast<NameExpr>(conversion->operand);
}

bool LogicRules::isLength(const Expr *expr) const {
  const auto *inner = this->withoutWrappers(expr);
  auto *member = dyn_cast<MemberExpr>(inner);

  return member != nullptr && BuiltinPropertyTable::isName(member->name, BuiltinProperty::LENGTH);
}

const Expr *LogicRules::withoutWrappers(const Expr *expr) const {
  const Expr *inner = expr;
  const auto *next = this->wrappedExpr(inner);
  while (next != nullptr) {
    inner = next;
    next = this->wrappedExpr(inner);
  }

  return inner;
}

const Expr *LogicRules::wrappedExpr(const Expr *expr) const {
  auto *conversion = dyn_cast<CastExpr>(expr);
  auto *chain = dyn_cast<OptionalChainExpr>(expr);
  if (conversion != nullptr) {
    return conversion->operand;
  }
  if (chain != nullptr) {
    return chain->chain;
  }

  return nullptr;
}

llvm::StringRef LogicRules::useOperator(TruthUse use) const {
  switch (use) {
    case TruthUse::Condition:
      return "";
    case TruthUse::Not:
      return "!";
    case TruthUse::Or:
      return "||";
    case TruthUse::And:
      return "&&";
    case TruthUse::OrAssign:
      return "||=";
    case TruthUse::AndAssign:
      return "&&=";
  }

  return "";
}

std::string LogicRules::paramMessage(const Type *type, const Type *base, TruthUse use,
                                     llvm::StringRef valueText, llvm::StringRef otherText) const {
  bool isNegated = use == TruthUse::Not;
  auto what = ErrorTextTruthy::typeValue(type->name());
  auto head = ErrorTextTruthy::cannotTest(this->useOperator(use), what) +
              ErrorTextTruthy::mayBeNum(base->name());
  if (use == TruthUse::Or) {
    return head + ErrorTextTruthy::orDefault(valueText, otherText);
  }
  if (use == TruthUse::OrAssign) {
    return head + ErrorTextTruthy::orAssign(valueText, otherText);
  }
  if (use == TruthUse::AndAssign) {
    return head + ErrorTextTruthy::andAssign(valueText, otherText);
  }

  return head + ErrorTextTruthy::undefTest(valueText, isNegated);
}

std::string LogicRules::jsonMessage(TruthUse use, llvm::StringRef valueText,
                                    llvm::StringRef otherText) const {
  bool isNegated = use == TruthUse::Not;
  auto head = ErrorTextTruthy::jsonHead(this->useOperator(use)) + ErrorTextTruthy::jsonMay();
  if (use == TruthUse::Or) {
    return head + ErrorTextTruthy::jsonOr(valueText, otherText);
  }
  if (use == TruthUse::OrAssign) {
    return head + ErrorTextTruthy::orAssign(valueText, otherText);
  }

  return head + ErrorTextTruthy::jsonTest(valueText, isNegated);
}

std::string LogicRules::unknownMessage(TruthUse use, llvm::StringRef valueText,
                                       llvm::StringRef otherText) const {
  bool isNegated = use == TruthUse::Not;
  auto head = ErrorTextTruthy::unknownHd(this->useOperator(use)) + ErrorTextTruthy::unknownMay();
  if (use == TruthUse::Or) {
    return head + ErrorTextTruthy::orDefault(valueText, otherText);
  }
  if (use == TruthUse::OrAssign) {
    return head + ErrorTextTruthy::orAssign(valueText, otherText);
  }

  return head + ErrorTextTruthy::unknownTst(valueText, isNegated);
}

std::string LogicRules::numberMessage(const Expr *expr, const Type *type, const Type *base,
                                      TruthUse use, llvm::StringRef valueText,
                                      llvm::StringRef otherText) const {
  bool isNegated = use == TruthUse::Not;
  bool isLength = this->isLength(expr);
  auto what = ErrorTextTruthy::number(base->isDecimal(), type->isOptional());
  auto head = ErrorTextTruthy::cannotTest(this->useOperator(use), what) + ErrorTextTruthy::zero();
  if (type->isOptional()) {
    auto advice = this->optionalNumberAdvice(use, isLength, valueText, otherText);

    return head + ErrorTextTruthy::too() + advice;
  }
  if (base->isFloat()) {
    return head + ErrorTextTruthy::floatTest(valueText, isNegated);
  }

  auto test = ErrorTextTruthy::zeroTest(valueText, isNegated, isLength);
  if (use == TruthUse::Or) {
    return head + ErrorTextTruthy::orCompare(test, valueText, otherText);
  }
  if (use == TruthUse::OrAssign) {
    return head + ErrorTextTruthy::orAssignIf(valueText, otherText);
  }
  if (use == TruthUse::AndAssign) {
    return head + ErrorTextTruthy::andIf(test, valueText, otherText);
  }

  return head + ErrorTextTruthy::compare(test);
}

std::string LogicRules::optionalNumberAdvice(TruthUse use, bool isLength, llvm::StringRef valueText,
                                             llvm::StringRef otherText) const {
  bool isNegated = use == TruthUse::Not;
  bool isLengthUse =
      isLength && (use == TruthUse::Condition || use == TruthUse::Not || use == TruthUse::And);
  if (isLengthUse) {
    return ErrorTextTruthy::lengthTest(valueText, isNegated);
  }
  if (use == TruthUse::Or) {
    return ErrorTextTruthy::orDefault(valueText, otherText);
  }
  if (use == TruthUse::OrAssign) {
    return ErrorTextTruthy::orAssign(valueText, otherText);
  }

  return ErrorTextTruthy::undefTest(valueText, isNegated);
}

std::string LogicRules::objectMessage(const Expr *expr, const Type *base, TruthUse use,
                                      llvm::StringRef valueText, llvm::StringRef otherText) const {
  auto truth = this->objectTruth(expr, base);
  auto message = this->objectHead(truth, use);
  auto advice = this->objectAdvice(truth, base, use, valueText, otherText);

  return message + advice;
}

LogicRules::ObjectTruth LogicRules::objectTruth(const Expr *expr, const Type *base) const {
  bool isClass = base->isClassRef();
  bool isPromise = base->isPromise();
  const auto *bare = ExprParts::withoutImplicitCasts(expr);
  auto *name = dyn_cast<NameExpr>(bare);
  bool isClassName = isClass && name != nullptr && name->cls() != nullptr && name->var() == nullptr;

  return ObjectTruth{
      .narrowed = this->narrowedVariable(expr),
      .isArray = base->isArray(),
      .isPromise = isPromise,
      .isClass = isClass,
      .isObject = base->isObject() || base->isIterator() || base->isDict() || isClass || isPromise,
      .isClassName = isClassName,
  };
}

std::string LogicRules::objectHead(const ObjectTruth &truth, TruthUse use) const {
  auto what = ErrorTextTruthy::kind(truth.isArray, truth.isPromise, truth.isClass, truth.isObject);
  auto subject = this->objectSubject(truth, what);
  auto consequence = this->useConsequence(use);

  return subject + consequence;
}

std::string LogicRules::objectSubject(const ObjectTruth &truth, llvm::StringRef what) const {
  if (truth.narrowed != nullptr) {
    return ErrorTextTruthy::holds(truth.narrowed->name, what);
  }
  if (truth.isArray) {
    return ErrorTextTruthy::arrayTrue();
  }

  return ErrorTextTruthy::alwaysTrue(what);
}

std::string LogicRules::useConsequence(TruthUse use) const {
  switch (use) {
    case TruthUse::Condition:
      return ErrorTextTruthy::soAlways();
    case TruthUse::Not:
      return ErrorTextTruthy::soNot();
    case TruthUse::Or:
      return ErrorTextTruthy::soOr();
    case TruthUse::And:
      return ErrorTextTruthy::soAnd();
    case TruthUse::OrAssign:
      return ErrorTextTruthy::soOrSet();
    case TruthUse::AndAssign:
      return ErrorTextTruthy::soAndSet();
  }

  return "";
}

std::string LogicRules::objectAdvice(const ObjectTruth &truth, const Type *base, TruthUse use,
                                     llvm::StringRef valueText, llvm::StringRef otherText) const {
  bool isNegated = use == TruthUse::Not;
  auto length = ErrorTextTruthy::lengthCmp(valueText, isNegated);
  bool isNamedObject = truth.narrowed != nullptr || truth.isObject;
  bool isKnownHolder = truth.narrowed != nullptr || truth.isClassName;
  if (use == TruthUse::AndAssign) {
    return ErrorTextTruthy::useAssign();
  }
  if (use == TruthUse::Or) {
    return ErrorTextTruthy::dropOr(otherText);
  }
  if (use == TruthUse::OrAssign) {
    return truth.isArray ? ErrorTextTruthy::arrayLen(valueText, otherText)
                         : ErrorTextTruthy::dropAssign();
  }
  if (isNamedObject && use == TruthUse::And) {
    return ErrorTextTruthy::dropAnd(valueText);
  }
  if (isKnownHolder) {
    return truth.isArray ? ErrorTextTruthy::dropOrLen(length) : ErrorTextTruthy::dropCheck();
  }
  if (truth.isArray) {
    return ErrorTextTruthy::checkLen(length);
  }
  if (truth.isObject) {
    return ErrorTextTruthy::allowUndef(base->name());
  }
  bool isWithoutArgs = base->minArgs() == 0;
  auto call = ErrorTextTruthy::callOf(valueText, isNegated, isWithoutArgs);

  return ErrorTextTruthy::callIt(call);
}

LogicRules::ResultTruthIsTested LogicRules::checkTruthy(const Expr *expr, TruthUse use,
                                                        const Expr *other) const {
  const Type *type = expr->type;
  const auto *declared = type->nonOptional();
  const auto *base = this->state.generics.apparent.apparentType(declared);
  auto problem = this->truthProblem(type, base);
  if (!problem.has_value()) {
    return ok(expr);
  }
  auto message = this->truthMessage(*problem, expr, use, other, base);

  return err(CodeLogicRegistryCheckTruthyError{
      .code = *problem,
      .message = message,
  });
}

std::optional<CodeLogicRegistryCheckTruthyErrorCode>
LogicRules::truthProblem(const Type *type, const Type *base) const {
  if (this->isTestable(type, base)) {
    return std::nullopt;
  }
  if (base->isUnion() || base->isLiteral()) {
    return this->unionProblem(type, base);
  }
  if (type->isVoid()) {
    return CodeLogicRegistryCheckTruthyErrorCode::VOID_VALUE;
  }
  if (type->isUndefined()) {
    return CodeLogicRegistryCheckTruthyErrorCode::ALWAYS_UNDEFINED;
  }
  if (base->isParam()) {
    return CodeLogicRegistryCheckTruthyErrorCode::PARAM_MAY_BE_NUMBER;
  }
  if (base->isJson()) {
    return CodeLogicRegistryCheckTruthyErrorCode::JSON_VALUE;
  }
  if (base->isUnknown()) {
    return CodeLogicRegistryCheckTruthyErrorCode::UNKNOWN_VALUE;
  }
  if (base->isResult()) {
    return CodeLogicRegistryCheckTruthyErrorCode::RESULT_VALUE;
  }
  if (base->isNumeric() || base->isDecimal()) {
    return CodeLogicRegistryCheckTruthyErrorCode::NUMBER_VALUE;
  }

  return CodeLogicRegistryCheckTruthyErrorCode::ALWAYS_TRUTHY;
}

bool LogicRules::isTestable(const Type *type, const Type *base) const {
  if (type->isUndefined() || base->isNever()) {
    return true;
  }
  bool isHolder = base->isArray() || base->isFunction() || base->isObject() || base->isResult() ||
                  base->isIterator() || base->isDict() || base->isPromise() || base->isClassRef();
  bool isOptionalObject = type->isOptional() && isHolder;
  if (type->isError() || base->isBool() || base->isString() || isOptionalObject) {
    return true;
  }

  return base->isParam() && !base->alternatives().empty() &&
         this->areAllBoolOrString(base->alternatives());
}

bool LogicRules::areAllBoolOrString(llvm::ArrayRef<const Type *> types) const {
  for (const Type *type : types) {
    if (!type->isBool() && !type->isString()) {
      return false;
    }
  }

  return true;
}

std::optional<CodeLogicRegistryCheckTruthyErrorCode>
LogicRules::unionProblem(const Type *type, const Type *base) const {
  const auto *number = this->unionNumber(base);
  bool isFalsy = type->isOptional() || this->hasFalsyMember(base);
  if (number == nullptr && isFalsy) {
    return std::nullopt;
  }
  if (number == nullptr) {
    return CodeLogicRegistryCheckTruthyErrorCode::UNION_ALWAYS_TRUTHY;
  }

  return CodeLogicRegistryCheckTruthyErrorCode::UNION_WITH_NUMBER;
}

std::vector<const Type *> LogicRules::unionMembers(const Type *base) const {
  if (!base->isUnion()) {
    return {base};
  }
  auto members = base->members();

  return std::vector<const Type *>(members.begin(), members.end());
}

const Type *LogicRules::unionNumber(const Type *base) const {
  const Type *number = nullptr;
  for (const Type *member : this->unionMembers(base)) {
    if (this->isNumberMember(member)) {
      number = member;
    }
  }

  return number;
}

bool LogicRules::isNumberMember(const Type *member) const {
  const Type *plain = member->isLiteral() ? member->literalBase() : member;

  return plain->isNumeric() || plain->isDecimal() || plain->isParam() || plain->isJson();
}

bool LogicRules::hasFalsyMember(const Type *base) const {
  for (const Type *member : this->unionMembers(base)) {
    if (this->isFalsyMember(member)) {
      return true;
    }
  }

  return false;
}

bool LogicRules::isFalsyMember(const Type *member) const {
  if (!member->isLiteral()) {
    return member->isBool() || member->isString();
  }
  if (member->isStringLiteral()) {
    return member->literalText().empty();
  }

  return member->literalNumber() == 0;
}

std::string LogicRules::truthMessage(CodeLogicRegistryCheckTruthyErrorCode code, const Expr *expr,
                                     TruthUse use, const Expr *other, const Type *base) const {
  const Type *type = expr->type;
  bool isNegated = use == TruthUse::Not;
  auto valueText = this->hintText(expr);
  auto otherText = this->hintText(other, kEllipsis);
  switch (code) {
    case CodeLogicRegistryCheckTruthyErrorCode::UNION_ALWAYS_TRUTHY:
      return ErrorTextTruthy::unionTrue(type->name());
    case CodeLogicRegistryCheckTruthyErrorCode::UNION_WITH_NUMBER:
      return this->unionNumberMessage(type, base, valueText);
    case CodeLogicRegistryCheckTruthyErrorCode::VOID_VALUE:
      return ErrorTextTruthy::voidValue();
    case CodeLogicRegistryCheckTruthyErrorCode::ALWAYS_UNDEFINED:
      return ErrorTextTruthy::undefined();
    case CodeLogicRegistryCheckTruthyErrorCode::PARAM_MAY_BE_NUMBER:
      return this->paramMessage(type, base, use, valueText, otherText);
    case CodeLogicRegistryCheckTruthyErrorCode::JSON_VALUE:
      return this->jsonMessage(use, valueText, otherText);
    case CodeLogicRegistryCheckTruthyErrorCode::UNKNOWN_VALUE:
      return this->unknownMessage(use, valueText, otherText);
    case CodeLogicRegistryCheckTruthyErrorCode::RESULT_VALUE:
      return ErrorTextTruthy::result(valueText, isNegated);
    case CodeLogicRegistryCheckTruthyErrorCode::NUMBER_VALUE:
      return this->numberMessage(expr, type, base, use, valueText, otherText);
    case CodeLogicRegistryCheckTruthyErrorCode::ALWAYS_TRUTHY:
      return this->objectMessage(expr, base, use, valueText, otherText);
  }

  return this->objectMessage(expr, base, use, valueText, otherText);
}

std::string LogicRules::unionNumberMessage(const Type *type, const Type *base,
                                           llvm::StringRef value) const {
  const auto *number = this->unionNumber(base);

  return ErrorTextTruthy::unionNum(type->name(), number->name(), value);
}

}
