#include "include/IPObfuscationContext.h"
#include "include/Utils.h"
#include "llvm/IR/AbstractCallSite.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "ipobf"

using namespace llvm;

namespace llvm {

PreservedAnalyses IPObfuscationContext::run(Module &M,
                                            ModuleAnalysisManager &) {
  // find all local functions with local linkage, then append to LocalFunctions
  for (auto &F : M) {
    SurveyFunction(F);
  }

  // alloc secret slot for all local functions
  for (auto &F : M) {
    if (F.isDeclaration()) {
      continue;
    }
    IPOInfo *Info = AllocaSecretSlot(F);

    IPOInfoList.push_back(Info);
    IPOInfoMap[&F] = Info;
  }

  // replace each LocalFunction to a new function with secret argument
  std::vector<Function *> NewFuncs;
  for (auto *F : LocalFunctions) {
    Function *NF = InsertSecretArgument(F);
    NewFuncs.push_back(NF);
  }

  for (auto *F : NewFuncs) {
    computeCallSiteSecretArgument(F);
  }

  // remove dead slots and it's uses
  for (AllocaInst *Slot : DeadSlots) {
    for (Value::use_iterator I = Slot->use_begin(), E = Slot->use_end(); I != E;
         ++I) {
      if (auto *Inst = dyn_cast<Instruction>(I->getUser())) {
        Inst->eraseFromParent();
      }
    }
    Slot->eraseFromParent();
  }
  return PreservedAnalyses::none();
}

void IPObfuscationContext::SurveyFunction(Function &F) {
  if (!F.hasLocalLinkage() || F.isDeclaration()) {
    return; // not local linkage or no primary definition in this module
  }

  for (const Use &U : F.uses()) {
    AbstractCallSite CS(&U);
    if (!CS || !CS.isCallee(&U)) {
      return;
    }

    const Instruction *TheCall = CS.getInstruction();
    if (!TheCall) {
      return;
    }
  }

  LLVM_DEBUG(dbgs() << "Enqueue Local Function  " << F.getName() << "\n");
  LocalFunctions.insert(&F);
}

Function *IPObfuscationContext::InsertSecretArgument(Function *F) {
  FunctionType *FTy = F->getFunctionType();
  std::vector<Type *> Params;
  SmallVector<AttributeSet, 8> ArgAttrVec;

  const AttributeList &PAL = F->getAttributes();

  Params.push_back(Type::getInt32PtrTy(F->getContext()));
  ArgAttrVec.push_back(AttributeSet());

  unsigned int i = 0;
  for (Function::arg_iterator I = F->arg_begin(), E = F->arg_end(); I != E;
       ++I, ++i) {
    Params.push_back(I->getType());
    ArgAttrVec.push_back(PAL.getParamAttrs(i));
  }

  // Find out the new return value.
  Type *RetTy = FTy->getReturnType();

  // The existing function return attributes.
  AttributeSet RAttrs = PAL.getRetAttrs();

  // Reconstruct the AttributesList based on the vector we constructed.
  AttributeList NewPAL =
      AttributeList::get(F->getContext(), PAL.getFnAttrs(), RAttrs, ArgAttrVec);

  // Create the new function type based on the recomputed parameters.
  FunctionType *NFTy = FunctionType::get(RetTy, Params, FTy->isVarArg());

  // Create the new function body and insert it into the module...
  Function *NF = Function::Create(NFTy, F->getLinkage());
  NF->copyAttributesFrom(F);
  NF->setComdat(F->getComdat());
  NF->setAttributes(NewPAL);
  // Insert the new function before the old function, so we won't be processing
  // it again.
  F->getParent()->getFunctionList().insert(F->getIterator(), NF);
  NF->takeName(F);
  NF->setSubprogram(F->getSubprogram());

  SmallVector<Value *, 8> Args;
  while (!F->use_empty()) {
    AbstractCallSite CS(&*F->materialized_use_begin());
    Instruction *Call = CS.getInstruction();

    ArgAttrVec.clear();
    const AttributeList &CallPAL = CS.getInstruction()->getAttributes();

    // Get the Secret Token
    Function *Caller = Call->getParent()->getParent();
    IPOInfo *SecretInfo = IPOInfoMap[Caller];
    Args.push_back(SecretInfo->CalleeSlot);
    ArgAttrVec.push_back(AttributeSet());
    // Declare these outside of the loops, so we can reuse them for the second
    // loop, which loops the varargs.
    auto *I = CS.getInstruction()->arg_begin();
    i = 0;
    // Loop over those operands, corresponding to the normal arguments to the
    // original function, and add those that are still alive.
    for (unsigned e = FTy->getNumParams(); i != e; ++I, ++i) {
      Args.push_back(*I);
      AttributeSet Attrs = CallPAL.getParamAttrs(i);
      ArgAttrVec.push_back(Attrs);
    }

    // Push any varargs arguments on the list. Don't forget their attributes.
    for (auto *E = CS.getInstruction()->arg_end(); I != E; ++I, ++i) {
      Args.push_back(*I);
      ArgAttrVec.push_back(CallPAL.getParamAttrs(i));
    }

    // Reconstruct the AttributesList based on the vector we constructed.
    AttributeList NewCallPAL =
        AttributeList::get(F->getContext(), CallPAL.getFnAttrs(),
                           CallPAL.getRetAttrs(), ArgAttrVec);

    Instruction *New;
    if (auto *II = dyn_cast<InvokeInst>(Call)) {
      New = InvokeInst::Create(NF, II->getNormalDest(), II->getUnwindDest(),
                               Args, "", Call);
      cast<InvokeInst>(New)->setCallingConv(
          CS.getInstruction()->getCallingConv());
      cast<InvokeInst>(New)->setAttributes(NewCallPAL);
    } else {
      New = CallInst::Create(NF, Args, "", Call);
      cast<CallInst>(New)->setCallingConv(
          CS.getInstruction()->getCallingConv());
      cast<CallInst>(New)->setAttributes(NewCallPAL);
      if (cast<CallInst>(Call)->isTailCall())
        cast<CallInst>(New)->setTailCall();
    }
    New->setDebugLoc(Call->getDebugLoc());

    Args.clear();

    if (!Call->use_empty()) {
      Call->replaceAllUsesWith(New);
      New->takeName(Call);
    }

    // Finally, remove the old call from the program, reducing the use-count of
    // F.
    Call->eraseFromParent();
  }

  NF->getBasicBlockList().splice(NF->begin(), F->getBasicBlockList());

  // Loop over the argument list, transferring uses of the old arguments over to
  // the new arguments, also transferring over the names as well.
  Function::arg_iterator I2 = NF->arg_begin();
  I2->setName("SecretArg");
  ++I2;
  for (Function::arg_iterator I = F->arg_begin(), E = F->arg_end(); I != E;
       ++I) {
    I->replaceAllUsesWith(I2);
    I2->takeName(I);
    ++I2;
  }

  // Load Secret Token from the secret argument
  IntegerType *I32Ty = Type::getInt32Ty(NF->getContext());
  IRBuilder<> IRB(&NF->getEntryBlock().front());
  Value *Ptr = IRB.CreateBitCast(NF->arg_begin(), I32Ty->getPointerTo());
  LoadInst *MySecret = IRB.CreateLoad(I32Ty->getPointerTo(), Ptr);

  IPOInfo *Info = IPOInfoMap[F];
  Info->SecretLI->eraseFromParent();
  Info->SecretLI = MySecret;
  DeadSlots.push_back(Info->CallerSlot);

  IPOInfoMap[NF] = Info;
  IPOInfoMap.erase(F);

  F->eraseFromParent();

  return NF;
}

// Create StackSlots for Secrets and a LoadInst for caller's secret slot
IPObfuscationContext::IPOInfo *
IPObfuscationContext::AllocaSecretSlot(Function &F) {
  IRBuilder<> IRB(&F.getEntryBlock().front());
  IntegerType *I32Ty = Type::getInt32Ty(F.getContext());
  AllocaInst *CallerSlot = IRB.CreateAlloca(I32Ty, nullptr, "CallerSlot");
  CallerSlot->setAlignment(Align(4));
  AllocaInst *CalleeSlot = IRB.CreateAlloca(I32Ty, nullptr, "CalleeSlot");
  CalleeSlot->setAlignment(Align(4));
  uint32_t V = RandomEngine.get_uint32_t();
  ConstantInt *SecretCI = ConstantInt::get(I32Ty, V, false);
  IRB.CreateStore(SecretCI, CallerSlot);
  LoadInst *MySecret = IRB.CreateLoad(I32Ty, CallerSlot, "MySecret");

  return new IPOInfo(CallerSlot, CalleeSlot, MySecret, SecretCI);
}

const IPObfuscationContext::IPOInfo *
IPObfuscationContext::getIPOInfo(Function *F) {
  return IPOInfoMap[F];
}

// at each call site, compute the callee's secret argument using the caller's
void IPObfuscationContext::computeCallSiteSecretArgument(Function *F) {
  IPOInfo *CalleeIPOInfo = IPOInfoMap[F];

  for (const Use &U : F->uses()) {
    AbstractCallSite CS(&U);
    Instruction *Call = CS.getInstruction();
    IRBuilder<> IRB(Call);

    Function *Caller = Call->getParent()->getParent();
    IPOInfo *CallerIPOInfo = IPOInfoMap[Caller];

    Value *CallerSecret;
    CallerSecret = CallerIPOInfo->SecretLI;

    // CalleeSecret = CallerSecret - (CallerSecretInt - CalleeSecretInt)
    // X = CallerSecretInt - CalleeSecretInt
    Constant *X =
        ConstantExpr::getSub(CallerIPOInfo->SecretCI, CalleeIPOInfo->SecretCI);
    Value *CalleeSecret = IRB.CreateSub(CallerSecret, X);
    IRB.CreateStore(CalleeSecret, CallerIPOInfo->CalleeSlot);
  }
}

IPObfuscationContext::IPObfuscationContext(bool enable,
                                           const std::string &seed) {
  this->enable = enable;
  RandomEngine.prng_seed(seed);
}
} // namespace llvm
