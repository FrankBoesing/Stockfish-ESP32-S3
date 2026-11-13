# Stockfish-ESP32-S3 – Portierungsanalyse

Stand: 2026-11-13

## 1. Ausgangspunkt und Revisions-Pin

Das Projekt wird auf einem separaten Branch weitergeführt:

- Arbeitsbranch: `stockfish-port-analysis`
- Ausgangsbranch: `board-diagnostics`
- Ausgangscommit: `80cd7a4a2b486836e5bc9eae1f53ce4d6c10422f`

Der Branch wurde exakt von diesem Commit angelegt. Die Diagnose-Firmware und ihre Ergebnisse bleiben damit als unveränderter Referenzstand erhalten.

Für die erste Engine-Integration wird **Stockfish 19 (Tag `sf_19`)** als reproduzierbare Upstream-Basis vorgeschlagen:

- Upstream: https://github.com/official-stockfish/Stockfish
- Tag-Commit: `edb0d9db6731067ec50ce619ff372b463bc4dd5d`
- Standardnetz: `nn-1a298aa575a0.nnue`
- Release: 2026-09-05

Nicht gegen den beweglichen `master`-Branch entwickeln. Upstream hat seit dem SF19-Release bereits weitere NNUE-Architekturänderungen vorgenommen. Diese sollten erst später bewusst übernommen werden.

## 2. Wichtigster Befund: Standard-SF19-NNUE passt nicht in den Speicher

Stockfish 19 verwendet SFNNv16. Die Architektur in `src/nnue/nnue_architecture.h` setzt:

- `L1 = 1024`
- `L2 = 32`
- `L3 = 32`
- `LayerStacks = 8`
- Feature-Sets: `HalfKAv2_hm`, `FullThreats`, `PP_3Wide`

Die relevanten Input-Dimensionen sind:

| Feature-Set | Dimensionen |
|---|---:|
| HalfKAv2_hm | 22.528 |
| FullThreats | 59.808 |
| PP_3Wide | 4.560 |
| Threats + Pawn-Pairs | 64.368 |

Der Feature-Transformer legt seine Gewichte als feste Arrays im `Network`-Objekt ab. Aus den Typen und Dimensionen in `nnue_common.h` und `nnue_feature_transformer.h` ergibt sich:

| Array | Speicher |
|---|---:|
| PSQ-Gewichte: 1024 × 22.528 × 2 Byte | 46.137.344 Byte |
| Threat/Pawn-Pair-Gewichte: 64.368 × 1024 × 1 Byte | 65.912.832 Byte |
| PSQT-Gewichte: 8 × 22.528 × 4 Byte | 720.896 Byte |
| Threat/Pawn-Pair-PSQT: 8 × 64.368 × 4 Byte | 2.059.776 Byte |
| Biases: 1024 × 2 Byte | 2.048 Byte |
| **Summe Feature-Transformer** | **114.832.896 Byte** |

Das sind rund **109,5 MiB**, noch ohne die nachgelagerten Netzwerk-Layer. Das normale SF19-Netz ist daher mit 8 MiB PSRAM nicht ladbar. Die Netzwerkdatei selbst liegt außerdem in einer Größenordnung von rund 99 MB und passt nicht in den 16-MiB-Flash.

Das ist eine harte Kapazitätsgrenze, kein durch einen besseren Allocator lösbares Problem. Ein direktes Auswerten aus Flash oder SD wäre eine andere, deutlich aufwendigere Architektur und würde die bestehenden festen Netzwerkarrays nicht umgehen.

## 3. Konkreter Kandidat: Stockfish 19 Smallnet

Es existiert eine Stockfish-19-Variante mit einem kleineren, speziell trainierten NNUE-Netz:

- Patch-Sammlung: https://github.com/lichess-org/stockfish-web/tree/main/patches/sf_19_smallnet
- Patch 1 (Feature-Änderung): https://raw.githubusercontent.com/lichess-org/stockfish-web/main/patches/sf_19_smallnet/0001-simple-mirrored-piece-square-features.patch
- Patch 2 (Lichess-Web-Integration): https://raw.githubusercontent.com/lichess-org/stockfish-web/main/patches/sf_19_smallnet/0002-stockfish-web.patch
- Patch-Basis: `3f6f417b87c0e80ee30914b6b539b4ab7d3b2a5b` (SF19-Entwicklungsstand vom 2026-09-01; Vorfahr des Releases), Commit-Präfix `edb0d9db`
- Netz: `nn-61e7af4bb97d.nnue`

Der Smallnet-Patch ersetzt die drei SF19-Feature-Sets durch `P_hm`. Dieses Feature bildet die farbbezogene Figur und ihr Feld ab und hat nur:

`12 × 64 = 768` Input-Dimensionen.

Die Standard-Netzwerkbreite `L1 = 1024` bleibt in diesem Patch erhalten. Threat- und Pawn-Pair-Gewichtsarrays entfallen. Für den Feature-Transformer ergibt sich damit:

| Array | Speicher |
|---|---:|
| Gewichte: 1024 × 768 × 2 Byte | 1.572.864 Byte |
| PSQT-Gewichte: 8 × 768 × 4 Byte | 24.576 Byte |
| Biases: 1024 × 2 Byte | 2.048 Byte |
| **Summe Feature-Transformer** | **1.599.488 Byte (1,53 MiB)** |

Das ist rund 71,8-mal kleiner als der Feature-Transformer des Standardnetzes. Die eigentlichen FC-Layer kommen noch hinzu, bleiben bei den kleinen Layer-Dimensionen aber vergleichsweise klein. Die häufig genannte Größenordnung von etwa 1,2 MB ist bislang nicht für genau diese Patch-/Netzrevision verifiziert. Dateigröße und Hash müssen beim Beschaffen des Netzes separat gemessen werden; 1,2 MB bleibt eine vorläufige Annahme.

**Einordnung:** Dies ist kein unverändertes Stockfish-19-Standardnetz. Der NNUE-Feature-Raum und die dazugehörigen Trainingsgewichte wurden geändert. Die Stockfish-basierte Such-Engine bleibt aber die Grundlage; es handelt sich nicht um eine vereinfachte Fremd-Engine. Die Patch-Beschreibung dokumentiert einen Test mit 20.000 Partien und einen Zugewinn von 9,75 ± 2,7 Elo gegenüber Stockfish 18 mit einem 15-MB-Netz. Das ist kein direkter Vergleich mit dem vollständigen Stockfish-19-Standardnetz und darf nicht als solcher interpretiert werden.

**Empfehlung:** Diese Smallnet-Variante ist der erste ernsthafte Kandidat für den ESP32-S3-Port. Vor Integration werden Patch-Basis, Netz-Hash, Lizenzhinweise und Build-Reproduzierbarkeit geprüft. Das Standardnetz bleibt als Referenz für Desktop-Vergleiche erhalten.

## 3a. Prüfung des Smallnet-Patches gegen SF19

Der Patch `0001-simple-mirrored-piece-square-features.patch` wurde anhand seiner Git-Index-Blob-IDs gegen den vollständigen Git-Tree des SF19-Release-Commits geprüft.

Ergebnis:

- Alle 18 vorhandenen Quell-/Builddateien stimmen in ihren alten Blob-IDs exakt mit SF19 überein.
- Die einzige zusätzliche Datei ist `src/nnue/features/p_hm.h`.
- Die erwarteten Ziel-Blob-IDs der geänderten Dateien stimmen mit dem vendorten Smallnet-Quellstand überein.
- Damit ist die Smallnet-Feature-Änderung auf dem festgepinnten SF19-Release exakt anwendbar; es handelt sich nicht nur um eine ähnlich passende spätere Entwicklungsrevision.
- Patch 2 der Patch-Sammlung ist eine WebAssembly-/Lichess-Integration und wurde nicht übernommen.

Der Patch-Basis-Commit `3f6f417b87c0e80ee30914b6b539b4ab7d3b2a5b` in der Patchbeschreibung ist damit für die Portierung kein Grund, auf eine spätere Entwicklungsrevision auszuweichen.

## 4. Xtensa / Build-System

Der ESP32-S3 verwendet Xtensa LX7. Stockfish 19 kennt dafür keinen eigenen Architektur- bzw. SIMD-Buildpfad.

Wichtige Feststellungen:

- `src/nnue/simd.h` enthält einen generischen Fallback: Sind keine unterstützten SIMD-Makros definiert, wird `VECTOR` deaktiviert.
- Die NNUE-Implementierung besitzt damit einen skalaren Rechenpfad. Für den ersten Port muss keine neue SIMD-Implementierung erfunden werden.
- Eine Xtensa-spezifische Optimierung mit ESP32-S3-DSP-Instruktionen wäre ein späterer, separat zu benchmarkender Schritt.
- `ARCH=general-32` im Stockfish-Makefile ist nicht direkt als Xtensa-Ziel verwendbar: Der Makefile-Pfad setzt `bits=32` und erzeugt für GCC `-m32`. Das ist nicht der richtige Xtensa-Compiler-Modus.
- Daher sollte PlatformIO die Quellen mit dem ESP32-S3-Toolchain-Compiler bauen. Das Stockfish-Makefile wird zunächst nur als Referenz für Quellliste und Build-Defines verwendet.
- UCI/Engine nutzt C++17, Streams und `std::filesystem`. Die Verfügbarkeit und das Laufzeitverhalten dieser Teile im konkreten Arduino-ESP32-/ESP-IDF-Toolchain-Setup müssen durch einen frühen Compile-/Link-Smoke-Test bestätigt werden.

Für den ersten Build ist die skalare NNUE-Variante ohne x86-, ARM- oder RISC-V-SIMD-Defines vorgesehen. Prefetch sollte zunächst deaktiviert werden, bis die Xtensa-Codegenerierung geprüft ist.

## 5. Speicheraufteilung und Allokation

### Transposition-Table

Stockfish 19 verwendet 32-Byte-Cluster. Die TT-Implementierung selbst kann zunächst unverändert bleiben:

- 1 MiB = 32.768 Cluster
- 2 MiB = 65.536 Cluster
- 4 MiB = 131.072 Cluster
- 7 MiB = 229.376 Cluster

`TranspositionTable::resize()` reserviert die Tabelle über `aligned_large_pages_alloc_with_hint()`. Der generische Nicht-Linux-Pfad in `memory.cpp` nutzt jedoch regulär ausgerichteten Heap-Speicher und garantiert keinen PSRAM.

**Vorgeschlagene Anpassung:** Eine ESP32-spezifische Implementierung dieses Allocator-Paars verwenden, die TT-Speicher explizit mit `heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` reserviert und mit dem passenden Free-Aufruf freigibt. Das TT-Layout, Probe-/Store-Protokoll und die Suchlogik bleiben dabei unangetastet.

Mit 1 MiB TT beginnen; danach 2 und 4 MiB vergleichen. 7 MiB ist zunächst kein sinnvoller Startwert.

### Worker und NNUE-Arbeitsspeicher

Ein wichtiger Speicherposten liegt in `Search::Worker`:

- `AccumulatorStack` speichert `MAX_PLY + 1 = 247` Zustände.
- Jeder Zustand enthält zwei 1024-elementige 16-Bit-Akkumulatoren sowie weitere Daten.
- Allein die Akkumulatoren belegen damit ungefähr 1 MiB pro Worker.
- Die Finny-/Refresh-Tabelle enthält 64 × 2 Einträge. Jeder Eintrag speichert erneut einen 1024-elementigen 16-Bit-Akkumulator. Das sind zusätzlich grob 270 KiB pro Worker, zuzüglich Alignment und Metadaten.

Stockfish legt den Worker über `make_unique_large_page<Search::Worker>()` an. Der konkrete Speicherbedarf sollte im ersten erfolgreichen Build per `sizeof(Search::Worker)` ausgegeben werden; die obigen Zahlen sind eine Größenabschätzung aus den Quellstrukturen.

Damit ist auch der Worker-Allocator relevant. Eine erste ESP-Allocator-Implementierung kann Worker und TT im PSRAM unterbringen. Das bedeutet allerdings, dass regelmäßig verwendete Such- und NNUE-Daten dann ebenfalls im langsameren PSRAM liegen. Später kann man die großen, selteneren Bereiche und die besonders heißen Daten getrennt platzieren. Eine solche Aufteilung sollte erst nach einer korrekten Referenzmessung erfolgen.

### Netzwerkobjekt

`Network` enthält den Feature-Transformer und die Netz-Layer als feste Member-Arrays. `Engine::get_default_network()` verwendet zunächst `std::make_unique<Network>()`. Bei der Smallnet-Variante ist das prinzipiell speicherbar, die Platzierung sollte aber ausdrücklich kontrolliert werden. Eine normale `new`-Allokation garantiert nicht automatisch PSRAM.

## 6. Threading und zwei Cores

Stockfish verwendet `pthread`, `std::mutex` und `std::condition_variable`. ESP-IDF 5.5 stellt eine POSIX-Pthread-Schicht bereit und bietet über `esp_pthread_set_cfg()` Konfiguration für Task-Stack, Priorität und Core-Affinity.

Ein unmittelbares Hindernis ist die Stockfish-Datei `thread_native.h`:

- Stockfish setzt bei Suchthreads `NativeThreadOptions{}.setLargeStack(true)`.
- `TH_STACK_SIZE` ist dort auf 8 MiB festgelegt.
- Das ist für den ESP32-S3 nicht realistisch und muss durch eine ESP-spezifische, gemessene Stackgröße ersetzt werden.

Keinen Stackwert ungeprüft übernehmen. Die tatsächliche Stacknutzung soll während tiefer Suchläufe über den FreeRTOS-High-Water-Mark gemessen werden. Stack-Reserve und Worst-Case-Tests sind erforderlich.

Weitere Punkte:

- Stockfish verwendet für nicht-Linux-/nicht-Windows-Systeme eine einfache NUMA-Konfiguration. Für den ESP32-S3 reicht zunächst ein NUMA-Knoten.
- Die maximal angebotene Threadzahl sollte auf 2 begrenzt werden. Stockfishs Standard-Obergrenze von 1024 ist für diese Plattform nicht sinnvoll.
- Zuerst mit einem Search-Thread starten. Danach 2 Threads mit kontrollierter Core-Affinity und identischer Suchstellung vergleichen.
- Die Display-/Touch-Aufgaben dürfen nicht unkontrolliert mit der Suche um dieselben Ressourcen konkurrieren. Core-Affinity und Task-Prioritäten erst nach dem Ein-Thread-Baseline-Test festlegen.

## 7. Empfohlene Portierungsreihenfolge

1. **Upstream-Basis pinnen:** Stockfish 19 exakt auf `edb0d9db6731067ec50ce619ff372b463bc4dd5d` festlegen und Lizenz-/Quellherkunft dokumentieren.
2. **Smallnet validieren:** Den Smallnet-Patch auf genau diese Basis prüfen, Hash und Dateigröße des Netzes kontrollieren und eine Desktop-Referenz erstellen.
3. **Build-Skelett:** Stockfish-Quellen in PlatformIO aufnehmen, zunächst nur skalaren Compile-/Link-Test durchführen. Noch keine UI-Integration.
4. **UCI-Smoke-Test:** `uci`, `isready`, `position`, `go depth`, `stop`, `bestmove` sowie `perft` testen.
5. **PSRAM-Allocator:** TT und Worker kontrolliert allokieren. Fehlerfälle und tatsächlich verwendete Heap-Capabilities protokollieren.
6. **Ein-Thread-Baseline:** Smallnet, 1 Thread und 1 MiB TT; NPS, Speichernutzung und Suchkorrektheit erfassen.
7. **Messreihe:** TT 1/2/4 MiB sowie ein und zwei Threads vergleichen. Erst danach Display und Touch im gleichzeitigen Betrieb zuschalten.
8. **Optimierung:** Erst mit reproduzierbaren NPS-Messungen entscheiden, ob Worker-Daten teilweise in internen RAM verschoben oder ein Xtensa-spezifischer NNUE-Rechenpfad entwickelt werden soll.

## 8. Aktueller Implementierungs- und Prüfstand

- Stockfish-19-Smallnet-Quellen sind inzwischen unter `src/stockfish/` integriert; der Compile-/Link-Test steht noch aus.
- Es wurde kein PlatformIO-Build für den Stockfish-Port ausgeführt.
- Die Smallnet-Variante wurde auf dem ESP32-S3 noch nicht kompiliert oder getestet.
- Die exakten `sizeof`-Werte von Network, Worker und Accumulator-Strukturen müssen im Zielbuild gemessen werden.
- Thread-Stack, UCI-Ein-/Ausgabe, `std::filesystem` und die PSRAM-Platzierung sind noch nicht auf dem Board verifiziert.

Diese Analyse ist eine Architektur- und Integrationsgrundlage. Der Compile-/Link-Smoke-Test ist angelegt, aber noch nicht erfolgreich bestätigt; eine lauffähige UCI-Portierung ist damit ausdrücklich noch nicht nachgewiesen.

## Quellen

- Stockfish Upstream: https://github.com/official-stockfish/Stockfish/tree/sf_19
- Stockfish-19 NNUE-Architektur: https://github.com/official-stockfish/Stockfish/tree/sf_19/src/nnue
- Stockfish-19 Smallnet-Patches: https://github.com/lichess-org/stockfish-web/tree/main/patches/sf_19_smallnet
- ESP-IDF 5.5 POSIX/Pthreads: https://docs.espressif.com/projects/esp-idf/en/release-v5.5/esp32/api-reference/system/pthread.html
- ESP-IDF externe RAM-Speicherverwaltung: https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32/api-reference/system/mem_alloc.html
- Lite-Netz-Größe / Stockfish-19-Lite-Referenz: https://mychesslab.uk/credits


## 9. Smallnet-Patch-Abgleich mit dem offiziellen SF19-Release

Die Smallnet-Patchserie basiert auf dem Upstream-Entwicklungsstand `3f6f417b87c0e80ee30914b6b539b4ab7d3b2a5b`, nicht direkt auf dem späteren Release-Commit `edb0d9db6731067ec50ce619ff372b463bc4dd5d`. Zwischen beiden liegen sechs Upstream-Commits.

Der Smallnet-Patch wurde quelltextbasiert auf den tatsächlichen SF19-Release-Stand vorwärtsportiert:
- Alle betroffenen Hunk-Blöcke für Makefile, NNUE-Code, Position und Search ließen sich anhand der SF19-Dateien übernehmen.
- Nur der Threat-Datenstrukturblock in `types.h` brauchte eine manuelle Auflösung. Entfernt wurden `DirtyThreat`, `DirtyThreats`, `DirtyPawnPairs` und `Dirties`; die übrigen SF19-Änderungen blieben erhalten.
- Der WebAssembly-Patch 0002 wird nicht übernommen.
- Ein lokales `git apply --check` und ein tatsächlicher Zielbuild sind noch nicht erfolgt.

## 10. Compile-/Link-Testziel

Die Quelle liegt jetzt unter `src/stockfish/`; Upstream-Lizenz und AUTHORS-Datei sind enthalten. `platformio.ini` hat zwei getrennte Umgebungen:
- `freenove_fnk0104b`: bestehende Board-Diagnose, Stockfish-Quellen explizit ausgeschlossen.
- `freenove_stockfish_compile`: SF19-Smallnet-Quellen, Xtensa-Skalarpfad, C++17, kein Prefetch, keine Syzygy-Tablebases und keine eingebettete NNUE-Datei.

Der Probe-Sketch initialisiert Attack-/Positionsdaten und gibt Objektgrößen aus. Er startet keine Suche, erzeugt keine Worker-Threads und lädt keine NNUE-Gewichte.

Die Firmware wurde hier noch nicht gebaut. Die Quellen sind integriert; die Compile-/Link-Verifikation bleibt der nächste Test.
