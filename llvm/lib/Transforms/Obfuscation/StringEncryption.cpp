#include "include/StringEncryption.h"
#include "include/CryptoUtils.h"
#include "include/IPObfuscationContext.h"
#include "include/ObfuscationOptions.h"
#include "include/Utils.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/GlobalStatus.h"
#include <algorithm>
#include <iostream>
#include <map>
#include <set>

#define DEBUG_TYPE "string-encryption"

using namespace llvm;

PreservedAnalyses StringEncryption::run(Module &M, ModuleAnalysisManager &){
  std::set<GlobalVariable *> ConstantStringUsers;

  // collect all c strings
  LLVMContext &Ctx = M.getContext();
  ConstantInt *Zero = ConstantInt::get(Type::getInt32Ty(Ctx), 0);
  for (GlobalVariable &GV : M.globals()) {
    if (!GV.isConstant() || !GV.hasInitializer()) {
      continue;
    }
    Constant *Init = GV.getInitializer();
    if (Init == nullptr)
      continue;
    if (ConstantDataSequential *CDS = dyn_cast<ConstantDataSequential>(Init)) {
      if (isCString(CDS)) {
        CSPEntry *Entry = new CSPEntry();
        StringRef Data = CDS->getRawDataValues();
        Entry->Data.reserve(Data.size());
        for (char i : Data) {
          Entry->Data.push_back(static_cast<uint8_t>(i));
        }
        Entry->ID = static_cast<unsigned>(ConstantStringPool.size());
        ConstantAggregateZero *ZeroInit =
            ConstantAggregateZero::get(CDS->getType());
        GlobalVariable *DecGV = new GlobalVariable(
            M, CDS->getType(), false, GlobalValue::PrivateLinkage, ZeroInit,
            "dec" + Twine::utohexstr(Entry->ID) + GV.getName());
        GlobalVariable *DecStatus = new GlobalVariable(
            M, Type::getInt32Ty(Ctx), false, GlobalValue::PrivateLinkage, Zero,
            "dec_status_" + Twine::utohexstr(Entry->ID) + GV.getName());
        DecGV->setAlignment(MaybeAlign(GV.getAlignment()));
        Entry->DecGV = DecGV;
        Entry->DecStatus = DecStatus;
        ConstantStringPool.push_back(Entry);
        CSPEntryMap[&GV] = Entry;
        collectConstantStringUser(&GV, ConstantStringUsers);
      }
    }
  }

  // encrypt those strings, build corresponding decrypt function
  for (CSPEntry *Entry : ConstantStringPool) {
    getRandomBytes(Entry->EncKey, 16, 32);
    for (unsigned i = 0; i < Entry->Data.size(); ++i) {
      Entry->Data[i] ^= Entry->EncKey[i % Entry->EncKey.size()];
    }
    Entry->DecFunc = buildDecryptFunction(&M, Entry);
  }

  // build initialization function for supported constant string users
  for (GlobalVariable *GV : ConstantStringUsers) {
    if (isValidToEncrypt(GV)) {
      Type *EltType = GV->getType()->getElementType();
      GlobalVariable *DecGV = new GlobalVariable(
          M, EltType, false, GlobalValue::PrivateLinkage,
          Constant::getNullValue(EltType), "dec_" + GV->getName());
      DecGV->setAlignment(MaybeAlign(GV->getAlignment()));
      GlobalVariable *DecStatus = new GlobalVariable(
          M, Type::getInt32Ty(Ctx), false, GlobalValue::PrivateLinkage, Zero,
          "dec_status_" + GV->getName());
      CSUser *User = new CSUser(GV, DecGV);
      User->DecStatus = DecStatus;
      User->InitFunc = buildInitFunction(&M, User);
      CSUserMap[GV] = User;
    }
  }

  // emit the constant string pool
  // | junk bytes | key 1 | encrypted string 1 | junk bytes | key 2 | encrypted string 2 | ...
  std::vector<uint8_t> Data;
  std::vector<uint8_t> JunkBytes;

  JunkBytes.reserve(32);
  for (CSPEntry *Entry : ConstantStringPool) {
    JunkBytes.clear();
    getRandomBytes(JunkBytes, 16, 32);
    Data.insert(Data.end(), JunkBytes.begin(), JunkBytes.end());
    Entry->Offset = static_cast<unsigned>(Data.size());
    Data.insert(Data.end(), Entry->EncKey.begin(), Entry->EncKey.end());
    Data.insert(Data.end(), Entry->Data.begin(), Entry->Data.end());
  }

  Constant *CDA =
      ConstantDataArray::get(M.getContext(), ArrayRef<uint8_t>(Data));
  EncryptedStringTable =
      new GlobalVariable(M, CDA->getType(), true, GlobalValue::PrivateLinkage,
                         CDA, "EncryptedStringTable");

  // decrypt string back at every use, change the plain string use to the decrypted one
  bool Changed = false;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    Changed |= processConstantStringUse(&F);
  }

  for (auto &I : CSUserMap) {
    CSUser *User = I.second;
    Changed |= processConstantStringUse(User->InitFunc);
  }

  // delete unused global variables
  deleteUnusedGlobalVariable();
  for (CSPEntry *Entry : ConstantStringPool) {
    if (Entry->DecFunc->use_empty()) {
      Entry->DecFunc->eraseFromParent();
      Entry->DecGV->eraseFromParent();
      Entry->DecStatus->eraseFromParent();
    }
  }
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

void StringEncryption::getRandomBytes(std::vector<uint8_t> &Bytes,
                                      uint32_t MinSize, uint32_t MaxSize) {
  uint32_t N = IPO->RandomEngine.get_uint32_t();
  uint32_t Len;

  assert(MaxSize >= MinSize);

  if (MinSize == MaxSize) {
    Len = MinSize;
  } else {
    Len = MinSize + (N % (MaxSize - MinSize));
  }

  char *Buffer = new char[Len];
  IPO->RandomEngine.get_bytes(Buffer, Len);
  for (uint32_t i = 0; i < Len; ++i) {
    Bytes.push_back(static_cast<uint8_t>(Buffer[i]));
  }

  delete[] Buffer;
}

//
// static void goron_decrypt_string(uint8_t *plain_string, const uint8_t *data)
//{
//  const uint8_t *key = data;
//  uint32_t key_size = 1234;
//  uint8_t *es = (uint8_t *) &data[key_size];
//  uint32_t i;
//  for (i = 0;i < 5678;i ++) {
//    plain_string[i] = es[i] ^ key[i % key_size];
//  }
//}

Function *StringEncryption::buildDecryptFunction(
    Module *M, const StringEncryption::CSPEntry *Entry) {
  LLVMContext &Ctx = M->getContext();
  IRBuilder<> IRB(Ctx);
  FunctionType *FuncTy = FunctionType::get(
      Type::getVoidTy(Ctx), {IRB.getInt8PtrTy(), IRB.getInt8PtrTy()}, false);
  Function *DecFunc = Function::Create(
      FuncTy, GlobalValue::PrivateLinkage,
      "goron_decrypt_string_" + Twine::utohexstr(Entry->ID), M);

  auto ArgIt = DecFunc->arg_begin();
  Argument *PlainString = ArgIt; // output
  ++ArgIt;
  Argument *Data = ArgIt; // input

  PlainString->setName("plain_string");
  PlainString->addAttr(Attribute::NoCapture);
  Data->setName("data");
  Data->addAttr(Attribute::NoCapture);
  Data->addAttr(Attribute::ReadOnly);

  BasicBlock *Enter = BasicBlock::Create(Ctx, "Enter", DecFunc);
  BasicBlock *LoopBody = BasicBlock::Create(Ctx, "LoopBody", DecFunc);
  BasicBlock *UpdateDecStatus =
      BasicBlock::Create(Ctx, "UpdateDecStatus", DecFunc);
  BasicBlock *Exit = BasicBlock::Create(Ctx, "Exit", DecFunc);

  IRB.SetInsertPoint(Enter);
  ConstantInt *KeySize =
      ConstantInt::get(Type::getInt32Ty(Ctx), Entry->EncKey.size());
  Value *EncPtr = IRB.CreateInBoundsGEP(Data, KeySize);
  Value *DecStatus = IRB.CreateLoad(Entry->DecStatus);
  Value *IsDecrypted = IRB.CreateICmpEQ(DecStatus, IRB.getInt32(1));
  IRB.CreateCondBr(IsDecrypted, Exit, LoopBody);

  IRB.SetInsertPoint(LoopBody);
  PHINode *LoopCounter = IRB.CreatePHI(IRB.getInt32Ty(), 2);
  LoopCounter->addIncoming(IRB.getInt32(0), Enter);

  Value *EncCharPtr = IRB.CreateInBoundsGEP(EncPtr, LoopCounter);
  Value *EncChar = IRB.CreateLoad(EncCharPtr);
  Value *KeyIdx = IRB.CreateURem(LoopCounter, KeySize);

  Value *KeyCharPtr = IRB.CreateInBoundsGEP(Data, KeyIdx);
  Value *KeyChar = IRB.CreateLoad(KeyCharPtr);

  Value *DecChar = IRB.CreateXor(EncChar, KeyChar);
  Value *DecCharPtr = IRB.CreateInBoundsGEP(PlainString, LoopCounter);
  IRB.CreateStore(DecChar, DecCharPtr);

  Value *NewCounter =
      IRB.CreateAdd(LoopCounter, IRB.getInt32(1), "", true, true);
  LoopCounter->addIncoming(NewCounter, LoopBody);

  Value *Cond = IRB.CreateICmpEQ(
      NewCounter, IRB.getInt32(static_cast<uint32_t>(Entry->Data.size())));
  IRB.CreateCondBr(Cond, UpdateDecStatus, LoopBody);

  IRB.SetInsertPoint(UpdateDecStatus);
  IRB.CreateStore(IRB.getInt32(1), Entry->DecStatus);
  IRB.CreateBr(Exit);

  IRB.SetInsertPoint(Exit);
  IRB.CreateRetVoid();

  return DecFunc;
}

Function *
StringEncryption::buildInitFunction(Module *M,
                                    const StringEncryption::CSUser *User) {
  LLVMContext &Ctx = M->getContext();
  IRBuilder<> IRB(Ctx);
  FunctionType *FuncTy =
      FunctionType::get(Type::getVoidTy(Ctx), {User->DecGV->getType()}, false);
  Function *InitFunc =
      Function::Create(FuncTy, GlobalValue::PrivateLinkage,
                       "global_variable_init_" + User->GV->getName(), M);

  auto ArgIt = InitFunc->arg_begin();
  Argument *thiz = ArgIt;

  thiz->setName("this");
  thiz->addAttr(Attribute::NoCapture);

  BasicBlock *Enter = BasicBlock::Create(Ctx, "Enter", InitFunc);
  BasicBlock *InitBlock = BasicBlock::Create(Ctx, "InitBlock", InitFunc);
  BasicBlock *Exit = BasicBlock::Create(Ctx, "Exit", InitFunc);

  IRB.SetInsertPoint(Enter);
  Value *DecStatus = IRB.CreateLoad(User->DecStatus);
  Value *IsDecrypted = IRB.CreateICmpEQ(DecStatus, IRB.getInt32(1));
  IRB.CreateCondBr(IsDecrypted, Exit, InitBlock);

  IRB.SetInsertPoint(InitBlock);
  Constant *Init = User->GV->getInitializer();

  // convert constant initializer into a series of instructions
  lowerGlobalConstant(Init, IRB, User->DecGV);

  if (isObjCSelectorPtr(User->GV)) {
    // resolve selector
    Function *sel_registerName = (Function *)M->getOrInsertFunction(
        "sel_registerName",
        FunctionType::get(IRB.getInt8PtrTy(), {IRB.getInt8PtrTy()}, false)).getCallee();
    Value *Selector = IRB.CreateCall(sel_registerName, {Init});
    IRB.CreateStore(Selector, User->DecGV);
  }

  IRB.CreateStore(IRB.getInt32(1), User->DecStatus);
  IRB.CreateBr(Exit);

  IRB.SetInsertPoint(Exit);
  IRB.CreateRetVoid();
  return InitFunc;
}

void StringEncryption::lowerGlobalConstant(Constant *CV, IRBuilder<> &IRB,
                                           Value *Ptr) {
  if (isa<ConstantAggregateZero>(CV)) {
    IRB.CreateStore(CV, Ptr);
    return;
  }

  if (ConstantArray *CA = dyn_cast<ConstantArray>(CV)) {
    lowerGlobalConstantArray(CA, IRB, Ptr);
  } else if (ConstantStruct *CS = dyn_cast<ConstantStruct>(CV)) {
    lowerGlobalConstantStruct(CS, IRB, Ptr);
  } else {
    IRB.CreateStore(CV, Ptr);
  }
}

void StringEncryption::lowerGlobalConstantArray(ConstantArray *CA,
                                                IRBuilder<> &IRB, Value *Ptr) {
  for (unsigned i = 0, e = CA->getNumOperands(); i != e; ++i) {
    Constant *CV = CA->getOperand(i);
    Value *GEP = IRB.CreateGEP(Ptr, {IRB.getInt32(0), IRB.getInt32(i)});
    lowerGlobalConstant(CV, IRB, GEP);
  }
}

void StringEncryption::lowerGlobalConstantStruct(ConstantStruct *CS,
                                                 IRBuilder<> &IRB, Value *Ptr) {
  for (unsigned i = 0, e = CS->getNumOperands(); i != e; ++i) {
    Value *GEP = IRB.CreateGEP(Ptr, {IRB.getInt32(0), IRB.getInt32(i)});
    lowerGlobalConstant(CS->getOperand(i), IRB, GEP);
  }
}

bool StringEncryption::processConstantStringUse(Function *F) {
  if (!toObfuscate(enable, F, "cse")) {
    return false;
  }
  if (Options && Options->skipFunction(F->getName())) {
    return false;
  }
  LowerConstantExpr(*F);
  SmallPtrSet<GlobalVariable *, 16>
      DecryptedGV; // if GV has multiple use in a block, decrypt only at the first use
  bool Changed = false;
  for (BasicBlock &BB : *F) {
    DecryptedGV.clear();
    for (Instruction &Inst : BB) {
      if (PHINode *PHI = dyn_cast<PHINode>(&Inst)) {
        for (unsigned int i = 0; i < PHI->getNumIncomingValues(); ++i) {
          if (GlobalVariable *GV =
                  dyn_cast<GlobalVariable>(PHI->getIncomingValue(i))) {
            auto Iter1 = CSPEntryMap.find(GV);
            auto Iter2 = CSUserMap.find(GV);
            if (Iter2 != CSUserMap.end()) { // GV is a constant string user
              CSUser *User = Iter2->second;
              if (DecryptedGV.count(GV) > 0) {
                Inst.replaceUsesOfWith(GV, User->DecGV);
              } else {
                Instruction *InsertPoint =
                    PHI->getIncomingBlock(i)->getTerminator();
                IRBuilder<> IRB(InsertPoint);
                IRB.CreateCall(User->InitFunc, {User->DecGV});
                Inst.replaceUsesOfWith(GV, User->DecGV);
                MaybeDeadGlobalVars.insert(GV);
                DecryptedGV.insert(GV);
                Changed = true;
              }
            } else if (Iter1 != CSPEntryMap.end()) { // GV is a constant string
              CSPEntry *Entry = Iter1->second;
              if (DecryptedGV.count(GV) > 0) {
                Inst.replaceUsesOfWith(GV, Entry->DecGV);
              } else {
                Instruction *InsertPoint =
                    PHI->getIncomingBlock(i)->getTerminator();
                IRBuilder<> IRB(InsertPoint);

                Value *OutBuf =
                    IRB.CreateBitCast(Entry->DecGV, IRB.getInt8PtrTy());
                Value *Data = IRB.CreateInBoundsGEP(
                    EncryptedStringTable,
                    {IRB.getInt32(0), IRB.getInt32(Entry->Offset)});
                IRB.CreateCall(Entry->DecFunc, {OutBuf, Data});

                Inst.replaceUsesOfWith(GV, Entry->DecGV);
                MaybeDeadGlobalVars.insert(GV);
                DecryptedGV.insert(GV);
                Changed = true;
              }
            }
          }
        }
      } else {
        for (User::op_iterator op = Inst.op_begin(); op != Inst.op_end();
             ++op) {
          if (GlobalVariable *GV = dyn_cast<GlobalVariable>(*op)) {
            auto Iter1 = CSPEntryMap.find(GV);
            auto Iter2 = CSUserMap.find(GV);
            if (Iter2 != CSUserMap.end()) {
              CSUser *User = Iter2->second;
              if (DecryptedGV.count(GV) > 0) {
                Inst.replaceUsesOfWith(GV, User->DecGV);
              } else {
                IRBuilder<> IRB(&Inst);
                IRB.CreateCall(User->InitFunc, {User->DecGV});
                Inst.replaceUsesOfWith(GV, User->DecGV);
                MaybeDeadGlobalVars.insert(GV);
                DecryptedGV.insert(GV);
                Changed = true;
              }
            } else if (Iter1 != CSPEntryMap.end()) {
              CSPEntry *Entry = Iter1->second;
              if (DecryptedGV.count(GV) > 0) {
                Inst.replaceUsesOfWith(GV, Entry->DecGV);
              } else {
                IRBuilder<> IRB(&Inst);

                Value *OutBuf =
                    IRB.CreateBitCast(Entry->DecGV, IRB.getInt8PtrTy());
                Value *Data = IRB.CreateInBoundsGEP(
                    EncryptedStringTable,
                    {IRB.getInt32(0), IRB.getInt32(Entry->Offset)});
                IRB.CreateCall(Entry->DecFunc, {OutBuf, Data});

                Inst.replaceUsesOfWith(GV, Entry->DecGV);
                MaybeDeadGlobalVars.insert(GV);
                DecryptedGV.insert(GV);
                Changed = true;
              }
            }
          }
        }
      }
    }
  }
  return Changed;
}

void StringEncryption::collectConstantStringUser(
    GlobalVariable *CString, std::set<GlobalVariable *> &Users) {
  SmallPtrSet<Value *, 16> Visited;
  SmallVector<Value *, 16> ToVisit;

  ToVisit.push_back(CString);
  while (!ToVisit.empty()) {
    Value *V = ToVisit.pop_back_val();
    if (Visited.count(V) > 0)
      continue;
    Visited.insert(V);
    for (Value *User : V->users()) {
      if (auto *GV = dyn_cast<GlobalVariable>(User)) {
        Users.insert(GV);
      } else {
        ToVisit.push_back(User);
      }
    }
  }
}

bool StringEncryption::isValidToEncrypt(GlobalVariable *GV) {
  if (!GV->hasInitializer()) {
    return false;
  }
  if (GV->isConstant()) {
    return true;
  } else if (isCFConstantStringTag(GV) || isObjCSelectorPtr(GV)) {
    return true;
  } else {
    return false;
  }
}

bool StringEncryption::isCString(const ConstantDataSequential *CDS) {
  // isString
  if (!isa<ArrayType>(CDS->getType()))
    return false;
  if (!CDS->getElementType()->isIntegerTy(8) &&
      !CDS->getElementType()->isIntegerTy(16) &&
      !CDS->getElementType()->isIntegerTy(32))
    return false;

  for (unsigned i = 0, e = CDS->getNumElements(); i != e; ++i) {
    uint64_t Elt = CDS->getElementAsInteger(i);
    if (Elt == 0) {
      return i == (e - 1); // last element is null
    }
  }
  return false; // null not found
}

bool StringEncryption::isObjCSelectorPtr(const GlobalVariable *GV) {
  return GV->isExternallyInitialized() && GV->hasLocalLinkage() &&
         GV->getName().startswith("OBJC_SELECTOR_REFERENCES_");
}

bool StringEncryption::isCFConstantStringTag(const GlobalVariable *GV) {
  Type *ETy = GV->getType()->getElementType();
  return ETy->isStructTy() &&
         ETy->getStructName() == "struct.__NSConstantString_tag";
}

void StringEncryption::deleteUnusedGlobalVariable() {
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (auto Iter = MaybeDeadGlobalVars.begin();
         Iter != MaybeDeadGlobalVars.end();) {
      GlobalVariable *GV = *Iter;
      if (!GV->hasLocalLinkage()) {
        ++Iter;
        continue;
      }

      GV->removeDeadConstantUsers();
      if (GV->use_empty()) {
        if (GV->hasInitializer()) {
          Constant *Init = GV->getInitializer();
          GV->setInitializer(nullptr);
          if (isSafeToDestroyConstant(Init))
            Init->destroyConstant();
        }
        Iter = MaybeDeadGlobalVars.erase(Iter);
        GV->eraseFromParent();
        Changed = true;
      } else {
        ++Iter;
      }
    }
  }
}
