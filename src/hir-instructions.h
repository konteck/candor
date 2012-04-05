#ifndef _SRC_HIR_INSTRUCTIONS_H_
#define _SRC_HIR_INSTRUCTIONS_H_

#include "ast.h" // BinOp
#include "utils.h" // PowerOfTwo
#include "zone.h" // ZoneObject

#include <stdlib.h> // NULL
#include <stdint.h> // int64_t
#include <assert.h> // assert

namespace candor {
namespace internal {

// Forward declarations
class HIRBasicBlock;
class HIRValue;
class HIRPhi;
class LIR;
class LIROperand;
class RelocationInfo;

// Instruction base class
class HIRInstruction : public ZoneObject {
 public:
  enum Type {
    kNone,
    kNop,

    // Instruction w/o side-effects
    kParallelMove,
    kEntry,
    kReturn,
    kGoto,
    kStoreLocal,
    kStoreContext,
    kLoadRoot,
    kLoadLocal,
    kLoadContext,

    // Branching
    kBranchBool,

    // Stubs and instructions with side-effects
    kCall,
    kStoreProperty,
    kLoadProperty,
    kBinOp,
    kTypeof,
    kSizeof,
    kKeysof,
    kNot,
    kCollectGarbage,
    kGetStackTrace,
    kAllocateFunction,
    kAllocateObject
  };

  HIRInstruction(Type type) : type_(type),
                              id_(-1),
                              block_(NULL),
                              def_(NULL),
                              result_(NULL),
                              prev_(NULL),
                              next_(NULL) {
  }

  // Called by HIR to associate instruction with block and to
  // give instruction a monothonic id
  virtual void Init(HIRBasicBlock* block);

  // Wrapper to push into value->uses()
  void Use(HIRValue* value);

  // Replace all uses (not definitions!) of variable
  void ReplaceVarUse(HIRValue* source, HIRValue* target);

  // Type checks
  inline Type type() { return type_; }
  inline bool is(Type type) { return type_ == type; }

  // Block in which this instruction is located
  inline HIRBasicBlock* block() { return block_; }

  // Instruction with side
  virtual bool HasSideEffects() const { return false; };

  // Instruction takes multiple inputs
  inline void SetInput(HIRValue* input) {
    Use(input);
    values()->Push(input);
  }

  // And one output
  inline void SetResult(HIRValue* result) {
    assert(result_ == NULL);

    Use(result);
    values()->Push(result);
    result_ = result;
  }
  inline HIRValue* GetResult() { return result_; }

  // Definition
  inline void SetDef(HIRValue* def) { def_ = def; }
  inline HIRValue* GetDef() { return def_; }

  // List of all used values
  inline ZoneList<HIRValue*>* values() { return &values_; }

  // Links to previous and next instructions
  inline HIRInstruction* prev() { return prev_; }
  inline void prev(HIRInstruction* prev) { prev_ = prev; }
  inline HIRInstruction* next() { return next_; }
  inline void next(HIRInstruction* next) { next_ = next; }

  // For liveness-range calculations
  inline int id() { return id_; }
  inline void id(int id) { id_ = id; }

  // Debug printing
  virtual void Print(PrintBuffer* p);

 private:
  Type type_;
  int id_;
  HIRBasicBlock* block_;

  ZoneList<HIRValue*> values_;

  HIRValue* def_;
  HIRValue* result_;

  HIRInstruction* prev_;
  HIRInstruction* next_;
};

class HIRLoadBase : public HIRInstruction {
 public:
  HIRLoadBase(Type type, HIRValue* value) : HIRInstruction(type),
                                            value_(value) {
    SetResult(value);
  }

  inline HIRValue* value() { return value_; }

 private:
  HIRValue* value_;
};

class HIRStoreBase : public HIRInstruction {
 public:
  HIRStoreBase(Type type, HIRValue* lhs, HIRValue* rhs) : HIRInstruction(type),
                                                          lhs_(lhs),
                                                          rhs_(rhs) {
    SetInput(lhs);
    SetDef(lhs);
    SetResult(rhs);
  }

  inline HIRValue* lhs() { return lhs_; }
  inline HIRValue* rhs() { return rhs_; }

 private:
  HIRValue* lhs_;
  HIRValue* rhs_;
};

class HIRBranchBase : public HIRInstruction {
 public:
  HIRBranchBase(Type type,
                HIRValue* clause,
                HIRBasicBlock* left,
                HIRBasicBlock* right) : HIRInstruction(type),
                                        clause_(clause),
                                        left_(left),
                                        right_(right) {
    SetInput(clause);
  }

  virtual void Init(HIRBasicBlock* block);

  virtual bool HasSideEffects() const { return true; };

  inline HIRValue* clause() { return clause_; }
  inline HIRBasicBlock* left() { return left_; }
  inline HIRBasicBlock* right() { return right_; }

 private:
  HIRValue* clause_;
  HIRBasicBlock* left_;
  HIRBasicBlock* right_;
};

class HIRStubCall : public HIRInstruction {
 public:
  HIRStubCall(Type type) : HIRInstruction(type) {
  }

  void Init(HIRBasicBlock* block);
  virtual bool HasSideEffects() const { return true; };
};

class HIRPrefixKeyword : public HIRStubCall {
 public:
  HIRPrefixKeyword(Type type, HIRValue* expr) : HIRStubCall(type), expr_(expr) {
    SetInput(expr);
  }

  inline HIRValue* expr() { return expr_; }

 private:
  HIRValue* expr_;
};

class HIRParallelMove : public HIRInstruction {
 public:
  typedef ZoneList<LIROperand*> OperandList;

  HIRParallelMove() : HIRInstruction(kParallelMove), spill_(NULL) {
  }

  // Record movement
  void AddMove(LIROperand* source, LIROperand* target);

  // Order movements to prevent "overlapping"
  void Reorder(LIR* lir);

  static inline HIRParallelMove* Cast(HIRInstruction* instr) {
    assert(instr->type() == kParallelMove);
    return reinterpret_cast<HIRParallelMove*>(instr);
  }

  static inline HIRParallelMove* GetBefore(HIRInstruction* instr) {
    return Cast(instr->prev());
  }

  static inline HIRParallelMove* GetAfter(HIRInstruction* instr) {
    return Cast(instr->next());
  }

  inline OperandList* sources() { return &sources_; }
  inline OperandList* targets() { return &targets_; }
  inline OperandList* raw_sources() { return &raw_sources_; }
  inline OperandList* raw_targets() { return &raw_targets_; }

 private:
  // For recursive reordering
  void Reorder(LIR* lir,
               ZoneList<LIROperand*>::Item* source,
               ZoneList<LIROperand*>::Item* target);

  // Only one spill scratch is required (cache it)
  LIROperand* spill_;

  // Sources/Targets after reordering
  OperandList sources_;
  OperandList targets_;

  // Sources/Targets before reordering (because it's cheaper to leave it here)
  OperandList raw_sources_;
  OperandList raw_targets_;
};

class HIRNop : public HIRInstruction {
 public:
  HIRNop() : HIRInstruction(kNop) {
  }

  HIRNop(HIRValue* result) : HIRInstruction(kNop) {
    SetResult(result);
  }
};

class HIREntry : public HIRInstruction {
 public:
  HIREntry(int context_slots) : HIRInstruction(kEntry),
                                context_slots_(context_slots) {
  }

  void AddArg(HIRValue* arg);

  inline int context_slots() { return context_slots_; }
  inline ZoneList<HIRValue*>* args() { return &args_; }

 private:
  int context_slots_;
  ZoneList<HIRValue*> args_;
};

class HIRReturn : public HIRInstruction {
 public:
  HIRReturn(HIRValue* value) : HIRInstruction(kReturn) {
    SetInput(value);
  }
};

class HIRGoto : public HIRInstruction {
 public:
  HIRGoto() : HIRInstruction(kGoto) {
  }
};

class HIRLoadRoot : public HIRLoadBase {
 public:
  HIRLoadRoot(HIRValue* value) : HIRLoadBase(kLoadRoot, value), value_(value) {
  }

  inline HIRValue* value() { return value_; }

 private:
  HIRValue* value_;
};

class HIRLoadLocal : public HIRLoadBase {
 public:
  HIRLoadLocal(HIRValue* value) : HIRLoadBase(kLoadLocal, value) {
  }
};

class HIRLoadContext : public HIRLoadBase {
 public:
  HIRLoadContext(HIRValue* value) : HIRLoadBase(kLoadContext, value) {
  }
};

class HIRLoadProperty : public HIRStubCall {
 public:
  HIRLoadProperty(HIRValue* receiver, HIRValue* property)
      : HIRStubCall(kLoadProperty),
        receiver_(receiver),
        property_(property) {
    SetInput(receiver);
    SetInput(property);
  }

  inline HIRValue* receiver() { return receiver_; }
  inline HIRValue* property() { return property_; }

 private:
  HIRValue* receiver_;
  HIRValue* property_;
};

class HIRBinOp : public HIRStubCall {
 public:
  HIRBinOp(BinOp::BinOpType type, HIRValue* lhs, HIRValue* rhs)
      : HIRStubCall(kBinOp),
        type_(type),
        lhs_(lhs),
        rhs_(rhs) {
    SetInput(lhs);
    SetInput(rhs);
  }

  inline BinOp::BinOpType type() { return type_; }
  inline HIRValue* lhs() { return lhs_; }
  inline HIRValue* rhs() { return rhs_; }

 private:
  BinOp::BinOpType type_;
  HIRValue* lhs_;
  HIRValue* rhs_;
};

class HIRStoreLocal : public HIRStoreBase {
 public:
  HIRStoreLocal(HIRValue* lhs, HIRValue* rhs)
      : HIRStoreBase(kStoreLocal, lhs, rhs) {
  }
};

class HIRStoreContext : public HIRStoreBase {
 public:
  HIRStoreContext(HIRValue* lhs, HIRValue* rhs)
      : HIRStoreBase(kStoreContext, lhs, rhs) {
  }
};

class HIRStoreProperty : public HIRInstruction {
 public:
  HIRStoreProperty(HIRValue* receiver, HIRValue* property, HIRValue* rhs)
      : HIRInstruction(kStoreProperty),
        receiver_(receiver),
        property_(property),
        rhs_(rhs) {
    SetInput(receiver);
    SetInput(property);
    // Propagate result
    SetResult(rhs);
  }

  virtual bool HasSideEffects() const { return true; };

  inline HIRValue* receiver() { return receiver_; }
  inline HIRValue* property() { return property_; }
  inline HIRValue* rhs() { return rhs_; }

 private:
  HIRValue* receiver_;
  HIRValue* property_;
  HIRValue* rhs_;
};

class HIRBranchBool : public HIRBranchBase {
 public:
  HIRBranchBool(HIRValue* clause, HIRBasicBlock* left, HIRBasicBlock* right)
      : HIRBranchBase(kBranchBool, clause, left, right) {
  }
};

class HIRCall : public HIRStubCall {
 public:
  HIRCall(HIRValue* fn) : HIRStubCall(kCall), fn_(fn) {
    SetInput(fn);
  }

  void AddArg(HIRValue* arg);

  inline HIRValue* fn() { return fn_; }
  inline ZoneList<HIRValue*>* args() { return &args_; }

 private:
  HIRValue* fn_;
  ZoneList<HIRValue*> args_;
};

class HIRTypeof : public HIRPrefixKeyword {
 public:
  HIRTypeof(HIRValue* expr) : HIRPrefixKeyword(kTypeof, expr) {
  }
};

class HIRSizeof : public HIRPrefixKeyword {
 public:
  HIRSizeof(HIRValue* expr) : HIRPrefixKeyword(kSizeof, expr) {
  }
};

class HIRKeysof : public HIRPrefixKeyword {
 public:
  HIRKeysof(HIRValue* expr) : HIRPrefixKeyword(kKeysof, expr) {
  }
};

class HIRNot : public HIRPrefixKeyword {
 public:
  HIRNot(HIRValue* expr) : HIRPrefixKeyword(kNot, expr) {
  }
};

class HIRCollectGarbage : public HIRStubCall {
 public:
  HIRCollectGarbage() : HIRStubCall(kCollectGarbage) {
  }
};

class HIRGetStackTrace : public HIRStubCall {
 public:
  HIRGetStackTrace() : HIRStubCall(kGetStackTrace) {
  }
};

class HIRAllocateFunction : public HIRStubCall {
 public:
  HIRAllocateFunction(HIRBasicBlock* body, int argc)
      : HIRStubCall(kAllocateFunction),
        argc_(argc),
        body_(body) {
  }

  inline int argc() { return argc_; }
  inline HIRBasicBlock* body() { return body_; }

 private:
  int argc_;
  HIRBasicBlock* body_;
};

class HIRAllocateObject : public HIRStubCall {
 public:
  enum ObjectKind {
    kObject,
    kArray
  };

  HIRAllocateObject(ObjectKind kind, int size)
      : HIRStubCall(kAllocateObject),
        kind_(kind),
        size_(PowerOfTwo(size << 1)) {
  }

  inline ObjectKind kind() { return kind_; }
  inline int size() { return size_; }

 private:
  ObjectKind kind_;
  int size_;
};

} // namespace internal
} // namespace candor

#endif // _SRC_HIR_INSTRUCTIONS_H_