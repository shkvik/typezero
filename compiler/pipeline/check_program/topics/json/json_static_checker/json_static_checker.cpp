#include "compiler/pipeline/check_program/topics/json/json_static_checker/json_static_checker.h"
#include "compiler/packages/json_text/json_text.h"
#include "compiler/pipeline/check_program/declare/prelude/prelude.h"
#include "compiler/pipeline/check_program/topics/bytes/buffer_static_checker/buffer_static_checker.h"
#include "compiler/pipeline/check_program/topics/expr/argument_checker/argument_checker.h"
#include "compiler/pipeline/check_program/topics/expr/call_checker/call_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "compiler/pipeline/check_program/topics/json/json_checker/json_checker.h"
#include "compiler/pipeline/check_program/topics/reflect/reflect_checker/reflect_checker.h"
#include "compiler/pipeline/check_program/topics/tz_modules/library_calls/library_calls.h"
#include "compiler/pipeline/check_program/types/type_relations/type_relations.h"
#include "compiler/pipeline/check_program/types/type_resolver/type_resolver.h"
#include "compiler/pipeline/check_program/whole_program/flows/flows.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

std::string JsonStaticChecker::toUtf8(const std::u16string &text) {
  std::string utf8;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      utf8);

  return utf8;
}

bool JsonStaticChecker::mayHoldItself(const Type *type, std::vector<const Type *> &open) {
  type = type->nonOptional();
  if (type->isParam() || type->isAnyObject()) {
    return false;
  }
  if (type->isJson() || (type->isBase() && !type->classDecl()) || type->isUnknown() ||
      llvm::is_contained(open, type)) {
    return true;
  }
  open.push_back(type);
  bool may = false;
  auto fieldsHold = [&](const Type *t) {
    return llvm::any_of(t->fields(), [&](const Field &f) {
      return JsonStaticChecker::mayHoldItself(f.type, open);
    });
  };
  if (const ClassDecl *cls = type->classDecl()) {
    std::vector<const ClassDecl *> family{cls};
    for (size_t i = 0; i < family.size() && !may; ++i) {
      if (family[i]->type) {
        may = fieldsHold(family[i]->type);
      }
      family.insert(family.end(), family[i]->children.begin(), family[i]->children.end());
    }
  } else if (type->isArray()) {
    may = JsonStaticChecker::mayHoldItself(type->inner(), open);
  } else if (type->isResult()) {
    may = JsonStaticChecker::mayHoldItself(type->okType(), open) ||
          JsonStaticChecker::mayHoldItself(type->errType(), open);
  } else if (type->isDict()) {
    may = JsonStaticChecker::mayHoldItself(type->valueType(), open);
  } else if (type->isObject()) {
    may = fieldsHold(type);
  } else if (type->isUnion()) {
    may = llvm::any_of(type->members(),
                       [&](const Type *m) { return JsonStaticChecker::mayHoldItself(m, open); });
  }
  open.pop_back();

  return may;
}

const ClassDecl *JsonStaticChecker::classWithToJson(const Type *type,
                                                    llvm::SmallPtrSetImpl<const Type *> &seen) {
  while (type->isOptional() || type->isArray()) {
    type = type->inner();
  }
  if (type->isResult()) {
    if (const ClassDecl *cls = JsonStaticChecker::classWithToJson(type->okType(), seen)) {
      return cls;
    }
    return JsonStaticChecker::classWithToJson(type->errType(), seen);
  }
  if (type->isRecord()) {
    return JsonStaticChecker::classWithToJson(type->valueType(), seen);
  }
  if (!type->isObject() || !seen.insert(type).second) {
    return nullptr;
  }
  if (const ClassDecl *cls = type->classDecl()) {
    for (const FunctionDecl *fn : cls->methods) {
      if (fn->name == "toJSON" && !fn->isStatic()) {
        return cls;
      }
    }
  }
  for (const Field &f : type->fields()) {
    if (const ClassDecl *cls = JsonStaticChecker::classWithToJson(f.type, seen)) {
      return cls;
    }
  }

  return nullptr;
}

bool JsonStaticChecker::mayStringifyToUndefined(const Type *type) {
  return type->isUndefined() || type->isOptional() || type->isFunction() || type->isNever() ||
         type->isUnknown();
}

JsonStaticChecker::JsonStaticChecker(CheckState &owner)
    : state(owner) {}

Expr *JsonStaticChecker::checkJsonStatic(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      if (!JsonChecker::isNullName(arg)) {
        arg = ExprChecker(this->state).checkExpr(arg);
      }
    }
  };
  if (CallChecker(this->state).rejectSpread(call)) {
    return call;
  }
  llvm::StringRef fn = member->name;
  if (fn == "parse") {
    if (call->args.size() == 2) {
      this->state.getDiagnostics().error(call->args[1]->loc(),
                                         "the reviver of JSON.parse is not supported");
      checkArgsLoosely();
      return call;
    }
    if (!member->typeArgs.empty()) {
      this->state.getDiagnostics().error(member->loc(), "JSON.parse takes no type arguments");
      checkArgsLoosely();
      return call;
    }
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    call->args[0] = ArgumentChecker(this->state).checkArg(call->args[0], Type::string());
    this->checkConstantJson(call->args[0]);
    call->target = BuiltinCall{.builtin = Builtin::JsonParse};
    call->type = Type::result(Type::json(), Prelude(this->state).builtinType("JsonError"));
    return call;
  }
  if (!member->typeArgs.empty()) {
    this->state.getDiagnostics().error(member->loc(), "'JSON." + fn + "' takes no type arguments");
  }
  if (fn != "stringify") {
    this->state.getDiagnostics().error(
        member->loc(), "'JSON." + fn + "' does not exist; JSON has parse and stringify");
    checkArgsLoosely();
    return call;
  }
  ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 3);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  Expr *value = call->args[0] = ExprChecker(this->state).checkExpr(call->args[0]);
  bool ok = !value->type->isError();
  if (value->type->isVoid()) {
    this->state.getDiagnostics().error(
        value->loc(), "an expression of type 'void' has no value to write as JSON");
    ok = false;
  }
  llvm::SmallPtrSet<const Type *, 8> seen;
  if (const ClassDecl *cls = ok ? JsonStaticChecker::classWithToJson(value->type, seen) : nullptr) {
    this->state.getDiagnostics().error(
        value->loc(), "JSON.stringify does not call the method toJSON of class '" + cls->name +
                          "': tz calls no code by itself; call it, e.g. "
                          "'JSON.stringify(x.toJSON())'");
    ok = false;
  }
  std::vector<Expr *> args{value};
  if (call->args.size() >= 2) {
    Expr *replacer = call->args[1];
    if (!JsonChecker::isNullName(replacer) && !isa<UndefinedExpr>(replacer)) {
      replacer = ExprChecker(this->state).checkExpr(replacer);
      if (!replacer->type->isError()) {
        this->state.getDiagnostics().error(
            replacer->loc(), "a replacer is not supported: tz calls no code by itself; "
                             "build the value to write first, and pass 'null' here");
      }
      ok = false;
    }
  }
  if (call->args.size() == 3) {
    Expr *space = call->args[2];
    if (!JsonChecker::isNullName(space) && !isa<UndefinedExpr>(space)) {
      space = ExprChecker(this->state).checkExpr(space);
      const Type *type = space->type;
      if (type->isNumeric()) {
        args.push_back(TypeRelations(this->state).convert(space, Type::doubleTy()));
      } else if (type->isString()) {
        args.push_back(space);
      } else if (!type->isError()) {
        this->state.getDiagnostics().error(space->loc(),
                                           "the space of JSON.stringify is a number of spaces or a "
                                           "string, found '" +
                                               type->name() + "'");
        ok = false;
      }
    }
  }
  if (!ok) {
    return call;
  }
  call->args = std::move(args);
  call->target = BuiltinCall{.builtin = Builtin::JsonStringify};
  call->type = JsonStaticChecker::mayStringifyToUndefined(value->type)
                   ? Type::optional(Type::string())
                   : Type::string();
  std::vector<const Type *> open;
  if (!LibraryCalls(this->state).inLibrary(call->loc()) &&
      JsonStaticChecker::mayHoldItself(value->type, open)) {
    call->type = Type::result(call->type, Prelude(this->state).builtinType("JsonError"));
  }

  return call;
}

bool JsonStaticChecker::readEmbedded(CallExpr *call, llvm::StringRef fn, std::string &bytes,
                                     std::string &shown, std::string &full) {
  Expr *arg = call->args[0];
  std::string example =
      (fn + (fn == "embedText" ? "(\"./page.html\")" : "(\"./data.json\")")).str();
  std::u16string path;
  if (auto *lit = dyn_cast<StringLitExpr>(arg)) {
    path = lit->value;
  } else if (auto *tmpl = dyn_cast<TemplateExpr>(arg); tmpl && tmpl->exprs.empty()) {
    path = tmpl->quasis[0];
  } else {
    call->args[0] = ExprChecker(this->state).checkExpr(arg);
    this->state.getDiagnostics().error(arg->loc(),
                                       "the path of " + fn +
                                           " must be a string literal: the file is read "
                                           "when the program is compiled, e.g. '" +
                                           example + "'");
    return false;
  }
  shown = JsonStaticChecker::toUtf8(path);
  if (llvm::StringRef(shown).contains('\\')) {
    this->state.getDiagnostics().error(arg->loc(),
                                       "write '/' in the path, not '\\', as on every system");
    return false;
  }
  if (shown.empty() || llvm::sys::path::is_absolute(shown) || shown[0] == '/') {
    this->state.getDiagnostics().error(arg->loc(), "the path of " + fn +
                                                       " is relative to the file it is written in, "
                                                       "e.g. '" +
                                                       example + "'");
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
    this->state.getDiagnostics().error(arg->loc(), "cannot read '" + shown +
                                                       "': " + buffer.getError().message());
    return false;
  }
  bytes = (*buffer)->getBuffer().str();

  return true;
}

void JsonStaticChecker::checkConstantJson(const Expr *arg) {
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
  std::string text;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(lit->value.data()),
                                  lit->value.size()),
      text);
  if (llvm::StringRef(text).starts_with("\xEF\xBB\xBF")) {
    this->state.getDiagnostics().error(lit->loc(),
                                       "this text is not JSON, so JSON.parse would always fail: it "
                                       "starts with a byte order mark (BOM)");
    return;
  }
  pkg::JsonProblem problem = pkg::checkJsonText(text);
  if (problem.message.empty()) {
    return;
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
  this->state.getDiagnostics().error(
      lit->loc(), "this text is not JSON, so JSON.parse would always fail: " + problem.message +
                      " (line " + std::to_string(line) + ", column " + std::to_string(column) +
                      ")");
}

Expr *JsonStaticChecker::checkJsonGlobal(CallExpr *call, NameExpr *name) {
  static const llvm::StringLiteral names[] = {"embedText", "embedBytes"};
  if (!llvm::any_of(names, [&](llvm::StringRef n) {
        return CallChecker(this->state).isGlobalName(name, n);
      })) {
    return nullptr;
  }
  llvm::StringRef fn = name->name;
  call->type = Type::error();
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = ExprChecker(this->state).checkExpr(arg);
    }
  };
  if (CallChecker(this->state).rejectSpread(call)) {
    return call;
  }

  if (!name->typeArgs.empty()) {
    this->state.getDiagnostics().error(name->loc(), "'" + fn + "' takes no type arguments");
  }

  if (fn == "embedBytes") {
    return BufferStaticChecker(this->state).checkEmbedBytes(call, name);
  }
  ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 1);
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
    this->state.getDiagnostics().error(call->args[0]->loc(),
                                       "'" + shown + "' is not text in UTF-8");
    return call;
  }
  std::u16string content(units.begin(), units.end());

  auto *lit = this->state.getAst().make<StringLitExpr>(call->loc(), std::move(content));
  lit->type = Type::string();

  return lit;
}

}
