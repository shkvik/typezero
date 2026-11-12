#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/classes/class_bases/class_bases_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class FsDeclarer;
class TypeDeclarer;
class TypeResolver;
class ClassDeclarer;
class ModuleDeclarer;
class EventMapInference;
class GenericClassInstantiator;

class ClassBases {
public:
  ClassBases(SemanticState &owner, SemanticParts &all);

  void resolveClassBases(ClassDecl *classDecl);
  void addBase(const Type *type, const Type *base);
  const ClassField *findField(const ClassDecl *classDecl, llvm::StringRef name,
                              const ClassDecl **owner = nullptr) const;
  std::vector<Field> interfaceFields(TypeAliasDecl *alias, std::vector<const Type *> &bases,
                                     std::vector<Overload> &overloads);

private:
  using ResultParentClassFound = Result<ClassDecl *, ClassBasesBaseClassError>;
  using ResultUnusedMemberName = Result<const Field *, ClassBasesNewFieldError>;
  using ResultObjectTypeAsBase = Result<const Type *, ClassBasesInterfaceBaseError>;
  using ResultFieldNarrowsDown = Result<const Field *, ClassBasesNarrowsFieldError>;
  using ResultFieldTypesAreOne = Result<const Field *, ClassBasesSameFieldTypeError>;
  using ResultImplementedShape = Result<const Type *, ClassBasesImplementedTypeError>;
  using ResultUnusedMethodName = Result<const Overload *, ClassBasesNewOverloadError>;
  using ResultAccessIsNotLower = Result<const ClassField *, ClassBasesAccessKeptError>;
  using ResultValueOnRedeclare = Result<const ClassField *, ClassBasesValueGivenError>;
  using ResultInterfaceNotSelf = Result<const TypeAliasDecl *, ClassBasesNotSelfError>;
  using ResultMethodSignatures = Result<const Overload *, ClassBasesSameSignaturesError>;
  using ResultOverrideTargeted = Result<const ClassField *, ClassBasesOverrideTargetError>;
  using ResultInheritedVisible = Result<const ClassField *, ClassBasesInheritedVisibleError>;

  struct ParentLookup {
    ClassDecl *parent;
    Stmt *found;
    bool isInstance;
  };

  struct InterfaceShape {
    std::vector<Field> fields;
    std::vector<std::string> origins;
  };

  bool isErrorClass(llvm::StringRef name) const;
  bool isUninstantiatedGeneric(const FunctionDecl *functionDecl) const;
  ResultParentClassFound checkBaseClass(const ClassDecl *classDecl, const TypeRef &ref,
                                        bool isNamed, const ParentLookup &lookup) const;
  ResultParentClassFound checkUsableParent(const ClassDecl *classDecl, ClassDecl *parent) const;
  ResultParentClassFound checkMissingParent(const ClassDecl *classDecl, const TypeRef &ref,
                                            const Stmt *found) const;
  ResultImplementedShape checkImplementedType(const Type *type, const ClassDecl *classDecl) const;
  ResultOverrideTargeted checkOverrideTarget(const ClassDecl *classDecl,
                                             const ClassField &field) const;
  ResultInheritedVisible checkInheritedVisible(const ClassField &field, const ClassField *inherited,
                                               const ClassDecl *owner) const;
  ResultAccessIsNotLower checkAccessKept(const ClassField &field, const ClassField *inherited,
                                         const ClassDecl *owner, const ClassDecl *classDecl) const;
  ResultValueOnRedeclare checkValueGiven(const ClassDecl *classDecl, const ClassField &field,
                                         const ClassDecl *owner) const;
  void resolveParentClass(ClassDecl *classDecl, const TypeRef &ref);
  ParentLookup parentOf(ClassDecl *classDecl, const TypeRef &ref, Stmt *found);
  ParentLookup emitterParent(ClassDecl *classDecl, const TypeRef &ref, ClassDecl *emitter,
                             Stmt *found);
  void linkParent(ClassDecl *classDecl, ClassDecl *parent);
  void addImplemented(ClassDecl *classDecl, const TypeRef &ref);
  void walkRedeclaredField(ClassDecl *classDecl, ClassField &field);
  ResultInterfaceNotSelf checkNotSelf(const TypeAliasDecl *alias, const TypeAliasDecl *other) const;
  ResultObjectTypeAsBase checkInterfaceBase(const Type *base) const;
  ResultMethodSignatures checkSameSignatures(const TypeAliasDecl *alias, const Overload &known,
                                             const Overload &other) const;
  ResultFieldTypesAreOne checkSameFieldType(const TypeAliasDecl *alias, const Field &known,
                                            const Field &field, llvm::StringRef origin,
                                            llvm::StringRef baseName) const;
  ResultUnusedMethodName checkNewOverload(const TypeAliasDecl *alias, const Overload &overload,
                                          llvm::ArrayRef<Field> fields,
                                          llvm::ArrayRef<Overload> overloads) const;
  ResultUnusedMemberName checkNewField(const TypeAliasDecl *alias, const Field &field,
                                       llvm::ArrayRef<Overload> overloads) const;
  ResultFieldNarrowsDown checkNarrowsField(const TypeAliasDecl *alias, const Field &field,
                                           const Field &known, llvm::StringRef origin) const;
  const Type *interfaceBaseType(const TypeAliasDecl *alias, const TypeRef &ref,
                                llvm::ArrayRef<const Type *> bases);
  void mergeBaseOverloads(const TypeAliasDecl *alias, const TypeRef &ref, const Type *base,
                          std::vector<Overload> &overloads);
  void walkSameSignatures(const TypeAliasDecl *alias, const TypeRef &ref, const Overload &known,
                          const Overload &overload);
  const Overload *overloadNamed(llvm::ArrayRef<Overload> overloads, llvm::StringRef name) const;
  size_t fieldIndex(llvm::ArrayRef<Field> fields, llvm::StringRef name) const;
  void mergeBaseFields(const TypeAliasDecl *alias, const TypeRef &ref, const Type *base,
                       InterfaceShape &shape);
  void mergeBaseField(const TypeAliasDecl *alias, const TypeRef &ref, const Field &field,
                      const std::string &baseName, InterfaceShape &shape);
  void addOwnField(const TypeAliasDecl *alias, const Field &field,
                   llvm::ArrayRef<Overload> overloads, InterfaceShape &shape);
  llvm::SMLoc ownFieldLoc(const TypeAliasDecl *alias, llvm::StringRef name) const;

  SemanticState &state;
  FsDeclarer *fsDeclarer = nullptr;
  TypeDeclarer *typeDeclarer = nullptr;
  TypeResolver *typeResolver = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  ModuleDeclarer *moduleDeclarer = nullptr;
  EventMapInference *eventMapInference = nullptr;
  GenericClassInstantiator *genericClassInstantiator = nullptr;
};

}
