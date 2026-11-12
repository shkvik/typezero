#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/declare/type_declarer/type_declarer_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Scopes;
class ClassBases;
class ExprChecker;
class IndexProofs;
class LiteralTypes;
class TypeResolver;
class ClassDeclarer;
class TypeRelations;
class ConstantFolder;
class ModuleDeclarer;
class AliasInstantiator;
class BuiltinModuleTypes;
class BuiltinModuleChecker;

struct AliasFrame {
  TypeAliasDecl *alias;
  std::optional<Module *> previousModule;
  bool isBuiltinModule;
  std::vector<TypeAliasDecl *> needed;
  size_t nextNeeded;
};

struct EnumCount {
  double next;
  bool isCounting;
  bool hasNumbers;
  bool hasStrings;
};

struct EnumFrame {
  EnumDecl *enumDecl;
  std::optional<Module *> previousModule;
  size_t nextMember;
  EnumCount count;
};

class TypeDeclarer {
public:
  using ResultEnumIndexValue = Result<Expr *, TypeDeclarerCheckEnumIndexError>;
  using ResultEnumMemberRead = Result<Expr *, TypeDeclarerCheckEnumMemberError>;

  TypeDeclarer(SemanticState &owner, SemanticParts &all);

  void resolveEnum(EnumDecl *enumDecl);
  const Type *enumType(EnumDecl *enumDecl);
  Stmt *lookupType(llvm::StringRef name) const;
  const Type *resolveAlias(TypeAliasDecl *alias);
  void resolveTypes(llvm::ArrayRef<Stmt *> declared);
  std::vector<Stmt *> declareTypes(llvm::ArrayRef<Stmt *> stmts);
  std::vector<Stmt *> declareTypeNames(llvm::ArrayRef<Stmt *> stmts);
  ResultEnumIndexValue checkEnumIndex(EnumDecl *enumDecl, IndexExpr *index);
  ResultEnumMemberRead checkEnumMember(EnumDecl *enumDecl, MemberExpr *member,
                                       const Type *expected);

private:
  using ResultMemberIntValue = Result<double, TypeDeclarerWholeMemberError>;
  using ResultTypeNameIsFree = Result<llvm::StringRef, TypeDeclarerFreeTypeNameError>;
  using ResultUnionRecursion = Result<const Type *, TypeDeclarerRecursiveUnionError>;
  using ResultEnumIsResolved = Result<const EnumDecl *, TypeDeclarerEnumCycleError>;
  using ResultEnumKindIsSame = Result<const EnumDecl *, TypeDeclarerEnumKindsError>;
  using ResultMemberIsUnique = Result<const EnumMember *, TypeDeclarerUniqueMemberError>;
  using ResultMemberHasValue = Result<const EnumMember *, TypeDeclarerCountedMemberError>;
  using ResultMemberConstant = Result<ConstantValue, TypeDeclarerMemberConstantError>;
  using ResultAliasIsOwnType = Result<const TypeAliasDecl *, TypeDeclarerBuiltinAliasError>;
  using ResultAliasNotCyclic = Result<const TypeAliasDecl *, TypeDeclarerAliasCycleError>;

  std::string formatNumber(double value) const;
  bool isMentioningTypeOf(const TypeRef &ref) const;
  ResultEnumIsResolved enumCycle(const EnumDecl *enumDecl) const;
  ResultAliasNotCyclic aliasCycle(const TypeAliasDecl *alias) const;
  ResultAliasIsOwnType builtinAlias(const TypeAliasDecl *alias) const;
  ResultMemberIntValue wholeMember(const EnumMember &member, double value) const;
  ResultMemberHasValue countedMember(const EnumMember &member, bool isCounting) const;
  ResultMemberIsUnique uniqueMember(const EnumMember &member, const EnumMember &other) const;
  ResultUnionRecursion recursiveUnion(const TypeAliasDecl *alias, const Type *type) const;
  ResultEnumKindIsSame enumKinds(const EnumDecl *enumDecl, const EnumCount &count) const;
  ResultMemberConstant memberConstant(std::optional<ConstantValue> constant) const;

  ResultTypeNameIsFree freeTypeName(llvm::StringRef name) const;

  TypeAliasDecl *takeUnresolvedAlias(AliasFrame &frame) const;
  Type *declareSelf(const TypeAliasDecl *alias) const;
  const Type *resolvingAlias(TypeAliasDecl *alias);
  void startAlias(std::vector<AliasFrame> &frames, TypeAliasDecl *alias);
  void finishAlias(std::vector<AliasFrame> &frames);
  std::optional<Module *> enterHomeModule(const Stmt *declared);
  std::vector<TypeAliasDecl *> neededAliases(const TypeAliasDecl *alias) const;
  TypeAliasDecl *plainAliasNamed(const TypeRef &part) const;
  const Type *resolveAliasBody(TypeAliasDecl *alias);
  const Type *resolveObjectAlias(TypeAliasDecl *alias);
  const Type *resolveUnionAlias(TypeAliasDecl *alias);
  void nameBuiltinUnion(const TypeAliasDecl *alias);
  void resolveDeclared(Stmt *stmt);
  void resolveDeclaredAlias(TypeAliasDecl *alias);
  llvm::StringRef declareEnumName(EnumDecl *enumDecl);
  std::optional<llvm::StringRef> declaredTypeName(Stmt *stmt);
  std::optional<llvm::StringRef> ownAliasName(const TypeAliasDecl *alias);
  void addNamedType(llvm::StringRef name, Stmt *stmt, std::vector<Stmt *> &declared);
  const Type *definedObject(const TypeAliasDecl *alias, std::vector<Field> fields,
                            std::vector<Overload> overloads) const;

  void reportEnumCycle(const EnumDecl *enumDecl);
  void startEnum(std::vector<EnumFrame> &frames, EnumDecl *enumDecl);
  void enterMember(const EnumFrame &frame);
  EnumDecl *unresolvedEnum(const EnumFrame &frame) const;
  void resolveNextMember(std::vector<EnumFrame> &frames);
  void reportNeededCycles(const EnumFrame &frame);
  void countMember(EnumFrame &frame);
  std::optional<double> memberNumber(EnumFrame &frame, EnumMember &member);
  void numberMember(EnumFrame &frame, EnumMember &member, double value);
  void finishEnum(std::vector<EnumFrame> &frames);
  void markTextsAssigned(const EnumDecl *enumDecl);
  Expr *memberNameAt(const EnumDecl *enumDecl, int64_t number, llvm::SMLoc loc);
  const EnumMember *lastMemberNumbered(const EnumDecl *enumDecl, int64_t number) const;
  void rememberMemberUse(const EnumDecl *enumDecl, const MemberExpr *member, const Expr *value);
  Expr *textMemberValue(EnumDecl *enumDecl, const MemberExpr *member, const EnumMember &enumMember,
                        const Type *expected);
  Expr *numberMemberValue(const EnumDecl *enumDecl, const MemberExpr *member,
                          const EnumMember &enumMember, const Type *expected);

  SemanticState &state;
  Scopes *scopes = nullptr;
  ClassBases *classBases = nullptr;
  ExprChecker *exprChecker = nullptr;
  IndexProofs *indexProofs = nullptr;
  LiteralTypes *literalTypes = nullptr;
  TypeResolver *typeResolver = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  TypeRelations *typeRelations = nullptr;
  ConstantFolder *constantFolder = nullptr;
  ModuleDeclarer *moduleDeclarer = nullptr;
  AliasInstantiator *aliasInstantiator = nullptr;
  BuiltinModuleTypes *builtinModuleTypes = nullptr;
  BuiltinModuleChecker *builtinModuleChecker = nullptr;
};

}
