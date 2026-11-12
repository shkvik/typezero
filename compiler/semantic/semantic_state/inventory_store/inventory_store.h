#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/program_inventory/program_inventory.h"
#include "compiler/program/source_file/source_file.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include <string>
#include <vector>

namespace tz {

class InventoryStore {
public:
  explicit InventoryStore(const SourceFile &loaded);

  void markUsesTls();
  void markUsesAsync();
  bool isUsingTls() const;
  void markTopLevelAwait();
  void markComparesFunctions();
  void addLateClass(ClassDecl *classDecl);
  void addInstance(FunctionDecl *functionDecl);
  void addSubtype(const Type *type);
  ProgramInventory takeInventory();
  void addFieldName(llvm::StringRef name);
  std::vector<Module *> getModules() const;
  void rememberNativeError(const Type *type);
  void addListenerReceiver(const Type *type);
  bool hasFieldName(llvm::StringRef name) const;
  void addView(const Type *from, const Type *to);
  std::vector<const Type *> getSubtypes() const;
  void rememberNamedUse(const Expr *use, const NamedUse &named);
  std::vector<std::string> getStringConstants(llvm::StringRef name) const;

private:
  const SourceFile &file;
  ProgramInventory inventory;
  llvm::StringSet<> fieldNames;
};

}
