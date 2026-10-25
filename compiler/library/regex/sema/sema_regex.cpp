// Regular expressions (docs/regex.md): literals, compiled here by the
// runtime's own compiler (runtime/library/regex/regex_compile/regex_compile.c,
// built into tz); the types of matches; the methods of RegExp and of the
// strings that take a regex; and the traps of JavaScript, which are errors
// here.
//
// A literal is compiled once, while the program compiles: its syntax errors
// are compile errors, and its flags and groups are known. So are those of a
// variable that holds a literal all along. A match of such a regex knows its
// groups: `m[1]` of a group that takes part in every match is a string, of
// one that may not `string | undefined`, `m[4]` with three groups an error,
// and `m.groups` an object with a field for each name. A regex from elsewhere
// (a parameter, a field, `new RegExp(s)`) is known only when the program runs:
// its flags too, so `s.match(re)`, whose result depends on the g flag, is an
// error for it.

#include "compiler/check/sema/sema.h"

#include "runtime/library/regex/regex/regex.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

#include <cstring>

using llvm::cast;
using llvm::dyn_cast;
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
  for (uint32_t i = 0; i < length; ++i)
    text += static_cast<char16_t>(tz_re_name_unit(program, start, i));
  return text;
}

// The text of a literal as a regex that matches it: `/\./` for ".".
std::string escapedLiteral(const std::u16string &text) {
  std::u16string escaped;
  for (char16_t c : text) {
    if (c < 128 && std::strchr("^$\\.*+?()[]{}|/", static_cast<int>(c)))
      escaped += u'\\';
    escaped += c;
  }
  return "/" + utf8(escaped) + "/";
}

// How a string argument is written, for hints: a literal as itself.
std::string argumentText(const Expr *arg) {
  if (auto *lit = dyn_cast<StringLitExpr>(arg)) {
    std::string text = utf8(lit->value);
    if (llvm::all_of(text, [](char c) { return c >= 0x20 && c != '"' && c != '\\'; }))
      return "\"" + text + "\"";
  }
  if (auto *name = dyn_cast<NameExpr>(arg))
    return name->name.str();
  return "s";
}

bool isFlag(llvm::StringRef name, uint32_t &flag) {
  static const std::pair<llvm::StringLiteral, uint32_t> flags[] = {
      {"hasIndices", TZ_RE_HAS_INDICES},   {"global", TZ_RE_GLOBAL},
      {"ignoreCase", TZ_RE_IGNORE_CASE},   {"multiline", TZ_RE_MULTILINE},
      {"dotAll", TZ_RE_DOT_ALL},           {"unicode", TZ_RE_UNICODE},
      {"unicodeSets", TZ_RE_UNICODE_SETS}, {"sticky", TZ_RE_STICKY},
  };
  for (const auto &[n, f] : flags)
    if (name == n) {
      flag = f;
      return true;
    }
  return false;
}

const char kUnknownRegex[] =
    "the regex is known only when the program runs (it is not a literal, nor a variable that "
    "holds one all along)";

} // namespace

//===----------------------------------------------------------------------===//
// Literals
//===----------------------------------------------------------------------===//

Expr *Sema::checkRegexLit(RegexLitExpr *lit) {
  lit->type = Type::regExp();
  // The literal as the source has it, to show where an error is; empty for
  // one that `new RegExp("...")` stands for.
  llvm::StringRef source(lit->loc().getPointer(), lit->written);
  std::vector<uint16_t> flagUnits(lit->flags.begin(), lit->flags.end());
  uint32_t flags = 0;
  if (!tz_re_parse_flags(flagUnits.data(), flagUnits.size(), &flags)) {
    llvm::SMLoc at = lit->written
                         ? llvm::SMLoc::getFromPointer(source.end() - utf8(lit->flags).size())
                         : lit->loc();
    diags_.error(at, "invalid regular expression flags '" + utf8(lit->flags) +
                         "': the flags are d, g, i, m, s, u, v and y, each at most once, and not "
                         "u with v");
    return lit;
  }
  std::vector<uint16_t> pattern(lit->pattern.begin(), lit->pattern.end());
  const char *error = nullptr;
  size_t errorAt = 0;
  uint32_t *program =
      tz_re_compile(pattern.data(), pattern.size(), flags | TZ_RE_STRICT, &error, &errorAt);
  if (!program) {
    // Where in the pattern: its code units counted through the UTF-8 of the
    // source.
    llvm::SMLoc at = lit->loc();
    if (lit->written) {
      const char *p = source.begin() + 1;
      for (size_t units = 0; units < errorAt && p < source.end();) {
        unsigned char lead = static_cast<unsigned char>(*p);
        size_t size = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        units += size == 4 ? 2 : 1;
        p += size;
      }
      at = llvm::SMLoc::getFromPointer(p);
    }
    std::string message = error;
    if (!message.empty() && message[0] >= 'A' && message[0] <= 'Z')
      message[0] = static_cast<char>(message[0] - 'A' + 'a');
    if (message == "nothing to repeat" || message == "lone quantifier brackets")
      message += "; to match the character itself, escape it with '\\'";
    diags_.error(at, "invalid regular expression: " + message);
    return lit;
  }
  lit->program.assign(program, program + program[TZ_RE_H_SIZE]);
  tz_re_free(program);
  return lit;
}

const RegexLitExpr *Sema::knownRegex(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit())
      break;
    expr = conv->operand;
  }
  if (auto *lit = dyn_cast<RegexLitExpr>(expr))
    return lit->program.empty() ? nullptr : lit;
  if (auto *name = dyn_cast<NameExpr>(expr); name && name->var)
    return name->var->regex;
  return nullptr;
}

void Sema::noteRegexVariable(VarSymbol *var, const Expr *init, bool isConst,
                             llvm::ArrayRef<Stmt *> block, size_t index) {
  auto *lit = llvm::dyn_cast_or_null<RegexLitExpr>(init);
  if (!lit || lit->program.empty())
    return;
  if (!isConst) {
    // A `let` that nothing assigns to after its declaration.
    if (block.empty())
      return;
    bool assigned = false;
    for (const Stmt *stmt : followingStatements(block, index))
      visitExprs(stmt, [&](const Expr *e) {
        if (auto *assign = dyn_cast<AssignExpr>(e))
          if (auto *target = dyn_cast<NameExpr>(assign->target);
              target && target->name == var->name)
            assigned = true;
      });
    if (assigned)
      return;
  }
  var->regex = lit;
}

const Type *Sema::matchType(const RegexLitExpr *regex) {
  if (!regex)
    return Type::match(-1, {}, nullptr, false);
  const uint32_t *program = regex->program.data();
  uint32_t captures = tz_re_captures(program);
  std::vector<bool> mandatory(captures);
  for (uint32_t k = 0; k < captures; ++k)
    mandatory[k] = tz_re_mandatory(program, k);
  // The named groups, a field each, in the order the names first come.
  std::vector<Field> fields;
  for (uint32_t k = 1; k < captures; ++k) {
    uint32_t start, length = tz_re_name(program, k, &start);
    if (!length)
      continue;
    std::string name = utf8(units(program, start, length));
    if (llvm::any_of(fields, [&](const Field &f) { return f.name == name; }))
      continue;
    const Type *type =
        tz_re_name_mandatory(program, k) ? Type::string() : Type::optional(Type::string());
    fields.push_back({name, type, false});
  }
  const Type *named = fields.empty() ? nullptr : Type::object(std::move(fields));
  return Type::match(int(captures) - 1, std::move(mandatory), named,
                     (tz_re_flags(program) & TZ_RE_HAS_INDICES) != 0);
}

Expr *Sema::regexToString(Expr *regex) {
  auto *callee = ctx_.make<NameExpr>(regex->loc(), "String");
  return builtinCall(regex->loc(), callee, Builtin::RegExpToString, {regex}, Type::string());
}

//===----------------------------------------------------------------------===//
// RegExp
//===----------------------------------------------------------------------===//

Expr *Sema::checkRegexMethod(CallExpr *call, MemberExpr *member) {
  llvm::StringRef name = member->name;
  const RegexLitExpr *regex = knownRegex(member->object);
  call->regex = regex;
  call->type = Type::error();
  if (rejectSpread(call))
    return call;
  if (name == "test" || name == "exec") {
    if (!checkArity(call, member->loc(), 1, 1))
      return call;
    call->args[0] = checkArg(call->args[0], Type::string());
    uint32_t flags = regex ? tz_re_flags(regex->program.data()) : 0;
    if (name == "test" && (flags & TZ_RE_GLOBAL)) {
      diags_.error(member->loc(),
                   "'test' of a regex with the 'g' flag starts where the previous match ended "
                   "(lastIndex), so the same call can give true and then false; remove 'g'");
      return call;
    }
    call->builtin = name == "test" ? Builtin::RegExpTest : Builtin::RegExpExec;
    call->type = name == "test" ? Type::boolTy() : Type::optional(matchType(regex));
    return call;
  }
  if (name == "toString") {
    if (!checkArity(call, member->loc(), 0, 0))
      return call;
    call->args.push_back(regexToString(member->object));
    call->builtin = Builtin::StringOf;
    call->type = Type::string();
    return call;
  }
  if (name == "compile")
    diags_.error(member->loc(), "'compile' is a legacy method that changes the regex in place; "
                                "make a new one instead: 'new RegExp(pattern, flags)'");
  else
    diags_.error(member->loc(), "property '" + name + "' does not exist on type 'RegExp'");
  for (Expr *&arg : call->args)
    arg = checkExpr(arg);
  return call;
}

// The properties of a regex: lastIndex is a field; the others say what the
// regex is.
Expr *Sema::checkRegexMember(MemberExpr *member) {
  llvm::StringRef name = member->name;
  if (name == "lastIndex")
    return checkField(member);
  if (name == "source")
    return builtinCall(member->loc(), member, Builtin::RegExpSource, {}, Type::string());
  if (name == "flags")
    return builtinCall(member->loc(), member, Builtin::RegExpFlags, {}, Type::string());
  uint32_t flag;
  if (isFlag(name, flag)) {
    auto *bit = ctx_.make<IntLitExpr>(member->loc(), flag);
    bit->type = Type::intTy();
    return builtinCall(member->loc(), member, Builtin::RegExpFlag, {bit}, Type::boolTy());
  }
  diags_.error(member->loc(),
               "property '" + name + "' does not exist on type 'RegExp'" +
                   (name == "test" || name == "exec" ? "; methods must be called" : ""));
  member->type = Type::error();
  return member;
}

Expr *Sema::checkRegExpConstructor(llvm::SMLoc loc, std::vector<Expr *> &args) {
  auto loosely = [&] {
    for (Expr *&arg : args)
      arg = checkExpr(arg);
    auto *error = ctx_.make<UndefinedExpr>(loc);
    error->type = Type::error();
    return error;
  };
  if (args.empty() || args.size() > 2) {
    diags_.error(loc,
                 "expected " + argumentCount(1, 2) + ", but got " + std::to_string(args.size()));
    return loosely();
  }
  // Text known when compiling: a literal, or a template without values.
  auto literal = [](const Expr *e) -> std::optional<std::u16string> {
    if (auto *lit = dyn_cast<StringLitExpr>(e))
      return lit->value;
    if (auto *tmpl = dyn_cast<TemplateExpr>(e); tmpl && tmpl->exprs.empty())
      return tmpl->quasis[0];
    return std::nullopt;
  };
  std::optional<std::u16string> pattern = literal(args[0]);
  std::optional<std::u16string> flags = args.size() > 1 ? literal(args[1]) : std::u16string();
  if (pattern && flags) {
    // The same as a literal, compiled now.
    auto *lit = ctx_.make<RegexLitExpr>(loc, *pattern, *flags);
    return checkRegexLit(lit);
  }
  args[0] = checkExpr(args[0]);
  if (args[0]->type->isRegExp()) {
    diags_.error(args[0]->loc(), "'new RegExp' takes the pattern as a string; for a copy of a "
                                 "regex with other flags write 'new RegExp(re.source, flags)'");
    return loosely();
  }
  args[0] = coerce(args[0], Type::string());
  if (args.size() > 1)
    args[1] = checkArg(args[1], Type::string());
  return builtinCall(loc, ctx_.make<NameExpr>(loc, "RegExp"), Builtin::RegExpNew, args,
                     Type::regExp());
}

Expr *Sema::checkRegExpStatic(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  if (member->name != "escape") {
    checkRegExpStaticMember(member);
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
    return call;
  }
  if (rejectSpread(call) || !checkArity(call, member->loc(), 1, 1))
    return call;
  call->args[0] = checkArg(call->args[0], Type::string());
  call->builtin = Builtin::RegExpEscape;
  call->type = Type::string();
  return call;
}

Expr *Sema::checkRegExpStaticMember(MemberExpr *member) {
  member->type = Type::error();
  llvm::StringRef name = member->name;
  if (name == "escape")
    diags_.error(member->loc(), "'RegExp.escape' can be used as a value only where a function "
                                "type is expected, e.g. as a callback");
  else if (name.starts_with("$") || name == "input" || name == "lastMatch" || name == "lastParen" ||
           name == "leftContext" || name == "rightContext")
    diags_.error(member->loc(),
                 "'RegExp." + name +
                     "' is a legacy property that holds what the last match of any regex "
                     "found; read it from the match itself, e.g. 'm[1]'");
  else
    diags_.error(member->loc(), "property '" + name + "' does not exist on 'RegExp'");
  return member;
}

//===----------------------------------------------------------------------===//
// Strings
//===----------------------------------------------------------------------===//

Expr *Sema::checkStringRegexMethod(CallExpr *call, MemberExpr *member) {
  llvm::StringRef name = member->name;
  static const llvm::StringLiteral methods[] = {"match",   "matchAll",   "search",
                                                "replace", "replaceAll", "split"};
  if (!llvm::is_contained(methods, name) || (name == "split" && call->args.empty()))
    return nullptr;
  call->type = Type::error();
  bool replace = name == "replace" || name == "replaceAll";
  unsigned min = replace ? 2 : name == "split" ? 0 : 1, max = name == "split" ? 2 : replace ? 2 : 1;
  if (!checkArity(call, member->loc(), min, max))
    return call;
  auto restLoosely = [&] {
    for (size_t i = 1; i < call->args.size(); ++i)
      call->args[i] = checkExpr(call->args[i]);
    return call;
  };
  Expr *&pattern = call->args[0];
  pattern = checkExpr(pattern);
  const Type *type = pattern->type;
  if (type->isError())
    return restLoosely();

  if (type->isRegExp()) {
    const RegexLitExpr *regex = knownRegex(pattern);
    call->regex = regex;
    std::optional<uint32_t> flags;
    if (regex)
      flags = tz_re_flags(regex->program.data());
    bool global = flags && (*flags & TZ_RE_GLOBAL);
    if (name == "match") {
      if (!flags) {
        diags_.error(member->loc(),
                     std::string("what 'match' gives depends on the g flag (all the texts that "
                                 "match, or one match with its groups), and ") +
                         kUnknownRegex +
                         "; for one match write 're.exec(s)', for all of them "
                         "'s.matchAll(re)'");
        return call;
      }
      call->builtin = global ? Builtin::StrMatchGlobal : Builtin::StrMatch;
      call->type = Type::optional(global ? Type::array(Type::string()) : matchType(regex));
      return call;
    }
    if (name == "matchAll" || name == "replaceAll") {
      if (flags && !global) {
        std::string source = "/" + utf8(regex->pattern) + "/" + utf8(regex->flags) + "g";
        diags_.error(pattern->loc(), "'" + name +
                                         "' needs a regex with the g flag (JavaScript throws a "
                                         "TypeError without it), e.g. '" +
                                         source + "'");
        return restLoosely();
      }
      if (name == "matchAll") {
        // A lazy iterator, as in JavaScript: each match is found when it is
        // asked for.
        call->builtin = Builtin::StrMatchAll;
        call->type = Type::iterator(matchType(regex));
        return call;
      }
    }
    if (name == "search") {
      call->builtin = Builtin::StrSearch;
      call->type = Type::intTy();
      return call;
    }
    if (name == "split") {
      if (call->args.size() == 2)
        call->args[1] = checkPosition(call->args[1]);
      // A group that may not take part in a match gives undefined.
      bool undefinedGroups = !regex;
      for (uint32_t k = 1; regex && k < tz_re_captures(regex->program.data()); ++k)
        undefinedGroups |= !tz_re_mandatory(regex->program.data(), k);
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
      if (call->args.size() == 2)
        call->args[1] = checkPosition(call->args[1]);
      call->builtin = Builtin::StrSplit;
      call->type = Type::array(Type::string());
      return call;
    }
    if (name == "replaceAll") {
      Expr *&replacement = call->args[1];
      if (isa<FunctionExpr>(replacement)) {
        diags_.error(replacement->loc(),
                     "a function as the replacement needs a regex: 'replaceAll(" +
                         (lit ? escapedLiteral(lit->value) + "g"
                              : "new RegExp(RegExp.escape(" + text + "), \"g\")") +
                         ", ...)'");
        return restLoosely();
      }
      replacement = checkArg(replacement, Type::string());
      call->builtin = Builtin::StrReplaceAll;
      call->type = Type::string();
      return call;
    }
    if (name == "replace")
      diags_.error(
          member->loc(),
          "'replace' with a string replaces only the first occurrence (\"a-b-c\".replace(\"-\", "
          "\"+\") is \"a+b-c\" in JavaScript); to replace all of them write 'replaceAll(" +
              text + ", ...)', for only the first one a regex: 'replace(" + regexText + ", ...)'");
    else if (name == "search")
      diags_.error(member->loc(), "'search' takes a regex, and JavaScript makes one of a string, "
                                  "where '.' matches any character; to find the text write "
                                  "'indexOf(" +
                                      text + ")', for a regex '" +
                                      (lit ? regexText : "search(" + regexText + ")") + "'");
    else
      diags_.error(
          member->loc(),
          "'" + name +
              "' takes a regex, and JavaScript makes one of a string, where "
              "'.' matches any character; to check for the text write "
              "'includes(" +
              text + ")', for a regex '" +
              (lit ? regexText + (name == "matchAll" ? "g" : "") : name + "(" + regexText + ")") +
              "'");
    return restLoosely();
  }
  diags_.error(pattern->loc(), "'" + name + "' takes a regex" +
                                   (name == "split" || name == "replaceAll" ? " or a string" : "") +
                                   ", found '" + type->name() + "'");
  return restLoosely();
}

// replace(re, x) and replaceAll(re, x): x is the text that replaces each
// match, with its patterns ($1, $<name>, $&...), or a function that gives it.
Expr *Sema::checkReplacement(CallExpr *call, MemberExpr *member, const RegexLitExpr *regex) {
  Expr *&replacement = call->args[1];
  bool function = isa<FunctionExpr>(replacement);
  if (auto *name = dyn_cast<NameExpr>(replacement)) {
    ScopeEntry entry = lookup(name->name);
    function = entry.func ||
               (entry.var && entry.var->type && entry.var->type->nonOptional()->isFunction());
  }
  if (!function) {
    replacement = checkArg(replacement, Type::string());
    call->builtin = Builtin::StrReplaceRegex;
    call->type = Type::string();
    auto *text = dyn_cast<StringLitExpr>(replacement);
    if (!regex || !text)
      return call;
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
      if (value[i] != u'$')
        continue;
      char16_t c = value[i + 1];
      if (c == u'$') {
        ++i;
      } else if (c >= u'1' && c <= u'9') {
        uint32_t one = c - u'0';
        char16_t next = i + 2 < value.size() ? value[i + 2] : 0;
        uint32_t two = next >= u'0' && next <= u'9' ? one * 10 + (next - u'0') : 0;
        if ((two && two <= groups) || one <= groups)
          continue;
        diags_.error(text->loc(),
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
        if (found)
          continue;
        diags_.error(
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
  Signature sig;
  sig.pad = false;
  sig.ret = Type::string();
  sig.params.push_back(Type::string());
  const Type *match = matchType(regex);
  if (regex) {
    for (int k = 1; k <= match->matchGroups(); ++k)
      sig.params.push_back(match->captureMandatory(unsigned(k)) ? Type::string()
                                                                : Type::optional(Type::string()));
    sig.params.push_back(Type::intTy());
    sig.params.push_back(Type::string());
    if (const Type *named = match->field("groups")->type; !named->isOptional())
      sig.params.push_back(named);
  }
  if (auto *fe = dyn_cast<FunctionExpr>(replacement);
      fe && fe->fn->params.size() > sig.params.size()) {
    if (!regex)
      diags_.error(fe->fn->params[1].loc, std::string("the function gets only the match: ") +
                                              kUnknownRegex +
                                              ", so its groups are not known when compiling");
    else
      diags_.error(fe->fn->params[sig.params.size()].loc,
                   "'" + member->name + "' passes " + std::to_string(sig.params.size()) +
                       " arguments: the match, " + std::to_string(match->matchGroups()) + " group" +
                       (match->matchGroups() == 1 ? "" : "s") + ", the offset and the string" +
                       (sig.params.back()->isObject() ? ", and the named groups" : ""));
    // Checked all the same, as if it got strings there.
    while (sig.params.size() < fe->fn->params.size())
      sig.params.push_back(Type::string());
    replacement = checkFunctionArg(replacement, sig, unsigned(sig.params.size()), member->name);
    return call;
  }
  replacement = checkFunctionArg(replacement, sig, unsigned(sig.params.size()), member->name);
  if (!replacement->type->isFunction())
    return call;
  call->builtin = Builtin::StrReplaceRegexFn;
  call->type = Type::string();
  calledFunction(); // the function may change anything
  return call;
}

//===----------------------------------------------------------------------===//
// Matches
//===----------------------------------------------------------------------===//

Expr *Sema::matchItems(Expr *match) {
  const Type *array = Type::array(Type::optional(Type::string()));
  if (match->type->isOptional()) {
    // `m?.%items`
    bool outer = std::exchange(chainShortCircuits_, false);
    Expr *items = matchItems(chainLink(match));
    auto *chain = ctx_.make<OptionalChainExpr>(match->loc(), items);
    chain->mayShortCircuit = chainShortCircuits_;
    chain->type = Type::optional(array);
    chainShortCircuits_ = outer;
    return chain;
  }
  auto *items = ctx_.make<MemberExpr>(match->loc(), match, "%items");
  items->type = array;
  return items;
}

Expr *Sema::checkMatchMember(MemberExpr *member) {
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  if (name == "length") {
    member->object = matchItems(member->object);
    member->type = Type::intTy();
    return member;
  }
  if (name == "index" || name == "input")
    return checkField(member);
  member->type = Type::error();
  if (name == "groups") {
    const Type *groups = type->field("groups")->type;
    if (!groups->isOptional())
      return checkField(member);
    if (type->matchGroups() < 0)
      diags_.error(member->loc(), std::string("the named groups of a match are not supported "
                                              "yet when ") +
                                      kUnknownRegex + "; read the groups by number: 'm[1]'");
    else
      diags_.error(member->loc(), "the regex has no named groups; name one with '(?<name>...)'");
    return member;
  }
  if (name == "indices") {
    if (type->field("indices"))
      return checkField(member);
    diags_.error(member->loc(),
                 type->matchGroups() < 0
                     ? std::string("'indices' of a match needs the d flag, and ") + kUnknownRegex
                     : "a match has 'indices' only with the d flag: add 'd' to "
                       "the flags of the regex");
    return member;
  }
  diags_.error(member->loc(),
               "property '" + name + "' does not exist on type '" + type->name() + "'");
  return member;
}

Expr *Sema::checkMatchIndex(IndexExpr *index) {
  const Type *type = index->object->type;
  index->index = checkPosition(index->index);
  index->type = Type::error();
  if (index->index->type->isError())
    return index;
  index->object = matchItems(index->object);
  const Type *item = Type::optional(Type::string());
  std::optional<int64_t> k = intConstant(index->index);
  int groups = type->matchGroups();
  // The match itself is m[0] of any regex.
  if (k && *k == 0) {
    index->mayBeOutOfRange = false;
    index->type = item;
    return convert(index, Type::string());
  }
  if (k && groups >= 0) {
    if (*k < 0 || *k > groups) {
      diags_.error(index->index->loc(),
                   "the regex has " + std::to_string(groups) + " group" + (groups == 1 ? "" : "s") +
                       (groups ? " (m[1] to m[" + std::to_string(groups) + "]; m[0] is the match)"
                               : " (m[0] is the match)") +
                       ", so there is no m[" + std::to_string(*k) + "]");
      return index;
    }
    index->mayBeOutOfRange = false;
    index->type = item;
    // A group that takes part in every match holds a value.
    if (type->captureMandatory(unsigned(*k)))
      return convert(index, Type::string());
    return index;
  }
  // Otherwise as an element of an array: undefined past the end.
  index->mayBeOutOfRange = true;
  index->type = item;
  return index;
}

} // namespace tz
