#pragma once
#include <QMainWindow>
#include <QScrollBar>
#include <QEvent>
#include "Agent.h"
#include "ConfigDialog.h"
#include "EditorDock.h"

namespace Ui { class MainWindow; }

// ─── MainWindow ───────────────────────────────────────────────────────────────
// Pattern: View aus MVP.
// Kennt nur Widgets und Agent. Keine Business-Logik.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    // eventFilter: Enter/Shift+Enter im inputEdit abfangen.
    // Override von QObject::eventFilter() — muss im Header deklariert sein
    // damit der Compiler die Methode als Override von QObject::eventFilter()
    // erkennt und nicht als freie neue Methode.
    bool eventFilter(QObject *obj, QEvent *event) override;

private slots:
    // UI-Events
    void onSendClicked();
    void onClearToolsClicked();
    void onSettingsClicked();

    // Agent → UI
    void onAppendChat(const QString &html, const QString &cssClass);
    void onAppendChatToken(const QString &text);
    void onAppendTools(const QString &html, const QString &cssClass);
    void onInputEnabled(bool enabled);
    void onStatsUpdated(int promptTokens, int generatedTokens,
                        int totalTokens,  int ctxSize);

private:
    void setupUi();
    void setupConnections();

    bool isScrolledToBottom(QScrollBar *sb) const;
    void scrollToBottomIfNeeded(QScrollBar *sb);

    Ui::MainWindow *ui;
    Agent          *m_agent;

    EditorDock      *m_editorDock;

    // MODEL_PATH kommt jetzt aus AppConfig — nicht mehr hardcodiert hier.

    static constexpr int AUTOSCROLL_THRESHOLD = 20;
};
