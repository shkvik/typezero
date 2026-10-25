#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// Json, JSON.parse and stringify, embedJson.
class JsonLowerer {
public:
  explicit JsonLowerer(CodeGen &codegen);

  // Json (codegen_json.cpp): a pointer to a node of the runtime
  // (runtime/library/json/json/json.c), counted as objects are; `Json |
  // undefined` is that pointer, null for undefined.
  llvm::FunctionCallee jsonFunction(llvm::StringRef name, llvm::Type *ret,
                                    llvm::ArrayRef<llvm::Type *> params);
  // Whether a borrowed `Json | undefined` holds something other than null.
  llvm::Value *jsonPresent(llvm::Value *value);
  llvm::Value *emitJsonNull();
  llvm::Value *emitJsonMember(MemberExpr *member);
  llvm::Value *emitJsonIndex(IndexExpr *index);
  llvm::Value *emitJsonAssign(AssignExpr *assign, bool needResult);
  // An owned value converted to or from a Json (see emitConversion).
  llvm::Value *emitJsonConversion(llvm::Value *value, const Type *from, const Type *to);
  llvm::Value *emitJsonEquality(BinaryExpr *bin);
  llvm::Value *emitJsonNullish(BinaryExpr *bin);
  llvm::Value *emitJsonChainLink(CastExpr *link);
  llvm::Value *emitJsonTypeOf(Expr *operand);
  llvm::Value *emitJsonCall(CallExpr *call);
  llvm::Value *emitJsonObjectLiteral(ObjectLitExpr *lit);
  llvm::Value *emitJsonObjectCall(CallExpr *call);

private:
  CodeGen &codegen_;
  // The values of embedJson, by the path of the file.
  std::map<std::u16string, llvm::GlobalVariable *> embedCaches_;
};

} // namespace tz
