//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2011 EMC Corp.
//
//	@filename:
//		CJoinOrder.cpp
//
//	@doc:
//		Implementation of join order logic
//---------------------------------------------------------------------------

#include "gpos/base.h"

#include "gpos/io/COstreamString.h"
#include "gpos/string/CWStringDynamic.h"

#include "gpos/common/clibwrapper.h"
#include "gpos/common/CBitSet.h"

#include "gpopt/base/CDrvdPropScalar.h"
#include "gpopt/base/CColRefSetIter.h"
#include "gpopt/operators/ops.h"
#include "gpopt/operators/CPredicateUtils.h"
#include "gpopt/xforms/CJoinOrder.h"


using namespace gpopt;

			
//---------------------------------------------------------------------------
//	@function:
//		ICmpEdgesByLength
//
//	@doc:
//		Comparison function for simple join ordering: sort edges by length
//		only to guaranteed that single-table predicates don't end up above 
//		joins;
//
//---------------------------------------------------------------------------
INT ICmpEdgesByLength
	(
	const void *pvOne,
	const void *pvTwo
	)
{
	CJoinOrder::SEdge *pedgeOne = *(CJoinOrder::SEdge**)pvOne;
	CJoinOrder::SEdge *pedgeTwo = *(CJoinOrder::SEdge**)pvTwo;

	
	INT iDiff = (pedgeOne->m_pbs->Size() - pedgeTwo->m_pbs->Size());
	if (0 == iDiff)
	{
		return (INT)pedgeOne->m_pbs->HashValue() - (INT)pedgeTwo->m_pbs->HashValue();
	}
		
	return iDiff;
}


// ctor
CJoinOrder::SComponent::SComponent
	(
	IMemoryPool *mp,
	CExpression *pexpr,
	INT parent_loj_id,
	EPosition position
	)
	:
	m_pbs(NULL),
	m_edge_set(NULL),
	m_pexpr(pexpr),
	m_fUsed(false),
	m_parent_loj_id(parent_loj_id),
	m_position(position)
{
	m_pbs = GPOS_NEW(mp) CBitSet(mp);
	m_edge_set = GPOS_NEW(mp) CBitSet(mp);
	GPOS_ASSERT_IMP(EpSentinel != m_position, NON_LOJ_DEFAULT_ID < m_parent_loj_id);
}

// ctor
CJoinOrder::SComponent::SComponent
	(
	CExpression *pexpr,
	CBitSet *pbs,
	CBitSet *edge_set,
	INT parent_loj_id,
	EPosition position
	)
	:
	m_pbs(pbs),
	m_edge_set(edge_set),
	m_pexpr(pexpr),
	m_fUsed(false),
	m_parent_loj_id(parent_loj_id),
	m_position(position)
{
	GPOS_ASSERT(NULL != pbs);
	GPOS_ASSERT_IMP(EpSentinel != m_position, NON_LOJ_DEFAULT_ID < m_parent_loj_id);
}

//---------------------------------------------------------------------------
//	@function:
//		CJoinOrder::SComponent::~SComponent
//
//	@doc:
//		Ctor
//
//---------------------------------------------------------------------------
CJoinOrder::SComponent::~SComponent()
{	
	m_pbs->Release();
	m_edge_set->Release();
	CRefCount::SafeRelease(m_pexpr);
}

//---------------------------------------------------------------------------
//	@function:
//		CJoinOrder::SComponent::OsPrint
//
//	@doc:
//		Debug print
//
//---------------------------------------------------------------------------
IOstream &
CJoinOrder::SComponent::OsPrint
	(
	IOstream &os
	)
const
{
	CBitSet *pbs = m_pbs;
	os 
		<< "Component: ";
	os
		<< (*pbs) << std::endl;
	os
		<< *m_pexpr << std::endl;
	
	if (m_parent_loj_id > NON_LOJ_DEFAULT_ID)
	{
		GPOS_ASSERT(m_position != EpSentinel);
		os
			<< "Parent LOJ id: ";
		os
			<<  m_parent_loj_id << std::endl;
		os
			<< "Child Position: ";
		os
			<<  m_position << std::endl;
	}

	return os;
}

//---------------------------------------------------------------------------
//	@function:
//		CJoinOrder::SEdge::SEdge
//
//	@doc:
//		Ctor
//
//---------------------------------------------------------------------------
CJoinOrder::SEdge::SEdge
	(
	IMemoryPool *mp,
	CExpression *pexpr
	)
	:
	m_pbs(NULL),
	m_pexpr(pexpr),
	m_fUsed(false)
{	
	m_pbs = GPOS_NEW(mp) CBitSet(mp);
}


//---------------------------------------------------------------------------
//	@function:
//		CJoinOrder::SEdge::~SEdge
//
//	@doc:
//		Ctor
//
//---------------------------------------------------------------------------
CJoinOrder::SEdge::~SEdge()
{	
	m_pbs->Release();
	m_pexpr->Release();
}


//---------------------------------------------------------------------------
//	@function:
//		CJoinOrder::SEdge::OsPrint
//
//	@doc:
//		Debug print
//
//---------------------------------------------------------------------------
IOstream &
CJoinOrder::SEdge::OsPrint
	(
	IOstream &os
	)
	const
{
	return os 
		<< "Edge : " 
		<< (*m_pbs) << std::endl 
		<< *m_pexpr << std::endl;
}


//---------------------------------------------------------------------------
//	@function:
//		CJoinOrder::CJoinOrder
//
//	@doc:
//		Ctor
//
//---------------------------------------------------------------------------
CJoinOrder::CJoinOrder
	(
	IMemoryPool *mp,
	CExpressionArray *pdrgpexpr,
	CExpressionArray *pdrgpexprConj,
	BOOL include_loj_childs
	)
	:
	m_mp(mp),
	m_rgpedge(NULL),
	m_ulEdges(0),
	m_rgpcomp(NULL),
	m_ulComps(0),
	m_include_loj_childs(include_loj_childs)
{
	typedef SComponent* Pcomp;
	typedef SEdge* Pedge;
	
	const ULONG num_of_nary_children = pdrgpexpr->Size();
	INT num_of_lojs = 0;

	// Since we are using a static array, we need to know size of the array before hand
	// e.g.
	// +--CLogicalNAryJoin
	// |--CLogicalGet "t1"
	// |--CLogicalLeftOuterJoin
	// |  |--CLogicalGet "t5"
	// |  |--CLogicalGet "t4"
	// |  +--CScalarCmp (=)
	// +--CScalarCmp (=)
	//
	// In above case the pdrgpexpr comes with two elements in it:
	//  - CLogicalGet "t1"
	//  - CLogicalLeftOuterJoin
	// We need to create components out of "t1", "t4", "t5" and store them
	// in m_rgcomp.
	// so, total number of components = size of pdrgpexpr + no. of LOJs in it
	for (ULONG ul = 0; ul < num_of_nary_children && m_include_loj_childs; ul++)
	{
		CExpression *pexpr = (*pdrgpexpr)[ul];
		if (COperator::EopLogicalLeftOuterJoin == pexpr->Pop()->Eopid())
		{
			num_of_lojs++;
		}
	}

	m_ulComps = num_of_nary_children + num_of_lojs;
	m_rgpcomp = GPOS_NEW_ARRAY(mp, Pcomp, m_ulComps);

	INT loj_id = 0;
	INT comp_num = 0;

	for (ULONG ul = 0; ul < num_of_nary_children; ul++, comp_num++)
	{
		CExpression *expr = (*pdrgpexpr)[ul];
		if (m_include_loj_childs &&
			COperator::EopLogicalLeftOuterJoin == expr->Pop()->Eopid())
		{
			// counter for number of loj available in tree
			++loj_id;

			// add left child
			AddComponent(mp, (*expr)[0], loj_id, EpLeft, comp_num);

			// increment comp_num, as it's the second child for the same ul
			comp_num++;
			// add right child.
			AddComponent(mp, (*expr)[1], loj_id, EpRight, comp_num);

			// add scalar
			CExpression *scalar_expr = (*expr)[2];
			scalar_expr->AddRef();
			pdrgpexprConj->Append(scalar_expr);
		}
		else
		{
			AddComponent(mp, expr, NON_LOJ_DEFAULT_ID, EpSentinel, comp_num);
		}
		

	}

	m_ulEdges = pdrgpexprConj->Size();
	m_rgpedge = GPOS_NEW_ARRAY(mp, Pedge, m_ulEdges);
	
	for (ULONG ul = 0; ul < m_ulEdges; ul++)
	{
		CExpression *pexprEdge = (*pdrgpexprConj)[ul];
		pexprEdge->AddRef();
		m_rgpedge[ul] = GPOS_NEW(mp) SEdge(mp, pexprEdge);
	}
	
	pdrgpexpr->Release();
	pdrgpexprConj->Release();
	
	ComputeEdgeCover();
}


//---------------------------------------------------------------------------
//	@function:
//		CJoinOrder::~CJoinOrder
//
//	@doc:
//		Dtor
//
//---------------------------------------------------------------------------
CJoinOrder::~CJoinOrder()
{
	for (ULONG ul = 0; ul < m_ulComps; ul++)
	{
		m_rgpcomp[ul]->Release();
	}
	GPOS_DELETE_ARRAY(m_rgpcomp);

	for (ULONG ul = 0; ul < m_ulEdges; ul++)
	{
		m_rgpedge[ul]->Release();
	}
	GPOS_DELETE_ARRAY(m_rgpedge);
}


//---------------------------------------------------------------------------
//	@function:
//		CJoinOrder::ComputeEdgeCover
//
//	@doc:
//		Compute cover for each edge and also the index of edges associated
//		with each component
//
//---------------------------------------------------------------------------
void
CJoinOrder::ComputeEdgeCover()
{
	for (ULONG ulEdge = 0; ulEdge < m_ulEdges; ulEdge++)
	{
		CExpression *pexpr = m_rgpedge[ulEdge]->m_pexpr;
		CColRefSet *pcrsUsed = CDrvdPropScalar::GetDrvdScalarProps(pexpr->PdpDerive())->PcrsUsed();

		for (ULONG ulComp = 0; ulComp < m_ulComps; ulComp++)
		{
			CExpression *pexprComp = m_rgpcomp[ulComp]->m_pexpr;
			CColRefSet *pcrsOutput = CDrvdPropRelational::GetRelationalProperties(pexprComp->PdpDerive())->PcrsOutput();


			if (!pcrsUsed->IsDisjoint(pcrsOutput))
			{
				(void) m_rgpcomp[ulComp]->m_edge_set->ExchangeSet(ulEdge);
				(void) m_rgpedge[ulEdge]->m_pbs->ExchangeSet(ulComp);
			}
		}
	}
}

//---------------------------------------------------------------------------
//	@function:
//		CJoinOrder::PcompCombine
//
//	@doc:
//		Combine the two given components using applicable edges
//
//
//---------------------------------------------------------------------------
CJoinOrder::SComponent *
CJoinOrder::PcompCombine
	(
	SComponent *comp1,
	SComponent *comp2
	)
{
	GPOS_ASSERT(IsValidJoinCombination(comp1, comp2));
	CBitSet *pbs = GPOS_NEW(m_mp) CBitSet(m_mp);
	CBitSet *edge_set = GPOS_NEW(m_mp) CBitSet(m_mp);

	pbs->Union(comp1->m_pbs);
	pbs->Union(comp2->m_pbs);

	// edges connecting with the current component
	edge_set->Union(comp1->m_edge_set);
	edge_set->Union(comp2->m_edge_set);

	CExpressionArray *pdrgpexpr = GPOS_NEW(m_mp) CExpressionArray(m_mp);
	for (ULONG ul = 0; ul < m_ulEdges; ul++)
	{
		SEdge *pedge = m_rgpedge[ul];
		if (pedge->m_fUsed)
		{
			// edge is already used in result component
			continue;
		}

		if (pbs->ContainsAll(pedge->m_pbs))
		{
			// edge is subsumed by the cover of the combined component
			CExpression *pexpr = pedge->m_pexpr;
			pexpr->AddRef();
			pdrgpexpr->Append(pexpr);
		}
	}

	CExpression *pexprChild1 = comp1->m_pexpr;
	CExpression *pexprChild2 = comp2->m_pexpr;
	CExpression *pexprScalar = CPredicateUtils::PexprConjunction(m_mp, pdrgpexpr);

	CExpression *pexpr = NULL;
	INT parent_loj_id = NON_LOJ_DEFAULT_ID;
	EPosition position = EpSentinel;

	if (NULL == pexprChild1)
	{
		// first call to this function, we create a Select node
		parent_loj_id = comp2->ParentLojId();
		position = comp2->Position();
		pexpr = CUtils::PexprCollapseSelect(m_mp, pexprChild2, pexprScalar);
		pexprScalar->Release();
	}
	else
	{
		// not first call, we create an Inner Join or LOJ
		GPOS_ASSERT(NULL != pexprChild2);
		pexprChild2->AddRef();
		pexprChild1->AddRef();

		if (IsChildOfSameLOJ(comp1, comp2))
		{
			// if both the components are child of the same LOJ, ensure that the left child
			// stays left child and vice versa while creating the LOJ.
			// for this component we need not track the parent_loj_id, as we are only
			// concerned for joining LOJ childs with other non-LOJ components
			CExpression *pexprLeft = comp1->Position() == CJoinOrder::EpLeft ? pexprChild1: pexprChild2;
			CExpression *pexprRight = comp1->Position() == CJoinOrder::EpLeft ? pexprChild2: pexprChild1;
			pexpr = CUtils::PexprLogicalJoin<CLogicalLeftOuterJoin>(m_mp, pexprLeft, pexprRight, pexprScalar);
		}
		else
		{
			if (comp1->ParentLojId() > NON_LOJ_DEFAULT_ID || comp2->ParentLojId() > NON_LOJ_DEFAULT_ID)
			{
				// one of the component is the child of an LOJ, and can be joined with another relation
				// to create an Inner Join. for other non LOJ childs of NAry join, the parent loj id is
				// defaulted to 0, so assert the condition.
				GPOS_ASSERT_IMP(comp1->ParentLojId() > NON_LOJ_DEFAULT_ID, comp2->ParentLojId() == 0);
				GPOS_ASSERT_IMP(comp2->ParentLojId() > NON_LOJ_DEFAULT_ID, comp1->ParentLojId() == 0);

				parent_loj_id = NON_LOJ_DEFAULT_ID < comp1->ParentLojId() ? comp1->ParentLojId(): comp2->ParentLojId();
				position = NON_LOJ_DEFAULT_ID < comp1->ParentLojId()  ? comp1->Position(): comp2->Position();

				// since we only support joining the left child of LOJ to other relations in NAry Join,
				// we must not get right child of LOJ here, as that join combination must have been isolated
				// by IsValidJoinCombination earlier.
				GPOS_ASSERT(CJoinOrder::EpLeft == position);

				// we track if this join component contains the left child of LOJ,
				// so the parent loj id must be some positive non-zero number (i.e > 0)
				// for this join component
				GPOS_ASSERT(NON_LOJ_DEFAULT_ID < parent_loj_id);
			}
			pexpr = CUtils::PexprLogicalJoin<CLogicalInnerJoin>(m_mp, pexprChild1, pexprChild2, pexprScalar);
		}
	}
	// if the component has parent_loj_id > 0, it must be the left child or has the left child
	// of loj id indicated by parent_loj_id
	GPOS_ASSERT_IMP(NON_LOJ_DEFAULT_ID < parent_loj_id, EpLeft == position);
	SComponent *join_comp = GPOS_NEW(m_mp) SComponent(pexpr, pbs, edge_set, parent_loj_id, position);

	return join_comp;
}


//---------------------------------------------------------------------------
//	@function:
//		CJoinOrder::DeriveStats
//
//	@doc:
//		Helper function to derive stats on a given component
//
//---------------------------------------------------------------------------
void
CJoinOrder::DeriveStats
	(
	CExpression *pexpr
	)
{
	GPOS_ASSERT(NULL != pexpr);

	if (NULL != pexpr->Pstats())
	{
		// stats have been already derived
		return;
	}

	CExpressionHandle exprhdl(m_mp);
	exprhdl.Attach(pexpr);
	exprhdl.DeriveStats(m_mp, m_mp, NULL /*prprel*/, NULL /*pdrgpstatCtxt*/);
}


//---------------------------------------------------------------------------
//	@function:
//		CJoinOrder::OsPrint
//
//	@doc:
//		Helper function to print a join order class
//
//---------------------------------------------------------------------------
IOstream &
CJoinOrder::OsPrint
	(
	IOstream &os
	)
	const
{
	os	
		<< "Join Order: " << std::endl
		<< "Edges: " << m_ulEdges << std::endl;
		
	for (ULONG ul = 0; ul < m_ulEdges; ul++)
	{
		m_rgpedge[ul]->OsPrint(os);
		os << std::endl;
	}

	os << "Components: " << m_ulComps << std::endl;
	for (ULONG ul = 0; ul < m_ulComps; ul++)
	{
		os << (void*)m_rgpcomp[ul] << " - " << std::endl;
		m_rgpcomp[ul]->OsPrint(os);
	}
	
	return os;
}

BOOL
CJoinOrder::IsValidJoinCombination
	(
		SComponent *comp1,
		SComponent *comp2
	)
const
{
	INT comp1_parent_loj_id = comp1->ParentLojId();
	INT comp2_parent_loj_id = comp2->ParentLojId();
	EPosition comp1_position = comp1->Position();
	EPosition comp2_position = comp2->Position();
	

	// Consider the below tree, for examples used:
	//+--CLogicalNAryJoin
	//	|--CLogicalGet "t1"
	//	|--CLogicalGet "t2"
	//	|--CLogicalLeftOuterJoin => LOJ 1
	//	|  |--CLogicalGet "t3" => {1, EpLeft}
	//	|  |--CLogicalGet "t4" => {1, EpRight}
	//	|  +--<Join Condition>
	//	|--CLogicalLeftOuterJoin => LOJ 2
	//	|  |--CLogicalGet "t5" => {2, EpLeft}
	//	|  |--CLogicalGet "t6" => {2, EpRight}
	//	|  +--<Join Condition>
	//	+--<Join Condition>

	if ((NON_LOJ_DEFAULT_ID == comp1_parent_loj_id &&
		NON_LOJ_DEFAULT_ID == comp2_parent_loj_id))
	{
		// neither component contains any LOJs childs,
		// this is valid
		// example: CLogicalGet "t1" join CLogicalGet "t2"
		return true;
	}

	if (NON_LOJ_DEFAULT_ID < comp1_parent_loj_id &&
		NON_LOJ_DEFAULT_ID < comp2_parent_loj_id)
	{
		// both components contain LOJs child,
		// check whether they are referring to same LOJ
		if (comp1_parent_loj_id == comp2_parent_loj_id)
		{
			// one of them should be a left child and other one right child
			// example: CLogicalGet "t3" join CLogicalGet "t4" is valid
			GPOS_ASSERT(comp1_position != EpSentinel && comp2_position != EpSentinel);
			if ((comp1_position == EpLeft && comp2_position == EpRight) ||
				(comp1_position == EpRight && comp2_position == EpLeft))
			{
				return true;
			}
		}
		// two components with children from different LOJs, this is not valid
		// example: CLogicalGet "t3" join CLogicalGet "t5"
		return false;
	}

	// one of the components contains one LOJ child without the sibling,
	// this is allowed if it is a left LOJ child
	// example 1: CLogicalGet "t1" join CLogicalGet "t3" is valid
	// example 2: CLogicalGet "t1" join CLogicalGet "t4 is not valid
	return comp1_position != EpRight && comp2_position != EpRight;
}

BOOL
CJoinOrder::IsChildOfSameLOJ
	(
	SComponent *comp1,
	SComponent *comp2
	)
	const
{
	// check if these components are inner and outer children of a same join
	BOOL child_of_same_loj = comp1->ParentLojId() == comp2->ParentLojId() &&
							 comp1->ParentLojId() != NON_LOJ_DEFAULT_ID &&
							 ((comp1->Position() == CJoinOrder::EpLeft &&
								comp2->Position() == CJoinOrder::EpRight) ||
							 (comp1->Position() == CJoinOrder::EpRight &&
								comp2->Position() == CJoinOrder::EpLeft));
	
	return child_of_same_loj;
}

//---------------------------------------------------------------------------
//	@function:
//		CJoinOrder::MarkUsedEdges
//
//	@doc:
//		Mark edges used by result component
//
//---------------------------------------------------------------------------
void
CJoinOrder::MarkUsedEdges
	(
	SComponent *pcomponent
	)
{
	GPOS_ASSERT(NULL != pcomponent);

	CExpression *pexpr = pcomponent->m_pexpr;

	COperator::EOperatorId eopid = pexpr->Pop()->Eopid();
	if (0 == pexpr->Arity() ||
		(COperator::EopLogicalSelect != eopid &&
		 COperator::EopLogicalInnerJoin != eopid &&
		 COperator::EopLogicalLeftOuterJoin != eopid))
	{
		// result component does not have a scalar child, e.g. a Get node
		return;
	}

	CBitSetIter edges_iter(*(pcomponent->m_edge_set));

	while (edges_iter.Advance())
	{
		SEdge *pedge = m_rgpedge[edges_iter.Bit()];
		if (pedge->m_fUsed)
		{
			continue;
		}

		if (pcomponent->m_pbs->ContainsAll(pedge->m_pbs))
		{
			pedge->m_fUsed = true;
		}
	}
}

void
CJoinOrder::AddComponent
	(
	IMemoryPool *mp,
	CExpression *expr,
	INT loj_id,
	EPosition position,
	INT comp_num
	)
{
	expr->AddRef();
	SComponent *comp = GPOS_NEW(mp) SComponent(
											   mp,
											   expr,
											   loj_id,
											   position
											   );
	m_rgpcomp[comp_num] = comp;
	// component always covers itself
	(void) m_rgpcomp[comp_num]->m_pbs->ExchangeSet(comp_num);
}
// EOF