#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <Adafruit_BMP280.h>
#include <LittleFS.h>
#include <vector>

// --- НАСТРОЙКА СВЕТОДИОДА ---
#ifndef LED_BUILTIN
  #define LED_BUILTIN 2 
#endif

// --- ПЕРЕМЕННЫЕ КОНФИГУРАЦИИ (ЗАГРУЖАЮТСЯ ИЗ JSON) ---
String wifi_ssid;
String wifi_pass;
String vk_token;
String group_id;
std::vector<long> admin_peer_ids;
int i2c_sda = 21;
int i2c_scl = 22;

// --- СЕНСОРЫ И ИХ ФЛАГИ ---
Adafruit_AHTX0 aht;
Adafruit_BMP280 bmp;

bool isAhtConnected = false;
bool isBmpConnected = false;

// --- ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ---
String server;
String key;
String ts;
bool isBootMessageSent = false;

WiFiClientSecure client;

// Загрузка параметров из config.json через LittleFS
bool loadConfig() {
  if (!LittleFS.begin(true)) {
    Serial.println("[FS] Ошибка монтирования LittleFS!");
    return false;
  }

  File configFile = LittleFS.open("/config.json", "r");
  if (!configFile) {
    Serial.println("[FS] Не удалось открыть config.json!");
    return false;
  }

  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, configFile);
  configFile.close();

  if (error) {
    Serial.printf("[FS] Ошибка парсинга config.json: %s\n", error.c_str());
    return false;
  }

  wifi_ssid = doc["wifi_ssid"].as<String>();
  wifi_pass = doc["wifi_pass"].as<String>();
  vk_token  = doc["vk_token"].as<String>();
  group_id  = doc["group_id"].as<String>();
  i2c_sda   = doc["i2c_sda"] | 21;
  i2c_scl   = doc["i2c_scl"] | 22;

  admin_peer_ids.clear();
  JsonArray admins = doc["admin_peer_ids"].as<JsonArray>();
  for (JsonVariant v : admins) {
    admin_peer_ids.push_back(v.as<long>());
  }

  Serial.println("[FS] Конфигурация успешно загружена!");
  return true;
}

// Мигание встроенным светодиодом на 1 секунду
void blinkSuccess() {
  digitalWrite(LED_BUILTIN, HIGH);
  delay(1000);
  digitalWrite(LED_BUILTIN, LOW);
}

// Проверка физического присутствия устройства на шине I2C по его адресу
bool isI2CDeviceConnected(uint8_t address) {
  Wire.beginTransmission(address);
  return (Wire.endTransmission() == 0);
}

// Функция для формирования строки с показаниями датчиков
String getSensorsReport() {
  blinkSuccess();

  String report = "📊 Данные с датчиков:\n\n";

  // --- 1. Проверка и опрос AHT20 (I2C адрес 0x38) ---
  if (isAhtConnected && isI2CDeviceConnected(0x38)) {
    sensors_event_t humidity, temp;
    if (aht.getEvent(&humidity, &temp) && !isnan(temp.temperature) && temp.temperature > -50.0f && temp.temperature < 100.0f) {
      report += "🌡 Температура: " + String(temp.temperature, 1) + " °C\n";
      report += "💧 Влажность: " + String(humidity.relative_humidity, 1) + " %\n";
    } else {
      report += "⚠️ Ошибка AHT20: сбой чтения данных\n";
    }
  } else {
    report += "⚠️ Датчик AHT20 не подключен!\n";
  }

  // --- 2. Проверка и опрос BMP280 (I2C адрес 0x76 или 0x77) ---
  uint8_t bmpAddress = 0;
  if (isI2CDeviceConnected(0x76)) bmpAddress = 0x76;
  else if (isI2CDeviceConnected(0x77)) bmpAddress = 0x77;

  if (isBmpConnected && bmpAddress != 0) {
    float pressure = bmp.readPressure() / 133.322f; // Перевод в мм рт. ст.
    
    // Проверка на физическую реалистичность атмосферного давления (от 300 до 900 мм рт. ст.)
    if (!isnan(pressure) && pressure >= 300.0f && pressure <= 900.0f) {
      report += "🎈 Давление: " + String(pressure, 1) + " мм рт. ст.\n";
    } else {
      report += "⚠️ Ошибка BMP280: нереалистичные показания\n";
    }
  } else {
    report += "⚠️ Датчик BMP280 не подключен!\n";
  }

  return report;
}

// URL-кодирование
String urlEncode(String str) {
  String encodedString = "";
  char c, code0, code1;
  for (int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encodedString += c;
    } else {
      code0 = (c >> 4) & 0xF;
      code1 = c & 0xF;
      encodedString += '%';
      encodedString += (char)(code0 < 10 ? code0 + '0' : code0 - 10 + 'A');
      encodedString += (char)(code1 < 10 ? code1 + '0' : code1 - 10 + 'A');
    }
  }
  return encodedString;
}

// Отправка сообщения пользователю VK
void sendMessage(long peer_id, String message) {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.setTimeout(5000);
  String url = "https://api.vk.com/method/messages.send";
  
  String postData = "peer_id=" + String(peer_id) +
                    "&message=" + urlEncode(message) +
                    "&random_id=" + String(random(0, 100000000)) +
                    "&access_token=" + vk_token +
                    "&v=5.131";

  if (http.begin(client, url)) {
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    int httpCode = http.POST(postData);
    Serial.printf("[VK] Отправка сообщения пользователю %ld. Ответ: %d\n", peer_id, httpCode);
    http.end();
  }
}

// Отправка рассылки администраторам
void sendBroadcast(String message) {
  for (long admin_id : admin_peer_ids) {
    sendMessage(admin_id, message);
    delay(200); 
  }
}

// Получение Long Poll сервера VK
bool getLongPollServer() {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.setTimeout(5000);

  String url = "https://api.vk.com/method/groups.getLongPollServer?group_id=" + 
               group_id + "&access_token=" + vk_token + "&v=5.131";

  if (!http.begin(client, url)) return false;

  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    DynamicJsonDocument doc(2048);
    deserializeJson(doc, payload);

    if (doc.containsKey("response")) {
      server = doc["response"]["server"].as<String>();
      key = doc["response"]["key"].as<String>();
      ts = doc["response"]["ts"].as<String>();
      
      if (!isBootMessageSent) {
        bool currentAhtOk = isAhtConnected && isI2CDeviceConnected(0x38);
        bool currentBmpOk = isBmpConnected && (isI2CDeviceConnected(0x76) || isI2CDeviceConnected(0x77));

        String bootMessage = (!currentAhtOk || !currentBmpOk)
          ? "⚠️ Устройство ESP32 запущено, НО обнаружены проблемы с датчиками!\n\n"
          : "🚀 Устройство ESP32 успешно запущено и готово к работе!\n\n";

        bootMessage += getSensorsReport();
        sendBroadcast(bootMessage);
        isBootMessageSent = true;
      }

      http.end();
      return true;
    }
  }

  http.end();
  return false;
}

// Проверка текста на команду получения показаний
bool isSensorCommand(String text) {
  text.toLowerCase();
  text.trim();
  return (text.indexOf("датчик") != -1 || text.indexOf("датчики") != -1 || text.indexOf("Датчик") != -1 || text.indexOf("Датчики") != -1);
}

// Проверка текста на команду перезагрузки
bool isRebootCommand(String text) {
  text.toLowerCase();
  text.trim();
  return (text.indexOf("перезагрузка") != -1 || text.indexOf("Перезагрузка") != -1);
}

// Проверка входящих сообщений Long Poll
void checkLongPoll() {
  if (server == "" || key == "" || ts == "") {
    if (!getLongPollServer()) {
      delay(3000);
      return;
    }
  }

  HTTPClient http;
  http.setTimeout(30000);

  String url = server + "?act=a_check&key=" + key + "&ts=" + ts + "&wait=25";

  if (!http.begin(client, url)) {
    server = "";
    return;
  }

  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    DynamicJsonDocument doc(4096);
    deserializeJson(doc, payload);

    if (doc.containsKey("failed")) {
      server = "";
      http.end();
      return;
    }

    if (doc.containsKey("ts")) ts = doc["ts"].as<String>();

    if (doc.containsKey("updates")) {
      JsonArray updates = doc["updates"].as<JsonArray>();
      for (JsonObject update : updates) {
        if (update["type"] == "message_new") {
          JsonObject message = update["object"]["message"];
          long peer_id = message["peer_id"];
          String text = message["text"].as<String>();

          // 1. Команда получения показаний
          if (isSensorCommand(text)) {
            sendMessage(peer_id, getSensorsReport());
          } 
          // 2. Команда дистанционной перезагрузки
          else if (isRebootCommand(text)) {
            sendMessage(peer_id, "🔄 Выполняется перезагрузка ESP32...");
            delay(1000); // Даем время HTTP-клиенту полностью завершить отправку пакета
            ESP.restart();
          }
        }
      }
    }
  } else {
    delay(2000);
  }
  http.end();
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  // 1. Загрузка конфигурации из JSON через LittleFS
  if (!loadConfig()) {
    Serial.println("Остановка выполнения: не удалось загрузить конфиг.");
    return;
  }

  // 2. Инициализация I2C пинов из конфига
  Wire.begin(i2c_sda, i2c_scl);

  // 3. Первичная проверка инициализации датчиков
  if (aht.begin()) {
    isAhtConnected = true;
    Serial.println("AHT20 успешно подключен.");
  } else {
    Serial.println("Ошибка: AHT20 не найден!");
  }

  if (bmp.begin(0x77) || bmp.begin(0x76)) {
    isBmpConnected = true;
    Serial.println("BMP280 успешно подключен.");
  } else {
    Serial.println("Ошибка: BMP280 не найден!");
  }

  // 4. Подключение к Wi-Fi из конфига
  WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
  Serial.print("Подключение к Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWi-Fi подключен!");

  client.setInsecure();
  client.setTimeout(5000);

  getLongPollServer();
}

void loop() {
  checkLongPoll();
}