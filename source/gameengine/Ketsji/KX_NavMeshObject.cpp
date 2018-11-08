/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

#include "MEM_guardedalloc.h"

#include "BLI_math_vector.h"
#include "KX_NavMeshObject.h"
#include "KX_Mesh.h"
#include "RAS_DisplayArray.h"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"

extern "C" {
#  include "BKE_scene.h"
#  include "BKE_customdata.h"
#  include "BKE_cdderivedmesh.h"
#  include "BKE_DerivedMesh.h"
#  include "BKE_navmesh_conversion.h"

#  include "BLI_alloca.h"
}

#include "KX_Globals.h"
#include "KX_PyMath.h"
#include "EXP_Value.h"
#include "Recast.h"
#include "DetourStatNavMeshBuilder.h"
#include "KX_ObstacleSimulation.h"

#include "CM_Message.h"

#define MAX_PATH_LEN 256
static const float polyPickExt[3] = {2, 4, 2};

static void calcMeshBounds(const float *vert, int nverts, float *bmin, float *bmax)
{
	bmin[0] = bmax[0] = vert[0];
	bmin[1] = bmax[1] = vert[1];
	bmin[2] = bmax[2] = vert[2];
	for (int i = 1; i < nverts; i++) {
		if (bmin[0] > vert[3 * i + 0]) {
			bmin[0] = vert[3 * i + 0];
		}
		if (bmin[1] > vert[3 * i + 1]) {
			bmin[1] = vert[3 * i + 1];
		}
		if (bmin[2] > vert[3 * i + 2]) {
			bmin[2] = vert[3 * i + 2];
		}

		if (bmax[0] < vert[3 * i + 0]) {
			bmax[0] = vert[3 * i + 0];
		}
		if (bmax[1] < vert[3 * i + 1]) {
			bmax[1] = vert[3 * i + 1];
		}
		if (bmax[2] < vert[3 * i + 2]) {
			bmax[2] = vert[3 * i + 2];
		}
	}
}

inline void flipAxes(mt::vec3& vec)
{
	std::swap(vec.y, vec.z);
}

inline void flipAxes(float vec[3])
{
	std::swap(vec[1], vec[2]);
}

KX_NavMeshObject::KX_NavMeshObject(void *sgReplicationInfo, SG_Callbacks callbacks)
	:KX_GameObject(sgReplicationInfo, callbacks),
	m_navMesh(nullptr)
{
}

KX_NavMeshObject::~KX_NavMeshObject()
{
	if (m_navMesh) {
		delete m_navMesh;
	}
}

EXP_Value *KX_NavMeshObject::GetReplica()
{
	KX_NavMeshObject *replica = new KX_NavMeshObject(*this);
	replica->ProcessReplica();
	return replica;
}

void KX_NavMeshObject::ProcessReplica()
{
	KX_GameObject::ProcessReplica();
	m_navMesh = nullptr;
}

int KX_NavMeshObject::GetGameObjectType() const
{
	return OBJ_NAVMESH;
}

bool KX_NavMeshObject::BuildFromDerivedMesh(float *&vertices, int& nverts,
                                          unsigned short * &polys, int& npolys, unsigned short *&dmeshes,
                                          float *&dvertices, int &ndvertsuniq, unsigned short *&dtris,
                                          int& ndtris, int &vertsPerPoly)
{
	KX_Mesh *meshobj = m_meshes.front();
	if (!meshobj->GetMesh()) {
		return false;
	}

	DerivedMesh *dm = CDDM_from_mesh(meshobj->GetMesh());
	CustomData *pdata = dm->getPolyDataLayout(dm);
	int *recastData = (int *)CustomData_get_layer(pdata, CD_RECAST);
	if (!recastData) {
		dm->release(dm);
		return false;
	}

	int *dtrisToPolysMap = nullptr, *dtrisToTrisMap = nullptr, *trisToFacesMap = nullptr;
	int nAllVerts = 0;
	float *allVerts = nullptr;
	buildNavMeshDataByDerivedMesh(dm, &vertsPerPoly, &nAllVerts, &allVerts, &ndtris, &dtris,
	                              &npolys, &dmeshes, &polys, &dtrisToPolysMap, &dtrisToTrisMap, &trisToFacesMap);

	MEM_SAFE_FREE(dtrisToPolysMap);
	MEM_SAFE_FREE(dtrisToTrisMap);
	MEM_SAFE_FREE(trisToFacesMap);

	unsigned short *verticesMap = (unsigned short *)MEM_mallocN(sizeof(*verticesMap) * nAllVerts, __func__);
	memset(verticesMap, 0xff, sizeof(*verticesMap) * nAllVerts);
	int curIdx = 0;
	//vertices - mesh verts
	//iterate over all polys and create map for their vertices first...
	for (int polyidx = 0; polyidx < npolys; polyidx++) {
		unsigned short *poly = &polys[polyidx * vertsPerPoly * 2];
		for (int i = 0; i < vertsPerPoly; i++) {
			unsigned short idx = poly[i];
			if (idx == 0xffff) {
				break;
			}
			if (verticesMap[idx] == 0xffff) {
				verticesMap[idx] = curIdx++;
			}
			poly[i] = verticesMap[idx];
		}
	}
	nverts = curIdx;
	//...then iterate over detailed meshes
	//transform indices to local ones (for each navigation polygon)
	for (int polyidx = 0; polyidx < npolys; polyidx++) {
		unsigned short *poly = &polys[polyidx * vertsPerPoly * 2];
		int nv = polyNumVerts(poly, vertsPerPoly);
		unsigned short *dmesh = &dmeshes[4 * polyidx];
		unsigned short tribase = dmesh[2];
		unsigned short trinum = dmesh[3];
		unsigned short vbase = curIdx;
		for (int j = 0; j < trinum; j++) {
			unsigned short *dtri = &dtris[(tribase + j) * 3 * 2];
			for (int k = 0; k < 3; k++) {
				int newVertexIdx = verticesMap[dtri[k]];
				if (newVertexIdx == 0xffff) {
					newVertexIdx = curIdx++;
					verticesMap[dtri[k]] = newVertexIdx;
				}

				if (newVertexIdx < nverts) {
					//it's polygon vertex ("shared")
					int idxInPoly = polyFindVertex(poly, vertsPerPoly, newVertexIdx);
					if (idxInPoly == -1) {
						CM_Error("building NavMeshObject, can't find vertex in polygon\n");
						return false;
					}
					dtri[k] = idxInPoly;
				}
				else {
					dtri[k] = newVertexIdx - vbase + nv;
				}
			}
		}
		dmesh[0] = vbase - nverts; //verts base
		dmesh[1] = curIdx - vbase; //verts num
	}

	vertices = new float[nverts * 3];
	ndvertsuniq = curIdx - nverts;
	if (ndvertsuniq > 0) {
		dvertices = new float[ndvertsuniq * 3];
	}
	for (int vi = 0; vi < nAllVerts; vi++) {
		int newIdx = verticesMap[vi];
		if (newIdx != 0xffff) {
			if (newIdx < nverts) {
				//navigation mesh vertex
				memcpy(vertices + 3 * newIdx, allVerts + 3 * vi, 3 * sizeof(float));
			}
			else {
				//detailed mesh vertex
				memcpy(dvertices + 3 * (newIdx - nverts), allVerts + 3 * vi, 3 * sizeof(float));
			}
		}
	}

	MEM_SAFE_FREE(allVerts);
	MEM_freeN(verticesMap);
	dm->release(dm);

	return true;
}

bool KX_NavMeshObject::BuildFromMesh(float *&vertices, int& nverts,
                                          unsigned short * &polys, int& npolys, unsigned short *&dmeshes,
                                          float *&dvertices, int &ndvertsuniq, unsigned short *&dtris,
                                          int& ndtris, int &vertsPerPoly)
{
	KX_Mesh *meshobj = m_meshes.front();
	vertsPerPoly = 3;

	// Indices count.
	unsigned int numindices = 0;
	// Original (without split of normal or UV) vertex count.
	unsigned int numvertices = 0;

	for (RAS_MeshMaterial *meshmat : meshobj->GetMeshMaterialList()) {
		RAS_DisplayArray *array = meshmat->GetDisplayArray();

		numindices += array->GetTriangleIndexCount();
		numvertices = std::max(numvertices, array->GetMaxOrigIndex() + 1);
	}

	// Detour can't manage more than 65536 vertices.
	if (numvertices > 0xffff) {
		return false;
	}

	vertices = new float[numvertices * 3];
	// Detour supports 6 indices per polygons natively, 0xffff is the discard value.
	polys = (unsigned short *)MEM_callocN(sizeof(unsigned short) * numindices * 2, "BuildVertIndArrays polys");
	memset(polys, 0xff, sizeof(unsigned short) * numindices * 2);

	/// Map from original vertex index to m_vertexArray vertex index.
	std::vector<int> vertRemap(numvertices, -1);

	// Current vertex written.
	unsigned int curvert = 0;
	// Current index written.
	unsigned int curind = 0;
	for (RAS_MeshMaterial *meshmat : meshobj->GetMeshMaterialList()) {
		RAS_DisplayArray *array = meshmat->GetDisplayArray();
		// Convert location of all vertices and remap if vertices weren't already converted.
		for (unsigned int j = 0, numvert = array->GetVertexCount(); j < numvert; ++j) {
			const RAS_VertexInfo& info = array->GetVertexInfo(j);
			const unsigned int origIndex = info.GetOrigIndex();
			/* Avoid double conversion of two unique vertices using the same base:
			 * using the same original vertex and so the same position.
			 */
			if (vertRemap[origIndex] != -1) {
				continue;
			}

			copy_v3_v3(&vertices[curvert * 3], array->GetPosition(j).data);

			// Register the vertex index where the position was converted in m_vertexArray.
			vertRemap[origIndex] = curvert++;
		}

		for (unsigned int j = 0, numtris = array->GetTriangleIndexCount() / 3; j < numtris; ++j) {
			for (unsigned short k = 0; k < 3; ++k) {
				const unsigned int index = array->GetTriangleIndex(j * 3 + k);
				const RAS_VertexInfo& info = array->GetVertexInfo(index);
				const unsigned int origIndex = info.GetOrigIndex();
				polys[curind + k] = vertRemap[origIndex];
			}
			curind += 6;
		}
	}

	nverts = numvertices;
	npolys = numindices / vertsPerPoly;
	dmeshes = nullptr;
	dvertices = nullptr;
	ndvertsuniq = 0;
	dtris = nullptr;
	ndtris = npolys;

	return true;
}

bool KX_NavMeshObject::BuildVertIndArrays(float *&vertices, int& nverts,
                                          unsigned short * &polys, int& npolys, unsigned short *&dmeshes,
                                          float *&dvertices, int &ndvertsuniq, unsigned short *&dtris,
                                          int& ndtris, int &vertsPerPoly)
{
	if (BuildFromDerivedMesh(vertices, nverts, polys, npolys, dmeshes, dvertices, ndvertsuniq, dtris, ndtris, vertsPerPoly)) {
		return true;
	}

	return BuildFromMesh(vertices, nverts, polys, npolys, dmeshes, dvertices, ndvertsuniq, dtris, ndtris, vertsPerPoly);
}

bool KX_NavMeshObject::BuildNavMesh()
{
	KX_ObstacleSimulation *obssimulation = GetScene()->GetObstacleSimulation();

	if (obssimulation) {
		obssimulation->DestroyObstacleForObj(this);
	}

	if (m_navMesh) {
		delete m_navMesh;
		m_navMesh = nullptr;
	}

	if (m_meshes.empty()) {
		CM_Error("can't find mesh for navmesh object: " << m_name);
		return false;
	}

	float *vertices = nullptr, *dvertices = nullptr;
	unsigned short *polys = nullptr, *dtris = nullptr, *dmeshes = nullptr;
	int nverts = 0, npolys = 0, ndvertsuniq = 0, ndtris = 0;
	int vertsPerPoly = 0;
	if (!BuildVertIndArrays(vertices, nverts, polys, npolys, dmeshes, dvertices, ndvertsuniq, dtris, ndtris, vertsPerPoly) ||
	    vertsPerPoly < 3) {
		CM_Error("can't build navigation mesh data for object: " << m_name);
		if (vertices) {
			delete[] vertices;
		}
		if (dvertices) {
			delete[] dvertices;
		}
		return false;
	}

	mt::vec3 pos;
	if (dmeshes == nullptr) {
		for (int i = 0; i < nverts; i++) {
			flipAxes(&vertices[i * 3]);
		}
		for (int i = 0; i < ndvertsuniq; i++) {
			flipAxes(&dvertices[i * 3]);
		}
	}

	if (!buildMeshAdjacency(polys, npolys, nverts, vertsPerPoly)) {
		CM_FunctionError("unable to build mesh adjacency information.");
		delete[] vertices;
		return false;
	}

	float cs = 0.2f;

	if (!nverts || !npolys) {
		CM_FunctionError("unable to build navigation mesh");
		if (vertices) {
			delete[] vertices;
		}
		return false;
	}

	float bmin[3], bmax[3];
	calcMeshBounds(vertices, nverts, bmin, bmax);
	//quantize vertex pos
	unsigned short *vertsi = new unsigned short[3 * nverts];
	float ics = 1.f / cs;
	for (int i = 0; i < nverts; i++) {
		vertsi[3 * i + 0] = static_cast<unsigned short>((vertices[3 * i + 0] - bmin[0]) * ics);
		vertsi[3 * i + 1] = static_cast<unsigned short>((vertices[3 * i + 1] - bmin[1]) * ics);
		vertsi[3 * i + 2] = static_cast<unsigned short>((vertices[3 * i + 2] - bmin[2]) * ics);
	}

	// Calculate data size
	const int headerSize = sizeof(dtStatNavMeshHeader);
	const int vertsSize = sizeof(float) * 3 * nverts;
	const int polysSize = sizeof(dtStatPoly) * npolys;
	const int nodesSize = sizeof(dtStatBVNode) * npolys * 2;
	const int detailMeshesSize = sizeof(dtStatPolyDetail) * npolys;
	const int detailVertsSize = sizeof(float) * 3 * ndvertsuniq;
	const int detailTrisSize = sizeof(unsigned char) * 4 * ndtris;

	const int dataSize = headerSize + vertsSize + polysSize + nodesSize +
	                     detailMeshesSize + detailVertsSize + detailTrisSize;
	unsigned char *data = new unsigned char[dataSize];

	memset(data, 0, dataSize);

	unsigned char *d = data;
	dtStatNavMeshHeader *header = (dtStatNavMeshHeader *)d; d += headerSize;
	float *navVerts = (float *)d; d += vertsSize;
	dtStatPoly *navPolys = (dtStatPoly *)d; d += polysSize;
	dtStatBVNode *navNodes = (dtStatBVNode *)d; d += nodesSize;
	dtStatPolyDetail *navDMeshes = (dtStatPolyDetail *)d; d += detailMeshesSize;
	float *navDVerts = (float *)d; d += detailVertsSize;
	unsigned char *navDTris = (unsigned char *)d; d += detailTrisSize;

	// Store header
	header->magic = DT_STAT_NAVMESH_MAGIC;
	header->version = DT_STAT_NAVMESH_VERSION;
	header->npolys = npolys;
	header->nverts = nverts;
	header->cs = cs;
	header->bmin[0] = bmin[0];
	header->bmin[1] = bmin[1];
	header->bmin[2] = bmin[2];
	header->bmax[0] = bmax[0];
	header->bmax[1] = bmax[1];
	header->bmax[2] = bmax[2];
	header->ndmeshes = npolys;
	header->ndverts = ndvertsuniq;
	header->ndtris = ndtris;

	// Store vertices
	for (int i = 0; i < nverts; ++i) {
		const unsigned short *iv = &vertsi[i * 3];
		float *v = &navVerts[i * 3];
		v[0] = bmin[0] + iv[0] * cs;
		v[1] = bmin[1] + iv[1] * cs;
		v[2] = bmin[2] + iv[2] * cs;
	}

	// Store polygons.
	const unsigned short *src = polys;
	for (int i = 0; i < npolys; ++i) {
		dtStatPoly *p = &navPolys[i];
		p->nv = 0;
		for (int j = 0; j < vertsPerPoly; ++j) {
			if (src[j] == 0xffff) {
				break;
			}
			p->v[j] = src[j];
			p->n[j] = src[vertsPerPoly + j] + 1;
			p->nv++;
		}
		src += vertsPerPoly * 2;
	}

	header->nnodes = createBVTree(vertsi, nverts, polys, npolys, vertsPerPoly, cs, cs, npolys * 2, navNodes);

	if (!dmeshes) {
		// Create fake detail meshes.
		for (int i = 0; i < npolys; ++i) {
			dtStatPolyDetail& dtl = navDMeshes[i];
			dtl.vbase = 0;
			dtl.nverts = 0;
			dtl.tbase = i;
			dtl.ntris = 1;
		}
		// Setup triangles.
		unsigned char *tri = navDTris;
		for (unsigned int i = 0; i < ndtris; i++) {
			for (unsigned int j = 0; j < 3; j++) {
				tri[4 * i + j] = j;
			}
		}
	}
	else {
		// Vertices.
		memcpy(navDVerts, dvertices, ndvertsuniq * 3 * sizeof(float));
		// Triangles.
		unsigned char *tri = navDTris;
		for (unsigned int i = 0; i < ndtris; i++) {
			for (unsigned int j = 0; j < 3; j++) {
				tri[4 * i + j] = dtris[6 * i + j];
			}
		}
		// Detailed meshes.
		for (int i = 0; i < npolys; ++i) {
			dtStatPolyDetail& dtl = navDMeshes[i];
			dtl.vbase = dmeshes[i * 4 + 0];
			dtl.nverts = dmeshes[i * 4 + 1];
			dtl.tbase = dmeshes[i * 4 + 2];
			dtl.ntris = dmeshes[i * 4 + 3];
		}
	}

	m_navMesh = new dtStatNavMesh();
	m_navMesh->init(data, dataSize, true);

	delete[] vertices;

	// Navmesh conversion is using C guarded alloc for memory allocaitons.
	MEM_freeN(polys);
	if (dmeshes) {
		MEM_freeN(dmeshes);
	}
	if (dtris) {
		MEM_freeN(dtris);
	}

	if (dvertices) {
		delete[] dvertices;
	}

	if (vertsi) {
		delete[] vertsi;
	}

	if (obssimulation) {
		obssimulation->AddObstaclesForNavMesh(this);
	}

	return true;
}

dtStatNavMesh *KX_NavMeshObject::GetNavMesh() const
{
	return m_navMesh;
}

void KX_NavMeshObject::DrawNavMesh(NavMeshRenderMode renderMode) const
{
	if (!m_navMesh) {
		return;
	}

	const mt::vec4 color(0.0f, 0.0f, 0.0f, 1.0f);

	switch (renderMode) {
		case RM_POLYS:
		case RM_WALLS:
		{
			for (int pi = 0; pi < m_navMesh->getPolyCount(); pi++) {
				const dtStatPoly *poly = m_navMesh->getPoly(pi);

				for (int i = 0, j = (int)poly->nv - 1; i < (int)poly->nv; j = i++) {
					if (poly->n[j] && renderMode == RM_WALLS) {
						continue;
					}
					const float *vif = m_navMesh->getVertex(poly->v[i]);
					const float *vjf = m_navMesh->getVertex(poly->v[j]);
					mt::vec3 vi(vif[0], vif[2], vif[1]);
					mt::vec3 vj(vjf[0], vjf[2], vjf[1]);
					vi = TransformToWorldCoords(vi);
					vj = TransformToWorldCoords(vj);
					KX_RasterizerDrawDebugLine(vi, vj, color);
				}
			}
			break;
		}
		case RM_TRIS:
		{
			for (int i = 0; i < m_navMesh->getPolyDetailCount(); ++i) {
				const dtStatPoly *p = m_navMesh->getPoly(i);
				const dtStatPolyDetail *pd = m_navMesh->getPolyDetail(i);

				for (int j = 0; j < pd->ntris; ++j) {
					const unsigned char *t = m_navMesh->getDetailTri(pd->tbase + j);
					mt::vec3 tri[3];
					for (int k = 0; k < 3; ++k) {
						const float *v;
						if (t[k] < p->nv) {
							v = m_navMesh->getVertex(p->v[t[k]]);
						}
						else {
							v =  m_navMesh->getDetailVertex(pd->vbase + (t[k] - p->nv));
						}
						float pos[3];
						rcVcopy(pos, v);
						flipAxes(pos);
						tri[k] = mt::vec3(pos);
					}

					for (int k = 0; k < 3; k++) {
						tri[k] = TransformToWorldCoords(tri[k]);
					}

					for (int k = 0; k < 3; k++) {
						KX_RasterizerDrawDebugLine(tri[k], tri[(k + 1) % 3], color);
					}
				}
			}
			break;
		}
		default:
		{
			break;
		}
	}
}

mt::vec3 KX_NavMeshObject::TransformToLocalCoords(const mt::vec3& wpos) const
{
	return (NodeGetWorldTransform().Inverse() * wpos);
}

mt::vec3 KX_NavMeshObject::TransformToWorldCoords(const mt::vec3& lpos) const
{
	return (NodeGetWorldTransform() * lpos);
}

KX_NavMeshObject::PathType KX_NavMeshObject::FindPath(const mt::vec3& from, const mt::vec3& to, unsigned int maxPathLen) const
{
	PathType path;

	if (!m_navMesh) {
		return path;
	}

	mt::vec3 localfrom = TransformToLocalCoords(from);
	mt::vec3 localto = TransformToLocalCoords(to);
	flipAxes(localfrom);
	flipAxes(localto);
	dtStatPolyRef sPolyRef = m_navMesh->findNearestPoly(localfrom.Data(), polyPickExt);
	dtStatPolyRef ePolyRef = m_navMesh->findNearestPoly(localto.Data(), polyPickExt);

	if (sPolyRef && ePolyRef) {
		dtStatPolyRef *polys = (dtStatPolyRef *)BLI_array_alloca(polys, maxPathLen);
		const unsigned int npolys = m_navMesh->findPath(sPolyRef, ePolyRef, localfrom.Data(), localto.Data(), polys, maxPathLen);
		if (npolys > 0) {
			float(*points)[3] = (float(*)[3])BLI_array_alloca(points, maxPathLen);
			const unsigned int pathLen = m_navMesh->findStraightPath(localfrom.Data(), localto.Data(), polys, npolys,
			                                                         &points[0][0], maxPathLen);

			path.resize(pathLen);
			for (unsigned int i = 0; i < pathLen; ++i) {
				mt::vec3 waypoint(points[i]);
				flipAxes(waypoint);
				path[i] = TransformToWorldCoords(waypoint);
			}
		}
	}

	return path;
}

float KX_NavMeshObject::Raycast(const mt::vec3& from, const mt::vec3& to) const
{
	if (!m_navMesh) {
		return 0.f;
	}

	mt::vec3 localfrom = TransformToLocalCoords(from);
	mt::vec3 localto = TransformToLocalCoords(to);
	flipAxes(localfrom);
	flipAxes(localto);

	dtStatPolyRef sPolyRef = m_navMesh->findNearestPoly(localfrom.Data(), polyPickExt);

	float t = 0.0f;
	static dtStatPolyRef polys[MAX_PATH_LEN];
	m_navMesh->raycast(sPolyRef, localfrom.Data(), localto.Data(), t, polys, MAX_PATH_LEN);
	return t;
}

void KX_NavMeshObject::DrawPath(const PathType& path, const mt::vec4& color) const
{
	for (unsigned int i = 0, size = (path.size() - 1); i < size; ++i) {
		KX_RasterizerDrawDebugLine(path[i], path[i + 1], color);
	}
}

#ifdef WITH_PYTHON

PyTypeObject KX_NavMeshObject::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_NavMeshObject",
	sizeof(EXP_PyObjectPlus_Proxy),
	0,
	py_base_dealloc,
	0,
	0,
	0,
	0,
	py_base_repr,
	0,
	0,
	0,
	0, 0, 0, 0, 0, 0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	0, 0, 0, 0, 0, 0, 0,
	Methods,
	0,
	0,
	&KX_GameObject::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyAttributeDef KX_NavMeshObject::Attributes[] = {
	EXP_PYATTRIBUTE_NULL // Sentinel.
};

PyMethodDef KX_NavMeshObject::Methods[] = {
	EXP_PYMETHODTABLE(KX_NavMeshObject, findPath),
	EXP_PYMETHODTABLE(KX_NavMeshObject, raycast),
	EXP_PYMETHODTABLE(KX_NavMeshObject, draw),
	EXP_PYMETHODTABLE(KX_NavMeshObject, rebuild),
	{nullptr, nullptr} // Sentinel.
};

EXP_PYMETHODDEF_DOC(KX_NavMeshObject, findPath,
                    "findPath(start, goal): find path from start to goal points\n"
                    "Returns a path as list of points)\n")
{
	PyObject *ob_from, *ob_to;
	if (!PyArg_ParseTuple(args, "OO:getPath", &ob_from, &ob_to)) {
		return nullptr;
	}
	mt::vec3 from, to;
	if (!PyVecTo(ob_from, from) || !PyVecTo(ob_to, to)) {
		return nullptr;
	}

	const PathType path = FindPath(from, to, MAX_PATH_LEN);
	const unsigned int pathLen = path.size();
	PyObject *pathList = PyList_New(pathLen);
	for (unsigned int i = 0; i < pathLen; ++i) {
		PyList_SET_ITEM(pathList, i, PyObjectFrom(path[i]));
	}

	return pathList;
}

EXP_PYMETHODDEF_DOC(KX_NavMeshObject, raycast,
                    "raycast(start, goal): raycast from start to goal points\n"
                    "Returns hit factor)\n")
{
	PyObject *ob_from, *ob_to;
	if (!PyArg_ParseTuple(args, "OO:getPath", &ob_from, &ob_to)) {
		return nullptr;
	}
	mt::vec3 from, to;
	if (!PyVecTo(ob_from, from) || !PyVecTo(ob_to, to)) {
		return nullptr;
	}
	float hit = Raycast(from, to);
	return PyFloat_FromDouble(hit);
}

EXP_PYMETHODDEF_DOC(KX_NavMeshObject, draw,
                    "draw(mode): navigation mesh debug drawing\n"
                    "mode: WALLS, POLYS, TRIS\n")
{
	int arg;
	NavMeshRenderMode renderMode = RM_TRIS;
	if (PyArg_ParseTuple(args, "i:rebuild", &arg) && arg >= 0 && arg < RM_MAX) {
		renderMode = (NavMeshRenderMode)arg;
	}
	DrawNavMesh(renderMode);
	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC_NOARGS(KX_NavMeshObject, rebuild,
                           "rebuild(): rebuild navigation mesh\n")
{
	BuildNavMesh();
	Py_RETURN_NONE;
}

#endif  // WITH_PYTHON
