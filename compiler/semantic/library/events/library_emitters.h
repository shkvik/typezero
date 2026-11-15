#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library/events/library_events_registry_errors.h"
#include <string>

namespace tz {

class SemanticState;

class LibraryEmitters {
public:
  using ResultEventMapFits = Result<const Type *, LibraryEmittersEventMapFitsError>;

  explicit LibraryEmitters(SemanticState &owner);

  bool isServiceEvent(llvm::StringRef name) const;
  bool isEmitterClass(const ClassDecl *classDecl) const;


  const Type *serviceEventArgs() const;
  const Type *eventMapOf(const Type *type) const;
  const Type *listenerType(const Type *args, const Type *returnType) const;


  ResultEventMapFits checkEventMapFits(const Type *events) const;
  std::string nearestEvent(const Type *events, llvm::StringRef name) const;

private:
  bool isCarryingOne(const Type *args) const;


  ResultEventMapFits checkRecordMapFits(const Type *events) const;
  ResultEventMapFits checkEventFieldFits(const Field &field) const;

  SemanticState &state;
};

}
