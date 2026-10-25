#include "compiler/library/regex/regex_checker/regex_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/objects/object_checker/object_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/library/process/process_checker/process_checker.h"
#include "compiler/whole_program/flows/flows.h"
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

bool isFlag(llvm::StringRef name, uint32_t &flag) {
  static const std::pair<llvm::StringLiteral, uint32_t> flags[] = {
      {"hasIndices", TZ_RE_HAS_INDICES},   {"global", TZ_RE_GLOBAL},
      {"ignoreCase", TZ_RE_IGNORE_CASE},   {"multiline", TZ_RE_MULTILINE},
      {"dotAll", TZ_RE_DOT_ALL},           {"unicode", TZ_RE_UNICODE},
      {"unicodeSets", TZ_RE_UNICODE_SETS}, {"sticky", TZ_RE_STICKY},
  };
  for (const auto &[n, f] : flags) {
    if (name == n) {
      flag = f;
      return true;
    }
  }

  return false;
}

} // namespace

RegexChecker::RegexChecker(Checker &checker) : checker_(checker) {}

Expr *RegexChecker::checkRegexLit(RegexLitExpr *lit) {
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
    checker_.diags_.error(
        at, "invalid regular expression flags '" + utf8(lit->flags) +
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
    if (!message.empty() && message[0] >= 'A' && message[0] <= 'Z') {
      message[0] = static_cast<char>(message[0] - 'A' + 'a');
    }
    if (message == "nothing to repeat" || message == "lone quantifier brackets") {
      message += "; to match the character itself, escape it with '\\'";
    }
    checker_.diags_.error(at, "invalid regular expression: " + message);
    return lit;
  }
  lit->program.assign(program, program + program[TZ_RE_H_SIZE]);
  tz_re_free(program);

  return lit;
}

const RegexLitExpr *RegexChecker::knownRegex(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit()) {
      break;
    }
    expr = conv->operand;
  }
  if (auto *lit = dyn_cast<RegexLitExpr>(expr)) {
    return lit->program.empty() ? nullptr : lit;
  }
  if (auto *name = dyn_cast<NameExpr>(expr); name && name->var) {
    return name->var->regex;
  }

  return nullptr;
}

void RegexChecker::noteRegexVariable(VarSymbol *var, const Expr *init, bool isConst,
                                     llvm::ArrayRef<Stmt *> block, size_t index) {
  auto *lit = llvm::dyn_cast_or_null<RegexLitExpr>(init);
  if (!lit || lit->program.empty()) {
    return;
  }
  if (!isConst) {
    // A `let` that nothing assigns to after its declaration.
    if (block.empty()) {
      return;
    }
    bool assigned = false;
    for (const Stmt *stmt : followingStatements(block, index)) {
      visitExprs(stmt, [&](const Expr *e) {
        if (auto *assign = dyn_cast<AssignExpr>(e)) {
          if (auto *target = dyn_cast<NameExpr>(assign->target);
              target && target->name == var->name) {
            assigned = true;
          }
        }
      });
    }
    if (assigned) {
      return;
    }
  }
  var->regex = lit;
}

const Type *RegexChecker::matchType(const RegexLitExpr *regex) {
  if (!regex) {
    return Type::match(-1, {}, nullptr, false);
  }
  const uint32_t *program = regex->program.data();
  uint32_t captures = tz_re_captures(program);
  std::vector<bool> mandatory(captures);
  for (uint32_t k = 0; k < captures; ++k) {
    mandatory[k] = tz_re_mandatory(program, k);
  }
  // The named groups, a field each, in the order the names first come.
  std::vector<Field> fields;
  for (uint32_t k = 1; k < captures; ++k) {
    uint32_t start, length = tz_re_name(program, k, &start);
    if (!length) {
      continue;
    }
    std::string name = utf8(units(program, start, length));
    if (llvm::any_of(fields, [&](const Field &f) { return f.name == name; })) {
      continue;
    }
    const Type *type =
        tz_re_name_mandatory(program, k) ? Type::string() : Type::optional(Type::string());
    fields.push_back({name, type, false});
  }
  const Type *named = fields.empty() ? nullptr : Type::object(std::move(fields));

  return Type::match(int(captures) - 1, std::move(mandatory), named,
                     (tz_re_flags(program) & TZ_RE_HAS_INDICES) != 0);
}

Expr *RegexChecker::regexToString(Expr *regex) {
  auto *callee = checker_.ctx_.make<NameExpr>(regex->loc(), "String");

  return checker_.processChecker().builtinCall(regex->loc(), callee, Builtin::RegExpToString,
                                               {regex}, Type::string());
}

Expr *RegexChecker::checkRegexMethod(CallExpr *call, MemberExpr *member) {
  llvm::StringRef name = member->name;
  const RegexLitExpr *regex = knownRegex(member->object);
  call->regex = regex;
  call->type = Type::error();
  if (checker_.callChecker().rejectSpread(call)) {
    return call;
  }
  if (name == "test" || name == "exec") {
    if (!checker_.argumentChecker().checkArity(call, member->loc(), 1, 1)) {
      return call;
    }
    call->args[0] = checker_.argumentChecker().checkArg(call->args[0], Type::string());
    uint32_t flags = regex ? tz_re_flags(regex->program.data()) : 0;
    if (name == "test" && (flags & TZ_RE_GLOBAL)) {
      checker_.diags_.error(
          member->loc(),
          "'test' of a regex with the 'g' flag starts where the previous match ended "
          "(lastIndex), so the same call can give true and then false; remove 'g'");
      return call;
    }
    call->builtin = name == "test" ? Builtin::RegExpTest : Builtin::RegExpExec;
    call->type = name == "test" ? Type::boolTy() : Type::optional(matchType(regex));
    return call;
  }
  if (name == "toString") {
    if (!checker_.argumentChecker().checkArity(call, member->loc(), 0, 0)) {
      return call;
    }
    call->args.push_back(regexToString(member->object));
    call->builtin = Builtin::StringOf;
    call->type = Type::string();
    return call;
  }
  if (name == "compile") {
    checker_.diags_.error(member->loc(),
                          "'compile' is a legacy method that changes the regex in place; "
                          "make a new one instead: 'new RegExp(pattern, flags)'");
  } else {
    checker_.diags_.error(member->loc(), "property '" + name + "' does not exist on type 'RegExp'");
  }
  for (Expr *&arg : call->args) {
    arg = checker_.exprChecker().checkExpr(arg);
  }

  return call;
}

// The properties of a regex: lastIndex is a field; the others say what the
// regex is.
Expr *RegexChecker::checkRegexMember(MemberExpr *member) {
  llvm::StringRef name = member->name;
  if (name == "lastIndex") {
    return checker_.objectChecker().checkField(member);
  }
  if (name == "source") {
    return checker_.processChecker().builtinCall(member->loc(), member, Builtin::RegExpSource, {},
                                                 Type::string());
  }
  if (name == "flags") {
    return checker_.processChecker().builtinCall(member->loc(), member, Builtin::RegExpFlags, {},
                                                 Type::string());
  }
  uint32_t flag;
  if (isFlag(name, flag)) {
    auto *bit = checker_.ctx_.make<IntLitExpr>(member->loc(), flag);
    bit->type = Type::intTy();
    return checker_.processChecker().builtinCall(member->loc(), member, Builtin::RegExpFlag, {bit},
                                                 Type::boolTy());
  }
  checker_.diags_.error(member->loc(),
                        "property '" + name + "' does not exist on type 'RegExp'" +
                            (name == "test" || name == "exec" ? "; methods must be called" : ""));
  member->type = Type::error();

  return member;
}

Expr *RegexChecker::checkRegExpConstructor(llvm::SMLoc loc, std::vector<Expr *> &args) {
  auto loosely = [&] {
    for (Expr *&arg : args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }
    auto *error = checker_.ctx_.make<UndefinedExpr>(loc);
    error->type = Type::error();

    return error;
  };
  if (args.empty() || args.size() > 2) {
    checker_.diags_.error(loc, "expected " + argumentCount(1, 2) + ", but got " +
                                   std::to_string(args.size()));
    return loosely();
  }
  // Text known when compiling: a literal, or a template without values.
  auto literal = [](const Expr *e) -> std::optional<std::u16string> {
    if (auto *lit = dyn_cast<StringLitExpr>(e)) {
      return lit->value;
    }
    if (auto *tmpl = dyn_cast<TemplateExpr>(e); tmpl && tmpl->exprs.empty()) {
      return tmpl->quasis[0];
    }

    return std::nullopt;
  };
  std::optional<std::u16string> pattern = literal(args[0]);
  std::optional<std::u16string> flags = args.size() > 1 ? literal(args[1]) : std::u16string();
  if (pattern && flags) {
    // The same as a literal, compiled now.
    auto *lit = checker_.ctx_.make<RegexLitExpr>(loc, *pattern, *flags);
    return checkRegexLit(lit);
  }
  args[0] = checker_.exprChecker().checkExpr(args[0]);
  if (args[0]->type->isRegExp()) {
    checker_.diags_.error(args[0]->loc(),
                          "'new RegExp' takes the pattern as a string; for a copy of a "
                          "regex with other flags write 'new RegExp(re.source, flags)'");

    return loosely();
  }
  args[0] = checker_.typeRelations().coerce(args[0], Type::string());
  if (args.size() > 1) {
    args[1] = checker_.argumentChecker().checkArg(args[1], Type::string());
  }

  return checker_.processChecker().builtinCall(loc, checker_.ctx_.make<NameExpr>(loc, "RegExp"),
                                               Builtin::RegExpNew, args, Type::regExp());
}

Expr *RegexChecker::checkRegExpStatic(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  if (member->name != "escape") {
    checkRegExpStaticMember(member);
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }
    return call;
  }
  if (checker_.callChecker().rejectSpread(call) ||
      !checker_.argumentChecker().checkArity(call, member->loc(), 1, 1)) {
    return call;
  }
  call->args[0] = checker_.argumentChecker().checkArg(call->args[0], Type::string());
  call->builtin = Builtin::RegExpEscape;
  call->type = Type::string();

  return call;
}

Expr *RegexChecker::checkRegExpStaticMember(MemberExpr *member) {
  member->type = Type::error();
  llvm::StringRef name = member->name;
  if (name == "escape") {
    checker_.diags_.error(member->loc(),
                          "'RegExp.escape' can be used as a value only where a function "
                          "type is expected, e.g. as a callback");
  } else if (name.starts_with("$") || name == "input" || name == "lastMatch" ||
             name == "lastParen" || name == "leftContext" || name == "rightContext") {
    checker_.diags_.error(member->loc(),
                          "'RegExp." + name +
                              "' is a legacy property that holds what the last match of any regex "
                              "found; read it from the match itself, e.g. 'm[1]'");
  } else {
    checker_.diags_.error(member->loc(), "property '" + name + "' does not exist on 'RegExp'");
  }

  return member;
}

} // namespace tz
