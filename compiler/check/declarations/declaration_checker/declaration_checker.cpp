#include "compiler/check/declarations/declaration_checker/declaration_checker.h"
#include "compiler/check/top_level_checker/top_level_checker.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/declare/module_declarer/module_declarer.h"
#include "compiler/declare/type_declarer/type_declarer.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Path.h"

using llvm::dyn_cast;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

const char *accessWord(Access access) {
  return access == Access::Public      ? "public"
         : access == Access::Protected ? "protected"
                                       : "private";
}

}

DeclarationChecker::DeclarationChecker(Checker &checker) : checker_(checker) {}

std::string DeclarationChecker::fileName() const {
  return llvm::sys::path::filename(module_->path).str();
}

const Type *DeclarationChecker::resolve(const std::optional<TypeRef> &ref, bool allowVoid) {
  if (!ref) {
    return nullptr;
  }

  return checker_.typeResolver().resolveType(*ref, allowVoid);
}

void DeclarationChecker::mismatch(llvm::SMLoc loc, const std::string &what, const Type *here,
                                  const Type *there) {
  checker_.diags_.error(loc, what + " is '" + here->name().str() + "' here, and '" +
                                 there->name().str() + "' in '" + fileName() + "'");
}

void DeclarationChecker::check(Module *module) {
  module_ = module;
  Module *outer = checker_.module_;
  checker_.moduleDeclarer().useModule(module);
  exported_.clear();
  for (const ExportDecl &decl : module->file.exports) {
    if (!decl.path.empty() || decl.all) {
      continue;
    }
    for (const ModuleName &name : decl.names) {
      exported_[name.as.empty() ? name.name : name.as] = {name.name, name.loc};
    }
  }
  llvm::StringSet<> declared;
  const DeclarationFile &file = *module->declaration;
  auto lookup = [&](llvm::StringRef name, llvm::SMLoc loc) -> const Exported * {
    declared.insert(name);
    auto found = exported_.find(name);
    if (found == exported_.end()) {
      checker_.diags_.error(loc, "'" + name + "' is declared here, but '" + fileName() +
                                     "' does not export it");
      return nullptr;
    }

    return &found->second;
  };
  for (Stmt *item : file.items) {
    if (auto *alias = dyn_cast<TypeAliasDecl>(item)) {
      if (const Exported *exported = lookup(alias->name, alias->loc())) {
        checkAlias(alias, *exported);
      }
    } else if (auto *fn = dyn_cast<FunctionDecl>(item)) {
      if (const Exported *exported = lookup(fn->name, fn->loc())) {
        checkFunction(fn, *exported);
      }
    }
  }
  for (const DeclaredClass &cls : file.classes) {
    if (const Exported *exported = lookup(cls.name, cls.loc)) {
      checkClass(cls, *exported);
    }
  }
  std::string declarations = llvm::sys::path::filename(file.path).str();
  for (const auto &entry : exported_) {
    if (!declared.contains(entry.getKey())) {
      checker_.diags_.error(entry.second.loc,
                            "'" + entry.getKey() + "' is exported, but '" + declarations +
                                "' does not declare it: a declaration file declares all that its "
                                "module exports");
    }
  }
  if (outer) {
    checker_.moduleDeclarer().useModule(outer);
  }
}

void DeclarationChecker::checkAlias(TypeAliasDecl *declared, const Exported &exported) {
  auto *impl = dyn_cast_or_null<TypeAliasDecl>(checker_.typeDeclarer().lookupType(exported.local));
  if (!impl) {
    checker_.diags_.error(declared->loc(),
                          "'" + declared->name + "' is not a type in '" + fileName() + "'");
    return;
  }
  if (declared->typeParams.size() != impl->typeParams.size()) {
    checker_.diags_.error(declared->loc(), "'" + declared->name + "' takes " +
                                               std::to_string(declared->typeParams.size()) +
                                               " type parameters here, and " +
                                               std::to_string(impl->typeParams.size()) + " in '" +
                                               fileName() + "'");
    return;
  }
  if (!impl->typeParams.empty()) {
    return;
  }
  const Type *there = checker_.typeDeclarer().resolveAlias(impl);
  const Type *here = checker_.typeDeclarer().resolveAlias(declared);
  if (!here || !there || here == there || here->isError() || there->isError()) {
    return;
  }
  std::string what = "'" + declared->name.str() + "'";
  if (here->isObject() && there->isObject() && !here->isClass() && !there->isClass()) {
    for (const Field &mine : here->fields()) {
      const Field *theirs = there->field(mine.name);
      if (!theirs) {
        checker_.diags_.error(declared->loc(), what + " has field '" + mine.name +
                                                   "' here, and not in '" + fileName() + "'");
        return;
      }
      if (mine.type != theirs->type) {
        mismatch(declared->loc(), "field '" + mine.name + "' of " + what, mine.type, theirs->type);
        return;
      }
      if (mine.readonly != theirs->readonly) {
        checker_.diags_.error(declared->loc(),
                              "field '" + mine.name + "' of " + what + " is " +
                                  (theirs->readonly ? "readonly" : "not readonly") + " in '" +
                                  fileName() + "'");
        return;
      }
    }
    for (const Field &theirs : there->fields()) {
      if (!here->field(theirs.name)) {
        checker_.diags_.error(declared->loc(), what + " has field '" + theirs.name + "' in '" +
                                                   fileName() + "', which is not declared here");
        return;
      }
    }
  }
  mismatch(declared->loc(), what, here, there);
}

bool DeclarationChecker::sameParams(const std::vector<Param> &declared, const FunctionDecl *fn,
                                    llvm::SMLoc loc, const std::string &what) {
  if (declared.size() != fn->params.size()) {
    checker_.diags_.error(loc, what + " takes " + std::to_string(declared.size()) +
                                   " parameters here, and " + std::to_string(fn->params.size()) +
                                   " in '" + fileName() + "'");
    return false;
  }
  for (size_t i = 0; i < declared.size(); ++i) {
    const Param &mine = declared[i], &theirs = fn->params[i];
    std::string param = "parameter '" + mine.name.str() + "' of " + what;
    if (mine.optional != (theirs.optional || theirs.defaultValue) || mine.rest != theirs.rest) {
      checker_.diags_.error(mine.loc, param + " is " + (mine.optional ? "optional" : "required") +
                                          " here, and not in '" + fileName() + "'");
      return false;
    }
    const Type *here = resolve(mine.type, false);
    const Type *there = theirs.declaredType;
    if (!here || !there || here->isError() || there->isError()) {
      continue;
    }
    if (here != there && Type::optional(here) != there) {
      mismatch(mine.loc, "the type of " + param, here, there);
      return false;
    }
  }

  return true;
}

void DeclarationChecker::checkFunction(FunctionDecl *declared, const Exported &exported) {
  Checker::ScopeEntry entry = checker_.topLevelChecker().lookupValue(exported.local);
  if (!entry.func || !entry.func->decl) {
    checker_.diags_.error(declared->loc(),
                          "'" + declared->name + "' is not a function in '" + fileName() + "'");
    return;
  }
  const FunctionDecl *fn = entry.func->decl;
  std::string what = "'" + declared->name.str() + "'";
  if (!declared->typeParams.empty() || !fn->typeParams.empty()) {
    if (declared->typeParams.size() != fn->typeParams.size()) {
      checker_.diags_.error(declared->loc(), what +
                                                 " takes another number of type parameters in '" +
                                                 fileName() + "'");
    }
    return;
  }
  checker_.returnInferrer().resolveParams(entry.func);
  if (!sameParams(declared->params, fn, declared->loc(), what)) {
    return;
  }
  if (!declared->returnType) {
    checker_.diags_.error(declared->loc(), "write what " + what +
                                               " gives: ': T' after its "
                                               "parameters");
    return;
  }
  const Type *here = resolve(declared->returnType, true);
  const Type *there = checker_.returnInferrer().returnType(entry.func);
  if (here && there && here != there && !here->isError() && !there->isError()) {
    mismatch(declared->loc(), "what " + what + " gives", here, there);
  }
}

bool DeclarationChecker::mentionsHidden(const FunctionDecl *fn) const {
  for (const Param &param : fn->params) {
    const Type *type = param.declaredType ? param.declaredType->nonOptional() : nullptr;
    const ClassDecl *cls = type ? ClassDeclarer::classOf(type) : nullptr;
    if (!cls) {
      continue;
    }
    bool shown = false;
    for (const auto &entry : exported_) {
      if (entry.second.local == cls->name) {
        shown = true;
      }
    }
    if (!shown && checker_.declModules_.lookup(cls) == module_) {
      return true;
    }
  }

  return false;
}

void DeclarationChecker::checkField(const DeclaredMember &member, const ClassField &field,
                                    const ClassDecl *cls) {
  std::string what = "'" + member.name.str() + "' of '" + cls->name.str() + "'";
  if (member.method || member.constructor) {
    checker_.diags_.error(member.loc, what + " is a field in '" + fileName() + "'");
    return;
  }
  if (member.readonly != field.readonly) {
    checker_.diags_.error(member.loc, what +
                                          (field.readonly ? " is readonly" : " is not readonly") +
                                          " in '" + fileName() + "'");
  }
  if (member.optional != field.optional) {
    checker_.diags_.error(member.loc, what +
                                          (field.optional ? " is optional" : " is not optional") +
                                          " in '" + fileName() + "'");
  }
  const Type *here = resolve(member.type, false);
  const Type *there = field.resolved;
  if (here && there && !here->isError() && !there->isError() && here != there &&
      Type::optional(here) != there) {
    mismatch(member.loc, "the type of " + what, here, there);
  }
}

void DeclarationChecker::checkMethod(const DeclaredMember &member, const FunctionDecl *fn,
                                     const ClassDecl *cls) {
  std::string what = "'" + member.name.str() + "' of '" + cls->name.str() + "'";
  if (!member.method && !member.constructor) {
    checker_.diags_.error(member.loc, what + " is a method in '" + fileName() + "'");
    return;
  }
  if (!member.typeParams.empty() || !fn->typeParams.empty()) {
    if (member.typeParams.size() != fn->typeParams.size()) {
      checker_.diags_.error(member.loc, what + " takes another number of type parameters in '" +
                                            fileName() + "'");
    }
    return;
  }
  checker_.returnInferrer().resolveParams(fn->sym);
  if (!sameParams(member.params, fn, member.loc, what) || member.constructor) {
    return;
  }
  if (!member.type) {
    checker_.diags_.error(member.loc, "write what " + what +
                                          " gives: ': T' after its "
                                          "parameters");
    return;
  }
  const Type *here = resolve(member.type, true);
  const Type *there = checker_.returnInferrer().returnType(fn->sym);
  if (here && there && here != there && !here->isError() && !there->isError()) {
    mismatch(member.loc, "what " + what + " gives", here, there);
  }
}

void DeclarationChecker::checkClass(const DeclaredClass &declared, const Exported &exported) {
  auto *cls = dyn_cast_or_null<ClassDecl>(checker_.typeDeclarer().lookupType(exported.local));
  if (!cls) {
    checker_.diags_.error(declared.loc,
                          "'" + declared.name + "' is not a class in '" + fileName() + "'");
    return;
  }
  if (declared.typeParams.size() != cls->typeParams.size()) {
    checker_.diags_.error(declared.loc, "'" + declared.name +
                                            "' takes another number of type parameters in '" +
                                            fileName() + "'");
    return;
  }
  if (!cls->typeParams.empty()) {
    return;
  }
  checker_.classDeclarer().completeClass(cls);
  llvm::StringSet<> seen;
  for (const DeclaredMember &member : declared.members) {
    std::string key = (member.isStatic ? "static " : "") + member.name.str();
    seen.insert(key);
    std::string what = "'" + member.name.str() + "' of '" + cls->name.str() + "'";
    if (!member.accessWritten) {
      checker_.diags_.error(member.loc,
                            "write the access of " + what + ": public, protected or private");
    }
    if (member.constructor) {
      const FunctionDecl *ctor = nullptr;
      for (const FunctionDecl *c : cls->constructors) {
        if (c->access != Access::Private) {
          ctor = c;
        }
      }
      if (!ctor) {
        checker_.diags_.error(member.loc, "'" + cls->name +
                                              "' has no constructor that programs "
                                              "can call in '" +
                                              fileName() + "'");
        continue;
      }
      if (member.access != ctor->access) {
        checker_.diags_.error(member.loc, "the constructor of '" + cls->name + "' is " +
                                              accessWord(ctor->access) + " in '" + fileName() +
                                              "'");
      }
      checkMethod(member, ctor, cls);
      continue;
    }
    const ClassField *field = nullptr;
    for (const ClassField &f : cls->fields) {
      if (f.name == member.name && f.isStatic == member.isStatic && !f.method) {
        field = &f;
      }
    }
    const FunctionDecl *method = nullptr;
    for (const FunctionDecl *fn : cls->methods) {
      if (fn->name == member.name && fn->isStatic == member.isStatic) {
        method = fn;
      }
    }
    Access access = field ? field->access : method ? method->access : Access::Public;
    if (!field && !method) {
      checker_.diags_.error(member.loc, "'" + cls->name + "' has no " +
                                            (member.isStatic ? "static member '" : "member '") +
                                            member.name + "' in '" + fileName() + "'");
      continue;
    }
    if (access == Access::Private) {
      checker_.diags_.error(member.loc, what + " is private in '" + fileName() +
                                            "': a declaration file declares only what programs "
                                            "can use");
      continue;
    }
    if (member.accessWritten && member.access != access) {
      checker_.diags_.error(member.loc,
                            what + " is " + accessWord(access) + " in '" + fileName() + "'");
    }
    if (field) {
      checkField(member, *field, cls);
    } else {
      checkMethod(member, method, cls);
    }
  }
  std::string declarations = llvm::sys::path::filename(module_->declaration->path).str();
  auto missing = [&](llvm::SMLoc loc, llvm::StringRef name, bool isStatic) {
    checker_.diags_.error(loc, "'" + name + "' of '" + cls->name + "' is " +
                                   (isStatic ? "a static member" : "a member") +
                                   " that programs can use, but '" + declarations +
                                   "' does not declare it");
  };
  for (const ClassField &f : cls->fields) {
    std::string key = (f.isStatic ? "static " : "") + f.name.str();
    if (!f.method && f.access != Access::Private && !seen.contains(key)) {
      missing(f.loc, f.name, f.isStatic);
    }
  }
  for (const FunctionDecl *fn : cls->methods) {
    std::string key = (fn->isStatic ? "static " : "") + fn->name.str();
    if (fn->access != Access::Private && !seen.contains(key)) {
      missing(fn->loc(), fn->name, fn->isStatic);
    }
  }
  for (const FunctionDecl *ctor : cls->constructors) {
    if (ctor->access != Access::Private && !seen.contains("constructor") && !mentionsHidden(ctor)) {
      missing(ctor->loc(), "constructor", false);
    }
  }
}

}
