//#include <ElegantOTA.h>
#include <M5Atom.h>
#include "HX711.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// --- CONFIGURATION WI-FI ---
const char* ssid     = "SSID";
const char* password = "¨PASSWORD";

// --- CONFIGURATION TELEGRAM ---
const String botToken = "BOTTOKEN";
const String chatId   = "CHATID";

// --- CABLAGE HX711 ---
#define LOADCELL_DOUT_PIN 32
#define LOADCELL_SCK_PIN  26

HX711 scale;

// --- VARIABLES DE SUIVI ---
float poidsEntree = 0;
bool occupe = false;
unsigned long tempsEntree = 0;

//WebServer server(80);

// --- FONCTION NOTIFICATION TELEGRAM ---
void envoyerNotification(String chat, String action, float poids, float poids_chat, unsigned long duree, String alerte) {
    if(WiFi.status() == WL_CONNECTED) {
        WiFiClientSecure client;
        client.setInsecure(); // C'est ICI qu'on dit à l'ESP32 d'ignorer le certificat SSL
        
        HTTPClient http;
        
        // Construction du message (on utilise des %0A pour les sauts de ligne)
        String message = "Rapport Litiere %0A";
        message += "Chat : " + chat + "%0A" + "Poids " + String(poids_chat, 1) + "kg%0A";
        message += "Action : " + action + "%0A";
        if (poids > 0) message += "*Poids :* " + String(poids, 1) + "g %0A";
        if (duree > 0) message += "*Durée :* " + String(duree) + "s";

        if (alerte != "") {
            message += "%0A%0A" + alerte; // Ajoute l'alerte en bas du message
        }
        
        message.replace(" ", "%20"); // Remplace les espaces pour l'URL

        // URL Telegram
        String url = "https://api.telegram.org/bot" + botToken + "/sendMessage?chat_id=" + chatId + "&text=" + message;

        // On passe le 'client' sécurisé à HTTPClient
        if (http.begin(client, url)) { 
            int httpCode = http.GET();
            if (httpCode > 0) {
                Serial.printf("Telegram envoyé ! Code : %d\n", httpCode);
            } else {
                Serial.printf("Erreur HTTP : %s\n", http.errorToString(httpCode).c_str());
            }
            http.end();
        }
    }
}

void setup() {
    Serial.begin(115200);
    btStop();

    M5.begin(false, false, true);
    
    // 1. Connexion Wi-Fi
    M5.dis.fillpix(0x0000ff); // Bleu pendant la connexion
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(ssid, password);
    //Serial.print("Connexion Wi-Fi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        //Serial.print(".");
    }
    //Serial.println("\nWi-Fi Connecté !");

    //Serial.print("IP address: ");
    //Serial.println(WiFi.localIP());
    //server.on("/", []() {
    //server.send(200, "text/plain", "Hi! This is ElegantOTA Demo.");
    //});
    
    //ElegantOTA.begin(&server);    // Start ElegantOTA
    //ElegantOTA.setAutoReboot(true);
    //server.begin();

    //Serial.print("Server started");

    // 2. Initialisation Balance
    M5.dis.fillpix(0xff0000); // Rouge pendant la tare
    scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);
    scale.set_scale(24.79f); // Ta valeur de calibration finale

    // --- AMÉLIORATION : TARE SÉCURISÉE ---
    M5.dis.fillpix(0x0000ff); // Bleu : Initialisation
    delay(2000);
    scale.tare();
    
    M5.dis.fillpix(0x00ff00); // Vert : Prêt !
    //Serial.println("Système de litière prêt et calibré.");
    envoyerNotification("Système", "Litière connectée et prête !", 0, 0, 0, "");
}

void verifierConnexion() {
    if (WiFi.status() != WL_CONNECTED) {
        //Serial.println("Connexion perdue. Tentative de reconnexion...");
        M5.dis.fillpix(0x330000); // Rouge sombre pour indiquer le souci
        
        WiFi.disconnect();
        WiFi.begin(ssid, password);
        
        // On attend max 10 secondes pour ne pas bloquer la balance
        int tentative = 0;
        while (WiFi.status() != WL_CONNECTED && tentative < 20) {
            delay(500);
            tentative++;
        }
        
        if (WiFi.status() == WL_CONNECTED) {
            //Serial.println("✅ Reconnecté !");
            M5.dis.fillpix(0x00ff00); // Retour au vert
        }
    }
}

void loop() {
    //server.handleClient();
    //ElegantOTA.loop();
    M5.update();
    
    // Mesure du poids (moyenne de 5)
    float weight = scale.get_units(5) / 1000.0;

    // --- 1. DÉTECTION D'ENTRÉE ---
    if (weight > 2.0 && !occupe) {
        //Serial.println("Mouvement détecté, vérification stabilité...");
        delay(1500); 
        float weightStable = scale.get_units(10) / 1000.0; 
        
        if (weightStable > 2.0) { 
            occupe = true;
            poidsEntree = weightStable; 
            tempsEntree = millis(); 
            M5.dis.fillpix(0xff0000); // Rouge : Occupé
            //Serial.println("\n--- CHAT ENTRÉ ---");
        }
    }

    // --- 2. SÉCURITÉ ANTI-BLOCAGE (10 MIN) ---
    if (occupe && (millis() - tempsEntree > 600000)) {
        envoyerNotification("Système", "*Alerte Sécurité*", 0, poidsEntree, 0, "Session de +10 min détectée. Reset automatique de la balance.");
        scale.tare();
        occupe = false;
        M5.dis.fillpix(0x00ff00);
    }

    // --- 3. DÉTECTION DE SORTIE ---
    if (weight < 1.0 && occupe) {
        //Serial.println("Chat sorti, analyse finale...");
        delay(5000); // Temps pour que le chat s'éloigne et que la balance se stabilise
        
        float poidsFinalGrames = scale.get_units(15); 
        unsigned long dureeSession = (millis() - tempsEntree) / 1000;
        
        // Identification du chat
        String nomChat = "Inconnu";
        if (poidsEntree > 2.5 && poidsEntree < 5.0) {
            nomChat = "Sully";
            M5.dis.fillpix(0x0000ff); // Bleu pour le petit
        } 
        else if (poidsEntree > 6.0 && poidsEntree < 9.0) {
            nomChat = "Krokmou";
            M5.dis.fillpix(0xffa500); // Orange pour le gros
        }

        String diagnostic = "";
        String alerte = "";

        // --- LOGIQUE SANTÉ AFFINÉE ---
        if (poidsFinalGrames < 15.0) {
            diagnostic = "Simple visite";
            if (dureeSession > 90) alerte = "*Alerte :* Grattage long mais rien n'est sorti.";
        } 
        else if (poidsFinalGrames >= 15.0 && poidsFinalGrames < 55.0) {
            if (dureeSession > 120) {
                diagnostic = "Petit Pipi";
                alerte = "*Vigilance :* A mis longtemps pour un petit résultat.";
            } else {
                diagnostic = "Petit Pipi";
            }
        } 
        else { // + de 55g
            if (dureeSession > 90) diagnostic = "Caca probable";
            else diagnostic = "Gros Pipi";
        }

        // Alerte temps extrême (indépendamment du poids)
        if (dureeSession > 240 && alerte == "") {
            alerte = "*Attention :* Session extrêmement longue (+4 min).";
        }
        
        // Envoi du rapport via Telegram
        verifierConnexion();
        envoyerNotification(nomChat, diagnostic, poidsFinalGrames, poidsEntree, dureeSession, alerte);
        // Reset pour la suite
        //Serial.println("Reset Balance...");
        delay(500);
        scale.tare(); 
        occupe = false;
        poidsEntree = 0;
        tempsEntree = 0;
        M5.dis.fillpix(0x00ff00); 
    }

    // --- 4. DÉTECTION NETTOYAGE (Auto-Tare) ---
    // Si le poids est négatif (ex: -50g), cela veut dire qu'on a enlevé de la matière
    if (weight < -0.05 && !occupe) { 
        //Serial.println("Poids négatif détecté (Nettoyage ?). Attente stabilité...");
        delay(3000); // On attend que tu finisses de pelleter
        
        // On revérifie si c'est toujours négatif après 3 secondes
        float weightCheck = scale.get_units(10) / 1000.0;
        if (weightCheck < -0.05) {
            M5.dis.fillpix(0x00ffff); // Cyan : Auto-Tare en cours
            //Serial.println(">>> AUTO-TARE (Nettoyage détecté)");
            
            scale.tare();
            
            // Optionnel : Notification pour te confirmer que c'est fait
            verifierConnexion();
            envoyerNotification("Système", "Nettoyage détecté", 0, 0, 0, "La litière a été remise à zéro automatiquement.");
            
            delay(1000);
            M5.dis.fillpix(0x00ff00); // Retour au vert
        }
    }

    // Debug affichage poids au repos
    //if (!occupe) {
       //Serial.printf("Poids litière : %.2fkg \r", weight);
    //}

    // Tare Manuelle (Bouton central)
    if (M5.Btn.wasPressed()) {
        M5.dis.fillpix(0x0000ff); 
        scale.tare();
        delay(500);
        M5.dis.fillpix(0x00ff00);
        ////Serial.println("\n>>> TARE MANUELLE");
        envoyerNotification("Système", "Tare manuelle faite!", 0, 0, 0, "");
    }

    delay(200);
}
