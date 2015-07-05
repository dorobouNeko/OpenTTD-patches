/* $Id$ */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file script_controller.cpp Implementation of ScriptControler. */

#include "../../stdafx.h"
#include "../../string.h"
#include "../../script/squirrel.hpp"
#include "../../rev.h"

#include "script_controller.hpp"
#include "script_error.hpp"
#include "../script_fatalerror.hpp"
#include "../script_info.hpp"
#include "../script_instance.hpp"
#include "script_log.hpp"
#include "../../ai/ai_gui.hpp"
#include "../../settings_type.h"
#include "../../network/network.h"

/* static */ void ScriptController::SetCommandDelay(int ticks)
{
	if (ticks <= 0) return;
	ScriptObject::SetDoCommandDelay(ticks);
}

/* static */ void ScriptController::Sleep(int ticks)
{
	if (!ScriptObject::CanSuspend()) {
		throw Script_FatalError("You are not allowed to call Sleep in your constructor, Save(), Load(), and any valuator.");
	}

	if (ticks <= 0) {
		ScriptLog::Warning("Sleep() value should be > 0. Assuming value 1.");
		ticks = 1;
	}

	throw Script_Suspend(ticks, NULL);
}

/* static */ void ScriptController::Break(const char* message)
{
	if (_network_dedicated || !_settings_client.gui.ai_developer_tools) return;

	ScriptObject::GetActiveInstance()->Pause();

	char log_message[1024];
	bstrfmt (log_message, "Break: %s", message);
	ScriptLog::Log(ScriptLog::LOG_SQ_ERROR, log_message);

	/* Inform script developer that his script has been paused and
	 * needs manual action to continue. */
	ShowAIDebugWindow(ScriptObject::GetRootCompany());

	if ((_pause_mode & PM_PAUSED_NORMAL) == PM_UNPAUSED) {
		ScriptObject::DoCommand(0, PM_PAUSED_NORMAL, 1, CMD_PAUSE);
	}
}

/* static */ void ScriptController::Print(bool error_msg, const char *message)
{
	ScriptLog::Log(error_msg ? ScriptLog::LOG_SQ_ERROR : ScriptLog::LOG_SQ_INFO, message);
}

ScriptController::ScriptController(CompanyID company) :
	ticks(0),
	loaded_library_count(0)
{
	ScriptObject::SetCompany(company);
}

ScriptController::~ScriptController()
{
	for (LoadedLibraryList::iterator iter = this->loaded_library.begin(); iter != this->loaded_library.end(); iter++) {
		free((*iter).second);
		free((*iter).first);
	}

	this->loaded_library.clear();
}

/* static */ uint ScriptController::GetTick()
{
	return ScriptObject::GetActiveInstance()->GetController()->ticks;
}

/* static */ int ScriptController::GetOpsTillSuspend()
{
	return ScriptObject::GetActiveInstance()->GetOpsTillSuspend();
}

/* static */ int ScriptController::GetSetting(const char *name)
{
	return ScriptObject::GetActiveInstance()->GetSetting(name);
}

/* static */ uint ScriptController::GetVersion()
{
	return _openttd_newgrf_version;
}

/* static */ SQInteger ScriptController::Import (HSQUIRRELVM vm)
{
	ttd_unique_free_ptr<char> library_ptr (SQConvert::GetString (vm, 2));
	ttd_unique_free_ptr<char> cname_ptr   (SQConvert::GetString (vm, 3));
	int version = SQConvert::GetInteger (vm, 4);
	const char *library    = library_ptr.get();
	const char *class_name = cname_ptr.get();

	ScriptController *controller = ScriptObject::GetActiveInstance()->GetController();
	Squirrel *engine = ScriptObject::GetActiveInstance()->engine;
	assert (engine->GetVM() == vm);

	/* Internally we store libraries as 'library.version' */
	char library_name[1024];
	bstrfmt (library_name, "%s.%d", library, version);
	strtolower(library_name);

	ScriptInfo *lib = ScriptObject::GetActiveInstance()->FindLibrary(library, version);
	if (lib == NULL) {
		char error[1024];
		bstrfmt (error, "couldn't find library '%s' with version %d", library, version);
		return sq_throwerror (vm, error);
	}

	/* Get the current table/class we belong to */
	HSQOBJECT parent;
	sq_getstackobj(vm, 1, &parent);

	char fake_class[1024];

	LoadedLibraryList::iterator iter = controller->loaded_library.find(library_name);
	if (iter != controller->loaded_library.end()) {
		bstrcpy (fake_class, (*iter).second);
	} else {
		int next_number = ++controller->loaded_library_count;

		/* Create a new fake internal name */
		bstrfmt (fake_class, "_internalNA%d", next_number);

		/* Load the library in a 'fake' namespace, so we can link it to the name the user requested */
		sq_pushroottable(vm);
		sq_pushstring(vm, fake_class, -1);
		sq_newclass(vm, SQFalse);
		/* Load the library */
		if (!engine->LoadScript(vm, lib->GetMainScript(), false)) {
			char error[1024];
			bstrfmt (error, "there was a compile error when importing '%s' version %d", library, version);
			return sq_throwerror (vm, error);
		}
		/* Create the fake class */
		sq_newslot(vm, -3, SQFalse);
		sq_pop(vm, 1);

		controller->loaded_library[xstrdup(library_name)] = xstrdup(fake_class);
	}

	/* Find the real class inside the fake class (like 'sets.Vector') */
	sq_pushroottable(vm);
	sq_pushstring(vm, fake_class, -1);
	if (SQ_FAILED(sq_get(vm, -2))) {
		return sq_throwerror (vm, "internal error assigning library class");
	}
	sq_pushstring(vm, lib->GetInstanceName(), -1);
	if (SQ_FAILED(sq_get(vm, -2))) {
		char error[1024];
		bstrfmt (error, "unable to find class '%s' in the library '%s' version %d", lib->GetInstanceName(), library, version);
		return sq_throwerror (vm, error);
	}
	HSQOBJECT obj;
	sq_getstackobj(vm, -1, &obj);
	sq_pop(vm, 3);

	if (!StrEmpty(class_name)) {
		/* Now link the name the user wanted to our 'fake' class */
		sq_pushobject (vm, parent);
		sq_pushstring (vm, class_name, -1);
		sq_pushobject (vm, obj);
		sq_newclass (vm, SQTrue);
		sq_newslot (vm, -3, SQFalse);
		sq_pop (vm, 1);
	}

	sq_pushobject (vm, obj);
	return 1;
}

void SQController_Register (Squirrel *engine, const char *name)
{
	engine->AddClassBegin (name);
	SQConvert::DefSQStaticMethod (engine, &ScriptController::GetTick,           "GetTick",           1, ".");
	SQConvert::DefSQStaticMethod (engine, &ScriptController::GetOpsTillSuspend, "GetOpsTillSuspend", 1, ".");
	SQConvert::DefSQStaticMethod (engine, &ScriptController::SetCommandDelay,   "SetCommandDelay",   2, ".i");
	SQConvert::DefSQStaticMethod (engine, &ScriptController::Sleep,             "Sleep",             2, ".i");
	SQConvert::DefSQStaticMethod (engine, &ScriptController::Break,             "Break",             2, ".s");
	SQConvert::DefSQStaticMethod (engine, &ScriptController::GetSetting,        "GetSetting",        2, ".s");
	SQConvert::DefSQStaticMethod (engine, &ScriptController::GetVersion,        "GetVersion",        1, ".");
	SQConvert::DefSQStaticMethod (engine, &ScriptController::Print,             "Print",             3, ".bs");
	engine->AddClassEnd();

	/* Register the import statement to the global scope */
	engine->AddMethod ("import", &ScriptController::Import, 4, ".ssi");
}
