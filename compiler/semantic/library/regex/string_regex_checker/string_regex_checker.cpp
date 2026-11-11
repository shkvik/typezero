#include "compiler/semantic/library/regex/string_regex_checker/string_regex_checker.h"
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

StringRegexChecker::StringRegexChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.getScopes();
  this->exprChecker = &all.getExprChecker();
  this->flowChecker = &all.getFlowChecker();
  this->regexChecker = &all.getRegexChecker();
  this->memberChecker = &all.getMemberChecker();
  this->argumentChecker = &all.getArgumentChecker();
  this->callbackChecker = &all.getCallbackChecker();
}

std::u16string StringRegexChecker::units(const uint32_t *program, uint32_t start, uint32_t length) {
  std::u16string text;
  for (uint32_t i = 0; i < length; ++i) {
    text += static_cast<char16_t>(tz_re_name_unit(program, start, i));
  }

  return text;
}

std::string StringRegexChecker::escapedLiteral(const std::u16string &text) {
  std::u16string escaped;
  for (char16_t c : text) {
    if (c < 128 && std::strchr("^$\\.*+?()[]{}|/", static_cast<int>(c))) {
      escaped += u'\\';
    }
    escaped += c;
  }

  return "/" + ToUtf8::toUtf8(escaped) + "/";
}

std::string StringRegexChecker::argumentText(const Expr *arg) {
  if (auto *lit = dyn_cast<StringLitExpr>(arg)) {
    std::string text = ToUtf8::toUtf8(lit->value);
    if (llvm::all_of(text, [](char c) { return c >= 0x20 && c != '"' && c != '\\'; })) {
      return "\"" + text + "\"";
    }
  }
  if (auto *name = dyn_cast<NameExpr>(arg)) {
    return name->name.str();
  }

  return "s";
}

Expr *StringRegexChecker::checkStringRegexMethod(CallExpr *call, MemberExpr *member) {
  const StringMethodInfo *found = StringMethodTable::find(member->name);
  bool takesPattern = found != nullptr && (found->form.is<TextOrPatternStringMethod>() ||
                                           found->form.is<PatternStringMethod>());
  if (!takesPattern) {
    return nullptr;
  }
  StringMethod method = found->method;
  bool split = method == StringMethod::SPLIT;
  if (split && call->args.empty()) {
    return nullptr;
  }
  call->type = Type::error();
  bool replace = method == StringMethod::REPLACE || method == StringMethod::REPLACE_ALL;
  unsigned min = replace ? 2 : split ? 0 : 1;
  unsigned max = split || replace ? 2 : 1;
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, min, max);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  Expr *pattern = this->exprChecker->checkExpr(call->args[0]);
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
  bool textAllowed = split || method == StringMethod::REPLACE_ALL;
  std::string message = ErrorTextStringRegex::notRegex(member->name, textAllowed, type->name());
  this->state.getDiagnostics().error(pattern->loc(), message);

  return this->checkRestLoosely(call);
}

Expr *StringRegexChecker::checkRestLoosely(CallExpr *call) {
  for (size_t i = 1; i < call->args.size(); ++i) {
    call->args[i] = this->exprChecker->checkExpr(call->args[i]);
  }

  return call;
}

Expr *StringRegexChecker::toBuiltinCall(CallExpr *call, Builtin builtin, const Type *result) {
  call->target = BuiltinCall{.builtin = builtin};
  call->type = result;

  return call;
}

Expr *StringRegexChecker::checkRegexPattern(const PatternCall &pattern) {
  CallExpr *call = pattern.call;
  const RegexLitExpr *regex = RegexChecker::knownRegex(call->args[0]);
  call->regex = regex;
  StringMethod method = pattern.method;
  if (method == StringMethod::MATCH) {
    return this->checkRegexMatch(pattern, regex);
  }
  bool needsGlobal = method == StringMethod::MATCH_ALL || method == StringMethod::REPLACE_ALL;
  if (needsGlobal && StringRegexChecker::lacksGlobalFlag(regex)) {
    return this->reportNeedsGlobal(pattern, regex);
  }
  if (method == StringMethod::MATCH_ALL) {
    const Type *matches = Type::iterator(this->regexChecker->matchType(regex));
    return StringRegexChecker::toBuiltinCall(call, Builtin::StrMatchAll, matches);
  }
  if (method == StringMethod::SEARCH) {
    return StringRegexChecker::toBuiltinCall(call, Builtin::StrSearch, Type::intTy());
  }
  if (method == StringMethod::SPLIT) {
    return this->checkRegexSplit(call, regex);
  }

  return this->checkReplacement(call, pattern.member, regex);
}

bool StringRegexChecker::lacksGlobalFlag(const RegexLitExpr *regex) {
  if (regex == nullptr) {
    return false;
  }
  uint32_t flags = tz_re_flags(regex->program.data());

  return (flags & TZ_RE_GLOBAL) == 0;
}

Expr *StringRegexChecker::checkRegexMatch(const PatternCall &pattern, const RegexLitExpr *regex) {
  CallExpr *call = pattern.call;
  if (regex == nullptr) {
    std::string message = ErrorTextStringRegex::matchFlag();
    this->state.getDiagnostics().error(pattern.member->loc(), message);
    return call;
  }
  uint32_t flags = tz_re_flags(regex->program.data());
  bool global = (flags & TZ_RE_GLOBAL) != 0;
  if (global) {
    const Type *matches = Type::optional(Type::array(Type::string()));
    return StringRegexChecker::toBuiltinCall(call, Builtin::StrMatchGlobal, matches);
  }
  const Type *match = Type::optional(this->regexChecker->matchType(regex));

  return StringRegexChecker::toBuiltinCall(call, Builtin::StrMatch, match);
}

Expr *StringRegexChecker::reportNeedsGlobal(const PatternCall &pattern, const RegexLitExpr *regex) {
  std::string source = ToUtf8::toUtf8(regex->pattern);
  std::string flagsText = ToUtf8::toUtf8(regex->flags);
  std::string message = ErrorTextStringRegex::needsG(pattern.member->name, source, flagsText);
  this->state.getDiagnostics().error(pattern.call->args[0]->loc(), message);

  return this->checkRestLoosely(pattern.call);
}

Expr *StringRegexChecker::checkRegexSplit(CallExpr *call, const RegexLitExpr *regex) {
  if (call->args.size() == 2) {
    call->args[1] = this->memberChecker->checkPosition(call->args[1]);
  }
  bool undefinedGroups = regex == nullptr;
  for (uint32_t k = 1; regex != nullptr && k < tz_re_captures(regex->program.data()); ++k) {
    undefinedGroups |= !tz_re_mandatory(regex->program.data(), k);
  }
  const Type *part = undefinedGroups ? Type::optional(Type::string()) : Type::string();

  return StringRegexChecker::toBuiltinCall(call, Builtin::StrSplitRegex, Type::array(part));
}

Expr *StringRegexChecker::checkTextPattern(const PatternCall &pattern) {
  CallExpr *call = pattern.call;
  if (pattern.method == StringMethod::SPLIT) {
    if (call->args.size() == 2) {
      call->args[1] = this->memberChecker->checkPosition(call->args[1]);
    }
    return StringRegexChecker::toBuiltinCall(call, Builtin::StrSplit, Type::array(Type::string()));
  }
  const Expr *patternExpr = call->args[0];
  std::string text = StringRegexChecker::argumentText(patternExpr);
  auto *literal = dyn_cast<StringLitExpr>(patternExpr);
  std::string escaped =
      literal != nullptr ? StringRegexChecker::escapedLiteral(literal->value) : std::string();
  if (pattern.method == StringMethod::REPLACE_ALL) {
    return this->checkTextReplaceAll(call, text, escaped);
  }
  std::string regexText = literal != nullptr ? escaped : ErrorTextStringRegex::escapedRe(text);
  bool written = literal != nullptr;
  std::string message;
  if (pattern.method == StringMethod::REPLACE) {
    message = ErrorTextStringRegex::replaceStr(text, regexText);
  } else if (pattern.method == StringMethod::SEARCH) {
    message = ErrorTextStringRegex::searchStr(text, regexText, written);
  } else {
    message = ErrorTextStringRegex::includeStr(pattern.member->name, text, regexText, written);
  }
  this->state.getDiagnostics().error(pattern.member->loc(), message);

  return this->checkRestLoosely(call);
}

Expr *StringRegexChecker::checkTextReplaceAll(CallExpr *call, const std::string &text,
                                              const std::string &escaped) {
  Expr *&replacement = call->args[1];
  if (isa<FunctionExpr>(replacement)) {
    std::string message = ErrorTextStringRegex::functionRe(escaped, text);
    this->state.getDiagnostics().error(replacement->loc(), message);
    return this->checkRestLoosely(call);
  }
  replacement = this->argumentChecker->checkArg(replacement, Type::string());

  return StringRegexChecker::toBuiltinCall(call, Builtin::StrReplaceAll, Type::string());
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
  ScopeEntry entry = this->scopes->lookup(name->name);
  const VarSymbol *var = entry.var();
  bool functionVar =
      var != nullptr && var->type != nullptr && var->type->nonOptional()->isFunction();

  return entry.func() != nullptr || functionVar;
}

Expr *StringRegexChecker::checkTextReplacement(CallExpr *call, const RegexLitExpr *regex) {
  Expr *replacement = this->argumentChecker->checkArg(call->args[1], Type::string());
  call->args[1] = replacement;
  call->target = BuiltinCall{.builtin = Builtin::StrReplaceRegex};
  call->type = Type::string();
  auto *text = dyn_cast<StringLitExpr>(replacement);
  if (regex == nullptr || text == nullptr) {
    return call;
  }
  ResultGroupsReferenced groups = StringRegexChecker::replacementGroups(regex, text->value);
  if (groups.isErr()) {
    this->state.getDiagnostics().error(text->loc(), groups.error().message);
  }

  return call;
}

StringRegexChecker::ResultGroupsReferenced
StringRegexChecker::replacementGroups(const RegexLitExpr *regex, const std::u16string &value) {
  const uint32_t *program = regex->program.data();
  uint32_t groups = tz_re_captures(program) - 1;
  for (size_t i = 0; i + 1 < value.size(); ++i) {
    if (value[i] != u'$') {
      continue;
    }
    char16_t next = value[i + 1];
    if (next == u'$') {
      ++i;
      continue;
    }
    bool numbered = next >= u'1' && next <= u'9';
    if (numbered && !StringRegexChecker::numberedGroupKnown(value, i, groups)) {
      uint32_t one = next - u'0';
      std::string message = ErrorTextStringRegex::noGroup(groups, one);

      return err(StringRegexCheckerReplacementGroupsError{
          .code = StringRegexCheckerReplacementGroupsErrorCode::NO_SUCH_GROUP,
          .message = message,
      });
    }
    if (next != u'<') {
      continue;
    }
    std::string groupName = StringRegexChecker::groupNameAt(value, i);
    if (!StringRegexChecker::hasNamedGroup(program, groups, groupName)) {
      bool named = StringRegexChecker::hasNamedGroups(program, groups);
      std::string message = ErrorTextStringRegex::noNamed(named, groupName);

      return err(StringRegexCheckerReplacementGroupsError{
          .code = StringRegexCheckerReplacementGroupsErrorCode::NO_SUCH_NAMED_GROUP,
          .message = message,
      });
    }
  }

  return ok(&value);
}

bool StringRegexChecker::numberedGroupKnown(const std::u16string &value, size_t at,
                                            uint32_t groups) {
  uint32_t one = value[at + 1] - u'0';
  char16_t next = at + 2 < value.size() ? value[at + 2] : 0;
  uint32_t two = next >= u'0' && next <= u'9' ? one * 10 + (next - u'0') : 0;

  return (two != 0 && two <= groups) || one <= groups;
}

std::string StringRegexChecker::groupNameAt(const std::u16string &value, size_t at) {
  size_t close = value.find(u'>', at + 2);
  if (close == std::u16string::npos) {
    return "";
  }

  return ToUtf8::toUtf8(value.substr(at + 2, close - at - 2));
}

bool StringRegexChecker::hasNamedGroup(const uint32_t *program, uint32_t groups,
                                       const std::string &groupName) {
  for (uint32_t k = 1; k <= groups; ++k) {
    uint32_t start = 0;
    uint32_t length = tz_re_name(program, k, &start);
    bool same = length != 0 &&
                ToUtf8::toUtf8(StringRegexChecker::units(program, start, length)) == groupName;
    if (same) {
      return true;
    }
  }

  return false;
}

bool StringRegexChecker::hasNamedGroups(const uint32_t *program, uint32_t groups) {
  for (uint32_t k = 1; k <= groups; ++k) {
    uint32_t start = 0;
    if (tz_re_name(program, k, &start) != 0) {
      return true;
    }
  }

  return false;
}

Expr *StringRegexChecker::checkFunctionReplacement(CallExpr *call, MemberExpr *member,
                                                   const RegexLitExpr *regex) {
  const Type *match = this->regexChecker->matchType(regex);
  Signature signature = StringRegexChecker::replacerSignature(regex, match);
  auto *closure = dyn_cast<FunctionExpr>(call->args[1]);
  bool extraParams = closure != nullptr && closure->fn->params.size() > signature.params.size();
  if (extraParams) {
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

Signature StringRegexChecker::replacerSignature(const RegexLitExpr *regex, const Type *match) {
  Signature signature;
  signature.pad = false;
  signature.ret = Type::string();
  signature.params.push_back(Type::string());
  if (regex == nullptr) {
    return signature;
  }
  for (int k = 1; k <= match->matchGroups(); ++k) {
    bool mandatory = match->captureMandatory(unsigned(k));
    signature.params.push_back(mandatory ? Type::string() : Type::optional(Type::string()));
  }
  signature.params.push_back(Type::intTy());
  signature.params.push_back(Type::string());
  const Type *named = match->field("groups")->type;
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
    std::string message = ErrorTextStringRegex::onlyMatch();
    this->state.getDiagnostics().error(closure->fn->params[1].loc, message);
  } else {
    bool groupsObject = signature.params.back()->isObject();
    std::string message = ErrorTextStringRegex::passes(member->name, signature.params.size(),
                                                       match->matchGroups(), groupsObject);
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
