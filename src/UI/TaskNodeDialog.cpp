#include "TaskNodeDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QDialogButtonBox>
#include <QSplitter>
#include <QHeaderView>
#include <QDateTime>

// ─── Konstruktor ──────────────────────────────────────────────────────────────
TaskNodeDialog::TaskNodeDialog(TaskTree *tree, TaskNode *node, QWidget *parent)
    : QDialog(parent)
    , m_tree(tree)
    , m_node(node)
{
    setWindowTitle(QString("Knoten bearbeiten — %1").arg(node->title));
    setMinimumSize(700, 520);
    setupUi();
    populateFields();
}

// ─── setupUi ─────────────────────────────────────────────────────────────────
// Layout-Struktur:
//
//  ┌─ Metadaten (Grid) ────────────────────────────────────────────────┐
//  │  Titel: [_________]  Level: [▼]  Scope: [▼]  Status: [▼]        │
//  └───────────────────────────────────────────────────────────────────┘
//  ┌─ QSplitter (vertikal) ────────────────────────────────────────────┐
//  │  ┌─ Beschreibung ──────────────────────────────────────────────┐  │
//  │  │  [QPlainTextEdit]                                           │  │
//  │  └─────────────────────────────────────────────────────────────┘  │
//  │  ┌─ Ergebnis ──────────────────────────────────────────────────┐  │
//  │  │  [QPlainTextEdit]                                           │  │
//  │  └─────────────────────────────────────────────────────────────┘  │
//  └───────────────────────────────────────────────────────────────────┘
//  ┌─ Abhängigkeiten ──────────────────────────────────────────────────┐
//  │  [Filter______]                                                   │
//  │  [Verfügbar       ] [→] [Abhängig          ]                      │
//  │                     [←]                                           │
//  └───────────────────────────────────────────────────────────────────┘
//  [ OK ]  [ Abbrechen ]
void TaskNodeDialog::setupUi()
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(8);

    // ── Metadaten-Gruppe ──────────────────────────────────────────────────
    // Grid-Layout: Label in Spalte 0, Widget in Spalte 1
    auto *metaGroup = new QGroupBox("Metadaten");
    auto *metaGrid  = new QGridLayout(metaGroup);
    metaGrid->setColumnStretch(1, 1);  // Spalte 1 (Widgets) dehnt sich aus

    metaGrid->addWidget(new QLabel("Titel:"), 0, 0);
    m_titleEdit = new QLineEdit;
    metaGrid->addWidget(m_titleEdit, 0, 1);

    metaGrid->addWidget(new QLabel("Ebene:"), 1, 0);
    m_levelCombo = new QComboBox;
    // Bekannte Ebenen + offen nach oben
    // TaskLevel-Enum: Goal=0, Files=1, Class=2, Impl=3, Validation=4
    // Wir zeigen H0..H6, User kann also bis H6 wählen.
    // Analogie AVR: wie ein Prescaler-Register — feste Stufen, aber erweiterbar.
    for (int i = 0; i <= 6; ++i)
        m_levelCombo->addItem(TaskNode::levelName(i), i);
    metaGrid->addWidget(m_levelCombo, 1, 1);

    metaGrid->addWidget(new QLabel("Scope:"), 2, 0);
    m_scopeCombo = new QComboBox;
    m_scopeCombo->addItem("internal", static_cast<int>(TaskScope::Internal));
    m_scopeCombo->addItem("external", static_cast<int>(TaskScope::External));
    metaGrid->addWidget(m_scopeCombo, 2, 1);

    metaGrid->addWidget(new QLabel("Status:"), 3, 0);
    m_statusCombo = new QComboBox;
    // Alle TaskStatus-Werte — Reihenfolge muss mit enum class TaskStatus übereinstimmen
    m_statusCombo->addItem("pending",  static_cast<int>(TaskStatus::Pending));
    m_statusCombo->addItem("running",  static_cast<int>(TaskStatus::Running));
    m_statusCombo->addItem("done",     static_cast<int>(TaskStatus::Done));
    m_statusCombo->addItem("failed",   static_cast<int>(TaskStatus::Failed));
    m_statusCombo->addItem("blocked",  static_cast<int>(TaskStatus::Blocked));
    metaGrid->addWidget(m_statusCombo, 3, 1);

    mainLayout->addWidget(metaGroup);

    // ── Texte: Beschreibung + Ergebnis (QSplitter) ───────────────────────
    // QSplitter erlaubt dem User die Aufteilung anzupassen.
    // Beide QPlainTextEdit: monospace für Code-Fragmente in der description.
    auto *textSplitter = new QSplitter(Qt::Vertical);

    auto *descGroup = new QGroupBox("Beschreibung (Aufgabe, Impl-Hints, Signaturen)");
    auto *descLayout = new QVBoxLayout(descGroup);
    m_descEdit = new QPlainTextEdit;
    m_descEdit->setFont(QFont("monospace", 10));
    m_descEdit->setPlaceholderText(
        "Konkrete Implementierungshinweise, erwartete Signaturen, Abhängigkeiten...");
    descLayout->addWidget(m_descEdit);
    textSplitter->addWidget(descGroup);

    auto *resultGroup = new QGroupBox("Ergebnis (was der Agent produziert hat)");
    auto *resultLayout = new QVBoxLayout(resultGroup);
    m_resultEdit = new QPlainTextEdit;
    m_resultEdit->setFont(QFont("monospace", 10));
    m_resultEdit->setPlaceholderText("(leer = noch nicht bearbeitet)");
    resultLayout->addWidget(m_resultEdit);
    textSplitter->addWidget(resultGroup);

    textSplitter->setStretchFactor(0, 2);  // Beschreibung bekommt 2x mehr Platz
    textSplitter->setStretchFactor(1, 1);
    mainLayout->addWidget(textSplitter, 1);  // stretch=1 → nimmt verfügbaren Platz

    // ── dependsOn Transfer-Widget ─────────────────────────────────────────
    // Pattern: Transfer Widget (klassisch aus Qt-Dokumentation)
    //
    // Layout:
    //   [Filter__________]
    //   [Verfügbar  ] [→] [Abhängig   ]
    //                [←]
    //
    // Wie es funktioniert:
    //   1. Alle Nodes werden in m_availableList geladen (außer m_node selbst)
    //   2. Nodes die schon in dependsOn sind: wandern nach m_depsList
    //   3. User kann per Buttons zwischen den Listen wechseln
    //   4. applyToNode() liest m_depsList aus und schreibt IDs in node->dependsOn
    //
    // QListWidgetItem::data(Qt::UserRole) speichert die Node-ID (qint64).
    // So müssen wir keine String-Parsung machen.
    auto *depsGroup  = new QGroupBox("Abhängigkeiten (dependsOn)");
    auto *depsLayout = new QVBoxLayout(depsGroup);

    // Suchfeld
    auto *filterLayout = new QHBoxLayout;
    filterLayout->addWidget(new QLabel("Filter:"));
    m_filterEdit = new QLineEdit;
    m_filterEdit->setPlaceholderText("Titel oder Level eingeben...");
    m_filterEdit->setClearButtonEnabled(true);
    filterLayout->addWidget(m_filterEdit);
    depsLayout->addLayout(filterLayout);

    // Zwei Listen + Buttons
    auto *listsLayout = new QHBoxLayout;

    // Linke Liste: verfügbare Nodes
    auto *availLayout = new QVBoxLayout;
    availLayout->addWidget(new QLabel("Verfügbar:"));
    m_availableList = new QListWidget;
    m_availableList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_availableList->setAlternatingRowColors(true);
    m_availableList->setMinimumHeight(100);
    availLayout->addWidget(m_availableList);
    listsLayout->addLayout(availLayout);

    // Pfeil-Buttons in der Mitte
    // Vertikal zentriert mit Stretchern oben und unten.
    auto *btnLayout = new QVBoxLayout;
    btnLayout->addStretch();
    m_addBtn = new QPushButton("→");
    m_addBtn->setFixedWidth(36);
    m_addBtn->setToolTip("Markierte Nodes als Abhängigkeit hinzufügen");
    btnLayout->addWidget(m_addBtn);
    m_removeBtn = new QPushButton("←");
    m_removeBtn->setFixedWidth(36);
    m_removeBtn->setToolTip("Markierte Abhängigkeiten entfernen");
    btnLayout->addWidget(m_removeBtn);
    btnLayout->addStretch();
    listsLayout->addLayout(btnLayout);

    // Rechte Liste: abhängige Nodes
    auto *depLayout = new QVBoxLayout;
    depLayout->addWidget(new QLabel("Abhängig von:"));
    m_depsList = new QListWidget;
    m_depsList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_depsList->setAlternatingRowColors(true);
    m_depsList->setMinimumHeight(100);
    depLayout->addWidget(m_depsList);
    listsLayout->addLayout(depLayout);

    depsLayout->addLayout(listsLayout);
    mainLayout->addWidget(depsGroup);

    // ── OK / Abbrechen ────────────────────────────────────────────────────
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(buttons);

    // ── Connections ───────────────────────────────────────────────────────
    connect(m_filterEdit, &QLineEdit::textChanged,
            this,         &TaskNodeDialog::onFilterChanged);
    connect(m_addBtn,    &QPushButton::clicked,
            this,        &TaskNodeDialog::onAddDep);
    connect(m_removeBtn, &QPushButton::clicked,
            this,        &TaskNodeDialog::onRemoveDep);
}

// ─── populateFields ──────────────────────────────────────────────────────────
// Füllt alle Felder mit den aktuellen Werten des Nodes.
void TaskNodeDialog::populateFields()
{
    m_titleEdit->setText(m_node->title);
    m_descEdit->setPlainText(m_node->description);
    m_resultEdit->setPlainText(m_node->result);

    // Level-ComboBox: finde den Index dessen UserRole-Wert == node->level
    // QComboBox::findData() sucht nach UserRole. O(n) aber n <= 7 — egal.
    int levelIdx = m_levelCombo->findData(m_node->level);
    if (levelIdx >= 0) m_levelCombo->setCurrentIndex(levelIdx);

    int scopeIdx = m_scopeCombo->findData(static_cast<int>(m_node->scope));
    if (scopeIdx >= 0) m_scopeCombo->setCurrentIndex(scopeIdx);

    int statusIdx = m_statusCombo->findData(static_cast<int>(m_node->status));
    if (statusIdx >= 0) m_statusCombo->setCurrentIndex(statusIdx);

    // dependsOn-Listen aufbauen
    populateAvailableList();
}

// ─── nodeLabel ───────────────────────────────────────────────────────────────
// Einheitliches Anzeigeformat für einen Node in beiden Listen.
// Format: "H2: MainWindow.h"
// Warum kein "(id=3)"? Zu technisch für den User. ID steckt in UserRole.
QString TaskNodeDialog::nodeLabel(const TaskNode *n)
{
    return QString("%1: %2").arg(TaskNode::levelName(n->level), n->title);
}

// ─── populateAvailableList ───────────────────────────────────────────────────
// Füllt die linke Liste mit allen Nodes außer m_node selbst.
// Nodes die bereits in dependsOn sind → gehen in die rechte Liste.
//
// Traversierung über TaskTree::traverse() (Tiefensuche).
// Jeder QListWidgetItem bekommt:
//   Qt::DisplayRole  → nodeLabel()
//   Qt::UserRole     → node->id (qint64) für spätere ID-Auflösung
void TaskNodeDialog::populateAvailableList()
{
    m_availableList->clear();
    m_depsList->clear();

    m_tree->traverse([this](const TaskNode *n) {
        // Sich selbst nicht anzeigen — man kann nicht von sich selbst abhängen
        if (n->id == m_node->id) return;

        auto *item = new QListWidgetItem(nodeLabel(n));
        item->setData(Qt::UserRole, static_cast<qlonglong>(n->id));

        // Tooltip zeigt description — so sieht der User was hinter dem Titel steckt
        if (!n->description.isEmpty())
            item->setToolTip(n->description.left(200));

        // Gehört dieser Node bereits zu dependsOn → rechte Liste
        if (m_node->dependsOn.contains(n->id))
            m_depsList->addItem(item);
        else
            m_availableList->addItem(item);
    });
}

// ─── onFilterChanged ─────────────────────────────────────────────────────────
// Blendet Einträge in der linken Liste aus die nicht zum Filtertext passen.
// Qt::UserRole ändern wir nicht — nur sichtbar/unsichtbar schalten.
//
// Analogie AVR: wie ein Hardware-Filter der nur bestimmte Frequenzen durchlässt —
// die Daten bleiben erhalten, nur die Anzeige ändert sich.
void TaskNodeDialog::onFilterChanged(const QString &text)
{
    for (int i = 0; i < m_availableList->count(); ++i) {
        QListWidgetItem *item = m_availableList->item(i);
        // Groß-/Kleinschreibung ignorieren (Qt::CaseInsensitive)
        bool matches = text.isEmpty() ||
                       item->text().contains(text, Qt::CaseInsensitive);
        item->setHidden(!matches);
    }
}

// ─── onAddDep ────────────────────────────────────────────────────────────────
// Verschiebt markierte Einträge von links nach rechts.
// "Verschieben" = aus availableList entfernen + in depsList einfügen.
//
// Warum takeItem() statt nur addItem()?
//   QListWidget ist owner der Items. takeItem() übergibt ownership —
//   ohne takeItem() würde das Item doppelt existieren (UB in Qt).
void TaskNodeDialog::onAddDep()
{
    // selectedItems() gibt Zeiger zurück — takeItem() braucht den Row-Index.
    // Wir sammeln die Rows erst, dann nehmen wir von hinten (höchster Index zuerst)
    // damit sich die Indizes nicht verschieben während wir löschen.
    //
    // Analogie AVR: wie UART-Buffer von hinten leeren — vermeidet Index-Drift.
    QList<int> rows;
    for (QListWidgetItem *item : m_availableList->selectedItems())
        rows.append(m_availableList->row(item));
    std::sort(rows.rbegin(), rows.rend());  // absteigend

    for (int row : rows) {
        QListWidgetItem *item = m_availableList->takeItem(row);
        item->setHidden(false);  // Filter zurücksetzen
        m_depsList->addItem(item);
    }
}

// ─── onRemoveDep ─────────────────────────────────────────────────────────────
// Verschiebt markierte Einträge von rechts zurück nach links.
void TaskNodeDialog::onRemoveDep()
{
    QList<int> rows;
    for (QListWidgetItem *item : m_depsList->selectedItems())
        rows.append(m_depsList->row(item));
    std::sort(rows.rbegin(), rows.rend());

    for (int row : rows) {
        QListWidgetItem *item = m_depsList->takeItem(row);
        item->setHidden(false);
        m_availableList->addItem(item);
    }

    // Filter neu anwenden — der zurückgewanderte Eintrag soll gefiltert werden
    // wenn der Filtertext aktiv ist.
    onFilterChanged(m_filterEdit->text());
}

// ─── applyToNode ─────────────────────────────────────────────────────────────
// Schreibt alle Formularwerte in den Node zurück.
// Wird von PlannerDock nach accept() aufgerufen — nicht direkt im Dialog
// damit PlannerDock kontrollieren kann wann der Write passiert.
//
// Warum nicht im accept()-Handler?
//   accept() ist ein Qt-Slot der den Dialog schließt. Wenn wir dort schreiben,
//   muss PlannerDock trotzdem noch taskTreeUpdated() emittieren.
//   Mit applyToNode() als separater Methode bleibt die Verantwortung klar:
//   Dialog = Formular, PlannerDock = Orchestrierung.
void TaskNodeDialog::applyToNode()
{
    m_node->title       = m_titleEdit->text().trimmed();
    m_node->description = m_descEdit->toPlainText();
    m_node->result      = m_resultEdit->toPlainText();
    m_node->level       = m_levelCombo->currentData().toInt();
    m_node->scope       = static_cast<TaskScope>(m_scopeCombo->currentData().toInt());
    m_node->status      = static_cast<TaskStatus>(m_statusCombo->currentData().toInt());
    m_node->updatedAt   = QDateTime::currentDateTime();
    m_node->dirty       = true;

    // dependsOn: IDs aus rechter Liste lesen
    m_node->dependsOn.clear();
    for (int i = 0; i < m_depsList->count(); ++i) {
        qint64 id = static_cast<qint64>(
            m_depsList->item(i)->data(Qt::UserRole).toLongLong());
        m_node->dependsOn.append(id);
    }
}
