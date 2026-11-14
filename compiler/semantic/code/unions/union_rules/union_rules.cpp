#include "compiler/semantic/code/unions/union_rules/union_rules.h"
#include "compiler/program/library/builtin_names/access_keyword_table/access_keyword_table.h"
#include "compiler/semantic/code/classes/code_classes_registry_errors.h"

namespace tz {

UnionRules::UnionRules(SemanticState &owner)
    : state(owner) {}

UnionRules::ResultMemberAccessible
UnionRules::checkAccess(const ClassDecl *classDecl, Access access, llvm::StringRef name) const {
  const ClassDecl *currentClass = this->state.walk.self.getCurrentClass();
  if (access == Access::Public || this->isSameClass(currentClass, classDecl)) {
    return ok(classDecl);
  }
  if (access == Access::Protected && this->isDerivedFrom(currentClass, classDecl)) {
    return ok(classDecl);
  }
  auto word = AccessKeywordTable::getName(access);
  if (access == Access::Protected) {
    auto message = ErrorTextAccess::hiddenProtected(name, word, classDecl->name);

    return err(CodeClassesRegistryCheckAccessError{
        .code = CodeClassesRegistryCheckAccessErrorCode::PROTECTED_MEMBER,
        .message = message,
    });
  }
  auto message = ErrorTextAccess::hiddenPrivate(name, word, classDecl->name);

  return err(CodeClassesRegistryCheckAccessError{
      .code = CodeClassesRegistryCheckAccessErrorCode::PRIVATE_MEMBER,
      .message = message,
  });
}

bool UnionRules::isSameClass(const ClassDecl *first, const ClassDecl *second) const {
  return first != nullptr && second != nullptr && first->staticOwner() == second->staticOwner();
}

bool UnionRules::isDerivedFrom(const ClassDecl *from, const ClassDecl *base) const {
  for (const ClassDecl *current = from; current != nullptr; current = current->parent) {
    if (this->isSameClass(current, base)) {
      return true;
    }
  }

  return false;
}

bool UnionRules::hasDataField(const ClassDecl *classDecl, llvm::StringRef name) const {
  const ClassField *declared = classDecl->findField(name);

  return declared != nullptr && declared->method() == nullptr;
}

ClassDecl *UnionRules::classOf(const Type *type) const {
  if (type == nullptr) {
    return nullptr;
  }
  const auto *classDecl = type->classDecl();

  return const_cast<ClassDecl *>(classDecl);
}

}
