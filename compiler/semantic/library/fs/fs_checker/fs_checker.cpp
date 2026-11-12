#include "compiler/semantic/library/fs/fs_checker/fs_checker.h"
#include "compiler/program/to_utf16/to_utf16.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/declare/prelude/prelude.h"
#include "compiler/semantic/declare/type_declarer/type_declarer.h"
#include "compiler/semantic/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/semantic/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/semantic/library/fs/fs_errors/fs_errors.h"
#include "compiler/semantic/library/fs/fs_exports/fs_exports.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

FsChecker::FsChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->fsErrors = &all.fsErrors;
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->bytesChecker = &all.bytesChecker;
  this->typeDeclarer = &all.typeDeclarer;
  this->typeRelations = &all.typeRelations;
  this->argumentChecker = &all.argumentChecker;
  this->builtinModuleChecker = &all.builtinModuleChecker;
}

FsChecker::ResultOctalMode FsChecker::octalMode(llvm::StringRef text) const {
  unsigned mode = 0;
  if (text.empty() || text.getAsInteger(8, mode)) {
    auto message = ErrorTextFsCall::chmod();

    return err(FsCheckerOctalModeError{
        .code = FsCheckerOctalModeErrorCode::NOT_OCTAL,
        .message = message,
    });
  }

  return ok(mode);
}

bool FsChecker::nodeOptions(CallExpr *call, size_t index, llvm::StringRef functionName,
                            llvm::ArrayRef<llvm::StringRef> allowed,
                            llvm::StringMap<Expr *> &options) {
  if (index >= call->args.size() || isa<UndefinedExpr>(call->args[index])) {
    return true;
  }
  Expr *arg = call->args[index];
  auto *literal = dyn_cast<ObjectLitExpr>(arg);
  if (literal == nullptr) {
    auto message = ErrorTextFsOptions::literal(functionName, allowed[0]);
    this->state.getDiagnostics().error(arg->loc(), message);
    call->args[index] = this->exprChecker->checkExpr(arg);
    return false;
  }
  for (const ObjectLitExpr::Property &property : literal->props) {
    if (property.spread() || property.key() || property.method() || property.name.empty()) {
      auto message = ErrorTextFsOptions::byName(functionName, allowed[0]);
      this->state.getDiagnostics().error(property.loc, message);
      return false;
    }
    if (!llvm::is_contained(allowed, property.name)) {
      auto message = ErrorTextFsOptions::unknown(functionName, property.name, allowed);
      this->state.getDiagnostics().error(property.loc, message);
      return false;
    }
    options[property.name] = property.value;
  }

  return true;
}

const Type *FsChecker::nativeError() const {
  auto *alias = this->state.getLibraryDeclarations().findLibraryType("NativeError");

  return alias != nullptr ? this->typeDeclarer->resolveAlias(alias) : Type::error();
}

void FsChecker::rememberNativeError() {
  const auto *native = this->nativeError();
  const Type *nativeError = native->isError() ? nullptr : native;
  this->state.getInventoryStore().rememberNativeError(nativeError);
}

Expr *FsChecker::checkEncoding(Expr *encoding, llvm::StringRef functionName, bool any) {
  auto *literal = dyn_cast<StringLitExpr>(encoding);
  if (literal == nullptr) {
    auto message = ErrorTextFsEncoding::literal(functionName);
    this->state.getDiagnostics().error(encoding->loc(), message);
    this->exprChecker->checkExpr(encoding);
    return nullptr;
  }
  literal->type = Type::string();
  auto *number = this->bytesChecker->checkBufferEncoding(literal);
  if (number->type->isError()) {
    return nullptr;
  }
  if (!any && cast<IntLitExpr>(number)->value != 0) {
    auto written = ToUtf8::toUtf8(literal->value);
    auto message = ErrorTextFsEncoding::utf8Only(functionName, written);
    this->state.getDiagnostics().error(encoding->loc(), message);

    return nullptr;
  }

  return number;
}

Expr *FsChecker::checkFsCall(CallExpr *call, llvm::StringRef functionName, llvm::SMLoc loc) {
  call->type = Type::error();
  std::string syncName = this->state.getFileSystemStore().isInsidePromiseCall()
                             ? (functionName + "Sync").str()
                             : functionName.str();
  auto function = FsFunctionTable::find(syncName);
  bool isValue = function == FsFunction::CONSTANTS || function == FsFunction::STATS ||
                 function == FsFunction::DIRENT;
  if (isValue) {
    return this->reportNotFunction(call, functionName, loc);
  }
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }
  if (!function.has_value()) {
    return this->reportUnknownFsFunction(call, functionName, loc);
  }

  FsCall fs{
      .call = call,
      .function = *function,
      .fn = functionName,
      .loc = loc,
  };
  switch (fs.function) {
    case FsFunction::CONSTANTS:
    case FsFunction::STATS:
    case FsFunction::DIRENT:
      return this->reportNotFunction(call, functionName, loc);
    case FsFunction::READ_FILE_SYNC:
      return this->checkReadFile(fs);
    case FsFunction::WRITE_FILE_SYNC:
    case FsFunction::APPEND_FILE_SYNC:
      return this->checkWriteFile(fs);
    case FsFunction::EXISTS_SYNC:
    case FsFunction::UNLINK_SYNC:
    case FsFunction::REALPATH_SYNC:
    case FsFunction::MKDTEMP_SYNC:
      return this->checkPathCall(fs);
    case FsFunction::ACCESS_SYNC:
    case FsFunction::CHMOD_SYNC:
    case FsFunction::TRUNCATE_SYNC:
      return this->checkPathWithArgument(fs);
    case FsFunction::STAT_SYNC:
    case FsFunction::LSTAT_SYNC:
      return this->checkStat(fs);
    case FsFunction::READDIR_SYNC:
      return this->checkReaddir(fs);
    case FsFunction::MKDIR_SYNC:
      return this->checkMkdir(fs);
    case FsFunction::RMDIR_SYNC:
      return this->checkRmdir(fs);
    case FsFunction::RM_SYNC:
      return this->checkRm(fs);
    case FsFunction::RENAME_SYNC:
    case FsFunction::COPY_FILE_SYNC:
    case FsFunction::REPLACE_SYNC:
      return this->checkTwoPaths(fs);
  }

  return call;
}

Expr *FsChecker::reportNotFunction(CallExpr *call, llvm::StringRef functionName, llvm::SMLoc loc) {
  auto message = ErrorTextFsCall::notFunc(functionName);
  this->state.getDiagnostics().error(loc, message);

  return this->checkLoosely(call);
}

Expr *FsChecker::reportUnknownFsFunction(CallExpr *call, llvm::StringRef functionName,
                                         llvm::SMLoc loc) {
  auto found = FsExports::fsExport(functionName, "node:fs");
  std::string message = found.isErr() ? found.error().message : std::string();
  this->state.getDiagnostics().error(loc, message);

  return this->checkLoosely(call);
}

Expr *FsChecker::checkLoosely(CallExpr *call) {
  for (Expr *&arg : call->args) {
    arg = this->exprChecker->checkExpr(arg);
  }

  return call;
}

BoolLitExpr *FsChecker::makeBoolLiteral(llvm::SMLoc loc, bool value) {
  auto *literal = this->state.getAst().make<BoolLitExpr>(loc, value);
  literal->type = Type::boolTy();

  return literal;
}

IntLitExpr *FsChecker::makeIntLiteral(llvm::SMLoc loc, uint64_t value) {
  auto *literal = this->state.getAst().make<IntLitExpr>(loc, value);
  literal->type = Type::intTy();

  return literal;
}

StringLitExpr *FsChecker::makeStringLiteral(llvm::SMLoc loc, llvm::StringRef text) {
  auto *literal = this->state.getAst().make<StringLitExpr>(loc, ToUtf16::toUtf16(text));
  literal->type = Type::string();

  return literal;
}

Expr *FsChecker::checkPath(Expr *arg, bool fd) {
  const auto *string = Type::string();
  auto *checked = this->exprChecker->checkExpr(arg, string);
  auto *literal = dyn_cast<StringLitExpr>(checked);
  if (literal != nullptr && literal->value.find(u'\0') != std::u16string::npos) {
    auto message = ErrorTextFsCall::zeroPath();
    this->state.getDiagnostics().error(checked->loc(), message);
  }
  bool isDescriptor =
      fd && !this->state.getFileSystemStore().isInsidePromiseCall() && checked->type->isInt();
  if (isDescriptor) {
    return checked;
  }

  return this->typeRelations->coerce(checked, string);
}

bool FsChecker::literalFlag(FsCall &fs, Expr *value, llvm::StringRef option, bool &out) {
  auto *literal = dyn_cast<BoolLitExpr>(value);
  if (literal != nullptr) {
    literal->type = Type::boolTy();
    out = literal->value;
    return true;
  }
  auto message = ErrorTextFsOptions::flag(fs.fn, option);
  this->state.getDiagnostics().error(value->loc(), message);
  this->exprChecker->checkExpr(value);

  return false;
}

Expr *FsChecker::openFlag(FsCall &fs, Expr *flag, llvm::StringRef fallback) {
  if (flag == nullptr) {
    return this->makeStringLiteral(fs.loc, fallback);
  }
  static constexpr llvm::StringLiteral flags[] = {
      "r",   "rs", "sr", "r+", "rs+", "sr+", "w",  "wx",  "xw",  "w+",  "wx+",
      "xw+", "a",  "ax", "xa", "as",  "sa",  "a+", "ax+", "xa+", "as+", "sa+"};
  auto *literal = dyn_cast<StringLitExpr>(flag);
  std::string written = literal != nullptr ? ToUtf8::toUtf8(literal->value) : std::string();
  if (literal != nullptr && llvm::is_contained(flags, llvm::StringRef(written))) {
    literal->type = Type::string();
    return literal;
  }
  this->exprChecker->checkExpr(flag);
  auto message = ErrorTextFsCall::flag(fs.fn, written, literal != nullptr);
  this->state.getDiagnostics().error(flag->loc(), message);

  return nullptr;
}

bool FsChecker::encodingOrOptions(FsCall &fs, size_t index,
                                  llvm::ArrayRef<llvm::StringRef> allowed) {
  CallExpr *call = fs.call;
  if (index >= call->args.size()) {
    return true;
  }
  Expr *arg = call->args[index];
  if (isa<StringLitExpr>(arg)) {
    fs.options["encoding"] = arg;
    return true;
  }
  if (isa<ObjectLitExpr>(arg) || isa<UndefinedExpr>(arg)) {
    return this->nodeOptions(call, index, fs.fn, allowed, fs.options);
  }
  auto *checked = this->exprChecker->checkExpr(arg);
  if (checked->type->isString()) {
    auto message = ErrorTextFsEncoding::dependsOn(fs.fn);
    this->state.getDiagnostics().error(checked->loc(), message);
  } else if (!checked->type->isError()) {
    auto message = ErrorTextFsOptions::literal(fs.fn, allowed[0]);
    this->state.getDiagnostics().error(checked->loc(), message);
  }

  return false;
}

Expr *FsChecker::checkReadFile(FsCall &fs) {
  CallExpr *call = fs.call;
  llvm::StringRef functionName = fs.fn;
  llvm::SMLoc loc = fs.loc;
  llvm::StringMap<Expr *> &options = fs.options;
  const auto *string = Type::string();
  auto arity = this->argumentChecker->checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  auto *file = this->checkPath(call->args[0], true);
  if (!this->encodingOrOptions(fs, 1, {"encoding", "flag"})) {
    return call;
  }
  Expr *encoding = options.lookup("encoding");
  Expr *number = nullptr;
  if (encoding != nullptr && !isa<UndefinedExpr>(encoding) &&
      !(number = this->checkEncoding(encoding, functionName, true))) {
    return call;
  }
  auto *flag = this->openFlag(fs, options.lookup("flag"), "r");
  if (flag == nullptr) {
    return call;
  }
  call->args = {file, flag};
  call->target = BuiltinCall{.builtin = Builtin::FsReadFile};
  call->type = number != nullptr ? string : Type::bytesType(Type::Bytes::Buffer);
  if (number != nullptr) {
    call->args.push_back(number);
  }
  return call;
}

Expr *FsChecker::checkWriteFile(FsCall &fs) {
  CallExpr *call = fs.call;
  llvm::StringRef functionName = fs.fn;
  llvm::SMLoc loc = fs.loc;
  llvm::StringMap<Expr *> &options = fs.options;
  const auto *string = Type::string();
  auto arity = this->argumentChecker->checkArity(call, 2, 3);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  auto *file = this->checkPath(call->args[0], true);
  auto *data = this->exprChecker->checkExpr(call->args[1], string);
  auto view = [](const Type *type) { return type->isByteView(); };
  bool hasBytes =
      view(data->type) || (data->type->isUnion() && llvm::all_of(data->type->members(), view));
  if (!data->type->isString() && !hasBytes && !data->type->isError()) {
    auto message = ErrorTextFsCall::data(functionName, data->type);
    this->state.getDiagnostics().error(data->loc(), message);
    return this->checkLoosely(call);
  }
  if (!this->encodingOrOptions(fs, 2, {"encoding", "flag", "mode"})) {
    return call;
  }
  Expr *number = nullptr;
  Expr *encoding = options.lookup("encoding");
  if (encoding != nullptr && !isa<UndefinedExpr>(encoding)) {
    if (hasBytes) {
      auto message = ErrorTextFsEncoding::forText(functionName);
      this->state.getDiagnostics().error(encoding->loc(), message);
      return call;
    }
    if (!(number = this->checkEncoding(encoding, functionName, true))) {
      return call;
    }
  }
  auto *flag = this->openFlag(fs, options.lookup("flag"),
                              fs.function == FsFunction::WRITE_FILE_SYNC ? "w" : "a");
  if (flag == nullptr) {
    return call;
  }
  Expr *mode = options.lookup("mode");
  call->args = {file, data, flag,
                mode != nullptr ? this->argumentChecker->checkArg(mode, Type::intTy())
                                : this->makeIntLiteral(loc, 0666)};
  if (!hasBytes) {
    call->args.push_back(number != nullptr ? number : this->makeIntLiteral(loc, 0));
  }
  call->target = BuiltinCall{.builtin = Builtin::FsWriteFile};
  call->type = Type::voidTy();
  return call;
}

Expr *FsChecker::checkPathCall(FsCall &fs) {
  CallExpr *call = fs.call;
  llvm::StringRef functionName = fs.fn;
  llvm::SMLoc loc = fs.loc;
  llvm::StringMap<Expr *> &options = fs.options;
  const auto *string = Type::string();
  unsigned max =
      fs.function == FsFunction::EXISTS_SYNC || fs.function == FsFunction::UNLINK_SYNC ? 1 : 2;
  auto arity = this->argumentChecker->checkArity(call, 1, max);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  auto *file = this->checkPath(call->args[0], false);
  if (max == 2 && !this->encodingOrOptions(fs, 1, {"encoding"})) {
    return call;
  }
  Expr *encoding = options.lookup("encoding");
  if (encoding != nullptr && !isa<UndefinedExpr>(encoding)) {
    if (this->checkEncoding(encoding, functionName, false) == nullptr) {
      return call;
    }
  }
  call->args = {file};
  call->target =
      BuiltinCall{.builtin = fs.function == FsFunction::EXISTS_SYNC     ? Builtin::FsExists
                             : fs.function == FsFunction::UNLINK_SYNC   ? Builtin::FsUnlink
                             : fs.function == FsFunction::REALPATH_SYNC ? Builtin::FsRealpath
                                                                        : Builtin::FsMkdtemp};
  call->type = fs.function == FsFunction::EXISTS_SYNC   ? Type::boolTy()
               : fs.function == FsFunction::UNLINK_SYNC ? Type::voidTy()
                                                        : string;
  return call;
}

Expr *FsChecker::checkPathWithArgument(FsCall &fs) {
  CallExpr *call = fs.call;
  llvm::SMLoc loc = fs.loc;
  auto arity =
      this->argumentChecker->checkArity(call, fs.function == FsFunction::CHMOD_SYNC ? 2 : 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  auto *file = this->checkPath(call->args[0], false);
  Expr *value;
  if (fs.function == FsFunction::TRUNCATE_SYNC) {
    value = call->args.size() > 1 ? this->argumentChecker->checkArg(call->args[1], Type::intTy())
                                  : nullptr;
    if (value == nullptr) {
      auto *zero = this->state.getAst().make<IntLitExpr>(loc, 0);
      zero->type = Type::intTy();
      value = zero;
    }
  } else if (fs.function == FsFunction::CHMOD_SYNC && isa<StringLitExpr>(call->args[1])) {
    auto text = ToUtf8::toUtf8(cast<StringLitExpr>(call->args[1])->value);
    auto mode = this->octalMode(text);
    if (mode.isErr()) {
      this->state.getDiagnostics().error(call->args[1]->loc(), mode.error().message);
      return call;
    }
    value = this->makeIntLiteral(loc, mode.value());
  } else {
    value = call->args.size() > 1 ? this->argumentChecker->checkArg(call->args[1], Type::intTy())
                                  : this->makeIntLiteral(loc, 0);
  }
  call->args = {file, value};
  call->target =
      BuiltinCall{.builtin = fs.function == FsFunction::ACCESS_SYNC  ? Builtin::FsAccess
                             : fs.function == FsFunction::CHMOD_SYNC ? Builtin::FsChmod
                                                                     : Builtin::FsTruncate};
  call->type = Type::voidTy();
  return call;
}

Expr *FsChecker::checkStat(FsCall &fs) {
  CallExpr *call = fs.call;
  llvm::StringRef functionName = fs.fn;
  llvm::SMLoc loc = fs.loc;
  llvm::StringMap<Expr *> &options = fs.options;
  auto arity = this->argumentChecker->checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  auto *file = this->checkPath(call->args[0], false);
  if (!this->nodeOptions(call, 1, functionName,
                         this->state.getFileSystemStore().isInsidePromiseCall()
                             ? llvm::ArrayRef<llvm::StringRef>{"bigint"}
                             : llvm::ArrayRef<llvm::StringRef>{"throwIfNoEntry", "bigint"},
                         options)) {
    return call;
  }
  bool isThrowIfMissing = true;
  bool isBig = false;
  if (Expr *value = options.lookup("throwIfNoEntry");
      value != nullptr && !this->literalFlag(fs, value, "throwIfNoEntry", isThrowIfMissing)) {
    return call;
  }
  if (Expr *value = options.lookup("bigint");
      value != nullptr && !this->literalFlag(fs, value, "bigint", isBig)) {
    return call;
  }
  if (isBig) {
    auto message = ErrorTextFsCall::bigint();
    this->state.getDiagnostics().error(options.lookup("bigint")->loc(), message);
    return call;
  }
  call->args = {file, this->makeBoolLiteral(loc, fs.function == FsFunction::STAT_SYNC),
                this->makeBoolLiteral(loc, isThrowIfMissing)};
  call->target = BuiltinCall{.builtin = Builtin::FsStat};
  const Type *stats = this->state.getFileSystemStore().getStatsClass()->type;
  call->type = isThrowIfMissing ? stats : Type::optional(stats);
  return call;
}

Expr *FsChecker::checkReaddir(FsCall &fs) {
  CallExpr *call = fs.call;
  llvm::StringRef functionName = fs.fn;
  llvm::SMLoc loc = fs.loc;
  llvm::StringMap<Expr *> &options = fs.options;
  const auto *string = Type::string();
  auto arity = this->argumentChecker->checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  auto *dir = this->checkPath(call->args[0], false);
  if (!this->encodingOrOptions(fs, 1, {"encoding", "withFileTypes", "recursive"})) {
    return call;
  }
  Expr *encoding = options.lookup("encoding");
  if (encoding != nullptr && !isa<UndefinedExpr>(encoding)) {
    if (this->checkEncoding(encoding, functionName, false) == nullptr) {
      return call;
    }
  }
  bool hasTypes = false;
  Expr *value = options.lookup("withFileTypes");
  if (value != nullptr && !this->literalFlag(fs, value, "withFileTypes", hasTypes)) {
    return call;
  }
  Expr *recursive = options.lookup("recursive");
  call->args = {dir, recursive != nullptr
                         ? this->argumentChecker->checkArg(recursive, Type::boolTy())
                         : this->makeBoolLiteral(loc, false)};
  call->target = BuiltinCall{.builtin = Builtin::FsReaddir};
  const Type *dirent = this->state.getFileSystemStore().getDirentClass()->type;
  call->type = Type::array(hasTypes ? dirent : string);
  return call;
}

Expr *FsChecker::checkMkdir(FsCall &fs) {
  CallExpr *call = fs.call;
  llvm::StringRef functionName = fs.fn;
  llvm::SMLoc loc = fs.loc;
  llvm::StringMap<Expr *> &options = fs.options;
  const auto *string = Type::string();
  auto arity = this->argumentChecker->checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  auto *dir = this->checkPath(call->args[0], false);
  Expr *mode = nullptr;
  if (call->args.size() > 1 && !isa<ObjectLitExpr>(call->args[1]) &&
      !isa<UndefinedExpr>(call->args[1])) {
    mode = this->argumentChecker->checkArg(call->args[1], Type::intTy());
  } else {
    if (!this->nodeOptions(call, 1, functionName, {"recursive", "mode"}, options)) {
      return call;
    }
    Expr *value = options.lookup("mode");
    if (value != nullptr) {
      mode = this->argumentChecker->checkArg(value, Type::intTy());
    }
  }
  Expr *recursive = options.lookup("recursive");
  call->args = {dir,
                recursive != nullptr ? this->argumentChecker->checkArg(recursive, Type::boolTy())
                                     : this->makeBoolLiteral(loc, false),
                mode != nullptr ? mode : this->makeIntLiteral(loc, 0777)};
  call->target = BuiltinCall{.builtin = Builtin::FsMkdir};
  call->type = Type::optional(string);
  return call;
}

Expr *FsChecker::checkRmdir(FsCall &fs) {
  CallExpr *call = fs.call;
  llvm::SMLoc loc = fs.loc;
  auto arity = this->argumentChecker->checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  auto *dir = this->checkPath(call->args[0], false);
  if (call->args.size() > 1 && !isa<UndefinedExpr>(call->args[1])) {
    auto message = ErrorTextFsCall::rmdir();
    this->state.getDiagnostics().error(call->args[1]->loc(), message);
    return this->checkLoosely(call);
  }
  call->args = {dir};
  call->target = BuiltinCall{.builtin = Builtin::FsRmdir};
  call->type = Type::voidTy();
  return call;
}

Expr *FsChecker::checkRm(FsCall &fs) {
  CallExpr *call = fs.call;
  llvm::StringRef functionName = fs.fn;
  llvm::SMLoc loc = fs.loc;
  llvm::StringMap<Expr *> &options = fs.options;
  auto arity = this->argumentChecker->checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  auto *target = this->checkPath(call->args[0], false);
  if (!this->nodeOptions(call, 1, functionName, {"recursive", "force", "maxRetries", "retryDelay"},
                         options)) {
    return call;
  }
  Expr *recursive = options.lookup("recursive");
  Expr *force = options.lookup("force");
  for (llvm::StringRef retry : {"maxRetries", "retryDelay"}) {
    Expr *value = options.lookup(retry);
    if (value != nullptr) {
      this->argumentChecker->checkArg(value, Type::intTy());
    }
  }
  call->args = {target,
                recursive != nullptr ? this->argumentChecker->checkArg(recursive, Type::boolTy())
                                     : this->makeBoolLiteral(loc, false),
                force != nullptr ? this->argumentChecker->checkArg(force, Type::boolTy())
                                 : this->makeBoolLiteral(loc, false)};
  call->target = BuiltinCall{.builtin = Builtin::FsRm};
  call->type = Type::voidTy();
  return call;
}

Expr *FsChecker::checkTwoPaths(FsCall &fs) {
  CallExpr *call = fs.call;
  llvm::StringRef functionName = fs.fn;
  llvm::SMLoc loc = fs.loc;
  if (fs.function == FsFunction::COPY_FILE_SYNC && call->args.size() == 3) {
    auto message = ErrorTextFsCall::copyMode(functionName);
    this->state.getDiagnostics().error(call->args[2]->loc(), message);
    return this->checkLoosely(call);
  }
  auto arity = this->argumentChecker->checkArity(call, 2, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  auto *from = this->checkPath(call->args[0], false);
  auto *to = this->checkPath(call->args[1], false);
  call->args = {from, to};
  call->target =
      BuiltinCall{.builtin = fs.function == FsFunction::RENAME_SYNC    ? Builtin::FsRename
                             : fs.function == FsFunction::REPLACE_SYNC ? Builtin::FsReplace
                                                                       : Builtin::FsCopyFile};
  call->type = Type::voidTy();
  return call;
}

Expr *FsChecker::checkFsModuleCall(CallExpr *call, llvm::StringRef functionName, llvm::SMLoc loc) {
  auto *checked = this->checkFsCall(call, functionName, loc);
  auto *done = llvm::dyn_cast<CallExpr>(checked);
  bool isTyped =
      done != nullptr && BuiltinGroups::isFsBuiltin(done->builtin()) && !done->type->isError();
  if (isTyped) {
    done->type = Type::result(done->type, this->fsErrors->ofCall(done));
  }

  return checked;
}

Expr *FsChecker::checkTzIoCall(CallExpr *call, llvm::SMLoc loc) {
  bool isRejected = this->callChecker->rejectSpread(call);
  auto arity = isRejected ? ok(call) : this->argumentChecker->checkArity(call, 0, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
  }
  if (isRejected || arity.isErr()) {
    return call;
  }
  if (!call->args.empty()) {
    call->args[0] = this->argumentChecker->checkArg(call->args[0], Type::string());
  }
  call->target = BuiltinCall{.builtin = Builtin::ReadLine};
  call->type = Type::optional(Type::string());

  return call;
}

Expr *FsChecker::checkFsValue(llvm::StringRef name, Expr *expr) {
  if (name != "constants") {
    return nullptr;
  }
  auto message = ErrorTextFsConstant::value();
  this->state.getDiagnostics().error(expr->loc(), message);

  return expr;
}

Expr *FsChecker::checkFsConstant(MemberExpr *member) {
  auto isFs = [](const NodeName *nodeName) {
    return nodeName != nullptr &&
           (nodeName->module == NodeModule::Fs || nodeName->module == NodeModule::FsPromises);
  };
  auto constantOf = [&](const Expr *object) {
    if (auto *name = dyn_cast<NameExpr>(object)) {
      const auto *nodeName = this->builtinModuleChecker->nodeName(name->name);
      return isFs(nodeName) && nodeName->name == "constants";
    }
    auto *inner = dyn_cast<MemberExpr>(object);
    auto *name = inner != nullptr ? dyn_cast<NameExpr>(inner->object) : nullptr;
    const NodeName *nodeName =
        name != nullptr ? this->builtinModuleChecker->nodeName(name->name) : nullptr;

    return isFs(nodeName) && nodeName->name.empty() && inner->name == "constants";
  };
  if (!constantOf(member->object)) {
    return nullptr;
  }
  static const std::pair<llvm::StringLiteral, int> constants[] = {{"F_OK", 0},
                                                                  {"R_OK", 4},
                                                                  {"W_OK", 2},
                                                                  {"X_OK", 1},
                                                                  {"COPYFILE_EXCL", 1},
                                                                  {"COPYFILE_FICLONE", 2},
                                                                  {"COPYFILE_FICLONE_FORCE", 4}};
  for (const auto &[name, value] : constants) {
    if (name == member->name) {
      auto *literal = this->state.getAst().make<IntLitExpr>(member->loc(), uint64_t(value));
      literal->type = Type::intTy();
      return literal;
    }
  }
  auto message = ErrorTextFsConstant::notYet(member->name);
  this->state.getDiagnostics().error(member->loc(), message);
  member->type = Type::error();

  return member;
}

}
