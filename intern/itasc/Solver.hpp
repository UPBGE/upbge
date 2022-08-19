/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2009 Ruben Smits. */

/** \file
 * \ingroup intern_itasc
 */

#ifndef SOLVER_HPP_
#define SOLVER_HPP_

#include <vector>
#include "eigen_types.hpp"

namespace iTaSC{

class Solver{
public:
	enum SolverParam {
		DLS_QMAX = 0,
		DLS_LAMBDA_MAX,
		DLS_EPSILON
	};
    virtual ~Solver(){};

	// gc = grouping of constraint output , 
	//      size of vector = nc, alternance of true / false to indicate the grouping of output
	virtual bool init(unsigned int nq, unsigned int nc, const std::vector<bool>& gc)=0;
    virtual bool solve(const e_matrix& A, const e_vector& Wy, const e_vector& ydot, const e_matrix& Wq, e_vector& qdot, e_scalar& nlcoef)=0;
	virtual void setParam(SolverParam param, double value)=0;
};

}
#endif /* SOLVER_HPP_ */
