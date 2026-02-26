#include <ElegantOTA.h>
#include <M5Atom.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>

#include "HX711.h"
#include "config.h"
#include "diagnostic.h"
#include "logger.h"
#include "notifier.h"
#include "secrets.h"

// --- BALANCE ---
HX711 scale;

// --- SERVEUR WEB ---
WebServer server(80);

// --- ÉTAT SYSTÈME ---
float poidsEntree = 0;
bool occupe = false;
bool exitPending = false;
bool otaInProgress = false;
unsigned long tempsEntree = 0;
unsigned long exitDetectedAt = 0;
unsigned long dernierCheckWifi = 0;

// --- SUIVI DERNIÈRE ACTIVITÉ ---
unsigned long sullyDernierPipi = 0;
unsigned long sullyDernierCaca = 0;
unsigned long krokmouDernierPipi = 0;
unsigned long krokmouDernierCaca = 0;

// --- LOGS ---
String logBuffer = "";
int logLineCount = 0;

// ---------------------------------------------------------------------------
// FORWARD DECLARATIONS
// ---------------------------------------------------------------------------
void detecterEntree();
void traiterSortieChat();
void detecterNettoyage();

Preferences prefs;

// ---------------------------------------------------------------------------
// SETUP
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    btStop();
    M5.begin(false, false, true);

    // WiFi
    M5.dis.fillpix(LED_BLEU);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(ssid, password);
    addLog("Connexion Wi-Fi...");
    while (WiFi.status() != WL_CONNECTED) delay(500);
    addLog("Wi-Fi Connecté ! IP : " + WiFi.localIP().toString());

    prefs.begin("litiere", false);
    sullyDernierPipi = prefs.getULong("s_pipi", 0);
    sullyDernierCaca = prefs.getULong("s_caca", 0);
    krokmouDernierPipi = prefs.getULong("k_pipi", 0);
    krokmouDernierCaca = prefs.getULong("k_caca", 0);
    addLog("NVS chargé — s_pipi=" + String(sullyDernierPipi) +
           " s_caca=" + String(sullyDernierCaca) + " k_pipi=" + String(krokmouDernierPipi) +
           " k_caca=" + String(krokmouDernierCaca));

    // Watchdog
    esp_task_wdt_init(WDT_TIMEOUT_S, true);
    esp_task_wdt_add(NULL);

    // Routes web
    server.on("/", []() {
        String html = "<html><head><meta charset='UTF-8'></head><body>";
        html += "<h1>🐱 Litière</h1>";
        html += "<p>État : " + String(occupe ? "🔴 Occupée" : "🟢 Libre") + "</p>";
        html += "<p>Dernier pipi Sully : " + String((millis() - sullyDernierPipi) / 3600000) +
                "h ago</p>";
        html += "<p>Dernier pipi Krokmou : " + String((millis() - krokmouDernierPipi) / 3600000) +
                "h ago</p>";
        html += "<a href='/logs'>Logs</a> | <a href='/tare'>Tare</a> | <a href='/update'>OTA</a>";
        html += "</body></html>";
        server.send(200, "text/html", html);
    });

    server.on("/status", []() {
        String json = "{";
        json += "\"occupe\":" + String(occupe ? "true" : "false") + ",";
        json += "\"uptime\":" + String(millis() / 1000) + ",";
        json += "\"sully_dernier_pipi\":" + String(sullyDernierPipi) + ",";
        json += "\"krokmou_dernier_pipi\":" + String(krokmouDernierPipi);
        json += "}";
        server.send(200, "application/json", json);
    });

    server.on("/tare", []() {
        M5.dis.fillpix(LED_BLEU);
        scale.tare();
        delay(500);
        M5.dis.fillpix(LED_VERT);
        verifierConnexion();
        envoyerNotification("Système", "Tare distante faite!", 0, 0, 0, "");
        server.send(200, "text/plain", "Tare effectuée !");
    });

    server.on("/logs", []() {
        String html = "<html><head>";
        html += "<meta charset='UTF-8'>";
        html += "<meta http-equiv='refresh' content='10'>";
        html += "<style>body{font-family:monospace;background:#111;color:#0f0;padding:20px;}";
        html += "h2{color:#fff;}</style></head><body>";
        html += "<h2>🐱 Litière — Logs</h2>";
        html += logBuffer.length() > 0 ? logBuffer : "Aucun log pour le moment.";
        html += "</body></html>";
        server.send(200, "text/html", html);
    });

    // OTA
    ElegantOTA.begin(&server);
    ElegantOTA.setAutoReboot(true);
    ElegantOTA.onStart([]() { otaInProgress = true; });
    ElegantOTA.onEnd([](bool success) { otaInProgress = false; });
    server.begin();
    addLog("Serveur démarré.");

    // Balance
    M5.dis.fillpix(LED_ROUGE);
    scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);
    scale.set_scale(CALIBRATION_SCALE);
    M5.dis.fillpix(LED_BLEU);
    delay(2000);
    scale.tare();

    M5.dis.fillpix(LED_VERT);
    addLog("Système prêt et calibré.");

    // Notification démarrage
    verifierConnexion();
    envoyerNotification("Système", "Litière connectée et prête !", 0, 0, 0, "");

    // Valider le firmware (rollback protection)
    esp_ota_mark_app_valid_cancel_rollback();
    addLog("Firmware marqué comme valide ✅");
}

// ---------------------------------------------------------------------------
// LOOP
// ---------------------------------------------------------------------------
void loop() {
    esp_task_wdt_reset();
    server.handleClient();
    ElegantOTA.loop();
    M5.update();

    float weight = scale.get_units(5) / 1000.0;
    static unsigned long dernierCheckSante = 0;

    // Vérification WiFi périodique
    if (millis() - dernierCheckWifi > WIFI_CHECK_INTERVAL && !otaInProgress) {
        dernierCheckWifi = millis();
        verifierConnexion();
    }

    // 1. Détection entrée
    if (weight > SEUIL_ENTREE_KG && !occupe && !exitPending) {
        detecterEntree();
    }

    // 2. Anti-blocage 10 min
    if (occupe && (millis() - tempsEntree > DUREE_SESSION_MAX_MS)) {
        addLog("⚠️ Session +10 min détectée. Reset forcé.");
        verifierConnexion();
        envoyerNotification("Système", "*Alerte Sécurité*", 0, poidsEntree, 0,
                            "Session de +10 min détectée. Reset automatique.");
        scale.tare();
        occupe = false;
        exitPending = false;
        M5.dis.fillpix(LED_VERT);
    }

    // 3. Détection sortie — phase 1
    if (weight < SEUIL_SORTIE_KG && occupe && !exitPending) {
        exitPending = true;
        exitDetectedAt = millis();
    }

    // 3. Détection sortie — phase 2
    if (exitPending && (millis() - exitDetectedAt >= EXIT_STABILISATION_MS)) {
        exitPending = false;
        occupe = false;
        traiterSortieChat();
    }

    // 4. Détection nettoyage
    if (weight < SEUIL_NETTOYAGE_KG && !occupe && !exitPending) {
        detecterNettoyage();
    }

    // 5. Tare manuelle
    if (M5.Btn.wasPressed()) {
        M5.dis.fillpix(LED_BLEU);
        scale.tare();
        delay(500);
        M5.dis.fillpix(LED_VERT);
        addLog(">>> TARE MANUELLE");
        verifierConnexion();
        envoyerNotification("Système", "Tare manuelle faite!", 0, 0, 0, "");
    }

    // 6. Alertes santé
    if (millis() - dernierCheckSante > SANTE_CHECK_INTERVAL) {
        dernierCheckSante = millis();
        verifierAlertesSante();
    }

    if (!otaInProgress) delay(200);
}

// ---------------------------------------------------------------------------
// FONCTIONS
// ---------------------------------------------------------------------------

void detecterEntree() {
    addLog("Mouvement détecté, vérification stabilité...");
    delay(1500);

    float weightStable = 0;
    for (int i = 0; i < 5; i++) {
        weightStable += scale.get_units(10) / 1000.0;
        delay(300);
    }
    weightStable /= 5;
    addLog("Poids stabilisé : " + String(weightStable, 2) + "kg");

    if (weightStable > SEUIL_ENTREE_KG) {
        occupe = true;
        exitPending = false;
        poidsEntree = weightStable;
        tempsEntree = millis();
        M5.dis.fillpix(LED_ROUGE);
        addLog("--- CHAT ENTRÉ : " + String(poidsEntree, 2) + "kg ---");
    }
}

void traiterSortieChat() {
    // Vérifier que le chat est vraiment parti
    float weightCheck = scale.get_units(5) / 1000.0;
    if (weightCheck > SEUIL_SORTIE_KG) {
        occupe = true;
        M5.dis.fillpix(LED_ROUGE);
        addLog("Fausse sortie détectée, chat toujours présent.");
        return;
    }

    // Attendre stabilisation de la litière
    esp_task_wdt_reset();
    delay(3000);
    esp_task_wdt_reset();

    float poidsFinalGrames = scale.get_units(30);
    unsigned long dureeSession = (millis() - tempsEntree) / 1000;

    String nomChat = identifierChat(poidsEntree);
    setCouleurChat(nomChat);

    String diagnostic = "";
    String alerte = "";
    calculerDiagnostic(nomChat, poidsFinalGrames, dureeSession, diagnostic, alerte);

    if (dureeSession > DUREE_ALERTE_S && alerte == "") {
        alerte = "*Attention :* Session extrêmement longue (+4 min).";
    }

    addLog("Chat : " + nomChat + " | " + diagnostic + " | " + String(poidsFinalGrames, 1) + "g | " +
           String(dureeSession) + "s");

    verifierConnexion();
    envoyerNotification(nomChat, diagnostic, poidsFinalGrames, poidsEntree, dureeSession, alerte);
    envoyerDonneesSheets(nomChat, diagnostic, poidsFinalGrames, poidsEntree, dureeSession, alerte);

    // Reset
    esp_task_wdt_reset();
    delay(500);
    scale.tare();
    poidsEntree = 0;
    tempsEntree = 0;
    M5.dis.fillpix(LED_VERT);
    addLog("Balance remise à zéro.");
}

void detecterNettoyage() {
    addLog("Poids négatif détecté (Nettoyage ?). Attente stabilité...");
    delay(NETTOYAGE_ATTENTE_MS);

    float weightCheck = scale.get_units(10) / 1000.0;
    if (weightCheck < SEUIL_NETTOYAGE_KG) {
        M5.dis.fillpix(LED_CYAN);
        addLog(">>> AUTO-TARE (Nettoyage détecté)");
        scale.tare();
        verifierConnexion();
        envoyerNotification("Système", "Nettoyage détecté", 0, 0, 0,
                            "La litière a été remise à zéro automatiquement.");
        delay(1000);
        M5.dis.fillpix(LED_VERT);
    }
}