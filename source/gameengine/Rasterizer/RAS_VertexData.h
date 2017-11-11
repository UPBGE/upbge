#ifndef __RAS_VERTEX_DATA_H__
#define __RAS_VERTEX_DATA_H__

#include "MT_Vector4.h"

#include "BLI_math_vector.h"

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

	inline RAS_VertexDataBasic(const float _position[3], const float _normal[3], const float _tangent[4])
	{
		copy_v3_v3(position, _position);
		copy_v3_v3(normal, _normal);
		copy_v4_v4(tangent, _tangent);
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

	inline RAS_VertexDataExtra(const float _uvs[uvSize][2], const unsigned int _colors[colorSize])
	{
		for (unsigned short i = 0; i < uvSize; ++i) {
			copy_v2_v2(uvs[i], _uvs[i]);
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

	inline RAS_IVertexData(const float position[3], const float normal[3], const float tangent[4])
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

	inline RAS_VertexData(const float xyz[3],
						  const float uvs[uvSize][2],
						  const float tangent[4],
						  const unsigned int rgba[colorSize],
						  const float normal[3])
		:RAS_IVertexData(xyz, normal, tangent),
		RAS_VertexDataExtra<uvSize, colorSize>(uvs, rgba)
	{
	}
};

#endif  // __RAS_VERTEX_DATA_H__
