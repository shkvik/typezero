#include "compiler/pipeline/check_program/topics/regex/regex_checker/regex_checker.h"
#include "compiler/pipeline/check_program/ast_walker/ast_walker.h"
#include "compiler/pipeline/check_program/declare/prelude/prelude.h"
#include "compiler/pipeline/check_program/topics/expr/argument_checker/argument_checker.h"
#include "compiler/pipeline/check_program/topics/expr/call_checker/call_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "compiler/pipeline/check_program/topics/objects/object_checker/object_checker.h"
#include "compiler/pipeline/check_program/topics/process/process_checker/process_checker.h"
#include "compiler/pipeline/check_program/topics/stmt/var_checker/var_checker.h"
#include "compiler/pipeline/check_program/types/type_relations/type_relations.h"
#include "compiler/pipeline/check_program/whole_program/flows/flows.h"
#include "runtime/library/regex/regex/regex.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <cstring>

using llvm::dyn_cast;

namespace tz {

std::string RegexChecker::utf8(const std::u16string &text) {
  std::string out;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      out);

  return out;
}

std::u16string RegexChecker::units(const uint32_t *program, uint32_t start, uint32_t length) {
  std::u16string text;
  for (uint32_t i = 0; i < length; ++i) {
    text += static_cast<char16_t>(tz_re_name_unit(program, start, i));
  }

  return text;
}

bool RegexChecker::isFlag(llvm::StringRef name, uint32_t &flag) {
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

RegexChecker::RegexChecker(CheckState &owner)
    : state(owner) {}

Expr *RegexChecker::checkRegexLit(RegexLitExpr *lit) {
  lit->type = Type::regExp();
  llvm::StringRef source(lit->loc().getPointer(), lit->written);
  std::vector<uint16_t> flagUnits(lit->flags.begin(), lit->flags.end());
  uint32_t flags = 0;
  if (!tz_re_parse_flags(flagUnits.data(), flagUnits.size(), &flags)) {
    llvm::SMLoc at =
        lit->written
            ? llvm::SMLoc::getFromPointer(source.end() - RegexChecker::utf8(lit->flags).size())
            : lit->loc();
    this->state.getDiagnostics().error(
        at, "invalid regular expression flags '" + RegexChecker::utf8(lit->flags) +
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
    this->state.getDiagnostics().error(at, "invalid regular expression: " + message);
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
  if (auto *name = dyn_cast<NameExpr>(expr); name && name->var()) {
    return name->var()->regex();
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
    if (block.empty()) {
      return;
    }
    bool assigned = false;
    for (const Stmt *stmt : VarChecker::followingStatements(block, index)) {
      AstWalker::visitExprs(stmt, [&](const Expr *e) {
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
  var->holds = RegexLiteral{.regex = lit};
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
  std::vector<Field> fields;
  for (uint32_t k = 1; k < captures; ++k) {
    uint32_t start, length = tz_re_name(program, k, &start);
    if (!length) {
      continue;
    }
    std::string name = RegexChecker::utf8(RegexChecker::units(program, start, length));
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
  auto *callee = this->state.getAst().make<NameExpr>(regex->loc(), "String");

  return ProcessChecker(this->state)
      .builtinCall(regex->loc(), callee, Builtin::RegExpToString, {regex}, Type::string());
}

Expr *RegexChecker::checkRegexMethod(CallExpr *call, MemberExpr *member) {
  llvm::StringRef name = member->name;
  const RegexLitExpr *regex = RegexChecker::knownRegex(member->object);
  call->regex = regex;
  call->type = Type::error();
  if (CallChecker(this->state).rejectSpread(call)) {
    return call;
  }
  if (name == "test" || name == "exec") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    call->args[0] = ArgumentChecker(this->state).checkArg(call->args[0], Type::string());
    uint32_t flags = regex ? tz_re_flags(regex->program.data()) : 0;
    if (name == "test" && (flags & TZ_RE_GLOBAL)) {
      this->state.getDiagnostics().error(
          member->loc(),
          "'test' of a regex with the 'g' flag starts where the previous match ended "
          "(lastIndex), so the same call can give true and then false; remove 'g'");
      return call;
    }
    call->target =
        BuiltinCall{.builtin = name == "test" ? Builtin::RegExpTest : Builtin::RegExpExec};
    call->type = name == "test" ? Type::boolTy() : Type::optional(this->matchType(regex));
    return call;
  }
  if (name == "toString") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 0, 0);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    call->args.push_back(this->regexToString(member->object));
    call->target = BuiltinCall{.builtin = Builtin::StringOf};
    call->type = Type::string();
    return call;
  }
  if (name == "compile") {
    this->state.getDiagnostics().error(
        member->loc(), "'compile' is a legacy method that changes the regex in place; "
                       "make a new one instead: 'new RegExp(pattern, flags)'");
  } else {
    this->state.getDiagnostics().error(member->loc(),
                                       "property '" + name + "' does not exist on type 'RegExp'");
  }
  for (Expr *&arg : call->args) {
    arg = ExprChecker(this->state).checkExpr(arg);
  }

  return call;
}

Expr *RegexChecker::checkRegexMember(MemberExpr *member) {
  llvm::StringRef name = member->name;
  if (name == "lastIndex") {
    ObjectChecker::ResultObjectFieldValue field = ObjectChecker(this->state).checkField(member);
    if (field.isErr()) {
      this->state.getDiagnostics().error(member->loc(), field.error().message);
      return member;
    }
    return field.value();
  }
  if (name == "source") {
    return ProcessChecker(this->state)
        .builtinCall(member->loc(), member, Builtin::RegExpSource, {}, Type::string());
  }
  if (name == "flags") {
    return ProcessChecker(this->state)
        .builtinCall(member->loc(), member, Builtin::RegExpFlags, {}, Type::string());
  }
  uint32_t flag;
  if (RegexChecker::isFlag(name, flag)) {
    auto *bit = this->state.getAst().make<IntLitExpr>(member->loc(), flag);
    bit->type = Type::intTy();
    return ProcessChecker(this->state)
        .builtinCall(member->loc(), member, Builtin::RegExpFlag, {bit}, Type::boolTy());
  }
  this->state.getDiagnostics().error(
      member->loc(), "property '" + name + "' does not exist on type 'RegExp'" +
                         (name == "test" || name == "exec" ? "; methods must be called" : ""));
  member->type = Type::error();

  return member;
}

Expr *RegexChecker::checkRegExpConstructor(llvm::SMLoc loc, std::vector<Expr *> &args) {
  auto loosely = [&] {
    for (Expr *&arg : args) {
      arg = ExprChecker(this->state).checkExpr(arg);
    }
    auto *error = this->state.getAst().make<UndefinedExpr>(loc);
    error->type = Type::error();

    return error;
  };
  if (args.empty() || args.size() > 2) {
    this->state.getDiagnostics().error(loc, "expected " + ErrorTextArity::wanted(1, 2) +
                                                ", but got " + std::to_string(args.size()));
    return loosely();
  }
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
    auto *lit = this->state.getAst().make<RegexLitExpr>(loc, *pattern, *flags);
    return this->checkRegexLit(lit);
  }
  auto *escaped = dyn_cast<CallExpr>(args[0]);
  auto *escape = escaped ? dyn_cast<MemberExpr>(escaped->callee) : nullptr;
  bool fromEscape = escape && escape->name == "escape" &&
                    CallChecker(this->state).isGlobalName(escape->object, "RegExp");
  if (!fromEscape || !flags) {
    this->state.getDiagnostics().error(
        loc, "'new RegExp' of a pattern known only when the program runs may fail, "
             "and 'new' gives no Result; write 'RegExp.parse(pattern" +
                 std::string(args.size() > 1 ? ", flags" : "") +
                 ")', which gives a Result<RegExp, RegexError>");
    return loosely();
  }
  std::vector<uint16_t> flagUnits(flags->begin(), flags->end());
  uint32_t parsedFlags = 0;
  if (!tz_re_parse_flags(flagUnits.data(), flagUnits.size(), &parsedFlags)) {
    this->state.getDiagnostics().error(
        args[1]->loc(), "invalid regular expression flags '" + RegexChecker::utf8(*flags) +
                            "': the flags are d, g, i, m, s, u, v and y, each at most once, "
                            "and not u with v");
    return loosely();
  }
  args[0] = ExprChecker(this->state).checkExpr(args[0]);
  args[0] = TypeRelations(this->state).coerce(args[0], Type::string());
  if (args.size() > 1) {
    args[1] = ArgumentChecker(this->state).checkArg(args[1], Type::string());
  }

  return ProcessChecker(this->state)
      .builtinCall(loc, this->state.getAst().make<NameExpr>(loc, "RegExp"), Builtin::RegExpNew,
                   args, Type::regExp());
}

Expr *RegexChecker::checkRegExpStatic(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  if (member->name == "parse") {
    bool rejected = CallChecker(this->state).rejectSpread(call);
    ArgumentChecker::ResultArityMatches arity =
        rejected ? ok(call) : ArgumentChecker(this->state).checkArity(call, 1, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
    }
    if (rejected || arity.isErr()) {
      return call;
    }
    for (Expr *&arg : call->args) {
      arg = ArgumentChecker(this->state).checkArg(arg, Type::string());
    }
    call->target = BuiltinCall{.builtin = Builtin::RegExpParse};
    call->type = Type::result(Type::regExp(), Prelude(this->state).builtinType("RegexError"));
    return call;
  }
  if (member->name != "escape") {
    this->checkRegExpStaticMember(member);
    for (Expr *&arg : call->args) {
      arg = ExprChecker(this->state).checkExpr(arg);
    }
    return call;
  }
  bool rejected = CallChecker(this->state).rejectSpread(call);
  ArgumentChecker::ResultArityMatches arity =
      rejected ? ok(call) : ArgumentChecker(this->state).checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
  }
  if (rejected || arity.isErr()) {
    return call;
  }
  call->args[0] = ArgumentChecker(this->state).checkArg(call->args[0], Type::string());
  call->target = BuiltinCall{.builtin = Builtin::RegExpEscape};
  call->type = Type::string();

  return call;
}

Expr *RegexChecker::checkRegExpStaticMember(MemberExpr *member) {
  member->type = Type::error();
  llvm::StringRef name = member->name;
  if (name == "escape") {
    this->state.getDiagnostics().error(
        member->loc(), "'RegExp.escape' can be used as a value only where a function "
                       "type is expected, e.g. as a callback");
  } else if (name.starts_with("$") || name == "input" || name == "lastMatch" ||
             name == "lastParen" || name == "leftContext" || name == "rightContext") {
    this->state.getDiagnostics().error(
        member->loc(), "'RegExp." + name +
                           "' is a legacy property that holds what the last match of any regex "
                           "found; read it from the match itself, e.g. 'm[1]'");
  } else {
    this->state.getDiagnostics().error(member->loc(),
                                       "property '" + name + "' does not exist on 'RegExp'");
  }

  return member;
}

}
