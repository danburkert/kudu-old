// Copyright 2014 Cloudera inc.

#include "kudu/codegen/row_projector.h"

#include <string>
#include <ostream>
#include <vector>

#include <boost/assign/list_of.hpp>
#include <boost/foreach.hpp>
#include "kudu/codegen/llvm_include.h"
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

#include "kudu/codegen/module_builder.h"
#include "kudu/common/row.h"
#include "kudu/common/schema.h"
#include "kudu/gutil/strings/strcat.h"
#include "kudu/util/status.h"

namespace llvm {
class LLVMContext;
} // namespace llvm

using boost::assign::list_of;
using llvm::Argument;
using llvm::BasicBlock;
using llvm::ConstantInt;
using llvm::ExecutionEngine;
using llvm::Function;
using llvm::FunctionType;
using llvm::GenericValue;
using llvm::LLVMContext;
using llvm::Module;
using llvm::PointerType;
using llvm::Type;
using llvm::Value;
using std::string;
using std::ostream;
using std::vector;

DECLARE_bool(codegen_dump_functions);

namespace kudu {
namespace codegen {

namespace {

// Generates a schema-to-schema projection function of the form:
// bool(int8_t* src, RowBlockRow* row, Arena* arena)
// Requires src is a contiguous row of the base schema.
// Returns a boolean indicating success. Failure can only occur if a string
// relocation fails.
//
// Uses CHECKs to make sure projection is well-formed. Use
// kudu::RowProjector::Init() to return an error status instead.
template<bool READ>
llvm::Function* MakeProjection(const string& name,
                               ModuleBuilder* mbuilder,
                               const kudu::RowProjector& proj) {
  // Get the IRBuilder
  ModuleBuilder::LLVMBuilder* builder = mbuilder->builder();
  LLVMContext& context = builder->getContext();

  // Extract schema information from projector
  const Schema& base_schema = *proj.base_schema();
  const Schema& projection = *proj.projection();

  // Create the function after providing a declaration
  vector<Type*> argtypes = list_of<Type*>
    (Type::getInt8PtrTy(context))
    (PointerType::getUnqual(mbuilder->GetType("class.kudu::RowBlockRow")))
    (PointerType::getUnqual(mbuilder->GetType("class.kudu::Arena")));
  FunctionType* fty =
    FunctionType::get(Type::getInt1Ty(context), argtypes, false);
  Function* f = mbuilder->Create(fty, name);

  // Get the function's Arguments
  Function::arg_iterator it = f->arg_begin();
  Argument* src = &*it++;
  Argument* rbrow = &*it++;
  Argument* arena = &*it++;
  DCHECK(it == f->arg_end());

  // Give names to the arguments for debugging IR.
  src->setName("src");
  rbrow->setName("rbrow");
  arena->setName("arena");

  // Project row function in IR (note: values in angle brackets are
  // constants whose values are determined right now, at JIT time).
  //
  // define i1 @name(i8* %src, i8* %rbrow, i8* %arena)
  // entry:
  //   %src_bitmap = getelementptr i8* %src, i64 <offset to bitmap>
  //   <for each base column to projection column mapping>
  //     %src_cell = getelementptr i8* %src, i64 <base offset>
  //     %result = call i1 @CopyCellToRowBlock(
  //       i64 <type size>, i8* %src_cell, RowBlockRow* %rbrow,
  //       i64 <column index>, i1 <is string>, Arena* %arena)**
  //   %success = and %success, %result***
  //   <end implicit for each>
  //   <for each projection column that needs defaults>
  //     %src_cell = inttoptr i64 <default value location> to i8*
  //     %result = call i1 @CopyCellToRowBlock(
  //       i64 <type size>, i8* %src_cell, RowBlockRow* %rbrow,
  //       i64 <column index>, i1 <is string>, Arena* %arena)
  //   %success = and %success, %result***
  //   <end implicit for each>
  //   ret i1 %success
  //
  // **If the column is nullable, then the call is replaced with
  // call i1 @CopyCellToRowBlockNullable(
  //   i64 <type size>, i8* %src_cell, RowBlockRow* %rbrow, i64 <column index>,
  //   i1 <is_string>, Arena* %arena, i8* src_bitmap, i64 <bitmap_idx>)
  // ***Technically, llvm ir does not support mutable registers. Thus,
  // this is implemented by having "success" be the most recent result
  // register of the last "and" instruction. The different "success" values
  // can be differentiated by using a success_update_number.

  // Retrieve copy cell to rowblock functions
  Function* copy_cell_not_null =
    mbuilder->GetFunction("_PrecompiledCopyCellToRowBlock");
  Function* copy_cell_nullable =
    mbuilder->GetFunction("_PrecompiledCopyCellToRowBlockNullable");

  // The bitmap for a contiguous row goes after the row data
  // See common/row.h ContiguousRowHelper class
  builder->SetInsertPoint(BasicBlock::Create(context, "entry", f));
  Value* src_bitmap = builder->CreateConstGEP1_64(src, base_schema.byte_size());
  src_bitmap->setName("src_bitmap");
  Value* success = builder->getInt1(true);
  int success_update_number = 0;

  // Copy base data
  BOOST_FOREACH(const kudu::RowProjector::ProjectionIdxMapping& pmap,
                proj.base_cols_mapping()) {
    // Retrieve information regarding this column-to-column transformation
    size_t proj_idx = pmap.first;
    size_t base_idx = pmap.second;
    size_t src_offset = base_schema.column_offset(base_idx);
    const ColumnSchema& col = base_schema.column(base_idx);

    // Create the common values between the nullable and nonnullable calls
    Value* size = builder->getInt64(col.type_info()->size());
    Value* src_cell = builder->CreateConstGEP1_64(src, src_offset);
    src_cell->setName(StrCat("src_cell_base_", base_idx));
    Value* col_idx = builder->getInt64(proj_idx);
    ConstantInt* is_string = builder->getInt1(col.type_info()->type() == STRING);
    vector<Value*> args = list_of<Value*>
      (size)(src_cell)(rbrow)(col_idx)(is_string)(arena);

    // Add additional arguments if nullable
    Function* to_call = copy_cell_not_null;
    if (col.is_nullable()) {
      args.push_back(src_bitmap);
      args.push_back(builder->getInt64(base_idx));
      to_call = copy_cell_nullable;
    }

    // Make the call and check the return value
    Value* result = builder->CreateCall(to_call, args);
    result->setName(StrCat("result_b", base_idx, "_p", proj_idx));
    success = builder->CreateAnd(success, result);
    success->setName(StrCat("success", success_update_number++));
  }

  // TODO: Copy adapted base data
  DCHECK(proj.adapter_cols_mapping().size() == 0)
    << "Value Adapter not supported yet";

  // Fill defaults
  BOOST_FOREACH(size_t dfl_idx, proj.projection_defaults()) {
    // Retrieve mapping information
    const ColumnSchema& col = projection.column(dfl_idx);
    const void* dfl = READ ? col.read_default_value() :
      col.write_default_value();

    // If there are defaults, then at least READ default must be defined.
    CHECK(!(READ && dfl == NULL))
      << "Requested default value for projection index " << dfl_idx
      << " in projection (s1, s2) with no default value specified:\n"
      << "\ts1 = " << base_schema.ToString() << "\n"
      << "\ts2 = " << projection.ToString();

    // Generate arguments
    Value* size = builder->getInt64(col.type_info()->size());
    Value* src_cell = mbuilder->GetPointerValue(const_cast<void*>(dfl));
    Value* col_idx = builder->getInt64(dfl_idx);
    ConstantInt* is_string = builder->getInt1(col.type_info()->type() == STRING);

    // Make the call and check the return value
    vector<Value*> args = list_of
      (size)(src_cell)(rbrow)(col_idx)(is_string)(arena);
    Value* result = builder->CreateCall(copy_cell_not_null, args);
    result->setName(StrCat("result_dfl", dfl_idx));
    success = builder->CreateAnd(success, result);
    success->setName(StrCat("success", success_update_number++));
  }

  // Return
  builder->CreateRet(success);

  if (FLAGS_codegen_dump_functions) {
    LOG(INFO) << "Dumping " << (READ? "read" : "write") << " projection:";
    f->dump();
  }

  return f;
}

} // anonymous namespace

RowProjector::RowProjector(const Schema* base_schema, const Schema* projection)
  : read_f_(NULL),
    write_f_(NULL),
    projector_(base_schema, projection) {}

Status RowProjector::Create(const Schema* base_schema, const Schema* projection,
                            ModuleBuilder* builder,
                            gscoped_ptr<RowProjector>* out) {
  // Create the rowprojector and initialize the mapping without code generation
  gscoped_ptr<RowProjector> local_proj(new RowProjector(base_schema,
                                                        projection));
  kudu::RowProjector* no_codegen = &local_proj->projector_;
  RETURN_NOT_OK(no_codegen->Init());

  // Build the functions for code gen
  const string base_name = StrCat("Proj",
                                  reinterpret_cast<uintptr_t>(local_proj.get()));
  Function* read = MakeProjection<true>(StrCat(base_name, "R"), builder,
                                        *no_codegen);
  Function* write = MakeProjection<false>(StrCat(base_name, "W"), builder,
                                          *no_codegen);

  // Have the ModuleBuilder accept promises to compile the functions
  builder->AddJITPromise(read, &local_proj->read_f_);
  builder->AddJITPromise(write, &local_proj->write_f_);

  // Upon success, write to out
  *out = local_proj.Pass();
  return Status::OK();
}

RowProjector::~RowProjector() {}

void RowProjector::TakeEngine(gscoped_ptr<ExecutionEngine> engine) {
  engine_ = engine.Pass();
}

ostream& operator<<(ostream& o, const RowProjector& rp) {
  o << "Row Projector s1->s2 with:\n"
    << "\ts1 = " << rp.base_schema()->ToString() << "\n"
    << "\ts2 = " << rp.projection()->ToString();
  return o;
}

} // namespace codegen
} // namespace kudu