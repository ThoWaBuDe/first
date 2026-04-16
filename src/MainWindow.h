#pragma once
#include <QMainWindow>
#include <QScrollBar>
#include <QEvent>
#include "Agent.h"
#include "ConfigDialog.h"
#include "EditorDock.h"
#include "PlannerDock.h"
#include "NodeGraphView.h"
#include "SearchBar.h"


namespace Ui { class MainWindow; }

// ─── MainWindow ───────────────────────────────────────────────────────────────
// Pattern: View aus MVP.
// Kennt nur Widgets und Agent. Keine Business-Logik.
//
// Neu: PlannerDock als QDockWidget an der linken Seite.
// Wird initial ausgeblendet und durch Agent::modeChanged(Plan) eingeblendet.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    bool eventFilter(QObject *obj, QEvent *event) override;

private slots:
    void onSendClicked();
    void onClearToolsClicked();
    void onSettingsClicked();

    void onAppendChat(const QString &html, const QString &cssClass);
    void onAppendChatToken(const QString &text);
    void onAppendTools(const QString &html, const QString &cssClass);
    void onInputEnabled(bool enabled);
    void onStatsUpdated(int promptTokens, int generatedTokens,
                        int totalTokens,  int ctxSize);

    // Modus-Änderung: PlannerDock ein-/ausblenden
    void onModeChanged(AgentMode mode);

 //   void onShowGraph();

    void onRefreshGraph();

    void onSearchRequested();

private:
    void setupUi();
    void setupConnections();

    bool isScrolledToBottom(QScrollBar *sb) const;
    void scrollToBottomIfNeeded(QScrollBar *sb);

    Ui::MainWindow *ui;
    Agent          *m_agent;
    EditorDock     *m_editorDock;
    PlannerDock    *m_plannerDock;   // NEU
    NodeGraphView *m_graphView = nullptr;
    SearchBar *m_searchBar = nullptr;

    static constexpr int AUTOSCROLL_THRESHOLD = 20;
};
