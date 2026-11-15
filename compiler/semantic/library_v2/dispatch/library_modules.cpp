#include "compiler/semantic/library_v2/dispatch/library_modules.h"
#include "compiler/program/library/async/timers_promises_function_table/timers_promises_function_table.h"
#include "compiler/program/library/builtin_modules/pending_export_table/pending_export_table.h"
#include "compiler/program/library/bytes/buffer_export_table/buffer_export_table.h"
#include "compiler/program/library/child_process/child_export_table/child_export_table.h"
#include "compiler/program/library/crypto/crypto_async_function_table/crypto_async_function_table.h"
#include "compiler/program/library/crypto/crypto_class_table/crypto_class_table.h"
#include "compiler/program/library/crypto/crypto_export_table/crypto_export_table.h"
#include "compiler/program/library/crypto/crypto_sync_function_table/crypto_sync_function_table.h"
#include "compiler/program/library/fs/fs_export_table/fs_export_table.h"
#include "compiler/program/library/fs/fs_function_table/fs_function_table.h"
#include "compiler/program/library/os/os_function_table/os_function_table.h"
#include "compiler/program/library/os/os_value_table/os_value_table.h"
#include "compiler/program/library/path/path_function_table/path_function_table.h"
#include "compiler/program/library/path/path_value_table/path_value_table.h"
#include "compiler/program/library/process/process_member_table/process_member_table.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

namespace {

const ProcessMember kExportedMembers[] = {
    ProcessMember::ARGV,      ProcessMember::ENV,    ProcessMember::EXIT,
    ProcessMember::EXIT_CODE, ProcessMember::CWD,    ProcessMember::PLATFORM,
    ProcessMember::EXEC_PATH, ProcessMember::STDIN,  ProcessMember::STDOUT,
    ProcessMember::STDERR,    ProcessMember::UPTIME, ProcessMember::PID};

}

LibraryModules::LibraryModules(SemanticState &owner)
    : state(owner) {}

LibraryModules::ResultExportFound LibraryModules::checkFsExport(llvm::StringRef name,
                                                                llvm::StringRef path) const {
  auto kind = FsExportTable::find(NodeModule::Fs, name);
  if (kind == FsExport::FUNCTION || kind == FsExport::VALUE) {
    return ok(name);
  }
  if (kind == FsExport::IN_TZ_FS) {
    auto message = ErrorTextFsExport::inTzFs(path, name);

    return err(LibraryModulesExportFoundError{
        .code = LibraryModulesExportFoundErrorCode::ELSEWHERE,
        .message = message,
    });
  }
  std::string sync = FsFunctionTable::getSyncName(name);
  bool hasSyncForm = FsExportTable::find(NodeModule::Fs, sync) == FsExport::FUNCTION;
  if (hasSyncForm && FsExportTable::isFunction(NodeModule::FsPromises, name)) {
    auto message = ErrorTextFsExport::callback(name, path, sync);

    return err(LibraryModulesExportFoundError{
        .code = LibraryModulesExportFoundErrorCode::CALLBACK_FORM,
        .message = message,
    });
  }
  if (hasSyncForm) {
    auto message = ErrorTextFsExport::syncOnly(name, path, sync);

    return err(LibraryModulesExportFoundError{
        .code = LibraryModulesExportFoundErrorCode::CALLBACK_FORM,
        .message = message,
    });
  }
  if (kind == FsExport::NOT_YET) {
    auto message = ErrorTextModuleExport::notYet(name, path);

    return err(LibraryModulesExportFoundError{
        .code = LibraryModulesExportFoundErrorCode::NOT_YET,
        .message = message,
    });
  }
  auto message = ErrorTextModuleExport::none(path, name);

  return err(LibraryModulesExportFoundError{
      .code = LibraryModulesExportFoundErrorCode::NO_EXPORT,
      .message = message,
  });
}

LibraryModules::ResultExportFound LibraryModules::checkPromisesExport(llvm::StringRef name,
                                                                      llvm::StringRef path) const {
  auto kind = FsExportTable::find(NodeModule::FsPromises, name);
  if (FsExportTable::isFunction(NodeModule::FsPromises, name) || kind == FsExport::VALUE) {
    return ok(name);
  }
  if (kind == FsExport::IN_TZ_FS) {
    auto message = ErrorTextFsExport::inTzFs(path, name);

    return err(LibraryModulesExportFoundError{
        .code = LibraryModulesExportFoundErrorCode::ELSEWHERE,
        .message = message,
    });
  }
  if (kind == FsExport::NOT_YET) {
    auto message = ErrorTextModuleExport::notYet(name, path);

    return err(LibraryModulesExportFoundError{
        .code = LibraryModulesExportFoundErrorCode::NOT_YET,
        .message = message,
    });
  }
  if (FsExportTable::find(NodeModule::Fs, name) == FsExport::FUNCTION) {
    auto message = ErrorTextFsExport::waiting(path, name);

    return err(LibraryModulesExportFoundError{
        .code = LibraryModulesExportFoundErrorCode::ELSEWHERE,
        .message = message,
    });
  }
  auto message = ErrorTextModuleExport::none(path, name);

  return err(LibraryModulesExportFoundError{
      .code = LibraryModulesExportFoundErrorCode::NO_EXPORT,
      .message = message,
  });
}

LibraryModules::ResultExportFound LibraryModules::checkReadlineExport(llvm::StringRef name,
                                                                      llvm::StringRef path,
                                                                      NodeModule module) const {
  auto kind = FsExportTable::find(module, name);
  if (kind == FsExport::FUNCTION || kind == FsExport::VALUE) {
    return ok(name);
  }
  if (kind == FsExport::NOT_YET) {
    auto message = ErrorTextFsExport::readline(name, path);

    return err(LibraryModulesExportFoundError{
        .code = LibraryModulesExportFoundErrorCode::NOT_YET,
        .message = message,
    });
  }
  auto message = ErrorTextModuleExport::none(path, name);

  return err(LibraryModulesExportFoundError{
      .code = LibraryModulesExportFoundErrorCode::NO_EXPORT,
      .message = message,
  });
}

LibraryModules::ResultExportFound LibraryModules::checkTzFsExport(llvm::StringRef name,
                                                                  llvm::StringRef path) const {
  if (FsExportTable::isFunction(NodeModule::TzFs, name)) {
    return ok(name);
  }
  bool isInNodeFs = FsExportTable::find(NodeModule::Fs, name) == FsExport::FUNCTION ||
                    FsExportTable::isFunction(NodeModule::FsPromises, name);
  auto message = ErrorTextFsExport::tzFs(path, name, isInNodeFs);

  return err(LibraryModulesExportFoundError{
      .code = LibraryModulesExportFoundErrorCode::NO_EXPORT,
      .message = message,
  });
}

LibraryModules::ResultExportFound LibraryModules::checkTzIoExport(llvm::StringRef name,
                                                                  llvm::StringRef path) const {
  if (FsExportTable::isFunction(NodeModule::TzIo, name)) {
    return ok(name);
  }
  auto message = ErrorTextFsExport::tzIo(path, name);

  return err(LibraryModulesExportFoundError{
      .code = LibraryModulesExportFoundErrorCode::NO_EXPORT,
      .message = message,
  });
}

LibraryModules::ResultExportFound LibraryModules::checkPathExport(llvm::StringRef name,
                                                                  llvm::StringRef path) const {
  bool isExported = PathFunctionTable::find(name) != nullptr ||
                    PathValueTable::find(name).has_value() ||
                    KnownModules::findSubmodule(NodeModule::Path, name).has_value();
  if (isExported) {
    return ok(name);
  }
  if (PendingExportTable::isListed(NodeModule::Path, name)) {
    auto message = ErrorTextModuleExport::notYet(name, path);

    return err(LibraryModulesExportFoundError{
        .code = LibraryModulesExportFoundErrorCode::NOT_YET,
        .message = message,
    });
  }
  auto message = ErrorTextModuleExport::none(path, name);

  return err(LibraryModulesExportFoundError{
      .code = LibraryModulesExportFoundErrorCode::NO_EXPORT,
      .message = message,
  });
}

LibraryModules::ResultExportFound LibraryModules::checkOsExport(llvm::StringRef name,
                                                                llvm::StringRef path) const {
  if (OsFunctionTable::find(name).has_value() || OsValueTable::find(name).has_value()) {
    return ok(name);
  }
  if (PendingExportTable::isListed(NodeModule::Os, name)) {
    auto message = ErrorTextModuleExport::notYet(name, path);

    return err(LibraryModulesExportFoundError{
        .code = LibraryModulesExportFoundErrorCode::NOT_YET,
        .message = message,
    });
  }
  auto message = ErrorTextModuleExport::none(path, name);

  return err(LibraryModulesExportFoundError{
      .code = LibraryModulesExportFoundErrorCode::NO_EXPORT,
      .message = message,
  });
}

LibraryModules::ResultExportFound LibraryModules::checkProcessExport(llvm::StringRef name,
                                                                     llvm::StringRef path) const {
  auto member = ProcessMemberTable::find(name);
  if (member.has_value() && llvm::is_contained(kExportedMembers, *member)) {
    return ok(name);
  }
  auto message = ErrorTextProcessExport::notYet(name, path);

  return err(LibraryModulesExportFoundError{
      .code = LibraryModulesExportFoundErrorCode::NOT_YET,
      .message = message,
  });
}

LibraryModules::ResultExportFound LibraryModules::checkBufferExport(llvm::StringRef name,
                                                                    llvm::StringRef path) const {
  auto exported = BufferExportTable::find(name);
  if (exported == BufferExport::BUFFER) {
    return ok(name);
  }
  if (exported == BufferExport::NOT_YET) {
    auto message = ErrorTextBufferExport::notYet(name, path);

    return err(LibraryModulesExportFoundError{
        .code = LibraryModulesExportFoundErrorCode::NOT_YET,
        .message = message,
    });
  }
  auto message = ErrorTextModuleExport::none(path, name);

  return err(LibraryModulesExportFoundError{
      .code = LibraryModulesExportFoundErrorCode::NO_EXPORT,
      .message = message,
  });
}

LibraryModules::ResultExportFound LibraryModules::checkTimersExport(llvm::StringRef name,
                                                                    llvm::StringRef path) const {
  if (TimersPromisesFunctionTable::find(name).has_value()) {
    return ok(name);
  }
  auto message = ErrorTextTimersExport::none(path, name);

  return err(LibraryModulesExportFoundError{
      .code = LibraryModulesExportFoundErrorCode::NO_EXPORT,
      .message = message,
  });
}

LibraryModules::ResultExportFound LibraryModules::checkChildExport(llvm::StringRef name,
                                                                   llvm::StringRef path) const {
  auto exported = ChildExportTable::find(name);
  if (exported == ChildExport::SYNC_FUNCTION) {
    return ok(name);
  }
  if (exported == ChildExport::ASYNC_FUNCTION) {
    auto message = ErrorTextChildExport::async(name, path);

    return err(LibraryModulesExportFoundError{
        .code = LibraryModulesExportFoundErrorCode::NOT_YET,
        .message = message,
    });
  }
  if (exported == ChildExport::FORK) {
    auto message = ErrorTextChildExport::fork(name, path);

    return err(LibraryModulesExportFoundError{
        .code = LibraryModulesExportFoundErrorCode::NOT_POSSIBLE,
        .message = message,
    });
  }
  if (exported == ChildExport::NOT_YET) {
    auto message = ErrorTextChildExport::notYet(name, path);

    return err(LibraryModulesExportFoundError{
        .code = LibraryModulesExportFoundErrorCode::NOT_YET,
        .message = message,
    });
  }
  auto message = ErrorTextModuleExport::none(path, name);

  return err(LibraryModulesExportFoundError{
      .code = LibraryModulesExportFoundErrorCode::NO_EXPORT,
      .message = message,
  });
}

LibraryModules::ResultExportFound LibraryModules::checkCryptoExport(llvm::StringRef name,
                                                                    llvm::StringRef path) const {
  const CryptoAsyncFunctionInfo *async = CryptoAsyncFunctionTable::find(name);
  bool isFunction = CryptoSyncFunctionTable::find(name).has_value() ||
                    (async != nullptr && async->module == NodeModule::Crypto);
  if (isFunction || CryptoClassTable::find(name).has_value()) {
    return ok(name);
  }
  auto exported = CryptoExportTable::find(name);
  if (exported == CryptoExport::WEBCRYPTO) {
    auto message = ErrorTextCryptoExport::webcrypto(name, path);

    return err(LibraryModulesExportFoundError{
        .code = LibraryModulesExportFoundErrorCode::ELSEWHERE,
        .message = message,
    });
  }
  if (exported == CryptoExport::SUBTLE) {
    auto message = ErrorTextCryptoExport::subtle(name, path);

    return err(LibraryModulesExportFoundError{
        .code = LibraryModulesExportFoundErrorCode::ELSEWHERE,
        .message = message,
    });
  }
  if (exported == CryptoExport::NOT_YET) {
    auto message = ErrorTextModuleExport::notYet(name, path);

    return err(LibraryModulesExportFoundError{
        .code = LibraryModulesExportFoundErrorCode::NOT_YET,
        .message = message,
    });
  }
  auto message = ErrorTextModuleExport::none(path, name);

  return err(LibraryModulesExportFoundError{
      .code = LibraryModulesExportFoundErrorCode::NO_EXPORT,
      .message = message,
  });
}

LibraryModules::ResultExportFound LibraryModules::checkTzCryptoExport(llvm::StringRef name,
                                                                      llvm::StringRef path) const {
  const CryptoAsyncFunctionInfo *async = CryptoAsyncFunctionTable::find(name);
  if (async != nullptr && async->module == NodeModule::TzCrypto) {
    return ok(name);
  }
  auto message = ErrorTextModuleExport::none(path, name);

  return err(LibraryModulesExportFoundError{
      .code = LibraryModulesExportFoundErrorCode::NO_EXPORT,
      .message = message,
  });
}

LibraryModules::ResultExportFound LibraryModules::checkModuleExport(NodeModule module,
                                                                    llvm::StringRef name,
                                                                    llvm::StringRef path) const {
  switch (module) {
    case NodeModule::Fs:
      return this->checkFsExport(name, path);
    case NodeModule::FsPromises:
      return this->checkPromisesExport(name, path);
    case NodeModule::Readline:
    case NodeModule::ReadlinePromises:
      return this->checkReadlineExport(name, path, module);
    case NodeModule::TzIo:
      return this->checkTzIoExport(name, path);
    case NodeModule::TzFs:
      return this->checkTzFsExport(name, path);
    case NodeModule::Path:
    case NodeModule::PathPosix:
    case NodeModule::PathWin32:
      return this->checkPathExport(name, path);
    case NodeModule::Os:
      return this->checkOsExport(name, path);
    case NodeModule::Process:
      return this->checkProcessExport(name, path);
    case NodeModule::Buffer:
      return this->checkBufferExport(name, path);
    case NodeModule::TimersPromises:
      return this->checkTimersExport(name, path);
    case NodeModule::ChildProcess:
      return this->checkChildExport(name, path);
    case NodeModule::Crypto:
      return this->checkCryptoExport(name, path);
    case NodeModule::TzCrypto:
      return this->checkTzCryptoExport(name, path);
  }
  llvm_unreachable("unknown node module");
}

LibraryModules::ResultModuleFound LibraryModules::checkExportFound(NodeModule module,
                                                                   llvm::StringRef name,
                                                                   llvm::StringRef path) const {
  if (this->findExported(module, name) != nullptr) {
    return ok(module);
  }
  auto found = this->checkModuleExport(module, name, path);
  if (found.isErr()) {
    std::string message = found.error().message;

    return err(LibraryModulesModuleFoundError{
        .code = LibraryModulesModuleFoundErrorCode::NOT_EXPORTED,
        .message = message,
    });
  }

  return ok(module);
}

TypeAliasDecl *LibraryModules::findExported(NodeModule module, llvm::StringRef name) const {
  if (!this->state.library.declarations.isModuleExport(module, name)) {
    return nullptr;
  }

  return this->state.library.declarations.findModuleAlias(name);
}

}
