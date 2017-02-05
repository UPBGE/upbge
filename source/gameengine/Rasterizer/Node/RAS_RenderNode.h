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

class RAS_BucketManager;
class RAS_MaterialBucket;
class RAS_DisplayArrayBucket;
class RAS_MeshSlot;
class RAS_IRasterizer;
class MT_Transform;

class RAS_MaterialDownwardNode;
class RAS_ManagerDownwardNode;
class RAS_DisplayArrayDownwardNode;

class RAS_MaterialUpwardNode;
class RAS_ManagerUpwardNode;
class RAS_DisplayArrayUpwardNode;
class RAS_MeshSlotUpwardNode;

class RAS_RenderNodeArguments
{
public:
	const MT_Transform& m_trans;
	RAS_IRasterizer *m_rasty;
	bool m_sort;
	bool m_shaderOverride;

	RAS_RenderNodeArguments(const MT_Transform& trans, RAS_IRasterizer *rasty, bool sort, bool shaderOverride)
		:m_trans(trans),
		m_rasty(rasty),
		m_sort(sort),
		m_shaderOverride(shaderOverride)
	{
	}
};

class RAS_ManagerDownwardNode : public RAS_DownwardNode<RAS_MaterialDownwardNode, RAS_BucketManager, RAS_NodeFlag::NEVER_FINAL,
	RAS_RenderNodeArguments>
{
public:
	RAS_ManagerDownwardNode()
		:RAS_DownwardNode<RAS_MaterialDownwardNode, RAS_BucketManager, RAS_NodeFlag::NEVER_FINAL,
		RAS_RenderNodeArguments>::RAS_DownwardNode()
	{}
	RAS_ManagerDownwardNode(RAS_BucketManager *b, Function f1, Function f2)
		:RAS_DownwardNode<RAS_MaterialDownwardNode, RAS_BucketManager, RAS_NodeFlag::NEVER_FINAL,
		RAS_RenderNodeArguments>::RAS_DownwardNode(b, f1, f2)
	{}
};

class RAS_MaterialDownwardNode : public RAS_DownwardNode<RAS_DisplayArrayDownwardNode, RAS_MaterialBucket, RAS_NodeFlag::NEVER_FINAL,
	RAS_RenderNodeArguments>
{
public:
	RAS_MaterialDownwardNode()
		:RAS_DownwardNode<RAS_DisplayArrayDownwardNode, RAS_MaterialBucket, RAS_NodeFlag::NEVER_FINAL,
		RAS_RenderNodeArguments>::RAS_DownwardNode()
	{}
	RAS_MaterialDownwardNode(RAS_MaterialBucket *m, Function f1, Function f2)
		:RAS_DownwardNode<RAS_DisplayArrayDownwardNode, RAS_MaterialBucket, RAS_NodeFlag::NEVER_FINAL,
		RAS_RenderNodeArguments>::RAS_DownwardNode(m, f1, f2)
	{}
};

class RAS_DisplayArrayDownwardNode : public RAS_DownwardNode<RAS_DummyNode, RAS_DisplayArrayBucket, RAS_NodeFlag::ALWAYS_FINAL,
	RAS_RenderNodeArguments>
{
public:
	RAS_DisplayArrayDownwardNode()
		:RAS_DownwardNode<RAS_DummyNode, RAS_DisplayArrayBucket, RAS_NodeFlag::ALWAYS_FINAL,
		RAS_RenderNodeArguments>::RAS_DownwardNode()
	{}
	RAS_DisplayArrayDownwardNode(RAS_DisplayArrayBucket *d, Function f1, Function f2)
		:RAS_DownwardNode<RAS_DummyNode, RAS_DisplayArrayBucket, RAS_NodeFlag::ALWAYS_FINAL,
		RAS_RenderNodeArguments>::RAS_DownwardNode(d, f1, f2)
	{}
};

class RAS_ManagerUpwardNode : public RAS_UpwardNode<RAS_DummyNode, RAS_BucketManager, RAS_NodeFlag::NEVER_FINAL,
	RAS_RenderNodeArguments>
{
public:
	RAS_ManagerUpwardNode()
		:RAS_UpwardNode<RAS_DummyNode, RAS_BucketManager, RAS_NodeFlag::NEVER_FINAL,
		RAS_RenderNodeArguments>::RAS_UpwardNode()
	{}
	RAS_ManagerUpwardNode(RAS_BucketManager *b, Function f1, Function f2)
		:RAS_UpwardNode<RAS_DummyNode, RAS_BucketManager, RAS_NodeFlag::NEVER_FINAL,
		RAS_RenderNodeArguments>::RAS_UpwardNode(b, f1, f2)
	{}
};

class RAS_MaterialUpwardNode : public RAS_UpwardNode<RAS_ManagerUpwardNode, RAS_MaterialBucket, RAS_NodeFlag::NEVER_FINAL,
	RAS_RenderNodeArguments>
{
public:
	RAS_MaterialUpwardNode()
		:RAS_UpwardNode<RAS_ManagerUpwardNode, RAS_MaterialBucket, RAS_NodeFlag::NEVER_FINAL,
		RAS_RenderNodeArguments>::RAS_UpwardNode()
	{}
	RAS_MaterialUpwardNode(RAS_MaterialBucket *m, Function f1, Function f2)
		:RAS_UpwardNode<RAS_ManagerUpwardNode, RAS_MaterialBucket, RAS_NodeFlag::NEVER_FINAL,
		RAS_RenderNodeArguments>::RAS_UpwardNode(m, f1, f2)
	{}
};

class RAS_DisplayArrayUpwardNode : public RAS_UpwardNode<RAS_MaterialUpwardNode, RAS_DisplayArrayBucket, RAS_NodeFlag::NEVER_FINAL,
	RAS_RenderNodeArguments>
{
public:
	RAS_DisplayArrayUpwardNode()
		:RAS_UpwardNode<RAS_MaterialUpwardNode, RAS_DisplayArrayBucket, RAS_NodeFlag::NEVER_FINAL,
		RAS_RenderNodeArguments>::RAS_UpwardNode()
	{}
	RAS_DisplayArrayUpwardNode(RAS_DisplayArrayBucket *d, Function f1, Function f2)
		:RAS_UpwardNode<RAS_MaterialUpwardNode, RAS_DisplayArrayBucket, RAS_NodeFlag::NEVER_FINAL,
		RAS_RenderNodeArguments>::RAS_UpwardNode(d, f1, f2)
	{}
};

class RAS_MeshSlotUpwardNode : public RAS_UpwardNode<RAS_DisplayArrayUpwardNode, RAS_MeshSlot, RAS_NodeFlag::ALWAYS_FINAL,
	RAS_RenderNodeArguments>
{
public:
	RAS_MeshSlotUpwardNode()
		:RAS_UpwardNode<RAS_DisplayArrayUpwardNode, RAS_MeshSlot, RAS_NodeFlag::ALWAYS_FINAL,
		RAS_RenderNodeArguments>::RAS_UpwardNode()
	{}
	RAS_MeshSlotUpwardNode(RAS_MeshSlot *m, Function f1, Function f2)
		:RAS_UpwardNode<RAS_DisplayArrayUpwardNode, RAS_MeshSlot, RAS_NodeFlag::ALWAYS_FINAL,
		RAS_RenderNodeArguments>::RAS_UpwardNode(m, f1, f2)
	{}
};

typedef std::vector<RAS_MeshSlotUpwardNode *> RAS_UpwardTreeLeafs;

class RAS_MeshSlotUpwardNodeIterator : public RAS_UpwardNodeIterator<RAS_MeshSlotUpwardNode, RAS_RenderNodeArguments>
{
public:
	RAS_MeshSlotUpwardNodeIterator()
		:RAS_UpwardNodeIterator<RAS_MeshSlotUpwardNode, RAS_RenderNodeArguments>::RAS_UpwardNodeIterator()
	{}
};

#endif  // __RAS_RENDER_NODE__
