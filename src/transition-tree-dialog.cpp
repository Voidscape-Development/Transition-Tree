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
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMainWindow>
#include <QMessageBox>
#include <QMetaObject>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStringList>
#include <QTableWidget>
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

QString MarkSummary(const Mark &mark)
{
	const auto usable = mark.UsableCandidates();
	if (mark.candidates.empty())
		return Tr("Mark.NoTransitions");
	if (usable.size() == 1)
		return QString::fromUtf8(mark.candidates[usable.front()].transition.c_str());
	return Tr("Mark.TransitionCount").arg(usable.size()).arg(mark.candidates.size());
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
	setMinimumSize(900, 520);
	if (g_savedWidth > 0 && g_savedHeight > 0)
		resize(g_savedWidth, g_savedHeight);

	auto *splitter = new QSplitter(Qt::Horizontal);
	splitter->addWidget(BuildTreePane());
	splitter->addWidget(BuildDetailPane());
	splitter->setStretchFactor(0, 3);
	splitter->setStretchFactor(1, 2);

	auto *layout = new QVBoxLayout;
	layout->addWidget(BuildPresetBar());
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

QWidget *TransitionTreeDialog::BuildPresetBar()
{
	auto *bar = new QWidget;
	auto *layout = new QHBoxLayout;
	layout->setContentsMargins(0, 0, 0, 0);

	layout->addWidget(new QLabel(Tr("Preset")));

	m_presetCombo = new QComboBox;
	m_presetCombo->setMinimumWidth(200);
	m_presetCombo->setToolTip(Tr("Preset.Tooltip"));
	connect(m_presetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		&TransitionTreeDialog::OnPresetChanged);
	layout->addWidget(m_presetCombo);

	m_scopeCombo = new QComboBox;
	m_scopeCombo->addItem(Tr("Scope.Local"));
	m_scopeCombo->addItem(Tr("Scope.Global"));
	m_scopeCombo->setToolTip(Tr("Scope.Tooltip"));
	connect(m_scopeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		[this](int index) { ChangePresetScope(index); });
	layout->addWidget(m_scopeCombo);

	auto addButton = [&](const char *key, void (TransitionTreeDialog::*slot)()) {
		auto *button = new QPushButton(Tr(key));
		connect(button, &QPushButton::clicked, this, slot);
		layout->addWidget(button);
		return button;
	};
	addButton("Preset.New", &TransitionTreeDialog::AddPreset);
	addButton("Preset.Duplicate", &TransitionTreeDialog::DuplicatePreset);
	addButton("Preset.Rename", &TransitionTreeDialog::RenamePreset);
	addButton("Preset.Delete", &TransitionTreeDialog::DeletePreset);

	layout->addStretch(1);

	m_enabledCheck = new QCheckBox(Tr("TreeEnabled"));
	m_enabledCheck->setToolTip(Tr("TreeEnabled.Tooltip"));
	connect(m_enabledCheck, &QCheckBox::toggled, this, [this](bool checked) {
		if (m_updating)
			return;
		SetTreeEnabled(checked);
	});
	layout->addWidget(m_enabledCheck);

	bar->setLayout(layout);
	return bar;
}

QWidget *TransitionTreeDialog::BuildTreePane()
{
	auto *pane = new QWidget;
	auto *layout = new QVBoxLayout;
	layout->setContentsMargins(0, 0, 0, 0);

	/* The search box only ever filters the view. Adding a mark is a separate
	 * button, so narrowing the list can never hide the controls needed to
	 * create one. */
	auto *searchRow = new QHBoxLayout;
	searchRow->addWidget(new QLabel(Tr("Search")));
	m_search = new QLineEdit;
	m_search->setClearButtonEnabled(true);
	m_search->setPlaceholderText(Tr("Search.Placeholder"));
	connect(m_search, &QLineEdit::textChanged, this, &TransitionTreeDialog::OnSearchChanged);
	searchRow->addWidget(m_search, 1);
	layout->addLayout(searchRow);

	m_tree = new QTreeWidget;
	m_tree->setColumnCount(5);
	m_tree->setHeaderLabels({Tr("Column.Scenes"), Tr("Column.Transitions"), Tr("Column.Mode"), Tr("Column.Canvas"),
				 Tr("Column.Enabled")});
	m_tree->setRootIsDecorated(true);
	m_tree->setAlternatingRowColors(true);
	m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_tree->header()->setStretchLastSection(false);
	m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
	m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	m_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	m_tree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
	m_tree->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
	connect(m_tree, &QTreeWidget::itemSelectionChanged, this, &TransitionTreeDialog::OnTreeSelectionChanged);
	connect(m_tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item, int column) {
		if (m_updating || column != 4)
			return;
		const std::string markId = item->data(0, kMarkIdRole).toString().toStdString();
		Preset *preset = CurrentPreset();
		if (!preset || markId.empty())
			return;
		Mark *mark = preset->FindMark(markId);
		if (!mark)
			return;
		mark->enabled = item->checkState(4) == Qt::Checked;
		CommitChange(true);
	});
	layout->addWidget(m_tree, 1);

	auto *buttons = new QHBoxLayout;
	auto *addMark = new QPushButton(Tr("Mark.Add"));
	addMark->setToolTip(Tr("Mark.Add.Tooltip"));
	connect(addMark, &QPushButton::clicked, this, &TransitionTreeDialog::AddMark);
	buttons->addWidget(addMark);

	m_duplicateMarkButton = new QPushButton(Tr("Mark.Duplicate"));
	connect(m_duplicateMarkButton, &QPushButton::clicked, this, &TransitionTreeDialog::DuplicateMark);
	buttons->addWidget(m_duplicateMarkButton);

	m_deleteMarkButton = new QPushButton(Tr("Mark.Delete"));
	connect(m_deleteMarkButton, &QPushButton::clicked, this, &TransitionTreeDialog::DeleteMark);
	buttons->addWidget(m_deleteMarkButton);

	m_toggleMarkButton = new QPushButton(Tr("Mark.Toggle"));
	connect(m_toggleMarkButton, &QPushButton::clicked, this, &TransitionTreeDialog::ToggleMarkEnabled);
	buttons->addWidget(m_toggleMarkButton);

	m_testMarkButton = new QPushButton(Tr("Mark.Test"));
	m_testMarkButton->setToolTip(Tr("Mark.Test.Tooltip"));
	connect(m_testMarkButton, &QPushButton::clicked, this, &TransitionTreeDialog::TestMark);
	buttons->addWidget(m_testMarkButton);

	buttons->addStretch(1);
	layout->addLayout(buttons);

	pane->setLayout(layout);
	return pane;
}

QWidget *TransitionTreeDialog::BuildDetailPane()
{
	m_detail = new QStackedWidget;
	m_detail->addWidget(BuildPresetPage());
	m_detail->addWidget(BuildMarkPage());
	return m_detail;
}

QWidget *TransitionTreeDialog::BuildPresetPage()
{
	auto *page = new QWidget;
	auto *layout = new QVBoxLayout;

	m_presetSummary = new QLabel;
	m_presetSummary->setWordWrap(true);
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
	form->addRow(Tr("Column.Transitions"), m_defaultTransition);

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

	auto *hint = new QLabel(Tr("Preset.Hint"));
	hint->setWordWrap(true);
	layout->addWidget(hint);

	layout->addStretch(1);
	page->setLayout(layout);
	return page;
}

QWidget *TransitionTreeDialog::BuildMarkPage()
{
	auto *page = new QWidget;
	auto *layout = new QVBoxLayout;

	auto buildMatcherGroup = [this](const char *titleKey, QComboBox *&typeCombo, QListWidget *&sceneList) {
		auto *group = new QGroupBox(Tr(titleKey));
		auto *groupLayout = new QVBoxLayout;

		typeCombo = new QComboBox;
		typeCombo->addItem(Tr("Match.Scenes"));
		typeCombo->addItem(Tr("Match.Any"));
		typeCombo->addItem(Tr("Match.AnyExcept"));
		groupLayout->addWidget(typeCombo);

		sceneList = new QListWidget;
		sceneList->setSelectionMode(QAbstractItemView::NoSelection);
		sceneList->setMaximumHeight(130);
		groupLayout->addWidget(sceneList);

		group->setLayout(groupLayout);
		return group;
	};

	layout->addWidget(buildMatcherGroup("Mark.From", m_fromType, m_fromScenes));
	layout->addWidget(buildMatcherGroup("Mark.To", m_toType, m_toScenes));

	auto matcherChanged = [this]() {
		if (m_updating)
			return;
		Mark *mark = SelectedMark();
		if (!mark)
			return;
		ReadMatcherFromWidgets(mark->from, m_fromType, m_fromScenes);
		ReadMatcherFromWidgets(mark->to, m_toType, m_toScenes);
		m_fromScenes->setEnabled(m_fromType->currentIndex() != (int)MatchType::Any);
		m_toScenes->setEnabled(m_toType->currentIndex() != (int)MatchType::Any);
		/* Refreshed in place rather than by rebuilding the page, so the
		 * list the user is clicking through is not destroyed under them. */
		RefreshMarkWarning(*mark);
		CommitChange(true);
	};
	connect(m_fromType, QOverload<int>::of(&QComboBox::currentIndexChanged), this, matcherChanged);
	connect(m_toType, QOverload<int>::of(&QComboBox::currentIndexChanged), this, matcherChanged);
	connect(m_fromScenes, &QListWidget::itemChanged, this, matcherChanged);
	connect(m_toScenes, &QListWidget::itemChanged, this, matcherChanged);

	auto *form = new QFormLayout;

	m_canvasCombo = new QComboBox;
	connect(m_canvasCombo, &QComboBox::currentTextChanged, this, [this](const QString &text) {
		if (m_updating)
			return;
		Mark *mark = SelectedMark();
		if (!mark)
			return;
		mark->canvas = text.toStdString();
		CommitChange(true);
	});
	form->addRow(Tr("Column.Canvas"), m_canvasCombo);

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
		m_candidates->setColumnHidden(2, mark->mode != SelectionMode::Weighted);
		CommitChange(true);
	});
	form->addRow(Tr("Column.Mode"), m_modeCombo);

	m_markEnabled = new QCheckBox(Tr("Mark.Enabled"));
	connect(m_markEnabled, &QCheckBox::toggled, this, [this](bool checked) {
		if (m_updating)
			return;
		Mark *mark = SelectedMark();
		if (!mark)
			return;
		mark->enabled = checked;
		CommitChange(true);
	});
	form->addRow(m_markEnabled);

	layout->addLayout(form);

	m_markWarning = new QLabel;
	m_markWarning->setWordWrap(true);
	m_markWarning->setStyleSheet(QString("color: %1;").arg(InvalidColor().name()));
	layout->addWidget(m_markWarning);

	auto *candidateGroup = new QGroupBox(Tr("Candidates"));
	auto *candidateLayout = new QVBoxLayout;

	m_candidates = new QTableWidget(0, 4);
	m_candidates->setHorizontalHeaderLabels(
		{Tr("Column.Transition"), Tr("Column.Duration"), Tr("Column.Weight"), Tr("Column.Enabled")});
	m_candidates->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
	m_candidates->verticalHeader()->setVisible(false);
	m_candidates->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_candidates->setSelectionMode(QAbstractItemView::SingleSelection);
	candidateLayout->addWidget(m_candidates);

	auto *candidateButtons = new QHBoxLayout;
	auto *addCandidate = new QPushButton(Tr("Candidate.Add"));
	connect(addCandidate, &QPushButton::clicked, this, &TransitionTreeDialog::AddCandidate);
	candidateButtons->addWidget(addCandidate);

	m_removeCandidateButton = new QPushButton(Tr("Candidate.Remove"));
	connect(m_removeCandidateButton, &QPushButton::clicked, this, &TransitionTreeDialog::RemoveCandidate);
	candidateButtons->addWidget(m_removeCandidateButton);

	m_moveUpButton = new QPushButton(Tr("Candidate.MoveUp"));
	connect(m_moveUpButton, &QPushButton::clicked, this, [this]() { MoveCandidate(-1); });
	candidateButtons->addWidget(m_moveUpButton);

	m_moveDownButton = new QPushButton(Tr("Candidate.MoveDown"));
	connect(m_moveDownButton, &QPushButton::clicked, this, [this]() { MoveCandidate(1); });
	candidateButtons->addWidget(m_moveDownButton);

	candidateButtons->addStretch(1);
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
	credit->setOpenExternalLinks(true);
	layout->addWidget(credit, 1, Qt::AlignLeft);

	auto *newTransition = new QPushButton(Tr("NewTransition"));
	newTransition->setToolTip(Tr("NewTransition.Tooltip"));
	connect(newTransition, &QPushButton::clicked, this, &TransitionTreeDialog::CreateTransition);
	layout->addWidget(newTransition);

	auto *importButton = new QPushButton(Tr("Import"));
	connect(importButton, &QPushButton::clicked, this, &TransitionTreeDialog::ImportPresets);
	layout->addWidget(importButton);

	auto *exportButton = new QPushButton(Tr("Export"));
	connect(exportButton, &QPushButton::clicked, this, &TransitionTreeDialog::ExportPresets);
	layout->addWidget(exportButton);

	auto *closeButton = new QPushButton(Tr("Close"));
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
	RefreshTree();
	RefreshDetail();

	m_updating = true;
	m_enabledCheck->setChecked(IsTreeEnabled());
	m_updating = false;
}

void TransitionTreeDialog::RefreshPresetCombo()
{
	m_updating = true;

	const QString previous = m_presetCombo->currentData().toString();
	m_presetCombo->clear();

	Store &store = GetStore();
	for (const auto &preset : store.presets) {
		const QString name = QString::fromUtf8(preset.name.c_str());
		const QString label = preset.global ? Tr("Preset.GlobalLabel").arg(name) : name;
		m_presetCombo->addItem(label, name);
	}

	/* Follow the active preset unless the user was looking at another one
	 * that still exists. */
	QString target = QString::fromUtf8(store.activePreset.c_str());
	if (!previous.isEmpty() && store.Find(previous.toStdString()))
		target = previous;

	const int index = m_presetCombo->findData(target);
	m_presetCombo->setCurrentIndex(index >= 0 ? index : (m_presetCombo->count() > 0 ? 0 : -1));

	Preset *preset = CurrentPreset();
	m_scopeCombo->setEnabled(preset != nullptr);
	m_scopeCombo->setCurrentIndex(preset && preset->global ? 1 : 0);

	m_updating = false;
}

void TransitionTreeDialog::RefreshTree()
{
	m_updating = true;

	/* Remember which groups were open so editing does not collapse the view. */
	QStringList expanded;
	for (int i = 0; i < m_tree->topLevelItemCount(); i++) {
		QTreeWidgetItem *item = m_tree->topLevelItem(i);
		if (item->isExpanded())
			expanded << item->text(0);
	}

	m_tree->clear();

	Preset *preset = CurrentPreset();
	if (preset) {
		const QString filter = m_search->text().trimmed();
		const QString mainCanvas = QString::fromUtf8(MainCanvasName().c_str());

		QHash<QString, QTreeWidgetItem *> groups;

		for (auto &mark : preset->marks) {
			const QString fromText = QString::fromUtf8(mark.from.Describe().c_str());
			const QString toText = QString::fromUtf8(mark.to.Describe().c_str());
			const QString canvas = mark.canvas.empty() ? mainCanvas
								   : QString::fromUtf8(mark.canvas.c_str());
			const QString summary = MarkSummary(mark);

			if (!filter.isEmpty()) {
				QString haystack = fromText + " " + toText + " " + summary + " " + canvas + " " +
						   ModeName(mark.mode);
				for (const auto &candidate : mark.candidates)
					haystack += " " + QString::fromUtf8(candidate.transition.c_str());
				if (!haystack.contains(filter, Qt::CaseInsensitive))
					continue;
			}

			const QString groupKey = fromText;
			QTreeWidgetItem *group = groups.value(groupKey, nullptr);
			if (!group) {
				group = new QTreeWidgetItem(m_tree);
				group->setText(0, fromText);
				group->setFirstColumnSpanned(false);
				QFont font = group->font(0);
				font.setBold(true);
				group->setFont(0, font);
				if (!mark.from.MissingScenes().empty())
					group->setForeground(0, InvalidColor());
				groups.insert(groupKey, group);
			}

			auto *item = new QTreeWidgetItem(group);
			item->setText(0, Tr("Mark.ToPrefix").arg(toText));
			item->setText(1, summary);
			item->setText(2, ModeName(mark.mode));
			item->setText(3, canvas);
			item->setData(0, kMarkIdRole, QString::fromUtf8(mark.id.c_str()));
			item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
			item->setCheckState(4, mark.enabled ? Qt::Checked : Qt::Unchecked);

			auto missing = mark.from.MissingScenes();
			for (const auto &scene : mark.to.MissingScenes())
				missing.push_back(scene);
			if (!missing.empty()) {
				QStringList names;
				for (const auto &scene : missing)
					names << QString::fromUtf8(scene.c_str());
				item->setForeground(0, InvalidColor());
				item->setToolTip(0, Tr("Mark.MissingScenes").arg(names.join(", ")));
			}
			if (!mark.enabled) {
				QFont font = item->font(0);
				font.setItalic(true);
				item->setFont(0, font);
			}

			for (size_t i = 0; i < mark.candidates.size(); i++) {
				const Candidate &candidate = mark.candidates[i];
				auto *child = new QTreeWidgetItem(item);
				child->setText(0, QString::fromUtf8(candidate.transition.c_str()));
				child->setText(1, QString("%1 ms").arg(candidate.duration));
				if (mark.mode == SelectionMode::Weighted)
					child->setText(2, Tr("Weight.Label").arg(candidate.weight, 0, 'g', 3));
				child->setData(0, kMarkIdRole, QString::fromUtf8(mark.id.c_str()));
				if (!candidate.enabled) {
					QFont font = child->font(0);
					font.setStrikeOut(true);
					child->setFont(0, font);
				}
			}
		}

		for (int i = 0; i < m_tree->topLevelItemCount(); i++) {
			QTreeWidgetItem *item = m_tree->topLevelItem(i);
			/* Expand by default; a filtered view is most useful open. */
			item->setExpanded(expanded.isEmpty() || expanded.contains(item->text(0)) || !filter.isEmpty());
		}
	}

	/* Restore the previous selection where the mark still exists. */
	if (!m_selectedMarkId.empty()) {
		const QString wanted = QString::fromUtf8(m_selectedMarkId.c_str());
		QTreeWidgetItemIterator iterator(m_tree);
		while (*iterator) {
			if ((*iterator)->data(0, kMarkIdRole).toString() == wanted &&
			    (*iterator)->parent() != nullptr && (*iterator)->parent()->parent() == nullptr) {
				(*iterator)->setSelected(true);
				break;
			}
			++iterator;
		}
	}

	m_updating = false;

	const bool hasMark = SelectedMark() != nullptr;
	m_duplicateMarkButton->setEnabled(hasMark);
	m_deleteMarkButton->setEnabled(hasMark);
	m_toggleMarkButton->setEnabled(hasMark);
	m_testMarkButton->setEnabled(hasMark);
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
}

void TransitionTreeDialog::RefreshMatcherWidgets(const SceneMatcher &matcher, QComboBox *typeCombo,
						 QListWidget *sceneList, const QString &canvas)
{
	typeCombo->setCurrentIndex((int)matcher.type);
	sceneList->setEnabled(matcher.type != MatchType::Any);

	sceneList->clear();

	QStringList scenes = SceneNamesForCanvas(canvas);

	/* Keep scene names the matcher references even when they are missing from
	 * this collection, so switching collections never silently drops them. */
	for (const auto &name : matcher.scenes) {
		const QString scene = QString::fromUtf8(name.c_str());
		if (!scenes.contains(scene))
			scenes << scene;
	}

	const auto missing = matcher.MissingScenes();
	for (const QString &scene : scenes) {
		auto *item = new QListWidgetItem(scene, sceneList);
		item->setFlags(item->flags() | Qt::ItemIsUserCheckable);

		const bool checked = std::find(matcher.scenes.begin(), matcher.scenes.end(), scene.toStdString()) !=
				     matcher.scenes.end();
		item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);

		if (std::find(missing.begin(), missing.end(), scene.toStdString()) != missing.end()) {
			item->setForeground(InvalidColor());
			item->setToolTip(Tr("Scene.Missing"));
		}
	}
}

void TransitionTreeDialog::RefreshMarkPage(Mark &mark)
{
	m_updating = true;

	const QString canvas = mark.canvas.empty() ? QString::fromUtf8(MainCanvasName().c_str())
						   : QString::fromUtf8(mark.canvas.c_str());

	m_canvasCombo->clear();
	m_canvasCombo->addItems(CanvasNames());
	const int canvasIndex = m_canvasCombo->findText(canvas);
	if (canvasIndex >= 0)
		m_canvasCombo->setCurrentIndex(canvasIndex);

	RefreshMatcherWidgets(mark.from, m_fromType, m_fromScenes, canvas);
	RefreshMatcherWidgets(mark.to, m_toType, m_toScenes, canvas);

	m_modeCombo->setCurrentIndex((int)mark.mode);
	m_markEnabled->setChecked(mark.enabled);

	RefreshMarkWarning(mark);

	m_candidates->setColumnHidden(2, mark.mode != SelectionMode::Weighted);

	m_updating = false;

	RefreshCandidateTable(mark);
}

void TransitionTreeDialog::RefreshMarkWarning(const Mark &mark)
{
	QStringList warnings;
	if (mark.from.IsEmpty() || mark.to.IsEmpty())
		warnings << Tr("Warning.NoScenesSelected");
	if (mark.UsableCandidates().empty())
		warnings << Tr("Warning.NoUsableCandidates");

	m_markWarning->setText(warnings.join(" "));
	m_markWarning->setVisible(!warnings.isEmpty());
}

void TransitionTreeDialog::RefreshCandidateTable(Mark &mark)
{
	m_updating = true;

	const int previousRow = m_candidates->currentRow();
	const QStringList transitions = TransitionNames();

	m_candidates->setRowCount((int)mark.candidates.size());

	for (int row = 0; row < (int)mark.candidates.size(); row++) {
		const Candidate &candidate = mark.candidates[(size_t)row];

		auto *transitionCombo = new QComboBox;
		transitionCombo->addItems(transitions);
		const QString current = QString::fromUtf8(candidate.transition.c_str());
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
			CommitChange(true);
		});
		m_candidates->setCellWidget(row, 0, transitionCombo);

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
			CommitChange(true);
		});
		m_candidates->setCellWidget(row, 1, duration);

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
			CommitChange(true);
		});
		m_candidates->setCellWidget(row, 2, weight);

		auto *enabled = new QCheckBox;
		enabled->setChecked(candidate.enabled);
		connect(enabled, &QCheckBox::toggled, this, [this, row](bool checked) {
			if (m_updating)
				return;
			Mark *target = SelectedMark();
			if (!target || row >= (int)target->candidates.size())
				return;
			target->candidates[(size_t)row].enabled = checked;
			CommitChange(true);
		});
		m_candidates->setCellWidget(row, 3, enabled);
	}

	if (previousRow >= 0 && previousRow < m_candidates->rowCount())
		m_candidates->setCurrentCell(previousRow, 0);

	m_updating = false;
}

void TransitionTreeDialog::RefreshPresetPage()
{
	m_updating = true;

	Preset *preset = CurrentPreset();
	if (!preset) {
		m_presetSummary->setText(Tr("Preset.None"));
		m_setDefaultTransition->setEnabled(false);
		m_defaultTransition->setEnabled(false);
		m_defaultDuration->setEnabled(false);
		m_updating = false;
		return;
	}

	size_t enabledMarks = 0;
	for (const auto &mark : preset->marks) {
		if (mark.enabled)
			enabledMarks++;
	}

	const bool isActive = GetStore().activePreset == preset->name;
	m_presetSummary->setText(Tr("Preset.Summary")
					 .arg(QString::fromUtf8(preset->name.c_str()))
					 .arg(preset->global ? Tr("Scope.Global") : Tr("Scope.Local"))
					 .arg(preset->marks.size())
					 .arg(enabledMarks)
					 .arg(isActive ? Tr("Preset.Active") : Tr("Preset.Inactive")));

	m_setDefaultTransition->setEnabled(true);
	m_setDefaultTransition->setChecked(preset->setDefaultTransition);

	m_defaultTransition->clear();
	m_defaultTransition->addItems(TransitionNames());
	m_defaultTransition->setCurrentText(QString::fromUtf8(preset->defaultTransition.c_str()));
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

	Preset *preset = CurrentPreset();
	m_scopeCombo->setEnabled(preset != nullptr);

	m_updating = true;
	m_scopeCombo->setCurrentIndex(preset && preset->global ? 1 : 0);
	m_updating = false;

	/* Selecting a preset in the editor also activates it: a preset that is
	 * being edited but not in effect would be a trap during a live show. */
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
		markId = selected.first()->data(0, kMarkIdRole).toString().toStdString();

	if (markId == m_selectedMarkId)
		return;

	m_selectedMarkId = markId;

	const bool hasMark = SelectedMark() != nullptr;
	m_duplicateMarkButton->setEnabled(hasMark);
	m_deleteMarkButton->setEnabled(hasMark);
	m_toggleMarkButton->setEnabled(hasMark);
	m_testMarkButton->setEnabled(hasMark);

	RefreshDetail();
}

void TransitionTreeDialog::OnSearchChanged(const QString &)
{
	if (m_updating)
		return;
	RefreshTree();
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

	Store &store = GetStore();
	Preset preset;
	preset.name = store.UniqueName(name.trimmed().toStdString());
	preset.global = m_scopeCombo->currentIndex() == 1;
	store.presets.push_back(preset);

	RefreshPresetHotkeys();
	if (preset.global)
		SaveGlobalPresets();

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
						   QLineEdit::Normal, QString::fromUtf8(preset->name.c_str()),
						   &accepted);
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

	const QString name = QString::fromUtf8(preset->name.c_str());
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

	if (wasActive)
		SetActivePreset(store.presets.empty() ? std::string() : store.presets.front().name);
	else
		RefreshAll();
}

void TransitionTreeDialog::ChangePresetScope(int index)
{
	if (m_updating)
		return;

	Preset *preset = CurrentPreset();
	if (!preset)
		return;

	const bool global = index == 1;
	if (preset->global == global)
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
			preset->marks.erase(it);
			break;
		}
	}

	m_selectedMarkId.clear();
	CommitChange(true);
	RefreshDetail();
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
	const QString canvasName = mark->canvas.empty() ? QString::fromUtf8(MainCanvasName().c_str())
							: QString::fromUtf8(mark->canvas.c_str());
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

	if (QMessageBox::question(
		    this, Tr("Mark.Test"),
		    Tr("Test.Confirm").arg(QString::fromUtf8(candidate.transition.c_str())).arg(destination)) !=
	    QMessageBox::Yes)
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

void TransitionTreeDialog::RemoveCandidate()
{
	Mark *mark = SelectedMark();
	if (!mark)
		return;

	const int row = m_candidates->currentRow();
	if (row < 0 || row >= (int)mark->candidates.size())
		return;

	mark->candidates.erase(mark->candidates.begin() + row);
	mark->ResetRuntimeState();

	RefreshMarkPage(*mark);
	CommitChange(true);
}

void TransitionTreeDialog::MoveCandidate(int delta)
{
	Mark *mark = SelectedMark();
	if (!mark)
		return;

	const int row = m_candidates->currentRow();
	const int target = row + delta;
	if (row < 0 || row >= (int)mark->candidates.size() || target < 0 || target >= (int)mark->candidates.size())
		return;

	std::swap(mark->candidates[(size_t)row], mark->candidates[(size_t)target]);
	mark->ResetRuntimeState();

	RefreshMarkPage(*mark);
	m_candidates->setCurrentCell(target, 0);
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
		QMessageBox::warning(this, Tr("Import.Title"), QString::fromUtf8(error.c_str()));
		return;
	}

	Store &store = GetStore();
	const bool asGlobal = m_scopeCombo->currentIndex() == 1;
	std::string firstName;

	for (auto &preset : imported) {
		preset.global = asGlobal;
		preset.name = store.UniqueName(preset.name);
		if (firstName.empty())
			firstName = preset.name;
		store.presets.push_back(preset);
	}

	RefreshPresetHotkeys();
	if (asGlobal)
		SaveGlobalPresets();

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

void TransitionTreeDialog::ReadMatcherFromWidgets(SceneMatcher &matcher, QComboBox *typeCombo, QListWidget *sceneList)
{
	const int index = typeCombo->currentIndex();
	matcher.type = (index >= 0 && index <= 2) ? (MatchType)index : MatchType::Scenes;

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
