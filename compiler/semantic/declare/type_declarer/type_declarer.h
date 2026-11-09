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
  bool builtinModule;
  std::vector<TypeAliasDecl *> needed;
  size_t nextNeeded;
};

struct EnumCount {
  double next;
  bool counting;
  bool numbers;
  bool strings;
};

struct EnumFrame {
  EnumDecl *decl;
  std::optional<Module *> previousModule;
  size_t nextMember;
  EnumCount count;
};

class TypeDeclarer {
public:
  using ResultEnumIndexValue = Result<Expr *, TypeDeclarerCheckEnumIndexError>;
  using ResultEnumMemberRead = Result<Expr *, TypeDeclarerCheckEnumMemberError>;

  TypeDeclarer(SemanticState &owner, SemanticParts &all);

  void resolveEnum(EnumDecl *decl);
  const Type *enumType(EnumDecl *decl);
  Stmt *lookupType(llvm::StringRef name) const;
  const Type *resolveAlias(TypeAliasDecl *alias);
  void resolveTypes(llvm::ArrayRef<Stmt *> declared);
  ResultEnumIndexValue checkEnumIndex(EnumDecl *decl, IndexExpr *index);
  std::vector<Stmt *> declareTypes(llvm::ArrayRef<Stmt *> stmts, bool resolve = true);
  ResultEnumMemberRead checkEnumMember(EnumDecl *decl, MemberExpr *member, const Type *expected);

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

  static std::string formatNumber(double value);
  static bool mentionsTypeOf(const TypeRef &ref);
  static std::u16string toUtf16(llvm::StringRef text);
  static ResultEnumIsResolved enumCycle(const EnumDecl *decl);
  static ResultAliasNotCyclic aliasCycle(const TypeAliasDecl *alias);
  static ResultAliasIsOwnType builtinAlias(const TypeAliasDecl *alias);
  static ResultMemberIntValue wholeMember(const EnumMember &m, double value);
  static ResultMemberHasValue countedMember(const EnumMember &m, bool counting);
  static ResultMemberIsUnique uniqueMember(const EnumMember &m, const EnumMember &other);
  static ResultUnionRecursion recursiveUnion(const TypeAliasDecl *alias, const Type *type);
  static ResultEnumKindIsSame enumKinds(const EnumDecl *decl, bool numbers, bool strings);
  static ResultMemberConstant memberConstant(std::optional<ConstantValue> constant);

  ResultTypeNameIsFree freeTypeName(llvm::StringRef name) const;

  static TypeAliasDecl *takeUnresolvedAlias(AliasFrame &frame);
  static Type *declareSelf(const TypeAliasDecl *alias);
  const Type *resolvingAlias(TypeAliasDecl *alias);
  void startAlias(std::vector<AliasFrame> &frames, TypeAliasDecl *alias);
  void finishAlias(std::vector<AliasFrame> &frames);
  std::optional<Module *> enterHomeModule(const Stmt *decl);
  std::vector<TypeAliasDecl *> neededAliases(const TypeAliasDecl *alias) const;
  TypeAliasDecl *plainAliasNamed(const TypeRef &part) const;
  const Type *resolveAliasBody(TypeAliasDecl *alias);
  const Type *resolveObjectAlias(TypeAliasDecl *alias);
  const Type *resolveUnionAlias(TypeAliasDecl *alias);
  void nameBuiltinUnion(const TypeAliasDecl *alias);

  void reportEnumCycle(const EnumDecl *decl);
  void startEnum(std::vector<EnumFrame> &frames, EnumDecl *decl);
  void enterMember(const EnumFrame &frame);
  EnumDecl *unresolvedEnum(const EnumFrame &frame) const;
  void resolveNextMember(std::vector<EnumFrame> &frames);
  void countMember(EnumFrame &frame);
  std::optional<double> memberNumber(EnumFrame &frame, EnumMember &m);
  void numberMember(EnumFrame &frame, EnumMember &m, double value);
  void finishEnum(std::vector<EnumFrame> &frames);

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
