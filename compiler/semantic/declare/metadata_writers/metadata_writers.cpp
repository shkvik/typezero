#include "compiler/semantic/declare/metadata_writers/metadata_writers.h"
#include "compiler/semantic/flow/placed_nodes/placed_nodes.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

using llvm::dyn_cast;

namespace tz {

MetadataWriters::MetadataWriters(SemanticState &owner)
    : state(owner) {}

void MetadataWriters::rememberWriters(SourceFile &program) {
  for (Module *module : program.modules) {
    for (Stmt *item : module->file.items) {
      std::vector<FunctionDecl *> functions;
      if (auto *fn = dyn_cast<FunctionDecl>(item)) {
        functions.push_back(fn);
      } else if (auto *cls = dyn_cast<ClassDecl>(item)) {
        functions = cls->methods;
      }
      for (FunctionDecl *fn : functions) {
        this->rememberWritersIn(fn);
      }
    }
  }
}

void MetadataWriters::rememberWritersIn(FunctionDecl *fn) {
  std::vector<const Expr *> defaults;
  for (const Param &param : fn->params) {
    defaults.push_back(param.defaultValue());
  }
  PlacedNodes nodes = PlacedNodes::collect({fn->body}, defaults);
  for (const PlacedExpr &placed : nodes.getExprs()) {
    this->rememberWriter(fn, placed.expr);
  }
}

void MetadataWriters::rememberWriter(FunctionDecl *fn, const Expr *expr) {
  auto *call = dyn_cast<CallExpr>(expr);
  auto *member = call ? dyn_cast<MemberExpr>(call->callee) : nullptr;
  auto *object = member ? dyn_cast<NameExpr>(member->object) : nullptr;
  bool defines = object && object->name == "Reflect" && member->name == "defineMetadata" &&
                 !call->args.empty();
  if (!defines) {
    return;
  }
  if (auto *key = dyn_cast<NameExpr>(call->args[0])) {
    this->state.getReflectStore().addMetadataWriter(key->name, fn);
  }
}

}
