#pragma once
// ─── SearchBar ────────────────────────────────────────────────────────────────
// Wiederverwendbare Such-Toolbar für QTextEdit und QPlainTextEdit.
//
// Features:
//   - Ctrl+F öffnet, Escape schließt
//   - Enter / ▼ = nächster Treffer, Shift+Enter / ▲ = vorheriger
//   - Treffer werden farbig hervorgehoben (QTextEdit::ExtraSelections)
//   - Wrap-Around: nach letztem Treffer springt es zum ersten
//   - Funktioniert mit QTextEdit UND QPlainTextEdit
//
// Verwendung:
//   SearchBar *bar = new SearchBar(this);
//   bar->attachTo(m_chatView);    // QTextEdit
//   bar->attachTo(m_editor);      // QPlainTextEdit
//   bar->hide();                  // initial versteckt
//
//   // Shortcut:
//   QShortcut *s = new QShortcut(QKeySequence::Find, parentWidget);
//   connect(s, &QShortcut::activated, bar, &SearchBar::show);
//
// Pattern: Adapter — SearchBar adaptiert QTextEdit und QPlainTextEdit
//          hinter einer gemeinsamen Schnittstelle (TextEditAdapter).

#include <QWidget>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QTextDocument>
#include <QShortcut>
#include <QKeySequence>
#include <QKeyEvent>

// ─── Interner Adapter ─────────────────────────────────────────────────────────
// Versteckt den Unterschied zwischen QTextEdit und QPlainTextEdit.
// Analogie AVR: HAL (Hardware Abstraction Layer) — gleiche API, anderer Chip.
struct TextEditAdapter {
    QTextEdit      *te  = nullptr;
    QPlainTextEdit *pte = nullptr;

    explicit TextEditAdapter(QTextEdit *e)      : te(e)  {}
    explicit TextEditAdapter(QPlainTextEdit *e) : pte(e) {}

    bool isValid() const { return te || pte; }

    // Sucht vorwärts oder rückwärts. Gibt true zurück wenn gefunden.
    bool find(const QString &text, bool backward = false) {
        QTextDocument::FindFlags flags;
        if (backward) flags |= QTextDocument::FindBackward;
        if (te)  return te->find(text, flags);
        if (pte) return pte->find(text, flags);
        return false;
    }

    // Zum Anfang springen (für Wrap-Around)
    void moveToStart(bool backward) {
        QTextCursor c = backward ? textCursor() : QTextCursor(document());
        if (!backward) c.movePosition(QTextCursor::Start);
        else           c.movePosition(QTextCursor::End);
        setTextCursor(c);
    }

    QTextDocument *document() {
        if (te)  return te->document();
        if (pte) return pte->document();
        return nullptr;
    }

    QTextCursor textCursor() {
        if (te)  return te->textCursor();
        if (pte) return pte->textCursor();
        return {};
    }

    void setTextCursor(const QTextCursor &c) {
        if (te)  te->setTextCursor(c);
        if (pte) pte->setTextCursor(c);
    }

    // Alle Treffer highlighten (ExtraSelections)
    void highlightAll(const QString &text) {
        if (text.isEmpty()) {
            clearHighlights();
            return;
        }

        QList<QTextEdit::ExtraSelection> extras;
        QTextDocument *doc = document();
        if (!doc) return;

        QTextCursor c(doc);
        QTextCharFormat fmt;
        fmt.setBackground(QColor("#fff176")); // gelb
        fmt.setForeground(QColor("#000000"));

        while (true) {
            c = doc->find(text, c);
            if (c.isNull()) break;
            QTextEdit::ExtraSelection sel;
            sel.format = fmt;
            sel.cursor = c;
            extras << sel;
        }

        if (te)  te->setExtraSelections(extras);
        if (pte) pte->setExtraSelections(extras);
    }

    void clearHighlights() {
        if (te)  te->setExtraSelections({});
        if (pte) pte->setExtraSelections({});
    }
};

// ─── SearchBar ────────────────────────────────────────────────────────────────
class SearchBar : public QWidget
{
    Q_OBJECT

public:
    explicit SearchBar(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setAutoFillBackground(true);
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(4, 2, 4, 2);
        layout->setSpacing(4);

        // 🔍 Label
        auto *icon = new QLabel("🔍");
        layout->addWidget(icon);

        // Suchfeld
        m_input = new QLineEdit;
        m_input->setPlaceholderText("Suchen... (Enter=weiter, Shift+Enter=zurück)");
        m_input->setFixedHeight(24);
        m_input->setClearButtonEnabled(true);
        layout->addWidget(m_input, 1);

        // ▼ Vorwärts
        m_nextBtn = new QPushButton("▼");
        m_nextBtn->setFixedSize(24, 24);
        m_nextBtn->setToolTip("Nächster Treffer (Enter)");
        layout->addWidget(m_nextBtn);

        // ▲ Rückwärts
        m_prevBtn = new QPushButton("▲");
        m_prevBtn->setFixedSize(24, 24);
        m_prevBtn->setToolTip("Vorheriger Treffer (Shift+Enter)");
        layout->addWidget(m_prevBtn);

        // Treffer-Anzeige "3/12"
        m_countLabel = new QLabel;
        m_countLabel->setStyleSheet("color: #888; font-size: 10px; min-width: 40px;");
        layout->addWidget(m_countLabel);

        // ✕ Schließen
        m_closeBtn = new QPushButton("✕");
        m_closeBtn->setFixedSize(20, 20);
        m_closeBtn->setStyleSheet("QPushButton { border: none; color: #888; }"
                                   "QPushButton:hover { color: #c00; }");
        layout->addWidget(m_closeBtn);

        // Verbindungen
        connect(m_input,    &QLineEdit::textChanged,
                this,       &SearchBar::onTextChanged);
        connect(m_input,    &QLineEdit::returnPressed,
                this,       &SearchBar::findNext);
        connect(m_nextBtn,  &QPushButton::clicked,
                this,       &SearchBar::findNext);
        connect(m_prevBtn,  &QPushButton::clicked,
                this,       &SearchBar::findPrev);
        connect(m_closeBtn, &QPushButton::clicked,
                this,       &SearchBar::closeBar);

        // Escape schließt
        auto *esc = new QShortcut(Qt::Key_Escape, this);
        connect(esc, &QShortcut::activated, this, &SearchBar::closeBar);

        setFixedHeight(32);
        hide();
    }

    // Aktiven TextEdit setzen (von außen aufgerufen)
    void attachTo(QTextEdit *te) {
        detach();
        m_adapter = new TextEditAdapter(te);
        if (!isHidden()) onTextChanged(m_input->text());
    }

    void attachTo(QPlainTextEdit *pte) {
        detach();
        m_adapter = new TextEditAdapter(pte);
        if (!isHidden()) onTextChanged(m_input->text());
    }

    void detach() {
        if (m_adapter) {
            m_adapter->clearHighlights();
            delete m_adapter;
            m_adapter = nullptr;
        }
        m_countLabel->clear();
    }

public slots:
    void openBar() {
        show();
        m_input->setFocus();
        m_input->selectAll();
        onTextChanged(m_input->text());
    }

    void closeBar() {
        hide();
        if (m_adapter) m_adapter->clearHighlights();
        m_countLabel->clear();
    }

    void findNext() {
        if (!m_adapter || !m_adapter->isValid()) return;
        QString text = m_input->text();
        if (text.isEmpty()) return;

        bool found = m_adapter->find(text, /*backward=*/false);
        if (!found) {
            // Wrap-Around: zum Anfang springen
            m_adapter->moveToStart(false);
            found = m_adapter->find(text, false);
        }
        updateStyle(found);
        updateCount(text);
    }

    void findPrev() {
        if (!m_adapter || !m_adapter->isValid()) return;
        QString text = m_input->text();
        if (text.isEmpty()) return;

        bool found = m_adapter->find(text, /*backward=*/true);
        if (!found) {
            // Wrap-Around: zum Ende springen
            m_adapter->moveToStart(true);
            found = m_adapter->find(text, true);
        }
        updateStyle(found);
        updateCount(text);
    }

private slots:
    void onTextChanged(const QString &text) {
        if (!m_adapter) return;
        m_adapter->highlightAll(text);
        updateCount(text);
        updateStyle(true); // Reset Farbe
        if (!text.isEmpty()) findNext();
    }

private:
    QLineEdit   *m_input     = nullptr;
    QPushButton *m_nextBtn   = nullptr;
    QPushButton *m_prevBtn   = nullptr;
    QLabel      *m_countLabel= nullptr;
    QPushButton *m_closeBtn  = nullptr;
    TextEditAdapter *m_adapter = nullptr;

    // Anzahl Treffer zählen und anzeigen
    void updateCount(const QString &text) {
        if (!m_adapter || text.isEmpty()) {
            m_countLabel->clear();
            return;
        }
        QTextDocument *doc = m_adapter->document();
        if (!doc) return;

        int count = 0;
        QTextCursor c(doc);
        while (true) {
            c = doc->find(text, c);
            if (c.isNull()) break;
            ++count;
        }
        if (count == 0)
            m_countLabel->setText("0");
        else
            m_countLabel->setText(QString("%1").arg(count));
    }

    // Suchfeld rot färben wenn kein Treffer
    void updateStyle(bool found) {
        if (m_input->text().isEmpty()) {
            m_input->setStyleSheet("");
        } else if (!found) {
            m_input->setStyleSheet(
                "QLineEdit { background: #fce8e6; color: #c5221f; }");
        } else {
            m_input->setStyleSheet(
                "QLineEdit { background: #e6f4ea; }");
        }
    }
};
