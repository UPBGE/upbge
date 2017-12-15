#ifndef __RAS_VERTEX_FORMAT_H__
#define __RAS_VERTEX_FORMAT_H__

/// Struct used to pass the vertex format to functions.
struct RAS_VertexFormat
{
	uint8_t uvSize;
	uint8_t colorSize;

	RAS_VertexFormat() = default;

	/*template <class FormatType>
	RAS_VertexFormat(const FormatType& formatType)
		:uvSize(FormatType::UvSize),
		colorSize(FormatType::ColorSize)
	{
	}*/

	/// Operators used to compare the contents (uv size, color size, ...) of two vertex formats.
	inline bool operator== (const RAS_VertexFormat& other) const
	{
		return (uvSize == other.uvSize && colorSize == other.colorSize);
	}

	inline bool operator!= (const RAS_VertexFormat& other) const
	{
		return !(*this == other);
	}
};

template <uint8_t uvSize, uint8_t colorSize>
struct RAS_VertexFormatType
{
	enum {
		UvSize = uvSize,
		ColorSize = colorSize
	};

	inline bool operator== (const RAS_VertexFormat& format) const
	{
		return (format.uvSize == UvSize && format.colorSize == ColorSize);
	}

	inline bool operator!= (const RAS_VertexFormat& format) const
	{
		return !(*this == format);
	}
};

template <uint8_t color>
using RAS_VertexFormatUvTuple = std::tuple<
	RAS_VertexFormatType<1, color>,
	RAS_VertexFormatType<2, color>,
	RAS_VertexFormatType<3, color>,
	RAS_VertexFormatType<4, color>,
	RAS_VertexFormatType<5, color>,
	RAS_VertexFormatType<6, color>,
	RAS_VertexFormatType<7, color>
>;

using RAS_VertexFormatTuple = decltype(std::tuple_cat(
	RAS_VertexFormatUvTuple<1>(),
	RAS_VertexFormatUvTuple<2>(),
	RAS_VertexFormatUvTuple<3>(),
	RAS_VertexFormatUvTuple<4>(),
	RAS_VertexFormatUvTuple<5>(),
	RAS_VertexFormatUvTuple<6>(),
	RAS_VertexFormatUvTuple<7>()
));

#endif  // __RAS_VERTEX_FORMAT_H__
