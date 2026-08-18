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

#include <QMainWindow>
#include <QMetaObject>

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace tt {

obs_data_t *g_pendingMigration = nullptr;

namespace {

/* The candidate this plugin pre-armed for a destination scene, remembered so
 * that the switch which actually happens can be committed to the right mark. */
struct ArmedPick {
	std::string markId;
	int candidate = -1;
};

/* Per-canvas state, all touched only from the Qt main thread. */
std::map<std::string, std::map<std::string, ArmedPick>> g_armed;
std::map<std::string, std::string> g_lastProgram;
std::map<std::string, uint64_t> g_armedGeneration;

/* Scenes whose private settings this plugin has written. Tracking them means a
 * transition override the user configured in OBS itself is left alone unless a
 * mark actually targets that scene. */
std::map<std::string, std::set<std::string>> g_written;

obs_hotkey_pair_id g_enableDisableHotkey = OBS_INVALID_HOTKEY_PAIR_ID;
obs_hotkey_id g_nextPresetHotkey = OBS_INVALID_HOTKEY_ID;
obs_hotkey_id g_prevPresetHotkey = OBS_INVALID_HOTKEY_ID;
std::string g_nextPresetBindings;
std::string g_prevPresetBindings;

/* Registered activation hotkeys, one per preset. The payload is the preset name
 * and is owned here. */
struct PresetHotkey {
	obs_hotkey_id id = OBS_INVALID_HOTKEY_ID;
	std::string *name = nullptr;
};
std::vector<PresetHotkey> g_presetHotkeys;

bool g_transitionTableDetected = false;

/* ------------------------------------------------------------------------ */

void RunOnMainThread(std::function<void()> task)
{
	auto *mainWindow = (QMainWindow *)obs_frontend_get_main_window();
	if (!mainWindow) {
		task();
		return;
	}
	QMetaObject::invokeMethod(mainWindow, std::move(task), Qt::QueuedConnection);
}

std::string CurrentProgramScene(obs_canvas_t *canvas)
{
	obs_source_t *source = obs_canvas_get_channel(canvas, 0);
	if (!source)
		return std::string();

	if (obs_source_get_type(source) == OBS_SOURCE_TYPE_TRANSITION) {
		obs_source_t *active = obs_transition_get_active_source(source);
		obs_source_release(source);
		source = active;
		if (!source)
			return std::string();
	}

	const char *name = obs_source_get_name(source);
	std::string result = name ? name : "";
	obs_source_release(source);
	return result;
}

std::vector<std::string> CanvasSceneNames(obs_canvas_t *canvas)
{
	std::vector<std::string> names;
	obs_canvas_enum_scenes(
		canvas,
		[](void *param, obs_source_t *scene) {
			auto *out = (std::vector<std::string> *)param;
			const char *name = obs_source_get_name(scene);
			if (name)
				out->push_back(name);
			return true;
		},
		&names);
	return names;
}

std::string MarkCanvas(const Mark &mark)
{
	return mark.canvas.empty() ? MainCanvasName() : mark.canvas;
}

/* Applies the armed picks for one canvas to the scenes' private settings. */
void WriteOverrides(obs_canvas_t *canvas, const std::string &canvasName)
{
	Preset *preset = GetStore().Active();
	auto &armed = g_armed[canvasName];
	auto &written = g_written[canvasName];

	std::set<std::string> stillWritten;

	for (const auto &sceneName : CanvasSceneNames(canvas)) {
		obs_source_t *scene = obs_get_source_by_name(sceneName.c_str());
		if (!scene)
			continue;

		obs_data_t *settings = obs_source_get_private_settings(scene);
		auto it = armed.find(sceneName);

		const Candidate *candidate = nullptr;
		if (it != armed.end() && preset) {
			const Mark *mark = preset->FindMark(it->second.markId);
			if (mark && it->second.candidate >= 0 && (size_t)it->second.candidate < mark->candidates.size())
				candidate = &mark->candidates[(size_t)it->second.candidate];
		}

		if (candidate) {
			obs_data_set_string(settings, "transition", candidate->transition.c_str());
			obs_data_set_int(settings, "transition_duration", candidate->duration);
			stillWritten.insert(sceneName);
		} else if (written.count(sceneName)) {
			/* Only clear what we put there ourselves. */
			obs_data_erase(settings, "transition");
		}

		obs_data_release(settings);
		obs_source_release(scene);
	}

	written = stillWritten;
}

void ApplyOverridesForCanvas(obs_canvas_t *canvas)
{
	if (!canvas || obs_canvas_removed(canvas))
		return;

	const char *rawName = obs_canvas_get_name(canvas);
	if (!rawName)
		return;
	const std::string canvasName = rawName;

	Store &store = GetStore();
	const std::string current = CurrentProgramScene(canvas);

	auto &lastProgram = g_lastProgram[canvasName];
	const bool programChanged = current != lastProgram;

	if (programChanged) {
		/* The switch that just completed used whatever we armed for the
		 * scene now on program, so that pick is the one to commit. */
		Preset *preset = store.Active();
		auto canvasArmed = g_armed.find(canvasName);
		if (preset && canvasArmed != g_armed.end()) {
			auto pick = canvasArmed->second.find(current);
			if (pick != canvasArmed->second.end()) {
				Mark *mark = preset->FindMark(pick->second.markId);
				if (mark)
					mark->CommitPick(pick->second.candidate);
			}
		}
		lastProgram = current;
	}

	/* Re-arm only when the situation actually changed. Holding the picks
	 * steady while sitting on a scene means each switch consumes exactly one
	 * random roll, instead of re-rolling on every incidental signal. */
	const bool needsRearm = programChanged || g_armed.find(canvasName) == g_armed.end() ||
				g_armedGeneration[canvasName] != store.generation;

	if (needsRearm) {
		auto &armed = g_armed[canvasName];
		armed.clear();

		if (store.enabled && store.Active()) {
			for (const auto &destination : CanvasSceneNames(canvas)) {
				Mark *mark = ResolveMark(canvasName, current, destination);
				if (!mark)
					continue;
				const int candidate = mark->PickCandidate();
				if (candidate < 0)
					continue;
				armed[destination] = ArmedPick{mark->id, candidate};
			}
		}

		g_armedGeneration[canvasName] = store.generation;
	}

	WriteOverrides(canvas, canvasName);
}

void ApplyOverridesNow()
{
	obs_enum_canvases(
		[](void *, obs_canvas_t *canvas) {
			ApplyOverridesForCanvas(canvas);
			return true;
		},
		nullptr);
}

/* ------------------------------------------------------------------------ */
/* Signals                                                                   */
/* ------------------------------------------------------------------------ */

void OnTransitionStart(void *, calldata_t *)
{
	ApplyOverrides();
}

void OnChannelChange(void *, calldata_t *callData)
{
	if (calldata_int(callData, "channel") != 0)
		return;

	auto *source = (obs_source_t *)calldata_ptr(callData, "source");
	auto *previous = (obs_source_t *)calldata_ptr(callData, "prev_source");

	if (previous && obs_source_get_type(previous) == OBS_SOURCE_TYPE_TRANSITION) {
		signal_handler_t *handler = obs_source_get_signal_handler(previous);
		signal_handler_disconnect(handler, "transition_start", OnTransitionStart, nullptr);
	}
	if (source && obs_source_get_type(source) == OBS_SOURCE_TYPE_TRANSITION) {
		signal_handler_t *handler = obs_source_get_signal_handler(source);
		signal_handler_disconnect(handler, "transition_start", OnTransitionStart, nullptr);
		signal_handler_connect(handler, "transition_start", OnTransitionStart, nullptr);
	}

	ApplyOverrides();
}

void OnSourceRename(void *, calldata_t *callData)
{
	auto *source = (obs_source_t *)calldata_ptr(callData, "source");
	if (!source || obs_source_get_type(source) != OBS_SOURCE_TYPE_SCENE)
		return;

	const char *rawPrevious = calldata_string(callData, "prev_name");
	const char *rawNext = calldata_string(callData, "new_name");
	if (!rawPrevious || !rawNext)
		return;

	const std::string previous = rawPrevious;
	const std::string next = rawNext;

	RunOnMainThread([previous, next]() {
		Store &store = GetStore();
		bool changed = false;

		for (auto &preset : store.presets) {
			for (auto &mark : preset.marks) {
				const auto before = std::make_pair(mark.from.scenes, mark.to.scenes);
				mark.from.RenameScene(previous, next);
				mark.to.RenameScene(previous, next);
				if (before.first != mark.from.scenes || before.second != mark.to.scenes)
					changed = true;
			}
		}

		if (changed) {
			store.Touch();
			SaveGlobalPresets();
			ApplyOverrides();
			TransitionTreeDialog::NotifyDataChanged();
		}
	});
}

void ConnectCanvasSignals()
{
	obs_enum_canvases(
		[](void *, obs_canvas_t *canvas) {
			signal_handler_t *handler = obs_canvas_get_signal_handler(canvas);
			if (handler) {
				signal_handler_disconnect(handler, "channel_change", OnChannelChange, nullptr);
				signal_handler_connect(handler, "channel_change", OnChannelChange, nullptr);
			}

			/* Pick up the transition that is already in channel 0 so the
			 * very first switch after load is covered. */
			obs_source_t *source = obs_canvas_get_channel(canvas, 0);
			if (source) {
				if (obs_source_get_type(source) == OBS_SOURCE_TYPE_TRANSITION) {
					signal_handler_t *sh = obs_source_get_signal_handler(source);
					signal_handler_disconnect(sh, "transition_start", OnTransitionStart, nullptr);
					signal_handler_connect(sh, "transition_start", OnTransitionStart, nullptr);
				}
				obs_source_release(source);
			}
			return true;
		},
		nullptr);
}

/* ------------------------------------------------------------------------ */
/* Hotkeys                                                                   */
/* ------------------------------------------------------------------------ */

std::string HotkeyBindingsToJson(obs_hotkey_id id)
{
	if (id == OBS_INVALID_HOTKEY_ID)
		return std::string();

	obs_data_array_t *array = obs_hotkey_save(id);
	if (!array)
		return std::string();

	obs_data_t *wrapper = obs_data_create();
	obs_data_set_array(wrapper, "bindings", array);
	const char *json = obs_data_get_json(wrapper);
	std::string result = json ? json : "";
	obs_data_release(wrapper);
	obs_data_array_release(array);
	return result;
}

void HotkeyBindingsFromJson(obs_hotkey_id id, const std::string &json)
{
	if (id == OBS_INVALID_HOTKEY_ID || json.empty())
		return;

	obs_data_t *wrapper = obs_data_create_from_json(json.c_str());
	if (!wrapper)
		return;

	obs_data_array_t *array = obs_data_get_array(wrapper, "bindings");
	if (array) {
		obs_hotkey_load(id, array);
		obs_data_array_release(array);
	}
	obs_data_release(wrapper);
}

bool OnEnableHotkey(void *, obs_hotkey_pair_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed || GetStore().enabled)
		return false;
	RunOnMainThread([]() { SetTreeEnabled(true); });
	return true;
}

bool OnDisableHotkey(void *, obs_hotkey_pair_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed || !GetStore().enabled)
		return false;
	RunOnMainThread([]() { SetTreeEnabled(false); });
	return true;
}

void OnNextPresetHotkey(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	RunOnMainThread([]() { CycleActivePreset(1); });
}

void OnPreviousPresetHotkey(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	RunOnMainThread([]() { CycleActivePreset(-1); });
}

void OnPresetHotkey(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed || !data)
		return;
	const std::string name = *(std::string *)data;
	RunOnMainThread([name]() { SetActivePreset(name); });
}

void UnregisterPresetHotkeys()
{
	for (auto &entry : g_presetHotkeys) {
		if (entry.id != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(entry.id);
		delete entry.name;
	}
	g_presetHotkeys.clear();
}

} // namespace

/* ------------------------------------------------------------------------ */
/* Public engine API                                                         */
/* ------------------------------------------------------------------------ */

std::string MainCanvasName()
{
	obs_canvas_t *canvas = obs_get_main_canvas();
	if (!canvas)
		return std::string();
	const char *name = obs_canvas_get_name(canvas);
	std::string result = name ? name : "";
	obs_canvas_release(canvas);
	return result;
}

void ApplyOverrides()
{
	RunOnMainThread([]() { ApplyOverridesNow(); });
}

void ClearOverrides()
{
	for (auto &canvasEntry : g_written) {
		for (const auto &sceneName : canvasEntry.second) {
			obs_source_t *scene = obs_get_source_by_name(sceneName.c_str());
			if (!scene)
				continue;
			obs_data_t *settings = obs_source_get_private_settings(scene);
			obs_data_erase(settings, "transition");
			obs_data_release(settings);
			obs_source_release(scene);
		}
	}
	g_written.clear();
	g_armed.clear();
	g_armedGeneration.clear();
}

Mark *ResolveMark(const std::string &canvas, const std::string &fromScene, const std::string &toScene)
{
	Store &store = GetStore();
	if (!store.enabled)
		return nullptr;

	Preset *preset = store.Active();
	if (!preset)
		return nullptr;

	Mark *best = nullptr;
	int bestScore = -1;

	for (auto &mark : preset->marks) {
		if (!mark.enabled)
			continue;
		if (MarkCanvas(mark) != canvas)
			continue;
		if (!mark.from.Matches(fromScene) || !mark.to.Matches(toScene))
			continue;
		if (mark.UsableCandidates().empty())
			continue;

		/* Strictly greater, so the first mark listed wins a tie. */
		if (mark.Score() > bestScore) {
			best = &mark;
			bestScore = mark.Score();
		}
	}

	return best;
}

bool ResolveTransition(const std::string &canvas, const std::string &fromScene, const std::string &toScene,
		       std::string &transition, int &duration)
{
	const std::string canvasName = canvas.empty() ? MainCanvasName() : canvas;

	Mark *mark = ResolveMark(canvasName, fromScene, toScene);
	if (!mark)
		return false;

	const int index = mark->PickCandidate();
	if (index < 0 || (size_t)index >= mark->candidates.size())
		return false;

	transition = mark->candidates[(size_t)index].transition;
	duration = mark->candidates[(size_t)index].duration;
	return true;
}

void ApplyPresetDefaultTransition()
{
	Preset *preset = GetStore().Active();
	if (!preset || !preset->setDefaultTransition || preset->defaultTransition.empty())
		return;

	struct obs_frontend_source_list transitions = {};
	obs_frontend_get_transitions(&transitions);

	for (size_t i = 0; i < transitions.sources.num; i++) {
		obs_source_t *transition = transitions.sources.array[i];
		const char *name = obs_source_get_name(transition);
		if (name && preset->defaultTransition == name) {
			obs_frontend_set_current_transition(transition);
			if (preset->defaultDuration > 0)
				obs_frontend_set_transition_duration(preset->defaultDuration);
			break;
		}
	}

	obs_frontend_source_list_free(&transitions);
}

void SetActivePreset(const std::string &name)
{
	Store &store = GetStore();

	if (!name.empty() && !store.Find(name)) {
		obs_log(LOG_WARNING, "cannot activate unknown preset '%s'", name.c_str());
		return;
	}

	store.activePreset = name;

	Preset *preset = store.Active();
	if (preset)
		preset->ResetRuntimeState();

	store.Touch();

	/* A preset swap starts a fresh cycle, so nothing from the previous one
	 * should carry over. */
	g_armed.clear();
	g_armedGeneration.clear();

	ApplyPresetDefaultTransition();
	ApplyOverridesNow();
	WebsocketEmitPresetChanged(name);
	TransitionTreeDialog::NotifyDataChanged();

	obs_log(LOG_INFO, "active preset is now '%s'", name.empty() ? "(none)" : name.c_str());
}

void CycleActivePreset(int delta)
{
	Store &store = GetStore();
	if (store.presets.empty())
		return;

	int index = -1;
	for (size_t i = 0; i < store.presets.size(); i++) {
		if (store.presets[i].name == store.activePreset) {
			index = (int)i;
			break;
		}
	}

	const int count = (int)store.presets.size();
	if (index < 0) {
		index = delta >= 0 ? 0 : count - 1;
	} else {
		index = ((index + delta) % count + count) % count;
	}

	SetActivePreset(store.presets[(size_t)index].name);
}

void SetTreeEnabled(bool enabled)
{
	Store &store = GetStore();
	if (store.enabled == enabled)
		return;

	store.enabled = enabled;
	store.Touch();

	if (enabled) {
		ApplyOverridesNow();
	} else {
		ClearOverrides();
	}

	TransitionTreeDialog::NotifyDataChanged();
	obs_log(LOG_INFO, "transition tree %s", enabled ? "enabled" : "disabled");
}

bool IsTreeEnabled()
{
	return GetStore().enabled;
}

void RefreshPresetHotkeys()
{
	/* Capture whatever the user has bound before tearing the old set down.
	 * Matching on the hotkey id rather than the preset name is what lets a
	 * binding survive a rename. */
	for (auto &entry : g_presetHotkeys) {
		if (entry.id == OBS_INVALID_HOTKEY_ID)
			continue;
		for (auto &preset : GetStore().presets) {
			if (preset.hotkey == entry.id) {
				preset.hotkeyBindings = HotkeyBindingsToJson(entry.id);
				break;
			}
		}
	}

	UnregisterPresetHotkeys();

	for (auto &preset : GetStore().presets) {
		preset.hotkey = OBS_INVALID_HOTKEY_ID;

		const std::string hotkeyName = "transition-tree.preset." + preset.name;
		const std::string description =
			std::string(obs_module_text("Hotkey.ActivatePreset")) + " " + preset.name;

		auto *payload = new std::string(preset.name);
		const obs_hotkey_id id =
			obs_hotkey_register_frontend(hotkeyName.c_str(), description.c_str(), OnPresetHotkey, payload);

		if (id == OBS_INVALID_HOTKEY_ID) {
			delete payload;
			continue;
		}

		preset.hotkey = id;
		HotkeyBindingsFromJson(id, preset.hotkeyBindings);
		g_presetHotkeys.push_back(PresetHotkey{id, payload});
	}
}

bool TransitionTableDetected()
{
	return g_transitionTableDetected;
}

/* ------------------------------------------------------------------------ */
/* Save / load                                                               */
/* ------------------------------------------------------------------------ */

static void SaveCollectionData(obs_data_t *saveData)
{
	Store &store = GetStore();

	/* Refresh cached bindings from the live hotkey registrations. */
	for (auto &entry : g_presetHotkeys) {
		if (entry.id == OBS_INVALID_HOTKEY_ID)
			continue;
		for (auto &preset : store.presets) {
			if (preset.hotkey == entry.id) {
				preset.hotkeyBindings = HotkeyBindingsToJson(entry.id);
				break;
			}
		}
	}
	g_nextPresetBindings = HotkeyBindingsToJson(g_nextPresetHotkey);
	g_prevPresetBindings = HotkeyBindingsToJson(g_prevPresetHotkey);

	obs_data_t *root = obs_data_create();
	obs_data_set_int(root, "schema_version", kSchemaVersion);
	obs_data_set_string(root, "active_preset", store.activePreset.c_str());
	obs_data_set_bool(root, "enabled", store.enabled);
	obs_data_set_string(root, "next_preset_hotkey", g_nextPresetBindings.c_str());
	obs_data_set_string(root, "previous_preset_hotkey", g_prevPresetBindings.c_str());

	obs_data_array_t *localPresets = obs_data_array_create();
	for (const auto &preset : store.presets) {
		if (preset.global)
			continue;
		obs_data_t *item = SavePreset(preset);
		obs_data_array_push_back(localPresets, item);
		obs_data_release(item);
	}
	obs_data_set_array(root, "presets", localPresets);
	obs_data_array_release(localPresets);

	obs_data_array_t *enableHotkey = nullptr;
	obs_data_array_t *disableHotkey = nullptr;
	obs_hotkey_pair_save(g_enableDisableHotkey, &enableHotkey, &disableHotkey);
	if (enableHotkey) {
		obs_data_set_array(root, "enable_hotkey", enableHotkey);
		obs_data_array_release(enableHotkey);
	}
	if (disableHotkey) {
		obs_data_set_array(root, "disable_hotkey", disableHotkey);
		obs_data_array_release(disableHotkey);
	}

	TransitionTreeDialog::SaveGeometry(root);

	obs_data_set_obj(saveData, "transition-tree", root);
	obs_data_release(root);

	/* Global presets are shared across collections, so persist them here as
	 * well as when they are edited. */
	SaveGlobalPresets();
}

static void LoadCollectionData(obs_data_t *saveData)
{
	Store &store = GetStore();

	/* Local presets belong to the collection being replaced. */
	store.presets.erase(std::remove_if(store.presets.begin(), store.presets.end(),
					   [](const Preset &preset) { return !preset.global; }),
			    store.presets.end());
	store.activePreset.clear();

	g_armed.clear();
	g_armedGeneration.clear();
	g_lastProgram.clear();
	g_written.clear();

	if (g_pendingMigration) {
		obs_data_release(g_pendingMigration);
		g_pendingMigration = nullptr;
	}

	obs_data_t *root = obs_data_get_obj(saveData, "transition-tree");
	if (root) {
		obs_data_set_default_bool(root, "enabled", true);
		store.enabled = obs_data_get_bool(root, "enabled");

		const char *nextBindings = obs_data_get_string(root, "next_preset_hotkey");
		g_nextPresetBindings = nextBindings ? nextBindings : "";
		const char *prevBindings = obs_data_get_string(root, "previous_preset_hotkey");
		g_prevPresetBindings = prevBindings ? prevBindings : "";
		HotkeyBindingsFromJson(g_nextPresetHotkey, g_nextPresetBindings);
		HotkeyBindingsFromJson(g_prevPresetHotkey, g_prevPresetBindings);

		obs_data_array_t *presets = obs_data_get_array(root, "presets");
		if (presets) {
			const size_t count = obs_data_array_count(presets);
			for (size_t i = 0; i < count; i++) {
				obs_data_t *item = obs_data_array_item(presets, i);
				Preset preset;
				if (LoadPreset(item, preset)) {
					preset.global = false;
					preset.name = store.UniqueName(preset.name);
					store.presets.push_back(preset);
				}
				obs_data_release(item);
			}
			obs_data_array_release(presets);
		}

		obs_data_array_t *enableHotkey = obs_data_get_array(root, "enable_hotkey");
		obs_data_array_t *disableHotkey = obs_data_get_array(root, "disable_hotkey");
		obs_hotkey_pair_load(g_enableDisableHotkey, enableHotkey, disableHotkey);
		obs_data_array_release(enableHotkey);
		obs_data_array_release(disableHotkey);

		TransitionTreeDialog::RestoreGeometry(root);

		const char *active = obs_data_get_string(root, "active_preset");
		if (active && *active && store.Find(active))
			store.activePreset = active;

		obs_data_release(root);
	} else {
		/* No Transition Tree data in this collection. If Transition Table
		 * left some behind, hold on to it and offer to migrate once the
		 * frontend has finished loading. */
		obs_data_t *legacy = obs_data_get_obj(saveData, "transition-table");
		if (legacy) {
			obs_data_array_t *transitions = obs_data_get_array(legacy, "transitions");
			const bool hasEntries = transitions && obs_data_array_count(transitions) > 0;
			obs_data_array_release(transitions);

			if (hasEntries)
				g_pendingMigration = legacy;
			else
				obs_data_release(legacy);
		}
	}

	/* Global presets outlive the collection swap, so their sequence positions
	 * need clearing too. */
	for (auto &preset : store.presets)
		preset.ResetRuntimeState();

	store.Touch();
	RefreshPresetHotkeys();
}

static void OnFrontendSaveLoad(obs_data_t *saveData, bool saving, void *)
{
	if (saving)
		SaveCollectionData(saveData);
	else
		LoadCollectionData(saveData);
}

static void OnFrontendEvent(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		ApplyOverridesNow();
		break;

	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
		ApplyOverridesNow();
		/* The editor lists scenes for matcher selection, so it has to
		 * follow scenes being added or removed. */
		TransitionTreeDialog::NotifyDataChanged();
		break;

	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		ConnectCanvasSignals();
		ApplyPresetDefaultTransition();
		ApplyOverridesNow();
		TransitionTreeDialog::NotifyDataChanged();
		TransitionTreeDialog::OfferPendingMigration();
		break;

	case OBS_FRONTEND_EVENT_TRANSITION_LIST_CHANGED:
		TransitionTreeDialog::NotifyDataChanged();
		break;

	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
	case OBS_FRONTEND_EVENT_EXIT:
		g_armed.clear();
		g_armedGeneration.clear();
		g_lastProgram.clear();
		g_written.clear();
		break;

	default:
		break;
	}
}

static void DetectTransitionTable()
{
	obs_enum_modules(
		[](void *, obs_module_t *module) {
			const char *file = obs_get_module_file_name(module);
			if (file && strstr(file, "transition-table"))
				g_transitionTableDetected = true;
		},
		nullptr);

	if (g_transitionTableDetected) {
		obs_log(LOG_WARNING, "Exeldro's Transition Table is also loaded; both plugins write the same "
				     "per-scene transition overrides and will fight over them");
	}
}

void EngineInit()
{
	DetectTransitionTable();

	g_enableDisableHotkey = obs_hotkey_pair_register_frontend(
		"transition-tree.enable", obs_module_text("Hotkey.Enable"), "transition-tree.disable",
		obs_module_text("Hotkey.Disable"), OnEnableHotkey, OnDisableHotkey, nullptr, nullptr);

	g_nextPresetHotkey = obs_hotkey_register_frontend(
		"transition-tree.next-preset", obs_module_text("Hotkey.NextPreset"), OnNextPresetHotkey, nullptr);
	g_prevPresetHotkey = obs_hotkey_register_frontend("transition-tree.previous-preset",
							  obs_module_text("Hotkey.PreviousPreset"),
							  OnPreviousPresetHotkey, nullptr);

	LoadGlobalPresets();

	obs_frontend_add_save_callback(OnFrontendSaveLoad, nullptr);
	obs_frontend_add_event_callback(OnFrontendEvent, nullptr);

	signal_handler_connect(obs_get_signal_handler(), "source_rename", OnSourceRename, nullptr);

	ConnectCanvasSignals();
}

void EngineShutdown()
{
	obs_frontend_remove_save_callback(OnFrontendSaveLoad, nullptr);
	obs_frontend_remove_event_callback(OnFrontendEvent, nullptr);
	signal_handler_disconnect(obs_get_signal_handler(), "source_rename", OnSourceRename, nullptr);

	UnregisterPresetHotkeys();

	if (g_enableDisableHotkey != OBS_INVALID_HOTKEY_PAIR_ID)
		obs_hotkey_pair_unregister(g_enableDisableHotkey);
	if (g_nextPresetHotkey != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(g_nextPresetHotkey);
	if (g_prevPresetHotkey != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(g_prevPresetHotkey);

	if (g_pendingMigration) {
		obs_data_release(g_pendingMigration);
		g_pendingMigration = nullptr;
	}

	g_armed.clear();
	g_armedGeneration.clear();
	g_lastProgram.clear();
	g_written.clear();
	GetStore().presets.clear();
}

} // namespace tt
