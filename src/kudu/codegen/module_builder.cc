// Copyright 2014 Cloudera inc.

// Requires that the location of the precompiled.ll file is defined
#ifndef KUDU_CODEGEN_MODULE_BUILDER_PRECOMPILED_LL
#error "KUDU_CODEGEN_MODULE_BUILDER_PRECOMPILED_LL should be defined to " \
  "the location of the LLVM IR file for kudu/codegen/precompiled.cc"
#endif

#include "kudu/codegen/module_builder.h"

#include <cstdlib>
#include <sstream>
#include <string>

#include <boost/foreach.hpp>
#include <glog/logging.h>
#include "kudu/codegen/llvm_include.h"
#include <llvm/ExecutionEngine/MCJIT.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_os_ostream.h>

#include "kudu/gutil/macros.h"
#include "kudu/util/status.h"

using llvm::CodeGenOpt::Level;
using llvm::ConstantExpr;
using llvm::ConstantInt;
using llvm::EngineBuilder;
using llvm::ExecutionEngine;
using llvm::Function;
using llvm::FunctionType;
using llvm::IntegerType;
using llvm::LLVMContext;
using llvm::Module;
using llvm::PointerType;
using llvm::raw_os_ostream;
using llvm::SMDiagnostic;
using llvm::Type;
using llvm::Value;
using std::ostream;
using std::string;
using std::stringstream;

namespace kudu {
namespace codegen {

namespace {

string ToString(const SMDiagnostic& err) {
  stringstream sstr;
  raw_os_ostream os(sstr);
  err.print("kudu/codegen", os);
  os.flush();
  return sstr.str();
}

string ToString(const Module& m) {
  stringstream sstr;
  raw_os_ostream os(sstr);
  os << m;
  return sstr.str();
}

// This method is needed for the implicit conversion from
// llvm::StringRef to std::string
string ToString(const Function* f) {
  return f->getName();
}

bool ModuleContains(const Module& m, const Function* fptr) {
  for (Module::const_iterator it = m.begin(); it != m.end(); ++it) {
    if (&*it == fptr) return true;
  }
  return false;
}

} // anonymous namespace

const char* const ModuleBuilder::kKuduIRFile =
  KUDU_CODEGEN_MODULE_BUILDER_PRECOMPILED_LL;

ModuleBuilder::ModuleBuilder()
  : state_(kUninitialized),
    context_(new LLVMContext()),
    builder_(*context_) {}

ModuleBuilder::~ModuleBuilder() {}

Status ModuleBuilder::Init() {
  CHECK_EQ(state_, kUninitialized) << "Cannot Init() twice";
  // Parse IR file
  SMDiagnostic err;
  module_.reset(llvm::ParseIRFile(kKuduIRFile, err, *context_));
  if (!module_) {
    return Status::ConfigurationError("Could not parse IR file",
                                      ToString(err));
  }
  VLOG(3) << "Successfully parsed IR file at " << kKuduIRFile << ":\n"
          << ToString(*module_);

  // TODO: consider loading this module once and then just copying it
  // from memory. If this strategy is used it may be worth trying to
  // reduce the .ll file size.

  state_ = kBuilding;
  return Status::OK();
}

Function* ModuleBuilder::Create(FunctionType* fty, const string& name) {
  CHECK_EQ(state_, kBuilding);
  return Function::Create(fty, Function::ExternalLinkage, name, module_.get());
}

Function* ModuleBuilder::GetFunction(const string& name) {
  CHECK_EQ(state_, kBuilding);
  // All extern "C" functions are guaranteed to have the same
  // exact name as declared in the source file.
  return CHECK_NOTNULL(module_->getFunction(name));
}

Type* ModuleBuilder::GetType(const string& name) {
  CHECK_EQ(state_, kBuilding);
  // Technically clang is not obligated to name every
  // class as "class.kudu::ClassName" but so long as there
  // are no naming conflicts in the LLVM context it appears
  // to do so (naming conflicts are avoided by having 1 context
  // per module)
  return CHECK_NOTNULL(module_->getTypeByName(name));
}

Value* ModuleBuilder::GetPointerValue(void* ptr) const {
  CHECK_EQ(state_, kBuilding);
  // No direct way of creating constant pointer values in LLVM, so
  // first a constant int has to be created and then casted to a pointer
  IntegerType* llvm_uintptr_t = Type::getIntNTy(*context_, 8 * sizeof(ptr));
  uintptr_t int_value = reinterpret_cast<uintptr_t>(ptr);
  ConstantInt* llvm_int_value = ConstantInt::get(llvm_uintptr_t,
                                                 int_value, false);
  Type* llvm_ptr_t = Type::getInt8PtrTy(*context_);
  return ConstantExpr::getIntToPtr(llvm_int_value, llvm_ptr_t);
}


void ModuleBuilder::AddJITPromise(llvm::Function* llvm_f,
                                  FunctionAddress* actual_f) {
  CHECK_EQ(state_, kBuilding);
  DCHECK(ModuleContains(*module_, llvm_f))
    << "Function " << ToString(llvm_f) << " does not belong to ModuleBuilder.";
  JITFuture fut;
  fut.llvm_f_ = llvm_f;
  fut.actual_f_ = actual_f;
  futures_.push_back(fut);
}

Status ModuleBuilder::Compile(gscoped_ptr<ExecutionEngine>* out) {
  CHECK_EQ(state_, kBuilding);

  // Attempt to generate the engine
  string str;
#ifdef NDEBUG
  Level opt_level = llvm::CodeGenOpt::Aggressive;
#else
  Level opt_level = llvm::CodeGenOpt::None;
#endif
  gscoped_ptr<ExecutionEngine> local_engine(EngineBuilder(module_.get())
                                            .setErrorStr(&str)
                                            .setUseMCJIT(true)
                                            .setOptLevel(opt_level)
                                            .create());
  if (!local_engine) {
    return Status::ConfigurationError("Code generation for module failed. "
                                      "Could not start ExecutionEngine",
                                      str);
  }

  // Compile the module
  // TODO add a pass to internalize the linkage of all functions except the
  // ones that are JITPromised
  // TODO use various target machine options
  // TODO various engine builder options (opt level, etc)
  // TODO calling convention?
  // TODO whole-module optimizations
  local_engine->finalizeObject();

  // Satisfy the promises
  BOOST_FOREACH(JITFuture& fut, futures_) {
    *fut.actual_f_ = local_engine->getPointerToFunction(fut.llvm_f_);
    if (*fut.actual_f_ == NULL) {
      return Status::NotFound(
        "Code generation for module failed. Could not find function \""
        + ToString(fut.llvm_f_) + "\".");
    }
  }

  // For LLVM 3.4, generated code lasts exactly as long as the execution engine
  // that created it does. Furthermore, if the module is removed from the
  // engine's ownership, neither the context nor the module have to stick
  // around for the jitted code to run. NOTE: this may change in LLVM 3.5
  CHECK(local_engine->removeModule(module_.get())); // releases ownership

  // Upon success write to the output parameter
  *out = local_engine.Pass();
  state_ = kCompiled;
  return Status::OK();
}

} // namespace codegen
} // namespace kudu