#include "compiler/memory/memory_analysis/memory_analysis.h"
#include "compiler/memory/cycle_finder/cycle_finder.h"
#include "compiler/memory/cycle_scanner/cycle_scanner.h"
#include "compiler/memory/held_members/held_members.h"
#include "compiler/memory/owner_scanner/owner_scanner.h"
#include "compiler/memory/store_scanner/store_scanner.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

// A class whose construction or methods let `this` go somewhere.
bool letsThisGo(const ClassDecl *cls) {
  for (const FunctionDecl *fn : cls->constructors) {
    if (fn->thisVar && CycleScanner::thisEscapes(fn->body, fn->thisVar)) {
      return true;
    }
  }
  if (cls->init && cls->init->thisVar &&
      CycleScanner::thisEscapes(cls->init->body, cls->init->thisVar)) {
    return true;
  }
  for (const FunctionDecl *fn : cls->methods) {
    if (!fn->isStatic && fn->thisVar && fn->body &&
        CycleScanner::thisEscapes(fn->body, fn->thisVar)) {
      return true;
    }
  }
  for (const ClassField &f : cls->fields) {
    if (f.method && f.method->thisVar &&
        CycleScanner::thisEscapes(f.method->body, f.method->thisVar)) {
      return true;
    }
  }

  return false;
}

} // namespace

MemoryAnalysis::MemoryAnalysis(MemoryPlan &plan) : plan_(plan) {}

void MemoryAnalysis::analyzeCycles(SourceFile &file) {
  CycleScanner scan;
  scanProgram(file, scan);
  // Layer 2: the functions in owned objects whose references to their owners need no count.
  OwnerScanner owners;
  std::vector<OwnerScanner::BackRef> backRefs = findBackRefs(file, owners);

  // The graph, without the references layer 2 proved need no count; an owned object that is
  // cyclic all the same may outlive its owner through its group, so its proof is dropped and the
  // graph made again.
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

  // WeakRef<T>: the objects of T, and of the types declared to be it.
  for (const Type *t : scan.types) {
    if (t->isWeakRef()) {
      for (const Type *s : finder.runtimeTypesOf(t->weakTarget())) {
        plan_.weakTargets.insert(s);
      }
    }
  }

  if (plan_.hasCycles) {
    findUnstored(file);
  }
  findEternals(file);
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
        if (fn->typeParams.empty() && (fn->isStatic ? !cls->generic : cls->isEmitted())) {
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
        if (fn->typeParams.empty() && (fn->isStatic ? !cls->generic : cls->isEmitted())) {
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
  // The globals that hold their values for good: declared at the top level of a file and never
  // assigned again (see pinValue). Top-level code that awaits keeps those no function uses in its
  // coroutine, which ends while timers may still run: they are not eternal there.
  for (Stmt *item : file.items) {
    if (auto *decl = dyn_cast<VarDeclStmt>(item)) {
      for (const VarDeclarator &d : decl->decls) {
        if (d.sym && !d.alias && !d.sym->reassigned && plan_.mayHoldMember(d.sym->type) &&
            (d.sym->usedInFunction || !file.topLevelAwait)) {
          plan_.eternals.insert(d.sym);
        }
      }
    }
    // The static fields of a class are globals too.
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
      return !fn->nonEscaping && plan_.isCyclicEnv(key);
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
        if (fn->typeParams.empty() && (fn->isStatic ? !cls->generic : cls->isEmitted())) {
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
      // Reflection may call any method of the class (Reflect.get, then
      // Reflect.apply), and whoever called it may keep what it gives.
      if (cls->reflectMethods) {
        for (const ClassDecl *c = cls; c; c = c->parent) {
          for (const FunctionDecl *fn : c->methods) {
            if (!fn->isStatic) {
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

} // namespace tz
