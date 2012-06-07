#ifndef _SRC_MARCOASSEMBLER_INL_H_
#define _SRC_MARCOASSEMBLER_INL_H_

#include "macroassembler.h"
#include "heap.h" // HValue
#include "lir-allocator.h" // LIROperand

namespace candor {
namespace internal {

inline void Masm::Push(Register src) {
  ChangeAlign(1);
  push(src);
}


inline void Masm::Pop(Register src) {
  pop(src);
  ChangeAlign(-1);
}


inline void Masm::PreservePop(Register src, Register preserve) {
  if (src.is(preserve)) {
    pop(scratch);
  } else {
    pop(src);
  }
}


inline void Masm::Push(LIROperand* src) {
  if (src->is_register()) {
    push(RegisterByIndex(src->value()));
  } else if (src->is_immediate()) {
    push(Immediate(src->value()));
  } else {
    push(SpillToOperand(src->value()));
  }
}


inline void Masm::Pop(LIROperand* dst) {
  if (dst->is_register()) {
    pop(RegisterByIndex(dst->value()));
  } else {
    pop(scratch);
    if (dst->is_spill()) {
      Mov(dst, scratch);
    }
  }
}


inline void Masm::Mov(Register dst, LIROperand* src) {
  if (src->is_register()) {
    if (dst.is(RegisterByIndex(src->value()))) return;

    mov(dst, RegisterByIndex(src->value()));
  } else if (src->is_immediate()) {
    mov(dst, Immediate(src->value()));
  } else {
    mov(dst, SpillToOperand(src->value()));
  }
}


inline void Masm::Mov(Operand& dst, LIROperand* src) {
  if (src->is_register()) {
    mov(dst, RegisterByIndex(src->value()));
  } else if (src->is_immediate()) {
    mov(dst, Immediate(src->value()));
  } else {
    mov(scratch, SpillToOperand(src->value()));
    mov(dst, scratch);
  }
}


inline void Masm::Mov(LIROperand* dst, Register src) {
  if (dst->is_register()) {
    if (!RegisterByIndex(dst->value()).is(src)) {
      mov(RegisterByIndex(dst->value()), src);
    }
  } else if (dst->is_spill()) {
    mov(SpillToOperand(dst->value()), src);
  } else {
    UNEXPECTED
  }
}


inline void Masm::Mov(LIROperand* dst, Operand& src) {
  if (dst->is_register()) {
    mov(RegisterByIndex(dst->value()), src);
  } else if (dst->is_spill()) {
    mov(scratch, src);
    mov(SpillToOperand(dst->value()), scratch);
  } else {
    UNEXPECTED
  }
}


inline void Masm::Mov(LIROperand* dst, Immediate src) {
  if (dst->is_register()) {
    mov(RegisterByIndex(dst->value()), src);
  } else if (dst->is_spill()) {
    mov(SpillToOperand(dst->value()), src);
  } else {
    UNEXPECTED
  }
}


inline void Masm::Mov(LIROperand* dst, LIROperand* src) {
  if (src == dst) return;

  if (dst->is_register()) {
    Mov(RegisterByIndex(dst->value()), src);
  } else if (dst->is_spill()) {
    if (src->is_register()) {
      mov(SpillToOperand(dst->value()), RegisterByIndex(src->value()));
    } else if (src->is_immediate()) {
      mov(SpillToOperand(dst->value()), Immediate(src->value()));
    } else {
      mov(scratch, SpillToOperand(src->value()));
      mov(SpillToOperand(dst->value()), scratch);
    }
  } else {
    UNEXPECTED
  }
}


inline void Masm::TagNumber(Register src) {
  sal(src, Immediate(1));
}


inline void Masm::Untag(Register src) {
  sar(src, Immediate(1));
}


inline Operand& Masm::SpillToOperand(int index) {
  spill_operand_.disp(- 8 * (index + 1));
  return spill_operand_;
}


inline Condition Masm::BinOpToCondition(BinOp::BinOpType type,
                                        BinOpUsage usage) {
  if (usage == kIntegral) {
    switch (type) {
     case BinOp::kStrictEq:
     case BinOp::kEq: return kEq;
     case BinOp::kStrictNe:
     case BinOp::kNe: return kNe;
     case BinOp::kLt: return kLt;
     case BinOp::kGt: return kGt;
     case BinOp::kLe: return kLe;
     case BinOp::kGe: return kGe;
     default: UNEXPECTED
    }
  } else if (usage == kDouble) {
    switch (type) {
     case BinOp::kStrictEq:
     case BinOp::kEq: return kEq;
     case BinOp::kStrictNe:
     case BinOp::kNe: return kNe;
     case BinOp::kLt: return kBelow;
     case BinOp::kGt: return kAbove;
     case BinOp::kLe: return kBe;
     case BinOp::kGe: return kAe;
     default: UNEXPECTED
    }
  }

  // Just to shut up compiler
  return kEq;
}


inline void Masm::SpillSlot(uint32_t index, Operand& op) {
  op.base(rbp);
  op.disp(-spill_offset_ - HValue::kPointerSize * index);
}

} // namespace internal
} // namespace candor

#endif // _SRC_MARCOASSEMBLER_INL_H_
