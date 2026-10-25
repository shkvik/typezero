#pragma once

#include "compiler/program/ast/nested_group/nested_group.h"
#include "compiler/program/ast/node/node.h"
#include "compiler/program/module/module.h"
#include "compiler/program/symbols/func_symbol/func_symbol.h"
#include "compiler/program/symbols/var_symbol/var_symbol.h"

namespace tz {

// Owns all AST nodes and symbols of a compilation.
class AstContext {
public:
  template <typename T, typename... Args> T *make(Args &&...args) {
    auto node = std::make_unique<T>(std::forward<Args>(args)...);
    T *raw = node.get();
    nodes_.push_back(std::move(node));

    return raw;
  }

  VarSymbol *newVar() { return &vars_.emplace_back(); }
  FuncSymbol *newFunc() { return &funcs_.emplace_back(); }
  NestedGroup *newGroup() { return &groups_.emplace_back(); }
  Module *newModule() { return &modules_.emplace_back(); }
  // A name the source cannot contain, for a hidden variable the parser
  // introduces (destructuring keeps the value there).
  llvm::StringRef hiddenName() { return names_.emplace_back("%" + std::to_string(names_.size())); }
  // A name that is not in the source as it is, such as a field name written
  // as a string literal.
  llvm::StringRef keepName(std::string name) { return names_.emplace_back(std::move(name)); }

private:
  std::vector<std::unique_ptr<Node>> nodes_;
  std::deque<std::string> names_;
  std::deque<VarSymbol> vars_;
  std::deque<FuncSymbol> funcs_;
  std::deque<NestedGroup> groups_;
  std::deque<Module> modules_;
};

} // namespace tz
