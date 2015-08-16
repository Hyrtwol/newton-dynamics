/* Copyright (c) <2003-2011> <Julio Jerez, Newton Game Dynamics>
* 
* This software is provided 'as-is', without any express or implied
* warranty. In no event will the authors be held liable for any damages
* arising from the use of this software.
* 
* Permission is granted to anyone to use this software for any purpose,
* including commercial applications, and to alter it and redistribute it
* freely, subject to the following restrictions:
* 
* 1. The origin of this software must not be misrepresented; you must not
* claim that you wrote the original software. If you use this software
* in a product, an acknowledgment in the product documentation would be
* appreciated but is not required.
* 
* 2. Altered source versions must be plainly marked as such, and must not be
* misrepresented as being the original software.
* 
* 3. This notice may not be removed or altered from any source distribution.
*/

#include "dgPhysicsStdafx.h"
#include "dgBody.h"
#include "dgWorld.h"
#include "dgConstraint.h"
#include "dgDynamicBody.h"
#include "dgSkeletonContainer.h"
#include "dgWorldDynamicUpdate.h"
#include "dgBilateralConstraint.h"


#define DG_SKELETON_STACK_SIZE		512


//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////// 
dgInt32 dgSkeletonContainer::m_uniqueID = DG_SKELETON_BASEW_UNIQUE_ID;

DG_MSC_VECTOR_ALIGMENT
class dgSkeletonContainer::dgSkeletonGraph
{
	public:
	DG_CLASS_ALLOCATOR(allocator)
	dgSkeletonGraph (dgDynamicBody* const body, dgBilateralConstraint* const Joint, dgSkeletonGraph* const parent)
		:m_parent(parent)
		,m_body (body)
		,m_joint (Joint)
		,m_child(NULL)
		,m_sibling(NULL)
		,m_index(0)
		,m_dof(0)
	{
		m_bodyMass.SetZero();
		m_jointMass.SetZero();
		m_bodyInvMass.SetZero();
		m_jointInvMass.SetZero();
		m_jointJ.SetZero();
		m_bodyJt.SetZero();
		m_bodyForce.SetZero();
		m_jointForce.SetZero();

		if (m_parent) {
			if (m_parent->m_child) {
				m_sibling = m_parent->m_child;
			}
			m_parent->m_child = this;
		}
	}

	DG_INLINE ~dgSkeletonGraph()
	{
		dgSkeletonGraph* next;
		m_body->SetSkeleton(NULL);

		for (dgSkeletonGraph* ptr = m_child; ptr; ptr = next) {
			next = ptr->m_sibling;
			delete ptr;
		}
	}

	DG_INLINE void SetPriority(dgUnsigned32 priority) const
	{
		if (m_joint) {
			m_joint->m_priority = priority;
		}
	}

	DG_INLINE void Factorize()
	{
		if (m_body->GetInvMass().m_w != dgFloat32(0.0f)) {
			for (dgSkeletonGraph* child = m_child; child; child = child->m_sibling) {
				CalculateBodyDiagonal(child);
			}
			m_bodyMass.Inverse(m_bodyInvMass, 6);
		} else {
			m_bodyInvMass.SetZero();
		}

		if (m_joint) {
			dgAssert(m_parent);
			for (dgInt32 i = 0; i < m_dof; i++) {
				m_bodyInvMass.MultiplyNxNMatrixTimeVector(m_bodyJt[i], m_bodyJt[i], 6);
			}
			CalculateJointDiagonal();
			CalculateJacobianBlock();
		}
	}

	DG_INLINE void GetInertia()
	{
            dgFloat32 mass = m_body->GetMass().m_w;
		    dgAssert(mass < dgFloat32(1.0e10f));
		    dgMatrix inertia(m_body->CalculateInertiaMatrix());
		    for (dgInt32 i = 0; i < 3; i++) {
			    m_bodyMass[i][i] = mass;
			    for (dgInt32 j = 0; j < 3; j++) {
				    m_bodyMass[i + 3][j + 3] = inertia[i][j];
			    }
		    }
		}

	DG_INLINE void GetJacobians(const dgJointInfo* const jointInfo, const dgJacobianMatrixElement* const matrixRow)
	{
		dgAssert(m_parent);
		dgAssert(jointInfo->m_joint == m_joint);
		dgAssert(jointInfo->m_joint->GetBody0() == m_body);
		dgAssert(jointInfo->m_joint->GetBody1() == m_parent->m_body);

		for (dgInt32 i = 0; i < m_dof; i++) {
			const dgJacobianMatrixElement* const row = &matrixRow[jointInfo->m_pairStart + m_sourceJacobianIndex[i]];
			m_jointMass[i].SetZero();
			//m_jointMass[i][i] = 0.0f;
			m_jointMass[i][i] = -row->m_diagDamp;
			for (dgInt32 j = 0; j < 3; j++) {
				m_bodyJt[i][j + 0] = row->m_Jt.m_jacobianM0.m_linear[j];
				m_bodyJt[i][j + 3] = row->m_Jt.m_jacobianM0.m_angular[j];
				m_jointJ[i][j + 0] = row->m_Jt.m_jacobianM1.m_linear[j];
				m_jointJ[i][j + 3] = row->m_Jt.m_jacobianM1.m_angular[j];
			}
		}
	}

	DG_INLINE void Factorize(const dgJointInfo* const jointInfoArray, dgJacobian* const internalForces, dgJacobianMatrixElement* const matrixRow)
	{
		dgAssert((dgUnsigned64(&m_bodyMass) & 0x0f) == 0);

		m_bodyMass.SetZero();
        if (m_body->GetInvMass().m_w != dgFloat32 (0.0f)) {
			GetInertia();
		}

		if (m_joint) {
			dgAssert (m_parent);
			const dgJointInfo* const jointInfo = &jointInfoArray[m_joint->m_index];
			dgAssert(jointInfo->m_joint == m_joint);
			dgAssert(jointInfo->m_joint->GetBody0() == m_body);
			dgAssert(jointInfo->m_joint->GetBody1() == m_parent->m_body);

			const dgInt32 m0 = jointInfo->m_m0;
			const dgInt32 m1 = jointInfo->m_m1;
			dgJacobian& y0 = internalForces[m0];
			dgJacobian& y1 = internalForces[m1];

			m_dof = 0;
			const dgInt32 count = jointInfo->m_pairCount;
			const dgInt32 first = jointInfo->m_pairStart;
			for (dgInt32 i = 0; i < count; i++) {
				dgJacobianMatrixElement* const row = &matrixRow[i + first];
				dgVector residual(row->m_JMinv.m_jacobianM0.m_linear.CompProduct4(y0.m_linear) + row->m_JMinv.m_jacobianM0.m_angular.CompProduct4(y0.m_angular) +
								  row->m_JMinv.m_jacobianM1.m_linear.CompProduct4(y1.m_linear) + row->m_JMinv.m_jacobianM1.m_angular.CompProduct4(y1.m_angular));
				residual = dgVector(row->m_coordenateAccel) - residual.AddHorizontal();
				dgVector force(row->m_force + row->m_invJMinvJt * residual.GetScalar());
				dgVector lowerFrictionForce(row->m_lowerBoundFrictionCoefficent);
				dgVector upperFrictionForce(row->m_upperBoundFrictionCoefficent);
				dgFloat32 val = (dgVector::m_one & ((force > upperFrictionForce) | (force < lowerFrictionForce))).GetScalar();
				m_sourceJacobianIndex[m_dof] = dgInt8(i);
				m_dof ++;
				if (val) {
					val = force.GetMax(lowerFrictionForce).GetMin(upperFrictionForce).GetScalar();
					dgFloat32 deltaForce = val - row->m_force;
					row->m_force = val;
					if (dgAbsf(val - row->m_force) > dgFloat32 (1.0e-5f)) { 
						y0.m_linear += row->m_Jt.m_jacobianM0.m_linear.CompProduct4(deltaForce);
						y0.m_angular += row->m_Jt.m_jacobianM0.m_angular.CompProduct4(deltaForce);
						y1.m_linear += row->m_Jt.m_jacobianM1.m_linear.CompProduct4(deltaForce);
						y1.m_angular += row->m_Jt.m_jacobianM1.m_angular.CompProduct4(deltaForce);
					}
					m_dof--;
				}
			}
			GetJacobians(jointInfo, matrixRow);
		}

		Factorize();
	}


	DG_INLINE void RebuildMassMatrix(const dgJointInfo* const jointInfoArray, const dgJacobian* const internalForces, const dgJacobianMatrixElement* const matrixRow)
	{
		dgAssert((dgUnsigned64(&m_bodyMass) & 0x0f) == 0);

		m_bodyMass.SetZero();
		if (m_body->GetInvMass().m_w != dgFloat32(0.0f)) {
			GetInertia();
		}

		if (m_joint) {
			dgAssert(m_parent);
			GetJacobians(&jointInfoArray[m_joint->m_index], matrixRow);
		}

		Factorize();
	}


	DG_INLINE void CalculateBodyDiagonal(dgSkeletonGraph* const child)
	{
		dgAssert(child->m_joint);
		
		dgSpatialMatrix copy;
		copy.SetZero();
		const dgInt32 dof = child->m_dof;
		const dgSpatialMatrix& jacobianMatrix = child->m_jointJ;
		const dgSpatialMatrix& childDiagonal = child->m_jointMass;
		for (dgInt32 i = 0; i < dof ; i++) {
			const dgSpatialVector& jacobian = jacobianMatrix[i];
			for (dgInt32 j = 0; j < dof ; j++) {
				dgAssert(dgAreEqual (childDiagonal[i][j], childDiagonal[j][i], dgFloat64(1.0e-5f)));
				dgFloat64 val = childDiagonal[i][j];
				jacobian.ScaleAdd(val, copy[j], copy[j]);
			}
		}

		for (dgInt32 i = 0; i < dof; i++) {
			const dgSpatialVector& Jacobian = copy[i];
			const dgSpatialVector& JacobianTranspose = jacobianMatrix[i];
			for (dgInt32 j = 0; j < 6; j++) {
				dgFloat64 val = -Jacobian[j];
				JacobianTranspose.ScaleAdd(val, m_bodyMass[j], m_bodyMass[j]);
			}
		}
	}

	DG_INLINE void CalculateJointDiagonal ()
	{
		dgSpatialMatrix tmp;
		for (dgInt32 i = 0; i < m_dof; i++) {
			m_bodyMass.MultiplyNxNMatrixTimeVector(m_bodyJt[i], tmp[i], 6);
		}

		for (dgInt32 i = 0; i < m_dof; i++) {
			dgFloat64 a = m_bodyJt[i].DotProduct(tmp[i]);
			m_jointMass[i][i] -= a;
			for (dgInt32 j = i + 1; j < m_dof; j++) {
				a = - m_bodyJt[i].DotProduct(tmp[j]);
				m_jointMass[i][j] = a;
				m_jointMass[j][i] = a;
			}
		}
		m_jointMass.Inverse(m_jointInvMass, m_dof);
	}


	DG_INLINE void CalculateJacobianBlock()
	{
		dgSpatialMatrix copy;
		for (dgInt32 i = 0; i < m_dof; i++) {
			copy[i] = m_jointJ[i];
			m_jointJ[i].SetZero();
		}

		for (dgInt32 i = 0; i < m_dof; i++) {
			const dgSpatialVector& jacobian = copy[i];
			const dgSpatialVector& invDiagonalRow = m_jointInvMass[i];
			for (dgInt32 j = 0; j < m_dof; j++) {
				dgFloat64 val = invDiagonalRow[j];
				jacobian.ScaleAdd(val, m_jointJ[j], m_jointJ[j]);
			}
		}
	}


	DG_INLINE void JointJacobianTimeMassForward ()
	{
		for (dgInt32 i = 0; i < m_dof; i++) {
			m_jointForce[i] -= m_bodyJt[i].DotProduct(m_bodyForce);
		}
	}


	DG_INLINE void BodyJacobianTimeMassForward() const 
	{
		for (dgInt32 i = 0; i < m_dof; i++) {
			m_jointJ[i].ScaleAdd(-m_jointForce[i], m_parent->m_bodyForce, m_parent->m_bodyForce);
		}
	}

	DG_INLINE void JointJacobianTimeSolutionBackward()
	{
		const dgSpatialVector& force = m_parent->m_bodyForce;
		for (dgInt32 i = 0; i < m_dof; i++) {
			m_jointForce[i] -= force.DotProduct(m_jointJ[i]);
		}
	}

	DG_INLINE void BodyJacobianTimeSolutionBackward()
	{
		for (dgInt32 i = 0; i < m_dof; i++) {
			m_bodyJt[i].ScaleAdd(-m_jointForce[i], m_bodyForce, m_bodyForce);
		}
	}


	DG_INLINE void BodyDiagInvTimeSolution()
	{
		m_bodyInvMass.MultiplyNxNMatrixTimeVector(m_bodyForce, m_bodyForce, 6);
	}

	DG_INLINE void JointDiagInvTimeSolution()
	{
		m_jointInvMass.MultiplyNxNMatrixTimeVector (m_jointForce, m_jointForce, m_dof);
	}

	dgSpatialMatrix m_bodyMass;
	dgSpatialMatrix m_jointMass;
	dgSpatialMatrix m_bodyInvMass;
	dgSpatialMatrix m_jointInvMass;
	dgSpatialMatrix m_jointJ;
	dgSpatialMatrix m_bodyJt;
	dgSpatialVector m_bodyForce;
	dgSpatialVector m_jointForce;
	dgDynamicBody* m_body;
	dgBilateralConstraint* m_joint;
	dgSkeletonGraph* m_parent;
	dgSkeletonGraph* m_child;
	dgSkeletonGraph* m_sibling;
	dgInt16 m_index;
	dgInt16 m_dof;
	dgInt8 m_sourceJacobianIndex[6];
} DG_GCC_VECTOR_ALIGMENT;


dgSkeletonContainer::dgSkeletonContainer(dgWorld* const world, dgDynamicBody* const rootBody)
	:m_world(world)
	,m_skeleton(new (rootBody->GetWorld()->GetAllocator()) dgSkeletonGraph(rootBody, NULL, NULL))
	,m_nodesOrder(NULL)
	,m_destructor(NULL)
	,m_id(m_uniqueID)
	,m_nodeCount(1)
{
	rootBody->SetSkeleton(this);
	m_uniqueID++;
}

dgSkeletonContainer::~dgSkeletonContainer()
{
	if (m_destructor) {
		m_destructor (this);
	}
	
	dgMemoryAllocator* const allocator = m_world->GetAllocator();
	if (m_nodesOrder) {
		allocator->Free(m_nodesOrder);
	}
	delete m_skeleton;
}

dgWorld* dgSkeletonContainer::GetWorld() const
{
	return m_world;
}

void dgSkeletonContainer::SetDestructorCallback (dgOnSkeletonContainerDestroyCallback destructor)
{
	m_destructor = destructor;
}

void dgSkeletonContainer::ResetUniqueId(dgInt32 id)
{
	m_uniqueID = id;
}

void dgSkeletonContainer::SortGraph(dgSkeletonGraph* const root, dgSkeletonGraph* const parent, dgInt32& index)
{
	for (dgSkeletonGraph* node = root->m_child; node; node = node->m_sibling) {
		SortGraph(node, root, index);
	}

	root->SetPriority((m_id << DG_SKELETON_BIT_SHIFT_KEY) + index);
	dgAssert((m_nodeCount - index - 1) >= 0);
	m_nodesOrder[index] = root;
	root->m_index = dgInt16(index);
	index++;
	dgAssert(index <= m_nodeCount);
}

dgSkeletonContainer::dgSkeletonGraph* dgSkeletonContainer::FindNode(dgDynamicBody* const body) const
{
	dgInt32 stack = 1;
	dgSkeletonGraph* stackPool[DG_SKELETON_STACK_SIZE];

	stackPool[0] = m_skeleton;
	while (stack) {
		stack--;
		dgSkeletonGraph* const node = stackPool[stack];
		if (node->m_body == body) {
			return node;
		}

		for (dgSkeletonGraph* ptr = node->m_child; ptr; ptr = ptr->m_sibling) {
			stackPool[stack] = ptr;
			stack++;
			dgAssert(stack < dgInt32(sizeof (stackPool) / sizeof (stackPool[0])));
		}
	}
	return NULL;
}

dgSkeletonContainer::dgSkeletonGraph* dgSkeletonContainer::AddChild(dgBody* const child, dgBody* const parent)
{
	dgAssert(child);
	dgBody* const parentBody = parent ? parent : m_skeleton->m_body;
	dgAssert(parentBody);
	dgAssert(child->GetType() == dgBody::m_dynamicBody);
	dgAssert(parentBody->GetType() == dgBody::m_dynamicBody);
	return AddChild((dgDynamicBody*)child, (dgDynamicBody*)parentBody);
}

dgSkeletonContainer::dgSkeletonGraph* dgSkeletonContainer::AddChild(dgDynamicBody* const child, dgDynamicBody* const parent)
{
	dgAssert (m_skeleton->m_body);
	dgBilateralConstraint* const joint = m_world->FindBilateralJoint(child, parent);
	dgAssert(joint);

	dgSkeletonGraph* node = NULL;
	dgMemoryAllocator* const allocator = m_world->GetAllocator();
	if ((joint->GetBody0() == child) && (joint->GetBody1() == parent)) {
		dgSkeletonGraph* const parentNode = FindNode(parent);
		dgAssert(parentNode);
		node = new (allocator)dgSkeletonGraph(child, joint, parentNode);
	} else {
		dgAssert(joint->GetBody1() == child);
		dgAssert(joint->GetBody0() == parent);
		dgAssert (m_skeleton->m_body == parent);
		dgAssert (m_skeleton->m_joint == NULL);
		dgAssert (m_skeleton->m_sibling == NULL);
		m_skeleton->m_joint = joint;
		node = new (allocator) dgSkeletonGraph (child, NULL, NULL);
		node->m_child = m_skeleton;
		m_skeleton->m_parent = node;
		dgSwap (m_skeleton, node);
	}

	dgAssert(node->m_joint->GetBody0() == node->m_body);
	dgAssert(node->m_joint->GetBody1() == node->m_parent->m_body);
	m_nodeCount ++;

	dgAssert (child->GetWorld()->GetSentinelBody() != child);
	child->SetSkeleton(this);
	
	return node;
}


void dgSkeletonContainer::AddJointList (dgInt32 count, dgBilateralConstraint** const array)
{
	dgTree<dgBody*, dgBody*> filter(m_world->GetAllocator());
	dgTree<dgConstraint*, dgConstraint*> jointMap(m_world->GetAllocator());

	dgInt32 stack = 0;
	dgBody* pool[1024][2];
	filter.Insert(m_skeleton->m_body, m_skeleton->m_body);
	for (dgInt32 i = 0; i < count; i++) {
		dgBilateralConstraint* const joint = array[i];
		jointMap.Insert(joint, joint);

		dgBody* const body0 = joint->GetBody0();
		dgBody* const body1 = joint->GetBody1();
		if (body1 == m_skeleton->m_body) {
			pool[stack][0] = joint->GetBody0();
			pool[stack][1] = joint->GetBody1();
			filter.Insert(pool[stack][0], pool[stack][0]);
			stack++;
		} else if (body0 == m_skeleton->m_body) {
			pool[stack][0] = joint->GetBody1();
			pool[stack][1] = joint->GetBody0();
			filter.Insert(pool[stack][0], pool[stack][0]);
			stack++;
		}
	}

	while (stack) {
		stack--;
		dgBody* const child = pool[stack][0];
		dgBody* const parent = pool[stack][1];
		AddChild(child, parent);

		for (dgConstraint* joint = child->GetFirstJoint(); joint; joint = child->GetNextJoint(joint)) {
			dgAssert(joint->IsBilateral());
			if (jointMap.Find(joint)) {
				dgBody* const body = (joint->GetBody0() != child) ? joint->GetBody0() : joint->GetBody1();
				if (!filter.Find(body)) {
					pool[stack][0] = body;
					pool[stack][1] = child;
					stack++;
					filter.Insert(body, body);
					dgAssert(stack < sizeof (pool) / (2 * sizeof (pool[0][0])));
				}
			}
		}
	}
}


void dgSkeletonContainer::Finalize()
{
	dgAssert(m_nodeCount >= 1);

	dgMemoryAllocator* const allocator = m_skeleton->m_body->GetWorld()->GetAllocator();
	m_nodesOrder = (dgSkeletonGraph**)allocator->Malloc(m_nodeCount * sizeof (dgSkeletonGraph*));

	dgInt32 index = 0;
	SortGraph(m_skeleton, NULL, index);
	dgAssert(index == m_nodeCount);
}


void dgSkeletonContainer::InitMassMatrix (const dgJointInfo* const jointInfoArray, dgJacobian* const internalForces, dgJacobianMatrixElement* const matrixRow)
{
	for (dgInt32 i = 0; i < m_nodeCount; i ++) {
		m_nodesOrder[i]->Factorize(jointInfoArray, internalForces, matrixRow);
	}
}

void dgSkeletonContainer::RebuildMassMatrix(dgInt32 start, const dgJointInfo* const jointInfoArray, const dgJacobian* const internalForces, const dgJacobianMatrixElement* const matrixRow)
{
	for (dgInt32 i = start; i < m_nodeCount; i++) {
		m_nodesOrder[i]->RebuildMassMatrix(jointInfoArray, internalForces, matrixRow);
	}
}


dgFloat32 dgSkeletonContainer::SolveUnilaterals (dgJointInfo* const jointInfoArray, const dgBodyInfo* const bodyArray, dgJacobian* const internalForces, dgJacobianMatrixElement* const matrixRow) const
{
	dgFloat32 retAccel = dgFloat32(0.0f);
	const dgWorldDynamicUpdate& dynamicsUpdate = *m_world;
	for (dgInt32 i = m_nodeCount - 2; i >= 0; i--) {
		dgSkeletonGraph* const node = m_nodesOrder[i];
		dgAssert(node->m_joint);
		const dgJointInfo* const jointInfo = &jointInfoArray[node->m_joint->m_index];
		dgAssert(jointInfo->m_joint == node->m_joint);
		const dgInt32 count = node->m_dof;
		if (count < jointInfo->m_pairCount) {
			dgJointInfo info(*jointInfo);
			info.m_pairStart += count;
			info.m_pairCount = jointInfo->m_pairCount - dgInt16(count);
			dgFloat32 accel = dynamicsUpdate.CalculateJointForce(&info, bodyArray, internalForces, matrixRow);
			dgAssert (accel >= dgFloat32 (0.0f));
			retAccel = dgMax( accel,  retAccel);
		}
	}
	return retAccel;
}

void dgSkeletonContainer::SolveFoward () const
{
	for (dgInt32 i = 0; i < m_nodeCount - 1; i++) {
		dgSkeletonGraph* const node = m_nodesOrder[i];
		for (dgSkeletonGraph* child = node->m_child; child; child = child->m_sibling) {
			dgAssert(child->m_joint);
			child->BodyJacobianTimeMassForward();
		}
		dgAssert(node->m_joint);
		node->JointJacobianTimeMassForward();
	}

	for (dgSkeletonGraph* child = m_nodesOrder[m_nodeCount - 1]->m_child; child; child = child->m_sibling) {
		child->BodyJacobianTimeMassForward();
	}
}

void dgSkeletonContainer::SolveBackward () const
{
	m_nodesOrder[m_nodeCount - 1]->BodyDiagInvTimeSolution();
	for (dgInt32 i = m_nodeCount - 2; i >= 0; i--) {
		dgSkeletonGraph* const node = m_nodesOrder[i];
		node->JointDiagInvTimeSolution();
		node->JointJacobianTimeSolutionBackward();
		node->BodyDiagInvTimeSolution();
		node->BodyJacobianTimeSolutionBackward();
	}
}


dgFloat32 dgSkeletonContainer::CalculateJointAccel(dgJointInfo* const jointInfoArray, dgJacobian* const internalForces, dgJacobianMatrixElement* const matrixRow) const
{
	dgVector accNorm (dgFloat32(0.0f));
	for (dgInt32 i = 0; i < m_nodeCount - 1; i++) {
		dgSkeletonGraph* const node = m_nodesOrder[i];
		dgAssert(node->m_body);
		node->m_bodyForce.SetZero();

		dgAssert(node->m_joint);
		const dgJointInfo* const jointInfo = &jointInfoArray[node->m_joint->m_index];
		dgAssert(jointInfo->m_joint == node->m_joint);
		const dgInt32 first = jointInfo->m_pairStart;
		const dgInt32 count = node->m_dof;
		const dgInt32 m0 = jointInfo->m_m0;
		const dgInt32 m1 = jointInfo->m_m1;
		const dgJacobian& y0 = internalForces[m0];
		const dgJacobian& y1 = internalForces[m1];

		dgSpatialVector& accel = node->m_jointForce;
		for (dgInt32 j = 0; j < count; j++) {
			dgJacobianMatrixElement* const row = &matrixRow[first + node->m_sourceJacobianIndex[j]];
			dgVector acc(row->m_JMinv.m_jacobianM0.m_linear.CompProduct4(y0.m_linear) + row->m_JMinv.m_jacobianM0.m_angular.CompProduct4(y0.m_angular) +
						 row->m_JMinv.m_jacobianM1.m_linear.CompProduct4(y1.m_linear) + row->m_JMinv.m_jacobianM1.m_angular.CompProduct4(y1.m_angular));
			acc = dgVector(row->m_coordenateAccel) - acc.AddHorizontal();
			dgVector force(row->m_force + row->m_invJMinvJt * acc.m_x);
			dgVector lowerFrictionForce(row->m_lowerBoundFrictionCoefficent);
			dgVector upperFrictionForce(row->m_upperBoundFrictionCoefficent);
			accel[j] = -acc.GetScalar();
			acc = acc.AndNot((force > upperFrictionForce) | (force < lowerFrictionForce));
			//accNorm += acc.Abs().GetScalar();
			accNorm = accNorm.GetMax(acc.Abs());
		}
	}
	m_nodesOrder[m_nodeCount - 1]->m_bodyForce.SetZero();
	m_nodesOrder[m_nodeCount - 1]->m_jointForce.SetZero();
	return accNorm.GetScalar();
}


dgInt32 dgSkeletonContainer::UpdateForces (dgJointInfo* const jointInfoArray, dgJacobian* const internalForces, dgJacobianMatrixElement* const matrixRow) const
{
	dgInt32 start = m_nodeCount;
	for (dgInt32 i = 0; i < (m_nodeCount - 1)  ; i ++) {
		dgSkeletonGraph* const node = m_nodesOrder[i];
		const dgJointInfo* const jointInfo = &jointInfoArray[node->m_joint->m_index];

		dgJacobian y0;
		dgJacobian y1;
		y0.m_linear = dgVector::m_zero;
		y0.m_angular = dgVector::m_zero;
		y1.m_linear = dgVector::m_zero;
		y1.m_angular = dgVector::m_zero;

		dgAssert(jointInfo->m_joint == node->m_joint);
		const dgInt32 first = jointInfo->m_pairStart;
		dgSpatialVector& force = node->m_jointForce;

		dgInt32 dof = 0;
		for (dgInt32 j = 0; j < node->m_dof; j ++) {
			dgInt32 k = node->m_sourceJacobianIndex[j];
			dgJacobianMatrixElement* const row = &matrixRow[first + k];
			dgVector jointForce (dgFloat32 (force[j]) + row->m_force);
			dgVector lowerFrictionForce(row->m_lowerBoundFrictionCoefficent);
			dgVector upperFrictionForce(row->m_upperBoundFrictionCoefficent);
			bool isClapmed = (dgVector::m_one & ((jointForce > upperFrictionForce) | (jointForce < lowerFrictionForce))).GetScalar() ? true : false;
			jointForce = jointForce.GetMax(lowerFrictionForce).GetMin(upperFrictionForce).GetScalar();
			dgVector val (jointForce - dgVector(row->m_force));
			row->m_force = jointForce.GetScalar();
			dgAssert ((row->m_force >= row->m_lowerBoundFrictionCoefficent) && (row->m_force <= row->m_upperBoundFrictionCoefficent));
			y0.m_linear += row->m_Jt.m_jacobianM0.m_linear.CompProduct4(val);
			y0.m_angular += row->m_Jt.m_jacobianM0.m_angular.CompProduct4(val);
			y1.m_linear += row->m_Jt.m_jacobianM1.m_linear.CompProduct4(val);
			y1.m_angular += row->m_Jt.m_jacobianM1.m_angular.CompProduct4(val);
			node->m_sourceJacobianIndex[dof] = dgInt8 (k);
			dof ++;
			if (isClapmed) {
				dof --;
				start = dgMin (start, i);
			}
		}
		node->m_dof = dgInt16(dof);
		const dgInt32 m0 = jointInfo->m_m0;
		const dgInt32 m1 = jointInfo->m_m1;

		internalForces[m0].m_linear += y0.m_linear;
		internalForces[m0].m_angular += y0.m_angular;
		internalForces[m1].m_linear += y1.m_linear;
		internalForces[m1].m_angular += y1.m_angular;
	}
	return start;
}


dgFloat32 dgSkeletonContainer::CalculateJointForce (dgJointInfo* const jointInfoArray, const dgBodyInfo* const bodyArray, dgJacobian* const internalForces, dgJacobianMatrixElement* const matrixRow)
{
//static int xxx;
//xxx ++;
	dgFloat32 retAccel = CalculateJointAccel(jointInfoArray, internalForces, matrixRow);
	dgFloat32 jointAccel = retAccel;
	for (int i = 0; (i < 8) && (jointAccel > dgFloat32 (5.0e-2f)); i ++) {
		SolveFoward ();
		SolveBackward ();
		dgInt32 start = UpdateForces(jointInfoArray, internalForces, matrixRow);
		if (start < m_nodeCount) {
			RebuildMassMatrix (start, jointInfoArray, internalForces, matrixRow);
		}
		jointAccel = CalculateJointAccel(jointInfoArray, internalForces, matrixRow);
//dgTrace (("%f ", jointAccel));
	}
//dgTrace (("\n"));
	
	return retAccel;
}