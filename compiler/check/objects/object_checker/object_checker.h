#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/check/objects/object_checker/object_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class ObjectChecker {
public:
  using ResultObjectFieldValue = Result<Expr *, ObjectCheckerCheckFieldError>;
  using ResultFieldCallIsTyped = Result<Expr *, ObjectCheckerCheckFieldCallError>;
  using ResultObjectStaticCall = Result<Expr *, ObjectCheckerCheckObjectCallError>;

  explicit ObjectChecker(Checker &owner);

  void checkForIn(ForOfStmt *stmt);
  ResultObjectFieldValue checkField(MemberExpr *member);
  Expr *enumObject(NameExpr *name, const Type *expected);
  const Type *checkObjectFieldTarget(MemberExpr *member);
  ResultFieldCallIsTyped checkFieldCall(CallExpr *call, MemberExpr *member);
  std::string objectMismatch(const Type *from, const Type *to, bool conflict = false) const;
  ResultObjectStaticCall checkObjectCall(CallExpr *call, MemberExpr *member, const Type *expected);

private:
  using ResultEnumMemberValues = Result<Expr *, ObjectCheckerEnumMembersError>;
  using ResultForInKeysAllowed = Result<const Type *, ObjectCheckerWalkableError>;
  using ResultLengthIsNotFixed = Result<const Type *, ObjectCheckerFixedLengthError>;
  using ResultTargetFieldFound = Result<const Field *, ObjectCheckerFieldExistsError>;
  using ResultTargetObjectType = Result<const Type *, ObjectCheckerObjectDefinedError>;
  using ResultTargetIsWritable = Result<const Field *, ObjectCheckerFieldWritableError>;

  static bool isInherited(llvm::StringRef name);
  static std::u16string utf16(llvm::StringRef text);
  static bool hasMethodFields(const ClassDecl *cls);
  static std::string noFieldText(const Type *type, llvm::StringRef name);
  static ResultForInKeysAllowed walkable(const Type *type, llvm::StringRef text);
  static ResultLengthIsNotFixed fixedLength(const Type *type, const MemberExpr *member);
  static ResultTargetFieldFound fieldExists(const Type *type, const MemberExpr *member);
  ResultEnumMemberValues enumMembers(CallExpr *call, EnumDecl *decl, llvm::StringRef fn);
  static ResultTargetObjectType objectDefined(const Type *type, const MemberExpr *member);
  static ResultTargetIsWritable fieldWritable(const Field *field, const Type *type,
                                              const MemberExpr *member);

  Checker &checker;
};

}
