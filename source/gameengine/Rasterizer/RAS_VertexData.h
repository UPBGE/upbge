#ifndef __RAS_VERTEX_DATA_H__
#define __RAS_VERTEX_DATA_H__

#include "mathfu.h"

#include "RAS_VertexFormat.h"

#include "BLI_math_vector.h"

struct RAS_VertexDataBasic
{
	float position[3];
	float normal[3];
	float tangent[4];

	inline RAS_VertexDataBasic() = default;

	inline RAS_VertexDataBasic(const mt::vec3& _position, const mt::vec3& _normal, const mt::vec4& _tangent)
	{
		_position.Pack(position);
		_normal.Pack(normal);
		_tangent.Pack(tangent);
	}

	inline RAS_VertexDataBasic(const float _position[3], const float _normal[3], const float _tangent[4])
	{
		copy_v3_v3(position, _position);
		copy_v3_v3(normal, _normal);
		copy_v4_v4(tangent, _tangent);
	}
};

template <class Format>
struct RAS_VertexDataExtra
{
	float uvs[Format::UvSize][2];
	unsigned int colors[Format::ColorSize];

	inline RAS_VertexDataExtra() = default;

	inline RAS_VertexDataExtra(const mt::vec2 _uvs[Format::UvSize], const unsigned int _colors[Format::ColorSize])
	{
		for (unsigned short i = 0; i < Format::UvSize; ++i) {
			_uvs[i].Pack(uvs[i]);
		}

		for (unsigned short i = 0; i < Format::ColorSize; ++i) {
			colors[i] = _colors[i];
		}
	}

	inline RAS_VertexDataExtra(const float _uvs[Format::UvSize][2], const unsigned int _colors[Format::ColorSize])
	{
		for (unsigned short i = 0; i < Format::UvSize; ++i) {
			copy_v2_v2(uvs[i], _uvs[i]);
		}

		for (unsigned short i = 0; i < Format::ColorSize; ++i) {
			colors[i] = _colors[i];
		}
	}
};

struct RAS_IVertexData : RAS_VertexDataBasic
{
	inline RAS_IVertexData() = default;

	inline RAS_IVertexData(const mt::vec3& _position, const mt::vec3& _normal, const mt::vec4& _tangent)
		:RAS_VertexDataBasic(_position, _normal, _tangent)
	{
	}

	inline RAS_IVertexData(const float _position[3], const float _normal[3], const float _tangent[4])
		:RAS_VertexDataBasic(_position, _normal, _tangent)
	{
	}
};

struct RAS_VertexDataMemoryFormat
{
	/// Offsets of data in vertex class.
	intptr_t position;
	intptr_t normal;
	intptr_t tangent;
	intptr_t uvs;
	intptr_t colors;
	/// Size of vertex class.
	uint8_t size;
};

template <class Format>
struct RAS_VertexData : RAS_IVertexData, RAS_VertexDataExtra<Format>
{
	inline RAS_VertexData() = default;

	inline RAS_VertexData(const mt::vec3& _position,
						  const mt::vec2 _uvs[Format::UvSize],
						  const mt::vec4& _tangent,
						  const unsigned int _colors[Format::ColorSize],
						  const mt::vec3& _normal)
		:RAS_IVertexData(_position, _normal, _tangent),
		RAS_VertexDataExtra<Format>(_uvs, _colors)
	{
	}

	inline RAS_VertexData(const float _position[3],
						  const float _uvs[Format::UvSize][2],
						  const float _tangent[4],
						  const unsigned int _colors[Format::ColorSize],
						  const float _normal[3])
		:RAS_IVertexData(_position, _normal, _tangent),
		RAS_VertexDataExtra<Format>(_uvs, _colors)
	{
	}

	static const RAS_VertexDataMemoryFormat GetMemoryFormat()
	{
		return {offsetof(RAS_VertexData, position),
				offsetof(RAS_VertexData, normal),
				offsetof(RAS_VertexData, tangent),
				offsetof(RAS_VertexData, uvs),
				offsetof(RAS_VertexData, colors),
				sizeof(RAS_VertexData)};
	}

};

#endif  // __RAS_VERTEX_DATA_H__
