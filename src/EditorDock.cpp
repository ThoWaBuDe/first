#include "EditorDock.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QFile>
#include <QTextStream>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QShortcut>
#include <QKeySequence>
#include <QFontMetrics>
#include <QFont>

// ═════════════════════════════════════════════════════════════════════════════
// CppHighlighter
// ═════════════════════════════════════════════════════════════════════════════

CppHighlighter::CppHighlighter(QTextDocument *parent)
    : QSyntaxHighlighter(parent)
    , m_commentStart(R"(/\*)")
    , m_commentEnd  (R"(\*/)")
{
    // Hilfslambda: QTextCharFormat kompakt erzeugen
    auto makeFormat = [](QColor color, bool bold = false, bool italic = false) {
        QTextCharFormat f;
        f.setForeground(color);
        if (bold)   f.setFontWeight(QFont::Bold);
        if (italic) f.setFontItalic(true);
        return f;
    };

    // ── Preprocessor: #include, #define, #pragma ... ──────────────────────
    // Vor Keywords weil "#if" sonst als Keyword erkannt wird.
    m_rules.append({ QRegularExpression(R"(^\s*#\s*\w+)"),
                     makeFormat(QColor("#7a3f9d"), true) });

    // ── Qt-Makros ─────────────────────────────────────────────────────────
    m_rules.append({ QRegularExpression(
        R"(\b(Q_OBJECT|Q_ENUM|Q_PROPERTY|Q_DECLARE_METATYPE|)"
        R"(Q_ARG|Q_UNUSED|signals|slots|emit|SIGNAL|SLOT)\b)"),
        makeFormat(QColor("#b5651d"), true) });

    // ── C++ Keywords ──────────────────────────────────────────────────────
    // \b = Wortgrenze: "class" in "className" matcht nicht.
    m_rules.append({ QRegularExpression(
        R"(\b(alignas|alignof|auto|bool|break|case|catch|char|class|)"
        R"(const|constexpr|continue|decltype|default|delete|do|double|)"
        R"(else|enum|explicit|extern|false|float|for|friend|goto|if|)"
        R"(inline|int|long|mutable|namespace|new|noexcept|nullptr|)"
        R"(operator|override|private|protected|public|return|short|)"
        R"(signed|sizeof|static|static_assert|static_cast|struct|switch|)"
        R"(template|this|throw|true|try|typedef|typename|union|unsigned|)"
        R"(using|virtual|void|volatile|while)\b)"),
        makeFormat(QColor("#0033b3"), true) });

    // ── Zahlen: Hex, Float, Integer ───────────────────────────────────────
    m_rules.append({ QRegularExpression(
        R"(\b(0[xX][0-9a-fA-F]+[uUlL]*|\d+\.\d*[fFlL]?|\.\d+[fFlL]?|\d+[uUlL]*)\b)"),
        makeFormat(QColor("#1750eb")) });

    // ── String-Literale "..." ─────────────────────────────────────────────
    m_rules.append({ QRegularExpression(R"("(?:[^"\\]|\\.)*")"),
                     makeFormat(QColor("#067d17")) });

    // ── Char-Literale '...' ───────────────────────────────────────────────
    m_rules.append({ QRegularExpression(R"('(?:[^'\\]|\\.)*')"),
                     makeFormat(QColor("#067d17")) });

    // ── Einzeilige Kommentare // ──────────────────────────────────────────
    // Zuletzt → überschreibt Keywords/Strings in Kommentaren.
    m_rules.append({ QRegularExpression(R"(//[^\n]*)"),
                     makeFormat(QColor("#8c8c8c"), false, true) });

    // Mehrzeilige Kommentare: in highlightBlock() separat behandelt.
    m_multiLineCommentFormat = makeFormat(QColor("#8c8c8c"), false, true);
}

// ─── highlightBlock ──────────────────────────────────────────────────────────
// Von Qt automatisch für jede Zeile aufgerufen.
//
// Phase 1: alle einfachen Rules (Regex → setFormat)
// Phase 2: mehrzeiliger Kommentar-Automat (Block-State)
//
// Block-State-Automat:
//   State 0 = normal
//   State 1 = innerhalb /* ... */
// previousBlockState() liefert den State der Vorgängerzeile.
// setCurrentBlockState() setzt den State für die nachfolgende Zeile.
// Analogie AVR: State überlebt zwischen ISR-Aufrufen wie eine globale
// Variable im SRAM.
void CppHighlighter::highlightBlock(const QString &text)
{
    // Phase 1
    for (const Rule &rule : m_rules) {
        QRegularExpressionMatchIterator it = rule.pattern.globalMatch(text);
        while (it.hasNext()) {
            auto m = it.next();
            setFormat(m.capturedStart(), m.capturedLength(), rule.format);
        }
    }

    // Phase 2: /* ... */ über Zeilengrenzen
    setCurrentBlockState(0);

    int startIndex = 0;
    if (previousBlockState() != 1)
        startIndex = text.indexOf(m_commentStart);

    while (startIndex >= 0) {
        auto endMatch  = m_commentEnd.match(text, startIndex);
        int  endIndex  = endMatch.capturedStart();
        int  commentLen;

        if (endIndex == -1) {
            // Kein */ → Kommentar geht in nächste Zeile weiter
            setCurrentBlockState(1);
            commentLen = text.length() - startIndex;
        } else {
            commentLen = endIndex - startIndex + endMatch.capturedLength();
        }

        setFormat(startIndex, commentLen, m_multiLineCommentFormat);
        startIndex = text.indexOf(m_commentStart, startIndex + commentLen);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// EditorTab
// ═════════════════════════════════════════════════════════════════════════════

EditorTab::EditorTab(const QString &filePath, QWidget *parent)
    : QWidget(parent)
    , m_filePath(filePath)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // ── QPlainTextEdit ────────────────────────────────────────────────────
    // QPlainTextEdit statt QTextEdit: kein HTML-Overhead, effizientes
    // Scrollen auch bei großen Dateien.
    m_editor = new QPlainTextEdit(this);

    // Monospace Font: JetBrains Mono bevorzugt, Fallback auf System-Mono.
    // Qt sucht automatisch Fallbacks wenn die gewünschte Font nicht installiert ist.
    QFont font("JetBrains Mono");
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    font.setPointSize(10);
    m_editor->setFont(font);

    // Tab-Breite = 4 Leezeichen (in Pixeln)
    QFontMetrics fm(font);
    m_editor->setTabStopDistance(4.0 * fm.horizontalAdvance(' '));

    // Syntax-Highlighting: verbindet sich intern mit dem QTextDocument.
    // Kein manuelles Signal nötig — Qt ruft highlightBlock() automatisch.
    m_highlighter = new CppHighlighter(m_editor->document());

    layout->addWidget(m_editor);

    // ── Datei laden ───────────────────────────────────────────────────────
    reload();

    // ── dirty-Flag: User tippt etwas ─────────────────────────────────────
    // contentsChanged() feuert bei JEDER Textänderung im Dokument.
    // m_loading-Guard verhindert false-positive während reload() lädt.
    connect(m_editor->document(), &QTextDocument::contentsChanged,
            this, [this]() {
        if (m_loading) return;
        if (!m_dirty) {
            m_dirty = true;
            emit stateChanged();
        }
    });
}

void EditorTab::reload()
{
    QFile file(m_filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;

    // Guard setzen: contentsChanged() soll während reload() nicht feuern.
    // Analogie AVR: cli() vor kritischer Section, sei() danach.
    m_loading = true;
    m_editor->setPlainText(QTextStream(&file).readAll());
    m_loading = false;

    m_dirty         = false;
    m_externChanged = false;
    emit stateChanged();
}

bool EditorTab::save()
{
    QFile file(m_filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;

    QTextStream(&file) << m_editor->toPlainText();

    m_dirty         = false;
    m_externChanged = false;
    emit stateChanged();
    return true;
}

void EditorTab::setExternChanged(bool v)
{
    if (m_externChanged == v) return;
    m_externChanged = v;
    emit stateChanged();
}

// ═════════════════════════════════════════════════════════════════════════════
// EditorDock
// ═════════════════════════════════════════════════════════════════════════════

EditorDock::EditorDock(QWidget *parent)
    : QDockWidget("Code-Editor", parent)
{
    setMinimumWidth(450);

    // ── Container + Layout ────────────────────────────────────────────────
    auto *container = new QWidget(this);
    auto *layout    = new QVBoxLayout(container);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    // ── Toolbar (manuell, kein QToolBar) ──────────────────────────────────
    // QToolBar ist für QMainWindow gedacht. Im QDockWidget bauen wir
    // eine einfache Button-Reihe mit QHBoxLayout.
    auto *toolbar    = new QWidget(container);
    auto *toolLayout = new QHBoxLayout(toolbar);
    toolLayout->setContentsMargins(0, 0, 0, 0);
    toolLayout->setSpacing(4);

    auto *openBtn = new QPushButton("📂 Öffnen");
    m_saveBtn     = new QPushButton("💾 Speichern");
    m_reloadBtn   = new QPushButton("🔄 Neu laden");
    m_closeBtn    = new QPushButton("✕ Schließen");

    toolLayout->addWidget(openBtn);
    toolLayout->addWidget(m_saveBtn);
    toolLayout->addWidget(m_reloadBtn);
    toolLayout->addWidget(m_closeBtn);
    toolLayout->addStretch();
    layout->addWidget(toolbar);

    // ── Tab-Widget ────────────────────────────────────────────────────────
    m_tabs = new QTabWidget(container);
    m_tabs->setMovable(true);          // Tabs per Drag umsortierbar
    m_tabs->setTabsClosable(false);    // wir haben eigenen Close-Button
    layout->addWidget(m_tabs);

    setWidget(container);

    // ── Signal-Slot-Verbindungen ──────────────────────────────────────────
    connect(openBtn,     &QPushButton::clicked, this, &EditorDock::onOpenClicked);
    connect(m_saveBtn,   &QPushButton::clicked, this, &EditorDock::onSaveClicked);
    connect(m_reloadBtn, &QPushButton::clicked, this, &EditorDock::onReloadClicked);
    connect(m_closeBtn,  &QPushButton::clicked, this, &EditorDock::onCloseTabClicked);
    connect(m_tabs, &QTabWidget::currentChanged, this, &EditorDock::onTabChanged);

    // QFileSystemWatcher: ein Signal für alle überwachten Dateien.
    // Die Quelle (welche Datei) kommt als Parameter im Signal.
    connect(&m_watcher, &QFileSystemWatcher::fileChanged,
            this, &EditorDock::onFileChanged);

    // Ctrl+S Shortcut: wirkt wenn EditorDock oder ein Kind-Widget den Fokus hat
    auto *saveShortcut = new QShortcut(QKeySequence::Save, this);
    connect(saveShortcut, &QShortcut::activated, this, &EditorDock::onSaveClicked);

    updateButtonStates();
}

// ─── sandboxPath ─────────────────────────────────────────────────────────────
QString EditorDock::sandboxPath()
{
    QString sandbox = QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
                      + "/llamatools";
    return QDir(sandbox).exists() ? sandbox
           : QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
}

// ─── openFile ────────────────────────────────────────────────────────────────
// Öffnet Datei in neuem Tab. Wenn bereits offen: Tab-Fokus wechseln.
// Pattern: Idempotenz — doppelter Aufruf hat keine Nebenwirkungen.
void EditorDock::openFile(const QString &path)
{
    int existing = findTab(path);
    if (existing >= 0) {
        m_tabs->setCurrentIndex(existing);
        return;
    }

    auto *tab = new EditorTab(path, this);

    // stateChanged() → Tab-Titel + Buttons aktualisieren
    connect(tab, &EditorTab::stateChanged, this, [this, tab]() {
        int idx = m_tabs->indexOf(tab);
        if (idx >= 0) updateTabTitle(idx);
        updateButtonStates();
    });

    m_tabs->addTab(tab, QFileInfo(path).fileName());
    m_tabs->setCurrentWidget(tab);

    // Datei in QFileSystemWatcher eintragen
    m_watcher.addPath(path);

    updateButtonStates();
}

// ─── onOpenClicked ───────────────────────────────────────────────────────────
void EditorDock::onOpenClicked()
{
    QString path = QFileDialog::getOpenFileName(
        this,
        "Datei öffnen",
        sandboxPath(),
        "C/C++ Dateien (*.cpp *.cxx *.cc *.c *.h *.hpp *.hxx);;"
        "CMake (CMakeLists.txt *.cmake);;"
        "Markdown (*.md *.txt);;"
        "Alle Dateien (*)");

    if (!path.isEmpty())
        openFile(path);
}

// ─── onSaveClicked ───────────────────────────────────────────────────────────
void EditorDock::onSaveClicked()
{
    EditorTab *tab = currentEditorTab();
    if (!tab || !tab->isDirty()) return;

    QString path = tab->filePath();

    // Watcher kurz deaktivieren damit unser eigenes Speichern nicht als
    // "externe Änderung" interpretiert wird.
    // Analogie AVR: cli() vor Schreibzugriff, sei() danach.
    m_watcher.removePath(path);

    bool ok = tab->save();

    m_watcher.addPath(path);   // immer wieder hinzufügen, auch bei Fehler

    if (ok) {
        emit fileSavedByUser(path);
    } else {
        QMessageBox::warning(this, "Speichern fehlgeschlagen",
            QString("Datei konnte nicht gespeichert werden:\n%1").arg(path));
    }

    updateTabTitle(m_tabs->currentIndex());
    updateButtonStates();
}

// ─── onReloadClicked ─────────────────────────────────────────────────────────
void EditorDock::onReloadClicked()
{
    EditorTab *tab = currentEditorTab();
    if (!tab) return;

    if (tab->isDirty()) {
        auto answer = QMessageBox::question(
            this, "Neu laden",
            QString("'%1' hat ungespeicherte Änderungen.\n"
                    "Wirklich neu laden und Änderungen verwerfen?")
            .arg(QFileInfo(tab->filePath()).fileName()),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (answer != QMessageBox::Yes) return;
    }

    tab->reload();
    updateTabTitle(m_tabs->currentIndex());
    updateButtonStates();
}

// ─── onCloseTabClicked ───────────────────────────────────────────────────────
void EditorDock::onCloseTabClicked()
{
    int idx = m_tabs->currentIndex();
    if (idx < 0) return;

    EditorTab *tab = currentEditorTab();
    if (!tab) return;

    if (tab->isDirty()) {
        auto answer = QMessageBox::question(
            this, "Tab schließen",
            QString("'%1' hat ungespeicherte Änderungen.\n"
                    "Trotzdem schließen?")
            .arg(QFileInfo(tab->filePath()).fileName()),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (answer != QMessageBox::Yes) return;
    }

    m_watcher.removePath(tab->filePath());
    m_tabs->removeTab(idx);
    delete tab;

    updateButtonStates();
}

// ─── onTabChanged ────────────────────────────────────────────────────────────
void EditorDock::onTabChanged(int /*index*/)
{
    updateButtonStates();
}

// ─── onFileChanged ───────────────────────────────────────────────────────────
// QFileSystemWatcher meldet: Datei auf Disk hat sich geändert.
//
// Linux-Besonderheit ("inotify quirk"):
//   Viele Editoren und Tools schreiben Dateien atomar:
//   1. Neue Datei schreiben (temp)
//   2. Alte Datei löschen
//   3. Temp umbenennen → Original
//   Das löst IN_DELETE_SELF aus — QFileSystemWatcher entfernt den Pfad
//   automatisch. Wir müssen ihn mit addPath() wieder eintragen.
void EditorDock::onFileChanged(const QString &path)
{
    int idx = findTab(path);
    if (idx < 0) return;

    // Re-add nach atomic rename (s. Linux-Besonderheit oben)
    if (!m_watcher.files().contains(path))
        m_watcher.addPath(path);

    auto *tab = qobject_cast<EditorTab*>(m_tabs->widget(idx));
    if (!tab) return;

    tab->setExternChanged(true);
    updateTabTitle(idx);
    updateButtonStates();
}

// ─── updateButtonStates ──────────────────────────────────────────────────────
void EditorDock::updateButtonStates()
{
    EditorTab *tab = currentEditorTab();
    bool hasTab    = (tab != nullptr);

    m_saveBtn->setEnabled  (hasTab && tab->isDirty());
    m_reloadBtn->setEnabled(hasTab && tab->isExternChanged());
    m_closeBtn->setEnabled (hasTab);
}

// ─── updateTabTitle ──────────────────────────────────────────────────────────
void EditorDock::updateTabTitle(int index)
{
    if (index < 0 || index >= m_tabs->count()) return;
    auto *tab = qobject_cast<EditorTab*>(m_tabs->widget(index));
    if (tab) m_tabs->setTabText(index, tabTitle(tab));
}

// ─── tabTitle ────────────────────────────────────────────────────────────────
// Präfix-Konvention:
//   "*" = User hat editiert (dirty)
//   "!" = extern geändert (Modell hat geschrieben)
//   beide = "*!"
QString EditorDock::tabTitle(EditorTab *tab) const
{
    QString prefix;
    if (tab->isDirty())         prefix += "*";
    if (tab->isExternChanged()) prefix += "!";
    if (!prefix.isEmpty())      prefix += " ";
    return prefix + QFileInfo(tab->filePath()).fileName();
}

// ─── currentEditorTab ────────────────────────────────────────────────────────
EditorTab *EditorDock::currentEditorTab() const
{
    return qobject_cast<EditorTab*>(m_tabs->currentWidget());
}

// ─── findTab ─────────────────────────────────────────────────────────────────
// Linearer Scan — bei n ≤ ~10 Tabs optimal (kein Hash-Overhead).
int EditorDock::findTab(const QString &path) const
{
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *tab = qobject_cast<EditorTab*>(m_tabs->widget(i));
        if (tab && tab->filePath() == path)
            return i;
    }
    return -1;
}
