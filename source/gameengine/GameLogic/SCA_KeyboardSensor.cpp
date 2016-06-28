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
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 * Sensor for keyboard input
 */

/** \file gameengine/GameLogic/SCA_KeyboardSensor.cpp
 *  \ingroup gamelogic
 */


#include <stddef.h>

#include "SCA_KeyboardSensor.h"
#include "SCA_KeyboardManager.h"
#include "SCA_LogicManager.h"
#include "EXP_StringValue.h"
#include "SCA_IInputDevice.h"

extern "C" {
	#include "BLI_string_utf8.h"
	#include "BLI_string_cursor_utf8.h"
}

/* ------------------------------------------------------------------------- */
/* Native functions                                                          */
/* ------------------------------------------------------------------------- */

SCA_KeyboardSensor::SCA_KeyboardSensor(SCA_KeyboardManager* keybdmgr,
									   short int hotkey,
									   short int qual,
									   short int qual2,
									   bool bAllKeys,
									   const STR_String& targetProp,
									   const STR_String& toggleProp,
									   SCA_IObject* gameobj,
									   short int exitKey)
	:SCA_ISensor(gameobj,keybdmgr),
	 m_hotkey(hotkey),
	 m_qual(qual),
	 m_qual2(qual2),
	 m_bAllKeys(bAllKeys),
	 m_targetprop(targetProp),
	 m_toggleprop(toggleProp)
{
	if (hotkey == exitKey) {
		keybdmgr->GetInputDevice()->SetHookExitKey(true);
	}

	m_status[0] = false;
	m_status[1] = false;
	m_status[2] = false;

	Init();
}



SCA_KeyboardSensor::~SCA_KeyboardSensor()
{
}

void SCA_KeyboardSensor::Init()
{
	// this function is used when the sensor is disconnected from all controllers
	// by the state engine. It reinitializes the sensor as if it was just created.
	// However, if the target key is pressed when the sensor is reactivated, it
	// will not generated an event (see remark in Evaluate()).
	m_val = (m_invert)?1:0;
	m_reset = true;
}

CValue* SCA_KeyboardSensor::GetReplica()
{
	SCA_KeyboardSensor* replica = new SCA_KeyboardSensor(*this);
	// this will copy properties and so on...
	replica->ProcessReplica();
	replica->Init();
	return replica;
}



short int SCA_KeyboardSensor::GetHotkey()
{
	return m_hotkey;
}



bool SCA_KeyboardSensor::IsPositiveTrigger()
{ 
	bool result = (m_val != 0);

	if (m_invert)
		result = !result;
		
	return result;
}



bool SCA_KeyboardSensor::TriggerOnAllKeys()
{ 
	return m_bAllKeys;
}



bool SCA_KeyboardSensor::Evaluate()
{
	bool result    = false;
	bool reset     = m_reset && m_level;
	bool qual[2] = {false, false};

	SCA_IInputDevice* inputdev = ((SCA_KeyboardManager *)m_eventmgr)->GetInputDevice();
	//  	cerr << "SCA_KeyboardSensor::Eval event, sensing for "<< m_hotkey << " at device " << inputdev << "\n";

	/* See if we need to do logging: togPropState exists and is
	 * different from 0 */
	CValue* myparent = GetParent();
	CValue* togPropState = myparent->GetProperty(m_toggleprop);
	if (togPropState &&
		(((int)togPropState->GetNumber()) != 0) )
	{
		LogKeystrokes();
	}

	m_reset = false;

	/* Now see whether events must be bounced. */
	if (m_bAllKeys)
	{
		bool active = false;

		for (int i=SCA_IInputDevice::BEGINKEY ; i<= SCA_IInputDevice::ENDKEY;i++) {
			const SCA_InputEvent & input = inputdev->GetInput((SCA_IInputDevice::SCA_EnumInputs) i);
			if (input.Find(SCA_InputEvent::ACTIVE)) {
				active = true;
				break;
			}
		}

		// One of all keys is active
		if (active) {
			m_val = 1;
			// The keys was not enabled before.
			if (active != m_status[0]) {
				result = true;
			}
		}
		// The keys are now disabled.
		else {
			m_val = 0;
			// The key was not disabled before.
			if (active != m_status[0]) {
				result = true;
			}
		}
		m_status[0] = active;
	}
	else {
		const SCA_InputEvent & input = inputdev->GetInput((SCA_IInputDevice::SCA_EnumInputs) m_hotkey);

		/* Check qualifier keys
		 * - see if the qualifiers we request are pressed - 'qual' true/false
		 * - see if the qualifiers we request changed their state - 'qual_change' true/false
		 */
		if (m_qual > 0) {
			const SCA_InputEvent & qualevent = inputdev->GetInput((SCA_IInputDevice::SCA_EnumInputs) m_qual);
			if (qualevent.Find(SCA_InputEvent::ACTIVE)) {
				qual[0] = true;
			}
		}
		if (m_qual2 > 0) {
			const SCA_InputEvent & qualevent = inputdev->GetInput((SCA_IInputDevice::SCA_EnumInputs) m_qual2);
			/* copy of above */
			if (qualevent.Find(SCA_InputEvent::ACTIVE)) {
				qual[1] = true;
			}
		}
		/* done reading qualifiers */

		if (input.Find(SCA_InputEvent::ACTIVE)) {
			m_val = 1;
		}
		else {
			m_val = 0;
		}

		/* Modify the key state based on qual(s)
		 * Tested carefully. don't touch unless your really sure.
		 * note, this will only change the results if key modifiers are set.
		 *
		 * When all modifiers and keys are positive
		 *  - pulse true
		 * 
		 * When ANY of the modifiers or main key become inactive,
		 *  - pulse false
		 */

		// One of the third keys value from last logic frame changed.
		if (m_status[0] != (bool)m_val || m_status[1] != qual[0] || m_status[2] != qual[1]) {
			result = true;
		}
		m_status[0] = (bool)m_val;
		m_status[1] = qual[0];
		m_status[2] = qual[1];

		if ((m_qual > 0 && !qual[0]) || (m_qual2 > 0 && !qual[1])) { /* one of the used qualifiers are not pressed */
			m_val = 0; /* since one of the qualifiers is not on, set the state to false */
		}
		/* done with key quals */
	}

	if (reset)
		// force an event
		result = true;
	return result;

}

/**
 * Tests whether shift is pressed
 */
bool SCA_KeyboardSensor::IsShifted(void)
{
	SCA_IInputDevice* inputdev = ((SCA_KeyboardManager *)m_eventmgr)->GetInputDevice();
	
	return (inputdev->GetInput(SCA_IInputDevice::RIGHTSHIFTKEY).Find(SCA_InputEvent::ACTIVE) ||
			inputdev->GetInput(SCA_IInputDevice::LEFTSHIFTKEY).Find(SCA_InputEvent::ACTIVE));
}

void SCA_KeyboardSensor::LogKeystrokes()
{
	CValue *tprop = GetParent()->GetProperty(m_targetprop);

	SCA_IInputDevice *inputdev = ((SCA_KeyboardManager *)m_eventmgr)->GetInputDevice();

	std::wstring typedtext = inputdev->GetText();
	std::wstring proptext = L"";

	{
		const char *utf8buf = tprop->GetText().ReadPtr();
		int utf8len = BLI_strlen_utf8(utf8buf);
		if (utf8len != 0) {
			wchar_t *wcharbuf = new wchar_t[utf8len + 1];
			BLI_strncpy_wchar_from_utf8(wcharbuf, utf8buf, utf8len + 1);
			proptext = wcharbuf;
			delete wcharbuf;
		}

		/* Convert all typed key in the prop string, if the key are del or
		 * backspace we remove the last string item.
		 */
		for (unsigned int i = 0, size = typedtext.size(); i < size; ++i) {
			const wchar_t item = typedtext[i];
			if (item == '\b' || item == 127) {
				if (proptext.size()) {
					proptext.resize(proptext.size() - 1);
				}
			}
			else if (item == '\r') {
				// Do nothing
			}
			else {
				proptext.push_back(item);
			}
		}
	}

	{
		STR_String newpropstr = "";

		const wchar_t *cproptext = proptext.data();
		size_t utf8len = BLI_wstrlen_utf8(cproptext);
		if (utf8len != 0) {
			char *utf8buf = new char[utf8len + 1];
			BLI_strncpy_wchar_as_utf8(utf8buf, cproptext, utf8len + 1);
			newpropstr = STR_String(utf8buf);
			delete utf8buf;
		}

		CStringValue *newstringprop = new CStringValue(newpropstr, m_targetprop);
		GetParent()->SetProperty(m_targetprop, newstringprop);
		newstringprop->Release();
	}
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python Functions						       */
/* ------------------------------------------------------------------------- */

KX_PYMETHODDEF_DOC_O(SCA_KeyboardSensor, getKeyStatus,
"getKeyStatus(keycode)\n"
"\tGet the given key's status (NONE, JUSTACTIVATED, ACTIVE or JUSTRELEASED).\n")
{
	ShowDeprecationWarning("sensor.getKeyStatus(keycode)", "logic.keyboard.events[keycode]");

	if (!PyLong_Check(value)) {
		PyErr_SetString(PyExc_ValueError, "sensor.getKeyStatus(int): Keyboard Sensor, expected an int");
		return NULL;
	}
	
	SCA_IInputDevice::SCA_EnumInputs keycode = (SCA_IInputDevice::SCA_EnumInputs)PyLong_AsLong(value);
	
	if ((keycode < SCA_IInputDevice::BEGINKEY) ||
	    (keycode > SCA_IInputDevice::ENDKEY))
	{
		PyErr_SetString(PyExc_AttributeError, "sensor.getKeyStatus(int): Keyboard Sensor, invalid keycode specified!");
		return NULL;
	}
	
	SCA_IInputDevice* inputdev = ((SCA_KeyboardManager *)m_eventmgr)->GetInputDevice();
	const SCA_InputEvent & input = inputdev->GetInput(keycode);
	return PyLong_FromLong(input.m_status[input.m_status.size() - 1]);
	Py_RETURN_NONE;
}

/* ------------------------------------------------------------------------- */
/* Python Integration Hooks					       */
/* ------------------------------------------------------------------------- */

PyTypeObject SCA_KeyboardSensor::Type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	"SCA_KeyboardSensor",
	sizeof(PyObjectPlus_Proxy),
	0,
	py_base_dealloc,
	0,
	0,
	0,
	0,
	py_base_repr,
	0,0,0,0,0,0,0,0,0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	0,0,0,0,0,0,0,
	Methods,
	0,
	0,
	&SCA_ISensor::Type,
	0,0,0,0,0,0,
	py_base_new
};

PyMethodDef SCA_KeyboardSensor::Methods[] = {
	KX_PYMETHODTABLE_O(SCA_KeyboardSensor, getKeyStatus),
	{NULL,NULL} //Sentinel
};

PyAttributeDef SCA_KeyboardSensor::Attributes[] = {
	KX_PYATTRIBUTE_RO_FUNCTION("events", SCA_KeyboardSensor, pyattr_get_events),
	KX_PYATTRIBUTE_RO_FUNCTION("inputs", SCA_KeyboardSensor, pyattr_get_inputs),
	KX_PYATTRIBUTE_BOOL_RW("useAllKeys",SCA_KeyboardSensor,m_bAllKeys),
	KX_PYATTRIBUTE_INT_RW("key",0,SCA_IInputDevice::ENDKEY,true,SCA_KeyboardSensor,m_hotkey),
	KX_PYATTRIBUTE_SHORT_RW("hold1",0,SCA_IInputDevice::ENDKEY,true,SCA_KeyboardSensor,m_qual),
	KX_PYATTRIBUTE_SHORT_RW("hold2",0,SCA_IInputDevice::ENDKEY,true,SCA_KeyboardSensor,m_qual2),
	KX_PYATTRIBUTE_STRING_RW("toggleProperty",0,MAX_PROP_NAME,false,SCA_KeyboardSensor,m_toggleprop),
	KX_PYATTRIBUTE_STRING_RW("targetProperty",0,MAX_PROP_NAME,false,SCA_KeyboardSensor,m_targetprop),
	{ NULL }	//Sentinel
};


PyObject *SCA_KeyboardSensor::pyattr_get_inputs(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	SCA_KeyboardSensor* self = static_cast<SCA_KeyboardSensor*>(self_v);

	SCA_IInputDevice* inputdev = ((SCA_KeyboardManager *)self->m_eventmgr)->GetInputDevice();

	PyObject *resultlist = PyList_New(0);
	
	for (int i=SCA_IInputDevice::BEGINKEY ; i<= SCA_IInputDevice::ENDKEY;i++)
	{
		SCA_InputEvent& input = inputdev->GetInput((SCA_IInputDevice::SCA_EnumInputs) i);
		if (input.Find(SCA_InputEvent::ACTIVE))
		{
			PyObject *keypair = PyList_New(2);
			PyList_SET_ITEM(keypair,0,PyLong_FromLong(i));
			PyList_SET_ITEM(keypair,1,input.GetProxy());
			PyList_Append(resultlist,keypair);
			Py_DECREF(keypair);
		}
	}
	return resultlist;
}

PyObject *SCA_KeyboardSensor::pyattr_get_events(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	SCA_KeyboardSensor* self = static_cast<SCA_KeyboardSensor*>(self_v);

	ShowDeprecationWarning("sensor.events", "sensor.inputs");

	SCA_IInputDevice* inputdev = ((SCA_KeyboardManager *)self->m_eventmgr)->GetInputDevice();

	PyObject *resultlist = PyList_New(0);

	for (int i=SCA_IInputDevice::BEGINKEY ; i<= SCA_IInputDevice::ENDKEY;i++)
	{
		SCA_InputEvent& input = inputdev->GetInput((SCA_IInputDevice::SCA_EnumInputs) i);
		int event = 0;
		if (input.m_queue.size() > 0) {
			event = input.m_queue[input.m_queue.size() - 1];
		}
		else {
			event = input.m_status[input.m_status.size() - 1];
		}

		if (event != SCA_InputEvent::NONE) {
			PyObject *keypair = PyList_New(2);
			PyList_SET_ITEM(keypair,0,PyLong_FromLong(i));
			PyList_SET_ITEM(keypair,1,PyLong_FromLong(event));
			PyList_Append(resultlist,keypair);
			Py_DECREF(keypair);
		}
	}
	return resultlist;
}

#endif // WITH_PYTHON
