#include "compiler/semantic/library/fs/fs_exports/fs_exports.h"
#include "compiler/program/library/fs/fs_export_table/fs_export_table.h"
#include "compiler/program/library/fs/fs_function_table/fs_function_table.h"

namespace tz {

FsExports::ResultExportFound FsExports::checkFsExport(llvm::StringRef name, llvm::StringRef path) {
  auto kind = FsExportTable::find(NodeModule::Fs, name);
  if (kind == FsExport::FUNCTION || kind == FsExport::VALUE) {
    return ok(name);
  }
  if (kind == FsExport::IN_TZ_FS) {
    auto message = ErrorTextFsExport::inTzFs(path, name);

    return err(ModuleExportFoundError{
        .code = ModuleExportFoundErrorCode::ELSEWHERE,
        .message = message,
    });
  }
  std::string sync = FsFunctionTable::getSyncName(name);
  bool hasSyncForm = FsExportTable::find(NodeModule::Fs, sync) == FsExport::FUNCTION;
  if (hasSyncForm && FsExportTable::isFunction(NodeModule::FsPromises, name)) {
    auto message = ErrorTextFsExport::callback(name, path, sync);

    return err(ModuleExportFoundError{
        .code = ModuleExportFoundErrorCode::CALLBACK_FORM,
        .message = message,
    });
  }
  if (hasSyncForm) {
    auto message = ErrorTextFsExport::syncOnly(name, path, sync);

    return err(ModuleExportFoundError{
        .code = ModuleExportFoundErrorCode::CALLBACK_FORM,
        .message = message,
    });
  }
  if (kind == FsExport::NOT_YET) {
    auto message = ErrorTextModuleExport::notYet(name, path);

    return err(ModuleExportFoundError{
        .code = ModuleExportFoundErrorCode::NOT_YET,
        .message = message,
    });
  }
  auto message = ErrorTextModuleExport::none(path, name);

  return err(ModuleExportFoundError{
      .code = ModuleExportFoundErrorCode::NO_EXPORT,
      .message = message,
  });
}

FsExports::ResultExportFound FsExports::checkPromisesExport(llvm::StringRef name,
                                                            llvm::StringRef path) {
  auto kind = FsExportTable::find(NodeModule::FsPromises, name);
  if (FsExportTable::isFunction(NodeModule::FsPromises, name) || kind == FsExport::VALUE) {
    return ok(name);
  }
  if (kind == FsExport::IN_TZ_FS) {
    auto message = ErrorTextFsExport::inTzFs(path, name);

    return err(ModuleExportFoundError{
        .code = ModuleExportFoundErrorCode::ELSEWHERE,
        .message = message,
    });
  }
  if (kind == FsExport::NOT_YET) {
    auto message = ErrorTextModuleExport::notYet(name, path);

    return err(ModuleExportFoundError{
        .code = ModuleExportFoundErrorCode::NOT_YET,
        .message = message,
    });
  }
  if (FsExportTable::find(NodeModule::Fs, name) == FsExport::FUNCTION) {
    auto message = ErrorTextFsExport::waiting(path, name);

    return err(ModuleExportFoundError{
        .code = ModuleExportFoundErrorCode::ELSEWHERE,
        .message = message,
    });
  }
  auto message = ErrorTextModuleExport::none(path, name);

  return err(ModuleExportFoundError{
      .code = ModuleExportFoundErrorCode::NO_EXPORT,
      .message = message,
  });
}

FsExports::ResultExportFound
FsExports::checkReadlineExport(llvm::StringRef name, llvm::StringRef path, NodeModule module) {
  auto kind = FsExportTable::find(module, name);
  if (kind == FsExport::FUNCTION || kind == FsExport::VALUE) {
    return ok(name);
  }
  if (kind == FsExport::NOT_YET) {
    auto message = ErrorTextFsExport::readline(name, path);

    return err(ModuleExportFoundError{
        .code = ModuleExportFoundErrorCode::NOT_YET,
        .message = message,
    });
  }
  auto message = ErrorTextModuleExport::none(path, name);

  return err(ModuleExportFoundError{
      .code = ModuleExportFoundErrorCode::NO_EXPORT,
      .message = message,
  });
}

FsExports::ResultExportFound FsExports::checkTzFsExport(llvm::StringRef name,
                                                        llvm::StringRef path) {
  if (FsExportTable::isFunction(NodeModule::TzFs, name)) {
    return ok(name);
  }
  bool isInNodeFs = FsExportTable::find(NodeModule::Fs, name) == FsExport::FUNCTION ||
                    FsExportTable::isFunction(NodeModule::FsPromises, name);
  auto message = ErrorTextFsExport::tzFs(path, name, isInNodeFs);

  return err(ModuleExportFoundError{
      .code = ModuleExportFoundErrorCode::NO_EXPORT,
      .message = message,
  });
}

FsExports::ResultExportFound FsExports::checkTzIoExport(llvm::StringRef name,
                                                        llvm::StringRef path) {
  if (FsExportTable::isFunction(NodeModule::TzIo, name)) {
    return ok(name);
  }
  auto message = ErrorTextFsExport::tzIo(path, name);

  return err(ModuleExportFoundError{
      .code = ModuleExportFoundErrorCode::NO_EXPORT,
      .message = message,
  });
}

}
