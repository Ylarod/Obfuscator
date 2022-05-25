//===- FlatteningIncludes.h - Flattening Obfuscation pass------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains includes and defines for the flattening pass
//
//===----------------------------------------------------------------------===//

#ifndef _FLATTENING_INCLUDES_
#define _FLATTENING_INCLUDES_

#include "llvm/Pass.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Transforms/Obfuscation/CryptoUtils.h"
#include "llvm/Transforms/Obfuscation/IPObfuscationContext.h"
#include "llvm/Transforms/Obfuscation/ObfuscationOptions.h"

namespace llvm {

struct Flattening : public PassInfoMixin<Flattening> {
  bool enable;

  IPObfuscationContext *IPO;
  ObfuscationOptions *Options;
  CryptoUtils RandomEngine;

  Flattening(bool enable, IPObfuscationContext *IPO, ObfuscationOptions *Options) {
    this->enable = enable;
    this->IPO = IPO;
    this->Options = Options;
  }

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM) const;
  bool flatten(Function *f, FunctionAnalysisManager &AM) const;
};
}

#endif

