#include "BL_Resource.h"

#include "BLI_utildefines.h"

BL_Resource::Library::Library()
	:m_id(0)
{
}

BL_Resource::Library::Library(Main *maggie)
	:m_id((uintptr_t)maggie)
{
}

bool BL_Resource::Library::Valid() const
{
	return (m_id != 0);
}

void BL_Resource::SetOwner(const Library& libraryId)
{
	// Forbid changing of library, replacing a valid library.
	BLI_assert(!m_libraryId.Valid());

	m_libraryId = libraryId;
}

bool BL_Resource::Belong(const Library& libraryId) const
{
	return (m_libraryId == libraryId);
}
