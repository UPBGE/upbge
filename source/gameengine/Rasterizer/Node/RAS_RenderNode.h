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
* Contributor(s): Porteries Tristan.
*
* ***** END GPL LICENSE BLOCK *****
*/

#ifndef __RAS_RENDER_NODE__
#define __RAS_RENDER_NODE__

#include "RAS_DownwardNode.h"
#include "RAS_UpwardNode.h"
#include "RAS_UpwardNodeIterator.h"
#include "RAS_Rasterizer.h"

#include <type_traits>

class RAS_BucketManager;
class RAS_MaterialBucket;
class RAS_DisplayArray;
class RAS_DisplayArrayBucket;
class RAS_MeshSlot;
class RAS_IPolyMaterial;
class RAS_Rasterizer;
class RAS_DisplayArrayStorage;
class RAS_AttributeArrayStorage;

class RAS_MaterialDownwardNode;
class RAS_ManagerDownwardNode;
class RAS_DisplayArrayDownwardNode;

class RAS_MaterialUpwardNode;
class RAS_ManagerUpwardNode;
class RAS_DisplayArrayUpwardNode;
class RAS_MeshSlotUpwardNode;

struct RAS_ManagerNodeData
{
	mt::mat3x4 m_trans;
	RAS_Rasterizer *m_rasty;
	RAS_Rasterizer::DrawType m_drawingMode;
	bool m_sort;
	bool m_shaderOverride;
};

struct RAS_MaterialNodeData
{
	RAS_IPolyMaterial *m_material;
	int m_drawingMode;
	bool m_cullFace;
	bool m_zsort;
	bool m_text;
	float m_zoffset;
};

struct RAS_DisplayArrayNodeData
{
	RAS_DisplayArray *m_array;
	RAS_AttributeArrayStorage *m_attribStorage;
	RAS_DisplayArrayStorage *m_arrayStorage;
	bool m_applyMatrix;
};

struct RAS_MeshSlotNodeData
{
};

/// Data passed to material node.
struct RAS_MaterialNodeTuple
{
	RAS_ManagerNodeData *m_managerData;

	RAS_MaterialNodeTuple(const RAS_DummyNodeTuple& dummyTuple, RAS_ManagerNodeData *managerData)
		:m_managerData(managerData)
	{
	}
};

/// Data passed to display array node.
struct RAS_DisplayArrayNodeTuple : RAS_MaterialNodeTuple
{
	RAS_MaterialNodeData *m_materialData;

	RAS_DisplayArrayNodeTuple(const RAS_MaterialNodeTuple& materialTuple, RAS_MaterialNodeData *materialData)
		:RAS_MaterialNodeTuple(materialTuple),
		m_materialData(materialData)
	{
	}
};

/// Data passed to mesh slot node.
struct RAS_MeshSlotNodeTuple : RAS_DisplayArrayNodeTuple
{
	RAS_DisplayArrayNodeData *m_displayArrayData;

	RAS_MeshSlotNodeTuple(const RAS_DisplayArrayNodeTuple& displayArrayTuple, RAS_DisplayArrayNodeData *displayArrayData)
		:RAS_DisplayArrayNodeTuple(displayArrayTuple),
		m_displayArrayData(displayArrayData)
	{
	}
};

struct RAS_ManagerNodeInfo
{
	/// Wrapped type owning the node.
	using OwnerType = RAS_BucketManager;
	/// Data the node will pass to the sub node.
	using DataType = RAS_ManagerNodeData;
	/// Data tuple used in bind/unbind functions.
	using TupleType = RAS_DummyNodeTuple;
	/// True if the node is a leaf/final.
	using Leaf = std::false_type;
};

struct RAS_MaterialNodeInfo
{
	using OwnerType = RAS_MaterialBucket;
	using DataType = RAS_MaterialNodeData;
	using TupleType = RAS_MaterialNodeTuple;
	using Leaf = std::false_type;
};

/// Node info for display array for the downward tree, this node is a leaf.
struct RAS_DisplayArrayDownwardNodeInfo
{
	using OwnerType = RAS_DisplayArrayBucket;
	using DataType = RAS_DisplayArrayNodeData;
	using TupleType = RAS_DisplayArrayNodeTuple;
	using Leaf = std::true_type;
};

/// Node info for display array for the upward tree, this node is not a leaf.
struct RAS_DisplayArrayUpwardNodeInfo
{
	using OwnerType = RAS_DisplayArrayBucket;
	using DataType = RAS_DisplayArrayNodeData;
	using TupleType = RAS_DisplayArrayNodeTuple;
	using Leaf = std::false_type;
};

struct RAS_MeshSlotNodeInfo
{
	using OwnerType = RAS_MeshSlot;
	using DataType = RAS_DummyNodeData;
	using TupleType = RAS_MeshSlotNodeTuple;
	using Leaf = std::true_type;
};

class RAS_ManagerDownwardNode : public RAS_DownwardNode<RAS_ManagerNodeInfo, RAS_MaterialDownwardNode>
{
public:
	RAS_ManagerDownwardNode(OwnerType *owner, DataType *data, Function bind, Function unbind)
		:RAS_DownwardNode<RAS_ManagerNodeInfo, RAS_MaterialDownwardNode>::RAS_DownwardNode(owner, data, bind, unbind)
	{
	}

	RAS_ManagerDownwardNode() = default;
};

class RAS_MaterialDownwardNode : public RAS_DownwardNode<RAS_MaterialNodeInfo, RAS_DisplayArrayDownwardNode>
{
public:
	RAS_MaterialDownwardNode(OwnerType *owner, DataType *data, Function bind, Function unbind)
		:RAS_DownwardNode<RAS_MaterialNodeInfo, RAS_DisplayArrayDownwardNode>::RAS_DownwardNode(owner, data, bind, unbind)
	{
	}

	RAS_MaterialDownwardNode() = default;
};

class RAS_DisplayArrayDownwardNode : public RAS_DownwardNode<RAS_DisplayArrayDownwardNodeInfo, RAS_DummyNode>
{
public:
	RAS_DisplayArrayDownwardNode(OwnerType *owner, DataType *data, Function bind, Function unbind)
		:RAS_DownwardNode<RAS_DisplayArrayDownwardNodeInfo, RAS_DummyNode>::RAS_DownwardNode(owner, data, bind, unbind)
	{
	}

	RAS_DisplayArrayDownwardNode() = default;
};

class RAS_ManagerUpwardNode : public RAS_UpwardNode<RAS_ManagerNodeInfo, RAS_DummyNode>
{
public:
	RAS_ManagerUpwardNode(OwnerType *owner, DataType *data, Function bind, Function unbind)
		:RAS_UpwardNode<RAS_ManagerNodeInfo, RAS_DummyNode>::RAS_UpwardNode(owner, data, bind, unbind)
	{
	}

	RAS_ManagerUpwardNode() = default;
};

class RAS_MaterialUpwardNode : public RAS_UpwardNode<RAS_MaterialNodeInfo, RAS_ManagerUpwardNode>
{
public:
	RAS_MaterialUpwardNode(OwnerType *owner, DataType *data, Function bind, Function unbind)
		:RAS_UpwardNode<RAS_MaterialNodeInfo, RAS_ManagerUpwardNode>::RAS_UpwardNode(owner, data, bind, unbind)
	{
	}

	RAS_MaterialUpwardNode() = default;
};

class RAS_DisplayArrayUpwardNode : public RAS_UpwardNode<RAS_DisplayArrayUpwardNodeInfo, RAS_MaterialUpwardNode>
{
public:
	RAS_DisplayArrayUpwardNode(OwnerType *owner, DataType *data, Function bind, Function unbind)
		:RAS_UpwardNode<RAS_DisplayArrayUpwardNodeInfo, RAS_MaterialUpwardNode>::RAS_UpwardNode(owner, data, bind, unbind)
	{
	}

	RAS_DisplayArrayUpwardNode() = default;
};

class RAS_MeshSlotUpwardNode : public RAS_UpwardNode<RAS_MeshSlotNodeInfo, RAS_DisplayArrayUpwardNode>
{
public:
	RAS_MeshSlotUpwardNode(OwnerType *owner, DataType *data, Function bind, Function unbind)
		:RAS_UpwardNode<RAS_MeshSlotNodeInfo, RAS_DisplayArrayUpwardNode>::RAS_UpwardNode(owner, data, bind, unbind)
	{
	}

	RAS_MeshSlotUpwardNode() = default;
};

typedef std::vector<RAS_MeshSlotUpwardNode *> RAS_UpwardTreeLeafs;

class RAS_MeshSlotUpwardNodeIterator : public RAS_UpwardNodeIterator<RAS_MeshSlotUpwardNode>
{
public:
	RAS_MeshSlotUpwardNodeIterator(NodeType *node)
		:RAS_UpwardNodeIterator<RAS_MeshSlotUpwardNode>(node)
	{
	}
};

#endif  // __RAS_RENDER_NODE__
