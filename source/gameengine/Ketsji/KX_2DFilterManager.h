
#ifndef __KX_2DFILTER_MANAGER_H__
#define __KX_2DFILTER_MANAGER_H__

#include "RAS_2DFilterManager.h"

class KX_2DFilterManager : public RAS_2DFilterManager
{
private:

public:
	KX_2DFilterManager(RAS_ICanvas *canvas);
	virtual ~KX_2DFilterManager();

	virtual RAS_2DFilter *NewFilter(RAS_2DFilterData& filterData);
};

#endif // __KX_2DFILTER_MANAGER_H__