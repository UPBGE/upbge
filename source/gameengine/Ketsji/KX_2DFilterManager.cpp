
#include "KX_2DFilterManager.h"
#include "KX_2DFilter.h"

KX_2DFilterManager::KX_2DFilterManager(RAS_ICanvas *canvas)
	:RAS_2DFilterManager(canvas)
{
}

KX_2DFilterManager::~KX_2DFilterManager()
{
}

RAS_2DFilter *KX_2DFilterManager::NewFilter(RAS_2DFilterData& filterData)
{
	return new KX_2DFilter(filterData, this);
}
