#include "compiler/semantic/library/fs/fs_checker/fs_checker.h"
#include "compiler/program/library/fs/fs_constant_table/fs_constant_table.h"
#include "compiler/program/to_utf16/to_utf16.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/declare/prelude/prelude.h"
#include "compiler/semantic/declare/type_declarer/type_declarer.h"
#include "compiler/semantic/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/semantic/library/builtin_modules/node_check_helpers/node_check_helpers.h"
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

namespace {

constexpr llvm::StringLiteral kSyncSuffix = "Sync";
constexpr llvm::StringLiteral kEncodingOption = "encoding";
constexpr llvm::StringLiteral kFlagOption = "flag";
constexpr llvm::StringLiteral kModeOption = "mode";
constexpr llvm::StringLiteral kRecursiveOption = "recursive";
constexpr llvm::StringLiteral kForceOption = "force";
constexpr llvm::StringLiteral kWithFileTypesOption = "withFileTypes";
constexpr llvm::StringLiteral kThrowIfNoEntryOption = "throwIfNoEntry";
constexpr llvm::StringLiteral kBigIntOption = "bigint";
constexpr uint64_t kFileMode = 0666;
constexpr uint64_t kDirectoryMode = 0777;

const llvm::StringRef kStatOptions[] = {"throwIfNoEntry", "bigint"};
const llvm::StringRef kPromiseStatOptions[] = {"bigint"};

constexpr llvm::StringLiteral kOpenFlags[] = {"r",  "rs", "sr",  "r+",  "rs+", "sr+", "w",  "wx",
                                              "xw", "w+", "wx+", "xw+", "a",   "ax",  "xa", "as",
                                              "sa", "a+", "ax+", "xa+", "as+", "sa+"};

}

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

FsChecker::ResultOctalMode FsChecker::checkOctalMode(llvm::StringRef text) const {
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

bool FsChecker::readNodeOptions(CallExpr *call, size_t index, llvm::StringRef functionName,
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
  auto *alias = this->state.getLibraryDeclarations().findLibraryType(LibraryType::NATIVE_ERROR);

  return alias != nullptr ? this->typeDeclarer->resolveAlias(alias) : Type::error();
}

void FsChecker::rememberNativeError() {
  const auto *native = this->nativeError();
  const Type *nativeError = native->isError() ? nullptr : native;
  this->state.getInventoryStore().rememberNativeError(nativeError);
}

Expr *FsChecker::checkEncoding(Expr *encoding, llvm::StringRef functionName,
                               EncodingSet encodings) {
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
  bool isUtf8 = cast<IntLitExpr>(number)->value == 0;
  if (encodings == EncodingSet::UTF8_ONLY && !isUtf8) {
    auto written = ToUtf8::toUtf8(literal->value);
    auto message = ErrorTextFsEncoding::utf8Only(functionName, written);
    this->state.getDiagnostics().error(encoding->loc(), message);
    return nullptr;
  }

  return number;
}

Expr *FsChecker::checkFsCall(CallExpr *call, llvm::StringRef functionName, llvm::SMLoc loc) {
  call->type = Type::error();
  bool isPromise = this->state.getFileSystemStore().isInsidePromiseCall();
  std::string syncName = isPromise ? (functionName + kSyncSuffix).str() : functionName.str();
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
      .functionName = functionName,
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
  this->argumentChecker->walkArgsLoosely(call);

  return call;
}

Expr *FsChecker::reportUnknownFsFunction(CallExpr *call, llvm::StringRef functionName,
                                         llvm::SMLoc loc) {
  auto moduleName = NodeCheckHelpers::moduleLabel(NodeModule::Fs);
  auto found = FsExports::checkFsExport(functionName, moduleName);
  std::string message = found.isErr() ? found.error().message : std::string();
  this->state.getDiagnostics().error(loc, message);
  this->argumentChecker->walkArgsLoosely(call);

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
  auto units = ToUtf16::toUtf16(text);
  auto *literal = this->state.getAst().make<StringLitExpr>(loc, units);
  literal->type = Type::string();

  return literal;
}

Expr *FsChecker::checkPath(Expr *arg, PathForm form) {
  const auto *string = Type::string();
  auto *checked = this->exprChecker->checkExpr(arg, string);
  auto *literal = dyn_cast<StringLitExpr>(checked);
  if (literal != nullptr && literal->value.find(u'\0') != std::u16string::npos) {
    auto message = ErrorTextFsCall::zeroPath();
    this->state.getDiagnostics().error(checked->loc(), message);
  }
  bool isDescriptor = form == PathForm::PATH_OR_DESCRIPTOR &&
                      !this->state.getFileSystemStore().isInsidePromiseCall() &&
                      checked->type->isInt();
  if (isDescriptor) {
    return checked;
  }

  return this->typeRelations->coerce(checked, string);
}

FsChecker::FlagRead FsChecker::readFlag(FsCall &fs, llvm::StringRef option) {
  Expr *value = fs.options.lookup(option);
  if (value == nullptr) {
    return FlagRead{.isFailed = false, .value = std::nullopt};
  }
  auto *literal = dyn_cast<BoolLitExpr>(value);
  if (literal != nullptr) {
    literal->type = Type::boolTy();
    return FlagRead{.isFailed = false, .value = literal->value};
  }
  auto message = ErrorTextFsOptions::flag(fs.functionName, option);
  this->state.getDiagnostics().error(value->loc(), message);
  this->exprChecker->checkExpr(value);

  return FlagRead{.isFailed = true, .value = std::nullopt};
}

Expr *FsChecker::openFlag(FsCall &fs, Expr *flag, llvm::StringRef fallback) {
  if (flag == nullptr) {
    return this->makeStringLiteral(fs.loc, fallback);
  }
  auto *literal = dyn_cast<StringLitExpr>(flag);
  bool isLiteral = literal != nullptr;
  std::string written = isLiteral ? ToUtf8::toUtf8(literal->value) : std::string();
  if (isLiteral && llvm::is_contained(kOpenFlags, llvm::StringRef(written))) {
    literal->type = Type::string();
    return literal;
  }
  this->exprChecker->checkExpr(flag);
  auto message = ErrorTextFsCall::flag(fs.functionName, written, isLiteral);
  this->state.getDiagnostics().error(flag->loc(), message);

  return nullptr;
}

bool FsChecker::readEncodingOrOptions(FsCall &fs, size_t index,
                                      llvm::ArrayRef<llvm::StringRef> allowed) {
  CallExpr *call = fs.call;
  if (index >= call->args.size()) {
    return true;
  }
  Expr *arg = call->args[index];
  if (isa<StringLitExpr>(arg)) {
    fs.options[kEncodingOption] = arg;
    return true;
  }
  if (isa<ObjectLitExpr>(arg) || isa<UndefinedExpr>(arg)) {
    return this->readNodeOptions(call, index, fs.functionName, allowed, fs.options);
  }
  auto *checked = this->exprChecker->checkExpr(arg);
  if (checked->type->isString()) {
    auto message = ErrorTextFsEncoding::dependsOn(fs.functionName);
    this->state.getDiagnostics().error(checked->loc(), message);
  } else if (!checked->type->isError()) {
    auto message = ErrorTextFsOptions::literal(fs.functionName, allowed[0]);
    this->state.getDiagnostics().error(checked->loc(), message);
  }

  return false;
}

Expr *FsChecker::givenEncoding(FsCall &fs) const {
  Expr *encoding = fs.options.lookup(kEncodingOption);
  bool isGiven = encoding != nullptr && !isa<UndefinedExpr>(encoding);

  return isGiven ? encoding : nullptr;
}

Expr *FsChecker::checkReadFile(FsCall &fs) {
  CallExpr *call = fs.call;
  auto arity = this->argumentChecker->checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(fs.loc, arity.error().message);
    return call;
  }
  auto *file = this->checkPath(call->args[0], PathForm::PATH_OR_DESCRIPTOR);
  if (!this->readEncodingOrOptions(fs, 1, {"encoding", "flag"})) {
    return call;
  }
  Expr *encoding = this->givenEncoding(fs);
  Expr *number = encoding != nullptr
                     ? this->checkEncoding(encoding, fs.functionName, EncodingSet::ANY)
                     : nullptr;
  if (encoding != nullptr && number == nullptr) {
    return call;
  }
  Expr *flagOption = fs.options.lookup(kFlagOption);
  auto *flag = this->openFlag(fs, flagOption, "r");
  if (flag == nullptr) {
    return call;
  }
  call->args = {file, flag};
  call->target = BuiltinCall{.builtin = Builtin::FsReadFile};
  call->type = number != nullptr ? Type::string() : Type::bytesType(Type::Bytes::Buffer);
  if (number != nullptr) {
    call->args.push_back(number);
  }

  return call;
}

Expr *FsChecker::checkWriteFile(FsCall &fs) {
  CallExpr *call = fs.call;
  llvm::SMLoc loc = fs.loc;
  auto arity = this->argumentChecker->checkArity(call, 2, 3);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  auto *file = this->checkPath(call->args[0], PathForm::PATH_OR_DESCRIPTOR);
  auto *data = this->exprChecker->checkExpr(call->args[1], Type::string());
  bool hasBytes = this->isBytesData(data->type);
  if (!data->type->isString() && !hasBytes && !data->type->isError()) {
    auto message = ErrorTextFsCall::data(fs.functionName, data->type);
    this->state.getDiagnostics().error(data->loc(), message);
    this->argumentChecker->walkArgsLoosely(call);
    return call;
  }
  if (!this->readEncodingOrOptions(fs, 2, {"encoding", "flag", "mode"})) {
    return call;
  }
  Expr *encoding = this->givenEncoding(fs);
  if (encoding != nullptr && hasBytes) {
    auto message = ErrorTextFsEncoding::forText(fs.functionName);
    this->state.getDiagnostics().error(encoding->loc(), message);
    return call;
  }
  Expr *number = encoding != nullptr
                     ? this->checkEncoding(encoding, fs.functionName, EncodingSet::ANY)
                     : nullptr;
  if (encoding != nullptr && number == nullptr) {
    return call;
  }
  llvm::StringRef fallback = fs.function == FsFunction::WRITE_FILE_SYNC ? "w" : "a";
  Expr *flagOption = fs.options.lookup(kFlagOption);
  auto *flag = this->openFlag(fs, flagOption, fallback);
  if (flag == nullptr) {
    return call;
  }
  Expr *mode = fs.options.lookup(kModeOption);
  auto *checkedMode = mode != nullptr ? this->argumentChecker->checkArg(mode, Type::intTy())
                                      : this->makeIntLiteral(loc, kFileMode);
  call->args = {file, data, flag, checkedMode};
  if (!hasBytes) {
    auto *textEncoding = number != nullptr ? number : this->makeIntLiteral(loc, 0);
    call->args.push_back(textEncoding);
  }
  call->target = BuiltinCall{.builtin = Builtin::FsWriteFile};
  call->type = Type::voidTy();

  return call;
}

bool FsChecker::isBytesData(const Type *type) const {
  auto isByteView = std::bind_front(&FsChecker::isByteViewType, this);

  return type->isByteView() || (type->isUnion() && llvm::all_of(type->members(), isByteView));
}

bool FsChecker::isByteViewType(const Type *type) const {
  return type->isByteView();
}

Expr *FsChecker::checkPathCall(FsCall &fs) {
  CallExpr *call = fs.call;
  bool isSingle = fs.function == FsFunction::EXISTS_SYNC || fs.function == FsFunction::UNLINK_SYNC;
  unsigned max = isSingle ? 1 : 2;
  auto arity = this->argumentChecker->checkArity(call, 1, max);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(fs.loc, arity.error().message);
    return call;
  }
  auto *file = this->checkPath(call->args[0], PathForm::PATH_ONLY);
  if (max == 2 && !this->readEncodingOrOptions(fs, 1, {"encoding"})) {
    return call;
  }
  Expr *encoding = this->givenEncoding(fs);
  bool isEncodingFailed =
      encoding != nullptr &&
      this->checkEncoding(encoding, fs.functionName, EncodingSet::UTF8_ONLY) == nullptr;
  if (isEncodingFailed) {
    return call;
  }
  call->args = {file};
  call->target = BuiltinCall{.builtin = this->pathCallBuiltin(fs.function)};
  call->type = this->pathCallType(fs.function);

  return call;
}

Builtin FsChecker::pathCallBuiltin(FsFunction function) const {
  if (function == FsFunction::EXISTS_SYNC) {
    return Builtin::FsExists;
  }
  if (function == FsFunction::UNLINK_SYNC) {
    return Builtin::FsUnlink;
  }
  if (function == FsFunction::REALPATH_SYNC) {
    return Builtin::FsRealpath;
  }

  return Builtin::FsMkdtemp;
}

const Type *FsChecker::pathCallType(FsFunction function) const {
  if (function == FsFunction::EXISTS_SYNC) {
    return Type::boolTy();
  }
  if (function == FsFunction::UNLINK_SYNC) {
    return Type::voidTy();
  }

  return Type::string();
}

Expr *FsChecker::checkPathWithArgument(FsCall &fs) {
  CallExpr *call = fs.call;
  unsigned min = fs.function == FsFunction::CHMOD_SYNC ? 2 : 1;
  auto arity = this->argumentChecker->checkArity(call, min, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(fs.loc, arity.error().message);
    return call;
  }
  auto *file = this->checkPath(call->args[0], PathForm::PATH_ONLY);
  auto *value = this->pathArgument(fs);
  if (value == nullptr) {
    return call;
  }
  call->args = {file, value};
  call->target = BuiltinCall{.builtin = this->pathArgumentBuiltin(fs.function)};
  call->type = Type::voidTy();

  return call;
}

Expr *FsChecker::pathArgument(FsCall &fs) {
  CallExpr *call = fs.call;
  bool hasValue = call->args.size() > 1;
  if (fs.function == FsFunction::TRUNCATE_SYNC) {
    auto *length =
        hasValue ? this->argumentChecker->checkArg(call->args[1], Type::intTy()) : nullptr;
    return length != nullptr ? length : this->makeIntLiteral(fs.loc, 0);
  }
  if (fs.function == FsFunction::CHMOD_SYNC && isa<StringLitExpr>(call->args[1])) {
    return this->octalModeArgument(call->args[1], fs.loc);
  }
  if (hasValue) {
    return this->argumentChecker->checkArg(call->args[1], Type::intTy());
  }

  return this->makeIntLiteral(fs.loc, 0);
}

Expr *FsChecker::octalModeArgument(Expr *arg, llvm::SMLoc loc) {
  const auto *literal = cast<StringLitExpr>(arg);
  auto text = ToUtf8::toUtf8(literal->value);
  auto mode = this->checkOctalMode(text);
  if (mode.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), mode.error().message);
    return nullptr;
  }

  return this->makeIntLiteral(loc, mode.value());
}

Builtin FsChecker::pathArgumentBuiltin(FsFunction function) const {
  if (function == FsFunction::ACCESS_SYNC) {
    return Builtin::FsAccess;
  }
  if (function == FsFunction::CHMOD_SYNC) {
    return Builtin::FsChmod;
  }

  return Builtin::FsTruncate;
}

Expr *FsChecker::checkStat(FsCall &fs) {
  CallExpr *call = fs.call;
  llvm::SMLoc loc = fs.loc;
  auto arity = this->argumentChecker->checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  auto *file = this->checkPath(call->args[0], PathForm::PATH_ONLY);
  bool isPromise = this->state.getFileSystemStore().isInsidePromiseCall();
  llvm::ArrayRef<llvm::StringRef> allowed =
      isPromise ? llvm::ArrayRef(kPromiseStatOptions) : llvm::ArrayRef(kStatOptions);
  if (!this->readNodeOptions(call, 1, fs.functionName, allowed, fs.options)) {
    return call;
  }
  auto throwIfMissing = this->readFlag(fs, kThrowIfNoEntryOption);
  if (throwIfMissing.isFailed) {
    return call;
  }
  auto bigint = this->readFlag(fs, kBigIntOption);
  if (bigint.isFailed) {
    return call;
  }
  if (bigint.value.value_or(false)) {
    auto message = ErrorTextFsCall::bigint();
    this->state.getDiagnostics().error(fs.options.lookup(kBigIntOption)->loc(), message);
    return call;
  }
  bool isThrowIfMissing = throwIfMissing.value.value_or(true);
  auto *isFollowing = this->makeBoolLiteral(loc, fs.function == FsFunction::STAT_SYNC);
  auto *isThrowing = this->makeBoolLiteral(loc, isThrowIfMissing);
  call->args = {file, isFollowing, isThrowing};
  call->target = BuiltinCall{.builtin = Builtin::FsStat};
  const Type *stats = this->state.getFileSystemStore().getStatsClass()->type;
  call->type = isThrowIfMissing ? stats : Type::optional(stats);

  return call;
}

Expr *FsChecker::checkReaddir(FsCall &fs) {
  CallExpr *call = fs.call;
  auto arity = this->argumentChecker->checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(fs.loc, arity.error().message);
    return call;
  }
  auto *dir = this->checkPath(call->args[0], PathForm::PATH_ONLY);
  if (!this->readEncodingOrOptions(fs, 1, {"encoding", "withFileTypes", "recursive"})) {
    return call;
  }
  Expr *encoding = this->givenEncoding(fs);
  bool isEncodingFailed =
      encoding != nullptr &&
      this->checkEncoding(encoding, fs.functionName, EncodingSet::UTF8_ONLY) == nullptr;
  if (isEncodingFailed) {
    return call;
  }
  auto withTypes = this->readFlag(fs, kWithFileTypesOption);
  if (withTypes.isFailed) {
    return call;
  }
  auto *recursive = this->boolOption(fs, kRecursiveOption);
  call->args = {dir, recursive};
  call->target = BuiltinCall{.builtin = Builtin::FsReaddir};
  const Type *dirent = this->state.getFileSystemStore().getDirentClass()->type;
  bool hasTypes = withTypes.value.value_or(false);
  call->type = Type::array(hasTypes ? dirent : Type::string());

  return call;
}

Expr *FsChecker::boolOption(FsCall &fs, llvm::StringRef option) {
  Expr *value = fs.options.lookup(option);
  if (value != nullptr) {
    return this->argumentChecker->checkArg(value, Type::boolTy());
  }

  return this->makeBoolLiteral(fs.loc, false);
}

Expr *FsChecker::checkMkdir(FsCall &fs) {
  CallExpr *call = fs.call;
  auto arity = this->argumentChecker->checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(fs.loc, arity.error().message);
    return call;
  }
  auto *dir = this->checkPath(call->args[0], PathForm::PATH_ONLY);
  bool isModeGiven = call->args.size() > 1 && !isa<ObjectLitExpr>(call->args[1]) &&
                     !isa<UndefinedExpr>(call->args[1]);
  if (!isModeGiven &&
      !this->readNodeOptions(call, 1, fs.functionName, {"recursive", "mode"}, fs.options)) {
    return call;
  }
  Expr *mode = isModeGiven ? call->args[1] : fs.options.lookup(kModeOption);
  auto *checkedMode = mode != nullptr ? this->argumentChecker->checkArg(mode, Type::intTy())
                                      : this->makeIntLiteral(fs.loc, kDirectoryMode);
  auto *recursive = this->boolOption(fs, kRecursiveOption);
  call->args = {dir, recursive, checkedMode};
  call->target = BuiltinCall{.builtin = Builtin::FsMkdir};
  call->type = Type::optional(Type::string());

  return call;
}

Expr *FsChecker::checkRmdir(FsCall &fs) {
  CallExpr *call = fs.call;
  auto arity = this->argumentChecker->checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(fs.loc, arity.error().message);
    return call;
  }
  auto *dir = this->checkPath(call->args[0], PathForm::PATH_ONLY);
  if (call->args.size() > 1 && !isa<UndefinedExpr>(call->args[1])) {
    auto message = ErrorTextFsCall::rmdir();
    this->state.getDiagnostics().error(call->args[1]->loc(), message);
    this->argumentChecker->walkArgsLoosely(call);
    return call;
  }
  call->args = {dir};
  call->target = BuiltinCall{.builtin = Builtin::FsRmdir};
  call->type = Type::voidTy();

  return call;
}

Expr *FsChecker::checkRm(FsCall &fs) {
  CallExpr *call = fs.call;
  auto arity = this->argumentChecker->checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(fs.loc, arity.error().message);
    return call;
  }
  auto *target = this->checkPath(call->args[0], PathForm::PATH_ONLY);
  bool isRead = this->readNodeOptions(
      call, 1, fs.functionName, {"recursive", "force", "maxRetries", "retryDelay"}, fs.options);
  if (!isRead) {
    return call;
  }
  for (llvm::StringRef retry : {"maxRetries", "retryDelay"}) {
    Expr *value = fs.options.lookup(retry);
    if (value != nullptr) {
      this->argumentChecker->checkArg(value, Type::intTy());
    }
  }
  auto *recursive = this->boolOption(fs, kRecursiveOption);
  auto *force = this->boolOption(fs, kForceOption);
  call->args = {target, recursive, force};
  call->target = BuiltinCall{.builtin = Builtin::FsRm};
  call->type = Type::voidTy();

  return call;
}

Expr *FsChecker::checkTwoPaths(FsCall &fs) {
  CallExpr *call = fs.call;
  if (fs.function == FsFunction::COPY_FILE_SYNC && call->args.size() == 3) {
    auto message = ErrorTextFsCall::copyMode(fs.functionName);
    this->state.getDiagnostics().error(call->args[2]->loc(), message);
    this->argumentChecker->walkArgsLoosely(call);
    return call;
  }
  auto arity = this->argumentChecker->checkArity(call, 2, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(fs.loc, arity.error().message);
    return call;
  }
  auto *from = this->checkPath(call->args[0], PathForm::PATH_ONLY);
  auto *to = this->checkPath(call->args[1], PathForm::PATH_ONLY);
  call->args = {from, to};
  call->target = BuiltinCall{.builtin = this->twoPathsBuiltin(fs.function)};
  call->type = Type::voidTy();

  return call;
}

Builtin FsChecker::twoPathsBuiltin(FsFunction function) const {
  if (function == FsFunction::RENAME_SYNC) {
    return Builtin::FsRename;
  }
  if (function == FsFunction::REPLACE_SYNC) {
    return Builtin::FsReplace;
  }

  return Builtin::FsCopyFile;
}

Expr *FsChecker::checkFsModuleCall(CallExpr *call, llvm::StringRef functionName, llvm::SMLoc loc) {
  auto *checked = this->checkFsCall(call, functionName, loc);
  auto *done = llvm::dyn_cast<CallExpr>(checked);
  bool isTyped =
      done != nullptr && BuiltinGroups::isFsBuiltin(done->builtin()) && !done->type->isError();
  if (isTyped) {
    const auto *errors = this->fsErrors->ofCall(done);
    done->type = Type::result(done->type, errors);
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
  if (FsFunctionTable::find(name) != FsFunction::CONSTANTS) {
    return nullptr;
  }
  auto message = ErrorTextFsConstant::value();
  this->state.getDiagnostics().error(expr->loc(), message);

  return expr;
}

Expr *FsChecker::checkFsConstant(MemberExpr *member) {
  if (!this->isFsConstants(member->object)) {
    return nullptr;
  }
  auto value = FsConstantTable::find(member->name);
  if (value.has_value()) {
    auto *literal = this->state.getAst().make<IntLitExpr>(member->loc(), uint64_t(value.value()));
    literal->type = Type::intTy();
    return literal;
  }
  auto message = ErrorTextFsConstant::notYet(member->name);
  this->state.getDiagnostics().error(member->loc(), message);
  member->type = Type::error();

  return member;
}

bool FsChecker::isFsConstants(const Expr *object) const {
  const auto *name = dyn_cast<NameExpr>(object);
  if (name != nullptr) {
    const auto *nodeName = this->builtinModuleChecker->nodeName(name->name);
    return this->isFsModuleName(nodeName) &&
           FsFunctionTable::find(nodeName->name) == FsFunction::CONSTANTS;
  }
  const auto *inner = dyn_cast<MemberExpr>(object);
  const auto *owner = inner != nullptr ? dyn_cast<NameExpr>(inner->object) : nullptr;
  const NodeName *nodeName =
      owner != nullptr ? this->builtinModuleChecker->nodeName(owner->name) : nullptr;

  return this->isFsModuleName(nodeName) && nodeName->name.empty() &&
         FsFunctionTable::find(inner->name) == FsFunction::CONSTANTS;
}

bool FsChecker::isFsModuleName(const NodeName *nodeName) const {
  return nodeName != nullptr &&
         (nodeName->module == NodeModule::Fs || nodeName->module == NodeModule::FsPromises);
}

}
