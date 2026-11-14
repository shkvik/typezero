#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "compiler/program/ast/ast.h"
#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/program/library/bytes/buffer_export_table/buffer_export_table.h"

using llvm::dyn_cast;

namespace tz {

SemanticState::SemanticState(AstContext &context, Diagnostics &collected, SourceFile &loaded)
    : ast(context),
      code{.literals = LiteralContext(loaded)},
      flow{
          .loops = FlowLoops(*this),
          .proofs = FlowProofs(*this),
          .changes = FlowChanges(*this),
          .narrowing = FlowNarrowing(*this),
          .conditions = FlowConditions(*this),
          .defined = FlowDefinedValues(*this),
          .tracked = FlowTrackedValues(*this),
          .functions = FlowFunctionFacts(*this),
          .inequalities = FlowInequalities(*this),
          .measurements = FlowMeasurements(*this),
          .reachability = FlowReachability(*this),
      },
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

const NodeName *SemanticState::findNodeName(llvm::StringRef name) const {
  const auto &modules = this->declaration.modules;
  auto *current = modules.getCurrentModule();
  if (current == nullptr || !modules.hasModuleState(current)) {
    return nullptr;
  }
  const auto &names = modules.getModuleState(current).nodeNames;
  auto found = names.find(name);
  if (found == names.end()) {
    return nullptr;
  }
  bool isBufferName = BufferExportTable::find(name) == BufferExport::BUFFER;
  bool isGlobalBuffer = found->second.module == NodeModule::Buffer && isBufferName;
  if (isGlobalBuffer) {
    return nullptr;
  }
  auto entry = this->walk.scopes.findName(name);
  if (entry.isFound()) {
    return nullptr;
  }

  return &found->second;
}

Stmt *SemanticState::findTypeDeclaration(llvm::StringRef name) const {
  auto *declared = this->walk.scopes.findType(name);
  if (declared != nullptr) {
    return declared;
  }
  auto *alias = this->declaration.prelude.findBuiltinAlias(name);
  if (alias != nullptr) {
    return alias;
  }
  const auto *nodeName = this->findNodeName(name);
  if (nodeName != nullptr) {
    return this->findModuleExport(*nodeName);
  }
  if (this->declaration.modules.isInsideBuiltinModule()) {
    return this->library.declarations.findModuleAlias(name);
  }

  return nullptr;
}

TypeAliasDecl *SemanticState::findModuleExport(const NodeName &nodeName) const {
  const auto &declarations = this->library.declarations;
  if (!declarations.isModuleExport(nodeName.module, nodeName.name)) {
    return nullptr;
  }

  return declarations.findModuleAlias(nodeName.name);
}

bool SemanticState::isIterableRef(const TypeRef &ref) const {
  return ref.kind() == TypeRefKind::GENERIC_REF &&
         TypeNameTable::isName(ref.name, TypeName::ITERABLE) &&
         this->findTypeDeclaration(ref.name) == nullptr;
}

bool SemanticState::isUnshadowedGlobal(const NameExpr *name) const {
  const auto *nodeName = this->findNodeName(name->name);
  if (nodeName != nullptr) {
    return false;
  }
  auto shadow = this->walk.scopes.findName(name->name);

  return shadow.var() == nullptr && shadow.func() == nullptr && shadow.cls() == nullptr;
}

bool SemanticState::isGlobalName(const Expr *expr, GlobalName global) const {
  bool isThroughGlobalThis = global == GlobalName::CRYPTO && this->isGlobalThisMember(expr, global);
  if (isThroughGlobalThis) {
    return true;
  }

  return this->isNamingGlobal(expr, global);
}

bool SemanticState::isGlobalThisMember(const Expr *expr, GlobalName global) const {
  auto name = GlobalNameTable::getName(global);
  const auto *member = dyn_cast<MemberExpr>(expr);
  if (member == nullptr || member->optional || member->name != name) {
    return false;
  }

  return this->isNamingGlobal(member->object, GlobalName::GLOBAL_THIS);
}

bool SemanticState::isNamingGlobal(const Expr *expr, GlobalName global) const {
  auto name = GlobalNameTable::getName(global);
  const auto *named = dyn_cast<NameExpr>(expr);
  if (named == nullptr || named->name != name) {
    return false;
  }

  return this->isUnshadowedGlobal(named);
}

bool SemanticState::isGlobalMember(const MemberExpr *member, GlobalMember which) const {
  auto owner = GlobalMemberTable::getOwner(which);
  auto found = GlobalMemberTable::find(owner, member->name);
  if (found != which) {
    return false;
  }

  return this->isGlobalName(member->object, owner);
}

Module *SemanticState::findLaterSource(llvm::StringRef name, llvm::StringRef &declared) const {
  auto &here =
      this->declaration.modules.getModuleState(this->declaration.modules.getCurrentModule());
  declared = name;
  auto found = here.imports.find(name);
  if (found != here.imports.end()) {
    declared = found->second.target.name;
    Module *source = found->second.target.module;
    return this->declaration.modules.getModuleState(source).loaded ? nullptr : source;
  }
  if (this->declaration.modules.getLoadingModule() != nullptr &&
      this->declaration.modules.getCurrentModule() !=
          this->declaration.modules.getLoadingModule() &&
      !here.loaded && here.topLevelNames.contains(name)) {
    return this->declaration.modules.getCurrentModule();
  }

  return nullptr;
}

std::optional<StartLoadEarly> SemanticState::findEarlyLoad(llvm::StringRef name) const {
  auto entry = this->walk.scopes.findName(name);
  auto *loading = this->declaration.modules.getLoadingModule();
  if (entry.isFound() || loading == nullptr) {
    return std::nullopt;
  }
  llvm::StringRef declared;
  auto *source = this->findLaterSource(name, declared);
  bool isLoadingHere = !this->walk.functions.isInsideFunction() &&
                       this->declaration.modules.getCurrentModule() == loading;
  if (source == nullptr || isLoadingHere || !this->isLoadable(source, declared)) {
    return std::nullopt;
  }

  return StartLoadEarly{.source = source, .name = declared};
}

bool SemanticState::isLoadable(Module *source, llvm::StringRef name) const {
  const auto &there = this->declaration.modules.getModuleState(source);

  return !there.loaded && !there.busy && there.topLevelNames.contains(name);
}

}
