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

#pragma once

#include <obs.h>
#include <obs-hotkey.h>

#include <string>
#include <vector>

namespace tt {

/* Bumped whenever the exported JSON layout changes in a way importers care
 * about. Importers accept anything <= this. */
constexpr int kSchemaVersion = 1;

/* ------------------------------------------------------------------------ */
/* Scene matching                                                            */
/* ------------------------------------------------------------------------ */

/* How one side (from or to) of a mark selects scenes. The values are
 * serialized, so keep them stable. */
enum class MatchType {
	Scenes = 0,   /* matches any scene listed in `scenes` */
	Any = 1,      /* matches every scene */
	AnyExcept = 2 /* matches every scene not listed in `scenes` */
};

struct SceneMatcher {
	MatchType type = MatchType::Scenes;
	std::vector<std::string> scenes;

	bool Matches(const std::string &scene) const;

	/* Higher is more specific. Resolution prefers the most specific match:
	 * an explicit scene list beats "any except", which beats "any". */
	int Specificity() const;

	/* True when the matcher can never fire because it names no scene. */
	bool IsEmpty() const { return type == MatchType::Scenes && scenes.empty(); }

	/* Scene names this matcher references that do not exist in the current
	 * scene collection. Only meaningful for Scenes/AnyExcept. */
	std::vector<std::string> MissingScenes() const;

	void RenameScene(const std::string &prev, const std::string &next);

	/* Human-readable, translated summary for the tree and detail panel. */
	std::string Describe() const;
};

/* ------------------------------------------------------------------------ */
/* Marks                                                                     */
/* ------------------------------------------------------------------------ */

enum class SelectionMode {
	Sequential = 0,     /* walk the list in order, wrapping around */
	Random = 1,         /* uniform random */
	RandomNoRepeat = 2, /* uniform random, never the previous pick */
	Weighted = 3        /* random, proportional to per-candidate weight */
};

struct Candidate {
	std::string transition;
	int duration = 300;
	double weight = 1.0;
	bool enabled = true;
};

struct Mark {
	std::string id;
	std::string canvas;
	SceneMatcher from;
	SceneMatcher to;
	std::vector<Candidate> candidates;
	SelectionMode mode = SelectionMode::Sequential;
	bool enabled = true;

	/* Runtime state. Lives for the session only: it is deliberately not
	 * serialized, and is reset whenever a preset is activated. */
	size_t sequenceIndex = 0;
	int lastPicked = -1;

	/* Ranking used to choose between several marks that all match the same
	 * switch. From-side specificity dominates the to-side. */
	int Score() const { return from.Specificity() * 4 + to.Specificity(); }

	bool Matches(const std::string &fromScene, const std::string &toScene) const
	{
		return enabled && from.Matches(fromScene) && to.Matches(toScene);
	}

	/* Indices of candidates that are enabled and name a transition. */
	std::vector<size_t> UsableCandidates() const;

	/* Chooses the candidate that should fire next without recording it.
	 * Returns -1 when the mark has nothing usable. */
	int PickCandidate() const;

	/* Records that `index` actually fired, advancing sequential state. */
	void CommitPick(int index);

	void ResetRuntimeState()
	{
		sequenceIndex = 0;
		lastPicked = -1;
	}
};

/* ------------------------------------------------------------------------ */
/* Presets                                                                   */
/* ------------------------------------------------------------------------ */

struct Preset {
	std::string name;

	/* Global presets live in the plugin config directory and are offered in
	 * every scene collection. Local presets live in the scene collection's
	 * own save data. */
	bool global = false;

	/* Optionally drives OBS's current transition and default duration, which
	 * is the fallback used whenever no mark matches a switch. */
	bool setDefaultTransition = false;
	std::string defaultTransition;
	int defaultDuration = 300;

	std::vector<Mark> marks;

	/* Serialized obs_data array of key bindings, kept as JSON so presets stay
	 * trivially copyable. Empty when the preset has no binding. */
	std::string hotkeyBindings;
	obs_hotkey_id hotkey = OBS_INVALID_HOTKEY_ID;

	Mark *FindMark(const std::string &id);
	const Mark *FindMark(const std::string &id) const;
	void ResetRuntimeState();
};

/* ------------------------------------------------------------------------ */
/* Store                                                                     */
/* ------------------------------------------------------------------------ */

/* Everything the plugin knows about, for every scope. Presets are addressed by
 * name, which is unique across both scopes. Guarded by the Qt main thread: all
 * mutation happens either from the UI or from callbacks marshalled onto it. */
struct Store {
	std::vector<Preset> presets;

	/* Name of the active preset for the current scene collection, or empty
	 * when the tree is idle. */
	std::string activePreset;

	/* Master switch, toggled by the enable/disable hotkey pair. */
	bool enabled = true;

	/* Bumped on every change that can affect resolution, so the engine knows
	 * to re-arm rather than reuse its cached picks. */
	uint64_t generation = 1;

	Preset *Find(const std::string &name);
	const Preset *Find(const std::string &name) const;
	Preset *Active();
	std::string UniqueName(const std::string &desired) const;
	void Touch() { generation++; }
};

Store &GetStore();

/* ------------------------------------------------------------------------ */
/* Serialization                                                             */
/* ------------------------------------------------------------------------ */

obs_data_t *SaveMark(const Mark &mark);
bool LoadMark(obs_data_t *data, Mark &mark);

obs_data_t *SavePreset(const Preset &preset);
bool LoadPreset(obs_data_t *data, Preset &preset);

/* Export format: { "schema_version": N, "presets": [...] }. */
obs_data_t *ExportPresets(const std::vector<const Preset *> &presets);

/* Accepts our own export format, a bare preset, and Transition Table's
 * { "transitions": [...] } export. Imported presets are appended to `out`. */
bool ImportPresets(obs_data_t *data, std::vector<Preset> &out, std::string &error);

/* Converts Transition Table's flat transition array into a single preset. */
Preset PresetFromTransitionTable(obs_data_t *data, const char *presetName, const char *fallbackCanvas);

/* Global presets, stored in the plugin's config directory. */
void LoadGlobalPresets();
void SaveGlobalPresets();

/* ------------------------------------------------------------------------ */
/* Engine                                                                    */
/* ------------------------------------------------------------------------ */

void EngineInit();
void EngineShutdown();

/* Re-applies scene transition overrides for every canvas. Safe to call from
 * any thread; the work is marshalled onto the Qt main thread. */
void ApplyOverrides();

/* Clears every override this plugin may have written. */
void ClearOverrides();

/* Read-only resolution used by the websocket vendor and proc handler. Does not
 * advance sequential state. Returns false when no mark matches. */
bool ResolveTransition(const std::string &canvas, const std::string &fromScene, const std::string &toScene,
		       std::string &transition, int &duration);

/* Finds the mark that would drive a given switch, or nullptr. */
Mark *ResolveMark(const std::string &canvas, const std::string &fromScene, const std::string &toScene);

void SetActivePreset(const std::string &name);
void CycleActivePreset(int delta);
void SetTreeEnabled(bool enabled);
bool IsTreeEnabled();

/* Re-registers per-preset activation hotkeys after presets change. */
void RefreshPresetHotkeys();

/* Name of the main canvas, cached for convenience. */
std::string MainCanvasName();

/* Applies the active preset's default-transition override, if it has one. */
void ApplyPresetDefaultTransition();

/* True when Exeldro's Transition Table is also loaded; the two plugins write
 * the same per-scene settings and will fight. */
bool TransitionTableDetected();

/* Transition Table data found in the scene collection at load time, awaiting
 * the user's decision on whether to migrate it. Empty when there is none. */
extern obs_data_t *g_pendingMigration;

/* ------------------------------------------------------------------------ */
/* Websocket vendor                                                          */
/* ------------------------------------------------------------------------ */

void WebsocketInit();
void WebsocketShutdown();
void WebsocketEmitPresetChanged(const std::string &name);

} // namespace tt
