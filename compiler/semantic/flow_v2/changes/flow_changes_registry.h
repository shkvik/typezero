#pragma once

#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class FlowChangesRegistry {
public:
  explicit FlowChangesRegistry(SemanticState &owner);

  void calledFunction();
  void rememberAssigned(SourceFile &program);
  void noteAssigned(const NoteAssigned &task);
  void noteIncremented(const NoteIncremented &task);
  void noteArrayResized(const NoteArrayResized &task);

private:
  using NarrowedVar = std::pair<const VarSymbol *, const Type *>;

  void rememberTarget(const Expr *expr);
  void incremented(const VarSymbol *variable, int64_t delta);
  void assignedLength(const VarSymbol *variable, const Expr *value);
  void addMinMaxBounds(const VarSymbol *variable, const Expr *value);
  void assignedInteger(const VarSymbol *variable, const Expr *value);
  void resetAssignedFlow(const VarSymbol *variable, const Expr *value);
  void rememberResultState(const VarSymbol *variable, const Type *given);
  void addAtMost(Facts &facts, const Linear &smaller, const Linear &larger) const;


  bool isShiftSafe(const Term &term, int64_t delta) const;
  bool isResizedPart(const Term &term, const Type *array, const VarSymbol *except) const;
  bool isResizedTerm(const Term &term, const Type *array, const VarSymbol *except) const;
  bool isResizedFact(const Type *array, Resize resize, const VarSymbol *except,
                     const Facts::Fact &fact) const;


  std::vector<std::string> keyNames(const Expr *key) const;
  std::vector<std::string> memberConstants(const MemberExpr *member) const;


  const Expr *changedTarget(const Expr *expr) const;

  SemanticState &state;
};

}
