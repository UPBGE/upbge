/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2009 Benoit Bolsee. */

/** \file
 * \ingroup intern_itasc
 */

#ifndef FIXEDOBJECT_HPP_
#define FIXEDOBJECT_HPP_

#include "UncontrolledObject.hpp"
#include <vector>


namespace iTaSC{

class FixedObject: public UncontrolledObject {
public:
    FixedObject();
    virtual ~FixedObject();

	int addFrame(const std::string& name, const Frame& frame);

	virtual void updateCoordinates(const Timestamp& timestamp) {};
	virtual int addEndEffector(const std::string& name);
	virtual bool finalize();
	virtual const Frame& getPose(const unsigned int frameIndex);
	virtual void updateKinematics(const Timestamp& timestamp) {};
	virtual void pushCache(const Timestamp& timestamp) {};
	virtual void initCache(Cache *_cache) {};

protected:
	virtual void updateJacobian() {}
private:
    typedef std::vector<std::pair<std::string, Frame> > FrameList;

	bool m_finalized;
	unsigned int m_nframe;
	FrameList m_frameArray;

};

}

#endif /* FIXEDOBJECT_H_ */
