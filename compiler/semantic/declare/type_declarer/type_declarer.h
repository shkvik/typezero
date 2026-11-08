#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/declare/type_declarer/type_declarer_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class TypeDeclarer {
public:
  using ResultEnumIndexValue = Result<Expr *, TypeDeclarerCheckEnumIndexError>;
  using ResultEnumMemberRead = Result<Expr *, TypeDeclarerCheckEnumMemberError>;

  explicit TypeDeclarer(SemanticState &owner);

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

  SemanticState &state;
};

}
