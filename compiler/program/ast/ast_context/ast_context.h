#pragma once

#include "compiler/program/ast/function_decl/function_decl.h"
#include "compiler/program/ast/nested_group/nested_group.h"
#include "compiler/program/ast/node/node.h"
#include "compiler/program/module/module.h"
#include "compiler/program/symbols/func_symbol/func_symbol.h"
#include "compiler/program/symbols/var_symbol/var_symbol.h"
#include <type_traits>

namespace tz {

class AstContext {
public:
  AstContext();

  template <typename T, typename... Args> T *make(Args &&...args) {
    auto node = std::make_unique<T>(std::forward<Args>(args)...);
    T *raw = node.get();
    this->nodes.push_back(std::move(node));
    if constexpr (std::is_same_v<T, FunctionDecl>) {
      raw->id = functionCount_++;
    }

    return raw;
  }

  VarSymbol *newVar() {
    return &vars_.emplace_back();
  }
  FuncSymbol *newFunc() {
    return &this->funcs.emplace_back();
  }
  NestedGroup *newGroup() {
    return &this->groups.emplace_back();
  }
  Module *newModule() {
    return &modules_.emplace_back();
  }
  llvm::StringRef hiddenName() {
    return this->names.emplace_back("%" + std::to_string(this->names.size()));
  }
  llvm::StringRef keepName(std::string name) {
    return this->names.emplace_back(std::move(name));
  }
  const FunctionDecl *moduleFunction() const;
  int functionCount() const;

private:
  std::vector<std::unique_ptr<Node>> nodes;
  std::deque<std::string> names;
  std::deque<VarSymbol> vars_;
  std::deque<FuncSymbol> funcs;
  std::deque<NestedGroup> groups;
  std::deque<Module> modules_;
  int functionCount_ = 0;
  FunctionDecl *moduleFunction_ = nullptr;
};

}
