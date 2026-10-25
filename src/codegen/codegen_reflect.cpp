// Reflection (docs/decorators.md, section 6): `new` of a class known only
// when the program runs, Reflect.get, Reflect.apply, Reflect.construct, the
// name of such a class, fromJson(cls, j), and the descriptions of the classes
// they reach.
//
// The fields and methods of an object of tz lie at places fixed when
// compiling, without names. A class that reflection reaches gets a
// description, TzClassInfo (runtime/tzrt.h), which the object of the class
// points to:
//
//   - construct: makes an object from arguments as unknowns, checking each
//     against a constructor (the first that takes them, of several);
//   - from_json: makes one from a Json through the constructor, as
//     fromJson(cls, j) does for a class known when compiling;
//   - the methods, as functions `(this: unknown, ...params) => R` whose code
//     checks the `this` it gets, as Reflect.get gives them.
//
// The fields need nothing: the layout of every object names them, for
// printing. A function value in an unknown is called through the invoker of
// its type, which checks the arguments against its parameters; the program
// gives the runtime those of the types that reach an unknown. A value that
// does not fit is a TypeError that names the parameter.

#include "codegen/codegen.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

// TZ_LAYOUT_CLASS and TZ_LAYOUT_DATE of runtime/tzrt.h.
constexpr uint32_t kLayoutClass = 2097152;
constexpr uint32_t kLayoutDate = 1024;

// A count no program reaches, as literals have (see codegen.cpp).
constexpr int64_t kStaticRefCount = int64_t(1) << 62;

// What a class is called in messages: its name, or what it extends.
std::string className(const ClassDecl *cls) {
  return cls->name.empty() && cls->type ? cls->type->name().str() : cls->name.str();
}

// The constructors `new` of a class chooses from: its own, or those of the
// nearest class up its chain that has any.
std::vector<FunctionDecl *> constructorsOf(const ClassDecl *cls) {
  for (const ClassDecl *c = cls; c; c = c->parent)
    if (!c->constructors.empty())
      return c->constructors;
  return {};
}

} // namespace

//===----------------------------------------------------------------------===//
// Unknowns as the values of parameters
//===----------------------------------------------------------------------===//

llvm::Value *CodeGen::classIdIn(llvm::Value *id, llvm::ArrayRef<const ClassDecl *> classes) {
  llvm::Value *any = builder_.getFalse();
  for (const ClassDecl *cls : classes) {
    unsigned last = std::max(cls->id, cls->lastId);
    llvm::Value *from = builder_.CreateSub(id, builder_.getInt32(cls->id));
    any = builder_.CreateOr(any, builder_.CreateICmpULE(from, builder_.getInt32(last - cls->id)));
  }
  return any;
}

std::vector<const ClassDecl *> CodeGen::classesOfType(const Type *type) {
  std::vector<const ClassDecl *> classes;
  if (const ClassDecl *cls = type->classDecl()) {
    classes.push_back(cls);
    return classes;
  }
  for (const ClassDecl *cls : emittedClasses_)
    if (cls->type && !cls->literal && cls->type->isSubtypeOf(type))
      classes.push_back(cls);
  return classes;
}

llvm::Value *CodeGen::emitUnknownAs(llvm::Value *u, const Type *to, llvm::BasicBlock *fail) {
  llvm::Type *ptr = builder_.getPtrTy(), *i32 = builder_.getInt32Ty();
  // Goes on where `ok` holds.
  auto test = [&](llvm::Value *ok) {
    llvm::BasicBlock *next = newBlock("reflect.fits");
    builder_.CreateCondBr(ok, next, fail);
    switchTo(next);
  };
  // tz_unknown_to_X(u, &out): whether it holds one, and the value.
  auto through = [&](const char *name, llvm::Type *out) {
    llvm::Value *slot = createStackSlot(out, "reflect.value");
    llvm::Value *ok = builder_.CreateCall(unknownFunction(name, i32, {ptr, ptr}), {u, slot});
    test(builder_.CreateICmpNE(ok, builder_.getInt32(0)));
    return builder_.CreateLoad(out, slot);
  };
  if (to->isUnknown()) {
    retainValue(u, to);
    return u;
  }
  if (to->isOptional()) {
    // Undefined is undefined; anything else must be a T.
    llvm::BasicBlock *entry = builder_.GetInsertBlock();
    llvm::BasicBlock *valueBlock = newBlock("reflect.optional");
    llvm::BasicBlock *endBlock = newBlock("reflect.optional.end");
    builder_.CreateCondBr(builder_.CreateIsNull(u), endBlock, valueBlock);
    switchTo(valueBlock);
    llvm::Value *value = makeOptional(emitUnknownAs(u, to->inner(), fail), builder_.getTrue(), to);
    llvm::BasicBlock *valueEnd = builder_.GetInsertBlock();
    builder_.CreateBr(endBlock);
    switchTo(endBlock);
    llvm::PHINode *phi = builder_.CreatePHI(lower(to), 2);
    phi->addIncoming(undefinedValue(to), entry);
    phi->addIncoming(value, valueEnd);
    return phi;
  }
  if (to->isUnion()) {
    // The first member that it holds.
    llvm::BasicBlock *endBlock = newBlock("reflect.union.end");
    std::vector<std::pair<llvm::Value *, llvm::BasicBlock *>> found;
    for (const Type *member : to->members()) {
      llvm::BasicBlock *next = newBlock("reflect.union.next");
      llvm::Value *value = emitConversion(emitUnknownAs(u, member, next), member, to);
      found.push_back({value, builder_.GetInsertBlock()});
      builder_.CreateBr(endBlock);
      switchTo(next);
    }
    builder_.CreateBr(fail);
    switchTo(endBlock);
    llvm::PHINode *phi = builder_.CreatePHI(lower(to), unsigned(found.size()));
    for (auto [value, block] : found)
      phi->addIncoming(value, block);
    return phi;
  }
  switch (to->kind()) {
  case Type::Kind::Int:
    return through("tz_unknown_to_int", i32);
  case Type::Kind::Long:
    return through("tz_unknown_to_long", builder_.getInt64Ty());
  case Type::Kind::Double:
    return through("tz_unknown_to_double", builder_.getDoubleTy());
  case Type::Kind::Bool:
    return builder_.CreateICmpNE(through("tz_unknown_to_bool", i32), builder_.getInt32(0));
  case Type::Kind::String:
    return through("tz_unknown_to_string", strTy_);
  case Type::Kind::Array:
  case Type::Kind::Function:
  case Type::Kind::Decimal:
  case Type::Kind::Result:
  case Type::Kind::Json: {
    llvm::Value *slot = createStackSlot(lower(to), "reflect.value");
    llvm::Value *ok = builder_.CreateCall(
        unknownFunction("tz_unknown_to_box", i32, {ptr, ptr, ptr}), {u, typeDescriptor(to), slot});
    test(builder_.CreateICmpNE(ok, builder_.getInt32(0)));
    return builder_.CreateLoad(lower(to), slot);
  }
  case Type::Kind::Object:
  case Type::Kind::Dict:
  case Type::Kind::ClassRef: {
    llvm::Value *layout = builder_.CreateCall(unknownFunction("tz_unknown_layout", ptr, {ptr}), {u},
                                              "layout");
    test(builder_.CreateIsNotNull(layout));
    auto loadLayout = [&](unsigned field) {
      return builder_.CreateLoad(i32, builder_.CreateStructGEP(layoutDescTy_, layout, field));
    };
    llvm::Value *flags = loadLayout(3);
    llvm::Value *isClass =
        builder_.CreateICmpNE(builder_.CreateAnd(flags, builder_.getInt32(kLayoutClass)),
                              builder_.getInt32(0));
    if (to->isClassRef()) {
      // A class itself: that one, or one whose objects are T.
      test(isClass);
      if (const ClassDecl *cls = to->classRef()) {
        test(builder_.CreateICmpEQ(u, classObject(cls)));
      } else if (!to->constructed()->isAnyObject()) {
        // TzClass: { rc, layout, parent, i32 id, ... }.
        llvm::Value *id = builder_.CreateLoad(
            i32, builder_.CreateConstInBoundsGEP1_64(builder_.getInt8Ty(), u, 24), "class");
        test(classIdIn(id, classesOfType(to->constructed())));
      }
      return u; // never counted
    }
    if (to->isDict()) {
      test(builder_.CreateICmpEQ(layout, dictLayout(to).descriptor));
    } else if (to->isDate()) {
      test(builder_.CreateICmpNE(builder_.CreateAnd(flags, builder_.getInt32(kLayoutDate)),
                                 builder_.getInt32(0)));
    } else if (!to->isAnyObject()) {
      std::vector<const ClassDecl *> classes = to->isClass() || to->isBase() || !to->bases().empty()
                                                   ? classesOfType(to)
                                                   : std::vector<const ClassDecl *>();
      if (classes.empty()) { // no check tells a literal type apart
        builder_.CreateBr(fail);
        startDeadBlock();
        return llvm::Constant::getNullValue(lower(to));
      }
      test(builder_.CreateNot(isClass));
      test(classIdIn(loadLayout(6), classes));
    }
    retainValue(u, to);
    return u;
  }
  default:
    builder_.CreateBr(fail);
    startDeadBlock();
    return llvm::Constant::getNullValue(lower(to));
  }
}

//===----------------------------------------------------------------------===//
// Choosing among variants when the program runs
//===----------------------------------------------------------------------===//

namespace {

// TZ_T_* of runtime/tzrt.h, and the kinds tz_unknown_kind gives.
enum : uint32_t { kBoxInt = 1, kBoxLong = 2, kBoxDouble = 3 };
enum : uint32_t { kKindString = 2, kKindBoolean = 3 };

// Whether a value of type `a` goes where one of `b` is without an explicit
// conversion, as a parameter of `a` is narrower than one of `b`: the rule a
// call that compiles prefers a variant by (docs/overloads.md, section 2).
bool narrower(const Type *a, const Type *b) {
  if (a == b || b->isUnknown())
    return true;
  if (b->isOptional())
    return a->isUndefined() || narrower(a->isOptional() ? a->inner() : a, b->inner());
  if (a->isOptional())
    return false;
  if (b->isUnion())
    return llvm::any_of(b->members(), [&](const Type *m) { return narrower(a, m); });
  if (a->isLiteral())
    return narrower(a->literalBase(), b);
  if (a->isInt())
    return b->kind() == Type::Kind::Long || b->isFloat() || b->isDecimal();
  if (a->kind() == Type::Kind::Long)
    return b->isFloat() || b->isDecimal();
  if (a->isObject() && b->isObject())
    return b->isAnyObject() || a->isSubtypeOf(b);
  return false;
}

} // namespace

llvm::Value *CodeGen::emitUnknownFit(llvm::Value *u, const Type *to) {
  llvm::Type *ptr = builder_.getPtrTy(), *i32 = builder_.getInt32Ty();
  auto level = [&](uint32_t n) { return builder_.getInt32(n); };
  auto pick = [&](llvm::Value *cond, uint32_t yes, llvm::Value *no) {
    return builder_.CreateSelect(cond, level(yes), no);
  };
  if (to->isUnknown())
    return level(1);
  if (to->isOptional())
    return builder_.CreateSelect(builder_.CreateIsNull(u), level(3),
                                 emitUnknownFit(u, to->inner()));
  if (to->isUnion()) {
    // As well as the member it fits best.
    llvm::Value *best = level(0);
    for (const Type *member : to->members()) {
      llvm::Value *fit = emitUnknownFit(u, member);
      best = builder_.CreateSelect(builder_.CreateICmpUGT(fit, best), fit, best);
    }
    return best;
  }
  if (to->isLiteral())
    return level(0); // no check tells one value of a type apart
  auto boxKind = [&] {
    return builder_.CreateCall(unknownFunction("tz_unknown_box_kind", i32, {ptr}), {u});
  };
  auto kind = [&] {
    return builder_.CreateCall(unknownFunction("tz_unknown_kind", i32, {ptr}), {u});
  };
  // Whether tz_unknown_to_X takes it, for a number.
  auto takes = [&](const char *name, llvm::Type *out) {
    llvm::Value *slot = createStackSlot(out, "fit.value");
    llvm::Value *ok = builder_.CreateCall(unknownFunction(name, i32, {ptr, ptr}), {u, slot});
    return builder_.CreateICmpNE(ok, builder_.getInt32(0));
  };
  switch (to->kind()) {
  case Type::Kind::Int: {
    llvm::Value *box = boxKind();
    return pick(builder_.CreateICmpEQ(box, level(kBoxInt)), 3,
                pick(takes("tz_unknown_to_int", i32), 1, level(0)));
  }
  case Type::Kind::Long: {
    llvm::Value *box = boxKind();
    return pick(builder_.CreateICmpEQ(box, level(kBoxLong)), 3,
                pick(builder_.CreateICmpEQ(box, level(kBoxInt)), 2,
                     pick(takes("tz_unknown_to_long", builder_.getInt64Ty()), 1, level(0))));
  }
  case Type::Kind::Double: {
    llvm::Value *box = boxKind();
    llvm::Value *integer = builder_.CreateOr(builder_.CreateICmpEQ(box, level(kBoxInt)),
                                             builder_.CreateICmpEQ(box, level(kBoxLong)));
    return pick(builder_.CreateICmpEQ(box, level(kBoxDouble)), 3,
                pick(integer, 2, pick(takes("tz_unknown_to_double", builder_.getDoubleTy()), 1,
                                      level(0))));
  }
  case Type::Kind::Bool:
    return pick(builder_.CreateICmpEQ(kind(), level(kKindBoolean)), 3, level(0));
  case Type::Kind::String:
    return pick(builder_.CreateICmpEQ(kind(), level(kKindString)), 3, level(0));
  case Type::Kind::Array:
  case Type::Kind::Function:
  case Type::Kind::Decimal:
  case Type::Kind::Result:
  case Type::Kind::Json: {
    llvm::Value *holds = builder_.CreateCall(
        unknownFunction("tz_unknown_holds", i32, {ptr, ptr}), {u, typeDescriptor(to)});
    return pick(builder_.CreateICmpNE(holds, level(0)), 3, level(0));
  }
  case Type::Kind::Object:
  case Type::Kind::Dict:
  case Type::Kind::ClassRef:
    break;
  default:
    return level(0);
  }
  // An object of the language: its layout tells, as for emitUnknownAs.
  llvm::Value *layout =
      builder_.CreateCall(unknownFunction("tz_unknown_layout", ptr, {ptr}), {u}, "layout");
  llvm::BasicBlock *entry = builder_.GetInsertBlock();
  llvm::BasicBlock *objectBlock = newBlock("fit.object");
  llvm::BasicBlock *endBlock = newBlock("fit.end");
  builder_.CreateCondBr(builder_.CreateIsNotNull(layout), objectBlock, endBlock);
  switchTo(objectBlock);
  auto loadLayout = [&](unsigned field) {
    return builder_.CreateLoad(i32, builder_.CreateStructGEP(layoutDescTy_, layout, field));
  };
  llvm::Value *flags = loadLayout(3);
  llvm::Value *isClass = builder_.CreateICmpNE(
      builder_.CreateAnd(flags, builder_.getInt32(kLayoutClass)), builder_.getInt32(0));
  llvm::Value *fit = level(0);
  if (to->isClassRef()) {
    if (const ClassDecl *cls = to->classRef()) {
      fit = pick(builder_.CreateICmpEQ(u, classObject(cls)), 3, level(0));
    } else if (to->constructed()->isAnyObject()) {
      fit = pick(isClass, 2, level(0));
    } else {
      // TzClass: { rc, layout, parent, i32 id, ... }; read only of a class.
      llvm::Value *at = builder_.CreateSelect(
          isClass, builder_.CreateConstGEP1_64(builder_.getInt8Ty(), u, 24), layout);
      llvm::Value *id = builder_.CreateLoad(i32, at, "class");
      fit = pick(builder_.CreateAnd(isClass, classIdIn(id, classesOfType(to->constructed()))), 2,
                 level(0));
    }
  } else if (to->isDict()) {
    fit = pick(builder_.CreateICmpEQ(layout, dictLayout(to).descriptor), 3, level(0));
  } else if (to->isDate()) {
    fit = pick(builder_.CreateICmpNE(builder_.CreateAnd(flags, builder_.getInt32(kLayoutDate)),
                                     builder_.getInt32(0)),
               3, level(0));
  } else if (to->isAnyObject()) {
    fit = level(1);
  } else if (to->isClass() || to->isBase() || !to->bases().empty()) {
    std::vector<const ClassDecl *> classes = classesOfType(to);
    if (!classes.empty()) {
      llvm::Value *id = loadLayout(6);
      llvm::Value *in = builder_.CreateAnd(builder_.CreateNot(isClass), classIdIn(id, classes));
      llvm::Value *exact = builder_.getFalse();
      if (const ClassDecl *cls = to->classDecl())
        exact = builder_.CreateICmpEQ(id, builder_.getInt32(cls->id));
      fit = pick(builder_.CreateAnd(in, exact), 3, pick(in, 2, level(0)));
    }
  }
  llvm::BasicBlock *objectEnd = builder_.GetInsertBlock();
  builder_.CreateBr(endBlock);
  switchTo(endBlock);
  llvm::PHINode *phi = builder_.CreatePHI(i32, 2, "fit");
  phi->addIncoming(level(0), entry);
  phi->addIncoming(fit, objectEnd);
  return phi;
}

llvm::Value *CodeGen::emitChooseVariant(llvm::ArrayRef<const FunctionDecl *> variants,
                                        llvm::Value *selfId, llvm::Value *args,
                                        llvm::Value *count) {
  llvm::Type *ptr = builder_.getPtrTy(), *i32 = builder_.getInt32Ty();
  // args[i], or undefined past the end, read from a slot that holds it then.
  llvm::Value *missing = createStackSlot(ptr, "variant.missing");
  builder_.CreateStore(llvm::ConstantPointerNull::get(builder_.getPtrTy()), missing);
  auto given = [&](size_t i) {
    return builder_.CreateICmpULT(builder_.getInt32(uint32_t(i)), count);
  };
  auto argAt = [&](size_t i) {
    llvm::Value *at = builder_.CreateGEP(ptr, args, builder_.getInt64(i));
    return builder_.CreateLoad(ptr, builder_.CreateSelect(given(i), at, missing), "arg");
  };
  struct Variant {
    const FuncSymbol *sym;
    size_t fixed;
    llvm::Value *ok;
    // How the arguments fit, up to the most parameters a variant has before
    // a rest: of a rest parameter, its element.
    std::vector<llvm::Value *> fits;
    const Type *param(size_t i) const {
      if (i < fixed)
        return sym->params[i];
      return sym->hasRest ? sym->params.back()->inner() : nullptr;
    }
  };
  size_t compared = 0;
  for (const FunctionDecl *fn : variants)
    compared = std::max(compared, fn->sym->params.size() - (fn->sym->hasRest ? 1 : 0));
  std::vector<Variant> all;
  for (const FunctionDecl *fn : variants) {
    const FuncSymbol *sym = fn->sym;
    Variant v{sym, sym->hasRest ? sym->params.size() - 1 : sym->params.size(), nullptr, {}};
    llvm::Value *ok = builder_.CreateICmpUGE(count, builder_.getInt32(uint32_t(sym->minArgs)));
    if (!sym->hasRest)
      ok = builder_.CreateAnd(ok,
                              builder_.CreateICmpULE(count, builder_.getInt32(uint32_t(v.fixed))));
    if (selfId)
      ok = builder_.CreateAnd(ok, classIdIn(selfId, {fn->cls}));
    for (size_t i = 0; i < compared; ++i) {
      const Type *param = v.param(i);
      if (!param) {
        v.fits.push_back(nullptr);
        continue;
      }
      // Left out, a parameter that may be: no worse than given.
      llvm::Value *fit = builder_.CreateSelect(given(i), emitUnknownFit(argAt(i), param),
                                               builder_.getInt32(3));
      if (i < v.fixed)
        ok = builder_.CreateAnd(ok, builder_.CreateICmpNE(fit, builder_.getInt32(0)));
      v.fits.push_back(fit);
    }
    if (sym->hasRest) {
      // Each of the rest fits the element.
      const Type *element = sym->params.back()->inner();
      llvm::BasicBlock *entry = builder_.GetInsertBlock();
      llvm::BasicBlock *loopBlock = newBlock("variant.rest");
      llvm::BasicBlock *bodyBlock = newBlock("variant.rest.body");
      llvm::BasicBlock *doneBlock = newBlock("variant.rest.done");
      builder_.CreateBr(loopBlock);
      switchTo(loopBlock);
      llvm::PHINode *i = builder_.CreatePHI(i32, 2, "i");
      llvm::PHINode *fits = builder_.CreatePHI(builder_.getInt1Ty(), 2, "fits");
      i->addIncoming(builder_.getInt32(uint32_t(v.fixed)), entry);
      fits->addIncoming(builder_.getTrue(), entry);
      builder_.CreateCondBr(builder_.CreateICmpULT(i, count), bodyBlock, doneBlock);
      switchTo(bodyBlock);
      llvm::Value *at = builder_.CreateZExt(i, builder_.getInt64Ty());
      llvm::Value *u = builder_.CreateLoad(ptr, builder_.CreateInBoundsGEP(ptr, args, at));
      llvm::Value *fit = emitUnknownFit(u, element);
      i->addIncoming(builder_.CreateAdd(i, builder_.getInt32(1)), builder_.GetInsertBlock());
      fits->addIncoming(builder_.CreateAnd(fits, builder_.CreateICmpNE(fit, builder_.getInt32(0))),
                        builder_.GetInsertBlock());
      builder_.CreateBr(loopBlock);
      switchTo(doneBlock);
      ok = builder_.CreateAnd(ok, fits);
    }
    v.ok = ok;
    all.push_back(std::move(v));
  }
  // Whether `a` fits better than `b`: each argument no worse, one better;
  // or the same, and `a` takes no rest where `b` does.
  auto better = [&](const Variant &a, const Variant &b) {
    llvm::Value *noWorse = builder_.getTrue(), *anyBetter = builder_.getFalse(),
                *anyWorse = builder_.getFalse();
    for (size_t i = 0; i < compared; ++i) {
      const Type *pa = a.param(i), *pb = b.param(i);
      if (!pa || !pb)
        continue; // one of them takes no argument there: it fits no such call
      bool toB = narrower(pa, pb), toA = narrower(pb, pa);
      llvm::Value *fa = a.fits[i], *fb = b.fits[i];
      llvm::Value *same = builder_.CreateICmpEQ(fa, fb);
      llvm::Value *higher = builder_.CreateICmpUGT(fa, fb), *lower = builder_.CreateICmpULT(fa, fb);
      llvm::Value *ge = toB ? builder_.CreateOr(higher, same) : higher;
      llvm::Value *gt = toB && !toA ? builder_.CreateOr(higher, same) : higher;
      llvm::Value *lt = toA && !toB ? builder_.CreateOr(lower, same) : lower;
      llvm::Value *in = given(i);
      noWorse = builder_.CreateAnd(noWorse, builder_.CreateOr(builder_.CreateNot(in), ge));
      anyBetter = builder_.CreateOr(anyBetter, builder_.CreateAnd(in, gt));
      anyWorse = builder_.CreateOr(anyWorse, builder_.CreateAnd(in, lt));
    }
    llvm::Value *plainer = builder_.getInt1(!a.sym->hasRest && b.sym->hasRest);
    return builder_.CreateAnd(
        noWorse, builder_.CreateOr(anyBetter,
                                   builder_.CreateAnd(builder_.CreateNot(anyWorse), plainer)));
  };
  llvm::Value *anyOk = builder_.getFalse();
  for (const Variant &v : all)
    anyOk = builder_.CreateOr(anyOk, v.ok);
  llvm::Value *chosen = builder_.CreateSelect(anyOk, builder_.getInt32(uint32_t(-2)),
                                              builder_.getInt32(uint32_t(-1)));
  for (size_t a = all.size(); a-- > 0;) {
    llvm::Value *best = all[a].ok;
    for (size_t b = 0; b < all.size(); ++b)
      if (b != a)
        best = builder_.CreateAnd(
            best, builder_.CreateOr(builder_.CreateNot(all[b].ok), better(all[a], all[b])));
    chosen = builder_.CreateSelect(best, builder_.getInt32(uint32_t(a)), chosen, "variant");
  }
  return chosen;
}

void CodeGen::emitReflectMismatch(llvm::StringRef what, llvm::Value *index, const Type *expected,
                                  llvm::Value *got) {
  llvm::Type *ptr = builder_.getPtrTy(), *i32 = builder_.getInt32Ty();
  llvm::StringRef name = expected->name();
  builder_.CreateCall(unknownFunction("tz_reflect_mismatch", builder_.getVoidTy(),
                                      {ptr, i32, i32, ptr, i32, ptr}),
                      {utf8Constant(what), builder_.getInt32(uint32_t(what.size())),
                       index, utf8Constant(name),
                       builder_.getInt32(uint32_t(name.size())), got});
}

// The arguments `args[from..count)` as the parameters `params` take them,
// the rest parameter last (of `rest`, an array type, or null), in `values`;
// on a mismatch what was converted is released and control goes to the
// block `onFail` gives for the argument, with what it holds. `min` arguments
// are surely there.
void CodeGen::emitReflectArgs(llvm::Value *args, llvm::Value *count, size_t min,
                              llvm::ArrayRef<const Type *> params, const Type *rest,
                              std::vector<llvm::Value *> &values,
                              llvm::function_ref<void(llvm::Value *, const Type *, llvm::Value *)>
                                  onFail) {
  llvm::Type *ptr = builder_.getPtrTy(), *i64 = builder_.getInt64Ty();
  size_t before = values.size();
  auto releaseMade = [&](size_t upTo) {
    for (size_t k = before; k < upTo; ++k)
      releaseValue(values[k], params[k - before]);
  };
  auto argAt = [&](llvm::Value *i) {
    return builder_.CreateLoad(ptr, builder_.CreateInBoundsGEP(ptr, args, i), "arg");
  };
  for (size_t i = 0; i < params.size(); ++i) {
    llvm::Value *u;
    if (i < min) {
      u = argAt(builder_.getInt64(i));
    } else {
      // Left out: undefined.
      llvm::BasicBlock *entry = builder_.GetInsertBlock();
      llvm::BasicBlock *givenBlock = newBlock("reflect.given");
      llvm::BasicBlock *endBlock = newBlock("reflect.arg");
      builder_.CreateCondBr(builder_.CreateICmpULT(builder_.getInt32(uint32_t(i)), count),
                            givenBlock, endBlock);
      switchTo(givenBlock);
      llvm::Value *given = argAt(builder_.getInt64(i));
      llvm::BasicBlock *givenEnd = builder_.GetInsertBlock();
      builder_.CreateBr(endBlock);
      switchTo(endBlock);
      llvm::PHINode *phi = builder_.CreatePHI(ptr, 2);
      phi->addIncoming(llvm::ConstantPointerNull::get(builder_.getPtrTy()), entry);
      phi->addIncoming(given, givenEnd);
      u = phi;
    }
    llvm::BasicBlock *failBlock = newBlock("reflect.mismatch");
    values.push_back(emitUnknownAs(u, params[i], failBlock));
    llvm::IRBuilderBase::InsertPoint point = builder_.saveIP();
    switchTo(failBlock);
    releaseMade(before + i);
    onFail(builder_.getInt32(uint32_t(i)), params[i], u);
    builder_.restoreIP(point);
  }
  if (!rest)
    return;
  // The rest, as an array of its elements.
  const Type *element = rest->inner();
  llvm::Value *from = builder_.getInt32(uint32_t(params.size()));
  llvm::Value *more = builder_.CreateSelect(builder_.CreateICmpUGT(count, from),
                                            builder_.CreateSub(count, from), builder_.getInt32(0));
  llvm::Value *array =
      builder_.CreateCall(rt_.arrNew, {typeDescriptor(element), builder_.getInt64(0),
                                       builder_.CreateZExt(more, i64)},
                          "rest");
  llvm::BasicBlock *entry = builder_.GetInsertBlock();
  llvm::BasicBlock *loopBlock = newBlock("reflect.rest");
  llvm::BasicBlock *bodyBlock = newBlock("reflect.rest.body");
  llvm::BasicBlock *doneBlock = newBlock("reflect.rest.done");
  builder_.CreateBr(loopBlock);
  switchTo(loopBlock);
  llvm::PHINode *i = builder_.CreatePHI(builder_.getInt32Ty(), 2, "i");
  i->addIncoming(from, entry);
  builder_.CreateCondBr(builder_.CreateICmpULT(i, count), bodyBlock, doneBlock);
  switchTo(bodyBlock);
  llvm::Value *u = argAt(builder_.CreateZExt(i, i64));
  llvm::BasicBlock *failBlock = newBlock("reflect.mismatch");
  llvm::Value *value = emitUnknownAs(u, element, failBlock);
  emitPush(array, {value}, element);
  i->addIncoming(builder_.CreateAdd(i, builder_.getInt32(1)), builder_.GetInsertBlock());
  builder_.CreateBr(loopBlock);
  llvm::IRBuilderBase::InsertPoint point = builder_.saveIP();
  switchTo(failBlock);
  releaseValue(array, rest);
  releaseMade(before + params.size());
  onFail(i, element, u);
  builder_.restoreIP(point);
  switchTo(doneBlock);
  values.push_back(array);
}

//===----------------------------------------------------------------------===//
// The descriptions of classes
//===----------------------------------------------------------------------===//

llvm::StructType *CodeGen::classInfoType() {
  // TzClassInfo: { ptr construct, ptr from_json, ptr methods, i32 count }.
  if (!classInfoTy_) {
    llvm::Type *ptr = builder_.getPtrTy();
    classInfoTy_ =
        llvm::StructType::create(ctx_, {ptr, ptr, ptr, builder_.getInt32Ty()}, "tz.classinfo");
  }
  return classInfoTy_;
}

llvm::Constant *CodeGen::classInfoGlobal(const ClassDecl *cls) {
  llvm::GlobalVariable *&global = classInfos_[cls];
  if (!global) {
    // Filled at the end (finishReflection), once the program is emitted.
    global = new llvm::GlobalVariable(*module_, classInfoType(), /*isConstant=*/true,
                                      llvm::GlobalValue::PrivateLinkage,
                                      llvm::Constant::getNullValue(classInfoType()),
                                      "tz.classinfo." + symbolPrefixes_.lookup(cls) +
                                          className(cls));
  }
  return global;
}

// (ptr args, i32 count) -> ptr: a new object of the class from arguments as
// unknowns, through its constructor (a mismatch names the argument); of
// several, through the variant the arguments fit best, as for a method
// (docs/overloads.md, section 6). NULL after throwing.
void CodeGen::emitConstructThunk(const ClassDecl *cls, llvm::Function *fn) {
  llvm::Type *ptr = builder_.getPtrTy(), *i32 = builder_.getInt32Ty();
  beginFunction(fn, nullptr);
  loc_ = cls->loc();
  pushScope();
  llvm::Value *args = fn->getArg(0), *count = fn->getArg(1);
  std::string what = "new " + className(cls);
  llvm::Value *null = llvm::ConstantPointerNull::get(builder_.getPtrTy());
  auto fail = [&] {
    emitCleanups(0);
    builder_.CreateRet(null);
  };
  std::vector<FunctionDecl *> ctors = constructorsOf(cls);
  bool precise = ctors.size() <= 1;
  llvm::BasicBlock *none = precise ? nullptr : newBlock("reflect.none");
  llvm::BasicBlock *several = precise ? nullptr : newBlock("reflect.several");
  // A class without a constructor takes no arguments.
  std::vector<const FuncSymbol *> variants;
  for (FunctionDecl *ctor : ctors)
    variants.push_back(ctor->sym);
  if (variants.empty())
    variants.push_back(nullptr);
  // Of several, the one the arguments fit best.
  std::vector<llvm::BasicBlock *> starts;
  if (!precise) {
    std::vector<const FunctionDecl *> choices(ctors.begin(), ctors.end());
    llvm::Value *chosen = emitChooseVariant(choices, nullptr, args, count);
    llvm::SwitchInst *which = builder_.CreateSwitch(chosen, none, unsigned(ctors.size() + 1));
    which->addCase(builder_.getInt32(uint32_t(-2)), several);
    for (size_t v = 0; v < ctors.size(); ++v) {
      starts.push_back(newBlock("reflect.variant"));
      which->addCase(builder_.getInt32(uint32_t(v)), starts.back());
    }
  }
  for (size_t v = 0; v < variants.size(); ++v) {
    const FuncSymbol *sym = variants[v];
    if (!precise)
      switchTo(starts[v]);
    llvm::BasicBlock *next = none; // which the choice rules out
    size_t min = sym ? sym->minArgs : 0;
    bool rest = sym && sym->hasRest;
    std::vector<const Type *> params = sym ? sym->params : std::vector<const Type *>();
    const Type *restType = rest ? params.back() : nullptr;
    if (rest)
      params.pop_back();
    // How many it takes.
    llvm::Value *fits = builder_.CreateICmpUGE(count, builder_.getInt32(uint32_t(min)));
    if (!rest)
      fits = builder_.CreateAnd(
          fits, builder_.CreateICmpULE(count, builder_.getInt32(uint32_t(params.size()))));
    llvm::BasicBlock *arityOk = newBlock("reflect.arity");
    llvm::BasicBlock *arityWrong = newBlock("reflect.arity.wrong");
    builder_.CreateCondBr(fits, arityOk, arityWrong);
    switchTo(arityWrong);
    if (precise) {
      builder_.CreateCall(unknownFunction("tz_reflect_arity", builder_.getVoidTy(),
                                          {ptr, i32, i32, i32, i32}),
                          {utf8Constant(what), builder_.getInt32(uint32_t(what.size())),
                           builder_.getInt32(uint32_t(min)),
                           builder_.getInt32(rest ? -1 : int32_t(params.size())), count});
      fail();
    } else {
      builder_.CreateBr(next);
    }
    switchTo(arityOk);
    std::vector<llvm::Value *> values;
    emitReflectArgs(args, count, min, params, restType, values,
                    [&](llvm::Value *i, const Type *expected, llvm::Value *got) {
                      if (!precise) {
                        builder_.CreateBr(next);
                        return;
                      }
                      emitReflectMismatch(what, i, expected, got);
                      fail();
                    });
    llvm::Value *object = allocInstance(cls);
    Held held = hold(object, cls->type, constructionThrows(cls, sym));
    emitConstruct(object, cls, sym, values);
    llvm::Value *made = take(held);
    emitCleanups(0);
    builder_.CreateRet(made);
  }
  auto throwText = [&](const std::string &message) {
    llvm::Value *text = stringLiteral(std::u16string(message.begin(), message.end()));
    builder_.CreateCall(unknownFunction("tz_throw_str", builder_.getVoidTy(),
                                        {i32, ptr, builder_.getInt64Ty()}),
                        {builder_.getInt32(1), stringObj(text), stringMeta(text)});
    fail();
  };
  if (none) {
    switchTo(none);
    throwText(what + ": no constructor of " + className(cls) + " takes these arguments");
    switchTo(several);
    throwText(what + ": the arguments fit several constructors of " + className(cls) +
              " equally well");
  }
  startDeadBlock();
  popScope();
  finishFunction();
}

// The method as a function value that takes its `this`: `(env, this:
// unknown, params...) -> R`, which checks that the `this` is an object of the
// method's class (TypeError otherwise) and calls the method with it. The
// callee owns its `this`, as a function with a `this` parameter does.
llvm::Constant *CodeGen::reflectAdapterInfo(const FunctionDecl *fn, const Type *&type) {
  const FuncSymbol *sym = fn->sym;
  std::vector<const Type *> params{Type::unknown()};
  params.insert(params.end(), sym->params.begin(), sym->params.end());
  type = Type::function(params, sym->minArgs + 1, sym->hasRest, sym->ret, /*hasThis=*/true);
  if (llvm::Constant *found = reflectAdapters_.lookup(fn))
    return found;
  llvm::Function *method = methodCode(fn);
  std::string name = method->getName().str();
  auto *adapter = llvm::Function::Create(closureFnType(type), llvm::GlobalValue::InternalLinkage,
                                         name + ".reflect", module_.get());
  setFunctionAttributes(adapter);
  llvm::Constant *info = llvm::ConstantStruct::get(
      fnInfoTy_, {adapter, stringLiteral(std::u16string(fn->name.begin(), fn->name.end()))});
  auto *global = new llvm::GlobalVariable(*module_, fnInfoTy_, /*isConstant=*/true,
                                          llvm::GlobalValue::PrivateLinkage, info,
                                          name + ".reflect.info");
  reflectAdapters_[fn] = global;

  llvm::IRBuilderBase::InsertPointGuard guard(builder_);
  llvm::Function *outer = std::exchange(function_, adapter);
  builder_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", adapter));
  llvm::Type *ptr = builder_.getPtrTy();
  llvm::Value *self = adapter->getArg(1);
  const Type *ret = sym->ret;
  auto finish = [&](llvm::Value *result) {
    builder_.CreateCall(instanceReleaseFn_, {self});
    if (ret->isVoid())
      builder_.CreateRetVoid();
    else
      builder_.CreateRet(result);
  };
  const ClassDecl *cls = fn->cls;
  llvm::BasicBlock *failBlock = llvm::BasicBlock::Create(ctx_, "this.wrong", adapter);
  llvm::BasicBlock *testBlock = llvm::BasicBlock::Create(ctx_, "this.test", adapter);
  llvm::BasicBlock *callBlock = llvm::BasicBlock::Create(ctx_, "this.ok", adapter);
  llvm::Value *layout = builder_.CreateCall(unknownFunction("tz_unknown_layout", ptr, {ptr}),
                                            {self}, "layout");
  builder_.CreateCondBr(builder_.CreateIsNotNull(layout), testBlock, failBlock);
  builder_.SetInsertPoint(testBlock);
  llvm::Value *id = builder_.CreateLoad(
      builder_.getInt32Ty(), builder_.CreateStructGEP(layoutDescTy_, layout, 6), "class");
  unsigned last = std::max(cls->id, cls->lastId);
  llvm::Value *matches = builder_.CreateICmpULE(builder_.CreateSub(id, builder_.getInt32(cls->id)),
                                                builder_.getInt32(last - cls->id));
  builder_.CreateCondBr(matches, callBlock, failBlock);
  builder_.SetInsertPoint(failBlock);
  std::string message = className(cls) + "." + fn->name.str() +
                        " was called with a 'this' that is not an instance of " + className(cls);
  llvm::Value *text = stringLiteral(std::u16string(message.begin(), message.end()));
  builder_.CreateCall(unknownFunction("tz_throw_str", builder_.getVoidTy(),
                                      {builder_.getInt32Ty(), ptr, builder_.getInt64Ty()}),
                      {builder_.getInt32(1), stringObj(text), stringMeta(text)});
  for (size_t i = 1; i < params.size(); ++i)
    releaseValue(adapter->getArg(unsigned(i + 1)), params[i]);
  finish(ret->isVoid() ? nullptr : llvm::Constant::getNullValue(lower(ret)));
  builder_.SetInsertPoint(callBlock);
  std::vector<llvm::Value *> args{self}; // borrowed, as a method takes its object
  for (unsigned i = 2; i < adapter->arg_size(); ++i)
    args.push_back(adapter->getArg(i));
  llvm::CallInst *result = builder_.CreateCall(method, args);
  finish(ret->isVoid() ? nullptr : result);
  function_ = outer;
  return global;
}

std::vector<const FunctionDecl *> CodeGen::reflectVariants(const ClassDecl *cls,
                                                           llvm::StringRef name) {
  auto overrides = [](const FunctionDecl *fn, const FunctionDecl *base) {
    for (const FunctionDecl *p = fn; p; p = p->overrides)
      if (p == base)
        return true;
    return false;
  };
  std::vector<const FunctionDecl *> found;
  for (const ClassDecl *c = cls; c; c = c->parent) {
    std::vector<const FunctionDecl *> own(c->methods.begin(), c->methods.end());
    for (const ClassField &f : c->fields)
      if (f.method)
        own.push_back(f.method);
    for (const FunctionDecl *fn : own)
      if (fn->name == name && !fn->isStatic && !fn->isAbstract && fn->typeParams.empty() &&
          fn->sym && fn->sym->ret &&
          llvm::none_of(found, [&](const FunctionDecl *v) { return overrides(v, fn); }))
        found.push_back(fn);
  }
  return found;
}

// (env, this: unknown, args: unknown[]) -> unknown: checks that the `this`
// is an object of the class that declares the variants highest up its
// chain, chooses the variant the arguments fit best (or throws a TypeError:
// none fits, or several equally well), and calls it. The callee owns its
// `this` and its arguments, as a function with a `this` parameter does.
llvm::Constant *CodeGen::reflectDispatchInfo(const ClassDecl *cls,
                                             llvm::ArrayRef<const FunctionDecl *> variants,
                                             const Type *&type) {
  const Type *unknown = Type::unknown(), *list = Type::array(unknown);
  type = Type::function({unknown, list}, 1, /*hasRest=*/true, unknown, /*hasThis=*/true);
  llvm::StringRef name = variants.front()->name;
  llvm::Constant *&found = reflectDispatchers_[{cls, name.str()}];
  if (found)
    return found;
  std::string base = "tz." + symbolPrefixes_.lookup(cls) + className(cls) + "." + name.str();
  auto *dispatch = llvm::Function::Create(closureFnType(type), llvm::GlobalValue::InternalLinkage,
                                          base + ".dispatch", module_.get());
  setFunctionAttributes(dispatch);
  llvm::Constant *info = llvm::ConstantStruct::get(
      fnInfoTy_, {dispatch, stringLiteral(std::u16string(name.begin(), name.end()))});
  found = new llvm::GlobalVariable(*module_, fnInfoTy_, /*isConstant=*/true,
                                   llvm::GlobalValue::PrivateLinkage, info,
                                   base + ".dispatch.info");

  llvm::IRBuilderBase::InsertPointGuard guard(builder_);
  llvm::Function *outer = std::exchange(function_, dispatch);
  builder_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", dispatch));
  llvm::Type *ptr = builder_.getPtrTy(), *i32 = builder_.getInt32Ty();
  llvm::Value *null = llvm::ConstantPointerNull::get(builder_.getPtrTy());
  llvm::Value *self = dispatch->getArg(1), *array = dispatch->getArg(2);
  auto release = [&] {
    releaseValue(array, list);
    releaseValue(self, unknown);
  };
  auto fail = [&](const std::string &message) {
    llvm::Value *text = stringLiteral(std::u16string(message.begin(), message.end()));
    builder_.CreateCall(unknownFunction("tz_throw_str", builder_.getVoidTy(),
                                        {i32, ptr, builder_.getInt64Ty()}),
                        {builder_.getInt32(1), stringObj(text), stringMeta(text)});
    release();
    builder_.CreateRet(null);
  };
  const ClassDecl *top = nullptr;
  for (const FunctionDecl *fn : variants)
    if (!top || top->derivesFrom(fn->cls))
      top = fn->cls;
  std::string what = className(cls) + "." + name.str();
  // The `this`.
  llvm::BasicBlock *wrongBlock = newBlock("this.wrong");
  llvm::BasicBlock *testBlock = newBlock("this.test");
  llvm::BasicBlock *okBlock = newBlock("this.ok");
  llvm::Value *layout =
      builder_.CreateCall(unknownFunction("tz_unknown_layout", ptr, {ptr}), {self}, "layout");
  builder_.CreateCondBr(builder_.CreateIsNotNull(layout), testBlock, wrongBlock);
  switchTo(testBlock);
  llvm::Value *id =
      builder_.CreateLoad(i32, builder_.CreateStructGEP(layoutDescTy_, layout, 6), "class");
  builder_.CreateCondBr(classIdIn(id, {top}), okBlock, wrongBlock);
  switchTo(wrongBlock);
  fail(className(top) + "." + name.str() + " was called with a 'this' that is not an instance of " +
       className(top));
  // The variant.
  switchTo(okBlock);
  llvm::Value *count = builder_.CreateTrunc(arrayLength(array), i32, "count");
  llvm::Value *args = arrayData(array);
  llvm::Value *chosen = emitChooseVariant(variants, id, args, count);
  llvm::BasicBlock *noneBlock = newBlock("variant.none");
  llvm::BasicBlock *severalBlock = newBlock("variant.several");
  llvm::SwitchInst *which = builder_.CreateSwitch(chosen, noneBlock, unsigned(variants.size() + 1));
  which->addCase(builder_.getInt32(uint32_t(-2)), severalBlock);
  for (size_t v = 0; v < variants.size(); ++v) {
    const FuncSymbol *sym = variants[v]->sym;
    llvm::BasicBlock *callBlock = newBlock("variant.call");
    which->addCase(builder_.getInt32(uint32_t(v)), callBlock);
    switchTo(callBlock);
    std::vector<const Type *> params = sym->params;
    const Type *rest = sym->hasRest ? params.back() : nullptr;
    if (rest)
      params.pop_back();
    std::vector<llvm::Value *> values{self}; // borrowed, as a method takes its object
    emitReflectArgs(args, count, sym->minArgs, params, rest, values,
                    [&](llvm::Value *, const Type *, llvm::Value *) {
                      fail("Reflect.apply: no variant of " + what + " takes these arguments");
                    });
    llvm::Value *result = builder_.CreateCall(methodCode(variants[v]), values);
    llvm::BasicBlock *thrownBlock = newBlock("variant.thrown");
    llvm::BasicBlock *doneBlock = newBlock("variant.done");
    builder_.CreateCondBr(builder_.CreateIsNotNull(builder_.CreateLoad(ptr, thrownSlot())),
                          thrownBlock, doneBlock,
                          llvm::MDBuilder(ctx_).createUnlikelyBranchWeights());
    switchTo(thrownBlock);
    release();
    builder_.CreateRet(null);
    switchTo(doneBlock);
    llvm::Value *value =
        sym->ret->isVoid() ? null : emitConversion(result, sym->ret, Type::unknown());
    release();
    builder_.CreateRet(value);
  }
  switchTo(noneBlock);
  fail("Reflect.apply: no variant of " + what + " takes these arguments");
  switchTo(severalBlock);
  fail("Reflect.apply: the arguments fit several variants of " + what + " equally well");
  function_ = outer;
  return found;
}

// (ptr f, ptr self, ptr args, i32 count) -> ptr: calls the function value
// `*f` of `fnType` with arguments as unknowns (borrowed), and `self` for a
// `this` parameter; the result as an unknown, NULL after throwing.
llvm::Function *CodeGen::invokerFor(const Type *fnType) {
  llvm::Function *&fn = invokers_[fnType];
  if (fn)
    return fn;
  llvm::Type *ptr = builder_.getPtrTy(), *i32 = builder_.getInt32Ty();
  fn = llvm::Function::Create(llvm::FunctionType::get(ptr, {ptr, ptr, ptr, i32}, false),
                              llvm::GlobalValue::InternalLinkage, "tz.invoke", module_.get());
  setFunctionAttributes(fn);
  llvm::IRBuilderBase::InsertPointGuard guard(builder_);
  llvm::Function *outer = std::exchange(function_, fn);
  builder_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", fn));
  llvm::Value *null = llvm::ConstantPointerNull::get(builder_.getPtrTy());
  llvm::Value *value = builder_.CreateLoad(fnTy_, fn->getArg(0), "fn");
  llvm::Value *self = fn->getArg(1), *args = fn->getArg(2), *count = fn->getArg(3);
  size_t first = fnType->hasThis() ? 1 : 0;
  std::vector<const Type *> params(fnType->params().begin() + first, fnType->params().end());
  const Type *rest = nullptr;
  if (fnType->hasRest()) {
    rest = params.back();
    params.pop_back();
  }
  size_t min = fnType->minArgs() > first ? fnType->minArgs() - first : 0;
  const char what[] = "Reflect.apply";
  llvm::Value *fits = builder_.CreateICmpUGE(count, builder_.getInt32(uint32_t(min)));
  if (!rest)
    fits = builder_.CreateAnd(
        fits, builder_.CreateICmpULE(count, builder_.getInt32(uint32_t(params.size()))));
  llvm::BasicBlock *okBlock = newBlock("reflect.arity");
  llvm::BasicBlock *wrongBlock = newBlock("reflect.arity.wrong");
  builder_.CreateCondBr(fits, okBlock, wrongBlock);
  switchTo(wrongBlock);
  builder_.CreateCall(
      unknownFunction("tz_reflect_arity", builder_.getVoidTy(), {ptr, i32, i32, i32, i32}),
      {utf8Constant(what), builder_.getInt32(uint32_t(sizeof what - 1)),
       builder_.getInt32(uint32_t(min)), builder_.getInt32(rest ? -1 : int32_t(params.size())),
       count});
  builder_.CreateRet(null);
  switchTo(okBlock);
  std::vector<llvm::Value *> values;
  if (first) {
    retainValue(self, Type::unknown()); // the callee owns its `this`
    values.push_back(self);
  }
  emitReflectArgs(args, count, min, params, rest, values,
                  [&](llvm::Value *i, const Type *expected, llvm::Value *got) {
                    if (first)
                      releaseValue(self, Type::unknown());
                    emitReflectMismatch(what, i, expected, got);
                    builder_.CreateRet(null);
                  });
  llvm::Value *result = callFunctionValue(value, fnType, values);
  llvm::BasicBlock *thrownBlock = newBlock("reflect.thrown");
  llvm::BasicBlock *doneBlock = newBlock("reflect.done");
  builder_.CreateCondBr(builder_.CreateIsNotNull(builder_.CreateLoad(ptr, thrownSlot())),
                        thrownBlock, doneBlock,
                        llvm::MDBuilder(ctx_).createUnlikelyBranchWeights());
  switchTo(thrownBlock);
  builder_.CreateRet(null);
  switchTo(doneBlock);
  const Type *ret = fnType->ret();
  builder_.CreateRet(ret->isVoid() || !result ? null
                                              : emitConversion(result, ret, Type::unknown()));
  function_ = outer;
  return fn;
}

llvm::Function *CodeGen::classFromJsonFunction(const ClassDecl *cls) {
  llvm::Function *&fn = classConverters_[cls];
  if (!fn) {
    llvm::Type *ptr = builder_.getPtrTy();
    fn = llvm::Function::Create(llvm::FunctionType::get(ptr, {ptr, ptr}, false),
                                llvm::GlobalValue::InternalLinkage,
                                "tz.fromJson.class." + symbolPrefixes_.lookup(cls) + className(cls),
                                module_.get());
    setFunctionAttributes(fn);
    pendingConverters_.push_back(cls); // its body comes once the program is emitted
  }
  return fn;
}

// (ptr json, ptr path) -> ptr: the object the constructor makes from the
// keys of the Json named as its parameters, each checked as fromJson<T>
// checks a field; then the public fields it may write that the Json has.
void CodeGen::emitClassFromJson(const ClassDecl *cls, llvm::Function *fn) {
  llvm::Type *ptr = builder_.getPtrTy(), *i64 = builder_.getInt64Ty();
  beginFunction(fn, nullptr);
  loc_ = cls->loc();
  pushScope();
  if (!jsonPathTy_) // runtime/json.c: TzJsonPath
    jsonPathTy_ = llvm::StructType::create(ctx_, {ptr, ptr, i64, i64}, "tz.jsonpath");
  llvm::Value *json = fn->getArg(0), *path = fn->getArg(1);
  llvm::Value *null = llvm::ConstantPointerNull::get(builder_.getPtrTy());
  std::string name = className(cls);
  llvm::Value *link = createStackSlot(jsonPathTy_, "path");
  builder_.CreateStore(path, builder_.CreateStructGEP(jsonPathTy_, link, 0));
  llvm::Value *keyObj = builder_.CreateStructGEP(jsonPathTy_, link, 1);
  llvm::Value *keyMeta = builder_.CreateStructGEP(jsonPathTy_, link, 2);
  builder_.CreateStore(llvm::ConstantInt::getSigned(i64, -1),
                       builder_.CreateStructGEP(jsonPathTy_, link, 3));
  auto failIfThrown = [&](llvm::function_ref<void()> release) {
    llvm::BasicBlock *failBlock = newBlock("json.failed");
    llvm::BasicBlock *goBlock = newBlock("json.fits");
    builder_.CreateCondBr(builder_.CreateIsNotNull(builder_.CreateLoad(ptr, thrownSlot())),
                          failBlock, goBlock, llvm::MDBuilder(ctx_).createUnlikelyBranchWeights());
    switchTo(failBlock);
    release();
    emitCleanups(0);
    builder_.CreateRet(null);
    switchTo(goBlock);
  };
  builder_.CreateCall(
      jsonFunction("tz_json_expect_object", builder_.getVoidTy(), {ptr, ptr, ptr, i64}),
      {json, path, utf8Constant(name), builder_.getInt64(name.size())});
  failIfThrown([] {});
  auto keyed = [&](llvm::StringRef key) {
    llvm::Value *text = stringLiteral(std::u16string(key.begin(), key.end()));
    builder_.CreateStore(stringObj(text), keyObj);
    builder_.CreateStore(stringMeta(text), keyMeta);
    return builder_.CreateCall(jsonFunction("tz_json_field", ptr, {ptr, ptr, i64}),
                               {json, stringObj(text), stringMeta(text)}, key);
  };
  // The arguments: the keys named as the parameters of the constructor.
  std::vector<FunctionDecl *> ctors = constructorsOf(cls);
  const FunctionDecl *ctor = ctors.empty() ? nullptr : ctors.front();
  const FuncSymbol *sym = ctor ? ctor->sym : nullptr;
  std::vector<llvm::Value *> values;
  llvm::StringSet<> given;
  for (size_t k = 0; sym && k < sym->params.size() && k < cls->jsonParams.size(); ++k) {
    llvm::StringRef param = cls->jsonParams[k];
    given.insert(param);
    llvm::Value *item = keyed(param);
    values.push_back(emitFromJson(sym->params[k], item, link));
    failIfThrown([&] {
      for (size_t made = 0; made < k; ++made)
        releaseValue(values[made], sym->params[made]);
    });
  }
  llvm::Value *object = allocInstance(cls);
  Held held = hold(object, cls->type, true);
  emitConstruct(object, cls, sym, values);
  object = take(held);
  // Then the other fields that code may write, if the Json has them.
  for (const Field &f : cls->type->fields()) {
    if (given.contains(f.name) || !llvm::is_contained(cls->jsonFields, f.name))
      continue;
    llvm::Value *item = keyed(f.name);
    llvm::BasicBlock *setBlock = newBlock("json.field");
    llvm::BasicBlock *nextBlock = newBlock("json.field.next");
    builder_.CreateCondBr(builder_.CreateIsNotNull(item), setBlock, nextBlock);
    switchTo(setBlock);
    llvm::Value *value = emitFromJson(f.type, item, link);
    failIfThrown([&] { releaseValue(object, cls->type); });
    storeBarrier(object, value, f.type);
    storeReplacing(fieldAddress(object, cls->type, f.name), value, f.type);
    builder_.CreateBr(nextBlock);
    switchTo(nextBlock);
  }
  emitCleanups(0);
  builder_.CreateRet(object);
  startDeadBlock();
  popScope();
  finishFunction();
}

llvm::Function *CodeGen::constructFunction(const ClassDecl *cls) {
  if (llvm::Function *found = constructFunctions_.lookup(cls))
    return found;
  llvm::Type *ptr = builder_.getPtrTy();
  auto *fn = llvm::Function::Create(
      llvm::FunctionType::get(ptr, {ptr, builder_.getInt32Ty()}, false),
      llvm::GlobalValue::InternalLinkage,
      "tz.construct." + symbolPrefixes_.lookup(cls) + className(cls), module_.get());
  setFunctionAttributes(fn);
  constructFunctions_[cls] = fn; // its body comes once the program is emitted
  return fn;
}

void CodeGen::finishReflection(SourceFile &file, llvm::Instruction *init) {
  // The bodies of the functions that make objects of classes from Json, which
  // may need those of more classes.
  for (size_t i = 0; i < pendingConverters_.size(); ++i)
    emitClassFromJson(pendingConverters_[i], classConverters_.lookup(pendingConverters_[i]));
  // A class is described only if the program looks up what the description
  // holds: when every `new` and Reflect call is of classes known when
  // compiling, none is.
  auto described = [&](const ClassDecl *cls) {
    return cls->isEmitted() && !cls->literal && !cls->builtin &&
           ((reflectsNew_ && cls->reflectNew) || (reflectsGet_ && cls->reflectMethods) ||
            (reflectsJson_ && cls->reflectJson));
  };
  llvm::Type *ptr = builder_.getPtrTy(), *i32 = builder_.getInt32Ty();
  llvm::Constant *null = llvm::ConstantPointerNull::get(builder_.getPtrTy());
  std::vector<const ClassDecl *> classes;
  unsigned count = 1;
  for (Stmt *item : file.items) {
    auto *cls = dyn_cast<ClassDecl>(item);
    if (!cls || !described(cls))
      continue;
    classes.push_back(cls);
    count = std::max(count, cls->id + 1);
  }
  // The description of each class.
  std::vector<const Type *> invoked;
  for (const ClassDecl *cls : classes) {
    llvm::Constant *construct = null;
    if (reflectsNew_ && cls->reflectNew && !cls->isAbstract)
      construct = constructFunction(cls);
    llvm::Constant *fromJson = null;
    if (reflectsJson_ && cls->reflectJson && !cls->isAbstract)
      fromJson = classFromJsonFunction(cls);
    // Its methods, and those up its chain it does not override, by name: of
    // several variants, the function that chooses one by the arguments
    // (docs/overloads.md, section 6).
    std::vector<llvm::Constant *> methods;
    llvm::StringSet<> named;
    for (const ClassDecl *c = cls; c && reflectsGet_ && cls->reflectMethods; c = c->parent) {
      std::vector<FunctionDecl *> own;
      for (FunctionDecl *fn : c->methods)
        own.push_back(fn);
      for (const ClassField &f : c->fields)
        if (f.method)
          own.push_back(f.method);
      for (FunctionDecl *fn : own) {
        if (fn->isStatic || named.contains(fn->name))
          continue;
        std::vector<const FunctionDecl *> variants = reflectVariants(cls, fn->name);
        if (variants.empty())
          continue;
        named.insert(fn->name);
        const Type *type = nullptr;
        llvm::Constant *info = variants.size() == 1 ? reflectAdapterInfo(variants[0], type)
                                                    : reflectDispatchInfo(cls, variants, type);
        invoked.push_back(type);
        methods.push_back(llvm::ConstantStruct::get(
            reflectMethodTy(), {utf8Constant(fn->name),
                                builder_.getInt32(uint32_t(fn->name.size())),
                                typeDescriptor(type), info}));
      }
    }
    llvm::Constant *table = null;
    if (!methods.empty()) {
      auto *type = llvm::ArrayType::get(reflectMethodTy(), methods.size());
      auto *global = new llvm::GlobalVariable(*module_, type, /*isConstant=*/true,
                                              llvm::GlobalValue::PrivateLinkage,
                                              llvm::ConstantArray::get(type, methods),
                                              "tz.methods." + className(cls));
      global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
      table = global;
    }
    auto *info = cast<llvm::GlobalVariable>(classInfoGlobal(cls));
    info->setInitializer(
        llvm::ConstantStruct::get(classInfoType(), {construct, fromJson, table,
                                                    builder_.getInt32(uint32_t(methods.size()))}));
  }
  // The bodies of the functions that make objects from unknowns, which the
  // descriptions and Reflect.construct of classes known when compiling call.
  for (size_t i = 0; i < constructFunctions_.size(); ++i) {
    auto [cls, fn] = *(constructFunctions_.begin() + i);
    if (fn->empty())
      emitConstructThunk(cls, fn);
  }
  for (size_t i = 0; i < pendingConverters_.size(); ++i)
    if (classConverters_.lookup(pendingConverters_[i])->empty())
      emitClassFromJson(pendingConverters_[i], classConverters_.lookup(pendingConverters_[i]));
  // The objects of the other classes point to no description.
  for (auto &[cls, global] : classInfos_)
    if (global && !llvm::is_contained(classes, cls)) {
      global->replaceAllUsesWith(null);
      global->eraseFromParent();
      global = nullptr;
    }
  if (!reflectsNew_ && !reflectsGet_ && !reflectsJson_ && !usesApply_)
    return;

  // The objects of the classes by number, and the invokers of the types of
  // the functions that may reach Reflect.apply.
  std::vector<llvm::Constant *> byId(count, null);
  for (const ClassDecl *cls : classes)
    byId[cls->id] = classObject(cls);
  auto *classesType = llvm::ArrayType::get(ptr, count);
  auto *classTable = new llvm::GlobalVariable(*module_, classesType, /*isConstant=*/true,
                                              llvm::GlobalValue::PrivateLinkage,
                                              llvm::ConstantArray::get(classesType, byId),
                                              "tz.reflect.classes");
  std::vector<llvm::Constant *> entries;
  if (usesApply_) {
    invoked.insert(invoked.end(), unknownFunctionTypes_.begin(), unknownFunctionTypes_.end());
    llvm::SmallPtrSet<const Type *, 8> seen;
    auto *entryType = llvm::StructType::get(ctx_, {ptr, ptr});
    for (const Type *type : invoked)
      if (seen.insert(type).second)
        entries.push_back(
            llvm::ConstantStruct::get(entryType, {typeDescriptor(type), invokerFor(type)}));
  }
  llvm::Constant *invokers = null;
  if (!entries.empty()) {
    auto *type = llvm::ArrayType::get(entries.front()->getType(), entries.size());
    invokers = new llvm::GlobalVariable(*module_, type, /*isConstant=*/true,
                                        llvm::GlobalValue::PrivateLinkage,
                                        llvm::ConstantArray::get(type, entries), "tz.invokers");
  }
  llvm::IRBuilder<> b(init->getNextNode());
  b.CreateCall(unknownFunction("tz_reflect_init", builder_.getVoidTy(), {ptr, i32, ptr, i32}),
               {classTable, b.getInt32(count), invokers, b.getInt32(uint32_t(entries.size()))});
}

llvm::StructType *CodeGen::reflectMethodTy() {
  // TzReflectMethod: { ptr name, i32 length, ptr type, ptr info }.
  if (!reflectMethodTy_) {
    llvm::Type *ptr = builder_.getPtrTy();
    reflectMethodTy_ = llvm::StructType::create(ctx_, {ptr, builder_.getInt32Ty(), ptr, ptr},
                                                "tz.reflectmethod");
  }
  return reflectMethodTy_;
}

//===----------------------------------------------------------------------===//
// The calls
//===----------------------------------------------------------------------===//

// The class first, then the arguments as unknowns, to the description of
// the class the value is.
llvm::Value *CodeGen::emitReflectNew(NewExpr *expr) {
  llvm::Type *ptr = builder_.getPtrTy(), *i32 = builder_.getInt32Ty();
  reflectsNew_ = true;
  llvm::Value *cls = emitExpr(expr->callee); // a class is never counted
  std::vector<llvm::Value *> values = emitHeldValues(expr->args);
  auto *arrayType = llvm::ArrayType::get(ptr, std::max<size_t>(values.size(), 1));
  llvm::Value *args = createStackSlot(arrayType, "reflect.args");
  for (size_t i = 0; i < values.size(); ++i)
    builder_.CreateStore(values[i],
                         builder_.CreateConstInBoundsGEP2_32(arrayType, args, 0, unsigned(i)));
  llvm::Value *object = builder_.CreateCall(
      unknownFunction("tz_reflect_new", ptr, {ptr, ptr, i32}),
      {cls, args, builder_.getInt32(uint32_t(values.size()))}, "object");
  for (llvm::Value *value : values)
    releaseValue(value, Type::unknown());
  emitThrowCheck();
  return object;
}

llvm::Value *CodeGen::emitReflectCall(CallExpr *call) {
  llvm::Type *ptr = builder_.getPtrTy(), *i64 = builder_.getInt64Ty();
  switch (call->builtin) {
  case Builtin::ClassName: {
    llvm::Value *cls = emitExpr(call->args[0]);
    return callReturningString(unknownFunction("tz_class_name", builder_.getVoidTy(), {ptr, ptr}),
                               {cls});
  }
  case Builtin::ReflectGet: {
    reflectsGet_ = true;
    std::vector<llvm::Value *> values = emitHeldValues(call->args);
    llvm::Value *result = builder_.CreateCall(
        unknownFunction("tz_reflect_get", ptr, {ptr, ptr, i64}),
        {values[0], stringObj(values[1]), stringMeta(values[1])}, "got");
    releaseValue(values[0], call->args[0]->type);
    releaseValue(values[1], Type::string());
    emitThrowCheck();
    return result;
  }
  case Builtin::ReflectApply: {
    usesApply_ = true;
    std::vector<llvm::Value *> values = emitHeldValues(call->args);
    llvm::Value *result =
        builder_.CreateCall(unknownFunction("tz_reflect_apply", ptr, {ptr, ptr, ptr}),
                            {values[0], values[1], values[2]}, "result");
    for (size_t i = 0; i < values.size(); ++i)
      releaseValue(values[i], call->args[i]->type);
    emitThrowCheck();
    return result;
  }
  case Builtin::ReflectConstruct: {
    std::vector<llvm::Value *> values = emitHeldValues(call->args);
    llvm::Value *object;
    if (const ClassDecl *known = call->args[0]->type->classRef()) {
      // A class known when compiling: its function, without its description.
      object = builder_.CreateCall(unknownFunction("tz_reflect_make", ptr, {ptr, ptr}),
                                   {constructFunction(known), values[1]}, "object");
    } else {
      reflectsNew_ = true;
      object = builder_.CreateCall(unknownFunction("tz_reflect_construct", ptr, {ptr, ptr}),
                                   {values[0], values[1]}, "object");
    }
    releaseValue(values[1], call->args[1]->type);
    emitThrowCheck();
    return object;
  }
  case Builtin::FromJsonClass: {
    llvm::Value *cls = emitExpr(call->args[0]);
    Operand json = emitOperand(call->args[1], /*canBorrow=*/true);
    llvm::Value *object;
    if (const ClassDecl *known = call->args[0]->type->classRef()) {
      object = builder_.CreateCall(
          classFromJsonFunction(known),
          {json.value, llvm::ConstantPointerNull::get(builder_.getPtrTy())}, "object");
    } else {
      reflectsJson_ = true;
      object = builder_.CreateCall(unknownFunction("tz_reflect_from_json", ptr, {ptr, ptr}),
                                   {cls, json.value}, "object");
    }
    dropOperand(json);
    emitThrowCheck();
    return object;
  }
  case Builtin::ReflectMethod: {
    // The object only says which class: its method, as the description
    // would give it, is a function value without an environment, in a box
    // that lives as long as the program, as the object of a class does.
    Operand object = emitOperand(call->args[0], /*canBorrow=*/true);
    dropOperand(object);
    // Of several variants, the function that chooses one.
    const FunctionDecl *method = call->method->decl;
    const ClassDecl *cls = call->args[0]->type->classDecl();
    std::vector<const FunctionDecl *> variants = reflectVariants(cls, method->name);
    const Type *type = nullptr;
    llvm::Constant *info = variants.size() > 1 ? reflectDispatchInfo(cls, variants, type)
                                               : reflectAdapterInfo(method, type);
    unknownFunctionTypes_.insert(type); // Reflect.apply may call it
    llvm::GlobalVariable *&box = reflectBoxes_[info];
    if (!box) {
      llvm::Constant *init = llvm::ConstantStruct::getAnon(
          {builder_.getInt64(kStaticRefCount),
           module_->getOrInsertGlobal("tz_box_layout", builder_.getInt8Ty()),
           typeDescriptor(type),
           llvm::ConstantStruct::get(fnTy_, {info, llvm::ConstantPointerNull::get(ptr)})});
      box = new llvm::GlobalVariable(*module_, init->getType(), /*isConstant=*/false,
                                     llvm::GlobalValue::InternalLinkage, init,
                                     info->getName() + ".box");
    }
    return box;
  }
  default:
    llvm_unreachable("not a builtin of reflection");
  }
}

} // namespace tz
