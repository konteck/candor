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

#include "lir.h"

#define __STDC_LIMIT_MACROS
#include <limits.h>  // INT_MAX
#include <string.h>  // memset

#include "lir-inl.h"
#include "hir.h"
#include "hir-inl.h"
#include "lir-instructions.h"
#include "lir-instructions-inl.h"
#include "source-map.h"  // SourceMap

namespace candor {
namespace internal {

bool LGen::log_ = false;

LGen::LGen(HIRGen* hir, const char* filename, HIRBlock* root)
    : hir_(hir),
      instr_id_(0),
      interval_id_(0),
      virtual_index_(40),
      current_block_(NULL),
      current_instruction_(NULL),
      intervals_(kIntervalsInitial),
      unhandled_(kIntervalsInitial),
      active_(kIntervalsInitial),
      inactive_(kIntervalsInitial),
      spill_index_(0),
      unhandled_spills_(kSpillsInitial),
      active_spills_(kSpillsInitial),
      inactive_spills_(kSpillsInitial),
      free_spills_(kSpillsInitial) {
  // Initialize fixed intervals
  for (int i = 0; i < kLIRRegisterCount; i++) {
    registers_[i] = CreateRegister(RegisterByIndex(i));
    registers_[i]->MarkFixed();
  }

  FlattenBlocks(root);
  GenerateInstructions();
  ComputeLocalLiveSets();
  ComputeGlobalLiveSets();
  BuildIntervals();
  WalkIntervals();
  ResolveDataFlow();
  AllocateSpills();

  if (log_) {
    PrintBuffer p(stdout);
    p.Print("## LIR %s Start ##\n", filename == NULL ? "unknown" : filename);
    Print(&p, true);
    p.Print("## LIR End ##\n");
  }
}


void LGen::EnableLogging() {
  log_ = true;
}


void LGen::DisableLogging() {
  log_ = false;
}


void LGen::FlattenBlocks(HIRBlock* root) {
  int* visits = reinterpret_cast<int*>(Zone::current()->Allocate(
      sizeof(*visits) * hir_->blocks()->length()));
  memset(visits, 0, sizeof(*visits) * hir_->blocks()->length());

  // Flatten blocks in a linear structure
  HIRBlockList work_queue;

  // Enqueue root
  work_queue.Push(root);

  while (work_queue.length() > 0) {
    HIRBlock* b = work_queue.Shift();

    visits[b->id]++;
    if (b->pred_count() == 0) {
      // Root block
    } else if (b->IsLoop()) {
      // Loop start
      if (visits[b->id] != 1) continue;
    } else if (visits[b->id] != b->pred_count()) {
      // Regular block
      continue;
    }

    // Generate lir form of block if needed
    // (It may be generated in LFunction)
    if (b->lir() == NULL) new LBlock(b);

    blocks_.Push(b);

    for (int i = b->succ_count() - 1; i >= 0; i--) {
      work_queue.Unshift(b->SuccAt(i));
    }
  }
}


void LGen::GenerateInstructions() {
  HIRBlockList::Item* head = blocks_.head();

  for (; head != NULL; head = head->next()) {
    HIRBlock* b = head->value();

    current_block_ = b->lir();
    Add(current_block_->label());

    HIRInstructionList::Item* ihead = b->instructions()->head();
    for (; ihead != NULL; ihead = ihead->next()) {
      current_instruction_ = ihead->value();
      VisitInstruction(ihead->value());
    }
  }
}

#define LGEN_VISIT_SWITCH(V) \
    case HIRInstruction::k##V: Visit##V(instr); break;

void LGen::VisitInstruction(HIRInstruction* instr) {
  switch (instr->type()) {
    HIR_INSTRUCTION_TYPES(LGEN_VISIT_SWITCH)
   default:
    UNEXPECTED
  }
}

// Common functions

void LGen::VisitGoto(HIRInstruction* instr) {
  HIRBlock* succ = instr->block()->SuccAt(0);
  int parent_index = succ->PredAt(0) != instr->block();

  HIRPhiList::Item* head = succ->phis()->head();
  for (; head != NULL; head = head->next()) {
    HIRPhi* phi = head->value();

    // Skip phis that are eliminated by Dead Code Eliminator
    if (!phi->is_live) continue;

    LInstruction* lphi = NULL;

    assert(!phi->IsRemoved());

    // Initialize LIR representation of phi
    if (phi->lir() == NULL) {
      LInterval* iphi = CreateVirtual();

      lphi = new LPhi();
      lphi->AddArg(iphi, LUse::kAny)
          ->SetResult(iphi, LUse::kAny);

      phi->lir(lphi);
    } else {
      lphi = phi->lir();
    }
    assert(lphi != NULL);

    HIRInstruction* input = phi->InputAt(parent_index);
    // Inputs can be not generated yet
    if (input->Is(HIRInstruction::kPhi) && input->lir() == NULL) {
      assert(!input->IsRemoved());
      LInterval* iphi = CreateVirtual();

      LPhi* pinput = new LPhi();
      pinput->AddArg(iphi, LUse::kAny)
            ->SetResult(iphi, LUse::kAny);

      input->lir(pinput);
    }

    Add(new LMove())
        ->SetResult(lphi->result->interval(), LUse::kAny)
        ->AddArg(input, LUse::kAny);
  }

  Bind(new LGoto());
}


void LGen::VisitPhi(HIRInstruction* instr) {
  assert(instr->lir() != NULL);
  assert(instr->lir()->input_count() == 1);
  assert(instr->lir()->result != NULL);

  Bind(instr->lir());
}

#undef LGEN_VISIT_SWITCH

void LGen::ComputeLocalLiveSets() {
  HIRBlockList::Item* head = blocks_.head();

  for (; head != NULL; head = head->next()) {
    HIRBlock* b = head->value();
    LBlock* l = b->lir();

    LInstructionList::Item* ihead = b->lir()->instructions()->head();
    for (; ihead != NULL; ihead = ihead->next()) {
      LInstruction* instr = ihead->value();

      // Inputs to live_gen
      for (int i = 0; i < instr->input_count(); i++) {
        LUse* input = instr->inputs[i];
        NumberKey* key = NumberKey::New(input->interval()->id);

        if (l->live_kill.Get(key) == NULL) {
          l->live_gen.Set(key, input);
        }
      }

      // Scratches to live_kill
      for (int i = 0; i < instr->scratch_count(); i++) {
        LUse* scratch = instr->scratches[i];
        l->live_kill.Set(NumberKey::New(scratch->interval()->id), scratch);
      }

      // Result to live_kill
      if (instr->result) {
        LUse* result = instr->result;
        l->live_kill.Set(NumberKey::New(result->interval()->id), result);
      }
    }
  }
}


void LGen::ComputeGlobalLiveSets() {
  bool change;
  LUseMap::Item* mitem;

  do {
    change = false;

    // Traverse blocks in reverse order
    HIRBlockList::Item* tail = blocks_.tail();
    for (; tail != NULL; tail = tail->prev()) {
      HIRBlock* b = tail->value();
      LBlock* l = b->lir();

      // Every successor's input adds to current's output
      for (int i = 0; i < b->succ_count(); i++) {
        mitem = b->SuccAt(i)->lir()->live_in.head();
        for (; mitem != NULL; mitem = mitem->next_scalar()) {
          if (l->live_out.Get(mitem->key()) == NULL) {
            l->live_out.Set(mitem->key(), mitem->value());
            change = true;
          }
        }
      }

      // Inputs are live_gen...
      mitem = l->live_gen.head();
      for (; mitem != NULL; mitem = mitem->next_scalar()) {
        if (l->live_in.Get(mitem->key()) == NULL) {
          l->live_in.Set(mitem->key(), mitem->value());
          change = true;
        }
      }

      // ...and everything in output that isn't killed by current block
      mitem = l->live_out.head();
      for (; mitem != NULL; mitem = mitem->next_scalar()) {
        if (l->live_in.Get(mitem->key()) == NULL &&
            l->live_kill.Get(mitem->key()) == NULL) {
          l->live_in.Set(mitem->key(), mitem->value());
          change = true;
        }
      }
    }

    // Loop while there're any changes
  } while (change);
}


void LGen::BuildIntervals() {
  // Traverse blocks in reverse order
  HIRBlockList::Item* tail = blocks_.tail();
  LUseMap::Item* mitem;
  for (; tail != NULL; tail = tail->prev()) {
    HIRBlock* b = tail->value();
    LBlock* l = b->lir();

    // Set block's start and end instruction ids
    l->start_id = b->lir()->instructions()->head()->value()->id;
    l->end_id = b->lir()->instructions()->tail()->value()->id;

    // Add full block range to intervals that live out of this block
    // (we'll shorten those range later if needed).
    mitem = l->live_out.head();
    for (; mitem != NULL; mitem = mitem->next_scalar()) {
      mitem->value()->interval()->AddRange(l->start_id, l->end_id + 2);
    }

    // And instructions too
    LInstructionList::Item* itail = b->lir()->instructions()->tail();
    for (; itail != NULL; itail = itail->prev()) {
      LInstruction* instr = itail->value();

      if (instr->HasCall()) {
        for (int i = 0; i < kLIRRegisterCount; i++) {
          if (registers_[i]->Covers(instr->id)) continue;
          registers_[i]->AddRange(instr->id, instr->id + 1);
          registers_[i]->Use(LUse::kRegister, instr);
        }
      }

      if (instr->result) {
        LInterval* res = instr->result->interval();

        // Add [id, id+1) range, result isn't used anywhere except in the
        // instruction itself
        if (res->ranges()->length() == 0) {
          res->AddRange(instr->id, instr->id + 1);
        } else if (l->live_in.Get(NumberKey::New(res->id)) == NULL) {
          // Shorten first range
          res->ranges()->head()->start(instr->id);
        }
      }

      // Scratches are live only right before instruction
      // (this way fixed intervals wouldn't spill it)
      for (int i = 0; i < instr->scratch_count(); i++) {
        instr->scratches[i]->interval()->AddRange(instr->id - 1, instr->id);
      }

      // Inputs are initially live from block's start to instruction
      for (int i = 0; i < instr->input_count(); i++) {
        // If interval's range already covers instruction it should last
        // up to the block's start
        if (!instr->inputs[i]->interval()->Covers(instr->id - 1)) {
          instr->inputs[i]->interval()->AddRange(l->start_id, instr->id);
        }
      }
    }
  }
}


void LGen::ShuffleIntervals(LIntervalList* active,
                            LIntervalList* inactive,
                            LIntervalList* handled,
                            int pos) {
  // Check for intervals in active that are expired or inactive
  for (int i = 0; i < active->length(); i++) {
    LInterval* interval = active->At(i);

    if (interval->end() < pos) {
      // Interval has ended before current position
      active->RemoveAt(i--);
      if (handled != NULL) handled->Push(interval);
    } else if (!interval->Covers(pos)) {
      // Interval isn't covering current position - move to ininterval
      active->RemoveAt(i--);
      inactive->Push(interval);
    }
  }

  // Check for intervals in inactive that are expired or active
  for (int i = 0; i < inactive->length(); i++) {
    LInterval* interval = inactive->At(i);

    if (interval->end() < pos) {
      // Interval has ended before current position
      inactive->RemoveAt(i--);
      if (handled != NULL) handled->Push(interval);
    } else if (interval->Covers(pos)) {
      // Interval is covering current position - move to active
      inactive->RemoveAt(i--);
      active->Push(interval);
    }
  }
}


void LGen::WalkIntervals() {
  // First populate and sort unhandled list
  for (int i = 0; i < intervals_.length(); i++) {
    LInterval* interval = intervals_.At(i);

    // Skip empty intervals
    if (interval->ranges()->length() == 0) continue;

    if (interval->IsFixed()) {
      // Fixed register

      // Skip unused
      if (interval->ranges()->length() == 0) continue;
      inactive_.Push(interval);
    } else if (interval->is_const()) {
      // Rematerialize const intervals before their uses
      for (int i = interval->uses()->length() - 1; i >= 0; i--) {
        LUse* use = interval->uses()->At(i);

        // Skip constant definition
        if (use->instr()->result == use) continue;

        // Skip use in movements that was just created
        if (use->instr()->type() == LInstruction::kGap) continue;

        LInterval* reg = CreateVirtual();
        LGap* gap = GetGap(use->instr()->id - 1);
        gap->Add(interval->Use(LUse::kAny, gap),
                 reg->Use(LUse::kRegister, gap));

        // Replace interval in use
        use->interval(reg);
        reg->AddRange(use->instr()->id - 1, use->instr()->id);

        // Current use could be probably moved
        if (interval->uses()->At(i) != use) i++;
      }
    } else if (interval->is_stackslot()) {
      // Fix gap's stackslots
    } else {
      // Regular virtual one
      assert(interval->is_virtual());
      unhandled_.Push(interval);
    }
  }

  // Sort by starting position
  unhandled_.Sort();
  inactive_.Sort();

  while (unhandled_.length() > 0) {
    // Pick first interval
    LInterval* current = unhandled_.Shift();
    int pos = current->start();

    ShuffleIntervals(&active_, &inactive_, NULL, pos);

    // Skip spilled intervals
    if (!current->is_virtual()) continue;

    // Find free register for current interval
    TryAllocateFreeReg(current);

    // If allocation has failed
    if (!current->is_register()) {
      // Spill something and allocate just-freed register
      AllocateBlockedReg(current);
    }

    // If interval wasn't spilled itself - add it to active
    assert(current->is_register() || current->is_stackslot());
    if (current->is_register()) {
      active_.Push(current);
    }
  }
}


void LGen::TryAllocateFreeReg(LInterval* current) {
  int free_pos[kLIRRegisterCount];

  // Initially all registers are free for any visible future
  for (int i = 0; i < kLIRRegisterCount; i++) {
    free_pos[i] = INT_MAX;
  }

  // But registers that are used by active intervals are not free at all
  for (int i = 0; i < active_.length(); i++) {
    LInterval* active = active_.At(i);
    assert(active->is_register());

    free_pos[active->index()] = 0;
  }

  // Inactive intervals can limit availablity too, but only at the places
  // that are intersecting with current interval
  for (int i = 0; i < inactive_.length(); i++) {
    LInterval* inactive = inactive_.At(i);
    assert(inactive->is_register());

    int pos = current->FindIntersection(inactive);
    if (pos == -1) continue;
    if (free_pos[inactive->index()] <= pos) continue;
    free_pos[inactive->index()] = pos;
  }

  // Now we need to find register that is free for maximum time
  int max = -1;
  int max_reg = 0;
  for (int i = 0; i < kLIRRegisterCount; i++) {
    if (free_pos[i] > max) {
      max = free_pos[i];
      max_reg = i;
    }
  }
  assert(max >= 0);

  // Prefer register hint if possible
  if (current->register_hint != NULL && current->register_hint->is_register()) {
    int reg = current->register_hint->interval()->index();
    if (free_pos[reg] - 2 > current->start()) {
      max = free_pos[reg];
      max_reg = reg;
    }
  }

  // All registers are occupied - failure
  if (max - 2 <= current->start()) return;

  if (max <= current->end()) {
    // Split before `max` is needed
    Split(current, max % 2  == 0 ? (max - 1) : (max - 2));
  }

  // Register is available for whole interval's lifetime
  current->Allocate(max_reg);
}


void LGen::AllocateBlockedReg(LInterval* current) {
  LUse* first_use = current->UseAfter(0, LUse::kRegister);
  if (first_use == NULL) {
    // No register use is needed - just spill interval
    Spill(current);
    return;
  }

  int use_pos[kLIRRegisterCount];
  int block_pos[kLIRRegisterCount];

  for (int i = 0; i < kLIRRegisterCount; i++) {
    use_pos[i] = INT_MAX;
    block_pos[i] = INT_MAX;
  }

  // In all active intervals
  for (int i = 0; i < active_.length(); i++) {
    LInterval* active = active_.At(i);
    int index = active->index();

    if (active->IsFixed()) {
      // Fixed intervals blocks register (i.e. this register can't be spilled)
      block_pos[index] = 0;
      use_pos[index] = 0;
    } else {
      LUse* use = active->UseAfter(current->start());
      if (use == NULL) continue;
      int pos = use->instr()->id;

      // Uses of other intervals are recorded
      if (use_pos[index] > pos) use_pos[index] = pos;
    }
  }

  // Almost he same for inactive
  for (int i = 0; i < inactive_.length(); i++) {
    LInterval* inactive = inactive_.At(i);
    int index = inactive->index();
    int pos = current->FindIntersection(inactive);

    // Count only intersecting intervals
    if (pos == -1) continue;

    if (inactive->IsFixed()) {
      if (block_pos[index] > pos)  block_pos[index] = pos;
      if (use_pos[index] > pos) use_pos[index] = pos;
    } else {
      LUse* use = inactive->UseAfter(current->start());
      if (use == NULL) continue;
      int pos = use->instr()->id;

      if (use_pos[index] > pos) use_pos[index] = pos;
    }
  }

  int use_max = -1;
  int use_reg = 0;
  for (int i = 0; i < kLIRRegisterCount; i++) {
    if (use_pos[i] > use_max) {
      use_max = use_pos[i];
      use_reg = i;
    }
  }
  assert(use_max >= 0);

  if (first_use == NULL ||
      use_max < first_use->instr()->id ||
      block_pos[use_reg] <= current->start()) {
    Spill(current);

    if (first_use != NULL && first_use->instr()->id - 1 > current->start()) {
      Split(current, first_use->instr()->id - 1);
    }
  } else {
    // Intervals using register will be spilled
    current->Allocate(use_reg);

    // If register is blocked somewhere before interval's end
    if (block_pos[use_reg] <= current->end()) {
      // Interval should be splitted
      Split(current, block_pos[use_reg] - 1);
    }

    // Split and spill all intersecting intervals
    int split_pos = current->start();
    if (split_pos % 2 == 0) split_pos--;

    // Active intervals
    for (int i = 0; i < active_.length(); i++) {
      LInterval* interval = active_.At(i);
      if (!interval->IsEqual(current)) continue;

      // Split before current interval, and let allocator process it later
      Split(interval, split_pos);
    }

    // Inactive intervals
    for (int i = 0; i < inactive_.length(); i++) {
      LInterval* interval = inactive_.At(i);
      if (interval->IsFixed() || !interval->IsEqual(current)) continue;

      int intersection = current->FindIntersection(interval);
      if (intersection == -1) continue;

      LUse* next_use = interval->UseAfter(current->start(), LUse::kRegister);

      if (next_use == NULL) {
        // Split before current interval
        Split(interval, split_pos);
      } else {
        int next_pos = next_use->instr()->id;

        if (intersection >= next_pos) {
          // Interval is used before intersection - it would be ok to split it
          // at this position
          Split(interval, intersection);
        } else {
          // Split interval right before next use
          Split(interval, next_pos - 1);
        }
      }

      inactive_.RemoveAt(i--);
    }
  }
}


void LGen::ResolveDataFlow() {
  HIRBlockList::Item* bhead = blocks_.head();
  for (; bhead != NULL; bhead = bhead->next()) {
    LBlock* b = bhead->value()->lir();

    for (int i = 0; i < b->hir()->succ_count(); i++) {
      LGap* gap = NULL;
      LBlock* succ = b->hir()->SuccAt(i)->lir();

      // Create movements for non-matching parts of intervals
      LUseMap::Item* mitem = succ->live_in.head();
      for (; mitem != NULL; mitem = mitem->next_scalar()) {
        LInterval* parent = mitem->value()->interval();
        if (parent->split_parent()) parent = parent->split_parent();

        // Skip intervals that wasn't split
        if (parent->split_children()->length() == 0) continue;

        LInterval* left = parent->ChildAt(b->end_id);
        LInterval* right = parent->ChildAt(succ->start_id);

        if (left != right) {
          // Lazily allocate gap
          if (gap == NULL) {
            if (b->hir()->succ_count() == 2) {
              // Gap should be inserted in branch
              gap = GetGap(succ->start_id + 1);
            } else {
              // Or before join
              gap = GetGap(b->end_id - 1);
            }
          }

          gap->Add(left->Use(LUse::kAny, gap), right->Use(LUse::kAny, gap));
        }
      }

      // Remove goto instructions on adjacent blocks
      LInstruction* control = b->instructions()->tail()->value();
      assert(control->type() == LInstruction::kGoto ||
             control->type() == LInstruction::kBranch ||
             control->type() == LInstruction::kBranchNumber);

      if (control->type() == LInstruction::kGoto &&
          bhead->next()->value()->lir() == succ) {
        b->instructions()->Pop();
      } else {
        // Assign labels to other movement instructions
        LLabel* label = LLabel::Cast(succ->instructions()->head()->value());
        LControlInstruction::Cast(control)->AddTarget(label);
      }
    }
  }
}


void LGen::AllocateSpills() {
  // Sort by starting position
  unhandled_spills_.Sort();

  while (unhandled_spills_.length() > 0) {
    LInterval* current = unhandled_spills_.Shift();
    int pos = current->start();

    ShuffleIntervals(&active_spills_, &inactive_spills_, &free_spills_, pos);

    // Assign free spill
    if (free_spills_.length() > 0) {
      LInterval* f = NULL;
      do {
        f = free_spills_.Pop();

        // Check that this spill is really free
        for (int i = 0; f != NULL && i < active_spills_.length(); i++) {
          if (active_spills_.At(i)->IsEqual(f)) f = NULL;
        }

        for (int i = 0; f != NULL && i < inactive_spills_.length(); i++) {
          LInterval* inactive = inactive_spills_.At(i);
          if (inactive->IsEqual(f) &&
              inactive->FindIntersection(current) != -1) {
            f = NULL;
          }
        }
      } while (f == NULL && free_spills_.length() > 0);

      if (f != NULL) {
        current->Spill(f->index());
        active_spills_.Push(current);
        continue;
      }
    }

    ZoneMap<NumberKey, LInterval, ZoneObject> blocked;
    int max_index = 0;

    for (int i = 0; i < active_spills_.length(); i++) {
      LInterval* active = active_spills_.At(i);
      blocked.Set(NumberKey::New(active->index()), active);
      if (active->index() > max_index) max_index = active->index();
    }

    for (int i = 0; i < inactive_spills_.length(); i++) {
      LInterval* inactive = inactive_spills_.At(i);
      if (inactive->FindIntersection(current) != -1) {
        blocked.Set(NumberKey::New(inactive->index()), inactive);
        if (inactive->index() > max_index) max_index = inactive->index();
      }
    }

    // Reuse spill if it's unused now
    for (int i = 0; i < max_index; i++) {
      if (blocked.Get(NumberKey::New(i)) == NULL) {
        current->Spill(i);
        active_spills_.Push(current);
        break;
      }
    }

    // If succeed - move to next spill
    if (current->index() != -1) continue;

    // Allocate new spill
    current->Spill(spill_index_++);
    active_spills_.Push(current);
  }
}


void LGen::Generate(Masm* masm, SourceMap* map) {
  // +1 for argc
  masm->stack_slots(spill_index_ + 1);

  // Generate all instructions
  HIRBlockList::Item* bhead = blocks_.head();
  for (; bhead != NULL; bhead = bhead->next()) {
    LBlock* l = bhead->value()->lir();

    LInstructionList::Item* lhead = l->instructions()->head();
    for (; lhead != NULL; lhead = lhead->next()) {
      LInstruction* instr = lhead->value();

      if (instr->hir() != NULL && instr->hir()->ast() != NULL &&
          instr->hir()->ast()->offset() >= 0) {
        map->Push(masm->offset(), instr->hir()->ast()->offset());
      }
      instr->Generate(masm);
    }
  }

  masm->FinalizeSpills();
  masm->AlignCode();
}


void LGen::Print(PrintBuffer* p, bool extended) {
  // Only for debugging purposes
  if (extended) PrintIntervals(p);

  HIRBlockList::Item* bhead = blocks_.head();
  for (; bhead != NULL; bhead = bhead->next()) {
    HIRBlock* b = bhead->value();
    b->lir()->PrintHeader(p);

    LInstructionList::Item* ihead = b->lir()->instructions()->head();
    for (; ihead != NULL; ihead = ihead->next()) {
      ihead->value()->Print(p);
    }

    p->Print("\n");
  }
}


void LGen::PrintIntervals(PrintBuffer* p) {
  for (int i = 0; i < intervals_.length(); i++) {
    LInterval* interval = intervals_.At(i);
    if (interval->id < kLIRRegisterCount) {
      p->Print("%s     : ", RegisterNameByIndex(interval->id));
    } else if (interval->is_stackslot()) {
      p->Print("%03d [%02d]: ", interval->id, interval->index());
    } else if (interval->is_const()) {
      p->Print("%03d c   : ", interval->id);
    } else {
      p->Print("%03d     : ", interval->id);
    }

    for (int i = 0; i < instr_id_; i++) {
      LUse* use = interval->UseAt(i);
      if (use == NULL) {
        if (interval->Covers(i)) {
          p->Print("_");
        } else {
          p->Print(".");
        }
      } else if (use->instr()->result == use) {
        switch (use->type()) {
         case LUse::kRegister: p->Print("R"); break;
         case LUse::kAny: p->Print("A"); break;
         default: UNEXPECTED
        }
      } else {
        switch (use->type()) {
         case LUse::kRegister: p->Print("r"); break;
         case LUse::kAny: p->Print("a"); break;
         default: UNEXPECTED
        }
      }

      // Make block boundaries visible
      if (IsBlockStart(i + 1) != NULL) p->Print("|");
    }

    if (interval->split_parent() != NULL) {
      p->Print(" P:%d", interval->split_parent()->id);
    }

    p->Print("\n");
  }

  p->Print("\n");
}


LInterval* LGen::CreateInterval(LInterval::Type type, int index) {
  LInterval* res = new LInterval(type, index);
  res->id = interval_id();
  intervals_.Push(res);
  return res;
}


LInterval* LGen::ToFixed(HIRInstruction* instr, Register reg) {
  LInterval* res = registers_[IndexByRegister(reg)];

  LInstruction* move = Add(new LMove())
      ->SetResult(res, LUse::kRegister)
      ->AddArg(instr, LUse::kAny);
  assert(instr->lir()->result != NULL);

  instr->lir()->result->interval()->register_hint = move->result;

  return res;
}


void LGen::ResultFromFixed(LInstruction* instr, Register reg) {
  LInterval* ireg = registers_[IndexByRegister(reg)];
  LInterval* res = CreateVirtual();

  LInstruction* move = Add(new LMove())
      ->SetResult(res, LUse::kAny)
      ->AddArg(ireg, LUse::kRegister);
  res->register_hint = move->inputs[0];

  instr->SetResult(ireg, LUse::kRegister);
  instr->Propagate(res->uses()->head());
}


LInterval* LGen::Split(LInterval* i, int pos) {
  // TODO(indutny): Find optimal split position here
  assert(!i->IsFixed());

  assert(pos > i->start() && pos < i->end());
  LInterval* child = CreateVirtual();

  // Move uses from parent to child
  for (int j = i->uses()->length() - 1; j >= 0; j--) {
    LUse* use = i->uses()->At(j);

    // Uses are sorted - so break early
    if (use->instr()->id < pos) break;

    i->uses()->RemoveAt(j);
    child->uses()->Unshift(use);
    use->interval(child);
  }

  // Move ranges from parent to child
  for (int j = i->ranges()->length() - 1; j >= 0; j--) {
    LRange* range = i->ranges()->At(j);

    // Ranges are sorted too
    if (range->end() <= pos) break;

    i->ranges()->RemoveAt(j);
    if (range->start() < pos) {
      // Range needs to be splitted first
      i->ranges()->Push(new LRange(i, range->start(), pos));
      range->start(pos);
    }
    child->ranges()->Unshift(range);
    range->interval(child);
  }

  LInterval* parent = i->split_parent() == NULL ? i : i->split_parent();
  child->split_parent(parent);
  parent->split_children()->Unshift(child);

  unhandled_.InsertSorted(child);

  assert(i->end() <= pos);
  assert(child->start() >= pos);

  // If parent ends on block's edge - move will be inserted when resolving
  // data flow
  if (IsBlockStart(i->end())) return child;

  // Insert move right before split position, because
  // left side is definitely live here and right side haven't been used yet
  LGap* gap = GetGap(pos);
  gap->Add(i->Use(LUse::kAny, gap), child->Use(LUse::kAny, gap));

  return child;
}


LGap* LGen::GetGap(int pos) {
  HIRBlockList::Item* bhead = blocks_.head();
  LInstructionList::Item* lhead = NULL;
  LBlock* l = NULL;
  for (; bhead != NULL; bhead = bhead->next()) {
    l = bhead->value()->lir();

    // Skip blocks that definitely can't contain gap
    if (l->end_id <= pos) continue;

    // Search for gap within block
    lhead = l->instructions()->head();
    for (; lhead != NULL; lhead = lhead->next()) {
      LInstruction* instr = lhead->value();
      if (instr->id < pos) continue;

      // Return existing gap
      if (instr->id == pos) return LGap::Cast(instr);

      break;
    }

    if (lhead != NULL) break;
  }
  assert(lhead != NULL && lhead->prev() != NULL && l != NULL);

  // Create temporary spill for gap
  LInterval* tmp = CreateVirtual();
  tmp->AddRange(pos - 1, pos + 1);
  Spill(tmp);

  // Create new gap
  LGap* gap = new LGap(tmp);
  gap->id = pos;
  gap->block(l);
  l->instructions()->InsertBefore(lhead, gap);

  return gap;
}


void LGen::Spill(LInterval* interval) {
  assert(!interval->is_stackslot());

  interval->Spill(-1);
  unhandled_spills_.Push(interval);
}


LUse* LInterval::Use(LUse::Type type, LInstruction* instr) {
  LUse* use = new LUse(this, type, instr);

  uses_.InsertSorted(use);

  return use;
}


void LInterval::AddRange(int start, int end) {
  // Check if current range can be extended
  if (ranges_.length() > 0) {
    LRange* head = ranges_.head();
    if (head->start() == end) {
      head->start(start);
      return;
    }

    // Create new range and append it to the list
    assert(end < head->start());
  }

  LRange* range = new LRange(this, start, end);

  ranges_.Unshift(range);
}


bool LInterval::Covers(int pos) {
  for (int i = 0; i < ranges_.length(); i++) {
    LRange* range = ranges_.At(i);
    if (range->start() > pos) return false;
    if (range->end() > pos) return true;
  }

  return false;
}


LUse* LInterval::UseAt(int pos) {
  for (int i = 0; i < uses_.length(); i++) {
    LUse* use = uses_.At(i);
    if (use->instr()->id == pos) return use;
  }

  return NULL;
}


LUse* LInterval::UseAfter(int pos, LUse::Type use_type) {
  for (int i = 0; i < uses_.length(); i++) {
    LUse* use = uses_.At(i);
    if (use->instr()->id >= pos &&
        (use_type == LUse::kAny || use->type() == use_type)) {
      return use;
    }
  }

  return NULL;
}


int LInterval::FindIntersection(LInterval* with) {
  for (int i = 0; i < ranges()->length(); i++) {
    for (int j = 0; j < with->ranges()->length(); j++) {
      int r = ranges()->At(i)->FindIntersection(with->ranges()->At(j));
      if (r != -1) return r;
    }
  }
  return -1;
}


LInterval* LInterval::ChildAt(int pos) {
  if (split_parent() != NULL) return split_parent()->ChildAt(pos);
  if (Covers(pos)) return this;

  for (int i = 0; i < split_children_.length(); i++) {
    LInterval* child = split_children_.At(i);
    if (child->Covers(pos)) return child;
  }

  UNEXPECTED
}


int LRange::FindIntersection(LRange* with) {
  // First intersection is either our start or `with`'s start
  if (start() >= with->start() && start() < with->end()) {
    return start();
  } else if (with->start() >= start() && with->start() < end()) {
    return with->start();
  } else {
    return -1;
  }
}


int LInterval::Compare(LInterval* a, LInterval* b) {
  return a->start() > b->start() ? 1 : a->start() < b->start() ? -1 : 0;
}


int LRange::Compare(LRange* a, LRange* b) {
  return a->start() > b->start() ? 1 : a->start() < b->start() ? -1 : 0;
}


int LUse::Compare(LUse* a, LUse* b) {
  return a->instr()->id > b->instr()->id ? 1 :
         a->instr()->id < b->instr()->id ? -1 : 0;
}


LBlock::LBlock(HIRBlock* hir) : start_id(-1),
                                end_id(-1),
                                hir_(hir),
                                label_(new LLabel()) {
  hir->lir(this);
}

}  // namespace internal
}  // namespace candor
