#include "compiler/semantic/library/fs/fs_errors/fs_errors.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/library/builtin_modules/builtin_module_types/builtin_module_types.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"

using llvm::dyn_cast;

namespace tz {

FsErrors::FsErrors(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->builtinModuleTypes = &all.builtinModuleTypes;
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
      auto flag = this->textOf(arg(2));
      bool isAppend = flag.find('a') != std::string::npos;
      bool isFresh = flag.find('x') != std::string::npos;
      if (isAppend) {
        return this->named(isFresh ? "AppendNewFileError" : "AppendFileError");
      }
      return this->named(isFresh ? "WriteNewFileError" : "WriteFileError");
    }
    case Builtin::FsAccess:
      return this->named("AccessError");
    case Builtin::FsChmod:
      return this->named("ChangeModeError");
    case Builtin::FsTruncate:
      return this->named("TruncateError");
    case Builtin::FsStat: {
      bool isFollow = this->literalTrue(arg(1));
      bool isThrowIfMissing = this->literalTrue(arg(2));
      if (isFollow) {
        return this->named(isThrowIfMissing ? "StatusError" : "StatusIfExistsError");
      }
      return this->named(isThrowIfMissing ? "LinkStatusError" : "LinkStatusIfExistsError");
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
      return this->named(this->literalTrue(arg(2)) ? "ForceRemoveError" : "RemoveError");
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

std::string FsErrors::textOf(const Expr *expr) const {
  const StringLitExpr *literal = expr != nullptr ? dyn_cast<StringLitExpr>(expr) : nullptr;
  if (literal == nullptr) {
    return "";
  }

  return ToUtf8::toUtf8(literal->value);
}

bool FsErrors::literalTrue(const Expr *expr) const {
  const BoolLitExpr *literal = expr != nullptr ? dyn_cast<BoolLitExpr>(expr) : nullptr;

  return literal != nullptr && literal->value;
}

}
