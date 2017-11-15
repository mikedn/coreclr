// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#pragma once

#include "jitstd.h"

struct SsaRenameState
{
    // A stack entry is used to store the current SSA number of a given local.
    // Each entry is chained to 2 singly linked lists via m_list and m_stack.
    struct Stack
    {
        // A list of all entries, in the order they have been pushed. This allows
        // for easy popping of all entries that belong to a block. This is also
        // used to maintain a free list of entries - when a block is popped all
        // its entries (that form a list too) are moved to the free list.
        Stack* m_list;
        // A per local stack of entries. The top entry contains the current SSA
        // number for local lclNum. Note that if there are multiple definitions
        // of the same local in a block then a new entry is pushed onto the stack
        // only for the first definition. For subsequent definitions ssaNum is
        // updated instead of pushing a new entry.
        Stack* m_stack;
        // The basic block number. Used only when popping blocks.
        unsigned m_bbNum;
        // The local number. Also used only when popping blocks.
        unsigned m_lclNum;
        // The actual information Stack stores - the SSA number.
        unsigned m_ssaNum;

        Stack(Stack* list, Stack* stack, unsigned bbNum, unsigned lclNum, unsigned ssaNum)
            : m_list(list), m_stack(stack), m_bbNum(bbNum), m_lclNum(lclNum), m_ssaNum(ssaNum)
        {
        }
    };

    typedef unsigned* Counts;

    SsaRenameState(CompAllocator* allocator, unsigned lvaCount, bool byrefStatesMatchGcHeapStates);

    void EnsureCounts();
    void EnsureStacks();

    // Requires "lclNum" to be a variable number for which a new count corresponding to a
    // definition is desired. The method post increments the counter for the "lclNum."
    unsigned CountForDef(unsigned lclNum);

    // Requires "lclNum" to be a variable number for which an ssa number at the top of the
    // stack is required i.e., for variable "uses."
    unsigned CountForUse(unsigned lclNum);

    // Allocates a new Stack (possibly by taking it from the free list)
    // using the specified constructor arguments.
    Stack* AllocBlockState(Stack* list, Stack* stack, unsigned bbNum, unsigned lclNum, unsigned ssaNum);

    // Returns the specified list of block states to the free list.
    void FreeBlockStateList(Stack* first, Stack* last);

    // Requires "lclNum" to be a variable number, and requires "ssaNum" to represent
    // an SSA number, that needs to be pushed on to the stack corresponding to the lclNum.
    void PushLclInit(unsigned lclNum, unsigned ssaNum);

    // Requires "lclNum" to be a variable number, and requires "ssaNum" to represent
    // an SSA number, that needs to be pushed on to the stack corresponding to the lclNum.
    void Push(BasicBlock* bb, unsigned lclNum, unsigned ssaNum);

    // Pop all stacks that have an entry for "bb" on top.
    void PopBlockStacks(BasicBlock* bb);

    // Similar functions for the special implicit memory variable.
    unsigned CountForMemoryDef()
    {
        if (memoryCount == 0)
        {
            memoryCount = SsaConfig::FIRST_SSA_NUM;
        }
        unsigned res = memoryCount;
        memoryCount++;
        return res;
    }
    unsigned CountForMemoryUse(MemoryKind memoryKind)
    {
        if ((memoryKind == GcHeap) && byrefStatesMatchGcHeapStates)
        {
            // Share rename stacks in this configuration.
            memoryKind = ByrefExposed;
        }
        return m_memoryStacks[memoryKind]->m_ssaNum;
    }

    void PushMemory(MemoryKind memoryKind, BasicBlock* block, unsigned ssaNum)
    {
        if ((memoryKind == GcHeap) && byrefStatesMatchGcHeapStates)
        {
            // Share rename stacks in this configuration.
            memoryKind = ByrefExposed;
        }
        m_memoryStacks[memoryKind] = new Stack(nullptr, m_memoryStacks[memoryKind], block->bbNum, BAD_VAR_NUM, ssaNum);
    }

    void PopBlockMemoryStack(MemoryKind memoryKind, BasicBlock* bb);

    unsigned MemoryCount()
    {
        return memoryCount;
    }

#ifdef DEBUG
    // Debug interface
    void DumpStacks();
#endif

private:
    // Map of lclNum -> count.
    Counts counts;

    // An array of state stacks, one for each possible lclNum.
    Stack** m_lclStacks;

    // A stack of all states.
    Stack* m_blockStack;

    // A stack of free states.
    Stack* m_freeStack;

    // Same state for the special implicit memory variables.
    Stack* m_memoryStacks[MemoryKindCount];
    unsigned memoryCount;

    // Number of stacks/counts to allocate.
    unsigned lvaCount;

    // Allocator to allocate stacks.
    CompAllocator* m_alloc;

    // Indicates whether GcHeap and ByrefExposed use the same state.
    bool byrefStatesMatchGcHeapStates;
};
