#include <WiFi.h>
#include <esp_wifi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ArduinoJson 7. Preencha a rede Wi-Fi antes de gravar o sketch.
constexpr char WIFI_SSID[] = "";
constexpr char WIFI_PASSWORD[] = "";
constexpr bool FORCAR_MAC = false;
const uint8_t MAC_PERSONALIZADO[6] = {0x24, 0x6F, 0x28, 0xAA, 0xBB, 0xCC};
int canalRede = 0;
uint8_t bssidRede[6] = {};
bool encontrouRede = false;

constexpr char MQTT_HOST[] = "broker.hivemq.com";
constexpr uint16_t MQTT_PORT = 1883;
constexpr char MQTT_TOPIC[] = "smartbus/stops/1/incoming";
constexpr uint16_t MQTT_BUFFER = 4096;

constexpr int OLED_SDA = 21, OLED_SCL = 22;
constexpr uint8_t OLED_ADDR = 0x3C;
constexpr int TRIG_PIN = 5, ECHO_PIN = 18, BUZZER_PIN = 23;
constexpr float DISPLAY_RANGE_CM = 40.0f;
constexpr unsigned long DATA_TTL_MS = 120000UL;
constexpr unsigned long DEMO_MOVE_MS = 3400UL;
constexpr unsigned long DEMO_SCREEN_MS = 6000UL;
constexpr unsigned long DEMO_FRAME_MS = 60UL;

Adafruit_SSD1306 display(128, 64, &Wire, -1);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
JsonDocument mqttFilter;

bool oledReady = false, screenOn = false, mqttBufferReady = false;
bool wifiWasConnected = false, received = false;
bool gpsOnly = false, noService = false;
String busLine, busName, arrivalTime, vehicleId;
bool simulationData = false;
bool demoRunning = false;
unsigned long demoStarted = 0, lastFrame = 0;
int etaMinutes = -1, vehicleCount = -1;
float distanceCm = -1.0f;
unsigned long lastMessage = 0, lastSensor = 0;
unsigned long lastWifiAttempt = 0, lastMqttAttempt = 0;
constexpr unsigned long BEEP_MS = 200UL;
bool buzzerActive = false;
unsigned long buzzerStart = 0;

bool dataFresh(unsigned long now) {
  return received && now - lastMessage <= DATA_TTL_MS;
}

// A fonte padrao do Adafruit_GFX nao interpreta texto UTF-8.
// Converte letras acentuadas do portugues para o equivalente ASCII.
String oledText(const char* input) {
  String result;
  while (*input) {
    const uint8_t c = static_cast<uint8_t>(*input++);
    if (c >= 32 && c < 127) {
      result += static_cast<char>(c);
    } else if (c == '\n' || c == '\r' || c == '\t') {
      result += ' ';
    } else if (c >= 128) {
      char replacement = '?';
      if (c == 0xC3 && *input) {
        const uint8_t next = static_cast<uint8_t>(*input);
        if (next >= 0x80 && next <= 0xBF) {
          static constexpr char letters[] =
            "AAAAAA?CEEEEIIIIDNOOOOO?OUUUUY??"
            "aaaaaa?ceeeeiiiidnooooo?ouuuuy?y";
          static_assert(sizeof letters == 65, "Tabela Latin-1 incompleta");
          replacement = letters[next - 0x80];
        }
      }
      while ((static_cast<uint8_t>(*input) & 0xC0) == 0x80) ++input;
      result += replacement;
    }
  }
  result.trim();
  return result;
}

constexpr unsigned int OLED_COLS = 21;  // 128 px / 6 px por caractere.

void drawTextRow(int16_t y, const String& text) {
  display.setCursor(0, y);
  if (text.length() <= OLED_COLS) {
    display.print(text);
  } else {
    display.print(text.substring(0, OLED_COLS - 3));
    display.print("...");
  }
}

void drawBusName(int16_t y) {
  if (!busName.length()) {
    drawTextRow(y, "Nome indisponivel");
    return;
  }
  if (busName.length() <= OLED_COLS) {
    drawTextRow(y, busName);
    return;
  }
  unsigned int split = OLED_COLS;
  // Se possivel, quebra no espaco. Palavras longas podem ocupar as duas linhas.
  for (unsigned int i = OLED_COLS; i > 0; --i) {
    if (busName[i] == ' ') {
      split = i;
      break;
    }
  }
  String rest = busName.substring(split);
  rest.trim();
  if (rest.length() > OLED_COLS && busName.length() <= 2 * OLED_COLS) {
    split = OLED_COLS;
    rest = busName.substring(split);
    rest.trim();
  }
  drawTextRow(y, busName.substring(0, split));
  drawTextRow(y + 9, rest);
}

void beginOledFrame() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setTextWrap(false);
}

int16_t demoBusX(unsigned long elapsed) {
  if (elapsed >= DEMO_MOVE_MS) return 90;
  const long remaining = DEMO_MOVE_MS - elapsed;
  // Freia suavemente antes da placa, com interpolacao quadratica.
  const long duration = DEMO_MOVE_MS;
  return 90 - (114L * remaining * remaining) / (duration * duration);
}

void drawDemo(unsigned long now) {
  const int16_t x = demoBusX(now - demoStarted);
  constexpr int16_t y = 38;
  beginOledFrame();
  drawTextRow(0, "ONIBUS CHEGANDO");
  drawTextRow(10, String("Linha: ") + busLine);
  drawBusName(20);

  display.drawFastHLine(0, 54, 128, SSD1306_WHITE);
  display.drawFastVLine(122, 45, 9, SSD1306_WHITE);
  display.drawRect(118, 38, 9, 7, SSD1306_WHITE);
  display.drawFastHLine(120, 41, 5, SSD1306_WHITE);

  display.drawRoundRect(x, y, 26, 12, 2, SSD1306_WHITE);
  display.fillRect(x + 3, y + 2, 4, 4, SSD1306_WHITE);
  display.fillRect(x + 9, y + 2, 4, 4, SSD1306_WHITE);
  display.drawRect(x + 16, y + 2, 6, 8, SSD1306_WHITE);
  display.drawPixel(x + 24, y + 8, SSD1306_WHITE);
  display.fillCircle(x + 5, y + 12, 2, SSD1306_WHITE);
  display.fillCircle(x + 20, y + 12, 2, SSD1306_WHITE);

  drawTextRow(56, etaMinutes == 0 ? "Chegada agora" : "Chegada em 1 min");
  display.display();
}

void drawScreen() {
  if (!oledReady || !screenOn) return;
  const unsigned long now = millis();
  if (demoRunning && now - demoStarted < DEMO_SCREEN_MS) {
    drawDemo(now);
    return;
  }
  beginOledFrame();
  const bool fresh = dataFresh(now);
  drawTextRow(0, fresh && simulationData ? "SMARTBUS - SIMULACAO" :
                 fresh && gpsOnly ? "SMARTBUS - OLHO VIVO" : "SMARTBUS");
  display.drawFastHLine(0, 9, 128, SSD1306_WHITE);
  if (fresh) {
    drawTextRow(12, String("Linha: ") + busLine);
    drawBusName(22);
    if (noService) drawTextRow(42, "Sem horario previsto");
    else if (etaMinutes == 0) drawTextRow(42, "Chegada agora");
    else if (etaMinutes > 0) drawTextRow(42, String("Chegada: ") + etaMinutes + " min");
    else if (gpsOnly) drawTextRow(42, "Sem ETA por parada");
    else drawTextRow(42, "Aguardando previsao");
  } else {
    drawTextRow(15, received ? "Dados desatualizados" : "Aguardando dados");
  }

  if (WiFi.status() != WL_CONNECTED) drawTextRow(54, "Wi-Fi desconectado");
  else if (!mqtt.connected()) drawTextRow(54, "MQTT desconectado");
  else if (fresh && gpsOnly && vehicleCount >= 0) {
    drawTextRow(54, String("GPS: ") + vehicleCount + " onibus");
  } else if (fresh && vehicleId.length() && !simulationData) {
    drawTextRow(54, String("Carro: ") + vehicleId);
  } else {
    drawTextRow(54, "MQTT conectado");
  }
  display.display();
}

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, MQTT_TOPIC) != 0) return;

  JsonDocument doc;
  DeserializationError err = deserializeJson(
    doc, payload, length, DeserializationOption::Filter(mqttFilter));
  if (err) {
    Serial.printf("JSON invalido (%u bytes): %s\n", length, err.c_str());
    return;
  }

  const String line = oledText(doc["line"] | "");
  if (!line.length()) {
    Serial.println("Mensagem ignorada: sem linha.");
    return;
  }
  const char* source = doc["source"] | "";
  const bool nextGpsOnly = strcmp(source, "olho_vivo_gps") == 0;
  const bool nextNoService = doc["noService"] | false;
  const int nextEta = (nextGpsOnly || nextNoService) ? -1 : (doc["etaMinutes"] | -1);
  const int nextCount = doc["vehicleCount"] | -1;
  const char* nextArrival = doc["arrivalTime"] | "";

  const unsigned long now = millis();
  const bool approaching = nextEta == 0 || nextEta == 1;
  const bool wasApproaching = etaMinutes == 0 || etaMinutes == 1;
  // A mesma previsao pode chegar varias vezes pelo MQTT.
  const bool newArrival = approaching &&
    (!dataFresh(now) || !wasApproaching || busLine != line ||
     (arrivalTime.length() && *nextArrival && arrivalTime != nextArrival &&
      !(etaMinutes == 1 && nextEta == 0)));

  const bool nextSimulation = strcmp(source, "teste") == 0 ||
                              strcmp(source, "simulador") == 0 ||
                              strcmp(source, "banco_oracle") == 0;

  if (newArrival) {
    demoStarted = now;
    demoRunning = true;
    Serial.printf("OLED: animacao de chegada (%d min).\n", nextEta);
  } else if (!approaching) {
    demoRunning = false;
  }

  simulationData = nextSimulation;
  vehicleId = oledText(doc["vehicleId"] | "");
  busLine = line;
  busName = oledText(doc["busName"] | "");
  if (!busName.length()) busName = oledText(doc["destination"] | "");
  arrivalTime = nextArrival;
  gpsOnly = nextGpsOnly;
  noService = nextNoService;
  vehicleCount = nextCount;
  etaMinutes = nextEta;
  received = true;
  lastMessage = now;

  if (newArrival) {
    buzzerStart = now;
    buzzerActive = true;
    Serial.printf("Alerta: chegada prevista em %d min.\n", nextEta);
  }

  Serial.printf("MQTT recebido (%u bytes) | Linha: %s | Nome: %s",
                length, busLine.c_str(), busName.length() ? busName.c_str() : "(ausente)");
  if (gpsOnly && vehicleCount >= 0) Serial.printf(" | GPS: %d onibus", vehicleCount);
  if (etaMinutes >= 0) Serial.printf(" | ETA: %d min\n", etaMinutes);
  else Serial.println(" | ETA indisponivel");
  drawScreen();
}

void aoEventoWiFi(WiFiEvent_t evento, WiFiEventInfo_t info) {
  if (evento == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    Serial.printf("Wi-Fi desconectado. Motivo: %u\n",
                  unsigned(info.wifi_sta_disconnected.reason));
  }
}

void conectarRedeEncontrada() {
  if (encontrouRede) {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD, canalRede, bssidRede);
  } else {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

void iniciarWiFi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(500);

  uint8_t macAtual[6] = {};
  const bool leuMac = esp_wifi_get_mac(WIFI_IF_STA, macAtual) == ESP_OK;
  bool macZerado = leuMac;
  for (uint8_t octeto : macAtual) {
    if (octeto != 0) macZerado = false;
  }
  if (FORCAR_MAC || macZerado) {
    esp_wifi_set_mac(WIFI_IF_STA, MAC_PERSONALIZADO);
  }

  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  esp_wifi_set_protocol(WIFI_IF_STA,
      WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
  WiFi.onEvent(aoEventoWiFi);

  Serial.println("Procurando rede Wi-Fi...");
  const int quantidade = WiFi.scanNetworks();
  int melhorRSSI = -1000;
  encontrouRede = false;
  for (int i = 0; i < quantidade; ++i) {
    if (WiFi.SSID(i) != WIFI_SSID) continue;
    const int rssi = WiFi.RSSI(i);
    const uint8_t* bssid = WiFi.BSSID(i);
    if (!bssid || rssi <= melhorRSSI) continue;
    melhorRSSI = rssi;
    canalRede = WiFi.channel(i);
    memcpy(bssidRede, bssid, sizeof bssidRede);
    encontrouRede = true;
  }
  WiFi.scanDelete();
  if (encontrouRede) {
    Serial.printf("Rede encontrada: canal %d, sinal %d dBm\n", canalRede, melhorRSSI);
  } else {
    Serial.printf("Rede nao encontrada na busca (resultado %d). Tentando pelo SSID.\n", quantidade);
  }
  Serial.println("Iniciando conexao Wi-Fi...");
  conectarRedeEncontrada();
  lastWifiAttempt = millis();
}

void checkWifi(unsigned long now) {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiWasConnected) {
      Serial.print("Wi-Fi conectado. IP: ");
      Serial.println(WiFi.localIP());
    }
    wifiWasConnected = true;
    return;
  }
  if (wifiWasConnected) wifiClient.stop();
  wifiWasConnected = false;
  if (now - lastWifiAttempt >= 10000UL) {
    lastWifiAttempt = now;
    Serial.println("Tentando reconectar Wi-Fi...");
    WiFi.disconnect(false, false);
    delay(50);
    conectarRedeEncontrada();
  }
}

void checkMqtt(unsigned long now) {
  if (!mqttBufferReady || WiFi.status() != WL_CONNECTED) return;
  if (mqtt.connected()) {
    mqtt.loop();
    return;
  }
  if (buzzerActive || now - lastMqttAttempt < 5000UL) return;
  lastMqttAttempt = now;
  String id = "SmartBusESP32-" + WiFi.macAddress();
  id.replace(":", "");
  Serial.print("Conectando ao MQTT... ");
  if (!mqtt.connect(id.c_str())) {
    Serial.printf("falhou, codigo %d\n", mqtt.state());
    return;
  }
  Serial.println("MQTT conectado!");
  if (mqtt.subscribe(MQTT_TOPIC)) {
    Serial.print("Inscricao solicitada: ");
    Serial.println(MQTT_TOPIC);
  } else {
    Serial.println("Falha ao enviar inscricao MQTT.");
    wifiClient.stop();
  }
}

void updateSensor() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  const unsigned long us = pulseIn(ECHO_PIN, HIGH, 25000UL);
  distanceCm = us ? us * 0.0343f / 2.0f : -1.0f;
}

void updateDisplay(unsigned long now) {
  if (!oledReady) return;
  if (demoRunning && now - demoStarted >= DEMO_SCREEN_MS) demoRunning = false;
  const bool shouldShow = demoRunning ||
      (distanceCm > 0 && distanceCm <= DISPLAY_RANGE_CM);
  if (shouldShow != screenOn) {
    screenOn = shouldShow;
    if (screenOn) {
      drawScreen();
      lastFrame = now;
      display.ssd1306_command(SSD1306_DISPLAYON);
      return;
    }
    display.ssd1306_command(SSD1306_DISPLAYOFF);
  }
  const unsigned long refreshMs = demoRunning ? DEMO_FRAME_MS : 300UL;
  if (screenOn && now - lastFrame >= refreshMs) {
    lastFrame = now;
    drawScreen();
  }
}

void updateBuzzer() {
  if (!buzzerActive) return;
  const unsigned long step = (millis() - buzzerStart) / BEEP_MS;
  if (step >= 5) {
    digitalWrite(BUZZER_PIN, LOW);
    buzzerActive = false;
  } else {
    digitalWrite(BUZZER_PIN, (step % 2 == 0) ? HIGH : LOW);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(TRIG_PIN, OUTPUT);
  digitalWrite(TRIG_PIN, LOW);
  pinMode(ECHO_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  Wire.begin(OLED_SDA, OLED_SCL);
  oledReady = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR, false, false);
  if (oledReady) display.ssd1306_command(SSD1306_DISPLAYOFF);
  else Serial.println("Falha ao inicializar OLED.");

  mqttFilter["line"] = true;
  mqttFilter["busName"] = true;
  mqttFilter["destination"] = true;
  mqttFilter["etaMinutes"] = true;
  mqttFilter["noService"] = true;
  mqttFilter["source"] = true;
  mqttFilter["vehicleCount"] = true;
  mqttFilter["arrivalTime"] = true;
  mqttFilter["vehicleId"] = true;

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setSocketTimeout(3);
  mqttBufferReady = mqtt.setBufferSize(MQTT_BUFFER);
  if (mqttBufferReady) Serial.printf("Buffer MQTT: %u bytes\n", unsigned(MQTT_BUFFER));
  else Serial.println("Falha ao reservar buffer MQTT.");

  iniciarWiFi();
  lastMqttAttempt = millis() - 5000UL;
}

void loop() {
  const unsigned long now = millis();
  checkWifi(now);
  checkMqtt(now);
  updateBuzzer();
  if (now - lastSensor >= 300UL) {
    lastSensor = now;
    updateSensor();
  }
  // A recepcao MQTT pode ter iniciado a animacao depois do millis() do topo.
  updateDisplay(millis());
  delay(5);
}
