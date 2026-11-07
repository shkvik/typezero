#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/check/async/async_checker/async_checker.h"
#include "compiler/check/classes/class_value_checker/class_value_checker.h"
#include "compiler/check/classes/decorator_checker/decorator_checker.h"
#include "compiler/check/flow/predicate_checker/predicate_checker.h"
#include "compiler/check/generics/alias_instantiator/alias_instantiator.h"
#include "compiler/check/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/check/iterators/generator_checker/generator_checker.h"
#include "compiler/check/objects/dict_checker/dict_checker.h"
#include "compiler/check/types/conditional_types/conditional_types.h"
#include "compiler/check/types/template_types/template_types.h"
#include "compiler/check/types/type_operators/type_operators.h"
#include "compiler/check/types/utility_types/utility_types.h"
#include "compiler/declare/type_declarer/type_declarer.h"
#include "compiler/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/library/crypto/crypto_checker/crypto_checker.h"
#include "compiler/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/library/intl/intl_checker/intl_checker.h"
#include "compiler/library/result/result_types/result_types.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;

namespace tz {

TypeResolver::TypeResolver(Checker &owner)
    : checker(owner) {}

const Type *TypeResolver::resolveType(const TypeRef &ref, bool allowVoid) {
  const Type *type = this->resolveTypeInner(ref);
  if (type->isVoid() && !allowVoid) {
    auto *bound =
        ref.kind() == TypeRefKind::NAME_REF
            ? dyn_cast_or_null<TypeAliasDecl>(this->checker.typeDeclarer().lookupType(ref.name))
            : nullptr;
    if (bound && bound->boundParam) {
      return Type::undefinedTy();
    }
    this->checker.diags_.error(ref.loc,
                               "'void' can only be used as a return type or a type argument");
    return Type::error();
  }
  if (type->isUndefined()) {
    this->checker.diags_.error(ref.loc,
                               "the type 'undefined' alone is not supported; use 'T | undefined'");
    return Type::error();
  }

  return type;
}

const Type *TypeResolver::resolveTypeInner(const TypeRef &ref) {
  switch (ref.kind()) {
    case TypeRefKind::NAME_REF: {
      if (ref.name.empty()) {
        return Type::error();
      }
      if (const Type *type = Type::fromName(ref.name)) {
        return type;
      }
      if (ref.name == "never" && !this->checker.typeDeclarer().lookupType(ref.name)) {
        return Type::never();
      }
      if (Stmt *decl = this->checker.typeDeclarer().lookupType(ref.name)) {
        if (auto *alias = dyn_cast<TypeAliasDecl>(decl)) {
          return alias->typeParams.empty()
                     ? this->checker.typeDeclarer().resolveAlias(alias)
                     : this->checker.aliasInstantiator().instantiateAlias(alias, {}, ref.loc);
        }
        if (auto *cls = dyn_cast<ClassDecl>(decl)) {
          if (cls->isGeneric()) {
            GenericClassInstantiator::ResultClassMade made =
                this->checker.genericClassInstantiator().instantiateClass(cls, {}, ref.loc);
            if (made.isErr() &&
                made.error().code !=
                    GenericClassInstantiatorInstantiateClassErrorCode::ALREADY_REPORTED) {
              llvm::SMLoc at =
                  made.error().code ==
                          GenericClassInstantiatorInstantiateClassErrorCode::ENDLESS_INSTANCES
                      ? cls->loc()
                      : ref.loc;
              this->checker.diags_.error(at, made.error().message);
            }
            return made.isOk() ? made.value()->type : Type::error();
          }
          return ClassValueChecker::finalClass(cls)->type;
        }
        return this->checker.typeDeclarer().enumType(cast<EnumDecl>(decl));
      }
      if (auto [owner, member] = ref.name.split('.'); !member.empty()) {
        if (auto *e = dyn_cast_or_null<EnumDecl>(this->checker.typeDeclarer().lookupType(owner))) {
          this->checker.typeDeclarer().resolveEnum(e);
          const EnumMember *m = e->find(member);
          if (!m) {
            this->checker.diags_.error(ref.loc,
                                       "enum '" + owner + "' has no member '" + member + "'");
            return Type::error();
          }
          return m->isString ? Type::stringLiteral(m->text)
                             : Type::numberLiteral(double(m->number), Type::intTy());
        }
      }
      if (ref.name == "Intl.Locale") {
        if (auto *cls = dyn_cast_or_null<ClassDecl>(
                this->checker.typeDeclarer().lookupType(kIntlLocaleName))) {
          return ClassValueChecker::finalClass(cls)->type;
        }
      }
      if (ref.name.starts_with("Intl.")) {
        if (const Type *intl = IntlChecker::intlTypeNamed(ref.name.drop_front(5))) {
          return intl;
        }
        this->checker.diags_.error(ref.loc, "cannot find type '" + ref.name + "'");
        return Type::error();
      }
      if (ref.name == "TemplateStringsArray") {
        return Type::array(Type::string());
      }
      if (ref.name == "RegExp") {
        return Type::regExp();
      }
      if (ref.name == "Date") {
        return Type::date();
      }
      if (ref.name == "Timeout" || ref.name == "NodeJS.Timeout") {
        return Type::timeout();
      }
      if (ref.name == "Immediate" || ref.name == "NodeJS.Immediate") {
        return Type::immediate();
      }
      if (const Type *bytes = BytesChecker::bytesTypeNamed(ref.name)) {
        return bytes;
      }
      if (const Checker::NodeName *n = this->checker.builtinModuleChecker().nodeName(ref.name);
          n && (n->module == NodeModule::Readline || n->module == NodeModule::ReadlinePromises) &&
          n->name == "Interface") {
        return Type::runtimeObject(n->module == NodeModule::Readline
                                       ? Type::Runtime::Readline
                                       : Type::Runtime::ReadlinePromises);
      }
      if (const Type *crypto = this->checker.cryptoChecker().cryptoTypeNamed(ref.name)) {
        return crypto;
      }
      if (ref.name == "RegExpMatchArray" || ref.name == "RegExpExecArray") {
        return Type::match(-1, {}, nullptr, false);
      }
      if (const Type *dict = this->checker.dictChecker().resolveDictType(ref)) {
        return dict;
      }
      if (ref.name == "Constructor") {
        return Type::constructorOf(Type::anyObject());
      }
      if (ref.name == "ConstructorArgs") {
        return this->checker.classValueChecker().constructorArgs(ref.loc);
      }
      if (ref.name == "Array") {
        this->checker.diags_.error(ref.loc,
                                   "'Array' needs an element type, e.g. 'Array<int>' or 'int[]'");
      } else if (llvm::StringRef result = this->checker.resultTypes().resultTypeName(ref.name);
                 !result.empty()) {
        this->checker.diags_.error(
            ref.loc, "'" + ref.name + "' needs the types of its value and its error, e.g. '" +
                         ref.name + "<int, string>'");
      } else if (ref.name == "Generator" || ref.name == "IterableIterator") {
        this->checker.diags_.error(ref.loc, "'" + ref.name +
                                                "' needs the type of the values it gives, e.g. '" +
                                                ref.name + "<int>'");
      } else if (ref.name == "Promise") {
        this->checker.diags_.error(ref.loc,
                                   "'Promise' needs the type of its value, e.g. 'Promise<int>' or "
                                   "'Promise<void>'");
      } else if (ref.name == "null") {
        this->checker.diags_.error(
            ref.loc, "'null' is not supported: the only value for nothing is 'undefined'; "
                     "write 'T | undefined'");
      } else if (ref.name == "bigint") {
        this->checker.decimalChecker().reportBigInt(ref.loc, "the type 'bigint'");
      } else if (ref.name == "BigInt64Array" || ref.name == "BigUint64Array") {
        this->checker.decimalChecker().reportBigInt(ref.loc, "'" + ref.name.str() + "'");
      } else if (ref.name == "long") {
        this->checker.diags_.error(ref.loc, "there is no 'long': 'int' is 64 bits");
      } else if (ref.name == "SharedArrayBuffer") {
        this->checker.diags_.error(
            ref.loc, "'SharedArrayBuffer' is not supported yet: it comes with the shared "
                     "memory of async (docs/bytes.md, section 7); use 'ArrayBuffer'");
      } else {
        this->checker.diags_.error(ref.loc, "cannot find type '" + ref.name + "'");
      }
      return Type::error();
    }
    case TypeRefKind::GENERIC_REF: {
      if (const Type *bytes = BytesChecker::bytesTypeNamed(ref.name);
          bytes && bytes->isTypedArray() && !this->checker.typeDeclarer().lookupType(ref.name)) {
        if (ref.args.size() != 1 || ref.args[0].kind() != TypeRefKind::NAME_REF ||
            (ref.args[0].name != "ArrayBuffer" && ref.args[0].name != "ArrayBufferLike")) {
          this->checker.diags_.error(ref.loc, "the type argument of '" + ref.name +
                                                  "' can only be 'ArrayBuffer'; or leave it out");
          return Type::error();
        }
        return bytes;
      }
      if (llvm::StringRef result = this->checker.resultTypes().resultTypeName(ref.name);
          !result.empty()) {
        return this->checker.resultTypes().resolveResultType(ref, result);
      }
      if (ref.name == "Array" || ref.name == "ReadonlyArray") {
        if (ref.args.size() != 1) {
          this->checker.diags_.error(ref.loc, "'" + ref.name + "' takes one type argument");
          return Type::error();
        }
        TypeRef array;
        array.form = ArrayTypeRef{};
        array.loc = ref.loc;
        array.args = ref.args;
        return this->resolveTypeInner(array);
      }
      if (auto *cls = llvm::dyn_cast_or_null<ClassDecl>(
              this->checker.typeDeclarer().lookupType(ref.name))) {
        std::vector<const Type *> args;
        for (const TypeRef &arg : ref.args) {
          args.push_back(this->resolveType(arg, true));
          if (args.back()->isError()) {
            return args.back();
          }
        }
        if (!cls->isGeneric()) {
          this->checker.diags_.error(ref.loc, "class '" + ref.name +
                                                  "' is not generic; remove the type arguments");
          return Type::error();
        }
        GenericClassInstantiator::ResultClassMade made =
            this->checker.genericClassInstantiator().instantiateClass(cls, std::move(args),
                                                                      ref.loc);
        if (made.isErr() &&
            made.error().code !=
                GenericClassInstantiatorInstantiateClassErrorCode::ALREADY_REPORTED) {
          llvm::SMLoc at =
              made.error().code ==
                      GenericClassInstantiatorInstantiateClassErrorCode::ENDLESS_INSTANCES
                  ? cls->loc()
                  : ref.loc;
          this->checker.diags_.error(at, made.error().message);
        }
        return made.isOk() ? made.value()->type : Type::error();
      }
      if (auto *alias = llvm::dyn_cast_or_null<TypeAliasDecl>(
              this->checker.typeDeclarer().lookupType(ref.name))) {
        std::vector<const Type *> args;
        for (const TypeRef &arg : ref.args) {
          args.push_back(this->checker.conditionalTypes().resolveTypeArgument(arg));
          if (args.back()->isError()) {
            return args.back();
          }
        }
        if (alias->typeParams.empty()) {
          this->checker.diags_.error(ref.loc, "type '" + ref.name +
                                                  "' is not generic; remove the type arguments");
          return Type::error();
        }
        return this->checker.aliasInstantiator().instantiateAlias(alias, std::move(args), ref.loc);
      }
      if (const Type *iterator = this->checker.generatorChecker().resolveIteratorType(ref)) {
        return iterator;
      }
      if (const Type *dict = this->checker.dictChecker().resolveDictType(ref)) {
        return dict;
      }
      if (const Type *promise = this->checker.asyncChecker().resolvePromiseType(ref)) {
        return promise;
      }
      if (ref.name == "MethodDescriptor") {
        if (ref.args.size() != 2) {
          this->checker.diags_.error(
              ref.loc, "'MethodDescriptor' takes two type arguments: the types of the "
                       "method's parameters and what it returns, e.g. "
                       "'MethodDescriptor<[int, int], int>'");
          return Type::error();
        }
        const Type *args = this->resolveType(ref.args[0], false);
        const Type *ret = this->resolveType(ref.args[1], true);
        if (args->isError() || ret->isError()) {
          return Type::error();
        }
        if (!args->isTuple() && !args->isListParam()) {
          this->checker.diags_.error(ref.args[0].loc,
                                     "the first type argument of 'MethodDescriptor' is the list of "
                                     "the method's parameter types, a tuple such as '[int, "
                                     "string]', or 'A' of '<A extends unknown[]>'");
          return Type::error();
        }
        return this->checker.decoratorChecker().methodDescriptorType(args, ret);
      }
      UtilityTypes::ResultUtilityApplied utility =
          this->checker.utilityTypes().resolveUtilityType(ref);
      bool isUtility = utility.isOk() || utility.error().code !=
                                             UtilityTypesResolveUtilityTypeErrorCode::NOT_A_UTILITY;
      if (utility.isErr() && isUtility) {
        this->checker.diags_.error(ref.loc, utility.error().message);
        return Type::error();
      }
      if (isUtility) {
        return utility.value();
      }
      if (ref.name == "Constructor") {
        return this->checker.classValueChecker().resolveConstructorType(ref);
      }
      if (ref.name == "InstanceType" && ref.args.size() == 1) {
        const Type *cls = this->resolveType(ref.args[0], false);
        if (cls->isError()) {
          return cls;
        }
        const Type *made = cls->isClassRef() ? ClassValueChecker::constructedBy(cls) : nullptr;
        if (!made) {
          this->checker.diags_.error(ref.args[0].loc,
                                     "'InstanceType' takes a class as a value, e.g. "
                                     "'InstanceType<typeof Point>', found '" +
                                         cls->name() + "'");
          return Type::error();
        }
        return made;
      }
      this->checker.diags_.error(ref.loc, "generic type '" + ref.name + "' is not supported");
      return Type::error();
    }
    case TypeRefKind::ARRAY_REF: {
      const Type *element = this->resolveTypeInner(ref.args[0]);
      if (element->isVoid() || element->isUndefined()) {
        this->checker.diags_.error(ref.loc,
                                   "arrays of '" + element->name() + "' are not supported");
        return Type::error();
      }
      return Type::array(element);
    }
    case TypeRefKind::UNION_REF: {
      bool withNull = false;
      std::vector<const Type *> members;
      for (const TypeRef &member : ref.args) {
        if (member.kind() == TypeRefKind::NAME_REF && member.name == "null") {
          withNull = true;
          continue;
        }
        const Type *type = this->resolveTypeInner(member);
        if (type->isError()) {
          return type;
        }
        if (type->isVoid()) {
          this->checker.diags_.error(ref.loc, "'void' cannot be part of a union");
          return Type::error();
        }
        members.push_back(type);
      }
      const Type *type = Type::unionOf(members);
      if (withNull && !type->nonOptional()->isJson()) {
        this->checker.diags_.error(
            ref.loc, "'null' is not supported: the only value for nothing is 'undefined'; "
                     "write 'T | undefined' (null exists only in Json values)");
        return Type::error();
      }
      if (std::string problem = unionProblem(type); !problem.empty()) {
        this->checker.diags_.error(ref.loc,
                                   "union '" + type->name() + "' is not supported: " + problem);
        return Type::error();
      }
      return type;
    }
    case TypeRefKind::LITERAL_REF: {
      if (ref.name == "string") {
        return Type::stringLiteral(ref.text());
      }
      if (ref.name == "boolean") {
        return Type::boolLiteral(ref.number() != 0);
      }
      bool whole = ref.number() == std::trunc(ref.number()) &&
                   std::fabs(ref.number()) <= 9007199254740992.0 &&
                   !(ref.number() == 0 && std::signbit(ref.number()));
      return Type::numberLiteral(ref.number(), whole ? Type::intTy() : Type::doubleTy());
    }
    case TypeRefKind::PREDICATE_REF:
      this->checker.diags_.error(ref.loc, "a type predicate, '" + ref.name +
                                              " is T', can only be the return type of a function");
      return Type::error();
    case TypeRefKind::KEY_OF_REF:
    case TypeRefKind::INDEXED_REF:
    case TypeRefKind::TYPE_OF_REF:
    case TypeRefKind::MAPPED_REF: {
      TypeOperators::ResultTypeOpResolved resolved =
          this->checker.typeOperators().resolveTypeOp(ref);
      if (resolved.isOk()) {
        return resolved.value();
      }
      llvm::SMLoc at = ref.loc;
      switch (resolved.error().code) {
        case TypeOperatorsResolveTypeOpErrorCode::RENAME_NOT_STRING:
          at = ref.args[2].loc;
          break;
        case TypeOperatorsResolveTypeOpErrorCode::KEYS_NOT_STRINGS:
          at = ref.args[0].loc;
          break;
        case TypeOperatorsResolveTypeOpErrorCode::NOT_AN_OBJECT_TYPE:
        case TypeOperatorsResolveTypeOpErrorCode::NO_SUCH_MEMBER:
        case TypeOperatorsResolveTypeOpErrorCode::TYPE_OF_UNKNOWN:
        case TypeOperatorsResolveTypeOpErrorCode::NO_SUCH_FIELD:
          break;
      }
      this->checker.diags_.error(at, resolved.error().message);
      return Type::error();
    }
    case TypeRefKind::CONDITIONAL_REF:
      return this->checker.conditionalTypes().resolveConditional(ref);
    case TypeRefKind::TEMPLATE_REF:
      return this->checker.templateTypes().resolveTemplate(ref);
    case TypeRefKind::INTERSECTION_REF: {
      ConditionalTypes::ResultIntersectionJoined joined =
          this->checker.conditionalTypes().resolveIntersection(ref);
      if (joined.isErr()) {
        this->checker.diags_.error(ref.loc, joined.error().message);
        return Type::error();
      }
      return joined.value();
    }
    case TypeRefKind::INFER_REF: {
      auto *binding =
          llvm::dyn_cast_or_null<TypeAliasDecl>(this->checker.typeDeclarer().lookupType(ref.name));
      if (!this->checker.extendsClause_ || !binding || !binding->resolved) {
        this->checker.diags_.error(
            ref.loc, "'infer' declares a type only in the 'extends' clause of a conditional "
                     "type, e.g. 'T extends Array<infer U> ? U : never'");
        return Type::error();
      }
      return binding->resolved;
    }
    case TypeRefKind::FUNCTION_REF: {
      std::vector<const Type *> params;
      size_t minArgs = 0;
      bool optional = false;
      bool ok = true;
      size_t count = ref.args.size() - 1;
      bool hasThis = count > 0 && !ref.names().empty() && ref.names()[0] == "this" &&
                     !ref.optional()[0] && !(ref.rest() && count == 1);
      for (size_t i = 0; i < count; ++i) {
        bool rest = ref.rest() && i + 1 == count;
        bool outerList = std::exchange(this->checker.listContext_, rest);
        const Type *type = this->resolveType(ref.args[i], false);
        this->checker.listContext_ = outerList;
        ok &= !type->isError();
        if (rest) {
          if (!type->isArray() && !type->isListParam() && !type->isTuple() && !type->isError()) {
            this->checker.diags_.error(ref.args[i].loc,
                                       "a rest parameter must be an array, e.g. '...xs: int[]'");
            ok = false;
          }
        } else if (ref.optional()[i]) {
          type = Type::optional(type);
          optional = true;
        } else {
          if (optional) {
            this->checker.diags_.error(ref.args[i].loc,
                                       "a required parameter cannot follow an optional one");
            ok = false;
          }
          minArgs = i + 1;
        }
        params.push_back(type);
      }
      const TypeRef &returned = ref.args.back();
      const Type *ret = returned.kind() != TypeRefKind::PREDICATE_REF
                            ? this->resolveType(returned, true)
                            : Type::boolTy();
      if (returned.kind() == TypeRefKind::PREDICATE_REF && returned.asserts()) {
        const Type *type =
            returned.args.empty() ? nullptr : this->resolveType(returned.args[0], false);
        std::string shown = returned.name.str() + (type ? " is " + type->name().str() : "");
        std::string message = ErrorTextAsserts::notAllowed(shown, type != nullptr, "");
        this->checker.diags_.error(returned.loc, message);
        ret = Type::error();
      }
      if (!ok || ret->isError()) {
        return Type::error();
      }
      return Type::function(params, minArgs, ref.rest(), ret, hasThis);
    }
    case TypeRefKind::OBJECT_REF:
      return Type::object(this->resolveFields(ref));
    case TypeRefKind::TUPLE_REF: {
      std::vector<const Type *> elements;
      for (const TypeRef &element : ref.args) {
        elements.push_back(this->resolveType(element, false));
      }
      if (llvm::is_contained(ref.spread(), true)) {
        bool waiting = false;
        for (size_t i = 0; i < elements.size(); ++i) {
          waiting |= ref.spread()[i] && elements[i]->typeOp() != TypeOp::None &&
                     !elements[i]->isListParam();
        }
        if (waiting && llvm::none_of(elements, [](const Type *t) { return t->isError(); })) {
          std::vector<const Type *> operands;
          for (size_t i = 0; i < elements.size(); ++i) {
            operands.push_back(Type::boolLiteral(ref.spread()[i]));
            operands.push_back(elements[i]);
          }
          return this->checker.typeOperators().evalTypeOp(Type::typeOp(TypeOp::Spread, operands),
                                                          operands);
        }
        std::string why;
        const Type *list = spreadTuple(elements, ref.spread(), why);
        if (!list) {
          this->checker.diags_.error(ref.loc, why);
          return Type::error();
        }
        if (list->isTuple() && list->tupleRest() && !this->checker.listContext_ &&
            !this->checker.extendsClause_) {
          this->checker.diags_.error(
              ref.loc, "a rest element in a tuple type is not supported: a tuple has a fixed "
                       "length; use an array, e.g. 'int[]'");
          return Type::error();
        }
        return list;
      }
      for (size_t i = 0; i < elements.size(); ++i) {
        if (elements[i]->isListParam()) {
          this->checker.diags_.error(ref.args[i].loc, "'" + elements[i]->name() +
                                                          "' is a list of types; spread it "
                                                          "into the tuple, '..." +
                                                          elements[i]->name() + "'");

          return Type::error();
        }
      }
      return Type::tuple(elements, !ref.readonly().empty() && ref.readonly()[0]);
    }
  }
  llvm_unreachable("unknown type reference");
}

std::vector<Field> TypeResolver::resolveFields(const TypeRef &ref,
                                               std::vector<Overload> *overloads) {
  std::vector<Field> fields;
  auto isMethod = [&](size_t i) { return i < ref.methods().size() && ref.methods()[i]; };
  for (size_t i = 0; i < ref.args.size(); ++i) {
    llvm::StringRef name = ref.names()[i];
    bool several = false;
    bool others = false;
    for (size_t j = 0; j < ref.args.size(); ++j) {
      if (j != i && ref.names()[j] == name) {
        several = true;
        others |= !isMethod(j);
      }
    }
    if (several && isMethod(i) && !others) {
      if (!overloads) {
        this->checker.diags_.error(ref.locs()[i],
                                   "several signatures of method '" + name +
                                       "' are supported only in an interface; declare one, "
                                       "'interface Name { ... }'");
        continue;
      }
      const Type *type = this->resolveType(ref.args[i], false);
      auto it = llvm::find_if(*overloads, [&](const Overload &o) { return o.name == name; });
      if (it == overloads->end()) {
        it = overloads->insert(overloads->end(), Overload{name.str(), {}});
      }
      if (llvm::is_contained(it->signatures, type)) {
        this->checker.diags_.error(ref.locs()[i],
                                   "duplicate signature '" + type->name() + "' of method '" + name +
                                       "': the signatures must differ in their parameters");
      } else if (!type->isError()) {
        it->signatures.push_back(type);
      }
      continue;
    }
    if (llvm::any_of(fields, [&](const Field &f) { return f.name == name; })) {
      this->checker.diags_.error(ref.locs()[i], "duplicate field '" + name + "'");
      continue;
    }
    const Type *type = this->resolveType(ref.args[i], false);
    if (ref.optional()[i]) {
      type = Type::optional(type);
    }
    fields.push_back({name.str(), type, ref.readonly()[i]});
  }

  return fields;
}

}
