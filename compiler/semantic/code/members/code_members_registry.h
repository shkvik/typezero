#pragma once

#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/code/members/code_members_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <optional>

namespace tz {

class CodeMembersRegistry {
public:
  explicit CodeMembersRegistry(SemanticState &owner);

  void checkMember(const CheckMember &task);
  void afterListPart(const AfterListPart &task);
  void afterNodeMember(const AfterNodeMember &task);
  void afterMemberObject(const AfterMemberObject &task);
  void afterEnumResolved(const AfterEnumResolved &task);
  void afterProcessMember(const AfterProcessMember &task);
  void memberOfCommonBase(const MemberOfCommonBase &task);
  void afterBytesStaticMember(const AfterBytesStaticMember &task);

private:
  using ResultMemberReadable = Result<Expr *, CodeMembersRegistryCheckMemberError>;

  void walkBuiltinValueMember(MemberExpr *member, const Type *type);
  void pushMemberRead(MemberExpr *member, const ResultMemberReadable &read);
  void memberOfType(MemberExpr *member, const Type *type, const Type *expected);
  void rememberMemberUse(const EnumDecl *enumDecl, const MemberExpr *member, const Expr *value);
  void reportMember(MemberExpr *member, CodeMembersRegistryCheckMemberErrorCode code,
                    const std::string &message);
  void pushTextMember(EnumDecl *enumDecl, const MemberExpr *member, const EnumMember &enumMember,
                      const Type *expected);
  void pushNumberMember(const EnumDecl *enumDecl, const MemberExpr *member,
                        const EnumMember &enumMember, const Type *expected);


  Expr *chainLink(Expr *object);
  Expr *shortCircuitLink(Expr *object, const Type *type);
  Expr *globalConstant(GlobalName object, llvm::StringRef name, llvm::SMLoc loc);


  std::string globalThisMessage(llvm::StringRef name) const;
  ResultMemberReadable globalConstantMember(MemberExpr *member, GlobalName global);
  std::optional<ResultMemberReadable> constantObjectMember(MemberExpr *member);

  SemanticState &state;
};

}
