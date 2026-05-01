#pragma once
#include <QDockWidget>
#include <QTabWidget>
#include <QPlainTextEdit>
#include <QToolBar>
#include <QPushButton>
#include <QFileSystemWatcher>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QRegularExpression>
#include <QVector>

// ─── CppHighlighter ───────────────────────────────────────────────────────────
// Einfacher C++ Syntax-Highlighter.
// Pattern: Template Method — QSyntaxHighlighter ist die abstrakte Basis,
// wir überschreiben nur highlightBlock().
//
// QSyntaxHighlighter arbeitet blockweise: highlightBlock() wird für jeden
// Absatz (= eine Zeile im QPlainTextEdit) aufgerufen wenn sich der Text
// ändert. Analogie AVR: wie ein UART-RX-Interrupt — pro Zeile getriggert,
// nicht für den ganzen Text auf einmal.
//
// Erkannte Kategorien:
//   Keywords      — if, while, class, return, ...
//   Preprocessor  — #include, #define, #pragma, ...
//   Qt-Makros     — Q_OBJECT, signals:, slots:, emit, ...
//   Strings       — "..." und '...'
//   Zahlen        — 42, 0xFF, 3.14f
//   Kommentare    — // einzeilig und /* ... */ mehrzeilig
//
// Mehrzeilige Kommentare sind der einzig komplexe Fall:
// QSyntaxHighlighter hat dafür einen Block-State-Mechanismus.
//   State 0 = normal
//   State 1 = innerhalb /* ... */
// Der State überlebt den Übergang zwischen zwei highlightBlock()-Aufrufen —
// genau wie ein Zustandsautomat im AVR der seinen State zwischen zwei
// ISR-Aufrufen im SRAM behält.

class CppHighlighter : public QSyntaxHighlighter {
    Q_OBJECT
public:
    explicit CppHighlighter(QTextDocument *parent = nullptr);

protected:
    // Wird von Qt automatisch für jede Zeile aufgerufen wenn sich Text ändert.
    void highlightBlock(const QString &text) override;

private:
    struct Rule {
        QRegularExpression pattern;
        QTextCharFormat    format;
    };
    QVector<Rule>   m_rules;

    // Mehrzeilige Kommentare brauchen eigene Behandlung (s.o.)
    QTextCharFormat    m_multiLineCommentFormat;
    QRegularExpression m_commentStart;  // /*
    QRegularExpression m_commentEnd;    // */
};

// ─── EditorTab ────────────────────────────────────────────────────────────────
// Ein einzelner Editor-Tab: Datei + Zustand.
//
// Warum QPlainTextEdit statt QTextEdit?
//   QPlainTextEdit ist für Plain-Text optimiert — kein HTML-Rendering,
//   kein Paragraph-Overhead, scrollt effizienter bei langen Dateien.
//   QTextEdit (chatView) rendert HTML — für Code-Editing ungeeignet.
//
// Zwei Dirty-Flags, weil zwei unabhängige Quellen Änderungen melden:
//   m_dirty        → User hat im Editor getippt (nicht gespeichert)
//   m_externChanged → QFileSystemWatcher: Datei wurde von außen verändert
//                     (z.B. Modell hat str_replace aufgerufen)
//
// Tab-Titel zeigt "*" wenn irgendeins der beiden Flags gesetzt ist.
// "Neu laden" ist aktiv wenn m_externChanged.
// "Speichern" ist aktiv wenn m_dirty.

class EditorTab : public QWidget {
    Q_OBJECT
public:
    explicit EditorTab(const QString &filePath, QWidget *parent = nullptr);

    QString filePath()      const { return m_filePath; }
    bool    isDirty()       const { return m_dirty; }
    bool    isExternChanged() const { return m_externChanged; }

    // Lädt Dateiinhalt neu vom Disk — verwirft ungespeicherte Änderungen.
    // m_loading-Flag verhindert dass reload() selbst einen dirty-Signal auslöst.
    void reload();

    // Speichert aktuellen Editor-Inhalt auf Disk.
    // Gibt false zurück wenn die Datei nicht schreibbar ist.
    bool save();

    // Wird von EditorDock gesetzt wenn QFileSystemWatcher eine externe
    // Änderung gemeldet hat.
    void setExternChanged(bool v);

    QPlainTextEdit *editor() { return m_editor; }

signals:
    // Emittiert wenn sich dirty oder externChanged ändert.
    // → EditorDock aktualisiert Tab-Titel und Button-Zustände.
    void stateChanged();

private:
    QString        m_filePath;
    QPlainTextEdit *m_editor      = nullptr;
    CppHighlighter *m_highlighter = nullptr;
    bool           m_dirty        = false;
    bool           m_externChanged = false;

    // Guard: verhindert dirty-Signal während wir selbst laden/speichern
    bool           m_loading      = false;
};

// ─── EditorDock ───────────────────────────────────────────────────────────────
// Das andockbare Editor-Fenster mit Toolbar und Tab-Widget.
//
// Pattern: Facade — versteckt QTabWidget, QFileSystemWatcher und
// mehrere EditorTabs hinter einer einfachen Schnittstelle nach außen.
//
// QFileSystemWatcher — eine Instanz für alle offenen Dateien:
//   addPath()    wenn Tab geöffnet wird
//   removePath() wenn Tab geschlossen wird
//   fileChanged() Signal → betroffener Tab wird als extern-dirty markiert
//
//   Analogie AVR: wie ein Pin-Change-Interrupt der mehrere Pins
//   überwacht — ein Handler, mehrere Quellen, Quelle per Vergleich ermitteln.
//
// Toolbar-Buttons (immer sichtbar, aber kontextsensitiv aktiv/inaktiv):
//   [Öffnen]        — immer aktiv
//   [Speichern]     — aktiv wenn aktiver Tab m_dirty
//   [Neu laden]     — aktiv wenn aktiver Tab m_externChanged
//   [Tab schließen] — aktiv wenn mindestens ein Tab offen

class EditorDock : public QDockWidget {
    Q_OBJECT

public:
    explicit EditorDock(QWidget *parent = nullptr);
    QPlainTextEdit *currentEditor() const {
        EditorTab *tab = currentEditorTab();
        return tab ? tab->editor() : nullptr;
    }

    // Öffnet eine Datei in einem neuen Tab.
    // Wenn die Datei bereits offen ist, wird zum Tab gewechselt (kein Duplikat).
    void openFile(const QString &path);

signals:
    // Emittiert nach manuellem Speichern durch den User (Ctrl+S oder Button).
    // → MainWindow leitet weiter an Agent → sichtbare Systemnachricht im chatView.
    void fileSavedByUser(const QString &filePath);
    void searchRequested();

private slots:
    void onOpenClicked();
    void onSaveClicked();
    void onReloadClicked();
    void onCloseTabClicked();
    void onTabChanged(int index);

    // QFileSystemWatcher Signal: Datei auf Disk hat sich geändert.
    void onFileChanged(const QString &path);

private:
    void    updateButtonStates();
    void    updateTabTitle(int index);

    // Hilfsfunktion: Tab-Titel mit "*" wenn dirty
    QString tabTitle(EditorTab *tab) const;

    // Gibt den EditorTab des aktuell sichtbaren Tabs zurück, oder nullptr
    EditorTab *currentEditorTab() const;

    // Gibt den Tab-Index für einen Dateipfad zurück, oder -1
    int findTab(const QString &path) const;

    QTabWidget         *m_tabs    = nullptr;
    QFileSystemWatcher  m_watcher;

    // Toolbar-Buttons — Referenzen für updateButtonStates()
    QPushButton *m_saveBtn   = nullptr;
    QPushButton *m_reloadBtn = nullptr;
    QPushButton *m_closeBtn  = nullptr;

    // Sandbox-Startpfad für QFileDialog
    static QString sandboxPath();
};
