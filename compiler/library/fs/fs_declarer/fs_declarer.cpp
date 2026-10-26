#include "compiler/library/fs/fs_declarer/fs_declarer.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/library/process/process_checker/process_checker.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

std::optional<int> fileKind(llvm::StringRef method) {
  if (method == "isFile") {
    return 0100000;
  }
  if (method == "isDirectory") {
    return 0040000;
  }
  if (method == "isSymbolicLink") {
    return 0120000;
  }
  if (method == "isFIFO") {
    return 0010000;
  }
  if (method == "isSocket") {
    return 0140000;
  }
  if (method == "isBlockDevice") {
    return 0060000;
  }
  if (method == "isCharacterDevice") {
    return 0020000;
  }

  return std::nullopt;
}

}

FsDeclarer::FsDeclarer(Checker &checker) : checker_(checker) {}

void FsDeclarer::declareFsClasses() {
  auto declare = [&](llvm::StringRef name,
                     llvm::ArrayRef<std::pair<llvm::StringRef, const Type *>> fields) {
    auto *cls = checker_.ctx_.make<ClassDecl>(llvm::SMLoc(), name);
    cls->builtin = true;
    cls->resolved = cls->declared = true;
    checker_.classes_.push_back(cls);
    Type *type = Type::classType(name, cls);
    cls->type = type;
    std::vector<Field> typeFields;
    for (const auto &[fieldName, fieldType] : fields) {
      ClassField f;
      f.name = fieldName;
      f.resolved = fieldType;
      f.access = fieldName.starts_with("#") ? Access::Private : Access::Public;
      cls->fields.push_back(f);
      typeFields.push_back({fieldName.str(), fieldType, false});
    }
    type->defineFields(std::move(typeFields));

    return cls;
  };
  const Type *d = Type::doubleTy();
  checker_.statsClass_ = declare("Stats", {{"dev", d},
                                           {"mode", d},
                                           {"nlink", d},
                                           {"uid", d},
                                           {"gid", d},
                                           {"rdev", d},
                                           {"blksize", d},
                                           {"ino", d},
                                           {"size", d},
                                           {"blocks", d},
                                           {"atimeMs", d},
                                           {"mtimeMs", d},
                                           {"ctimeMs", d},
                                           {"birthtimeMs", d}});
  checker_.direntClass_ =
      declare("Dirent",
              {{"name", Type::string()}, {"parentPath", Type::string()}, {"#type", Type::intTy()}});
}

Expr *FsDeclarer::checkStatsDate(MemberExpr *member, ClassDecl *cls) {
  static const llvm::StringLiteral dates[] = {"atime", "mtime", "ctime", "birthtime"};
  if (cls != checker_.statsClass_ || !llvm::is_contained(dates, member->name)) {
    return nullptr;
  }

  return checker_.processChecker().builtinCall(member->loc(), member, Builtin::FsStatsDate, {},
                                               Type::date());
}

bool FsDeclarer::isRuntimeClass(const ClassDecl *cls) const {
  return cls && (cls == checker_.systemErrorClass_ || cls == checker_.statsClass_ ||
                 cls == checker_.direntClass_);
}

Expr *FsDeclarer::checkFsClassMethod(CallExpr *call, MemberExpr *member, ClassDecl *cls) {
  if (cls != checker_.statsClass_ && cls != checker_.direntClass_) {
    return nullptr;
  }
  std::optional<int> kind = fileKind(member->name);
  if (!kind) {
    return nullptr;
  }
  if (checker_.callChecker().rejectSpread(call) ||
      !checker_.argumentChecker().checkArity(call, member->loc(), 0, 0)) {
    return call;
  }
  auto *lit = checker_.ctx_.make<IntLitExpr>(member->loc(), uint64_t(*kind));
  lit->type = Type::intTy();
  call->args = {lit};
  call->builtin = Builtin::FsIsKind;
  call->type = Type::boolTy();
  member->type = Type::function({}, 0, false, Type::boolTy());

  return call;
}

}
