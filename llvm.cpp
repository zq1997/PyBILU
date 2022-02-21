// #include <memory>
//
// using namespace std;
//
// #include <Python.h>
//
// #include "llvm.h"
//
// using namespace llvm;
// using namespace llvm::orc;
//
//
// static unique_ptr<MyJIT> jit;
//
// static unique_ptr<LLVMContext> context;
// static unique_ptr<Module> module;
// static unique_ptr<IRBuilder<>> builder;
//
//
// static void gen_code() {
//     auto func = Function::Create(
//             FunctionType::get(Type::getVoidTy(*context)->getPointerTo(), false),
//             Function::ExternalLinkage,
//             "main",
//             module.get()
//     );
//     auto py_func = Function::Create(
//             FunctionType::get(
//                     Type::getVoidTy(*context)->getPointerTo(),
//                     {Type::getScalarTy<long>(*context)},
//                     false
//             ),
//             Function::ExternalLinkage,
//             "PyLong_FromLong",
//             module.get()
//     );
//     BasicBlock *bb = BasicBlock::Create(*context, "entry", func);
//     builder->SetInsertPoint(bb);
//     auto v = builder->CreateCall(py_func, ConstantInt::get(Type::getScalarTy<long>(*context), 42));
//     builder->CreateRet(v);
//     assert(!verifyFunction(*func, &errs()));
// }
//
//
// void *run(PyCodeObject *) {
//     MyJIT::Init();
//     jit = cantFail(MyJIT::Create());
//     context = make_unique<LLVMContext>();
//     module = make_unique<Module>("", *context);
//     builder = make_unique<IRBuilder<>>(*context);
//     gen_code();
//     jit->addModule(ThreadSafeModule(move(module), move(context)));
//     auto sym = cantFail(jit->lookup("main"));
//     return reinterpret_cast<void *>(sym.getAddress());
//     // auto JIT = cantFail(orc::LLJITBuilder().create());
//     // cantFail(JIT->addIRModule(orc::ThreadSafeModule(move(module), move(context))));
//     // auto sym = cantFail(JIT->lookup("main"));
// }
