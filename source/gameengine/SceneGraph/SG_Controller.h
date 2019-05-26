/*
 * Implementationclass to derive controllers from
 *
 *
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file SG_Controller.h
 *  \ingroup bgesg
 */

#ifndef __SG_CONTROLLER_H__
#define __SG_CONTROLLER_H__

#include "SG_Interpolator.h"

class SG_Node;

/**
 * A scenegraph controller
 */
class SG_Controller
{
	friend SG_Node;
public:
	/**
	 * Option-identifiers: SG_CONTR_<controller-type>_<option>.
	 * Options only apply to a specific controller type. The
	 * semantics are defined by whoever uses the setting.
	 */
	enum SG_ControllerOption {
		SG_CONTR_NODEF = 0,
		SG_CONTR_IPO_IPO_AS_FORCE,
		SG_CONTR_IPO_IPO_ADD,
		SG_CONTR_IPO_LOCAL,
		SG_CONTR_IPO_RESET,
		SG_CONTR_MAX
	};

	SG_Controller();
	virtual ~SG_Controller() = default;

	/// Perform an update, returns true when the update was performed.
	virtual bool Update(SG_Node *node);

	void SetSimulatedTime(double time);

	/**
	 * Pass options to specific controllers
	 * \param option An integer identifying the option.
	 * \param value  The value of this option.
	 */
	virtual void SetOption(SG_ControllerOption option, bool value);

	void AddInterpolator(const SG_Interpolator& interp);

	bool Empty() const;

protected:
	SG_InterpolatorList m_interpolators;
	/// Were settings altered since the last update?
	bool m_modified;
	/// Local time of this ipo.
	double m_time;

};

#endif  /* __SG_CONTROLLER_H__ */
