#include <WiFi.h>
#include <WebSocketsClient.h>  // bibliothèques utilisées
#include <ArduinoJson.h>

const int   PIN_EMG = 34;       // broche ou est branché le capteur
const float ALPHA   = 0.05f;    // puissance du lissage des données ici 5% de la nouvelle valeur
const int   SEUIL   = 200;      // seuil de détection de contraction

float smoothedValue = 0.0f;
int   sampleCount   = 0;        // compteur du nombre d'échantillons

const char* ssid     = "47";    // configuration WiFi
const char* password = "Kaan?????";

int         idSeance = 1;       // identifiant de séance reçu du serveur
const char* cote     = "SAIN";  // ici que le SAIN

// Données reçues du serveur lors de l'envoi d'un exercice
String nomExercice     = "";    // nom de l'exercice
String nomMouvement    = "";    // description du mouvement
String typeContraction = "";    // type de contraction (ex: Excentrique)
int    maxRepetition   = 0;     // nombre de répétitions maximum
int    maxSerie        = 0;     // nombre de séries maximum

// Compteurs de progression de l'exercice
int numeroRepetition = 0;       // compteur de répétitions, démarre à 0 et augmente à chaque contraction
int numeroSerie      = 1;       // compteur de séries, démarre à 1 et augmente à chaque fin de série

bool          enContraction           = false;  // true si signal au dessus du seuil
unsigned long debutContraction        = 0;      // timestamp début contraction
unsigned long finContraction          = 0;      // timestamp fin contraction
unsigned long debutRelaxation         = 0;      // timestamp début relaxation
int           amplitudeMax            = 0;      // pic max pendant la contraction
unsigned long dernierDebutContraction = 0;      // timestamp contraction précédente

bool exerciceActif = false;     // true si le serveur a envoyé START et que l'exercice est en cours

WebSocketsClient webSocket;     // déclaration du WebSocket

// Envoie un BIP au serveur pour signaler le début d'un mouvement détecté
void envoyerBip() // l'ordinateur effectue le bip via un son émis
{
  if (!webSocket.isConnected()) return;
  StaticJsonDocument<64> doc;
  doc["type"] = "BIP";
  doc["data"] = nullptr;        // pas de données associées au BIP
  char msg[64];
  serializeJson(doc, msg);
  webSocket.sendTXT(msg);
}

// Construit et envoie les données au serveur WebSocket
void envoyerDonnees(unsigned long dureeContraction, unsigned long dureeRelaxation, unsigned long latence, int amplitude)
{
  if (!webSocket.isConnected()) return;

  float tension = (smoothedValue / 4094.0f) * 10.0f; // valeur normalisée de 0 à 10

  StaticJsonDocument<512> doc;
  doc["type"]                 = "DATA";                          // type du message
  doc["nom_exercice"]         = nomExercice;                     // nom de l'exercice en cours
  doc["nom_mouvement"]        = nomMouvement;                    // description du mouvement
  doc["numero_repetition"]    = numeroRepetition;                // numéro de la répétition en cours
  doc["numero_serie"]         = numeroSerie;                     // numéro de la série en cours
  doc["id_seance"]            = idSeance;                        // identifiant séance
  doc["cote"]                 = cote;                            // SAIN ou LESE
  doc["tension"]              = round(tension * 100) / 100.0f;  // normalisé sur 10.0
  doc["temps_contraction_ms"] = dureeContraction;                // durée au dessus du seuil
  doc["temps_relaxation_ms"]  = dureeRelaxation;                 // durée entre deux contractions
  doc["latence_ms"]           = latence;                         // délai entre deux contractions
  doc["amplitude"]            = amplitude;                       // pic max pendant la contraction
  doc["timestamp"]            = millis();                        // timestamp en ms

  char msg[512];
  serializeJson(doc, msg); // convertit le doc en chaîne JSON
  webSocket.sendTXT(msg);  // envoie le message au serveur
}
// Gestion des évennement Websocket, fonction de rappel
void webSocketEvent(WStype_t type, uint8_t* payload, size_t length)
{
  switch (type)
  {
    // Fonction de surveillance pour voir si la communication se fait correctement
    case WStype_CONNECTED:
      break;

    case WStype_DISCONNECTED:
      exerciceActif = false; // stoppe la récolte si déconnecté
      break;

    case WStype_TEXT:
    {
      StaticJsonDocument<1024> cmd; // taille du document JSON
      DeserializationError err = deserializeJson(cmd, payload, length);
      // analyse et interprete les données du JSON reçu du serveur

      if (err) break;

      // Démarrage de la récolte sur réception du START avec chargement des paramètres
      if (cmd.containsKey("type") && cmd["type"] == "START")
      {
        idSeance = cmd["data"]["id_seance"]; // identifiant séance

        // récupération du premier mouvement dans la liste
        JsonObject premierMouvement = cmd["data"]["nom_exercice"]["mouvement"][0];

        nomExercice     = cmd["data"]["nom_exercice"]["nom"].as<String>(); // nom de l'exercice
        nomMouvement    = premierMouvement["description"].as<String>();    // description du mouvement
        typeContraction = premierMouvement["contraction"].as<String>();    // type de contraction
        maxRepetition   = premierMouvement["repetition"];                  // nombre de répétitions maximum
        maxSerie        = premierMouvement["serie"];                       // nombre de séries maximum

        numeroRepetition = 0;    // reset du compteur de répétitions
        numeroSerie      = 1;    // reset du compteur de séries
        exerciceActif    = true; // active la récolte de données
      }

      // Arrêt immédiat de la récolte sur réception du KILL
      if (cmd.containsKey("type") && cmd["type"] == "KILL")
      {
        exerciceActif    = false; // stoppe la récolte immédiatement
        enContraction    = false; // reset de l'état de contraction
        numeroRepetition = 0;     // reset du compteur de répétitions
        numeroSerie      = 1;     // reset du compteur de séries
        nomExercice      = "";    // reset des données de l'exercice
        nomMouvement     = "";    // reset de la description du mouvement
        typeContraction  = "";    // reset du type de contraction
        maxRepetition    = 0;     // reset du nombre de répétitions maximum
        maxSerie         = 0;     // reset du nombre de séries maximum
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

void setup()
{
  delay(2000); // attente de 2s pour que l'alimentation soit stable

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db); // plage de lecture 0 à 3.3V

  WiFi.begin(ssid, password); // connexion avec le WiFi
  while (WiFi.status() != WL_CONNECTED) // attend la connexion au WiFi
    delay(500);

  webSocket.begin("192.168.0.130", 3001, "/"); // connexion avec le WebSocket
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000); // retente toutes les 5s si déconnecté
}

void loop()
{
  webSocket.loop();
  int rawValue  = analogRead(PIN_EMG);                                // lit la tension sur le GPIO34 de l'ESP32
  smoothedValue = ALPHA * rawValue + (1.0f - ALPHA) * smoothedValue;  // filtre pour réduire le bruit électrique
  int emgValue  = (int)smoothedValue;
  sampleCount++;

  if (exerciceActif) // traitement uniquement si l'exercice est démarré
  {
    if (!enContraction && emgValue > SEUIL) // détection début de contraction
    {
      enContraction    = true;
      debutContraction = millis();
      amplitudeMax     = emgValue;
      envoyerBip(); // BIP envoyé au serveur pour signaler le début du mouvement
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

        numeroRepetition++; // incrémente le compteur de répétitions à chaque fin de contraction

        if (numeroRepetition >= maxRepetition) // vérifie si le nombre max de répétitions est atteint
        {
          numeroRepetition = 0;  // reset du compteur de répétitions pour la série suivante
          numeroSerie++;         // passage à la série suivante

          if (numeroSerie > maxSerie) // vérifie si toutes les séries sont terminées
            exerciceActif = false; // stoppe la récolte, exercice terminé
        }

        dernierDebutContraction = debutContraction; // sauvegarde pour la prochaine répétition
        debutRelaxation         = finContraction;
        amplitudeMax            = 0;
      }
    }

    // envoi des données en continu toutes les 100ms (~10 fois par seconde)
    if (sampleCount >= 100)
    {
      sampleCount = 0;
      envoyerDonnees(
        enContraction ? millis() - debutContraction : 0,                              // durée contraction en cours ou 0
        enContraction ? debutContraction - debutRelaxation : 0,                       // durée relaxation précédente ou 0
        dernierDebutContraction > 0 ? debutContraction - dernierDebutContraction : 0, // latence ou 0
        amplitudeMax                                                                   // pic max en cours ou 0
      );
    }
  }

  delay(1);
}