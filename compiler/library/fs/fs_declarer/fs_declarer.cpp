#include "compiler/library/fs/fs_declarer/fs_declarer.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/library/process/process_checker/process_checker.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"


namespace tz {

std::optional<int> FsDeclarer::fileKind(llvm::StringRef method) {
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

FsDeclarer::FsDeclarer(Checker &owner)
    : checker(owner) {}

void FsDeclarer::declareFsClasses() {
  auto declare = [&](llvm::StringRef name,
                     llvm::ArrayRef<std::pair<llvm::StringRef, const Type *>> fields,
                     size_t privateFrom) {
    auto *cls = this->checker.ctx_.make<ClassDecl>(llvm::SMLoc(), name);
    cls->origin = ClassOrigin::BUILTIN;
    cls->stage = ClassStage::RESOLVED;
    cls->declared = true;
    this->checker.classes_.push_back(cls);
    Type *type = Type::classType(name, cls);
    cls->type = type;
    std::vector<Field> typeFields;
    for (const auto &[fieldName, fieldType] : fields) {
      ClassField f;
      f.name = fieldName;
      f.resolved = fieldType;
      f.access = cls->fields.size() < privateFrom ? Access::Public : Access::Private;
      cls->fields.push_back(f);
      typeFields.push_back({fieldName.str(), fieldType, false});
    }
    type->defineFields(std::move(typeFields));

    return cls;
  };
  const Type *d = Type::doubleTy();
  const Type *i = Type::intTy();
  this->checker.statsClass_ = declare("Stats",
                                      {{"dev", i},
                                       {"mode", i},
                                       {"nlink", i},
                                       {"uid", i},
                                       {"gid", i},
                                       {"rdev", i},
                                       {"blksize", i},
                                       {"ino", i},
                                       {"size", i},
                                       {"blocks", i},
                                       {"atimeMs", d},
                                       {"mtimeMs", d},
                                       {"ctimeMs", d},
                                       {"birthtimeMs", d}},
                                      14);
  this->checker.direntClass_ = declare(
      "Dirent",
      {{"name", Type::string()}, {"parentPath", Type::string()}, {"Symbol(type)", Type::intTy()}},
      2);
}

Expr *FsDeclarer::checkStatsDate(MemberExpr *member, ClassDecl *cls) {
  static const llvm::StringLiteral dates[] = {"atime", "mtime", "ctime", "birthtime"};
  if (cls != this->checker.statsClass_ || !llvm::is_contained(dates, member->name)) {
    return nullptr;
  }

  return this->checker.processChecker().builtinCall(member->loc(), member, Builtin::FsStatsDate, {},
                                                    Type::date());
}

bool FsDeclarer::isRuntimeClass(const ClassDecl *cls) const {
  return cls && (cls == this->checker.statsClass_ || cls == this->checker.direntClass_);
}

Expr *FsDeclarer::checkFsClassMethod(CallExpr *call, MemberExpr *member, ClassDecl *cls) {
  if (cls != this->checker.statsClass_ && cls != this->checker.direntClass_) {
    return nullptr;
  }
  std::optional<int> kind = FsDeclarer::fileKind(member->name);
  if (!kind) {
    return nullptr;
  }
  bool rejected = this->checker.callChecker().rejectSpread(call);
  ArgumentChecker::ResultArityMatches arity =
      rejected ? ok(call) : this->checker.argumentChecker().checkArity(call, 0, 0);
  if (arity.isErr()) {
    this->checker.diags_.error(member->loc(), arity.error().message);
  }
  if (rejected || arity.isErr()) {
    return call;
  }
  auto *lit = this->checker.ctx_.make<IntLitExpr>(member->loc(), uint64_t(*kind));
  lit->type = Type::intTy();
  call->args = {lit};
  call->target = BuiltinCall{.builtin = Builtin::FsIsKind};
  call->type = Type::boolTy();
  member->type = Type::function({}, 0, false, Type::boolTy());

  return call;
}

}
