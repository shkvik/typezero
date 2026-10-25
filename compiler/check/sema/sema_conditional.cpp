// Conditional types, `infer`, template literal types and the intrinsic
// string types (docs/generics.md, section 10): the computations over types
// that choose, as `T extends string ? A : B` does, and that make strings, as
// `` `on${Capitalize<K>}` `` does.
//
// A conditional type of known types gives the branch it chooses, and only
// that branch is resolved: a type alias may refer to itself there, which is
// how types loop over the elements of a tuple or the characters of a string
// literal. A branch that is another instance of an alias is a tail call,
// which instantiateAlias makes in a loop: such calls may go 1000 deep, other
// recursion 50 deep, as in TypeScript. Over type parameters a conditional
// type waits (Type::typeOp) until a copy gives them types, and is resolved
// again then.
//
// A template literal type of literals is the union of the strings it makes;
// with `string` or `number` in it, it is a string of a pattern
// (Type::stringPattern), which a literal is if it matches.

#include "compiler/check/sema/sema.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

#include <cmath>

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

bool typeMentions(const Type *type, llvm::ArrayRef<const Type *> params);

namespace {

// More strings than this from one template literal type is an error, as in
// TypeScript.
constexpr size_t kMaxTemplateStrings = 100000;

std::string utf8(const std::u16string &text) {
  std::string out;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      out);
  return out;
}

std::u16string utf16(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);
  return std::u16string(units.begin(), units.end());
}

// A name bound to a type, as a type parameter is (Sema::bindTypeParams),
// rather than declared.
bool isBinding(const Stmt *decl) {
  auto *alias = llvm::dyn_cast_or_null<TypeAliasDecl>(decl);
  return alias && alias->resolved && alias->typeParams.empty() &&
         alias->type.kind == TypeRef::Kind::Name && alias->type.name.empty();
}

// The members a conditional type goes over one by one when the type it
// checks is a type parameter: those of a union, undefined of an optional, and
// false and true of boolean, as in TypeScript.
void distribute(const Type *type, std::vector<const Type *> &out) {
  if (type->isOptional()) {
    distribute(type->inner(), out);
    out.push_back(Type::undefinedTy());
  } else if (type->isUnion()) {
    for (const Type *m : type->members())
      distribute(m, out);
  } else if (type->isBool()) {
    out.push_back(Type::boolLiteral(false));
    out.push_back(Type::boolLiteral(true));
  } else if (!type->isNever()) {
    out.push_back(type);
  }
}

std::vector<const Type *> distributed(const Type *type) {
  std::vector<const Type *> out;
  distribute(type, out);
  return out;
}

// The names of types that a type written uses, but `infer` and the keys of
// mapped types, which it declares.
void collectNames(const TypeRef &ref, std::vector<llvm::StringRef> &names) {
  if ((ref.kind == TypeRef::Kind::Name || ref.kind == TypeRef::Kind::Generic) &&
      !ref.name.empty() && !llvm::is_contained(names, ref.name))
    names.push_back(ref.name);
  for (const TypeRef &arg : ref.args)
    collectNames(arg, names);
}

// The types `infer` declares in the pattern of a conditional type (not in
// one nested there, which has its own), and whether each is spread into a
// tuple or into the parameters of a function: `[infer H, ...infer R]` makes R
// a list of types.
void collectInfers(const TypeRef &ref, std::vector<const TypeRef *> &infers,
                   std::vector<bool> &lists, bool spread = false) {
  if (ref.kind == TypeRef::Kind::Conditional)
    return;
  if (ref.kind == TypeRef::Kind::Infer) {
    auto found = llvm::find_if(infers, [&](const TypeRef *r) { return r->name == ref.name; });
    if (found == infers.end()) {
      infers.push_back(&ref);
      lists.push_back(spread);
    } else if (spread) {
      lists[size_t(found - infers.begin())] = true;
    }
  }
  for (size_t i = 0; i < ref.args.size(); ++i) {
    bool spreadHere =
        (ref.kind == TypeRef::Kind::Tuple && i < ref.spread.size() && ref.spread[i]) ||
        (ref.kind == TypeRef::Kind::Function && ref.rest && i + 2 == ref.args.size());
    collectInfers(ref.args[i], infers, lists, spreadHere);
  }
}

// See mentionsParamBesides.
bool mentionsOtherParams(const Type *type, llvm::ArrayRef<const Type *> allowed,
                         llvm::SmallPtrSetImpl<const Type *> &seen) {
  if (!mentionsParam(type))
    return false;
  auto other = [&](const Type *t) { return t && mentionsOtherParams(t, allowed, seen); };
  switch (type->kind()) {
  case Type::Kind::Param:
    // A conditional type that waits, by what its names stand for.
    if (type->typeOp() == TypeOp::Conditional)
      return llvm::any_of(type->operands().drop_front(), other);
    if (type->typeOp() != TypeOp::None)
      return llvm::any_of(type->operands(), other);
    return !llvm::is_contained(allowed, type);
  case Type::Kind::Optional:
  case Type::Kind::Array:
  case Type::Kind::Promise: return other(type->inner());
  case Type::Kind::Result: return other(type->okType()) || other(type->errType());
  case Type::Kind::Function: return other(type->ret()) || llvm::any_of(type->params(), other);
  case Type::Kind::Iterator:
    return other(type->yielded()) || other(type->returned()) || other(type->sent());
  case Type::Kind::Dict: return other(type->keyType()) || other(type->valueType());
  case Type::Kind::Union: return llvm::any_of(type->members(), other);
  case Type::Kind::Object:
    if (type->isAsyncIterator())
      return other(type->yielded()) || other(type->returned()) || other(type->sent());
    if (!type->typeArgs().empty())
      return llvm::any_of(type->typeArgs(), other);
    if (!seen.insert(type).second)
      return false;
    if (other(type->tupleRest()))
      return true;
    return llvm::any_of(type->fields(), [&](const Field &f) { return other(f.type); });
  default: return false;
  }
}

// The type parameters `type` mentions, but those of the computations in it.
void collectParams(const Type *type, std::vector<const Type *> &params,
                   llvm::SmallPtrSetImpl<const Type *> &seen) {
  if (!mentionsParam(type) || !seen.insert(type).second)
    return;
  auto add = [&](const Type *t) {
    if (t)
      collectParams(t, params, seen);
  };
  switch (type->kind()) {
  case Type::Kind::Param:
    if (type->typeOp() == TypeOp::None)
      params.push_back(type);
    break;
  case Type::Kind::Optional:
  case Type::Kind::Array:
  case Type::Kind::Promise: add(type->inner()); break;
  case Type::Kind::Result:
    add(type->okType());
    add(type->errType());
    break;
  case Type::Kind::Function:
    add(type->ret());
    for (const Type *p : type->params())
      add(p);
    break;
  case Type::Kind::Dict:
    add(type->keyType());
    add(type->valueType());
    break;
  case Type::Kind::Union:
    for (const Type *m : type->members())
      add(m);
    break;
  case Type::Kind::Object:
    for (const Type *t : type->typeArgs())
      add(t);
    for (const Field &f : type->fields())
      add(f.type);
    break;
  default: break;
  }
}

// What a template literal type may hold: strings, numbers, booleans,
// undefined, and in generic code a type parameter.
bool templateHole(const Type *type) {
  return type->isString() || type->isNumeric() || type->isBool() || type->isLiteral() ||
         type->isUndefined() || type->isParam();
}

// Upper and lower case of a character as JavaScript's toUpperCase and
// toLowerCase give them, for the Latin, Greek and Cyrillic letters; other
// characters stay.
std::u16string upperCase(char16_t c) {
  if (c >= u'a' && c <= u'z')
    return std::u16string(1, char16_t(c - 32));
  if (c == 0xdf)
    return u"SS";
  if (c == 0xb5)
    return std::u16string(1, char16_t(0x39c));
  if (c >= 0xe0 && c <= 0xfe && c != 0xf7)
    return std::u16string(1, char16_t(c - 32));
  if (c == 0xff)
    return std::u16string(1, char16_t(0x178));
  if (c == 0x131 || c == 0x17f)
    return c == 0x131 ? u"I" : u"S";
  // Latin Extended-A: pairs of an upper and a lower case letter.
  if ((c >= 0x100 && c <= 0x137) || (c >= 0x14a && c <= 0x177))
    return std::u16string(1, char16_t(c & ~1u));
  if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17e))
    return std::u16string(1, char16_t(c % 2 == 0 ? c - 1 : c));
  if (c == 0x3c2)
    return std::u16string(1, char16_t(0x3a3));
  if ((c >= 0x3b1 && c <= 0x3c1) || (c >= 0x3c3 && c <= 0x3cb))
    return std::u16string(1, char16_t(c - 32));
  if (c == 0x3ac)
    return std::u16string(1, char16_t(0x386));
  if (c >= 0x3ad && c <= 0x3af)
    return std::u16string(1, char16_t(c - 37));
  if (c == 0x3cc)
    return std::u16string(1, char16_t(0x38c));
  if (c == 0x3cd || c == 0x3ce)
    return std::u16string(1, char16_t(c - 63));
  if (c >= 0x430 && c <= 0x44f)
    return std::u16string(1, char16_t(c - 32));
  if (c >= 0x450 && c <= 0x45f)
    return std::u16string(1, char16_t(c - 80));
  return std::u16string(1, c);
}

std::u16string lowerCase(char16_t c) {
  if (c >= u'A' && c <= u'Z')
    return std::u16string(1, char16_t(c + 32));
  if (c >= 0xc0 && c <= 0xde && c != 0xd7)
    return std::u16string(1, char16_t(c + 32));
  if (c == 0x178)
    return std::u16string(1, char16_t(0xff));
  if (c == 0x130) // i with a combining dot above
    return std::u16string{u'i', char16_t(0x307)};
  if ((c >= 0x100 && c <= 0x137) || (c >= 0x14a && c <= 0x177))
    return std::u16string(1, char16_t(c | 1u));
  if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17e))
    return std::u16string(1, char16_t(c % 2 == 1 ? c + 1 : c));
  if ((c >= 0x391 && c <= 0x3a1) || (c >= 0x3a3 && c <= 0x3ab))
    return std::u16string(1, char16_t(c + 32));
  if (c == 0x386)
    return std::u16string(1, char16_t(0x3ac));
  if (c >= 0x388 && c <= 0x38a)
    return std::u16string(1, char16_t(c + 37));
  if (c == 0x38c)
    return std::u16string(1, char16_t(0x3cc));
  if (c == 0x38e || c == 0x38f)
    return std::u16string(1, char16_t(c + 63));
  if (c >= 0x410 && c <= 0x42f)
    return std::u16string(1, char16_t(c + 32));
  if (c >= 0x400 && c <= 0x40f)
    return std::u16string(1, char16_t(c + 80));
  return std::u16string(1, c);
}

std::u16string changeCase(TypeOp op, const std::u16string &text) {
  std::u16string out;
  for (size_t i = 0; i < text.size(); ++i) {
    bool first = i == 0;
    if (op == TypeOp::Uppercase || (op == TypeOp::Capitalize && first))
      out += upperCase(text[i]);
    else if (op == TypeOp::Lowercase || (op == TypeOp::Uncapitalize && first))
      out += lowerCase(text[i]);
    else
      out += text[i];
  }
  return out;
}

} // namespace

bool mentionsParamBesides(const Type *type, llvm::ArrayRef<const Type *> allowed) {
  llvm::SmallPtrSet<const Type *, 4> seen;
  return mentionsOtherParams(type, allowed, seen);
}

void Sema::bindType(llvm::StringRef name, const Type *type, llvm::SMLoc loc) {
  auto *alias = ctx_.make<TypeAliasDecl>(loc, name, TypeRef());
  alias->resolved = type;
  typeScopes_.back()[name] = alias;
}

const Type *Sema::resolveTypeArgument(const TypeRef &ref) { return resolveTypeInner(ref); }

//===----------------------------------------------------------------------===//
// Conditional types
//===----------------------------------------------------------------------===//

const Type *Sema::resolveConditional(const TypeRef &ref) {
  bool tail = std::exchange(tailAllowed_, false);
  if (ref.args.size() != 4)
    return Type::error(); // the parser reported it
  const Type *check = resolveTypeInner(ref.args[0]);
  if (check->isError())
    return check;
  // The types `infer` declares: type parameters of the pattern, which the
  // match gives types.
  std::vector<const TypeRef *> inferRefs;
  std::vector<bool> lists;
  collectInfers(ref.args[1], inferRefs, lists);
  std::vector<const Type *> infers;
  typeScopes_.emplace_back();
  for (size_t i = 0; i < inferRefs.size(); ++i) {
    Type *param = Type::param(inferRefs[i]->name);
    if (lists[i])
      param->setList();
    infers.push_back(param);
    bindType(inferRefs[i]->name, param, inferRefs[i]->loc);
  }
  for (size_t i = 0; i < inferRefs.size(); ++i) {
    if (inferRefs[i]->args.empty())
      continue;
    const Type *constraint = resolveTypeInner(inferRefs[i]->args[0]);
    if (constraint->isError()) {
      typeScopes_.pop_back();
      return constraint;
    }
    const_cast<Type *>(infers[i])->setConstraint(constraint, {});
  }
  ++extendsClause_;
  const Type *pattern = resolveTypeInner(ref.args[1]);
  --extendsClause_;
  typeScopes_.pop_back();
  if (pattern->isError())
    return pattern;
  // Over type parameters it waits for a copy.
  if (mentionsParam(check) || mentionsParamBesides(pattern, infers))
    return deferConditional(ref);
  // A type parameter that stands for a union gives the union of what each
  // of its members gives, as in TypeScript; the check of a type parameter
  // that stands for never gives never.
  if (ref.args[0].kind == TypeRef::Kind::Name && isBinding(lookupType(ref.args[0].name))) {
    std::vector<const Type *> members = distributed(check);
    if (members.size() != 1) {
      std::vector<const Type *> results;
      for (const Type *m : members) {
        typeScopes_.emplace_back();
        bindType(ref.args[0].name, m, ref.loc);
        const Type *result = resolveConditional(ref);
        typeScopes_.pop_back();
        if (result->isError())
          return result;
        results.push_back(result);
      }
      return results.empty() ? Type::never() : Type::unionOf(results);
    }
  }
  InferMatch inference{infers, std::vector<const Type *>(infers.size(), nullptr)};
  bool yes = extendsType(check, pattern, &inference);
  typeScopes_.emplace_back();
  // What `infer` matched; one that matched nothing is its constraint, or
  // unknown.
  for (size_t i = 0; yes && i < infers.size(); ++i) {
    const Type *bound = inference.bound[i];
    if (!bound)
      bound = infers[i]->constraint() ? infers[i]->constraint() : Type::unknown();
    bindType(inferRefs[i]->name, bound, inferRefs[i]->loc);
  }
  const TypeRef &branch = ref.args[yes ? 2 : 3];
  const Type *result = nullptr;
  if (tail && branch.kind == TypeRef::Kind::Conditional) {
    tailAllowed_ = true;
    result = resolveTypeInner(branch);
  } else if (auto *alias = tail && branch.kind == TypeRef::Kind::Generic
                               ? llvm::dyn_cast_or_null<TypeAliasDecl>(lookupType(branch.name))
                               : nullptr;
             alias && !alias->typeParams.empty()) {
    // A tail call: instantiateAlias goes on with it.
    std::vector<const Type *> args;
    for (const TypeRef &arg : branch.args) {
      args.push_back(resolveTypeArgument(arg));
      if (args.back()->isError()) {
        typeScopes_.pop_back();
        return args.back();
      }
    }
    tailCall_ = TailCall{alias, std::move(args)};
  } else {
    result = resolveTypeInner(branch);
  }
  typeScopes_.pop_back();
  return result;
}

const Type *Sema::deferConditional(const TypeRef &ref) {
  ConditionalInfo &info = conditionals_[&ref];
  if (!info.tag) {
    info.ref = &ref;
    // Named by its text, the spaces in it collapsed.
    std::string spelling;
    for (char c : ref.source) {
      if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
        if (!spelling.empty() && spelling.back() != ' ')
          spelling += ' ';
      } else {
        spelling += c;
      }
    }
    while (!spelling.empty() && spelling.back() == ' ')
      spelling.pop_back();
    info.tag = Type::param(spelling.empty() ? "conditional type" : spelling);
    conditionalTags_[info.tag] = &info;
    std::vector<llvm::StringRef> names;
    collectNames(ref, names);
    for (llvm::StringRef name : names) {
      Stmt *decl = lookupType(name);
      if (!decl)
        continue;
      if (isBinding(decl))
        info.bound.push_back(name);
      else
        info.decls.push_back({name, decl});
    }
  }
  std::vector<const Type *> operands{info.tag};
  for (llvm::StringRef name : info.bound) {
    auto *alias = llvm::dyn_cast_or_null<TypeAliasDecl>(lookupType(name));
    operands.push_back(alias && alias->resolved ? alias->resolved : Type::error());
  }
  return Type::typeOp(TypeOp::Conditional, operands);
}

const Type *Sema::evalConditional(llvm::ArrayRef<const Type *> operands) {
  const ConditionalInfo *info = conditionalTags_.lookup(operands[0]);
  if (!info)
    return Type::error();
  for (const Type *t : operands.drop_front())
    if (t->isError())
      return t;
  typeScopes_.emplace_back();
  for (const auto &[name, decl] : info->decls)
    typeScopes_.back()[name] = decl;
  for (size_t i = 0; i < info->bound.size() && i + 1 < operands.size(); ++i)
    bindType(info->bound[i], operands[i + 1], info->ref->loc);
  bool tail = std::exchange(tailAllowed_, false);
  const Type *result = resolveConditional(*info->ref);
  tailAllowed_ = tail;
  typeScopes_.pop_back();
  return result ? result : Type::error();
}

std::vector<const Type *> Sema::conditionalBranches(const Type *conditional) {
  if (conditional->typeOp() != TypeOp::Conditional)
    return {};
  llvm::ArrayRef<const Type *> operands = conditional->operands();
  const ConditionalInfo *info = conditionalTags_.lookup(operands[0]);
  if (!info)
    return {};
  typeScopes_.emplace_back();
  for (const auto &[name, decl] : info->decls)
    typeScopes_.back()[name] = decl;
  for (size_t i = 0; i < info->bound.size() && i + 1 < operands.size(); ++i)
    bindType(info->bound[i], operands[i + 1], info->ref->loc);
  // What `infer` gives is not known: type parameters of their own.
  std::vector<const TypeRef *> inferRefs;
  std::vector<bool> lists;
  collectInfers(info->ref->args[1], inferRefs, lists);
  for (size_t i = 0; i < inferRefs.size(); ++i) {
    Type *param = Type::param(inferRefs[i]->name);
    if (lists[i])
      param->setList();
    bindType(inferRefs[i]->name, param, inferRefs[i]->loc);
  }
  bool muted = diags_.setMuted(true);
  std::vector<const Type *> branches;
  for (size_t i : {2, 3}) {
    const Type *branch = resolveTypeInner(info->ref->args[i]);
    if (branch && !branch->isError())
      branches.push_back(branch);
  }
  diags_.setMuted(muted);
  typeScopes_.pop_back();
  return branches;
}

bool Sema::fitsConditional(const Type *from, const Type *to, unsigned depth) {
  if (depth > 8 || to->typeOp() != TypeOp::Conditional)
    return false;
  // A value of a union, `flag ? "text" : "other"`: each member of either.
  if (from->isUnion())
    return llvm::all_of(from->members(),
                        [&](const Type *m) { return fitsConditional(m, to, depth + 1); });
  for (const Type *branch : conditionalBranches(to)) {
    if (isImplicitlyConvertible(from, branch) || fitsConditional(from, branch, depth + 1))
      return true;
    // Or it fits for some types of the type parameters the branch mentions:
    // `{ text: x }` with x narrowed to a string, for `{ text: T }`.
    std::vector<const Type *> params;
    llvm::SmallPtrSet<const Type *, 8> seen;
    collectParams(branch, params, seen);
    InferMatch match{params, std::vector<const Type *>(params.size(), nullptr)};
    if (!params.empty() && extendsType(from, branch, &match))
      return true;
  }
  return false;
}

//===----------------------------------------------------------------------===//
// The relation of conditional types
//===----------------------------------------------------------------------===//

bool Sema::extendsType(const Type *type, const Type *pattern, InferMatch *inference) {
  // `infer U`: what stands in its place, which must meet its constraint.
  if (inference) {
    auto found = llvm::find(inference->params, pattern);
    if (found != inference->params.end()) {
      if (const Type *constraint = pattern->constraint();
          constraint && !extendsType(type, constraint, inference))
        return false;
      const Type *&bound = inference->bound[size_t(found - inference->params.begin())];
      bound = bound && bound != type ? Type::unionOf({bound, type}) : type;
      return true;
    }
  }
  if (type == pattern || type->isError() || pattern->isError() || type->isNever())
    return true;
  if (pattern->isUnknown())
    return true;
  if (pattern->isNever())
    return false;
  // A union extends what each of its members does.
  if (type->isUnion() || type->isOptional())
    return llvm::all_of(distributed(type),
                        [&](const Type *m) { return extendsType(m, pattern, inference); });
  if (pattern->isOptional())
    return type->isUndefined() || extendsType(type, pattern->inner(), inference);
  if (type->isUndefined())
    return false;
  if (pattern->isUnion()) {
    for (const Type *m : pattern->members()) {
      std::vector<const Type *> saved = inference ? inference->bound : std::vector<const Type *>();
      if (extendsType(type, m, inference))
        return true;
      if (inference)
        inference->bound = std::move(saved);
    }
    return false;
  }
  // Strings and their patterns.
  if (pattern->typeOp() == TypeOp::Template)
    return type->isStringLiteral() &&
           matchTemplate(type->literalText(), pattern->operands(), inference);
  if (pattern->isPattern())
    return type->isStringLiteral() && matchesPattern(type->literalText(), pattern);
  if (type->isPattern())
    return pattern == Type::string();
  // A literal extends what its base does: `"a" extends string`.
  if (type->isLiteral())
    return !pattern->isLiteral() && extendsType(type->literalBase(), pattern, inference);
  if (pattern->isLiteral() || type->isParam() || pattern->isParam())
    return false;
  if (pattern->isAnyObject())
    return isObjectReference(type);
  if (type->isArray())
    return pattern->isArray() && extendsType(type->inner(), pattern->inner(), inference);
  if (type->isTuple()) {
    if (pattern->isArray()) {
      for (const Field &f : type->fields())
        if (!extendsType(f.type, pattern->inner(), inference))
          return false;
      return !type->tupleRest() ||
             extendsType(type->tupleRest()->inner(), pattern->inner(), inference);
    }
    return pattern->isTuple() && extendsTuple(type, pattern, inference);
  }
  if (type->isFunction())
    return pattern->isFunction() && extendsFunction(type, pattern, inference);
  if (type->isPromise())
    return pattern->isPromise() && extendsType(type->promised(), pattern->promised(), inference);
  if (type->isIterator())
    return pattern->isIterator() && extendsType(type->yielded(), pattern->yielded(), inference) &&
           extendsType(type->returned(), pattern->returned(), inference) &&
           extendsType(type->sent(), pattern->sent(), inference);
  if (type->isDict())
    return pattern->isDict() && type->dictKind() == pattern->dictKind() &&
           extendsType(type->keyType(), pattern->keyType(), inference) &&
           extendsType(type->valueType(), pattern->valueType(), inference);
  if (type->isResult())
    return pattern->isResult() && extendsType(type->okType(), pattern->okType(), inference) &&
           extendsType(type->errType(), pattern->errType(), inference);
  if (type->isObject() && pattern->isObject())
    return extendsObject(type, pattern, inference);
  // Numbers and the other values: as they convert, `int extends number`.
  return !pattern->isObject() && !type->isObject() && isImplicitlyConvertible(type, pattern);
}

bool Sema::extendsTuple(const Type *type, const Type *pattern, InferMatch *inference) {
  std::vector<const Type *> mine, theirs;
  for (const Field &f : type->fields())
    mine.push_back(f.type);
  for (const Field &f : pattern->fields())
    theirs.push_back(f.type);
  // A list of types in the pattern, `[infer H, ...infer R]`, takes the
  // elements the others leave.
  size_t spread = theirs.size();
  for (size_t i = 0; i < theirs.size(); ++i)
    if (theirs[i]->isListParam()) {
      if (spread != theirs.size())
        return false;
      spread = i;
    }
  // An argument list, with elements that may be left out: only as a whole.
  if (type->tupleRest() || type->tupleMin() < mine.size())
    return spread == 0 && theirs.size() == 1 && extendsType(type, theirs[0], inference);
  if (spread == theirs.size()) {
    if (pattern->tupleRest()) {
      if (mine.size() < theirs.size())
        return false;
      for (size_t i = 0; i < mine.size(); ++i)
        if (!extendsType(mine[i], i < theirs.size() ? theirs[i] : pattern->tupleRest()->inner(),
                         inference))
          return false;
      return true;
    }
    if (mine.size() != theirs.size() || pattern->tupleMin() < theirs.size())
      return false;
    for (size_t i = 0; i < mine.size(); ++i)
      if (!extendsType(mine[i], theirs[i], inference))
        return false;
    return true;
  }
  size_t after = theirs.size() - spread - 1;
  if (mine.size() < spread + after)
    return false;
  for (size_t i = 0; i < spread; ++i)
    if (!extendsType(mine[i], theirs[i], inference))
      return false;
  for (size_t k = 0; k < after; ++k)
    if (!extendsType(mine[mine.size() - after + k], theirs[spread + 1 + k], inference))
      return false;
  std::vector<const Type *> middle(mine.begin() + long(spread), mine.end() - long(after));
  return extendsType(Type::tuple(middle), theirs[spread], inference);
}

bool Sema::extendsFunction(const Type *type, const Type *pattern, InferMatch *inference) {
  // What the function returns; anything goes where void is expected.
  if (!pattern->ret()->isVoid() && !extendsType(type->ret(), pattern->ret(), inference))
    return false;
  llvm::ArrayRef<const Type *> mine = type->params(), theirs = pattern->params();
  size_t myFixed = mine.size() - (type->hasRest() ? 1 : 0);
  size_t theirFixed = theirs.size() - (pattern->hasRest() ? 1 : 0);
  // A function that needs more arguments than the pattern gives does not
  // fit; one that takes fewer does.
  if (!pattern->hasRest() && type->minArgs() > theirs.size())
    return false;
  auto mineAt = [&](size_t i) -> const Type * {
    if (i < myFixed)
      return mine[i];
    return type->hasRest() && mine.back()->isArray() ? mine.back()->inner() : nullptr;
  };
  // A parameter of the pattern: one with `infer` takes mine; another must go
  // where mine is expected, as parameters are contravariant.
  auto fits = [&](const Type *my, const Type *their) {
    if (inference && typeMentions(their, inference->params))
      return extendsType(my, their, inference);
    return isImplicitlyConvertible(their, my) || extendsType(their, my);
  };
  for (size_t i = 0; i < theirFixed; ++i)
    if (const Type *my = mineAt(i); my && !fits(my, theirs[i]))
      return false;
  if (!pattern->hasRest())
    return true;
  const Type *rest = theirs.back();
  if (inference && llvm::is_contained(inference->params, rest)) {
    // `(...args: infer A) => R`: my parameters from there on, as a list.
    size_t from = std::min(theirFixed, myFixed);
    std::vector<const Type *> elements(mine.begin() + long(from), mine.begin() + long(myFixed));
    size_t min = type->minArgs() > from ? type->minArgs() - from : 0;
    const Type *list = Type::argumentList(elements, std::min(min, elements.size()),
                                          type->hasRest() ? mine.back() : nullptr);
    return extendsType(list, rest, inference);
  }
  // `...args: unknown[]` or `never[]`: any parameters, as `any[]` is in
  // TypeScript.
  const Type *element = rest->isArray() ? rest->inner() : nullptr;
  if (!element || element->isUnknown() || element->isNever())
    return true;
  for (size_t i = theirFixed; i < myFixed; ++i)
    if (!fits(mine[i], element))
      return false;
  return true;
}

bool Sema::extendsObject(const Type *type, const Type *pattern, InferMatch *inference) {
  if (type->isSubtypeOf(pattern))
    return true;
  if (pattern->isClass()) {
    // `Box<infer U>`: an instance of the same generic class, up the chain.
    const ClassDecl *want = pattern->classDecl();
    want = want->generic ? want->generic : want;
    if (pattern->typeArgs().empty())
      return false;
    std::vector<const Type *> todo{type};
    while (!todo.empty()) {
      const Type *t = todo.back();
      todo.pop_back();
      const ClassDecl *cls = t->classDecl();
      if (cls && (cls->generic ? cls->generic : cls) == want &&
          t->typeArgs().size() == pattern->typeArgs().size()) {
        bool all = true;
        for (size_t k = 0; all && k < t->typeArgs().size(); ++k)
          all = extendsType(t->typeArgs()[k], pattern->typeArgs()[k], inference);
        if (all)
          return true;
      }
      todo.insert(todo.end(), t->bases().begin(), t->bases().end());
    }
    return false;
  }
  if (type->isTuple() || pattern->isTuple() || type->isAnyObject() || !pattern->overloads().empty())
    return false;
  // At least the fields of the pattern, of types that extend theirs; a method
  // of a class is a field that holds a function, and a getter (of a class of
  // a built-in module, `status` of a Response) one of what it gives.
  ClassDecl *cls = classOf(type);
  if (cls)
    completeClass(cls);
  for (const Field &f : pattern->fields()) {
    const Type *own = nullptr;
    if (const Field *have = type->field(f.name))
      own = have->type;
    else if (cls)
      if (std::vector<FunctionDecl *> methods = methodsNamed(cls, f.name, /*isStatic=*/false);
          !methods.empty() && methods.back()->sym && methods.back()->typeParams.empty()) {
        own = functionType(methods.back()->sym);
        if (methods.back()->accessor == FunctionDecl::Accessor::Getter)
          own = own->ret();
        else if (methods.back()->accessor == FunctionDecl::Accessor::Setter)
          own = nullptr;
      }
    if (!own) {
      if (f.type->isOptional())
        continue;
      return false;
    }
    if (!extendsType(own, f.type, inference))
      return false;
  }
  return true;
}

bool Sema::matchTemplate(const std::u16string &text, llvm::ArrayRef<const Type *> parts,
                         InferMatch *inference) {
  std::vector<std::u16string> texts;
  std::vector<const Type *> holes;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i % 2 == 0)
      texts.push_back(parts[i]->literalText());
    else
      holes.push_back(parts[i]);
  }
  std::vector<std::u16string> pieces;
  if (!splitByTemplate(text, texts, pieces))
    return false;
  for (size_t k = 0; k < holes.size(); ++k) {
    const Type *hole = holes[k];
    if (!mentionsParam(hole)) {
      if (!holeAccepts(hole, pieces[k]))
        return false;
      continue;
    }
    const Type *piece = Type::stringLiteral(pieces[k]);
    // `infer N extends int`: the number the text is, if it reads back as
    // that text; the constraint itself if not.
    if (inference && llvm::is_contained(inference->params, hole))
      if (const Type *constraint = hole->constraint();
          constraint && constraint->nonOptional()->isNumeric()) {
        double value = 0;
        if (!numberString(pieces[k], value))
          return false;
        bool whole = value == std::trunc(value) && value >= -2147483648.0 && value <= 2147483647.0;
        const Type *number = Type::numberLiteral(value, whole ? Type::intTy() : Type::doubleTy());
        piece = number->name() == utf8(pieces[k]) ? number : constraint->nonOptional();
      }
    if (!extendsType(piece, hole, inference))
      return false;
  }
  return true;
}

//===----------------------------------------------------------------------===//
// Template literal types and the intrinsic string types
//===----------------------------------------------------------------------===//

const Type *Sema::resolveTemplate(const TypeRef &ref) {
  std::vector<const Type *> operands;
  for (size_t i = 0; i < ref.texts.size(); ++i) {
    operands.push_back(Type::stringLiteral(ref.texts[i]));
    if (i >= ref.args.size())
      break;
    const Type *hole = resolveTypeInner(ref.args[i]);
    if (hole->isError())
      return hole;
    for (const Type *m : distributed(hole))
      if (!templateHole(m)) {
        diags_.error(ref.args[i].loc,
                     "a template literal type holds strings, numbers, booleans and "
                     "undefined, not '" +
                         m->name() + "'");
        return Type::error();
      }
    operands.push_back(hole);
  }
  templateLoc_ = ref.loc;
  const Type *type = evalTypeOp(Type::typeOp(TypeOp::Template, operands), operands);
  templateLoc_ = llvm::SMLoc();
  return type;
}

const Type *Sema::evalTemplate(llvm::ArrayRef<const Type *> operands) {
  // Each choice of the members of the holes makes a string literal, or a
  // pattern if some hole is `string` or a number.
  struct Part {
    std::vector<std::u16string> texts;
    std::vector<const Type *> holes;
  };
  std::vector<Part> parts{{{operands[0]->literalText()}, {}}};
  for (size_t i = 1; i + 1 < operands.size(); i += 2) {
    std::vector<const Type *> members = distributed(operands[i]);
    const std::u16string &after = operands[i + 1]->literalText();
    std::vector<Part> next;
    for (const Part &part : parts)
      for (const Type *m : members) {
        Part made = part;
        if (m->isStringLiteral()) {
          made.texts.back() += m->literalText();
        } else if (m->isLiteral()) {
          made.texts.back() += utf16(m->name()); // a number or a boolean, as written
        } else if (m->isUndefined()) {
          made.texts.back() += u"undefined";
        } else if (m->isPattern()) {
          made.texts.back() += m->patternTexts()[0];
          for (size_t k = 0; k < m->patternHoles().size(); ++k) {
            made.holes.push_back(m->patternHoles()[k]);
            made.texts.push_back(m->patternTexts()[k + 1]);
          }
        } else if (m->isString() || m->isNumeric()) {
          made.holes.push_back(m);
          made.texts.emplace_back();
        } else {
          return Type::error();
        }
        made.texts.back() += after;
        next.push_back(std::move(made));
        if (next.size() > kMaxTemplateStrings) {
          llvm::SMLoc loc = templateLoc_.isValid() ? templateLoc_ : instanceLoc_;
          if (loc.isValid())
            diags_.error(loc, "this template literal type makes more than " +
                                  std::to_string(kMaxTemplateStrings) +
                                  " strings, the most TypeScript allows; use 'string'");
          return Type::error();
        }
      }
    parts = std::move(next);
  }
  std::vector<const Type *> results;
  for (Part &part : parts)
    results.push_back(part.holes.empty() ? Type::stringLiteral(part.texts[0])
                                         : Type::stringPattern(part.texts, part.holes));
  return results.empty() ? Type::never() : Type::unionOf(results);
}

const Type *Sema::evalStringCase(TypeOp op, const Type *type) {
  std::vector<const Type *> results;
  for (const Type *m : distributed(type)) {
    if (m->isStringLiteral()) {
      results.push_back(Type::stringLiteral(changeCase(op, m->literalText())));
    } else if (m->isPattern()) {
      // The texts of a pattern; what its holes hold stays as it is.
      std::vector<std::u16string> texts(m->patternTexts().begin(), m->patternTexts().end());
      if (op == TypeOp::Uppercase || op == TypeOp::Lowercase)
        for (std::u16string &text : texts)
          text = changeCase(op, text);
      else
        texts[0] = changeCase(op, texts[0]);
      results.push_back(Type::stringPattern(
          texts, std::vector<const Type *>(m->patternHoles().begin(), m->patternHoles().end())));
    } else if (m->isString()) {
      results.push_back(Type::string());
    } else {
      return Type::error();
    }
  }
  return results.empty() ? Type::never() : Type::unionOf(results);
}

const Type *Sema::resolveIntersection(const TypeRef &ref) {
  std::vector<const Type *> parts;
  for (const TypeRef &arg : ref.args) {
    parts.push_back(resolveTypeInner(arg));
    if (parts.back()->isError())
      return parts.back();
  }
  auto primitive = [](const Type *t) {
    return (t->isString() && !t->isPattern()) || t->isNumeric() || t->isBool();
  };
  auto emptyObject = [](const Type *t) {
    return t->isObject() && !t->isClass() && !t->isTuple() && !t->isAnyObject() &&
           t->fields().empty() && t->overloads().empty();
  };
  if (parts.size() == 2)
    for (size_t i : {0, 1}) {
      const Type *other = parts[1 - i];
      // `K & string`: what of K is a string, as `Extract<K, string>`.
      if (primitive(parts[i]))
        return evalTypeOp(Type::typeOp(TypeOp::Extract, {other, parts[i]}), {other, parts[i]});
      // `T & {}`: T without undefined, as `NonNullable<T>`.
      if (emptyObject(parts[i]))
        return evalTypeOp(Type::typeOp(TypeOp::NonNullable, {other}), {other});
    }
  diags_.error(ref.loc,
               "intersection types are not supported: a value has exactly the fields of its one "
               "type; of TypeScript's idioms with '&', 'K & string' (what of K is a string) and "
               "'T & {}' (T without undefined) work");
  return Type::error();
}

//===----------------------------------------------------------------------===//
// ReturnType and Parameters of generic and overloaded functions
//===----------------------------------------------------------------------===//

const Type *Sema::signatureForUtility(const TypeRef &ref) {
  if (ref.kind != TypeRef::Kind::TypeOf || ref.names.empty())
    return nullptr;
  // Each type parameter as its constraint, or unknown, as TypeScript
  // instantiates a generic signature here.
  auto erased = [&](FuncSymbol *fn) {
    const Type *type = functionType(fn);
    std::vector<const Type *> args;
    for (const Type *p : fn->typeParams) {
      if (p->constraint())
        args.push_back(p->constraint());
      else if (!p->alternatives().empty())
        args.push_back(Type::unionOf(p->alternatives()));
      else if (p->isListParam())
        args.push_back(Type::array(Type::unknown()));
      else
        args.push_back(Type::unknown());
    }
    return substitute(type, fn->typeParams, args);
  };
  if (ref.names.size() == 1) {
    ScopeEntry entry = lookupValue(ref.names.front());
    if (entry.func && entry.func->isGeneric())
      return erased(entry.func);
    return nullptr;
  }
  // `typeof obj.method`: a method of the class of obj; of several, the last
  // one.
  if (ref.names.size() != 2)
    return nullptr;
  VarSymbol *var = lookupValue(ref.names.front()).var;
  if (!var || !var->type)
    return nullptr;
  const Type *object = var->type->nonOptional();
  if (const Overload *overload = object->isObject() ? object->overload(ref.names[1]) : nullptr)
    return overload->signatures.back();
  ClassDecl *cls = classOf(object);
  if (!cls || object->field(ref.names[1]))
    return nullptr;
  completeClass(cls);
  std::vector<FunctionDecl *> methods = methodsNamed(cls, ref.names[1], /*isStatic=*/false);
  if (methods.empty() || !methods.back()->sym)
    return nullptr;
  return erased(methods.back()->sym);
}

} // namespace tz
