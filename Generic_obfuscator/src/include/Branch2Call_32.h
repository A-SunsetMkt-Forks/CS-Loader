#ifndef LLVM_Generic_obfuscator_Branch2Call_32_H
#define LLVM_Generic_obfuscator_Branch2Call_32_H
//现在仅适配了AArch64架构 因为不同架构之间汇编不一样
#include "llvm/IR/PassManager.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include "llvm/Target/TargetMachine.h" // For TargetMachine
#include "llvm/Support/TargetSelect.h"  // For TargetRegistry::lookupTarget()

using namespace llvm;
namespace llvm
{
  class Branch2Call_32 : public PassInfoMixin<Branch2Call_32>
  {
  public:
    PreservedAnalyses run(Module &F, ModuleAnalysisManager &AM);
    // 保证不被跳过
    static bool isRequired() { return true; }
  };
} // namespace llvm
#endif