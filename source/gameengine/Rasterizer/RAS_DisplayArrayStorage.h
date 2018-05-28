#ifndef __RAS_DISPLAY_ARRAY_STORAGE_H__
#define __RAS_DISPLAY_ARRAY_STORAGE_H__

#include <vector>
#include <memory>

class RAS_DisplayArray;
class RAS_StorageVbo;
class RAS_StorageVao;

class RAS_DisplayArrayStorage
{
	friend RAS_StorageVao;

private:
	std::unique_ptr<RAS_StorageVbo> m_vbo;

	RAS_StorageVbo *GetVbo() const;

public:
	RAS_DisplayArrayStorage();
	~RAS_DisplayArrayStorage();

	// Construct manually to take care that the OpenGL context is current (case of asynchronous libloading).
	void Construct(RAS_DisplayArray *array);

	void UpdateVertexData(unsigned int modifiedFlag);
	void UpdateSize();
	/// Map the index data and return its pointer.
	unsigned int *GetIndexMap();
	/// Flush the index data map.
	void FlushIndexMap();

	/// Render the display array.
	void IndexPrimitives();
	/** Render the display array using instancing.
	 * \param numslots The number of instance to render.
	 */
	void IndexPrimitivesInstancing(unsigned int numslots);
	/** Render the display array using indirect indices array.
	 * \param indices The list of indices pointers to read.
	 * \param counts The number of indices associated to the indices pointers.
	 */
	void IndexPrimitivesBatching(const std::vector<intptr_t>& indices, const std::vector<int>& counts);
};

#endif  // __RAS_DISPLAY_ARRAY_STORAGE_H__
