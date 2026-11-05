#include "compiler/memory/memory_analysis/memory_analysis.h"
#include "compiler/memory/cycle_finder/cycle_finder.h"
#include "compiler/memory/cycle_scanner/cycle_scanner.h"
#include "compiler/memory/held_members/held_members.h"
#include "compiler/memory/owner_scanner/owner_scanner.h"
#include "compiler/memory/store_scanner/store_scanner.h"
#include "llvm/ADT/STLExtras.h"
#include <algorithm>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

bool letsThisGo(const ClassDecl *cls) {
  for (const FunctionDecl *fn : cls->constructors) {
    if (fn->thisVar() && CycleScanner::thisEscapes(fn->body, fn->thisVar())) {
      return true;
    }
  }
  if (cls->init && cls->init->thisVar() &&
      CycleScanner::thisEscapes(cls->init->body, cls->init->thisVar())) {
    return true;
  }
  for (const FunctionDecl *fn : cls->methods) {
    if (!fn->isStatic() && fn->thisVar() && fn->body &&
        CycleScanner::thisEscapes(fn->body, fn->thisVar())) {
      return true;
    }
  }
  for (const ClassField &f : cls->fields) {
    if (f.method && f.method->thisVar() &&
        CycleScanner::thisEscapes(f.method->body, f.method->thisVar())) {
      return true;
    }
  }

  return false;
}

}

MemoryAnalysis::MemoryAnalysis(MemoryPlan &plan) : plan_(plan) {}

void MemoryAnalysis::analyzeCycles(SourceFile &file) {
  CycleScanner scan;
  scanProgram(file, scan);
  OwnerScanner owners;
  std::vector<OwnerScanner::BackRef> backRefs = findBackRefs(file, owners);

  CycleFinder finder(plan_, scan, file);
  for (;;) {
    plan_.uncounted.clear();
    for (const OwnerScanner::BackRef &b : backRefs) {
      plan_.uncounted.insert({b.fn, b.self});
    }
    finder.run();
    if (plan_.hasCycles) {
      HeldMembers held(plan_, scan, finder);
      held.run();
    }
    size_t before = backRefs.size();
    llvm::erase_if(backRefs, [&](const OwnerScanner::BackRef &b) {
      const ClassDecl *made = owners.madeBy(b.field);

      return !made || plan_.refMayBeMember(made->type);
    });
    if (backRefs.size() == before) {
      for (const OwnerScanner::BackRef &b : backRefs) {
        plan_.ownedFields.insert({b.owner->type, b.field.str()});
        plan_.backRefFields.insert({owners.madeBy(b.field)->type, b.callback.str()});
      }
      break;
    }
  }

  for (const Type *t : scan.types) {
    if (t->isWeakRef()) {
      for (const Type *s : finder.runtimeTypesOf(t->weakTarget())) {
        plan_.weakTargets.insert(s);
      }
    }
  }
  for (const Type *t : file.listenerReceivers) {
    for (const Type *s : finder.runtimeTypesOf(t)) {
      plan_.weakTargets.insert(s);
    }
  }

  plan_.carriedMembers.clear();
  if (plan_.hasCycles) {
    for (const InstanceFlow &flow : scan.instanceFlows) {
      carry(flow.concrete, flow.rep, flow.out, 0);
    }
    findUnstored(file);
  }
  findEternals(file);
}

void MemoryAnalysis::carry(const Type *concrete, const Type *rep, bool out, unsigned depth) {
  if (!concrete || !rep || concrete == rep || depth > 16 || concrete->kind() != rep->kind()) {
    return;
  }
  switch (concrete->kind()) {
  case Type::Kind::Optional:
    carry(concrete->inner(), rep->inner(), out, depth + 1);
    return;
  case Type::Kind::Result:
    carry(concrete->okType(), rep->okType(), out, depth + 1);
    carry(concrete->errType(), rep->errType(), out, depth + 1);
    return;
  case Type::Kind::Promise:
    carry(concrete->promised(), rep->promised(), out, depth + 1);
    return;
  case Type::Kind::Function: {
    size_t count = std::min(concrete->params().size(), rep->params().size());
    for (size_t i = 0; i < count; ++i) {
      carry(concrete->params()[i], rep->params()[i], !out, depth + 1);
    }
    carry(concrete->ret(), rep->ret(), out, depth + 1);
    return;
  }
  case Type::Kind::Union:
    if (concrete->members().size() == rep->members().size()) {
      for (size_t i = 0; i < concrete->members().size(); ++i) {
        carry(concrete->members()[i], rep->members()[i], out, depth + 1);
      }
    }
    return;
  default:
    break;
  }
  if (rep->isAnyObject()) {
    return;
  }
  if (out && plan_.refMayBeMember(rep) && !plan_.refMayBeMember(concrete)) {
    plan_.carriedMembers.insert(concrete);
  }
  switch (concrete->kind()) {
  case Type::Kind::Array:
    carry(concrete->inner(), rep->inner(), out, depth + 1);
    break;
  case Type::Kind::Dict:
    carry(concrete->keyType(), rep->keyType(), out, depth + 1);
    carry(concrete->valueType(), rep->valueType(), out, depth + 1);
    break;
  case Type::Kind::Object:
    for (const Field &f : concrete->fields()) {
      if (const Field *other = rep->field(f.name)) {
        carry(f.type, other->type, out, depth + 1);
      }
    }
    break;
  default:
    break;
  }
}

void MemoryAnalysis::scanProgram(SourceFile &file, CycleScanner &scan) {
  for (Stmt *item : file.items) {
    if (auto *fn = dyn_cast<FunctionDecl>(item)) {
      if (fn->typeParams.empty() && !file.unusedFunctions.contains(fn)) {
        scan.function(fn);
      }
      continue;
    }
    if (auto *cls = dyn_cast<ClassDecl>(item)) {
      for (const FunctionDecl *fn : cls->methods) {
        if (fn->typeParams.empty() && (fn->isStatic() ? !cls->generic : cls->isEmitted())) {
          scan.function(fn);
        }
      }
      if (cls->isEmitted()) {
        for (const FunctionDecl *fn : cls->methodInstances) {
          scan.function(fn);
        }
        for (const FunctionDecl *fn : cls->constructors) {
          scan.function(fn);
        }
        scan.function(cls->init);
        for (const ClassField &f : cls->fields) {
          scan.function(f.method);
        }
        scan.classDecl(cls);
      }
      continue;
    }
    scan.stmt(item);
  }
  for (const FunctionDecl *fn : file.instances) {
    scan.function(fn);
  }
  for (const Type *type : plan_.subtypes) {
    scan.addType(type);
  }
  for (const auto &[from, to] : file.views) {
    scan.addType(from);
    scan.addType(to);
  }
}

std::vector<OwnerScanner::BackRef> MemoryAnalysis::findBackRefs(SourceFile &file,
                                                                OwnerScanner &owners) {
  for (Stmt *item : file.items) {
    if (auto *fn = dyn_cast<FunctionDecl>(item)) {
      if (fn->typeParams.empty() && !file.unusedFunctions.contains(fn)) {
        owners.function(fn);
      }
      continue;
    }
    if (auto *cls = dyn_cast<ClassDecl>(item)) {
      for (const FunctionDecl *fn : cls->methods) {
        if (fn->typeParams.empty() && (fn->isStatic() ? !cls->generic : cls->isEmitted())) {
          owners.function(fn);
        }
      }
      if (cls->isEmitted()) {
        for (const FunctionDecl *fn : cls->methodInstances) {
          owners.function(fn);
        }
        for (const FunctionDecl *fn : cls->constructors) {
          owners.function(fn);
        }
        owners.function(cls->init);
        for (const ClassField &f : cls->fields) {
          owners.function(f.method);
        }
      }
    }
    owners.topLevel(item);
  }
  for (const FunctionDecl *fn : file.instances) {
    owners.function(fn);
  }

  return owners.proven(letsThisGo);
}

void MemoryAnalysis::findEternals(SourceFile &file) {
  for (Stmt *item : file.items) {
    if (auto *decl = dyn_cast<VarDeclStmt>(item)) {
      for (const VarDeclarator &d : decl->decls) {
        if (d.sym && !d.alias && !d.sym->reassigned && plan_.mayHoldMember(d.sym->type) &&
            (d.sym->usedInFunction || !file.topLevelAwait)) {
          plan_.eternals.insert(d.sym);
        }
      }
    }
    if (auto *cls = dyn_cast<ClassDecl>(item)) {
      for (const ClassField &f : cls->fields) {
        if (f.isStatic && f.var && f.init && !f.var->reassigned &&
            plan_.mayHoldMember(f.var->type)) {
          plan_.eternals.insert(f.var);
        }
      }
    }
  }
}

void MemoryAnalysis::findUnstored(SourceFile &file) {
  auto isSite = [this](const Expr *e) -> bool {
    switch (e->kind()) {
    case Expr::Kind::New: {
      auto *create = cast<NewExpr>(e);
      return create->cls && plan_.isCyclic(create->cls->type);
    }
    case Expr::Kind::ObjectLit: {
      auto *lit = cast<ObjectLitExpr>(e);
      const Type *type = lit->cls ? lit->cls->type : lit->type;
      return type && plan_.isCyclic(type);
    }
    case Expr::Kind::ArrayLit:
      return e->type && plan_.isCyclic(e->type);
    case Expr::Kind::Call:
      switch (cast<CallExpr>(e)->builtin) {
      case Builtin::ArrSlice:
      case Builtin::ArrMap:
      case Builtin::ArrFilter:
      case Builtin::ArrFlatMap:
      case Builtin::ArrConcat:
      case Builtin::DictNew:
        return e->type && plan_.isCyclic(e->type);
      default:

        return false;
      }
    case Expr::Kind::Function: {
      const FunctionDecl *fn = cast<FunctionExpr>(e)->fn;
      const void *key = fn->group ? static_cast<const void *>(fn->group) : fn;
      return !fn->nonEscaping && !fn->receiverEnv && plan_.isCyclicEnv(key);
    }
    default:
      return false;
    }
  };
  StoreScanner scan(isSite);
  for (Stmt *item : file.items) {
    if (auto *fn = dyn_cast<FunctionDecl>(item)) {
      if (fn->typeParams.empty() && !file.unusedFunctions.contains(fn)) {
        scan.function(fn);
      }
      continue;
    }
    if (auto *cls = dyn_cast<ClassDecl>(item)) {
      for (const FunctionDecl *fn : cls->methods) {
        if (fn->typeParams.empty() && (fn->isStatic() ? !cls->generic : cls->isEmitted())) {
          scan.function(fn);
        }
      }
      if (cls->isEmitted()) {
        for (const FunctionDecl *fn : cls->methodInstances) {
          scan.function(fn);
        }
        for (const FunctionDecl *fn : cls->constructors) {
          scan.function(fn);
        }
        scan.function(cls->init);
        for (const ClassField &f : cls->fields) {
          scan.function(f.method);
        }
      }
      if (cls->reflectMethods) {
        for (const ClassDecl *c = cls; c; c = c->parent) {
          for (const FunctionDecl *fn : c->methods) {
            if (!fn->isStatic()) {
              scan.escapes(fn);
            }
          }
        }
      }
    }
    scan.topLevel(item);
  }
  for (const FunctionDecl *fn : file.instances) {
    scan.function(fn);
  }
  plan_.unstoredSites = scan.unstored();
}

}
