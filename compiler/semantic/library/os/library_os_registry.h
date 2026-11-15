#pragma once

#include "compiler/program/library/os/os_function_table/os_function_table.h"
#include "compiler/program/library/os/os_shape_field_table/os_shape_field_table.h"
#include "compiler/program/library/os/os_value_table/os_value_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library/os/library_os_registry_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include <string>

namespace tz {

class LibraryOsRegistry {
public:
  explicit LibraryOsRegistry(SemanticState &owner);

  void checkOsCall(const CheckOsCall &task);
  void checkOsValue(const CheckOsValue &task);

private:
  using ResultOsFunction = Result<llvm::StringRef, LibraryOsRegistryOsFunctionError>;

  void setWhichArg(CallExpr *call, llvm::SMLoc loc, int which);


  const Type *cpuInfoType() const;
  const Type *userInfoType() const;


  Expr *endiannessLiteral(llvm::SMLoc loc);
  Expr *osStringCall(CallExpr *call, llvm::SMLoc loc, int which);
  Expr *osNumberCall(CallExpr *call, llvm::SMLoc loc, int which);
  Expr *builtinOsCall(CallExpr *call, Builtin builtin, const Type *result) const;
  Expr *checkOsFunction(CallExpr *call, OsFunction function, llvm::SMLoc loc);
  Expr *reportNotYet(CallExpr *call, llvm::StringRef functionName, llvm::SMLoc loc);


  std::string shapeFieldName(OsShapeField field) const;
  llvm::StringRef osValueText(OsValue value) const;
  ResultOsFunction checkKnownOsFunction(llvm::StringRef functionName) const;

  SemanticState &state;
};

}
