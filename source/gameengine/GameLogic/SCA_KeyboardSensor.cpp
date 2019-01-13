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


#include "SCA_KeyboardSensor.h"
#include "SCA_KeyboardManager.h"
#include "SCA_LogicManager.h"
#include "EXP_StringValue.h"
#include "SCA_IInputDevice.h"

#include <locale>
#include <codecvt>

/* ------------------------------------------------------------------------- */
/* Native functions                                                          */
/* ------------------------------------------------------------------------- */

SCA_KeyboardSensor::SCA_KeyboardSensor(SCA_KeyboardManager *keybdmgr,
                                       short int hotkey,
                                       short int qual,
                                       short int qual2,
                                       bool bAllKeys,
                                       const std::string& targetProp,
                                       const std::string& toggleProp,
                                       SCA_IObject *gameobj,
                                       short int exitKey)
	:SCA_ISensor(gameobj, keybdmgr),
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
	m_val = (m_invert) ? 1 : 0;
	m_reset = true;
}

EXP_Value *SCA_KeyboardSensor::GetReplica()
{
	SCA_KeyboardSensor *replica = new SCA_KeyboardSensor(*this);
	// this will copy properties and so on...
	replica->ProcessReplica();
	replica->Init();
	return replica;
}

bool SCA_KeyboardSensor::IsPositiveTrigger()
{
	bool result = (m_val != 0);

	if (m_invert) {
		result = !result;
	}

	return result;
}

bool SCA_KeyboardSensor::Evaluate()
{
	bool result    = false;
	bool reset     = m_reset && m_level;

	SCA_IInputDevice *inputdev = ((SCA_KeyboardManager *)m_eventmgr)->GetInputDevice();
	//      cerr << "SCA_KeyboardSensor::Eval event, sensing for "<< m_hotkey << " at device " << inputdev << "\n";

	/* See if we need to do logging: togPropState exists and is
	 * different from 0 */
	EXP_Value *myparent = GetParent();
	EXP_Value *togPropState = myparent->GetProperty(m_toggleprop);
	if (togPropState &&
	    (((int)togPropState->GetNumber()) != 0)) {
		LogKeystrokes();
	}

	m_reset = false;

	/* Now see whether events must be bounced. */
	if (m_bAllKeys) {
		bool status = false;
		bool events = false;

		for (int i = SCA_IInputDevice::BEGINKEY; i <= SCA_IInputDevice::ENDKEY; ++i) {
			const SCA_InputEvent& input = inputdev->GetInput((SCA_IInputDevice::SCA_EnumInputs)i);
			if (input.End(SCA_InputEvent::ACTIVE)) {
				status = true;
				break;
			}
		}

		for (int i = SCA_IInputDevice::BEGINKEY; i <= SCA_IInputDevice::ENDKEY; ++i) {
			const SCA_InputEvent& input = inputdev->GetInput((SCA_IInputDevice::SCA_EnumInputs)i);
			if (!input.m_queue.empty()) {
				events = true;
				break;
			}
		}

		m_val = status;
		result = events;
	}
	else {
		bool status[3] = {false, false, false};
		bool events[3] = {false, false, false};
		const SCA_InputEvent & input = inputdev->GetInput((SCA_IInputDevice::SCA_EnumInputs)m_hotkey);

		/* Check qualifier keys
		 * - see if the qualifiers we request are pressed - 'qual' true/false
		 * - see if the qualifiers we request changed their state - 'qual_change' true/false
		 */
		if (m_qual > 0) {
			const SCA_InputEvent & qualevent = inputdev->GetInput((SCA_IInputDevice::SCA_EnumInputs)m_qual);
			status[1] = qualevent.End(SCA_InputEvent::ACTIVE);
			events[1] = !qualevent.m_queue.empty();
		}
		if (m_qual2 > 0) {
			const SCA_InputEvent & qualevent = inputdev->GetInput((SCA_IInputDevice::SCA_EnumInputs)m_qual2);
			/* copy of above */
			status[2] = qualevent.End(SCA_InputEvent::ACTIVE);
			events[2] = !qualevent.m_queue.empty();
		}
		/* done reading qualifiers */

		status[0] = input.End(SCA_InputEvent::ACTIVE);
		events[0] = !input.m_queue.empty();

		/* Modify the key state based on qual(s)
		 * Tested carefully. don't touch unless your really sure.
		 * note, this will only change the results if key modifiers are set.
		 *
		 * When all modifiers and keys are positive
		 * - pulse true
		 *
		 * When ANY of the modifiers or main key become inactive,
		 * - pulse false
		 */

		// One of the third keys value from last logic frame changed.
		if (events[0] || events[1] || events[2]) {
			result = true;
		}

		if (!status[0] || (m_qual > 0 && !status[1]) || (m_qual2 > 0 && !status[2])) { /* one of the used qualifiers are not pressed */
			m_val = false; /* since one of the qualifiers is not on, set the state to false */
		}
		else {
			m_val = true;
		}
		/* done with key quals */
	}

	if (reset) {
		// force an event
		result = true;
	}
	return result;

}

void SCA_KeyboardSensor::LogKeystrokes()
{
	EXP_Value *tprop = GetParent()->GetProperty(m_targetprop);

	SCA_IInputDevice *inputdev = ((SCA_KeyboardManager *)m_eventmgr)->GetInputDevice();

	std::wstring_convert<std::codecvt_utf8<wchar_t> > converter;
	const std::wstring typedtext = inputdev->GetText();
	std::wstring proptext = converter.from_bytes(tprop->GetText());

	/* Convert all typed key in the prop string, if the key are del or
	 * backspace we remove the last string item.
	 */
	for (const wchar_t item : typedtext) {
		if (item == '\b' || item == 127) {
			if (!proptext.empty()) {
				proptext.resize(proptext.size() - 1);
			}
		}
		else if (item == '\r') {
			proptext.push_back('\n');
		}
		else {
			proptext.push_back(item);
		}
	}

	EXP_StringValue *newstringprop = new EXP_StringValue(converter.to_bytes(proptext), m_targetprop);
	GetParent()->SetProperty(m_targetprop, newstringprop);
	newstringprop->Release();
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python Functions						       */
/* ------------------------------------------------------------------------- */

EXP_PYMETHODDEF_DOC_O(SCA_KeyboardSensor, getKeyStatus,
                      "getKeyStatus(keycode)\n"
                      "\tGet the given key's status (NONE, JUSTACTIVATED, ACTIVE or JUSTRELEASED).\n")
{
	EXP_ShowDeprecationWarning("sensor.getKeyStatus(keycode)", "logic.keyboard.events[keycode]");

	if (!PyLong_Check(value)) {
		PyErr_SetString(PyExc_ValueError, "sensor.getKeyStatus(int): Keyboard Sensor, expected an int");
		return nullptr;
	}

	SCA_IInputDevice::SCA_EnumInputs keycode = (SCA_IInputDevice::SCA_EnumInputs)PyLong_AsLong(value);

	if ((keycode < SCA_IInputDevice::BEGINKEY) ||
	    (keycode > SCA_IInputDevice::ENDKEY)) {
		PyErr_SetString(PyExc_AttributeError, "sensor.getKeyStatus(int): Keyboard Sensor, invalid keycode specified!");
		return nullptr;
	}

	SCA_IInputDevice *inputdev = ((SCA_KeyboardManager *)m_eventmgr)->GetInputDevice();
	const SCA_InputEvent & input = inputdev->GetInput(keycode);
	return PyLong_FromLong(input.m_status[input.m_status.size() - 1]);
	Py_RETURN_NONE;
}

/* ------------------------------------------------------------------------- */
/* Python Integration Hooks					       */
/* ------------------------------------------------------------------------- */

PyTypeObject SCA_KeyboardSensor::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"SCA_KeyboardSensor",
	sizeof(EXP_PyObjectPlus_Proxy),
	0,
	py_base_dealloc,
	0,
	0,
	0,
	0,
	py_base_repr,
	0, 0, 0, 0, 0, 0, 0, 0, 0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	0, 0, 0, 0, 0, 0, 0,
	Methods,
	0,
	0,
	&SCA_ISensor::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyMethodDef SCA_KeyboardSensor::Methods[] = {
	EXP_PYMETHODTABLE_O(SCA_KeyboardSensor, getKeyStatus),
	{nullptr, nullptr} //Sentinel
};

PyAttributeDef SCA_KeyboardSensor::Attributes[] = {
	EXP_PYATTRIBUTE_RO_FUNCTION("events", SCA_KeyboardSensor, pyattr_get_events),
	EXP_PYATTRIBUTE_RO_FUNCTION("inputs", SCA_KeyboardSensor, pyattr_get_inputs),
	EXP_PYATTRIBUTE_BOOL_RW("useAllKeys", SCA_KeyboardSensor, m_bAllKeys),
	EXP_PYATTRIBUTE_INT_RW("key", 0, SCA_IInputDevice::ENDKEY, true, SCA_KeyboardSensor, m_hotkey),
	EXP_PYATTRIBUTE_SHORT_RW("hold1", 0, SCA_IInputDevice::ENDKEY, true, SCA_KeyboardSensor, m_qual),
	EXP_PYATTRIBUTE_SHORT_RW("hold2", 0, SCA_IInputDevice::ENDKEY, true, SCA_KeyboardSensor, m_qual2),
	EXP_PYATTRIBUTE_STRING_RW("toggleProperty", 0, MAX_PROP_NAME, false, SCA_KeyboardSensor, m_toggleprop),
	EXP_PYATTRIBUTE_STRING_RW("targetProperty", 0, MAX_PROP_NAME, false, SCA_KeyboardSensor, m_targetprop),
	EXP_PYATTRIBUTE_NULL    //Sentinel
};


PyObject *SCA_KeyboardSensor::pyattr_get_inputs(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	SCA_KeyboardSensor *self = static_cast<SCA_KeyboardSensor *>(self_v);

	SCA_IInputDevice *inputdev = ((SCA_KeyboardManager *)self->m_eventmgr)->GetInputDevice();

	PyObject *dict = PyDict_New();

	for (int i = SCA_IInputDevice::BEGINKEY; i <= SCA_IInputDevice::ENDKEY; i++)
	{
		SCA_InputEvent& input = inputdev->GetInput((SCA_IInputDevice::SCA_EnumInputs)i);
		if (input.Find(SCA_InputEvent::ACTIVE)) {
			PyObject *key = PyLong_FromLong(i);

			PyDict_SetItem(dict, key, input.GetProxy());

			Py_DECREF(key);
		}
	}
	return dict;
}

PyObject *SCA_KeyboardSensor::pyattr_get_events(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	SCA_KeyboardSensor *self = static_cast<SCA_KeyboardSensor *>(self_v);

	EXP_ShowDeprecationWarning("sensor.events", "sensor.inputs");

	SCA_IInputDevice *inputdev = ((SCA_KeyboardManager *)self->m_eventmgr)->GetInputDevice();

	PyObject *resultlist = PyList_New(0);

	for (int i = SCA_IInputDevice::BEGINKEY; i <= SCA_IInputDevice::ENDKEY; i++)
	{
		SCA_InputEvent& input = inputdev->GetInput((SCA_IInputDevice::SCA_EnumInputs)i);
		int event = 0;
		if (input.m_queue.empty()) {
			event = input.m_status[input.m_status.size() - 1];
		}
		else {
			event = input.m_queue[input.m_queue.size() - 1];
		}

		if (event != SCA_InputEvent::NONE) {
			PyObject *keypair = PyList_New(2);
			PyList_SET_ITEM(keypair, 0, PyLong_FromLong(i));
			PyList_SET_ITEM(keypair, 1, PyLong_FromLong(event));
			PyList_Append(resultlist, keypair);
			Py_DECREF(keypair);
		}
	}
	return resultlist;
}

#endif // WITH_PYTHON
