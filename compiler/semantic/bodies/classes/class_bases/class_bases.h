#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/classes/class_bases/class_bases_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class ClassBases {
public:
  explicit ClassBases(SemanticState &owner);

  void resolveClassBases(ClassDecl *cls);
  void addBase(const Type *type, const Type *base);
  static const ClassField *findField(const ClassDecl *cls, llvm::StringRef name,
                                     const ClassDecl **owner = nullptr);
  std::vector<Field> interfaceFields(TypeAliasDecl *alias, std::vector<const Type *> &bases,
                                     std::vector<Overload> &overloads);

private:
  using ResultParentClassFound = Result<ClassDecl *, ClassBasesBaseClassError>;
  using ResultUnusedMemberName = Result<const Field *, ClassBasesNewFieldError>;
  using ResultEventMapInferred = Result<const Type *, ClassBasesEventMapFoundError>;
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
  static ResultEventMapInferred eventMapFound(const Type *events);
  static ResultImplementedShape implementedType(const Type *type, const ClassDecl *cls);
  static ResultValueOnRedeclare valueGiven(const ClassField &field, const ClassDecl *owner);
  static ResultOverrideTargeted overrideTarget(const ClassDecl *cls, const ClassField &field);
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
  static ResultUnusedMethodName newOverload(const TypeAliasDecl *alias, const Overload &overload,
                                            llvm::ArrayRef<Field> fields,
                                            llvm::ArrayRef<Overload> overloads);
  static ResultFieldTypesAreOne sameFieldType(const TypeAliasDecl *alias, const Field &known,
                                              const Field &field, llvm::StringRef origin,
                                              llvm::StringRef baseName);

  SemanticState &state;
};

}
