#include "compiler/semantic/library/regex/string_regex_checker/string_regex_checker.h"
#include "compiler/program/library/regex/match_member_table/match_member_table.h"
#include "compiler/program/library/string/string_method_table/string_method_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/expr/member_checker/member_checker.h"
#include "compiler/semantic/bodies/functions/callback_checker/callback_checker.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/library/regex/regex_checker/regex_checker.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "runtime/library/regex/regex/regex.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <cstring>

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr const char *kRegexSpecials = "^$\\.*+?()[]{}|/";
constexpr llvm::StringLiteral kStringPlaceholder = "s";

}

StringRegexChecker::StringRegexChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.scopes;
  this->exprChecker = &all.exprChecker;
  this->flowChecker = &all.flowChecker;
  this->regexChecker = &all.regexChecker;
  this->memberChecker = &all.memberChecker;
  this->argumentChecker = &all.argumentChecker;
  this->callbackChecker = &all.callbackChecker;
}

std::string StringRegexChecker::escapedLiteral(const std::u16string &text) const {
  auto escaped = std::u16string();
  for (char16_t unit : text) {
    if (unit < 128 && std::strchr(kRegexSpecials, static_cast<int>(unit)) != nullptr) {
      escaped += u'\\';
    }
    escaped += unit;
  }

  return "/" + ToUtf8::toUtf8(escaped) + "/";
}

std::string StringRegexChecker::argumentText(const Expr *arg) const {
  const auto *literal = dyn_cast<StringLitExpr>(arg);
  if (literal != nullptr) {
    auto text = ToUtf8::toUtf8(literal->value);
    auto isPlain = std::bind_front(&StringRegexChecker::isPlainQuotedChar, this);
    if (llvm::all_of(text, isPlain)) {
      return "\"" + text + "\"";
    }
  }
  const auto *name = dyn_cast<NameExpr>(arg);
  if (name != nullptr) {
    return name->name.str();
  }

  return kStringPlaceholder.str();
}

bool StringRegexChecker::isPlainQuotedChar(char character) const {
  return character >= 0x20 && character != '"' && character != '\\';
}

unsigned StringRegexChecker::patternMinArgs(StringMethod method) const {
  if (method == StringMethod::REPLACE || method == StringMethod::REPLACE_ALL) {
    return 2;
  }
  if (method == StringMethod::SPLIT) {
    return 0;
  }

  return 1;
}

Expr *StringRegexChecker::checkStringRegexMethod(CallExpr *call, MemberExpr *member) {
  const auto *found = StringMethodTable::find(member->name);
  bool isTakingPattern = found != nullptr && (found->form.is<TextOrPatternStringMethod>() ||
                                              found->form.is<PatternStringMethod>());
  if (!isTakingPattern) {
    return nullptr;
  }
  StringMethod method = found->method;
  bool isSplit = method == StringMethod::SPLIT;
  if (isSplit && call->args.empty()) {
    return nullptr;
  }
  call->type = Type::error();
  bool isReplace = method == StringMethod::REPLACE || method == StringMethod::REPLACE_ALL;
  unsigned min = this->patternMinArgs(method);
  unsigned max = isSplit || isReplace ? 2 : 1;
  auto arity = this->argumentChecker->checkArity(call, min, max);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  auto *pattern = this->exprChecker->checkExpr(call->args[0]);
  call->args[0] = pattern;
  const Type *type = pattern->type;
  if (type->isError()) {
    return this->checkRestLoosely(call);
  }

  PatternCall patternCall{
      .call = call,
      .member = member,
      .method = method,
  };
  if (type->isRegExp()) {
    return this->checkRegexPattern(patternCall);
  }
  if (type->isString()) {
    return this->checkTextPattern(patternCall);
  }
  bool isTextAllowed = isSplit || method == StringMethod::REPLACE_ALL;
  auto message = ErrorTextStringRegex::notRegex(member->name, isTextAllowed, type->name());
  this->state.getDiagnostics().error(pattern->loc(), message);

  return this->checkRestLoosely(call);
}

Expr *StringRegexChecker::checkRestLoosely(CallExpr *call) {
  for (size_t index = 1; index < call->args.size(); ++index) {
    call->args[index] = this->exprChecker->checkExpr(call->args[index]);
  }

  return call;
}

Expr *StringRegexChecker::toBuiltinCall(CallExpr *call, Builtin builtin, const Type *result) const {
  call->target = BuiltinCall{.builtin = builtin};
  call->type = result;

  return call;
}

Expr *StringRegexChecker::checkRegexPattern(const PatternCall &pattern) {
  CallExpr *call = pattern.call;
  const auto *regex = this->regexChecker->knownRegex(call->args[0]);
  call->regex = regex;
  StringMethod method = pattern.method;
  if (method == StringMethod::MATCH) {
    return this->checkRegexMatch(pattern, regex);
  }
  bool isNeedingGlobal = method == StringMethod::MATCH_ALL || method == StringMethod::REPLACE_ALL;
  if (isNeedingGlobal && this->isMissingGlobalFlag(regex)) {
    return this->reportNeedsGlobal(pattern, regex);
  }
  if (method == StringMethod::MATCH_ALL) {
    const auto *matches = Type::iterator(this->regexChecker->matchType(regex));
    return this->toBuiltinCall(call, Builtin::StrMatchAll, matches);
  }
  if (method == StringMethod::SEARCH) {
    return this->toBuiltinCall(call, Builtin::StrSearch, Type::intTy());
  }
  if (method == StringMethod::SPLIT) {
    return this->checkRegexSplit(call, regex);
  }

  return this->checkReplacement(call, pattern.member, regex);
}

bool StringRegexChecker::isMissingGlobalFlag(const RegexLitExpr *regex) const {
  if (regex == nullptr) {
    return false;
  }
  uint32_t flags = tz_re_flags(regex->program.data());

  return (flags & TZ_RE_GLOBAL) == 0;
}

Expr *StringRegexChecker::checkRegexMatch(const PatternCall &pattern, const RegexLitExpr *regex) {
  CallExpr *call = pattern.call;
  if (regex == nullptr) {
    auto message = ErrorTextStringRegex::matchFlag();
    this->state.getDiagnostics().error(pattern.member->loc(), message);
    return call;
  }
  uint32_t flags = tz_re_flags(regex->program.data());
  bool isGlobal = (flags & TZ_RE_GLOBAL) != 0;
  if (isGlobal) {
    const auto *matches = Type::optional(Type::array(Type::string()));
    return this->toBuiltinCall(call, Builtin::StrMatchGlobal, matches);
  }
  const auto *match = Type::optional(this->regexChecker->matchType(regex));

  return this->toBuiltinCall(call, Builtin::StrMatch, match);
}

Expr *StringRegexChecker::reportNeedsGlobal(const PatternCall &pattern, const RegexLitExpr *regex) {
  auto source = ToUtf8::toUtf8(regex->pattern);
  auto flagsText = ToUtf8::toUtf8(regex->flags);
  auto message = ErrorTextStringRegex::needsG(pattern.member->name, source, flagsText);
  this->state.getDiagnostics().error(pattern.call->args[0]->loc(), message);

  return this->checkRestLoosely(pattern.call);
}

Expr *StringRegexChecker::checkRegexSplit(CallExpr *call, const RegexLitExpr *regex) {
  if (call->args.size() == 2) {
    call->args[1] = this->memberChecker->checkPosition(call->args[1]);
  }
  bool isUndefinedGroups = this->hasOptionalGroups(regex);
  const Type *part = isUndefinedGroups ? Type::optional(Type::string()) : Type::string();

  return this->toBuiltinCall(call, Builtin::StrSplitRegex, Type::array(part));
}

bool StringRegexChecker::hasOptionalGroups(const RegexLitExpr *regex) const {
  if (regex == nullptr) {
    return true;
  }
  const uint32_t *program = regex->program.data();
  uint32_t captures = tz_re_captures(program);
  for (uint32_t group = 1; group < captures; ++group) {
    if (!tz_re_mandatory(program, group)) {
      return true;
    }
  }

  return false;
}

Expr *StringRegexChecker::checkTextPattern(const PatternCall &pattern) {
  CallExpr *call = pattern.call;
  if (pattern.method == StringMethod::SPLIT) {
    if (call->args.size() == 2) {
      call->args[1] = this->memberChecker->checkPosition(call->args[1]);
    }
    return this->toBuiltinCall(call, Builtin::StrSplit, Type::array(Type::string()));
  }
  const Expr *patternExpr = call->args[0];
  auto text = this->argumentText(patternExpr);
  auto *literal = dyn_cast<StringLitExpr>(patternExpr);
  std::string escaped = literal != nullptr ? this->escapedLiteral(literal->value) : std::string();
  if (pattern.method == StringMethod::REPLACE_ALL) {
    return this->checkTextReplaceAll(call, text, escaped);
  }
  std::string regexText = literal != nullptr ? escaped : ErrorTextStringRegex::escapedRe(text);
  bool isWritten = literal != nullptr;
  auto message = std::string();
  if (pattern.method == StringMethod::REPLACE) {
    message = ErrorTextStringRegex::replaceStr(text, regexText);
  } else if (pattern.method == StringMethod::SEARCH) {
    message = ErrorTextStringRegex::searchStr(text, regexText, isWritten);
  } else {
    message = ErrorTextStringRegex::includeStr(pattern.member->name, text, regexText, isWritten);
  }
  this->state.getDiagnostics().error(pattern.member->loc(), message);

  return this->checkRestLoosely(call);
}

Expr *StringRegexChecker::checkTextReplaceAll(CallExpr *call, const std::string &text,
                                              const std::string &escaped) {
  Expr *&replacement = call->args[1];
  if (isa<FunctionExpr>(replacement)) {
    auto message = ErrorTextStringRegex::functionRe(escaped, text);
    this->state.getDiagnostics().error(replacement->loc(), message);
    return this->checkRestLoosely(call);
  }
  replacement = this->argumentChecker->checkArg(replacement, Type::string());

  return this->toBuiltinCall(call, Builtin::StrReplaceAll, Type::string());
}

Expr *StringRegexChecker::checkReplacement(CallExpr *call, MemberExpr *member,
                                           const RegexLitExpr *regex) {
  if (this->isFunctionReplacement(call->args[1])) {
    return this->checkFunctionReplacement(call, member, regex);
  }

  return this->checkTextReplacement(call, regex);
}

bool StringRegexChecker::isFunctionReplacement(const Expr *replacement) const {
  auto *name = dyn_cast<NameExpr>(replacement);
  if (name == nullptr) {
    return isa<FunctionExpr>(replacement);
  }
  auto entry = this->scopes->lookup(name->name);
  const VarSymbol *variable = entry.var();
  bool isFunctionVar = variable != nullptr && variable->type != nullptr &&
                       variable->type->nonOptional()->isFunction();

  return entry.func() != nullptr || isFunctionVar;
}

Expr *StringRegexChecker::checkTextReplacement(CallExpr *call, const RegexLitExpr *regex) {
  auto *replacement = this->argumentChecker->checkArg(call->args[1], Type::string());
  call->args[1] = replacement;
  call->target = BuiltinCall{.builtin = Builtin::StrReplaceRegex};
  call->type = Type::string();
  auto *text = dyn_cast<StringLitExpr>(replacement);
  if (regex == nullptr || text == nullptr) {
    return call;
  }
  auto groups = this->checkReplacementGroups(regex, text->value);
  if (groups.isErr()) {
    this->state.getDiagnostics().error(text->loc(), groups.error().message);
  }

  return call;
}

StringRegexChecker::ResultGroupsReferenced
StringRegexChecker::checkReplacementGroups(const RegexLitExpr *regex,
                                           const std::u16string &value) const {
  const uint32_t *program = regex->program.data();
  uint32_t groups = tz_re_captures(program) - 1;
  for (size_t at : this->groupReferences(value)) {
    auto reference = this->checkGroupReference(program, groups, value, at);
    if (reference.isErr()) {
      return err(reference.error());
    }
  }

  return ok(&value);
}

std::vector<size_t> StringRegexChecker::groupReferences(const std::u16string &value) const {
  auto positions = std::vector<size_t>();
  size_t index = 0;
  while (index + 1 < value.size()) {
    bool isDollar = value[index] == u'$';
    bool isEscaped = isDollar && value[index + 1] == u'$';
    if (isDollar && !isEscaped) {
      positions.push_back(index);
    }
    index += isEscaped ? 2 : 1;
  }

  return positions;
}

StringRegexChecker::ResultGroupsReferenced
StringRegexChecker::checkGroupReference(const uint32_t *program, uint32_t groups,
                                        const std::u16string &value, size_t at) const {
  char16_t next = value[at + 1];
  bool isNumbered = next >= u'1' && next <= u'9';
  if (isNumbered && !this->isNumberedGroupKnown(value, at, groups)) {
    uint32_t digit = next - u'0';
    auto message = ErrorTextStringRegex::noGroup(groups, digit);

    return err(StringRegexCheckerReplacementGroupsError{
        .code = StringRegexCheckerReplacementGroupsErrorCode::NO_SUCH_GROUP,
        .message = message,
    });
  }
  if (next != u'<') {
    return ok(&value);
  }
  auto groupName = this->groupNameAt(value, at);
  if (!this->hasNamedGroup(program, groups, groupName)) {
    bool isNamed = this->hasNamedGroups(program, groups);
    auto message = ErrorTextStringRegex::noNamed(isNamed, groupName);

    return err(StringRegexCheckerReplacementGroupsError{
        .code = StringRegexCheckerReplacementGroupsErrorCode::NO_SUCH_NAMED_GROUP,
        .message = message,
    });
  }

  return ok(&value);
}

bool StringRegexChecker::isNumberedGroupKnown(const std::u16string &value, size_t at,
                                              uint32_t groups) const {
  uint32_t one = value[at + 1] - u'0';
  char16_t next = at + 2 < value.size() ? value[at + 2] : 0;
  uint32_t two = next >= u'0' && next <= u'9' ? one * 10 + (next - u'0') : 0;

  return (two != 0 && two <= groups) || one <= groups;
}

std::string StringRegexChecker::groupNameAt(const std::u16string &value, size_t at) const {
  size_t close = value.find(u'>', at + 2);
  if (close == std::u16string::npos) {
    return "";
  }

  return ToUtf8::toUtf8(value.substr(at + 2, close - at - 2));
}

bool StringRegexChecker::hasNamedGroup(const uint32_t *program, uint32_t groups,
                                       const std::string &groupName) const {
  for (uint32_t group = 1; group <= groups; ++group) {
    uint32_t start = 0;
    uint32_t length = tz_re_name(program, group, &start);
    if (length != 0 && this->groupName(program, start, length) == groupName) {
      return true;
    }
  }

  return false;
}

std::string StringRegexChecker::groupName(const uint32_t *program, uint32_t start,
                                          uint32_t length) const {
  auto units = this->regexChecker->units(program, start, length);

  return ToUtf8::toUtf8(units);
}

bool StringRegexChecker::hasNamedGroups(const uint32_t *program, uint32_t groups) const {
  for (uint32_t group = 1; group <= groups; ++group) {
    uint32_t start = 0;
    if (tz_re_name(program, group, &start) != 0) {
      return true;
    }
  }

  return false;
}

Expr *StringRegexChecker::checkFunctionReplacement(CallExpr *call, MemberExpr *member,
                                                   const RegexLitExpr *regex) {
  const auto *match = this->regexChecker->matchType(regex);
  auto signature = this->replacerSignature(regex, match);
  auto *closure = dyn_cast<FunctionExpr>(call->args[1]);
  bool isExtraParams = closure != nullptr && closure->fn->params.size() > signature.params.size();
  if (isExtraParams) {
    return this->checkExtraReplacerParams(call, member, regex, match, signature);
  }
  unsigned paramCount = unsigned(signature.params.size());
  call->args[1] =
      this->callbackChecker->checkFunctionArg(call->args[1], signature, paramCount, member->name);
  if (!call->args[1]->type->isFunction()) {
    return call;
  }
  call->target = BuiltinCall{.builtin = Builtin::StrReplaceRegexFn};
  call->type = Type::string();
  this->flowChecker->calledFunction();

  return call;
}

Signature StringRegexChecker::replacerSignature(const RegexLitExpr *regex,
                                                const Type *match) const {
  Signature signature;
  signature.pad = false;
  signature.ret = Type::string();
  signature.params.push_back(Type::string());
  if (regex == nullptr) {
    return signature;
  }
  for (int group = 1; group <= match->matchGroups(); ++group) {
    bool isMandatory = match->captureMandatory(unsigned(group));
    signature.params.push_back(isMandatory ? Type::string() : Type::optional(Type::string()));
  }
  signature.params.push_back(Type::intTy());
  signature.params.push_back(Type::string());
  auto groupsName = MatchMemberTable::getName(MatchMember::GROUPS);
  const Type *named = match->field(groupsName)->type;
  if (!named->isOptional()) {
    signature.params.push_back(named);
  }

  return signature;
}

Expr *StringRegexChecker::checkExtraReplacerParams(CallExpr *call, MemberExpr *member,
                                                   const RegexLitExpr *regex, const Type *match,
                                                   Signature signature) {
  auto *closure = llvm::cast<FunctionExpr>(call->args[1]);
  if (regex == nullptr) {
    auto message = ErrorTextStringRegex::onlyMatch();
    this->state.getDiagnostics().error(closure->fn->params[1].loc, message);
  } else {
    bool hasGroupsObject = signature.params.back()->isObject();
    auto message = ErrorTextStringRegex::passes(member->name, signature.params.size(),
                                                match->matchGroups(), hasGroupsObject);
    this->state.getDiagnostics().error(closure->fn->params[signature.params.size()].loc, message);
  }
  while (signature.params.size() < closure->fn->params.size()) {
    signature.params.push_back(Type::string());
  }
  unsigned paramCount = unsigned(signature.params.size());
  call->args[1] =
      this->callbackChecker->checkFunctionArg(call->args[1], signature, paramCount, member->name);

  return call;
}

}
