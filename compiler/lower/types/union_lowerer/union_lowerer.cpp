#include "compiler/lower/types/union_lowerer/union_lowerer.h"
#include "compiler/library/json/json_lowerer/json_lowerer.h"
#include "compiler/library/regex/matcher_builder/matcher_builder.h"
#include "compiler/lower/expr/equality_lowerer/equality_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/expr/truth_lowerer/truth_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

namespace {

constexpr unsigned kDescriptorClass = 6;

}

static UnionSlot targetSlot(UnionSlot slot, const Type *to) {
  auto has = [&](UnionSlot s) { return llvm::is_contained(to->slots(), s); };
  if (has(slot)) {
    return slot;
  }
  if (slot == UnionSlot::Atom && has(UnionSlot::String)) {
    return UnionSlot::String;
  }
  if (slot == UnionSlot::Int) {
    for (UnionSlot s : {UnionSlot::Double, UnionSlot::Decimal}) {
      if (has(s)) {
        return s;
      }
    }
  }

  return UnionSlot::None;
}

UnionLowerer::UnionLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Value *UnionLowerer::emitUnionConversion(llvm::Value *value, const Type *from,
                                               const Type *to) {
  if (from == to) {
    return value;
  }
  if (to->isOptional()) {
    if (from->isUndefined()) {
      return lowering_.optionalLowerer().undefinedValue(to);
    }
    if (from->isOptional()) {
      return lowering_.optionalLowerer().makeOptional(
          lowering_.exprLowerer().emitConversion(
              lowering_.optionalLowerer().optionalValue(value, from), from->inner(), to->inner()),
          lowering_.optionalLowerer().optionalHas(value, from), to);
    }
    return lowering_.optionalLowerer().makeOptional(
        lowering_.exprLowerer().emitConversion(value, from, to->inner()),
        lowering_.builder_.getTrue(), to);
  }
  if (from->isOptional()) {
    return lowering_.exprLowerer().emitConversion(
        lowering_.optionalLowerer().optionalValue(value, from), from->inner(), to);
  }

  if (UnionLayout::isAtomType(from) && (to->isString() || to->isJson())) {
    return lowering_.exprLowerer().emitConversion(lowering_.unionLayout().atomString(value),
                                                  Type::string(), to);
  }
  if (from->isString() && UnionLayout::isAtomType(to)) {
    std::vector<const Type *> literals =
        to->isStringLiteral()
            ? std::vector<const Type *>{to}
            : std::vector<const Type *>(to->members().begin(), to->members().end());
    llvm::Value *atom = lowering_.builder_.getInt32(literals.front()->atom());
    for (const Type *literal : llvm::ArrayRef<const Type *>(literals).drop_front()) {
      llvm::Value *same = lowering_.equalityLowerer().emitValuesEqual(
          value, lowering_.stringLowerer().stringLiteral(literal->literalText()), Type::string());
      atom =
          lowering_.builder_.CreateSelect(same, lowering_.builder_.getInt32(literal->atom()), atom);
    }
    lowering_.refCounts().releaseString(value);
    return atom;
  }
  if (from->isJson() && to->isTagged()) {
    llvm::Value *kind = lowering_.builder_.CreateCall(
        lowering_.jsonLowerer().jsonFunction("tz_json_typeof", lowering_.builder_.getInt32Ty(),
                                             {lowering_.builder_.getPtrTy()}),
        {value});
    llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("json.union.end");
    std::vector<std::pair<llvm::Value *, llvm::BasicBlock *>> incoming;
    for (UnionSlot slot : to->slots()) {
      llvm::BasicBlock *block = lowering_.functionFrame().newBlock("json.union.slot");
      llvm::BasicBlock *next = lowering_.functionFrame().newBlock("json.union.next");
      int code = slot == UnionSlot::Bool     ? 2
                 : slot == UnionSlot::Double ? 3
                 : slot == UnionSlot::String ? 4
                                             : -1;
      if (code >= 0) {
        lowering_.builder_.CreateCondBr(
            lowering_.builder_.CreateICmpEQ(kind, lowering_.builder_.getInt32(code)), block, next);
      } else {
        lowering_.builder_.CreateBr(block);
      }
      lowering_.functionFrame().switchTo(block);
      llvm::Value *part =
          lowering_.jsonLowerer().emitJsonConversion(value, from, to->slotPart(slot));
      incoming.push_back({lowering_.unionLayout().makeTagged(to, slot, part),
                          lowering_.builder_.GetInsertBlock()});
      lowering_.builder_.CreateBr(endBlock);
      lowering_.functionFrame().switchTo(next);
    }
    lowering_.builder_.CreateUnreachable();
    lowering_.functionFrame().switchTo(endBlock);
    llvm::PHINode *phi = lowering_.builder_.CreatePHI(lowering_.moduleBuilder().lower(to),
                                                      unsigned(incoming.size()));
    for (auto [v, block] : incoming) {
      phi->addIncoming(v, block);
    }
    return phi;
  }

  if (to->isTagged()) {
    if (from->isTagged()) {
      std::vector<UnionSlot> possible;
      bool same = true;
      for (UnionSlot slot : from->slots()) {
        UnionSlot target = targetSlot(slot, to);
        if (target == UnionSlot::None) {
          continue;
        }
        possible.push_back(slot);
        same &= target == slot && lowering_.moduleBuilder().lower(to->slotPart(slot)) ==
                                      lowering_.moduleBuilder().lower(from->slotPart(slot));
      }
      if (same) {
        llvm::Value *result = llvm::Constant::getNullValue(lowering_.moduleBuilder().lower(to));
        unsigned words =
            cast<llvm::StructType>(lowering_.moduleBuilder().lower(to))->getNumElements();
        unsigned have = cast<llvm::StructType>(value->getType())->getNumElements();
        for (unsigned i = 0; i < words && i < have; ++i) {
          result = lowering_.builder_.CreateInsertValue(
              result, lowering_.builder_.CreateExtractValue(value, i), i);
        }
        return result;
      }
      return lowering_.unionLayout().forEachSlot(
          value, possible,
          [&](UnionSlot slot) {
            UnionSlot target = targetSlot(slot, to);
            llvm::Value *part = lowering_.exprLowerer().emitConversion(
                lowering_.unionLayout().taggedPart(value, from, slot), from->slotPart(slot),
                to->slotPart(target));

            return lowering_.unionLayout().makeTagged(to, target, part);
          },
          lowering_.moduleBuilder().lower(to),
          llvm::Constant::getNullValue(lowering_.moduleBuilder().lower(to)));
    }
    UnionSlot slot = targetSlot(unionSlot(from), to);
    assert(slot != UnionSlot::None && "Checker converts a member into its union");
    return lowering_.unionLayout().makeTagged(
        to, slot, lowering_.exprLowerer().emitConversion(value, from, to->slotPart(slot)));
  }
  if (from->isTagged()) {
    UnionSlot slot = unionSlot(to);
    return lowering_.exprLowerer().emitConversion(
        lowering_.unionLayout().taggedPart(value, from, slot), from->slotPart(slot), to);
  }

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
  llvm::Value *sameTag = lowering_.builder_.CreateICmpEQ(lowering_.unionLayout().unionTag(a),
                                                         lowering_.unionLayout().unionTag(b));
  llvm::BasicBlock *before = lowering_.builder_.GetInsertBlock();
  llvm::BasicBlock *compareBlock = lowering_.functionFrame().newBlock("union.compare");
  llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("union.equal");
  lowering_.builder_.CreateCondBr(sameTag, compareBlock, endBlock);
  lowering_.functionFrame().switchTo(compareBlock);
  llvm::Value *equal = lowering_.unionLayout().forEachSlot(
      a, type->slots(),
      [&](UnionSlot slot) {
        return lowering_.equalityLowerer().emitValuesEqual(
            lowering_.unionLayout().taggedPart(a, type, slot),
            lowering_.unionLayout().taggedPart(b, type, slot), type->slotPart(slot));
      },
      lowering_.builder_.getInt1Ty(), lowering_.builder_.getTrue());
  llvm::BasicBlock *compareEnd = lowering_.builder_.GetInsertBlock();
  lowering_.builder_.CreateBr(endBlock);
  lowering_.functionFrame().switchTo(endBlock);
  llvm::PHINode *phi = lowering_.builder_.CreatePHI(lowering_.builder_.getInt1Ty(), 2);
  phi->addIncoming(lowering_.builder_.getFalse(), before);
  phi->addIncoming(equal, compareEnd);

  return phi;
}

llvm::Value *UnionLowerer::emitUnionTypeOf(llvm::Value *value, const Type *type) {
  llvm::Value *tag = lowering_.unionLayout().unionTag(value);
  llvm::Value *result = lowering_.stringLowerer().stringLiteral(u"undefined");
  for (UnionSlot slot : type->slots()) {
    llvm::StringRef name = typeOfName(type->slotPart(slot));
    result = lowering_.builder_.CreateSelect(
        lowering_.builder_.CreateICmpEQ(tag, lowering_.builder_.getInt32(unsigned(slot))),
        lowering_.stringLowerer().stringLiteral(std::u16string(name.begin(), name.end())), result);
  }

  return result;
}

llvm::Value *UnionLowerer::emitUnionTruth(llvm::Value *value, const Type *type) {
  if (UnionLayout::isAtomType(type)) {
    return lowering_.truthLowerer().emitTruth(lowering_.unionLayout().atomString(value),
                                              Type::string());
  }
  if (const Type *stored = UnionLayout::storedType(type); stored != type) {
    return lowering_.truthLowerer().emitTruth(value, stored);
  }
  if (UnionLayout::isObjectUnion(type)) {
    return lowering_.builder_.getTrue();
  }

  return lowering_.unionLayout().forEachSlot(
      value, type->slots(),
      [&](UnionSlot slot) -> llvm::Value * {
        const Type *part = type->slotPart(slot);
        if (part->isString() || part->isBool() || UnionLayout::isAtomType(part) ||
            part->isLiteral()) {
          return lowering_.truthLowerer().emitTruth(
              lowering_.unionLayout().taggedPart(value, type, slot), part);
        }
        return lowering_.builder_.getTrue();
      },
      lowering_.builder_.getInt1Ty(), lowering_.builder_.getFalse());
}

uint32_t UnionLowerer::plainTypeId(const Type *type) {
  if (type->isPromise()) {
    return kPlainIdBase;
  }
  if (type->isBytes()) {
    return kBytesIdBase + uint32_t(type->bytes());
  }
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
  llvm::Value *descriptor = lowering_.builder_.CreateLoad(
      lowering_.builder_.getPtrTy(),
      lowering_.builder_.CreateConstInBoundsGEP1_64(lowering_.builder_.getInt8Ty(), object, 8),
      "descriptor");

  return lowering_.builder_.CreateLoad(
      lowering_.builder_.getInt32Ty(),
      lowering_.builder_.CreateStructGEP(lowering_.layoutDescTy_, descriptor, kDescriptorClass),
      "class");
}

llvm::Value *UnionLowerer::emitIsOneOf(llvm::Value *id, llvm::ArrayRef<const Type *> types) {
  llvm::Value *result = lowering_.builder_.getFalse();
  auto test = [&](const Type *t) {
    if (const ClassDecl *cls = t->classDecl()) {
      llvm::Value *in =
          cls->lastId <= cls->id
              ? lowering_.builder_.CreateICmpEQ(id, lowering_.builder_.getInt32(cls->id))
              : lowering_.builder_.CreateICmpULE(
                    lowering_.builder_.CreateSub(id, lowering_.builder_.getInt32(cls->id)),
                    lowering_.builder_.getInt32(cls->lastId - cls->id));
      result = lowering_.builder_.CreateOr(result, in);
      return;
    }
    uint32_t number =
        t->isRegExp() || t->isMatch() || t->isIntl() || t->isCrypto() ? 0 : plainTypeId(t);
    result = lowering_.builder_.CreateOr(
        result, lowering_.builder_.CreateICmpEQ(id, lowering_.builder_.getInt32(number)));
    if (t->bytes() == Type::Bytes::Uint8) {
      result = lowering_.builder_.CreateOr(
          result, lowering_.builder_.CreateICmpEQ(id, lowering_.builder_.getInt32(plainTypeId(
                                                          Type::bytesType(Type::Bytes::Buffer)))));
    }
  };
  for (const Type *t : types) {
    test(t);
    for (const Type *sub : lowering_.subtypes_) {
      if (sub != t && sub->isSubtypeOf(t) && !(sub->classDecl() && t->classDecl())) {
        test(sub);
      }
    }
  }

  return result;
}

}
