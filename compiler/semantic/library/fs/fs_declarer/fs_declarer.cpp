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

void FsDeclarer::declareFsClasses() {
  ClassDecl *stats = this->declareFsClass(FsClassShape::find(FsClass::STATS));
  this->state.getFileSystemStore().rememberStatsClass(stats);
  ClassDecl *dirent = this->declareFsClass(FsClassShape::find(FsClass::DIRENT));
  this->state.getFileSystemStore().rememberDirentClass(dirent);
}

ClassDecl *FsDeclarer::declareFsClass(const FsClassInfo &shape) {
  llvm::StringRef name = shape.name;
  auto *classDecl = this->state.getAst().make<ClassDecl>(llvm::SMLoc(), name);
  classDecl->origin = ClassOrigin::BUILTIN;
  classDecl->stage = ClassStage::RESOLVED;
  classDecl->declared = true;
  this->state.getClassStore().addClass(classDecl);
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

const Type *FsDeclarer::fieldType(FsFieldType type) const {
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

Expr *FsDeclarer::checkStatsDate(MemberExpr *member, ClassDecl *classDecl) {
  if (!this->state.getFileSystemStore().isStatsClass(classDecl) ||
      !FsClassShape::isStatsDate(member->name)) {
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
  auto kind = FsClassShape::fileKindOf(member->name);
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
