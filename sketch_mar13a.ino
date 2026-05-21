#include <WiFi.h>
#include <WebSocketsClient.h>  // bibliothèques utilisées
#include <ArduinoJson.h>

const int   PIN_EMG    = 34;    // broche ou est branché le capteur
const float ALPHA      = 0.05f; // puissance du lissage des données ici 5% de la nouvelle valeur
const int   SEUIL      = 250;   // seuil de détection de contraction

float smoothedValue = 0.0f;
int   sampleCount   = 0;        // compteur du nombre d'échantillons

const char* ssid     = "47";    // configuration WiFi
const char* password = "!!!!!!!!!!";

int         idSeance     = 1;       // identifiant de séance reçu du serveur
const char* cote         = "SAIN"; // "SAIN" ou "LESE" selon le capteur branché

unsigned long dernierBip = 0;       // timestamp du dernier bip

bool          enContraction           = false;  // true si signal au dessus du seuil
unsigned long debutContraction        = 0;      // timestamp début contraction
unsigned long finContraction          = 0;      // timestamp fin contraction
unsigned long debutRelaxation         = 0;      // timestamp début relaxation
int           amplitudeMax            = 0;      // pic max pendant la contraction
unsigned long dernierDebutContraction = 0;      // timestamp contraction précédente

WebSocketsClient webSocket; // déclaration du WebSocket

// Fonction de surveillance pour voir si la communication se fait correctement
void webSocketEvent(WStype_t type, uint8_t* payload, size_t length)
{
  switch (type)
  {
    case WStype_CONNECTED:
      Serial.println("WebSocket connecté !");
      break;

    case WStype_DISCONNECTED:
      Serial.println("WebSocket déconnecté !");
      break;

    case WStype_TEXT:
    {
      StaticJsonDocument<128> cmd;
      DeserializationError err = deserializeJson(cmd, payload, length); 
      // analyse et interprete les données du JSON reçu du serveur

      if (err)
      {
        Serial.println("Erreur d'analyse du JSON reçu");
        break;
      }

      if (cmd.containsKey("id_seance"))
        idSeance = cmd["id_seance"]; // mise à jour de l'id de séance

      StaticJsonDocument<64> ack;
      ack["status"] = "ok"; // confirmation de réception
      char ackMsg[64];
      serializeJson(ack, ackMsg);
      webSocket.sendTXT(ackMsg);
      break;
    }
  }
}

// Construit et envoie les données au serveur WebSocket
void envoyerDonnees(unsigned long dureeContraction, unsigned long dureeRelaxation, unsigned long latence, int amplitude)
{
  if (!webSocket.isConnected()) return;

  float tension = (smoothedValue / 4094.0f) * 10.0f; // valeur normalisée de 0 à s10

  StaticJsonDocument<256> doc;  
  doc["id_seance"]            = idSeance;                       // identifiant séance
  doc["cote"]                 = cote;                           // SAIN ou LESE
  doc["tension"]              = round(tension * 100) / 100.0f; // normalisé sur 10.0
  doc["temps_contraction_ms"] = dureeContraction;               // durée au dessus du seuil
  doc["temps_relaxation_ms"]  = dureeRelaxation;                // durée entre deux contractions
  doc["latence_ms"]           = latence;                        // délai entre deux contractions
  doc["amplitude"]            = amplitude;                      // pic max pendant la contraction
  doc["timestamp"]            = millis();                       // timestamp en ms

  char msg[256];
  serializeJson(doc, msg); // convertit le doc en chaîne JSON
  Serial.println(msg);     // debug série
  webSocket.sendTXT(msg);  // envoie le message au serveur
}

void setup()
{
  Serial.begin(115200);
  delay(2000); // attente de 2s pour que l'alimentation soit stable

  pinMode(PIN_BUZZER, OUTPUT); // initialise la broche du buzzer

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db); // plage de lecture 0 à 3.3V

  Serial.println("Lecture capteur Myoware 2.0");

  WiFi.begin(ssid, password); // connexion avec le WiFi
  Serial.print("Connexion WiFi");
  while (WiFi.status() != WL_CONNECTED) // attend la connexion au WiFi
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connecté !");

  webSocket.begin("192.168.0.25", 8080, "/"); // connexion avec le WebSocket
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000); // retente toutes les 5s si déconnecté
}

void loop()
{
  webSocket.loop();
  int rawValue  = analogRead(PIN_EMG); // lit la tension sur le GPIO34 de l'ESP32
  smoothedValue = ALPHA * rawValue + (1.0f - ALPHA) * smoothedValue; // filtre pour réduire le bruit électrique
  int emgValue  = (int)smoothedValue;
  sampleCount++;

  if (!enContraction && emgValue > SEUIL) // détection début de contraction
  {
    enContraction    = true;
    debutContraction = millis();
    amplitudeMax     = emgValue;
  }
  else if (enContraction)
  {
    if (emgValue > amplitudeMax)
      amplitudeMax = emgValue; // mise à jour du pic

    if (emgValue <= SEUIL) // détection fin de contraction
    {
      enContraction  = false;
      finContraction = millis();

      unsigned long dureeContraction = finContraction - debutContraction;          // durée de la contraction
      unsigned long dureeRelaxation  = debutContraction - debutRelaxation;         // durée de la relaxation précédente
      unsigned long latence          = debutContraction - dernierDebutContraction; // délai entre contractions

      envoyerDonnees(dureeContraction, dureeRelaxation, latence, amplitudeMax);

      dernierDebutContraction = debutContraction; // sauvegarde pour la prochaine répétition
      debutRelaxation         = finContraction;
      amplitudeMax            = 0;
    }
  }

  if (sampleCount >= 40) // affiche 25 fois par seconde pour le Serial Plotter
  {
    sampleCount = 0;
    Serial.println(emgValue);
  }

  delay(1);
}