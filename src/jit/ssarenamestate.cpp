// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#include "jitpch.h"
#include "ssaconfig.h"
#include "ssarenamestate.h"

/**
 * Constructor - initialize the stacks and counters maps (lclVar -> stack/counter) map.
 *
 * @params alloc The allocator class used to allocate jitstd data.
 */
SsaRenameState::SsaRenameState(const jitstd::allocator<int>& alloc,
                               unsigned                      lvaCount,
                               bool                          byrefStatesMatchGcHeapStates)
    : m_lclDefCounts(nullptr)
    , stacks(nullptr)
    , definedLocs(alloc)
    , memoryStack(alloc)
    , memoryCount(0)
    , lvaCount(lvaCount)
    , m_alloc(alloc)
    , byrefStatesMatchGcHeapStates(byrefStatesMatchGcHeapStates)
{
}

/**
 * Allocates memory to hold SSA variable def counts,
 * if not allocated already.
 *
 */
void SsaRenameState::EnsureCounts()
{
    if (m_lclDefCounts == nullptr)
    {
        m_lclDefCounts = jitstd::utility::allocate<unsigned>(m_alloc, lvaCount);
        for (unsigned i = 0; i < lvaCount; ++i)
        {
            m_lclDefCounts[i] = SsaConfig::FIRST_SSA_NUM;
        }
    }
}

/**
 * Allocates memory for holding pointers to lcl's stacks,
 * if not allocated already.
 *
 */
void SsaRenameState::EnsureStacks()
{
    if (stacks == nullptr)
    {
        stacks = jitstd::utility::allocate<Stack*>(m_alloc, lvaCount);
        for (unsigned i = 0; i < lvaCount; ++i)
        {
            stacks[i] = nullptr;
        }
    }
}

/**
 * Returns a SSA count number for a local variable and does a post increment.
 *
 * If there is no counter for the local yet, initializes it with the default value
 * else, returns the count with a post increment, so the next def gets a new count.
 *
 * @params lclNum The local variable def for which a count has to be returned.
 * @return the variable name for the current definition.
 *
 */
unsigned SsaRenameState::AllocSsaNum(unsigned lclNum)
{
    EnsureCounts();
    unsigned count = m_lclDefCounts[lclNum];
    m_lclDefCounts[lclNum]++;
    DBG_SSA_JITDUMP("Incrementing counter = %d by 1 for V%02u.\n", count, lclNum);
    return count;
}

/**
 * Returns a SSA count number for a local variable from top of the stack.
 *
 * @params lclNum The local variable def for which a count has to be returned.
 * @return the current variable name for the "use".
 *
 * @remarks If the stack is empty, then we have an use before a def. To handle this
 *          special case, we need to initialize the count with 'default+1', so the
 *          next definition will always use 'default+1' but return 'default' for
 *          all uses until a definition.
 *
 */
unsigned SsaRenameState::GetTopSsaNum(unsigned lclNum)
{
    EnsureStacks();
    DBG_SSA_JITDUMP("[SsaRenameState::CountForUse] V%02u\n", lclNum);

    Stack* stack = stacks[lclNum];
    if (stack == nullptr || stack->empty())
    {
        return SsaConfig::UNINIT_SSA_NUM;
    }
    return stack->back().m_ssaNum;
}

/**
 * Pushes a count value on the variable stack.
 *
 * @params lclNum The local variable def whose stack the count needs to be pushed onto.
 * @params count The current count value that needs to be pushed on to the stack.
 *
 * @remarks Usually called when renaming a "def."
 *          Create stack lazily when needed for the first time.
 */
void SsaRenameState::Push(BasicBlock* bb, unsigned lclNum, unsigned ssaNum)
{
    EnsureStacks();

    // We'll use BB00 here to indicate the "block before any real blocks..."
    unsigned bbNum = (bb == nullptr) ? 0 : bb->bbNum;

    DBG_SSA_JITDUMP("[SsaRenameState::Push] BB%02u, V%02u, count = %d\n", bbNum, lclNum, ssaNum);

    Stack* stack = stacks[lclNum];

    if (stack == nullptr)
    {
        DBG_SSA_JITDUMP("\tCreating a new stack\n");
        stack = stacks[lclNum] = new (jitstd::utility::allocate<Stack>(m_alloc), jitstd::placement_t()) Stack(m_alloc);
    }

    if (stack->empty() || (stack->back().m_bbNum != bbNum))
    {
        stack->push_back(BlockState(bbNum, ssaNum));
        // Remember that we've pushed a def for this loc (so we don't have
        // to traverse *all* the locs to do the necessary pops later).
        definedLocs.push_back(LclDefState(bbNum, lclNum));
    }
    else
    {
        stack->back().m_ssaNum = ssaNum;
    }

#ifdef DEBUG
    if (JitTls::GetCompiler()->verboseSsa)
    {
        printf("\tContents of the stack: [");
        for (Stack::iterator iter2 = stack->begin(); iter2 != stack->end(); iter2++)
        {
            printf("<BB%02u, %d>", iter2->m_bbNum, iter2->m_ssaNum);
        }
        printf("]\n");

        DumpStacks();
    }
#endif
}

void SsaRenameState::PopBlockStacks(BasicBlock* block)
{
    unsigned bbNum = block->bbNum;

    DBG_SSA_JITDUMP("[SsaRenameState::PopBlockStacks] BB%02u\n", bbNum);
    // Iterate over the stacks for all the variables, popping those that have an entry
    // for "block" on top.
    while (!definedLocs.empty() && (definedLocs.back().m_bbNum == bbNum))
    {
        unsigned lclNum = definedLocs.back().m_lclNum;
        assert(stacks != nullptr); // Cannot be empty because definedLocs is not empty.
        Stack* stack = stacks[lclNum];
        assert(stack != nullptr);
        assert(stack->back().m_bbNum == bbNum);
        stack->pop_back();
        definedLocs.pop_back();
    }
#ifdef DEBUG
    // It should now be the case that no stack in stacks has an entry for "block" on top --
    // the loop above popped them all.
    for (unsigned i = 0; i < lvaCount; ++i)
    {
        if (stacks != nullptr && stacks[i] != nullptr && !stacks[i]->empty())
        {
            assert(stacks[i]->back().m_bbNum != bbNum);
        }
    }
    if (JitTls::GetCompiler()->verboseSsa)
    {
        DumpStacks();
    }
#endif // DEBUG
}

void SsaRenameState::PopBlockMemoryStack(MemoryKind memoryKind, BasicBlock* block)
{
    auto& stack = memoryStack[memoryKind];
    while ((stack.size() > 0) && (stack.back().m_bbNum == block->bbNum))
    {
        stack.pop_back();
    }
}

#ifdef DEBUG
/**
 * Print the stack data for each variable in a loop.
 */
void SsaRenameState::DumpStacks()
{
    printf("Dumping stacks:\n-------------------------------\n");
    if (lvaCount == 0)
    {
        printf("None\n");
    }
    else
    {
        EnsureStacks();
        for (unsigned i = 0; i < lvaCount; ++i)
        {
            Stack* stack = stacks[i];
            printf("V%02u:\t", i);
            if (stack != nullptr)
            {
                for (Stack::iterator iter2 = stack->begin(); iter2 != stack->end(); ++iter2)
                {
                    if (iter2 != stack->begin())
                    {
                        printf(", ");
                    }
                    printf("<BB%02u, %2d>", iter2->m_bbNum, iter2->m_ssaNum);
                }
            }
            printf("\n");
        }
    }
}
#endif // DEBUG
