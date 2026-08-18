# Transition Tree

An OBS Studio plugin for driving which transition plays on each scene change,
with presets, several transitions per rule, hotkeys and obs-websocket control.

Transition Tree is a from-scratch implementation that covers what
[Transition Table](https://github.com/exeldro/obs-transition-table) by Exeldro
does, and adds presets, multiple transitions per rule, wildcard exclusions and a
tree-shaped editor. Transition Table is the prior art this plugin's feature set
was modelled on, and its exported JSON and stored scene-collection data can both
be imported.

## Features

- **Presets.** Save whole sets of rules and switch between them. Each preset is
  either *local* to a scene collection or *global* and offered in every
  collection; each scene collection remembers which preset it had active.
- **Several transitions per mark.** A single from → to rule can hold a list of
  transitions, chosen **sequentially**, at **random**, at **random without
  repeating** the previous pick, or by **weighted random**.
- **Flexible scene matching.** Either side of a mark can name specific scenes
  (more than one), match *any* scene, or match *any scene except* a named set.
- **Enable and disable marks** individually without deleting them.
- **A tree instead of dropdown-driven filtering.** Marks are grouped by their
  from-scene, and the search box only ever filters the view — adding a mark is a
  separate button, so narrowing the list can never hide the controls needed to
  create one.
- **Hotkeys** for activating each preset by name, stepping to the next or
  previous preset, and enabling or disabling the whole tree.
- **obs-websocket vendor requests**, including preset switching, plus
  Transition Table's request names for drop-in compatibility.
- **Import and export** presets as JSON.
- **Create transitions** without leaving the window.

## How it works

Transition Tree does not perform transitions itself. Before each switch it
writes OBS's own per-scene transition override (the `transition` and
`transition_duration` private settings) on every scene that could be the next
destination, and OBS picks it up when the switch happens. Overrides are
recalculated whenever a transition starts, the program scene changes, or channel
0 changes, for every canvas.

Only scenes this plugin has actually written to are ever cleared again, so a
transition override you set in OBS yourself is left alone unless a mark targets
that scene.

### Which mark wins

Several marks can match the same switch. The most specific one wins, from-side
first:

| Match type | Specificity |
|---|---|
| Specific scene(s) | highest |
| Any except … | middle |
| Any scene | lowest |

So `A → not-C` beats `Any → D`, but loses to `A → D`. When two marks tie, the
one listed first in the preset wins. Marks that are disabled, that name no
scenes, or that have no usable transition are skipped entirely, and the next
matching rule applies.

### Choosing between several transitions

The pick for each possible destination is made when you arrive on a scene, and
held until you leave it — so each switch consumes exactly one random roll rather
than re-rolling on every incidental signal. Sequential positions advance only
for the mark that actually fired. Positions last for the session, and reset when
a preset is activated.

## Building

This plugin uses the standard OBS plugin template build system. See the
[plugin template wiki](https://github.com/obsproject/obs-plugintemplate/wiki)
for prerequisites and platform specifics.

```sh
cmake --preset ubuntu-x86_64   # or windows-x64 / macos
cmake --build --preset ubuntu-x86_64
```

Requires OBS Studio 31.1 or newer — the multi-canvas API this plugin uses was
introduced in OBS 31.

## obs-websocket

Requests are sent to the vendor `transition-tree` via `CallVendorRequest`. Every
response carries `success`, plus `error` when it is false.

### Presets

| Request | Fields | Returns |
|---|---|---|
| `get_presets` | — | `presets` (name, global, active, mark_count), `active_preset`, `enabled` |
| `get_active_preset` | — | `preset`, `enabled` |
| `set_active_preset` | `preset` | `preset` |
| `next_preset` | — | `preset` |
| `previous_preset` | — | `preset` |
| `set_enabled` | `enabled` | `enabled` |

Activating a preset emits the vendor event `preset_changed` with `preset` and
`enabled`.

### Marks

| Request | Fields | Returns |
|---|---|---|
| `get_tree` | `preset` (optional) | `preset`, `marks` |
| `set_mark` | see below | `id` |
| `remove_mark` | `id`, `preset` (optional) | — |
| `set_mark_enabled` | `id`, `enabled`, `preset` (optional) | — |

`set_mark` creates a mark, or updates one when `id` is given. Omitting `preset`
targets the active preset.

```jsonc
{
  "from": { "type": "scenes", "scenes": [{ "name": "Intro" }] },
  "to":   { "type": "any_except", "scenes": [{ "name": "BRB" }] },
  "mode": "weighted",              // sequential | random | random_no_repeat | weighted
  "enabled": true,
  "canvas": "Main Canvas",         // optional, defaults to the main canvas
  "transitions": [
    { "transition": "Fade",  "duration": 300, "weight": 3.0, "enabled": true },
    { "transition": "Swipe", "duration": 500, "weight": 1.0 }
  ]
}
```

`from_scene` / `to_scene` strings are accepted instead of the matcher objects,
where the literal `"Any"` means the wildcard, and `transition` + `duration` are
accepted instead of the `transitions` array.

### Transition Table compatible requests

`get_transition`, `set_transition` and `get_table` are registered under the same
names and shapes Transition Table uses, so existing scripts keep working — note
that the vendor name is `transition-tree`, and that `get_table` flattens marks
into one row per scene pair per transition.

### Procedure handler

`get_transition_tree_transition(string canvas, string from_scene, string to_scene, out string transition, out int duration)`
resolves a switch without going through obs-websocket.

## Import and export

Exported files carry a `schema_version` and a `presets` array. The import dialog
also accepts a single bare preset object and Transition Table's own
`{"transitions": [...]}` export. Key bindings are deliberately left out of
exports.

If a scene collection contains Transition Table settings and no Transition Tree
settings, the plugin offers once to import them into a new preset.

Global presets are stored in the plugin's config directory as
`global-presets.json`; local presets live in the scene collection.

## Known interactions

- **Running Transition Table at the same time is not supported.** Both plugins
  write the same per-scene overrides and will overwrite each other. Transition
  Tree detects it at load, logs a warning and says so once when the window opens.
- Marks in a global preset that reference scenes missing from the current
  collection are kept and shown in red, and skipped at runtime.

## Credits

- Modelled on [Transition Table](https://github.com/exeldro/obs-transition-table)
  by Exeldro, which established the per-scene override approach this plugin
  builds on.
- Built from the [OBS plugin template](https://github.com/obsproject/obs-plugintemplate).

## License

GPL-2.0-or-later. See [LICENSE](LICENSE).
