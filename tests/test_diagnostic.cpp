// tests/test_diagnostic.cpp
#include <cassert>
#include <iostream>
#include <string>
typedef std::string String;

unsigned long sullyDernierPipi = 0;
unsigned long sullyDernierCaca = 0;
unsigned long krokmouDernierPipi = 0;
unsigned long krokmouDernierCaca = 0;

// Mock millis()
unsigned long millis() { return 0; }

#include "../src/config.h"
#include "../src/diagnostic.h"

// --- identifierChat ---
void test_identifierChat() {
    assert(identifierChat(3.5) == "Sully");
    assert(identifierChat(7.5) == "Krokmou");
    assert(identifierChat(5.5) == "Inconnu");   // boundary
    assert(identifierChat(1.0) == "Inconnu");   // trop léger
    assert(identifierChat(10.0) == "Inconnu");  // trop lourd
    std::cout << "✅ identifierChat OK\n";
}

// --- Sully diagnostics ---
void test_diagnostic_sully_visite() {
    String d, a;
    calculerDiagnostic("Sully", 5.0, 30, d, a);
    assert(d == "Simple visite");
    assert(a == "");
    std::cout << "✅ Sully visite simple OK\n";
}

void test_diagnostic_sully_visite_longue() {
    String d, a;
    calculerDiagnostic("Sully", 5.0, 95, d, a);
    assert(d == "Simple visite");
    assert(a != "");  // doit avoir une alerte grattage
    std::cout << "✅ Sully visite longue OK\n";
}

void test_diagnostic_sully_pipi_normal() {
    String d, a;
    calculerDiagnostic("Sully", 20.0, 60, d, a);
    assert(d == "Pipi 🟡");
    assert(a == "");
    std::cout << "✅ Sully pipi normal OK\n";
}

void test_diagnostic_sully_petit_pipi() {
    String d, a;
    calculerDiagnostic("Sully", 20.0, 130, d, a);
    assert(d == "Petit Pipi 🟡");
    assert(a != "");  // doit avoir une alerte vigilance
    std::cout << "✅ Sully petit pipi OK\n";
}

void test_diagnostic_sully_caca() {
    String d, a;
    calculerDiagnostic("Sully", 40.0, 100, d, a);
    assert(d == "Caca 🟤");
    std::cout << "✅ Sully caca OK\n";
}

void test_diagnostic_sully_gros_pipi() {
    String d, a;
    calculerDiagnostic("Sully", 40.0, 60, d, a);
    assert(d == "Gros Pipi 🟡");
    std::cout << "✅ Sully gros pipi OK\n";
}

// --- Krokmou diagnostics ---
void test_diagnostic_krokmou_visite() {
    String d, a;
    calculerDiagnostic("Krokmou", 10.0, 30, d, a);
    assert(d == "Simple visite");
    std::cout << "✅ Krokmou visite OK\n";
}

void test_diagnostic_krokmou_pipi() {
    String d, a;
    calculerDiagnostic("Krokmou", 50.0, 60, d, a);
    assert(d == "Pipi 🟡");
    std::cout << "✅ Krokmou pipi OK\n";
}

void test_diagnostic_krokmou_caca() {
    String d, a;
    calculerDiagnostic("Krokmou", 80.0, 100, d, a);
    assert(d == "Caca 🟤");
    std::cout << "✅ Krokmou caca OK\n";
}

// --- Boundaries ---
void test_boundaries_sully() {
    String d, a;
    // Exactement à la limite visite/pipi
    calculerDiagnostic("Sully", SULLY_VISITE_MAX, 60, d, a);
    assert(d == "Pipi 🟡");  // >= SULLY_VISITE_MAX → pipi
    // Exactement à la limite pipi/caca
    calculerDiagnostic("Sully", SULLY_PIPI_MAX, 60, d, a);
    assert(d == "Gros Pipi 🟡");  // >= SULLY_PIPI_MAX → gros pipi
    std::cout << "✅ Sully boundaries OK\n";
}

void test_boundaries_krokmou() {
    String d, a;
    calculerDiagnostic("Krokmou", KROKMOU_VISITE_MAX, 60, d, a);
    assert(d == "Pipi 🟡");
    calculerDiagnostic("Krokmou", KROKMOU_PIPI_MAX, 60, d, a);
    assert(d == "Gros Pipi 🟡");
    std::cout << "✅ Krokmou boundaries OK\n";
}

int main() {
    test_identifierChat();
    test_diagnostic_sully_visite();
    test_diagnostic_sully_visite_longue();
    test_diagnostic_sully_pipi_normal();
    test_diagnostic_sully_petit_pipi();
    test_diagnostic_sully_caca();
    test_diagnostic_sully_gros_pipi();
    test_diagnostic_krokmou_visite();
    test_diagnostic_krokmou_pipi();
    test_diagnostic_krokmou_caca();
    test_boundaries_sully();
    test_boundaries_krokmou();
    std::cout << "\n✅ Tous les tests diagnostic passés !\n";
    return 0;
}