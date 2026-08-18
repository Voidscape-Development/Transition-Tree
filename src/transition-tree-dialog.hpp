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

#include <QDialog>
#include <QString>

#include <string>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QMainWindow;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTableWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace tt {
struct Mark;
struct Preset;
struct SceneMatcher;
} // namespace tt

/* The Transition Tree editor. A single instance at a time; opening it again
 * raises the existing window. */
class TransitionTreeDialog : public QDialog {
	Q_OBJECT

public:
	explicit TransitionTreeDialog(QMainWindow *parent);
	~TransitionTreeDialog() override;

	/* Opens the dialog, or raises it when already open. */
	static void ShowDialog();

	/* Rebuilds the open dialog, if any, after the model changed elsewhere
	 * (hotkey, websocket, scene rename, collection load). Safe to call when
	 * no dialog exists. */
	static void NotifyDataChanged();

	static void SaveGeometry(obs_data_t *data);
	static void RestoreGeometry(obs_data_t *data);

	/* Prompts to import Transition Table data found in the scene collection.
	 * Does nothing when there is none. */
	static void OfferPendingMigration();

private slots:
	void OnPresetChanged(int index);
	void OnTreeSelectionChanged();
	void OnSearchChanged(const QString &text);

private:
	/* --- construction helpers --- */
	QWidget *BuildPresetBar();
	QWidget *BuildTreePane();
	QWidget *BuildDetailPane();
	QWidget *BuildMarkPage();
	QWidget *BuildPresetPage();
	QWidget *BuildBottomBar();

	/* --- refresh --- */
	void RefreshAll();
	void RefreshPresetCombo();
	void RefreshTree();
	void RefreshDetail();
	void RefreshMarkPage(tt::Mark &mark);
	void RefreshMarkWarning(const tt::Mark &mark);
	void RefreshPresetPage();
	void RefreshMatcherWidgets(const tt::SceneMatcher &matcher, QComboBox *typeCombo, QListWidget *sceneList,
				   const QString &canvas);
	void RefreshCandidateTable(tt::Mark &mark);

	/* --- model access --- */
	tt::Preset *CurrentPreset();
	tt::Mark *SelectedMark();
	void CommitChange(bool structural);

	/* --- actions --- */
	void AddPreset();
	void DuplicatePreset();
	void RenamePreset();
	void DeletePreset();
	void ChangePresetScope(int index);
	void AddMark();
	void DuplicateMark();
	void DeleteMark();
	void ToggleMarkEnabled();
	void TestMark();
	void AddCandidate();
	void RemoveCandidate();
	void MoveCandidate(int delta);
	void ImportPresets();
	void ExportPresets();
	void CreateTransition();
	void ReadMatcherFromWidgets(tt::SceneMatcher &matcher, QComboBox *typeCombo, QListWidget *sceneList);

	QStringList SceneNamesForCanvas(const QString &canvas) const;
	QStringList TransitionNames() const;
	QStringList CanvasNames() const;
	void WarnAboutTransitionTable();

	/* --- widgets --- */
	QComboBox *m_presetCombo = nullptr;
	QComboBox *m_scopeCombo = nullptr;
	QCheckBox *m_enabledCheck = nullptr;
	QLineEdit *m_search = nullptr;
	QTreeWidget *m_tree = nullptr;
	QStackedWidget *m_detail = nullptr;

	QPushButton *m_duplicateMarkButton = nullptr;
	QPushButton *m_deleteMarkButton = nullptr;
	QPushButton *m_toggleMarkButton = nullptr;
	QPushButton *m_testMarkButton = nullptr;

	/* Mark page */
	QComboBox *m_fromType = nullptr;
	QListWidget *m_fromScenes = nullptr;
	QComboBox *m_toType = nullptr;
	QListWidget *m_toScenes = nullptr;
	QComboBox *m_canvasCombo = nullptr;
	QComboBox *m_modeCombo = nullptr;
	QCheckBox *m_markEnabled = nullptr;
	QLabel *m_markWarning = nullptr;
	QTableWidget *m_candidates = nullptr;
	QPushButton *m_removeCandidateButton = nullptr;
	QPushButton *m_moveUpButton = nullptr;
	QPushButton *m_moveDownButton = nullptr;

	/* Preset page */
	QLabel *m_presetSummary = nullptr;
	QCheckBox *m_setDefaultTransition = nullptr;
	QComboBox *m_defaultTransition = nullptr;
	QSpinBox *m_defaultDuration = nullptr;

	/* Selected mark id, so selection survives a tree rebuild. */
	std::string m_selectedMarkId;

	/* Guards widget signal handlers while the UI is being repopulated. */
	bool m_updating = false;
};
