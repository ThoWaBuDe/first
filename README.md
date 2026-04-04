<<<<<<< HEAD
# first
=======
# LlamaQt — Qt6 llama.cpp Chat Interface

## Abhängigkeiten

```bash
sudo apt install qt6-base-dev cmake build-essential
```

## Build

```bash
# Projektverzeichnis anlegen
mkdir -p ~/llamaqt && cd ~/llamaqt
# Dateien hineinkopieren, dann:

mkdir build && cd build

cmake .. \
  -DLLAMA_BUILD_DIR=$HOME/ai/qLP/build \
  -DLLAMA_SRC_DIR=$HOME/ai/qLP \
  -DCMAKE_BUILD_TYPE=Release

make -j$(nproc)
```

## Starten

```bash
./LlamaQt
# oder falls libllama.so nicht im Systempfad:
LD_LIBRARY_PATH=$HOME/ai/qLP/build:$HOME/ai/qLP/build/src ./LlamaQt
```

## Modell-Pfad anpassen

In `src/MainWindow.h`:
```cpp
static constexpr const char *MODEL_PATH =
    "/home/thomas/ai/models/Qwen3.5-9B-Q6_K.gguf";
```

## Tool-Sandbox

Das Modell kann nur auf `~/llamatools/` zugreifen.
Dateien dort ablegen zum Testen:
```bash
mkdir ~/llamatools
echo "Hallo Welt" > ~/llamatools/test.txt
```

Dann im Chat: *"Lies die Datei test.txt"*

## Architektur

```
GUI-Thread                    Worker-Thread
──────────────────────────    ──────────────────────────
MainWindow                    LlamaWorker
  ├── ChatModel                 ├── llama_model*
  ├── ToolDispatcher            ├── llama_context*
  └── Qt Widgets                └── llama_sampler*

Signal-Slot (QueuedConnection = thread-sichere Queue):
  tokenGenerated  →  onTokenReceived
  generationDone  →  onGenerationDone
  modelLoaded     →  onModelLoaded

Patterns:
  Active Object    - LlamaWorker im eigenen Thread
  Producer/Consumer- Token-Streaming via Signals
  MVC              - ChatModel / Widgets / MainWindow
  Command Dispatcher - ToolDispatcher
  Chain of Responsibility - Sampler-Chain
```
>>>>>>> 9de199c (inital push)
