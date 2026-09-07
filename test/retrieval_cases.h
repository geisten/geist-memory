#ifndef RETRIEVAL_CASES_H
#define RETRIEVAL_CASES_H
#include <stddef.h>
/* Original project-authored fixture, Apache-2.0 (repository LICENSE).
 * Version 1: eight short documents, one German and one English query each.
 * Each query has exactly one manually assigned relevant document.
 * This tiny regression corpus is not a representative retrieval benchmark. */
static const char *const retrieval_docs[] = {
    "Yeast consumes sugar in bread dough and releases carbon dioxide. The gas expands "
    "the gluten network and makes the loaf rise. Cold dough ferments more slowly.",
    "Der Mond zieht mit seiner Schwerkraft das Meerwasser an. Zusammen mit der "
    "Erdrotation entstehen an vielen Küsten täglich zwei Hochwasser und zwei Niedrigwasser.",
    "A mortgage payment first covers interest on the outstanding loan balance. "
    "The remaining amount reduces the principal. Over time the interest portion falls.",
    "Beim Kompostieren zersetzen Mikroorganismen Pflanzenreste. Luft, ausreichend "
    "Feuchtigkeit und eine Mischung aus grünen und braunen Materialien helfen dabei.",
    "A bicycle's rear derailleur moves the chain between sprockets. "
    "Cable tension controls alignment. Poor adjustment can cause clicking and skipped gears.",
    "Polarlichter entstehen, wenn geladene Teilchen auf Gase der oberen Atmosphäre "
    "treffen. Das Erdmagnetfeld lenkt viele dieser Teilchen in Richtung der Pole.",
    "A password manager creates and stores different random passwords for each account. "
    "A strong master password protects the vault. Reusing passwords spreads a breach.",
    "Wärmepumpen übertragen Wärme aus Luft oder Boden in ein Gebäude. Ein Verdichter "
    "benötigt dafür Strom. Niedrige Vorlauftemperaturen verbessern meist die Effizienz.",
};
static const struct {
    const char *language, *text;
    size_t relevant;
} retrieval_queries[] = {
    {"de", "Warum geht mein Brotteig im kalten Zimmer kaum auf?", 0},
    {"en", "What produces the bubbles that lift a loaf?", 0},
    {"de", "Warum liegt der Strand morgens unter Wasser und später wieder frei?", 1},
    {"en", "What causes the sea level at a coast to rise and fall each day?", 1},
    {"de", "Welcher Teil meiner Kreditrate verringert die Restschuld?", 2},
    {"en", "Why does the interest share of a home loan instalment shrink over time?", 2},
    {"de", "Was brauchen Gartenabfälle, um sich im Kompost gut zu zersetzen?", 3},
    {"en", "How can I help microorganisms turn plant scraps into soil material?", 3},
    {"de", "Warum klickt meine Fahrradkette und springt beim Schalten?", 4},
    {"en", "Which adjustment aligns a bike chain with the rear sprockets?", 4},
    {"de", "Wie entstehen die farbigen Lichter am Himmel nahe den Polen?", 5},
    {"en", "Why do charged particles make the polar night sky glow?", 5},
    {"de", "Wie verhindere ich, dass ein gestohlenes Passwort mehrere Konten gefährdet?", 6},
    {"en", "What tool stores a unique random login secret for each website?", 6},
    {"de", "Wie kann eine Heizung Wärme aus kalter Außenluft gewinnen?", 7},
    {"en", "Why does a heat pump work better with cooler heating water?", 7},
};
#endif
