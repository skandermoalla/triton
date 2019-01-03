#include "ir/context.h"
#include "ir/basic_block.h"
#include "ir/instructions.h"
#include "ir/constant.h"
#include "ir/type.h"

namespace tdl{
namespace ir{

//===----------------------------------------------------------------------===//
//                               instruction classes
//===----------------------------------------------------------------------===//

instruction::instruction(type *ty, unsigned num_ops, const std::string &name, instruction *next)
    : user(ty, num_ops, name) {
  if(next){
    basic_block *block = next->get_parent();
    assert(block && "Next instruction is not in a basic block!");
    auto it = std::find(block->begin(), block->end(), next);
    block->get_inst_list().insert(it, next);
  }
}

//===----------------------------------------------------------------------===//
//                               phi_node classes
//===----------------------------------------------------------------------===//

phi_node::phi_node(type *ty, unsigned num_reserved, std::string const &name, instruction *next)
  : instruction(ty, num_reserved, name, next){ }

// Set incoming value
void phi_node::set_incoming_value(unsigned i, value *v){
  assert(v && "PHI node got a null value!");
  assert(get_type() == v->get_type() &&
         "All operands to PHI node must be the same type as the PHI node!");
  set_operand(i, v);
}

// Set incoming block
void phi_node::set_incoming_block(unsigned i, basic_block *block){
  assert(block && "PHI node got a null basic block!");
  blocks_[i] = block;
}

// Add incoming
void phi_node::add_incoming(value *v, basic_block *block){
  if(get_num_operands()==num_reserved_){
    num_reserved_++;
    resize_ops(num_reserved_);
    blocks_.resize(num_reserved_);
  }
  set_incoming_value(get_num_operands() - 1, v);
  set_incoming_block(get_num_operands() - 1, block);
}

// Factory methods
phi_node* phi_node::create(type *ty, unsigned num_reserved, const std::string &name, instruction *next){
  return new phi_node(ty, num_reserved, name, next);
}


//===----------------------------------------------------------------------===//
//                               binary_operator classes
//===----------------------------------------------------------------------===//

binary_operator::binary_operator(op_t op, value *lhs, value *rhs, type *ty, const std::string &name, instruction *next)
    : instruction(ty, 2, name, next), op_(op){
  set_operand(0, lhs);
  set_operand(1, rhs);
}

binary_operator *binary_operator::create(op_t op, value *lhs, value *rhs, const std::string &name, instruction *next){
  assert(lhs->get_type() == rhs->get_type() &&
         "Cannot create binary operator with two operands of differing type!");
  return new binary_operator(op, lhs, rhs, lhs->get_type(), name, next);
}

binary_operator *binary_operator::create_fneg(value *arg, const std::string &name, instruction *next){
  assert(arg->get_type()->is_floating_point_ty());
  value *zero = constant_fp::get_zero_value_for_negation(arg->get_type());
  return binary_operator::create(llvm::Instruction::FSub, zero, arg, name, next);
}

binary_operator *binary_operator::create_neg(value *arg, const std::string &name, instruction *next){
  assert(arg->get_type()->is_integer_ty());
  value *zero = constant_fp::get_zero_value_for_negation(arg->get_type());
  return binary_operator::create(llvm::Instruction::Sub, zero, arg, name, next);
}

binary_operator *binary_operator::create_not(value *arg, const std::string &name, instruction *next){
  assert(arg->get_type()->is_integer_ty());
  constant *mask = constant::get_all_ones_value(arg->get_type());
  return binary_operator::create(llvm::Instruction::Xor, arg, mask, name, next);
}

//===----------------------------------------------------------------------===//
//                               cmp_inst classes
//===----------------------------------------------------------------------===//

// cmp_inst

cmp_inst::cmp_inst(type *ty, cmp_inst::pred_t pred, value *lhs, value *rhs, const std::string &name, instruction *next)
    : instruction(ty, 2, name, next), pred_(pred) {
  set_operand(0, lhs);
  set_operand(1, rhs);
}

type* cmp_inst::make_cmp_result_type(type *ty){
  type* int1_ty = type::get_int1_ty(ty->get_context());
  if (tile_type* tile_ty = dynamic_cast<tile_type*>(ty))
    return tile_type::get_same_shapes(int1_ty, tile_ty);
  return int1_ty;
}




bool cmp_inst::is_fp_predicate(pred_t pred) {
  return pred >= pcmp::FIRST_FCMP_PREDICATE && pred <= pcmp::LAST_FCMP_PREDICATE;
}

bool cmp_inst::is_int_predicate(pred_t pred) {
  return pred >= pcmp::FIRST_ICMP_PREDICATE && pred <= pcmp::LAST_ICMP_PREDICATE;
}

// icmp_inst

icmp_inst* icmp_inst::create(pred_t pred, value *lhs, value *rhs, const std::string &name, instruction *next){
  assert(is_int_predicate(pred));
  type *res_ty = make_cmp_result_type(lhs->get_type());
  return new icmp_inst(res_ty, pred, lhs, rhs, name, next);
}

// fcmp_inst

fcmp_inst* fcmp_inst::create(pred_t pred, value *lhs, value *rhs, const std::string &name, instruction *next){
  assert(is_fp_predicate(pred));
  type *res_ty = make_cmp_result_type(lhs->get_type());
  return new fcmp_inst(res_ty, pred, lhs, rhs, name, next);
}

//===----------------------------------------------------------------------===//
//                               unary_inst classes
//===----------------------------------------------------------------------===//

unary_inst::unary_inst(type *ty, value *v, const std::string &name, instruction *next)
    : instruction(ty, 1, name, next) {
  set_operand(0, v);
}

//===----------------------------------------------------------------------===//
//                               cast_inst classes
//===----------------------------------------------------------------------===//

cast_inst *cast_inst::create(op_t op, value *arg, type *ty, const std::string &name, instruction *next){
  assert(is_valid(op, arg, ty) && "Invalid cast!");
  // Construct and return the appropriate CastInst subclass
  switch (op) {
  case ic::Trunc:         return new trunc_inst           (ty, arg, name, next);
  case ic::ZExt:          return new z_ext_inst           (ty, arg, name, next);
  case ic::SExt:          return new s_ext_inst           (ty, arg, name, next);
  case ic::FPTrunc:       return new fp_trunc_inst        (ty, arg, name, next);
  case ic::FPExt:         return new fp_ext_inst          (ty, arg, name, next);
  case ic::UIToFP:        return new ui_to_fp_inst        (ty, arg, name, next);
  case ic::SIToFP:        return new si_to_fp_inst        (ty, arg, name, next);
  case ic::FPToUI:        return new fp_to_ui_inst        (ty, arg, name, next);
  case ic::FPToSI:        return new fp_to_si_inst        (ty, arg, name, next);
  case ic::PtrToInt:      return new ptr_to_int_inst      (ty, arg, name, next);
  case ic::IntToPtr:      return new int_to_ptr_inst      (ty, arg, name, next);
  case ic::BitCast:       return new bit_cast_inst        (ty, arg, name, next);
  case ic::AddrSpaceCast: return new addr_space_cast_inst (ty, arg, name, next);
  default: throw std::runtime_error("unreachable");
  }
}

cast_inst *cast_inst::create_integer_cast(value *arg, type *ty, bool is_signed, const std::string &name, instruction *next){
  type *arg_ty = arg->get_type();
  assert(arg_ty->is_int_or_tileint_ty() && ty->is_int_or_tileint_ty() && "Invalid integer cast!");
  unsigned arg_bits = arg_ty->get_integer_bitwidth();
  unsigned dst_bits = ty->get_integer_bitwidth();
  op_t op = (arg_bits == dst_bits ? ic::BitCast :
            (arg_bits > dst_bits  ? ic::Trunc :
            (is_signed            ? ic::SExt : ic::ZExt)));
  return create(op, arg, ty, name, next);
}

//===----------------------------------------------------------------------===//
//                               terminator_inst classes
//===----------------------------------------------------------------------===//


// return_inst

return_inst::return_inst(context &ctx, value *ret_val, instruction *next)
    : terminator_inst(type::get_void_ty(ctx), !!ret_val, "", next){
  if(ret_val)
    set_operand(0, ret_val);
}

return_inst *return_inst::create(context &ctx, value *ret_val, instruction *next){
  return new return_inst(ctx, ret_val, next);
}


// conditional/unconditional branch

branch_inst::branch_inst(basic_block *dst, instruction *next)
    : terminator_inst(type::get_void_ty(dst->get_context()), 1, "", next){
  set_operand(0, dst);
}

branch_inst::branch_inst(basic_block *if_dst, basic_block *else_dst, value *cond, instruction *next)
    : terminator_inst(type::get_void_ty(if_dst->get_context()), 3, "", next){
  assert(cond->get_type()->is_integer_ty(1) && "May only branch on boolean predicates!");
  set_operand(0, if_dst);
  set_operand(1, else_dst);
  set_operand(2, cond);
}

branch_inst* branch_inst::create(basic_block *dst, instruction *next) {
  assert(dst && "Branch destination may not be null!");
  return new branch_inst(dst, next);
}

branch_inst* branch_inst::create(value *cond, basic_block *if_dst, basic_block *else_dst, instruction *next) {
  assert(cond->get_type()->is_integer_ty(1) && "May only branch on boolean predicates!");
  return new branch_inst(if_dst, else_dst, cond, next);
}


//===----------------------------------------------------------------------===//
//                               getelementptr_inst classes
//===----------------------------------------------------------------------===//

getelementptr_inst::getelementptr_inst(type *pointee_ty, value *ptr, const std::vector<value *> &idx, const std::string &name, instruction *next)
    : instruction(get_return_type(pointee_ty, ptr, idx), idx.size(), name, next),
      source_elt_ty(pointee_ty),
      res_elt_ty(get_indexed_type(pointee_ty, idx)){
  type *expected_ty = ((pointer_type*)(get_type()->get_scalar_ty()))->get_element_ty();
  assert(res_elt_ty == expected_ty);
  set_operand(0, ptr);
  for(size_t i = 0; i < idx.size(); i++)
    set_operand(1 + i, idx[i]);
}

type *getelementptr_inst::get_return_type(type *elt_ty, value *ptr, const std::vector<value *> &idx_list) {
  // result pointer type
  type *ptr_ty = pointer_type::get(get_indexed_type(elt_ty, idx_list), ptr->get_type()->get_pointer_address_space());
  // Tile GEP
  if(ptr->get_type()->is_tile_ty())
    return tile_type::get_same_shapes(ptr_ty, ptr->get_type());
  for(value *idx : idx_list)
  if (idx->get_type()->is_tile_ty())
    return tile_type::get_same_shapes(ptr_ty, idx->get_type());
  // Scalar GEP
  return ptr_ty;
}

type *getelementptr_inst::get_indexed_type_impl(type *ty, const std::vector<value *> &idx_list) {
  if(idx_list.empty())
    return ty;
  if(!ty->is_sized())
    return nullptr;
  unsigned cur_idx = 1;
  for(; cur_idx != idx_list.size(); cur_idx++){
    composite_type *cty = dynamic_cast<composite_type*>(ty);
    if(!cty || cty->is_pointer_ty())
      break;
    value *idx = idx_list[cur_idx];
    if(!cty->index_valid(idx))
      break;
    ty = cty->get_type_at_index(idx);
  }
  return (cur_idx == idx_list.size())? ty : nullptr;
}

type *getelementptr_inst::get_indexed_type(type *ty, const std::vector<value *> &idx_list) {
  type *result = get_indexed_type_impl(ty, idx_list);
  assert(result && "invalid GEP type!");
  return result;
}

getelementptr_inst *getelementptr_inst::create(type *pointee_ty, value *ptr, const std::vector<value *> &idx, const std::string &name, instruction *next) {
  return new getelementptr_inst(pointee_ty, ptr, idx, name, next);
}



//===----------------------------------------------------------------------===//
//                               retile_inst classes
//===----------------------------------------------------------------------===//

retile_inst::retile_inst(value *arg, const std::vector<unsigned> &shapes,
                         const std::string &name, instruction *next)
   : instruction(tile_type::get(arg->get_type()->get_scalar_ty(), shapes), 1, name, next) {
  set_operand(0, arg);
}

// reshape

instruction* reshape_inst::create(value *arg, const std::vector<unsigned> &shapes,
                                  const std::string &name, instruction *next) {
  return new reshape_inst(arg, shapes, name, next);
}


// splat

instruction* splat_inst::create(value *arg, const std::vector<unsigned> &shapes,
                                  const std::string &name, instruction *next) {
  return new splat_inst(arg, shapes, name, next);
}

// broadcast

instruction* broadcast_inst::create(value *arg, const std::vector<unsigned> &shapes,
                                  const std::string &name, instruction *next) {
  return new broadcast_inst(arg, shapes, name, next);
}

}
}
