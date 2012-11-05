#include "hir.h"
#include "hir-inl.h"
#include <string.h> // memset, memcpy

namespace candor {
namespace internal {

HIRGen::HIRGen(Heap* heap, AstNode* root) : Visitor<HIRInstruction>(kPreorder),
                                            current_block_(NULL),
                                            current_root_(NULL),
                                            break_continue_info_(NULL),
                                            root_(heap),
                                            block_id_(0),
                                            instr_id_(-2),
                                            dfs_id_(0) {
  HIRInstruction* hroot = new HIRFunction(root);
  hroot->Init(this, NULL);
  work_queue_.Push(hroot);

  while (work_queue_.length() != 0) {
    HIRFunction* current = HIRFunction::Cast(work_queue_.Shift());

    HIRBlock* b = CreateBlock(current->ast()->stack_slots());
    set_current_block(b);
    set_current_root(b);

    roots_.Push(b);

    current->body = b;
    Visit(current->ast());
  }

  PrunePhis();
  DeriveDominators();
}


void HIRGen::PrunePhis() {
  HIRPhiList queue_;

  // First - get list of all phis in all blocks
  // (and remove them from those blocks for now).
  HIRBlockList::Item* bhead = blocks_.head();
  for (; bhead != NULL; bhead = bhead->next()) {
    HIRBlock* block = bhead->value();

    while (block->phis()->length() > 0) {
      queue_.Push(block->phis()->Shift());
    }
  }

  // Filter out phis that have zero or one inputs
  HIRPhiList::Item* phead = queue_.head();
  HIRPhiList::Item* next;
  for (; phead != NULL; phead = next) {
    HIRPhi* phi = phead->value();
    next = phead->next();

    if (phi->input_count() == 2) {
      if (phi->InputAt(1) != phi) continue;
      phi->input_count(1);
    }
    queue_.Remove(phead);

    if (phi->input_count() == 0) {
      phi->Nilify();
    } else if (phi->input_count() == 1) {
      // Enqueue all phi uses
      HIRInstructionList::Item* head = phi->uses()->head();
      for (; head != NULL; head = head->next()) {
        if (!head->value()->IsRemoved() &&
            head->value()->Is(HIRInstruction::kPhi)) {
          HIRPhi* use = HIRPhi::Cast(head->value());
          queue_.Push(use);
          if (next == NULL) next = queue_.tail();
        }
      }

      Replace(phi, phi->InputAt(0));
      phi->block()->Remove(phi);
    }
  }

  // Put phis back into blocks
  phead = queue_.head();
  for (; phead != NULL; phead = phead->next()) {
    HIRPhi* phi = phead->value();

    if (phi->IsRemoved()) continue;
    phi->block()->phis()->Push(phi);
  }
}


// Implementation of:
//   A fast algorithm for finding dominators in a flowgraph,
//   by T Lengauer, RE Tarjan
void HIRGen::DeriveDominators() {
  // Perform actions for each subtree
  HIRBlockList::Item* rhead = roots_.head();
  for (; rhead != NULL; rhead = rhead->next()) {
    HIRBlockList dfs_blocks_;
    HIRBlock* root = rhead->value();

    // Visit and enumarate blocks in DFS order
    EnumerateDFS(root, &dfs_blocks_);

    // Visit all blocks except root in reverse order
    HIRBlockList::Item* dhead = dfs_blocks_.tail();
    for (; dhead != dfs_blocks_.head(); dhead = dhead->prev()) {
      HIRBlock* w = dhead->value();
      HIRBlock* parent = w->parent();

      // Propagate dominators from predecessors
      for (int i = 0; i < w->pred_count(); i++) {
        HIRBlock* u = w->PredAt(i)->Evaluate();
        if (u->semi()->dfs_id < w->semi()->dfs_id) w->semi(u->semi());
      }
      w->semi()->dominates()->Push(w);
      w->ancestor(parent);

      // Empty parent's bucket and set semidominators
      while (parent->dominates()->length() > 0) {
        HIRBlock* v = parent->dominates()->Shift();
        HIRBlock* u = v->Evaluate();

        if (u->semi()->dfs_id < v->semi()->dfs_id) {
          v->dominator(u);
        } else {
          v->dominator(parent);
        }
      }
    }

    // Ignore one block graphs
    if (dhead == NULL) continue;

    // Skip root block
    dhead = dhead->next();

    // Last step, calculate real dominators
    // (Back sweep)
    for (; dhead != NULL; dhead = dhead->next()) {
      HIRBlock* w = dhead->value();
      if (w->dominator() != w->semi()) {
        w->dominator(w->dominator()->dominator());
      }

      HIRBlock* dom = w->dominator();

      // NOTE: We're using this list for two purpose:
      //  * Bucket for algorithm
      //  * And array of dominator tree children
      while (dom->dominates()->length() != 0) dom->dominates()->Shift();
      dom->dominates()->Push(w);
    }
  }
}


void HIRGen::EnumerateDFS(HIRBlock* b, HIRBlockList* blocks) {
  b->dfs_id = dfs_id();
  blocks->Push(b);

  for (int i = 0; i < b->succ_count(); i++) {
    HIRBlock* succ = b->SuccAt(i);
    if (succ->dfs_id != -1) continue;
    succ->parent(b);
    EnumerateDFS(succ, blocks);
  }
}


HIRInstruction* HIRGen::VisitFunction(AstNode* stmt) {
  FunctionLiteral* fn = FunctionLiteral::Cast(stmt);

  if (current_root() == current_block() &&
      current_block()->IsEmpty()) {
    Add(new HIREntry(stmt->context_slots()));
    HIRInstruction* index = NULL;
    int flat_index = 0;
    bool seen_varg = false;

    if (fn->args()->length() > 0) {
      index = GetNumber(0);
    }

    AstList::Item* args_head = fn->args()->head();
    for (int i = 0; args_head != NULL; args_head = args_head->next(), i++) {
      AstNode* arg = args_head->value();
      bool varg = false;

      HIRInstruction* instr;
      if (arg->is(AstNode::kVarArg)) {
        assert(arg->lhs()->is(AstNode::kValue));
        arg = arg->lhs();

        varg = true;
        seen_varg = true;
        instr = new HIRLoadVarArg();
      } else {
        instr = new HIRInstruction(HIRInstruction::kLoadArg);
      }

      AstValue* value = AstValue::Cast(arg);

      HIRInstruction* varg_rest = NULL;
      HIRInstruction* varg_arr = NULL;
      if (varg) {
        // Result vararg array
        varg_arr = Add(HIRInstruction::kAllocateArray);

        // Add number of arguments that are following varg
        varg_rest = GetNumber(fn->args()->length() - i - 1);
      }
      HIRInstruction* load_arg = Add(instr)->AddArg(index);

      if (varg) {
        load_arg->AddArg(varg_rest)->AddArg(varg_arr);
        load_arg = varg_arr;
      }

      if (value->slot()->is_stack()) {
        // No instruction is needed
        Assign(value->slot(), load_arg);
      } else {
        Add(new HIRStoreContext(value->slot()))->AddArg(load_arg);
      }

      // Do not generate index if args has ended
      if (args_head->next() == NULL) continue;

      // Increment index
      if (!varg) {
        // By 1
        if (!seen_varg) {
          // Index is linear here, just generate new literal
          index = GetNumber(++flat_index);
        } else {
          // Do "Full" math
          AstNode* one = new AstNode(AstNode::kNumber, stmt);

          one->value("1");
          one->length(1);

          HIRInstruction* hone = Visit(one);
          index = Add(new HIRBinOp(BinOp::kAdd))->AddArg(index)->AddArg(hone);
        }
      } else {
        HIRInstruction* length = Add(new HIRSizeof())
            ->AddArg(load_arg);

        // By length of vararg
        index = Add(new HIRBinOp(BinOp::kAdd))->AddArg(index)->AddArg(length);
      }
    }

    VisitChildren(stmt);

    if (!current_block()->IsEnded()) {
      HIRInstruction* val = Add(HIRInstruction::kNil);
      HIRInstruction* end = Return(HIRInstruction::kReturn);
      end->AddArg(val);
    }

    return NULL;
  } else {
    HIRFunction* f = new HIRFunction(stmt);
    f->arg_count = fn->args()->length();

    work_queue_.Push(f);
    return Add(f);
  }
}


HIRInstruction* HIRGen::VisitAssign(AstNode* stmt) {
  HIRInstruction* rhs = Visit(stmt->rhs());

  if (stmt->lhs()->is(AstNode::kValue)) {
    AstValue* value = AstValue::Cast(stmt->lhs());

    if (value->slot()->is_stack()) {
      // No instruction is needed
      Assign(value->slot(), rhs);
    } else {
      Add(new HIRStoreContext(value->slot()))->AddArg(rhs);
    }
    return rhs;
  } else if (stmt->lhs()->is(AstNode::kMember)) {
    HIRInstruction* property = Visit(stmt->lhs()->rhs());
    HIRInstruction* receiver = Visit(stmt->lhs()->lhs());

    return Add(new HIRStoreProperty())
        ->AddArg(receiver)
        ->AddArg(property)
        ->AddArg(rhs);
  } else {
    UNEXPECTED
  }
}


HIRInstruction* HIRGen::VisitReturn(AstNode* stmt) {
  HIRInstruction* lhs = Visit(stmt->lhs());
  return Return(HIRInstruction::kReturn)->AddArg(lhs);
}


HIRInstruction* HIRGen::VisitValue(AstNode* stmt) {
  AstValue* value = AstValue::Cast(stmt);
  ScopeSlot* slot = value->slot();
  if (slot->is_stack()) {
    HIRInstruction* i = current_block()->env()->At(slot);

    if (i != NULL && i->block() == current_block()) {
      // Local value
      return i;
    } else {
      HIRPhi* phi = CreatePhi(slot);
      if (i != NULL) phi->AddInput(i);

      // External value
      return Add(Assign(slot, phi));
    }
  } else {
    return Add(new HIRLoadContext(slot));
  }
}


HIRInstruction* HIRGen::VisitIf(AstNode* stmt) {
  HIRBlock* t = CreateBlock();
  HIRBlock* f = CreateBlock();
  HIRInstruction* cond = Visit(stmt->lhs());

  Branch(HIRInstruction::kIf, t, f)->AddArg(cond);

  set_current_block(t);
  Visit(stmt->rhs());
  t = current_block();

  AstList::Item* else_branch = stmt->children()->head()->next()->next();

  if (else_branch != NULL) {
    set_current_block(f);
    Visit(else_branch->value());
    f = current_block();
  }

  set_current_block(Join(t, f));

  return NULL;
}


HIRInstruction* HIRGen::VisitWhile(AstNode* stmt) {
  BreakContinueInfo* old = break_continue_info_;
  HIRBlock* start = CreateBlock();

  current_block()->MarkPreLoop();
  Goto(HIRInstruction::kGoto, start);

  // HIRBlock can't be join and branch at the same time
  set_current_block(CreateBlock());
  start->MarkLoop();
  start->Goto(HIRInstruction::kGoto, current_block());

  HIRInstruction* cond = Visit(stmt->lhs());

  HIRBlock* body = CreateBlock();
  HIRBlock* loop = CreateBlock();
  HIRBlock* end = CreateBlock();

  Branch(HIRInstruction::kIf, body, end)->AddArg(cond);

  set_current_block(body);
  break_continue_info_ = new BreakContinueInfo(this, end);

  Visit(stmt->rhs());

  while (break_continue_info_->continue_blocks()->length() > 0) {
    HIRBlock* next = break_continue_info_->continue_blocks()->Shift();
    Goto(HIRInstruction::kGoto, next);
    set_current_block(next);
  }
  Goto(HIRInstruction::kGoto, loop);
  loop->Goto(HIRInstruction::kGoto, start);

  // Next current block should not be a join
  set_current_block(break_continue_info_->GetBreak());

  // Restore break continue info
  break_continue_info_ = old;

  return NULL;
}


HIRInstruction* HIRGen::VisitBreak(AstNode* stmt) {
  assert(break_continue_info_ != NULL);
  Goto(HIRInstruction::kGoto, break_continue_info_->GetBreak());
  return NULL;
}


HIRInstruction* HIRGen::VisitContinue(AstNode* stmt) {
  assert(break_continue_info_ != NULL);
  Goto(HIRInstruction::kGoto, break_continue_info_->GetContinue());
  return NULL;
}


HIRInstruction* HIRGen::VisitUnOp(AstNode* stmt) {
  UnOp* op = UnOp::Cast(stmt);
  BinOp::BinOpType type;

  if (op->is_changing()) {
    HIRInstruction* load = NULL;
    HIRInstruction* res = NULL;
    HIRInstruction* value = NULL;

    // ++i, i++
    AstNode* one = new AstNode(AstNode::kNumber, stmt);

    one->value("1");
    one->length(1);

    type = (op->subtype() == UnOp::kPreInc || op->subtype() == UnOp::kPostInc) ?
          BinOp::kAdd : BinOp::kSub;

    AstNode* wrap = new BinOp(type, op->lhs(), one);

    if (op->subtype() == UnOp::kPreInc || op->subtype() == UnOp::kPreDec) {
      res = Visit(wrap);
      load = res->args()->head()->value();
      value = res;
    } else {
      HIRInstruction* ione = Visit(one);
      res = Visit(op->lhs());
      load = res;

      HIRInstruction* bin = Add(new HIRBinOp(type))->AddArg(res)->AddArg(ione);

      bin->ast(wrap);
      value = bin;
    }

    // Assign new value to variable
    if (op->lhs()->is(AstNode::kValue)) {
      ScopeSlot* slot = AstValue::Cast(op->lhs())->slot();

      if (slot->is_stack()) {
        // No instruction is needed
        Assign(slot, value);
      } else {
        Add(new HIRStoreContext(slot))->AddArg(value);
      }
    } else if (op->lhs()->is(AstNode::kMember)) {
      HIRInstruction* receiver = load->args()->head()->value();
      HIRInstruction* property = load->args()->tail()->value();

      Add(new HIRStoreProperty())
          ->AddArg(receiver)
          ->AddArg(property)
          ->AddArg(value);
    } else {
      UNEXPECTED
    }

    return res;
  } else if (op->subtype() == UnOp::kPlus || op->subtype() == UnOp::kMinus) {
    // +i = 0 + i,
    // -i = 0 - i
    AstNode* zero = new AstNode(AstNode::kNumber, stmt);
    zero->value("0");
    zero->length(1);

    type = op->subtype() == UnOp::kPlus ? BinOp::kAdd : BinOp::kSub;

    AstNode* wrap = new BinOp(type, zero, op->lhs());

    return Visit(wrap);
  } else if (op->subtype() == UnOp::kNot) {
    HIRInstruction* lhs = Visit(op->lhs());
    return Add(HIRInstruction::kNot)->AddArg(lhs);
  } else {
    UNEXPECTED
  }
}


HIRInstruction* HIRGen::VisitBinOp(AstNode* stmt) {
  BinOp* op = BinOp::Cast(stmt);
  HIRInstruction* res;

  if (!BinOp::is_bool_logic(op->subtype())) {
    HIRInstruction* lhs = Visit(op->lhs());
    HIRInstruction* rhs = Visit(op->rhs());
    res = Add(new HIRBinOp(op->subtype()))->AddArg(lhs)->AddArg(rhs);
  } else {
    HIRInstruction* lhs = Visit(op->lhs());
    HIRBlock* branch = CreateBlock();
    ScopeSlot* slot = current_block()->env()->logic_slot();

    Goto(HIRInstruction::kGoto, branch);
    set_current_block(branch);

    HIRBlock* t = CreateBlock();
    HIRBlock* f = CreateBlock();

    Branch(HIRInstruction::kIf, t, f)->AddArg(lhs);

    set_current_block(t);
    if (op->subtype() == BinOp::kLAnd) {
      Assign(slot, Visit(op->rhs()));
    } else {
      Assign(slot, lhs);
    }
    t = current_block();

    set_current_block(f);
    if (op->subtype() == BinOp::kLAnd) {
      Assign(slot, lhs);
    } else {
      Assign(slot, Visit(op->rhs()));
    }
    f = current_block();

    set_current_block(Join(t, f));
    HIRPhi* phi =  current_block()->env()->PhiAt(slot);
    assert(phi != NULL);

    return phi;
  }

  res->ast(stmt);
  return res;
}


HIRInstruction* HIRGen::VisitObjectLiteral(AstNode* stmt) {
  HIRInstruction* res = Add(new HIRAllocateObject());
  ObjectLiteral* obj = ObjectLiteral::Cast(stmt);

  AstList::Item* khead = obj->keys()->head();
  AstList::Item* vhead = obj->values()->head();
  for (; khead != NULL; khead = khead->next(), vhead = vhead->next()) {
    HIRInstruction* value = Visit(vhead->value());
    HIRInstruction* key = Visit(khead->value());

    Add(new HIRStoreProperty())
        ->AddArg(res)
        ->AddArg(key)
        ->AddArg(value);
  }

  return res;
}


HIRInstruction* HIRGen::VisitArrayLiteral(AstNode* stmt) {
  HIRInstruction* res = Add(new HIRAllocateArray());

  AstList::Item* head = stmt->children()->head();
  for (uint64_t i = 0; head != NULL; head = head->next(), i++) {
    HIRInstruction* key = GetNumber(i);
    HIRInstruction* value = Visit(head->value());

    Add(new HIRStoreProperty())
        ->AddArg(res)
        ->AddArg(key)
        ->AddArg(value);
  }

  return res;
}


HIRInstruction* HIRGen::VisitMember(AstNode* stmt) {
  HIRInstruction* prop = Visit(stmt->rhs());
  HIRInstruction* recv = Visit(stmt->lhs());
  return Add(HIRInstruction::kLoadProperty)->AddArg(recv)->AddArg(prop);
}


HIRInstruction* HIRGen::VisitDelete(AstNode* stmt) {
  HIRInstruction* prop = Visit(stmt->lhs()->rhs());
  HIRInstruction* recv = Visit(stmt->lhs()->lhs());
  Add(HIRInstruction::kDeleteProperty)->AddArg(recv)->AddArg(prop);

  // Delete property returns nil
  return Add(HIRInstruction::kNil);
}


HIRInstruction* HIRGen::VisitCall(AstNode* stmt) {
  FunctionLiteral* fn = FunctionLiteral::Cast(stmt);

  // handle __$gc() and __$trace() calls
  if (fn->variable()->is(AstNode::kValue)) {
    AstNode* name = AstValue::Cast(fn->variable())->name();
    if (name->length() == 5 && strncmp(name->value(), "__$gc", 5) == 0) {
      Add(HIRInstruction::kCollectGarbage);
      return Add(HIRInstruction::kNil);
    } else if (name->length() == 8 &&
               strncmp(name->value(), "__$trace", 8) == 0) {
      return Add(HIRInstruction::kGetStackTrace);
    }
  }

  // Generate all arg's values and populate list of stores
  HIRInstruction* vararg = NULL;
  HIRInstructionList stores_;
  AstList::Item* item = fn->args()->head();
  for (; item != NULL; item = item->next()) {
    AstNode* arg = item->value();
    HIRInstruction::Type type;
    HIRInstruction* rhs;

    if (arg->is(AstNode::kSelf)) {
      // Process self argument later
      continue;
    } else if (arg->is(AstNode::kVarArg)) {
      type = HIRInstruction::kStoreVarArg;
      rhs = Visit(arg->lhs());
      vararg = rhs;
    } else {
      type = HIRInstruction::kStoreArg;
      rhs = Visit(arg);
    }

    HIRInstruction* current = CreateInstruction(type);
    current->AddArg(rhs);

    stores_.Unshift(current);
  }

  // Determine argc and alignment
  int argc = fn->args()->length();
  if (vararg != NULL) argc--;

  HIRInstruction* hargc = GetNumber(argc);

  // If call has vararg - increase argc by ...
  if (vararg != NULL) {
    HIRInstruction* length = Add(new HIRSizeof())
        ->AddArg(vararg);

    // ... by the length of vararg
    hargc = Add(new HIRBinOp(BinOp::kAdd))->AddArg(hargc)->AddArg(length);
  }

  // Process self argument
  HIRInstruction* receiver = NULL;
  if (fn->args()->length() > 0 &&
      fn->args()->head()->value()->is(AstNode::kSelf)) {
    receiver = Visit(fn->variable()->lhs());
    HIRInstruction* store = CreateInstruction(HIRInstruction::kStoreArg);
    store->AddArg(receiver);
    stores_.Push(store);
  }

  HIRInstruction* var;
  if (fn->args()->length() > 0 &&
      fn->args()->head()->value()->is(AstNode::kSelf)) {
    assert(fn->variable()->is(AstNode::kMember));

    HIRInstruction* property = Visit(fn->variable()->rhs());

    var = Add(HIRInstruction::kLoadProperty)->AddArg(receiver)
                                            ->AddArg(property);
  } else {
    var = Visit(fn->variable());
  }

  // Add stack alignment instruction
  Add(HIRInstruction::kAlignStack)->AddArg(hargc);

  // Now add stores to hir
  HIRInstructionList::Item* hhead = stores_.head();
  for (; hhead != NULL; hhead = hhead->next()) {
    Add(hhead->value());
  }

  HIRInstruction* call = CreateInstruction(HIRInstruction::kCall)
      ->AddArg(var)->AddArg(hargc);

  return Add(call);
}


HIRInstruction* HIRGen::VisitTypeof(AstNode* stmt) {
  HIRInstruction* lhs = Visit(stmt->lhs());
  return Add(new HIRTypeof())->AddArg(lhs);
}


HIRInstruction* HIRGen::VisitKeysof(AstNode* stmt) {
  HIRInstruction* lhs = Visit(stmt->lhs());
  return Add(new HIRKeysof())->AddArg(lhs);
}

HIRInstruction* HIRGen::VisitSizeof(AstNode* stmt) {
  HIRInstruction* lhs = Visit(stmt->lhs());
  return Add(new HIRSizeof())->AddArg(lhs);
}


HIRInstruction* HIRGen::VisitClone(AstNode* stmt) {
  HIRInstruction* lhs = Visit(stmt->lhs());
  return Add(new HIRClone())->AddArg(lhs);
}


// Literals


HIRInstruction* HIRGen::VisitLiteral(AstNode* stmt) {
  HIRInstruction* i = Add(new HIRLiteral(stmt->type(), root_.Put(stmt)));

  i->ast(stmt);

  return i;
}


HIRInstruction* HIRGen::VisitNumber(AstNode* stmt) {
  return VisitLiteral(stmt);
}


HIRInstruction* HIRGen::VisitNil(AstNode* stmt) {
  return Add(HIRInstruction::kNil);
}


HIRInstruction* HIRGen::VisitTrue(AstNode* stmt) {
  return VisitLiteral(stmt);
}


HIRInstruction* HIRGen::VisitFalse(AstNode* stmt) {
  return VisitLiteral(stmt);
}


HIRInstruction* HIRGen::VisitString(AstNode* stmt) {
  return VisitLiteral(stmt);
}


HIRInstruction* HIRGen::VisitProperty(AstNode* stmt) {
  return VisitLiteral(stmt);
}


HIRBlock::HIRBlock(HIRGen* g) : id(g->block_id()),
                                dfs_id(-1),
                                g_(g),
                                loop_(false),
                                ended_(false),
                                env_(NULL),
                                pred_count_(0),
                                succ_count_(0),
                                parent_(NULL),
                                ancestor_(NULL),
                                label_(this),
                                semi_(this),
                                dominator_(NULL),
                                lir_(NULL),
                                start_id_(-1),
                                end_id_(-1) {
  pred_[0] = NULL;
  pred_[1] = NULL;
  succ_[0] = NULL;
  succ_[1] = NULL;
}


HIRInstruction* HIRBlock::Assign(ScopeSlot* slot, HIRInstruction* value) {
  assert(value != NULL);

  value->slot(slot);
  env()->Set(slot, value);

  return value;
}


void HIRBlock::AddPredecessor(HIRBlock* b) {
  assert(pred_count_ < 2);
  pred_[pred_count_++] = b;

  if (pred_count_ == 1) {
    // Fast path - copy environment
    env()->Copy(b->env());
    return;
  }

  for (int i = 0; i < b->env()->stack_slots(); i++) {
    HIRInstruction* curr = b->env()->At(i);
    if (curr == NULL) continue;

    HIRInstruction* old = this->env()->At(i);

    // Value already present in block
    if (old != NULL) {
      HIRPhi* phi = this->env()->PhiAt(i);

      // In loop values can be propagated up to the block where it was declared
      if (old == curr) continue;

      // Create phi if needed
      if (phi == NULL || phi->block() != this) {
        assert(phis_.length() == instructions_.length());

        phi = CreatePhi(curr->slot());
        Add(phi);
        phi->AddInput(old);

        Assign(curr->slot(), phi);
      }

      // Add value as phi's input
      phi->AddInput(curr);
    } else {
      // Propagate value
      this->env()->Set(i, curr);
    }
  }
}


void HIRBlock::MarkPreLoop() {
  // Every slot that wasn't seen before should have nil value
  for (int i = 0; i < env()->stack_slots() - 1; i++) {
    if (env()->At(i) != NULL) continue;

    ScopeSlot* slot = new ScopeSlot(ScopeSlot::kStack);
    slot->index(i);

    Assign(slot, Add(HIRInstruction::kNil));
  }
}


void HIRBlock::MarkLoop() {
  loop_ = true;

  // Create phi for every possible value (except logic_slot)
  for (int i = 0; i < env()->stack_slots() - 1; i++) {
    ScopeSlot* slot = new ScopeSlot(ScopeSlot::kStack);
    slot->index(i);

    HIRInstruction* old = env()->At(i);
    HIRPhi* phi = CreatePhi(slot);
    if (old != NULL) phi->AddInput(old);

    Add(Assign(slot, phi));
  }
}


void HIRGen::Replace(HIRInstruction* o, HIRInstruction* n) {
  HIRInstructionList::Item* head = o->uses()->head();
  for (; head != NULL; head = head->next()) {
    HIRInstruction* use = head->value();

    use->ReplaceArg(o, n);
  }
}


void HIRBlock::Remove(HIRInstruction* instr) {
  HIRInstructionList::Item* head = instructions_.head();
  HIRInstructionList::Item* next;
  for (; head != NULL; head = next) {
    HIRInstruction* i = head->value();
    next = head->next();

    if (i == instr) {
      instructions_.Remove(head);
      break;
    }
  }

  instr->Remove();
}


HIREnvironment::HIREnvironment(int stack_slots)
    : stack_slots_(stack_slots + 1) {
  // ^^ NOTE: One stack slot is reserved for bool logic binary operations
  logic_slot_ = new ScopeSlot(ScopeSlot::kStack);
  logic_slot_->index(stack_slots);

  instructions_ = reinterpret_cast<HIRInstruction**>(Zone::current()->Allocate(
      sizeof(*instructions_) * stack_slots_));
  memset(instructions_, 0, sizeof(*instructions_) * stack_slots_);

  phis_ = reinterpret_cast<HIRPhi**>(Zone::current()->Allocate(
      sizeof(*phis_) * stack_slots_));
  memset(phis_, 0, sizeof(*phis_) * stack_slots_);
}


void HIREnvironment::Copy(HIREnvironment* from) {
  memcpy(instructions_,
         from->instructions_,
         sizeof(*instructions_) * stack_slots_);
  memcpy(phis_,
         from->phis_,
         sizeof(*phis_) * stack_slots_);
}


BreakContinueInfo::BreakContinueInfo(HIRGen *g, HIRBlock* end) : g_(g),
                                                                 brk_(end) {
}


HIRBlock* BreakContinueInfo::GetContinue() {
  HIRBlock* b = g_->CreateBlock();

  continue_blocks()->Push(b);

  return b;
}


HIRBlock* BreakContinueInfo::GetBreak() {
  HIRBlock* b = g_->CreateBlock();
  brk_->Goto(HIRInstruction::kGoto, b);
  brk_ = b;

  return b;
}

} // namespace internal
} // namespace candor
