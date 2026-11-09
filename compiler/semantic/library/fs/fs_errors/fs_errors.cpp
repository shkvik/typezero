#include "compiler/semantic/library/fs/fs_errors/fs_errors.h"
#include "compiler/program/library/fs/fs_error_types/fs_error_types.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/library/builtin_modules/builtin_module_types/builtin_module_types.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"

using llvm::dyn_cast;

namespace tz {

FsErrors::FsErrors(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->builtinModuleTypes = &all.getBuiltinModuleTypes();
}

void FsErrors::declare() {
  BuiltinModuleTypes &types = *this->builtinModuleTypes;
  if (types.declared("ReadFileError")) {
    return;
  }
  types.declare(BuiltinText::FILE_SYSTEM_ERRORS);
  for (llvm::StringRef type : FsErrorTypes::getTypeNames()) {
    if (type == "ReplaceError") {
      types.exportFrom(NodeModule::TzFs, type);
      continue;
    }
    types.exportFrom(NodeModule::Fs, type);
    types.exportFrom(NodeModule::FsPromises, type);
  }
  for (llvm::StringRef side : {"Read", "Write", "WriteNew"}) {
    for (llvm::StringRef reason : {"NotFound", "Exists", "WrongType", "Permission", "Busy", "Full",
                                   "Path", "System", "InvalidRange"}) {
      std::string name = (side + "StreamError" + reason).str();
      if (types.declared(name)) {
        types.exportFrom(NodeModule::Fs, name);
      }
    }
  }
}

const Type *FsErrors::named(llvm::StringRef name) {
  return this->builtinModuleTypes->named(name);
}

const Type *FsErrors::ofCall(const CallExpr *call) {
  auto arg = [&](size_t i) -> const Expr * {
    return i < call->args.size() ? call->args[i] : nullptr;
  };
  switch (call->builtin()) {
    case Builtin::FsReadFile:
      return this->named("ReadFileError");
    case Builtin::FsWriteFile: {
      std::string flag = FsErrors::textOf(arg(2));
      bool append = flag.find('a') != std::string::npos;
      bool fresh = flag.find('x') != std::string::npos;
      if (append) {
        return this->named(fresh ? "AppendNewFileError" : "AppendFileError");
      }
      return this->named(fresh ? "WriteNewFileError" : "WriteFileError");
    }
    case Builtin::FsAccess:
      return this->named("AccessError");
    case Builtin::FsChmod:
      return this->named("ChangeModeError");
    case Builtin::FsTruncate:
      return this->named("TruncateError");
    case Builtin::FsStat: {
      bool follow = FsErrors::literalTrue(arg(1));
      bool throwIfMissing = FsErrors::literalTrue(arg(2));
      if (follow) {
        return this->named(throwIfMissing ? "StatusError" : "StatusIfExistsError");
      }
      return this->named(throwIfMissing ? "LinkStatusError" : "LinkStatusIfExistsError");
    }
    case Builtin::FsReaddir:
      return this->named("ReadDirectoryError");
    case Builtin::FsMkdir:
      return this->named("MakeDirectoryError");
    case Builtin::FsMkdtemp:
      return this->named("MakeTemporaryDirectoryError");
    case Builtin::FsRmdir:
      return this->named("RemoveDirectoryError");
    case Builtin::FsUnlink:
      return this->named("UnlinkError");
    case Builtin::FsRm:
      return this->named(FsErrors::literalTrue(arg(2)) ? "ForceRemoveError" : "RemoveError");
    case Builtin::FsRename:
      return this->named("RenameError");
    case Builtin::FsReplace:
      return this->named("ReplaceError");
    case Builtin::FsCopyFile:
      return this->named("CopyFileError");
    case Builtin::FsRealpath:
      return this->named("RealPathError");
    default:
      return Type::error();
  }
}

std::string FsErrors::textOf(const Expr *expr) {
  const StringLitExpr *literal = expr != nullptr ? dyn_cast<StringLitExpr>(expr) : nullptr;
  if (literal == nullptr) {
    return "";
  }

  return ToUtf8::toUtf8(literal->value);
}

bool FsErrors::literalTrue(const Expr *expr) {
  const BoolLitExpr *literal = expr != nullptr ? dyn_cast<BoolLitExpr>(expr) : nullptr;

  return literal != nullptr && literal->value;
}

}
