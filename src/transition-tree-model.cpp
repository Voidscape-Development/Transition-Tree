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

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <algorithm>
#include <cstring>
#include <random>

namespace tt {

static std::mt19937 &Rng()
{
	static std::mt19937 rng((unsigned)os_gettime_ns());
	return rng;
}

static std::string NewMarkId()
{
	static uint64_t counter = 0;
	return "mark-" + std::to_string(os_gettime_ns()) + "-" + std::to_string(++counter);
}

static bool SceneExists(const std::string &name)
{
	obs_source_t *source = obs_get_source_by_name(name.c_str());
	if (!source)
		return false;
	const bool isScene = obs_source_get_type(source) == OBS_SOURCE_TYPE_SCENE;
	obs_source_release(source);
	return isScene;
}

/* ------------------------------------------------------------------------ */
/* SceneMatcher                                                              */
/* ------------------------------------------------------------------------ */

bool SceneMatcher::Matches(const std::string &scene) const
{
	switch (type) {
	case MatchType::Any:
		return true;
	case MatchType::AnyExcept:
		return std::find(scenes.begin(), scenes.end(), scene) == scenes.end();
	case MatchType::Scenes:
	default:
		return std::find(scenes.begin(), scenes.end(), scene) != scenes.end();
	}
}

int SceneMatcher::Specificity() const
{
	switch (type) {
	case MatchType::Scenes:
		return 2;
	case MatchType::AnyExcept:
		return 1;
	case MatchType::Any:
	default:
		return 0;
	}
}

std::vector<std::string> SceneMatcher::MissingScenes() const
{
	std::vector<std::string> missing;
	if (type == MatchType::Any)
		return missing;
	for (const auto &scene : scenes) {
		if (!SceneExists(scene))
			missing.push_back(scene);
	}
	return missing;
}

void SceneMatcher::RenameScene(const std::string &prev, const std::string &next)
{
	for (auto &scene : scenes) {
		if (scene == prev)
			scene = next;
	}
}

std::string SceneMatcher::Describe() const
{
	if (type == MatchType::Any)
		return obs_module_text("Match.Any");

	std::string joined;
	for (size_t i = 0; i < scenes.size(); i++) {
		if (i)
			joined += ", ";
		joined += scenes[i];
	}

	if (type == MatchType::AnyExcept) {
		if (scenes.empty())
			return obs_module_text("Match.Any");
		return std::string(obs_module_text("Match.AnyExceptPrefix")) + " " + joined;
	}

	if (scenes.empty())
		return obs_module_text("Match.NoScenes");
	return joined;
}

/* ------------------------------------------------------------------------ */
/* Mark                                                                      */
/* ------------------------------------------------------------------------ */

std::vector<size_t> Mark::UsableCandidates() const
{
	std::vector<size_t> usable;
	for (size_t i = 0; i < candidates.size(); i++) {
		if (candidates[i].enabled && !candidates[i].transition.empty())
			usable.push_back(i);
	}
	return usable;
}

int Mark::PickCandidate() const
{
	const auto usable = UsableCandidates();
	if (usable.empty())
		return -1;
	if (usable.size() == 1)
		return (int)usable[0];

	switch (mode) {
	case SelectionMode::Sequential:
		return (int)usable[sequenceIndex % usable.size()];

	case SelectionMode::RandomNoRepeat: {
		/* Draw from every usable candidate except the one that fired last.
		 * Falls back to a plain uniform draw if the previous pick is no
		 * longer usable. */
		std::vector<size_t> pool;
		for (size_t index : usable) {
			if ((int)index != lastPicked)
				pool.push_back(index);
		}
		if (pool.empty())
			pool = usable;
		std::uniform_int_distribution<size_t> dist(0, pool.size() - 1);
		return (int)pool[dist(Rng())];
	}

	case SelectionMode::Weighted: {
		double total = 0.0;
		for (size_t index : usable)
			total += std::max(0.0, candidates[index].weight);
		if (total <= 0.0) {
			/* Every weight is zero, so fall back to uniform rather than
			 * refusing to fire. */
			std::uniform_int_distribution<size_t> dist(0, usable.size() - 1);
			return (int)usable[dist(Rng())];
		}
		std::uniform_real_distribution<double> dist(0.0, total);
		double roll = dist(Rng());
		for (size_t index : usable) {
			roll -= std::max(0.0, candidates[index].weight);
			if (roll <= 0.0)
				return (int)index;
		}
		return (int)usable.back();
	}

	case SelectionMode::Random:
	default: {
		std::uniform_int_distribution<size_t> dist(0, usable.size() - 1);
		return (int)usable[dist(Rng())];
	}
	}
}

void Mark::CommitPick(int index)
{
	if (index < 0)
		return;
	lastPicked = index;

	if (mode != SelectionMode::Sequential)
		return;

	/* Advance past the candidate that just fired. Positions are tracked
	 * against the usable subset so that disabling a candidate does not
	 * desynchronize the cycle. */
	const auto usable = UsableCandidates();
	if (usable.empty())
		return;
	for (size_t i = 0; i < usable.size(); i++) {
		if ((int)usable[i] == index) {
			sequenceIndex = (i + 1) % usable.size();
			return;
		}
	}
	sequenceIndex = 0;
}

/* ------------------------------------------------------------------------ */
/* Preset                                                                    */
/* ------------------------------------------------------------------------ */

Mark *Preset::FindMark(const std::string &id)
{
	for (auto &mark : marks) {
		if (mark.id == id)
			return &mark;
	}
	return nullptr;
}

const Mark *Preset::FindMark(const std::string &id) const
{
	for (const auto &mark : marks) {
		if (mark.id == id)
			return &mark;
	}
	return nullptr;
}

void Preset::ResetRuntimeState()
{
	for (auto &mark : marks)
		mark.ResetRuntimeState();
}

/* ------------------------------------------------------------------------ */
/* Store                                                                     */
/* ------------------------------------------------------------------------ */

Store &GetStore()
{
	static Store store;
	return store;
}

Preset *Store::Find(const std::string &name)
{
	for (auto &preset : presets) {
		if (preset.name == name)
			return &preset;
	}
	return nullptr;
}

const Preset *Store::Find(const std::string &name) const
{
	for (const auto &preset : presets) {
		if (preset.name == name)
			return &preset;
	}
	return nullptr;
}

Preset *Store::Active()
{
	if (activePreset.empty())
		return nullptr;
	return Find(activePreset);
}

std::string Store::UniqueName(const std::string &desired) const
{
	const std::string base = desired.empty() ? std::string(obs_module_text("Preset.Untitled")) : desired;
	if (!Find(base))
		return base;
	for (int i = 2; i < 10000; i++) {
		const std::string candidate = base + " (" + std::to_string(i) + ")";
		if (!Find(candidate))
			return candidate;
	}
	return base + " " + std::to_string(os_gettime_ns());
}

/* ------------------------------------------------------------------------ */
/* Serialization                                                             */
/* ------------------------------------------------------------------------ */

static void SaveMatcher(obs_data_t *parent, const char *key, const SceneMatcher &matcher)
{
	obs_data_t *obj = obs_data_create();
	obs_data_set_int(obj, "type", (int)matcher.type);

	obs_data_array_t *scenes = obs_data_array_create();
	for (const auto &scene : matcher.scenes) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "name", scene.c_str());
		obs_data_array_push_back(scenes, item);
		obs_data_release(item);
	}
	obs_data_set_array(obj, "scenes", scenes);
	obs_data_array_release(scenes);

	obs_data_set_obj(parent, key, obj);
	obs_data_release(obj);
}

static void LoadMatcher(obs_data_t *parent, const char *key, SceneMatcher &matcher)
{
	matcher.scenes.clear();

	obs_data_t *obj = obs_data_get_obj(parent, key);
	if (!obj)
		return;

	const int type = (int)obs_data_get_int(obj, "type");
	matcher.type = (type >= 0 && type <= 2) ? (MatchType)type : MatchType::Scenes;

	obs_data_array_t *scenes = obs_data_get_array(obj, "scenes");
	if (scenes) {
		const size_t count = obs_data_array_count(scenes);
		for (size_t i = 0; i < count; i++) {
			obs_data_t *item = obs_data_array_item(scenes, i);
			const char *name = obs_data_get_string(item, "name");
			if (name && *name)
				matcher.scenes.push_back(name);
			obs_data_release(item);
		}
		obs_data_array_release(scenes);
	}

	obs_data_release(obj);
}

obs_data_t *SaveMark(const Mark &mark)
{
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "id", mark.id.c_str());
	obs_data_set_string(data, "canvas", mark.canvas.c_str());
	obs_data_set_int(data, "mode", (int)mark.mode);
	obs_data_set_bool(data, "enabled", mark.enabled);
	SaveMatcher(data, "from", mark.from);
	SaveMatcher(data, "to", mark.to);

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
	obs_data_set_array(data, "candidates", candidates);
	obs_data_array_release(candidates);

	return data;
}

bool LoadMark(obs_data_t *data, Mark &mark)
{
	if (!data)
		return false;

	const char *id = obs_data_get_string(data, "id");
	mark.id = (id && *id) ? id : NewMarkId();

	const char *canvas = obs_data_get_string(data, "canvas");
	mark.canvas = canvas ? canvas : "";

	const int mode = (int)obs_data_get_int(data, "mode");
	mark.mode = (mode >= 0 && mode <= 3) ? (SelectionMode)mode : SelectionMode::Sequential;

	obs_data_set_default_bool(data, "enabled", true);
	mark.enabled = obs_data_get_bool(data, "enabled");

	LoadMatcher(data, "from", mark.from);
	LoadMatcher(data, "to", mark.to);

	mark.candidates.clear();
	obs_data_array_t *candidates = obs_data_get_array(data, "candidates");
	if (candidates) {
		const size_t count = obs_data_array_count(candidates);
		for (size_t i = 0; i < count; i++) {
			obs_data_t *item = obs_data_array_item(candidates, i);
			Candidate candidate;
			const char *transition = obs_data_get_string(item, "transition");
			candidate.transition = transition ? transition : "";
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
		obs_data_array_release(candidates);
	}

	mark.ResetRuntimeState();
	return true;
}

obs_data_t *SavePreset(const Preset &preset)
{
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "name", preset.name.c_str());
	obs_data_set_bool(data, "global", preset.global);
	obs_data_set_bool(data, "set_default_transition", preset.setDefaultTransition);
	obs_data_set_string(data, "default_transition", preset.defaultTransition.c_str());
	obs_data_set_int(data, "default_duration", preset.defaultDuration);

	if (!preset.hotkeyBindings.empty())
		obs_data_set_string(data, "hotkey", preset.hotkeyBindings.c_str());

	obs_data_array_t *marks = obs_data_array_create();
	for (const auto &mark : preset.marks) {
		obs_data_t *item = SaveMark(mark);
		obs_data_array_push_back(marks, item);
		obs_data_release(item);
	}
	obs_data_set_array(data, "marks", marks);
	obs_data_array_release(marks);

	return data;
}

bool LoadPreset(obs_data_t *data, Preset &preset)
{
	if (!data)
		return false;

	const char *name = obs_data_get_string(data, "name");
	preset.name = name ? name : "";
	preset.global = obs_data_get_bool(data, "global");
	preset.setDefaultTransition = obs_data_get_bool(data, "set_default_transition");

	const char *defaultTransition = obs_data_get_string(data, "default_transition");
	preset.defaultTransition = defaultTransition ? defaultTransition : "";

	obs_data_set_default_int(data, "default_duration", 300);
	preset.defaultDuration = (int)obs_data_get_int(data, "default_duration");

	const char *hotkey = obs_data_get_string(data, "hotkey");
	preset.hotkeyBindings = hotkey ? hotkey : "";

	preset.marks.clear();
	obs_data_array_t *marks = obs_data_get_array(data, "marks");
	if (marks) {
		const size_t count = obs_data_array_count(marks);
		for (size_t i = 0; i < count; i++) {
			obs_data_t *item = obs_data_array_item(marks, i);
			Mark mark;
			if (LoadMark(item, mark))
				preset.marks.push_back(mark);
			obs_data_release(item);
		}
		obs_data_array_release(marks);
	}

	return !preset.name.empty();
}

obs_data_t *ExportPresets(const std::vector<const Preset *> &presets)
{
	obs_data_t *root = obs_data_create();
	obs_data_set_int(root, "schema_version", kSchemaVersion);
	obs_data_set_string(root, "plugin", "transition-tree");

	obs_data_array_t *array = obs_data_array_create();
	for (const Preset *preset : presets) {
		if (!preset)
			continue;
		obs_data_t *item = SavePreset(*preset);
		/* Key bindings are machine-specific, so they are deliberately left
		 * out of exports. */
		obs_data_erase(item, "hotkey");
		obs_data_array_push_back(array, item);
		obs_data_release(item);
	}
	obs_data_set_array(root, "presets", array);
	obs_data_array_release(array);

	return root;
}

Preset PresetFromTransitionTable(obs_data_t *data, const char *presetName, const char *fallbackCanvas)
{
	Preset preset;
	preset.name = presetName ? presetName : "Transition Table";

	obs_data_array_t *transitions = obs_data_get_array(data, "transitions");
	if (!transitions)
		return preset;

	const size_t count = obs_data_array_count(transitions);
	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(transitions, i);

		const char *fromScene = obs_data_get_string(item, "from_scene");
		const char *toScene = obs_data_get_string(item, "to_scene");
		const char *transition = obs_data_get_string(item, "transition");

		if (fromScene && *fromScene && toScene && *toScene && transition && *transition) {
			Mark mark;
			mark.id = NewMarkId();

			const char *canvas = obs_data_get_string(item, "canvas");
			mark.canvas = (canvas && *canvas) ? canvas : (fallbackCanvas ? fallbackCanvas : "");

			/* Transition Table spells its wildcard as the literal
			 * scene name "Any" on either side. */
			if (strcmp(fromScene, "Any") == 0) {
				mark.from.type = MatchType::Any;
			} else {
				mark.from.type = MatchType::Scenes;
				mark.from.scenes.push_back(fromScene);
			}
			if (strcmp(toScene, "Any") == 0) {
				mark.to.type = MatchType::Any;
			} else {
				mark.to.type = MatchType::Scenes;
				mark.to.scenes.push_back(toScene);
			}

			Candidate candidate;
			candidate.transition = transition;
			candidate.duration = (int)obs_data_get_int(item, "duration");
			if (candidate.duration <= 0)
				candidate.duration = 300;
			mark.candidates.push_back(candidate);

			preset.marks.push_back(mark);
		}

		obs_data_release(item);
	}
	obs_data_array_release(transitions);

	return preset;
}

bool ImportPresets(obs_data_t *data, std::vector<Preset> &out, std::string &error)
{
	if (!data) {
		error = obs_module_text("Import.Unreadable");
		return false;
	}

	const size_t before = out.size();

	obs_data_array_t *presets = obs_data_get_array(data, "presets");
	if (presets) {
		const int schema = (int)obs_data_get_int(data, "schema_version");
		if (schema > kSchemaVersion) {
			error = obs_module_text("Import.NewerSchema");
			obs_data_array_release(presets);
			return false;
		}

		const size_t count = obs_data_array_count(presets);
		for (size_t i = 0; i < count; i++) {
			obs_data_t *item = obs_data_array_item(presets, i);
			Preset preset;
			if (LoadPreset(item, preset)) {
				preset.hotkeyBindings.clear();
				preset.hotkey = OBS_INVALID_HOTKEY_ID;
				out.push_back(preset);
			}
			obs_data_release(item);
		}
		obs_data_array_release(presets);
	} else if (obs_data_has_user_value(data, "marks")) {
		/* A single bare preset object. */
		Preset preset;
		if (LoadPreset(data, preset)) {
			preset.hotkeyBindings.clear();
			preset.hotkey = OBS_INVALID_HOTKEY_ID;
			out.push_back(preset);
		}
	} else if (obs_data_has_user_value(data, "transitions")) {
		/* Transition Table's own export format. */
		const std::string canvas = MainCanvasName();
		out.push_back(PresetFromTransitionTable(data, obs_module_text("Import.TransitionTablePreset"),
							canvas.c_str()));
	}

	if (out.size() == before) {
		error = obs_module_text("Import.NothingFound");
		return false;
	}

	return true;
}

/* ------------------------------------------------------------------------ */
/* Global preset storage                                                     */
/* ------------------------------------------------------------------------ */

static std::string GlobalPresetPath()
{
	char *path = obs_module_config_path("global-presets.json");
	if (!path)
		return std::string();
	std::string result = path;
	bfree(path);
	return result;
}

void LoadGlobalPresets()
{
	Store &store = GetStore();

	/* Drop any global presets already in memory; local ones belong to the
	 * scene collection and are loaded separately. */
	store.presets.erase(std::remove_if(store.presets.begin(), store.presets.end(),
					   [](const Preset &preset) { return preset.global; }),
			    store.presets.end());

	const std::string path = GlobalPresetPath();
	if (path.empty())
		return;

	obs_data_t *data = obs_data_create_from_json_file_safe(path.c_str(), "bak");
	if (!data)
		return;

	obs_data_array_t *presets = obs_data_get_array(data, "presets");
	if (presets) {
		const size_t count = obs_data_array_count(presets);
		for (size_t i = 0; i < count; i++) {
			obs_data_t *item = obs_data_array_item(presets, i);
			Preset preset;
			if (LoadPreset(item, preset)) {
				preset.global = true;
				preset.name = store.UniqueName(preset.name);
				store.presets.push_back(preset);
			}
			obs_data_release(item);
		}
		obs_data_array_release(presets);
	}

	obs_data_release(data);
	store.Touch();
}

void SaveGlobalPresets()
{
	const std::string path = GlobalPresetPath();
	if (path.empty())
		return;

	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}

	obs_data_t *root = obs_data_create();
	obs_data_set_int(root, "schema_version", kSchemaVersion);

	obs_data_array_t *array = obs_data_array_create();
	for (const auto &preset : GetStore().presets) {
		if (!preset.global)
			continue;
		obs_data_t *item = SavePreset(preset);
		obs_data_array_push_back(array, item);
		obs_data_release(item);
	}
	obs_data_set_array(root, "presets", array);
	obs_data_array_release(array);

	if (!obs_data_save_json_safe(root, path.c_str(), "tmp", "bak"))
		obs_log(LOG_WARNING, "failed to write global presets to %s", path.c_str());

	obs_data_release(root);
}

} // namespace tt
