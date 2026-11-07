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

DebugInfo::DebugInfo(Lowering &owner)
    : lowering(owner) {}

DebugInfo::~DebugInfo() = default;

void DebugInfo::start(const SourceFile &file) {
  if (this->lowering.debugLevel_ == DebugLevel::None || !this->lowering.sourceMgr_ ||
      this->lowering.sourceMgr_->getNumBuffers() == 0) {
    return;
  }
  for (const Module *module : file.modules) {
    for (const auto &[buffer, path] : module->bufferPaths) {
      this->paths[buffer] = path;
    }
  }
  llvm::Module &module = *this->lowering.module_;
  if (this->lowering.codeView_) {
    module.addModuleFlag(llvm::Module::Warning, "CodeView", 1);
  } else {
    module.addModuleFlag(llvm::Module::Warning, "Dwarf Version", 5);
  }
  module.addModuleFlag(llvm::Module::Warning, "Debug Info Version", llvm::DEBUG_METADATA_VERSION);
  this->builder = std::make_unique<llvm::DIBuilder>(module);
  this->entryBuffer = this->lowering.sourceMgr_->getMainFileID();
  this->unit = this->builder->createCompileUnit(
      llvm::DISourceLanguageName(llvm::dwarf::DW_LANG_C_plus_plus), this->fileOf(this->entryBuffer),
      "tz", this->lowering.optimized_, "", 0, "",
      this->lowering.debugLevel_ == DebugLevel::Full ? llvm::DICompileUnit::FullDebug
                                                     : llvm::DICompileUnit::LineTablesOnly);
  this->signature = this->builder->createSubroutineType(this->builder->getOrCreateTypeArray({}));
  if (this->lowering.debugLevel_ == DebugLevel::Full) {
    this->types = std::make_unique<DebugTypes>(this->lowering, *this->builder, this->unit);
  }
}

llvm::DIFile *DebugInfo::fileOf(unsigned buffer) {
  llvm::DIFile *found = this->files.lookup(buffer);
  if (found) {
    return found;
  }
  llvm::SmallString<256> path;
  auto known = this->paths.find(buffer);
  if (known != this->paths.end()) {
    path = known->second;
  } else {
    path = this->lowering.sourceMgr_->getMemoryBuffer(buffer)->getBufferIdentifier();
  }
  llvm::sys::fs::make_absolute(path);
  llvm::sys::path::remove_dots(path, true);
  llvm::sys::path::native(path);
  llvm::DIFile *file = this->builder->createFile(llvm::sys::path::filename(path),
                                                 llvm::sys::path::parent_path(path));
  this->files[buffer] = file;

  return file;
}

llvm::DIScope *DebugInfo::scopeIn(llvm::DISubprogram *subprogram, llvm::DIFile *file) {
  if (subprogram->getFile() == file) {
    return subprogram;
  }
  llvm::DIScope *&scope = this->scopes[{subprogram, file}];
  if (!scope) {
    scope = this->builder->createLexicalBlockFile(subprogram, file);
  }

  return scope;
}

llvm::DISubprogram *DebugInfo::describe(llvm::Function *function, llvm::StringRef name,
                                        llvm::SMLoc loc) {
  const llvm::SourceMgr &sources = *this->lowering.sourceMgr_;
  unsigned buffer = loc.isValid() ? sources.FindBufferContainingLoc(loc) : this->entryBuffer;
  if (!buffer) {
    buffer = this->entryBuffer;
  }
  unsigned line = loc.isValid() ? sources.getLineAndColumn(loc, buffer).first : 1;
  llvm::DIFile *file = this->fileOf(buffer);
  llvm::DISubprogram::DISPFlags flags = llvm::DISubprogram::SPFlagDefinition;
  if (this->lowering.optimized_) {
    flags |= llvm::DISubprogram::SPFlagOptimized;
  }
  if (function->hasLocalLinkage()) {
    flags |= llvm::DISubprogram::SPFlagLocalToUnit;
  }
  llvm::DISubprogram *subprogram =
      this->builder->createFunction(file, name, function->getName(), file, line, this->signature,
                                    line, llvm::DINode::FlagPrototyped, flags);
  function->setSubprogram(subprogram);
  this->lowering.builder_.SetCurrentDebugLocation(
      llvm::DILocation::get(this->lowering.ctx_, line, 0, subprogram));

  return subprogram;
}

void DebugInfo::beginFunction(llvm::Function *function, const FunctionDecl *fn) {
  this->lowering.builder_.SetCurrentDebugLocation(llvm::DebugLoc());
  if (!this->unit || !fn || function->getSubprogram()) {
    return;
  }
  std::string name = fn->name.empty() ? "<anonymous>" : fn->name.str();
  if (fn->cls() && !fn->cls()->literal()) {
    name = fn->isConstructor() || fn == fn->cls()->init ? "new " + fn->cls()->name.str()
                                                        : (fn->cls()->name + "." + name).str();
  }
  this->describe(function, name, fn->loc());
  this->owners[function] = fn;
}

void DebugInfo::beginTopLevel(llvm::Function *function) {
  if (this->unit && !function->getSubprogram()) {
    this->describe(function, "<top level>", llvm::SMLoc());
  }
}

void DebugInfo::at(llvm::SMLoc loc) {
  if (!this->unit || !loc.isValid()) {
    return;
  }
  llvm::BasicBlock *block = this->lowering.builder_.GetInsertBlock();
  llvm::DISubprogram *subprogram = block ? block->getParent()->getSubprogram() : nullptr;
  if (!subprogram) {
    return;
  }
  const llvm::SourceMgr &sources = *this->lowering.sourceMgr_;
  unsigned buffer = sources.FindBufferContainingLoc(loc);
  if (!buffer) {
    return;
  }
  auto [line, column] = sources.getLineAndColumn(loc, buffer);
  llvm::DIScope *scope = this->scopeIn(subprogram, this->fileOf(buffer));
  llvm::DebugLoc current = this->lowering.builder_.getCurrentDebugLocation();
  if (current && current->getLine() == line && current->getScope() == scope) {
    return;
  }
  this->lowering.builder_.SetCurrentDebugLocation(
      llvm::DILocation::get(this->lowering.ctx_, line, column, scope));
}

llvm::DebugLoc DebugInfo::here() const {
  return this->lowering.builder_.getCurrentDebugLocation();
}

void DebugInfo::restore(const llvm::DebugLoc &loc) {
  if (this->unit) {
    this->lowering.builder_.SetCurrentDebugLocation(loc);
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
        last = llvm::DILocation::get(this->lowering.ctx_, subprogram->getLine(), 0, subprogram);
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
  const FunctionDecl *fn = this->lowering.currentFn_;
  if (!fn) {
    return 0;
  }
  unsigned first = fn->thisVar() ? 2 : 1;
  if (var == fn->thisVar()) {
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
  llvm::BasicBlock *block = this->lowering.builder_.GetInsertBlock();
  llvm::Function *function = block ? block->getParent() : nullptr;
  llvm::DISubprogram *subprogram = function ? function->getSubprogram() : nullptr;
  if (!this->types || !subprogram || !var || var->name.empty() || !var->type || !address ||
      !this->declared.insert({function, var}).second) {
    return;
  }
  const llvm::SourceMgr &sources = *this->lowering.sourceMgr_;
  unsigned buffer = var->loc.isValid() ? sources.FindBufferContainingLoc(var->loc) : 0;
  llvm::DIFile *file = buffer ? this->fileOf(buffer) : subprogram->getFile();
  unsigned line = buffer ? sources.getLineAndColumn(var->loc, buffer).first : subprogram->getLine();
  llvm::DIScope *scope = this->scopeIn(subprogram, file);
  llvm::DIType *type = this->types->of(var->type);
  auto *storage = llvm::dyn_cast<llvm::AllocaInst>(address);
  if (!storage) {
    if (type->getTag() == llvm::dwarf::DW_TAG_reference_type) {
      type = this->builder->createPointerType(llvm::cast<llvm::DIDerivedType>(type)->getBaseType(),
                                              64);
    }
    type = this->types->referenceTo(type);
    storage = this->lowering.functionFrame().createStackSlot(this->lowering.builder_.getPtrTy(),
                                                             var->name + ".ref");
    this->lowering.builder_.CreateStore(address, storage);
  }
  unsigned argument =
      this->owners.lookup(function) == this->lowering.currentFn_ ? this->argumentOf(var) : 0;
  if (argument && type->getSizeInBits() > 64) {
    argument = 0;
  }
  llvm::DINode::DIFlags flags =
      var == (this->lowering.currentFn_ ? this->lowering.currentFn_->thisVar() : nullptr)
          ? llvm::DINode::FlagArtificial | llvm::DINode::FlagObjectPointer
          : llvm::DINode::FlagZero;
  llvm::DILocalVariable *variable =
      argument ? this->builder->createParameterVariable(scope, var->name, argument, file, line,
                                                        type, true, flags)
               : this->builder->createAutoVariable(scope, var->name, file, line, type, true, flags);
  this->builder->insertDeclare(storage, variable, this->builder->createExpression(),
                               llvm::DILocation::get(this->lowering.ctx_, line, 0, scope),
                               llvm::InsertPosition(std::next(storage->getIterator())));
}

void DebugInfo::declareGlobal(const VarSymbol *var, llvm::Value *address) {
  auto *global = llvm::dyn_cast_or_null<llvm::GlobalVariable>(address);
  if (!this->types || !global || !var || var->name.empty() || !var->type) {
    return;
  }
  const llvm::SourceMgr &sources = *this->lowering.sourceMgr_;
  unsigned buffer = var->loc.isValid() ? sources.FindBufferContainingLoc(var->loc) : 0;
  llvm::DIFile *file = this->fileOf(buffer ? buffer : this->entryBuffer);
  unsigned line = buffer ? sources.getLineAndColumn(var->loc, buffer).first : 0;
  global->addDebugInfo(this->builder->createGlobalVariableExpression(
      this->unit, var->name, global->getName(), file, line, this->types->of(var->type), true));
}

void DebugInfo::finish() {
  this->lowering.builder_.SetCurrentDebugLocation(llvm::DebugLoc());
  if (!this->unit) {
    return;
  }
  if (this->types && this->types->usesAtoms()) {
    llvm::GlobalVariable *atoms = this->lowering.unionLayout().atomTable();
    uint64_t count = atoms->getValueType()->getArrayNumElements();
    llvm::DIType *table = this->builder->createArrayType(
        count * 128, 64, this->types->of(Type::string()),
        this->builder->getOrCreateArray({this->builder->getOrCreateSubrange(0, int64_t(count))}));
    atoms->addDebugInfo(this->builder->createGlobalVariableExpression(
        this->unit, "tz_atoms", atoms->getName(), this->fileOf(this->entryBuffer), 0, table, true));
  }
  if (this->types) {
    this->types->finish();
  }
  for (llvm::Function &function : *this->lowering.module_) {
    if (!function.isDeclaration()) {
      this->repair(function);
    }
  }
  this->builder->finalize();
}

}
