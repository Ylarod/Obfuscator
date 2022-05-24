//
// Created by Ylarod on 2022/5/24.
//

#include "llvm/Transforms/Obfuscation/HelloWorld.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

PreservedAnalyses HelloWorld::run(Module &M, ModuleAnalysisManager &) {
  if (enable) {
    errs() << "Module name is " << M.getName() << "!\n";
  }
  return PreservedAnalyses::all();
}

PreservedAnalyses HelloWorld::run(Function &F, FunctionAnalysisManager &) {
  if (enable) {
    errs() << "Function name is " << F.getName() << "!\n";
  }
  return PreservedAnalyses();
}
