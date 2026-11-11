#include "compiler/program/library/builtin_modules/error_export_table/error_export_table.h"
#include "compiler/program/library/child_process/child_error_names/child_error_names.h"
#include "compiler/program/library/crypto/crypto_error_types/crypto_error_types.h"
#include "compiler/program/library/fs/fs_error_types/fs_error_types.h"

namespace tz {

namespace {

const BuiltinText kTexts[] = {
    BuiltinText::CRYPTO_ERRORS,
    BuiltinText::FILE_SYSTEM_ERRORS,
    BuiltinText::CHILD_PROCESS_ERRORS,
};

const ErrorExportInfo kExports[] = {
    {NodeModule::Crypto, CryptoErrorTypes::getTypeNames},
    {NodeModule::Fs, FsErrorTypes::getNodeFsNames},
    {NodeModule::FsPromises, FsErrorTypes::getPromiseNames},
    {NodeModule::TzFs, FsErrorTypes::getTzFsNames},
    {NodeModule::ChildProcess, ChildErrorNames::getTypeNames},
};

}

llvm::ArrayRef<BuiltinText> ErrorExportTable::getTexts() {
  return kTexts;
}

llvm::ArrayRef<ErrorExportInfo> ErrorExportTable::getExports() {
  return kExports;
}

}
