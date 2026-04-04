#pragma once
#include <QMainWindow>
#include <QThread>
#include "ChatModel.h"
#include "McpManager.h"

// ─── Forward Declaration des generierten UI-Namespace ────────────────────────
// uic erzeugt aus MainWindow.ui die Datei ui_MainWindow.h mit der Klasse
// Ui::MainWindow. Die enthält alle Widget-Pointer als public Member.
// Forward-Declaration im Header hält die Compile-Abhängigkeit klein —
// ui_MainWindow.h wird nur in MainWindow.cpp includiert.
namespace Ui { class MainWindow; }

class QTextEdit;
class QLineEdit;
class QPushButton;
class QLabel;
class LlamaWorker;

// ─── MainWindow ───────────────────────────────────────────────────────────────
// Haupt-UI Klasse. Koordiniert Worker-Thread, ChatModel und McpManager.
//
// Pattern: MVC (Model-View-Controller)
//   Model:      ChatModel (Daten)
//   View:       MainWindow.ui (Qt Designer)
//   Controller: MainWindow (Logik)
//
// UI-Widgets werden von ui_MainWindow.h verwaltet (generiert von uic).
// Zugriff: ui->chatView, ui->inputLine, ui->sendButton, etc.
//
// Thread-Architektur:
//   GUI-Thread:    MainWindow (alle UI-Operationen)
//   Worker-Thread: LlamaWorker (Inference)
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void onSendClicked();
    void onStopClicked();
    void onClearClicked();

    void onTokenReceived(const QString &token);
    void onGenerationDone(const QString &fullResponse);
    void onModelLoaded();
    void onError(const QString &error);

private:
    void setupWorker();
    void sendMessage(const QString &userText);
    void appendToChat(const QString &text, const QString &cssClass);
    void setInputEnabled(bool enabled);

    // ─── Generierter UI-Container ─────────────────────────────────────────
    // Ui::MainWindow ist eine von uic generierte Klasse.
    // Sie enthält alle im Designer definierten Widgets als public Member:
    //   ui->chatView    (QTextEdit*)
    //   ui->inputLine   (QLineEdit*)
    //   ui->sendButton  (QPushButton*)
    //   ui->stopButton  (QPushButton*)
    //   ui->clearButton (QPushButton*)
    //   ui->statusLabel (QLabel*)
    // PIMPL-Pattern: Ui::MainWindow als Pointer — Compile-Firewall.
    Ui::MainWindow *ui;

    // ─── Backend ──────────────────────────────────────────────────────────
    QThread        m_workerThread;
    LlamaWorker   *m_worker   = nullptr;
    McpManager     m_mcp;
    ChatModel      m_chatModel;

    QString m_currentResponse;
    bool    m_generating = false;

    // Continuation-Mechanismus für abgebrochene Tool-Call JSON-Blöcke
    int m_continuationCount = 0;
    static constexpr int MAX_CONTINUATIONS = 3;

    static constexpr const char *MODEL_PATH =
        "/home/thomas/ai/models/Qwen3.5-9B-Q6_K.gguf";
};
