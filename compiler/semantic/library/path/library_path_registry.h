#pragma once

#include "compiler/program/library/path/path_function_table/path_function_table.h"
#include "compiler/program/library/path/path_value_table/path_value_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library/path/library_path_registry_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include <optional>

namespace tz {

class LibraryPathRegistry {
public:
  explicit LibraryPathRegistry(SemanticState &owner);

  void checkPathCall(const CheckPathCall &task);
  void checkPathValue(const CheckPathValue &task);
  void finishTextPath(const FinishTextPath &task);
  void finishFormatPath(const FinishFormatPath &task);
  void afterFormatPathArg(const AfterFormatPathArg &task);

private:
  using ResultPathProperty = Result<llvm::StringRef, LibraryPathRegistryPathValueError>;
  using ResultPathFunction = Result<llvm::StringRef, LibraryPathRegistryPathFunctionError>;

  void checkJoinedPath(CallExpr *call, Expr *style, Builtin builtin, llvm::SMLoc loc);


  bool isWin32Style(NodeModule module) const;
  bool isFormatInput(const Type *type) const;
  bool isPathString(std::optional<PathValue> value) const;
  bool isPathModule(std::optional<PathValue> value) const;


  const Type *parsedPathType() const;
  const Type *formatInputType() const;
  const Type *pathResultType(PathFunctionForm form) const;
  const Type *pathObjectType(const Type *part, llvm::StringRef typeName) const;


  llvm::StringRef pathStringText(PathValue value, NodeModule module) const;
  ResultPathFunction checkPathFunction(llvm::StringRef functionName) const;
  ResultPathProperty checkPathProperty(llvm::StringRef name, NodeModule module) const;

  SemanticState &state;
};

}
