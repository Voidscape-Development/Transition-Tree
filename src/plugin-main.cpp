/*
Transition Tree
Copyright (C) 2026 Voidscape Development <dev@voidscape.dev>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "transition-tree.hpp"
#include "transition-tree-dialog.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QAction>

#include <string>

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("Voidscape Development")
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

/* Mirrors Transition Table's proc so tooling can ask what the tree would do for
 * a given switch without going through obs-websocket. */
static void ProcGetTransition(void *, calldata_t *callData)
{
	const char *value = nullptr;

	std::string canvas;
	if (calldata_get_string(callData, "canvas", &value) && value)
		canvas = value;
	else
		canvas = tt::MainCanvasName();

	value = nullptr;
	std::string fromScene;
	if (calldata_get_string(callData, "from_scene", &value) && value)
		fromScene = value;

	value = nullptr;
	std::string toScene;
	if (calldata_get_string(callData, "to_scene", &value) && value)
		toScene = value;

	std::string transition;
	int duration = 0;
	tt::ResolveTransition(canvas, fromScene, toScene, transition, duration);

	calldata_set_string(callData, "transition", transition.c_str());
	calldata_set_int(callData, "duration", duration);
}

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "loading Transition Tree version %s", PLUGIN_VERSION);

	auto *action = (QAction *)obs_frontend_add_tools_menu_qaction(obs_module_text("TransitionTree"));
	if (action)
		QAction::connect(action, &QAction::triggered, []() { TransitionTreeDialog::ShowDialog(); });

	tt::EngineInit();

	proc_handler_add(
		obs_get_proc_handler(),
		"void get_transition_tree_transition(string canvas, string from_scene, string to_scene, out string transition, out int duration)",
		ProcGetTransition, nullptr);

	return true;
}

void obs_module_post_load(void)
{
	tt::WebsocketInit();
}

void obs_module_unload(void)
{
	tt::WebsocketShutdown();
	tt::EngineShutdown();
	obs_log(LOG_INFO, "Transition Tree unloaded");
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Description");
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return obs_module_text("TransitionTree");
}
