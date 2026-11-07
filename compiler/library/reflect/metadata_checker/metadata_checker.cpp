#include "compiler/library/reflect/metadata_checker/metadata_checker.h"
#include "compiler/check/classes/class_value_checker/class_value_checker.h"
#include "compiler/check/functions/function_checker/function_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;

namespace tz {

MetadataChecker::MetadataChecker(Checker &owner)
    : checker(owner) {}

ClassDecl *MetadataChecker::builtinConstructor(llvm::StringRef name) {
  static const llvm::StringLiteral names[] = {"Number",       "String",
                                              "Boolean",      "Object",
                                              "Array",        "Function",
                                              "Date",         "RegExp",
                                              "Map",          "Set",
                                              "Promise",      "Uint8Array",
                                              "Int8Array",    "Uint8ClampedArray",
                                              "Int16Array",   "Uint16Array",
                                              "Int32Array",   "Uint32Array",
                                              "Float16Array", "Float32Array",
                                              "Float64Array", "Buffer",
                                              "ArrayBuffer",  "DataView",
                                              "TextEncoder",  "TextDecoder"};
  auto found = llvm::find(names, name);
  if (found == std::end(names)) {
    return nullptr;
  }
  ClassDecl *&cls = this->builtinConstructors[name];
  if (!cls) {
    cls = this->checker.ctx_.make<ClassDecl>(llvm::SMLoc(), *found);
    cls->origin = ClassOrigin::BUILTIN;
    cls->stage = ClassStage::RESOLVED;
    cls->declared = true;
  }

  return cls;
}

const void *MetadataChecker::metadataKey(const Expr *key, std::string &label) {
  auto *name = dyn_cast<NameExpr>(key);
  if (!name) {
    return nullptr;
  }
  ScopeEntry entry = this->checker.scopes().lookup(name->name);
  label = name->name.str();
  if (entry.var()) {
    return entry.var()->isConst && entry.var()->type && !entry.var()->type->isString() ? entry.var()
                                                                                       : nullptr;
  }
  if (entry.func()) {
    return entry.func();
  }

  return entry.cls();
}

void MetadataChecker::findMetadataWriters(SourceFile &program) {
  for (Module *module : program.modules) {
    for (Stmt *item : module->file.items) {
      std::vector<FunctionDecl *> fns;
      if (auto *fn = dyn_cast<FunctionDecl>(item)) {
        fns.push_back(fn);
      } else if (auto *cls = dyn_cast<ClassDecl>(item)) {
        fns = cls->methods;
      }
      for (FunctionDecl *fn : fns) {
        visitExprs(fn, [&](const Expr *e) {
          auto *call = dyn_cast<CallExpr>(e);
          auto *member = call ? dyn_cast<MemberExpr>(call->callee) : nullptr;
          auto *object = member ? dyn_cast<NameExpr>(member->object) : nullptr;
          if (!object || object->name != "Reflect" || member->name != "defineMetadata" ||
              call->args.empty()) {
            return;
          }
          if (auto *key = dyn_cast<NameExpr>(call->args[0])) {
            std::vector<FunctionDecl *> &writers = this->metaWriters[key->name];
            if (!llvm::is_contained(writers, fn)) {
              writers.push_back(fn);
            }
          }
        });
      }
    }
  }
}

void MetadataChecker::checkBodyEarly(FunctionDecl *fn) {
  if (!fn->sym || fn->checked || fn->sym->isGeneric() ||
      llvm::is_contained(this->checker.functions_, fn)) {
    return;
  }
  if (!fn->sym->ret) {
    this->checker.returnInferrer().inferReturnType(fn->sym);
    return;
  }
  if (fn->cls()) {
    return;
  }
  Checker::BodyState saved = this->checker.returnInferrer().enterBody(fn);
  if (!this->checker.topLevelDone_) {
    this->checker.early_ = fn->sym;
  }
  this->checker.flow_ = Checker::Flow();
  this->checker.functionChecker().checkFunctionBody(fn);
  this->checker.returnInferrer().leaveBody(saved);
}

const Type *MetadataChecker::metadataType(const void *key, llvm::StringRef label, llvm::SMLoc loc,
                                          const Type *expected) {
  if (auto found = this->checker.metaTypes_.find(key); found != this->checker.metaTypes_.end()) {
    return found->second.type;
  }
  if (auto writers = this->metaWriters.find(label); writers != this->metaWriters.end()) {
    for (FunctionDecl *fn : std::vector<FunctionDecl *>(writers->second)) {
      this->checkBodyEarly(fn);
      if (auto found = this->checker.metaTypes_.find(key);
          found != this->checker.metaTypes_.end()) {
        return found->second.type;
      }
    }
  }
  if (expected && !expected->nonOptional()->isUnknown() && !mentionsParam(expected)) {
    this->checker.metaTypes_[key] = {expected->nonOptional(), loc};
    return expected->nonOptional();
  }
  this->checker.diags_.error(
      loc, "the records of '" + label +
               "' have the type of the first one written, "
               "'Reflect.defineMetadata(" +
               label +
               ", value, target)', and none is written "
               "before this; write one first, give the type where this one goes (e.g. "
               "'const x: string[] = ...'), or use a string key, whose records are 'unknown'");

  return Type::error();
}

Expr *MetadataChecker::designClass(const Type *type, llvm::SMLoc loc) {
  return this->checker.classValueChecker().classValueAt(this->designConstructor(type), loc);
}

ClassDecl *MetadataChecker::designConstructor(const Type *type) {
  type = type->nonOptional();
  if (type->isLiteral()) {
    type = type->literalBase();
  }
  if (type->isUnion()) {
    ClassDecl *common = this->designConstructor(type->members().front());
    for (const Type *m : type->members()) {
      if (this->designConstructor(m) != common) {
        return this->builtinConstructor("Object");
      }
    }
    return common;
  }
  if (type->isBytes()) {
    return this->builtinConstructor(type->name());
  }
  llvm::StringRef name = "Object";
  if (type->isPromise()) {
    name = "Promise";
  } else if (type->isNumeric()) {
    name = "Number";
  } else if (type->isString()) {
    name = "String";
  } else if (type->isBool()) {
    name = "Boolean";
  } else if (type->isArray() || type->isTuple() || type->isMatch()) {
    name = "Array";
  } else if (type->isFunction() || type->isClassRef()) {
    name = "Function";
  } else if (type->isDate()) {
    name = "Date";
  } else if (type->isRegExp()) {
    name = "RegExp";
  } else if (type->isMap()) {
    name = "Map";
  } else if (type->isSet()) {
    name = "Set";
  } else if (ClassDecl *cls = ClassDeclarer::classOf(type); cls && !cls->literal()) {
    return cls;
  }

  return this->builtinConstructor(name);
}

void MetadataChecker::writeDesignRecord(ClassDecl *cls, const char16_t *key, Expr *value,
                                        llvm::StringRef member, llvm::SMLoc loc) {
  auto *callee = this->checker.ctx_.make<NameExpr>(loc, "Reflect");
  callee->type = Type::error();
  auto *keyLit = this->checker.ctx_.make<StringLitExpr>(loc, key);
  keyLit->type = Type::string();
  Expr *property;
  if (member.empty()) {
    property = this->checker.ctx_.make<UndefinedExpr>(loc);
    property->type = Type::undefinedTy();
  } else {
    property =
        this->checker.ctx_.make<StringLitExpr>(loc, std::u16string(member.begin(), member.end()));
    property->type = Type::string();
  }
  if (value->type->isClassRef()) {
    value = this->checker.typeRelations().convert(value, Type::constructorOf(Type::anyObject()));
  }
  std::vector<Expr *> args{
      keyLit, this->checker.typeRelations().convert(value, Type::unknown()),
      this->checker.typeRelations().convert(
          this->checker.classValueChecker().classValueAt(cls, loc), Type::anyObject()),
      this->checker.typeRelations().convert(property, Type::optional(Type::string()))};
  auto *call = this->checker.ctx_.make<CallExpr>(loc, callee, std::move(args));
  call->target = BuiltinCall{.builtin = Builtin::MetaDefine};
  call->designRecord = true;
  call->type = Type::voidTy();
  cls->decorations.push_back(this->checker.ctx_.make<ExprStmt>(loc, call));
}

void MetadataChecker::writeDesignRecords(ClassDecl *cls, ClassField *field, FunctionDecl *method,
                                         const FunctionDecl *ctor, llvm::SMLoc loc) {
  auto paramTypes = [&](const FunctionDecl *fn) {
    auto *list = this->checker.ctx_.make<ArrayLitExpr>(loc);
    this->checker.returnInferrer().resolveParams(fn->sym);
    const Type *any = Type::constructorOf(Type::anyObject());
    for (const Type *param : fn->sym->params) {
      if (param) {
        list->elements.push_back(
            {this->checker.typeRelations().convert(this->designClass(param, loc), any), false});
      }
    }
    list->type = Type::array(any);

    return list;
  };
  if (field) {
    this->checker.classDeclarer().completeClass(cls);
    if (field->resolved && !field->resolved->isError()) {
      this->writeDesignRecord(cls, u"design:type", this->designClass(field->resolved, loc),
                              field->name, loc);
    }
    return;
  }
  if (method) {
    const Type *ret = this->checker.returnInferrer().returnType(method->sym);
    if (ret && !ret->isVoid() && !ret->isError()) {
      this->writeDesignRecord(cls, u"design:returntype", this->designClass(ret, loc), method->name,
                              loc);
    }
    this->writeDesignRecord(cls, u"design:paramtypes", paramTypes(method), method->name, loc);
    this->writeDesignRecord(
        cls, u"design:type",
        this->checker.classValueChecker().classValueAt(this->builtinConstructor("Function"), loc),
        method->name, loc);
    return;
  }
  if (ctor) {
    this->writeDesignRecord(cls, u"design:paramtypes", paramTypes(ctor), {}, loc);
  }
}

}
