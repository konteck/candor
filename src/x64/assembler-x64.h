/**
 * Copyright (c) 2012, Fedor Indutny.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef _SRC_X64_ASSEMBLER_H_
#define _SRC_X64_ASSEMBLER_H_

#include <stdint.h>  // uint32_t
#include <stdlib.h>  // NULL
#include <string.h>  // memset

#include "zone.h"  // ZoneObject
#include "utils.h"  // List

namespace candor {
namespace internal {

// Forward declaration
class Assembler;
class Masm;
class Heap;
class Label;

struct Register {
  int high() {
    return (code_ >> 3) & 1;
  };

  int low() {
    return code_ & 7;
  };

  int code() {
    return code_;
  }

  inline bool is(Register reg) {
    return code_ == reg.code();
  }

  inline bool operator == (Register a) const {
    return a.code_ == code_;
  }

  inline bool operator != (Register a) const {
    return a.code_ != code_;
  }

  int code_;
};

const Register reg_nil = { -1 };

const Register rax = { 0 };
const Register rcx = { 1 };
const Register rdx = { 2 };
const Register rbx = { 3 };
const Register rsp = { 4 };
const Register rbp = { 5 };
const Register rsi = { 6 };
const Register rdi = { 7 };

const Register r8  = { 8 };
const Register r9  = { 9 };
const Register r10 = { 10 };
const Register r11 = { 11 };
const Register r12 = { 12 };
const Register r13 = { 13 };
const Register r14 = { 14 };
const Register r15 = { 15 };

const Register context_reg = rsi;
const Register root_reg = rdi;
const Register scratch = r14;

static inline Register RegisterByIndex(int index) {
  // rsi, rdi, r14, r15 are reserved
  switch (index) {
    case 0: return rax;
    case 1: return rbx;
    case 2: return rcx;
    case 3: return rdx;
    case 4: return r8;
    case 5: return r9;
    case 6: return r10;
    case 7: return r11;
    case 8: return r12;
    case 9: return r13;
    default: UNEXPECTED return reg_nil;
  }
}


static inline const char* RegisterNameByIndex(int index) {
  // rsi, rdi, r14, r15 are reserved
  switch (index) {
    case 0: return "rax";
    case 1: return "rbx";
    case 2: return "rcx";
    case 3: return "rdx";
    case 4: return "r8 ";
    case 5: return "r9 ";
    case 6: return "r10";
    case 7: return "r11";
    case 8: return "r12";
    case 9: return "r13";
    default: UNEXPECTED return "rnil";
  }
}


static inline int IndexByRegister(Register reg) {
  // rsi, rdi, r14, r15 are reserved
  switch (reg.code()) {
    case 0: return 0;
    case 1: return 2;
    case 2: return 3;
    case 3: return 1;
    case 8: return 4;
    case 9: return 5;
    case 10: return 6;
    case 11: return 7;
    case 12: return 8;
    case 13: return 9;
    default: UNEXPECTED return -1;
  }
}


struct DoubleRegister {
  int high() {
    return (code_ >> 3) & 1;
  };

  int low() {
    return code_ & 7;
  };

  int code() {
    return code_;
  }

  inline bool is(DoubleRegister reg) {
    return code_ == reg.code();
  }

  inline bool operator == (DoubleRegister a) const {
    return a.code_ == code_;
  }

  inline bool operator != (DoubleRegister a) const {
    return a.code_ != code_;
  }

  int code_;
};

const DoubleRegister xmm0 = { 0 };
const DoubleRegister xmm1 = { 1 };
const DoubleRegister xmm2 = { 2 };
const DoubleRegister xmm3 = { 3 };
const DoubleRegister xmm4 = { 4 };
const DoubleRegister xmm5 = { 5 };
const DoubleRegister xmm6 = { 6 };
const DoubleRegister xmm7 = { 7 };
const DoubleRegister xmm8 = { 8 };
const DoubleRegister xmm9 = { 9 };
const DoubleRegister xmm10 = { 10 };
const DoubleRegister xmm11 = { 11 };
const DoubleRegister xmm12 = { 12 };
const DoubleRegister xmm13 = { 13 };
const DoubleRegister xmm14 = { 14 };
const DoubleRegister xmm15 = { 15 };

const DoubleRegister fscratch = xmm11;

class Immediate : public ZoneObject {
 public:
  explicit Immediate(uint64_t value) : value_(value) {
  }

  inline uint64_t value() const { return value_; }
  inline bool is64() const { return value_ > 0xffffffff; }

 private:
  uint64_t value_;

  friend class Assembler;
  friend class Masm;
};

class Operand : public ZoneObject {
 public:
  enum Scale {
    one = 0,
    two = 1,
    four = 2,
    eight = 3
  };

  Operand(Register base, Scale scale, int32_t disp) : base_(base),
                                                       scale_(scale),
                                                       disp_(disp) {
  }
  Operand(Register base, int32_t disp) : base_(base),
                                          scale_(one),
                                          disp_(disp) {
  }

  inline Register base() const { return base_; }
  inline Scale scale() const { return scale_; }
  inline int32_t disp() const { return disp_; }

  inline bool byte_disp() const { return disp() > -128 && disp() < 128; }

 private:
  Register base_;
  Scale scale_;
  int32_t disp_;

  friend class Assembler;
  friend class Masm;
};

class RelocationInfo : public ZoneObject {
 public:
  enum RelocationInfoSize {
    kByte,
    kWord,
    kLong,
    kQuad,
    kPointer = kQuad
  };

  enum RelocationInfoType {
    kAbsolute,
    kValue,
    kRelative
  };

  RelocationInfo(RelocationInfoType type,
                 RelocationInfoSize size,
                 uint32_t offset) : type_(type),
                                    size_(size),
                                    offset_(offset),
                                    target_(0),
                                    notify_gc_(false) {
  }

  void Relocate(Heap* heap, char* buffer);

  inline void target(uint32_t target) { target_ = target; }

  RelocationInfoType type_;
  RelocationInfoSize size_;

  // Offset of address use in code
  uint32_t offset_;

  // Address to put
  uint32_t target_;

  // GC should relocate this info
  // (works only with absolute relocs)
  bool notify_gc_;
};

enum Condition {
  kEq,
  kNe,
  kLt,
  kLe,
  kGt,
  kGe,
  kAbove,
  kBelow,
  kAe,
  kBe,
  kCarry,
  kOverflow,
  kNoOverflow
};

enum RoundMode {
  kRoundNearest = 0x00,
  kRoundDown    = 0x01,
  kRoundUp      = 0x02,
  kRoundToward  = 0x03
};

class Assembler {
 public:
  Assembler() : offset_(0), length_(256) {
    buffer_ = new char[length_];
    memset(buffer_, 0xCC, length_);
  }

  ~Assembler() {
    delete[] buffer_;
  }

  // Relocate all absolute/relative addresses in new code space
  void Relocate(Heap* heap, char* buffer);

  // Instructions
  void nop();
  void cpuid();

  void push(Register src);
  void push(const Operand& src);
  void push(const Immediate imm);
  void pushb(const Immediate imm);
  void pop(Register dst);
  void ret(uint16_t imm);

  void bind(Label* label);
  void jmp(Label* label);
  void jmp(Condition cond, Label* label);

  void cmpq(Register dst, Register src);
  void cmpq(Register dst, const Operand& src);
  void cmpq(Register dst, const Immediate src);
  void cmpqb(Register dst, const Immediate src);
  void cmpq(const Operand& dst, const Immediate src);
  void cmpb(Register dst, const Operand& src);
  void cmpb(Register dst, const Immediate src);
  void cmpb(const Operand& dst, const Immediate src);

  void testb(Register dst, const Immediate src);
  void testl(Register dst, const Immediate src);

  void mov(Register dst, Register src);
  void mov(Register dst, const Operand& src);
  void mov(const Operand& dst, Register src);
  void mov(Register dst, const Immediate src);
  void mov(const Operand& dst, const Immediate src);
  void movl(Register dst, const Immediate src);
  void movl(const Operand& dst, const Immediate src);
  void movb(Register dst, const Immediate src);
  void movb(const Operand& dst, const Immediate src);
  void movb(const Operand& dst, Register src);
  void movzxb(Register dst, const Operand& src);

  void xchg(Register dst, Register src);

  void addq(Register dst, Register src);
  void addl(Register dst, Register src);
  void addq(Register dst, const Operand& src);
  void addq(Register dst, const Immediate src);
  void addqb(Register dst, const Immediate src);
  void subq(Register dst, Register src);
  void subq(Register dst, const Immediate src);
  void subqb(Register dst, const Immediate src);
  void imulq(Register src);
  void idivq(Register src);

  void andq(Register dst, Register src);
  void orq(Register dst, Register src);
  void orqb(Register dst, const Immediate src);
  void xorq(Register dst, Register src);
  void xorl(Register dst, Register src);

  void inc(Register dst);
  void dec(Register dst);
  void shl(Register dst, const Immediate src);
  void shr(Register dst, const Immediate src);
  void shll(Register dst, const Immediate src);
  void shrl(Register dst, const Immediate src);
  void shl(Register dst);
  void shr(Register dst);
  void sal(Register dst, const Immediate src);
  void sar(Register dst, const Immediate src);
  void sal(Register dst);
  void sar(Register dst);

  void callq(Register dst);
  void callq(const Operand& dst);

  // Floating point instructions
  void movd(DoubleRegister dst, Register src);
  void movd(DoubleRegister dst, const Operand& src);
  void movd(Register dst, DoubleRegister src);
  void movd(const Operand& dst, DoubleRegister src);
  void addqd(DoubleRegister dst, DoubleRegister src);
  void subqd(DoubleRegister dst, DoubleRegister src);
  void mulqd(DoubleRegister dst, DoubleRegister src);
  void divqd(DoubleRegister dst, DoubleRegister src);
  void xorqd(DoubleRegister dst, DoubleRegister src);
  void cvtsi2sd(DoubleRegister dst, Register src);
  void cvtsd2si(Register dst, DoubleRegister src);
  void cvttsd2si(Register dst, DoubleRegister src);
  void roundsd(DoubleRegister dst, DoubleRegister src, RoundMode mode);
  void ucomisd(DoubleRegister dst, DoubleRegister src);
  void cmpd(DoubleRegister dst, const Immediate src);

  // Routines
  inline void emit_rex_if_high(Register src);
  inline void emit_rexw(Register dst);
  inline void emit_rexw(const Operand& dst);
  inline void emit_rexw(Register dst, Register src);
  inline void emit_rexw(Register dst, const Operand& src);
  inline void emit_rexw(const Operand& dst, Register src);
  inline void emit_rexw(DoubleRegister dst, Register src);
  inline void emit_rexw(DoubleRegister dst, DoubleRegister src);
  inline void emit_rexw(Register dst, DoubleRegister src);
  inline void emit_rexw(DoubleRegister dst, const Operand& src);

  inline void emit_modrm(Register dst);
  inline void emit_modrm(const Operand &dst);
  inline void emit_modrm(Register dst, Register src);
  inline void emit_modrm(Register dst, const Operand& src);
  inline void emit_modrm(Register dst, uint32_t op);
  inline void emit_modrm(const Operand& dst, uint32_t op);
  inline void emit_modrm(DoubleRegister dst, Register src);
  inline void emit_modrm(Register dst, DoubleRegister src);
  inline void emit_modrm(DoubleRegister dst, DoubleRegister src);
  inline void emit_modrm(DoubleRegister dst, const Operand& src);
  inline void emit_modrm(const Operand& dst, DoubleRegister src);

  inline void emitb(uint8_t v);
  inline void emitw(uint16_t v);
  inline void emitl(uint32_t v);
  inline void emitq(uint64_t v);

  // Increase buffer size automatically
  void Grow();

  inline char* pos() { return buffer_ + offset_; }
  inline char* buffer() { return buffer_; }
  inline uint32_t offset() { return offset_; }
  inline uint32_t length() { return length_; }

  char* buffer_;
  uint32_t offset_;
  uint32_t length_;

  ZoneList<RelocationInfo*> relocation_info_;
  ZoneList<RelocationInfo*> gc_info_;
};

}  // namespace internal
}  // namespace candor

#endif  // _SRC_X64_ASSEMBLER_H_
