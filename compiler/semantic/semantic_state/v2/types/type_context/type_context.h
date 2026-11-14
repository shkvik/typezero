#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/SMLoc.h"
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace tz {

struct InferMatch {
  llvm::ArrayRef<const Type *> params;
  std::vector<const Type *> bound;
};

struct ConditionalInfo {
  const TypeRef *ref = nullptr;
  const Type *tag = nullptr;
  std::vector<llvm::StringRef> bound;
  std::vector<std::pair<llvm::StringRef, Stmt *>> decls;
};

struct TailCall {
  TypeAliasDecl *alias = nullptr;
  std::vector<const Type *> args;
};

class TypeContext {
public:
  void allowTailCall();
  void forbidTailCall();
  void leaveListContext();
  void forgetTemplateLoc();
  void enterExtendsClause();
  void leaveExtendsClause();
  void enterListContext(bool isList);
  void rememberTemplateLoc(llvm::SMLoc loc);
  void rememberTailCall(TypeAliasDecl *alias, std::vector<const Type *> args);
  void rememberConditionalBranches(const Type *conditional, std::vector<const Type *> branches);


  bool hasTailCall() const;
  bool isTailCallAllowed() const;
  bool isInsideListContext() const;
  bool isInsideExtendsClause() const;
  bool hasConditionalBranches(const Type *conditional) const;


  const ConditionalInfo *addConditional(ConditionalInfo info);
  const ConditionalInfo *findConditional(const TypeRef *ref) const;
  const ConditionalInfo *findConditionalByTag(const Type *tag) const;


  TailCall takeTailCall();
  llvm::SMLoc getTemplateLoc() const;
  std::vector<const Type *> findConditionalBranches(const Type *conditional) const;

private:
  llvm::SMLoc templateLoc;
  int extendsClauseDepth = 0;
  bool tailCallAllowed = false;
  std::vector<bool> listContexts;
  std::optional<TailCall> tailCall;
  std::map<const TypeRef *, ConditionalInfo> conditionals;
  llvm::DenseMap<const Type *, const ConditionalInfo *> conditionalTags;
  llvm::DenseMap<const Type *, std::vector<const Type *>> conditionalBranches;
};

}
