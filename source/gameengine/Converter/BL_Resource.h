#ifndef __BL_RESOURCE_H__
#define __BL_RESOURCE_H__

#include <cstdint>

class BL_SceneConverter;
struct Main;

/** Base class of resources. Used to identify the library of the resource.
 */
class BL_Resource
{
public:
	/// Opaque library identifier.
	class Library
	{
	private:
		uintptr_t m_id;

	public:
		Library();
		explicit Library(Main *maggie);

		/// Return true if the identifier was constructed along an existing library.
		bool Valid() const;

		inline bool operator==(const Library& other) const
		{
			return (m_id == other.m_id);
		}
	};

private:
	/// The identifier of library owning the resource.
	Library m_libraryId;

public:
	/// Initialize the library of this resource, must be called only once.
	void SetOwner(const Library& libraryId);

	/** Return true if the libraryId match m_libraryId.
	 * Meaning the resource was converted with date from this library.
	 */
	bool Belong(const Library& libraryId) const;
};

#endif  // __BL_RESOURCE_H__
