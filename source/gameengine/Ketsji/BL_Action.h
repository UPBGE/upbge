/*
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
 * Contributor(s): Mitchell Stokes.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file BL_Action.h
 *  \ingroup ketsji
 */

#ifndef __BL_ACTION_H__
#define __BL_ACTION_H__

#include <string>
#include <vector>

class KX_GameObject;
class SG_Controller;

struct bAction;
struct bPose;

class BL_Action
{
private:
	bAction* m_action;
	bAction* m_tmpaction;
	bPose* m_blendpose;
	bPose* m_blendinpose;
	std::vector<SG_Controller *> m_controllers;
	KX_GameObject* m_obj;
	std::vector<float>	m_blendshape;
	std::vector<float>	m_blendinshape;

	float m_startframe;
	float m_endframe;
	/// The current action frame.
	float m_localframe;
	float m_starttime;

	float m_blendin;
	float m_blendframe;
	float m_blendstart;

	float m_layer_weight;

	float m_speed;

	short m_priority;

	short m_playmode;
	short m_blendmode;

	short m_ipo_flags;

	bool m_done;
	/** Set to true when the last action update applies transformations
	 * to the object.
	 */
	bool m_appliedToObject;

	/// Set to true when the action was updated and applied. Back to false in the IPO update (UpdateIPO).
	bool m_requestIpo;
	bool m_calc_localtime;

	// The last update time to avoid double animation update.
	float m_prevUpdate;

	void ClearControllerList();
	void AddController(SG_Controller *cont);
	void InitIPO();
	void SetLocalTime(float curtime);
	void ResetStartTime(float curtime);
	void IncrementBlending(float curtime);
	void BlendShape(struct Key* key, float srcweight, std::vector<float>& blendshape);
public:
	BL_Action(class KX_GameObject* gameobj);
	~BL_Action();

	/**
	 * Play an action
	 */
	bool Play(const std::string& name,
			float start,
			float end,
			short priority,
			float blendin,
			short play_mode,
			float layer_weight,
			short ipo_flags,
			float playback_speed,
			short blend_mode);
	/**
	 * Whether or not the action is still playing
	 */
	bool IsDone();
	/**
	 * Update the action's frame, etc.
	 * \param curtime The current time used to compute the action's' frame.
	 * \param applyToObject Set to true when the action must be applied to the object,
	 * else it only manages action's' time/end.
	 * \param redundant True if the action is update twice with the same current time.
	 */
	void Update(float curtime, bool applyToObject, bool redundant);
	/**
	 * Update object IPOs (note: not thread-safe!)
	 */
	void UpdateIPOs();

	// Accessors
	float GetFrame();
	const std::string GetName();

	struct bAction *GetAction();

	// Mutators
	void SetFrame(float frame);
	void SetPlayMode(short play_mode);

	enum
	{
		ACT_MODE_PLAY = 0,
		ACT_MODE_LOOP,
		ACT_MODE_PING_PONG,
		ACT_MODE_MAX,
	};

	enum
	{
		ACT_BLEND_BLEND=0,
		ACT_BLEND_ADD=1,
		ACT_BLEND_MAX,
	};

	enum
	{
		ACT_IPOFLAG_FORCE = 1,
		ACT_IPOFLAG_LOCAL = 2,
		ACT_IPOFLAG_ADD = 4,
	};
};

#endif  /* BL_ACTION */
