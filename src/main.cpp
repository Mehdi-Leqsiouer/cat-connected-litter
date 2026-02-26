#include <ElegantOTA.h>
#include <HTTPClient.h>
#include <M5Atom.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>

#include "HX711.h"
#include "secrets.h"

// --- CABLAGE HX711 ---
#define LOADCELL_DOUT_PIN 32
#define LOADCELL_SCK_PIN 26

HX711 scale;

// --- VARIABLES DE SUIVI ---
float poidsEntree = 0;
bool occupe = false;
unsigned long tempsEntree = 0;
bool otaInProgress = false;
unsigned long exitDetectedAt = 0;

bool exitPending = false;
// --- LOG ---
String logBuffer = "";
const int MAX_LOG_LINES = 50;
int logLineCount = 0;

// --- SUIVI DERNIÈRE ACTIVITÉ ---
unsigned long sullyDernierPipi = 0;
unsigned long sullyDernierCaca = 0;
unsigned long krokmouDernierPipi = 0;
unsigned long krokmouDernierCaca = 0;

unsigned long dernierCheckWifi = 0;

const unsigned long ALERTE_PIPI_MS = 24UL * 60 * 60 * 1000;  // 24h en ms
const unsigned long ALERTE_CACA_MS = 48UL * 60 * 60 * 1000;  // 48h en ms

WebServer server(80);

void verifierConnexion();
void envoyerNotification(String chat, String action, float poids, float poids_chat,
                         unsigned long duree, String alerte);
void addLog(String message);
void verifierAlertesSante();
void envoyerDonneesSheets(String chat, String action, float poids, float poids_chat,
                          unsigned long duree, String alerte);

void setup() {
    Serial.begin(115200);
    btStop();

    M5.begin(false, false, true);

    // 1. Connexion Wi-Fi
    M5.dis.fillpix(0x0000ff);  // Bleu pendant la connexion
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(ssid, password);
    addLog("Connexion Wi-Fi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        addLog(".");
    }
    addLog("\nWi-Fi Connecté !");
    esp_task_wdt_init(30, true);  // reboot if loop() hangs for 30 seconds
    esp_task_wdt_add(NULL);

    addLog("IP address: ");
    addLog(String(WiFi.localIP()));
    server.on("/", []() { server.send(200, "text/plain", "Hi! This is ElegantOTA Demo."); });
    server.on("/tare", []() {
        M5.dis.fillpix(0x0000ff);
        scale.tare();
        delay(500);
        M5.dis.fillpix(0x00ff00);
        verifierConnexion();
        envoyerNotification("Système", "Tare distante faite!", 0, 0, 0, "");
        server.send(200, "text/plain", "Tare effectuée !");
    });

    server.on("/logs", []() {
        String html = "<html><head>";
        html += "<meta charset='UTF-8'>";
        html += "<meta http-equiv='refresh' content='10'>";  // auto-refresh every 10s
        html += "<style>body{font-family:monospace;background:#111;color:#0f0;padding:20px;}";
        html += "h2{color:#fff;}</style></head><body>";
        html += "<h2>🐱 Litière — Logs</h2>";
        html += logBuffer.length() > 0 ? logBuffer : "Aucun log pour le moment.";
        html += "</body></html>";
        server.send(200, "text/html", html);
    });

    ElegantOTA.begin(&server);  // Start ElegantOTA
    ElegantOTA.setAutoReboot(true);
    ElegantOTA.onStart([]() { otaInProgress = true; });
    ElegantOTA.onEnd([](bool success) { otaInProgress = false; });

    server.begin();

    addLog("Server started");

    // 2. Initialisation Balance
    M5.dis.fillpix(0xff0000);  // Rouge pendant la tare
    scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);
    scale.set_scale(24.79f);  // Ta valeur de calibration finale

    // --- AMÉLIORATION : TARE SÉCURISÉE ---
    M5.dis.fillpix(0x0000ff);  // Bleu : Initialisation
    delay(2000);
    scale.tare();

    M5.dis.fillpix(0x00ff00);  // Vert : Prêt !
    addLog("Système de litière prêt et calibré.");
    verifierConnexion();
    envoyerNotification("Système", "Litière connectée et prête !", 0, 0, 0, "");

    esp_ota_mark_app_valid_cancel_rollback();
    addLog("Firmware marqué comme valide ✅");
}

void loop() {
    esp_task_wdt_reset();
    server.handleClient();
    ElegantOTA.loop();
    M5.update();

    // Mesure du poids (moyenne de 5)
    float weight = scale.get_units(5) / 1000.0;

    static unsigned long dernierCheck = 0;

    if (millis() - dernierCheckWifi > 30000 && !otaInProgress) {  // every 30 seconds
        dernierCheckWifi = millis();
        verifierConnexion();
    }

    // --- 1. DÉTECTION D'ENTRÉE ---
    if (weight > 2.0 && !occupe) {
        addLog("Mouvement détecté, vérification stabilité...");
        delay(1500);

        // Moyenne de 5 lectures espacées pour plus de précision
        float weightStable = 0;
        for (int i = 0; i < 5; i++) {
            weightStable += scale.get_units(10) / 1000.0;
            delay(300);
        }
        weightStable /= 5;

        addLog("Poids stabilisé : " + String(weightStable, 2) + "kg");

        if (weightStable > 2.0) {
            occupe = true;
            exitPending = false;
            poidsEntree = weightStable;
            tempsEntree = millis();
            M5.dis.fillpix(0xff0000);
            addLog("--- CHAT ENTRÉ : " + String(poidsEntree, 2) + "kg ---");
        }
    }

    // --- 2. SÉCURITÉ ANTI-BLOCAGE (10 MIN) ---
    if (occupe && (millis() - tempsEntree > 600000)) {
        verifierConnexion();
        envoyerNotification("Système", "*Alerte Sécurité*", 0, poidsEntree, 0,
                            "Session de +10 min détectée. Reset automatique de la balance.");
        scale.tare();
        occupe = false;
        exitPending = false;
        M5.dis.fillpix(0x00ff00);
    }

    // --- 3. DÉTECTION DE SORTIE - PHASE 1 : déclenchement ---
    if (weight < 1.0 && occupe && !exitPending) {
        exitPending = true;
        exitDetectedAt = millis();
        // don't do anything yet, just start the timer
    }

    // --- 3. DÉTECTION DE SORTIE PHASE 2 ---
    if (exitPending && (millis() - exitDetectedAt >= 5000)) {
        addLog("Chat sorti, analyse finale...");
        exitPending = false;
        occupe = false;

        // Check if cat actually left (not a false positive)
        float weightCheck = scale.get_units(5) / 1000.0;
        if (weightCheck > 1.0) {
            // Cat came back during the 5s wait, cancel
            occupe = true;
            M5.dis.fillpix(0xff0000);
            return;
        }

        delay(3000);
        esp_task_wdt_reset();
        float poidsFinalGrames = scale.get_units(30);
        unsigned long dureeSession = (millis() - tempsEntree) / 1000;

        // Identification du chat
        String nomChat = "Inconnu";
        if (poidsEntree > 2.5 && poidsEntree < 5.5) {
            nomChat = "Sully";
            M5.dis.fillpix(0x0000ff);
        } else if (poidsEntree > 5.5 && poidsEntree < 9.0) {
            nomChat = "Krokmou";
            M5.dis.fillpix(0xffa500);
        }

        String diagnostic = "";
        String alerte = "";

        if (nomChat == "Sully") {  // ~3.5kg
            if (poidsFinalGrames < 10.0) {
                diagnostic = "Simple visite";
                if (dureeSession > 90) alerte = "*Alerte :* Grattage long sans résultat.";
            } else if (poidsFinalGrames < 35.0) {
                diagnostic = dureeSession > 120 ? "Petit Pipi 🟡" : "Pipi 🟡";
                sullyDernierPipi = millis();
                if (dureeSession > 120) alerte = "*Vigilance :* Long pour un petit résultat.";
            } else {
                diagnostic = dureeSession > 90 ? "Caca 🟤" : "Gros Pipi 🟡";
                sullyDernierCaca = millis();
            }
        } else if (nomChat == "Krokmou") {  // ~7.5kg
            if (poidsFinalGrames < 20.0) {
                diagnostic = "Simple visite";
                if (dureeSession > 90) alerte = "*Alerte :* Grattage long sans résultat.";
            } else if (poidsFinalGrames < 70.0) {
                diagnostic = dureeSession > 120 ? "Petit Pipi 🟡" : "Pipi 🟡";
                krokmouDernierPipi = millis();
                if (dureeSession > 120) alerte = "*Vigilance :* Long pour un petit résultat.";
            } else {
                diagnostic = dureeSession > 90 ? "Caca 🟤" : "Gros Pipi 🟡";
                krokmouDernierCaca = millis();
            }
        } else {
            // Inconnu - fallback aux anciens seuils
            if (poidsFinalGrames < 15.0)
                diagnostic = "Simple visite";
            else if (poidsFinalGrames < 55.0)
                diagnostic = "Petit Pipi 🟡";
            else
                diagnostic = dureeSession > 90 ? "Caca 🟤" : "Gros Pipi 🟡";
        }

        // Alerte temps extrême (indépendamment du poids)
        if (dureeSession > 240 && alerte == "") {
            alerte = "*Attention :* Session extrêmement longue (+4 min).";
        }

        // Envoi du rapport via Telegram
        verifierConnexion();
        envoyerNotification(nomChat, diagnostic, poidsFinalGrames, poidsEntree, dureeSession,
                            alerte);
        envoyerDonneesSheets(nomChat, diagnostic, poidsFinalGrames, poidsEntree, dureeSession,
                             alerte);
        addLog("DEBUG poidsEntree=" + String(poidsEntree, 2) +
               " poidsFinal=" + String(poidsFinalGrames, 1) + " duree=" + String(dureeSession));
        // Reset pour la suite
        addLog("Reset Balance...");
        delay(500);
        scale.tare();
        poidsEntree = 0;
        tempsEntree = 0;
        M5.dis.fillpix(0x00ff00);
    }

    // --- 4. DÉTECTION NETTOYAGE (Auto-Tare) ---
    // Si le poids est négatif (ex: -50g), cela veut dire qu'on a enlevé de la matière
    if (weight < -0.05 && !occupe) {
        addLog("Poids négatif détecté (Nettoyage ?). Attente stabilité...");
        delay(3000);  // On attend que tu finisses de pelleter
        esp_task_wdt_reset();

        // On revérifie si c'est toujours négatif après 3 secondes
        float weightCheck = scale.get_units(10) / 1000.0;
        if (weightCheck < -0.05) {
            M5.dis.fillpix(0x00ffff);  // Cyan : Auto-Tare en cours
            addLog(">>> AUTO-TARE (Nettoyage détecté)");

            scale.tare();

            // Optionnel : Notification pour te confirmer que c'est fait
            verifierConnexion();
            envoyerNotification("Système", "Nettoyage détecté", 0, 0, 0,
                                "La litière a été remise à zéro automatiquement.");

            delay(1000);
            esp_task_wdt_reset();
            M5.dis.fillpix(0x00ff00);  // Retour au vert
        }
    }

    // Tare Manuelle (Bouton central)
    if (M5.Btn.wasPressed()) {
        M5.dis.fillpix(0x0000ff);
        scale.tare();
        delay(500);
        M5.dis.fillpix(0x00ff00);
        addLog("\n>>> TARE MANUELLE");
        verifierConnexion();
        envoyerNotification("Système", "Tare manuelle faite!", 0, 0, 0, "");
    }

    if (millis() - dernierCheck > 60000) {  // every minute
        dernierCheck = millis();
        verifierAlertesSante();
    }

    if (!otaInProgress) delay(200);
}

// --- FONCTION NOTIFICATION TELEGRAM ---
void envoyerNotification(String chat, String action, float poids, float poids_chat,
                         unsigned long duree, String alerte) {
    if (WiFi.status() == WL_CONNECTED) {
        WiFiClientSecure client;
        client.setInsecure();  // C'est ICI qu'on dit à l'ESP32 d'ignorer le certificat SSL

        HTTPClient http;

        // Construction du message (on utilise des %0A pour les sauts de ligne)
        String message = "Rapport Litiere %0A";
        message += "Chat : " + chat + "%0A" + "Poids " + String(poids_chat, 1) + "kg%0A";
        message += "Action : " + action + "%0A";
        if (poids > 0) message += "*Poids :* " + String(poids, 1) + "g %0A";
        if (duree > 0) message += "*Durée :* " + String(duree) + "s";

        if (alerte != "") {
            message += "%0A%0A" + alerte;  // Ajoute l'alerte en bas du message
        }

        addLog("Envoi Telegram : " + message);

        message.replace(" ", "%20");  // Remplace les espaces pour l'URL

        // URL Telegram
        String url = "https://api.telegram.org/bot" + botToken + "/sendMessage?chat_id=" + chatId +
                     "&text=" + message;

        // On passe le 'client' sécurisé à HTTPClient
        if (http.begin(client, url)) {
            int httpCode = http.GET();
            if (httpCode > 0) {
                addLog("Telegram envoyé ! Code : " + String(httpCode));
            } else {
                addLog("Erreur HTTP : " + http.errorToString(httpCode));
            }
            http.end();
        }
    }
}

void addLog(String message) {
    String timestamp = String(millis() / 1000) + "s — ";
    logBuffer += timestamp + message + "<br>";
    logLineCount++;
    if (logLineCount > MAX_LOG_LINES) {
        // Trim oldest entry
        int firstBreak = logBuffer.indexOf("<br>");
        logBuffer = logBuffer.substring(firstBreak + 4);
        logLineCount--;
    }
    Serial.println(message);
}

void verifierConnexion() {
    if (WiFi.status() != WL_CONNECTED) {
        addLog("Connexion perdue. Tentative de reconnexion...");
        M5.dis.fillpix(0x330000);  // Rouge sombre pour indiquer le souci

        WiFi.disconnect();
        WiFi.begin(ssid, password);

        // On attend max 10 secondes pour ne pas bloquer la balance
        int tentative = 0;
        while (WiFi.status() != WL_CONNECTED && tentative < 20) {
            delay(500);
            tentative++;
        }

        if (WiFi.status() == WL_CONNECTED) {
            addLog("✅ Reconnecté !");
            M5.dis.fillpix(0x00ff00);  // Retour au vert
        }
    }
}

void verifierAlertesSante() {
    unsigned long maintenant = millis();

    // Sully - Pipi
    if (sullyDernierPipi > 0 && (maintenant - sullyDernierPipi > ALERTE_PIPI_MS)) {
        addLog("⚠️ Sully n'a pas fait pipi depuis +24h !");
        envoyerNotification("Sully", "Alerte Santé", 0, 0, 0,
                            "⚠️ Sully n'a pas fait pipi depuis +24h !");
        sullyDernierPipi = maintenant;  // reset pour ne pas respammer
    }

    // Sully - Caca
    if (sullyDernierCaca > 0 && (maintenant - sullyDernierCaca > ALERTE_CACA_MS)) {
        addLog("⚠️ Sully n'a pas fait caca depuis +48h !");
        envoyerNotification("Sully", "Alerte Santé", 0, 0, 0,
                            "⚠️ Sully n'a pas fait caca depuis +48h !");
        sullyDernierCaca = maintenant;
    }

    // Krokmou - Pipi
    if (krokmouDernierPipi > 0 && (maintenant - krokmouDernierPipi > ALERTE_PIPI_MS)) {
        addLog("⚠️ Krokmou n'a pas fait pipi depuis +24h !");
        envoyerNotification("Krokmou", "Alerte Santé", 0, 0, 0,
                            "⚠️ Krokmou n'a pas fait pipi depuis +24h !");
        krokmouDernierPipi = maintenant;
    }

    // Krokmou - Caca
    if (krokmouDernierCaca > 0 && (maintenant - krokmouDernierCaca > ALERTE_CACA_MS)) {
        addLog("⚠️ Krokmou n'a pas fait caca depuis +48h !");
        envoyerNotification("Krokmou", "Alerte Santé", 0, 0, 0,
                            "⚠️ Krokmou n'a pas fait caca depuis +48h !");
        krokmouDernierCaca = maintenant;
    }
}

void envoyerDonneesSheets(String chat, String action, float poids, float poids_chat,
                          unsigned long duree, String alerte) {
    if (WiFi.status() != WL_CONNECTED) return;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    // Build JSON payload
    String payload = "{";
    payload += "\"chat\":\"" + chat + "\",";
    payload += "\"action\":\"" + action + "\",";
    payload += "\"poids\":" + String(poids, 1) + ",";
    payload += "\"poids_chat\":" + String(poids_chat, 2) + ",";
    payload += "\"duree\":" + String(duree) + ",";
    payload += "\"alerte\":\"" + alerte + "\"";
    payload += "}";
    addLog("Payload: " + payload);

    if (http.begin(client, sheetsWebhookUrl)) {
        http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
        http.addHeader("Content-Type", "application/json");
        int httpCode = http.POST(payload);
        if (httpCode > 0) {
            String response = http.getString();
            addLog("Sheets envoyé ! Code : " + String(httpCode) + " Response: " + response);

        } else {
            addLog("Erreur Sheets : " + http.errorToString(httpCode));
        }
        http.end();
    }
}
