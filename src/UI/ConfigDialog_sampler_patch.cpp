// ─── PATCH ConfigDialog.h ─────────────────────────────────────────────────────
// Im private-Bereich, nach den Tool-Sampler-Feldern, hinzufügen:

/*
    // ─── Sampler Execute (NEU — Punkt I) ─────────────────────────────────
    QSpinBox       *m_executeTopK;
    QDoubleSpinBox *m_executeTemp;
    QDoubleSpinBox *m_executeTopP;
    QDoubleSpinBox *m_executeMinP;
*/

// ─── PATCH ConfigDialog.cpp — createSamplerTab() ─────────────────────────────
// Den bestehenden createSamplerTab() ersetzen:

QWidget *ConfigDialog::createSamplerTab()
{
    auto *w      = new QWidget;
    auto *layout = new QVBoxLayout(w);  // Vertikal statt Horizontal wegen 3 Gruppen
    layout->setSpacing(12);

    // ── Layout: 3 Gruppen nebeneinander ──────────────────────────────────
    auto *hLayout = new QHBoxLayout;
    hLayout->setSpacing(16);

    // ── Chat-Profil ───────────────────────────────────────────────────────
    auto *chatGroup = new QGroupBox("Chat  (Temp 0.7 — kreativ)");
    auto *chatForm  = new QFormLayout(chatGroup);
    m_chatTopK = makeSpinBox(1, 200, 40);
    m_chatTemp = makeDoubleSpinBox(0.0, 2.0, 0.05, 2, 0.7);
    m_chatTopP = makeDoubleSpinBox(0.0, 1.0, 0.05, 2, 0.95);
    m_chatMinP = makeDoubleSpinBox(0.0, 1.0, 0.01, 2, 0.05);
    chatForm->addRow("Top-K:", m_chatTopK);
    chatForm->addRow("Temp:",  m_chatTemp);
    chatForm->addRow("Top-P:", m_chatTopP);
    chatForm->addRow("Min-P:", m_chatMinP);
    hLayout->addWidget(chatGroup);

    // ── Execute-Profil (NEU — Punkt I) ────────────────────────────────────
    // Zwischen Chat und Tool: Temp 0.2 für Code-Generierung.
    // Niedrig genug für korrekte Syntax, hoch genug für Algorithmen.
    auto *execGroup = new QGroupBox("Execute  (Temp 0.2 — Code)");
    auto *execForm  = new QFormLayout(execGroup);
    m_executeTopK = makeSpinBox(1, 100, 20);
    m_executeTemp = makeDoubleSpinBox(0.0, 1.0, 0.05, 2, 0.2);
    m_executeTopP = makeDoubleSpinBox(0.0, 1.0, 0.05, 2, 0.6);
    m_executeMinP = makeDoubleSpinBox(0.0, 1.0, 0.01, 2, 0.05);
    execForm->addRow("Top-K:", m_executeTopK);
    execForm->addRow("Temp:",  m_executeTemp);
    execForm->addRow("Top-P:", m_executeTopP);
    execForm->addRow("Min-P:", m_executeMinP);
    auto *execHint = new QLabel(
        "<small style='color:#888'>"
        "Für Code-Generierung im Execute-Modus.<br>"
        "Temp zwischen Chat (kreativ) und Tool (JSON)."
        "</small>");
    execHint->setWordWrap(true);
    auto *execFormW = new QFormLayout;
    execFormW->addRow("", execHint);
    // Hint direkt in execGroup
    QVBoxLayout *execBox = new QVBoxLayout;
    execBox->addLayout(execForm);
    execBox->addWidget(execHint);
    execGroup->setLayout(execBox);
    hLayout->addWidget(execGroup);

    // ── Tool-Profil ───────────────────────────────────────────────────────
    auto *toolGroup = new QGroupBox("Tool  (Temp 0.1 — JSON)");
    auto *toolForm  = new QFormLayout(toolGroup);
    m_toolTopK = makeSpinBox(1, 100, 20);
    m_toolTemp = makeDoubleSpinBox(0.0, 1.0, 0.05, 2, 0.1);
    m_toolTopP = makeDoubleSpinBox(0.0, 1.0, 0.05, 2, 0.5);
    m_toolMinP = makeDoubleSpinBox(0.0, 1.0, 0.01, 2, 0.05);
    toolForm->addRow("Top-K:", m_toolTopK);
    toolForm->addRow("Temp:",  m_toolTemp);
    toolForm->addRow("Top-P:", m_toolTopP);
    toolForm->addRow("Min-P:", m_toolMinP);
    hLayout->addWidget(toolGroup);

    layout->addLayout(hLayout);

    // Alle Änderungen → samplersChanged markieren
    auto markSamplers = [this]{ m_samplersChanged = true; };
    for (auto *sb : {m_chatTopK, m_executeTopK, m_toolTopK})
        connect(sb, QOverload<int>::of(&QSpinBox::valueChanged),
                this, markSamplers);
    for (auto *sb : {m_chatTemp, m_chatTopP, m_chatMinP,
                     m_executeTemp, m_executeTopP, m_executeMinP,
                     m_toolTemp, m_toolTopP, m_toolMinP})
        connect(sb, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, markSamplers);

    layout->addStretch();
    return w;
}

// ─── PATCH loadFromConfig() — Execute-Werte laden ────────────────────────────
/*
    m_executeTopK->setValue(cfg.executeTopK());
    m_executeTemp->setValue(cfg.executeTemp());
    m_executeTopP->setValue(cfg.executeTopP());
    m_executeMinP->setValue(cfg.executeMinP());
*/

// ─── PATCH saveToConfig() — Execute-Werte speichern ─────────────────────────
/*
    cfg.setExecuteTopK(m_executeTopK->value());
    cfg.setExecuteTemp(static_cast<float>(m_executeTemp->value()));
    cfg.setExecuteTopP(static_cast<float>(m_executeTopP->value()));
    cfg.setExecuteMinP(static_cast<float>(m_executeMinP->value()));
*/

// ─── PATCH resetSamplerDefaults() — Execute-Defaults ────────────────────────
/*
    m_executeTopK->setValue(20); m_executeTemp->setValue(0.2);
    m_executeTopP->setValue(0.60); m_executeMinP->setValue(0.05);
*/
