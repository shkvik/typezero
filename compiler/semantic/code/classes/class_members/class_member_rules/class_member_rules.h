#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/classes/class_facts/class_facts.h"
#include "compiler/semantic/code/classes/code_classes_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <string>
#include <vector>

namespace tz {

class ClassMemberRules {
public:
  struct OwnedStaticField {
    ClassField *field;
    ClassDecl *owner;
  };
  using ResultFieldTypeIsKnown = Result<const Type *, CodeClassesRegistryFieldTypedError>;
  using ResultVariantOfType = Result<FuncSymbol *, CodeClassesRegistryVariantForTypeError>;
  using ResultFieldIsSetBefore = Result<llvm::StringRef, CodeClassesRegistryFieldSetError>;

  ClassMemberRules(SemanticState &owner, ClassFacts &sharedFacts);

  void pushVariantTypes(llvm::ArrayRef<FunctionDecl *> variants, const Type *expected);


  ResultFieldTypeIsKnown checkFieldTyped(const ClassField *field, llvm::StringRef name) const;
  ResultFieldIsSetBefore checkFieldSet(const ClassDecl *owner, llvm::StringRef name,
                                       bool isViaThis) const;


  NameExpr *classValueName(NameExpr *name, ClassDecl *classDecl);
  OwnedStaticField staticField(ClassDecl *classDecl, llvm::StringRef name) const;
  std::vector<const Type *> takeVariantTypes(llvm::ArrayRef<FunctionDecl *> variants,
                                             const Type *expected);
  ResultVariantOfType variantForType(llvm::ArrayRef<FunctionDecl *> variants,
                                     llvm::ArrayRef<const Type *> types, const Type *expected,
                                     const MemberExpr *member) const;

private:
  using ResultClassIsDeclared = Result<const ClassDecl *, CodeClassesRegistryClassDeclaredError>;

  bool isFittingCallback(const FuncSymbol *function, const Signature *callback) const;
  bool isFunctionWanted(llvm::ArrayRef<FunctionDecl *> variants, const Type *expected) const;


  std::string shownObject(const MemberExpr *member) const;
  std::string signatureText(const FunctionDecl *variant) const;


  ClassField *ownStaticField(ClassDecl *owner, llvm::StringRef name) const;
  ResultClassIsDeclared checkClassDeclared(const ClassDecl *classDecl) const;
  std::vector<FunctionDecl *> variantsOfType(llvm::ArrayRef<FunctionDecl *> variants,
                                             llvm::ArrayRef<const Type *> types,
                                             const Type *expected) const;

  ClassFacts &facts;
  SemanticState &state;
};

}
