#include "hir.h"
#include "hir-instructions.h"
#include "visitor.h" // Visitor
#include "ast.h" // AstNode
#include "macroassembler.h" // Masm, RelocationInfo
#include "utils.h" // List

#include <stdlib.h> // NULL
#include <stdint.h> // int64_t
#include <assert.h> // assert

#define __STDC_FORMAT_MACROS
#include <inttypes.h> // printf formats for big integers

namespace candor {
namespace internal {

HIRBasicBlock::HIRBasicBlock(HIR* hir) : hir_(hir),
                                         enumerated_(0),
                                         predecessors_count_(0),
                                         successors_count_(0),
                                         masm_(NULL),
                                         relocation_offset_(0),
                                         relocated_(false),
                                         finished_(false),
                                         id_(hir->get_block_index()) {
  predecessors_[0] = NULL;
  predecessors_[1] = NULL;
  successors_[0] = NULL;
  successors_[1] = NULL;
}


void HIRBasicBlock::AddValue(HIRValue* value) {
  // If value is already in values list - remove previous
  HIRValueList::Item* item = values()->head();
  for (; item != NULL; item = item->next()) {
    if (item->value()->slot() == value->slot()) {
      values()->Remove(item);
    }
  }
  values()->Push(value);
}


void HIRBasicBlock::AddPredecessor(HIRBasicBlock* block) {
  assert(predecessors_count() < 2);
  predecessors()[predecessors_count_++] = block;

  HIRValueList::Item* item;
  if (predecessors_count_ > 1) {
    // Mark values propagated from first predecessor
    item = values()->head();
    for (; item != NULL; item = item->next()) {
      if (item->value() == NULL) continue;

      item->value()->current_block(this);
      item->value()->slot()->hir(item->value());
    }
  }

  // Propagate used values from predecessor to current block
  item = block->values()->head();
  for (; item != NULL; item = item->next()) {
    HIRValue* value = item->value();

    if (value == NULL) continue;

    // If value is already in current block - insert phi!
    if (value->slot()->hir()->current_block() == this &&
        value->slot()->hir() != value) {
      HIRPhi* phi = HIRPhi::Cast(value->slot()->hir());
      if (!phi->is_phi()) {
        phi = new HIRPhi(this, value->slot()->hir());

        ReplaceVarUse(value->slot()->hir(), phi);
        value->slot()->hir(phi);
        AddValue(phi);

        // Push to block's and global phi list
        phis()->Push(phi);
        hir()->phis()->Push(phi);
        hir()->values()->Push(phi);
      }

      phi->inputs()->Push(value);

      // Replace all var uses if Phi appeared in loop
      ReplaceVarUse(value, phi);
    } else {
      // And associated with a current block
      value->current_block(this);
      value->slot()->hir(value);

      // Otherwise put value to the list
      AddValue(value);
    }
  }
}


void HIRBasicBlock::AddSuccessor(HIRBasicBlock* block) {
  assert(successors_count() < 2);
  successors()[successors_count_++] = block;
  block->AddPredecessor(this);
}


void HIRBasicBlock::Goto(HIRBasicBlock* block) {
  if (finished()) return;

  // Connect graph nodes
  AddSuccessor(block);

  // Add goto instruction and finalize block
  HIRGoto* instr = new HIRGoto();
  instructions()->Push(instr);
  instr->Init(this);
  finished(true);
}


void HIRBasicBlock::ReplaceVarUse(HIRValue* source, HIRValue* target) {
  if (instructions()->length() == 0) return;

  ZoneList<HIRBasicBlock*> work_list;
  ZoneList<HIRBasicBlock*> cleanup_list;

  work_list.Push(this);

  HIRBasicBlock* block;
  while ((block = work_list.Shift()) != NULL) {
    cleanup_list.Push(block);

    // Replace value use in each instruction
    ZoneList<HIRInstruction*>::Item* item = block->instructions()->head();
    for (; item != NULL; item = item->next()) {
      item->value()->ReplaceVarUse(source, target);
    }

    // Add block's successors to the work list
    for (int i = block->successors_count() - 1; i >= 0; i--) {
      // Skip processed blocks and join blocks that was visited only once
      block->successors()[i]->enumerate();
      if (block->successors()[i]->is_enumerated()) {
        work_list.Unshift(block->successors()[i]);
      }
    }
  }

  while ((block = cleanup_list.Shift()) != NULL) {
    block->reset_enumerate();
  }
}


bool HIRBasicBlock::Dominates(HIRBasicBlock* block) {
  while (block != NULL) {
    if (block == this) return true;
    block = block->predecessors()[0];
  }

  return false;
}


void HIRBasicBlock::AddUse(RelocationInfo* info) {
  if (relocated()) {
    info->target(relocation_offset_);
    masm_->relocation_info_.Push(info);
    return;
  }
  uses()->Push(info);
}


void HIRBasicBlock::Relocate(Masm* masm) {
  if (relocated()) return;
  masm_ = masm;
  relocation_offset_ = masm->offset();
  relocated(true);

  RelocationInfo* block_reloc;
  while ((block_reloc = uses()->Shift()) != NULL) {
    block_reloc->target(masm->offset());
    masm->relocation_info_.Push(block_reloc);
  }
}


bool HIRBasicBlock::IsPrintable() {
  return hir()->print_map()->Get(NumberKey::New(id())) == NULL;
}


void HIRBasicBlock::MarkPrinted() {
  int value;
  hir()->print_map()->Set(NumberKey::New(id()), &value);
}


void HIRBasicBlock::Print(PrintBuffer* p) {
  // Avoid loops and double prints
  MarkPrinted();

  p->Print("[Block#%d ", id());

  // Print values
  {
    HIRValueList::Item* item = values()->head();
    p->Print("{");
    while (item != NULL) {
      p->Print("%d", item->value()->id());
      item = item->next();
      if (item != NULL) p->Print(",");
    }
    p->Print("} ");
  }

  // Print phis
  {
    HIRPhiList::Item* item = phis()->head();
    while (item != NULL) {
      item->value()->Print(p);
      item = item->next();
      p->Print(" ");
    }
  }

  // Print instructions
  {
    HIRInstructionList::Item* item = instructions()->head();
    while (item != NULL) {
      item->value()->Print(p);
      item = item->next();
      p->Print(" ");
    }
  }

  // Print predecessors' ids
  if (predecessors_count() == 2) {
    p->Print("[%d,%d]", predecessors()[0]->id(), predecessors()[1]->id());
  } else if (predecessors_count() == 1) {
    p->Print("[%d]", predecessors()[0]->id());
  } else {
    p->Print("[]");
  }

  p->Print(">*>");

  // Print successors' ids
  if (successors_count() == 2) {
    p->Print("[%d,%d]", successors()[0]->id(), successors()[1]->id());
  } else if (successors_count() == 1) {
    p->Print("[%d]", successors()[0]->id());
  } else {
    p->Print("[]");
  }

  p->Print("]\n");

  // Print successors
  if (successors_count() == 2) {
    if (successors()[0]->IsPrintable()) successors()[0]->Print(p);
    if (successors()[1]->IsPrintable()) successors()[1]->Print(p);
  } else if (successors_count() == 1) {
    if (successors()[0]->IsPrintable()) successors()[0]->Print(p);
  }
}


HIRPhi::HIRPhi(HIRBasicBlock* block, HIRValue* value)
    : HIRValue(block, value->slot()) {
  type_ = kPhi;
  inputs()->Push(value);
}


void HIRPhi::Print(PrintBuffer* p) {
  p->Print("@[");
  HIRValueList::Item* item = inputs()->head();
  while (item != NULL) {
    p->Print("%d", item->value()->id());
    item = item->next();
    if (item != NULL) p->Print(",");
  }
  p->Print("]:%d", id());
}


HIRValue::HIRValue(HIRBasicBlock* block) : type_(kNormal),
                                           block_(block),
                                           current_block_(block),
                                           prev_def_(NULL),
                                           operand_(NULL) {
  slot_ = new ScopeSlot(ScopeSlot::kStack);
  Init();
}


HIRValue::HIRValue(HIRBasicBlock* block, ScopeSlot* slot)
    : type_(kNormal),
      block_(block),
      current_block_(block),
      prev_def_(NULL),
      operand_(NULL),
      slot_(slot) {
  Init();
}


void HIRValue::Init() {
  block()->AddValue(this);
  id_ = block()->hir()->get_variable_index();

  live_range()->start = -1;
  live_range()->end = -1;
}


void HIRValue::Print(PrintBuffer* p) {
  if (prev_def() == NULL) {
    p->Print("*[%d ", id());
  } else {
    p->Print("*[%d>%d ", prev_def()->id(), id());
  }
    slot()->Print(p);
    p->Print("]");
}


HIR::HIR(Heap* heap, AstNode* node) : Visitor(kPreorder),
                                      root_(heap),
                                      first_instruction_(NULL),
                                      last_instruction_(NULL),
                                      block_index_(0),
                                      variable_index_(0),
                                      instruction_index_(0),
                                      print_map_(NULL) {
  work_list_.Push(new HIRFunction(node, CreateBlock()));

  HIRFunction* fn;
  while ((fn = work_list_.Shift()) != NULL) {
    root_block_ = fn->block();
    roots()->Push(fn->block());
    set_current_block(fn->block());
    Visit(fn->node());
  }

  EnumInstructions();
}


HIRValue* HIR::FindPredecessorValue(ScopeSlot* slot) {
  assert(current_block() != NULL);

  // Find appropriate value
  HIRValue* previous = slot->hir();
  while (previous != NULL) {
    // Traverse blocks to the root, to check
    // if variable was used in predecessor
    HIRBasicBlock* block = current_block();
    while (block != NULL && previous->block() != block) {
      block = block->predecessors()[0];
    }
    if (block != NULL) break;
    previous = previous->prev_def();
  }

  return previous;
}


HIRValue* HIR::CreateValue(HIRBasicBlock* block, ScopeSlot* slot) {
  HIRValue* value = new HIRValue(block, slot);
  HIRValue* previous = FindPredecessorValue(slot);

  // Link with previous
  if (previous != NULL) {
    value->prev_def(previous);
    previous->next_defs()->Push(value);
  }

  slot->hir(value);

  // Push value to the values list
  values()->Push(value);

  return value;
}


HIRValue* HIR::CreateValue(HIRBasicBlock* block) {
  return CreateValue(block, new ScopeSlot(ScopeSlot::kStack));
}


HIRValue* HIR::CreateValue(ScopeSlot* slot) {
  return CreateValue(current_block(), slot);
}


HIRValue* HIR::GetValue(ScopeSlot* slot) {
  assert(current_block() != NULL);

  // Slot was used - find one in our branch
  HIRValue* previous = FindPredecessorValue(slot);

  // Lazily create new variable
  if (previous == NULL) {
    // Slot wasn't used in HIR yet
    // Insert new one HIRValue in the current block
    CreateValue(slot);
  } else {
    if (previous != slot->hir()) {
      // Create slot and link variables
      HIRValue* value = new HIRValue(current_block(), slot);

      // Link with previous
      if (slot->hir() != NULL) {
        value->prev_def(previous);
        previous->next_defs()->Push(value);
      }

      slot->hir(value);

      // Push value to the values list
      values()->Push(value);
    } else {
      slot->hir()->current_block(current_block());
    }
  }

  return slot->hir();
}


HIRBasicBlock* HIR::CreateBlock() {
  return new HIRBasicBlock(this);
}


HIRBasicBlock* HIR::CreateJoin(HIRBasicBlock* left, HIRBasicBlock* right) {
  HIRBasicBlock* join = CreateBlock();

  left->Goto(join);
  right->Goto(join);

  return join;
}


void HIR::AddInstruction(HIRInstruction* instr) {
  assert(current_block() != NULL);
  instr->Init(current_block());

  if (current_block()->finished()) return;

  current_block()->instructions()->Push(instr);
}


void HIR::Finish(HIRInstruction* instr) {
  assert(current_block() != NULL);
  AddInstruction(instr);
  current_block()->finished(true);
}


void HIR::EnumInstructions() {
  ZoneList<HIRBasicBlock*>::Item* root = roots()->head();
  ZoneList<HIRBasicBlock*> work_list;

  // Add roots to worklist
  for (; root != NULL; root = root->next()) {
    root->value()->enumerate();
    work_list.Push(root->value());
  }

  // Add first instruction
  {
    HIRParallelMove* move = new HIRParallelMove();
    move->Init(NULL);
    move->id(get_instruction_index());
    first_instruction(move);
    last_instruction(move);
  }

  // Process worklist
  HIRBasicBlock* current;
  while ((current = work_list.Shift()) != NULL) {
    // Insert nop instruction in empty blocks
    if (current->instructions()->length() == 0) {
      set_current_block(current);
      AddInstruction(new HIRNop());
    }

    // Go through all block's instructions, link them together and assign id
    HIRInstructionList::Item* instr = current->instructions()->head();
    for (; instr != NULL; instr = instr->next()) {
      // Insert instruction into linked list
      last_instruction()->next(instr->value());

      instr->value()->prev(last_instruction());
      instr->value()->id(get_instruction_index());
      last_instruction(instr->value());

      // Insert parallel move after each instruction
      HIRParallelMove* move = new HIRParallelMove();
      move->Init(instr->value()->block());

      last_instruction()->next(move);

      move->prev(last_instruction());
      move->id(get_instruction_index());
      last_instruction(move);
    }

    // Add block's successors to the work list
    for (int i = current->successors_count() - 1; i >= 0; i--) {
      // Skip processed blocks and join blocks that was visited only once
      current->successors()[i]->enumerate();
      if (current->successors()[i]->is_enumerated()) {
        work_list.Unshift(current->successors()[i]);
      }
    }
  }
}


HIRValue* HIR::GetValue(AstNode* node) {
  Visit(node);

  if (current_block() == NULL ||
      current_block()->instructions()->length() == 0) {
    return CreateValue(root()->Put(new AstNode(AstNode::kNil)));
  } else {
    return GetLastResult();
  }
}


HIRValue* HIR::GetLastResult() {
  return current_block()->instructions()->tail()->value()->GetResult();
}


void HIR::Print(char* buffer, uint32_t size) {
  PrintMap map;

  PrintBuffer p(buffer, size);
  print_map(&map);

  ZoneList<HIRBasicBlock*>::Item* item = roots()->head();
  for (; item != NULL; item = item->next()) {
    item->value()->Print(&p);
  }

  print_map(NULL);
  p.Finalize();
}


AstNode* HIR::VisitFunction(AstNode* stmt) {
  FunctionLiteral* fn = FunctionLiteral::Cast(stmt);

  if (current_block() == root_block() &&
      current_block()->instructions()->length() == 0) {
    HIREntry* entry = new HIREntry(fn->context_slots());

    AddInstruction(entry);

    // Add agruments
    AstList::Item* arg = fn->args()->head();
    for (; arg != NULL; arg = arg->next()) {
      AstValue* value = AstValue::Cast(arg->value());
      HIRValue* hir_value;
      if (value->slot()->is_context()) {
        // Create temporary on-stack variable
        hir_value = CreateValue(current_block());

        // And move it into context
        AddInstruction(new HIRStoreContext(GetValue(value->slot()),
                                           hir_value));
      } else {
        hir_value = GetValue(value->slot());
      }
      entry->AddArg(hir_value);
    }

    VisitChildren(stmt);

    AddInstruction(new HIRReturn(CreateValue(
            root()->Put(new AstNode(AstNode::kNil)))));
  } else {
    HIRBasicBlock* block = CreateBlock();
    AddInstruction(new HIRAllocateFunction(block, fn->args()->length()));

    work_list_.Push(new HIRFunction(stmt, block));
  }

  return stmt;
}


AstNode* HIR::VisitAssign(AstNode* stmt) {
  if (stmt->lhs()->is(AstNode::kValue)) {
    HIRValue* rhs = GetValue(stmt->rhs());

    AstValue* value = AstValue::Cast(stmt->lhs());
    HIRValue* lhs = CreateValue(value->slot());

    if (value->slot()->is_stack()) {
      AddInstruction(new HIRStoreLocal(lhs, rhs));
    } else {
      AddInstruction(new HIRStoreContext(lhs, rhs));
    }
  } else if (stmt->lhs()->is(AstNode::kMember)) {
    HIRValue* rhs = GetValue(stmt->rhs());
    HIRValue* property = GetValue(stmt->lhs()->rhs());
    HIRValue* receiver = GetValue(stmt->lhs()->lhs());

    AddInstruction(new HIRStoreProperty(receiver, property, rhs));
  } else {
    // TODO: Set error! Incorrect lhs
    abort();
  }

  return stmt;
}


AstNode* HIR::VisitValue(AstNode* node) {
  AstValue* value = AstValue::Cast(node);
  if (value->slot()->is_stack()) {
    AddInstruction(new HIRLoadLocal(GetValue(value->slot())));
  } else {
    AddInstruction(new HIRLoadContext(GetValue(value->slot())));
  }
  return node;
}


void HIR::VisitRootValue(AstNode* node) {
  AddInstruction(new HIRLoadRoot(CreateValue(root()->Put(node))));
}


AstNode* HIR::VisitIf(AstNode* node) {
  HIRBasicBlock* on_true = CreateBlock();
  HIRBasicBlock* on_false = CreateBlock();
  HIRBasicBlock* join = NULL;
  HIRBranchBool* branch = new HIRBranchBool(GetValue(node->lhs()),
                                            on_true,
                                            on_false);
  Finish(branch);

  set_current_block(on_true);
  Visit(node->rhs());
  on_true = current_block();

  AstList::Item* else_body = node->children()->head()->next()->next();
  set_current_block(on_false);

  if (else_body != NULL) {
    // Visit else body and create additional `join` block
    Visit(else_body->value());
  }

  on_false = current_block();

  set_current_block(CreateJoin(on_true, on_false));

  return node;
}


AstNode* HIR::VisitWhile(AstNode* node) {
  HIRBasicBlock* cond = CreateBlock();
  HIRBasicBlock* start = CreateBlock();
  HIRBasicBlock* body = CreateBlock();
  HIRBasicBlock* end = CreateBlock();

  //   entry
  //     |
  //    lhs
  //     | \
  //     |  ^
  //     |  |
  //     | body
  //     |  |
  //     |  ^
  //     | /
  //   branch
  //     |
  //     |
  //    end

  // Create new block and insert condition expression into it
  current_block()->Goto(cond);
  set_current_block(cond);
  HIRBranchBool* branch = new HIRBranchBool(GetValue(node->lhs()),
                                            body,
                                            end);
  cond->Goto(start);
  set_current_block(start);
  Finish(branch);

  // Generate loop's body
  set_current_block(body);
  Visit(node->rhs());

  // And loop it back to condition
  body->Goto(cond);

  // Execution will continue in the `end` block
  set_current_block(end);

  return node;
}


AstNode* HIR::VisitMember(AstNode* node) {
  HIRValue* property = GetValue(node->rhs());
  HIRValue* receiver = GetValue(node->lhs());

  AddInstruction(new HIRLoadProperty(receiver, property));

  return node;
}


AstNode* HIR::VisitCall(AstNode* stmt) {
  FunctionLiteral* fn = FunctionLiteral::Cast(stmt);

  Visit(fn->variable());
  HIRCall* call = new HIRCall(GetValue(fn->variable()));

  AstList::Item* item = fn->args()->head();
  for (; item != NULL; item = item->next()) {
    call->AddArg(GetValue(item->value()));
  }

  AddInstruction(call);

  return stmt;
}


void HIR::VisitGenericObject(AstNode* node) {
  int size = node->children()->length();
  HIRAllocateObject::ObjectKind kind;
  switch (node->type()) {
   case AstNode::kObjectLiteral:
    kind = HIRAllocateObject::kObject;
    break;
   case AstNode::kArrayLiteral:
    kind = HIRAllocateObject::kArray;
    break;
   default: UNEXPECTED break;
  }

  // Create object
  HIRAllocateObject* instr = new HIRAllocateObject(kind, size);
  AddInstruction(instr);

  // Get result
  HIRValue* result = instr->GetResult();

  // Insert properties
  switch (node->type()) {
   case AstNode::kObjectLiteral:
    {
      ObjectLiteral* obj = ObjectLiteral::Cast(node);

      assert(obj->keys()->length() == obj->values()->length());
      AstList::Item* key = obj->keys()->head();
      AstList::Item* value = obj->values()->head();
      while (key != NULL) {
        AddInstruction(new HIRStoreProperty(
              result,
              GetValue(key->value()),
              GetValue(value->value())));

        key = key->next();
        value = value->next();
      }
    }
    break;
   case AstNode::kArrayLiteral:
    {
      AstList::Item* item = node->children()->head();
      uint64_t index = 0;
      while (item != NULL) {
        char keystr[32];
        AstNode* key = new AstNode(AstNode::kNumber, node);
        key->value(keystr);
        key->length(snprintf(keystr, sizeof(keystr), "%" PRIu64, index));

        AddInstruction(new HIRStoreProperty(
              result,
              CreateValue(root()->Put(key)),
              GetValue(item->value())));

        item = item->next();
        index++;
      }
    }
    break;
   default: UNEXPECTED break;
  }

  // And return object
  AddInstruction(new HIRNop(result));
}


AstNode* HIR::VisitReturn(AstNode* node) {
  HIRValue* result = NULL;
  if (node->lhs() != NULL) result = GetValue(node->lhs());

  if (result == NULL) {
    result = CreateValue(root()->Put(new AstNode(AstNode::kNil)));
  }
  Finish(new HIRReturn(result));

  return node;
}


AstNode* HIR::VisitClone(AstNode* node) {
  return node;
}


AstNode* HIR::VisitDelete(AstNode* node) {
  return node;
}


AstNode* HIR::VisitBreak(AstNode* node) {
  return node;
}


AstNode* HIR::VisitContinue(AstNode* node) {
  return node;
}


AstNode* HIR::VisitTypeof(AstNode* node) {
  AddInstruction(new HIRTypeof(GetValue(node->lhs())));
  return node;
}


AstNode* HIR::VisitSizeof(AstNode* node) {
  AddInstruction(new HIRSizeof(GetValue(node->lhs())));
  return node;
}


AstNode* HIR::VisitKeysof(AstNode* node) {
  AddInstruction(new HIRKeysof(GetValue(node->lhs())));
  return node;
}


AstNode* HIR::VisitUnOp(AstNode* node) {
  UnOp* op = UnOp::Cast(node);

  // Changing ops should be translated into another form
  if (op->is_changing()) {
    AstNode* one = new AstNode(AstNode::kNumber, node);
    one->value("1");
    one->length(1);

    if (op->is_postfix()) {
      // a++ => t = a, a = t + 1, t

      AstNode* tmp = new AstValue(new ScopeSlot(ScopeSlot::kStack),
                                  AstValue::Cast(op->lhs())->name());
      AstNode* init = new AstNode(AstNode::kAssign);
      init->children()->Push(tmp);
      init->children()->Push(op->lhs());

      HIRValue* result = GetValue(init);

      AstNode* change = new AstNode(AstNode::kAssign);
      change->children()->Push(op->lhs());
      switch (op->subtype()) {
       case UnOp::kPostInc:
        change->children()->Push(new BinOp(BinOp::kAdd, tmp, one));
        break;
       case UnOp::kPostDec:
        change->children()->Push(new BinOp(BinOp::kSub, tmp, one));
        break;
       default:
        UNEXPECTED
        break;
      }
      Visit(change);

      // return result
      AddInstruction(new HIRNop(result));
    } else {
      // ++a => a = a + 1
      AstNode* rhs = NULL;
      switch (op->subtype()) {
       case UnOp::kPreInc:
        rhs = new BinOp(BinOp::kAdd, op->lhs(), one);
        break;
       case UnOp::kPreDec:
        rhs = new BinOp(BinOp::kSub, op->lhs(), one);
        break;
       default:
        UNEXPECTED
        break;
      }

      AstNode* assign = new AstNode(AstNode::kAssign, node);
      assign->children()->Push(op->lhs());
      assign->children()->Push(rhs);

      Visit(assign);
    }
  } else if (op->subtype() == UnOp::kPlus || op->subtype() == UnOp::kMinus) {
    // +a = 0 + a
    // -a = 0 - a
    // TODO: Parser should genereate negative numbers where possible

    AstNode* zero = new AstNode(AstNode::kNumber, node);
    zero->value("0");
    zero->length(1);

    AstNode* wrap = new BinOp(
        op->subtype() == UnOp::kPlus ? BinOp::kAdd : BinOp::kSub,
        zero,
        op->lhs());

    Visit(wrap);

  } else if (op->subtype() == UnOp::kNot) {
    AddInstruction(new HIRNot(GetValue(op->lhs())));
  } else {
    UNEXPECTED
  }

  return node;
}


AstNode* HIR::VisitBinOp(AstNode* node) {
  BinOp* op = BinOp::Cast(node);

  AddInstruction(new HIRBinOp(
        op->subtype(),
        GetValue(node->lhs()),
        GetValue(node->rhs())));

  return node;
}

} // namespace internal
} // namespace candor
