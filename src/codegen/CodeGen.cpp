#include "codegen/CodeGen.h"

#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

CodeGen::CodeGen(llvm::LLVMContext &ctx, llvm::StringRef moduleName)
    : ctx_(ctx), module_(std::make_unique<llvm::Module>(moduleName, ctx)), builder_(ctx) {}

std::unique_ptr<llvm::Module> CodeGen::generate(SourceFile &file) {
  declareRuntime();
  for (Stmt *item : file.items)
    if (auto *fn = dyn_cast<FunctionDecl>(item))
      declareFunction(fn);
  declareGlobals(file);
  for (Stmt *item : file.items)
    if (auto *fn = dyn_cast<FunctionDecl>(item))
      emitFunction(fn);
  emitMain(file);
  return std::move(module_);
}

llvm::Type *CodeGen::lower(const Type *type) {
  switch (type->kind()) {
  case Type::Kind::Void: return builder_.getVoidTy();
  case Type::Kind::Bool: return builder_.getInt1Ty();
  case Type::Kind::Int: return builder_.getInt32Ty();
  case Type::Kind::Long: return builder_.getInt64Ty();
  case Type::Kind::Double: return builder_.getDoubleTy();
  case Type::Kind::String: return builder_.getPtrTy();
  case Type::Kind::Error: break;
  }
  llvm_unreachable("an erroneous type reached code generation");
}

//===----------------------------------------------------------------------===//
// Module structure
//===----------------------------------------------------------------------===//

void CodeGen::declareRuntime() {
  llvm::Type *voidTy = builder_.getVoidTy();
  llvm::Type *i32 = builder_.getInt32Ty();
  llvm::Type *i64 = builder_.getInt64Ty();
  llvm::Type *f64 = builder_.getDoubleTy();
  llvm::Type *ptr = builder_.getPtrTy();

  auto declare = [&](llvm::StringRef name, llvm::Type *ret, llvm::ArrayRef<llvm::Type *> params) {
    llvm::FunctionCallee callee =
        module_->getOrInsertFunction(name, llvm::FunctionType::get(ret, params, false));
    cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);
    return callee;
  };
  rt_.init = declare("tz_rt_init", voidTy, {});
  rt_.printStr = declare("tz_print_str", voidTy, {ptr, i64});
  rt_.printI32 = declare("tz_print_i32", voidTy, {i32});
  rt_.printI64 = declare("tz_print_i64", voidTy, {i64});
  rt_.printF64 = declare("tz_print_f64", voidTy, {f64});
  rt_.printBool = declare("tz_print_bool", voidTy, {i32});
  rt_.printChar = declare("tz_print_char", voidTy, {i32});
  rt_.panic = declare("tz_panic", voidTy, {ptr, i64});

  auto *panic = cast<llvm::Function>(rt_.panic.getCallee());
  panic->addFnAttr(llvm::Attribute::NoReturn);
  panic->addFnAttr(llvm::Attribute::Cold);
}

void CodeGen::setFunctionAttributes(llvm::Function *fn) {
  // Nothing unwinds yet, but unwind tables still let debuggers and crash
  // handlers walk the stack.
  fn->addFnAttr(llvm::Attribute::NoUnwind);
  fn->setUWTableKind(llvm::UWTableKind::Default);
}

void CodeGen::declareFunction(FunctionDecl *fn) {
  const FuncSymbol *sym = fn->sym;
  std::vector<llvm::Type *> params;
  for (const Type *param : sym->params)
    params.push_back(lower(param));
  auto *type = llvm::FunctionType::get(lower(sym->ret), params, false);
  // The whole program is one module, so everything but `main` is internal and
  // LLVM is free to inline or drop it.
  auto *function = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage,
                                          "tz." + fn->name, module_.get());
  setFunctionAttributes(function);
  functions_[sym] = function;
}

void CodeGen::declareGlobals(SourceFile &file) {
  for (Stmt *item : file.items) {
    auto *decl = dyn_cast<VarDeclStmt>(item);
    if (!decl)
      continue;
    for (VarDeclarator &d : decl->decls) {
      llvm::Type *type = lower(d.sym->type);
      // Holds zero until the top-level code that declares it runs.
      addresses_[d.sym] = new llvm::GlobalVariable(*module_, type, /*isConstant=*/false,
                                                   llvm::GlobalValue::InternalLinkage,
                                                   llvm::Constant::getNullValue(type), "tz." + d.name);
    }
  }
}

void CodeGen::emitFunction(FunctionDecl *fn) {
  function_ = functions_.lookup(fn->sym);
  builder_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", function_));
  for (size_t i = 0; i < fn->params.size(); ++i) {
    const Param &param = fn->params[i];
    llvm::Argument *arg = function_->getArg(static_cast<unsigned>(i));
    arg->setName(param.name);
    // Parameters can be assigned to, so they live in stack slots like locals.
    llvm::AllocaInst *slot = createStackSlot(arg->getType(), param.name + ".addr");
    builder_.CreateStore(arg, slot);
    addresses_[param.sym] = slot;
  }
  emitStmt(fn->body);
  finishFunction();
}

void CodeGen::emitMain(SourceFile &file) {
  auto *type = llvm::FunctionType::get(builder_.getInt32Ty(), false);
  function_ = llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage, "main", module_.get());
  setFunctionAttributes(function_);
  builder_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", function_));
  builder_.CreateCall(rt_.init);
  for (Stmt *item : file.items)
    if (!isa<FunctionDecl>(item))
      emitStmt(item);
  finishFunction();
}

void CodeGen::finishFunction() {
  if (blockIsOpen()) {
    llvm::Type *ret = function_->getReturnType();
    if (ret->isVoidTy())
      builder_.CreateRetVoid();
    else if (function_->hasExternalLinkage()) // main
      builder_.CreateRet(builder_.getInt32(0));
    else // Sema proved that every path returns
      builder_.CreateUnreachable();
  }
  function_ = nullptr;
}

//===----------------------------------------------------------------------===//
// Blocks
//===----------------------------------------------------------------------===//

llvm::BasicBlock *CodeGen::newBlock(const llvm::Twine &name) {
  return llvm::BasicBlock::Create(ctx_, name);
}

void CodeGen::switchTo(llvm::BasicBlock *block) {
  function_->insert(function_->end(), block);
  builder_.SetInsertPoint(block);
}

void CodeGen::emitBlock(llvm::BasicBlock *block) {
  branchTo(block);
  switchTo(block);
}

bool CodeGen::blockIsOpen() { return !builder_.GetInsertBlock()->hasTerminator(); }

void CodeGen::branchTo(llvm::BasicBlock *block) {
  if (blockIsOpen())
    builder_.CreateBr(block);
}

void CodeGen::startDeadBlock() { switchTo(newBlock("dead")); }

llvm::AllocaInst *CodeGen::createStackSlot(llvm::Type *type, const llvm::Twine &name) {
  llvm::BasicBlock &entry = function_->getEntryBlock();
  llvm::IRBuilder<> entryBuilder(&entry, entry.begin());
  return entryBuilder.CreateAlloca(type, nullptr, name);
}

llvm::Value *CodeGen::addressOf(const VarSymbol *var) {
  llvm::Value *address = addresses_.lookup(var);
  assert(address && "variable used before its declaration was emitted");
  return address;
}

llvm::Value *CodeGen::addressOfTarget(Expr *target) {
  return addressOf(cast<NameExpr>(target)->var);
}

llvm::Constant *CodeGen::stringConstant(llvm::StringRef text) {
  auto [it, inserted] = strings_.try_emplace(text, nullptr);
  if (inserted)
    it->second = builder_.CreateGlobalString(text, ".str", 0, module_.get());
  return it->second;
}

//===----------------------------------------------------------------------===//
// Statements
//===----------------------------------------------------------------------===//

void CodeGen::emitStmt(Stmt *stmt) {
  switch (stmt->kind()) {
  case Stmt::Kind::Block:
    for (Stmt *s : cast<BlockStmt>(stmt)->body)
      emitStmt(s);
    break;
  case Stmt::Kind::VarDecl:
    emitVarDecl(cast<VarDeclStmt>(stmt));
    break;
  case Stmt::Kind::Function:
    break; // emitted on its own
  case Stmt::Kind::Expr:
    emitExpr(cast<ExprStmt>(stmt)->expr);
    break;
  case Stmt::Kind::If:
    emitIf(cast<IfStmt>(stmt));
    break;
  case Stmt::Kind::While:
    emitWhile(cast<WhileStmt>(stmt));
    break;
  case Stmt::Kind::DoWhile:
    emitDoWhile(cast<DoWhileStmt>(stmt));
    break;
  case Stmt::Kind::For:
    emitFor(cast<ForStmt>(stmt));
    break;
  case Stmt::Kind::Return:
    emitReturn(cast<ReturnStmt>(stmt));
    break;
  case Stmt::Kind::Break:
    builder_.CreateBr(loops_.back().breakTarget);
    startDeadBlock();
    break;
  case Stmt::Kind::Continue:
    builder_.CreateBr(loops_.back().continueTarget);
    startDeadBlock();
    break;
  case Stmt::Kind::Empty:
    break;
  }
}

void CodeGen::emitVarDecl(VarDeclStmt *decl) {
  for (VarDeclarator &d : decl->decls) {
    const VarSymbol *var = d.sym;
    llvm::Type *type = lower(var->type);
    llvm::Value *address;
    if (var->isGlobal) {
      address = addressOf(var);
    } else {
      address = createStackSlot(type, d.name);
      addresses_[var] = address;
    }
    llvm::Value *value = d.init ? emitExpr(d.init) : llvm::Constant::getNullValue(type);
    builder_.CreateStore(value, address);
  }
}

void CodeGen::emitIf(IfStmt *stmt) {
  llvm::Value *cond = emitExpr(stmt->cond);
  llvm::BasicBlock *thenBlock = newBlock("if.then");
  llvm::BasicBlock *endBlock = newBlock("if.end");
  llvm::BasicBlock *elseBlock = stmt->elseStmt ? newBlock("if.else") : endBlock;
  builder_.CreateCondBr(cond, thenBlock, elseBlock);

  switchTo(thenBlock);
  emitStmt(stmt->thenStmt);
  branchTo(endBlock);

  if (stmt->elseStmt) {
    switchTo(elseBlock);
    emitStmt(stmt->elseStmt);
    branchTo(endBlock);
  }
  switchTo(endBlock);
}

void CodeGen::emitWhile(WhileStmt *stmt) {
  llvm::BasicBlock *condBlock = newBlock("while.cond");
  llvm::BasicBlock *bodyBlock = newBlock("while.body");
  llvm::BasicBlock *endBlock = newBlock("while.end");

  emitBlock(condBlock);
  llvm::Value *cond = emitExpr(stmt->cond);
  builder_.CreateCondBr(cond, bodyBlock, endBlock);

  switchTo(bodyBlock);
  loops_.push_back({endBlock, condBlock});
  emitStmt(stmt->body);
  loops_.pop_back();
  branchTo(condBlock);

  switchTo(endBlock);
}

void CodeGen::emitDoWhile(DoWhileStmt *stmt) {
  llvm::BasicBlock *bodyBlock = newBlock("do.body");
  llvm::BasicBlock *condBlock = newBlock("do.cond");
  llvm::BasicBlock *endBlock = newBlock("do.end");

  emitBlock(bodyBlock);
  loops_.push_back({endBlock, condBlock});
  emitStmt(stmt->body);
  loops_.pop_back();

  emitBlock(condBlock);
  llvm::Value *cond = emitExpr(stmt->cond);
  builder_.CreateCondBr(cond, bodyBlock, endBlock);

  switchTo(endBlock);
}

void CodeGen::emitFor(ForStmt *stmt) {
  if (stmt->init)
    emitStmt(stmt->init);

  llvm::BasicBlock *condBlock = newBlock("for.cond");
  llvm::BasicBlock *bodyBlock = newBlock("for.body");
  llvm::BasicBlock *stepBlock = newBlock("for.step");
  llvm::BasicBlock *endBlock = newBlock("for.end");

  emitBlock(condBlock);
  if (stmt->cond) {
    llvm::Value *cond = emitExpr(stmt->cond);
    builder_.CreateCondBr(cond, bodyBlock, endBlock);
  } else {
    builder_.CreateBr(bodyBlock);
  }

  switchTo(bodyBlock);
  loops_.push_back({endBlock, stepBlock});
  emitStmt(stmt->body);
  loops_.pop_back();

  emitBlock(stepBlock);
  if (stmt->step)
    emitExpr(stmt->step);
  builder_.CreateBr(condBlock);

  switchTo(endBlock);
}

void CodeGen::emitReturn(ReturnStmt *stmt) {
  if (!stmt->value) {
    builder_.CreateRetVoid();
  } else {
    llvm::Value *value = emitExpr(stmt->value);
    if (stmt->value->type->isVoid())
      builder_.CreateRetVoid();
    else
      builder_.CreateRet(value);
  }
  startDeadBlock();
}

//===----------------------------------------------------------------------===//
// Expressions
//===----------------------------------------------------------------------===//

llvm::Value *CodeGen::emitExpr(Expr *expr) {
  switch (expr->kind()) {
  case Expr::Kind::IntLit: {
    auto *lit = cast<IntLitExpr>(expr);
    if (lit->type->isFloat())
      return llvm::ConstantFP::get(builder_.getDoubleTy(), static_cast<double>(lit->value));
    return llvm::ConstantInt::get(lower(lit->type), lit->value);
  }
  case Expr::Kind::FloatLit:
    return llvm::ConstantFP::get(builder_.getDoubleTy(), cast<FloatLitExpr>(expr)->value);
  case Expr::Kind::BoolLit:
    return builder_.getInt1(cast<BoolLitExpr>(expr)->value);
  case Expr::Kind::StringLit:
    return stringConstant(cast<StringLitExpr>(expr)->value);
  case Expr::Kind::Name: {
    auto *name = cast<NameExpr>(expr);
    return builder_.CreateLoad(lower(name->type), addressOf(name->var), name->name);
  }
  case Expr::Kind::Unary:
    return emitUnary(cast<UnaryExpr>(expr));
  case Expr::Kind::Binary:
    return emitBinary(cast<BinaryExpr>(expr));
  case Expr::Kind::Assign:
    return emitAssign(cast<AssignExpr>(expr));
  case Expr::Kind::Update:
    return emitUpdate(cast<UpdateExpr>(expr));
  case Expr::Kind::Call:
    return emitCall(cast<CallExpr>(expr));
  case Expr::Kind::Conditional:
    return emitConditional(cast<ConditionalExpr>(expr));
  case Expr::Kind::Cast: {
    auto *c = cast<CastExpr>(expr);
    return emitConversion(emitExpr(c->operand), c->operand->type, c->type);
  }
  case Expr::Kind::Member:
    break;
  }
  llvm_unreachable("Sema rejects this expression");
}

llvm::Value *CodeGen::emitUnary(UnaryExpr *unary) {
  llvm::Value *operand = emitExpr(unary->operand);
  switch (unary->op) {
  case UnaryOp::Neg:
    return unary->type->isFloat() ? builder_.CreateFNeg(operand) : builder_.CreateNeg(operand);
  case UnaryOp::Plus:
    return operand;
  case UnaryOp::Not:
  case UnaryOp::BitNot:
    return builder_.CreateNot(operand);
  }
  llvm_unreachable("unknown unary operator");
}

llvm::Value *CodeGen::emitBinary(BinaryExpr *bin) {
  if (bin->op == BinaryOp::LogAnd || bin->op == BinaryOp::LogOr)
    return emitLogical(bin);

  llvm::Value *lhs = emitExpr(bin->lhs);
  llvm::Value *rhs = emitExpr(bin->rhs);
  switch (bin->op) {
  case BinaryOp::Lt:
  case BinaryOp::Le:
  case BinaryOp::Gt:
  case BinaryOp::Ge:
  case BinaryOp::Eq:
  case BinaryOp::Ne:
    return emitComparison(bin->op, lhs, rhs, bin->lhs->type);
  default:
    return emitArithmetic(bin->op, lhs, rhs, bin->type);
  }
}

// `&&` and `||` evaluate their right side only when needed.
llvm::Value *CodeGen::emitLogical(BinaryExpr *bin) {
  bool isAnd = bin->op == BinaryOp::LogAnd;
  llvm::Value *lhs = emitExpr(bin->lhs);
  llvm::BasicBlock *lhsEnd = builder_.GetInsertBlock();
  llvm::BasicBlock *rhsBlock = newBlock(isAnd ? "and.rhs" : "or.rhs");
  llvm::BasicBlock *endBlock = newBlock(isAnd ? "and.end" : "or.end");
  if (isAnd)
    builder_.CreateCondBr(lhs, rhsBlock, endBlock);
  else
    builder_.CreateCondBr(lhs, endBlock, rhsBlock);

  switchTo(rhsBlock);
  llvm::Value *rhs = emitExpr(bin->rhs);
  llvm::BasicBlock *rhsEnd = builder_.GetInsertBlock();
  builder_.CreateBr(endBlock);

  switchTo(endBlock);
  llvm::PHINode *phi = builder_.CreatePHI(builder_.getInt1Ty(), 2);
  phi->addIncoming(builder_.getInt1(!isAnd), lhsEnd);
  phi->addIncoming(rhs, rhsEnd);
  return phi;
}

llvm::Value *CodeGen::emitArithmetic(BinaryOp op, llvm::Value *lhs, llvm::Value *rhs,
                                     const Type *type) {
  bool isFloat = type->isFloat();
  switch (op) {
  // Integer arithmetic wraps around on overflow, as in Go.
  case BinaryOp::Add:
    return isFloat ? builder_.CreateFAdd(lhs, rhs) : builder_.CreateAdd(lhs, rhs);
  case BinaryOp::Sub:
    return isFloat ? builder_.CreateFSub(lhs, rhs) : builder_.CreateSub(lhs, rhs);
  case BinaryOp::Mul:
    return isFloat ? builder_.CreateFMul(lhs, rhs) : builder_.CreateMul(lhs, rhs);
  case BinaryOp::Div:
    return isFloat ? builder_.CreateFDiv(lhs, rhs) : emitIntDivRem(op, lhs, rhs);
  case BinaryOp::Rem:
    return isFloat ? builder_.CreateFRem(lhs, rhs) : emitIntDivRem(op, lhs, rhs);
  case BinaryOp::BitAnd:
    return builder_.CreateAnd(lhs, rhs);
  case BinaryOp::BitOr:
    return builder_.CreateOr(lhs, rhs);
  case BinaryOp::BitXor:
    return builder_.CreateXor(lhs, rhs);
  case BinaryOp::Shl:
  case BinaryOp::Shr:
  case BinaryOp::UShr: {
    // As in JavaScript, only the low bits of the count are used. This also
    // keeps the shift defined for any count.
    llvm::Value *mask = llvm::ConstantInt::get(lhs->getType(), type->bitWidth() - 1);
    llvm::Value *count = builder_.CreateAnd(rhs, mask);
    if (op == BinaryOp::Shl)
      return builder_.CreateShl(lhs, count);
    if (op == BinaryOp::Shr)
      return builder_.CreateAShr(lhs, count);
    return builder_.CreateLShr(lhs, count);
  }
  default:
    break;
  }
  llvm_unreachable("not an arithmetic operator");
}

llvm::Value *CodeGen::emitComparison(BinaryOp op, llvm::Value *lhs, llvm::Value *rhs,
                                     const Type *type) {
  if (type->isFloat()) {
    switch (op) {
    case BinaryOp::Lt: return builder_.CreateFCmpOLT(lhs, rhs);
    case BinaryOp::Le: return builder_.CreateFCmpOLE(lhs, rhs);
    case BinaryOp::Gt: return builder_.CreateFCmpOGT(lhs, rhs);
    case BinaryOp::Ge: return builder_.CreateFCmpOGE(lhs, rhs);
    case BinaryOp::Eq: return builder_.CreateFCmpOEQ(lhs, rhs);
    case BinaryOp::Ne: return builder_.CreateFCmpUNE(lhs, rhs); // NaN !== NaN
    default: break;
    }
  } else {
    switch (op) {
    case BinaryOp::Lt: return builder_.CreateICmpSLT(lhs, rhs);
    case BinaryOp::Le: return builder_.CreateICmpSLE(lhs, rhs);
    case BinaryOp::Gt: return builder_.CreateICmpSGT(lhs, rhs);
    case BinaryOp::Ge: return builder_.CreateICmpSGE(lhs, rhs);
    case BinaryOp::Eq: return builder_.CreateICmpEQ(lhs, rhs);
    case BinaryOp::Ne: return builder_.CreateICmpNE(lhs, rhs);
    default: break;
    }
  }
  llvm_unreachable("not a comparison operator");
}

// Dividing by zero panics, as in Go. MIN / -1 wraps around to MIN (and
// MIN % -1 is 0) where the machine instruction would trap.
llvm::Value *CodeGen::emitIntDivRem(BinaryOp op, llvm::Value *lhs, llvm::Value *rhs) {
  bool isRem = op == BinaryOp::Rem;
  if (auto *divisor = dyn_cast<llvm::ConstantInt>(rhs);
      divisor && !divisor->isZero() && !divisor->isMinusOne())
    return isRem ? builder_.CreateSRem(lhs, rhs) : builder_.CreateSDiv(lhs, rhs);

  llvm::Type *type = lhs->getType();
  llvm::Value *isZero = builder_.CreateICmpEQ(rhs, llvm::ConstantInt::get(type, 0));
  llvm::BasicBlock *panicBlock = newBlock("div.zero");
  llvm::BasicBlock *okBlock = newBlock("div.ok");
  builder_.CreateCondBr(isZero, panicBlock, okBlock,
                        llvm::MDBuilder(ctx_).createUnlikelyBranchWeights());
  switchTo(panicBlock);
  emitPanic("integer divide by zero");

  switchTo(okBlock);
  llvm::Value *isMinusOne = builder_.CreateICmpEQ(rhs, llvm::ConstantInt::getSigned(type, -1));
  llvm::Value *divisor = builder_.CreateSelect(isMinusOne, llvm::ConstantInt::get(type, 1), rhs);
  if (isRem)
    return builder_.CreateSRem(lhs, divisor); // x % 1 == x % -1 == 0
  llvm::Value *quotient = builder_.CreateSDiv(lhs, divisor);
  return builder_.CreateSelect(isMinusOne, builder_.CreateNeg(lhs), quotient);
}

void CodeGen::emitPanic(llvm::StringRef message) {
  llvm::CallInst *call =
      builder_.CreateCall(rt_.panic, {stringConstant(message), builder_.getInt64(message.size())});
  call->setDoesNotReturn();
  builder_.CreateUnreachable();
}

llvm::Value *CodeGen::emitAssign(AssignExpr *assign) {
  llvm::Value *address = addressOfTarget(assign->target);
  llvm::Value *value;
  if (!assign->op) {
    value = emitExpr(assign->value);
  } else {
    // As in JavaScript, the old value is read before the right side runs.
    llvm::Value *old = builder_.CreateLoad(lower(assign->type), address);
    llvm::Value *rhs = emitExpr(assign->value);
    value = emitArithmetic(*assign->op, old, rhs, assign->type);
  }
  builder_.CreateStore(value, address);
  return value;
}

llvm::Value *CodeGen::emitUpdate(UpdateExpr *update) {
  llvm::Value *address = addressOfTarget(update->target);
  llvm::Type *type = lower(update->type);
  llvm::Value *old = builder_.CreateLoad(type, address);
  llvm::Value *updated;
  if (update->type->isFloat()) {
    llvm::Value *one = llvm::ConstantFP::get(type, 1.0);
    updated = update->increment ? builder_.CreateFAdd(old, one) : builder_.CreateFSub(old, one);
  } else {
    llvm::Value *one = llvm::ConstantInt::get(type, 1);
    updated = update->increment ? builder_.CreateAdd(old, one) : builder_.CreateSub(old, one);
  }
  builder_.CreateStore(updated, address);
  return update->prefix ? updated : old;
}

llvm::Value *CodeGen::emitCall(CallExpr *call) {
  if (call->builtin == Builtin::ConsoleLog) {
    emitConsoleLog(call);
    return nullptr;
  }
  std::vector<llvm::Value *> args;
  for (Expr *arg : call->args)
    args.push_back(emitExpr(arg));
  return builder_.CreateCall(functions_.lookup(call->func), args);
}

// console.log(a, b) prints its arguments separated by spaces, then a newline.
void CodeGen::emitConsoleLog(CallExpr *call) {
  for (size_t i = 0; i < call->args.size(); ++i) {
    if (i > 0)
      builder_.CreateCall(rt_.printChar, {builder_.getInt32(' ')});
    Expr *arg = call->args[i];
    if (auto *lit = dyn_cast<StringLitExpr>(arg)) {
      builder_.CreateCall(rt_.printStr,
                          {stringConstant(lit->value), builder_.getInt64(lit->value.size())});
      continue;
    }
    llvm::Value *value = emitExpr(arg);
    switch (arg->type->kind()) {
    case Type::Kind::Int:
      builder_.CreateCall(rt_.printI32, {value});
      break;
    case Type::Kind::Long:
      builder_.CreateCall(rt_.printI64, {value});
      break;
    case Type::Kind::Double:
      builder_.CreateCall(rt_.printF64, {value});
      break;
    case Type::Kind::Bool:
      builder_.CreateCall(rt_.printBool, {builder_.CreateZExt(value, builder_.getInt32Ty())});
      break;
    default:
      llvm_unreachable("Sema rejects other argument types");
    }
  }
  builder_.CreateCall(rt_.printChar, {builder_.getInt32('\n')});
}

llvm::Value *CodeGen::emitConditional(ConditionalExpr *cond) {
  llvm::Value *test = emitExpr(cond->cond);
  llvm::BasicBlock *thenBlock = newBlock("cond.then");
  llvm::BasicBlock *elseBlock = newBlock("cond.else");
  llvm::BasicBlock *endBlock = newBlock("cond.end");
  builder_.CreateCondBr(test, thenBlock, elseBlock);

  switchTo(thenBlock);
  llvm::Value *thenValue = emitExpr(cond->thenExpr);
  llvm::BasicBlock *thenEnd = builder_.GetInsertBlock();
  builder_.CreateBr(endBlock);

  switchTo(elseBlock);
  llvm::Value *elseValue = emitExpr(cond->elseExpr);
  llvm::BasicBlock *elseEnd = builder_.GetInsertBlock();
  builder_.CreateBr(endBlock);

  switchTo(endBlock);
  if (cond->type->isVoid())
    return nullptr;
  llvm::PHINode *phi = builder_.CreatePHI(lower(cond->type), 2);
  phi->addIncoming(thenValue, thenEnd);
  phi->addIncoming(elseValue, elseEnd);
  return phi;
}

llvm::Value *CodeGen::emitConversion(llvm::Value *value, const Type *from, const Type *to) {
  if (from == to)
    return value;
  llvm::Type *target = lower(to);
  if (from->isInteger() && to->isInteger())
    return from->bitWidth() < to->bitWidth() ? builder_.CreateSExt(value, target)
                                             : builder_.CreateTrunc(value, target);
  if (from->isInteger() && to->isFloat())
    return builder_.CreateSIToFP(value, target);
  if (from->isFloat() && to->isInteger())
    // Saturates instead of being undefined: NaN becomes 0 and out-of-range
    // values clamp to the limits, as in Rust.
    return builder_.CreateIntrinsic(llvm::Intrinsic::fptosi_sat, {target, value->getType()}, {value});
  llvm_unreachable("Sema rejects this conversion");
}

} // namespace tz
