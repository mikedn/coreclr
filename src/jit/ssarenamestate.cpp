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
SsaRenameState::SsaRenameState(CompAllocator* alloc,
                               unsigned       lvaCount,
                               bool           byrefStatesMatchGcHeapStates)
    : counts(nullptr)
    , m_lclStacks(nullptr)
    , m_blockStack(nullptr)
    , m_freeStack(nullptr)
    , m_memoryStacks()
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
    if (counts == nullptr)
    {
        counts = new (m_alloc) unsigned[lvaCount];
        for (unsigned i = 0; i < lvaCount; ++i)
        {
            counts[i] = SsaConfig::FIRST_SSA_NUM;
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
    if (m_lclStacks == nullptr)
    {
        m_lclStacks = new (m_alloc) Stack*[lvaCount];
        for (unsigned i = 0; i < lvaCount; ++i)
        {
            m_lclStacks[i] = nullptr;
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
unsigned SsaRenameState::CountForDef(unsigned lclNum)
{
    EnsureCounts();
    unsigned count = counts[lclNum];
    counts[lclNum]++;
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
unsigned SsaRenameState::CountForUse(unsigned lclNum)
{
    EnsureStacks();
    DBG_SSA_JITDUMP("[SsaRenameState::CountForUse] V%02u\n", lclNum);

    Stack* stack = m_lclStacks[lclNum];
    if (stack == nullptr)
    {
        return SsaConfig::UNINIT_SSA_NUM;
    }
    return stack->m_ssaNum;
}

SsaRenameState::Stack* SsaRenameState::AllocBlockState(Stack* list, Stack* stack, unsigned bbNum, unsigned lclNum, unsigned ssaNum)
{
    Stack* state = m_freeStack;

    if (state != nullptr)
    {
        m_freeStack = state->m_list;
    }
    else
    {
        state = static_cast<Stack*>(m_alloc->Alloc(sizeof(Stack)));
    }

    return new (state, jitstd::placement_t()) Stack(list, stack, bbNum, lclNum, ssaNum);
}

void SsaRenameState::FreeBlockStateList(Stack* first, Stack* last)
{
    last->m_list = m_freeStack;
    m_freeStack  = first;
}

/**
 * Pushes the initial SSA number onto the lclNum stack.
 *
 * @params lclNum The local variable def whose stack the count needs to be pushed onto.
 * @params ssaNum The initial SSA number to be pushed onto the stack.
 *
 * @remarks Usually called when renaming a "def."
 */
void SsaRenameState::PushLclInit(unsigned lclNum, unsigned ssaNum)
{
    EnsureStacks();

    // We'll use BB00 here to indicate the "block before any real blocks..."
    DBG_SSA_JITDUMP("[SsaRenameState::PushInit] BB00, V%02u, count = %d\n", lclNum, ssaNum);

    Stack* stack = m_lclStacks[lclNum];
    // The stack should be empty when PushInit is called
    assert(stack == nullptr);
    // Note that the block associated with these initialization definitions does not
    // actually exists thus it will never be popped. Because of this we don't need to
    // push these onto the block stack nor we need to use the free list.
    m_lclStacks[lclNum] = new (m_alloc) Stack(nullptr, stack, 0, lclNum, ssaNum);
}

/**
 * Pushes a count value on the variable stack.
 *
 * @params lclNum The local variable def whose stack the count needs to be pushed onto.
 * @params count The current count value that needs to be pushed on to the stack.
 *
 * @remarks Usually called when renaming a "def."
 */
void SsaRenameState::Push(BasicBlock* block, unsigned lclNum, unsigned ssaNum)
{
    EnsureStacks();

    unsigned bbNum = block->bbNum;

    DBG_SSA_JITDUMP("[SsaRenameState::Push] BB%02u, V%02u, count = %d\n", bbNum, lclNum, ssaNum);

    Stack* stack = m_lclStacks[lclNum];

    if ((stack == nullptr) || (stack->m_bbNum != bbNum))
    {
        stack = new (m_alloc) Stack(m_blockStack, stack, bbNum, lclNum, ssaNum);

        m_lclStacks[lclNum] = stack;
        m_blockStack = stack;
    }
    else
    {
        stack->m_ssaNum = ssaNum;
    }

#ifdef DEBUG
    if (JitTls::GetCompiler()->verboseSsa)
    {
        printf("\tContents of the stack: [");
        for (Stack* s = stack; s != nullptr; s = s->m_stack)
        {
            printf("<BB%02u, %d>", s->m_bbNum, s->m_ssaNum);
        }
        printf("]\n");

        DumpStacks();
    }
#endif
}

void SsaRenameState::PopBlockStacks(BasicBlock* block)
{
    unsigned const bbNum = block->bbNum;

    DBG_SSA_JITDUMP("[SsaRenameState::PopBlockStacks] BB%02u\n", bbNum);

    Stack* const firstFree = m_blockStack;
    Stack* lastFree        = nullptr;

    for (Stack* stack = m_blockStack; (stack != nullptr) && (stack->m_bbNum == bbNum); stack = stack->m_list)
    {
        // This states's local stack better have the state on top.
        assert(m_lclStacks[stack->m_lclNum] == stack); 
        
        // Pop the state from the local stack.
        m_lclStacks[stack->m_lclNum] = stack->m_stack;
        lastFree = stack;
    }

    if (lastFree != nullptr)
    {
        // Pop all states from the block stack.
        m_blockStack = lastFree->m_list;
        FreeBlockStateList(firstFree, lastFree);
    }

#ifdef DEBUG
    if (m_lclStacks != nullptr)
    {
        // It should now be the case that no stack in stacks has an entry for "block" on top --
        // the loop above popped them all.
        for (unsigned i = 0; i < lvaCount; ++i)
        {
            assert((m_lclStacks[i] == nullptr) || (m_lclStacks[i]->m_bbNum != bbNum));
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
    Stack*& stack = m_memoryStacks[memoryKind];
    while ((stack != nullptr) && (stack->m_bbNum == block->bbNum))
    {
        stack = stack->m_stack;
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
            printf("V%02u:\t", i);
            for (Stack* s = m_lclStacks[i]; s != nullptr; s = s->m_stack)
            {
                printf("<BB%02u, %2d>%s", s->m_bbNum, s->m_ssaNum, (s->m_stack != nullptr) ? ", " : "");
            }
            printf("\n");
        }
    }
}
#endif // DEBUG
