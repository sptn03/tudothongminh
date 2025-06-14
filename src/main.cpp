#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <vector>

// MQTT configuration
const char* mqtt_server = "n8n.nz03.com";
const char* mqtt_client_id = "esp32_client";
const char* mqtt_topic_sub = "locker/commands";
const char* mqtt_topic_pub = "locker/responses";

// LED pin - ESP32 có LED tích hợp ở chân GPIO2
const int LED_BUILTIN = 2;

WiFiClient espClient;
PubSubClient client(espClient);
WiFiManager wifiManager;

unsigned long lastReconnectAttempt = 0;
const long reconnectInterval = 5000; // Try to reconnect every 5 seconds

// LED blink control
unsigned long lastLedToggle = 0;
int ledState = LOW;
int ledBlinkInterval = 1000; // Mặc định 1s/lần

// Locker control
const long lockerOpenTime = 5000; // 5 seconds

// Cấu trúc để lưu trữ thông tin về mỗi tủ đang mở
struct LockerInfo {
  int gpio;
  unsigned long openedAt;
  String lockerId;
};

// Vector để lưu trữ danh sách các tủ đang mở
std::vector<LockerInfo> openLockers;

// WiFiManager parameters
char mqtt_server_param[40] = "n8n.nz03.com";
char mqtt_port_param[6] = "1883";
WiFiManagerParameter custom_mqtt_server("server", "MQTT Server", mqtt_server_param, 40);
WiFiManagerParameter custom_mqtt_port("port", "MQTT Port", mqtt_port_param, 6);

// Thêm vào phần khai báo biến toàn cục
const int RESET_BUTTON_PIN = 0; // Sử dụng chân GPIO0 (nút BOOT trên ESP32)

// Cập nhật trạng thái LED dựa trên trạng thái kết nối
void updateLedStatus() {
  unsigned long currentMillis = millis();
  
  // Nếu đang ở chế độ AP (WiFiManager config portal)
  if (wifiManager.getConfigPortalActive()) {
    ledBlinkInterval = 100; // Nhấp nháy nhanh 100ms/lần
  }
  // Nếu đã kết nối WiFi nhưng chưa kết nối MQTT
  else if (WiFi.status() == WL_CONNECTED && !client.connected()) {
    ledBlinkInterval = 1000; // Nhấp nháy 1s/lần
  }
  // Nếu đã kết nối cả WiFi và MQTT
  else if (WiFi.status() == WL_CONNECTED && client.connected()) {
    digitalWrite(LED_BUILTIN, HIGH); // Bật đèn LED liên tục
    return; // Thoát khỏi hàm, không cần nhấp nháy
  }
  
  // Nhấp nháy LED theo khoảng thời gian đã đặt
  if (currentMillis - lastLedToggle >= ledBlinkInterval) {
    lastLedToggle = currentMillis;
    ledState = (ledState == LOW) ? HIGH : LOW;
    digitalWrite(LED_BUILTIN, ledState);
  }
}

void openLocker(int gpio, const String& lockerId) {
  pinMode(gpio, OUTPUT);
  digitalWrite(gpio, HIGH);
  
  // Thêm tủ mới vào danh sách các tủ đang mở
  LockerInfo newLocker;
  newLocker.gpio = gpio;
  newLocker.openedAt = millis();
  newLocker.lockerId = lockerId;
  openLockers.push_back(newLocker);
  
  Serial.printf("Locker %s opened (GPIO %d set to HIGH)\n", lockerId.c_str(), gpio);
}

void closeLocker(int index) {
  if (index >= 0 && index < openLockers.size()) {
    LockerInfo& locker = openLockers[index];
    digitalWrite(locker.gpio, LOW);
    
    // Gửi thông báo trạng thái
    DynamicJsonDocument response(1024);
    response["success"] = true;
    response["locker_id"] = locker.lockerId;
    response["status"] = "closed";
    response["gpio"] = locker.gpio;

    String response_string;
    serializeJson(response, response_string);
    
    String pub_topic = String(mqtt_topic_pub) + "/" + locker.lockerId;
    client.publish(pub_topic.c_str(), response_string.c_str());
    
    Serial.printf("Locker %s closed (GPIO %d set to LOW)\n", locker.lockerId.c_str(), locker.gpio);
    
    // Xóa tủ khỏi danh sách
    openLockers.erase(openLockers.begin() + index);
  }
}

void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  
  String message;
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.println(message);

  DynamicJsonDocument doc(1024);
  deserializeJson(doc, message);

  const char* action = doc["action"];
  const char* locker_id = doc["locker_id"];
  int gpio = doc["gpio"];

  if (action && locker_id && gpio) {
    openLocker(gpio, String(locker_id));

    // Send status response
    DynamicJsonDocument response(1024);
    response["success"] = true;
    response["locker_id"] = locker_id;
    response["status"] = "open";
    response["gpio"] = gpio;

    String response_string;
    serializeJson(response, response_string);
    
    String pub_topic = String(mqtt_topic_pub) + "/" + String(locker_id);
    client.publish(pub_topic.c_str(), response_string.c_str());
    Serial.print("Published status: ");
    Serial.println(response_string);
  }
}

boolean reconnect() {
  if (client.connect(mqtt_client_id)) {
    Serial.println("MQTT connected");
    client.subscribe(mqtt_topic_sub);
    return true;
  }
  return false;
}

void saveParamsCallback() {
  Serial.println("Params saved");
  strcpy(mqtt_server_param, custom_mqtt_server.getValue());
  strcpy(mqtt_port_param, custom_mqtt_port.getValue());
}

void configModeCallback(WiFiManager *myWiFiManager) {
  Serial.println("Entered config mode");
  Serial.println(WiFi.softAPIP());
  Serial.println(myWiFiManager->getConfigPortalSSID());
}

void setup() {
  Serial.begin(115200);
  
  // Cấu hình LED
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  
  // WiFiManager setup
  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_mqtt_port);
  wifiManager.setSaveParamsCallback(saveParamsCallback);
  wifiManager.setAPCallback(configModeCallback);
  
  // Uncomment to reset settings - for testing
  // wifiManager.resetSettings();
  
  bool res = wifiManager.autoConnect("ESP32_AP");
  
  if(!res) {
    Serial.println("Failed to connect");
    ESP.restart();
  } else {
    Serial.println("WiFi connected");
    Serial.println("IP address: ");
    Serial.println(WiFi.localIP());
  }
  
  // Setup MQTT
  client.setServer(mqtt_server_param, atoi(mqtt_port_param));
  client.setCallback(callback);

  // Thêm vào hàm setup()
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
}

void loop() {
  // Cập nhật trạng thái LED
  updateLedStatus();
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi connection lost. Reconnecting...");
    wifiManager.autoConnect("ESP32_AP");
  }

  if (!client.connected()) {
    unsigned long now = millis();
    if (now - lastReconnectAttempt > reconnectInterval) {
      lastReconnectAttempt = now;
      if (reconnect()) {
        lastReconnectAttempt = 0;
      }
    }
  } else {
    client.loop();
  }

  // Kiểm tra tất cả các tủ đang mở
  unsigned long currentTime = millis();
  for (int i = openLockers.size() - 1; i >= 0; i--) {
    if (currentTime - openLockers[i].openedAt > lockerOpenTime) {
      closeLocker(i);
    }
  }

  // Thêm vào hàm loop()
  if (digitalRead(RESET_BUTTON_PIN) == LOW) {
    delay(5000); // Debounce
    if (digitalRead(RESET_BUTTON_PIN) == LOW) {
      Serial.println("Reset button pressed");
      wifiManager.resetSettings();
      ESP.restart();
    }
  }

  delay(10);  // Small delay to prevent watchdog reset
}
