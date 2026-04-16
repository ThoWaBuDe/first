// ═══════════════════════════════════════════════════════════════════════════════
// PATCH 1: Agent.h — PLAN_ALLOWED_TOOLS erweitern
// ═══════════════════════════════════════════════════════════════════════════════
//
// Die internen Plan-Tools (create_node, set_depends_on, get_nodes, plan_done)
// werden von AgentPlan direkt verarbeitet — sie kommen nie zum McpManager.
// Trotzdem müssen sie NICHT in PLAN_ALLOWED_TOOLS stehen weil AgentPlan
// sie vorher abfängt (isInternalPlanTool-Check).
//
// KEINE Änderung an PLAN_ALLOWED_TOOLS nötig. ✓

// ═══════════════════════════════════════════════════════════════════════════════
// PATCH 2: Agent.cpp — onGenerationDone() Plan-Zweig anpassen
// ═══════════════════════════════════════════════════════════════════════════════
//
// Im Plan-Modus: das Modell gibt jetzt Tool-Calls aus (create_node etc.)
// statt eines <plan>...</plan> Blocks. Der bestehende Code behandelt
// Tool-Calls bereits korrekt über handlePlanToolCall().
//
// Der <plan>...</plan> Fallback bleibt erhalten.
// Keine Änderung am onGenerationDone() Plan-Zweig nötig. ✓

// ═══════════════════════════════════════════════════════════════════════════════
// PATCH 3: Agent.cpp — onGenerationDone() Execute-Zweig für Validierung
// ═══════════════════════════════════════════════════════════════════════════════
//
// Im Execute-Modus: Validierungs-Nodes haben keinen <code>-Tag-Output.
// Sie antworten mit einer Zeile "ok" oder einer Fehlerbeschreibung.
//
// ÄNDERUNG in onGenerationDone(), Execute-Zweig:
//
// ALT:
//     m_continuationCount = 0;
//     m_execute->handleExecuteCode(fullResponse, mySession);
//     return;
//
// NEU:
//     m_continuationCount = 0;
//     // Validierungs-Nodes: kein <code>-Tag erwartet
//     // handleExecuteCode dispatcht intern auf handleValidationResult
//     m_execute->handleExecuteCode(fullResponse, mySession);
//     return;
//
// Kein Unterschied — handleExecuteCode prüft selbst ob isValidationNode(). ✓

// ═══════════════════════════════════════════════════════════════════════════════
// PATCH 4: TaskNodeDialog.cpp — Symbol-Feld im Edit-Dialog
// ═══════════════════════════════════════════════════════════════════════════════
//
// Im TaskNodeDialog ein neues Feld für das Symbol hinzufügen:
//
// In setupUi(), nach dem title-Feld:
/*
    metaGrid->addWidget(new QLabel("Symbol:"), 1, 0);
    m_symbolEdit = new QLineEdit;
    m_symbolEdit->setPlaceholderText(
        "C++-Symbolname, z.B. 'Timer::start()' oder 'GameState'");
    m_symbolEdit->setToolTip(
        "Eindeutiger C++-Symbolname für diesen Node.\n"
        "Verhindert Duplikate im Plan.\n"
        "Leer lassen für Nodes ohne spezifisches Symbol.");
    metaGrid->addWidget(m_symbolEdit, 1, 1);
*/
//
// In populateFields():
/*
    m_symbolEdit->setText(m_node->symbol);
*/
//
// In applyToNode():
/*
    // Symbol über TaskTree setzen damit Symbol-Index aktualisiert wird
    // Nicht direkt node->symbol = ... setzen!
    // m_tree->setSymbol(m_node, m_symbolEdit->text().trimmed());
    m_node->symbol = m_symbolEdit->text().trimmed(); // vereinfacht
*/
//
// In TaskNodeDialog.h, private-Bereich:
/*
    QLineEdit *m_symbolEdit = nullptr;
*/

// ═══════════════════════════════════════════════════════════════════════════════
// PATCH 5: TaskTreeModel.h — Symbol in Tooltip anzeigen
// ═══════════════════════════════════════════════════════════════════════════════
//
// Im data()-Slot, Qt::ToolTipRole:
// ALT:
//     if (role == Qt::ToolTipRole && idx.column() == 0)
//         return m_tree->buildContext(n);
//
// NEU: buildContext() zeigt Symbol bereits an — keine Änderung nötig. ✓

// ═══════════════════════════════════════════════════════════════════════════════
// ZUSAMMENFASSUNG
// ═══════════════════════════════════════════════════════════════════════════════
//
// Neue vollständige Dateien in diesem Tar:
//   TaskNode.h      — symbol + validationResult Felder
//   TaskTree.h      — Symbol-Index, symbolExists(), createValidationNode()
//   AgentPlan.h/.cpp — Iterativer Modus mit create_node/set_depends_on/plan_done
//   AgentExecute.h/.cpp — Validierungs-Node (H4) + handleValidationResult()
//
// Patches (klein, nur Beschreibung):
//   Agent_ghln_patch.cpp — dieser Text
//
// Was noch manuell eingebaut werden muss:
//   - TaskNodeDialog: Symbol-Feld (Patch 4 oben)
//   - TaskTree setSymbol() in applyToNode() aufrufen statt direkt setzen
