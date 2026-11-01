#include "compiler/lower/debug_info/debug_info.h"
#include "compiler/lower/debug_types/debug_types.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/program/module/module.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

namespace tz {

DebugInfo::DebugInfo(Lowering &lowering) : lowering_(lowering) {}

DebugInfo::~DebugInfo() = default;

void DebugInfo::start(const SourceFile &file) {
  if (lowering_.debugLevel_ == DebugLevel::None || !lowering_.sourceMgr_ ||
      lowering_.sourceMgr_->getNumBuffers() == 0) {
    return;
  }
  for (const Module *module : file.modules) {
    for (const auto &[buffer, path] : module->bufferPaths) {
      paths_[buffer] = path;
    }
  }
  llvm::Module &module = *lowering_.module_;
  if (lowering_.codeView_) {
    module.addModuleFlag(llvm::Module::Warning, "CodeView", 1);
  } else {
    module.addModuleFlag(llvm::Module::Warning, "Dwarf Version", 5);
  }
  module.addModuleFlag(llvm::Module::Warning, "Debug Info Version", llvm::DEBUG_METADATA_VERSION);
  builder_ = std::make_unique<llvm::DIBuilder>(module);
  entryBuffer_ = lowering_.sourceMgr_->getMainFileID();
  unit_ = builder_->createCompileUnit(llvm::DISourceLanguageName(llvm::dwarf::DW_LANG_C_plus_plus),
                                      fileOf(entryBuffer_), "tz", lowering_.optimized_, "", 0, "",
                                      lowering_.debugLevel_ == DebugLevel::Full
                                          ? llvm::DICompileUnit::FullDebug
                                          : llvm::DICompileUnit::LineTablesOnly);
  signature_ = builder_->createSubroutineType(builder_->getOrCreateTypeArray({}));
  if (lowering_.debugLevel_ == DebugLevel::Full) {
    types_ = std::make_unique<DebugTypes>(lowering_, *builder_, unit_);
  }
}

llvm::DIFile *DebugInfo::fileOf(unsigned buffer) {
  if (llvm::DIFile *found = files_.lookup(buffer)) {
    return found;
  }
  llvm::SmallString<256> path;
  if (auto known = paths_.find(buffer); known != paths_.end()) {
    path = known->second;
  } else {
    path = lowering_.sourceMgr_->getMemoryBuffer(buffer)->getBufferIdentifier();
  }
  llvm::sys::fs::make_absolute(path);
  llvm::sys::path::remove_dots(path, true);
  llvm::sys::path::native(path);
  llvm::DIFile *file =
      builder_->createFile(llvm::sys::path::filename(path), llvm::sys::path::parent_path(path));
  files_[buffer] = file;

  return file;
}

llvm::DIScope *DebugInfo::scopeIn(llvm::DISubprogram *subprogram, llvm::DIFile *file) {
  if (subprogram->getFile() == file) {
    return subprogram;
  }
  llvm::DIScope *&scope = scopes_[{subprogram, file}];
  if (!scope) {
    scope = builder_->createLexicalBlockFile(subprogram, file);
  }

  return scope;
}

llvm::DISubprogram *DebugInfo::describe(llvm::Function *function, llvm::StringRef name,
                                        llvm::SMLoc loc) {
  const llvm::SourceMgr &sources = *lowering_.sourceMgr_;
  unsigned buffer = loc.isValid() ? sources.FindBufferContainingLoc(loc) : entryBuffer_;
  if (!buffer) {
    buffer = entryBuffer_;
  }
  unsigned line = loc.isValid() ? sources.getLineAndColumn(loc, buffer).first : 1;
  llvm::DIFile *file = fileOf(buffer);
  llvm::DISubprogram::DISPFlags flags = llvm::DISubprogram::SPFlagDefinition;
  if (lowering_.optimized_) {
    flags |= llvm::DISubprogram::SPFlagOptimized;
  }
  if (function->hasLocalLinkage()) {
    flags |= llvm::DISubprogram::SPFlagLocalToUnit;
  }
  llvm::DISubprogram *subprogram =
      builder_->createFunction(file, name, function->getName(), file, line, signature_, line,
                               llvm::DINode::FlagPrototyped, flags);
  function->setSubprogram(subprogram);
  lowering_.builder_.SetCurrentDebugLocation(
      llvm::DILocation::get(lowering_.ctx_, line, 0, subprogram));

  return subprogram;
}

void DebugInfo::beginFunction(llvm::Function *function, const FunctionDecl *fn) {
  lowering_.builder_.SetCurrentDebugLocation(llvm::DebugLoc());
  if (!unit_ || !fn || function->getSubprogram()) {
    return;
  }
  std::string name = fn->name.empty() ? "<anonymous>" : fn->name.str();
  if (fn->cls && !fn->cls->literal) {
    name = fn->isConstructor || fn == fn->cls->init ? "new " + fn->cls->name.str()
                                                    : (fn->cls->name + "." + name).str();
  }
  describe(function, name, fn->loc());
  owners_[function] = fn;
}

void DebugInfo::beginTopLevel(llvm::Function *function) {
  if (unit_ && !function->getSubprogram()) {
    describe(function, "<top level>", llvm::SMLoc());
  }
}

void DebugInfo::at(llvm::SMLoc loc) {
  if (!unit_ || !loc.isValid()) {
    return;
  }
  llvm::BasicBlock *block = lowering_.builder_.GetInsertBlock();
  llvm::DISubprogram *subprogram = block ? block->getParent()->getSubprogram() : nullptr;
  if (!subprogram) {
    return;
  }
  const llvm::SourceMgr &sources = *lowering_.sourceMgr_;
  unsigned buffer = sources.FindBufferContainingLoc(loc);
  if (!buffer) {
    return;
  }
  auto [line, column] = sources.getLineAndColumn(loc, buffer);
  llvm::DIScope *scope = scopeIn(subprogram, fileOf(buffer));
  llvm::DebugLoc current = lowering_.builder_.getCurrentDebugLocation();
  if (current && current->getLine() == line && current->getScope() == scope) {
    return;
  }
  lowering_.builder_.SetCurrentDebugLocation(
      llvm::DILocation::get(lowering_.ctx_, line, column, scope));
}

llvm::DebugLoc DebugInfo::here() const { return lowering_.builder_.getCurrentDebugLocation(); }

void DebugInfo::restore(const llvm::DebugLoc &loc) {
  if (unit_) {
    lowering_.builder_.SetCurrentDebugLocation(loc);
  }
}

void DebugInfo::repair(llvm::Function &function) {
  llvm::DISubprogram *subprogram = function.getSubprogram();
  for (llvm::BasicBlock &block : function) {
    llvm::DILocation *last = nullptr;
    if (subprogram) {
      for (llvm::Instruction &inst : block) {
        llvm::DILocation *loc = inst.getDebugLoc().get();
        if (loc && loc->getScope()->getSubprogram() == subprogram) {
          last = loc;
          break;
        }
      }
      if (!last) {
        last = llvm::DILocation::get(lowering_.ctx_, subprogram->getLine(), 0, subprogram);
      }
    }
    for (llvm::Instruction &inst : block) {
      llvm::DILocation *loc = inst.getDebugLoc().get();
      if (!subprogram) {
        if (loc) {
          inst.setDebugLoc(llvm::DebugLoc());
        }
      } else if (loc && loc->getScope()->getSubprogram() == subprogram) {
        last = loc;
      } else {
        inst.setDebugLoc(last);
      }
    }
  }
}

unsigned DebugInfo::argumentOf(const VarSymbol *var) const {
  const FunctionDecl *fn = lowering_.currentFn_;
  if (!fn) {
    return 0;
  }
  unsigned first = fn->thisVar ? 2 : 1;
  if (var == fn->thisVar) {
    return 1;
  }
  for (size_t i = 0; i < fn->params.size(); ++i) {
    if (fn->params[i].sym == var) {
      return first + unsigned(i);
    }
  }

  return 0;
}

void DebugInfo::declare(const VarSymbol *var, llvm::Value *address) {
  llvm::BasicBlock *block = lowering_.builder_.GetInsertBlock();
  llvm::Function *function = block ? block->getParent() : nullptr;
  llvm::DISubprogram *subprogram = function ? function->getSubprogram() : nullptr;
  if (!types_ || !subprogram || !var || var->name.empty() || !var->type || !address ||
      !declared_.insert({function, var}).second) {
    return;
  }
  const llvm::SourceMgr &sources = *lowering_.sourceMgr_;
  unsigned buffer = var->loc.isValid() ? sources.FindBufferContainingLoc(var->loc) : 0;
  llvm::DIFile *file = buffer ? fileOf(buffer) : subprogram->getFile();
  unsigned line = buffer ? sources.getLineAndColumn(var->loc, buffer).first : subprogram->getLine();
  llvm::DIScope *scope = scopeIn(subprogram, file);
  llvm::DIType *type = types_->of(var->type);
  auto *storage = llvm::dyn_cast<llvm::AllocaInst>(address);
  if (!storage) {
    if (type->getTag() == llvm::dwarf::DW_TAG_reference_type) {
      type = builder_->createPointerType(llvm::cast<llvm::DIDerivedType>(type)->getBaseType(), 64);
    }
    type = types_->referenceTo(type);
    storage = lowering_.functionFrame().createStackSlot(lowering_.builder_.getPtrTy(),
                                                        var->name + ".ref");
    lowering_.builder_.CreateStore(address, storage);
  }
  unsigned argument = owners_.lookup(function) == lowering_.currentFn_ ? argumentOf(var) : 0;
  if (argument && type->getSizeInBits() > 64) {
    argument = 0;
  }
  llvm::DINode::DIFlags flags =
      var == (lowering_.currentFn_ ? lowering_.currentFn_->thisVar : nullptr)
          ? llvm::DINode::FlagArtificial | llvm::DINode::FlagObjectPointer
          : llvm::DINode::FlagZero;
  llvm::DILocalVariable *variable =
      argument ? builder_->createParameterVariable(scope, var->name, argument, file, line, type,
                                                   true, flags)
               : builder_->createAutoVariable(scope, var->name, file, line, type, true, flags);
  builder_->insertDeclare(storage, variable, builder_->createExpression(),
                          llvm::DILocation::get(lowering_.ctx_, line, 0, scope),
                          llvm::InsertPosition(std::next(storage->getIterator())));
}

void DebugInfo::declareGlobal(const VarSymbol *var, llvm::Value *address) {
  auto *global = llvm::dyn_cast_or_null<llvm::GlobalVariable>(address);
  if (!types_ || !global || !var || var->name.empty() || !var->type) {
    return;
  }
  const llvm::SourceMgr &sources = *lowering_.sourceMgr_;
  unsigned buffer = var->loc.isValid() ? sources.FindBufferContainingLoc(var->loc) : 0;
  llvm::DIFile *file = fileOf(buffer ? buffer : entryBuffer_);
  unsigned line = buffer ? sources.getLineAndColumn(var->loc, buffer).first : 0;
  global->addDebugInfo(builder_->createGlobalVariableExpression(
      unit_, var->name, global->getName(), file, line, types_->of(var->type), true));
}

void DebugInfo::finish() {
  lowering_.builder_.SetCurrentDebugLocation(llvm::DebugLoc());
  if (!unit_) {
    return;
  }
  if (types_ && types_->usesAtoms()) {
    llvm::GlobalVariable *atoms = lowering_.unionLayout().atomTable();
    uint64_t count = atoms->getValueType()->getArrayNumElements();
    llvm::DIType *table = builder_->createArrayType(
        count * 128, 64, types_->of(Type::string()),
        builder_->getOrCreateArray({builder_->getOrCreateSubrange(0, int64_t(count))}));
    atoms->addDebugInfo(builder_->createGlobalVariableExpression(
        unit_, "tz_atoms", atoms->getName(), fileOf(entryBuffer_), 0, table, true));
  }
  if (types_) {
    types_->finish();
  }
  for (llvm::Function &function : *lowering_.module_) {
    if (!function.isDeclaration()) {
      repair(function);
    }
  }
  builder_->finalize();
}

}
