#include "compiler/semantic/library/reflect/metadata_checker/metadata_checker.h"
#include "compiler/semantic/bodies/classes/class_value_checker/class_value_checker.h"
#include "compiler/semantic/bodies/functions/function_checker/function_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;

namespace tz {

MetadataChecker::MetadataChecker(SemanticState &owner)
    : state(owner) {}

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
  ReflectStore &reflect = this->state.getReflectStore();
  if (ClassDecl *known = reflect.findBuiltinConstructor(name)) {
    return known;
  }
  auto *cls = this->state.getAst().make<ClassDecl>(llvm::SMLoc(), *found);
  cls->origin = ClassOrigin::BUILTIN;
  cls->stage = ClassStage::RESOLVED;
  cls->declared = true;
  reflect.rememberBuiltinConstructor(name, cls);

  return cls;
}

const void *MetadataChecker::metadataKey(const Expr *key, std::string &label) {
  auto *name = dyn_cast<NameExpr>(key);
  if (!name) {
    return nullptr;
  }
  ScopeEntry entry = Scopes(this->state).lookup(name->name);
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

void MetadataChecker::checkBodyEarly(FunctionDecl *fn) {
  if (!fn->sym || fn->checked || fn->sym->isGeneric() ||
      this->state.getFunctionStack().isCheckingFunction(fn)) {
    return;
  }
  if (!fn->sym->hasReturnType()) {
    ReturnInferrer(this->state).inferReturnType(fn->sym);
    return;
  }
  if (fn->cls()) {
    return;
  }
  BodyState saved = ReturnInferrer(this->state).enterBody(fn);
  if (!this->state.getModuleStates().isTopLevelDone()) {
    this->state.getFunctionStack().rememberEarlyFunction(fn->sym);
  }
  this->state.getFlowState().replaceFlow(Flow());
  FunctionChecker(this->state).checkFunctionBody(fn);
  ReturnInferrer(this->state).leaveBody(saved);
}

MetadataChecker::ResultMetadataTyped MetadataChecker::metadataType(const void *key,
                                                                   llvm::StringRef label,
                                                                   llvm::SMLoc loc,
                                                                   const Type *expected) {
  ReflectStore &reflect = this->state.getReflectStore();
  if (const MetadataType *known = reflect.findMetadataType(key)) {
    return ok(known->type);
  }
  for (FunctionDecl *fn : reflect.getMetadataWriters(label)) {
    this->checkBodyEarly(fn);
    if (const MetadataType *known = reflect.findMetadataType(key)) {
      return ok(known->type);
    }
  }
  bool written =
      expected && !expected->nonOptional()->isUnknown() && !TypeQueries::mentionsParam(expected);
  if (!written) {
    std::string message = ErrorTextMetadataType::unknown(label);

    return err(MetadataCheckerMetadataTypeError{
        .code = MetadataCheckerMetadataTypeErrorCode::NO_RECORD_TYPE,
        .message = message,
    });
  }
  reflect.rememberMetadataType(key, expected->nonOptional(), loc);

  return ok(expected->nonOptional());
}

Expr *MetadataChecker::designClass(const Type *type, llvm::SMLoc loc) {
  return ClassValueChecker(this->state).classValueAt(this->designConstructor(type), loc);
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
  auto *callee = this->state.getAst().make<NameExpr>(loc, "Reflect");
  callee->type = Type::error();
  auto *keyLit = this->state.getAst().make<StringLitExpr>(loc, key);
  keyLit->type = Type::string();
  Expr *property;
  if (member.empty()) {
    property = this->state.getAst().make<UndefinedExpr>(loc);
    property->type = Type::undefinedTy();
  } else {
    property =
        this->state.getAst().make<StringLitExpr>(loc, std::u16string(member.begin(), member.end()));
    property->type = Type::string();
  }
  if (value->type->isClassRef()) {
    value = TypeRelations(this->state).convert(value, Type::constructorOf(Type::anyObject()));
  }
  std::vector<Expr *> args{
      keyLit, TypeRelations(this->state).convert(value, Type::unknown()),
      TypeRelations(this->state)
          .convert(ClassValueChecker(this->state).classValueAt(cls, loc), Type::anyObject()),
      TypeRelations(this->state).convert(property, Type::optional(Type::string()))};
  auto *call = this->state.getAst().make<CallExpr>(loc, callee, std::move(args));
  call->target = BuiltinCall{.builtin = Builtin::MetaDefine};
  call->designRecord = true;
  call->type = Type::voidTy();
  cls->decorations.push_back(this->state.getAst().make<ExprStmt>(loc, call));
}

void MetadataChecker::writeDesignRecords(ClassDecl *cls, ClassField *field, FunctionDecl *method,
                                         const FunctionDecl *ctor, llvm::SMLoc loc) {
  auto paramTypes = [&](const FunctionDecl *fn) {
    auto *list = this->state.getAst().make<ArrayLitExpr>(loc);
    ReturnInferrer(this->state).resolveParams(fn->sym);
    const Type *any = Type::constructorOf(Type::anyObject());
    for (const Type *param : fn->sym->params) {
      if (param) {
        list->elements.push_back(
            {TypeRelations(this->state).convert(this->designClass(param, loc), any), false});
      }
    }
    list->type = Type::array(any);

    return list;
  };
  if (field) {
    ClassDeclarer(this->state).completeClass(cls);
    if (field->resolved && !field->resolved->isError()) {
      this->writeDesignRecord(cls, u"design:type", this->designClass(field->resolved, loc),
                              field->name, loc);
    }
    return;
  }
  if (method) {
    const Type *ret = method->sym->getReturnType();
    if (ret && !ret->isVoid() && !ret->isError()) {
      this->writeDesignRecord(cls, u"design:returntype", this->designClass(ret, loc), method->name,
                              loc);
    }
    this->writeDesignRecord(cls, u"design:paramtypes", paramTypes(method), method->name, loc);
    this->writeDesignRecord(
        cls, u"design:type",
        ClassValueChecker(this->state).classValueAt(this->builtinConstructor("Function"), loc),
        method->name, loc);
    return;
  }
  if (ctor) {
    this->writeDesignRecord(cls, u"design:paramtypes", paramTypes(ctor), {}, loc);
  }
}

}
