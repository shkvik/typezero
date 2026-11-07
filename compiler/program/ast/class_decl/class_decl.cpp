#include "compiler/program/ast/class_decl/class_decl.h"

namespace tz {

bool ClassDecl::isGeneric() const {
  return !this->typeParams.empty() && !this->generic();
}

bool ClassDecl::isEmitted() const {
  return !this->isGeneric() && !this->checkOnly;
}

const ClassDecl *ClassDecl::staticOwner() const {
  const ClassDecl *original = this->generic();

  return original ? original : this;
}

ClassDecl *ClassDecl::staticOwner() {
  ClassDecl *original = this->generic();

  return original ? original : this;
}

ClassDecl *ClassDecl::generic() const {
  const GenericInstance *instance = this->generics.as<GenericInstance>();

  return instance ? instance->generic : nullptr;
}

llvm::ArrayRef<ClassDecl *> ClassDecl::instances() const {
  const GenericTemplate *templated = this->generics.as<GenericTemplate>();

  return templated ? llvm::ArrayRef<ClassDecl *>(templated->instances)
                   : llvm::ArrayRef<ClassDecl *>();
}

bool ClassDecl::library() const {
  return this->origin == ClassOrigin::LIBRARY;
}

bool ClassDecl::builtin() const {
  return this->origin == ClassOrigin::BUILTIN;
}

bool ClassDecl::expression() const {
  return this->syntax.is<ExpressionClass>();
}

ObjectLitExpr *ClassDecl::literal() const {
  const LiteralClass *made = this->syntax.as<LiteralClass>();

  return made ? made->literal : nullptr;
}

}
