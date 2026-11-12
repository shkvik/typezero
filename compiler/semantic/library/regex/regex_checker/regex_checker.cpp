#include "compiler/semantic/library/regex/regex_checker/regex_checker.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/objects/object_checker/object_checker.h"
#include "compiler/semantic/bodies/stmt/var_checker/var_checker.h"
#include "compiler/semantic/declare/prelude/prelude.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/flow/placed_nodes/placed_nodes.h"
#include "compiler/semantic/library/process/process_checker/process_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "runtime/library/regex/regex/regex.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <cstring>

using llvm::dyn_cast;

namespace tz {

RegexChecker::RegexChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->prelude = &all.prelude;
  this->varChecker = &all.varChecker;
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->objectChecker = &all.objectChecker;
  this->typeRelations = &all.typeRelations;
  this->processChecker = &all.processChecker;
  this->argumentChecker = &all.argumentChecker;
}

std::u16string RegexChecker::units(const uint32_t *program, uint32_t start, uint32_t length) const {
  auto text = std::u16string();
  for (uint32_t i = 0; i < length; ++i) {
    text += static_cast<char16_t>(tz_re_name_unit(program, start, i));
  }

  return text;
}

bool RegexChecker::isFlag(llvm::StringRef name, uint32_t &flag) const {
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

RegexChecker::ResultParsedFlags RegexChecker::parsedFlags(const std::u16string &flags) const {
  std::vector<uint16_t> flagUnits(flags.begin(), flags.end());
  uint32_t parsed = 0;
  if (!tz_re_parse_flags(flagUnits.data(), flagUnits.size(), &parsed)) {
    auto written = ToUtf8::toUtf8(flags);
    auto message = ErrorTextRegexFlags::invalid(written);

    return err(RegexCheckerParsedFlagsError{
        .code = RegexCheckerParsedFlagsErrorCode::INVALID_FLAGS,
        .message = message,
    });
  }

  return ok(parsed);
}

Expr *RegexChecker::checkRegexLit(RegexLitExpr *literal) {
  literal->type = Type::regExp();
  llvm::StringRef source(literal->loc().getPointer(), literal->written);
  auto parsed = this->parsedFlags(literal->flags);
  if (parsed.isErr()) {
    llvm::SMLoc at =
        literal->written
            ? llvm::SMLoc::getFromPointer(source.end() - ToUtf8::toUtf8(literal->flags).size())
            : literal->loc();
    this->state.getDiagnostics().error(at, parsed.error().message);
    return literal;
  }
  uint32_t flags = parsed.value();
  std::vector<uint16_t> pattern(literal->pattern.begin(), literal->pattern.end());
  const char *error = nullptr;
  size_t errorAt = 0;
  uint32_t *program =
      tz_re_compile(pattern.data(), pattern.size(), flags | TZ_RE_STRICT, &error, &errorAt);
  if (program == nullptr) {
    llvm::SMLoc at = literal->loc();
    if (literal->written) {
      const char *p = source.begin() + 1;
      for (size_t units = 0; units < errorAt && p < source.end();) {
        unsigned char lead = static_cast<unsigned char>(*p);
        size_t size = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        units += size == 4 ? 2 : 1;
        p += size;
      }
      at = llvm::SMLoc::getFromPointer(p);
    }
    auto message = ErrorTextRegexLit::invalid(error);
    this->state.getDiagnostics().error(at, message);
    return literal;
  }
  literal->program.assign(program, program + program[TZ_RE_H_SIZE]);
  tz_re_free(program);

  return literal;
}

const RegexLitExpr *RegexChecker::knownRegex(const Expr *expr) const {
  expr = ExprParts::withoutImplicitCasts(expr);
  auto *literal = dyn_cast<RegexLitExpr>(expr);
  if (literal != nullptr) {
    return literal->program.empty() ? nullptr : literal;
  }
  auto *name = dyn_cast<NameExpr>(expr);
  if (name != nullptr && name->var() != nullptr) {
    return name->var()->regex();
  }

  return nullptr;
}

void RegexChecker::noteRegexVariable(VarSymbol *variable, const Expr *init, bool isConst,
                                     llvm::ArrayRef<Stmt *> block, size_t index) {
  auto *literal = llvm::dyn_cast_or_null<RegexLitExpr>(init);
  if (literal == nullptr || literal->program.empty()) {
    return;
  }
  if (!isConst) {
    if (block.empty()) {
      return;
    }
    auto following = this->varChecker->followingStatements(block, index);
    bool isAssigned = this->assignsName(following, variable->name);
    if (isAssigned) {
      return;
    }
  }
  variable->holds = RegexLiteral{.regex = literal};
}

bool RegexChecker::assignsName(llvm::ArrayRef<const Stmt *> stmts, llvm::StringRef name) const {
  auto nodes = PlacedNodes::collect(stmts, {});
  for (const PlacedExpr &placed : nodes.getExprs()) {
    auto *assign = dyn_cast<AssignExpr>(placed.expr);
    auto *target = assign != nullptr ? dyn_cast<NameExpr>(assign->target) : nullptr;
    if (target != nullptr && target->name == name) {
      return true;
    }
  }

  return false;
}

const Type *RegexChecker::matchType(const RegexLitExpr *regex) {
  if (regex == nullptr) {
    return Type::match(-1, {}, nullptr, false);
  }
  const uint32_t *program = regex->program.data();
  uint32_t captures = tz_re_captures(program);
  std::vector<bool> mandatory(captures);
  for (uint32_t k = 0; k < captures; ++k) {
    mandatory[k] = tz_re_mandatory(program, k);
  }
  auto fields = std::vector<Field>();
  for (uint32_t k = 1; k < captures; ++k) {
    uint32_t start, length = tz_re_name(program, k, &start);
    if (!length) {
      continue;
    }
    auto name = ToUtf8::toUtf8(this->units(program, start, length));
    if (llvm::any_of(fields, [&](const Field &field) { return field.name == name; })) {
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

  return this->processChecker->builtinCall(regex->loc(), callee, Builtin::RegExpToString, {regex},
                                           Type::string());
}

Expr *RegexChecker::checkRegexMethod(CallExpr *call, MemberExpr *member) {
  llvm::StringRef name = member->name;
  const auto *regex = this->knownRegex(member->object);
  call->regex = regex;
  call->type = Type::error();
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }
  if (name == "test" || name == "exec") {
    auto arity = this->argumentChecker->checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    call->args[0] = this->argumentChecker->checkArg(call->args[0], Type::string());
    uint32_t flags = regex != nullptr ? tz_re_flags(regex->program.data()) : 0;
    if (name == "test" && (flags & TZ_RE_GLOBAL)) {
      auto message = ErrorTextRegexMethod::testG();
      this->state.getDiagnostics().error(member->loc(), message);
      return call;
    }
    call->target =
        BuiltinCall{.builtin = name == "test" ? Builtin::RegExpTest : Builtin::RegExpExec};
    call->type = name == "test" ? Type::boolTy() : Type::optional(this->matchType(regex));
    return call;
  }
  if (name == "toString") {
    auto arity = this->argumentChecker->checkArity(call, 0, 0);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    call->args.push_back(this->regexToString(member->object));
    call->target = BuiltinCall{.builtin = Builtin::StringOf};
    call->type = Type::string();
    return call;
  }
  std::string message =
      name == "compile" ? ErrorTextRegexMethod::compile() : ErrorTextRegexMethod::noMember(name);
  this->state.getDiagnostics().error(member->loc(), message);
  for (Expr *&arg : call->args) {
    arg = this->exprChecker->checkExpr(arg);
  }

  return call;
}

Expr *RegexChecker::checkRegexMember(MemberExpr *member) {
  llvm::StringRef name = member->name;
  if (name == "lastIndex") {
    auto field = this->objectChecker->checkField(member);
    if (field.isErr()) {
      this->state.getDiagnostics().error(member->loc(), field.error().message);
      return member;
    }
    return field.value();
  }
  if (name == "source") {
    return this->processChecker->builtinCall(member->loc(), member, Builtin::RegExpSource, {},
                                             Type::string());
  }
  if (name == "flags") {
    return this->processChecker->builtinCall(member->loc(), member, Builtin::RegExpFlags, {},
                                             Type::string());
  }
  uint32_t flag;
  if (this->isFlag(name, flag)) {
    auto *bit = this->state.getAst().make<IntLitExpr>(member->loc(), flag);
    bit->type = Type::intTy();
    return this->processChecker->builtinCall(member->loc(), member, Builtin::RegExpFlag, {bit},
                                             Type::boolTy());
  }
  auto message = ErrorTextRegexMethod::notCalled(name, name == "test" || name == "exec");
  this->state.getDiagnostics().error(member->loc(), message);
  member->type = Type::error();

  return member;
}

Expr *RegexChecker::checkRegExpConstructor(llvm::SMLoc loc, std::vector<Expr *> &args) {
  auto loosely = [&] {
    for (Expr *&arg : args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    auto *error = this->state.getAst().make<UndefinedExpr>(loc);
    error->type = Type::error();

    return error;
  };
  if (args.empty() || args.size() > 2) {
    auto message = ErrorTextRegExpNew::arity(args.size());
    this->state.getDiagnostics().error(loc, message);
    return loosely();
  }
  auto literal = [](const Expr *expr) -> std::optional<std::u16string> {
    if (auto *literalExpr = dyn_cast<StringLitExpr>(expr)) {
      return literalExpr->value;
    }
    auto *templateExpr = dyn_cast<TemplateExpr>(expr);
    if (templateExpr != nullptr && templateExpr->exprs.empty()) {
      return templateExpr->quasis[0];
    }

    return std::nullopt;
  };
  std::optional<std::u16string> pattern = literal(args[0]);
  std::optional<std::u16string> flags = args.size() > 1 ? literal(args[1]) : std::u16string();
  if (pattern && flags) {
    auto *literalExpr = this->state.getAst().make<RegexLitExpr>(loc, *pattern, *flags);
    return this->checkRegexLit(literalExpr);
  }
  auto *escaped = dyn_cast<CallExpr>(args[0]);
  auto *escape = escaped != nullptr ? dyn_cast<MemberExpr>(escaped->callee) : nullptr;
  bool isFromEscape = escape != nullptr && escape->name == "escape" &&
                      this->callChecker->isGlobalName(escape->object, GlobalName::REG_EXP);
  if (!isFromEscape || !flags) {
    auto message = ErrorTextRegExpNew::unknown(args.size() > 1);
    this->state.getDiagnostics().error(loc, message);
    return loosely();
  }
  auto parsed = this->parsedFlags(*flags);
  if (parsed.isErr()) {
    this->state.getDiagnostics().error(args[1]->loc(), parsed.error().message);
    return loosely();
  }
  args[0] = this->exprChecker->checkExpr(args[0]);
  args[0] = this->typeRelations->coerce(args[0], Type::string());
  if (args.size() > 1) {
    args[1] = this->argumentChecker->checkArg(args[1], Type::string());
  }

  return this->processChecker->builtinCall(loc, this->state.getAst().make<NameExpr>(loc, "RegExp"),
                                           Builtin::RegExpNew, args, Type::regExp());
}

Expr *RegexChecker::checkRegExpStatic(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  if (member->name == "parse") {
    bool isRejected = this->callChecker->rejectSpread(call);
    auto arity = isRejected ? ok(call) : this->argumentChecker->checkArity(call, 1, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
    }
    if (isRejected || arity.isErr()) {
      return call;
    }
    for (Expr *&arg : call->args) {
      arg = this->argumentChecker->checkArg(arg, Type::string());
    }
    call->target = BuiltinCall{.builtin = Builtin::RegExpParse};
    call->type = Type::result(Type::regExp(), this->prelude->builtinType(PreludeType::REGEX_ERROR));
    return call;
  }
  if (member->name != "escape") {
    this->checkRegExpStaticMember(member);
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    return call;
  }
  bool isRejected = this->callChecker->rejectSpread(call);
  auto arity = isRejected ? ok(call) : this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
  }
  if (isRejected || arity.isErr()) {
    return call;
  }
  call->args[0] = this->argumentChecker->checkArg(call->args[0], Type::string());
  call->target = BuiltinCall{.builtin = Builtin::RegExpEscape};
  call->type = Type::string();

  return call;
}

Expr *RegexChecker::checkRegExpStaticMember(MemberExpr *member) {
  member->type = Type::error();
  llvm::StringRef name = member->name;
  bool isLegacy = name.starts_with("$") || name == "input" || name == "lastMatch" ||
                  name == "lastParen" || name == "leftContext" || name == "rightContext";
  std::string message = name == "escape" ? ErrorTextRegExpStatic::escape()
                        : isLegacy       ? ErrorTextRegExpStatic::legacy(name)
                                         : ErrorTextRegExpStatic::noMember(name);
  this->state.getDiagnostics().error(member->loc(), message);

  return member;
}

}
