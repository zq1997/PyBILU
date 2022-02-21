#include <memory>
#include <iostream>

#include <Python.h>

#include "llvm.h"

using namespace std;
using namespace llvm;

static unique_ptr<MyJIT> jit;

static unique_ptr<LLVMContext> context;
static unique_ptr<Module> module;
static unique_ptr<IRBuilder<>> builder;


static void gen_code(PyCodeObject *cpy_ir) {
    assert(PyBytes_CheckExact(cpy_ir->co_code));
    auto instr_arr = PyBytes_AS_STRING(cpy_ir->co_code);
    auto instr_size = PyBytes_GET_SIZE(cpy_ir->co_code);
    auto instr_begin = reinterpret_cast<_Py_CODEUNIT *>(instr_arr);
    auto instr_end = reinterpret_cast<_Py_CODEUNIT *>(instr_arr + instr_size);
    for (auto iter = instr_begin; iter < instr_end; iter++) {
        auto opcode = _Py_OPCODE(*iter);
        auto oparg = _Py_OPARG(*iter);
        cout << opcode << '\t' << oparg << endl;
    }

    auto func = Function::Create(
            FunctionType::get(Type::getVoidTy(*context)->getPointerTo(), false),
            Function::ExternalLinkage,
            "main",
            module.get()
    );
    auto py_func = Function::Create(
            FunctionType::get(
                    Type::getVoidTy(*context)->getPointerTo(),
                    {Type::getScalarTy<long>(*context)},
                    false
            ),
            Function::ExternalLinkage,
            "PyLong_FromLong",
            module.get()
    );
    BasicBlock *bb = BasicBlock::Create(*context, "entry", func);
    builder->SetInsertPoint(bb);
    auto v = builder->CreateCall(py_func, ConstantInt::get(Type::getScalarTy<long>(*context), 42));
    builder->CreateRet(v);
    assert(!verifyFunction(*func, &errs()));
}

void *run(PyCodeObject *cpy_ir) {
    MyJIT::Init();
    jit = cantFail(MyJIT::Create());
    context = make_unique<LLVMContext>();
    module = make_unique<Module>("", *context);
    builder = make_unique<IRBuilder<>>(*context);
    gen_code(cpy_ir);
    jit->addModule(move(module), move(context));
    auto sym = cantFail(jit->lookup("main"));
    return reinterpret_cast<void *>(sym.getAddress());
}
