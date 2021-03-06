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

#include "hir.h"

#include <string.h>  // memset, memcpy

#include "hir-inl.h"
#include "macroassembler.h"  // Label
#include "splay-tree.h"

namespace candor {
namespace internal {

bool HIRGen::log_ = false;

HIRGen::HIRGen(Heap* heap, Root* root, const char* filename)
    : Visitor<HIRInstruction>(kPreorder),
      current_block_(NULL),
      current_root_(NULL),
      break_continue_info_(NULL),
      root_(root),
      filename_(filename),
      loop_depth_(0),
      block_id_(0),
      instr_id_(-2),
      dfs_id_(0) {
}


HIRGen::~HIRGen() {
  // Bitmaps in blocks are allocated :(
  HIRBlock* b;
  while ((b = blocks_.Shift()) != NULL) {
    delete b;
  }
}


void HIRGen::Build(AstNode* root) {
  HIRFunction* current = new HIRFunction(root);
  current->Init(this, NULL);

  HIRBlock* b = CreateBlock(current->ast()->stack_slots());
  set_current_block(b);
  set_current_root(b);

  roots_.Push(b);

  // Lazily create labels
  if (current->ast()->label() == NULL) {
    current->ast()->label(new Label());
  }

  Visit(current->ast());

  set_current_root(NULL);

  // Optimize
  FindReachableBlocks();
  DeriveDominators();
  PrunePhis();
  FindEffects();
  EliminateDeadCode();
  GlobalValueNumbering();
  GlobalCodeMotion();

  if (log_) {
    PrintBuffer p(stdout);
    p.Print("## HIR %s Start ##\n", filename_ == NULL ? "unknown" : filename_);
    Print(&p);
    p.Print("## HIR End ##\n");
  }
}


void HIRGen::EnableLogging() {
  log_ = true;
}


void HIRGen::DisableLogging() {
  log_ = false;
}


void HIRGen::PrunePhis() {
  HIRPhiList queue_;
  HIRPhiList phis_;

  // First - get list of all phis in all blocks
  // (and remove them from those blocks for now).
  HIRBlockList::Item* bhead = blocks_.head();
  for (; bhead != NULL; bhead = bhead->next()) {
    HIRBlock* block = bhead->value();

    while (block->phis()->length() > 0) {
      HIRPhi* phi = block->phis()->Shift();
      queue_.Push(phi);
      phis_.Push(phi);
    }
  }

  // Filter out phis that have zero or one inputs
  HIRPhiList::Item* phead = queue_.head();
  for (; phead != NULL; phead = phead->next()) {
    HIRPhi* phi = phead->value();

    if (phi->input_count() == 2) {
      if (phi->InputAt(1) != phi && phi->InputAt(0) != phi->InputAt(1)) {
        continue;
      }
      phi->input_count(1);
    }

    if (phi->input_count() == 0) {
      phi->Nilify();
      phi->Unpin();
    } else if (phi->input_count() == 1) {
      // Enqueue all phi uses
      HIRInstructionList::Item* head = phi->uses()->head();
      for (; head != NULL; head = head->next()) {
        if (!head->value()->IsRemoved() &&
            head->value()->Is(HIRInstruction::kPhi)) {
          HIRPhi* use = HIRPhi::Cast(head->value());
          queue_.Push(use);
        }
      }

      Replace(phi, phi->InputAt(0));
      phi->block()->Remove(phi);
    }
  }

  // Put phis back into blocks
  phead = phis_.head();
  for (; phead != NULL; phead = phead->next()) {
    HIRPhi* phi = phead->value();

    // Do not add removed and niled
    if (!phi->Is(HIRInstruction::kPhi) ||phi->IsRemoved()) continue;

    // Remove unused phis
    if (phi->uses()->length() == 0) {
      phi->block()->Remove(phi);
      continue;
    }
    phi->block()->phis()->Push(phi);
  }
}


void HIRGen::FindReachableBlocks() {
  bool change;
  do {
    change = false;

    HIRBlockList::Item* bhead = blocks_.head();
    for (; bhead != NULL; bhead = bhead->next()) {
      HIRBlock* block = bhead->value();

      for (int i = 0; i < block->succ_count(); i++) {
        block->SuccAt(i)->reachable_from()->Set(block->id);
        if (block->reachable_from()->Copy(
                block->SuccAt(i)->reachable_from())) {
          change = true;
        }
      }
    }
  } while (change);
};


// Implementation of:
//   A fast algorithm for finding dominators in a flowgraph,
//   by T Lengauer, RE Tarjan
void HIRGen::DeriveDominators() {
  // Perform actions for each subtree
  HIRBlockList::Item* rhead = roots_.head();
  for (; rhead != NULL; rhead = rhead->next()) {
    HIRBlockList dfs_blocks_;
    HIRBlock* root = rhead->value();

    // Visit and enumerate blocks in DFS order
    EnumerateDFS(root, &dfs_blocks_);

    // Visit all blocks except root in reverse order
    HIRBlockList::Item* dhead = dfs_blocks_.tail();
    for (; dhead != dfs_blocks_.head(); dhead = dhead->prev()) {
      HIRBlock* w = dhead->value();
      HIRBlock* parent = w->parent();

      // Propagate dominators from predecessors
      for (int i = 0; i < w->pred_count(); i++) {
        // Skip unreachable predecessors
        if (w->PredAt(i)->dfs_id == -1) continue;

        HIRBlock* u = w->PredAt(i)->Evaluate();
        if (u->semi()->dfs_id < w->semi()->dfs_id) w->semi(u->semi());
      }
      w->semi()->dominates()->Push(w);
      w->ancestor(parent);

      // Time to empty parent's bucket and set semidominators
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
        assert(w->dominator() != NULL);
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


void HIRGen::EliminateDeadCode() {
  HIRInstructionList instructions_;

  // For each block
  HIRBlockList::Item* bhead = blocks_.head();
  for (; bhead != NULL; bhead = bhead->next()) {
    HIRBlock* block = bhead->value();

    // Visit instructions with side effects
    HIRInstruction* instr;
    while ((instr = block->instructions()->Shift()) != NULL) {
      instructions_.Push(instr);
      if (!instr->HasSideEffects()) continue;

      EliminateDeadCode(instr);
    }
  }

  // And filter out instructions that are live
  HIRInstructionList::Item* ihead = instructions_.head();
  for (; ihead != NULL; ihead = ihead->next()) {
    HIRInstruction* instr = ihead->value();
    if (!instr->is_live) continue;
    instr->block()->instructions()->Push(instr);
  }
}


void HIRGen::EliminateDeadCode(HIRInstruction* instr) {
  // Skip already process instructions
  if (instr->is_live) return;
  instr->is_live = true;

  // Inputs of live instructions are live
  HIRInstructionList::Item* ahead = instr->args()->head();
  for (; ahead != NULL; ahead = ahead->next()) {
    EliminateDeadCode(ahead->value());
  }
}


void HIRGen::FindEffects() {
  // For each block
  HIRBlockList::Item* bhead = blocks_.head();
  for (; bhead != NULL; bhead = bhead->next()) {
    HIRBlock* block = bhead->value();

    // Visit instructions in order uses -> instr
    HIRInstructionList::Item* ihead = block->instructions()->head();
    for (; ihead != NULL; ihead = ihead->next()) {
      HIRInstruction* instr = ihead->value();
      FindOutEffects(instr);
    }
  }

  // For each block
  bhead = blocks_.head();
  for (; bhead != NULL; bhead = bhead->next()) {
    HIRBlock* block = bhead->value();

    // Visit instructions in order args -> instr
    HIRInstructionList::Item* ihead = block->instructions()->head();
    for (; ihead != NULL; ihead = ihead->next()) {
      HIRInstruction* instr = ihead->value();
      FindInEffects(instr);
    }
  }
}


void HIRGen::FindOutEffects(HIRInstruction* instr) {
  if (instr->alias_visited == 1) return;
  instr->alias_visited = 1;

  SplayTree<NumberKey, HIRInstruction, NopPolicy, ZoneObject> effects_;

  HIRInstructionList::Item* uhead = instr->uses()->head();
  for (; uhead != NULL; uhead = uhead->next()) {
    HIRInstruction* use = uhead->value();

    // Process uses first
    FindOutEffects(use);

    // And copy their effects in
    HIRInstructionList::Item* ehead = use->effects_out()->head();
    for (; ehead != NULL; ehead = ehead->next()) {
      HIRInstruction* effect = ehead->value();

      NumberKey* key = NumberKey::New(effect->id);
      if (!effects_.Insert(key, effect)) continue;
      instr->effects_out()->Push(effect);
    }

    // Phi effects it's inputs, and call effects it's arguments
    if (use->Effects(instr)) {
      NumberKey* key = NumberKey::New(use->id);
      if (!effects_.Insert(key, use)) continue;
      instr->effects_out()->Push(use);
    }
  }
}


void HIRGen::FindInEffects(HIRInstruction* instr) {
  if (instr->alias_visited == 2) return;
  instr->alias_visited = 2;

  SplayTree<NumberKey, HIRInstruction, NopPolicy, ZoneObject> effects_;

  HIRInstructionList::Item* ahead = instr->args()->head();
  for (; ahead != NULL; ahead = ahead->next()) {
    HIRInstruction* arg = ahead->value();

    // If inputs are under effect - instruction is under effect too
    FindInEffects(arg);
    HIRInstructionList::Item* ehead = instr->effects_in()->head();
    for (; ehead != NULL; ehead = ehead->next()) {
      HIRInstruction* effect = ehead->value();
      NumberKey* key = NumberKey::New(effect->id);
      if (!effects_.Insert(key, effect)) continue;
      instr->effects_in()->Push(effect);
    }

    ehead = arg->effects_out()->head();
    for (; ehead != NULL; ehead = ehead->next()) {
      // If instruction may be reachable from one of outcoming effects of
      // it's arguments are under effect.
      HIRInstruction* effect = ehead->value();
      if (instr->block()->reachable_from()->Test(effect->block()->id) ||
          (instr->block() == effect->block() && effect->id < instr->id)) {
        NumberKey* key = NumberKey::New(effect->id);
        if (!effects_.Insert(key, effect)) continue;
        instr->effects_in()->Push(effect);
      }
    }
  }
}


void HIRGen::GlobalValueNumbering() {
  HIRGVNMap* gvn = NULL;
  HIRBlock* root = NULL;

  // For each block
  HIRBlockList::Item* bhead = blocks_.head();
  for (; bhead != NULL; bhead = bhead->next()) {
    HIRBlock* block = bhead->value();

    if (root != block->root()) {
      root = block->root();
      gvn = new HIRGVNMap();
    }

    // Visit instructions that wasn't yet visited
    HIRInstructionList::Item* ihead = block->instructions()->head();
    for (; ihead != NULL; ihead = ihead->next()) {
      HIRInstruction* instr = ihead->value();

      GlobalValueNumbering(instr, gvn);
    }
  }
}


void HIRGen::GlobalValueNumbering(HIRInstruction* instr,
                                  HIRGVNMap* gvn) {
  if (instr->gvn_visited) return;
  instr->gvn_visited = 1;

  // Instructions with side effects can't be replaced
  if (instr->HasGVNSideEffects()) return;

  // Process inputs first
  HIRInstructionList::Item* ahead = instr->args()->head();
  for (; ahead != NULL; ahead = ahead->next()) {
    HIRInstruction* arg = ahead->value();
    GlobalValueNumbering(arg, gvn);
  }

  HIRInstruction* copy = gvn->Get(instr);

  // If there're already equivalent instruction in GVN, replace current with it
  if (copy != NULL) {
    Replace(instr, copy);
    instr->block()->Remove(instr);
    return;
  }

  gvn->Set(instr, instr);
}


// Implementation of Globel Code Motion algorithm from
// Cliff Click's paper.
void HIRGen::GlobalCodeMotion() {
  HIRInstructionList instructions_;

  // For each block
  HIRBlockList::Item* bhead = blocks_.head();
  for (; bhead != NULL; bhead = bhead->next()) {
    HIRBlock* block = bhead->value();

    if (block->IsLoop()) {
      // Pin instructions that are second inputs of loop's phis
      HIRPhiList::Item* phead = block->phis()->head();
      for (; phead != NULL; phead = phead->next()) {
        HIRPhi* phi = phead->value();
        phi->InputAt(1)->Pin();
      }
    }
  }

  // For each block
  bhead = blocks_.head();
  for (; bhead != NULL; bhead = bhead->next()) {
    HIRBlock* block = bhead->value();

    // Schedule arguments of every pinned instruction
    // (And clear blocks)
    HIRInstruction* instr;
    while ((instr = block->instructions()->Shift()) != NULL) {
      instructions_.Push(instr);
      if (!instr->IsPinned()) continue;

      instr->gcm_visited = 1;
      HIRInstructionList::Item* ahead = instr->args()->head();
      for (; ahead != NULL; ahead = ahead->next()) {
        ScheduleEarly(ahead->value(), block->root());
      }
    }
  }

  // Schedule early all unscheduled and unpinned instructions
  HIRInstructionList::Item* ihead = instructions_.head();
  for (; ihead != NULL; ihead = ihead->next()) {
    HIRInstruction* instr = ihead->value();
    if (instr->IsPinned() || instr->gcm_visited == 1) {
      continue;
    }
    ScheduleEarly(instr, instr->block()->root());
  }

  // Schedule uses of every pinned instruction
  ihead = instructions_.head();
  for (; ihead != NULL; ihead = ihead->next()) {
    HIRInstruction* instr = ihead->value();

    if (!instr->IsPinned()) continue;

    instr->gcm_visited = 2;
    HIRInstructionList::Item* uhead = instr->uses()->head();
    for (; uhead != NULL; uhead = uhead->next()) {
      ScheduleLate(uhead->value());
    }
  }

  // Constants have no uses - but schedule them anyway,
  // also visit previously unvisited instructions
  ihead = instructions_.head();
  for (; ihead != NULL; ihead = ihead->next()) {
    HIRInstruction* instr = ihead->value();
    if (instr->IsPinned() || instr->gcm_visited == 2) {
      continue;
    }
    ScheduleLate(instr);
  }

  // Put instructions back into blocks
  ihead = instructions_.tail();
  for (; ihead != NULL; ihead = ihead->prev()) {
    HIRInstruction* instr = ihead->value();

    if (instr->Is(HIRInstruction::kGoto) ||
        instr->Is(HIRInstruction::kIf) ||
        instr->Is(HIRInstruction::kReturn)) {
      // Control instructions are always at end
      instr->block()->instructions()->Push(instr);
    } else {
      // Others in any positions
      instr->block()->instructions()->Unshift(instr);
    }
  }
}


void HIRGen::ScheduleEarly(HIRInstruction* instr, HIRBlock* root) {
  // Ignore already visited instructions
  if (instr->gcm_visited) return;
  instr->gcm_visited = 1;
  if (instr->IsPinned()) return;

  // Start with the shallowest dominator
  if (instr->effects_in()->length() == 0) instr->block(root);

  // Schedule all inputs
  HIRInstructionList::Item* ahead = instr->args()->head();
  for (; ahead != NULL; ahead = ahead->next()) {
    HIRInstruction* arg = ahead->value();
    ScheduleEarly(arg, root);

    // Choose the deepest input in dominator tree
    if (instr->block()->dominator_depth() < arg->block()->dominator_depth()) {
      instr->block(arg->block());
    }
  }
}


void HIRGen::ScheduleLate(HIRInstruction* instr) {
  // Ignore already visited instructions
  if (instr->gcm_visited == 2) return;
  instr->gcm_visited = 2;
  if (instr->IsPinned()) return;

  HIRBlock* lca = NULL;

  HIRInstructionList::Item* uhead = instr->uses()->head();
  for (; uhead != NULL; uhead = uhead->next()) {
    HIRInstruction* use = uhead->value();
    ScheduleLate(use);
    HIRBlock* use_block = use->block();

    // Use occurs in `use`'s block, for phis
    if (use->Is(HIRInstruction::kPhi)) {
      int j = HIRPhi::Cast(use)->InputAt(0) == instr ? 0 : 1;
      use_block = use->block()->PredAt(j);
    }

    lca = FindLCA(lca, use_block);
  }

  if (lca == NULL) lca = instr->block();

  // Select best block between ->block() and lca
  HIRBlock* best = lca;

  if (lca->loop_depth < best->loop_depth) best = lca;

  while (lca != instr->block()) {
    lca = lca->dominator();
    if (lca == NULL) break;
    if (!lca->reachable_from()->Test(instr->block()->id) &&
        lca != instr->block()) {
      break;
    }

    if (lca->loop_depth < best->loop_depth) best = lca;
  }

  instr->block(best);
}


HIRBlock* HIRGen::FindLCA(HIRBlock* a, HIRBlock* b) {
  if (a == NULL) return b;

  while (a->dominator_depth() > b->dominator_depth()) {
    a = a->dominator();
  }

  while (b->dominator_depth() > a->dominator_depth()) {
    b = b->dominator();
  }

  while (a != b) {
    a = a->dominator();
    b = b->dominator();
  }

  return a;
}


HIRInstruction* HIRGen::Visit(AstNode* stmt) {
  // Do not generate code for functions in the ends of the graph
  if (current_block()->IsEnded()) return Add(new HIRNil());

  return Visitor<HIRInstruction>::Visit(stmt);
}


HIRInstruction* HIRGen::VisitFunction(AstNode* stmt) {
  FunctionLiteral* fn = FunctionLiteral::Cast(stmt);

  if (fn->label() == NULL) {
    fn->label(new Label());
  }

  if (current_root() == current_block() &&
      current_block()->IsEmpty()) {
    Add(new HIREntry(fn->label(), stmt->context_slots()));
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
        instr = new HIRLoadArg();
      }

      AstValue* value = AstValue::Cast(arg);

      HIRInstruction* varg_rest = NULL;
      HIRInstruction* varg_arr = NULL;
      if (varg) {
        // Result vararg array
        varg_arr = Add(new HIRAllocateArray(HArray::kVarArgLength));

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
          index = Add(new HIRBinOp(BinOp::kAdd))
              ->AddArg(index)
              ->AddArg(hone);
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
      HIRInstruction* val = Add(new HIRNil());
      HIRInstruction* end = Return(new HIRReturn());
      end->AddArg(val);
    }

    return NULL;
  } else {
    HIRFunction* f = new HIRFunction(stmt);
    f->arg_count = fn->args()->length();

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
  } else if (stmt->lhs()->is(AstNode::kMember)) {
    HIRInstruction* property = Visit(stmt->lhs()->rhs());
    HIRInstruction* receiver = Visit(stmt->lhs()->lhs());

    Add(new HIRStoreProperty())
        ->AddArg(receiver)
        ->AddArg(property)
        ->AddArg(rhs);
  } else {
    UNEXPECTED
  }
  return rhs;
}


HIRInstruction* HIRGen::VisitReturn(AstNode* stmt) {
  HIRInstruction* lhs = Visit(stmt->lhs());
  return Return(new HIRReturn())->AddArg(lhs);
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

  Branch(new HIRIf(), t, f)->AddArg(cond);

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
  loop_depth_++;
  BreakContinueInfo* old = break_continue_info_;
  HIRBlock* start = CreateBlock();

  current_block()->MarkPreLoop();
  Goto(start);

  // HIRBlock can't be join and branch at the same time
  set_current_block(CreateBlock());
  start->MarkLoop();
  start->Goto(current_block());

  HIRInstruction* cond = Visit(stmt->lhs());

  HIRBlock* body = CreateBlock();
  HIRBlock* loop = CreateBlock();
  HIRBlock* end = CreateBlock();

  Branch(new HIRIf(), body, end)->AddArg(cond);

  set_current_block(body);
  break_continue_info_ = new BreakContinueInfo(this, end);

  Visit(stmt->rhs());

  while (break_continue_info_->continue_blocks()->length() > 0) {
    HIRBlock* next = break_continue_info_->continue_blocks()->Shift();
    Goto(next);
    set_current_block(next);
  }
  Goto(loop);
  loop->Goto(start);
  end->loop_depth= --loop_depth_;

  // Next current block should not be a join
  set_current_block(break_continue_info_->GetBreak());

  // Restore break continue info
  break_continue_info_ = old;

  return NULL;
}


HIRInstruction* HIRGen::VisitBreak(AstNode* stmt) {
  assert(break_continue_info_ != NULL);
  Goto(break_continue_info_->GetBreak());
  return NULL;
}


HIRInstruction* HIRGen::VisitContinue(AstNode* stmt) {
  assert(break_continue_info_ != NULL);
  Goto(break_continue_info_->GetContinue());
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

      HIRInstruction* bin = Add(new HIRBinOp(type))
          ->Unpin()
          ->AddArg(res)
          ->AddArg(ione);

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
    res = Add(new HIRBinOp(op->subtype()))->Unpin()->AddArg(lhs)->AddArg(rhs);
  } else {
    HIRInstruction* lhs = Visit(op->lhs());
    HIRBlock* branch = CreateBlock();
    ScopeSlot* slot = current_block()->env()->logic_slot();

    Goto(branch);
    set_current_block(branch);

    HIRBlock* t = CreateBlock();
    HIRBlock* f = CreateBlock();

    Branch(new HIRIf(), t, f)->AddArg(lhs);

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
  ObjectLiteral* obj = ObjectLiteral::Cast(stmt);
  HIRInstruction* res = Add(new HIRAllocateObject(obj->keys()->length()));

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
  HIRInstruction* res = Add(new HIRAllocateArray(stmt->children()->length()));

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
  return Add(new HIRLoadProperty())->Unpin()->AddArg(recv)->AddArg(prop);
}


HIRInstruction* HIRGen::VisitDelete(AstNode* stmt) {
  HIRInstruction* prop = Visit(stmt->lhs()->rhs());
  HIRInstruction* recv = Visit(stmt->lhs()->lhs());
  Add(new HIRDeleteProperty())
      ->AddArg(recv)
      ->AddArg(prop);

  // Delete property returns nil
  return Add(new HIRNil());
}


HIRInstruction* HIRGen::VisitCall(AstNode* stmt) {
  FunctionLiteral* fn = FunctionLiteral::Cast(stmt);

  // handle __$gc() and __$trace() calls
  if (fn->variable()->is(AstNode::kValue)) {
    AstNode* name = AstValue::Cast(fn->variable())->name();
    if (name->length() == 5 && strncmp(name->value(), "__$gc", 5) == 0) {
      Add(new HIRCollectGarbage());
      return Add(new HIRNil());
    } else if (name->length() == 8 &&
               strncmp(name->value(), "__$trace", 8) == 0) {
      return Add(new HIRGetStackTrace());
    }
  }

  // Generate all arg's values and populate list of stores
  HIRInstruction* vararg = NULL;
  HIRInstructionList stores_;
  AstList::Item* item = fn->args()->head();
  for (; item != NULL; item = item->next()) {
    AstNode* arg = item->value();
    HIRInstruction* current;
    HIRInstruction* rhs;

    if (arg->is(AstNode::kSelf)) {
      // Process self argument later
      continue;
    } else if (arg->is(AstNode::kVarArg)) {
      current = new HIRStoreVarArg();
      rhs = Visit(arg->lhs());
      vararg = rhs;
    } else {
      current = new HIRStoreArg();
      rhs = Visit(arg);
    }

    current->AddArg(rhs);

    stores_.Unshift(current);
  }

  // Determine argc and alignment
  int argc = fn->args()->length();
  if (vararg != NULL) argc--;

  HIRInstruction* hargc = GetNumber(argc);
  HIRInstruction* length = NULL;

  // If call has vararg - increase argc by ...
  if (vararg != NULL) {
    length = Add(new HIRSizeof())
        ->AddArg(vararg);

    // ... by the length of vararg
    hargc = Add(new HIRBinOp(BinOp::kAdd))->AddArg(hargc)->AddArg(length);
  }

  // Process self argument
  HIRInstruction* receiver = NULL;
  if (fn->args()->length() > 0 &&
      fn->args()->head()->value()->is(AstNode::kSelf)) {
    receiver = Visit(fn->variable()->lhs());
    HIRInstruction* store = new HIRStoreArg();
    store->AddArg(receiver);
    stores_.Push(store);
  }

  HIRInstruction* var;
  if (fn->args()->length() > 0 &&
      fn->args()->head()->value()->is(AstNode::kSelf)) {
    assert(fn->variable()->is(AstNode::kMember));

    HIRInstruction* property = Visit(fn->variable()->rhs());

    var = Add(new HIRLoadProperty())
        ->Unpin()
        ->AddArg(receiver)
        ->AddArg(property);
  } else {
    var = Visit(fn->variable());
  }

  // Add stack alignment instruction
  Add(new HIRAlignStack())->AddArg(hargc);

  // Add indexes to stores
  HIRInstruction* index = GetNumber(0);
  bool seen_varg = false;
  HIRInstructionList::Item* htail = stores_.tail();
  for (int i = 0; htail != NULL; htail = htail->prev(), i++) {
    HIRInstruction* store = htail->value();

    if (store->Is(HIRInstruction::kStoreVarArg)) {
      assert(length != NULL);
      AstNode* one = new AstNode(AstNode::kNumber, stmt);

      one->value("1");
      one->length(1);

      index = Add(new HIRBinOp(BinOp::kAdd))->AddArg(index)->AddArg(length);
      HIRInstruction* hone = Visit(one);
      index = Add(new HIRBinOp(BinOp::kSub))->AddArg(index)->AddArg(hone);
      seen_varg = true;
    }

    store->AddArg(index);

    // No need to recalculate index after last argument
    if (htail->prev() == NULL) continue;

    if (seen_varg) {
      AstNode* one = new AstNode(AstNode::kNumber, stmt);

      one->value("1");
      one->length(1);

      HIRInstruction* hone = Visit(one);
      index = Add(new HIRBinOp(BinOp::kAdd))
          ->AddArg(index)
          ->AddArg(hone);
    } else {
      index = GetNumber(i + 1);
    }
  }

  // Now add stores to hir
  HIRInstructionList::Item* hhead = stores_.head();
  for (int i = 0; hhead != NULL; hhead = hhead->next(), i++) {
    Add(hhead->value());
  }

  return Add(new HIRCall())->AddArg(var)->AddArg(hargc);
}


HIRInstruction* HIRGen::VisitTypeof(AstNode* stmt) {
  HIRInstruction* lhs = Visit(stmt->lhs());
  return Add(new HIRTypeof())->Unpin()->AddArg(lhs);
}


HIRInstruction* HIRGen::VisitKeysof(AstNode* stmt) {
  HIRInstruction* lhs = Visit(stmt->lhs());
  return Add(new HIRKeysof())->Unpin()->AddArg(lhs);
}

HIRInstruction* HIRGen::VisitSizeof(AstNode* stmt) {
  HIRInstruction* lhs = Visit(stmt->lhs());
  return Add(new HIRSizeof())->Unpin()->AddArg(lhs);
}


HIRInstruction* HIRGen::VisitClone(AstNode* stmt) {
  HIRInstruction* lhs = Visit(stmt->lhs());
  return Add(new HIRClone())->AddArg(lhs);
}


// Literals


HIRInstruction* HIRGen::VisitLiteral(AstNode* stmt) {
  HIRInstruction* i = Add(new HIRLiteral(stmt->type(), root_->Put(stmt)))
      ->Unpin();

  i->ast(stmt);

  return i;
}


HIRInstruction* HIRGen::VisitNumber(AstNode* stmt) {
  return VisitLiteral(stmt);
}


HIRInstruction* HIRGen::VisitNil(AstNode* stmt) {
  return Add(new HIRNil())->Unpin();
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
                                loop_depth(-1),
                                g_(g),
                                reachable_from_(256),
                                loop_(false),
                                ended_(false),
                                env_(NULL),
                                pred_count_(0),
                                succ_count_(0),
                                root_(NULL),
                                parent_(NULL),
                                ancestor_(NULL),
                                label_(this),
                                semi_(this),
                                dominator_(NULL),
                                dominator_depth_(-1),
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

    // In loop values can be propagated up to the block where it was declared
    if (old == curr) continue;

    // Value already present in block
    if (old != NULL) {
      HIRPhi* phi = this->env()->PhiAt(i);

      // Create phi if needed
      if (phi == NULL || phi->block() != this) {
        assert(phis_.length() == instructions_.length());
        ScopeSlot* slot = new ScopeSlot(ScopeSlot::kStack);
        slot->index(i);

        phi = CreatePhi(slot);
        Add(phi);
        phi->AddInput(old);

        Assign(slot, phi);
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

    Assign(slot, Add(new HIRNil()));
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
  brk_->Goto(b);
  brk_ = b;

  return b;
}

}  // namespace internal
}  // namespace candor
