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

using DestinationPlaces = std::vector<std::pair<const Type *, llvm::SMLoc>>;

class TypeContext {
public:
  void enterExtendsClause();
  void leaveExtendsClause();
  bool isInsideExtendsClause() const;
  void enterListContext(bool isList);
  void leaveListContext();
  bool isInsideListContext() const;
  void allowTailCall();
  void forbidTailCall();
  bool isTailCallAllowed() const;
  void rememberTailCall(TypeAliasDecl *alias, std::vector<const Type *> args);
  bool hasTailCall() const;
  TailCall takeTailCall();
  const ConditionalInfo *addConditional(ConditionalInfo info);
  const ConditionalInfo *findConditional(const TypeRef *ref) const;
  const ConditionalInfo *findConditionalByTag(const Type *tag) const;
  void rememberTemplateLoc(llvm::SMLoc loc);
  void forgetTemplateLoc();
  llvm::SMLoc getTemplateLoc() const;
  void enterBuiltinModule();
  void leaveBuiltinModule();
  bool isInsideBuiltinModule() const;
  void rememberDestinations(const VarSymbol *var, DestinationPlaces places);
  DestinationPlaces getDestinations(const VarSymbol *var) const;
  void rememberDestinationConflict(DestinationPlaces places);
  DestinationPlaces takeDestinationConflict();

private:
  int extendsClauseDepth = 0;
  std::vector<bool> listContexts;
  bool tailCallAllowed = false;
  std::optional<TailCall> tailCall;
  std::map<const TypeRef *, ConditionalInfo> conditionals;
  llvm::DenseMap<const Type *, const ConditionalInfo *> conditionalTags;
  llvm::SMLoc templateLoc;
  int builtinModuleDepth = 0;
  llvm::DenseMap<const VarSymbol *, DestinationPlaces> destinations;
  DestinationPlaces destinationConflict;
};

}
