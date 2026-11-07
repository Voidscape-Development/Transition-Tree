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

#include "transition-tree-dialog.hpp"
#include "transition-tree.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <QAbstractButton>
#include <QAction>
#include <QActionGroup>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStringList>
#include <QStyle>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <algorithm>
#include <vector>

using namespace tt;

namespace {

TransitionTreeDialog *g_instance = nullptr;
int g_savedWidth = 0;
int g_savedHeight = 0;
bool g_warnedAboutTransitionTable = false;

/* Roles used to hang model identity off tree items. */
constexpr int kMarkIdRole = Qt::UserRole + 1;
constexpr int kGroupKeyRole = Qt::UserRole + 2;

/* How long a deleted mark can still be restored from the undo bar. */
constexpr int kUndoTimeoutMs = 8000;

enum TreeColumn { kColScenes = 0, kColTransition, kColMode, kColCanvas, kTreeColumnCount };

enum CandidateColumn {
	kCandTransition = 0,
	kCandDuration,
	kCandWeight,
	kCandShare,
	kCandEnabled,
	kCandActions,
	kCandColumnCount
};

/* Kept deliberately small so OBS's own theme still drives almost everything.
 * Colours are chosen to read on both the dark and light OBS themes. */
const char *kStyleSheet = R"(
QPushButton#ttMaster { padding: 4px 14px; border-radius: 13px; font-weight: bold; }
QPushButton#ttMaster:checked { background-color: #1f4d39; border: 1px solid #2f8a62; color: #d2f5e3; }
QFrame#ttBanner, QLabel#ttWarning {
	background-color: rgba(229, 169, 59, 0.16);
	border: 1px solid rgba(229, 169, 59, 0.6);
	border-radius: 4px;
}
QLabel#ttWarning { padding: 6px 8px; }
QFrame#ttUndoBar { background-color: rgba(127, 127, 127, 0.18); border-radius: 4px; }
QPushButton#ttTest { color: #e05a5a; }
QLabel#ttTitle { font-size: 14pt; font-weight: bold; }
QLabel#ttDim { color: #8b8b96; }
)";

QString Tr(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QString ModeName(SelectionMode mode)
{
	switch (mode) {
	case SelectionMode::Sequential:
		return Tr("Mode.Sequential");
	case SelectionMode::Random:
		return Tr("Mode.Random");
	case SelectionMode::RandomNoRepeat:
		return Tr("Mode.RandomNoRepeat");
	case SelectionMode::Weighted:
		return Tr("Mode.Weighted");
	}
	return QString();
}

QColor InvalidColor()
{
	return QColor(0xE0, 0x5A, 0x5A);
}

QColor DimColor()
{
	return QColor(0x8B, 0x8B, 0x96);
}

QLabel *DimLabel(const QString &text = QString())
{
	auto *label = new QLabel(text);
	label->setObjectName("ttDim");
	label->setWordWrap(true);
	return label;
}

/* OBS hands out scenes and transitions in dock order, which is arbitrary once
 * a collection grows. Lists the user has to hunt through are sorted the way
 * they would sort them by hand: case-insensitively, with case only breaking
 * ties so the order stays stable. */
void SortNames(QStringList &names)
{
	std::sort(names.begin(), names.end(), [](const QString &a, const QString &b) {
		const int order = QString::compare(a, b, Qt::CaseInsensitive);
		return order != 0 ? order < 0 : a < b;
	});
}

QString Utf8(const std::string &text)
{
	return QString::fromUtf8(text.c_str());
}

std::vector<std::string> MissingScenes(const Mark &mark)
{
	auto missing = mark.from.MissingScenes();
	for (const auto &scene : mark.to.MissingScenes())
		missing.push_back(scene);
	return missing;
}

/* Every reason a mark cannot fire, in the words shown to the user. */
QStringList MarkProblems(const Mark &mark)
{
	QStringList problems;

	const auto missing = MissingScenes(mark);
	if (!missing.empty()) {
		QStringList names;
		for (const auto &scene : missing)
			names << Utf8(scene);
		problems << Tr("Warning.MissingScenes").arg(names.join(", "));
	}
	if (mark.from.IsEmpty() || mark.to.IsEmpty())
		problems << Tr("Warning.NoScenesSelected");
	if (mark.UsableCandidates().empty())
		problems << Tr("Warning.NoUsableCandidates");

	return problems;
}

/* The transition column of the tree: the first usable transition and its
 * duration, plus how many more the mark can pick from. */
QString MarkSummary(const Mark &mark)
{
	const auto usable = mark.UsableCandidates();
	if (mark.candidates.empty())
		return Tr("Mark.NoTransitions");
	if (usable.empty())
		return Tr("Mark.NoneOn");

	const Candidate &first = mark.candidates[usable.front()];
	const QString summary =
		Tr("Mark.TransitionSummary").arg(Utf8(first.transition), QString::number(first.duration));
	if (usable.size() == 1)
		return summary;
	return Tr("Mark.TransitionMore").arg(summary, QString::number(usable.size() - 1));
}

QString CandidateList(const Mark &mark)
{
	QStringList lines;
	for (const auto &candidate : mark.candidates) {
		QString line = Tr("Mark.TransitionSummary")
				       .arg(Utf8(candidate.transition), QString::number(candidate.duration));
		if (!candidate.enabled)
			line = Tr("Candidate.OffSuffix").arg(line);
		lines << line;
	}
	return lines.join("\n");
}

QString JoinWithOr(const std::vector<std::string> &scenes)
{
	QStringList names;
	for (const auto &scene : scenes)
		names << Utf8(scene);
	if (names.size() < 2)
		return names.join(QString());
	const QString last = names.takeLast();
	return Tr("Sentence.OrJoin").arg(names.join(", "), last);
}

QString SidePhrase(const SceneMatcher &matcher)
{
	switch (matcher.type) {
	case MatchType::Any:
		return Tr("Sentence.AnyScene");
	case MatchType::AnyExcept:
		if (matcher.scenes.empty())
			return Tr("Sentence.AnyScene");
		return Tr("Sentence.AnyExcept").arg(JoinWithOr(matcher.scenes));
	case MatchType::Scenes:
		break;
	}
	if (matcher.scenes.empty())
		return Tr("Sentence.NoScene");
	return JoinWithOr(matcher.scenes);
}

/* The mark as one plain-English sentence, shown under the editor title. */
QString MarkSentence(const Mark &mark)
{
	const auto usable = mark.UsableCandidates();

	QString action;
	if (usable.empty()) {
		action = Tr("Sentence.Nothing");
	} else if (usable.size() == 1) {
		const Candidate &candidate = mark.candidates[usable.front()];
		action = Tr("Sentence.One").arg(Utf8(candidate.transition), QString::number(candidate.duration));
	} else {
		const char *key = "Sentence.Sequential";
		switch (mark.mode) {
		case SelectionMode::Sequential:
			key = "Sentence.Sequential";
			break;
		case SelectionMode::Random:
			key = "Sentence.Random";
			break;
		case SelectionMode::RandomNoRepeat:
			key = "Sentence.RandomNoRepeat";
			break;
		case SelectionMode::Weighted:
			key = "Sentence.Weighted";
			break;
		}
		action = Tr(key).arg(usable.size());
	}

	QString sentence = Tr("Sentence.Base").arg(SidePhrase(mark.from), SidePhrase(mark.to), action);
	if (!mark.enabled)
		sentence += " " + Tr("Sentence.Off");
	return sentence;
}

QString MarkTitle(const Mark &mark)
{
	return Tr("Mark.Title").arg(Utf8(mark.from.Describe()), Utf8(mark.to.Describe()));
}

} // namespace

/* ------------------------------------------------------------------------ */
/* Static entry points                                                       */
/* ------------------------------------------------------------------------ */

void TransitionTreeDialog::ShowDialog()
{
	if (g_instance) {
		g_instance->show();
		g_instance->raise();
		g_instance->activateWindow();
		return;
	}

	obs_frontend_push_ui_translation(obs_module_get_string);
	auto *dialog = new TransitionTreeDialog((QMainWindow *)obs_frontend_get_main_window());
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->show();
	obs_frontend_pop_ui_translation();
}

void TransitionTreeDialog::NotifyDataChanged()
{
	if (!g_instance)
		return;
	g_instance->RefreshAll();
}

void TransitionTreeDialog::SaveGeometry(obs_data_t *data)
{
	if (g_instance) {
		g_savedWidth = g_instance->width();
		g_savedHeight = g_instance->height();
	}
	if (g_savedWidth > 0 && g_savedHeight > 0) {
		obs_data_set_int(data, "dialog_width", g_savedWidth);
		obs_data_set_int(data, "dialog_height", g_savedHeight);
	}
}

void TransitionTreeDialog::RestoreGeometry(obs_data_t *data)
{
	g_savedWidth = (int)obs_data_get_int(data, "dialog_width");
	g_savedHeight = (int)obs_data_get_int(data, "dialog_height");
}

void TransitionTreeDialog::OfferPendingMigration()
{
	if (!g_pendingMigration)
		return;

	/* Take ownership up front so a declined prompt is not asked again. */
	obs_data_t *legacy = g_pendingMigration;
	g_pendingMigration = nullptr;

	auto *parent = (QMainWindow *)obs_frontend_get_main_window();
	const auto answer = QMessageBox::question(parent, Tr("Migrate.Title"), Tr("Migrate.Text"),
						  QMessageBox::Yes | QMessageBox::No);

	if (answer == QMessageBox::Yes) {
		Store &store = GetStore();
		Preset preset = PresetFromTransitionTable(legacy, obs_module_text("Migrate.PresetName"),
							  MainCanvasName().c_str());
		preset.global = false;
		preset.name = store.UniqueName(preset.name);

		const size_t markCount = preset.marks.size();
		store.presets.push_back(preset);
		RefreshPresetHotkeys();
		SetActivePreset(store.presets.back().name);

		obs_log(LOG_INFO, "migrated %zu Transition Table entries", markCount);
	}

	obs_data_release(legacy);
}

/* ------------------------------------------------------------------------ */
/* Construction                                                              */
/* ------------------------------------------------------------------------ */

TransitionTreeDialog::TransitionTreeDialog(QMainWindow *parent) : QDialog(parent)
{
	g_instance = this;

	setWindowTitle(Tr("TransitionTree"));
	setSizeGripEnabled(true);
	setMinimumSize(960, 560);
	if (g_savedWidth > 0 && g_savedHeight > 0)
		resize(g_savedWidth, g_savedHeight);
	setStyleSheet(kStyleSheet);

	BuildActions();

	auto *splitter = new QSplitter(Qt::Horizontal);
	splitter->addWidget(BuildTreePane());
	splitter->addWidget(BuildDetailPane());
	splitter->setStretchFactor(0, 1);
	splitter->setStretchFactor(1, 1);
	splitter->setChildrenCollapsible(false);
	/* Roughly even, so the tree has room for scene names. setSizes scales
	 * these to the real width. */
	splitter->setSizes({480, 520});

	auto *layout = new QVBoxLayout;
	layout->addWidget(BuildPresetBar());
	layout->addWidget(BuildOffBanner());
	layout->addWidget(splitter, 1);
	layout->addWidget(BuildBottomBar());
	setLayout(layout);

	RefreshAll();
	WarnAboutTransitionTable();
}

TransitionTreeDialog::~TransitionTreeDialog()
{
	g_savedWidth = width();
	g_savedHeight = height();
	if (g_instance == this)
		g_instance = nullptr;
}

void TransitionTreeDialog::BuildActions()
{
	m_addMarkAction = new QAction(Tr("Mark.Add"), this);
	m_addMarkAction->setToolTip(Tr("Mark.Add.Tooltip"));
	m_addMarkAction->setShortcut(QKeySequence::New);
	m_addMarkAction->setShortcutContext(Qt::WindowShortcut);
	connect(m_addMarkAction, &QAction::triggered, this, &TransitionTreeDialog::AddMark);
	addAction(m_addMarkAction);

	/* Mark shortcuts only fire while the tree has focus, so Delete in the
	 * search box or a spin box keeps its ordinary meaning. They are added
	 * to the tree once it exists. */
	m_duplicateMarkAction = new QAction(Tr("Mark.Duplicate"), this);
	m_duplicateMarkAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));
	m_duplicateMarkAction->setShortcutContext(Qt::WidgetShortcut);
	connect(m_duplicateMarkAction, &QAction::triggered, this, &TransitionTreeDialog::DuplicateMark);

	m_deleteMarkAction = new QAction(Tr("Mark.Delete"), this);
	m_deleteMarkAction->setShortcuts({QKeySequence(QKeySequence::Delete), QKeySequence(Qt::Key_Backspace)});
	m_deleteMarkAction->setShortcutContext(Qt::WidgetShortcut);
	connect(m_deleteMarkAction, &QAction::triggered, this, &TransitionTreeDialog::DeleteMark);

	m_toggleMarkAction = new QAction(Tr("Mark.TurnOff"), this);
	connect(m_toggleMarkAction, &QAction::triggered, this, &TransitionTreeDialog::ToggleMarkEnabled);

	m_testMarkAction = new QAction(Tr("Mark.TestEllipsis"), this);
	m_testMarkAction->setToolTip(Tr("Mark.Test.Tooltip"));
	connect(m_testMarkAction, &QAction::triggered, this, &TransitionTreeDialog::TestMark);

	auto *find = new QShortcut(QKeySequence::Find, this);
	connect(find, &QShortcut::activated, this, [this]() {
		m_search->setFocus(Qt::ShortcutFocusReason);
		m_search->selectAll();
	});
}

QWidget *TransitionTreeDialog::BuildPresetBar()
{
	auto *bar = new QWidget;
	auto *layout = new QHBoxLayout;
	layout->setContentsMargins(0, 0, 0, 0);

	/* The master switch leads the header: it decides whether anything else
	 * in this window has an effect at all. */
	m_enabledButton = new QPushButton;
	m_enabledButton->setObjectName("ttMaster");
	m_enabledButton->setCheckable(true);
	m_enabledButton->setAutoDefault(false);
	m_enabledButton->setToolTip(Tr("TreeEnabled.Tooltip"));
	connect(m_enabledButton, &QPushButton::toggled, this, [this](bool checked) {
		if (m_updating)
			return;
		SetTreeEnabled(checked);
		RefreshMasterState();
	});
	layout->addWidget(m_enabledButton);
	layout->addSpacing(12);

	layout->addWidget(new QLabel(Tr("Preset")));

	m_presetCombo = new QComboBox;
	m_presetCombo->setMinimumWidth(220);
	m_presetCombo->setToolTip(Tr("Preset.Tooltip"));
	connect(m_presetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		&TransitionTreeDialog::OnPresetChanged);
	layout->addWidget(m_presetCombo);

	auto *menu = new QMenu(this);
	auto addMenuAction = [&](const char *key, void (TransitionTreeDialog::*slot)()) {
		QAction *action = menu->addAction(Tr(key));
		connect(action, &QAction::triggered, this, slot);
		return action;
	};
	addMenuAction("Preset.New", &TransitionTreeDialog::AddPreset);
	m_duplicatePresetAction = addMenuAction("Preset.Duplicate", &TransitionTreeDialog::DuplicatePreset);
	m_renamePresetAction = addMenuAction("Preset.Rename", &TransitionTreeDialog::RenamePreset);
	menu->addSeparator();

	auto *scopeGroup = new QActionGroup(menu);
	scopeGroup->setExclusive(true);
	m_scopeLocalAction = menu->addAction(Tr("Scope.LocalMenu"));
	m_scopeGlobalAction = menu->addAction(Tr("Scope.GlobalMenu"));
	for (QAction *action : {m_scopeLocalAction, m_scopeGlobalAction}) {
		action->setCheckable(true);
		action->setToolTip(Tr("Scope.Tooltip"));
		scopeGroup->addAction(action);
	}
	connect(m_scopeLocalAction, &QAction::triggered, this, [this]() { ChangePresetScope(false); });
	connect(m_scopeGlobalAction, &QAction::triggered, this, [this]() { ChangePresetScope(true); });
	menu->setToolTipsVisible(true);
	menu->addSeparator();

	addMenuAction("Import", &TransitionTreeDialog::ImportPresets);
	m_exportAction = addMenuAction("Export", &TransitionTreeDialog::ExportPresets);
	menu->addSeparator();
	m_deletePresetAction = addMenuAction("Preset.Delete", &TransitionTreeDialog::DeletePreset);

	m_presetMenuButton = new QToolButton;
	m_presetMenuButton->setText(Tr("Menu.Glyph"));
	m_presetMenuButton->setToolTip(Tr("Preset.Menu"));
	m_presetMenuButton->setPopupMode(QToolButton::InstantPopup);
	m_presetMenuButton->setMenu(menu);
	layout->addWidget(m_presetMenuButton);

	layout->addStretch(1);
	QLabel *liveHint = DimLabel(Tr("LiveHint"));
	liveHint->setWordWrap(false);
	layout->addWidget(liveHint);

	bar->setLayout(layout);
	return bar;
}

QWidget *TransitionTreeDialog::BuildOffBanner()
{
	m_offBanner = new QFrame;
	m_offBanner->setObjectName("ttBanner");

	auto *layout = new QHBoxLayout;
	layout->setContentsMargins(10, 6, 6, 6);

	auto *text = new QLabel(Tr("TreeOff.Banner"));
	text->setWordWrap(true);
	text->setTextFormat(Qt::RichText);
	layout->addWidget(text, 1);

	auto *turnOn = new QPushButton(Tr("TreeOff.TurnOn"));
	turnOn->setAutoDefault(false);
	connect(turnOn, &QPushButton::clicked, this, []() { SetTreeEnabled(true); });
	layout->addWidget(turnOn);

	m_offBanner->setLayout(layout);
	m_offBanner->setVisible(false);
	return m_offBanner;
}

QWidget *TransitionTreeDialog::BuildTreePane()
{
	auto *pane = new QWidget;
	auto *layout = new QVBoxLayout;
	layout->setContentsMargins(0, 0, 0, 0);

	/* The search box only ever filters the view. Adding a mark is a separate
	 * button, so narrowing the list can never hide the controls needed to
	 * create one. */
	auto *toolbar = new QHBoxLayout;
	m_search = new QLineEdit;
	m_search->setClearButtonEnabled(true);
	m_search->setPlaceholderText(Tr("Search.Placeholder"));
	connect(m_search, &QLineEdit::textChanged, this, &TransitionTreeDialog::OnSearchChanged);
	toolbar->addWidget(m_search, 1);

	auto *addMark = new QPushButton(Tr("Mark.Add"));
	addMark->setToolTip(Tr("Mark.Add.Tooltip"));
	addMark->setAutoDefault(false);
	connect(addMark, &QPushButton::clicked, this, &TransitionTreeDialog::AddMark);
	toolbar->addWidget(addMark);
	layout->addLayout(toolbar);

	m_tree = new QTreeWidget;
	m_tree->setColumnCount(kTreeColumnCount);
	m_tree->setHeaderLabels({Tr("Column.Scenes"), Tr("Column.Transition"), Tr("Column.Mode"), Tr("Column.Canvas")});
	m_tree->setRootIsDecorated(true);
	m_tree->setAlternatingRowColors(true);
	m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_tree->header()->setStretchLastSection(false);
	m_tree->header()->setSectionResizeMode(kColScenes, QHeaderView::Stretch);
	m_tree->header()->setSectionResizeMode(kColTransition, QHeaderView::ResizeToContents);
	m_tree->header()->setSectionResizeMode(kColMode, QHeaderView::ResizeToContents);
	m_tree->header()->setSectionResizeMode(kColCanvas, QHeaderView::ResizeToContents);
	m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
	m_tree->addAction(m_duplicateMarkAction);
	m_tree->addAction(m_deleteMarkAction);
	connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &TransitionTreeDialog::ShowTreeContextMenu);
	connect(m_tree, &QTreeWidget::itemSelectionChanged, this, &TransitionTreeDialog::OnTreeSelectionChanged);
	connect(m_tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item, int column) {
		if (m_updating || column != kColScenes)
			return;
		const std::string markId = item->data(kColScenes, kMarkIdRole).toString().toStdString();
		Preset *preset = CurrentPreset();
		if (!preset || markId.empty())
			return;
		Mark *mark = preset->FindMark(markId);
		const bool enabled = item->checkState(kColScenes) == Qt::Checked;
		if (!mark || mark->enabled == enabled)
			return;
		mark->enabled = enabled;
		/* The tree is rebuilt on commit, which would delete the item that
		 * is still emitting this signal, so let the signal finish first. */
		QMetaObject::invokeMethod(
			this,
			[this]() {
				CommitChange(true);
				RefreshDetail();
			},
			Qt::QueuedConnection);
	});

	auto *empty = new QWidget;
	auto *emptyLayout = new QVBoxLayout;
	emptyLayout->addStretch(1);
	m_emptyLabel = DimLabel();
	m_emptyLabel->setAlignment(Qt::AlignCenter);
	emptyLayout->addWidget(m_emptyLabel);
	m_emptyButton = new QPushButton;
	m_emptyButton->setAutoDefault(false);
	connect(m_emptyButton, &QPushButton::clicked, this, [this]() {
		switch (m_emptyState) {
		case EmptyState::NoPreset:
			AddPreset();
			break;
		case EmptyState::NoMarks:
			AddMark();
			break;
		case EmptyState::NoMatches:
			m_search->clear();
			break;
		case EmptyState::None:
			break;
		}
	});
	emptyLayout->addWidget(m_emptyButton, 0, Qt::AlignHCenter);
	emptyLayout->addStretch(2);
	empty->setLayout(emptyLayout);

	m_treeStack = new QStackedWidget;
	m_treeStack->addWidget(m_tree);
	m_treeStack->addWidget(empty);
	layout->addWidget(m_treeStack, 1);

	m_undoBar = new QFrame;
	m_undoBar->setObjectName("ttUndoBar");
	auto *undoLayout = new QHBoxLayout;
	undoLayout->setContentsMargins(10, 4, 4, 4);
	m_undoLabel = new QLabel;
	undoLayout->addWidget(m_undoLabel, 1);
	auto *undoButton = new QPushButton(Tr("Mark.Undo"));
	undoButton->setAutoDefault(false);
	connect(undoButton, &QPushButton::clicked, this, &TransitionTreeDialog::UndoDeleteMark);
	undoLayout->addWidget(undoButton);
	m_undoBar->setLayout(undoLayout);
	m_undoBar->setVisible(false);
	layout->addWidget(m_undoBar);

	m_undoTimer = new QTimer(this);
	m_undoTimer->setSingleShot(true);
	m_undoTimer->setInterval(kUndoTimeoutMs);
	connect(m_undoTimer, &QTimer::timeout, this, &TransitionTreeDialog::ClearUndo);

	m_treeFooter = DimLabel();
	layout->addWidget(m_treeFooter);

	pane->setLayout(layout);
	return pane;
}

QWidget *TransitionTreeDialog::BuildDetailPane()
{
	m_detail = new QStackedWidget;
	m_detail->addWidget(BuildPresetPage());
	m_detail->addWidget(BuildMarkPage());

	/* The mark editor can outgrow a short window; scroll rather than
	 * squeezing the scene lists. */
	auto *scroll = new QScrollArea;
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setWidget(m_detail);
	return scroll;
}

QWidget *TransitionTreeDialog::BuildPresetPage()
{
	auto *page = new QWidget;
	auto *layout = new QVBoxLayout;

	m_presetTitle = new QLabel;
	m_presetTitle->setObjectName("ttTitle");
	m_presetTitle->setWordWrap(true);
	layout->addWidget(m_presetTitle);

	m_presetSummary = DimLabel();
	layout->addWidget(m_presetSummary);

	auto *group = new QGroupBox(Tr("Preset.DefaultTransition"));
	auto *form = new QFormLayout;

	m_setDefaultTransition = new QCheckBox(Tr("Preset.SetDefaultTransition"));
	m_setDefaultTransition->setToolTip(Tr("Preset.SetDefaultTransition.Tooltip"));
	connect(m_setDefaultTransition, &QCheckBox::toggled, this, [this](bool checked) {
		if (m_updating)
			return;
		Preset *preset = CurrentPreset();
		if (!preset)
			return;
		preset->setDefaultTransition = checked;
		m_defaultTransition->setEnabled(checked);
		m_defaultDuration->setEnabled(checked);
		ApplyPresetDefaultTransition();
		CommitChange(false);
	});
	form->addRow(m_setDefaultTransition);

	m_defaultTransition = new QComboBox;
	connect(m_defaultTransition, &QComboBox::currentTextChanged, this, [this](const QString &text) {
		if (m_updating)
			return;
		Preset *preset = CurrentPreset();
		if (!preset)
			return;
		preset->defaultTransition = text.toStdString();
		ApplyPresetDefaultTransition();
		CommitChange(false);
	});
	form->addRow(Tr("Column.Transition"), m_defaultTransition);

	m_defaultDuration = new QSpinBox;
	m_defaultDuration->setRange(0, 20000);
	m_defaultDuration->setSingleStep(50);
	m_defaultDuration->setSuffix(" ms");
	connect(m_defaultDuration, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
		if (m_updating)
			return;
		Preset *preset = CurrentPreset();
		if (!preset)
			return;
		preset->defaultDuration = value;
		ApplyPresetDefaultTransition();
		CommitChange(false);
	});
	form->addRow(Tr("Column.Duration"), m_defaultDuration);

	group->setLayout(form);
	layout->addWidget(group);

	layout->addWidget(DimLabel(Tr("Preset.Hint")));

	auto *share = new QHBoxLayout;
	auto *importButton = new QPushButton(Tr("Import"));
	importButton->setAutoDefault(false);
	connect(importButton, &QPushButton::clicked, this, &TransitionTreeDialog::ImportPresets);
	share->addWidget(importButton);
	m_presetExportButton = new QPushButton(Tr("Export"));
	m_presetExportButton->setAutoDefault(false);
	connect(m_presetExportButton, &QPushButton::clicked, this, &TransitionTreeDialog::ExportPresets);
	share->addWidget(m_presetExportButton);
	share->addStretch(1);
	layout->addLayout(share);

	layout->addStretch(1);
	page->setLayout(layout);
	return page;
}

QWidget *TransitionTreeDialog::BuildMarkPage()
{
	auto *page = new QWidget;
	auto *layout = new QVBoxLayout;

	/* Header: what is being edited, in words, with the mark's own controls
	 * beside it. Test performs a real switch, so it is set apart from
	 * Delete, which lives in the menu with an undo. */
	auto *header = new QHBoxLayout;
	auto *titles = new QVBoxLayout;
	titles->setSpacing(2);
	m_markTitle = new QLabel;
	m_markTitle->setObjectName("ttTitle");
	m_markTitle->setWordWrap(true);
	titles->addWidget(m_markTitle);
	m_markSentence = DimLabel();
	titles->addWidget(m_markSentence);
	header->addLayout(titles, 1);

	m_markEnabled = new QCheckBox(Tr("Mark.On"));
	m_markEnabled->setToolTip(Tr("Mark.On.Tooltip"));
	connect(m_markEnabled, &QCheckBox::toggled, this, [this](bool checked) {
		if (m_updating)
			return;
		Mark *mark = SelectedMark();
		if (!mark)
			return;
		mark->enabled = checked;
		RefreshMarkSummary(*mark);
		CommitChange(true);
	});
	header->addWidget(m_markEnabled, 0, Qt::AlignTop);

	m_testMarkButton = new QPushButton(Tr("Mark.Test"));
	m_testMarkButton->setObjectName("ttTest");
	m_testMarkButton->setToolTip(Tr("Mark.Test.Tooltip"));
	m_testMarkButton->setAutoDefault(false);
	connect(m_testMarkButton, &QPushButton::clicked, this, &TransitionTreeDialog::TestMark);
	header->addWidget(m_testMarkButton, 0, Qt::AlignTop);

	auto *markMenu = new QMenu(this);
	markMenu->addAction(m_duplicateMarkAction);
	markMenu->addSeparator();
	markMenu->addAction(m_deleteMarkAction);
	m_markMenuButton = new QToolButton;
	m_markMenuButton->setText(Tr("Menu.Glyph"));
	m_markMenuButton->setToolTip(Tr("Mark.Menu"));
	m_markMenuButton->setPopupMode(QToolButton::InstantPopup);
	m_markMenuButton->setMenu(markMenu);
	header->addWidget(m_markMenuButton, 0, Qt::AlignTop);
	layout->addLayout(header);

	m_markWarning = new QLabel;
	m_markWarning->setObjectName("ttWarning");
	m_markWarning->setWordWrap(true);
	m_markWarning->setVisible(false);
	layout->addWidget(m_markWarning);

	/* Canvas decides which scenes the lists below offer, so it comes first,
	 * and only appears when there is more than one canvas to choose from. */
	m_canvasRow = new QWidget;
	auto *canvasLayout = new QHBoxLayout;
	canvasLayout->setContentsMargins(0, 0, 0, 0);
	canvasLayout->addWidget(new QLabel(Tr("Column.Canvas")));
	m_canvasCombo = new QComboBox;
	canvasLayout->addWidget(m_canvasCombo, 1);
	m_canvasRow->setLayout(canvasLayout);
	layout->addWidget(m_canvasRow);
	connect(m_canvasCombo, &QComboBox::currentTextChanged, this, [this](const QString &text) {
		if (m_updating)
			return;
		Mark *mark = SelectedMark();
		if (!mark)
			return;
		mark->canvas = text.toStdString();
		CommitChange(true);
		/* The scene lists follow the canvas; rebuild them once the combo
		 * has finished emitting. */
		QMetaObject::invokeMethod(
			this,
			[this]() {
				if (Mark *current = SelectedMark())
					RefreshMarkPage(*current);
			},
			Qt::QueuedConnection);
	});

	auto buildMatcherGroup = [this](const char *titleKey, QButtonGroup *&typeGroup, QListWidget *&sceneList,
					QLabel *&anyNote, QLabel *&count) {
		auto *group = new QGroupBox(Tr(titleKey));
		auto *groupLayout = new QVBoxLayout;

		/* A segmented control: all three choices visible at once. */
		auto *segments = new QHBoxLayout;
		segments->setSpacing(0);
		typeGroup = new QButtonGroup(group);
		typeGroup->setExclusive(true);
		const std::pair<MatchType, const char *> types[] = {
			{MatchType::Scenes, "Match.Button.Scenes"},
			{MatchType::Any, "Match.Button.Any"},
			{MatchType::AnyExcept, "Match.Button.AnyExcept"},
		};
		for (const auto &[type, key] : types) {
			auto *button = new QToolButton;
			button->setText(Tr(key));
			button->setCheckable(true);
			button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
			typeGroup->addButton(button, (int)type);
			segments->addWidget(button);
		}
		groupLayout->addLayout(segments);

		sceneList = new QListWidget;
		sceneList->setSelectionMode(QAbstractItemView::NoSelection);
		sceneList->setMinimumHeight(110);
		sceneList->setMaximumHeight(180);
		groupLayout->addWidget(sceneList);

		anyNote = DimLabel(Tr("Match.AnyNote"));
		groupLayout->addWidget(anyNote);

		count = DimLabel();
		groupLayout->addWidget(count);

		groupLayout->addStretch(1);
		group->setLayout(groupLayout);
		return group;
	};

	/* From and To side by side, so the editor reads in the same direction
	 * as the rule. */
	auto *sides = new QHBoxLayout;
	sides->addWidget(buildMatcherGroup("Mark.From", m_fromType, m_fromScenes, m_fromAnyNote, m_fromCount), 1);
	sides->addWidget(buildMatcherGroup("Mark.To", m_toType, m_toScenes, m_toAnyNote, m_toCount), 1);
	layout->addLayout(sides);

	auto matcherChanged = [this]() {
		if (m_updating)
			return;
		Mark *mark = SelectedMark();
		if (!mark)
			return;
		ReadMatcherFromWidgets(mark->from, m_fromType, m_fromScenes);
		ReadMatcherFromWidgets(mark->to, m_toType, m_toScenes);
		/* Refreshed in place rather than by rebuilding the page, so the
		 * list the user is clicking through is not destroyed under them. */
		RefreshMatcherState(mark->from, m_fromScenes, m_fromAnyNote, m_fromCount);
		RefreshMatcherState(mark->to, m_toScenes, m_toAnyNote, m_toCount);
		RefreshMarkSummary(*mark);
		CommitChange(true);
	};
	connect(m_fromType, &QButtonGroup::idClicked, this, matcherChanged);
	connect(m_toType, &QButtonGroup::idClicked, this, matcherChanged);
	connect(m_fromScenes, &QListWidget::itemChanged, this, matcherChanged);
	connect(m_toScenes, &QListWidget::itemChanged, this, matcherChanged);

	auto *candidateGroup = new QGroupBox(Tr("Candidates"));
	auto *candidateLayout = new QVBoxLayout;

	/* How to choose only matters once there is something to choose from. */
	m_modeRow = new QWidget;
	auto *modeLayout = new QHBoxLayout;
	modeLayout->setContentsMargins(0, 0, 0, 0);
	modeLayout->addWidget(new QLabel(Tr("Mode.Label")));
	m_modeCombo = new QComboBox;
	m_modeCombo->addItem(ModeName(SelectionMode::Sequential));
	m_modeCombo->addItem(ModeName(SelectionMode::Random));
	m_modeCombo->addItem(ModeName(SelectionMode::RandomNoRepeat));
	m_modeCombo->addItem(ModeName(SelectionMode::Weighted));
	m_modeCombo->setToolTip(Tr("Mode.Tooltip"));
	connect(m_modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
		if (m_updating)
			return;
		Mark *mark = SelectedMark();
		if (!mark)
			return;
		mark->mode = (SelectionMode)index;
		mark->ResetRuntimeState();
		RefreshMarkSummary(*mark);
		CommitChange(true);
	});
	modeLayout->addWidget(m_modeCombo, 1);
	m_modeRow->setLayout(modeLayout);
	candidateLayout->addWidget(m_modeRow);

	m_candidates = new QTableWidget(0, kCandColumnCount);
	m_candidates->setHorizontalHeaderLabels({Tr("Column.Transition"), Tr("Column.Duration"), Tr("Column.Weight"),
						 Tr("Column.Share"), Tr("Column.Enabled"), QString()});
	m_candidates->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
	m_candidates->horizontalHeader()->setSectionResizeMode(kCandTransition, QHeaderView::Stretch);
	m_candidates->verticalHeader()->setVisible(false);
	m_candidates->setSelectionMode(QAbstractItemView::NoSelection);
	m_candidates->setMinimumHeight(110);
	candidateLayout->addWidget(m_candidates, 1);

	auto *candidateButtons = new QHBoxLayout;
	auto *addCandidate = new QPushButton(Tr("Candidate.Add"));
	addCandidate->setAutoDefault(false);
	connect(addCandidate, &QPushButton::clicked, this, &TransitionTreeDialog::AddCandidate);
	candidateButtons->addWidget(addCandidate);
	candidateButtons->addStretch(1);

	auto *newTransition = new QPushButton(Tr("NewTransition"));
	newTransition->setToolTip(Tr("NewTransition.Tooltip"));
	newTransition->setAutoDefault(false);
	connect(newTransition, &QPushButton::clicked, this, &TransitionTreeDialog::CreateTransition);
	candidateButtons->addWidget(newTransition);
	candidateLayout->addLayout(candidateButtons);

	candidateGroup->setLayout(candidateLayout);
	layout->addWidget(candidateGroup, 1);

	page->setLayout(layout);
	return page;
}

QWidget *TransitionTreeDialog::BuildBottomBar()
{
	auto *bar = new QWidget;
	auto *layout = new QHBoxLayout;
	layout->setContentsMargins(0, 0, 0, 0);

	auto *credit = new QLabel(QString("<a href=\"https://voidscape.dev\">Transition Tree</a> (%1) "
					  "by Voidscape Development")
					  .arg(QString::fromUtf8(PLUGIN_VERSION)));
	credit->setObjectName("ttDim");
	credit->setOpenExternalLinks(true);
	layout->addWidget(credit, 1, Qt::AlignLeft);

	auto *shortcuts = new QLabel(Tr("Shortcuts.Hint"));
	shortcuts->setObjectName("ttDim");
	layout->addWidget(shortcuts);

	auto *closeButton = new QPushButton(Tr("Close"));
	closeButton->setAutoDefault(false);
	connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
	layout->addWidget(closeButton);

	bar->setLayout(layout);
	return bar;
}

/* ------------------------------------------------------------------------ */
/* Model access                                                              */
/* ------------------------------------------------------------------------ */

Preset *TransitionTreeDialog::CurrentPreset()
{
	if (!m_presetCombo)
		return nullptr;
	const QString name = m_presetCombo->currentData().toString();
	if (name.isEmpty())
		return nullptr;
	return GetStore().Find(name.toStdString());
}

Mark *TransitionTreeDialog::SelectedMark()
{
	Preset *preset = CurrentPreset();
	if (!preset || m_selectedMarkId.empty())
		return nullptr;
	return preset->FindMark(m_selectedMarkId);
}

void TransitionTreeDialog::CommitChange(bool structural)
{
	Store &store = GetStore();
	store.Touch();

	Preset *preset = CurrentPreset();
	if (preset && preset->global)
		SaveGlobalPresets();

	ApplyOverrides();

	if (structural)
		RefreshTree();
}

/* ------------------------------------------------------------------------ */
/* Refresh                                                                   */
/* ------------------------------------------------------------------------ */

void TransitionTreeDialog::RefreshAll()
{
	RefreshPresetCombo();

	/* An undo can only restore into the preset it came from, and only while
	 * that preset is the one on screen. */
	Preset *preset = CurrentPreset();
	if (m_undoMark && (!preset || preset->name != m_undoPreset))
		ClearUndo();

	RefreshTree();
	RefreshDetail();
	RefreshMasterState();
}

void TransitionTreeDialog::RefreshMasterState()
{
	const bool enabled = IsTreeEnabled();

	m_updating = true;
	m_enabledButton->setChecked(enabled);
	m_updating = false;

	m_enabledButton->setText(enabled ? Tr("TreeEnabled.On") : Tr("TreeEnabled.Off"));
	m_offBanner->setVisible(!enabled);
}

void TransitionTreeDialog::RefreshPresetCombo()
{
	m_updating = true;

	const QString previous = m_presetCombo->currentData().toString();
	m_presetCombo->clear();

	Store &store = GetStore();
	for (const auto &preset : store.presets) {
		const QString name = Utf8(preset.name);
		const QString label = preset.global ? Tr("Preset.GlobalLabel").arg(name) : name;
		m_presetCombo->addItem(label, name);
	}

	/* Choosing a preset here activates it, so the combo always shows the
	 * active preset, including after a hotkey or obs-websocket switch. Only
	 * fall back to what was shown when nothing is active. */
	QString target = Utf8(store.activePreset);
	if (!store.Find(store.activePreset) && !previous.isEmpty() && store.Find(previous.toStdString()))
		target = previous;

	const int index = m_presetCombo->findData(target);
	m_presetCombo->setCurrentIndex(index >= 0 ? index : (m_presetCombo->count() > 0 ? 0 : -1));

	m_updating = false;

	RefreshPresetActions();
}

void TransitionTreeDialog::RefreshPresetActions()
{
	Preset *preset = CurrentPreset();
	const bool hasPreset = preset != nullptr;

	m_duplicatePresetAction->setEnabled(hasPreset);
	m_renamePresetAction->setEnabled(hasPreset);
	m_deletePresetAction->setEnabled(hasPreset);
	m_exportAction->setEnabled(hasPreset);
	m_scopeLocalAction->setEnabled(hasPreset);
	m_scopeGlobalAction->setEnabled(hasPreset);
	m_scopeLocalAction->setChecked(hasPreset && !preset->global);
	m_scopeGlobalAction->setChecked(hasPreset && preset->global);
	if (m_presetExportButton)
		m_presetExportButton->setEnabled(hasPreset);
}

void TransitionTreeDialog::RefreshTree()
{
	m_updating = true;

	/* Remember which groups were open so editing does not collapse the view. */
	QStringList expanded;
	for (int i = 0; i < m_tree->topLevelItemCount(); i++) {
		QTreeWidgetItem *item = m_tree->topLevelItem(i);
		if (item->isExpanded())
			expanded << item->data(kColScenes, kGroupKeyRole).toString();
	}

	m_tree->clear();

	/* Almost everyone has a single canvas; a column that says "Main" on
	 * every row is noise. */
	m_tree->setColumnHidden(kColCanvas, CanvasNames().size() < 2);

	Preset *preset = CurrentPreset();
	const QString filter = m_search->text().trimmed();
	size_t total = 0;
	size_t enabledCount = 0;
	size_t problemCount = 0;

	if (preset) {
		const QString mainCanvas = Utf8(MainCanvasName());
		const QIcon warningIcon = style()->standardIcon(QStyle::SP_MessageBoxWarning);

		QHash<QString, QTreeWidgetItem *> groups;
		QHash<QString, int> groupCounts;

		for (auto &mark : preset->marks) {
			const QStringList problems = MarkProblems(mark);
			total++;
			if (mark.enabled)
				enabledCount++;
			if (!problems.isEmpty())
				problemCount++;

			const QString fromText = Utf8(mark.from.Describe());
			const QString toText = Utf8(mark.to.Describe());
			const QString canvas = mark.canvas.empty() ? mainCanvas : Utf8(mark.canvas);
			const QString summary = MarkSummary(mark);
			const auto usable = mark.UsableCandidates();

			if (!filter.isEmpty()) {
				QString haystack = fromText + " " + toText + " " + summary + " " + canvas + " " +
						   ModeName(mark.mode);
				for (const auto &candidate : mark.candidates)
					haystack += " " + Utf8(candidate.transition);
				if (!haystack.contains(filter, Qt::CaseInsensitive))
					continue;
			}

			const QString groupKey = fromText;
			QTreeWidgetItem *group = groups.value(groupKey, nullptr);
			if (!group) {
				group = new QTreeWidgetItem(m_tree);
				group->setData(kColScenes, kGroupKeyRole, groupKey);
				QFont font = group->font(kColScenes);
				font.setBold(true);
				group->setFont(kColScenes, font);
				if (!mark.from.MissingScenes().empty())
					group->setForeground(kColScenes, InvalidColor());
				groups.insert(groupKey, group);
			}
			groupCounts[groupKey]++;

			auto *item = new QTreeWidgetItem(group);
			item->setText(kColScenes, Tr("Mark.ToPrefix").arg(toText));
			item->setText(kColTransition, summary);
			item->setToolTip(kColTransition, CandidateList(mark));
			/* The mode only means something with more than one choice. */
			item->setText(kColMode, usable.size() > 1 ? ModeName(mark.mode) : QString());
			item->setText(kColCanvas, canvas);
			item->setData(kColScenes, kMarkIdRole, Utf8(mark.id));
			item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
			item->setCheckState(kColScenes, mark.enabled ? Qt::Checked : Qt::Unchecked);

			if (!problems.isEmpty()) {
				item->setIcon(kColScenes, warningIcon);
				item->setToolTip(kColScenes, problems.join("\n"));
			}
			if (!MissingScenes(mark).empty())
				item->setForeground(kColScenes, InvalidColor());

			if (!mark.enabled) {
				for (int column = 0; column < kTreeColumnCount; column++) {
					if (column != kColScenes || MissingScenes(mark).empty())
						item->setForeground(column, DimColor());
				}
				QFont font = item->font(kColScenes);
				font.setItalic(true);
				item->setFont(kColScenes, font);
			}
		}

		for (int i = 0; i < m_tree->topLevelItemCount(); i++) {
			QTreeWidgetItem *item = m_tree->topLevelItem(i);
			const QString key = item->data(kColScenes, kGroupKeyRole).toString();
			item->setText(kColScenes, Tr("Tree.Group").arg(key, QString::number(groupCounts.value(key))));
			/* Expand by default; a filtered view is most useful open. */
			item->setExpanded(expanded.isEmpty() || expanded.contains(key) || !filter.isEmpty());
		}
	}

	/* Show a placeholder that says what to do instead of an empty list. */
	if (!preset) {
		m_emptyState = EmptyState::NoPreset;
		m_emptyLabel->setText(Tr("Preset.None"));
		m_emptyButton->setText(Tr("Preset.New"));
	} else if (preset->marks.empty()) {
		m_emptyState = EmptyState::NoMarks;
		m_emptyLabel->setText(Tr("Empty.NoMarks"));
		m_emptyButton->setText(Tr("Empty.AddFirst"));
	} else if (m_tree->topLevelItemCount() == 0) {
		m_emptyState = EmptyState::NoMatches;
		m_emptyLabel->setText(Tr("Empty.NoMatches").arg(filter));
		m_emptyButton->setText(Tr("Empty.ClearFilter"));
	} else {
		m_emptyState = EmptyState::None;
	}
	m_treeStack->setCurrentIndex(m_emptyState == EmptyState::None ? 0 : 1);

	QString footer;
	if (total > 0) {
		footer = Tr("Tree.Footer").arg(total).arg(enabledCount);
		if (problemCount > 0)
			footer = Tr("Tree.FooterProblems").arg(footer).arg(problemCount);
	}
	m_treeFooter->setText(footer);
	m_treeFooter->setVisible(!footer.isEmpty());

	/* Restore the previous selection where the mark still exists. */
	if (!m_selectedMarkId.empty()) {
		const QString wanted = Utf8(m_selectedMarkId);
		QTreeWidgetItemIterator iterator(m_tree);
		while (*iterator) {
			if ((*iterator)->data(kColScenes, kMarkIdRole).toString() == wanted) {
				(*iterator)->setSelected(true);
				m_tree->setCurrentItem(*iterator);
				break;
			}
			++iterator;
		}
	}

	m_updating = false;

	RefreshMarkActions();
}

void TransitionTreeDialog::RefreshMarkActions()
{
	Mark *mark = SelectedMark();
	const bool hasMark = mark != nullptr;

	m_duplicateMarkAction->setEnabled(hasMark);
	m_deleteMarkAction->setEnabled(hasMark);
	m_toggleMarkAction->setEnabled(hasMark);
	m_testMarkAction->setEnabled(hasMark);
	m_toggleMarkAction->setText(hasMark && !mark->enabled ? Tr("Mark.TurnOn") : Tr("Mark.TurnOff"));
	m_addMarkAction->setEnabled(CurrentPreset() != nullptr);
}

void TransitionTreeDialog::RefreshDetail()
{
	Mark *mark = SelectedMark();
	if (mark) {
		RefreshMarkPage(*mark);
		m_detail->setCurrentIndex(1);
	} else {
		RefreshPresetPage();
		m_detail->setCurrentIndex(0);
	}
	RefreshMarkActions();
}

void TransitionTreeDialog::RefreshMatcherWidgets(const SceneMatcher &matcher, QButtonGroup *typeGroup,
						 QListWidget *sceneList, QLabel *anyNote, QLabel *count,
						 const QString &canvas)
{
	if (QAbstractButton *button = typeGroup->button((int)matcher.type))
		button->setChecked(true);

	sceneList->clear();

	QStringList scenes = SceneNamesForCanvas(canvas);

	/* Keep scene names the matcher references even when they are missing from
	 * this collection, so switching collections never silently drops them. */
	for (const auto &name : matcher.scenes) {
		const QString scene = Utf8(name);
		if (!scenes.contains(scene))
			scenes << scene;
	}

	SortNames(scenes);

	const auto missing = matcher.MissingScenes();
	const QIcon warningIcon = style()->standardIcon(QStyle::SP_MessageBoxWarning);
	for (const QString &scene : scenes) {
		auto *item = new QListWidgetItem(scene, sceneList);
		item->setFlags(item->flags() | Qt::ItemIsUserCheckable);

		const bool checked = std::find(matcher.scenes.begin(), matcher.scenes.end(), scene.toStdString()) !=
				     matcher.scenes.end();
		item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);

		if (std::find(missing.begin(), missing.end(), scene.toStdString()) != missing.end()) {
			item->setForeground(InvalidColor());
			item->setIcon(warningIcon);
			item->setToolTip(Tr("Scene.Missing"));
		}
	}

	RefreshMatcherState(matcher, sceneList, anyNote, count);
}

void TransitionTreeDialog::RefreshMatcherState(const SceneMatcher &matcher, QListWidget *sceneList, QLabel *anyNote,
					       QLabel *count)
{
	/* "Any scene" needs no list at all; say what it means instead of
	 * leaving a disabled list on screen. */
	const bool any = matcher.type == MatchType::Any;
	sceneList->setVisible(!any);
	count->setVisible(!any);
	anyNote->setVisible(any);

	const int selected = (int)matcher.scenes.size();
	count->setText(matcher.type == MatchType::AnyExcept ? Tr("Match.ExcludedCount").arg(selected)
							    : Tr("Match.SelectedCount").arg(selected));
}

void TransitionTreeDialog::RefreshMarkPage(Mark &mark)
{
	m_updating = true;

	const QString canvas = mark.canvas.empty() ? Utf8(MainCanvasName()) : Utf8(mark.canvas);
	const QStringList canvases = CanvasNames();

	m_canvasCombo->clear();
	m_canvasCombo->addItems(canvases);
	const int canvasIndex = m_canvasCombo->findText(canvas);
	if (canvasIndex >= 0)
		m_canvasCombo->setCurrentIndex(canvasIndex);
	m_canvasRow->setVisible(canvases.size() > 1);

	RefreshMatcherWidgets(mark.from, m_fromType, m_fromScenes, m_fromAnyNote, m_fromCount, canvas);
	RefreshMatcherWidgets(mark.to, m_toType, m_toScenes, m_toAnyNote, m_toCount, canvas);

	m_modeCombo->setCurrentIndex((int)mark.mode);
	m_markEnabled->setChecked(mark.enabled);

	m_updating = false;

	RefreshCandidateTable(mark);
	RefreshMarkSummary(mark);
}

void TransitionTreeDialog::RefreshMarkSummary(const Mark &mark)
{
	m_markTitle->setText(MarkTitle(mark));
	m_markSentence->setText(MarkSentence(mark));
	RefreshMarkWarning(mark);

	m_modeRow->setVisible(mark.candidates.size() > 1);
	const bool weighted = mark.mode == SelectionMode::Weighted && mark.candidates.size() > 1;
	m_candidates->setColumnHidden(kCandWeight, !weighted);
	m_candidates->setColumnHidden(kCandShare, !weighted);
	RefreshCandidateShares(mark);

	RefreshMarkActions();
}

void TransitionTreeDialog::RefreshMarkWarning(const Mark &mark)
{
	const QStringList problems = MarkProblems(mark);
	m_markWarning->setText(problems.join("\n"));
	m_markWarning->setVisible(!problems.isEmpty());
}

void TransitionTreeDialog::RefreshCandidateTable(Mark &mark)
{
	m_updating = true;

	QStringList transitions = TransitionNames();
	SortNames(transitions);

	m_candidates->setRowCount((int)mark.candidates.size());
	const int lastRow = (int)mark.candidates.size() - 1;

	for (int row = 0; row < (int)mark.candidates.size(); row++) {
		const Candidate &candidate = mark.candidates[(size_t)row];

		auto *transitionCombo = new QComboBox;
		transitionCombo->addItems(transitions);
		const QString current = Utf8(candidate.transition);
		if (transitionCombo->findText(current) < 0 && !current.isEmpty())
			transitionCombo->addItem(current);
		transitionCombo->setCurrentText(current);
		connect(transitionCombo, &QComboBox::currentTextChanged, this, [this, row](const QString &text) {
			if (m_updating)
				return;
			Mark *target = SelectedMark();
			if (!target || row >= (int)target->candidates.size())
				return;
			target->candidates[(size_t)row].transition = text.toStdString();
			RefreshMarkSummary(*target);
			CommitChange(true);
		});
		m_candidates->setCellWidget(row, kCandTransition, transitionCombo);

		auto *duration = new QSpinBox;
		duration->setRange(0, 20000);
		duration->setSingleStep(50);
		duration->setSuffix(" ms");
		duration->setValue(candidate.duration);
		connect(duration, QOverload<int>::of(&QSpinBox::valueChanged), this, [this, row](int value) {
			if (m_updating)
				return;
			Mark *target = SelectedMark();
			if (!target || row >= (int)target->candidates.size())
				return;
			target->candidates[(size_t)row].duration = value;
			RefreshMarkSummary(*target);
			CommitChange(true);
		});
		m_candidates->setCellWidget(row, kCandDuration, duration);

		auto *weight = new QDoubleSpinBox;
		weight->setRange(0.0, 1000.0);
		weight->setSingleStep(0.5);
		weight->setDecimals(2);
		weight->setValue(candidate.weight);
		connect(weight, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, row](double value) {
			if (m_updating)
				return;
			Mark *target = SelectedMark();
			if (!target || row >= (int)target->candidates.size())
				return;
			target->candidates[(size_t)row].weight = value;
			RefreshCandidateShares(*target);
			CommitChange(true);
		});
		m_candidates->setCellWidget(row, kCandWeight, weight);

		auto *share = new QTableWidgetItem;
		share->setFlags(Qt::ItemIsEnabled);
		share->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
		m_candidates->setItem(row, kCandShare, share);

		auto *enabled = new QCheckBox;
		enabled->setChecked(candidate.enabled);
		connect(enabled, &QCheckBox::toggled, this, [this, row](bool checked) {
			if (m_updating)
				return;
			Mark *target = SelectedMark();
			if (!target || row >= (int)target->candidates.size())
				return;
			target->candidates[(size_t)row].enabled = checked;
			RefreshMarkSummary(*target);
			CommitChange(true);
		});
		m_candidates->setCellWidget(row, kCandEnabled, enabled);

		/* Per-row actions, so there is never a question of which row a
		 * toolbar button would act on. They rebuild the table, which
		 * deletes the button being clicked, so they run after the click. */
		auto *actions = new QWidget;
		auto *actionsLayout = new QHBoxLayout;
		actionsLayout->setContentsMargins(0, 0, 0, 0);
		actionsLayout->setSpacing(2);

		auto addRowAction = [&](QToolButton *button, const char *tooltipKey, auto handler) {
			button->setToolTip(Tr(tooltipKey));
			button->setAutoRaise(true);
			connect(button, &QToolButton::clicked, this,
				[this, handler]() { QMetaObject::invokeMethod(this, handler, Qt::QueuedConnection); });
			actionsLayout->addWidget(button);
		};

		auto *up = new QToolButton;
		up->setArrowType(Qt::UpArrow);
		up->setEnabled(row > 0);
		addRowAction(up, "Candidate.MoveUp", [this, row]() { MoveCandidate(row, -1); });

		auto *down = new QToolButton;
		down->setArrowType(Qt::DownArrow);
		down->setEnabled(row < lastRow);
		addRowAction(down, "Candidate.MoveDown", [this, row]() { MoveCandidate(row, 1); });

		auto *remove = new QToolButton;
		remove->setText(Tr("Candidate.RemoveGlyph"));
		addRowAction(remove, "Candidate.Remove", [this, row]() { RemoveCandidate(row); });

		actions->setLayout(actionsLayout);
		m_candidates->setCellWidget(row, kCandActions, actions);
	}

	m_updating = false;

	RefreshCandidateShares(mark);
}

void TransitionTreeDialog::RefreshCandidateShares(const Mark &mark)
{
	/* A bare weight like 2.00 means nothing on its own; show what share of
	 * the picks it actually gets among the transitions that can fire. */
	double total = 0.0;
	const auto usable = mark.UsableCandidates();
	for (size_t index : usable)
		total += std::max(0.0, mark.candidates[index].weight);

	const int rows = std::min(m_candidates->rowCount(), (int)mark.candidates.size());
	for (int row = 0; row < rows; row++) {
		QTableWidgetItem *item = m_candidates->item(row, kCandShare);
		if (!item)
			continue;
		const bool isUsable = std::find(usable.begin(), usable.end(), (size_t)row) != usable.end();
		const double weight = std::max(0.0, mark.candidates[(size_t)row].weight);
		if (isUsable && total > 0.0)
			item->setText(QString("%1%").arg(qRound(weight / total * 100.0)));
		else
			item->setText(Tr("Candidate.NoShare"));
	}
}

void TransitionTreeDialog::RefreshPresetPage()
{
	m_updating = true;

	Preset *preset = CurrentPreset();
	if (!preset) {
		m_presetTitle->setText(Tr("Preset.NoneTitle"));
		m_presetSummary->setText(Tr("Preset.None"));
		m_setDefaultTransition->setEnabled(false);
		m_defaultTransition->setEnabled(false);
		m_defaultDuration->setEnabled(false);
		m_updating = false;
		return;
	}

	size_t enabledMarks = 0;
	size_t problemMarks = 0;
	for (const auto &mark : preset->marks) {
		if (mark.enabled)
			enabledMarks++;
		if (!MarkProblems(mark).isEmpty())
			problemMarks++;
	}

	const bool isActive = GetStore().activePreset == preset->name;
	m_presetTitle->setText(Utf8(preset->name));
	m_presetSummary->setText(Tr("Preset.Summary")
					 .arg(isActive ? Tr("Preset.Active") : Tr("Preset.Inactive"),
					      preset->global ? Tr("Scope.GlobalLong") : Tr("Scope.LocalLong"),
					      QString::number(preset->marks.size()), QString::number(enabledMarks),
					      QString::number(problemMarks)) +
				 (preset->marks.empty() ? QString() : "\n" + Tr("Preset.SelectHint")));

	m_setDefaultTransition->setEnabled(true);
	m_setDefaultTransition->setChecked(preset->setDefaultTransition);

	QStringList transitions = TransitionNames();
	SortNames(transitions);

	m_defaultTransition->clear();
	m_defaultTransition->addItems(transitions);
	m_defaultTransition->setCurrentText(Utf8(preset->defaultTransition));
	m_defaultTransition->setEnabled(preset->setDefaultTransition);

	m_defaultDuration->setValue(preset->defaultDuration);
	m_defaultDuration->setEnabled(preset->setDefaultTransition);

	m_updating = false;
}

/* ------------------------------------------------------------------------ */
/* Slots                                                                     */
/* ------------------------------------------------------------------------ */

void TransitionTreeDialog::OnPresetChanged(int)
{
	if (m_updating)
		return;

	RefreshPresetActions();
	ClearUndo();

	/* Selecting a preset in the editor also activates it: a preset that is
	 * being edited but not in effect would be a trap during a live show. */
	Preset *preset = CurrentPreset();
	if (preset && GetStore().activePreset != preset->name) {
		SetActivePreset(preset->name);
		return;
	}

	m_selectedMarkId.clear();
	RefreshTree();
	RefreshDetail();
}

void TransitionTreeDialog::OnTreeSelectionChanged()
{
	if (m_updating)
		return;

	const auto selected = m_tree->selectedItems();
	std::string markId;
	if (!selected.isEmpty())
		markId = selected.first()->data(kColScenes, kMarkIdRole).toString().toStdString();

	if (markId == m_selectedMarkId)
		return;

	m_selectedMarkId = markId;
	RefreshDetail();
}

void TransitionTreeDialog::OnSearchChanged(const QString &)
{
	if (m_updating)
		return;
	RefreshTree();
}

void TransitionTreeDialog::ShowTreeContextMenu(const QPoint &pos)
{
	if (QTreeWidgetItem *item = m_tree->itemAt(pos))
		m_tree->setCurrentItem(item);

	QMenu menu(this);
	menu.addAction(m_addMarkAction);
	if (SelectedMark()) {
		menu.addSeparator();
		menu.addAction(m_toggleMarkAction);
		menu.addAction(m_duplicateMarkAction);
		menu.addAction(m_testMarkAction);
		menu.addSeparator();
		menu.addAction(m_deleteMarkAction);
	}
	menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

/* ------------------------------------------------------------------------ */
/* Preset actions                                                            */
/* ------------------------------------------------------------------------ */

void TransitionTreeDialog::AddPreset()
{
	bool accepted = false;
	const QString name = QInputDialog::getText(this, Tr("Preset.New"), Tr("Preset.NamePrompt"), QLineEdit::Normal,
						   Tr("Preset.DefaultName"), &accepted);
	if (!accepted || name.trimmed().isEmpty())
		return;

	/* New presets start local; the preset menu moves one to global. */
	Store &store = GetStore();
	Preset preset;
	preset.name = store.UniqueName(name.trimmed().toStdString());
	preset.global = false;
	store.presets.push_back(preset);

	RefreshPresetHotkeys();
	SetActivePreset(preset.name);
}

void TransitionTreeDialog::DuplicatePreset()
{
	Preset *preset = CurrentPreset();
	if (!preset)
		return;

	Store &store = GetStore();
	Preset copy = *preset;
	copy.name = store.UniqueName(preset->name + " " + obs_module_text("Preset.CopySuffix"));
	copy.hotkey = OBS_INVALID_HOTKEY_ID;
	copy.hotkeyBindings.clear();
	copy.ResetRuntimeState();
	store.presets.push_back(copy);

	RefreshPresetHotkeys();
	if (copy.global)
		SaveGlobalPresets();

	SetActivePreset(copy.name);
}

void TransitionTreeDialog::RenamePreset()
{
	Preset *preset = CurrentPreset();
	if (!preset)
		return;

	bool accepted = false;
	const QString name = QInputDialog::getText(this, Tr("Preset.Rename"), Tr("Preset.NamePrompt"),
						   QLineEdit::Normal, Utf8(preset->name), &accepted);
	if (!accepted || name.trimmed().isEmpty())
		return;

	Store &store = GetStore();
	const std::string previous = preset->name;
	const std::string next = name.trimmed().toStdString();
	if (next == previous)
		return;

	preset->name = store.UniqueName(next);
	if (store.activePreset == previous)
		store.activePreset = preset->name;
	if (m_undoPreset == previous)
		m_undoPreset = preset->name;

	/* The combo is keyed by name, so point it at the new one before the
	 * rebuild looks for it. */
	m_updating = true;
	const int index = m_presetCombo->currentIndex();
	if (index >= 0)
		m_presetCombo->setItemData(index, Utf8(preset->name));
	m_updating = false;

	RefreshPresetHotkeys();
	SaveGlobalPresets();
	store.Touch();
	RefreshAll();
}

void TransitionTreeDialog::DeletePreset()
{
	Preset *preset = CurrentPreset();
	if (!preset)
		return;

	const QString name = Utf8(preset->name);
	if (QMessageBox::question(this, Tr("Preset.Delete"), Tr("Preset.DeleteConfirm").arg(name)) != QMessageBox::Yes)
		return;

	Store &store = GetStore();
	const bool wasGlobal = preset->global;
	const bool wasActive = store.activePreset == preset->name;

	for (auto it = store.presets.begin(); it != store.presets.end(); ++it) {
		if (&(*it) == preset) {
			store.presets.erase(it);
			break;
		}
	}

	RefreshPresetHotkeys();
	if (wasGlobal)
		SaveGlobalPresets();

	m_selectedMarkId.clear();
	ClearUndo();

	if (wasActive)
		SetActivePreset(store.presets.empty() ? std::string() : store.presets.front().name);
	else
		RefreshAll();
}

void TransitionTreeDialog::ChangePresetScope(bool global)
{
	if (m_updating)
		return;

	Preset *preset = CurrentPreset();
	if (!preset || preset->global == global)
		return;

	preset->global = global;

	/* Whichever direction it moved, both stores need rewriting: the global
	 * file drops or gains it, and the collection blob follows on next save. */
	SaveGlobalPresets();
	GetStore().Touch();
	RefreshPresetCombo();
	RefreshDetail();
}

/* ------------------------------------------------------------------------ */
/* Mark actions                                                              */
/* ------------------------------------------------------------------------ */

void TransitionTreeDialog::AddMark()
{
	Preset *preset = CurrentPreset();
	if (!preset) {
		QMessageBox::information(this, Tr("Mark.Add"), Tr("Mark.NeedPreset"));
		return;
	}

	Mark mark;
	mark.id = "mark-" + std::to_string(os_gettime_ns());
	mark.canvas = MainCanvasName();

	/* Seed from the current program scene so a new mark is one click from
	 * being useful, and default the destination to "any scene". */
	obs_source_t *current = obs_frontend_get_current_scene();
	if (current) {
		const char *name = obs_source_get_name(current);
		if (name) {
			mark.from.type = MatchType::Scenes;
			mark.from.scenes.push_back(name);
		}
		obs_source_release(current);
	}
	if (mark.from.scenes.empty())
		mark.from.type = MatchType::Any;
	mark.to.type = MatchType::Any;

	const QStringList transitions = TransitionNames();
	if (!transitions.isEmpty()) {
		Candidate candidate;
		candidate.transition = transitions.first().toStdString();
		candidate.duration = obs_frontend_get_transition_duration();
		if (candidate.duration <= 0)
			candidate.duration = 300;
		mark.candidates.push_back(candidate);
	}

	/* A new mark that the filter hides would look like nothing happened. */
	if (!m_search->text().isEmpty()) {
		m_updating = true;
		m_search->clear();
		m_updating = false;
	}

	preset->marks.push_back(mark);
	m_selectedMarkId = mark.id;

	CommitChange(true);
	RefreshDetail();
}

void TransitionTreeDialog::DuplicateMark()
{
	Preset *preset = CurrentPreset();
	Mark *mark = SelectedMark();
	if (!preset || !mark)
		return;

	Mark copy = *mark;
	copy.id = "mark-" + std::to_string(os_gettime_ns());
	copy.ResetRuntimeState();
	preset->marks.push_back(copy);
	m_selectedMarkId = copy.id;

	CommitChange(true);
	RefreshDetail();
}

void TransitionTreeDialog::DeleteMark()
{
	Preset *preset = CurrentPreset();
	Mark *mark = SelectedMark();
	if (!preset || !mark)
		return;

	for (auto it = preset->marks.begin(); it != preset->marks.end(); ++it) {
		if (it->id == mark->id) {
			/* Deleting is immediate; the undo bar is the safety net. */
			m_undoMark = std::make_unique<Mark>(*it);
			m_undoPreset = preset->name;
			m_undoIndex = (size_t)(it - preset->marks.begin());
			preset->marks.erase(it);
			break;
		}
	}

	if (m_undoMark) {
		m_undoLabel->setText(Tr("Mark.Deleted").arg(MarkTitle(*m_undoMark)));
		m_undoBar->setVisible(true);
		m_undoTimer->start();
	}

	m_selectedMarkId.clear();
	CommitChange(true);
	RefreshDetail();
}

void TransitionTreeDialog::UndoDeleteMark()
{
	Preset *preset = CurrentPreset();
	if (!m_undoMark || !preset || preset->name != m_undoPreset) {
		ClearUndo();
		return;
	}

	const size_t index = std::min(m_undoIndex, preset->marks.size());
	m_selectedMarkId = m_undoMark->id;
	preset->marks.insert(preset->marks.begin() + (std::ptrdiff_t)index, *m_undoMark);
	ClearUndo();

	CommitChange(true);
	RefreshDetail();
}

void TransitionTreeDialog::ClearUndo()
{
	m_undoMark.reset();
	m_undoPreset.clear();
	m_undoIndex = 0;
	if (m_undoTimer)
		m_undoTimer->stop();
	if (m_undoBar)
		m_undoBar->setVisible(false);
}

void TransitionTreeDialog::ToggleMarkEnabled()
{
	Mark *mark = SelectedMark();
	if (!mark)
		return;

	mark->enabled = !mark->enabled;
	CommitChange(true);
	RefreshDetail();
}

void TransitionTreeDialog::TestMark()
{
	Mark *mark = SelectedMark();
	if (!mark)
		return;

	const int index = mark->PickCandidate();
	if (index < 0) {
		QMessageBox::information(this, Tr("Mark.Test"), Tr("Warning.NoUsableCandidates"));
		return;
	}
	const Candidate &candidate = mark->candidates[(size_t)index];

	/* Find a destination the mark's to-side actually matches. */
	const QString canvasName = mark->canvas.empty() ? Utf8(MainCanvasName()) : Utf8(mark->canvas);
	QString destination;
	for (const QString &scene : SceneNamesForCanvas(canvasName)) {
		if (mark->to.Matches(scene.toStdString())) {
			destination = scene;
			break;
		}
	}

	if (destination.isEmpty()) {
		QMessageBox::information(this, Tr("Mark.Test"), Tr("Test.NoDestination"));
		return;
	}

	if (QMessageBox::question(this, Tr("Mark.Test"),
				  Tr("Test.Confirm").arg(Utf8(candidate.transition), destination)) != QMessageBox::Yes)
		return;

	obs_source_t *scene = obs_get_source_by_name(destination.toUtf8().constData());
	if (!scene)
		return;

	obs_data_t *settings = obs_source_get_private_settings(scene);
	obs_data_set_string(settings, "transition", candidate.transition.c_str());
	obs_data_set_int(settings, "transition_duration", candidate.duration);
	obs_data_release(settings);

	obs_frontend_set_current_scene(scene);
	obs_source_release(scene);
}

/* ------------------------------------------------------------------------ */
/* Candidate actions                                                         */
/* ------------------------------------------------------------------------ */

void TransitionTreeDialog::AddCandidate()
{
	Mark *mark = SelectedMark();
	if (!mark)
		return;

	Candidate candidate;
	const QStringList transitions = TransitionNames();
	if (!transitions.isEmpty())
		candidate.transition = transitions.first().toStdString();
	candidate.duration = obs_frontend_get_transition_duration();
	if (candidate.duration <= 0)
		candidate.duration = 300;

	mark->candidates.push_back(candidate);
	mark->ResetRuntimeState();

	RefreshMarkPage(*mark);
	CommitChange(true);
}

void TransitionTreeDialog::RemoveCandidate(int row)
{
	Mark *mark = SelectedMark();
	if (!mark || row < 0 || row >= (int)mark->candidates.size())
		return;

	mark->candidates.erase(mark->candidates.begin() + row);
	mark->ResetRuntimeState();

	RefreshMarkPage(*mark);
	CommitChange(true);
}

void TransitionTreeDialog::MoveCandidate(int row, int delta)
{
	Mark *mark = SelectedMark();
	if (!mark)
		return;

	const int target = row + delta;
	if (row < 0 || row >= (int)mark->candidates.size() || target < 0 || target >= (int)mark->candidates.size())
		return;

	std::swap(mark->candidates[(size_t)row], mark->candidates[(size_t)target]);
	mark->ResetRuntimeState();

	RefreshMarkPage(*mark);
	CommitChange(true);
}

/* ------------------------------------------------------------------------ */
/* Import / export / transitions                                             */
/* ------------------------------------------------------------------------ */

void TransitionTreeDialog::ImportPresets()
{
	const QString fileName = QFileDialog::getOpenFileName(this, Tr("Import.Title"), QString(), Tr("Import.Filter"));
	if (fileName.isEmpty())
		return;

	obs_data_t *data = obs_data_create_from_json_file(fileName.toUtf8().constData());
	if (!data) {
		QMessageBox::warning(this, Tr("Import.Title"), Tr("Import.Unreadable"));
		return;
	}

	std::vector<Preset> imported;
	std::string error;
	const bool ok = tt::ImportPresets(data, imported, error);
	obs_data_release(data);

	if (!ok) {
		QMessageBox::warning(this, Tr("Import.Title"), Utf8(error));
		return;
	}

	/* Imports land in this scene collection; the preset menu can move any
	 * of them to global afterwards. */
	Store &store = GetStore();
	std::string firstName;

	for (auto &preset : imported) {
		preset.global = false;
		preset.name = store.UniqueName(preset.name);
		if (firstName.empty())
			firstName = preset.name;
		store.presets.push_back(preset);
	}

	RefreshPresetHotkeys();

	QMessageBox::information(this, Tr("Import.Title"), Tr("Import.Done").arg(imported.size()));

	if (!firstName.empty())
		SetActivePreset(firstName);
	else
		RefreshAll();
}

void TransitionTreeDialog::ExportPresets()
{
	Preset *preset = CurrentPreset();
	if (!preset) {
		QMessageBox::information(this, Tr("Export.Title"), Tr("Export.NothingToExport"));
		return;
	}

	QMessageBox box(this);
	box.setWindowTitle(Tr("Export.Title"));
	box.setText(Tr("Export.Scope"));
	QAbstractButton *currentButton = box.addButton(Tr("Export.Current"), QMessageBox::AcceptRole);
	QAbstractButton *allButton = box.addButton(Tr("Export.All"), QMessageBox::AcceptRole);
	box.addButton(QMessageBox::Cancel);
	box.exec();

	if (box.clickedButton() != currentButton && box.clickedButton() != allButton)
		return;

	std::vector<const Preset *> selection;
	if (box.clickedButton() == allButton) {
		for (const auto &candidate : GetStore().presets)
			selection.push_back(&candidate);
	} else {
		selection.push_back(preset);
	}

	const QString fileName = QFileDialog::getSaveFileName(this, Tr("Export.Title"), QString(), Tr("Import.Filter"));
	if (fileName.isEmpty())
		return;

	obs_data_t *data = tt::ExportPresets(selection);
	if (!obs_data_save_json_pretty_safe(data, fileName.toUtf8().constData(), "tmp", "bak"))
		QMessageBox::warning(this, Tr("Export.Title"), Tr("Export.Failed"));
	obs_data_release(data);
}

void TransitionTreeDialog::CreateTransition()
{
	/* OBS exposes no API for adding a transition to its own list, so drive
	 * the frontend's own "add transition" button. Going through OBS means the
	 * new transition is initialised, named and persisted exactly as if the
	 * user had added it from the transitions dock. */
	auto *mainWindow = (QMainWindow *)obs_frontend_get_main_window();
	QAbstractButton *addButton = mainWindow ? mainWindow->findChild<QAbstractButton *>("transitionAdd") : nullptr;

	if (!addButton) {
		QMessageBox::information(this, Tr("NewTransition"), Tr("NewTransition.Unavailable"));
		return;
	}

	addButton->click();
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

void TransitionTreeDialog::ReadMatcherFromWidgets(SceneMatcher &matcher, QButtonGroup *typeGroup,
						  QListWidget *sceneList)
{
	const int id = typeGroup->checkedId();
	matcher.type = (id >= 0 && id <= 2) ? (MatchType)id : MatchType::Scenes;

	matcher.scenes.clear();
	for (int row = 0; row < sceneList->count(); row++) {
		QListWidgetItem *item = sceneList->item(row);
		if (item->checkState() == Qt::Checked)
			matcher.scenes.push_back(item->text().toStdString());
	}
}

QStringList TransitionTreeDialog::SceneNamesForCanvas(const QString &canvas) const
{
	QStringList names;

	obs_canvas_t *target = obs_get_canvas_by_name(canvas.toUtf8().constData());
	if (!target)
		target = obs_get_main_canvas();
	if (!target)
		return names;

	obs_canvas_enum_scenes(
		target,
		[](void *param, obs_source_t *scene) {
			auto *out = (QStringList *)param;
			const char *name = obs_source_get_name(scene);
			if (name)
				*out << QString::fromUtf8(name);
			return true;
		},
		&names);

	obs_canvas_release(target);
	return names;
}

QStringList TransitionTreeDialog::TransitionNames() const
{
	QStringList names;

	struct obs_frontend_source_list transitions = {};
	obs_frontend_get_transitions(&transitions);
	for (size_t i = 0; i < transitions.sources.num; i++) {
		const char *name = obs_source_get_name(transitions.sources.array[i]);
		if (name)
			names << QString::fromUtf8(name);
	}
	obs_frontend_source_list_free(&transitions);

	return names;
}

QStringList TransitionTreeDialog::CanvasNames() const
{
	QStringList names;
	obs_enum_canvases(
		[](void *param, obs_canvas_t *canvas) {
			auto *out = (QStringList *)param;
			const char *name = obs_canvas_get_name(canvas);
			if (name)
				*out << QString::fromUtf8(name);
			return true;
		},
		&names);
	return names;
}

void TransitionTreeDialog::WarnAboutTransitionTable()
{
	if (g_warnedAboutTransitionTable || !TransitionTableDetected())
		return;

	g_warnedAboutTransitionTable = true;
	QMessageBox::warning(this, Tr("TransitionTree"), Tr("Warning.TransitionTableLoaded"));
}
