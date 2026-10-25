#include "compiler/lower/types/union_lowerer/union_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/expr/equality_lowerer/equality_lowerer.h"
#include "compiler/lower/expr/truth_lowerer/truth_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/library/json/json_lowerer/json_lowerer.h"
#include "compiler/library/regex/matcher_builder/matcher_builder.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

namespace {

// The descriptor of an object, whose class number tells its type.
constexpr unsigned kDescriptorClass = 6;

} // namespace

// The slot of `to`, a tagged union, that a part of a union in `slot` goes to
// when `to` has no such slot: a string literal is a string, an int widens.
// None for a part that a check ruled out (`to` is a narrowed type).
static UnionSlot targetSlot(UnionSlot slot, const Type *to) {
  auto has = [&](UnionSlot s) { return llvm::is_contained(to->slots(), s); };
  if (has(slot)) {
    return slot;
  }
  if (slot == UnionSlot::Atom && has(UnionSlot::String)) {
    return UnionSlot::String;
  }
  if (slot == UnionSlot::Int) {
    for (UnionSlot s : {UnionSlot::Long, UnionSlot::Double, UnionSlot::Decimal}) {
      if (has(s)) {
        return s;
      }
    }
  }
  if (slot == UnionSlot::Long && has(UnionSlot::Double)) {
    return UnionSlot::Double;
  }

  return UnionSlot::None;
}

UnionLowerer::UnionLowerer(CodeGen &codegen) : codegen_(codegen) {}

llvm::Value *UnionLowerer::emitUnionConversion(llvm::Value *value, const Type *from, const Type *to) {
  if (from == to) {
    return value;
  }
  if (to->isOptional()) {
    if (from->isUndefined()) {
      return codegen_.optionalLowerer().undefinedValue(to);
    }
    if (from->isOptional()) {
      return codegen_.optionalLowerer().makeOptional(codegen_.exprLowerer().emitConversion(codegen_.optionalLowerer().optionalValue(value, from), from->inner(), to->inner()),
                          codegen_.optionalLowerer().optionalHas(value, from), to);
    }
    return codegen_.optionalLowerer().makeOptional(codegen_.exprLowerer().emitConversion(value, from, to->inner()), codegen_.builder_.getTrue(), to);
  }
  if (from->isOptional()) { // narrowed: known to hold a value
    return codegen_.exprLowerer().emitConversion(codegen_.optionalLowerer().optionalValue(value, from), from->inner(), to);
  }

  // The text of a string literal: a constant, which is not counted.
  if (UnionLayout::isAtomType(from) && (to->isString() || to->isJson())) {
    return codegen_.exprLowerer().emitConversion(codegen_.unionLayout().atomString(value), Type::string(), to);
  }
  // A Json that checks showed to hold one of the members.
  if (from->isJson() && to->isTagged()) {
    llvm::Value *kind = codegen_.builder_.CreateCall(
        codegen_.jsonLowerer().jsonFunction("tz_json_typeof", codegen_.builder_.getInt32Ty(), {codegen_.builder_.getPtrTy()}), {value});
    llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("json.union.end");
    std::vector<std::pair<llvm::Value *, llvm::BasicBlock *>> incoming;
    // See tz_json_typeof: 2 a boolean, 3 a number, 4 a string, else an array.
    for (UnionSlot slot : to->slots()) {
      llvm::BasicBlock *block = codegen_.functionFrame().newBlock("json.union.slot");
      llvm::BasicBlock *next = codegen_.functionFrame().newBlock("json.union.next");
      int code = slot == UnionSlot::Bool     ? 2
                 : slot == UnionSlot::Double ? 3
                 : slot == UnionSlot::String ? 4
                                             : -1;
      if (code >= 0) {
        codegen_.builder_.CreateCondBr(codegen_.builder_.CreateICmpEQ(kind, codegen_.builder_.getInt32(code)), block, next);
      } else {
        codegen_.builder_.CreateBr(block);
      }
      codegen_.functionFrame().switchTo(block);
      llvm::Value *part = codegen_.jsonLowerer().emitJsonConversion(value, from, to->slotPart(slot));
      incoming.push_back({codegen_.unionLayout().makeTagged(to, slot, part), codegen_.builder_.GetInsertBlock()});
      codegen_.builder_.CreateBr(endBlock);
      codegen_.functionFrame().switchTo(next);
    }
    codegen_.builder_.CreateUnreachable();
    codegen_.functionFrame().switchTo(endBlock);
    llvm::PHINode *phi = codegen_.builder_.CreatePHI(codegen_.moduleBuilder().lower(to), unsigned(incoming.size()));
    for (auto [v, block] : incoming) {
      phi->addIncoming(v, block);
    }
    return phi;
  }

  if (to->isTagged()) {
    if (from->isTagged()) {
      // The same tags, where the parts lie alike; a part in a slot that `to`
      // lacks converts, unless a check ruled it out.
      std::vector<UnionSlot> possible;
      bool same = true;
      for (UnionSlot slot : from->slots()) {
        UnionSlot target = targetSlot(slot, to);
        if (target == UnionSlot::None) {
          continue;
        }
        possible.push_back(slot);
        same &= target == slot && codegen_.moduleBuilder().lower(to->slotPart(slot)) == codegen_.moduleBuilder().lower(from->slotPart(slot));
      }
      if (same) {
        llvm::Value *result = llvm::Constant::getNullValue(codegen_.moduleBuilder().lower(to));
        unsigned words = cast<llvm::StructType>(codegen_.moduleBuilder().lower(to))->getNumElements();
        unsigned have = cast<llvm::StructType>(value->getType())->getNumElements();
        for (unsigned i = 0; i < words && i < have; ++i) {
          result = codegen_.builder_.CreateInsertValue(result, codegen_.builder_.CreateExtractValue(value, i), i);
        }
        return result;
      }
      return codegen_.unionLayout().forEachSlot(
          value, possible,
          [&](UnionSlot slot) {
            UnionSlot target = targetSlot(slot, to);
            llvm::Value *part = codegen_.exprLowerer().emitConversion(codegen_.unionLayout().taggedPart(value, from, slot), from->slotPart(slot),
                                               to->slotPart(target));

            return codegen_.unionLayout().makeTagged(to, target, part);
          },
          codegen_.moduleBuilder().lower(to), llvm::Constant::getNullValue(codegen_.moduleBuilder().lower(to)));
    }
    // A member, or a union whose members take one slot.
    UnionSlot slot = targetSlot(unionSlot(from), to);
    assert(slot != UnionSlot::None && "Sema converts a member into its union");
    return codegen_.unionLayout().makeTagged(to, slot, codegen_.exprLowerer().emitConversion(value, from, to->slotPart(slot)));
  }
  // Narrowed: a member of the tagged union, or a part of it.
  if (from->isTagged()) {
    UnionSlot slot = unionSlot(to);
    return codegen_.exprLowerer().emitConversion(codegen_.unionLayout().taggedPart(value, from, slot), from->slotPart(slot), to);
  }

  // Atoms are atoms, objects references, whatever the union.
  return value;
}

bool UnionLowerer::narrowsUnion(const Type *from, const Type *to) {
  from = from->nonOptional();
  if (!from->isUnion() || to->isOptional() || to->isNever()) {
    return false;
  }
  llvm::ArrayRef<const Type *> members = from->members();
  if (to->isUnion()) {
    return llvm::all_of(to->members(),
                        [&](const Type *m) { return llvm::is_contained(members, m); });
  }

  return llvm::is_contained(members, to);
}

llvm::Value *UnionLowerer::emitUnionEqual(llvm::Value *a, llvm::Value *b, const Type *type) {
  // Two values of a tagged union are equal when they hold the same member
  // and those are equal; both undefined are equal too.
  llvm::Value *sameTag = codegen_.builder_.CreateICmpEQ(codegen_.unionLayout().unionTag(a), codegen_.unionLayout().unionTag(b));
  llvm::BasicBlock *before = codegen_.builder_.GetInsertBlock();
  llvm::BasicBlock *compareBlock = codegen_.functionFrame().newBlock("union.compare");
  llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("union.equal");
  codegen_.builder_.CreateCondBr(sameTag, compareBlock, endBlock);
  codegen_.functionFrame().switchTo(compareBlock);
  llvm::Value *equal = codegen_.unionLayout().forEachSlot(
      a, type->slots(),
      [&](UnionSlot slot) {
        return codegen_.equalityLowerer().emitValuesEqual(codegen_.unionLayout().taggedPart(a, type, slot), codegen_.unionLayout().taggedPart(b, type, slot),
                               type->slotPart(slot));
      },
      codegen_.builder_.getInt1Ty(), codegen_.builder_.getTrue());
  llvm::BasicBlock *compareEnd = codegen_.builder_.GetInsertBlock();
  codegen_.builder_.CreateBr(endBlock);
  codegen_.functionFrame().switchTo(endBlock);
  llvm::PHINode *phi = codegen_.builder_.CreatePHI(codegen_.builder_.getInt1Ty(), 2);
  phi->addIncoming(codegen_.builder_.getFalse(), before);
  phi->addIncoming(equal, compareEnd);

  return phi;
}

llvm::Value *UnionLowerer::emitUnionTypeOf(llvm::Value *value, const Type *type) {
  // A select over the tags: each slot has one name.
  llvm::Value *tag = codegen_.unionLayout().unionTag(value);
  llvm::Value *result = codegen_.stringLowerer().stringLiteral(u"undefined");
  for (UnionSlot slot : type->slots()) {
    llvm::StringRef name = typeOfName(type->slotPart(slot));
    result = codegen_.builder_.CreateSelect(codegen_.builder_.CreateICmpEQ(tag, codegen_.builder_.getInt32(unsigned(slot))),
                                   codegen_.stringLowerer().stringLiteral(std::u16string(name.begin(), name.end())), result);
  }

  return result;
}

llvm::Value *UnionLowerer::emitUnionTruth(llvm::Value *value, const Type *type) {
  if (UnionLayout::isAtomType(type)) {
    return codegen_.truthLowerer().emitTruth(codegen_.unionLayout().atomString(value), Type::string());
  }
  if (const Type *stored = UnionLayout::storedType(type); stored != type) {
    return codegen_.truthLowerer().emitTruth(value, stored);
  }
  if (UnionLayout::isObjectUnion(type)) {
    return codegen_.builder_.getTrue();
  }

  // Tagged: a string or a boolean as itself, an object always, undefined never.
  return codegen_.unionLayout().forEachSlot(
      value, type->slots(),
      [&](UnionSlot slot) -> llvm::Value * {
        const Type *part = type->slotPart(slot);
        if (part->isString() || part->isBool() || UnionLayout::isAtomType(part) || part->isLiteral()) {
          return codegen_.truthLowerer().emitTruth(codegen_.unionLayout().taggedPart(value, type, slot), part);
        }
        return codegen_.builder_.getTrue();
      },
      codegen_.builder_.getInt1Ty(), codegen_.builder_.getFalse());
}

uint32_t UnionLowerer::plainTypeId(const Type *type) {
  // Every Promise has one number: a union holds one Promise type, and a
  // Promise<never> stands for any.
  if (type->isPromise()) {
    return kPlainIdBase;
  }
  // The runtime makes the objects of binary data, numbered by their kind.
  if (type->isBytes()) {
    return kBytesIdBase + uint32_t(type->bytes());
  }
  // A type and its twin with read-only fields lie alike: one number.
  std::vector<Field> fields(type->fields().begin(), type->fields().end());
  for (Field &f : fields) {
    f.readonly = false;
  }
  const Type *canonical = type;
  if (type->isTuple()) {
    std::vector<const Type *> elements;
    for (const Field &f : fields) {
      elements.push_back(f.type);
    }
    canonical = Type::tuple(elements);
  } else if (!type->isClass() && !type->isClassRef() && !type->isIncomplete() &&
             !type->isRegExp() && !type->isMatch() && !type->isDate() && !type->isIntl() &&
             !type->isCrypto() && !type->isDict() && !type->isAnyObject() &&
             type->bases().empty() && type->overloads().empty()) {
    canonical = Type::object(std::move(fields));
  }
  auto [it, inserted] = plainIds_.try_emplace(canonical, 0);
  if (inserted) {
    it->second = kPlainIdBase + uint32_t(plainIds_.size());
  }

  return it->second;
}

llvm::Value *UnionLowerer::objectClassId(llvm::Value *object) {
  llvm::Value *descriptor = codegen_.builder_.CreateLoad(
      codegen_.builder_.getPtrTy(), codegen_.builder_.CreateConstInBoundsGEP1_64(codegen_.builder_.getInt8Ty(), object, 8),
      "descriptor");

  return codegen_.builder_.CreateLoad(codegen_.builder_.getInt32Ty(),
                             codegen_.builder_.CreateStructGEP(codegen_.layoutDescTy_, descriptor, kDescriptorClass),
                             "class");
}

llvm::Value *UnionLowerer::emitIsOneOf(llvm::Value *id, llvm::ArrayRef<const Type *> types) {
  llvm::Value *result = codegen_.builder_.getFalse();
  auto test = [&](const Type *t) {
    if (const ClassDecl *cls = t->classDecl()) {
      llvm::Value *in =
          cls->lastId <= cls->id
              ? codegen_.builder_.CreateICmpEQ(id, codegen_.builder_.getInt32(cls->id))
              : codegen_.builder_.CreateICmpULE(codegen_.builder_.CreateSub(id, codegen_.builder_.getInt32(cls->id)),
                                       codegen_.builder_.getInt32(cls->lastId - cls->id));
      result = codegen_.builder_.CreateOr(result, in);
      return;
    }
    // A regex, a match or an object of Intl is an object of the
    // runtime, which has no number.
    uint32_t number =
        t->isRegExp() || t->isMatch() || t->isIntl() || t->isCrypto() ? 0 : plainTypeId(t);
    result = codegen_.builder_.CreateOr(result, codegen_.builder_.CreateICmpEQ(id, codegen_.builder_.getInt32(number)));
    // A Buffer is a Uint8Array.
    if (t->bytes() == Type::Bytes::Uint8) {
      result = codegen_.builder_.CreateOr(
          result, codegen_.builder_.CreateICmpEQ(
                      id, codegen_.builder_.getInt32(plainTypeId(Type::bytesType(Type::Bytes::Buffer)))));
    }
  };
  for (const Type *t : types) {
    test(t);
    // The objects of the types declared to be it: classes that implement an
    // interface, interfaces that extend it.
    for (const Type *sub : codegen_.subtypes_) {
      if (sub != t && sub->isSubtypeOf(t) && !(sub->classDecl() && t->classDecl())) {
        test(sub);
      }
    }
  }

  return result;
}

} // namespace tz
