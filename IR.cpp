#include <memory>
#include <iostream>

#include <Python.h>

#include "llvm.h"

using namespace std;
using namespace llvm;

class MyContext {
public:
    LLVMContext context;
    IRBuilder<> builder;
    Module mod;
    Type *t_PyType_Object = nullptr;
    Type *t_PyObject = nullptr;
    Type *t_PyObject_p = nullptr;
    Function *func = nullptr;

public:
    MyContext() :
            context(),
            builder(context),
            mod("", context) {
        t_PyType_Object = PointerType::getUnqual(context);
        t_PyObject = StructType::create({
                Type::getScalarTy<Py_ssize_t>(context),
                t_PyType_Object->getPointerTo()
        });
        t_PyObject_p = t_PyObject->getPointerTo();
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
        func = Function::Create(
                FunctionType::get(t_PyObject_p, {t_PyObject_p->getPointerTo()}, false),
                Function::ExternalLinkage,
                "main",
                &mod
        );
        BasicBlock *bb = BasicBlock::Create(context, "entry", func);
        builder.SetInsertPoint(bb);

        auto args = func->getArg(0);
        auto p = builder.CreateInBoundsGEP(t_PyObject_p, args, ConstantInt::get(Type::getScalarTy<long>(context), 0));
        auto ret = builder.CreateLoad(t_PyObject_p, p);
        p = builder.CreateStructGEP(t_PyObject, ret, 0);
        Value *ref = builder.CreateLoad(Type::getScalarTy<Py_ssize_t>(context), p);
        ref = builder.CreateNSWAdd(ref, ConstantInt::get(Type::getScalarTy<long>(context), 1));
        builder.CreateStore(ref, p);

        // auto ret = builder.CreateExtractValue(args, 0);
        // auto cpy_PyLong_FromLong = Function::Create(
        //         FunctionType::get(
        //                 t_PyObject_p,
        //                 {Type::getScalarTy<long>(context)},
        //                 false
        //         ),
        //         Function::ExternalLinkage,
        //         "PyLong_FromLong",
        //         &mod
        // );
        // auto cpy_PyNumber_Add = Function::Create(
        //         FunctionType::get(
        //                 t_PyObject_p,
        //                 {t_PyObject_p, t_PyObject_p},
        //                 false
        //         ),
        //         Function::ExternalLinkage,
        //         "PyNumber_Add",
        //         mod
        // );
        // auto v1 = builder.CreateCall(cpy_PyLong_FromLong, ConstantInt::get(Type::getScalarTy<long>(context), 1));
        // auto v2 = builder.CreateCall(cpy_PyLong_FromLong, ConstantInt::get(Type::getScalarTy<long>(context), 2));
        // auto v3 = builder.CreateCall(cpy_PyLong_FromLong, ConstantInt::get(Type::getScalarTy<long>(context), 3));
        // auto s1 = builder.CreateCall(cpy_PyNumber_Add, {v1, v2});
        // auto ret = builder.CreateCall(cpy_PyNumber_Add, {s1, v3});
        builder.CreateRet(ret);
        assert(!verifyFunction(*func, &errs()));
    }
};


static unique_ptr<MyJIT> jit;
static unique_ptr<MyContext> ctx;


void *run(PyCodeObject *cpy_ir) {
    if (!jit) {
        MyJIT::init();
        jit = cantFail(MyJIT::create());
        ctx = make_unique<MyContext>();
        ctx->mod.setDataLayout(jit->getDL());
    } else {
        ctx->func->removeFromParent();
        delete ctx->func;
    }
    ctx->GenCode(cpy_ir);
    ctx->mod.print(outs(), nullptr);
    return jit->emitModule(ctx->mod);
}
