#ifndef __RAS_VERTEX_DATA_H__
#define __RAS_VERTEX_DATA_H__

#include "MT_Vector4.h"

struct RAS_VertexDataBasic
{
	float position[3];
	float normal[3];
	float tangent[4];

	inline RAS_VertexDataBasic() = default;

	inline RAS_VertexDataBasic(const MT_Vector3& _position, const MT_Vector3& _normal, const MT_Vector4& _tangent)
	{
		_position.getValue(position);
		_normal.getValue(normal);
		_tangent.getValue(tangent);
	}
};

template <unsigned short uvSize, unsigned short colorSize>
struct RAS_VertexDataExtra
{
	float uvs[uvSize][2];
	unsigned int colors[colorSize];

	inline RAS_VertexDataExtra() = default;

	inline RAS_VertexDataExtra(const MT_Vector2 _uvs[uvSize], const unsigned int _colors[colorSize])
	{
		for (unsigned short i = 0; i < uvSize; ++i) {
			_uvs[i].getValue(uvs[i]);
		}

		for (unsigned short i = 0; i < colorSize; ++i) {
			colors[i] = _colors[i];
		}
	}

};

struct RAS_IVertexData : RAS_VertexDataBasic
{
	inline RAS_IVertexData() = default;

	inline RAS_IVertexData(const MT_Vector3& position, const MT_Vector3& normal, const MT_Vector4& tangent)
		:RAS_VertexDataBasic(position, normal, tangent)
	{
	}
};

template <unsigned short uvSize, unsigned short colorSize>
struct RAS_VertexData : RAS_IVertexData, RAS_VertexDataExtra<uvSize, colorSize>
{
	enum {
		UvSize = uvSize,
		ColorSize = colorSize
	};

	inline RAS_VertexData() = default;

	inline RAS_VertexData(const MT_Vector3& xyz,
						  const MT_Vector2 uvs[uvSize],
						  const MT_Vector4& tangent,
						  const unsigned int rgba[colorSize],
						  const MT_Vector3& normal)
		:RAS_IVertexData(xyz, normal, tangent),
		RAS_VertexDataExtra<uvSize, colorSize>(uvs, rgba)
	{
	}
};

#endif  // __RAS_VERTEX_DATA_H__
