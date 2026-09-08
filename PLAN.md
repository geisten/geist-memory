# Plan: geist-memory als Vorzeigeprojekt für sichere C23-Bibliotheken

Stand: 2026-09-06. Der ursprüngliche Plan entstand nach dem Review von
Commit `1af7d07`; die Fortsetzung berücksichtigt die beabsichtigte Rücksetzung.
Die ursprüngliche Drei-Dateien-Struktur bleibt erhalten. Die folgende Übersicht
trennt implementierte Arbeit von noch ausstehender Abnahme. Die Abschnitte darunter
enthalten jetzt einzelne Checkboxen: `[x]` bedeutet implementiert und im angegebenen
Umfang geprüft; `[ ]` bedeutet offen. Die dokumentierte Abnahme umfasst jetzt macOS ARM64, echte Pi5-Hardware und
Ubuntu-/Alpine-ARM64-Container sowie das echte BitNet-Embedding-0.6B-Modell.
GitHub Actions hat alle acht Jobs für Linux x86-64/ARM64 (glibc und musl-static)
sowie macOS ARM64/x86-64 erfolgreich ausgeführt. Die dokumentierten Basisprofile
sind damit nativ geprüft; die übrigen Release-Gates bleiben ausdrücklich offen.

## Umsetzungsstand

| Etappe | Implementiert | Noch offen |
| --- | --- | --- |
| 0 | Make-Modi, gepinnte isolierte Engine, modellfreie Tests, Konfiguration | Keine offenen Punkte für die dokumentierten Basisprofile |
| 1 | Geprüfte Größen, Bounds, Hamming-Tails/Alignment, API-Lebensdauer, atomare Embedding-Vorbereitung | Keine bekannten reproduzierten Speicherfehler offen |
| 2 | Writer-Lock, Undo-Journal, wiederholbare Recovery, Inhaltsidentität, explizites LE-v2-Format mit SHA-256 und v1-Import | Abschließende Bewertung der dokumentierten Format-/Integritätsgrenzen |
| 3 | Referenztests, OOM/I/O/Crash-Tests einschließlich Recovery, Fuzzing, Analyse, CI-Definition | Fehlereinbringung in übrige Kernel- und Engine-Pfade |
| 4 | Plattformprofile, statische Archive, Linux-static-Profil, externer Consumer, Installation, lokales Paket | Keine offenen Punkte für die dokumentierten Basisprofile |
| 5 | Store-Budget, Statistiken, Kompaktierung, Scan-Benchmark, Modell-Messlauf und DE/EN-Testkorpus | Modell-/Pi-Messungen vorhanden; repräsentativer Qualitätsnachweis offen |
| 6 | API-/Format-/Build-Dokumentation, CLI, Beitragsregeln, Änderungsnotizen | Vollständiges Release-Gate bleibt offen |

Die lokalen Prüfergebnisse und ihre Grenzen stehen in
[docs/VALIDATION.md](docs/VALIDATION.md). Architekturabweichungen zum ursprünglichen
Plan sind in [docs/DECISIONS.md](docs/DECISIONS.md) festgehalten. Die v2-Serialisierung, vollständige Datenchecksummen und der explizite v1-Import
sind implementiert; unabhängige Byte-Fixtures bestehen lokal. Die bisherige
64-Bit-Modellkennung bleibt eine Kompatibilitätsprüfung, keine Authentifizierung.

## Ziel und Leitlinien

geist-memory soll eine kleine, verständliche und erweiterbare C23-Bibliothek
mit nachprüfbarer Speichersicherheit, verlässlicher Persistenz und reproduzierbaren
Builds auf mehreren Plattformen werden. GNU Make steuert Build, Tests,
Analyse, Benchmarks, Installation und Auslieferung. CI ruft dieselben Targets auf.

- Keine zusätzlichen externen Laufzeitbibliotheken im Basisprofil, soweit die
  Plattform und geistlib das erlauben. C-Laufzeit und notwendige Systembibliotheken
  werden ausdrücklich ausgewiesen. Optionale Beschleuniger sind Opt-in.
- geist-memory und geistlib werden als statische Archive gebaut und eingebunden.
  Vollständig statische Programme sind ein eigenes Linux-Auslieferungsprofil.
  Unter macOS ist das Ziel ein Programm ohne zusätzlich zu installierende
  Laufzeitbibliotheken; Systembibliotheken bleiben dynamisch eingebunden.
- C23 wird für konkrete Vorteile eingesetzt: geprüfte Integer-Arithmetik,
  klare Verträge, typisierte Konstanten und standardisierte Bitoperationen.
- Eigentum, Lebensdauern, Grenzen, Fehlerzustände und Dauerhaftigkeit sind Teil
  des öffentlichen Vertrags. Alle Eingaben aus Dateien werden validiert.
- Speicherbedarf, Geschwindigkeit und Retrieval-Qualität werden gemessen.
  Ein eigener Allocator, mmap, SIMD oder ANN werden nur bei belegtem Nutzen ergänzt.
- Testwerkzeuge und Build-Abhängigkeiten werden getrennt von Laufzeitabhängigkeiten
  dokumentiert. Normale Builds und modellunabhängige Tests benötigen kein Python,
  keinen Paketdienst und keinen Netzwerkzugriff.

## Plattformumfang

| Profil | Ziel | Abnahme |
| --- | --- | --- |
| Linux x86-64 | Generisches dokumentiertes CPU-Minimum; GCC und Clang | Native Tests, Sanitizer, Consumer-Linktest |
| Linux ARM64 | Generisches ARM64-Profil, unabhängig vom Pi | Native Tests, Sanitizer soweit unterstützt, Consumer-Linktest |
| Raspberry Pi 5 | Explizites optimiertes ARM64-Profil | Tests und Speicher-/Latenzmessungen auf echter Hardware |
| macOS ARM64 | Apple Silicon mit unterstütztem Apple Clang | Native Tests, Sanitizer, Prüfung dynamischer Abhängigkeiten |
| macOS x86-64 | Intel mit C23-Clang | Native Tests, Sanitizer, Consumer-Linktest, reproduzierbare Pakete |
| Linux musl static | x86-64 und ARM64, soweit Engine und Toolchain unterstützt | Statisches Testprogramm ohne ELF-Interpreter oder dynamische Bibliotheksabhängigkeiten; Lauf in minimaler Umgebung |

Windows bleibt ein nachgelagertes Erweiterungsziel und benötigt eigene Engine-,
Toolchain-, Dateisystem- und Laufzeittests.
Plattformportabilität bedeutet zunächst den obigen Umfang, keine Zusage für
beliebige ISO-C- oder Embedded-Umgebungen.

Automatische Erkennung wählt ausschließlich ein konservatives Host-Profil.
ARM64 wird nicht automatisch als Pi 5 behandelt. Installiertes OpenMP oder BLAS
ändert das Basisprofil nicht. Beim Cross-Compiling bestimmt die explizite
Zielkonfiguration die Architektur, nicht `uname` auf dem Build-Host.

## Reihenfolge und Abhängigkeiten

0. Minimale verlässliche Make-/Test-Grundlage schaffen.
1. Speicherfehler, Eingabeprüfungen und API-Verträge korrigieren.
2. Atomare Änderungen, Dateiformat und Wiederherstellung implementieren.
3. Fehlerfalltests, Fuzzing und plattformübergreifende Prüfungen vervollständigen.
4. Plattformprofile, statische Auslieferung und Bibliotheksintegration abschließen.
5. Speicherverbrauch, Kompaktierung und Retrieval-Qualität messen und verbessern.
6. Dokumentation und Release-Abnahme abschließen.

Tests für jede Korrektur entstehen bereits in ihrer jeweiligen Etappe.
Etappe 3 erweitert diese Tests systematisch. Jede Etappe endet mit einem
prüfbaren Zwischenstand; die jeweils nächste baut auf dessen Garantien auf.

## 0 — Verlässliche Make-/Test-Grundlage

### Arbeit

- [x] Einen bekannten geistlib-Commit festhalten. `make deps` holt ihn per
  `tools/fetch-dep.sh` nach `build/deps/geistlib`; `GEIST_REPO=/pfad` klont offline
  aus einem lokalen Repo. Der Pin steht nur in `mk/config.mk`, kein anderes Ziel
  holt nach. Ein ungepinntes Nachbarverzeichnis ist keine Release-Abhängigkeit.
- [x] `tools/fetch-dep.sh` ist byte-identisch mit der Referenz in geistlib;
  `check-deps` vergleicht nach `make deps` per `cmp`.
- [x] `make test-unit` ohne Modell und ohne Engine-Laufzeit einführen: Store-Tests
  sowie deterministische Test-Doubles an einer schmalen Embedding-Schnittstelle.
- [x] `MODE=release`, `debug` und `asan` tatsächlich auf unterschiedliche Compile-
  und Linkflags abbilden. `asan` aktiviert AddressSanitizer und UBSan in allen
  getesteten eigenen Übersetzungseinheiten; Engine-E2E-Tests verwenden einen
  passenden Engine-Build. Nicht unterstützte Kombinationen scheitern verständlich.
- [x] Die reproduzierten Review-Fälle als gezielte Regressionstests übernehmen.
- [x] `make help` und `make print-config` ergänzen. Compiler, Ziel, Modus,
  Engine-Revision, Backend, GEMM-Provider und Linkprofil sichtbar machen.
- [x] Einen kurzen Architekturentscheid festhalten: öffentlicher API-Vertrag,
  unterstützte Toolchains, Dateiformatwechsel und Persistenzgarantien.

### Fertig, wenn

- [x] Ein frischer Checkout mit bereitgestellter, gepinnter Engine lässt sich bauen.
- [x] Modellunabhängige Tests laufen ohne Downloads; Fehler liefern einen Fehlercode.
- [x] Sanitizer sind nachweislich in Compile- und Linkbefehlen aktiv.
- [x] Temporäre Stores sind pro Test eindeutig; `make -j` erzeugt keine Kollisionen.

## 1 — Speicher- und API-Sicherheit

### Arbeit

- [x] Größenprodukte, Additionen und Kapazitätsverdopplung mit `ckd_mul`/`ckd_add`
  absichern. Grenzen für Dokumente, Chunks, Dimensionen und Generationen definieren;
  Narrowing zu 32 Bit und Datei-Offsets explizit prüfen.
- [x] Struktur der Store-Daten prüfen: exakte bzw. vertraglich erlaubte Dateilängen,
  Zähler, Strides, Dokumentreferenzen, String-Terminierung und Beziehungen zwischen
  Dateien. Fehlende oder leere Dateien eines bestehenden Stores sind kein neuer Store.
- [x] Hamming-Vergleich für alle akzeptierten Dimensionen korrigieren. Wortzugriffe
  ohne Alignment-/Aliasing-Annahmen über `memcpy`; Restbytes mitzählen.
- [x] Zeigervertrag vor einer stabilen API korrigieren: `gm_doc_path()` liefert eine
  geliehene Ansicht bis zum nächsten mutierenden Aufruf oder Schließen.
  Eine Kopierfunktion mit Aufruferpuffer bietet unabhängig gültige Pfade.
  Diese Änderung gegenüber dem bisherigen Vertrag wird ausdrücklich dokumentiert.
- [x] Öffentliche Pufferparameter mit Kapazitäten und überprüfbaren Vorbedingungen
  versehen. Interne `[static n]`-Verträge nur verwenden, wenn sie wirklich gelten.
  Den öffentlichen Header gleichzeitig in strengem C23 und C++ kompilieren.
- [x] Query und Präfix niemals still kürzen. Größenlimits veröffentlichen und bei
  Überschreitung eindeutige Fehler liefern. Text mit expliziter Länge ermöglichen;
  eingebettete NUL-Bytes bewusst ablehnen oder mit klarer Semantik unterstützen.
- [x] Leere Inhalte als Ersetzung ohne Live-Chunks behandeln. Datei-Lesefehler,
  Short Reads und Änderungen während des Lesens erkennen; Metadaten vom geöffneten
  Handle beziehen. Inhaltsbasierte Änderungserkennung für zuverlässige Idempotenz
  vorsehen, statt allein auf sekundengenaue Zeitstempel und Größe zu vertrauen.
- [x] Ausgaben auf allen Fehlerpfaden definieren. Speicher- und Engine-Fehler soweit
  die Engine-API es ermöglicht unterscheiden; fehlende Engine-Diagnostik benennen.
- [x] Allokationen hinter wenigen überprüften internen Hilfsfunktionen bündeln;
  Allokationsfehler für Tests injizierbar machen. Keine allgemeine Allocator-API
  ohne Anwendungsfall hinzufügen.

### Fertig, wenn

- [x] Die nachgewiesenen Use-after-free-, Out-of-bounds- und Alignment-Fälle bestehen
  als Regressionstests unter ASan/UBSan.
- [x] Strukturell beschädigte Stores und ungültige Journalchecksummen werden
  abgelehnt. v2 prüft zusätzlich alle Nutzdatensätze einschließlich veralteter Vektoren
  mit SHA-256; unbemerkte bösartige Neuberechnung von Prüfsummen ist kein Schutzziel.
- [x] Überlange, leere und ungültige Eingaben haben dokumentierte, getestete Ergebnisse.
- [x] Die geliehene Pfadansicht hat eine ausdrücklich begrenzte Lebensdauer;
  der Kopierzugriff bleibt unabhängig von Array-Verschiebungen gültig.

## 2 — Atomare Änderungen und verlässliche Persistenz

### Arbeit

- [x] Eine Dokumentänderung als Transaktion behandeln: neue Chunks zunächst vollständig
  vorbereiten; erst ein Commit macht sie sichtbar und verdrängt alte Chunks.
  OOM-/Engine-Fehler vor dem Schreiben erhalten alte Inhalte. Unklare
  Schreibausgänge werden als GM_E_UNCERTAIN gemeldet und beim Öffnen aufgelöst.
- [x] Nach der beabsichtigten Rücksetzung bleibt die Drei-Dateien-Struktur erhalten.
  Ein kleines Undo-Journal koordiniert ihre Änderungen; Kompaktierung nutzt
  zusätzliche Hardlink-Sicherungen. Diese Entscheidung ersetzt den ursprünglich
  bevorzugten Einzeldatei-Log-Entwurf.
- [x] Layout und Recovery-Regeln in FORMAT.md dokumentieren. Explizite Little-Endian-
  Kodierung ersetzt persistierte C-Structs; interne Datensätze enthalten nur genutzte
  Felder. Das Dateiformat hängt nicht mehr von Alignment, Padding oder Host-ABI ab.
- [x] Header und sämtliche Datensätze durch SHA-256 prüfen; Datensatzprüfsummen an
  Dateityp, Dimension, Modellkennung und physische Position binden. Recovery validiert
  erhaltene Datensätze vor dem ersten Überschreiben.
- [x] ABI-unabhängiges v2-Layout mit vollständigen Datenchecksummen und explizitem
  Import umsetzen. Die Drei-Dateien-Struktur bleibt erhalten; v1 wird nur aus
  unveränderten Quelldateien in ein zuvor nicht existentes Ziel importiert.
- [x] Erfolgreicher Standard-Commit bedeutet: Daten wurden über die unterstützte
  Plattform-Synchronisation dauerhaft angefordert. Grenzen dieser Zusage, etwa
  Verhalten von Hardware oder Netzwerkdateisystemen, konkret dokumentieren.
  Fehler am Commit-Punkt können einen unklaren Ausgang haben: diesen Zustand
  explizit melden und Wiederöffnen/Recovery verlangen, statt Rollback zu behaupten.
- [x] Exaktes I/O und Sync-/Austausch-Helfer im Plattformmodul bündeln;
  Descriptor-Ownership und Writer-Lock bleiben direkt im Store. Einen zweiten
  Writer eindeutig abweisen; Thread-Sicherheit bleibt explizit.
- [x] Modellidentität über einen Digest des Modellinhalts binden. Auch die relevante
  Embedding-/Preprocessing-Konfiguration versionieren. Die Kennung behält 64 Bit;
  Zeitstempel sind kein Ersatz für Inhaltsidentität. Die Kostenmessung steht in Etappe 5.
- [x] Umgang mit alten Identitäten festlegen: keine stille In-place-Migration
  oder Löschung. Re-Indexierung in ein neues Verzeichnis benötigt Originaltexte.
  Der v1-Importer erhält die bisherige Modellkennung, IDs, Generationen und Tie-Reihenfolge;
  er repariert keine historischen Modellidentitäten und lehnt offene v1-Journale ab.

### Fertig, wenn

- [x] Abbruch vor dem Commit lässt alte Inhalte bestehen; nach erfolgreichem Commit
  ist der neue Stand nach Wiederöffnen vollständig vorhanden.
- [x] Fehler an jedem Schreib-/Sync-Schritt liefern den spezifizierten Zustand.
- [x] Recovery ist wiederholbar und erzeugt weder doppelte noch falsch zugeordnete Chunks.
- [x] Unabhängig erzeugte v2-Byte-Fixture, jede einzelne beschädigte Dateiposition,
  vertauschte Datensätze und 64-KiB-Batchgrenzen lokal unter GCC/Clang/Sanitizern prüfen.
- [x] Import mit OOM, partiellen I/O-Fehlern und Prozessabbrüchen prüfen; Quelldateien
  bleiben bytegleich, Recovery liefert ein leeres oder vollständig importiertes Ziel.
- [x] Dieselben Format-Fixtures auf allen dokumentierten nativen Basisplattformen
  abnehmen: Linux/macOS x86-64 und ARM64, Pi5 sowie Linux musl-static.
- [x] Ein zweiter Writer wird zuverlässig abgewiesen.

## 3 — Systematische Qualitätsabsicherung

### Arbeit

- [x] Bekannte Sign-Packing-Vektoren und unabhängige Referenz für Hamming-Distanz und
  Top-k verwenden. Zufallstests mit reproduzierbarem Seed, Grenzdimensionen,
  Gleichständen, leeren Stores und großem k ergänzen.
- [x] Store-Lader und Recovery mit einem speicherbegrenzten Fuzz-Harness prüfen.
  Gültige und beschädigte Format-Fixtures als Startkorpus pflegen.
- [x] OOM bei Öffnen, Laden, API-Indexierung, gemeinsamem Array-Wachstum und
  Kompaktierung sowie Short I/O, Sync-Fehler und Prozessabbrüche testen.
  Grenzen der Simulation gegenüber echten Stromausfällen dokumentieren.
- [x] Reale ENOSPC-Dateisystemtests auf separatem HFS+-Abbild und Linux-tmpfs:
  Anlage, Journal, partielle Appends, Kompaktierung, Recovery und Wiederholen.
- [x] Echte Tokenizer-Allokationen in GPT-2/Qwen2/SPM/Unigram injizieren: 40 Fehler,
  vollständiges Cleanup und identische Wiederholungen. Stilles Weglassen von Text
  bei Scratch-OOM im versionierten Engine-Patch korrigieren.
- [ ] Fehler an den übrigen Kerneloperationen und sämtliche Startup-Allokationen
  einschließlich Kombinationen und weiterer Modellfamilien injizieren.
- [x] Echte Modelltests separat halten. Die bisherige Re-Indexierungsprüfung ersetzen:
  alte Chunk-Generationen müssen tatsächlich ausgeschlossen werden.
- [x] `make test` führt modellunabhängige Core-/Store-/Format-/Import-/Auswertungstests aus; `make check` ergänzt Header-Prüfungen.
  `make test-e2e` verlangt explizit ein Modell. Fehlende Voraussetzungen sind in
  Pflichtjobs Fehler; optionale Jobs melden einen sichtbaren Skip.
- [x] GCC-/Clang-Warnprüfungen, statische Analyse, Header-/Consumer-Tests und CI-Jobs
  aufbauen. Tests müssen auch mit Optimierung und aktivierten Release-Annahmen gelten.
- [x] Warnungen für eigenen Code in CI als Fehler behandeln. C23-Bibliotheksfeatures
  durch Compile-/Linkproben prüfen; notwendige Fallbacks zentral kapseln.

### Fertig, wenn

- [x] Die nachgewiesenen Speicher-/API-Fehler besitzen Regressionstests;
  weitergehende Format- und Plattformziele stehen separat als offene Punkte.
- [x] Pflichtprüfungen können nicht durch fehlende Modelle oder pauschale Skips grün werden.
- [x] Die CI-Definition ruft dieselben Make-Targets wie die lokale Entwicklung auf.
  Die erweiterte Matrix besteht alle acht Jobs; Quellen und
  Einzelprüfungen sind in docs/VALIDATION.md mit dem getesteten Commit verknüpft.
- [x] Fuzz- und Fehlerfalltests laufen mit festgelegten Zeit-/Speicherbudgets.

## 4 — Plattform-Builds, statisches Linken und Integration

### Arbeit

- [x] Das Root-Makefile bleibt der Einstieg. Kleine `mk/`-Fragmente trennen Konfiguration,
  Toolchains, Modi, Plattformadapter, Abhängigkeiten, Tests und Installation.
  Engine-interne Makefiles werden nur über einen versionierten, geprüften Adapter
  oder eine exportierte Engine-Buildschnittstelle genutzt.
- [x] Zielplattform, Compiler, libc, Backend, GEMM-Provider und Threading getrennt
  konfigurieren. Das Basisprofil nutzt native GEMM und keine zusätzlichen
  OpenMP-/BLAS-Laufzeitbibliotheken. Dafür nötige Engine-Anpassungen sind eine
  ausdrückliche Voraussetzung; Änderungen an geist-memory allein genügen nicht.
- [x] Einstellungen ausdrücklich an den geistlib-Submake weitergeben. Engine und
  Bibliothek müssen dieselbe kompatible Konfiguration verwenden.
- [x] Build-Verzeichnisse und Konfigurationsstempel berücksichtigen alle ABI-/Codegen-
  relevanten Einstellungen sowie die Engine-Revision. Änderungen an Flags,
  Toolchain oder Profil dürfen keine inkompatiblen alten Objekte wiederverwenden.
- [x] `CC`, `AR`, `RANLIB`, `CPPFLAGS`, `CFLAGS`, `LDFLAGS`, `LDLIBS`, `PREFIX` und
  `DESTDIR` sinnvoll unterstützen; Projektpflichtflags getrennt von Benutzerflags
  führen. Parallelität und inkrementelle Builds prüfen.
- [x] `make install` installiert Header, statische Archive und Metadaten für korrektes
  statisches Linken einschließlich transitiver Abhängigkeiten. `pkg-config`-Dateien
  dürfen erzeugt werden; pkg-config ist keine Laufzeitabhängigkeit.
- [x] Ein externer Beispiel-Consumer baut ausschließlich gegen die installierten
  Dateien, ohne private Header oder Zugriff auf den Source-Checkout.
- [x] Ein `check-linkage`-Target untersucht echte Consumer-Binaries: Linux mittels
  ELF-Metadaten, macOS mittels Mach-O-Abhängigkeiten. Ein `.a` allein belegt keine
  vollständig statische Anwendung.
- [x] Kern und Engine-Adapter sind getrennte Archive. `libgeist_memory.a` enthält
  kein geistlib-Symbol und baut ohne Engine-Quelle; `libgeist_memory_geist.a`
  implementiert den öffentlichen Embedder-Vertrag `geist_memory_embedder.h`.
  `check-linkage` linkt den Kern mit einem Mock-Embedder ohne geistlib und prüft
  die undefinierten Symbole beider Archive.
- [x] Release-Artefakte mit Buildmanifest, Engine-Revision, Toolchain, Prüfsummen und
  Lizenzhinweisen erzeugen. Reproduzierbarkeit in zwei getrennten Buildpfaden
  nachweisen; verbleibende Unterschiede dokumentieren.

### Implementierte Make-Oberfläche

| Target | Bedeutung |
| --- | --- |
| `deps`, `check-deps` | Gepinnte geistlib holen bzw. Skript gegen die Referenzkopie prüfen |
| `all` / `lib`, `adapter`, `engine` | Kern-Archiv, geistlib-Embedder bzw. Engine-Archiv bauen |
| `help`, `print-config` | Bedienung und aufgelöste Konfiguration anzeigen |
| `test`, `test-unit`, `test-store` | Modellunabhängige Pflichtprüfungen |
| `test-enospc`, `prepare-model` | Echte Platzmangelfälle auf Testdateisystem bzw. SHA-gebundene Modellvorbereitung |
| `test-format`, `test-import`, `import-tool` | Byte-Format und Migration prüfen bzw. Import-CLI bauen |
| `test-e2e` | Tests mit explizit bereitgestelltem Modell |
| `check` | Tests und C-/C++-Header mit strengen Compiler-Warnungen |
| `analyze`, `format-check` | Zusätzliche Clang-Werkzeuge, getrennt vom Minimalbuild |
| `fuzz` | Fuzzing mit explizitem Zeit-/Speicherbudget |
| `check-linkage`, `check-install` | Abhängigkeiten und installierten Consumer prüfen |
| `bench`, `bench-model` | Store-Messung bzw. echter DE/EN-Modellvergleich mit Latenz/RSS |
| `test-quality`, `model-tools` | Modellfreie Auswertungstests bzw. Kompilieren der Modellwerkzeuge |
| `check-package`, `check-repro` | Paket-Fehlerfälle bzw. bytegleiche Pakete aus zwei frischen Builds |
| `install`, `dist`, `clean` | Installation, Auslieferung und profilbezogenes Aufräumen |

### Fertig, wenn

- [x] Jedes dokumentierte Basisprofil besteht seine native Abnahme;
  Cross-Compile allein zählt nicht. Reale Modelltests bleiben separat ausgewiesen.
- [x] Die Linux-musl-Consumer auf ARM64 und x86-64 laufen vollständig statisch;
  kein ELF-Interpreter und keine dynamischen Abhängigkeiten.
- [x] Pi5 mit GCC/Clang und generisches Linux ARM64 prüfen; Ubuntu ARM64 mit
  GCC/Clang/Sanitizern sowie Alpine musl-static bestehen die dokumentierten Tests.
- [x] Der macOS-Consumer benötigt im Basisprofil keine Homebrew-Laufzeitbibliotheken.
- [x] Ein Profilwechsel funktioniert ohne manuelles Bereinigen fremder Build-Artefakte.
- [x] Ein frisch installierter Consumer baut und läuft mit dokumentierten Befehlen.

## 5 — Speicherbudget, Kompaktierung und nachgewiesene Qualität

### Arbeit

- [x] Ein konfigurierbares Speicherbudget und überprüfte Reserve-Operationen ergänzen.
  Engine-Speicher, Store-Speicher und temporäre Arbeitspuffer getrennt ausweisen;
  kein Gesamtbudget versprechen, das die Engine nicht durchsetzen kann.
- [x] Datei-Inhalte nach validiertem Header möglichst direkt in ihre endgültigen
  Puffer dekodieren; dafür einen festen 64-KiB-Batchpuffer verwenden. Unnötige
  vollständige Doppelhaltung und Wachstumsspitzen vermeiden.
- [x] Live-/Obsolete-Chunk-Statistiken und explizite Kompaktierung ergänzen. Sie verwendet
  dieselben Commit-/Sync-/Recovery-Garantien und darf das letzte gültige Store-Abbild
  erst nach erfolgreicher Veröffentlichung des Ersatzes ablösen.
- [x] Store-Öffnen, Schreiben, Suche und Kompaktierung mit Dateigröße,
  Store-Kapazität, Prozess-RSS und p50/p95 messen (100.000 und 1 Mio. Chunks).
- [x] `make bench-model` implementieren: Modell-Digest, Hash-/Startzeit,
  Tokenzahlen, Embedding-Zeiten und Prozess-RSS; Testlauf mit Mock klar kennzeichnen.
- [x] Echten GGUF-Messlauf auf macOS und Pi5 ausführen: Hashing, Modellstart,
  Embedding-Latenz und Peak-RSS; Herkunft, Original- und vorbereiteten SHA festhalten.
- [x] Echte Indexierung/Re-Indexierung, Mehrfenster-Chunking, Kompaktierung und
  Wiederöffnen auf macOS und Pi5 prüfen.
- [x] Vollständigen Pi5-Modelltest mit ASan/UBSan und explizitem LeakSanitizer
  abnehmen; reproduziertes Engine-Leck (512 Bytes) im versionierten Patch beheben.
- [x] Allokationsspitzen der vollständigen Modellanwendung auf Pi5 messen und
  unnötige Modell-Kontextpuffer begrenzen: Peak -31,6 %, RSS -62,1 %; exakte
  Float-Embeddings auf Pi und macOS einschließlich voller Fenster unverändert.
- [x] 51 vollständige Modell-Fehlerfälle plus zwei Baselines prüfen: sämtliche
  gemessenen Remember-/Recall-/Compact-Allokationen, alle sechs Startup-
  Allokationen >=8 MiB und 24 weitere Startup-Stellen. Cleanup, Dateigleichheit,
  Wiederholung und persistierte Ergebnisse prüfen; Grenzen in MODEL_MEMORY.md.
- [ ] Alle 1065 Startup-Allokationen, größere/abweichende Anwendungspfade und
  kombinierte Fehler über die begrenzte Nightly-Stichprobe hinaus abnehmen.
- [x] Den reinen Suchscan getrennt von Tokenisierung und Modellinferenz messen.
  Die eigene allokationsfreie Suchschleife ist keine Zusage für den ganzen Recall-Aufruf.
- [x] Reproduzierbare Korpora mit kleinen Stores und beispielsweise 100.000 sowie
  einer Million Chunks nutzen, soweit das jeweilige Speicherbudget dies erlaubt.
- [x] Eigenen Apache-2.0-DE/EN-Testkorpus im Repository bereitstellen (8 Dokumente, 16 Fragen)
  und Float-Cosinus gegen Sign-Hamming vergleichen: Recall@1/3, MRR und Top-3-
  Übereinstimmung, insgesamt und pro Abfragesprache. Auswertung unabhängig testen.
- [x] Kleinen Korpus mit echtem Modell ausführen und Ergebnisse in MODEL_BENCHMARK.md
  veröffentlichen, einschließlich Präfix, Chunking, Backend und Modell-Hashes.
- [x] Größeren externen, SHA-fixierten SciFact-Teilbestand mit 256 Dokumenten und
  100 Fragen sowie vorab gesetzten Regressionsgrenzen messen: Pi Float/Binär
  Recall@3 94/92 %, macOS 95/95 %. Auswahl, Abschneiden und Grenzen dokumentieren.
- [x] Separaten Make-orchestrierten Modell-Nightly definieren: Qualität,
  Allokationspeak, Fehlereinbringung, exakter Embedding-Vergleich und Sanitizer;
  Reports als Artefakte behalten, Modelldownload vom normalen Build trennen.
- [ ] Nightly nach Übernahme des Workflows auf `main` aktivieren und ersten
  planmäßigen GitHub-Lauf abnehmen; der Abnahmebranch allein aktiviert ihn nicht.
- [ ] Repräsentative mehrsprachige und anwendungsspezifische Qualitätsabnahme
  abschließen. Der verkleinerte englische SciFact-Korpus ersetzt weder diese
  Abnahme noch die numerische Engine-Referenzprüfung gegen Microsoft.
- [x] Erst danach über SIMD, mmap, alternative Top-k-Verfahren oder ANN entscheiden.
  Jede Optimierung benötigt Referenzvergleich und Messung auf Zielhardware.

### Fertig, wenn

- [x] Pi-Messungen und native Abnahmen der dokumentierten Basisplattformen
  sind veröffentlicht. Die begrenzte Modell-/Backend-Abdeckung bleibt ausgewiesen.
- [x] Speichergrenzen und das Verhalten bei Erreichen der Grenzen sind getestet.
- [x] Wiederholtes Re-Indexieren lässt sich durch Kompaktierung kontrolliert bereinigen.
- [x] Qualitätsverluste der binären Quantisierung im kleinen gemessenen Korpus
  pro Sprache und Backend ausweisen; Verallgemeinerung bleibt offen.

## 6 — Dokumentation und Release-Abnahme

### Arbeit

- [x] README und API-Beispiele enthalten Fehlerprüfung, Cleanup und korrekte Pfade.
  Beispiele werden gebaut und getestet. Tilde-Expansion nicht implizit versprechen.
- [x] API-Verträge, Formatbeschreibung, Recovery-Verhalten, Plattformmatrix,
  Mindesttoolchains und Buildprofile dokumentieren.
- [x] Lokale Beitragsregeln für C23, Ownership, geprüfte Arithmetik und Tests hinzufügen;
  keine ausschließlich verlinkten Regeln eines veränderlichen Fremd-Repositories.
- [x] Stabilität und Versionierung der öffentlichen API festlegen. Änderungen an
  Lebensdauern oder Format werden in Release Notes ausdrücklich genannt.
- [x] Eine einfache nutzbare Remember-/Recall-CLI als Beispiel ergänzen, sofern sie
  für die Demonstration benötigt wird. Bibliothekslogik bleibt in der Bibliothek.
- [x] Abhängigkeiten und Lizenzen einschließlich transitiv eingebundenem Code prüfen.

### Release-Gate

Ein Release als Vorzeigeprojekt erfolgt erst, wenn:

- [x] Nachgewiesene ursprüngliche Speicherfehler lokal korrigiert und regressionsgetestet.
- [x] Transaktions-/Recovery-Fehlerfalltests bestehen lokal.
- [x] Dokumentierte Basisplattformmatrix einschließlich Consumer- und Linkprüfungen
  ist nativ grün; optionale Backends erfordern eine separate Modellabnahme.
- [x] Erste echte Modelltests und veröffentlichte Pi5-/Qualitätsmessungen liegen vor.
- [ ] Repräsentative Qualitätsabnahme und numerische Engine-Referenzprüfung abschließen.
- [x] Installation, Paketprüfung und kompilierte Beispiele funktionieren lokal.
- [ ] Abschließende Sicherheits-/Datenintegritätsabnahme einschließlich offener
  Formatgrenzen durchführen, bevor das Projekt als Vorzeige-Release bezeichnet wird.

## Bewusste spätere Erweiterungen

Mehrere gleichzeitige Writer, Hintergrunddienste, automatische Dateibeobachtung,
GPU-spezifische Beschleunigung, ein eigener allgemeiner Allocator und ANN gehören
nicht zum ersten Sicherheits- und Portabilitätsziel. Windows-Unterstützung erhält
eine eigene Etappe, sobald die Engine-Voraussetzungen und der Plattformadapter
verifiziert sind.
