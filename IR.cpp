#include <memory>
#include <iostream>

#include <Python.h>

#include "llvm.h"

using namespace std;
using namespace llvm;

class MyContext {
public:
    unique_ptr<LLVMContext> context;
    IRBuilder<> builder;
    unique_ptr<Module> mod;
    Type *type_PyObject_p = nullptr;
    Function *func = nullptr;

public:
    MyContext() :
            context(make_unique<LLVMContext>()),
            builder(*context),
            mod(make_unique<Module>("", *context)) {
        func = Function::Create(
                FunctionType::get(Type::getInt64Ty(*context)->getPointerTo(), false),
                Function::ExternalLinkage,
                "main",
                mod.get()
        );
        BasicBlock *bb = BasicBlock::Create(*context, "entry", func);
        builder.SetInsertPoint(bb);
    }

    void GenCode(PyCodeObject *cpy_ir) {
        // assert(PyBytes_CheckExact(cpy_ir->co_code));
        // auto instr_arr = PyBytes_AS_STRING(cpy_ir->co_code);
        // auto instr_size = PyBytes_GET_SIZE(cpy_ir->co_code);
        // auto instr_begin = reinterpret_cast<_Py_CODEUNIT *>(instr_arr);
        // auto instr_end = reinterpret_cast<_Py_CODEUNIT *>(instr_arr + instr_size);
        // for (auto iter = instr_begin; iter < instr_end; iter++) {
        //     auto opcode = _Py_OPCODE(*iter);
        //     auto oparg = _Py_OPARG(*iter);
        // }
        auto cpy_PyLong_FromLong = Function::Create(
                FunctionType::get(
                        Type::getInt64Ty(*context)->getPointerTo(),
                        {Type::getScalarTy<long>(*context)},
                        false
                ),
                Function::ExternalLinkage,
                "PyLong_FromLong",
                mod.get()
        );
        auto cpy_PyNumber_Add = Function::Create(
                FunctionType::get(
                        Type::getInt64Ty(*context)->getPointerTo(),
                        {Type::getInt64Ty(*context)->getPointerTo(), Type::getInt64Ty(*context)->getPointerTo()},
                        false
                ),
                Function::ExternalLinkage,
                "PyNumber_Add",
                mod.get()
        );
        auto v1 = builder.CreateCall(cpy_PyLong_FromLong, ConstantInt::get(Type::getScalarTy<long>(*context), 1));
        auto v2 = builder.CreateCall(cpy_PyLong_FromLong, ConstantInt::get(Type::getScalarTy<long>(*context), 2));
        auto v3 = builder.CreateCall(cpy_PyLong_FromLong, ConstantInt::get(Type::getScalarTy<long>(*context), 3));
        auto s1 = builder.CreateCall(cpy_PyNumber_Add, {v1, v2});
        auto s2 = builder.CreateCall(cpy_PyNumber_Add, {s1, v3});
        builder.CreateRet(s2);
        assert(!verifyFunction(*func, &errs()));
    }
};


static unique_ptr<MyJIT> jit;
static unique_ptr<MyContext> ctx;


void *run(PyCodeObject *cpy_ir) {
    MyJIT::Init();
    jit = cantFail(MyJIT::Create());
    ctx = make_unique<MyContext>();
    ctx->mod->setDataLayout(jit->getDL());
    ctx->GenCode(cpy_ir);
    return jit->emitModule(*ctx->mod);
    // jit->addModule(move(ctx->mod), move(ctx->context));
    // auto sym = cantFail(jit->lookup("main"));
    // return reinterpret_cast<void *>(sym.getAddress());
}
