// ![Fix Me]: It is possible that multiple decryptions may occur due to concurrency bugs. You need to pay attention when concurrency exists. -- Solved via LLVM native atomic operations
#include "GVEncrypt.h"
#include "config.h"
#include "utils.hpp"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/Alignment.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include "Log.hpp"
std::set<llvm::GlobalVariable*> needEncGV;
int needEncGV_count = 0;
std::map<llvm::GlobalVariable*, Generic_obfuscator::GVEncrypt::GVInfo> needEncGVInfos;
std::set<llvm::GlobalVariable*> GVUsedByFunc;
bool Generic_obfuscator::GVEncrypt::shouldSkip(GlobalVariable& GV)
{
    if (GV.getName().startswith("llvm.")) {
        return true;
    }
    if (!GV.hasInternalLinkage() && !GV.hasPrivateLinkage()) {
        return true;
    }
    if (!GV.getValueType()->isIntegerTy() && (!GV.getValueType()->isArrayTy() || !cast<ArrayType>(GV.getValueType())->getElementType()->isIntegerTy())) {
        return true;
    }
    if (!GV.hasInitializer() || !GV.getInitializer()) {
        return true;
    }
    // Make sure the GV doesn't belong to any custom section (which means it belongs .data section by default)
    // We conservatively skip data in custom section to avoid unexpected behaviors after obfuscation
    if (GV.hasSection()) {
        return true;
    }
    llvm::Constant* initializer = GV.getInitializer();
    if (llvm::isa<llvm::ConstantAggregateZero>(initializer)){
        return true;
    }
    return false;
}

Function* Generic_obfuscator::GVEncrypt::defineDecryptFunction(Module* M, GlobalVariable* GVIsDecrypted)
{
    std::vector<Type*> Params;
    Params.push_back(Type::getInt32Ty(M->getContext())); // index
    Params.push_back(Type::getInt8Ty(M->getContext())); // key
    Params.push_back(Type::getInt32Ty(M->getContext())); // len
    Params.push_back(Type::getInt8Ty(M->getContext())->getPointerTo()); // GV
    FunctionType* FT = FunctionType::get(Type::getVoidTy(M->getContext()), Params, false);
    Function* F = Function::Create(FT, GlobalValue::PrivateLinkage, "_Generic_obfuscator_decryptGV", M);
    BasicBlock* entryBB = BasicBlock::Create(M->getContext(), "entry", F);
    BasicBlock* bodyBB = BasicBlock::Create(M->getContext(), "body", F);
    BasicBlock* endBB = BasicBlock::Create(M->getContext(), "end", F);
    BasicBlock* forCond = BasicBlock::Create(M->getContext(), "for.cond", F);
    BasicBlock* forBody = BasicBlock::Create(M->getContext(), "for.body", F);
    BasicBlock* forInc = BasicBlock::Create(M->getContext(), "for.inc", F);
    Function::arg_iterator iter = F->arg_begin();
    Value* index = iter;
    Value* key = ++iter;
    Value* len = ++iter;
    Value* gvData = ++iter;

    // entryBB
    IRBuilder<> builder(entryBB);
    AllocaInst* arrayIndexPtr = builder.CreateAlloca(Type::getInt32Ty(M->getContext()));
    builder.CreateStore(ConstantInt::get(Type::getInt32Ty(M->getContext()), 0), arrayIndexPtr);
    Type* elemType = GVIsDecrypted->getValueType()->getArrayElementType();
    Value* elementPtr = builder.CreateInBoundsGEP(elemType, GVIsDecrypted, index);

    // Atomic option
    Type* i8Ty = Type::getInt8Ty(M->getContext());
    Value* expected = ConstantInt::get(i8Ty, 0);
    Value* desired = ConstantInt::get(i8Ty, 1);
    AtomicCmpXchgInst* cmpxchg = builder.CreateAtomicCmpXchg(
        elementPtr, expected, desired,
        MaybeAlign(), // 根据实际情况调整对齐
        AtomicOrdering::AcquireRelease,
        AtomicOrdering::Acquire);
    Value* cond = builder.CreateExtractValue(cmpxchg, 1);
    builder.CreateCondBr(cond, forCond, endBB);

    // forCond
    builder.SetInsertPoint(forCond);
    LoadInst* arrayIndex = builder.CreateLoad(Type::getInt32Ty(M->getContext()), arrayIndexPtr);
    Value* loopCond = builder.CreateICmpSLT(arrayIndex, len);
    builder.CreateCondBr(loopCond, forBody, bodyBB);

    // forBody
    builder.SetInsertPoint(forBody);
    Value* dataPtr = builder.CreateGEP(builder.getInt8Ty(), gvData, arrayIndex);
    Value* dataByte = builder.CreateLoad(builder.getInt8Ty(), dataPtr);
    Value* decValue = builder.CreateXor(key, dataByte);
    builder.CreateStore(decValue, dataPtr);
    builder.CreateBr(forInc);

    // forInc
    builder.SetInsertPoint(forInc);
    builder.CreateStore(builder.CreateAdd(arrayIndex, ConstantInt::get(Type::getInt32Ty(M->getContext()), 1)), arrayIndexPtr);
    builder.CreateBr(forCond);

    // bodyBB
    builder.SetInsertPoint(bodyBB);
    builder.CreateStore(ConstantInt::get(Type::getInt1Ty(M->getContext()), 1), elementPtr);
    builder.CreateBr(endBB);

    // endBB
    builder.SetInsertPoint(endBB);
    builder.CreateRetVoid();
    return F;
}

bool Generic_obfuscator::GVEncrypt::encryptGV(llvm::Function* F,  Function* decryptFunction)
{

    for (auto& BB : *F) {
        for (auto& I : BB) {
            std::set<Value*> opValSet;
            if (auto* op = dyn_cast<Instruction>(&I)) {
                for (unsigned i = 0; i < op->getNumOperands(); ++i) {
                    auto* operand = op->getOperand(i);
                    if (operand)
                        opValSet.insert(operand);
                }
            }
            for (auto* opVal : opValSet) {
                if (isa<GlobalVariable>(opVal)) {
                    auto* GV = cast<GlobalVariable>(opVal);
                    if (needEncGV.find(GV) != needEncGV.end()) {
                        GVUsedByFunc.insert(GV);
                    }
                }
            }
        }
    }
    IRBuilder<> builder(&*F->getEntryBlock().getFirstInsertionPt());
    for (GlobalVariable* GV : GVUsedByFunc) {
        std::vector<Value*> params;
        params.push_back(ConstantInt::get(Type::getInt32Ty(F->getContext()), needEncGVInfos[GV].index));
        params.push_back(ConstantInt::get(Type::getInt8Ty(F->getContext()), needEncGVInfos[GV].key));
        params.push_back(ConstantInt::get(Type::getInt32Ty(F->getContext()), needEncGVInfos[GV].len));
        Value* gvAsUInt8Ptr = builder.CreateBitCast(GV, PointerType::get(Type::getInt8Ty(F->getContext()), 0));
        params.push_back(gvAsUInt8Ptr);
        builder.CreateCall(decryptFunction, params);
    }
    // F->print(llvm::outs());
    return true;
}
static void encryptGvData(uint8_t* data, uint8_t key, int32_t len)
{
    for (int64_t i = 0; i < len; i++) {
        data[i] ^= key;
    }
}
PreservedAnalyses GVEncrypt::run(Module& M, ModuleAnalysisManager& AM)
{
    readConfig("/home/zzzccc/cxzz/Generic_obfuscator/config/config.json");
    bool is_processed = false;
    const DataLayout& DL = M.getDataLayout();
    if (gvEncrypt.model) {
        for (auto& GV : M.globals()) {
            if (!Generic_obfuscator::GVEncrypt::shouldSkip(GV) && needEncGV.find(&GV) == needEncGV.end()) {
                needEncGV.insert(&GV);
                uint32_t size = DL.getTypeAllocSize(GV.getValueType());
                Generic_obfuscator::GVEncrypt::GVInfo new_gv_info;
                new_gv_info.index = needEncGV_count++;
                new_gv_info.key = (uint8_t)getRandomNumber();
                new_gv_info.len = size;
                needEncGVInfos[&GV] = new_gv_info;
                if (GV.getValueType()->isIntegerTy()) {
                    ConstantInt* CI = (ConstantInt*)GV.getInitializer();
                    uint64_t V = CI->getZExtValue();
                    encryptGvData((uint8_t*)&V, needEncGVInfos[&GV].key, size);
                    GV.setInitializer(ConstantInt::get(GV.getValueType(), V));
                } else {
                    ConstantDataArray* CA = (ConstantDataArray*)GV.getInitializer();
                    const char* gvData = (const char*)CA->getRawDataValues().data();
                    char* tmp = new char[size];
                    memcpy(tmp, gvData, size);
                    encryptGvData((uint8_t*)tmp, needEncGVInfos[&GV].key, size);
                    GV.setConstant(false);
                    GV.setInitializer(ConstantDataArray::getRaw(StringRef((char*)tmp, size),
                    CA->getNumElements(),
                    CA->getElementType()));
                }
            }
        }
        // Initialize the global array to determine whether it has been decrypted
        std::vector<Constant*> Values(needEncGV_count);
        std::string globalName = M.getName().str() + "_isDecrypted";
        llvm::Module* module = &M;
        ArrayType* AT = ArrayType::get(
            Type::getInt8Ty(M.getContext()), needEncGV_count);
        GlobalVariable* GVIsDecrypted = (GlobalVariable*)module->getOrInsertGlobal(globalName, AT);
        for (int i = 0; i < needEncGV_count; i++) {
            Constant* CValue = ConstantInt::get(Type::getInt8Ty(M.getContext()), 0);
            Values[i] = CValue;
        }
        Constant* valueArray = ConstantArray::get(AT, ArrayRef<Constant*>(Values));
        if (!GVIsDecrypted->hasInitializer()) {
            GVIsDecrypted->setInitializer(valueArray);
            GVIsDecrypted->setLinkage(GlobalValue::PrivateLinkage);
        }

        Function* decryptFunction = Generic_obfuscator::GVEncrypt::defineDecryptFunction(&M, GVIsDecrypted);
        for (llvm::Function& F : M) {
            if (shouldSkip(F, gvEncrypt)) {
                continue;
            }
            Generic_obfuscator::GVEncrypt::encryptGV(&F, decryptFunction);
            is_processed = true;
        }
        PrintSuccess("GVEncrypt successfully process module ", M.getName().str());
    }

    if (is_processed) {
        return PreservedAnalyses::none();
    } else {
        return PreservedAnalyses::all();
    }
}