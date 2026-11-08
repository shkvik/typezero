#include "compiler/lower/library/weakref/weakref_library_lowerer/weakref_library_lowerer.h"
#include "compiler/lower/library/weakref/weak_ref_lowerer/weak_ref_lowerer.h"
#include "compiler/lower/lowering/lowering.h"
#include "compiler/program/ast/builtin/builtin.h"

namespace tz {

bool WeakRefLibraryLowerer::lowers(Builtin builtin) const {
  return builtin == Builtin::WeakRefNew || builtin == Builtin::WeakRefDeref;
}

llvm::Value *WeakRefLibraryLowerer::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.weakRefLowerer().emitWeakRefCall(call);
}

}
