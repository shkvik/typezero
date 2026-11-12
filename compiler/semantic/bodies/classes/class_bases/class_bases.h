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

  const char *accessName(Access access) const;
  bool isErrorClass(llvm::StringRef name) const;
  ResultObjectTypeAsBase interfaceBase(const Type *base) const;
  ResultImplementedShape implementedType(const Type *type, const ClassDecl *classDecl) const;
  ResultOverrideTargeted overrideTarget(const ClassDecl *classDecl, const ClassField &field) const;
  ResultValueOnRedeclare valueGiven(const ClassDecl *classDecl, const ClassField &field,
                                    const ClassDecl *owner) const;
  ResultInterfaceNotSelf notSelf(const TypeAliasDecl *alias, const TypeAliasDecl *other) const;
  ResultMethodSignatures sameSignatures(const TypeAliasDecl *alias, const Overload &known,
                                        const Overload &other) const;
  ResultUnusedMemberName newField(const TypeAliasDecl *alias, const Field &field,
                                  llvm::ArrayRef<Overload> overloads) const;
  ResultInheritedVisible inheritedVisible(const ClassField &field, const ClassField *inherited,
                                          const ClassDecl *owner) const;
  ResultFieldNarrowsDown narrowsField(const TypeAliasDecl *alias, const Field &field,
                                      const Field &known, llvm::StringRef origin) const;
  ResultAccessIsNotLower accessKept(const ClassField &field, const ClassField *inherited,
                                    const ClassDecl *owner, const ClassDecl *classDecl) const;
  ResultParentClassFound baseClass(const ClassDecl *classDecl, const TypeRef &ref, bool named,
                                   bool instance, ClassDecl *parent, const Stmt *found) const;
  ResultParentClassFound usableParent(const ClassDecl *classDecl, ClassDecl *parent) const;
  ResultParentClassFound missingParent(const ClassDecl *classDecl, const TypeRef &ref,
                                       const Stmt *found) const;
  ResultUnusedMethodName newOverload(const TypeAliasDecl *alias, const Overload &overload,
                                     llvm::ArrayRef<Field> fields,
                                     llvm::ArrayRef<Overload> overloads) const;
  ResultFieldTypesAreOne sameFieldType(const TypeAliasDecl *alias, const Field &known,
                                       const Field &field, llvm::StringRef origin,
                                       llvm::StringRef baseName) const;

  struct ParentLookup {
    ClassDecl *parent;
    Stmt *found;
    bool instance;
  };

  void resolveParentClass(ClassDecl *classDecl, const TypeRef &ref);
  ParentLookup parentOf(ClassDecl *classDecl, const TypeRef &ref, Stmt *found);
  ParentLookup emitterParent(ClassDecl *classDecl, const TypeRef &ref, ClassDecl *emitter,
                             Stmt *found);
  void linkParent(ClassDecl *classDecl, ClassDecl *parent);
  void addImplemented(ClassDecl *classDecl, const TypeRef &ref);
  void walkRedeclaredField(ClassDecl *classDecl, ClassField &field);

  struct InterfaceShape {
    std::vector<Field> fields;
    std::vector<std::string> origins;
  };

  const Type *interfaceBaseType(const TypeAliasDecl *alias, const TypeRef &ref,
                                llvm::ArrayRef<const Type *> bases);
  void mergeBaseOverloads(const TypeAliasDecl *alias, const TypeRef &ref, const Type *base,
                          std::vector<Overload> &overloads);
  const Overload *overloadNamed(const std::vector<Overload> &overloads, llvm::StringRef name) const;
  size_t fieldIndex(const std::vector<Field> &fields, llvm::StringRef name) const;
  void mergeBaseFields(const TypeAliasDecl *alias, const TypeRef &ref, const Type *base,
                       InterfaceShape &shape);
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
