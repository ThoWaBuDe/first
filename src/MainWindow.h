#pragma once
#include <QMainWindow>
#include "Agent.h"

namespace Ui { class MainWindow; }

// ─── MainWindow ───────────────────────────────────────────────────────────────
// Pattern: View aus MVP.
// Kennt nur Widgets und Agent. Keine Business-Logik.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    // UI-Events
    void onSendClicked();
    void onClearToolsClicked();

    // Agent → UI
    void onAppendChat(const QString &html, const QString &cssClass);

    // Token in laufenden Absatz einfügen (QTextCursor, kein neues <p>)
    void onAppendChatToken(const QString &text);

    void onAppendTools(const QString &html, const QString &cssClass);
    void onInputEnabled(bool enabled);
    void onStatsUpdated(int promptTokens, int generatedTokens,
                        int totalTokens,  int ctxSize);

private:
    void setupUi();
    void setupConnections();

    Ui::MainWindow *ui;
    Agent          *m_agent;

    static constexpr const char *MODEL_PATH =
        "/home/thomas/ai/models/Qwen3.5-9B-Q6_K.gguf";
};
