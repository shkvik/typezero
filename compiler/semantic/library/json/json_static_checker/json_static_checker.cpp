#include "compiler/semantic/library/json/json_static_checker/json_static_checker.h"
#include "compiler/packages/json_text/json_text.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/json/json_static_table/json_static_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/declare/prelude/prelude.h"
#include "compiler/semantic/library/bytes/buffer_static_checker/buffer_static_checker.h"
#include "compiler/semantic/library/json/json_checker/json_checker.h"
#include "compiler/semantic/library/reflect/reflect_checker/reflect_checker.h"
#include "compiler/semantic/library/tz_modules/library_calls/library_calls.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_parts/type_routes.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

JsonStaticChecker::JsonStaticChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->prelude = &all.prelude;
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->libraryCalls = &all.libraryCalls;
  this->typeRelations = &all.typeRelations;
  this->argumentChecker = &all.argumentChecker;
  this->bufferStaticChecker = &all.bufferStaticChecker;
}

bool JsonStaticChecker::mayHoldItself(const Type *type) {
  std::vector<const Type *> open;
  std::vector<HoldStep> steps = {EnterHold{type}};
  while (!steps.empty()) {
    HoldStep step = steps.back();
    steps.pop_back();
    if (step.is<LeaveHold>()) {
      open.pop_back();
    } else if (JsonStaticChecker::enterHold(step.as<EnterHold>()->type, open, steps)) {
      return true;
    }
  }

  return false;
}

bool JsonStaticChecker::enterHold(const Type *type, std::vector<const Type *> &open,
                                  std::vector<HoldStep> &steps) {
  const Type *plain = type->nonOptional();
  if (plain->isParam() || plain->isAnyObject()) {
    return false;
  }
  if (plain->isJson() || (plain->isBase() && !plain->classDecl()) || plain->isUnknown() ||
      llvm::is_contained(open, plain)) {
    return true;
  }
  open.push_back(plain);
  steps.push_back(LeaveHold{});
  std::vector<const Type *> parts = JsonStaticChecker::heldParts(plain);
  for (const Type *part : llvm::reverse(parts)) {
    steps.push_back(EnterHold{part});
  }

  return false;
}

std::vector<const Type *> JsonStaticChecker::heldParts(const Type *type) {
  if (const ClassDecl *cls = type->classDecl()) {
    return JsonStaticChecker::familyFields(cls);
  }
  if (type->isArray()) {
    return {type->inner()};
  }
  if (type->isResult()) {
    return {type->okType(), type->errType()};
  }
  if (type->isDict()) {
    return {type->valueType()};
  }
  if (type->isObject()) {
    return JsonStaticChecker::fieldTypes(type);
  }
  if (type->isUnion()) {
    return std::vector<const Type *>(type->members().begin(), type->members().end());
  }

  return {};
}

std::vector<const Type *> JsonStaticChecker::familyFields(const ClassDecl *cls) {
  std::vector<const ClassDecl *> family{cls};
  for (size_t i = 0; i < family.size(); ++i) {
    family.insert(family.end(), family[i]->children.begin(), family[i]->children.end());
  }
  std::vector<const Type *> parts;
  for (const ClassDecl *member : family) {
    if (member->type) {
      std::vector<const Type *> fields = JsonStaticChecker::fieldTypes(member->type);
      parts.insert(parts.end(), fields.begin(), fields.end());
    }
  }

  return parts;
}

std::vector<const Type *> JsonStaticChecker::fieldTypes(const Type *type) {
  std::vector<const Type *> types;
  for (const Field &f : type->fields()) {
    types.push_back(f.type);
  }

  return types;
}

const ClassDecl *JsonStaticChecker::classWithToJson(const Type *type) {
  TypeParts parts = TypeParts::collect(type, kToJsonRoute);
  for (const Type *part : parts.getTypes()) {
    const ClassDecl *cls = part->classDecl();
    if (cls != nullptr && JsonStaticChecker::hasToJson(cls)) {
      return cls;
    }
  }

  return nullptr;
}

bool JsonStaticChecker::hasToJson(const ClassDecl *cls) {
  for (const FunctionDecl *fn : cls->methods) {
    if (fn->name == "toJSON" && !fn->isStatic()) {
      return true;
    }
  }

  return false;
}

bool JsonStaticChecker::mayStringifyToUndefined(const Type *type) {
  return type->isUndefined() || type->isOptional() || type->isFunction() || type->isNever() ||
         type->isUnknown();
}

Expr *JsonStaticChecker::checkJsonStatic(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }
  std::optional<JsonStatic> fn = JsonStaticTable::find(member->name);
  if (fn == JsonStatic::PARSE) {
    return this->checkJsonParse(call, member);
  }
  if (!member->typeArgs.empty()) {
    std::string message = ErrorTextJsonStatic::typeArgs(member->name);
    this->state.getDiagnostics().error(member->loc(), message);
  }
  if (fn != JsonStatic::STRINGIFY) {
    std::string message = ErrorTextJsonStatic::noMember(member->name);
    this->state.getDiagnostics().error(member->loc(), message);
    this->walkArgsLoosely(call);
    return call;
  }

  return this->checkStringify(call, member);
}

void JsonStaticChecker::walkArgsLoosely(CallExpr *call) {
  for (Expr *&arg : call->args) {
    if (!JsonChecker::isNullName(arg)) {
      arg = this->exprChecker->checkExpr(arg);
    }
  }
}

Expr *JsonStaticChecker::checkJsonParse(CallExpr *call, MemberExpr *member) {
  if (call->args.size() == 2) {
    std::string message = ErrorTextJsonStatic::reviver();
    this->state.getDiagnostics().error(call->args[1]->loc(), message);
    this->walkArgsLoosely(call);
    return call;
  }
  if (!member->typeArgs.empty()) {
    std::string message = ErrorTextJsonStatic::parseTypes();
    this->state.getDiagnostics().error(member->loc(), message);
    this->walkArgsLoosely(call);
    return call;
  }
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  call->args[0] = this->argumentChecker->checkArg(call->args[0], Type::string());
  this->walkConstantJson(call->args[0]);
  call->target = BuiltinCall{.builtin = Builtin::JsonParse};
  call->type = Type::result(Type::json(), this->prelude->builtinType("JsonError"));

  return call;
}

Expr *JsonStaticChecker::checkStringify(CallExpr *call, MemberExpr *member) {
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 3);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  call->args[0] = this->exprChecker->checkExpr(call->args[0]);
  Expr *value = call->args[0];
  bool isOk = this->stringifiedValueFits(value);
  std::vector<Expr *> args{value};
  isOk &= this->stringifyReplacerFits(call);
  isOk &= this->stringifySpaceFits(call, args);
  if (!isOk) {
    return call;
  }
  call->args = std::move(args);
  call->target = BuiltinCall{.builtin = Builtin::JsonStringify};
  call->type = JsonStaticChecker::mayStringifyToUndefined(value->type)
                   ? Type::optional(Type::string())
                   : Type::string();
  bool isCyclic =
      !this->libraryCalls->inLibrary(call->loc()) && JsonStaticChecker::mayHoldItself(value->type);
  if (isCyclic) {
    call->type = Type::result(call->type, this->prelude->builtinType("JsonError"));
  }

  return call;
}

bool JsonStaticChecker::stringifiedValueFits(const Expr *value) {
  bool isOk = !value->type->isError();
  if (value->type->isVoid()) {
    std::string message = ErrorTextJsonStatic::voidValue();
    this->state.getDiagnostics().error(value->loc(), message);
    isOk = false;
  }
  const ClassDecl *cls = isOk ? JsonStaticChecker::classWithToJson(value->type) : nullptr;
  if (cls != nullptr) {
    std::string message = ErrorTextJsonStatic::toJson(cls->name);
    this->state.getDiagnostics().error(value->loc(), message);
    isOk = false;
  }

  return isOk;
}

bool JsonStaticChecker::stringifyReplacerFits(CallExpr *call) {
  if (call->args.size() < 2) {
    return true;
  }
  Expr *replacer = call->args[1];
  if (JsonChecker::isNullName(replacer) || isa<UndefinedExpr>(replacer)) {
    return true;
  }
  replacer = this->exprChecker->checkExpr(replacer);
  if (!replacer->type->isError()) {
    std::string message = ErrorTextJsonStatic::replacer();
    this->state.getDiagnostics().error(replacer->loc(), message);
  }

  return false;
}

bool JsonStaticChecker::stringifySpaceFits(CallExpr *call, std::vector<Expr *> &args) {
  if (call->args.size() != 3) {
    return true;
  }
  Expr *space = call->args[2];
  if (JsonChecker::isNullName(space) || isa<UndefinedExpr>(space)) {
    return true;
  }
  space = this->exprChecker->checkExpr(space);
  const Type *type = space->type;
  if (type->isNumeric()) {
    args.push_back(this->typeRelations->convert(space, Type::doubleTy()));
    return true;
  }
  if (type->isString()) {
    args.push_back(space);
    return true;
  }
  if (type->isError()) {
    return true;
  }
  std::string message = ErrorTextJsonStatic::space(type->name());
  this->state.getDiagnostics().error(space->loc(), message);

  return false;
}

bool JsonStaticChecker::readEmbedded(CallExpr *call, llvm::StringRef fn, std::string &bytes,
                                     std::string &shown, std::string &full) {
  Expr *arg = call->args[0];
  std::string example = ErrorTextEmbed::example(fn);
  std::u16string path;
  if (auto *lit = dyn_cast<StringLitExpr>(arg)) {
    path = lit->value;
  } else if (auto *tmpl = dyn_cast<TemplateExpr>(arg); tmpl && tmpl->exprs.empty()) {
    path = tmpl->quasis[0];
  } else {
    call->args[0] = this->exprChecker->checkExpr(arg);
    std::string message = ErrorTextEmbed::literal(fn, example);
    this->state.getDiagnostics().error(arg->loc(), message);
    return false;
  }
  shown = ToUtf8::toUtf8(path);
  ResultEmbeddedPath relative = JsonStaticChecker::embeddedPath(fn, shown);
  if (relative.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), relative.error().message);
    return false;
  }
  llvm::SourceMgr &sources = this->state.getDiagnostics().sourceMgr();
  unsigned id = sources.FindBufferContainingLoc(call->loc());
  llvm::StringRef source = id ? sources.getMemoryBuffer(id)->getBufferIdentifier() : "";
  llvm::SmallString<256> path8(llvm::sys::path::parent_path(source));
  llvm::sys::path::append(path8, shown);
  llvm::sys::path::remove_dots(path8, true);
  llvm::sys::path::native(path8);
  full = std::string(path8);
  auto buffer = llvm::MemoryBuffer::getFile(full, false, false);
  if (!buffer) {
    std::string why = buffer.getError().message();
    std::string message = ErrorTextEmbed::unread(shown, why);
    this->state.getDiagnostics().error(arg->loc(), message);
    return false;
  }
  bytes = (*buffer)->getBuffer().str();

  return true;
}

void JsonStaticChecker::walkConstantJson(const Expr *arg) {
  while (auto *conv = llvm::dyn_cast<CastExpr>(arg)) {
    if (!conv->isImplicit()) {
      return;
    }
    arg = conv->operand;
  }
  auto *lit = llvm::dyn_cast<StringLitExpr>(arg);
  if (!lit) {
    return;
  }
  std::string text = ToUtf8::toUtf8(lit->value);
  ResultConstantJson json = JsonStaticChecker::constantJson(text);
  if (json.isErr()) {
    this->state.getDiagnostics().error(lit->loc(), json.error().message);
  }
}

JsonStaticChecker::ResultEmbeddedPath JsonStaticChecker::embeddedPath(llvm::StringRef fn,
                                                                      llvm::StringRef shown) {
  if (shown.contains('\\')) {
    std::string message = ErrorTextEmbed::backslash();

    return err(JsonStaticCheckerEmbeddedPathError{
        .code = JsonStaticCheckerEmbeddedPathErrorCode::BACKSLASH,
        .message = message,
    });
  }
  if (shown.empty() || llvm::sys::path::is_absolute(shown) || shown[0] == '/') {
    std::string example = ErrorTextEmbed::example(fn);
    std::string message = ErrorTextEmbed::relative(fn, example);

    return err(JsonStaticCheckerEmbeddedPathError{
        .code = JsonStaticCheckerEmbeddedPathErrorCode::NOT_RELATIVE,
        .message = message,
    });
  }

  return ok(shown);
}

JsonStaticChecker::ResultConstantJson JsonStaticChecker::constantJson(llvm::StringRef text) {
  if (text.starts_with("\xEF\xBB\xBF")) {
    std::string message = ErrorTextConstantJson::bom();

    return err(JsonStaticCheckerConstantJsonError{
        .code = JsonStaticCheckerConstantJsonErrorCode::BYTE_ORDER_MARK,
        .message = message,
    });
  }
  pkg::JsonProblem problem = pkg::JsonText::checkJsonText(text);
  if (problem.message.empty()) {
    return ok(text);
  }
  size_t line = 1;
  size_t column = 1;
  for (size_t i = 0; i < problem.offset && i < text.size(); ++i) {
    if (text[i] == '\n') {
      ++line;
      column = 1;
    } else if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
      ++column;
    }
  }
  std::string message = ErrorTextConstantJson::invalid(problem.message, line, column);

  return err(JsonStaticCheckerConstantJsonError{
      .code = JsonStaticCheckerConstantJsonErrorCode::INVALID,
      .message = message,
  });
}

Expr *JsonStaticChecker::checkJsonGlobal(CallExpr *call, NameExpr *name) {
  bool isEmbedingText = this->callChecker->isGlobalName(name, GlobalName::EMBED_TEXT);
  bool isEmbedingBytes = this->callChecker->isGlobalName(name, GlobalName::EMBED_BYTES);
  if (!isEmbedingText && !isEmbedingBytes) {
    return nullptr;
  }
  llvm::StringRef fn = name->name;
  call->type = Type::error();
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
  };
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }

  if (!name->typeArgs.empty()) {
    std::string message = ErrorTextEmbed::typeArgs(fn);
    this->state.getDiagnostics().error(name->loc(), message);
  }

  if (isEmbedingBytes) {
    return this->bufferStaticChecker->checkEmbedBytes(call, name);
  }
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(name->loc(), arity.error().message);
    return call;
  }
  std::string bytes, shown, full;
  if (!this->readEmbedded(call, fn, bytes, shown, full)) {
    return call;
  }
  llvm::StringRef text(bytes);
  llvm::SmallVector<llvm::UTF16, 64> units;
  if (!llvm::convertUTF8ToUTF16String(text, units)) {
    std::string message = ErrorTextEmbed::notUtf8(shown);
    this->state.getDiagnostics().error(call->args[0]->loc(), message);
    return call;
  }
  std::u16string content(units.begin(), units.end());

  auto *lit = this->state.getAst().make<StringLitExpr>(call->loc(), std::move(content));
  lit->type = Type::string();

  return lit;
}

}
