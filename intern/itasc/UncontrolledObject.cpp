/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2009 Ruben Smits. */

/** \file
 * \ingroup intern_itasc
 */

#include "UncontrolledObject.hpp"

namespace iTaSC{

UncontrolledObject::UncontrolledObject():Object(UnControlled),
	m_nu(0), m_nf(0), m_xudot()
{
}

UncontrolledObject::~UncontrolledObject() 
{
}

void UncontrolledObject::initialize(unsigned int _nu, unsigned int _nf)
{
	assert (_nf >= 1);
	m_nu = _nu;
	m_nf = _nf;
	if (_nu > 0)
		m_xudot = e_zero_vector(_nu);
	// clear all Jacobian if any
	m_JuArray.clear();
	// reserve one more to have an zero matrix handy
	if (m_nu > 0)
		m_JuArray.resize(m_nf+1, e_zero_matrix(6,m_nu));
}

const e_matrix& UncontrolledObject::getJu(unsigned int frameIndex) const
{
	assert (m_nu > 0);
	return m_JuArray[(frameIndex>m_nf)?m_nf:frameIndex];
}



}
