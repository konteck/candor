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

#ifndef _SRC_FULLGEN_H_
#define _SRC_FULLGEN_H_

#include <assert.h>  // assert
#include <stdint.h>  // uint32_t

#include "visitor.h"
#include "root.h"
#include "ast.h"  // AstNode, FunctionLiteral
#include "zone.h"  // ZoneObject
#include "utils.h"  // List

namespace candor {
namespace internal {

// Forward declaration
class Heap;
class SourceMap;
class ScopeSlot;
class Fullgen;
class FInstruction;
class FFunction;
class FLabel;
class FOperand;
class Masm;
class Operand;

typedef ZoneList<FOperand*> FOperandList;

class FOperand : public ZoneObject {
 public:
  enum Type {
    kStack,
    kContext
  };

  FOperand(Type type, int index, int depth) : type_(type),
                                              index_(index),
                                              depth_(depth) {
  }

  inline bool is_stack();
  inline bool is_context();
  inline int index();
  inline int depth();
  inline bool operator==(FOperand* t);

  void Print(PrintBuffer* p);
  Operand* ToOperand();

 protected:
  Type type_;
  int index_;
  int depth_;
};

class FStackSlot : public FOperand {
 public:
  explicit FStackSlot(int index) : FOperand(kStack, index, -1) {
  }
};

class FContextSlot : public FOperand {
 public:
  FContextSlot(int index, int depth) : FOperand(kContext, index, depth) {
  }
};

class FScopedSlot {
 public:
  explicit FScopedSlot(Fullgen* f);
  ~FScopedSlot();

  inline FOperand* operand();
  inline FOperand* operator&() { return operand(); }

 protected:
  Fullgen* f_;
  FOperand* operand_;
};

// Generates non-optimized code by visiting each node in AST tree in-order
class Fullgen : public Visitor<FInstruction> {
 public:
  Fullgen(Heap* heap, Root* root, const char* filename);

  void Build(AstNode* ast);
  void Generate(Masm* masm);

  FInstruction* Visit(AstNode* node);
  void VisitChildren(AstNode* node);

  void LoadArguments(FunctionLiteral* fn);
  FInstruction* VisitFunction(AstNode* stmt);
  FInstruction* VisitCall(AstNode* stmt);
  FInstruction* VisitAssign(AstNode* stmt);

  FInstruction* VisitValue(AstNode* node);

  FInstruction* VisitLiteral(AstNode* node);
  FInstruction* VisitNumber(AstNode* node);
  FInstruction* VisitNil(AstNode* node);
  FInstruction* VisitTrue(AstNode* node);
  FInstruction* VisitFalse(AstNode* node);
  FInstruction* VisitString(AstNode* node);
  FInstruction* VisitProperty(AstNode* node);

  FInstruction* VisitIf(AstNode* node);
  FInstruction* VisitWhile(AstNode* node);

  FInstruction* VisitMember(AstNode* node);
  FInstruction* VisitObjectLiteral(AstNode* node);
  FInstruction* VisitArrayLiteral(AstNode* node);

  FInstruction* VisitReturn(AstNode* node);
  FInstruction* VisitClone(AstNode* node);
  FInstruction* VisitDelete(AstNode* node);
  FInstruction* VisitBreak(AstNode* node);
  FInstruction* VisitContinue(AstNode* node);

  FInstruction* VisitTypeof(AstNode* node);
  FInstruction* VisitSizeof(AstNode* node);
  FInstruction* VisitKeysof(AstNode* node);

  FInstruction* VisitUnOp(AstNode* node);
  FInstruction* VisitBinOp(AstNode* node);

  static void EnableLogging();
  static void DisableLogging();

  inline void Print(char* out, int32_t size);
  void Print(PrintBuffer* p);

  inline FInstruction* Add(FInstruction* instr);
  inline FOperand* CreateOperand(ScopeSlot* slot);

  inline void EmptySlots();
  inline FOperand* GetSlot();
  inline void ReleaseSlot(FOperand* slot);
  inline FInstruction* GetNumber(uint64_t i);

  inline int instr_id();

  inline FFunction* current_function();
  inline void set_current_function(FFunction* current_function);

  inline Root* root();
  inline SourceMap* source_map();

 private:
  static bool log_;

  Heap* heap_;
  Root* root_;
  const char* filename_;

  ZoneList<FInstruction*> instructions_;

  int instr_id_;
  FFunction* current_function_;
  FLabel* loop_start_;
  FLabel* loop_end_;

  int stack_index_;
  FOperandList free_slots_;

  SourceMap* source_map_;

  friend class FScopedSlot;
};

}  // namespace internal
}  // namespace candor

#endif  // _SRC_FULLGEN_H_
