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

  void resolveClassBases(ClassDecl *cls);
  void addBase(const Type *type, const Type *base);
  static const ClassField *findField(const ClassDecl *cls, llvm::StringRef name,
                                     const ClassDecl **owner = nullptr);
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

  static const char *accessName(Access access);
  static bool isErrorClass(llvm::StringRef name);
  static ResultObjectTypeAsBase interfaceBase(const Type *base);
  static ResultImplementedShape implementedType(const Type *type, const ClassDecl *cls);
  static ResultOverrideTargeted overrideTarget(const ClassDecl *cls, const ClassField &field);
  static ResultValueOnRedeclare valueGiven(const ClassDecl *cls, const ClassField &field,
                                           const ClassDecl *owner);
  static ResultInterfaceNotSelf notSelf(const TypeAliasDecl *alias, const TypeAliasDecl *other);
  static ResultMethodSignatures sameSignatures(const TypeAliasDecl *alias, const Overload &known,
                                               const Overload &other);
  static ResultUnusedMemberName newField(const TypeAliasDecl *alias, const Field &field,
                                         llvm::ArrayRef<Overload> overloads);
  static ResultInheritedVisible
  inheritedVisible(const ClassField &field, const ClassField *inherited, const ClassDecl *owner);
  static ResultFieldNarrowsDown narrowsField(const TypeAliasDecl *alias, const Field &field,
                                             const Field &known, llvm::StringRef origin);
  static ResultAccessIsNotLower accessKept(const ClassField &field, const ClassField *inherited,
                                           const ClassDecl *owner, const ClassDecl *cls);
  ResultParentClassFound baseClass(const ClassDecl *cls, const TypeRef &ref, bool named,
                                   bool instance, ClassDecl *parent, const Stmt *found) const;
  ResultParentClassFound usableParent(const ClassDecl *cls, ClassDecl *parent) const;
  static ResultParentClassFound missingParent(const ClassDecl *cls, const TypeRef &ref,
                                              const Stmt *found);
  static ResultUnusedMethodName newOverload(const TypeAliasDecl *alias, const Overload &overload,
                                            llvm::ArrayRef<Field> fields,
                                            llvm::ArrayRef<Overload> overloads);
  static ResultFieldTypesAreOne sameFieldType(const TypeAliasDecl *alias, const Field &known,
                                              const Field &field, llvm::StringRef origin,
                                              llvm::StringRef baseName);

  struct ParentLookup {
    ClassDecl *parent;
    Stmt *found;
    bool instance;
  };

  void resolveParentClass(ClassDecl *cls, const TypeRef &ref);
  ParentLookup parentOf(ClassDecl *cls, const TypeRef &ref, Stmt *found);
  ParentLookup emitterParent(ClassDecl *cls, const TypeRef &ref, ClassDecl *emitter, Stmt *found);
  void linkParent(ClassDecl *cls, ClassDecl *parent);
  void addImplemented(ClassDecl *cls, const TypeRef &ref);
  void walkRedeclaredField(ClassDecl *cls, ClassField &f);

  struct InterfaceShape {
    std::vector<Field> fields;
    std::vector<std::string> origins;
  };

  const Type *interfaceBaseType(const TypeAliasDecl *alias, const TypeRef &ref,
                                llvm::ArrayRef<const Type *> bases);
  void mergeBaseOverloads(const TypeAliasDecl *alias, const TypeRef &ref, const Type *base,
                          std::vector<Overload> &overloads);
  static const Overload *overloadNamed(const std::vector<Overload> &overloads,
                                       llvm::StringRef name);
  static size_t fieldIndex(const std::vector<Field> &fields, llvm::StringRef name);
  void mergeBaseFields(const TypeAliasDecl *alias, const TypeRef &ref, const Type *base,
                       InterfaceShape &shape);
  void addOwnField(const TypeAliasDecl *alias, const Field &f, llvm::ArrayRef<Overload> overloads,
                   InterfaceShape &shape);
  static llvm::SMLoc ownFieldLoc(const TypeAliasDecl *alias, llvm::StringRef name);

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
