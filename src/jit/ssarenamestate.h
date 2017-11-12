// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#pragma once

#include "jitstd.h"

// Fixed-size array that can hold elements with no default constructor;
// it will construct them all by forwarding whatever arguments are
// supplied to its constructor.
template <typename T, int N>
class ConstructedArray
{
    union {
        // Storage that gets used to hold the T objects.
        unsigned char bytes[N * sizeof(T)];

#if defined(_MSC_VER) && (_MSC_VER < 1900)
        // With MSVC pre-VS2015, the code in the #else branch would hit error C2621,
        // so in that case just count on pointer alignment being sufficient
        // (currently T is only ever instantiated as jitstd::list<SsaRenameStateForBlock>)

        // Unused (except to impart alignment requirement)
        void* pointer;
#else
        // Unused (except to impart alignment requirement)
        T alignedArray[N];
#endif // defined(_MSC_VER) && (_MSC_VER < 1900)
    };

public:
    T& operator[](size_t i)
    {
        return *(reinterpret_cast<T*>(bytes + i * sizeof(T)));
    }

    template <typename... Args>
    ConstructedArray(Args&&... args)
    {
        for (int i = 0; i < N; ++i)
        {
            new (bytes + i * sizeof(T), jitstd::placement_t()) T(jitstd::forward<Args>(args)...);
        }
    }

    ~ConstructedArray()
    {
        for (int i = 0; i < N; ++i)
        {
            operator[](i).~T();
        }
    }
};

class SsaRenameState
{
    struct BlockState
    {
        unsigned m_bbNum;
        unsigned m_count;

        BlockState(unsigned bbNum, unsigned count) : m_bbNum(bbNum), m_count(count)
        {
        }

        BlockState() : m_bbNum(0), m_count(0)
        {
        }
    };

    // A record indicating that local "m_lclNum" was defined in block "m_bbNum".
    struct LclDefState
    {
        unsigned m_bbNum;
        unsigned m_lclNum;

        LclDefState(unsigned bbNum, unsigned lclNum) : m_bbNum(bbNum), m_lclNum(lclNum)
        {
        }
    };

    typedef jitstd::list<BlockState>  Stack;
    typedef Stack**                   Stacks;
    typedef unsigned*                 Counts;
    typedef jitstd::list<LclDefState> DefStack;

public:
    SsaRenameState(const jitstd::allocator<int>& allocator, unsigned lvaCount, bool byrefStatesMatchGcHeapStates);

    // Requires "lclNum" to be a variable number for which a new count corresponding to a
    // definition is desired. The method post increments the counter for the "lclNum."
    unsigned CountForDef(unsigned lclNum);

    // Requires "lclNum" to be a variable number for which an ssa number at the top of the
    // stack is required i.e., for variable "uses."
    unsigned CountForUse(unsigned lclNum);

    // Requires "lclNum" to be a variable number, and requires "count" to represent
    // an ssa number, that needs to be pushed on to the stack corresponding to the lclNum.
    void Push(BasicBlock* bb, unsigned lclNum, unsigned count);

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
        return memoryStack[memoryKind].back().m_count;
    }

    void PushMemory(MemoryKind memoryKind, BasicBlock* bb, unsigned count)
    {
        if ((memoryKind == GcHeap) && byrefStatesMatchGcHeapStates)
        {
            // Share rename stacks in this configuration.
            memoryKind = ByrefExposed;
        }
        memoryStack[memoryKind].push_back(BlockState(bb->bbNum, count));
    }

    void PopBlockMemoryStack(MemoryKind memoryKind, BasicBlock* bb);

    unsigned MemoryCount()
    {
        return memoryCount;
    }

private:
#ifdef DEBUG
    // Debug interface
    void DumpStacks();
#endif

    void EnsureCounts();
    void EnsureStacks();

    // Map of lclNum -> count.
    Counts counts;

    // Map of lclNum -> SsaRenameStateForBlock.
    Stacks stacks;

    // This list represents the set of locals defined in the current block.
    DefStack definedLocs;

    // Same state for the special implicit memory variables.
    ConstructedArray<Stack, MemoryKindCount> memoryStack;
    unsigned memoryCount;

    // Number of stacks/counts to allocate.
    unsigned lvaCount;

    // Allocator to allocate stacks.
    jitstd::allocator<void> m_alloc;

    // Indicates whether GcHeap and ByrefExposed use the same state.
    bool byrefStatesMatchGcHeapStates;
};
