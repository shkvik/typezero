// Computations over types (docs/generics.md, section 10): `keyof T`, `T[K]`,
// mapped types `{ [K in keyof T]?: T[K] }`, `typeof` in types, and the
// utilities of TypeScript built on them: Partial, Required, Readonly, Pick
// and Omit are written as mapped types, Awaited as a conditional type
// (kPrelude below); Exclude, Extract, NonNullable, ReturnType and Parameters
// are computed here. Conditional types and template literal types are in
// sema_conditional.cpp.
//
// They exist only when compiling. Of known types each gives a type at once:
// `keyof User` is `"id" | "name"`, and `Pick<User, "id">` is the object type
// `{ id: int }`, a type of its own, as exact as any (a User is not one). In
// generic code a computation over type parameters waits, a type parameter
// of its own (Type::typeOp), until a copy gives them types.

#include "lex/lexer.h"
#include "parse/parser.h"
#include "sema/sema.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/MemoryBuffer.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

// The utilities that are mapped types, as TypeScript declares them (Omit
// takes keys of T only: a key that is not one is a mistake).
constexpr llvm::StringLiteral kPrelude = R"(type Partial<T> = { [K in keyof T]?: T[K] }
type Required<T> = { [K in keyof T]-?: T[K] }
type Readonly<T> = { readonly [K in keyof T]: T[K] }
type Pick<T, K extends keyof T> = { [P in K]: T[P] }
type Omit<T, K extends keyof T> = { [P in Exclude<keyof T, K>]: T[P] }
type Awaited<T> = T extends Promise<infer U> ? Awaited<U> : T
)";

std::u16string utf16(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);
  return std::u16string(units.begin(), units.end());
}

std::string utf8(const std::u16string &text) {
  std::string out;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()), out);
  return out;
}

llvm::ArrayRef<const Type *> membersOf(const Type *&type) {
  return type->isUnion() ? type->members() : llvm::ArrayRef<const Type *>(type);
}

// The members Exclude and Extract go over, as a conditional type does:
// those of a union, undefined of an optional, false and true of boolean.
std::vector<const Type *> distributedMembers(const Type *type) {
  std::vector<const Type *> out;
  if (type->isOptional()) {
    out = distributedMembers(type->inner());
    out.push_back(Type::undefinedTy());
  } else if (type->isUnion()) {
    for (const Type *m : type->members())
      for (const Type *n : distributedMembers(m))
        out.push_back(n);
  } else if (type->isBool()) {
    out = {Type::boolLiteral(false), Type::boolLiteral(true)};
  } else if (!type->isNever()) {
    out.push_back(type);
  }
  return out;
}

} // namespace

void Sema::declarePrelude() {
  std::unique_ptr<llvm::MemoryBuffer> buffer = llvm::MemoryBuffer::getMemBuffer(kPrelude, "<tz>");
  llvm::StringRef text = buffer->getBuffer();
  diags_.sourceMgr().AddNewSourceBuffer(std::move(buffer), llvm::SMLoc());
  Lexer lexer(text, diags_);
  Parser parser(lexer, ctx_, diags_);
  SourceFile prelude = parser.parseSourceFile();
  for (Stmt *item : prelude.items)
    if (auto *alias = dyn_cast<TypeAliasDecl>(item))
      builtinAliases_[alias->name] = alias;
}

// What `o.name` of a getter of the class gives (`status` of a Response):
// null for a name without one, or a getter only code inside may use.
const Type *Sema::getterType(ClassDecl *cls, llvm::StringRef name) {
  if (!cls || name.starts_with("#"))
    return nullptr;
  std::vector<FunctionDecl *> methods = methodsNamed(cls, name, /*isStatic=*/false);
  for (FunctionDecl *fn : methods)
    if (fn->accessor == FunctionDecl::Accessor::Getter && fn->sym && fn->access == Access::Public)
      return functionType(fn->sym)->ret();
  return nullptr;
}

const Type *Sema::keysOf(const Type *type) {
  type = type->nonOptional();
  // Of a union, the keys every member has.
  if (type->isUnion()) {
    std::vector<const Type *> common;
    bool first = true;
    for (const Type *m : type->members()) {
      const Type *keys = keysOf(m);
      if (!keys)
        return nullptr;
      std::vector<const Type *> mine;
      for (const Type *k : membersOf(keys))
        if (first || llvm::is_contained(common, k))
          mine.push_back(k);
      common = std::move(mine);
      first = false;
    }
    return common.empty() ? Type::never() : Type::unionOf(common);
  }
  if (type->isRecord())
    return type->keyType();
  if (!type->isObject() || type->isTuple() || type->isAnyObject())
    return nullptr;
  // The names of the fields, in their order; of a class, those code outside
  // it can use.
  ClassDecl *cls = classOf(type);
  if (cls)
    completeClass(cls);
  std::vector<const Type *> keys;
  for (const Field &f : type->fields()) {
    if (llvm::StringRef(f.name).starts_with("#") || llvm::StringRef(f.name).starts_with("%"))
      continue;
    if (const ClassField *field = cls ? findField(cls, f.name) : nullptr; field && field->access != Access::Public)
      continue;
    keys.push_back(Type::stringLiteral(utf16(f.name)));
  }
  // The getters of a class (of a built-in module) are properties too.
  for (const ClassDecl *k = cls; k; k = k->parent)
    for (const FunctionDecl *fn : k->methods)
      if (fn->accessor == FunctionDecl::Accessor::Getter && !fn->isStatic &&
          fn->access == Access::Public && !llvm::StringRef(fn->name).starts_with("#") &&
          !type->field(fn->name)) {
        const Type *key = Type::stringLiteral(utf16(fn->name));
        if (!llvm::is_contained(keys, key))
          keys.push_back(key);
      }
  return keys.empty() ? Type::never() : Type::unionOf(keys);
}

const Type *Sema::evalTypeOp(const Type *op, llvm::ArrayRef<const Type *> operands) {
  TypeOp kind = op->typeOp();
  // A conditional type is resolved again with what its names stand for.
  if (kind == TypeOp::Conditional)
    return evalConditional(operands);
  // A mapped type is open while its keys, or its values or new names for
  // some key, are.
  if (kind == TypeOp::Mapped) {
    const Type *key = operands[0], *keys = operands[1], *value = operands[2], *source = operands[3];
    const Type *rename = operands.size() > 4 ? operands[4] : nullptr;
    bool open = mentionsParam(keys) || (source && mentionsParam(source)) ||
                mentionsParamBesides(value, {key}) ||
                (rename && mentionsParamBesides(rename, {key}));
    if (open)
      return Type::mapped(key, keys, value, op->mappedOptional(), op->mappedReadonly(), source,
                          rename);
    if (keys->isNever())
      return Type::object({});
    std::vector<Field> fields;
    for (const Type *name : membersOf(keys)) {
      if (!name->isStringLiteral())
        return Type::error();
      std::string text = utf8(name->literalText());
      const Type *type = substitute(value, {key}, {name});
      if (type->isError())
        return type;
      const Field *from = source ? source->field(text) : nullptr;
      // `?` and `readonly` as written, or else as the field of the source.
      if (op->mappedOptional() > 0)
        type = Type::optional(type);
      else if (op->mappedOptional() < 0)
        type = type->nonOptional();
      bool readonly = op->mappedReadonly() > 0 || (op->mappedReadonly() == 0 && from && from->readonly);
      // `as`: the names the key gives, none for never; keys that give the
      // same name give a field of the union of their types, as in
      // TypeScript.
      std::vector<std::string> names{text};
      if (rename) {
        const Type *renamed = substitute(rename, {key}, {name});
        if (renamed->isError())
          return renamed;
        names.clear();
        for (const Type *n : distributedMembers(renamed)) {
          if (!n->isStringLiteral())
            return Type::error();
          names.push_back(utf8(n->literalText()));
        }
      }
      for (const std::string &field : names) {
        auto same = llvm::find_if(fields, [&](const Field &f) { return f.name == field; });
        if (same != fields.end())
          same->type = Type::unionOf({same->type, type});
        else
          fields.push_back({field, type, readonly});
      }
    }
    return Type::object(std::move(fields));
  }
  if (llvm::any_of(operands, [](const Type *t) { return t && mentionsParam(t); }))
    return Type::typeOp(kind, operands);
  for (const Type *t : operands)
    if (t && t->isError())
      return t;
  switch (kind) {
  case TypeOp::KeyOf: {
    const Type *keys = keysOf(operands[0]);
    return keys ? keys : Type::error();
  }
  case TypeOp::Index: {
    // `T["length"]` of a tuple: how many elements it has, as a literal type
    // (the way TypeScript counts in types); of an array or an argument list,
    // an int.
    const Type *object = operands[0];
    if (operands[1]->isStringLiteral() && operands[1]->literalText() == u"length" &&
        (object->isTuple() || object->isArray())) {
      if (object->isTuple() && !object->tupleRest() &&
          object->tupleMin() == object->fields().size())
        return Type::numberLiteral(double(object->fields().size()), Type::intTy());
      return Type::intTy();
    }
    const Type *member = memberType(operands[0], operands[1]);
    return member ? member : Type::error();
  }
  case TypeOp::Exclude:
  case TypeOp::Extract: {
    // The members of the first that extend (Extract) or do not extend
    // (Exclude) the second, one by one, as `T extends U ? T : never` does in
    // TypeScript: `Extract<Shape, { kind: "circle" }>` is the member with
    // that field.
    std::vector<const Type *> kept;
    for (const Type *m : distributedMembers(operands[0]))
      if (extendsType(m, operands[1]) == (kind == TypeOp::Extract))
        kept.push_back(m);
    return kept.empty() ? Type::never() : Type::unionOf(kept);
  }
  case TypeOp::Template:
    return evalTemplate(operands);
  case TypeOp::Spread: {
    std::vector<const Type *> elements;
    std::vector<bool> spread;
    for (size_t i = 0; i + 1 < operands.size(); i += 2) {
      spread.push_back(operands[i]->literalNumber() != 0);
      elements.push_back(operands[i + 1]);
    }
    std::string why;
    const Type *tuple = spreadTuple(elements, spread, why);
    return tuple ? tuple : Type::error();
  }
  case TypeOp::Uppercase:
  case TypeOp::Lowercase:
  case TypeOp::Capitalize:
  case TypeOp::Uncapitalize:
    return evalStringCase(kind, operands[0]);
  case TypeOp::NonNullable:
    return operands[0]->nonOptional();
  case TypeOp::ReturnType:
  case TypeOp::Parameters: {
    const Type *fn = operands[0]->nonOptional();
    if (!fn->isFunction())
      return Type::error();
    if (kind == TypeOp::ReturnType)
      return fn->ret();
    std::vector<const Type *> params(fn->params().begin(), fn->params().end() - (fn->hasRest() ? 1 : 0));
    return Type::argumentList(params, std::min(fn->minArgs(), params.size()),
                              fn->hasRest() ? fn->params().back() : nullptr);
  }
  default:
    return Type::error();
  }
}

// The type of the value `name` that the file `from` declares: a function, a
// class as a value, or a variable that its check gave a type.
const Type *Sema::importedValueType(Module *from, llvm::StringRef name) {
  for (Stmt *item : from->file.items) {
    if (auto *fn = dyn_cast<FunctionDecl>(item); fn && fn->name == name)
      return fn->sym && !fn->sym->isGeneric() ? functionType(fn->sym) : nullptr;
    if (auto *cls = dyn_cast<ClassDecl>(item); cls && cls->name == name)
      return cls->isGeneric() ? nullptr : classValueType(cls);
    if (auto *decl = dyn_cast<VarDeclStmt>(item))
      for (const VarDeclarator &d : decl->decls)
        if (d.name == name)
          return d.sym ? d.sym->type : nullptr;
  }
  return nullptr;
}

const Type *Sema::resolveTypeOp(const TypeRef &ref) {
  switch (ref.kind) {
  case TypeRef::Kind::KeyOf: {
    const Type *type = resolveType(ref.args[0], /*allowVoid=*/false);
    if (type->isError())
      return type;
    const Type *keys = evalTypeOp(Type::typeOp(TypeOp::KeyOf, {type}), {type});
    if (keys->isError())
      diags_.error(ref.loc, "'keyof' takes an object type, whose fields it names, not '" + type->name() + "'");
    return keys;
  }
  case TypeRef::Kind::Indexed: {
    const Type *object = resolveType(ref.args[0], /*allowVoid=*/false);
    const Type *key = resolveType(ref.args[1], /*allowVoid=*/false);
    if (object->isError() || key->isError())
      return Type::error();
    const Type *member = evalTypeOp(Type::indexedAccess(object, key), {object, key});
    if (member->isError())
      diags_.error(ref.loc, "'" + object->name() + "' has no member '" + key->name() + "'");
    return member;
  }
  case TypeRef::Kind::TypeOf: {
    // The type of the value: a variable, then its fields.
    llvm::StringRef name = ref.names.front();
    const Type *type = nullptr;
    ScopeEntry entry = lookupValue(name);
    if (VarSymbol *var = entry.var)
      type = var->type;
    else if (entry.cls && !entry.cls->isGeneric())
      type = classValueType(entry.cls); // of a class, the class as a value
    else if (entry.func && !entry.func->isGeneric())
      type = functionType(entry.func); // of a function, its type
    // A top-level constant not checked yet: the type its declaration says,
    // or that of its literal; a function not declared yet: the types it
    // declares.
    for (Stmt *item : module_ ? module_->file.items : std::vector<Stmt *>()) {
      if (type)
        break;
      if (auto *decl = dyn_cast<VarDeclStmt>(item))
        for (const VarDeclarator &d : decl->decls)
          if (d.name == name && !type)
            type = d.typeAnn ? resolveType(*d.typeAnn, /*allowVoid=*/false) : d.init ? literalShape(d.init) : nullptr;
      // Or the class its `new` makes: `const bus = new EventEmitter()`.
      if (auto *decl = dyn_cast<VarDeclStmt>(item); decl && !type)
        for (const VarDeclarator &d : decl->decls)
          if (auto *create = d.name == name ? llvm::dyn_cast_or_null<NewExpr>(d.init) : nullptr)
            type = createdType(create);
      auto *fn = dyn_cast<FunctionDecl>(item);
      if (!fn || fn->name != name || !fn->typeParams.empty() || !fn->returnType)
        continue;
      std::vector<const Type *> params;
      size_t minArgs = 0;
      bool written = true;
      for (const Param &p : fn->params) {
        if (!p.type) {
          written = false;
          break;
        }
        const Type *t = resolveType(*p.type, /*allowVoid=*/false);
        params.push_back(p.optional || p.defaultValue ? Type::optional(t) : t);
        if (!p.optional && !p.defaultValue && !p.rest)
          minArgs = params.size();
      }
      if (written)
        type = Type::function(params, minArgs, !fn->params.empty() && fn->params.back().rest,
                              resolveType(*fn->returnType, /*allowVoid=*/true));
    }
    // A name the file imports from one that loaded before it, while its
    // imports are not bound yet (the types of its declarations come first):
    // what that file declares, `typeof fetch` of the web's fetch.
    for (const ImportDecl &decl : module_ && !type ? module_->file.imports
                                                    : std::vector<ImportDecl>())
      for (const ModuleName &imported : decl.names)
        if (imported.as == name && decl.from && !type)
          type = importedValueType(decl.from, imported.name);
    if (!type) {
      diags_.error(ref.loc, "'typeof " + name + "' needs the type of '" + name +
                                "', which is not known here; declare it, e.g. 'const " + name +
                                ": T = ...'");
      return Type::error();
    }
    for (size_t i = 1; i < ref.names.size(); ++i) {
      const Field *field = type->isObject() ? type->field(ref.names[i]) : nullptr;
      // A method of a class, as a function; a getter (of a class of a
      // built-in module), what it gives.
      if (ClassDecl *cls = field ? nullptr : classOf(type)) {
        completeClass(cls);
        std::vector<FunctionDecl *> methods = methodsNamed(cls, ref.names[i], /*isStatic=*/false);
        if (!methods.empty() && methods.back()->sym &&
            methods.back()->accessor == FunctionDecl::Accessor::Getter) {
          type = functionType(methods.back()->sym)->ret();
          continue;
        }
        if (methods.size() == 1 && methods[0]->sym && methods[0]->typeParams.empty()) {
          type = functionType(methods[0]->sym);
          continue;
        }
      }
      if (!field) {
        diags_.error(ref.loc, "'" + type->name() + "' has no field '" + ref.names[i] + "'");
        return Type::error();
      }
      type = field->type;
    }
    return type;
  }
  case TypeRef::Kind::Mapped: {
    // The keys, before the name of the key is visible; then the values with
    // it, a type parameter of the mapped type.
    const Type *keys = resolveType(ref.args[0], /*allowVoid=*/false);
    if (keys->isError())
      return keys;
    const Type *source = nullptr;
    if (ref.args[0].kind == TypeRef::Kind::KeyOf)
      source = resolveType(ref.args[0].args[0], /*allowVoid=*/false);
    // `[P in K]` with `K extends keyof T`, and `[P in Exclude<keyof T, K>]`:
    // the fields of T keep their `?` and `readonly` too.
    for (const Type *k = keys; !source && k;) {
      if (k->typeOp() == TypeOp::KeyOf)
        source = k->operands()[0];
      else if (k->typeOp() == TypeOp::Exclude || k->typeOp() == TypeOp::Extract)
        k = k->operands()[0];
      else if (k->isParam() && k->constraint())
        k = k->constraint();
      else
        k = nullptr;
    }
    Type *key = Type::param(ref.name);
    key->setConstraint(keys, {});
    typeScopes_.emplace_back();
    auto *alias = ctx_.make<TypeAliasDecl>(ref.loc, ref.name, TypeRef());
    alias->resolved = key;
    typeScopes_.back()[ref.name] = alias;
    const Type *value = resolveType(ref.args[1], /*allowVoid=*/false);
    // `as`: the name each key gives instead (none for never).
    const Type *rename = ref.args.size() > 2 ? resolveTypeArgument(ref.args[2]) : nullptr;
    typeScopes_.pop_back();
    if (value->isError())
      return value;
    if (rename && rename->isError())
      return rename;
    if (rename && !mentionsParam(rename))
      for (const Type *n : distributedMembers(rename))
        if (!n->isStringLiteral()) {
          diags_.error(ref.args[2].loc,
                       "a mapped type renames its keys to names of fields, string literal "
                       "types such as `get${Capitalize<K>}`, not '" +
                           n->name() + "'");
          return Type::error();
        }
    for (const Type *k : mentionsParam(keys) ? llvm::ArrayRef<const Type *>() : membersOf(keys))
      if (!k->isStringLiteral() && !k->isNever()) {
        diags_.error(ref.args[0].loc, "the keys of a mapped type are names of fields, string literal types "
                                      "such as '\"id\" | \"name\"' or 'keyof T', not '" + k->name() + "'");
        return Type::error();
      }
    const Type *mapped =
        Type::mapped(key, keys, value, ref.mappedOptional, ref.mappedReadonly, source, rename);
    return evalTypeOp(mapped, mapped->operands());
  }
  default:
    return nullptr;
  }
}

const Type *Sema::resolveUtilityType(const TypeRef &ref) {
  static const struct {
    llvm::StringLiteral name;
    TypeOp op;
    size_t count;
  } utilities[] = {
      {"Exclude", TypeOp::Exclude, 2},           {"Extract", TypeOp::Extract, 2},
      {"NonNullable", TypeOp::NonNullable, 1},   {"ReturnType", TypeOp::ReturnType, 1},
      {"Parameters", TypeOp::Parameters, 1},     {"Uppercase", TypeOp::Uppercase, 1},
      {"Lowercase", TypeOp::Lowercase, 1},       {"Capitalize", TypeOp::Capitalize, 1},
      {"Uncapitalize", TypeOp::Uncapitalize, 1},
  };
  for (const auto &u : utilities) {
    if (ref.name != u.name || lookupType(ref.name))
      continue;
    if (ref.args.size() != u.count) {
      diags_.error(ref.loc, "'" + ref.name + "' takes " + std::to_string(u.count) + " type argument" +
                                (u.count == 1 ? "" : "s"));
      return Type::error();
    }
    bool function = u.op == TypeOp::ReturnType || u.op == TypeOp::Parameters;
    bool text = u.op == TypeOp::Uppercase || u.op == TypeOp::Lowercase ||
                u.op == TypeOp::Capitalize || u.op == TypeOp::Uncapitalize;
    std::vector<const Type *> operands;
    for (const TypeRef &arg : ref.args) {
      // `ReturnType<typeof f>` of a generic function or an overloaded method.
      const Type *signature = function ? signatureForUtility(arg) : nullptr;
      operands.push_back(signature ? signature : resolveTypeArgument(arg));
      if (operands.back()->isError())
        return operands.back();
    }
    if (function && !mentionsParam(operands[0]) && !operands[0]->nonOptional()->isFunction()) {
      diags_.error(ref.loc, "'" + ref.name + "' takes a function type, not '" + operands[0]->name() + "'");
      return Type::error();
    }
    if (text && !mentionsParam(operands[0]))
      for (const Type *m : distributedMembers(operands[0]))
        if (!m->isString() && !m->isStringLiteral()) {
          diags_.error(ref.loc, "'" + ref.name +
                                    "' takes a string type, such as a string literal type, "
                                    "not '" +
                                    m->name() + "'");
          return Type::error();
        }
    return evalTypeOp(Type::typeOp(u.op, operands), operands);
  }
  return nullptr;
}

Expr *Sema::checkGenericIndex(IndexExpr *index) {
  const Type *object = index->object->type;
  auto genericKey = [&](const Type *key) {
    return key->isParam() && !key->isListParam() && wantsLiteral(key);
  };
  bool open = object->isParam() && !object->isListParam() && !object->iterated();
  if (!open && !(object->isObject() && isa<NameExpr>(index->index)))
    return nullptr;
  // The key first, without changing it.
  if (!open) {
    auto *name = cast<NameExpr>(index->index);
    VarSymbol *var = lookup(name->name).var;
    if (!var || !var->type || !genericKey(var->type))
      return nullptr;
  }
  index->index = checkExpr(index->index);
  const Type *key = index->index->type;
  if (key->isError())
    return index;
  if (!key->isStringLiteral() && !genericKey(key)) {
    diags_.error(index->index->loc(), "'" + object->name() + "' is indexed by its keys, such as 'k: K' with 'K "
                                      "extends keyof " + object->name() + "', not by '" + key->name() + "'");
    return index;
  }
  index->type = evalTypeOp(Type::indexedAccess(object, key), {object, key});
  return index;
}

bool Sema::wantsLiteral(const Type *param) const {
  const Type *constraint = param->isParam() ? param->constraint() : nullptr;
  if (!constraint)
    return false;
  if (constraint->typeOp() == TypeOp::KeyOf)
    return true;
  const Type *c = constraint;
  return llvm::all_of(membersOf(c), [](const Type *m) { return m->isStringLiteral(); });
}

} // namespace tz
