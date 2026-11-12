#include "compiler/semantic/declare/metadata_writers/metadata_writers.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/reflect/reflect_function_table/reflect_function_table.h"
#include "compiler/semantic/flow/placed_nodes/placed_nodes.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

using llvm::dyn_cast;

namespace tz {

MetadataWriters::MetadataWriters(SemanticState &owner)
    : state(owner) {}

void MetadataWriters::rememberWriters(SourceFile &program) {
  for (Module *module : program.modules) {
    for (Stmt *item : module->file.items) {
      for (FunctionDecl *functionDecl : this->functionsOf(item)) {
        this->rememberWritersIn(functionDecl);
      }
    }
  }
}

std::vector<FunctionDecl *> MetadataWriters::functionsOf(Stmt *item) const {
  auto *functionDecl = dyn_cast<FunctionDecl>(item);
  auto *classDecl = dyn_cast<ClassDecl>(item);
  if (functionDecl != nullptr) {
    return {functionDecl};
  }
  if (classDecl != nullptr) {
    return classDecl->methods;
  }

  return {};
}

void MetadataWriters::rememberWritersIn(FunctionDecl *functionDecl) {
  auto defaults = std::vector<const Expr *>();
  for (const Param &param : functionDecl->params) {
    defaults.push_back(param.defaultValue());
  }
  auto nodes = PlacedNodes::collect({functionDecl->body}, defaults);
  for (const PlacedExpr &placed : nodes.getExprs()) {
    this->rememberWriter(functionDecl, placed.expr);
  }
}

void MetadataWriters::rememberWriter(FunctionDecl *functionDecl, const Expr *expr) {
  auto *call = dyn_cast<CallExpr>(expr);
  if (call == nullptr || call->args.empty() || !this->isDefiningMetadata(call)) {
    return;
  }
  auto *key = dyn_cast<NameExpr>(call->args[0]);
  if (key != nullptr) {
    this->state.getReflectStore().addMetadataWriter(key->name, functionDecl);
  }
}

bool MetadataWriters::isDefiningMetadata(const CallExpr *call) const {
  auto *member = dyn_cast<MemberExpr>(call->callee);
  if (member == nullptr) {
    return false;
  }
  auto *object = dyn_cast<NameExpr>(member->object);
  if (object == nullptr || object->name != GlobalNameTable::getName(GlobalName::REFLECT)) {
    return false;
  }

  return ReflectFunctionTable::find(member->name) == ReflectFunction::DEFINE_METADATA;
}

}
