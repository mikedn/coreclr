// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
//                                    Early Value Propagation
//
// This phase performs an SSA-based value propagation optimization that currently only applies to array
// lengths, runtime type handles, and explicit null checks. An SSA-based backwards tracking of local variables
// is performed at each point of interest, e.g., an array length reference site, a method table reference site, or
// an indirection.
// The tracking continues until an interesting value is encountered. The value is then used to rewrite
// the source site or the value.
//
///////////////////////////////////////////////////////////////////////////////////////

#include "jitpch.h"
#include "ssabuilder.h"

bool Compiler::optDoEarlyPropForFunc()
{
    bool propArrayLen  = (optMethodFlags & OMF_HAS_NEWARRAY) && (optMethodFlags & OMF_HAS_ARRAYREF);
    bool propGetType   = (optMethodFlags & OMF_HAS_NEWOBJ) && (optMethodFlags & OMF_HAS_VTABLEREF);
    bool propNullCheck = (optMethodFlags & OMF_HAS_NULLCHECK) != 0;
    return propArrayLen || propGetType || propNullCheck;
}

bool Compiler::optDoEarlyPropForBlock(BasicBlock* block)
{
    bool bbHasArrayRef  = (block->bbFlags & BBF_HAS_IDX_LEN) != 0;
    bool bbHasVtableRef = (block->bbFlags & BBF_HAS_VTABREF) != 0;
    bool bbHasNullCheck = (block->bbFlags & BBF_HAS_NULLCHECK) != 0;
    return bbHasArrayRef || bbHasVtableRef || bbHasNullCheck;
}

//--------------------------------------------------------------------
// gtIsVtableRef: Return true if the tree is a method table reference.
//
// Arguments:
//    tree           - The input tree.
//
// Return Value:
//    Return true if the tree is a method table reference.

bool Compiler::gtIsVtableRef(GenTree* tree)
{
    if (tree->OperGet() == GT_IND)
    {
        GenTree* addr = tree->AsIndir()->Addr();

        if (addr->OperIsAddrMode())
        {
            GenTreeAddrMode* addrMode = addr->AsAddrMode();

            return (!addrMode->HasIndex() && (addrMode->Base()->TypeGet() == TYP_REF));
        }
    }

    return false;
}

//------------------------------------------------------------------------------
// getArrayLengthFromAllocation: Return the array length for an array allocation
//                               helper call.
//
// Arguments:
//    tree           - The array allocation helper call.
//
// Return Value:
//    Return the array length node.

GenTree* Compiler::getArrayLengthFromAllocation(GenTree* tree)
{
    assert(tree != nullptr);

    if (tree->OperGet() == GT_CALL)
    {
        GenTreeCall* call = tree->AsCall();

        if (call->gtCallType == CT_HELPER)
        {
            if (call->gtCallMethHnd == eeFindHelper(CORINFO_HELP_NEWARR_1_DIRECT) ||
                call->gtCallMethHnd == eeFindHelper(CORINFO_HELP_NEWARR_1_R2R_DIRECT) ||
                call->gtCallMethHnd == eeFindHelper(CORINFO_HELP_NEWARR_1_OBJ) ||
                call->gtCallMethHnd == eeFindHelper(CORINFO_HELP_NEWARR_1_VC) ||
                call->gtCallMethHnd == eeFindHelper(CORINFO_HELP_NEWARR_1_ALIGN8))
            {
                // This is an array allocation site. Grab the array length node.
                return gtArgEntryByArgNum(call, 1)->node;
            }
        }
    }

    return nullptr;
}

//-----------------------------------------------------------------------------
// getObjectHandleNodeFromAllocation: Return the type handle for an object allocation
//                              helper call.
//
// Arguments:
//    tree           - The object allocation helper call.
//
// Return Value:
//    Return the object type handle node.

GenTree* Compiler::getObjectHandleNodeFromAllocation(GenTree* tree)
{
    assert(tree != nullptr);

    if (tree->OperGet() == GT_CALL)
    {
        GenTreeCall* call = tree->AsCall();

        if (call->gtCallType == CT_HELPER)
        {
            if (call->gtCallMethHnd == eeFindHelper(CORINFO_HELP_NEWFAST) ||
                call->gtCallMethHnd == eeFindHelper(CORINFO_HELP_NEWSFAST) ||
                call->gtCallMethHnd == eeFindHelper(CORINFO_HELP_NEWSFAST_FINALIZE) ||
                call->gtCallMethHnd == eeFindHelper(CORINFO_HELP_NEWSFAST_ALIGN8) ||
                call->gtCallMethHnd == eeFindHelper(CORINFO_HELP_NEWSFAST_ALIGN8_VC) ||
                call->gtCallMethHnd == eeFindHelper(CORINFO_HELP_NEWSFAST_ALIGN8_FINALIZE) ||
                call->gtCallMethHnd == eeFindHelper(CORINFO_HELP_NEWARR_1_DIRECT) ||
                call->gtCallMethHnd == eeFindHelper(CORINFO_HELP_NEWARR_1_R2R_DIRECT) ||
                call->gtCallMethHnd == eeFindHelper(CORINFO_HELP_NEWARR_1_OBJ) ||
                call->gtCallMethHnd == eeFindHelper(CORINFO_HELP_NEWARR_1_VC) ||
                call->gtCallMethHnd == eeFindHelper(CORINFO_HELP_NEWARR_1_ALIGN8))
            {
                // This is an object allocation site. Return the runtime type handle node.
                fgArgTabEntry* argTabEntry = gtArgEntryByArgNum(call, 0);
                return argTabEntry->node;
            }
        }
    }

    return nullptr;
}

//------------------------------------------------------------------------------------------
// optEarlyProp: The entry point of the early value propagation.
//
// Notes:
//    This phase performs an SSA-based value propagation, including
//      1. Array length propagation.
//      2. Runtime type handle propagation.
//      3. Null check folding.
//
//    For array length propagation, a demand-driven SSA-based backwards tracking of constant
//    array lengths is performed at each array length reference site which is in form of a
//    GT_ARR_LENGTH node. When a GT_ARR_LENGTH node is seen, the array ref pointer which is
//    the only child node of the GT_ARR_LENGTH is tracked. This is only done for array ref
//    pointers that have valid SSA forms.The tracking is along SSA use-def chain and stops
//    at the original array allocation site where we can grab the array length. The
//    GT_ARR_LENGTH node will then be rewritten to a GT_CNS_INT node if the array length is
//    constant.
//
//    Similarly, the same algorithm also applies to rewriting a method table (also known as
//    vtable) reference site which is in form of GT_INDIR node. The base pointer, which is
//    an object reference pointer, is treated in the same way as an array reference pointer.
//
//    Null check folding tries to find GT_INDIR(obj + const) that GT_NULLCHECK(obj) can be folded into
//    and removed. Currently, the algorithm only matches GT_INDIR and GT_NULLCHECK in the same basic block.

void Compiler::optEarlyProp()
{
#ifdef DEBUG
    if (verbose)
    {
        printf("*************** In optEarlyProp()\n");
    }
#endif

    assert(fgSsaPassesCompleted == 1);

    for (BasicBlock* block = fgFirstBB; block != nullptr; block = block->bbNext)
    {
        if (block->bbJumpKind == BBJ_COND)
        {
            compCurBB = block;
            optDoEarlyPropForJTrue(block);
        }
    }

    if (!optDoEarlyPropForFunc())
    {
        return;
    }

    for (BasicBlock* block = fgFirstBB; block != nullptr; block = block->bbNext)
    {
        if (!optDoEarlyPropForBlock(block))
        {
            continue;
        }

        compCurBB = block;

        for (GenTreeStmt* stmt = block->firstStmt(); stmt != nullptr;)
        {
            // Preserve the next link before the propagation and morph.
            GenTreeStmt* next = stmt->gtNextStmt;

            compCurStmt = stmt;

            // Walk the stmt tree in linear order to rewrite any array length reference with a
            // constant array length.
            bool isRewritten = false;
            for (GenTree* tree = stmt->gtStmt.gtStmtList; tree != nullptr; tree = tree->gtNext)
            {
                GenTree* rewrittenTree = optEarlyPropRewriteTree(tree);
                if (rewrittenTree != nullptr)
                {
                    gtUpdateSideEffects(stmt, rewrittenTree);
                    isRewritten = true;
                    tree        = rewrittenTree;
                }
            }

            // Update the evaluation order and the statement info if the stmt has been rewritten.
            if (isRewritten)
            {
                gtSetStmtInfo(stmt);
                fgSetStmtSeq(stmt);
            }

            stmt = next;
        }
    }

#ifdef DEBUG
    if (verbose)
    {
        JITDUMP("\nAfter optEarlyProp:\n");
        fgDispBasicBlocks(/*dumpTrees*/ true);
    }
#endif
}

//----------------------------------------------------------------
// optEarlyPropRewriteValue: Rewrite a tree to the actual value.
//
// Arguments:
//    tree           - The input tree node to be rewritten.
//
// Return Value:
//    Return a new tree if the original tree was successfully rewritten.
//    The containing tree links are updated.
//
GenTree* Compiler::optEarlyPropRewriteTree(GenTree* tree)
{
    GenTree*    objectRefPtr = nullptr;
    optPropKind propKind     = optPropKind::OPK_INVALID;

    if (tree->OperGet() == GT_ARR_LENGTH)
    {
        objectRefPtr = tree->gtOp.gtOp1;
        propKind     = optPropKind::OPK_ARRAYLEN;
    }
    else if (tree->OperIsIndir())
    {
        // optFoldNullCheck takes care of updating statement info if a null check is removed.
        optFoldNullCheck(tree);

        if (gtIsVtableRef(tree))
        {
            // Don't propagate type handles that are used as null checks, which are usually in
            // form of
            //      *  stmtExpr  void  (top level)
            //      \--*  indir     int
            //          \--*  lclVar    ref    V02 loc0
            if (compCurStmt->gtStmt.gtStmtExpr == tree)
            {
                return nullptr;
            }

            objectRefPtr = tree->AsIndir()->Addr();
            propKind     = optPropKind::OPK_OBJ_GETTYPE;
        }
        else
        {
            return nullptr;
        }
    }
    else
    {
        return nullptr;
    }

    if (!objectRefPtr->OperIsScalarLocal() || !lvaInSsa(objectRefPtr->AsLclVarCommon()->GetLclNum()))

    {
        return nullptr;
    }

    unsigned lclNum    = objectRefPtr->AsLclVarCommon()->GetLclNum();
    unsigned ssaNum    = objectRefPtr->AsLclVarCommon()->GetSsaNum();
    GenTree* actualVal = optPropGetValue(lclNum, ssaNum, propKind);

    if (actualVal != nullptr)
    {
        assert((propKind == optPropKind::OPK_ARRAYLEN) || (propKind == optPropKind::OPK_OBJ_GETTYPE));
        assert(actualVal->IsCnsIntOrI());
#if SMALL_TREE_NODES
        assert(actualVal->GetNodeSize() == TREE_NODE_SZ_SMALL);
#endif

        ssize_t actualConstVal = actualVal->AsIntCon()->IconValue();

        if (propKind == optPropKind::OPK_ARRAYLEN)
        {
            if ((actualConstVal < 0) || (actualConstVal > INT32_MAX))
            {
                // Don't propagate array lengths that are beyond the maximum value of a GT_ARR_LENGTH or negative.
                // node. CORINFO_HELP_NEWARR_1_OBJ helper call allows to take a long integer as the
                // array length argument, but the type of GT_ARR_LENGTH is always INT32.
                return nullptr;
            }

            // When replacing GT_ARR_LENGTH nodes with constants we can end up with GT_ARR_BOUNDS_CHECK
            // nodes that have constant operands and thus can be trivially proved to be useless. It's
            // better to remove these range checks here, otherwise they'll pass through assertion prop
            // (creating useless (c1 < c2)-like assertions) and reach RangeCheck where they are finally
            // removed. Common patterns like new int[] { x, y, z } benefit from this.

            if ((tree->gtNext != nullptr) && tree->gtNext->OperIs(GT_ARR_BOUNDS_CHECK))
            {
                GenTreeBoundsChk* check = tree->gtNext->AsBoundsChk();

                if ((check->gtArrLen == tree) && check->gtIndex->IsCnsIntOrI())
                {
                    ssize_t checkConstVal = check->gtIndex->AsIntCon()->IconValue();
                    if ((checkConstVal >= 0) && (checkConstVal < actualConstVal))
                    {
                        GenTree* comma = check->gtGetParent(nullptr);
                        if ((comma != nullptr) && comma->OperIs(GT_COMMA) && (comma->gtGetOp1() == check))
                        {
                            GenTree* next = check->gtNext;
                            optRemoveRangeCheck(comma, compCurStmt);
                            // Both `tree` and `check` have been removed from the statement.
                            // 'tree' was replaced with 'nop' or side effect list under 'comma'.
                            return comma->gtGetOp1();
                        }
                    }
                }
            }
        }

#ifdef DEBUG
        if (verbose)
        {
            printf("optEarlyProp Rewriting " FMT_BB "\n", compCurBB->bbNum);
            gtDispTree(compCurStmt);
            printf("\n");
        }
#endif

        GenTree* actualValClone = gtCloneExpr(actualVal);

        if (actualValClone->gtType != tree->gtType)
        {
            assert(actualValClone->gtType == TYP_LONG);
            assert(tree->gtType == TYP_INT);
            assert((actualConstVal >= 0) && (actualConstVal <= INT32_MAX));
            actualValClone->gtType = tree->gtType;
        }

        // Propagating a constant into an array index expression requires calling
        // LabelIndex to update the FieldSeq annotations.  EarlyProp may replace
        // array length expressions with constants, so check if this is an array
        // length operator that is part of an array index expression.
        bool isIndexExpr = (tree->OperGet() == GT_ARR_LENGTH && ((tree->gtFlags & GTF_ARRLEN_ARR_IDX) != 0));
        if (isIndexExpr)
        {
            actualValClone->LabelIndex(this);
        }

        // actualValClone has small tree node size, it is safe to use CopyFrom here.
        tree->ReplaceWith(actualValClone, this);

#ifdef DEBUG
        if (verbose)
        {
            printf("to\n");
            gtDispTree(compCurStmt);
            printf("\n");
        }
#endif
        return tree;
    }

    return nullptr;
}

//-------------------------------------------------------------------------------------------
// optPropGetValue: Given an SSA object ref pointer, get the value needed based on valueKind.
//
// Arguments:
//    lclNum         - The local var number of the ref pointer.
//    ssaNum         - The SSA var number of the ref pointer.
//    valueKind      - The kind of value of interest.
//
// Return Value:
//    Return the corresponding value based on valueKind.

GenTree* Compiler::optPropGetValue(unsigned lclNum, unsigned ssaNum, optPropKind valueKind)
{
    return optPropGetValueRec(lclNum, ssaNum, valueKind, 0);
}

//-----------------------------------------------------------------------------------
// optPropGetValueRec: Given an SSA object ref pointer, get the value needed based on valueKind
//                     within a recursion bound.
//
// Arguments:
//    lclNum         - The local var number of the array pointer.
//    ssaNum         - The SSA var number of the array pointer.
//    valueKind      - The kind of value of interest.
//    walkDepth      - Current recursive walking depth.
//
// Return Value:
//    Return the corresponding value based on valueKind.

GenTree* Compiler::optPropGetValueRec(unsigned lclNum, unsigned ssaNum, optPropKind valueKind, int walkDepth)
{
    if (ssaNum == SsaConfig::RESERVED_SSA_NUM)
    {
        return nullptr;
    }

    SSAName  ssaName(lclNum, ssaNum);
    GenTree* value = nullptr;

    // Bound the recursion with a hard limit.
    if (walkDepth > optEarlyPropRecurBound)
    {
        return nullptr;
    }

    // Track along the use-def chain to get the array length
    GenTree* treelhs = lvaTable[lclNum].GetPerSsaData(ssaNum)->m_defLoc.m_tree;

    if (treelhs == nullptr)
    {
        // Incoming parameters or live-in variables don't have actual definition tree node
        // for their FIRST_SSA_NUM. See SsaBuilder::RenameVariables.
        assert(ssaNum == SsaConfig::FIRST_SSA_NUM);
    }
    else
    {
        GenTree** lhsPtr;
        GenTree*  treeDefParent = treelhs->gtGetParent(&lhsPtr);

        if (treeDefParent->OperGet() == GT_ASG)
        {
            assert(treelhs == treeDefParent->gtGetOp1());
            GenTree* treeRhs = treeDefParent->gtGetOp2();

            if (treeRhs->OperIsScalarLocal() && lvaInSsa(treeRhs->AsLclVarCommon()->GetLclNum()))
            {
                // Recursively track the Rhs
                unsigned rhsLclNum = treeRhs->AsLclVarCommon()->GetLclNum();
                unsigned rhsSsaNum = treeRhs->AsLclVarCommon()->GetSsaNum();

                value = optPropGetValueRec(rhsLclNum, rhsSsaNum, valueKind, walkDepth + 1);
            }
            else
            {
                if (valueKind == optPropKind::OPK_ARRAYLEN)
                {
                    value = getArrayLengthFromAllocation(treeRhs);
                    if (value != nullptr)
                    {
                        if (!value->IsCnsIntOrI())
                        {
                            // Leave out non-constant-sized array
                            value = nullptr;
                        }
                    }
                }
                else if (valueKind == optPropKind::OPK_OBJ_GETTYPE)
                {
                    value = getObjectHandleNodeFromAllocation(treeRhs);
                    if (value != nullptr)
                    {
                        if (!value->IsCnsIntOrI())
                        {
                            // Leave out non-constant-sized array
                            value = nullptr;
                        }
                    }
                }
            }
        }
    }

    return value;
}

//----------------------------------------------------------------
// optFoldNullChecks: Try to find a GT_NULLCHECK node that can be folded into the GT_INDIR node.
//
// Arguments:
//    tree           - The input GT_INDIR tree.
//

void Compiler::optFoldNullCheck(GenTree* tree)
{
    //
    // Check for a pattern like this:
    //
    //                         =
    //                       /   \
    //                      x    comma
    //                           /   \
    //                     nullcheck  +
    //                         |     / \
    //                         y    y  const
    //
    //
    //                    some trees in the same
    //                    basic block with
    //                    no unsafe side effects
    //
    //                           indir
    //                             |
    //                             x
    //
    // where the const is suitably small
    // and transform it into
    //
    //                         =
    //                       /   \
    //                      x     +
    //                           / \
    //                          y  const
    //
    //
    //              some trees with no unsafe side effects here
    //
    //                           indir
    //                             |
    //                             x

    if ((compCurBB->bbFlags & BBF_HAS_NULLCHECK) == 0)
    {
        return;
    }

    assert(tree->OperIsIndir());

    GenTree* const addr = tree->AsIndir()->Addr();
    if (addr->OperGet() == GT_LCL_VAR)
    {
        // Check if we have the pattern above and find the nullcheck node if we do.

        // Find the definition of the indirected local (x in the picture)
        GenTreeLclVarCommon* const lclVarNode = addr->AsLclVarCommon();

        const unsigned lclNum = lclVarNode->GetLclNum();
        const unsigned ssaNum = lclVarNode->GetSsaNum();

        if (ssaNum != SsaConfig::RESERVED_SSA_NUM)
        {
            DefLoc      defLoc   = lvaTable[lclNum].GetPerSsaData(ssaNum)->m_defLoc;
            BasicBlock* defBlock = defLoc.m_blk;

            if (compCurBB == defBlock)
            {
                GenTree* defTree   = defLoc.m_tree;
                GenTree* defParent = defTree->gtGetParent(nullptr);

                if ((defParent->OperGet() == GT_ASG) && (defParent->gtNext == nullptr))
                {
                    GenTree* defRHS = defParent->gtGetOp2();
                    if (defRHS->OperGet() == GT_COMMA)
                    {
                        if (defRHS->gtGetOp1()->OperGet() == GT_NULLCHECK)
                        {
                            GenTree* nullCheckTree = defRHS->gtGetOp1();
                            if (nullCheckTree->gtGetOp1()->OperGet() == GT_LCL_VAR)
                            {
                                // We found a candidate for 'y' in the picture
                                unsigned nullCheckLclNum = nullCheckTree->gtGetOp1()->AsLclVarCommon()->GetLclNum();

                                if (defRHS->gtGetOp2()->OperGet() == GT_ADD)
                                {
                                    GenTree* additionNode = defRHS->gtGetOp2();
                                    if ((additionNode->gtGetOp1()->OperGet() == GT_LCL_VAR) &&
                                        (additionNode->gtGetOp1()->gtLclVarCommon.gtLclNum == nullCheckLclNum))
                                    {
                                        GenTree* offset = additionNode->gtGetOp2();
                                        if (offset->IsCnsIntOrI())
                                        {
                                            if (!fgIsBigOffset(offset->gtIntConCommon.IconValue()))
                                            {
                                                // Walk from the use to the def in reverse execution order to see
                                                // if any nodes have unsafe side effects.
                                                GenTree*       currentTree        = lclVarNode->gtPrev;
                                                bool           isInsideTry        = compCurBB->hasTryIndex();
                                                bool           canRemoveNullCheck = true;
                                                const unsigned maxNodesWalked     = 25;
                                                unsigned       nodesWalked        = 0;

                                                // First walk the nodes in the statement containing the indirection
                                                // in reverse execution order starting with the indirection's
                                                // predecessor.
                                                while (canRemoveNullCheck && (currentTree != nullptr))
                                                {
                                                    if ((nodesWalked++ > maxNodesWalked) ||
                                                        !optCanMoveNullCheckPastTree(currentTree, isInsideTry))
                                                    {
                                                        canRemoveNullCheck = false;
                                                    }
                                                    else
                                                    {
                                                        currentTree = currentTree->gtPrev;
                                                    }
                                                }

                                                // Then walk the statement list in reverse execution order
                                                // until we get to the statement containing the null check.
                                                // We only need to check the side effects at the root of each statement.
                                                GenTree* curStmt = compCurStmt->gtPrev;
                                                currentTree      = curStmt->gtStmt.gtStmtExpr;
                                                while (canRemoveNullCheck && (currentTree != defParent))
                                                {
                                                    if ((nodesWalked++ > maxNodesWalked) ||
                                                        !optCanMoveNullCheckPastTree(currentTree, isInsideTry))
                                                    {
                                                        canRemoveNullCheck = false;
                                                    }
                                                    else
                                                    {
                                                        curStmt = curStmt->gtStmt.gtPrevStmt;
                                                        assert(curStmt != nullptr);
                                                        currentTree = curStmt->gtStmt.gtStmtExpr;
                                                    }
                                                }

                                                if (canRemoveNullCheck)
                                                {
                                                    // Remove the null check
                                                    nullCheckTree->gtFlags &= ~(GTF_EXCEPT | GTF_DONT_CSE);

                                                    // Set this flag to prevent reordering
                                                    nullCheckTree->gtFlags |= GTF_ORDER_SIDEEFF;
                                                    nullCheckTree->gtFlags |= GTF_IND_NONFAULTING;

                                                    defRHS->gtFlags &= ~(GTF_EXCEPT | GTF_DONT_CSE);
                                                    defRHS->gtFlags |=
                                                        additionNode->gtFlags & (GTF_EXCEPT | GTF_DONT_CSE);

                                                    // Re-morph the statement.
                                                    fgMorphBlockStmt(compCurBB,
                                                                     curStmt->AsStmt() DEBUGARG("optFoldNullCheck"));
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

//----------------------------------------------------------------
// optCanMoveNullCheckPastTree: Check if GT_NULLCHECK can be folded into a node that
//                              is after tree is execution order.
//
// Arguments:
//    tree           - The input GT_INDIR tree.
//    isInsideTry    - True if tree is inside try, false otherwise
//
// Return Value:
//    True if GT_NULLCHECK can be folded into a node that is after tree is execution order,
//    false otherwise.

bool Compiler::optCanMoveNullCheckPastTree(GenTree* tree, bool isInsideTry)
{
    bool result = true;
    if (isInsideTry)
    {
        // We disallow calls, exception sources, and all assignments.
        // Assignments to locals are disallowed inside try because
        // they may be live in the handler.
        if ((tree->gtFlags & GTF_SIDE_EFFECT) != 0)
        {
            result = false;
        }
    }
    else
    {
        // We disallow calls, exception sources, and assignments to
        // global memory.
        if (GTF_GLOBALLY_VISIBLE_SIDE_EFFECTS(tree->gtFlags))
        {
            result = false;
        }
    }
    return result;
}

void Compiler::optDoEarlyPropForJTrue(BasicBlock* block)
{
    GenTreeUnOp* jtrue = block->lastNode()->AsUnOp();
    assert(jtrue->OperIs(GT_JTRUE));

    GenTreeOp* relop = jtrue->gtGetOp1()->AsOp();
    assert(relop->OperIsCompare());

    if (!relop->gtGetOp1()->OperIs(GT_LCL_VAR))
    {
        // First operand must be a local variable.
        return;
    }

    if (relop->IsReverseOp() && !relop->gtGetOp2()->OperIsConst())
    {
        // If the second operand is executed first then it must be a constant,
        // otherwise it doesn't matter.
        //
        // Of course, the second operand could be an arbitrary tree, if we can
        // prove that it doesn't interfere with the tree we're going to replace
        // the first operand with. Not an easy task, at least in part due to
        // SSA representation limitations.
        return;
    }

    GenTreeLclVar* lcl = relop->gtGetOp1()->AsLclVar();

    if (!lvaInSsa(lcl->GetLclNum()))
    {
        // Not a SSA local variable.
        return;
    }

    LclVarDsc*    lclDesc    = lvaGetDesc(lcl);
    LclSsaVarDsc* lclSsaDesc = lclDesc->lvPerSsaData.GetSsaDef(lcl->GetSsaNum());

    if (!lclSsaDesc->IsSingleUse())
    {
        // The SSA definition has multiple uses.
        return;
    }

    if (lclSsaDesc->m_defLoc.m_tree == nullptr)
    {
        // The definition doesn't actually exist, it's a parameter or uninitialized variable.
        return;
    }

    if (lclSsaDesc->m_defLoc.m_blk != block)
    {
        // The SSA definition is in another block. Perhaps it's worth trying to relax
        // this and see if it matches anything.
        return;
    }

    GenTreeOp* asg = lclSsaDesc->m_defLoc.m_tree->gtGetParent(nullptr)->AsOp();
    assert(asg->OperIs(GT_ASG) && (asg->gtGetOp1() == lclSsaDesc->m_defLoc.m_tree));

    if (!asg->gtGetOp1()->OperIs(GT_LCL_VAR))
    {
        // Make sure we don't run into a GT_LCL_FLD.
        return;
    }

    GenTree* rhs = asg->AsOp()->gtGetOp2();

    if (rhs->OperIs(GT_PHI))
    {
        // Can't do much with PHIs, at least not without a significant amount of work...
        return;
    }

    GenTreeStmt* jtrueStmt = block->lastStmt();
    assert(jtrueStmt->gtStmtExpr == jtrue);

    // Maybe we're lucky and the assignment is in the preceding statement.
    GenTreeStmt* asgStmt = jtrueStmt->gtPrevStmt;

    if ((asgStmt != nullptr) && (asgStmt->gtStmtExpr == asg))
    {
// OK, we can simply replace the lcl node with its definition tree.

#ifdef DEBUG
        if (verbose)
        {
            printf("found JTRUE tree using an entire single-use tree:\n");
            gtDispTree(asgStmt);
            printf("---------------\n");
            gtDispTree(jtrueStmt);
            printf("\n");
        }
#endif // DEBUG

        relop->gtOp1 = rhs;

        fgMorphTree(jtrue);

        // Erm, morph somtimes produces a JTRUE(0) or JTRUE(1) tree!?!.
        // That's not valid, put the relop back.
        if (!jtrue->gtGetOp1()->OperIsCompare())
        {
            assert(jtrue->gtGetOp1()->IsIntegralConst(0) || jtrue->gtGetOp1()->IsIntegralConst(1));
            jtrue->gtOp1 = gtNewOperNode(jtrue->gtGetOp1()->IsIntegralConst(0) ? GT_NE : GT_EQ, TYP_INT,
                                         gtNewIconNode(0), gtNewIconNode(0));
            jtrue->gtOp1->gtFlags |= GTF_RELOP_JMP_USED;
        }

        gtSetStmtInfo(jtrueStmt);
        fgSetStmtSeq(jtrueStmt);
        fgRemoveStmt(block, asgStmt);

#ifdef DEBUG
        if (verbose)
        {
            printf("changed to:\n");
            gtDispTree(jtrueStmt);
            printf("---------------\n\n");
        }
#endif // DEBUG

        return;
    }

    // Well, we weren't lucky and we don't know where the definiton is. We'll have to search
    // for the statement because we need it later to call gtSetStmtInfo and fgSetStmtSeq.

    for (GenTreeStmt* stmt = block->firstStmt(); stmt != nullptr; stmt = stmt->gtNextStmt)
    {
        if (stmt->AsStmt()->gtStmtExpr == asg)
        {
            asgStmt = stmt;
            break;
        }
    }

    if (asgStmt == nullptr)
    {
        // Could not find the statement. It's supposed to be in this block so it's probably
        // located inside a tree, wrapped in a COMMA or CALL. Maybe we should include the
        // statement in LclSsaVarDsc?
        return;
    }

    // Let's see what part of the definition tree we can move. We're looking for a relop that
    // can combine with the existing relop but some other opers could be useful as well:
    //   - GT_CAST can sometimes combine with a relop by relop narrowing
    //   - bitwise and arithmetic opers can combine with a 0/non-zero compare by means of flags
    //   - some shifts could also combine with a 0/non-zero compare, though that doesn't work today
    //
    // Binary operators are bit more problematic - we start with one live range and by moving
    // the node that one disappears.
    // If the operator we're moving is unary, a single live range we'll extend to replace the
    // old one so no harm done, hopefully.
    // If the operator is binary and both its operands are variables (or more complex trees
    // with even more variables) then we're going to end up extending more than one live range,
    // which may impact register allocation. So for now let's be conservative and only move
    // binary operators that have a constant operand.

    GenTree*  newRhs    = rhs;
    GenTree** newRhsUse = nullptr;

    while (((newRhs->gtFlags & GTF_ALL_EFFECT) == 0) &&
           (newRhs->OperIs(GT_NEG, GT_NOT, GT_CAST) ||
            (newRhs->OperIs(GT_ADD, GT_SUB, GT_MUL, GT_DIV, GT_UDIV, GT_MOD, GT_UMOD, GT_AND, GT_OR, GT_XOR, GT_LSH,
                            GT_RSH, GT_RSZ, GT_ROL, GT_ROR, GT_EQ, GT_NE, GT_GT, GT_GE, GT_LT, GT_LE) &&
             newRhs->gtGetOp2()->OperIsConst())))
    {
        newRhsUse = &newRhs->AsOp()->gtOp1;
        newRhs    = *newRhsUse;
    }

    if (rhs == newRhs)
    {
        // Could not find any suitable nodes to move.
        return;
    }

#ifdef DEBUG
    if (verbose)
    {
        printf("found JTRUE tree using a partial single-use tree:\n");
        gtDispTree(asgStmt);
        printf("---------------\n");
        gtDispTree(jtrueStmt);
        printf("\n");
    }
#endif // DEBUG

    // TODO Watch out for small int types and normalize on load/store
    if (newRhs->TypeGet() != lclDesc->TypeGet())
    {
        JITDUMP("changing variable type from %s to %s\n", varTypeName(lclDesc->TypeGet()),
                varTypeName(newRhs->TypeGet()));

        GenTreeLclVar* lhs = asg->gtGetOp1()->AsLclVar();

        if (lclDesc->lvPerSsaData.GetCount() > 1)
        {
            JITDUMP("existing variable has multiple definitions and it cannot be retyped\n");

            // The new RHS node has a different type than the old one. And the variable
            // has multiple definitions so we cannot change its type, create a new one.
            unsigned   newLclNum       = lvaGrabTemp(true DEBUGARG("jtrue-relop-subst"));
            LclVarDsc* newLclDsc       = lvaGetDesc(newLclNum);
            unsigned   newSsaNum       = newLclDsc->lvPerSsaData.AllocSsaNum(getAllocator(CMK_SSA), block, lhs);
            newLclDsc->lvType          = genActualType(newRhs->TypeGet());
            newLclDsc->lvStructGcCount = varTypeIsGC(newLclDsc->lvType);
            // Unfortunately we can't actually put the new variable in SSA. While allocating
            // a new SSA number for it isn't a problem, being in SSA also implies being tracked.
            // And setting up a new tracked variable is complicated because it has to be added
            // to lvaTrackedToVarNum and lvaTrackedCount. That it's not such a big problem in
            // itself but then you must also update the flowgraph liveness bitvectors...
            newLclDsc->lvInSsa   = false; // true;
            newLclDsc->lvTracked = false;

            lhs->gtType = newLclDsc->TypeGet();
            lhs->SetLclNum(newLclNum);
            lhs->SetSsaNum(newSsaNum);
            asg->gtType = newLclDsc->TypeGet();
            lcl->gtType = newLclDsc->TypeGet();
            lcl->SetLclNum(newLclNum);
            lcl->SetSsaNum(newSsaNum);
        }
        else
        {
            lclDesc->lvType = newRhs->TypeGet();

            lclDesc->lvStructGcCount = varTypeIsGC(newRhs->TypeGet());

            lhs->gtType = newRhs->TypeGet();
            asg->gtType = newRhs->TypeGet();
            lcl->gtType = newRhs->TypeGet();
        }
    }

    // Move the rhs - newRhs chain from the ASG tree to the JTRUE tree.
    asg->AsOp()->gtOp2 = newRhs;
    *newRhsUse         = lcl;
    relop->gtOp1       = rhs;

    // Morph and update both statements.
    fgMorphTree(asg);
    fgMorphTree(jtrue);

    gtSetStmtInfo(asgStmt);
    fgSetStmtSeq(asgStmt);

    gtSetStmtInfo(jtrueStmt);
    fgSetStmtSeq(jtrueStmt);

#ifdef DEBUG
    if (verbose)
    {
        printf("changed to:\n");
        gtDispTree(asgStmt);
        printf("---------------\n");
        gtDispTree(jtrueStmt);
        printf("---------------\n\n");
    }
#endif // DEBUG
}
