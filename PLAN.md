# Plan: geist-memory als Vorzeigeprojekt für sichere C23-Bibliotheken

Stand: 2026-09-05. Ausgangspunkt: Review von Commit `1af7d07`.
Dieses Dokument beschreibt geplante Arbeit; die genannten Make-Targets und
Garantien sind noch nicht implementiert.

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
| Linux musl static | x86-64 und ARM64, soweit Engine und Toolchain unterstützt | Statisches Testprogramm ohne ELF-Interpreter oder dynamische Bibliotheksabhängigkeiten; Lauf in minimaler Umgebung |

Windows und macOS x86-64 sind nachgelagerte Erweiterungsziele. Sie gelten erst
nach Engine-, Toolchain-, Dateisystem- und Laufzeittests als unterstützt.
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

- Einen bekannten geistlib-Commit festhalten. `GEISTLIB=/pfad` bleibt als
  Entwicklungsoption erhalten; Abweichungen vom geprüften Commit sind sichtbar.
  Ein ungepinntes Nachbarverzeichnis ist keine Release-Abhängigkeit.
- `make test-unit` ohne Modell und ohne Engine-Laufzeit einführen: Store-Tests
  sowie deterministische Test-Doubles an einer schmalen Embedding-Schnittstelle.
- `MODE=release`, `debug` und `asan` tatsächlich auf unterschiedliche Compile-
  und Linkflags abbilden. `asan` aktiviert AddressSanitizer und UBSan in allen
  getesteten eigenen Übersetzungseinheiten; Engine-E2E-Tests verwenden einen
  passenden Engine-Build. Nicht unterstützte Kombinationen scheitern verständlich.
- Die reproduzierten Review-Fälle als gezielte Regressionstests übernehmen.
- `make help` und `make print-config` ergänzen. Compiler, Ziel, Modus,
  Engine-Revision, Backend, GEMM-Provider und Linkprofil sichtbar machen.
- Einen kurzen Architekturentscheid festhalten: öffentlicher API-Vertrag,
  unterstützte Toolchains, Dateiformatwechsel und Persistenzgarantien.

### Fertig, wenn

- Ein frischer Checkout mit bereitgestellter, gepinnter Engine lässt sich bauen.
- Modellunabhängige Tests laufen ohne Downloads; Fehler liefern einen Fehlercode.
- Sanitizer sind nachweislich in Compile- und Linkbefehlen aktiv.
- Temporäre Stores sind pro Test eindeutig; `make -j` erzeugt keine Kollisionen.

## 1 — Speicher- und API-Sicherheit

### Arbeit

- Größenprodukte, Additionen und Kapazitätsverdopplung mit `ckd_mul`/`ckd_add`
  absichern. Grenzen für Dokumente, Chunks, Dimensionen und Generationen definieren;
  Narrowing zu 32 Bit und Datei-Offsets explizit prüfen.
- Store-Daten vollständig prüfen: exakte bzw. vertraglich erlaubte Dateilängen,
  Zähler, Strides, Dokumentreferenzen, String-Terminierung und Beziehungen zwischen
  Dateien. Fehlende oder leere Dateien eines bestehenden Stores sind kein neuer Store.
- Hamming-Vergleich für alle akzeptierten Dimensionen korrigieren. Wortzugriffe
  ohne Alignment-/Aliasing-Annahmen über `memcpy`; Restbytes mitzählen.
- Zeigervertrag vor einer stabilen API korrigieren: `gm_doc_path()` liefert eine
  geliehene Ansicht bis zum nächsten mutierenden Aufruf oder Schließen.
  Eine Kopierfunktion mit Aufruferpuffer bietet unabhängig gültige Pfade.
  Diese Änderung gegenüber dem bisherigen Vertrag wird ausdrücklich dokumentiert.
- Öffentliche Pufferparameter mit Kapazitäten und überprüfbaren Vorbedingungen
  versehen. Interne `[static n]`-Verträge nur verwenden, wenn sie wirklich gelten.
  Den öffentlichen Header gleichzeitig in strengem C23 und C++ kompilieren.
- Query und Präfix niemals still kürzen. Größenlimits veröffentlichen und bei
  Überschreitung eindeutige Fehler liefern. Text mit expliziter Länge ermöglichen;
  eingebettete NUL-Bytes bewusst ablehnen oder mit klarer Semantik unterstützen.
- Leere Inhalte als Ersetzung ohne Live-Chunks behandeln. Datei-Lesefehler,
  Short Reads und Änderungen während des Lesens erkennen; Metadaten vom geöffneten
  Handle beziehen. Inhaltsbasierte Änderungserkennung für zuverlässige Idempotenz
  vorsehen, statt allein auf sekundengenaue Zeitstempel und Größe zu vertrauen.
- Ausgaben auf allen Fehlerpfaden definieren. Speicher- und Engine-Fehler soweit
  die Engine-API es ermöglicht unterscheiden; fehlende Engine-Diagnostik benennen.
- Allokationen hinter wenigen überprüften internen Hilfsfunktionen bündeln;
  Allokationsfehler für Tests injizierbar machen. Keine allgemeine Allocator-API
  ohne Anwendungsfall hinzufügen.

### Fertig, wenn

- Die nachgewiesenen Use-after-free-, Out-of-bounds- und Alignment-Fälle bestehen
  als Regressionstests unter ASan/UBSan.
- Beschädigte Stores werden vor ihrer Nutzung abgelehnt, ohne sie zu verändern.
- Überlange, leere und ungültige Eingaben haben dokumentierte, getestete Ergebnisse.
- Keine öffentliche Lebensdauerzusage hängt von einem verschiebbaren Array ab.

## 2 — Atomare Änderungen und verlässliche Persistenz

### Arbeit

- Eine Dokumentänderung als Transaktion behandeln: neue Chunks zunächst vollständig
  vorbereiten; erst ein Commit macht sie sichtbar und verdrängt alte Chunks.
  Vor dem Commit bleiben alte Inhalte bei OOM, Engine- und I/O-Fehlern erhalten.
- Als bevorzugtes Format für Version 2 ein einzelnes append-only Transaktionslog
  aus Dokumentmetadaten, Chunk-/Vektordaten und prüfbarem Commit-Abschluss entwerfen.
  Damit entfällt der atomare Gleichlauf dreier autoritativer Dateien. Feste
  Vektorbreiten und kompakte Arrays für die Suche bleiben erhalten.
- Vor Implementierung das Layout und die Recovery-Regeln schriftlich festlegen:
  Byte-Reihenfolge, Feldbreiten, maximale Längen, Versionskennung, Checksummen,
  Commit-Grenzen und Behandlung unvollständiger Enden. Keine rohen C-Structs
  einschließlich möglichem Padding als portables Dateiformat schreiben.
- Beim Öffnen nur gültige, vollständig committed Transaktionen übernehmen.
  Unvollständige Enden kontrolliert behandeln; Korruption innerhalb bestätigter
  Daten als Fehler melden. Checksummen ersetzen keine Bounds-Prüfungen.
- Erfolgreicher Standard-Commit bedeutet: Daten wurden über die unterstützte
  Plattform-Synchronisation dauerhaft angefordert. Grenzen dieser Zusage, etwa
  Verhalten von Hardware oder Netzwerkdateisystemen, konkret dokumentieren.
  Fehler am Commit-Punkt können einen unklaren Ausgang haben: diesen Zustand
  explizit melden und Wiederöffnen/Recovery verlangen, statt Rollback zu behaupten.
- Dateisystemoperationen für Öffnen, Lesen/Schreiben, Sync, Sperren und atomaren
  Austausch in einem kleinen Plattformmodul kapseln. Einen zweiten Writer
  pro Store mit einem klaren Fehler abweisen; Thread-Sicherheit bleibt explizit.
- Modellidentität über einen Digest des Modellinhalts binden. Auch die relevante
  Embedding-/Preprocessing-Konfiguration versionieren. Kosten beim Modellstart
  messen; Zeitstempel sind kein Ersatz für Inhaltsidentität.
- Umgang mit Version 1 festlegen: keine stille In-place-Migration. Ein expliziter
  Import in einen neuen Store muss gültige alte Daten erhalten und die schwache
  alte Modellidentifikation offenlegen. Originaldateien bleiben unangetastet.
  Re-Indexierung ist nur dann eine Alternative, wenn die Originaltexte vorliegen.

### Fertig, wenn

- Abbruch vor dem Commit lässt alte Inhalte bestehen; nach erfolgreichem Commit
  ist der neue Stand nach Wiederöffnen vollständig vorhanden.
- Fehler an jedem Schreib-/Sync-Schritt liefern den spezifizierten Zustand.
- Recovery ist wiederholbar und erzeugt weder doppelte noch falsch zugeordnete Chunks.
- Format-Fixtures sind auf den unterstützten Plattformen identisch interpretierbar.
- Ein zweiter Writer wird zuverlässig abgewiesen.

## 3 — Systematische Qualitätsabsicherung

### Arbeit

- Kleine unabhängige Referenzimplementierung für Sign-Packing, Hamming-Distanz und
  Top-k verwenden. Zufallstests mit reproduzierbarem Seed, Grenzdimensionen,
  Gleichständen, leeren Stores und großem k ergänzen.
- Store-Lader und Recovery mit einem speicherbegrenzten Fuzz-Harness prüfen.
  Gültige und beschädigte Format-Fixtures als Startkorpus pflegen.
- OOM an jeder eigenen Allokation sowie Short Writes, Sync-Fehler, volle Datenträger
  und Prozessabbrüche an Transaktionsgrenzen injizieren. Verbleibende Grenzen
  solcher Simulationen gegenüber echten Stromausfällen dokumentieren.
- Echte Modelltests separat halten. Die bisherige Re-Indexierungsprüfung ersetzen:
  alte Chunk-Generationen müssen tatsächlich ausgeschlossen werden.
- `make test` führt alle modellunabhängigen Pflichtprüfungen aus.
  `make test-e2e` verlangt explizit ein Modell. Fehlende Voraussetzungen sind in
  Pflichtjobs Fehler; optionale Jobs melden einen sichtbaren Skip.
- GCC-/Clang-Warnprüfungen, statische Analyse, Header-/Consumer-Tests und CI-Jobs
  aufbauen. Tests müssen auch mit Optimierung und aktivierten Release-Annahmen gelten.
- Warnungen für eigenen Code in CI als Fehler behandeln. C23-Bibliotheksfeatures
  durch Compile-/Linkproben prüfen; notwendige Fallbacks zentral kapseln.

### Fertig, wenn

- Jeder Review-Befund besitzt einen aussagekräftigen Regressionstest.
- Pflichtprüfungen können nicht durch fehlende Modelle oder pauschale Skips grün werden.
- CI führt die gleichen Make-Targets aus wie die lokale Entwicklung.
- Fuzz- und Fehlerfalltests laufen mit festgelegten Zeit-/Speicherbudgets.

## 4 — Plattform-Builds, statisches Linken und Integration

### Arbeit

- Das Root-Makefile bleibt der Einstieg. Kleine `mk/`-Fragmente trennen Konfiguration,
  Toolchains, Modi, Plattformadapter, Abhängigkeiten, Tests und Installation.
  Engine-interne Makefiles werden nur über einen versionierten, geprüften Adapter
  oder eine exportierte Engine-Buildschnittstelle genutzt.
- Zielplattform, Compiler, libc, Backend, GEMM-Provider und Threading getrennt
  konfigurieren. Das Basisprofil nutzt native GEMM und keine zusätzlichen
  OpenMP-/BLAS-Laufzeitbibliotheken. Dafür nötige Engine-Anpassungen sind eine
  ausdrückliche Voraussetzung; Änderungen an geist-memory allein genügen nicht.
- Einstellungen ausdrücklich an den geistlib-Submake weitergeben. Engine und
  Bibliothek müssen dieselbe kompatible Konfiguration verwenden.
- Build-Verzeichnisse und Konfigurationsstempel berücksichtigen alle ABI-/Codegen-
  relevanten Einstellungen sowie die Engine-Revision. Änderungen an Flags,
  Toolchain oder Profil dürfen keine inkompatiblen alten Objekte wiederverwenden.
- `CC`, `AR`, `RANLIB`, `CPPFLAGS`, `CFLAGS`, `LDFLAGS`, `LDLIBS`, `PREFIX` und
  `DESTDIR` sinnvoll unterstützen; Projektpflichtflags getrennt von Benutzerflags
  führen. Parallelität und inkrementelle Builds prüfen.
- `make install` installiert Header, statische Archive und Metadaten für korrektes
  statisches Linken einschließlich transitiver Abhängigkeiten. `pkg-config`-Dateien
  dürfen erzeugt werden; pkg-config ist keine Laufzeitabhängigkeit.
- Ein externer Beispiel-Consumer baut ausschließlich gegen die installierten
  Dateien, ohne private Header oder Zugriff auf den Source-Checkout.
- Ein `check-linkage`-Target untersucht echte Consumer-Binaries: Linux mittels
  ELF-Metadaten, macOS mittels Mach-O-Abhängigkeiten. Ein `.a` allein belegt keine
  vollständig statische Anwendung.
- Release-Artefakte mit Buildmanifest, Engine-Revision, Toolchain, Prüfsummen und
  Lizenzhinweisen erzeugen. Reproduzierbarkeit in zwei getrennten Buildpfaden
  nachweisen; verbleibende Unterschiede dokumentieren.

### Geplante Make-Oberfläche

| Target | Bedeutung |
| --- | --- |
| `all` / `lib` | Statische Bibliothek und nötige Engine-Artefakte bauen |
| `help`, `print-config` | Bedienung und aufgelöste Konfiguration anzeigen |
| `test`, `test-unit`, `test-store` | Modellunabhängige Pflichtprüfungen |
| `test-e2e` | Tests mit explizit bereitgestelltem Modell |
| `check` | Format, Warnungen, Analyse und passende Pflichtprüfungen orchestrieren |
| `fuzz` | Fuzzing mit explizitem Zeit-/Speicherbudget |
| `check-linkage`, `check-install` | Abhängigkeiten und installierten Consumer prüfen |
| `bench` | Reproduzierbare Speicher-, Such- und Modellmessungen |
| `install`, `dist`, `clean` | Installation, Auslieferung und profilbezogenes Aufräumen |

### Fertig, wenn

- Jedes zugesagte Profil besteht seine native Abnahme; Cross-Compile allein zählt nicht.
- Der Linux-static-Consumer benötigt keine dynamischen Bibliotheken.
- Der macOS-Consumer benötigt im Basisprofil keine Homebrew-Laufzeitbibliotheken.
- Ein Profilwechsel funktioniert ohne manuelles Bereinigen fremder Build-Artefakte.
- Ein frisch installierter Consumer baut und läuft mit dokumentierten Befehlen.

## 5 — Speicherbudget, Kompaktierung und nachgewiesene Qualität

### Arbeit

- Ein konfigurierbares Speicherbudget und überprüfte Reserve-Operationen ergänzen.
  Engine-Speicher, Store-Speicher und temporäre Arbeitspuffer getrennt ausweisen;
  kein Gesamtbudget versprechen, das die Engine nicht durchsetzen kann.
- Datei-Inhalte nach validiertem Header möglichst direkt in ihre endgültigen
  Puffer lesen. Unnötige Doppelhaltung und Wachstumsspitzen vermeiden.
- Live-/Dead-Chunk-Statistiken und explizite Kompaktierung ergänzen. Sie verwendet
  dieselben Commit-/Sync-/Recovery-Garantien und darf das letzte gültige Store-Abbild
  erst nach erfolgreicher Veröffentlichung des Ersatzes ablösen.
- Öffnen, Indexieren, Re-Indexieren, Suche, Sync und Kompaktierung messen:
  Peak-RSS, eigene Allokationen, Dateigröße, Durchsatz sowie p50/p95-Latenzen.
- Den reinen Suchscan getrennt von Tokenisierung und Modellinferenz messen.
  Die eigene allokationsfreie Suchschleife ist keine Zusage für den ganzen Recall-Aufruf.
- Reproduzierbare Korpora mit kleinen Stores und beispielsweise 100.000 sowie
  einer Million Chunks nutzen, soweit das jeweilige Speicherbudget dies erlaubt.
- Retrieval-Qualität auf einem veröffentlichten, lizenzgeeigneten deutsch-/
  englischsprachigen Testkorpus messen: binäre Vektoren gegenüber Float-Referenz,
  Recall@k und Rangqualität; Modell, Präfix und Chunking exakt dokumentieren.
- Erst danach über SIMD, mmap, alternative Top-k-Verfahren oder ANN entscheiden.
  Jede Optimierung benötigt Referenzvergleich und Messung auf Zielhardware.

### Fertig, wenn

- Pi- und Plattformversprechen sind durch veröffentlichte Messungen gedeckt.
- Speichergrenzen und das Verhalten bei Erreichen der Grenzen sind getestet.
- Wiederholtes Re-Indexieren lässt sich durch Kompaktierung kontrolliert bereinigen.
- Qualitätsverluste der binären Quantisierung sind nachvollziehbar ausgewiesen.

## 6 — Dokumentation und Release-Abnahme

### Arbeit

- README und API-Beispiele enthalten Fehlerprüfung, Cleanup und korrekte Pfade.
  Beispiele werden gebaut und getestet. Tilde-Expansion nicht implizit versprechen.
- API-Verträge, Formatbeschreibung, Recovery-Verhalten, Plattformmatrix,
  Mindesttoolchains und Buildprofile dokumentieren.
- Lokale Beitragsregeln für C23, Ownership, geprüfte Arithmetik und Tests hinzufügen;
  keine ausschließlich verlinkten Regeln eines veränderlichen Fremd-Repositories.
- Stabilität und Versionierung der öffentlichen API festlegen. Änderungen an
  Lebensdauern oder Format werden in Release Notes ausdrücklich genannt.
- Eine einfache nutzbare Remember-/Recall-CLI als Beispiel ergänzen, sofern sie
  für die Demonstration benötigt wird. Bibliothekslogik bleibt in der Bibliothek.
- Abhängigkeiten und Lizenzen einschließlich transitiv eingebundenem Code prüfen.

### Release-Gate

Ein Release als Vorzeigeprojekt erfolgt erst, wenn:

1. alle Sicherheitsbefunde aus dem Review geschlossen und regressionsgetestet sind;
2. Transaktions- und Recovery-Verträge ihre Fehlerfalltests bestehen;
3. die zugesagte Plattformmatrix einschließlich Consumer- und Linkprüfungen grün ist;
4. echte Modelltests und veröffentlichte Zielhardware-Messungen vorliegen;
5. Installation, Dokumentation und Beispiele aus einem frischen Checkout funktionieren;
6. kein bekanntes Speicher- oder Datenverlustproblem offen ist.

## Bewusste spätere Erweiterungen

Mehrere gleichzeitige Writer, Hintergrunddienste, automatische Dateibeobachtung,
GPU-spezifische Beschleunigung, ein eigener allgemeiner Allocator und ANN gehören
nicht zum ersten Sicherheits- und Portabilitätsziel. Windows-Unterstützung erhält
eine eigene Etappe, sobald die Engine-Voraussetzungen und der Plattformadapter
verifiziert sind.
