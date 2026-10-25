#include "compiler/library/regex/string_regex_checker/string_regex_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/member_checker/member_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/functions/callback_checker/callback_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/library/regex/regex_checker/regex_checker.h"
#include "runtime/library/regex/regex/regex.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <cstring>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

std::string utf8(const std::u16string &text) {
  std::string out;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      out);

  return out;
}

std::u16string units(const uint32_t *program, uint32_t start, uint32_t length) {
  std::u16string text;
  for (uint32_t i = 0; i < length; ++i) {
    text += static_cast<char16_t>(tz_re_name_unit(program, start, i));
  }

  return text;
}

// The text of a literal as a regex that matches it: `/\./` for ".".
std::string escapedLiteral(const std::u16string &text) {
  std::u16string escaped;
  for (char16_t c : text) {
    if (c < 128 && std::strchr("^$\\.*+?()[]{}|/", static_cast<int>(c))) {
      escaped += u'\\';
    }
    escaped += c;
  }

  return "/" + utf8(escaped) + "/";
}

// How a string argument is written, for hints: a literal as itself.
std::string argumentText(const Expr *arg) {
  if (auto *lit = dyn_cast<StringLitExpr>(arg)) {
    std::string text = utf8(lit->value);
    if (llvm::all_of(text, [](char c) { return c >= 0x20 && c != '"' && c != '\\'; })) {
      return "\"" + text + "\"";
    }
  }
  if (auto *name = dyn_cast<NameExpr>(arg)) {
    return name->name.str();
  }

  return "s";
}

const char kUnknownRegex[] =
    "the regex is known only when the program runs (it is not a literal, nor a variable that "
    "holds one all along)";

} // namespace

StringRegexChecker::StringRegexChecker(Checker &checker) : checker_(checker) {}

Expr *StringRegexChecker::checkStringRegexMethod(CallExpr *call, MemberExpr *member) {
  llvm::StringRef name = member->name;
  static const llvm::StringLiteral methods[] = {"match",   "matchAll",   "search",
                                                "replace", "replaceAll", "split"};
  if (!llvm::is_contained(methods, name) || (name == "split" && call->args.empty())) {
    return nullptr;
  }
  call->type = Type::error();
  bool replace = name == "replace" || name == "replaceAll";
  unsigned min = replace ? 2 : name == "split" ? 0 : 1, max = name == "split" ? 2 : replace ? 2 : 1;
  if (!checker_.argumentChecker().checkArity(call, member->loc(), min, max)) {
    return call;
  }
  auto restLoosely = [&] {
    for (size_t i = 1; i < call->args.size(); ++i) {
      call->args[i] = checker_.exprChecker().checkExpr(call->args[i]);
    }

    return call;
  };
  Expr *&pattern = call->args[0];
  pattern = checker_.exprChecker().checkExpr(pattern);
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
        checker_.diags_.error(
            member->loc(),
            std::string("what 'match' gives depends on the g flag (all the texts that "
                        "match, or one match with its groups), and ") +
                kUnknownRegex +
                "; for one match write 're.exec(s)', for all of them "
                "'s.matchAll(re)'");
        return call;
      }
      call->builtin = global ? Builtin::StrMatchGlobal : Builtin::StrMatch;
      call->type = Type::optional(global ? Type::array(Type::string())
                                         : checker_.regexChecker().matchType(regex));
      return call;
    }
    if (name == "matchAll" || name == "replaceAll") {
      if (flags && !global) {
        std::string source = "/" + utf8(regex->pattern) + "/" + utf8(regex->flags) + "g";
        checker_.diags_.error(pattern->loc(),
                              "'" + name +
                                  "' needs a regex with the g flag (JavaScript throws a "
                                  "TypeError without it), e.g. '" +
                                  source + "'");
        return restLoosely();
      }
      if (name == "matchAll") {
        // A lazy iterator, as in JavaScript: each match is found when it is
        // asked for.
        call->builtin = Builtin::StrMatchAll;
        call->type = Type::iterator(checker_.regexChecker().matchType(regex));
        return call;
      }
    }
    if (name == "search") {
      call->builtin = Builtin::StrSearch;
      call->type = Type::intTy();
      return call;
    }
    if (name == "split") {
      if (call->args.size() == 2) {
        call->args[1] = checker_.memberChecker().checkPosition(call->args[1]);
      }
      // A group that may not take part in a match gives undefined.
      bool undefinedGroups = !regex;
      for (uint32_t k = 1; regex && k < tz_re_captures(regex->program.data()); ++k) {
        undefinedGroups |= !tz_re_mandatory(regex->program.data(), k);
      }
      call->builtin = Builtin::StrSplitRegex;
      call->type = Type::array(undefinedGroups ? Type::optional(Type::string()) : Type::string());
      return call;
    }
    return checkReplacement(call, member, regex);
  }

  if (type->isString()) {
    std::string text = argumentText(pattern);
    auto *lit = dyn_cast<StringLitExpr>(pattern);
    std::string regexText =
        lit ? escapedLiteral(lit->value) : "new RegExp(RegExp.escape(" + text + "))";
    if (name == "split") {
      if (call->args.size() == 2) {
        call->args[1] = checker_.memberChecker().checkPosition(call->args[1]);
      }
      call->builtin = Builtin::StrSplit;
      call->type = Type::array(Type::string());
      return call;
    }
    if (name == "replaceAll") {
      Expr *&replacement = call->args[1];
      if (isa<FunctionExpr>(replacement)) {
        checker_.diags_.error(replacement->loc(),
                              "a function as the replacement needs a regex: 'replaceAll(" +
                                  (lit ? escapedLiteral(lit->value) + "g"
                                       : "new RegExp(RegExp.escape(" + text + "), \"g\")") +
                                  ", ...)'");
        return restLoosely();
      }
      replacement = checker_.argumentChecker().checkArg(replacement, Type::string());
      call->builtin = Builtin::StrReplaceAll;
      call->type = Type::string();
      return call;
    }
    if (name == "replace") {
      checker_.diags_.error(
          member->loc(),
          "'replace' with a string replaces only the first occurrence (\"a-b-c\".replace(\"-\", "
          "\"+\") is \"a+b-c\" in JavaScript); to replace all of them write 'replaceAll(" +
              text + ", ...)', for only the first one a regex: 'replace(" + regexText + ", ...)'");
    } else if (name == "search") {
      checker_.diags_.error(member->loc(),
                            "'search' takes a regex, and JavaScript makes one of a string, "
                            "where '.' matches any character; to find the text write "
                            "'indexOf(" +
                                text + ")', for a regex '" +
                                (lit ? regexText : "search(" + regexText + ")") + "'");
    } else {
      checker_.diags_.error(
          member->loc(),
          "'" + name +
              "' takes a regex, and JavaScript makes one of a string, where "
              "'.' matches any character; to check for the text write "
              "'includes(" +
              text + ")', for a regex '" +
              (lit ? regexText + (name == "matchAll" ? "g" : "") : name + "(" + regexText + ")") +
              "'");
    }
    return restLoosely();
  }
  checker_.diags_.error(pattern->loc(),
                        "'" + name + "' takes a regex" +
                            (name == "split" || name == "replaceAll" ? " or a string" : "") +
                            ", found '" + type->name() + "'");

  return restLoosely();
}

// replace(re, x) and replaceAll(re, x): x is the text that replaces each
// match, with its patterns ($1, $<name>, $&...), or a function that gives it.
Expr *StringRegexChecker::checkReplacement(CallExpr *call, MemberExpr *member,
                                           const RegexLitExpr *regex) {
  Expr *&replacement = call->args[1];
  bool function = isa<FunctionExpr>(replacement);
  if (auto *name = dyn_cast<NameExpr>(replacement)) {
    Checker::ScopeEntry entry = checker_.scopes().lookup(name->name);
    function = entry.func ||
               (entry.var && entry.var->type && entry.var->type->nonOptional()->isFunction());
  }
  if (!function) {
    replacement = checker_.argumentChecker().checkArg(replacement, Type::string());
    call->builtin = Builtin::StrReplaceRegex;
    call->type = Type::string();
    auto *text = dyn_cast<StringLitExpr>(replacement);
    if (!regex || !text) {
      return call;
    }
    // `$4` with three groups, which JavaScript would insert as it is.
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
        checker_.diags_.error(text->loc(),
                              "the regex has " + std::to_string(groups) + " group" +
                                  (groups == 1 ? "" : "s") + ", so there is no group " +
                                  std::to_string(one) + " for '$" + std::to_string(one) +
                                  "' (JavaScript would insert it as it is); write '$$' for '$'");
        return call;
      } else if (c == u'<') {
        size_t close = value.find(u'>', i + 2);
        std::string groupName =
            close == std::u16string::npos ? "" : utf8(value.substr(i + 2, close - i - 2));
        bool found = false;
        for (uint32_t k = 1; k <= groups && !found; ++k) {
          uint32_t start, length = tz_re_name(program, k, &start);
          found = length && utf8(units(program, start, length)) == groupName;
        }
        if (found) {
          continue;
        }
        checker_.diags_.error(
            text->loc(),
            named ? "the regex has no group named '" + groupName + "' for '$<" + groupName + ">'"
                  : "the regex has no named groups, so JavaScript would insert '$<" + groupName +
                        ">' as it is; name a group with '(?<" + groupName + ">...)'");
        return call;
      }
    }
    return call;
  }

  // The function gets the match, each group, the offset of the match, the
  // whole string and, with named groups, the object of them.
  Checker::Signature sig;
  sig.pad = false;
  sig.ret = Type::string();
  sig.params.push_back(Type::string());
  const Type *match = checker_.regexChecker().matchType(regex);
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
      checker_.diags_.error(fe->fn->params[1].loc,
                            std::string("the function gets only the match: ") + kUnknownRegex +
                                ", so its groups are not known when compiling");
    } else {
      checker_.diags_.error(fe->fn->params[sig.params.size()].loc,
                            "'" + member->name + "' passes " + std::to_string(sig.params.size()) +
                                " arguments: the match, " + std::to_string(match->matchGroups()) +
                                " group" + (match->matchGroups() == 1 ? "" : "s") +
                                ", the offset and the string" +
                                (sig.params.back()->isObject() ? ", and the named groups" : ""));
    }
    // Checked all the same, as if it got strings there.
    while (sig.params.size() < fe->fn->params.size()) {
      sig.params.push_back(Type::string());
    }
    replacement = checker_.callbackChecker().checkFunctionArg(
        replacement, sig, unsigned(sig.params.size()), member->name);
    return call;
  }
  replacement = checker_.callbackChecker().checkFunctionArg(
      replacement, sig, unsigned(sig.params.size()), member->name);
  if (!replacement->type->isFunction()) {
    return call;
  }
  call->builtin = Builtin::StrReplaceRegexFn;
  call->type = Type::string();
  checker_.flowChecker().calledFunction(); // the function may change anything

  return call;
}

} // namespace tz
