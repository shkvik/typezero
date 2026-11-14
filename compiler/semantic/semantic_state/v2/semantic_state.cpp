#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

SemanticState::SemanticState(AstContext &context, Diagnostics &collected, SourceFile &loaded)
    : ast(context),
      code{.literals = LiteralContext(loaded)},
      file(loaded),
      library{.declarations = LibraryDeclarationContext(loaded.builtins)},
      diagnostics(collected),
      inventory(loaded.inventory) {}

Checkpoint SemanticState::saveBody() {
  auto saved = Checkpoint{
      .flow = this->walk.flow.takeFlow(),
      .block = this->walk.block.saveBody(),
      .scopes = this->walk.scopes.saveBody(),
      .module = this->declaration.modules.getCurrentModule(),
      .functions = this->walk.functions.saveBody(),
      .instance = this->generics.copies.getCheckingInstance(),
  };

  return saved;
}

void SemanticState::restoreBody(Checkpoint saved) {
  this->walk.flow.replaceFlow(std::move(saved.flow));
  this->walk.block.restoreBody(std::move(saved.block));
  this->walk.scopes.restoreBody(std::move(saved.scopes));
  this->declaration.modules.rememberCurrentModule(saved.module);
  this->walk.functions.restoreBody(std::move(saved.functions));
  this->generics.copies.rememberCheckingInstance(saved.instance);
}

}
