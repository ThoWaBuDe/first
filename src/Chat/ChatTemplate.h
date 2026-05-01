#pragma once
#include <QString>

// ─── ChatTemplate ─────────────────────────────────────────────────────────────
// Beschreibt das Token-Format das ein Modell für Rollen-Nachrichten erwartet.
//
// Pattern: Value Object — reines Daten-Struct, keine Logik, keine Qt-Abhängigkeit
//          außer QString.
//
// Jedes Modell hat sein eigenes "Chat Template" das definiert wie Nachrichten
// von verschiedenen Rollen (system / user / assistant / tool) eingerahmt werden.
//
// Beispiel ChatML (Qwen, viele andere):
//   <|im_start|>system
//   Du bist ein Assistent.<|im_end|>
//   <|im_start|>user
//   Hallo<|im_end|>
//   <|im_start|>assistant
//   ← Modell generiert hier
//
// assistantEnd ist bewusst leer: das Modell generiert bis zu seinem eigenen
// EOS-Token — wir öffnen den Assistant-Turn nur, schließen ihn nicht.
//
// toolRole: Manche Modelle erwarten Tool-Ergebnisse als "tool"-Rolle (Llama3),
// andere als "user"-Rolle (ChatML, Mistral). Wenn toolRole leer ist →
// Tool-Ergebnisse werden als User-Nachrichten formatiert (ChatModel-Verhalten).
//
// ─── TODO ────────────────────────────────────────────────────────────────────
// Jinja2-Rendering: GGUF-Dateien enthalten das Chat Template als Jinja2-String
// (Key: "tokenizer.chat_template"). Aktuell wird dieser String nur zur
// Heuristik-Erkennung des Presets verwendet (s. ChatTemplate::fromGguf()).
//
// Für vollständige Kompatibilität (z.B. Modelle mit unbekannten Templates)
// wäre ein echter Jinja2-Interpreter nötig. Kandidaten:
//   - inja (header-only C++ Jinja2, https://github.com/pantor/inja)
//   - llama.cpp hat seit v0.0.1875 llama_chat_apply_template() eingebaut
//     die den Jinja2-String intern rendert — das wäre die sauberste Lösung.
//
// Wenn llama_chat_apply_template() genutzt wird, könnte ChatModel::buildPrompt()
// den rohen Jinja2-String an llama.cpp delegieren statt selbst zu formatieren.
// Priorität: niedrig, solange die Preset-Heuristik für die unterstützten
// Modelle ausreicht.
// ─────────────────────────────────────────────────────────────────────────────

struct ChatTemplate {

    // ─── Enum: alle unterstützten Presets ────────────────────────────────
    enum class Preset {
        Auto,    // aus GGUF erkannt — kein eigener String, wird zu einem der anderen
        ChatML,  // <|im_start|> / <|im_end|>  — Qwen, viele Fine-Tunes
        Llama3,  // <|start_header_id|> / <|end_header_id|> / <|eot_id|>
        Gemma,   // <start_of_turn> / <end_of_turn>
        Mistral, // [INST] / [/INST]
        Custom   // User-definierter String in customTemplate
    };

    // ─── Format-Strings ──────────────────────────────────────────────────
    QString systemStart;     // vor system-Inhalt
    QString systemEnd;       // nach system-Inhalt
    QString userStart;       // vor user-Inhalt
    QString userEnd;         // nach user-Inhalt
    QString assistantStart;  // vor assistant-Inhalt (Modell generiert hier)
    QString assistantEnd;    // nach assistant-Inhalt (meist leer)
    QString toolRole;        // Rollenname für Tool-Ergebnisse ("tool" oder "user" oder "")

    // ─── Preset-Factory ──────────────────────────────────────────────────
    // Gibt ein fertig ausgefülltes ChatTemplate für das gewünschte Preset zurück.
    // Pattern: Factory Method — Erzeugung von Objekten hinter einer benannten
    // Methode statt rohem Konstruktor-Aufruf.

    static ChatTemplate forPreset(Preset preset)
    {
        switch (preset) {
        case Preset::ChatML:
        case Preset::Auto:   // Auto fällt auf ChatML zurück falls GGUF kein Template hat
            return chatML();
        case Preset::Llama3:
            return llama3();
        case Preset::Gemma:
            return gemma();
        case Preset::Mistral:
            return mistral();
        case Preset::Custom:
            return {};  // leer — wird von außen befüllt
        }
        return chatML();
    }

    // ─── Preset: ChatML ──────────────────────────────────────────────────
    // Verwendet von: Qwen2/3, viele Community-Fine-Tunes
    // Referenz: https://huggingface.co/docs/transformers/chat_templating
    static ChatTemplate chatML()
    {
        ChatTemplate t;
        t.systemStart    = "<|im_start|>system\n";
        t.systemEnd      = "<|im_end|>\n";
        t.userStart      = "<|im_start|>user\n";
        t.userEnd        = "<|im_end|>\n";
        t.assistantStart = "<|im_start|>assistant\n";
        t.assistantEnd   = "";       // Modell generiert bis <|im_end|>
        t.toolRole       = "user";   // Tool-Ergebnisse als user-Nachricht
        return t;
    }

    // ─── Preset: Llama 3 ─────────────────────────────────────────────────
    // Verwendet von: Meta Llama 3.x, viele Llama3-basierende Modelle
    // Referenz: https://llama.meta.com/docs/model-cards-and-prompt-formats/meta-llama-3/
    static ChatTemplate llama3()
    {
        ChatTemplate t;
        t.systemStart    = "<|start_header_id|>system<|end_header_id|>\n\n";
        t.systemEnd      = "<|eot_id|>\n";
        t.userStart      = "<|start_header_id|>user<|end_header_id|>\n\n";
        t.userEnd        = "<|eot_id|>\n";
        t.assistantStart = "<|start_header_id|>assistant<|end_header_id|>\n\n";
        t.assistantEnd   = "";
        t.toolRole       = "tool";   // Llama3 hat echte tool-Rolle
        return t;
    }

    // ─── Preset: Gemma ───────────────────────────────────────────────────
    // Verwendet von: Google Gemma 2/3
    // Hinweis: Gemma hat keine system-Rolle — System-Prompt wird als
    // erste user-Nachricht eingefügt (systemStart == userStart).
    // Referenz: https://ai.google.dev/gemma/docs/formatting
    static ChatTemplate gemma()
    {
        ChatTemplate t;
        t.systemStart    = "<start_of_turn>user\n";  // kein eigenes system-Tag
        t.systemEnd      = "<end_of_turn>\n";
        t.userStart      = "<start_of_turn>user\n";
        t.userEnd        = "<end_of_turn>\n";
        t.assistantStart = "<start_of_turn>model\n";
        t.assistantEnd   = "<end_of_turn>\n";
        t.toolRole       = "user";
        return t;
    }

    // ─── Preset: Mistral ─────────────────────────────────────────────────
    // Verwendet von: Mistral 7B v0.1/v0.2, Mixtral
    // Hinweis: Mistral hat ebenfalls keine dedizierte system-Rolle.
    // System-Prompt wird in die erste [INST]-Nachricht integriert.
    // Referenz: https://docs.mistral.ai/guides/tokenization/
    static ChatTemplate mistral()
    {
        ChatTemplate t;
        t.systemStart    = "[INST] ";   // System wird in erste [INST] verpackt
        t.systemEnd      = " [/INST]\n";
        t.userStart      = "[INST] ";
        t.userEnd        = " [/INST]\n";
        t.assistantStart = "";          // Mistral braucht keinen expliziten Öffner
        t.assistantEnd   = "</s>\n";
        t.toolRole       = "user";
        return t;
    }

    // ─── GGUF Auto-Detection ─────────────────────────────────────────────
    // Analysiert den Jinja2-Template-String aus dem GGUF-Metadaten-Feld
    // "tokenizer.chat_template" und erkennt das passende Preset anhand
    // charakteristischer Token-Strings.
    //
    // Pattern: Heuristic Matching — kein vollständiger Jinja2-Parser,
    // nur Substring-Suche nach eindeutigen Marker-Strings.
    //
    // Rückgabe: das erkannte Preset (nie Auto — Auto ist nur ein UI-Zustand).
    // Falls kein Preset erkannt wird → ChatML als sicherer Fallback.
    //
    // TODO: Wenn llama_chat_apply_template() in llama.cpp verfügbar ist,
    // könnte dieser Heuristik-Ansatz durch direktes Template-Rendering
    // ersetzt werden. Dann wäre "Auto" kein Mapping auf ein Preset mehr,
    // sondern ein Durchreichen des rohen Jinja2-Strings an llama.cpp.
    static Preset detectFromJinja(const QString &jinja)
    {
        if (jinja.isEmpty())
            return Preset::ChatML;  // Fallback

        if (jinja.contains("<|im_start|>"))
            return Preset::ChatML;

        if (jinja.contains("<|start_header_id|>"))
            return Preset::Llama3;

        if (jinja.contains("<start_of_turn>"))
            return Preset::Gemma;

        if (jinja.contains("[INST]"))
            return Preset::Mistral;

        // Unbekanntes Template → ChatML als Fallback, aber User sollte
        // informiert werden (AppConfig speichert den rohen Jinja2-String
        // für Debug-Zwecke in detectedJinjaTemplate).
        return Preset::ChatML;
    }

    // ─── Hilfsfunktion: Preset → Anzeigename ─────────────────────────────
    static QString presetName(Preset p)
    {
        switch (p) {
        case Preset::Auto:    return "Auto (aus Modell)";
        case Preset::ChatML:  return "ChatML (Qwen, standard)";
        case Preset::Llama3:  return "Llama 3";
        case Preset::Gemma:   return "Gemma";
        case Preset::Mistral: return "Mistral";
        case Preset::Custom:  return "Custom";
        }
        return "ChatML";
    }
};
