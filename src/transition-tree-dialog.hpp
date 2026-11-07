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

#include <memory>
#include <string>

class QAction;
class QButtonGroup;
class QCheckBox;
class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QListWidget;
class QMainWindow;
class QPoint;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTableWidget;
class QTimer;
class QToolButton;
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
	/* What the placeholder shown instead of an empty tree offers to do. */
	enum class EmptyState { None, NoPreset, NoMarks, NoMatches };

	/* --- construction helpers --- */
	void BuildActions();
	QWidget *BuildPresetBar();
	QWidget *BuildOffBanner();
	QWidget *BuildTreePane();
	QWidget *BuildDetailPane();
	QWidget *BuildMarkPage();
	QWidget *BuildPresetPage();
	QWidget *BuildBottomBar();

	/* --- refresh --- */
	void RefreshAll();
	void RefreshMasterState();
	void RefreshPresetCombo();
	void RefreshPresetActions();
	void RefreshTree();
	void RefreshDetail();
	void RefreshMarkActions();
	void RefreshMarkPage(tt::Mark &mark);
	void RefreshMarkSummary(const tt::Mark &mark);
	void RefreshMarkWarning(const tt::Mark &mark);
	void RefreshPresetPage();
	void RefreshMatcherWidgets(const tt::SceneMatcher &matcher, QButtonGroup *typeGroup, QListWidget *sceneList,
				   QLabel *anyNote, QLabel *count, const QString &canvas);
	void RefreshMatcherState(const tt::SceneMatcher &matcher, QListWidget *sceneList, QLabel *anyNote,
				 QLabel *count);
	void RefreshCandidateTable(tt::Mark &mark);
	void RefreshCandidateShares(const tt::Mark &mark);

	/* --- model access --- */
	tt::Preset *CurrentPreset();
	tt::Mark *SelectedMark();
	void CommitChange(bool structural);

	/* --- actions --- */
	void AddPreset();
	void DuplicatePreset();
	void RenamePreset();
	void DeletePreset();
	void ChangePresetScope(bool global);
	void AddMark();
	void DuplicateMark();
	void DeleteMark();
	void UndoDeleteMark();
	void ClearUndo();
	void ToggleMarkEnabled();
	void TestMark();
	void AddCandidate();
	void RemoveCandidate(int row);
	void MoveCandidate(int row, int delta);
	void ImportPresets();
	void ExportPresets();
	void CreateTransition();
	void ShowTreeContextMenu(const QPoint &pos);
	void ReadMatcherFromWidgets(tt::SceneMatcher &matcher, QButtonGroup *typeGroup, QListWidget *sceneList);

	QStringList SceneNamesForCanvas(const QString &canvas) const;
	QStringList TransitionNames() const;
	QStringList CanvasNames() const;
	void WarnAboutTransitionTable();

	/* --- actions shared by menus, the context menu and shortcuts --- */
	QAction *m_addMarkAction = nullptr;
	QAction *m_duplicateMarkAction = nullptr;
	QAction *m_deleteMarkAction = nullptr;
	QAction *m_toggleMarkAction = nullptr;
	QAction *m_testMarkAction = nullptr;

	QAction *m_duplicatePresetAction = nullptr;
	QAction *m_renamePresetAction = nullptr;
	QAction *m_deletePresetAction = nullptr;
	QAction *m_exportAction = nullptr;
	QAction *m_scopeLocalAction = nullptr;
	QAction *m_scopeGlobalAction = nullptr;

	/* --- header --- */
	QPushButton *m_enabledButton = nullptr;
	QComboBox *m_presetCombo = nullptr;
	QToolButton *m_presetMenuButton = nullptr;
	QFrame *m_offBanner = nullptr;

	/* --- tree pane --- */
	QLineEdit *m_search = nullptr;
	QStackedWidget *m_treeStack = nullptr;
	QTreeWidget *m_tree = nullptr;
	QLabel *m_emptyLabel = nullptr;
	QPushButton *m_emptyButton = nullptr;
	EmptyState m_emptyState = EmptyState::None;
	QFrame *m_undoBar = nullptr;
	QLabel *m_undoLabel = nullptr;
	QTimer *m_undoTimer = nullptr;
	QLabel *m_treeFooter = nullptr;

	QStackedWidget *m_detail = nullptr;

	/* --- mark page --- */
	QLabel *m_markTitle = nullptr;
	QLabel *m_markSentence = nullptr;
	QCheckBox *m_markEnabled = nullptr;
	QPushButton *m_testMarkButton = nullptr;
	QToolButton *m_markMenuButton = nullptr;
	QLabel *m_markWarning = nullptr;
	QWidget *m_canvasRow = nullptr;
	QComboBox *m_canvasCombo = nullptr;
	QButtonGroup *m_fromType = nullptr;
	QListWidget *m_fromScenes = nullptr;
	QLabel *m_fromAnyNote = nullptr;
	QLabel *m_fromCount = nullptr;
	QButtonGroup *m_toType = nullptr;
	QListWidget *m_toScenes = nullptr;
	QLabel *m_toAnyNote = nullptr;
	QLabel *m_toCount = nullptr;
	QWidget *m_modeRow = nullptr;
	QComboBox *m_modeCombo = nullptr;
	QTableWidget *m_candidates = nullptr;

	/* --- preset page --- */
	QLabel *m_presetTitle = nullptr;
	QLabel *m_presetSummary = nullptr;
	QCheckBox *m_setDefaultTransition = nullptr;
	QComboBox *m_defaultTransition = nullptr;
	QSpinBox *m_defaultDuration = nullptr;
	QPushButton *m_presetExportButton = nullptr;

	/* Selected mark id, so selection survives a tree rebuild. */
	std::string m_selectedMarkId;

	/* The most recently deleted mark, held until the undo bar times out. */
	std::unique_ptr<tt::Mark> m_undoMark;
	std::string m_undoPreset;
	size_t m_undoIndex = 0;

	/* Guards widget signal handlers while the UI is being repopulated. */
	bool m_updating = false;
};
