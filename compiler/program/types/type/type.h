#pragma once

#include "compiler/program/types/field/field.h"
#include "compiler/program/types/intl_kind/intl_kind.h"
#include "compiler/program/types/overload/overload.h"
#include "compiler/program/types/type_op/type_op.h"
#include "compiler/program/types/union_slot/union_slot.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <string>
#include <vector>

namespace tz {

struct ClassDecl;

class Type {
public:
  enum class Kind {
    Error,
    Void,
    Undefined,
    Bool,
    Int,
    Double,
    Decimal,
    String,
    Optional,
    Array,
    Function,
    Object,
    Param,
    Result,
    Never,
    Iterator,
    Json,
    Dict,
    Literal,
    Union,
    Promise,
    Unknown,
    ClassRef,
  };
  enum class DictKind { Record, Map, Set };

  Kind kind() const { return kind_; }
  bool isError() const { return kind_ == Kind::Error; }
  bool isVoid() const { return kind_ == Kind::Void; }
  bool isUndefined() const { return kind_ == Kind::Undefined; }
  bool isBool() const { return kind_ == Kind::Bool; }
  bool isInt() const { return kind_ == Kind::Int && bits_ == 64 && !unsigned_; }
  bool isString() const { return kind_ == Kind::String; }
  bool isInteger() const { return kind_ == Kind::Int; }
  bool isUnsigned() const { return unsigned_; }
  bool isWrapping() const { return wrapping_; }
  int64_t intMin() const {
    return unsigned_ ? 0 : bits_ == 64 ? INT64_MIN : -(int64_t(1) << (bits_ - 1));
  }
  uint64_t intMax() const {
    return bits_ == 64 ? (unsigned_ ? UINT64_MAX : uint64_t(INT64_MAX))
                       : (uint64_t(1) << (unsigned_ ? bits_ : bits_ - 1)) - 1;
  }
  bool holds(const Type *other) const {
    return other->isInteger() && isInteger() && other->intMin() >= intMin() &&
           other->intMax() <= intMax();
  }
  bool isFloat() const { return kind_ == Kind::Double; }
  bool isNumeric() const { return isInteger() || isFloat(); }
  bool isDecimal() const { return kind_ == Kind::Decimal; }
  bool isOptional() const { return kind_ == Kind::Optional; }
  bool isArray() const { return kind_ == Kind::Array; }
  bool isFunction() const { return kind_ == Kind::Function; }
  bool isObject() const { return kind_ == Kind::Object; }
  bool isTuple() const { return tuple_; }
  bool isParam() const { return kind_ == Kind::Param; }
  bool isResult() const { return kind_ == Kind::Result; }
  bool isNever() const { return kind_ == Kind::Never; }
  bool isClass() const { return cls_ != nullptr; }
  const ClassDecl *classDecl() const { return cls_; }
  bool isIterator() const { return kind_ == Kind::Iterator; }
  bool isJson() const { return kind_ == Kind::Json; }
  bool isDict() const { return kind_ == Kind::Dict; }
  bool isUnknown() const { return kind_ == Kind::Unknown; }
  bool isClassRef() const { return kind_ == Kind::ClassRef; }
  const ClassDecl *classRef() const { return isClassRef() ? refClass_ : nullptr; }
  bool isConstructor() const { return isClassRef() && !refClass_; }
  const Type *constructed() const { return isConstructor() ? inner_ : nullptr; }
  bool isRecord() const { return isDict() && dictKind_ == DictKind::Record; }
  bool isMap() const { return isDict() && dictKind_ == DictKind::Map; }
  bool isSet() const { return isDict() && dictKind_ == DictKind::Set; }
  DictKind dictKind() const { return dictKind_; }
  const Type *keyType() const { return isDict() ? params_[0] : nullptr; }
  const Type *valueType() const { return isDict() ? inner_ : nullptr; }
  bool isLiteral() const { return kind_ == Kind::Literal; }
  bool isUnion() const { return kind_ == Kind::Union; }

  const Type *literalBase() const { return isLiteral() ? inner_ : nullptr; }
  const std::u16string &literalText() const { return text_; }
  double literalNumber() const { return number_; }
  bool isStringLiteral() const { return isLiteral() && inner_->isString(); }
  unsigned atom() const { return atom_; }
  llvm::ArrayRef<const Type *> members() const {
    return isUnion() ? llvm::ArrayRef<const Type *>(params_) : llvm::ArrayRef<const Type *>();
  }
  llvm::ArrayRef<UnionSlot> slots() const { return slots_; }
  bool isTagged() const { return isUnion() && slots_.size() > 1; }
  const Type *slotPart(UnionSlot slot) const;
  unsigned id() const { return id_; }
  bool isPromise() const { return kind_ == Kind::Promise; }
  const Type *promised() const { return isPromise() ? inner_ : nullptr; }
  bool isTimer() const { return timer_ != 0; }
  bool isImmediate() const { return timer_ == 2; }
  enum class Runtime : uint8_t {
    None,
    AbortSignal,
    FileHandle,
    Readline,
    ReadlinePromises,
    AsyncIterator,
  };
  Runtime runtimeKind() const { return runtime_; }
  bool isRuntimeObject() const { return runtime_ != Runtime::None; }
  bool isAsyncIterator() const { return runtime_ == Runtime::AsyncIterator; }
  enum class Crypto { None, Hash, Hmac, Cipheriv, Decipheriv, KeyObject, Sign, Verify, Ecdh, X509 };
  Crypto crypto() const { return crypto_; }
  bool isCrypto() const { return crypto_ != Crypto::None; }

  const Type *inner() const { return inner_; }
  llvm::ArrayRef<const Type *> params() const { return params_; }
  size_t minArgs() const { return minArgs_; }
  bool hasRest() const { return hasRest_; }
  bool hasThis() const { return hasThis_; }
  const Type *ret() const { return inner_; }
  const Type *okType() const { return inner_; }
  const Type *errType() const { return error_; }
  const Type *nonOptional() const { return isOptional() ? inner_ : this; }
  llvm::ArrayRef<Field> fields() const { return fields_; }
  const Field *field(llvm::StringRef name) const;
  llvm::ArrayRef<Overload> overloads() const { return overloads_; }
  const Overload *overload(llvm::StringRef name) const;
  bool isIncomplete() const { return incomplete_; }
  bool isAnyObject() const { return anyObject_; }
  bool isRegExp() const { return regExp_; }
  bool isMatch() const { return match_; }
  bool isDate() const { return date_; }
  bool isDateFormat() const { return intl_ == IntlKind::DateTimeFormat; }
  bool isIntl() const { return intl_ != IntlKind::None; }
  IntlKind intlKind() const { return intl_; }
  bool isWeakRef() const { return weakRef_; }
  const Type *weakTarget() const { return weakRef_ ? inner_ : nullptr; }
  enum class Bytes {
    None,
    Uint8,
    Int8,
    Uint8Clamped,
    Int16,
    Uint16,
    Int32,
    Uint32,
    Float16,
    Float32,
    Float64,
    Buffer,
    ArrayBuffer,
    DataView,
    TextEncoder,
    TextDecoder,
    FatalTextDecoder,
  };
  Bytes bytes() const { return bytes_; }
  bool isBytes() const { return bytes_ != Bytes::None; }
  bool isTypedArray() const { return bytes_ >= Bytes::Uint8 && bytes_ <= Bytes::Buffer; }
  bool isBuffer() const { return bytes_ == Bytes::Buffer; }
  bool isByteView() const { return isTypedArray() || bytes_ == Bytes::DataView; }
  const Type *elementType() const;
  unsigned elementSize() const;
  const Type *accessType() const;
  bool elementRange(int64_t &lo, int64_t &hi) const;
  int matchGroups() const { return matchGroups_; }
  bool captureMandatory(unsigned k) const {
    return k == 0 || (k < mandatory_.size() && mandatory_[k]);
  }
  const Type *constraint() const { return isParam() ? inner_ : nullptr; }
  llvm::ArrayRef<const Type *> alternatives() const {
    return isParam() ? llvm::ArrayRef<const Type *>(params_) : llvm::ArrayRef<const Type *>();
  }
  const Type *iterated() const { return isParam() ? iterated_ : nullptr; }
  void setIterated(const Type *element) { iterated_ = element; }
  bool isListParam() const { return isParam() && list_; }
  void setList() { list_ = true; }
  static const Type *typeOp(TypeOp op, llvm::ArrayRef<const Type *> operands);
  static const Type *indexedAccess(const Type *object, const Type *key) {
    return typeOp(TypeOp::Index, {object, key});
  }
  static const Type *mapped(const Type *key, const Type *keys, const Type *value, int optional,
                            int readonly, const Type *source, const Type *rename = nullptr);
  TypeOp typeOp() const { return isParam() ? op_ : TypeOp::None; }
  llvm::ArrayRef<const Type *> operands() const { return operands_; }
  int mappedOptional() const { return mappedOptional_; }
  int mappedReadonly() const { return mappedReadonly_; }
  const Type *yielded() const { return inner_; }
  const Type *returned() const { return isIterator() || isAsyncIterator() ? params_[0] : nullptr; }
  const Type *sent() const { return isIterator() || isAsyncIterator() ? params_[1] : nullptr; }
  llvm::ArrayRef<const Type *> bases() const { return bases_; }
  void addBase(const Type *base);
  bool isBase() const { return isBase_; }
  bool isSubtypeOf(const Type *base) const;
  bool isManaged() const;
  bool hasZeroValue() const;

  unsigned bitWidth() const;
  llvm::StringRef name() const { return name_; }

  static const Type *error();
  static const Type *voidTy();
  static const Type *undefinedTy();
  static const Type *boolTy();
  static const Type *intTy();
  static const Type *integer(unsigned bits, bool isUnsigned);
  static const Type *wrapping(unsigned bits);
  static llvm::ArrayRef<const Type *> integers();
  static const Type *doubleTy();
  static const Type *decimalTy();
  static const Type *string();
  static const Type *json();
  static const Type *optional(const Type *type);
  static const Type *array(const Type *element);
  static const Type *function(llvm::ArrayRef<const Type *> params, size_t minArgs, bool hasRest,
                              const Type *ret, bool hasThis = false);
  static const Type *object(std::vector<Field> fields, llvm::StringRef name = {},
                            std::vector<Overload> overloads = {});
  static Type *declareObject(llvm::StringRef name);
  void defineFields(std::vector<Field> fields, std::vector<Overload> overloads = {});
  static Type *declareUnion(llvm::StringRef name);
  void defineMembers(const Type *of);
  static const Type *tuple(llvm::ArrayRef<const Type *> elements, bool readonly = false);
  static const Type *argumentList(llvm::ArrayRef<const Type *> elements, size_t min,
                                  const Type *rest);
  size_t tupleMin() const { return tupleMin_; }
  const Type *tupleRest() const { return tupleRest_; }
  static Type *param(llvm::StringRef name);
  void setConstraint(const Type *constraint, std::vector<const Type *> alternatives);
  static const Type *anyObject();
  static const Type *never();
  static const Type *unknown();
  static const Type *classRef(const ClassDecl *cls, llvm::StringRef name);
  static const Type *constructorOf(const Type *instance);
  static const Type *result(const Type *ok, const Type *err);
  static Type *classType(llvm::StringRef name, const ClassDecl *decl);
  void rename(std::string name) { name_ = std::move(name); }
  static void nameUnion(const Type *type, llvm::StringRef name);
  static void swapClasses(Type *a, Type *b);
  void forgetFields() {
    fields_.clear();
    incomplete_ = true;
  }
  void replaceBase(const Type *from, const Type *to) {
    for (const Type *&base : bases_) {
      if (base == from) {
        base = to;
      }
    }
  }
  llvm::ArrayRef<const Type *> typeArgs() const { return typeArgs_; }
  void setTypeArgs(std::vector<const Type *> args) { typeArgs_ = std::move(args); }
  static const Type *iterator(const Type *yielded, const Type *returned = voidTy(),
                              const Type *sent = voidTy());
  static const Type *regExp();
  static const Type *date();
  static const Type *dateFormat();
  static const Type *intl(IntlKind kind);
  static const Type *weakRef(const Type *target);
  static const Type *dict(DictKind kind, const Type *key, const Type *value);
  static const Type *promise(const Type *value);
  static const Type *timeout();
  static const Type *immediate();
  static const Type *cryptoType(Crypto kind);
  static const Type *bytesType(Bytes kind);
  static const Type *runtimeObject(Runtime kind);
  static const Type *asyncIterator(const Type *yielded, const Type *returned = voidTy(),
                                   const Type *sent = voidTy());
  static const Type *match(int groups, std::vector<bool> mandatory, const Type *named,
                           bool indices);
  static const Type *stringLiteral(const std::u16string &text);
  static const Type *stringPattern(std::vector<std::u16string> texts,
                                   std::vector<const Type *> holes);
  bool isPattern() const { return pattern_; }
  llvm::ArrayRef<std::u16string> patternTexts() const { return texts_; }
  llvm::ArrayRef<const Type *> patternHoles() const {
    return pattern_ ? llvm::ArrayRef<const Type *>(params_) : llvm::ArrayRef<const Type *>();
  }
  static const Type *numberLiteral(double value, const Type *base);
  static const Type *boolLiteral(bool value);
  static llvm::ArrayRef<const Type *> stringLiterals();
  static const Type *unionOf(llvm::ArrayRef<const Type *> types);

  static const Type *fromName(llvm::StringRef name);

private:
  Type(Kind kind, std::string name, const Type *inner = nullptr)
      : kind_(kind), inner_(inner), name_(std::move(name)), id_(++count_) {}
  static unsigned count_;

  Kind kind_;
  const Type *inner_;
  const Type *error_ = nullptr;
  std::string name_;
  std::vector<const Type *> params_;
  size_t minArgs_ = 0;
  bool hasRest_ = false;
  bool hasThis_ = false;
  const ClassDecl *refClass_ = nullptr;
  std::vector<Field> fields_;
  std::vector<Overload> overloads_;
  bool incomplete_ = false;
  bool tuple_ = false;
  bool anyObject_ = false;
  const ClassDecl *cls_ = nullptr;
  bool regExp_ = false;
  bool match_ = false;
  bool date_ = false;
  IntlKind intl_ = IntlKind::None;
  bool weakRef_ = false;
  int timer_ = 0;
  Crypto crypto_ = Crypto::None;
  Bytes bytes_ = Bytes::None;
  Runtime runtime_ = Runtime::None;
  int matchGroups_ = -1;
  std::vector<bool> mandatory_;
  std::vector<const Type *> bases_;
  bool isBase_ = false;
  DictKind dictKind_ = DictKind::Record;
  std::u16string text_;
  bool pattern_ = false;
  std::vector<std::u16string> texts_;
  double number_ = 0;
  unsigned atom_ = 0;
  unsigned bits_ = 0;
  bool unsigned_ = false;
  bool wrapping_ = false;
  std::vector<UnionSlot> slots_;
  std::vector<const Type *> typeArgs_;
  const Type *iterated_ = nullptr;
  bool list_ = false;
  TypeOp op_ = TypeOp::None;
  std::vector<const Type *> operands_;
  int mappedOptional_ = 0, mappedReadonly_ = 0;
  size_t tupleMin_ = 0;
  const Type *tupleRest_ = nullptr;
  unsigned id_;
};

}
