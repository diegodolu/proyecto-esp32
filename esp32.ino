#include <WiFi.h>
#include <HX711.h>
#include <BluetoothSerial.h>
#include <NTPClient.h>
#include <WiFiUdp.h>  // va con la librería WiFi
#include <FirebaseClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <PubSubClient.h>

// ------------------------------------------ DEFINICIÓN DE PINES --------------------------------------
#define SCK 18
#define DT 19
#define LED 16       // led envio de lectura AZUL
#define LEDALERTA 2  // led de bajo nivel de gas ROJO

// ---------------------------------- DEFINICIÓN DE REALTIME DATABASE - FIREBASE ------------------------
#define API_KEY ""
#define DATABASE_URL ""

// ------------------------------------------- OBJETOS ------------------------------------------------
BluetoothSerial SerialBT;
HX711 celda;
WiFiClient wifiClient;

// NTP Client para obtener la fecha y hora
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60000);

WiFiClientSecure ssl;
DefaultNetwork network;
AsyncClientClass asyncClient(ssl, getNetwork(network));  // Instancia de AsyncClientClass

FirebaseApp app;
RealtimeDatabase Database;
AsyncResult result;
NoAuth noAuth;

// -----------------------------------------Detalles del servidor MQTT-------------------------------------
const char* mqtt_server = "mqtt.flespi.io";
const int mqtt_port = 1883;
const char* mqtt_topic = "esp32GasSmart/gas_control";

WiFiClient espClient;
PubSubClient client(espClient);  // Objeto PubSubClient

// -------------------------------------------- VARIABLES -----------------------------------------------
int porcentaje = 0;
const float peso_balon = -373.0;
const int peso_gas = 1553;
String sensorId = "52";
const String name = "AQ52";
bool registrado = false;
int id_lectura = 1400;
String userId = "";
bool enviarLecturas = false;  // Definición de la variable enviarLecturas

// Credenciales de WiFi (inicialmente vacías)
String wifiSSID = "";
String wifiPassword = "";

// ------------------------------------------------- FUNCIONES ADICIONALES ---------------------------------------

// --------------------- Formatear fecha ---------------------

String getFormattedTime() {
  timeClient.update();
  unsigned long epochTime = timeClient.getEpochTime();
  if (epochTime == 0) {
    return "Invalid time";
  }
  time_t epochTime_t = static_cast<time_t>(epochTime);
  struct tm* ptm = gmtime(&epochTime_t);
  if (ptm == nullptr) {
    return "Conversion Error";
  }
  char timeString[30];
  sprintf(timeString, "%04d-%02d-%02dT%02d:%02d:%02dZ",
          ptm->tm_year + 1900,
          ptm->tm_mon + 1,
          ptm->tm_mday,
          ptm->tm_hour,
          ptm->tm_min,
          ptm->tm_sec);
  return String(timeString);
}

// --------------------- Obtener la compraId ---------------------

void getSensorCompraId(String sensorId, String& compraId) {
  String path = "/sensores/" + sensorId + "/compraId";
  compraId = Database.get<String>(asyncClient, path);
  if (asyncClient.lastError().code() == 0) {
    Serial.print("El id de la compra es: ");
    Serial.println(compraId);
  } else {
    Serial.println("Error al obtener la compraId: " + String(asyncClient.lastError().message()));
  }
}

// --------------------- Obtener la marcaId ---------------------

void getCompraMarcaId(String compraId, String& marcaId) {
  String path = "/compras/" + compraId + "/marca_id";
  marcaId = Database.get<String>(asyncClient, path);
  if (asyncClient.lastError().code() == 0) {
    Serial.print("El id de la marca es: ");
    Serial.println(marcaId);
  } else {
    Serial.println("Error al obtener la marcaId: " + String(asyncClient.lastError().message()));
  }
}

// --------------------- Verificar si el sensor está registrado ---------------------

bool checkSensorRegistration(String sensorId, String userId) {
  bool isSensorRegistered = Database.get<String>(asyncClient, "/sensores/" + sensorId + "/userId");
  if (isSensorRegistered) {
    Serial.print("El sensor ya está registrado para el usuario: ");
    Serial.println(sensorId);
    return true;
  } else {
    Serial.println("El sensor no está registrado: " + String(asyncClient.lastError().message()));
    return false;
  }
}

// --------------------- Registrar Sensor en Firebase ---------------------

void registerNewSensor(String sensorId, String userId) {
  String path = "/sensores/" + sensorId;
  String json = "{";
  json += "\"id\": \"" + sensorId + "\", ";
  json += "\"name\": \"" + name + "\", ";
  json += "\"userId\": \"" + userId + "\", ";
  json += "\"compraId\": \"" + String(0) + "\"";
  json += "}";
  Serial.println("JSON generado:");
  Serial.println(json);
  bool status = Database.set<object_t>(asyncClient, path, object_t(json.c_str()));
  Serial.print("Status registrar sensor: ");
  Serial.println(status);
  if (status) {
    Serial.println("Sensor registrado exitosamente");
  } else {
    Serial.println("Error al registrar sensor: " + String(asyncClient.lastError().message()));
  }
}

// --------------------- Enviar lectura a Firebase ---------------------

void sendGasReading(int porcentaje, String id_sensor) {
  String compraId;
  getSensorCompraId(id_sensor, compraId);
  String marcaId;
  getCompraMarcaId(compraId, marcaId);
  String fechaLectura = getFormattedTime();
  String json = "{";
  json += "\"id\": \"" + String(id_lectura) + "\", ";
  json += "\"sensorId\": \"" + String(id_sensor) + "\", ";
  json += "\"compraId\": \"" + String(compraId) + "\", ";
  json += "\"marcaId\": \"" + String(marcaId) + "\", ";
  json += "\"fechaLectura\": \"" + String(fechaLectura) + "\", ";
  json += "\"porcentajeGas\": \"" + String(porcentaje) + "\"";
  json += "}";
  Serial.println("JSON generado:");
  Serial.println(json);
  String status = Database.push<object_t>(asyncClient, "/lecturas", object_t(json.c_str()));
  if (status) {
    Serial.println("Envío de lectura exitoso");
    id_lectura += 1;
    digitalWrite(LED, HIGH);
    delay(2000);
    digitalWrite(LED, LOW);
  } else {
    Serial.println("Error al enviar lectura: " + String(asyncClient.lastError().message()));
  }
}

// --------------------- Calcular porcentaje del balón ---------------------

int porcentaje_balon(float peso) {
  if (peso < 0) {
    return 0;
  } else if (peso > 1553) {
    return 100;
  } else {
    return round((peso / (float)peso_gas) * 100);
  }
}

// --------------------- Esperar credenciales WiFi ---------------------------

bool esperarCredencialesWiFi() {
  unsigned long startTime = millis();
  unsigned long timeout = 60000;
  while (millis() - startTime < timeout) {
    if (SerialBT.available()) {
      userId = SerialBT.readStringUntil('\n');
      wifiSSID = SerialBT.readStringUntil('\n');
      wifiPassword = SerialBT.readStringUntil('\n');
      Serial.println("Credenciales WiFi recibidas:");
      Serial.println("userId: " + userId);
      Serial.println("SSID: " + wifiSSID);
      Serial.println("Password: " + wifiPassword);
      return true;
    }
    delay(500);
  }
  return false;
}

// ------------------------------------------------------------ SET UP ------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  SerialBT.begin("ESP32GasSmart");
  Serial.println("Bluetooth device started, you can pair it now");
  pinMode(LED, OUTPUT);
  pinMode(LEDALERTA, OUTPUT);
  celda.begin(DT, SCK);
  celda.set_scale(211.f);
  celda.tare();
  float tareValue = celda.get_units(10);
  float targetTare = peso_balon;
  float adjustment = targetTare - tareValue;
  celda.set_offset(celda.get_offset() - adjustment * celda.get_scale());
  if (esperarCredencialesWiFi()) {
    Serial.println("Credenciales WiFi recibidas. Intentando conectar...");
    WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
    int tiempoEspera = 0;
    while (WiFi.status() != WL_CONNECTED && tiempoEspera < 10000) {
      delay(500);
      Serial.print(".");
      tiempoEspera += 500;
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("WiFi Conectado");
      Serial.println("Configurando Firebase...");
      Firebase.printf("Firebase Client v%s\n", FIREBASE_CLIENT_VERSION);
      ssl.setInsecure();
      initializeApp(asyncClient, app, getAuth(noAuth));
      app.getApp<RealtimeDatabase>(Database);
      Database.url(DATABASE_URL);
      asyncClient.setAsyncResult(result);
      timeClient.begin();
      timeClient.setTimeOffset(-5 * 3600);
      timeClient.update();
      SerialBT.end();
      Serial.println("Bluetooth desactivado");
      client.setServer(mqtt_server, mqtt_port);
      client.setCallback(callback);
      reconnect();

    } else {
      Serial.println("Error: No se pudo conectar a WiFi. Revisa las credenciales.");
    }
  } else {
    Serial.println("No se recibieron credenciales WiFi. Verifica la conexión Bluetooth.");
  }
}

//-------------------------------------Función que se llama cuando se recibe un MENSAJE en el TOPIC suscrito ----------------------------------------------------------
void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Mensaje recibido [");
  Serial.print(topic);
  Serial.print("]: ");
  String message;
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.println(message);
  if (message == "1") {
    enviarLecturas = true;
    Serial.println("Activando envío de lecturas");
  } else if (message == "0") {
    enviarLecturas = false;
    Serial.println("Desactivando envío de lecturas");
  }
}

// ---------------------------------------------Función para RECONECTAR al broker MQTT en caso de desconexión----------------------------------------------
void reconnect() {
  while (!client.connected()) {
    Serial.print("Intentando conectar al broker MQTT...");
    if (client.connect("ESP32Client", "FlespiToken OAyj8LMQreahGxgRENm7sfbWd8bXsOcHhxgGhT5WkH5nCjuR6XuLfnUyy4YOZQmO", NULL)) {
      Serial.println("conectado");
      client.subscribe(mqtt_topic);
    } else {
      Serial.print("falló, rc=");
      Serial.print(client.state());
      Serial.println(" Intentando de nuevo en 5 segundos");
      delay(5000);
    }
  }
}

// ------------------------------------------------------------ LOOP ------------------------------------------------------------

void loop() {
  client.loop();  // Llamar frecuentemente a client.loop() para procesar los mensajes MQTT

  if (WiFi.status() == WL_CONNECTED) {
    if (!registrado) {
      registerNewSensor(sensorId, userId);
      registrado = true;
    }

    float peso = celda.get_units(10);
    int porcentaje = porcentaje_balon(peso);
    Serial.print("Peso: ");
    Serial.println(peso);
    Serial.print("Porcentaje: ");
    Serial.println(porcentaje);
    celda.power_down();
    delay(3000);
    celda.power_up();
    if (!client.connected()) {
      reconnect();
    }
    client.loop();
    if (enviarLecturas) {
      digitalWrite(LED, LOW);
      sendGasReading(porcentaje, sensorId);
      delay(10000);
    }
    if (porcentaje < 10) {
      Serial.println("Gas bajo, encendiendo alerta...");
      digitalWrite(LEDALERTA, HIGH);
    } else {
      digitalWrite(LEDALERTA, LOW);
    }
  }
  delay(1000);
}