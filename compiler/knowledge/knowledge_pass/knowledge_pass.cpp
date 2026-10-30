#include "compiler/knowledge/knowledge_pass/knowledge_pass.h"
#include "compiler/knowledge/knowledge_walk/knowledge_walk.h"
#include "compiler/knowledge/symbol_text/symbol_text.h"
#include "compiler/program/ast/ast.h"
#include "compiler/program/module/module.h"
#include "compiler/program/program/program.h"
#include "compiler/program/types/type/type.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/xxhash.h"
#include <algorithm>

using llvm::dyn_cast;

namespace tz {

namespace {

const char *lineStart(const char *at, const char *begin) {
  while (at > begin && at[-1] != '\n') {
    --at;
  }
  while (*at == ' ' || *at == '\t') {
    ++at;
  }

  return at;
}

const char *endOf(llvm::StringRef text) { return text.data() + text.size(); }

}

KnowledgePass::KnowledgePass(const Program &program, llvm::StringRef entry, llvm::StringRef root)
    : program_(program), places_(program.sourceMgr) {
  places_.nameFiles(program.file.modules);
  out_.entry = SourcePlaces::absolute(entry);
  out_.root = SourcePlaces::absolute(root);
}

Knowledge KnowledgePass::run() {
  addModules();
  for (const Module *module : program_.file.modules) {
    declare(moduleIndex_.lookup(module), *module);
  }
  scopes_.resize(out_.modules.size());
  for (size_t i = 0; i < out_.symbols.size(); ++i) {
    const KnownSymbol &symbol = out_.symbols[i];
    if (symbol.parent == Knowledge::kNone) {
      scopes_[symbol.module].try_emplace(symbol.name, i);
    }
  }
  for (const Module *module : program_.file.modules) {
    bindImports(moduleIndex_.lookup(module), *module);
  }
  walk();
  addMembers();

  return std::move(out_);
}

void KnowledgePass::local(const VarSymbol *var, size_t module, size_t owner, llvm::SMLoc use) {
  if (!var || !var->loc.isValid() || var->name.empty() || var->name.starts_with("%") ||
      variables_.count(var) != 0) {
    return;
  }
  auto [found, added] = locals_.try_emplace(var->loc.getPointer(), out_.locals.size());
  if (added) {
    std::optional<SourcePlace> place = places_.locate(var->loc);
    if (!place) {
      locals_.erase(found);
      return;
    }
    KnownLocal known;
    known.name = var->name.str();
    known.type = var->type ? var->type->name().str() : "";
    known.module = module;
    known.owner = owner;
    known.place = *place;
    out_.locals.push_back(std::move(known));
  }
  if (!use.isValid() || use.getPointer() == var->loc.getPointer() ||
      !seenLocalUses_.insert(use.getPointer()).second) {
    return;
  }
  if (std::optional<SourcePlace> place = places_.locate(use)) {
    out_.locals[found->second].uses.push_back(*place);
  }
}

void KnowledgePass::addMembers() {
  std::vector<std::vector<size_t>> children(out_.symbols.size());
  for (size_t i = 0; i < out_.symbols.size(); ++i) {
    if (out_.symbols[i].parent != Knowledge::kNone) {
      children[out_.symbols[i].parent].push_back(i);
    }
  }
  auto names = [](std::initializer_list<const char *> list, const char *kind) {
    std::vector<KnownMember> out;
    for (const char *name : list) {
      out.push_back({name, kind, ""});
    }

    return out;
  };
  for (const auto &[name, type] : memberTypes_) {
    std::vector<KnownMember> found;
    if (const ClassDecl *cls = type->classDecl()) {
      std::set<std::string> seen;
      for (const ClassDecl *at = cls; at; at = at->parent) {
        size_t symbol = classSymbol(at);
        if (symbol == Knowledge::kNone) {
          continue;
        }
        for (size_t member : children[symbol]) {
          const KnownSymbol &known = out_.symbols[member];
          if (known.kind != "constructor" && seen.insert(known.name).second) {
            found.push_back({known.name, known.kind, known.signature});
          }
        }
      }
    } else if (type->isString()) {
      found = names({"length",   "at",         "charAt",     "charCodeAt",  "codePointAt",
                     "endsWith", "includes",   "indexOf",    "lastIndexOf", "localeCompare",
                     "match",    "matchAll",   "normalize",  "padEnd",      "padStart",
                     "repeat",   "replace",    "replaceAll", "search",      "slice",
                     "split",    "startsWith", "substring",  "toLowerCase", "toUpperCase",
                     "trim",     "trimEnd",    "trimStart"},
                    "method");
    } else if (type->isArray()) {
      found =
          names({"length",      "at",       "concat",    "entries",  "every",         "fill",
                 "filter",      "find",     "findIndex", "findLast", "findLastIndex", "flat",
                 "flatMap",     "forEach",  "includes",  "indexOf",  "join",          "keys",
                 "lastIndexOf", "map",      "pop",       "push",     "reduce",        "reduceRight",
                 "reverse",     "shift",    "slice",     "some",     "sort",          "splice",
                 "toReversed",  "toSorted", "unshift",   "values"},
                "method");
    } else if (type->isResult()) {
      found = names({"isOk", "isErr", "value", "error", "map", "mapErr", "unwrapOr", "match"},
                    "method");
    } else if (type->isMap()) {
      found = names(
          {"size", "get", "set", "has", "delete", "clear", "keys", "values", "entries", "forEach"},
          "method");
    } else if (type->isSet()) {
      found = names({"size", "add", "has", "delete", "clear", "values", "forEach"}, "method");
    } else if (type->isObject()) {
      for (const Field &field : type->fields()) {
        if (!field.name.empty() && field.name[0] != '%') {
          found.push_back({field.name, "field", field.type ? field.type->name().str() : ""});
        }
      }
    }
    if (!found.empty()) {
      out_.typeMembers[name] = std::move(found);
    }
  }
}

std::string KnowledgePass::moduleKey(const Module &module) const {
  if (module.builtin) {
    return module.name;
  }

  return out_.relative(SourcePlaces::absolute(module.path));
}

void KnowledgePass::addModules() {
  llvm::StringMap<unsigned> buffers;
  const llvm::SourceMgr &sourceMgr = program_.sourceMgr;
  for (unsigned id = 1; id <= sourceMgr.getNumBuffers(); ++id) {
    buffers.try_emplace(sourceMgr.getMemoryBuffer(id)->getBufferIdentifier(), id);
  }
  for (const Module *module : program_.file.modules) {
    KnownModule known;
    known.library = module->builtin;
    known.entry = module->isEntry;
    known.path = module->builtin ? module->name : SourcePlaces::absolute(module->path);
    known.file = SourcePlaces::absolute(module->path);
    known.name = moduleKey(*module);
    unsigned id = module->builtin && !module->buffers.empty() ? module->buffers.front()
                                                              : buffers.lookup(module->path);
    if (id != 0) {
      llvm::StringRef text = sourceMgr.getMemoryBuffer(id)->getBuffer();
      known.hash = llvm::utohexstr(llvm::xxh3_64bits(llvm::arrayRefFromStringRef(text)), true, 16);
      known.lines = static_cast<unsigned>(text.count('\n')) + (text.ends_with("\n") ? 0 : 1);
    }
    moduleIndex_[module] = out_.modules.size();
    out_.modules.push_back(std::move(known));
  }
  for (const Module *module : program_.file.modules) {
    KnownModule &known = out_.modules[moduleIndex_.lookup(module)];
    for (const ImportDecl &decl : module->file.imports) {
      if (decl.implicit) {
        continue;
      }
      known.specifiers.push_back(decl.path);
      auto found = moduleIndex_.find(decl.from);
      if (found != moduleIndex_.end() && !llvm::is_contained(known.imports, found->second)) {
        known.imports.push_back(found->second);
      }
    }
  }
}

size_t KnowledgePass::add(size_t module, std::string kind, llvm::StringRef name, llvm::SMLoc loc,
                          const char *begin, const char *end, size_t parent) {
  std::optional<SourcePlace> place = places_.locate(loc);
  if (!place) {
    return Knowledge::kNone;
  }
  KnownSymbol symbol;
  symbol.kind = std::move(kind);
  symbol.name = name.str();
  symbol.module = module;
  symbol.parent = parent;
  symbol.place = *place;
  std::optional<SourcePlace> extent = begin && end ? places_.span(begin, end) : std::nullopt;
  symbol.extent = extent ? *extent : *place;
  if (extent) {
    unsigned id = program_.sourceMgr.FindBufferContainingLoc(llvm::SMLoc::getFromPointer(begin));
    const char *first = id == 0 ? begin : program_.sourceMgr.getMemoryBuffer(id)->getBufferStart();
    const char *from = begin;
    while (from > first && from[-1] != '\n') {
      --from;
    }
    symbol.source = std::string(from, end);
  }
  if (parent != Knowledge::kNone) {
    symbol.container = out_.symbols[parent].name;
  }
  std::string key = out_.modules[module].name + "#" +
                    (symbol.container.empty() ? symbol.name : symbol.container + "." + symbol.name);
  std::string unique = key;
  for (unsigned n = 2; keys_.count(unique) != 0; ++n) {
    unique = key + "/" + std::to_string(n);
  }
  keys_.insert(unique);
  symbol.key = unique;
  out_.symbols.push_back(std::move(symbol));

  return out_.symbols.size() - 1;
}

void KnowledgePass::declareClass(size_t module, const ClassDecl &cls, size_t parent) {
  const char *begin = cls.source.empty() ? nullptr : cls.source.data();
  size_t own = add(module, "class", cls.name, cls.loc(), begin, endOf(cls.source), parent);
  if (own == Knowledge::kNone) {
    return;
  }
  out_.symbols[own].signature = SymbolText::header(cls);
  classes_[&cls] = own;
  for (const ClassField &field : cls.fields) {
    if (field.parameter && field.method) {
      continue;
    }
    llvm::StringRef line = places_.lineText(field.loc);
    const char *start = line.empty() ? nullptr : lineStart(field.loc.getPointer(), line.data());
    size_t member = add(module, "field", field.name, field.loc, start, endOf(line), own);
    if (member == Knowledge::kNone) {
      continue;
    }
    out_.symbols[member].signature = SymbolText::field(field);
    fields_[{&cls, field.name.str(), field.isStatic}] = member;
  }
  for (const auto *list : {&cls.constructors, &cls.methods}) {
    for (const FunctionDecl *method : *list) {
      const char *start = method->source.empty() ? nullptr : method->source.data();
      size_t member = add(module, method->isConstructor ? "constructor" : "method",
                          method->isConstructor ? llvm::StringRef("constructor") : method->name,
                          method->loc(), start, endOf(method->source), own);
      if (member == Knowledge::kNone) {
        continue;
      }
      out_.symbols[member].signature = SymbolText::function(*method);
      functions_[method] = member;
      declaredAt_[method->loc().getPointer()] = member;
    }
  }
}

void KnowledgePass::declare(size_t module, const Module &source) {
  llvm::StringSet<> exported;
  for (const ExportDecl &decl : source.file.exports) {
    if (decl.path.empty()) {
      for (const ModuleName &name : decl.names) {
        exported.insert(name.name);
      }
    }
  }
  size_t first = out_.symbols.size();
  for (const Stmt *item : source.file.items) {
    if (auto *fn = dyn_cast<FunctionDecl>(item)) {
      if (fn->generic || fn->name.empty()) {
        continue;
      }
      const char *start = fn->source.empty() ? nullptr : fn->source.data();
      size_t symbol =
          add(module, "function", fn->name, fn->loc(), start, endOf(fn->source), Knowledge::kNone);
      if (symbol != Knowledge::kNone) {
        out_.symbols[symbol].signature = SymbolText::function(*fn);
        functions_[fn] = symbol;
        declaredAt_[fn->loc().getPointer()] = symbol;
      }
    } else if (auto *cls = dyn_cast<ClassDecl>(item)) {
      if (!cls->generic && !cls->name.empty()) {
        declareClass(module, *cls, Knowledge::kNone);
      }
    } else if (auto *vars = dyn_cast<VarDeclStmt>(item)) {
      for (const VarDeclarator &declarator : vars->decls) {
        if (declarator.name.starts_with("%") || declarator.name.empty()) {
          continue;
        }
        llvm::StringRef line = places_.lineText(declarator.loc);
        const char *start =
            line.empty() ? nullptr : lineStart(declarator.loc.getPointer(), line.data());
        unsigned id = program_.sourceMgr.FindBufferContainingLoc(declarator.loc);
        const char *end = start && id != 0
                              ? SymbolText::statementEnd(
                                    start, program_.sourceMgr.getMemoryBuffer(id)->getBufferEnd())
                              : nullptr;
        size_t symbol = add(module, vars->isConst ? "const" : "let", declarator.name,
                            declarator.loc, start, end, Knowledge::kNone);
        if (symbol != Knowledge::kNone) {
          out_.symbols[symbol].signature = SymbolText::variable(declarator, vars->isConst);
          if (declarator.sym) {
            variables_[declarator.sym] = symbol;
          }
        }
      }
    } else if (auto *alias = dyn_cast<TypeAliasDecl>(item)) {
      unsigned id = program_.sourceMgr.FindBufferContainingLoc(alias->loc());
      llvm::StringRef line = places_.lineText(alias->loc());
      if (id == 0 || line.empty()) {
        continue;
      }
      const char *start = lineStart(alias->loc().getPointer(), line.data());
      const char *end =
          SymbolText::statementEnd(start, program_.sourceMgr.getMemoryBuffer(id)->getBufferEnd());
      size_t symbol = add(module, "type", alias->name, alias->loc(), start, end, Knowledge::kNone);
      if (symbol != Knowledge::kNone) {
        out_.symbols[symbol].signature =
            SymbolText::collapse(llvm::StringRef(start, end - start), 400);
      }
    } else if (auto *e = dyn_cast<EnumDecl>(item)) {
      unsigned id = program_.sourceMgr.FindBufferContainingLoc(e->loc());
      llvm::StringRef line = places_.lineText(e->loc());
      if (id == 0 || line.empty()) {
        continue;
      }
      const char *start = lineStart(e->loc().getPointer(), line.data());
      const char *end =
          SymbolText::statementEnd(start, program_.sourceMgr.getMemoryBuffer(id)->getBufferEnd());
      size_t symbol = add(module, "enum", e->name, e->loc(), start, end, Knowledge::kNone);
      if (symbol != Knowledge::kNone) {
        out_.symbols[symbol].signature =
            SymbolText::collapse(llvm::StringRef(start, end - start), 400);
      }
    }
  }
  for (size_t i = first; i < out_.symbols.size(); ++i) {
    KnownSymbol &symbol = out_.symbols[i];
    if (symbol.parent == Knowledge::kNone) {
      symbol.exported = exported.contains(symbol.name);
    } else {
      symbol.exported = out_.symbols[symbol.parent].exported;
    }
  }
}

void KnowledgePass::bindImports(size_t module, const Module &source) {
  for (const ImportDecl &decl : source.file.imports) {
    auto from = moduleIndex_.find(decl.from);
    if (from == moduleIndex_.end()) {
      continue;
    }
    for (const ModuleName &name : decl.names) {
      auto found = scopes_[from->second].find(name.name);
      if (found != scopes_[from->second].end()) {
        scopes_[module].try_emplace(name.as.empty() ? name.name : name.as, found->second);
        reference(found->second, Knowledge::kNone, module, name.loc, false);
      }
    }
  }
  for (const ExportDecl &decl : source.file.exports) {
    if (!decl.path.empty()) {
      continue;
    }
    for (const ModuleName &name : decl.names) {
      auto found = scopes_[module].find(name.name);
      if (found != scopes_[module].end()) {
        reference(found->second, Knowledge::kNone, module, name.loc, false);
      }
    }
  }
}

size_t KnowledgePass::functionSymbol(const FunctionDecl *fn) const {
  if (!fn) {
    return Knowledge::kNone;
  }
  auto found = functions_.find(fn);
  if (found != functions_.end()) {
    return found->second;
  }
  if (fn->generic) {
    return functionSymbol(fn->generic);
  }
  auto copied = declaredAt_.find(fn->loc().getPointer());
  if (copied != declaredAt_.end()) {
    return copied->second;
  }
  if (fn->cls && fn->cls->generic) {
    const ClassDecl *original = fn->cls->generic;
    for (const auto &[own, theirs] : {std::pair(&fn->cls->methods, &original->methods),
                                      std::pair(&fn->cls->constructors, &original->constructors)}) {
      auto at = std::find(own->begin(), own->end(), fn);
      size_t index = static_cast<size_t>(at - own->begin());
      if (at != own->end() && index < theirs->size()) {
        return functionSymbol((*theirs)[index]);
      }
    }
  }

  return Knowledge::kNone;
}

size_t KnowledgePass::classSymbol(const ClassDecl *cls) const {
  for (const ClassDecl *at = cls; at; at = at->generic) {
    auto found = classes_.find(at);
    if (found != classes_.end()) {
      return found->second;
    }
  }

  return Knowledge::kNone;
}

size_t KnowledgePass::variableSymbol(const VarSymbol *var) const {
  auto found = variables_.find(var);

  return found == variables_.end() ? Knowledge::kNone : found->second;
}

size_t KnowledgePass::fieldSymbol(const ClassDecl *cls, llvm::StringRef name, bool isStatic) const {
  for (const ClassDecl *at = cls; at; at = at->parent) {
    const ClassDecl *original = at;
    while (original->generic) {
      original = original->generic;
    }
    auto found = fields_.find({original, name.str(), isStatic});
    if (found != fields_.end()) {
      return found->second;
    }
  }

  return Knowledge::kNone;
}

size_t KnowledgePass::typeSymbol(size_t module, llvm::StringRef name) const {
  if (module >= scopes_.size()) {
    return Knowledge::kNone;
  }
  auto found = scopes_[module].find(name);
  if (found == scopes_[module].end()) {
    return Knowledge::kNone;
  }
  const std::string &kind = out_.symbols[found->second].kind;

  return kind == "class" || kind == "type" || kind == "enum" ? found->second : Knowledge::kNone;
}

bool KnowledgePass::reference(size_t symbol, size_t from, size_t module, llvm::SMLoc loc,
                              bool call) {
  if (!seenReferences_.insert({symbol, loc.getPointer()}).second) {
    return true;
  }
  std::optional<SourcePlace> place = places_.locate(loc);
  if (!place) {
    return false;
  }
  const KnownSymbol &target = out_.symbols[symbol];
  if (target.place.path == place->path && target.place.line == place->line &&
      target.place.character == place->character) {
    return true;
  }
  KnownReference reference;
  reference.symbol = symbol;
  reference.from = from;
  reference.module = module;
  reference.place = *place;
  reference.call = call;
  reference.line = SymbolText::collapse(places_.lineText(loc), 200);
  out_.references.push_back(std::move(reference));

  return true;
}

void KnowledgePass::typed(size_t module, llvm::SMLoc loc, llvm::StringRef name, const Type *type) {
  if (!type || type->isError() || name.empty() || name.starts_with("%")) {
    return;
  }
  if (!seenTypes_.insert(loc.getPointer()).second) {
    return;
  }
  std::optional<SourcePlace> place = places_.locate(loc);
  if (!place) {
    return;
  }
  const Type *inner = type->nonOptional();
  out_.types.push_back({name.str(), type->name().str(), inner->name().str(), module, *place});
  memberTypes_.try_emplace(inner->name().str(), inner);
}

void KnowledgePass::walk() {
  for (const Module *module : program_.file.modules) {
    size_t index = moduleIndex_.lookup(module);
    for (const Stmt *item : module->file.items) {
      size_t from = Knowledge::kNone;
      if (auto *fn = dyn_cast<FunctionDecl>(item)) {
        from = functionSymbol(fn);
      } else if (auto *cls = dyn_cast<ClassDecl>(item)) {
        from = classSymbol(cls);
      } else if (auto *vars = dyn_cast<VarDeclStmt>(item)) {
        for (const VarDeclarator &declarator : vars->decls) {
          size_t own = declarator.sym ? variableSymbol(declarator.sym) : Knowledge::kNone;
          KnowledgeWalk(*this, index, own).declarator(declarator);
        }
        continue;
      } else if (auto *alias = dyn_cast<TypeAliasDecl>(item)) {
        auto found = scopes_[index].find(alias->name);
        from = found == scopes_[index].end() ? Knowledge::kNone : found->second;
      }
      KnowledgeWalk(*this, index, from).stmt(item);
    }
  }
  for (const FunctionDecl *fn : program_.file.instances) {
    size_t symbol = functionSymbol(fn);
    if (symbol != Knowledge::kNone) {
      KnowledgeWalk(*this, out_.symbols[symbol].module, symbol).function(fn);
      continue;
    }
    std::optional<SourcePlace> place = places_.locate(fn->loc());
    if (!place) {
      continue;
    }
    size_t module = Knowledge::kNone;
    for (size_t m = 0; m < out_.modules.size() && module == Knowledge::kNone; ++m) {
      if (out_.modules[m].path == place->path) {
        module = m;
      }
    }
    if (module == Knowledge::kNone) {
      continue;
    }
    std::vector<size_t> around = out_.at(module, place->line, std::nullopt);
    size_t from = around.size() == 1 ? around.front() : Knowledge::kNone;
    KnowledgeWalk(*this, module, from).function(fn);
  }
  std::vector<const ClassDecl *> originals;
  for (const auto &[cls, symbol] : classes_) {
    originals.push_back(cls);
  }
  for (const ClassDecl *cls : originals) {
    size_t symbol = classes_.lookup(cls);
    for (const ClassDecl *instance : cls->instances) {
      KnowledgeWalk(*this, out_.symbols[symbol].module, symbol).classDecl(instance);
    }
  }
}

}
