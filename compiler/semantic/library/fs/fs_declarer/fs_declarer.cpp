#include "compiler/semantic/library/fs/fs_declarer/fs_declarer.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/library/process/process_checker/process_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

namespace tz {

FsDeclarer::FsDeclarer(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->callChecker = &all.callChecker;
  this->processChecker = &all.processChecker;
  this->argumentChecker = &all.argumentChecker;
}

std::optional<int> FsDeclarer::fileKind(llvm::StringRef method) const {
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

void FsDeclarer::declareFsClasses() {
  auto declare = [&](llvm::StringRef name,
                     llvm::ArrayRef<std::pair<llvm::StringRef, const Type *>> fields,
                     size_t privateFrom) {
    auto *classDecl = this->state.getAst().make<ClassDecl>(llvm::SMLoc(), name);
    classDecl->origin = ClassOrigin::BUILTIN;
    classDecl->stage = ClassStage::RESOLVED;
    classDecl->declared = true;
    this->state.getClassStore().addClass(classDecl);
    auto *type = Type::classType(name, classDecl);
    classDecl->type = type;
    auto typeFields = std::vector<Field>();
    for (const auto &[fieldName, fieldType] : fields) {
      ClassField field;
      field.name = fieldName;
      field.resolved = fieldType;
      field.access = classDecl->fields.size() < privateFrom ? Access::Public : Access::Private;
      classDecl->fields.push_back(field);
      typeFields.push_back({fieldName.str(), fieldType, false});
    }
    type->defineFields(std::move(typeFields));

    return classDecl;
  };
  const auto *d = Type::doubleTy();
  const auto *i = Type::intTy();
  ClassDecl *stats = declare("Stats",
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
  this->state.getFileSystemStore().rememberStatsClass(stats);
  ClassDecl *dirent = declare(
      "Dirent",
      {{"name", Type::string()}, {"parentPath", Type::string()}, {"Symbol(type)", Type::intTy()}},
      2);
  this->state.getFileSystemStore().rememberDirentClass(dirent);
}

Expr *FsDeclarer::checkStatsDate(MemberExpr *member, ClassDecl *classDecl) {
  static const llvm::StringLiteral dates[] = {"atime", "mtime", "ctime", "birthtime"};
  if (!this->state.getFileSystemStore().isStatsClass(classDecl) ||
      !llvm::is_contained(dates, member->name)) {
    return nullptr;
  }

  return this->processChecker->builtinCall(member->loc(), member, Builtin::FsStatsDate, {},
                                           Type::date());
}

bool FsDeclarer::isRuntimeClass(const ClassDecl *classDecl) const {
  return this->state.getFileSystemStore().isFileClass(classDecl);
}

Expr *FsDeclarer::checkFsClassMethod(CallExpr *call, MemberExpr *member, ClassDecl *classDecl) {
  if (!this->state.getFileSystemStore().isFileClass(classDecl)) {
    return nullptr;
  }
  auto kind = this->fileKind(member->name);
  if (!kind) {
    return nullptr;
  }
  bool isRejected = this->callChecker->rejectSpread(call);
  auto arity = isRejected ? ok(call) : this->argumentChecker->checkArity(call, 0, 0);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
  }
  if (isRejected || arity.isErr()) {
    return call;
  }
  auto *literal = this->state.getAst().make<IntLitExpr>(member->loc(), uint64_t(*kind));
  literal->type = Type::intTy();
  call->args = {literal};
  call->target = BuiltinCall{.builtin = Builtin::FsIsKind};
  call->type = Type::boolTy();
  member->type = Type::function({}, 0, false, Type::boolTy());

  return call;
}

}
