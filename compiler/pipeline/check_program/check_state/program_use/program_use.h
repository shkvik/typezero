#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/source_file/source_file.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/StringRef.h"
#include <string>
#include <vector>

namespace tz {

class ProgramUse {
public:
  explicit ProgramUse(SourceFile &program);

  void markUsesAsync();
  void markTopLevelAwait();
  void markComparesFunctions();
  void markUsesTls();
  bool isUsingTls() const;
  void addSubtype(const Type *type);
  std::vector<const Type *> getSubtypes() const;
  void addView(const Type *from, const Type *to);
  void rememberNamedUse(const Expr *use, const NamedUse &named);
  void addFieldName(llvm::StringRef name);
  bool hasFieldName(llvm::StringRef name) const;
  std::vector<std::string> getStringConstants(llvm::StringRef name) const;
  std::vector<Module *> getModules() const;
  void addListenerReceiver(const Type *type);
  void addInstance(FunctionDecl *fn);

private:
  SourceFile &file;
};

}
