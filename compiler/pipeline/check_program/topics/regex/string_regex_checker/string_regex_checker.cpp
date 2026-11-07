#include "compiler/pipeline/check_program/topics/regex/string_regex_checker/string_regex_checker.h"
#include "compiler/load/to_utf8/to_utf8.h"
#include "compiler/pipeline/check_program/flow/flow_checker/flow_checker.h"
#include "compiler/pipeline/check_program/scopes/scopes.h"
#include "compiler/pipeline/check_program/topics/expr/argument_checker/argument_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "compiler/pipeline/check_program/topics/expr/member_checker/member_checker.h"
#include "compiler/pipeline/check_program/topics/functions/callback_checker/callback_checker.h"
#include "compiler/pipeline/check_program/topics/regex/regex_checker/regex_checker.h"
#include "runtime/library/regex/regex/regex.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <cstring>

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

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

StringRegexChecker::StringRegexChecker(CheckState &owner)
    : state(owner) {}

Expr *StringRegexChecker::checkStringRegexMethod(CallExpr *call, MemberExpr *member) {
  llvm::StringRef name = member->name;
  static const llvm::StringLiteral methods[] = {"match",   "matchAll",   "search",
                                                "replace", "replaceAll", "split"};
  if (!llvm::is_contained(methods, name) || (name == "split" && call->args.empty())) {
    return nullptr;
  }
  call->type = Type::error();
  bool replace = name == "replace" || name == "replaceAll";
  unsigned min = replace ? 2 : name == "split" ? 0 : 1;
  unsigned max = name == "split" ? 2 : replace ? 2 : 1;
  ArgumentChecker::ResultArityMatches arity =
      ArgumentChecker(this->state).checkArity(call, min, max);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  auto restLoosely = [&] {
    for (size_t i = 1; i < call->args.size(); ++i) {
      call->args[i] = ExprChecker(this->state).checkExpr(call->args[i]);
    }

    return call;
  };
  Expr *&pattern = call->args[0];
  pattern = ExprChecker(this->state).checkExpr(pattern);
  const Type *type = pattern->type;
  if (type->isError()) {
    return restLoosely();
  }

  if (type->isRegExp()) {
    const RegexLitExpr *regex = RegexChecker::knownRegex(pattern);
    call->regex = regex;
    std::optional<uint32_t> flags;
    if (regex) {
      flags = tz_re_flags(regex->program.data());
    }
    bool global = flags && (*flags & TZ_RE_GLOBAL);
    if (name == "match") {
      if (!flags) {
        std::string message = ErrorTextStringRegex::matchFlag();
        this->state.getDiagnostics().error(member->loc(), message);
        return call;
      }
      call->target = BuiltinCall{.builtin = global ? Builtin::StrMatchGlobal : Builtin::StrMatch};
      call->type = Type::optional(global ? Type::array(Type::string())
                                         : RegexChecker(this->state).matchType(regex));
      return call;
    }
    if (name == "matchAll" || name == "replaceAll") {
      if (flags && !global) {
        std::string source = ToUtf8::toUtf8(regex->pattern);
        std::string flagsText = ToUtf8::toUtf8(regex->flags);
        std::string message = ErrorTextStringRegex::needsG(name, source, flagsText);
        this->state.getDiagnostics().error(pattern->loc(), message);
        return restLoosely();
      }
      if (name == "matchAll") {
        call->target = BuiltinCall{.builtin = Builtin::StrMatchAll};
        call->type = Type::iterator(RegexChecker(this->state).matchType(regex));
        return call;
      }
    }
    if (name == "search") {
      call->target = BuiltinCall{.builtin = Builtin::StrSearch};
      call->type = Type::intTy();
      return call;
    }
    if (name == "split") {
      if (call->args.size() == 2) {
        call->args[1] = MemberChecker(this->state).checkPosition(call->args[1]);
      }
      bool undefinedGroups = !regex;
      for (uint32_t k = 1; regex && k < tz_re_captures(regex->program.data()); ++k) {
        undefinedGroups |= !tz_re_mandatory(regex->program.data(), k);
      }
      call->target = BuiltinCall{.builtin = Builtin::StrSplitRegex};
      call->type = Type::array(undefinedGroups ? Type::optional(Type::string()) : Type::string());
      return call;
    }
    return this->checkReplacement(call, member, regex);
  }

  if (type->isString()) {
    std::string text = StringRegexChecker::argumentText(pattern);
    auto *lit = dyn_cast<StringLitExpr>(pattern);
    std::string escaped = lit ? StringRegexChecker::escapedLiteral(lit->value) : std::string();
    std::string regexText = lit ? escaped : ErrorTextStringRegex::escapedRe(text);
    if (name == "split") {
      if (call->args.size() == 2) {
        call->args[1] = MemberChecker(this->state).checkPosition(call->args[1]);
      }
      call->target = BuiltinCall{.builtin = Builtin::StrSplit};
      call->type = Type::array(Type::string());
      return call;
    }
    if (name == "replaceAll") {
      Expr *&replacement = call->args[1];
      if (isa<FunctionExpr>(replacement)) {
        std::string message = ErrorTextStringRegex::functionRe(escaped, text);
        this->state.getDiagnostics().error(replacement->loc(), message);
        return restLoosely();
      }
      replacement = ArgumentChecker(this->state).checkArg(replacement, Type::string());
      call->target = BuiltinCall{.builtin = Builtin::StrReplaceAll};
      call->type = Type::string();
      return call;
    }
    std::string message =
        name == "replace" ? ErrorTextStringRegex::replaceStr(text, regexText)
        : name == "search"
            ? ErrorTextStringRegex::searchStr(text, regexText, lit != nullptr)
            : ErrorTextStringRegex::includeStr(name, text, regexText, lit != nullptr);
    this->state.getDiagnostics().error(member->loc(), message);
    return restLoosely();
  }
  std::string message =
      ErrorTextStringRegex::notRegex(name, name == "split" || name == "replaceAll", type->name());
  this->state.getDiagnostics().error(pattern->loc(), message);

  return restLoosely();
}

Expr *StringRegexChecker::checkReplacement(CallExpr *call, MemberExpr *member,
                                           const RegexLitExpr *regex) {
  Expr *&replacement = call->args[1];
  bool function = isa<FunctionExpr>(replacement);
  if (auto *name = dyn_cast<NameExpr>(replacement)) {
    ScopeEntry entry = Scopes(this->state).lookup(name->name);
    function = entry.func() ||
               (entry.var() && entry.var()->type && entry.var()->type->nonOptional()->isFunction());
  }
  if (!function) {
    replacement = ArgumentChecker(this->state).checkArg(replacement, Type::string());
    call->target = BuiltinCall{.builtin = Builtin::StrReplaceRegex};
    call->type = Type::string();
    auto *text = dyn_cast<StringLitExpr>(replacement);
    if (!regex || !text) {
      return call;
    }
    const uint32_t *program = regex->program.data();
    uint32_t groups = tz_re_captures(program) - 1;
    bool named = false;
    for (uint32_t k = 1; k <= groups; ++k) {
      uint32_t start;
      named |= tz_re_name(program, k, &start) != 0;
    }
    const std::u16string &value = text->value;
    for (size_t i = 0; i + 1 < value.size(); ++i) {
      if (value[i] != u'$') {
        continue;
      }
      char16_t c = value[i + 1];
      if (c == u'$') {
        ++i;
      } else if (c >= u'1' && c <= u'9') {
        uint32_t one = c - u'0';
        char16_t next = i + 2 < value.size() ? value[i + 2] : 0;
        uint32_t two = next >= u'0' && next <= u'9' ? one * 10 + (next - u'0') : 0;
        if ((two && two <= groups) || one <= groups) {
          continue;
        }
        std::string message = ErrorTextStringRegex::noGroup(groups, one);
        this->state.getDiagnostics().error(text->loc(), message);
        return call;
      } else if (c == u'<') {
        size_t close = value.find(u'>', i + 2);
        std::string groupName =
            close == std::u16string::npos ? "" : ToUtf8::toUtf8(value.substr(i + 2, close - i - 2));
        bool found = false;
        for (uint32_t k = 1; k <= groups && !found; ++k) {
          uint32_t start, length = tz_re_name(program, k, &start);
          found = length &&
                  ToUtf8::toUtf8(StringRegexChecker::units(program, start, length)) == groupName;
        }
        if (found) {
          continue;
        }
        std::string message = ErrorTextStringRegex::noNamed(named, groupName);
        this->state.getDiagnostics().error(text->loc(), message);
        return call;
      }
    }
    return call;
  }

  Signature sig;
  sig.pad = false;
  sig.ret = Type::string();
  sig.params.push_back(Type::string());
  const Type *match = RegexChecker(this->state).matchType(regex);
  if (regex) {
    for (int k = 1; k <= match->matchGroups(); ++k) {
      sig.params.push_back(match->captureMandatory(unsigned(k)) ? Type::string()
                                                                : Type::optional(Type::string()));
    }
    sig.params.push_back(Type::intTy());
    sig.params.push_back(Type::string());
    if (const Type *named = match->field("groups")->type; !named->isOptional()) {
      sig.params.push_back(named);
    }
  }
  if (auto *fe = dyn_cast<FunctionExpr>(replacement);
      fe && fe->fn->params.size() > sig.params.size()) {
    if (!regex) {
      std::string message = ErrorTextStringRegex::onlyMatch();
      this->state.getDiagnostics().error(fe->fn->params[1].loc, message);
    } else {
      std::string message = ErrorTextStringRegex::passes(
          member->name, sig.params.size(), match->matchGroups(), sig.params.back()->isObject());
      this->state.getDiagnostics().error(fe->fn->params[sig.params.size()].loc, message);
    }
    while (sig.params.size() < fe->fn->params.size()) {
      sig.params.push_back(Type::string());
    }
    replacement =
        CallbackChecker(this->state)
            .checkFunctionArg(replacement, sig, unsigned(sig.params.size()), member->name);
    return call;
  }
  replacement = CallbackChecker(this->state)
                    .checkFunctionArg(replacement, sig, unsigned(sig.params.size()), member->name);
  if (!replacement->type->isFunction()) {
    return call;
  }
  call->target = BuiltinCall{.builtin = Builtin::StrReplaceRegexFn};
  call->type = Type::string();
  FlowChecker(this->state).calledFunction();

  return call;
}

}
