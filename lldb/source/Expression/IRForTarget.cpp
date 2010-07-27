//===-- IRForTarget.cpp -------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lldb/Expression/IRForTarget.h"

#include "llvm/Support/raw_ostream.h"
#include "llvm/InstrTypes.h"
#include "llvm/Instructions.h"
#include "llvm/Module.h"
#include "llvm/Target/TargetData.h"

#include "clang/AST/ASTContext.h"

#include "lldb/Core/dwarf.h"
#include "lldb/Core/Log.h"
#include "lldb/Core/Scalar.h"
#include "lldb/Core/StreamString.h"
#include "lldb/Expression/ClangExpressionDeclMap.h"

#include <map>

using namespace llvm;

IRForTarget::IRForTarget(const void *pid,
                         lldb_private::ClangExpressionDeclMap *decl_map,
                         const llvm::TargetData *target_data) :
    ModulePass(pid),
    m_decl_map(decl_map),
    m_target_data(target_data)
{
}

IRForTarget::~IRForTarget()
{
}

static clang::NamedDecl *
DeclForGlobalValue(llvm::Module &module,
                   llvm::GlobalValue *global_value)
{
    NamedMDNode *named_metadata = module.getNamedMetadata("clang.global.decl.ptrs");
    
    if (!named_metadata)
        return NULL;
    
    unsigned num_nodes = named_metadata->getNumOperands();
    unsigned node_index;
    
    for (node_index = 0;
         node_index < num_nodes;
         ++node_index)
    {
        MDNode *metadata_node = named_metadata->getOperand(node_index);
        
        if (!metadata_node)
            return NULL;
        
        if (metadata_node->getNumOperands() != 2)
            return NULL;
        
        if (metadata_node->getOperand(0) != global_value)
            continue;
        
        ConstantInt *constant_int = dyn_cast<ConstantInt>(metadata_node->getOperand(1));
        
        if (!constant_int)
            return NULL;
        
        uintptr_t ptr = constant_int->getZExtValue();
        
        return reinterpret_cast<clang::NamedDecl *>(ptr);
    }
    
    return NULL;
}

bool 
IRForTarget::MaybeHandleVariable(Module &M, 
                                 llvm::Value *V,
                                 bool Store)
{
    if (GlobalVariable *global_variable = dyn_cast<GlobalVariable>(V))
    {        
        clang::NamedDecl *named_decl = DeclForGlobalValue(M, global_variable);
        
        std::string name = named_decl->getName().str();
        
        void *qual_type = NULL;
        clang::ASTContext *ast_context = NULL;
        
        if (clang::ValueDecl *value_decl = dyn_cast<clang::ValueDecl>(named_decl))
        {
            qual_type = value_decl->getType().getAsOpaquePtr();
            ast_context = &value_decl->getASTContext();
        }
        else
        {
            return false;
        }
            
        const llvm::Type *value_type = global_variable->getType();
        
        size_t value_size = m_target_data->getTypeStoreSize(value_type);
        off_t value_alignment = m_target_data->getPrefTypeAlignment(value_type);
        
        if (named_decl && !m_decl_map->AddValueToStruct(V, 
                                                        named_decl,
                                                        name,
                                                        qual_type,
                                                        ast_context,
                                                        value_size, 
                                                        value_alignment))
            return false;
    }
    
    return true;
}

bool
IRForTarget::MaybeHandleCall(Module &M,
                             CallInst *C)
{
    lldb_private::Log *log = lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS);

    llvm::Function *fun = C->getCalledFunction();
    
    if (fun == NULL)
        return true;
    
    clang::NamedDecl *fun_decl = DeclForGlobalValue(M, fun);
    
    if (!fun_decl)
    {
        if (log)
            log->Printf("Function %s wasn't in the metadata", fun->getName().str().c_str());
        return false;
    }
    
    uint64_t fun_addr = m_decl_map->GetFunctionAddress(fun_decl);
    
    if (fun_addr == 0)
    {
        if (log)
            log->Printf("Function %s had no address", fun_decl->getNameAsCString());
        return false;
    }
        
    if (log)
        log->Printf("Found %s at %llx", fun_decl->getNameAsCString(), fun_addr);
    
    return true;
}

bool
IRForTarget::runOnBasicBlock(Module &M, BasicBlock &BB)
{        
    /////////////////////////////////////////////////////////////////////////
    // Prepare the current basic block for execution in the remote process
    //
    
    llvm::BasicBlock::iterator ii;

    for (ii = BB.begin();
         ii != BB.end();
         ++ii)
    {
        Instruction &inst = *ii;
        
        if (LoadInst *load = dyn_cast<LoadInst>(&inst))
            if (!MaybeHandleVariable(M, load->getPointerOperand(), false))
                return false;
        
        if (StoreInst *store = dyn_cast<StoreInst>(&inst))
            if (!MaybeHandleVariable(M, store->getPointerOperand(), true))
                return false;
        
        if (CallInst *call = dyn_cast<CallInst>(&inst))
            if (!MaybeHandleCall(M, call))
                return false;
    }
    
    return true;
}

static std::string 
PrintValue(llvm::Value *V, bool truncate = false)
{
    std::string s;
    raw_string_ostream rso(s);
    V->print(rso);
    rso.flush();
    if (truncate)
        s.resize(s.length() - 1);
    return s;
}

static bool isGuardVariableRef(llvm::Value *V)
{
    ConstantExpr *C = dyn_cast<ConstantExpr>(V);
    
    if (!C || C->getOpcode() != Instruction::BitCast)
        return false;
    
    GlobalVariable *GV = dyn_cast<GlobalVariable>(C->getOperand(0));
    
    if (!GV || !GV->hasName() || !GV->getName().startswith("_ZGV"))
        return false;
    
    return true;
}

static void TurnGuardLoadIntoZero(Instruction* guard_load, Module &M)
{
    Constant* zero(ConstantInt::get(Type::getInt8Ty(M.getContext()), 0, true));

    Value::use_iterator ui;
    
    for (ui = guard_load->use_begin();
         ui != guard_load->use_end();
         ++ui)
    {
        if (isa<Constant>(ui))
        {
            // do nothing for the moment
        }
        else
        {
            ui->replaceUsesOfWith(guard_load, zero);
        }
    }
    
    guard_load->eraseFromParent();
}

static void ExciseGuardStore(Instruction* guard_store)
{
    guard_store->eraseFromParent();
}

bool
IRForTarget::removeGuards(Module &M, BasicBlock &BB)
{        
    ///////////////////////////////////////////////////////
    // Eliminate any reference to guard variables found.
    //
    
    llvm::BasicBlock::iterator ii;
    
    typedef llvm::SmallVector <Instruction*, 2> InstrList;
    typedef InstrList::iterator InstrIterator;
    
    InstrList guard_loads;
    InstrList guard_stores;
    
    for (ii = BB.begin();
         ii != BB.end();
         ++ii)
    {
        Instruction &inst = *ii;
        
        if (LoadInst *load = dyn_cast<LoadInst>(&inst))
            if (isGuardVariableRef(load->getPointerOperand()))
                guard_loads.push_back(&inst);                
        
        if (StoreInst *store = dyn_cast<StoreInst>(&inst))            
            if (isGuardVariableRef(store->getPointerOperand()))
                guard_stores.push_back(&inst);
    }
    
    InstrIterator iter;
    
    for (iter = guard_loads.begin();
         iter != guard_loads.end();
         ++iter)
        TurnGuardLoadIntoZero(*iter, M);
    
    for (iter = guard_stores.begin();
         iter != guard_stores.end();
         ++iter)
        ExciseGuardStore(*iter);
    
    return true;
}

// UnfoldConstant operates on a constant [C] which has just been replaced with a value
// [new_value].  We assume that new_value has been properly placed early in the function,
// most likely somewhere in front of the first instruction in the entry basic block 
// [first_entry_instruction].  
//
// UnfoldConstant reads through the uses of C and replaces C in those uses with new_value.
// Where those uses are constants, the function generates new instructions to compute the
// result of the new, non-constant expression and places them before first_entry_instruction.  
// These instructions replace the constant uses, so UnfoldConstant calls itself recursively
// for those.

static bool
UnfoldConstant(llvm::Constant *C, llvm::Value *new_value, llvm::Instruction *first_entry_instruction)
{
    lldb_private::Log *log = lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS);

    Value::use_iterator ui;
    
    for (ui = C->use_begin();
         ui != C->use_end();
         ++ui)
    {
        User *user = *ui;
        
        if (Constant *constant = dyn_cast<Constant>(user))
        {
            // synthesize a new non-constant equivalent of the constant
            
            if (ConstantExpr *constant_expr = dyn_cast<ConstantExpr>(constant))
            {
                switch (constant_expr->getOpcode())
                {
                default:
                    if (log)
                        log->Printf("Unhandled constant expression type: %s", PrintValue(constant_expr).c_str());
                    return false;
                case Instruction::BitCast:
                    {
                        // UnaryExpr
                        //   OperandList[0] is value
                        
                        Value *s = constant_expr->getOperand(0);
                        
                        if (s == C)
                            s = new_value;
                        
                        BitCastInst *bit_cast(new BitCastInst(s, C->getType(), "", first_entry_instruction));
                        
                        UnfoldConstant(constant_expr, bit_cast, first_entry_instruction);
                    }
                    break;
                case Instruction::GetElementPtr:
                    {
                        // GetElementPtrConstantExpr
                        //   OperandList[0] is base
                        //   OperandList[1]... are indices
                        
                        Value *ptr = constant_expr->getOperand(0);
                        
                        if (ptr == C)
                            ptr = new_value;
                        
                        SmallVector<Value*, 16> indices;
                        
                        unsigned operand_index;
                        unsigned num_operands = constant_expr->getNumOperands();
                        
                        for (operand_index = 1;
                             operand_index < num_operands;
                             ++operand_index)
                        {
                            Value *operand = constant_expr->getOperand(operand_index);
                            
                            if (operand == C)
                                operand = new_value;
                            
                            indices.push_back(operand);
                        }
                        
                        GetElementPtrInst *get_element_ptr(GetElementPtrInst::Create(ptr, indices.begin(), indices.end(), "", first_entry_instruction));
                        
                        UnfoldConstant(constant_expr, get_element_ptr, first_entry_instruction);
                    }
                    break;
                }
            }
            else
            {
                if (log)
                    log->Printf("Unhandled constant type: %s", PrintValue(constant).c_str());
                return false;
            }
        }
        else
        {
            // simple fall-through case for non-constants
            user->replaceUsesOfWith(C, new_value);
        }
    }
    
    return true;
}

bool 
IRForTarget::replaceVariables(Module &M, Function *F)
{
    lldb_private::Log *log = lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS);

    m_decl_map->DoStructLayout();
    
    if (log)
        log->Printf("Element arrangement:");
    
    uint32_t num_elements;
    uint32_t element_index;
    
    size_t size;
    off_t alignment;
    
    if (!m_decl_map->GetStructInfo (num_elements, size, alignment))
        return false;
    
    Function::arg_iterator iter(F->getArgumentList().begin());
    
    if (iter == F->getArgumentList().end())
        return false;
    
    llvm::Argument *argument = iter;
    
    if (!argument->getName().equals("___clang_arg"))
        return false;
    
    if (log)
        log->Printf("Arg: %s", PrintValue(argument).c_str());
    
    llvm::BasicBlock &entry_block(F->getEntryBlock());
    llvm::Instruction *first_entry_instruction(entry_block.getFirstNonPHIOrDbg());
    
    if (!first_entry_instruction)
        return false;
    
    LLVMContext &context(M.getContext());
    const IntegerType *offset_type(Type::getInt32Ty(context));
    
    if (!offset_type)
        return false;
        
    for (element_index = 0; element_index < num_elements; ++element_index)
    {
        const clang::NamedDecl *decl;
        llvm::Value *value;
        off_t offset;
        
        if (!m_decl_map->GetStructElement (decl, value, offset, element_index))
            return false;
        
        if (log)
            log->Printf("  %s (%s) placed at %d",
                        decl->getIdentifier()->getNameStart(),
                        PrintValue(value, true).c_str(),
                        offset);
        
        ConstantInt *offset_int(ConstantInt::getSigned(offset_type, offset));
        GetElementPtrInst *get_element_ptr = GetElementPtrInst::Create(argument, offset_int, "", first_entry_instruction);
        BitCastInst *bit_cast = new BitCastInst(get_element_ptr, value->getType(), "", first_entry_instruction);
        
        if (Constant *constant = dyn_cast<Constant>(value))
            UnfoldConstant(constant, bit_cast, first_entry_instruction);
        else
            value->replaceAllUsesWith(bit_cast);
    }
    
    if (log)
        log->Printf("Total structure [align %d, size %d]", alignment, size);
    
    return true;
}

bool
IRForTarget::runOnModule(Module &M)
{
    lldb_private::Log *log = lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS);
    
    llvm::Function* function = M.getFunction(StringRef("___clang_expr"));
    
    if (!function)
    {
        if (log)
            log->Printf("Couldn't find ___clang_expr() in the module");
        
        return false;
    }
        
    llvm::Function::iterator bbi;
    
    for (bbi = function->begin();
         bbi != function->end();
         ++bbi)
    {
        if (!runOnBasicBlock(M, *bbi))
            return false;
        
        if (!removeGuards(M, *bbi))
            return false;
    }
    
    if (!replaceVariables(M, function))
        return false;
    
    if (log)
    {
        for (bbi = function->begin();
             bbi != function->end();
             ++bbi)
        {
            log->Printf("Rewrote basic block %s for running: \n%s", 
                        bbi->hasName() ? bbi->getNameStr().c_str() : "[anonymous]",
                        PrintValue(bbi).c_str());
        }
        
    }
    
    return true;    
}

void
IRForTarget::assignPassManager(PMStack &PMS,
                               PassManagerType T)
{
}

PassManagerType
IRForTarget::getPotentialPassManagerType() const
{
    return PMT_ModulePassManager;
}
