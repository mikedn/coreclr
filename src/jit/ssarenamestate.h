// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#pragma once

#include "jitstd.h"

class SsaRenameState
{
    struct BlockState
    {
        BlockState* m_list;
        BlockState* m_stack;
        unsigned    m_bbNum;
        unsigned    m_lclNum;
        unsigned    m_ssaNum;

        BlockState(BlockState* list, BlockState* stack, unsigned bbNum, unsigned lclNum, unsigned ssaNum)
            : m_list(list), m_stack(stack), m_bbNum(bbNum), m_lclNum(lclNum), m_ssaNum(ssaNum)
        {
        }
    };

public:
    SsaRenameState(CompAllocator* alloc, unsigned lvaCount, bool byrefStatesMatchGcHeapStates);

    // Requires "lclNum" to be a variable number for which a SSA number corresponding to a
    // new definition is desired. The method post increments the counter for the "lclNum."
    unsigned AllocSsaNum(unsigned lclNum);

    // Requires "lclNum" to be a variable number for which an SSA number at the top of the
    // stack is required i.e., for variable "uses."
    unsigned GetTopSsaNum(unsigned lclNum);

    void PushInit(unsigned lclNum, unsigned ssaNum);

    // Requires "lclNum" to be a variable number, and requires "ssaNum" to represent
    // an SSA number, that needs to be pushed on to the stack corresponding to the lclNum.
    void Push(BasicBlock* bb, unsigned lclNum, unsigned ssaNum);

    // Pop all stacks that have an entry for "bb" on top.
    void PopBlockStacks(BasicBlock* bb);

    // Similar functions for the special implicit memory variable.
    unsigned AllocMemorySsaNum()
    {
        if (m_memoryCount == 0)
        {
            m_memoryCount = SsaConfig::FIRST_SSA_NUM;
        }
        unsigned res = m_memoryCount;
        m_memoryCount++;
        return res;
    }
    unsigned GetTopMemorySsaNum(MemoryKind memoryKind)
    {
        if ((memoryKind == GcHeap) && byrefStatesMatchGcHeapStates)
        {
            // Share rename stacks in this configuration.
            memoryKind = ByrefExposed;
        }
        return m_memoryStack[memoryKind]->m_ssaNum;
    }

    void PushMemory(MemoryKind memoryKind, BasicBlock* bb, unsigned ssaNum)
    {
        if ((memoryKind == GcHeap) && byrefStatesMatchGcHeapStates)
        {
            // Share rename stacks in this configuration.
            memoryKind = ByrefExposed;
        }
        m_memoryStack[memoryKind] = new (m_alloc) BlockState(nullptr, m_memoryStack[memoryKind], bb->bbNum, 0, ssaNum);
    }

    void PopBlockMemoryStack(MemoryKind memoryKind, BasicBlock* bb);

    unsigned MemoryCount()
    {
        return m_memoryCount;
    }

private:
    BlockState* AllocBlockState(BlockState* list, BlockState* stack, unsigned bbNum, unsigned lclNum, unsigned ssaNum)
    {
        BlockState* obj = m_freeList;

        if (obj == nullptr)
        {
            obj = static_cast<BlockState*>(m_alloc->Alloc(sizeof(BlockState)));
        }
        else
        {
            m_freeList = obj->m_list;
        }

        return new (obj, jitstd::placement_t()) BlockState(list, stack, bbNum, lclNum, ssaNum);
    }

    void FreeBlockStateList(BlockState* first, BlockState* last)
    {
        last->m_list = m_freeList;
        m_freeList   = first;
    }

#ifdef DEBUG
    // Debug interface
    void DumpStacks();
#endif

    void EnsureCounts();
    void EnsureStacks();

    // Map of lclNum -> definition count.
    unsigned* m_lclDefCounts;

    // Map of lclNum -> BlockState* (a stack of block states).
    BlockState** m_stacks;
    BlockState*  m_blockStack;
    BlockState*  m_freeList;

    // Same state for the special implicit memory variables.
    BlockState* m_memoryStack[MemoryKindCount];
    unsigned    m_memoryCount;

    // Number of stacks/counts to allocate.
    unsigned lvaCount;

    // Allocator to allocate stacks.
    CompAllocator* m_alloc;

    // Indicates whether GcHeap and ByrefExposed use the same state.
    bool byrefStatesMatchGcHeapStates;
};
