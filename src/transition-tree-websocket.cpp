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
#include "obs-websocket-api.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <QMainWindow>
#include <QMetaObject>
#include <QThread>

#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace tt {

namespace {

obs_websocket_vendor g_vendor = nullptr;

/* Vendor requests arrive on obs-websocket's thread, while the model is owned by
 * the Qt main thread. Everything is funnelled through here so requests both
 * stay race-free and can still answer synchronously. */
void RunSync(const std::function<void()> &task)
{
	auto *mainWindow = (QMainWindow *)obs_frontend_get_main_window();
	if (!mainWindow || QThread::currentThread() == mainWindow->thread()) {
		task();
		return;
	}
	QMetaObject::invokeMethod(mainWindow, task, Qt::BlockingQueuedConnection);
}

void Fail(obs_data_t *response, const char *message)
{
	obs_data_set_bool(response, "success", false);
	obs_data_set_string(response, "error", message);
}

void Succeed(obs_data_t *response)
{
	obs_data_set_bool(response, "success", true);
}

std::string RequestedCanvas(obs_data_t *request)
{
	const char *canvas = obs_data_get_string(request, "canvas");
	if (canvas && *canvas)
		return canvas;
	return MainCanvasName();
}

const char *MatchTypeName(MatchType type)
{
	switch (type) {
	case MatchType::Any:
		return "any";
	case MatchType::AnyExcept:
		return "any_except";
	case MatchType::Scenes:
	default:
		return "scenes";
	}
}

const char *ModeName(SelectionMode mode)
{
	switch (mode) {
	case SelectionMode::Random:
		return "random";
	case SelectionMode::RandomNoRepeat:
		return "random_no_repeat";
	case SelectionMode::Weighted:
		return "weighted";
	case SelectionMode::Sequential:
	default:
		return "sequential";
	}
}

bool ParseMode(const char *name, SelectionMode &mode)
{
	if (!name || !*name)
		return false;
	if (strcmp(name, "sequential") == 0)
		mode = SelectionMode::Sequential;
	else if (strcmp(name, "random") == 0)
		mode = SelectionMode::Random;
	else if (strcmp(name, "random_no_repeat") == 0)
		mode = SelectionMode::RandomNoRepeat;
	else if (strcmp(name, "weighted") == 0)
		mode = SelectionMode::Weighted;
	else
		return false;
	return true;
}

obs_data_t *SerializeMatcher(const SceneMatcher &matcher)
{
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "type", MatchTypeName(matcher.type));

	obs_data_array_t *scenes = obs_data_array_create();
	for (const auto &scene : matcher.scenes) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "name", scene.c_str());
		obs_data_array_push_back(scenes, item);
		obs_data_release(item);
	}
	obs_data_set_array(data, "scenes", scenes);
	obs_data_array_release(scenes);

	return data;
}

/* Accepts either a plain scene name (Transition Table style, where the literal
 * "Any" is the wildcard) or a matcher object. */
bool ParseMatcher(obs_data_t *request, const char *objectKey, const char *stringKey, SceneMatcher &matcher)
{
	obs_data_t *object = obs_data_get_obj(request, objectKey);
	if (object) {
		const char *type = obs_data_get_string(object, "type");
		if (type && strcmp(type, "any") == 0)
			matcher.type = MatchType::Any;
		else if (type && strcmp(type, "any_except") == 0)
			matcher.type = MatchType::AnyExcept;
		else
			matcher.type = MatchType::Scenes;

		matcher.scenes.clear();
		obs_data_array_t *scenes = obs_data_get_array(object, "scenes");
		if (scenes) {
			const size_t count = obs_data_array_count(scenes);
			for (size_t i = 0; i < count; i++) {
				obs_data_t *item = obs_data_array_item(scenes, i);
				/* Tolerate both ["a","b"] and [{"name":"a"}]. */
				const char *name = obs_data_get_string(item, "name");
				if (name && *name)
					matcher.scenes.push_back(name);
				obs_data_release(item);
			}
			obs_data_array_release(scenes);
		}

		obs_data_release(object);
		return true;
	}

	const char *scene = obs_data_get_string(request, stringKey);
	if (!scene || !*scene)
		return false;

	if (strcmp(scene, "Any") == 0) {
		matcher.type = MatchType::Any;
		matcher.scenes.clear();
	} else {
		matcher.type = MatchType::Scenes;
		matcher.scenes.assign(1, scene);
	}
	return true;
}

obs_data_t *SerializeMark(const Mark &mark)
{
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "id", mark.id.c_str());
	obs_data_set_string(data, "canvas", mark.canvas.empty() ? MainCanvasName().c_str() : mark.canvas.c_str());
	obs_data_set_string(data, "mode", ModeName(mark.mode));
	obs_data_set_bool(data, "enabled", mark.enabled);

	obs_data_t *from = SerializeMatcher(mark.from);
	obs_data_set_obj(data, "from", from);
	obs_data_release(from);

	obs_data_t *to = SerializeMatcher(mark.to);
	obs_data_set_obj(data, "to", to);
	obs_data_release(to);

	obs_data_array_t *candidates = obs_data_array_create();
	for (const auto &candidate : mark.candidates) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "transition", candidate.transition.c_str());
		obs_data_set_int(item, "duration", candidate.duration);
		obs_data_set_double(item, "weight", candidate.weight);
		obs_data_set_bool(item, "enabled", candidate.enabled);
		obs_data_array_push_back(candidates, item);
		obs_data_release(item);
	}
	obs_data_set_array(data, "transitions", candidates);
	obs_data_array_release(candidates);

	return data;
}

Preset *PresetForRequest(obs_data_t *request)
{
	const char *name = obs_data_get_string(request, "preset");
	if (name && *name)
		return GetStore().Find(name);
	return GetStore().Active();
}

/* ------------------------------------------------------------------------ */
/* Preset requests                                                           */
/* ------------------------------------------------------------------------ */

void RequestGetPresets(obs_data_t *, obs_data_t *response, void *)
{
	RunSync([&]() {
		Store &store = GetStore();

		obs_data_array_t *presets = obs_data_array_create();
		for (const auto &preset : store.presets) {
			obs_data_t *item = obs_data_create();
			obs_data_set_string(item, "name", preset.name.c_str());
			obs_data_set_bool(item, "global", preset.global);
			obs_data_set_bool(item, "active", preset.name == store.activePreset);
			obs_data_set_int(item, "mark_count", (long long)preset.marks.size());
			obs_data_array_push_back(presets, item);
			obs_data_release(item);
		}

		obs_data_set_array(response, "presets", presets);
		obs_data_array_release(presets);
		obs_data_set_string(response, "active_preset", store.activePreset.c_str());
		obs_data_set_bool(response, "enabled", store.enabled);
		Succeed(response);
	});
}

void RequestGetActivePreset(obs_data_t *, obs_data_t *response, void *)
{
	RunSync([&]() {
		Store &store = GetStore();
		obs_data_set_string(response, "preset", store.activePreset.c_str());
		obs_data_set_bool(response, "enabled", store.enabled);
		Succeed(response);
	});
}

void RequestSetActivePreset(obs_data_t *request, obs_data_t *response, void *)
{
	const char *name = obs_data_get_string(request, "preset");
	if (!name) {
		Fail(response, "'preset' not set");
		return;
	}
	const std::string wanted = name;

	RunSync([&]() {
		if (!wanted.empty() && !GetStore().Find(wanted)) {
			Fail(response, "preset not found");
			return;
		}
		SetActivePreset(wanted);
		obs_data_set_string(response, "preset", GetStore().activePreset.c_str());
		Succeed(response);
	});
}

void RequestCyclePreset(obs_data_t *, obs_data_t *response, void *param)
{
	const int delta = param ? *(int *)param : 1;
	RunSync([&]() {
		CycleActivePreset(delta);
		obs_data_set_string(response, "preset", GetStore().activePreset.c_str());
		Succeed(response);
	});
}

void RequestSetEnabled(obs_data_t *request, obs_data_t *response, void *)
{
	if (!obs_data_has_user_value(request, "enabled")) {
		Fail(response, "'enabled' not set");
		return;
	}
	const bool enabled = obs_data_get_bool(request, "enabled");

	RunSync([&]() {
		SetTreeEnabled(enabled);
		obs_data_set_bool(response, "enabled", GetStore().enabled);
		Succeed(response);
	});
}

/* ------------------------------------------------------------------------ */
/* Tree requests                                                             */
/* ------------------------------------------------------------------------ */

void RequestGetTree(obs_data_t *request, obs_data_t *response, void *)
{
	RunSync([&]() {
		Preset *preset = PresetForRequest(request);
		if (!preset) {
			Fail(response, "no preset active");
			return;
		}

		obs_data_array_t *marks = obs_data_array_create();
		for (const auto &mark : preset->marks) {
			obs_data_t *item = SerializeMark(mark);
			obs_data_array_push_back(marks, item);
			obs_data_release(item);
		}

		obs_data_set_string(response, "preset", preset->name.c_str());
		obs_data_set_array(response, "marks", marks);
		obs_data_array_release(marks);
		Succeed(response);
	});
}

void RequestSetMark(obs_data_t *request, obs_data_t *response, void *)
{
	RunSync([&]() {
		Preset *preset = PresetForRequest(request);
		if (!preset) {
			Fail(response, "no preset active");
			return;
		}

		Mark *existing = nullptr;
		const char *id = obs_data_get_string(request, "id");
		if (id && *id) {
			existing = preset->FindMark(id);
			if (!existing) {
				Fail(response, "mark not found");
				return;
			}
		}

		Mark mark = existing ? *existing : Mark();
		if (!existing) {
			mark.id = "mark-ws-" + std::to_string(os_gettime_ns());
			mark.canvas = RequestedCanvas(request);
		} else if (obs_data_has_user_value(request, "canvas")) {
			mark.canvas = RequestedCanvas(request);
		}

		if (!ParseMatcher(request, "from", "from_scene", mark.from) && !existing) {
			Fail(response, "'from' or 'from_scene' not set");
			return;
		}
		if (!ParseMatcher(request, "to", "to_scene", mark.to) && !existing) {
			Fail(response, "'to' or 'to_scene' not set");
			return;
		}

		SelectionMode mode = mark.mode;
		if (ParseMode(obs_data_get_string(request, "mode"), mode))
			mark.mode = mode;

		if (obs_data_has_user_value(request, "enabled"))
			mark.enabled = obs_data_get_bool(request, "enabled");

		obs_data_array_t *transitions = obs_data_get_array(request, "transitions");
		if (transitions) {
			mark.candidates.clear();
			const size_t count = obs_data_array_count(transitions);
			for (size_t i = 0; i < count; i++) {
				obs_data_t *item = obs_data_array_item(transitions, i);
				Candidate candidate;
				const char *name = obs_data_get_string(item, "transition");
				candidate.transition = name ? name : "";
				obs_data_set_default_int(item, "duration", 300);
				obs_data_set_default_double(item, "weight", 1.0);
				obs_data_set_default_bool(item, "enabled", true);
				candidate.duration = (int)obs_data_get_int(item, "duration");
				candidate.weight = obs_data_get_double(item, "weight");
				candidate.enabled = obs_data_get_bool(item, "enabled");
				if (!candidate.transition.empty())
					mark.candidates.push_back(candidate);
				obs_data_release(item);
			}
			obs_data_array_release(transitions);
		} else if (obs_data_has_user_value(request, "transition")) {
			Candidate candidate;
			candidate.transition = obs_data_get_string(request, "transition");
			obs_data_set_default_int(request, "duration", 300);
			candidate.duration = (int)obs_data_get_int(request, "duration");
			mark.candidates.assign(1, candidate);
		}

		if (mark.candidates.empty()) {
			Fail(response, "mark needs at least one transition");
			return;
		}

		mark.ResetRuntimeState();

		if (existing)
			*existing = mark;
		else
			preset->marks.push_back(mark);

		GetStore().Touch();
		if (preset->global)
			SaveGlobalPresets();
		ApplyOverrides();
		TransitionTreeDialog::NotifyDataChanged();

		obs_data_set_string(response, "id", mark.id.c_str());
		Succeed(response);
	});
}

void RequestRemoveMark(obs_data_t *request, obs_data_t *response, void *)
{
	RunSync([&]() {
		Preset *preset = PresetForRequest(request);
		if (!preset) {
			Fail(response, "no preset active");
			return;
		}

		const char *id = obs_data_get_string(request, "id");
		if (!id || !*id) {
			Fail(response, "'id' not set");
			return;
		}

		bool removed = false;
		for (auto it = preset->marks.begin(); it != preset->marks.end(); ++it) {
			if (it->id == id) {
				preset->marks.erase(it);
				removed = true;
				break;
			}
		}

		if (!removed) {
			Fail(response, "mark not found");
			return;
		}

		GetStore().Touch();
		if (preset->global)
			SaveGlobalPresets();
		ApplyOverrides();
		TransitionTreeDialog::NotifyDataChanged();
		Succeed(response);
	});
}

void RequestSetMarkEnabled(obs_data_t *request, obs_data_t *response, void *)
{
	RunSync([&]() {
		Preset *preset = PresetForRequest(request);
		if (!preset) {
			Fail(response, "no preset active");
			return;
		}

		const char *id = obs_data_get_string(request, "id");
		if (!id || !*id) {
			Fail(response, "'id' not set");
			return;
		}
		if (!obs_data_has_user_value(request, "enabled")) {
			Fail(response, "'enabled' not set");
			return;
		}

		Mark *mark = preset->FindMark(id);
		if (!mark) {
			Fail(response, "mark not found");
			return;
		}

		mark->enabled = obs_data_get_bool(request, "enabled");

		GetStore().Touch();
		if (preset->global)
			SaveGlobalPresets();
		ApplyOverrides();
		TransitionTreeDialog::NotifyDataChanged();
		Succeed(response);
	});
}

/* ------------------------------------------------------------------------ */
/* Transition Table compatible requests                                      */
/* ------------------------------------------------------------------------ */

void RequestGetTransition(obs_data_t *request, obs_data_t *response, void *)
{
	RunSync([&]() {
		const std::string canvas = RequestedCanvas(request);
		const char *fromScene = obs_data_get_string(request, "from_scene");
		const char *toScene = obs_data_get_string(request, "to_scene");

		std::string transition;
		int duration = 0;
		ResolveTransition(canvas, fromScene ? fromScene : "", toScene ? toScene : "", transition, duration);

		obs_data_set_string(response, "transition", transition.c_str());
		obs_data_set_int(response, "duration", duration);
		Succeed(response);
	});
}

void RequestSetTransition(obs_data_t *request, obs_data_t *response, void *)
{
	RunSync([&]() {
		Preset *preset = PresetForRequest(request);
		if (!preset) {
			Fail(response, "no preset active");
			return;
		}

		const char *fromScene = obs_data_get_string(request, "from_scene");
		const char *toScene = obs_data_get_string(request, "to_scene");
		if (!fromScene || !*fromScene) {
			Fail(response, "'from_scene' not set");
			return;
		}
		if (!toScene || !*toScene) {
			Fail(response, "'to_scene' not set");
			return;
		}

		SceneMatcher from;
		SceneMatcher to;
		ParseMatcher(request, "from", "from_scene", from);
		ParseMatcher(request, "to", "to_scene", to);

		const std::string canvas = RequestedCanvas(request);

		/* Transition Table addresses entries purely by from/to, so match the
		 * first mark that covers exactly the same pair. */
		Mark *existing = nullptr;
		for (auto &mark : preset->marks) {
			const std::string markCanvas = mark.canvas.empty() ? MainCanvasName() : mark.canvas;
			if (markCanvas == canvas && mark.from.type == from.type && mark.from.scenes == from.scenes &&
			    mark.to.type == to.type && mark.to.scenes == to.scenes) {
				existing = &mark;
				break;
			}
		}

		const char *transition = obs_data_get_string(request, "transition");
		if (!transition || !*transition) {
			/* An empty transition deletes, matching Transition Table. */
			if (!existing) {
				Fail(response, "no matching mark to remove");
				return;
			}
			for (auto it = preset->marks.begin(); it != preset->marks.end(); ++it) {
				if (&(*it) == existing) {
					preset->marks.erase(it);
					break;
				}
			}
		} else {
			obs_data_set_default_int(request, "duration", 300);
			Candidate candidate;
			candidate.transition = transition;
			candidate.duration = (int)obs_data_get_int(request, "duration");

			if (existing) {
				existing->candidates.assign(1, candidate);
				existing->ResetRuntimeState();
			} else {
				Mark mark;
				mark.id = "mark-ws-" + std::to_string(os_gettime_ns());
				mark.canvas = canvas;
				mark.from = from;
				mark.to = to;
				mark.candidates.assign(1, candidate);
				preset->marks.push_back(mark);
			}
		}

		GetStore().Touch();
		if (preset->global)
			SaveGlobalPresets();
		ApplyOverrides();
		TransitionTreeDialog::NotifyDataChanged();
		Succeed(response);
	});
}

void RequestGetTable(obs_data_t *request, obs_data_t *response, void *)
{
	RunSync([&]() {
		Preset *preset = PresetForRequest(request);
		if (!preset) {
			Fail(response, "no preset active");
			return;
		}

		/* Flattened, one row per candidate, in Transition Table's shape. Marks
		 * whose matchers cover several scenes are expanded so the rows stay
		 * meaningful to existing scripts. */
		obs_data_array_t *rows = obs_data_array_create();

		auto sceneNames = [](const SceneMatcher &matcher) {
			std::vector<std::string> names;
			if (matcher.type == MatchType::Scenes)
				names = matcher.scenes;
			else
				names.push_back("Any");
			return names;
		};

		for (const auto &mark : preset->marks) {
			for (const auto &fromScene : sceneNames(mark.from)) {
				for (const auto &toScene : sceneNames(mark.to)) {
					for (const auto &candidate : mark.candidates) {
						obs_data_t *row = obs_data_create();
						obs_data_set_string(row, "canvas",
								    mark.canvas.empty() ? MainCanvasName().c_str()
											: mark.canvas.c_str());
						obs_data_set_string(row, "from_scene", fromScene.c_str());
						obs_data_set_string(row, "to_scene", toScene.c_str());
						obs_data_set_string(row, "transition", candidate.transition.c_str());
						obs_data_set_int(row, "duration", candidate.duration);
						obs_data_set_string(row, "mark_id", mark.id.c_str());
						obs_data_set_bool(row, "enabled", mark.enabled && candidate.enabled);
						obs_data_array_push_back(rows, row);
						obs_data_release(row);
					}
				}
			}
		}

		obs_data_set_array(response, "transitions", rows);
		obs_data_array_release(rows);
		Succeed(response);
	});
}

int g_nextDelta = 1;
int g_previousDelta = -1;

} // namespace

void WebsocketInit()
{
	g_vendor = obs_websocket_register_vendor("transition-tree");
	if (!g_vendor) {
		obs_log(LOG_INFO, "obs-websocket not available; vendor requests are disabled");
		return;
	}

	obs_websocket_vendor_register_request(g_vendor, "get_presets", RequestGetPresets, nullptr);
	obs_websocket_vendor_register_request(g_vendor, "get_active_preset", RequestGetActivePreset, nullptr);
	obs_websocket_vendor_register_request(g_vendor, "set_active_preset", RequestSetActivePreset, nullptr);
	obs_websocket_vendor_register_request(g_vendor, "next_preset", RequestCyclePreset, &g_nextDelta);
	obs_websocket_vendor_register_request(g_vendor, "previous_preset", RequestCyclePreset, &g_previousDelta);
	obs_websocket_vendor_register_request(g_vendor, "set_enabled", RequestSetEnabled, nullptr);

	obs_websocket_vendor_register_request(g_vendor, "get_tree", RequestGetTree, nullptr);
	obs_websocket_vendor_register_request(g_vendor, "set_mark", RequestSetMark, nullptr);
	obs_websocket_vendor_register_request(g_vendor, "remove_mark", RequestRemoveMark, nullptr);
	obs_websocket_vendor_register_request(g_vendor, "set_mark_enabled", RequestSetMarkEnabled, nullptr);

	/* Transition Table compatible names, so existing scripts keep working. */
	obs_websocket_vendor_register_request(g_vendor, "get_transition", RequestGetTransition, nullptr);
	obs_websocket_vendor_register_request(g_vendor, "set_transition", RequestSetTransition, nullptr);
	obs_websocket_vendor_register_request(g_vendor, "get_table", RequestGetTable, nullptr);

	obs_log(LOG_INFO, "registered obs-websocket vendor 'transition-tree'");
}

void WebsocketShutdown()
{
	g_vendor = nullptr;
}

void WebsocketEmitPresetChanged(const std::string &name)
{
	if (!g_vendor)
		return;

	obs_data_t *event = obs_data_create();
	obs_data_set_string(event, "preset", name.c_str());
	obs_data_set_bool(event, "enabled", GetStore().enabled);
	obs_websocket_vendor_emit_event(g_vendor, "preset_changed", event);
	obs_data_release(event);
}

} // namespace tt
