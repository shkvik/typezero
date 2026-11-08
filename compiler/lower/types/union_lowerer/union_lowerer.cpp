#include "compiler/lower/types/union_lowerer/union_lowerer.h"
#include "compiler/lower/expr/equality_lowerer/equality_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/expr/truth_lowerer/truth_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/library/json/json_lowerer/json_lowerer.h"
#include "compiler/lower/library/regex/matcher_builder/matcher_builder.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/objects/dict_lowerer/dict_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/lower/types/unknown_lowerer/unknown_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/semantic/semantic_external.h"
#include "llvm/IR/MDBuilder.h"

using llvm::cast;

namespace tz {

namespace {

constexpr unsigned kDescriptorClass = 6;

}

UnionLowerer::UnionLowerer(Lowering &owner)
    : lowering(owner) {}

UnionSlot UnionLowerer::targetSlot(UnionSlot slot, const Type *to) {
  auto has = [&](UnionSlot s) { return llvm::is_contained(to->slots(), s); };
  if (has(slot)) {
    return slot;
  }
  if (slot == UnionSlot::Atom && has(UnionSlot::String)) {
    return UnionSlot::String;
  }
  bool integer = slot == UnionSlot::Int || (slot >= UnionSlot::Int8 && slot <= UnionSlot::Uint64);
  if (integer) {
    for (UnionSlot s : {UnionSlot::Int, UnionSlot::Double, UnionSlot::Decimal}) {
      if (has(s)) {
        return s;
      }
    }
  }

  return UnionSlot::None;
}

llvm::Value *UnionLowerer::emitUnionConversion(llvm::Value *value, const Type *from,
                                               const Type *to) {
  if (from == to) {
    return value;
  }
  if (to->isOptional()) {
    if (from->isUndefined()) {
      return this->lowering.optionalLowerer().undefinedValue(to);
    }
    if (from->isOptional()) {
      return this->lowering.optionalLowerer().makeOptional(
          this->lowering.exprLowerer().emitConversion(
              this->lowering.optionalLowerer().optionalValue(value, from), from->inner(),
              to->inner()),
          this->lowering.optionalLowerer().optionalHas(value, from), to);
    }
    return this->lowering.optionalLowerer().makeOptional(
        this->lowering.exprLowerer().emitConversion(value, from, to->inner()),
        this->lowering.builder.getTrue(), to);
  }
  if (from->isOptional()) {
    return this->lowering.exprLowerer().emitConversion(
        this->lowering.optionalLowerer().optionalValue(value, from), from->inner(), to);
  }

  if (UnionLayout::isAtomType(from) && (to->isString() || to->isJson())) {
    return this->lowering.exprLowerer().emitConversion(
        this->lowering.unionLayout().atomString(value), Type::string(), to);
  }
  if (from->isString() && UnionLayout::isAtomType(to)) {
    std::vector<const Type *> literals =
        to->isStringLiteral()
            ? std::vector<const Type *>{to}
            : std::vector<const Type *>(to->members().begin(), to->members().end());
    llvm::Value *atom = this->lowering.builder.getInt32(literals.front()->atom());
    for (const Type *literal : llvm::ArrayRef<const Type *>(literals).drop_front()) {
      llvm::Value *same = this->lowering.equalityLowerer().emitValuesEqual(
          value, this->lowering.stringLowerer().stringLiteral(literal->literalText()),
          Type::string());
      atom = this->lowering.builder.CreateSelect(
          same, this->lowering.builder.getInt32(literal->atom()), atom);
    }
    this->lowering.refCounts().releaseString(value);
    return atom;
  }
  if (from->isJson() && to->isTagged() && llvm::is_contained(to->slots(), UnionSlot::Json)) {
    return this->lowering.unionLayout().makeTagged(to, UnionSlot::Json, value);
  }
  if (from->isJson() && to->isTagged()) {
    llvm::Value *kind =
        this->lowering.builder.CreateCall(this->lowering.jsonLowerer().jsonFunction(
                                              "tz_json_typeof", this->lowering.builder.getInt32Ty(),
                                              {this->lowering.builder.getPtrTy()}),
                                          {value});
    llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("json.union.end");
    std::vector<std::pair<llvm::Value *, llvm::BasicBlock *>> incoming;
    for (UnionSlot slot : to->slots()) {
      llvm::BasicBlock *block = this->lowering.functionFrame().newBlock("json.union.slot");
      llvm::BasicBlock *next = this->lowering.functionFrame().newBlock("json.union.next");
      int code = slot == UnionSlot::Bool     ? 2
                 : slot == UnionSlot::Double ? 11
                 : slot == UnionSlot::String ? 13
                                             : -1;
      if (code >= 0) {
        this->lowering.builder.CreateCondBr(
            this->lowering.builder.CreateICmpEQ(kind, this->lowering.builder.getInt32(code)), block,
            next);
      } else {
        this->lowering.builder.CreateBr(block);
      }
      this->lowering.functionFrame().switchTo(block);
      llvm::Value *part =
          this->lowering.jsonLowerer().emitJsonConversion(value, from, to->slotPart(slot));
      incoming.push_back({this->lowering.unionLayout().makeTagged(to, slot, part),
                          this->lowering.builder.GetInsertBlock()});
      this->lowering.builder.CreateBr(endBlock);
      this->lowering.functionFrame().switchTo(next);
    }
    this->lowering.builder.CreateUnreachable();
    this->lowering.functionFrame().switchTo(endBlock);
    llvm::PHINode *phi = this->lowering.builder.CreatePHI(this->lowering.moduleBuilder().lower(to),
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
        UnionSlot target = UnionLowerer::targetSlot(slot, to);
        if (target == UnionSlot::None) {
          continue;
        }
        possible.push_back(slot);
        same &= target == slot && this->lowering.moduleBuilder().lower(to->slotPart(slot)) ==
                                      this->lowering.moduleBuilder().lower(from->slotPart(slot));
      }
      if (same) {
        llvm::Value *result =
            llvm::Constant::getNullValue(this->lowering.moduleBuilder().lower(to));
        unsigned words =
            cast<llvm::StructType>(this->lowering.moduleBuilder().lower(to))->getNumElements();
        unsigned have = cast<llvm::StructType>(value->getType())->getNumElements();
        for (unsigned i = 0; i < words && i < have; ++i) {
          result = this->lowering.builder.CreateInsertValue(
              result, this->lowering.builder.CreateExtractValue(value, i), i);
        }
        return result;
      }
      return this->lowering.unionLayout().forEachSlot(
          value, possible,
          [&](UnionSlot slot) {
            UnionSlot target = UnionLowerer::targetSlot(slot, to);
            llvm::Value *part = this->lowering.exprLowerer().emitConversion(
                this->lowering.unionLayout().taggedPart(value, from, slot), from->slotPart(slot),
                to->slotPart(target));

            return this->lowering.unionLayout().makeTagged(to, target, part);
          },
          this->lowering.moduleBuilder().lower(to),
          llvm::Constant::getNullValue(this->lowering.moduleBuilder().lower(to)));
    }
    UnionSlot slot = UnionLowerer::targetSlot(UnionSlots::unionSlot(from), to);
    assert(slot != UnionSlot::None && "Checker converts a member into its union");
    return this->lowering.unionLayout().makeTagged(
        to, slot, this->lowering.exprLowerer().emitConversion(value, from, to->slotPart(slot)));
  }
  if (from->isTagged()) {
    UnionSlot slot = UnionSlots::unionSlot(to);
    return this->lowering.exprLowerer().emitConversion(
        this->lowering.unionLayout().taggedPart(value, from, slot), from->slotPart(slot), to);
  }
  if (!from->isUnion() && to->isUnion() && !llvm::is_contained(to->members(), from)) {
    for (const Type *member : to->members()) {
      if (SemanticExternalFacade::copiesIntoRecord(from, member)) {
        return this->lowering.dictLowerer().emitRecordFromObject(value, from, member);
      }
    }
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
  llvm::Value *sameTag = this->lowering.builder.CreateICmpEQ(
      this->lowering.unionLayout().unionTag(a), this->lowering.unionLayout().unionTag(b));
  llvm::BasicBlock *before = this->lowering.builder.GetInsertBlock();
  llvm::BasicBlock *compareBlock = this->lowering.functionFrame().newBlock("union.compare");
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("union.equal");
  this->lowering.builder.CreateCondBr(sameTag, compareBlock, endBlock);
  this->lowering.functionFrame().switchTo(compareBlock);
  llvm::Value *equal = this->lowering.unionLayout().forEachSlot(
      a, type->slots(),
      [&](UnionSlot slot) {
        return this->lowering.equalityLowerer().emitValuesEqual(
            this->lowering.unionLayout().taggedPart(a, type, slot),
            this->lowering.unionLayout().taggedPart(b, type, slot), type->slotPart(slot));
      },
      this->lowering.builder.getInt1Ty(), this->lowering.builder.getTrue());
  llvm::BasicBlock *compareEnd = this->lowering.builder.GetInsertBlock();
  this->lowering.builder.CreateBr(endBlock);
  this->lowering.functionFrame().switchTo(endBlock);
  llvm::PHINode *phi = this->lowering.builder.CreatePHI(this->lowering.builder.getInt1Ty(), 2);
  phi->addIncoming(this->lowering.builder.getFalse(), before);
  phi->addIncoming(equal, compareEnd);

  return phi;
}

llvm::Value *UnionLowerer::emitUnionTypeOf(llvm::Value *value, const Type *type) {
  llvm::Value *tag = this->lowering.unionLayout().unionTag(value);
  llvm::Value *result = this->lowering.stringLowerer().stringLiteral(u"undefined");
  for (UnionSlot slot : type->slots()) {
    if (slot == UnionSlot::Json) {
      llvm::Value *isJson =
          this->lowering.builder.CreateICmpEQ(tag, this->lowering.builder.getInt32(unsigned(slot)));
      llvm::BasicBlock *before = this->lowering.builder.GetInsertBlock();
      llvm::BasicBlock *jsonBlock = this->lowering.functionFrame().newBlock("typeof.json");
      llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("typeof.end");
      this->lowering.builder.CreateCondBr(isJson, jsonBlock, endBlock);
      this->lowering.functionFrame().switchTo(jsonBlock);
      llvm::Value *code = this->lowering.builder.CreateCall(
          this->lowering.jsonLowerer().jsonFunction("tz_json_typeof",
                                                    this->lowering.builder.getInt32Ty(),
                                                    {this->lowering.builder.getPtrTy()}),
          {this->lowering.unionLayout().taggedPart(value, type, slot)});
      llvm::Value *text = this->lowering.unknownLowerer().typeOfText(code);
      llvm::BasicBlock *jsonEnd = this->lowering.builder.GetInsertBlock();
      this->lowering.builder.CreateBr(endBlock);
      this->lowering.functionFrame().switchTo(endBlock);
      llvm::PHINode *phi = this->lowering.builder.CreatePHI(result->getType(), 2);
      phi->addIncoming(result, before);
      phi->addIncoming(text, jsonEnd);
      result = phi;
      continue;
    }
    llvm::StringRef name = TypeSpelling::typeOfName(type->slotPart(slot));
    result = this->lowering.builder.CreateSelect(
        this->lowering.builder.CreateICmpEQ(tag, this->lowering.builder.getInt32(unsigned(slot))),
        this->lowering.stringLowerer().stringLiteral(std::u16string(name.begin(), name.end())),
        result);
  }

  return result;
}

llvm::Value *UnionLowerer::emitUnionTruth(llvm::Value *value, const Type *type) {
  if (UnionLayout::isAtomType(type)) {
    return this->lowering.truthLowerer().emitTruth(this->lowering.unionLayout().atomString(value),
                                                   Type::string());
  }
  const Type *stored = UnionLayout::storedType(type);
  if (stored != type) {
    return this->lowering.truthLowerer().emitTruth(value, stored);
  }
  if (UnionLayout::isObjectUnion(type)) {
    return this->lowering.builder.getTrue();
  }

  return this->lowering.unionLayout().forEachSlot(
      value, type->slots(),
      [&](UnionSlot slot) -> llvm::Value * {
        const Type *part = type->slotPart(slot);
        if (part->isString() || part->isBool() || UnionLayout::isAtomType(part) ||
            part->isLiteral()) {
          return this->lowering.truthLowerer().emitTruth(
              this->lowering.unionLayout().taggedPart(value, type, slot), part);
        }
        return this->lowering.builder.getTrue();
      },
      this->lowering.builder.getInt1Ty(), this->lowering.builder.getFalse());
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
  auto [it, inserted] = this->plainIds.try_emplace(canonical, 0);
  if (inserted) {
    it->second = kPlainIdBase + uint32_t(this->plainIds.size());
  }

  return it->second;
}

llvm::Value *UnionLowerer::objectClassId(llvm::Value *object) {
  llvm::Value *descriptor =
      this->lowering.builder.CreateLoad(this->lowering.builder.getPtrTy(),
                                        this->lowering.builder.CreateConstInBoundsGEP1_64(
                                            this->lowering.builder.getInt8Ty(), object, 8),
                                        "descriptor");

  return this->lowering.builder.CreateLoad(
      this->lowering.builder.getInt32Ty(),
      this->lowering.builder.CreateStructGEP(this->lowering.layoutDescTy, descriptor,
                                             kDescriptorClass),
      "class");
}

llvm::Value *UnionLowerer::emitIsOneOf(llvm::Value *id, llvm::ArrayRef<const Type *> types) {
  llvm::Value *result = this->lowering.builder.getFalse();
  auto test = [&](const Type *t) {
    const ClassDecl *cls = t->classDecl();
    if (cls) {
      llvm::Value *in =
          cls->lastId <= cls->id
              ? this->lowering.builder.CreateICmpEQ(id, this->lowering.builder.getInt32(cls->id))
              : this->lowering.builder.CreateICmpULE(
                    this->lowering.builder.CreateSub(id, this->lowering.builder.getInt32(cls->id)),
                    this->lowering.builder.getInt32(cls->lastId - cls->id));
      result = this->lowering.builder.CreateOr(result, in);
      return;
    }
    if (t->isClassRef() && !t->classRef()) {
      for (const ClassDecl *emitted : this->lowering.emittedClasses) {
        const Type *ref = Type::classRef(emitted, emitted->name);
        if (!emitted->literal() && ref->isSubtypeOf(t)) {
          result = this->lowering.builder.CreateOr(
              result, this->lowering.builder.CreateICmpEQ(
                          id, this->lowering.builder.getInt32(this->plainTypeId(ref))));
        }
      }
      return;
    }
    uint32_t number =
        t->isRegExp() || t->isMatch() || t->isIntl() || t->isCrypto() ? 0 : this->plainTypeId(t);
    result = this->lowering.builder.CreateOr(
        result, this->lowering.builder.CreateICmpEQ(id, this->lowering.builder.getInt32(number)));
    if (t->bytes() == Type::Bytes::Uint8) {
      result = this->lowering.builder.CreateOr(
          result,
          this->lowering.builder.CreateICmpEQ(id, this->lowering.builder.getInt32(this->plainTypeId(
                                                      Type::bytesType(Type::Bytes::Buffer)))));
    }
  };
  for (const Type *t : types) {
    test(t);
    for (const Type *sub : this->lowering.subtypes) {
      if (sub != t && sub->isSubtypeOf(t) && !(sub->classDecl() && t->classDecl())) {
        test(sub);
      }
    }
  }

  return result;
}

}
