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
SsaRenameState::SsaRenameState(CompAllocator* alloc, unsigned lvaCount, bool byrefStatesMatchGcHeapStates)
    : m_lclDefCounts(nullptr)
    , m_stacks(nullptr)
    , m_definedLocs(nullptr)
    , m_memoryStack()
    , m_memoryCount(0)
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
        m_lclDefCounts = new (m_alloc) unsigned[lvaCount];
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
    if (m_stacks == nullptr)
    {
        m_stacks = new (m_alloc) BlockState*[lvaCount]();
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

    BlockState* stack = m_stacks[lclNum];
    if (stack == nullptr)
    {
        return SsaConfig::UNINIT_SSA_NUM;
    }
    return stack->m_ssaNum;
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

    BlockState* state = m_stacks[lclNum];

    if ((state == nullptr) || (state->m_bbNum != bbNum))
    {
        m_stacks[lclNum] = new (m_alloc) BlockState(state, bbNum, ssaNum);
        // Remember that we've pushed a def for this loc (so we don't have
        // to traverse *all* the locs to do the necessary pops later).
        m_definedLocs = new (m_alloc) LclDefState(m_definedLocs, bbNum, lclNum);
    }
    else
    {
        state->m_ssaNum = ssaNum;
    }

#ifdef DEBUG
    if (JitTls::GetCompiler()->verboseSsa)
    {
        printf("\tContents of the stack: [");
        for (BlockState* state = m_stacks[lclNum]; state != nullptr; state = state->m_prev)
        {
            printf("<BB%02u, %d>", state->m_bbNum, state->m_ssaNum);
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
    while ((m_definedLocs != nullptr) && (m_definedLocs->m_bbNum == bbNum))
    {
        unsigned lclNum = m_definedLocs->m_lclNum;
        assert(m_stacks != nullptr); // Cannot be empty because definedLocs is not empty.
        BlockState* state = m_stacks[lclNum];
        assert(state != nullptr);
        assert(state->m_bbNum == bbNum);
        m_stacks[lclNum] = state->m_prev;
        m_definedLocs    = m_definedLocs->m_prev;
    }
#ifdef DEBUG
    if (m_stacks != nullptr)
    {
        // It should now be the case that no stack in stacks has an entry for "block" on top --
        // the loop above popped them all.
        for (unsigned i = 0; i < lvaCount; ++i)
        {
            assert((m_stacks[i] == nullptr) || (m_stacks[i]->m_bbNum != bbNum));
        }
        if (JitTls::GetCompiler()->verboseSsa)
        {
            DumpStacks();
        }
    }
#endif // DEBUG
}

void SsaRenameState::PopBlockMemoryStack(MemoryKind memoryKind, BasicBlock* block)
{
    BlockState* state = m_memoryStack[memoryKind];
    while ((state != nullptr) && (state->m_bbNum == block->bbNum))
    {
        state = state->m_prev;
    }
    m_memoryStack[memoryKind] = state;
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
            printf("V%02u:\t", i);
            for (BlockState* state = m_stacks[i]; state != nullptr; state = state->m_prev)
            {
                printf("<BB%02u, %2d>%s", state->m_bbNum, state->m_ssaNum, (state->m_prev != nullptr) ? "," : "");
            }
            printf("\n");
        }
    }
}
#endif // DEBUG
