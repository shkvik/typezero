#include "compiler/semantic/library_v2/fs/library_fs_registry.h"
#include "compiler/program/library/builtin_modules/known_modules/known_modules.h"
#include "compiler/program/library/fs/fs_constant_table/fs_constant_table.h"
#include "compiler/program/library/fs/fs_export_table/fs_export_table.h"
#include "compiler/program/library/fs/fs_open_flag_table/fs_open_flag_table.h"
#include "compiler/program/to_utf16/to_utf16.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr uint64_t kFileMode = 0666;
constexpr uint64_t kDirectoryMode = 0777;

const NodeOption kStatOptions[] = {NodeOption::THROW_IF_NO_ENTRY, NodeOption::BIGINT};
const NodeOption kPromiseStatOptions[] = {NodeOption::BIGINT};

}

LibraryFsRegistry::LibraryFsRegistry(SemanticState &owner)
    : state(owner) {}

void LibraryFsRegistry::checkFsCall(const CheckFsCall &task) {
  CallExpr *call = task.call;
  llvm::StringRef functionName = task.functionName;
  auto &tasks = this->state.walk.tasks;
  call->type = Type::error();
  tasks.pushChecked(call);
  bool isPromise = this->state.library.filesystem.isInsidePromiseCall();
  std::string syncName =
      isPromise ? FsFunctionTable::getSyncName(functionName) : functionName.str();
  auto function = FsFunctionTable::find(syncName);
  bool isValue = function == FsFunction::CONSTANTS || function == FsFunction::STATS ||
                 function == FsFunction::DIRENT;
  if (isValue) {
    auto message = ErrorTextFsCall::notFunc(functionName);
    this->state.diagnostics.error(task.loc, message);

    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  if (this->state.library.common.hasSpread(call)) {
    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  if (!function.has_value()) {
    auto moduleName = KnownModules::getPath(NodeModule::Fs);
    auto found = this->state.library.modules.checkFsExport(functionName, moduleName);
    std::string message = found.isErr() ? found.error().message : std::string();
    this->state.diagnostics.error(task.loc, message);

    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  auto draft = FsDraft{
      .call = call,
      .function = *function,
      .functionName = functionName,
      .loc = task.loc,
  };

  return this->startFsFunction(draft);
}

void LibraryFsRegistry::startFsFunction(const FsDraft &draft) {
  switch (draft.function) {
    case FsFunction::CONSTANTS:
    case FsFunction::STATS:
    case FsFunction::DIRENT:
      return;
    case FsFunction::READ_FILE_SYNC:
      return this->startReadFile(draft);
    case FsFunction::WRITE_FILE_SYNC:
    case FsFunction::APPEND_FILE_SYNC:
      return this->startWriteFile(draft);
    case FsFunction::EXISTS_SYNC:
    case FsFunction::UNLINK_SYNC:
    case FsFunction::REALPATH_SYNC:
    case FsFunction::MKDTEMP_SYNC:
      return this->startPathCall(draft);
    case FsFunction::ACCESS_SYNC:
    case FsFunction::CHMOD_SYNC:
    case FsFunction::TRUNCATE_SYNC:
      return this->startPathWithArgument(draft);
    case FsFunction::STAT_SYNC:
    case FsFunction::LSTAT_SYNC:
      return this->startStat(draft);
    case FsFunction::READDIR_SYNC:
      return this->startReaddir(draft);
    case FsFunction::MKDIR_SYNC:
      return this->startMkdir(draft);
    case FsFunction::RMDIR_SYNC:
      return this->startRmdir(draft);
    case FsFunction::RM_SYNC:
      return this->startRm(draft);
    case FsFunction::RENAME_SYNC:
    case FsFunction::COPY_FILE_SYNC:
    case FsFunction::REPLACE_SYNC:
      return this->startTwoPaths(draft);
  }
}

bool LibraryFsRegistry::hasFsArity(const FsDraft &draft, unsigned min, unsigned max) {
  auto arity = this->state.library.common.checkArity(draft.call, min, max);
  if (arity.isErr()) {
    this->state.diagnostics.error(draft.loc, arity.error().message);
  }

  return arity.isOk();
}

void LibraryFsRegistry::pushPathCheck(Expr **slot, FsPathForm form) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterPathArg{.slot = slot, .form = form});

  return tasks.pushTask(CheckExprSlot{.slot = slot, .expected = Type::string()});
}

void LibraryFsRegistry::afterPathArg(const AfterPathArg &task) {
  const Expr *checked = *task.slot;
  const auto *literal = dyn_cast<StringLitExpr>(checked);
  if (literal != nullptr && literal->value.find(u'\0') != std::u16string::npos) {
    auto message = ErrorTextFsCall::zeroPath();
    this->state.diagnostics.error(checked->loc(), message);
  }
  bool isDescriptor = task.form == FsPathForm::PATH_OR_DESCRIPTOR &&
                      !this->state.library.filesystem.isInsidePromiseCall() &&
                      checked->type->isInt();
  if (isDescriptor) {
    return;
  }

  return this->state.walk.tasks.pushTask(
      CoerceSlot{.slot = task.slot, .to = Type::string(), .what = ""});
}

void LibraryFsRegistry::readEncodingOrOptions(const ReadEncodingOrOptions &task) {
  CallExpr *call = task.call;
  size_t index = task.index;
  auto &tasks = this->state.walk.tasks;
  if (index >= call->args.size() || isa<StringLitExpr>(call->args[index])) {
    return tasks.pushVerdict(true);
  }
  Expr *arg = call->args[index];
  if (isa<ObjectLitExpr>(arg) || isa<UndefinedExpr>(arg)) {
    return tasks.pushTask(ReadNodeOptions{
        .call = call,
        .index = index,
        .functionName = task.functionName,
        .allowed = task.allowed,
    });
  }
  tasks.pushTask(AfterEncodingArg{
      .call = call,
      .index = index,
      .functionName = task.functionName,
      .first = task.allowed[0],
  });

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[index], .expected = nullptr});
}

void LibraryFsRegistry::afterEncodingArg(const AfterEncodingArg &task) {
  const Expr *checked = task.call->args[task.index];
  if (checked->type->isString()) {
    auto message = ErrorTextFsEncoding::dependsOn(task.functionName);
    this->state.diagnostics.error(checked->loc(), message);
  } else if (!checked->type->isError()) {
    auto first = NodeOptionTable::getName(task.first);
    auto message = ErrorTextNodeOptions::literal(task.functionName, first);
    this->state.diagnostics.error(checked->loc(), message);
  }

  return this->state.walk.tasks.pushVerdict(false);
}

Expr *LibraryFsRegistry::optionOf(const FsDraft &draft, size_t index, NodeOption option) const {
  return this->state.library.common.findNodeOption(draft.call, index, option);
}

Expr *LibraryFsRegistry::givenEncoding(const FsDraft &draft, size_t index) const {
  CallExpr *call = draft.call;
  if (index < call->args.size() && isa<StringLitExpr>(call->args[index])) {
    return call->args[index];
  }
  Expr *encoding = this->optionOf(draft, index, NodeOption::ENCODING);
  bool isGiven = encoding != nullptr && !isa<UndefinedExpr>(encoding);

  return isGiven ? encoding : nullptr;
}

LibraryFsRegistry::EncodingRead
LibraryFsRegistry::readEncoding(const FsDraft &draft, Expr *encoding, EncodingSet encodings) {
  auto failed = EncodingRead{.isFailed = true, .number = nullptr};
  if (encoding == nullptr) {
    return EncodingRead{.isFailed = false, .number = nullptr};
  }
  auto *literal = dyn_cast<StringLitExpr>(encoding);
  if (literal == nullptr) {
    auto message = ErrorTextFsEncoding::literal(draft.functionName);
    this->state.diagnostics.error(encoding->loc(), message);
    this->pushDroppedCheck(encoding);

    return failed;
  }
  literal->type = Type::string();
  auto written = ToUtf8::toUtf8(literal->value);
  auto known = this->state.library.common.checkKnownEncoding(written);
  if (known.isErr()) {
    this->state.diagnostics.error(literal->loc(), known.error().message);
    literal->type = Type::error();

    return failed;
  }
  bool isUtf8 = known.value() == 0;
  if (encodings == EncodingSet::UTF8_ONLY && !isUtf8) {
    auto message = ErrorTextFsEncoding::utf8Only(draft.functionName, written);
    this->state.diagnostics.error(encoding->loc(), message);

    return failed;
  }
  auto *number = this->makeIntLiteral(literal->loc(), uint64_t(known.value()));

  return EncodingRead{.isFailed = false, .number = number};
}

void LibraryFsRegistry::pushDroppedCheck(Expr *value) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(DiscardChecked{});

  return tasks.pushTask(CheckExpr{.expr = value, .expected = nullptr});
}

LibraryFsRegistry::FlagRead LibraryFsRegistry::readFlag(const FsDraft &draft, size_t index,
                                                        NodeOption option) {
  Expr *value = this->optionOf(draft, index, option);
  if (value == nullptr) {
    return FlagRead{.isFailed = false, .value = std::nullopt};
  }
  auto *literal = dyn_cast<BoolLitExpr>(value);
  if (literal != nullptr) {
    literal->type = Type::boolTy();

    return FlagRead{.isFailed = false, .value = literal->value};
  }
  auto optionName = NodeOptionTable::getName(option);
  auto message = ErrorTextNodeOptions::flag(draft.functionName, optionName);
  this->state.diagnostics.error(value->loc(), message);
  this->pushDroppedCheck(value);

  return FlagRead{.isFailed = true, .value = std::nullopt};
}

Expr *LibraryFsRegistry::openFlag(const FsDraft &draft, Expr *flag, llvm::StringRef fallback) {
  if (flag == nullptr) {
    return this->makeStringLiteral(draft.loc, fallback);
  }
  auto *literal = dyn_cast<StringLitExpr>(flag);
  bool isLiteral = literal != nullptr;
  std::string written = isLiteral ? ToUtf8::toUtf8(literal->value) : std::string();
  if (isLiteral && FsOpenFlagTable::isKnown(written)) {
    literal->type = Type::string();

    return literal;
  }
  auto message = ErrorTextFsCall::flag(draft.functionName, written, isLiteral);
  this->state.walk.tasks.pushTask(ReportError{.loc = flag->loc(), .message = message});
  this->pushDroppedCheck(flag);

  return nullptr;
}

void LibraryFsRegistry::pushValueCheck(Expr *value, const Type *type) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(Coerce{.to = type, .what = ""});

  return tasks.pushTask(CheckExpr{.expr = value, .expected = type});
}

void LibraryFsRegistry::startReadFile(const FsDraft &draft) {
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  if (!this->hasFsArity(draft, 1, 2)) {
    return;
  }
  tasks.pushTask(AfterReadFileOptions{.draft = draft});
  tasks.pushTask(ReadEncodingOrOptions{
      .call = call,
      .index = 1,
      .functionName = draft.functionName,
      .allowed = {NodeOption::ENCODING, NodeOption::FLAG},
  });

  return this->pushPathCheck(&call->args[0], FsPathForm::PATH_OR_DESCRIPTOR);
}

void LibraryFsRegistry::afterReadFileOptions(const AfterReadFileOptions &task) {
  const FsDraft &draft = task.draft;
  CallExpr *call = draft.call;
  if (!this->state.walk.tasks.takeVerdict()) {
    return;
  }
  Expr *given = this->givenEncoding(draft, 1);
  auto encoding = this->readEncoding(draft, given, EncodingSet::ANY);
  if (encoding.isFailed) {
    return;
  }
  Expr *flagOption = this->optionOf(draft, 1, NodeOption::FLAG);
  auto *flag = this->openFlag(draft, flagOption, FsOpenFlagTable::getDefault(draft.function));
  if (flag == nullptr) {
    return;
  }
  Expr *file = call->args[0];
  call->args = {file, flag};
  call->target = BuiltinCall{.builtin = Builtin::FsReadFile};
  call->type = encoding.number != nullptr ? Type::string() : Type::bytesType(Type::Bytes::Buffer);
  if (encoding.number != nullptr) {
    call->args.push_back(encoding.number);
  }
}

void LibraryFsRegistry::startWriteFile(const FsDraft &draft) {
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  if (!this->hasFsArity(draft, 2, 3)) {
    return;
  }
  tasks.pushTask(AfterWriteData{.draft = draft});
  tasks.pushTask(CheckExprSlot{.slot = &call->args[1], .expected = Type::string()});

  return this->pushPathCheck(&call->args[0], FsPathForm::PATH_OR_DESCRIPTOR);
}

void LibraryFsRegistry::afterWriteData(const AfterWriteData &task) {
  const FsDraft &draft = task.draft;
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  const Expr *data = call->args[1];
  bool hasBytes = this->isBytesData(data->type);
  if (!data->type->isString() && !hasBytes && !data->type->isError()) {
    auto message = ErrorTextFsCall::data(draft.functionName, data->type);
    this->state.diagnostics.error(data->loc(), message);

    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  tasks.pushTask(AfterWriteOptions{.draft = draft, .hasBytes = hasBytes});

  return tasks.pushTask(ReadEncodingOrOptions{
      .call = call,
      .index = 2,
      .functionName = draft.functionName,
      .allowed = {NodeOption::ENCODING, NodeOption::FLAG, NodeOption::MODE},
  });
}

void LibraryFsRegistry::afterWriteOptions(const AfterWriteOptions &task) {
  const FsDraft &draft = task.draft;
  auto &tasks = this->state.walk.tasks;
  if (!tasks.takeVerdict()) {
    return;
  }
  Expr *encoding = this->givenEncoding(draft, 2);
  if (encoding != nullptr && task.hasBytes) {
    auto message = ErrorTextFsEncoding::forText(draft.functionName);

    return this->state.diagnostics.error(encoding->loc(), message);
  }
  auto read = this->readEncoding(draft, encoding, EncodingSet::ANY);
  if (read.isFailed) {
    return;
  }
  Expr *flagOption = this->optionOf(draft, 2, NodeOption::FLAG);
  auto *flag = this->openFlag(draft, flagOption, FsOpenFlagTable::getDefault(draft.function));
  if (flag == nullptr) {
    return;
  }
  Expr *mode = this->optionOf(draft, 2, NodeOption::MODE);
  auto finish = FinishWriteFile{
      .draft = draft,
      .flag = flag,
      .number = read.number,
      .hasBytes = task.hasBytes,
      .hasMode = mode != nullptr,
  };
  if (mode == nullptr) {
    return this->finishWriteFile(finish);
  }
  tasks.pushTask(std::move(finish));

  return this->pushValueCheck(mode, Type::intTy());
}

void LibraryFsRegistry::finishWriteFile(const FinishWriteFile &task) {
  const FsDraft &draft = task.draft;
  CallExpr *call = draft.call;
  Expr *mode = task.hasMode ? this->state.walk.tasks.takeChecked()
                            : this->makeIntLiteral(draft.loc, kFileMode);
  Expr *file = call->args[0];
  Expr *data = call->args[1];
  call->args = {file, data, task.flag, mode};
  if (!task.hasBytes) {
    auto *textEncoding = task.number != nullptr ? task.number : this->makeIntLiteral(draft.loc, 0);
    call->args.push_back(textEncoding);
  }
  call->target = BuiltinCall{.builtin = Builtin::FsWriteFile};
  call->type = Type::voidTy();
}

void LibraryFsRegistry::startPathCall(const FsDraft &draft) {
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  bool isSingle =
      draft.function == FsFunction::EXISTS_SYNC || draft.function == FsFunction::UNLINK_SYNC;
  unsigned max = isSingle ? 1 : 2;
  if (!this->hasFsArity(draft, 1, max)) {
    return;
  }
  tasks.pushTask(AfterPathCallOptions{.draft = draft, .hasOptions = !isSingle});
  if (!isSingle) {
    tasks.pushTask(ReadEncodingOrOptions{
        .call = call,
        .index = 1,
        .functionName = draft.functionName,
        .allowed = {NodeOption::ENCODING},
    });
  }

  return this->pushPathCheck(&call->args[0], FsPathForm::PATH_ONLY);
}

void LibraryFsRegistry::afterPathCallOptions(const AfterPathCallOptions &task) {
  const FsDraft &draft = task.draft;
  CallExpr *call = draft.call;
  if (task.hasOptions && !this->state.walk.tasks.takeVerdict()) {
    return;
  }
  Expr *given = this->givenEncoding(draft, 1);
  auto encoding = this->readEncoding(draft, given, EncodingSet::UTF8_ONLY);
  if (encoding.isFailed) {
    return;
  }
  Expr *file = call->args[0];
  call->args = {file};
  call->target = BuiltinCall{.builtin = this->pathCallBuiltin(draft.function)};
  call->type = this->pathCallType(draft.function);
}

void LibraryFsRegistry::startPathWithArgument(const FsDraft &draft) {
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  unsigned min = draft.function == FsFunction::CHMOD_SYNC ? 2 : 1;
  if (!this->hasFsArity(draft, min, 2)) {
    return;
  }
  bool hasValue = call->args.size() > 1;
  bool isOctal = draft.function == FsFunction::CHMOD_SYNC && isa<StringLitExpr>(call->args[1]);
  auto source =
      isOctal ? FsValueSource::OCTAL : (hasValue ? FsValueSource::STACK : FsValueSource::ZERO);
  tasks.pushTask(FinishPathArgument{.draft = draft, .source = source});
  if (source == FsValueSource::STACK) {
    this->pushValueCheck(call->args[1], Type::intTy());
  }

  return this->pushPathCheck(&call->args[0], FsPathForm::PATH_ONLY);
}

void LibraryFsRegistry::finishPathArgument(const FinishPathArgument &task) {
  const FsDraft &draft = task.draft;
  CallExpr *call = draft.call;
  Expr *value = this->pathArgumentValue(task);
  if (value == nullptr) {
    return;
  }
  Expr *file = call->args[0];
  call->args = {file, value};
  call->target = BuiltinCall{.builtin = this->pathArgumentBuiltin(draft.function)};
  call->type = Type::voidTy();
}

Expr *LibraryFsRegistry::pathArgumentValue(const FinishPathArgument &task) {
  const FsDraft &draft = task.draft;
  switch (task.source) {
    case FsValueSource::STACK:
      return this->state.walk.tasks.takeChecked();
    case FsValueSource::ZERO:
      return this->makeIntLiteral(draft.loc, 0);
    case FsValueSource::OCTAL:
      return this->octalModeArgument(draft.call->args[1], draft.loc);
  }

  return nullptr;
}

void LibraryFsRegistry::startStat(const FsDraft &draft) {
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  if (!this->hasFsArity(draft, 1, 2)) {
    return;
  }
  bool isPromise = this->state.library.filesystem.isInsidePromiseCall();
  llvm::ArrayRef<NodeOption> allowed =
      isPromise ? llvm::ArrayRef(kPromiseStatOptions) : llvm::ArrayRef(kStatOptions);
  tasks.pushTask(AfterStatOptions{.draft = draft});
  tasks.pushTask(ReadNodeOptions{
      .call = call,
      .index = 1,
      .functionName = draft.functionName,
      .allowed = std::vector<NodeOption>(allowed.begin(), allowed.end()),
  });

  return this->pushPathCheck(&call->args[0], FsPathForm::PATH_ONLY);
}

void LibraryFsRegistry::afterStatOptions(const AfterStatOptions &task) {
  const FsDraft &draft = task.draft;
  CallExpr *call = draft.call;
  llvm::SMLoc loc = draft.loc;
  if (!this->state.walk.tasks.takeVerdict()) {
    return;
  }
  auto throwIfMissing = this->readFlag(draft, 1, NodeOption::THROW_IF_NO_ENTRY);
  if (throwIfMissing.isFailed) {
    return;
  }
  auto bigint = this->readFlag(draft, 1, NodeOption::BIGINT);
  if (bigint.isFailed) {
    return;
  }
  if (bigint.value.value_or(false)) {
    Expr *option = this->optionOf(draft, 1, NodeOption::BIGINT);
    auto message = ErrorTextFsCall::bigint();

    return this->state.diagnostics.error(option->loc(), message);
  }
  bool isThrowIfMissing = throwIfMissing.value.value_or(true);
  auto *isFollowing = this->makeBoolLiteral(loc, draft.function == FsFunction::STAT_SYNC);
  auto *isThrowing = this->makeBoolLiteral(loc, isThrowIfMissing);
  Expr *file = call->args[0];
  call->args = {file, isFollowing, isThrowing};
  call->target = BuiltinCall{.builtin = Builtin::FsStat};
  const Type *stats = this->state.library.filesystem.getStatsClass()->type;
  call->type = isThrowIfMissing ? stats : Type::optional(stats);
}

void LibraryFsRegistry::startReaddir(const FsDraft &draft) {
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  if (!this->hasFsArity(draft, 1, 2)) {
    return;
  }
  tasks.pushTask(AfterReaddirOptions{.draft = draft});
  tasks.pushTask(ReadEncodingOrOptions{
      .call = call,
      .index = 1,
      .functionName = draft.functionName,
      .allowed = {NodeOption::ENCODING, NodeOption::WITH_FILE_TYPES, NodeOption::RECURSIVE},
  });

  return this->pushPathCheck(&call->args[0], FsPathForm::PATH_ONLY);
}

void LibraryFsRegistry::afterReaddirOptions(const AfterReaddirOptions &task) {
  const FsDraft &draft = task.draft;
  auto &tasks = this->state.walk.tasks;
  if (!tasks.takeVerdict()) {
    return;
  }
  Expr *given = this->givenEncoding(draft, 1);
  auto encoding = this->readEncoding(draft, given, EncodingSet::UTF8_ONLY);
  if (encoding.isFailed) {
    return;
  }
  auto withTypes = this->readFlag(draft, 1, NodeOption::WITH_FILE_TYPES);
  if (withTypes.isFailed) {
    return;
  }
  Expr *recursive = this->optionOf(draft, 1, NodeOption::RECURSIVE);
  auto finish = FinishReaddir{
      .draft = draft,
      .hasTypes = withTypes.value.value_or(false),
      .hasRecursive = recursive != nullptr,
  };
  if (recursive == nullptr) {
    return this->finishReaddir(finish);
  }
  tasks.pushTask(std::move(finish));

  return this->pushValueCheck(recursive, Type::boolTy());
}

void LibraryFsRegistry::finishReaddir(const FinishReaddir &task) {
  const FsDraft &draft = task.draft;
  CallExpr *call = draft.call;
  Expr *recursive = task.hasRecursive ? this->state.walk.tasks.takeChecked()
                                      : this->makeBoolLiteral(draft.loc, false);
  Expr *dir = call->args[0];
  call->args = {dir, recursive};
  call->target = BuiltinCall{.builtin = Builtin::FsReaddir};
  const Type *dirent = this->state.library.filesystem.getDirentClass()->type;
  call->type = Type::array(task.hasTypes ? dirent : Type::string());
}

void LibraryFsRegistry::startMkdir(const FsDraft &draft) {
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  if (!this->hasFsArity(draft, 1, 2)) {
    return;
  }
  bool isModeGiven = call->args.size() > 1 && !isa<ObjectLitExpr>(call->args[1]) &&
                     !isa<UndefinedExpr>(call->args[1]);
  tasks.pushTask(AfterMkdirOptions{.draft = draft, .isModeGiven = isModeGiven});
  if (!isModeGiven) {
    tasks.pushTask(ReadNodeOptions{
        .call = call,
        .index = 1,
        .functionName = draft.functionName,
        .allowed = {NodeOption::RECURSIVE, NodeOption::MODE},
    });
  }

  return this->pushPathCheck(&call->args[0], FsPathForm::PATH_ONLY);
}

void LibraryFsRegistry::afterMkdirOptions(const AfterMkdirOptions &task) {
  const FsDraft &draft = task.draft;
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  if (!task.isModeGiven && !tasks.takeVerdict()) {
    return;
  }
  Expr *mode = task.isModeGiven ? call->args[1] : this->optionOf(draft, 1, NodeOption::MODE);
  Expr *recursive = task.isModeGiven ? nullptr : this->optionOf(draft, 1, NodeOption::RECURSIVE);
  tasks.pushTask(FinishMkdir{
      .draft = draft,
      .hasMode = mode != nullptr,
      .hasRecursive = recursive != nullptr,
  });
  if (recursive != nullptr) {
    this->pushValueCheck(recursive, Type::boolTy());
  }
  if (mode != nullptr) {
    this->pushValueCheck(mode, Type::intTy());
  }
}

void LibraryFsRegistry::finishMkdir(const FinishMkdir &task) {
  const FsDraft &draft = task.draft;
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  Expr *recursive =
      task.hasRecursive ? tasks.takeChecked() : this->makeBoolLiteral(draft.loc, false);
  Expr *mode = task.hasMode ? tasks.takeChecked() : this->makeIntLiteral(draft.loc, kDirectoryMode);
  Expr *dir = call->args[0];
  call->args = {dir, recursive, mode};
  call->target = BuiltinCall{.builtin = Builtin::FsMkdir};
  call->type = Type::optional(Type::string());
}

void LibraryFsRegistry::startRmdir(const FsDraft &draft) {
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  if (!this->hasFsArity(draft, 1, 2)) {
    return;
  }
  tasks.pushTask(FinishRmdir{.draft = draft});

  return this->pushPathCheck(&call->args[0], FsPathForm::PATH_ONLY);
}

void LibraryFsRegistry::finishRmdir(const FinishRmdir &task) {
  CallExpr *call = task.draft.call;
  if (call->args.size() > 1 && !isa<UndefinedExpr>(call->args[1])) {
    auto message = ErrorTextFsCall::rmdir();
    this->state.diagnostics.error(call->args[1]->loc(), message);

    return this->state.walk.tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  Expr *dir = call->args[0];
  call->args = {dir};
  call->target = BuiltinCall{.builtin = Builtin::FsRmdir};
  call->type = Type::voidTy();
}

void LibraryFsRegistry::startRm(const FsDraft &draft) {
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  if (!this->hasFsArity(draft, 1, 2)) {
    return;
  }
  tasks.pushTask(AfterRmOptions{.draft = draft});
  tasks.pushTask(ReadNodeOptions{
      .call = call,
      .index = 1,
      .functionName = draft.functionName,
      .allowed = {NodeOption::RECURSIVE, NodeOption::FORCE, NodeOption::MAX_RETRIES,
                  NodeOption::RETRY_DELAY},
  });

  return this->pushPathCheck(&call->args[0], FsPathForm::PATH_ONLY);
}

void LibraryFsRegistry::afterRmOptions(const AfterRmOptions &task) {
  const FsDraft &draft = task.draft;
  auto &tasks = this->state.walk.tasks;
  if (!tasks.takeVerdict()) {
    return;
  }
  Expr *recursive = this->optionOf(draft, 1, NodeOption::RECURSIVE);
  Expr *force = this->optionOf(draft, 1, NodeOption::FORCE);
  tasks.pushTask(FinishRm{
      .draft = draft,
      .hasRecursive = recursive != nullptr,
      .hasForce = force != nullptr,
  });
  if (force != nullptr) {
    this->pushValueCheck(force, Type::boolTy());
  }
  if (recursive != nullptr) {
    this->pushValueCheck(recursive, Type::boolTy());
  }
  for (NodeOption retry : {NodeOption::RETRY_DELAY, NodeOption::MAX_RETRIES}) {
    Expr *value = this->optionOf(draft, 1, retry);
    if (value != nullptr) {
      tasks.pushTask(DiscardChecked{});
      this->pushValueCheck(value, Type::intTy());
    }
  }
}

void LibraryFsRegistry::finishRm(const FinishRm &task) {
  const FsDraft &draft = task.draft;
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  Expr *force = task.hasForce ? tasks.takeChecked() : this->makeBoolLiteral(draft.loc, false);
  Expr *recursive =
      task.hasRecursive ? tasks.takeChecked() : this->makeBoolLiteral(draft.loc, false);
  Expr *target = call->args[0];
  call->args = {target, recursive, force};
  call->target = BuiltinCall{.builtin = Builtin::FsRm};
  call->type = Type::voidTy();
}

void LibraryFsRegistry::startTwoPaths(const FsDraft &draft) {
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  if (draft.function == FsFunction::COPY_FILE_SYNC && call->args.size() == 3) {
    auto message = ErrorTextFsCall::copyMode(draft.functionName);
    this->state.diagnostics.error(call->args[2]->loc(), message);

    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  if (!this->hasFsArity(draft, 2, 2)) {
    return;
  }
  tasks.pushTask(FinishTwoPaths{.draft = draft});
  this->pushPathCheck(&call->args[1], FsPathForm::PATH_ONLY);

  return this->pushPathCheck(&call->args[0], FsPathForm::PATH_ONLY);
}

void LibraryFsRegistry::finishTwoPaths(const FinishTwoPaths &task) {
  CallExpr *call = task.draft.call;
  Expr *from = call->args[0];
  Expr *to = call->args[1];
  call->args = {from, to};
  call->target = BuiltinCall{.builtin = this->twoPathsBuiltin(task.draft.function)};
  call->type = Type::voidTy();
}

void LibraryFsRegistry::checkFsModuleCall(const CheckFsModuleCall &task) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterFsModuleCall{});

  return tasks.pushTask(
      CheckFsCall{.call = task.call, .functionName = task.functionName, .loc = task.loc});
}

void LibraryFsRegistry::afterFsModuleCall() {
  auto &tasks = this->state.walk.tasks;
  Expr *checked = tasks.takeChecked();
  tasks.pushChecked(checked);
  auto *done = dyn_cast<CallExpr>(checked);
  bool isTyped =
      done != nullptr && BuiltinGroups::isFsBuiltin(done->builtin()) && !done->type->isError();
  if (!isTyped) {
    return;
  }
  return this->pushFsErrors(done, Builtin::None);
}

void LibraryFsRegistry::pushFsErrors(CallExpr *call, Builtin async) {
  auto &tasks = this->state.walk.tasks;
  llvm::StringRef name = this->fsErrorName(call);
  tasks.pushTask(WrapFsResult{.call = call, .async = async});
  if (name.empty()) {
    return tasks.pushAnswer(Type::error());
  }

  return tasks.pushTask(NamedModuleType{.name = name});
}

void LibraryFsRegistry::wrapFsResult(const WrapFsResult &task) {
  CallExpr *call = task.call;
  const Type *errors = this->state.walk.tasks.takeAnswer();
  if (task.async == Builtin::None) {
    call->type = Type::result(call->type, errors);

    return;
  }
  call->target = BuiltinCall{.builtin = task.async};
  call->type = Type::promise(Type::result(call->type, errors));
}

void LibraryFsRegistry::checkTzIoCall(const CheckTzIoCall &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  tasks.pushChecked(call);
  if (this->state.library.common.hasSpread(call)) {
    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  auto arity = this->state.library.common.checkArity(call, 0, 1);
  if (arity.isErr()) {
    return this->state.diagnostics.error(task.loc, arity.error().message);
  }
  call->target = BuiltinCall{.builtin = Builtin::ReadLine};
  call->type = Type::optional(Type::string());
  if (call->args.empty()) {
    return;
  }

  return tasks.pushTask(CheckArgSlot{.slot = &call->args[0], .to = Type::string()});
}

void LibraryFsRegistry::checkFsValue(const CheckFsValue &task) {
  auto &tasks = this->state.walk.tasks;
  if (FsFunctionTable::find(task.name) != FsFunction::CONSTANTS) {
    return tasks.pushChecked(nullptr);
  }
  auto message = ErrorTextFsConstant::value();
  this->state.diagnostics.error(task.expr->loc(), message);

  return tasks.pushChecked(task.expr);
}

void LibraryFsRegistry::checkFsConstant(const CheckFsConstant &task) {
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  if (!this->isFsConstants(member->object)) {
    return tasks.pushChecked(nullptr);
  }
  auto value = FsConstantTable::find(member->name);
  if (value.has_value()) {
    auto *literal = this->makeIntLiteral(member->loc(), uint64_t(value.value()));

    return tasks.pushChecked(literal);
  }
  auto message = ErrorTextFsConstant::notYet(member->name);
  this->state.diagnostics.error(member->loc(), message);
  member->type = Type::error();

  return tasks.pushChecked(member);
}

void LibraryFsRegistry::rememberNativeError() {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterNativeError{});

  return tasks.pushTask(NativeErrorType{});
}

void LibraryFsRegistry::afterNativeError() {
  const Type *native = this->state.walk.tasks.takeAnswer();
  this->state.inventory.nativeError = native->isError() ? nullptr : native;
}

void LibraryFsRegistry::nativeErrorType() {
  auto &tasks = this->state.walk.tasks;
  auto *alias = this->state.library.declarations.findLibraryType(LibraryType::NATIVE_ERROR);
  if (alias == nullptr) {
    return tasks.pushAnswer(Type::error());
  }
  tasks.pushTask(AnswerAliasType{.alias = alias});

  return tasks.pushTask(ResolveAlias{.alias = alias});
}

void LibraryFsRegistry::checkStatsDate(const CheckStatsDate &task) {
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  bool isStatsDate = this->state.library.filesystem.isStatsClass(task.classDecl) &&
                     FsClassShape::isStatsDate(member->name);
  if (!isStatsDate) {
    return tasks.pushChecked(nullptr);
  }
  auto *date = this->state.library.common.builtinCall(member->loc(), member, Builtin::FsStatsDate,
                                                      {}, Type::date());

  return tasks.pushChecked(date);
}

void LibraryFsRegistry::checkFsClassMethod(const CheckFsClassMethod &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  auto kind = this->state.library.filesystem.isFileClass(task.classDecl)
                  ? FsClassShape::fileKindOf(member->name)
                  : std::nullopt;
  if (!kind.has_value()) {
    return tasks.pushChecked(nullptr);
  }
  tasks.pushChecked(call);
  if (this->state.library.common.hasSpread(call)) {
    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  auto arity = this->state.library.common.checkArity(call, 0, 0);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  auto *literal = this->makeIntLiteral(member->loc(), uint64_t(*kind));
  call->args = {literal};
  call->target = BuiltinCall{.builtin = Builtin::FsIsKind};
  call->type = Type::boolTy();
  member->type = Type::function({}, 0, false, Type::boolTy());
}

void LibraryFsRegistry::checkFsPromisesCall(const CheckFsPromisesCall &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  this->state.inventory.usesAsync = true;
  call->type = Type::error();
  auto function = this->checkPromiseFunction(task.functionName);
  if (function.isErr()) {
    this->state.diagnostics.error(task.loc, function.error().message);
    tasks.pushChecked(call);

    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  this->state.library.filesystem.enterPromiseCall();
  tasks.pushTask(
      AfterFsPromisesCall{.call = call, .functionName = task.functionName, .loc = task.loc});

  return tasks.pushTask(
      CheckFsCall{.call = call, .functionName = task.functionName, .loc = task.loc});
}

void LibraryFsRegistry::afterFsPromisesCall(const AfterFsPromisesCall &task) {
  CallExpr *call = task.call;
  this->state.library.filesystem.leavePromiseCall();
  if (call->type->isError()) {
    return;
  }
  Builtin async = this->asyncFsBuiltin(call->builtin());
  if (async == Builtin::None) {
    auto message = ErrorTextFsPromises::notYet(task.functionName);
    this->state.diagnostics.error(task.loc, message);
    call->type = Type::error();

    return;
  }

  return this->pushFsErrors(call, async);
}

Builtin LibraryFsRegistry::asyncFsBuiltin(Builtin sync) const {
  static const std::pair<Builtin, Builtin> waiting[] = {
      {Builtin::FsReadFile, Builtin::FspReadFile}, {Builtin::FsWriteFile, Builtin::FspWriteFile},
      {Builtin::FsAccess, Builtin::FspAccess},     {Builtin::FsChmod, Builtin::FspChmod},
      {Builtin::FsTruncate, Builtin::FspTruncate}, {Builtin::FsStat, Builtin::FspStat},
      {Builtin::FsReaddir, Builtin::FspReaddir},   {Builtin::FsMkdir, Builtin::FspMkdir},
      {Builtin::FsMkdtemp, Builtin::FspMkdtemp},   {Builtin::FsRmdir, Builtin::FspRmdir},
      {Builtin::FsUnlink, Builtin::FspUnlink},     {Builtin::FsRm, Builtin::FspRm},
      {Builtin::FsRename, Builtin::FspRename},     {Builtin::FsCopyFile, Builtin::FspCopyFile},
      {Builtin::FsReplace, Builtin::FspReplace},   {Builtin::FsRealpath, Builtin::FspRealpath}};
  for (const auto &[from, to] : waiting) {
    if (from == sync) {
      return to;
    }
  }

  return Builtin::None;
}

void LibraryFsRegistry::checkReadlineCall(const CheckReadlineCall &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  this->state.inventory.usesAsync = true;
  call->type = Type::error();
  tasks.pushChecked(call);
  auto function = this->checkReadlineFunction(task.functionName, task.module);
  if (function.isErr()) {
    this->state.diagnostics.error(task.loc, function.error().message);

    return this->pushNonStreamChecks(call);
  }
  if (this->state.library.common.hasSpread(call)) {
    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  auto arity = this->state.library.common.checkArity(call, 1, 2);
  if (arity.isErr()) {
    return this->state.diagnostics.error(task.loc, arity.error().message);
  }
  if (!isa<ObjectLitExpr>(call->args[0])) {
    auto finish = FinishReadline{.call = call, .module = task.module, .loc = task.loc};

    return this->finishReadline(finish);
  }
  tasks.pushTask(AfterReadlineOptions{.call = call, .module = task.module, .loc = task.loc});

  return tasks.pushTask(ReadNodeOptions{
      .call = call,
      .index = 0,
      .functionName = task.functionName,
      .allowed = {NodeOption::INPUT, NodeOption::OUTPUT, NodeOption::TERMINAL,
                  NodeOption::CRLF_DELAY, NodeOption::HISTORY_SIZE,
                  NodeOption::REMOVE_HISTORY_DUPLICATES, NodeOption::PROMPT, NodeOption::TAB_SIZE,
                  NodeOption::COMPLETER, NodeOption::ESCAPE_CODE_TIMEOUT, NodeOption::HISTORY},
  });
}

void LibraryFsRegistry::pushNonStreamChecks(CallExpr *call) {
  auto &common = this->state.library.common;
  for (Expr *&arg : llvm::reverse(call->args)) {
    bool isStream = common.isProcessMember(arg, ProcessMember::STDIN) ||
                    common.isProcessMember(arg, ProcessMember::STDOUT) ||
                    common.isProcessMember(arg, ProcessMember::STDERR);
    if (!isStream) {
      this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &arg, .expected = nullptr});
    }
  }
}

void LibraryFsRegistry::afterReadlineOptions(const AfterReadlineOptions &task) {
  auto &tasks = this->state.walk.tasks;
  if (!tasks.takeVerdict()) {
    return;
  }
  CallExpr *call = task.call;
  auto &common = this->state.library.common;
  tasks.pushTask(FinishReadline{.call = call, .module = task.module, .loc = task.loc});
  for (NodeOption ignored : {NodeOption::ESCAPE_CODE_TIMEOUT, NodeOption::TAB_SIZE,
                             NodeOption::REMOVE_HISTORY_DUPLICATES, NodeOption::HISTORY_SIZE,
                             NodeOption::CRLF_DELAY, NodeOption::TERMINAL}) {
    Expr *value = common.findNodeOption(call, 0, ignored);
    if (value != nullptr) {
      this->pushDroppedCheck(value);
    }
  }
}

void LibraryFsRegistry::finishReadline(const FinishReadline &task) {
  CallExpr *call = task.call;
  auto &common = this->state.library.common;
  bool isOptions = isa<ObjectLitExpr>(call->args[0]);
  Expr *input = isOptions ? common.findNodeOption(call, 0, NodeOption::INPUT) : call->args[0];
  Expr *output = isOptions ? common.findNodeOption(call, 0, NodeOption::OUTPUT)
                           : (call->args.size() > 1 ? call->args[1] : nullptr);
  if (isOptions) {
    for (NodeOption unsupported :
         {NodeOption::PROMPT, NodeOption::COMPLETER, NodeOption::HISTORY}) {
      Expr *value = common.findNodeOption(call, 0, unsupported);
      if (value != nullptr) {
        auto name = NodeOptionTable::getName(unsupported);
        auto message = ErrorTextReadline::option(name);

        return this->state.diagnostics.error(value->loc(), message);
      }
    }
  }
  if (isOptions && input == nullptr) {
    auto message = ErrorTextReadline::noInput();

    return this->state.diagnostics.error(call->args[0]->loc(), message);
  }
  auto stdinInput = this->checkStdinInput(input);
  if (stdinInput.isErr()) {
    return this->state.diagnostics.error(input->loc(), stdinInput.error().message);
  }
  auto written = this->outputStream(output);
  if (!written.has_value()) {
    return;
  }
  auto *where = this->makeIntLiteral(task.loc, uint64_t(*written));
  call->args = {where};
  call->target = BuiltinCall{.builtin = Builtin::RlCreate};
  Type::Runtime runtime = task.module == NodeModule::Readline ? Type::Runtime::Readline
                                                              : Type::Runtime::ReadlinePromises;
  call->type = Type::runtimeObject(runtime);
}

std::optional<int> LibraryFsRegistry::outputStream(const Expr *output) {
  auto &common = this->state.library.common;
  if (output == nullptr || isa<UndefinedExpr>(output)) {
    return 0;
  }
  if (common.isProcessMember(output, ProcessMember::STDOUT)) {
    return 1;
  }
  if (common.isProcessMember(output, ProcessMember::STDERR)) {
    return 2;
  }
  auto message = ErrorTextReadline::output();
  this->state.diagnostics.error(output->loc(), message);

  return std::nullopt;
}

LibraryFsRegistry::ResultInputIsStdin LibraryFsRegistry::checkStdinInput(const Expr *input) const {
  if (!this->state.library.common.isProcessMember(input, ProcessMember::STDIN)) {
    auto message = ErrorTextReadline::notStdin();

    return err(LibraryFsRegistryStdinInputError{
        .code = LibraryFsRegistryStdinInputErrorCode::NOT_STDIN,
        .message = message,
    });
  }

  return ok(input);
}

llvm::StringRef LibraryFsRegistry::fsErrorName(const CallExpr *call) const {
  Builtin builtin = call->builtin();
  if (builtin == Builtin::FsWriteFile) {
    return this->writeErrorName(call);
  }
  if (builtin == Builtin::FsStat) {
    return this->statErrorName(call);
  }
  if (builtin == Builtin::FsRm) {
    const Expr *force = this->argAt(call, 2);

    return this->literalTrue(force) ? "ForceRemoveError" : "RemoveError";
  }

  return this->plainFsErrorName(builtin);
}

llvm::StringRef LibraryFsRegistry::writeErrorName(const CallExpr *call) const {
  const Expr *flagArg = this->argAt(call, 2);
  auto flag = this->textOf(flagArg);
  bool isAppend = flag.find('a') != std::string::npos;
  bool isFresh = flag.find('x') != std::string::npos;
  if (isAppend) {
    return isFresh ? "AppendNewFileError" : "AppendFileError";
  }

  return isFresh ? "WriteNewFileError" : "WriteFileError";
}

llvm::StringRef LibraryFsRegistry::statErrorName(const CallExpr *call) const {
  const Expr *following = this->argAt(call, 1);
  const Expr *throwing = this->argAt(call, 2);
  bool isFollow = this->literalTrue(following);
  bool isThrowIfMissing = this->literalTrue(throwing);
  if (isFollow) {
    return isThrowIfMissing ? "StatusError" : "StatusIfExistsError";
  }

  return isThrowIfMissing ? "LinkStatusError" : "LinkStatusIfExistsError";
}

const Expr *LibraryFsRegistry::argAt(const CallExpr *call, size_t index) const {
  return index < call->args.size() ? call->args[index] : nullptr;
}

llvm::StringRef LibraryFsRegistry::plainFsErrorName(Builtin builtin) const {
  static const std::pair<Builtin, llvm::StringLiteral> names[] = {
      {Builtin::FsReadFile, "ReadFileError"},
      {Builtin::FsAccess, "AccessError"},
      {Builtin::FsChmod, "ChangeModeError"},
      {Builtin::FsTruncate, "TruncateError"},
      {Builtin::FsReaddir, "ReadDirectoryError"},
      {Builtin::FsMkdir, "MakeDirectoryError"},
      {Builtin::FsMkdtemp, "MakeTemporaryDirectoryError"},
      {Builtin::FsRmdir, "RemoveDirectoryError"},
      {Builtin::FsUnlink, "UnlinkError"},
      {Builtin::FsRename, "RenameError"},
      {Builtin::FsReplace, "ReplaceError"},
      {Builtin::FsCopyFile, "CopyFileError"},
      {Builtin::FsRealpath, "RealPathError"}};
  for (const auto &[from, name] : names) {
    if (from == builtin) {
      return name;
    }
  }

  return "";
}

LibraryFsRegistry::ResultOctalMode LibraryFsRegistry::checkOctalMode(llvm::StringRef text) const {
  unsigned mode = 0;
  if (text.empty() || text.getAsInteger(8, mode)) {
    auto message = ErrorTextFsCall::chmod();

    return err(LibraryFsRegistryOctalModeError{
        .code = LibraryFsRegistryOctalModeErrorCode::NOT_OCTAL,
        .message = message,
    });
  }

  return ok(mode);
}

BoolLitExpr *LibraryFsRegistry::makeBoolLiteral(llvm::SMLoc loc, bool value) {
  auto *literal = this->state.ast.make<BoolLitExpr>(loc, value);
  literal->type = Type::boolTy();

  return literal;
}

IntLitExpr *LibraryFsRegistry::makeIntLiteral(llvm::SMLoc loc, uint64_t value) {
  auto *literal = this->state.ast.make<IntLitExpr>(loc, value);
  literal->type = Type::intTy();

  return literal;
}

StringLitExpr *LibraryFsRegistry::makeStringLiteral(llvm::SMLoc loc, llvm::StringRef text) {
  auto units = ToUtf16::toUtf16(text);
  auto *literal = this->state.ast.make<StringLitExpr>(loc, units);
  literal->type = Type::string();

  return literal;
}

bool LibraryFsRegistry::isBytesData(const Type *type) const {
  auto isByteView = std::bind_front(&LibraryFsRegistry::isByteViewType, this);

  return type->isByteView() || (type->isUnion() && llvm::all_of(type->members(), isByteView));
}

bool LibraryFsRegistry::isByteViewType(const Type *type) const {
  return type->isByteView();
}

Builtin LibraryFsRegistry::pathCallBuiltin(FsFunction function) const {
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

const Type *LibraryFsRegistry::pathCallType(FsFunction function) const {
  if (function == FsFunction::EXISTS_SYNC) {
    return Type::boolTy();
  }
  if (function == FsFunction::UNLINK_SYNC) {
    return Type::voidTy();
  }

  return Type::string();
}

Expr *LibraryFsRegistry::octalModeArgument(Expr *arg, llvm::SMLoc loc) {
  const auto *literal = cast<StringLitExpr>(arg);
  auto text = ToUtf8::toUtf8(literal->value);
  auto mode = this->checkOctalMode(text);
  if (mode.isErr()) {
    this->state.diagnostics.error(arg->loc(), mode.error().message);
    return nullptr;
  }

  return this->makeIntLiteral(loc, mode.value());
}

Builtin LibraryFsRegistry::pathArgumentBuiltin(FsFunction function) const {
  if (function == FsFunction::ACCESS_SYNC) {
    return Builtin::FsAccess;
  }
  if (function == FsFunction::CHMOD_SYNC) {
    return Builtin::FsChmod;
  }

  return Builtin::FsTruncate;
}

bool LibraryFsRegistry::isFsConstants(const Expr *object) const {
  const auto *name = dyn_cast<NameExpr>(object);
  if (name != nullptr) {
    const auto *nodeName = this->state.findNodeName(name->name);
    return this->isFsModuleName(nodeName) &&
           FsFunctionTable::find(nodeName->name) == FsFunction::CONSTANTS;
  }
  const auto *inner = dyn_cast<MemberExpr>(object);
  const auto *owner = inner != nullptr ? dyn_cast<NameExpr>(inner->object) : nullptr;
  const NodeName *nodeName =
      owner != nullptr ? this->state.findNodeName(owner->name) : nullptr;

  return this->isFsModuleName(nodeName) && nodeName->name.empty() &&
         FsFunctionTable::find(inner->name) == FsFunction::CONSTANTS;
}

bool LibraryFsRegistry::isFsModuleName(const NodeName *nodeName) const {
  return nodeName != nullptr &&
         (nodeName->module == NodeModule::Fs || nodeName->module == NodeModule::FsPromises);
}

void LibraryFsRegistry::declareFsClasses() {
  ClassDecl *stats = this->declareFsClass(FsClassShape::find(FsClass::STATS));
  this->state.library.filesystem.rememberStatsClass(stats);
  ClassDecl *dirent = this->declareFsClass(FsClassShape::find(FsClass::DIRENT));
  this->state.library.filesystem.rememberDirentClass(dirent);
}

ClassDecl *LibraryFsRegistry::declareFsClass(const FsClassInfo &shape) {
  llvm::StringRef name = shape.name;
  auto *classDecl = this->state.ast.make<ClassDecl>(llvm::SMLoc(), name);
  classDecl->origin = ClassOrigin::BUILTIN;
  classDecl->stage = ClassStage::RESOLVED;
  classDecl->declared = true;
  this->state.declaration.classes.addClass(classDecl);
  auto *type = Type::classType(name, classDecl);
  classDecl->type = type;
  auto typeFields = std::vector<Field>();
  for (const FsFieldInfo &shapeField : shape.fields) {
    const Type *fieldType = this->fieldType(shapeField.type);
    ClassField field;
    field.name = shapeField.name;
    field.resolved = fieldType;
    field.access = classDecl->fields.size() < shape.privateFrom ? Access::Public : Access::Private;
    classDecl->fields.push_back(field);
    typeFields.push_back({.name = shapeField.name.str(), .type = fieldType});
  }
  type->defineFields(std::move(typeFields));

  return classDecl;
}

const Type *LibraryFsRegistry::fieldType(FsFieldType type) const {
  switch (type) {
    case FsFieldType::INT:
      return Type::intTy();
    case FsFieldType::DOUBLE:
      return Type::doubleTy();
    case FsFieldType::STRING:
      return Type::string();
  }

  return Type::error();
}

Builtin LibraryFsRegistry::twoPathsBuiltin(FsFunction function) const {
  if (function == FsFunction::RENAME_SYNC) {
    return Builtin::FsRename;
  }
  if (function == FsFunction::REPLACE_SYNC) {
    return Builtin::FsReplace;
  }

  return Builtin::FsCopyFile;
}

std::string LibraryFsRegistry::textOf(const Expr *expr) const {
  const StringLitExpr *literal = expr != nullptr ? dyn_cast<StringLitExpr>(expr) : nullptr;
  if (literal == nullptr) {
    return "";
  }

  return ToUtf8::toUtf8(literal->value);
}

bool LibraryFsRegistry::literalTrue(const Expr *expr) const {
  const BoolLitExpr *literal = expr != nullptr ? dyn_cast<BoolLitExpr>(expr) : nullptr;

  return literal != nullptr && literal->value;
}

LibraryFsRegistry::ResultPromiseFunction
LibraryFsRegistry::checkPromiseFunction(llvm::StringRef functionName) const {
  if (FsFunctionTable::find(functionName) == FsFunction::CONSTANTS) {
    auto message = ErrorTextFsPromises::constants();

    return err(LibraryFsRegistryPromiseFunctionError{
        .code = LibraryFsRegistryPromiseFunctionErrorCode::NOT_A_FUNCTION,
        .message = message,
    });
  }
  auto exported = FsExportTable::find(NodeModule::FsPromises, functionName);
  if (exported == FsExport::HANDLE_FUNCTION) {
    auto message = ErrorTextFsPromises::open();

    return err(LibraryFsRegistryPromiseFunctionError{
        .code = LibraryFsRegistryPromiseFunctionErrorCode::NO_FILE_HANDLE,
        .message = message,
    });
  }

  return ok(functionName);
}

LibraryFsRegistry::ResultReadlineUsed
LibraryFsRegistry::checkReadlineFunction(llvm::StringRef functionName, NodeModule module) const {
  if (FsExportTable::find(module, functionName) != FsExport::FUNCTION) {
    auto message = ErrorTextReadline::typeOnly(functionName, module != NodeModule::Readline);

    return err(LibraryFsRegistryReadlineFunctionError{
        .code = LibraryFsRegistryReadlineFunctionErrorCode::TYPE_ONLY,
        .message = message,
    });
  }

  return ok(functionName);
}

}
