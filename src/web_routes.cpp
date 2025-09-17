// web_routes.cpp - Düzeltilmiş ve Temizlenmiş Routing
#include "web_routes.h"
#include "auth_system.h"
#include "settings.h"
#include "ntp_handler.h"
#include "uart_handler.h"
#include "log_system.h"
#include "backup_restore.h"
#include "password_policy.h"
#include <LittleFS.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include "datetime_handler.h"
#include "fault_parser.h"

extern DateTimeData datetimeData;

// UART istatistikleri - extern olarak kullan (uart_handler.cpp'de tanımlı)
extern UARTStatistics uartStats;  // DÜZELTME: Burada tanımlama değil, extern kullanım

// Rate limiting için global değişkenler
struct RateLimitData {
    IPAddress clientIP;
    unsigned long requests[20];
    int requestIndex = 0;
    unsigned long lastReset = 0;
};
RateLimitData rateLimitData;

extern String getCurrentDateTime();
extern String getUptime();
extern bool isTimeSynced();
extern WebServer server;
extern Settings settings;
extern bool ntpConfigured;
extern PasswordPolicy passwordPolicy;
extern int logIndex;

// Arıza kayıtları için global array
static FaultRecord faultRecords[100]; // Maksimum 100 arıza kaydı
static int faultCount = 0;


// Security headers ekle
void addSecurityHeaders() {
    server.sendHeader("X-Content-Type-Options", "nosniff");
    server.sendHeader("X-Frame-Options", "DENY");
    server.sendHeader("X-XSS-Protection", "1; mode=block");
    server.sendHeader("Referrer-Policy", "strict-origin-when-cross-origin");
    server.sendHeader("Content-Security-Policy", "default-src 'self' 'unsafe-inline'");
}

// Rate limiting kontrolü
bool checkRateLimit() {
    IPAddress clientIP = server.client().remoteIP();
    unsigned long now = millis();
    
    // Farklı IP veya 1 dakika geçmişse sıfırla
    if (clientIP != rateLimitData.clientIP || now - rateLimitData.lastReset > 60000) {
        rateLimitData.clientIP = clientIP;
        rateLimitData.requestIndex = 0;
        rateLimitData.lastReset = now;
    }
    
    // 1 dakikada 60 istekten fazlasına izin verme
    if (rateLimitData.requestIndex >= 20) {
        addLog("⚠️ Rate limit aşıldı: " + clientIP.toString(), WARN, "SECURITY");
        return false;
    }
    
    rateLimitData.requests[rateLimitData.requestIndex++] = now;
    return true;
}

// Device Info API
void handleDeviceInfoAPI() {
    JsonDocument doc;
    doc["ip"] = ETH.localIP().toString();
    doc["mac"] = ETH.macAddress();
    doc["hostname"] = "teias-eklim";
    doc["mdns"] = "teias-eklim.local";
    doc["version"] = "v5.2";
    doc["model"] = "WT32-ETH01";
    
    String output;
    serializeJson(doc, output);
    
    addSecurityHeaders();
    server.send(200, "application/json", output);
}

// System Info API (Auth gerekli)
void handleSystemInfoAPI() {
    if (!checkSession()) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    
    // Rate limiting
    if (!checkRateLimit()) {
        server.send(429, "application/json", "{\"error\":\"Too many requests\"}");
        return;
    }
    
    JsonDocument doc;
    
    // Hardware info
    doc["hardware"]["chip"] = "ESP32";
    doc["hardware"]["cores"] = 2;
    doc["hardware"]["frequency"] = getCpuFrequencyMhz();
    doc["hardware"]["revision"] = ESP.getChipRevision();
    doc["hardware"]["flashSize"] = ESP.getFlashChipSize();
    
    // Memory info
    doc["memory"]["totalHeap"] = ESP.getHeapSize();
    doc["memory"]["freeHeap"] = ESP.getFreeHeap();
    doc["memory"]["usedHeap"] = ESP.getHeapSize() - ESP.getFreeHeap();
    doc["memory"]["minFreeHeap"] = ESP.getMinFreeHeap();
    doc["memory"]["maxAllocHeap"] = ESP.getMaxAllocHeap();
    
    // Software info
    doc["software"]["version"] = "5.2";
    doc["software"]["sdk"] = ESP.getSdkVersion();
    doc["software"]["buildDate"] = __DATE__ " " __TIME__;
    doc["software"]["uptime"] = millis() / 1000;
    
    // UART statistics - uartStats extern olarak kullanılıyor
    doc["uart"]["txCount"] = uartStats.totalFramesSent;
    doc["uart"]["rxCount"] = uartStats.totalFramesReceived;
    doc["uart"]["errors"] = uartStats.frameErrors + uartStats.checksumErrors + uartStats.timeoutErrors;
    doc["uart"]["successRate"] = uartStats.successRate;
    doc["uart"]["baudRate"] = 250000;  // settings.currentBaudRate yerine sabit değer
    
    // File system info
    size_t totalBytes = LittleFS.totalBytes();
    size_t usedBytes = LittleFS.usedBytes();
    doc["filesystem"]["type"] = "LittleFS";
    doc["filesystem"]["total"] = totalBytes;
    doc["filesystem"]["used"] = usedBytes;
    doc["filesystem"]["free"] = totalBytes - usedBytes;
    
    String output;
    serializeJson(doc, output);
    
    addSecurityHeaders();
    server.send(200, "application/json", output);
}

// Network Configuration API - GET
void handleGetNetworkAPI() {
    if (!checkSession()) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    
    addSecurityHeaders();
    
    JsonDocument doc;
    
    // Mevcut ethernet durumu
    doc["linkUp"] = ETH.linkUp();
    doc["linkSpeed"] = ETH.linkSpeed();
    doc["fullDuplex"] = ETH.fullDuplex();
    doc["mac"] = ETH.macAddress();
    
    // IP bilgileri
    doc["ip"] = ETH.localIP().toString();
    doc["gateway"] = ETH.gatewayIP().toString();
    doc["subnet"] = ETH.subnetMask().toString();
    doc["dns1"] = ETH.dnsIP().toString();
    doc["dns2"] = ETH.dnsIP(1).toString();
    
    // Şu an için her zaman static IP olarak göster
    doc["dhcp"] = false;
    
    String output;
    serializeJson(doc, output);
    
    server.send(200, "application/json", output);
}

// Network Configuration API - POST (Basit versiyon)
void handlePostNetworkAPI() {
    if (!checkSession()) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    
    addSecurityHeaders();
    
    String ipMode = server.arg("ipMode");
    
    if (ipMode == "static") {
        String staticIP = server.arg("staticIP");
        String gateway = server.arg("gateway");
        String subnet = server.arg("subnet");
        String dns1 = server.arg("dns1");
        
        // Basit IP validation
        IPAddress testIP;
        if (!testIP.fromString(staticIP)) {
            server.send(400, "application/json", "{\"error\":\"Geçersiz IP adresi\"}");
            return;
        }
        
        if (!testIP.fromString(gateway)) {
            server.send(400, "application/json", "{\"error\":\"Geçersiz Gateway adresi\"}");
            return;
        }
        
        // Settings'e kaydet
        Preferences prefs;
        prefs.begin("app-settings", false);
        prefs.putString("local_ip", staticIP);
        prefs.putString("gateway", gateway);
        prefs.putString("subnet", subnet);
        prefs.putString("dns", dns1);
        prefs.end();
        
        // Global settings güncelle
        settings.local_IP.fromString(staticIP);
        settings.gateway.fromString(gateway);
        settings.subnet.fromString(subnet);
        settings.primaryDNS.fromString(dns1);
        
        addLog("✅ Network ayarları kaydedildi: " + staticIP, SUCCESS, "NETWORK");
        
        server.send(200, "application/json", "{\"success\":true,\"message\":\"Ayarlar kaydedildi. Cihaz yeniden başlatılıyor...\"}");
        
        // Yeniden başlat
        delay(1000);
        ESP.restart();
        
    } else {
        server.send(400, "application/json", "{\"error\":\"Sadece static IP destekleniyor\"}");
    }
}

// Notification API
void handleNotificationAPI() {
    if (!checkSession()) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    
    JsonDocument doc;
    JsonArray notifications = doc.to<JsonArray>();
    
    // Son kritik logları bildirim olarak göster
    extern LogEntry logs[50];
    extern int totalLogs;
    int notificationCount = 0;
    
    for (int i = 0; i < totalLogs && notificationCount < 10; i++) {
        int idx = (logIndex - 1 - i + 50) % 50;
        if (logs[idx].level == ERROR || logs[idx].level == WARN) {
            JsonObject notif = notifications.add<JsonObject>();
            notif["id"] = idx;
            notif["type"] = (logs[idx].level == ERROR) ? "error" : "warning";
            notif["message"] = logs[idx].message;
            notif["time"] = logs[idx].timestamp;
            notif["read"] = false;
            notificationCount++;
        }
    }
    
    doc["count"] = notificationCount;
    
    String output;
    serializeJson(doc, output);
    
    addSecurityHeaders();
    server.send(200, "application/json", output);
}

// System Reboot API
void handleSystemRebootAPI() {
    if (!checkSession()) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    
    addLog("🔄 Sistem yeniden başlatılıyor...", WARN, "SYSTEM");
    server.send(200, "application/json", "{\"success\":true,\"message\":\"Sistem 3 saniye içinde yeniden başlatılacak\"}");
    
    delay(3000);
    ESP.restart();
}

// DateTime bilgisi çek - GET /api/datetime
void handleGetDateTimeAPI() {
    if (!checkSession()) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    
    addSecurityHeaders();
    
    JsonDocument doc;
    
    // Mevcut datetime verisi
    doc["isValid"] = isDateTimeDataValid();
    doc["date"] = datetimeData.date;
    doc["time"] = datetimeData.time;
    doc["rawData"] = datetimeData.rawData;
    
    if (datetimeData.lastUpdate > 0) {
        unsigned long elapsed = (millis() - datetimeData.lastUpdate) / 1000;
        doc["lastUpdate"] = String(elapsed) + " saniye önce";
        doc["lastUpdateTimestamp"] = datetimeData.lastUpdate;
    } else {
        doc["lastUpdate"] = "Henüz çekilmedi";
        doc["lastUpdateTimestamp"] = 0;
    }
    
    // ESP32 sistem saati
    doc["esp32DateTime"] = getCurrentESP32DateTime();
    
    String output;
    serializeJson(doc, output);
    
    server.send(200, "application/json", output);
}

// DateTime bilgisi güncelle - POST /api/datetime/fetch  
void handleFetchDateTimeAPI() {
    if (!checkSession()) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    
    addSecurityHeaders();
    
    addLog("DateTime bilgisi dsPIC'ten çekiliyor...", INFO, "DATETIME");
    
    bool success = requestDateTimeFromDsPIC();
    
    JsonDocument doc;
    doc["success"] = success;
    
    if (success) {
        doc["message"] = "Tarih-saat bilgisi başarıyla güncellendi";
        doc["date"] = datetimeData.date;
        doc["time"] = datetimeData.time;
        doc["rawData"] = datetimeData.rawData;
    } else {
        doc["message"] = "Tarih-saat bilgisi alınamadı";
        doc["error"] = "dsPIC'ten yanıt alınamadı veya format geçersiz";
    }
    
    String output;
    serializeJson(doc, output);
    
    server.send(success ? 200 : 500, "application/json", output);
}

// DateTime ayarla - POST /api/datetime/set
void handleSetDateTimeAPI() {
    if (!checkSession()) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    
    addSecurityHeaders();
    
    String manualDate = server.arg("manualDate");  // Format: 2025-02-27
    String manualTime = server.arg("manualTime");  // Format: 11:22:33
    
    // Input validation
    if (manualDate.length() == 0 || manualTime.length() == 0) {
        server.send(400, "application/json", "{\"error\":\"Tarih ve saat alanları boş olamaz\"}");
        return;
    }
    
    if (!validateDateTime(manualDate, manualTime)) {
        server.send(400, "application/json", "{\"error\":\"Geçersiz tarih veya saat formatı\"}");
        return;
    }
    
    addLog("Manual tarih-saat ayarlanıyor: " + manualDate + " " + manualTime, INFO, "DATETIME");
    
    bool success = setDateTimeToDsPIC(manualDate, manualTime);
    
    JsonDocument doc;
    doc["success"] = success;
    
    if (success) {
        doc["message"] = "Tarih-saat başarıyla ayarlandı";
        doc["setDate"] = manualDate;
        doc["setTime"] = manualTime;
        doc["timeCommand"] = formatTimeCommand(manualTime);
        doc["dateCommand"] = formatDateCommand(manualDate);
    } else {
        doc["message"] = "Tarih-saat ayarlanamadı";
        doc["error"] = "Komut gönderimi başarısız";
    }
    
    String output;
    serializeJson(doc, output);
    
    server.send(success ? 200 : 500, "application/json", output);
}

// ESP32 saati ile senkronize et - POST /api/datetime/sync-esp32
void handleSyncESP32API() {
    if (!checkSession()) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    
    addSecurityHeaders();
    
    addLog("ESP32 saati ile senkronizasyon başlatılıyor", INFO, "DATETIME");
    
    bool success = syncWithESP32Time();
    
    JsonDocument doc;
    doc["success"] = success;
    doc["esp32DateTime"] = getCurrentESP32DateTime();
    
    if (success) {
        doc["message"] = "ESP32 saati ile senkronizasyon tamamlandı";
    } else {
        doc["message"] = "Senkronizasyon başarısız";
        doc["error"] = "ESP32 sistem saati alınamadı veya komut gönderimi başarısız";
    }
    
    String output;
    serializeJson(doc, output);
    
    server.send(success ? 200 : 500, "application/json", output);
}

// Şimdiki zamanı ayarla (client-side JavaScript Date) - POST /api/datetime/set-current
void handleSetCurrentTimeAPI() {
    if (!checkSession()) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    
    addSecurityHeaders();
    
    // Client'tan gelen timestamp (JavaScript Date.now())
    String clientTimestamp = server.arg("timestamp");
    
    if (clientTimestamp.length() == 0) {
        server.send(400, "application/json", "{\"error\":\"Timestamp parametresi gerekli\"}");
        return;
    }
    
    // Client timestamp'i parse et (milisaniye)
    unsigned long long timestamp = clientTimestamp.toInt();
    time_t clientTime = timestamp / 1000; // Saniyeye çevir
    
    struct tm* timeinfo = localtime(&clientTime);
    
    char dateBuffer[16];
    char timeBuffer[16];
    strftime(dateBuffer, sizeof(dateBuffer), "%Y-%m-%d", timeinfo);
    strftime(timeBuffer, sizeof(timeBuffer), "%H:%M:%S", timeinfo);
    
    String currentDate = String(dateBuffer);
    String currentTime = String(timeBuffer);
    
    addLog("Client'tan alınan zaman ayarlanıyor: " + currentDate + " " + currentTime, INFO, "DATETIME");
    
    bool success = setDateTimeToDsPIC(currentDate, currentTime);
    
    JsonDocument doc;
    doc["success"] = success;
    doc["clientDateTime"] = currentDate + " " + currentTime;
    
    if (success) {
        doc["message"] = "Şimdiki zaman başarıyla ayarlandı";
    } else {
        doc["message"] = "Zaman ayarlaması başarısız";
    }
    
    String output;
    serializeJson(doc, output);
    
    server.send(success ? 200 : 500, "application/json", output);
}

// Komut geçmişi - GET /api/datetime/history
void handleDateTimeHistoryAPI() {
    if (!checkSession()) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    
    addSecurityHeaders();
    
    String historyJSON = getCommandHistoryJSON();
    server.send(200, "application/json", historyJSON);
}

// Komut önizleme - POST /api/datetime/preview
void handleDateTimePreviewAPI() {
    if (!checkSession()) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    
    addSecurityHeaders();
    
    String previewDate = server.arg("previewDate");
    String previewTime = server.arg("previewTime");
    
    JsonDocument doc;
    
    if (validateDateTime(previewDate, previewTime)) {
        doc["valid"] = true;
        doc["timeCommand"] = formatTimeCommand(previewTime);
        doc["dateCommand"] = formatDateCommand(previewDate);
        doc["displayDate"] = previewDate;
        doc["displayTime"] = previewTime;
    } else {
        doc["valid"] = false;
        doc["error"] = "Geçersiz tarih veya saat formatı";
    }
    
    String output;
    serializeJson(doc, output);
    
    server.send(200, "application/json", output);
}

// mDNS güncelleme (teias-eklim.local)
void updateMDNS() {
    MDNS.end();
    
    if (MDNS.begin("teias-eklim")) {
        MDNS.addService("http", "tcp", 80);
        addLog("✅ mDNS güncellendi: teias-eklim.local", SUCCESS, "mDNS");
    } else {
        addLog("❌ mDNS başlatılamadı", ERROR, "mDNS");
    }
}

void serveStaticFile(const String& path, const String& contentType) {
    String pathWithGz = path + ".gz";
    if (LittleFS.exists(pathWithGz)) {
        File file = LittleFS.open(pathWithGz, "r");
        server.sendHeader("Content-Encoding", "gzip");
        server.streamFile(file, contentType);
        file.close();
        return;
    }

    if (LittleFS.exists(path)) {
        File file = LittleFS.open(path, "r");
        server.streamFile(file, contentType);
        file.close();
        return;
    }

    server.send(404, "text/plain", "404: Not Found");
}

String getUptime() {
    unsigned long sec = millis() / 1000;
    char buffer[32];
    sprintf(buffer, "%lu:%02lu:%02lu", sec/3600, (sec%3600)/60, sec%60);
    return String(buffer);
}

// API Handler'lar
void handleStatusAPI() {
    if (!checkSession()) {
        server.send(401, "text/plain", "Unauthorized");
        return;
    }
    
    JsonDocument doc;
    doc["datetime"] = getCurrentDateTime();
    doc["uptime"] = getUptime();
    doc["deviceName"] = settings.deviceName;
    doc["tmName"] = settings.transformerStation;
    doc["deviceIP"] = ETH.localIP().toString();
    doc["ethernetStatus"] = ETH.linkUp();
    doc["timeSynced"] = isTimeSynced();
    doc["freeHeap"] = ESP.getFreeHeap();
    doc["totalHeap"] = ESP.getHeapSize();

    String output;
    serializeJson(doc, output);
    server.send(200, "application/json", output);
}

void handleGetSettingsAPI() {
    if (!checkSession()) { server.send(401); return; }
    JsonDocument doc;
    doc["deviceName"] = settings.deviceName;
    doc["tmName"] = settings.transformerStation;
    doc["username"] = settings.username;
    String output;
    serializeJson(doc, output);
    server.send(200, "application/json", output);
}

void handlePostSettingsAPI() {
    if (!checkSession()) { server.send(401); return; }
    if (saveSettings(server.arg("deviceName"), server.arg("tmName"), server.arg("username"), server.arg("password"))) {
        server.send(200, "text/plain", "OK");
    } else {
        server.send(400, "text/plain", "Error");
    }
}

// YENİ: Arıza sayısını al API'si
void handleGetFaultCountAPI() {
    if (!checkSession()) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    
    addLog("📊 Arıza sayısı sorgulanıyor", INFO, "API");
    
    int count = getTotalFaultCount(); // uart_handler.cpp'deki yeni fonksiyon
    
    JsonDocument doc;
    doc["success"] = (count > 0);
    doc["count"] = count;
    doc["message"] = count > 0 ? 
        "Toplam " + String(count) + " arıza bulundu" : 
        "Arıza sayısı alınamadı";
    
    String output;
    serializeJson(doc, output);
    
    addSecurityHeaders();
    server.send(200, "application/json", output);
}

// YENİ: Belirli bir arıza kaydını al
void handleGetSpecificFaultAPI() {
    if (!checkSession()) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    
    String faultNoStr = server.arg("faultNo");
    if (faultNoStr.length() == 0) {
        server.send(400, "application/json", "{\"error\":\"faultNo parameter required\"}");
        return;
    }
    
    int faultNo = faultNoStr.toInt();
    if (faultNo < 1 || faultNo > 9999) {
        server.send(400, "application/json", "{\"error\":\"Invalid fault number\"}");
        return;
    }
    
    addLog("🔍 Arıza " + String(faultNo) + " sorgulanıyor", INFO, "API");
    
    bool success = requestSpecificFault(faultNo);
    
    if (success) {
        String response = getLastFaultResponse();
        
        JsonDocument doc;
        doc["success"] = true;
        doc["faultNo"] = faultNo;
        doc["rawData"] = response;
        doc["length"] = response.length();
        
        String output;
        serializeJson(doc, output);
        
        server.send(200, "application/json", output);
    } else {
        server.send(500, "application/json", 
            "{\"success\":false,\"error\":\"Arıza kaydı alınamadı\"}");
    }
}

// Mevcut handleParsedFaultAPI fonksiyonunu GÜNCELLE
void handleParsedFaultAPI() {
    if (!checkSession()) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    
    String action = server.arg("action");
    
    if (action == "count") {
        // Toplam arıza sayısını döndür
        int count = getTotalFaultCount();
        
        JsonDocument doc;
        doc["success"] = (count > 0);
        doc["count"] = count;
        doc["message"] = count > 0 ? 
            String(count) + " adet arıza bulundu" : 
            "Sistemde arıza kaydı yok";
        
        String output;
        serializeJson(doc, output);
        server.send(200, "application/json", output);
        
    } else if (action == "get") {
        // Belirli bir arıza kaydını al ve parse et
        String faultNoStr = server.arg("faultNo");
        if (faultNoStr.length() == 0) {
            server.send(400, "application/json", "{\"error\":\"faultNo parameter required\"}");
            return;
        }
        
        int faultNo = faultNoStr.toInt();
        if (requestSpecificFault(faultNo)) {
            String rawResponse = getLastFaultResponse();
            FaultRecord fault = parseFaultData(rawResponse);
            
            if (fault.isValid) {
                JsonDocument doc;
                doc["success"] = true;
                doc["faultNo"] = faultNo;
                doc["fault"]["pinNumber"] = fault.pinNumber;
                doc["fault"]["pinType"] = fault.pinType;
                doc["fault"]["pinName"] = fault.pinName;
                doc["fault"]["dateTime"] = fault.dateTime;
                doc["fault"]["duration"] = formatDuration(fault.duration);
                doc["fault"]["durationSeconds"] = fault.duration;
                doc["fault"]["millisecond"] = fault.millisecond;
                doc["fault"]["rawData"] = fault.rawData;
                
                String output;
                serializeJson(doc, output);
                server.send(200, "application/json", output);
            } else {
                server.send(400, "application/json", 
                    "{\"success\":false,\"error\":\"" + fault.errorMessage + "\"}");
            }
        } else {
            server.send(500, "application/json", 
                "{\"success\":false,\"error\":\"Arıza kaydı alınamadı\"}");
        }
        
    } else if (action == "clear") {
        // Arıza kayıtlarını temizle (sadece ESP32 tarafında)
        faultCount = 0;
        server.send(200, "application/json", 
            "{\"success\":true,\"message\":\"Arıza kayıtları temizlendi\"}");
            
    } else {
        server.send(400, "application/json", 
            "{\"error\":\"Invalid action. Use: count, get, or clear\"}");
    }
}

// ✅ handleUARTTestAPI fonksiyonu
void handleUARTTestAPI() {
    if (!checkSession()) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }
    
    addLog("🧪 UART test başlatılıyor...", INFO, "WEB");
    
    JsonDocument doc;
    doc["uartHealthy"] = uartHealthy;
    doc["baudRate"] = 250000;
    
    // Basit test komutu gönder
    String testResponse;
    bool testResult = sendCustomCommand("TEST", testResponse, 2000);
    
    doc["testCommand"] = "TEST";
    doc["testSuccess"] = testResult;
    doc["testResponse"] = testResponse;
    doc["responseLength"] = testResponse.length();
    
    // İstatistikler
    doc["stats"]["sent"] = uartStats.totalFramesSent;
    doc["stats"]["received"] = uartStats.totalFramesReceived;
    doc["stats"]["errors"] = uartStats.frameErrors + uartStats.checksumErrors + uartStats.timeoutErrors;
    doc["stats"]["successRate"] = uartStats.successRate;
    
    String output;
    serializeJson(doc, output);
    
    addSecurityHeaders();
    server.send(200, "application/json", output);
}

void handleGetNtpAPI() {
    if (!checkSession()) { server.send(401); return; }
    JsonDocument doc;
    doc["ntpServer1"] = ntpConfig.ntpServer1;
    doc["ntpServer2"] = ntpConfig.ntpServer2;
    String output;
    serializeJson(doc, output);
    server.send(200, "application/json", output);
}

void handlePostNtpAPI() {
    if (!checkSession()) { server.send(401); return; }
    if (saveNTPSettings(server.arg("ntpServer1"), server.arg("ntpServer2"), 3)) {
        sendNTPConfigToBackend();
        server.send(200, "text/plain", "OK");
    } else {
        server.send(400, "text/plain", "Error");
    }
}

void handleGetBaudRateAPI() {
    if (!checkSession()) { server.send(401); return; }
    JsonDocument doc;
    doc["baudRate"] = settings.currentBaudRate;
    String output;
    serializeJson(doc, output);
    server.send(200, "application/json", output);
}

void handlePostBaudRateAPI() {
    if (!checkSession()) { server.send(401); return; }
    if (changeBaudRate(server.arg("baud").toInt())) {
        server.send(200, "text/plain", "OK");
    } else {
        server.send(500, "text/plain", "Error");
    }
}

void handleGetLogsAPI() {
    if (!checkSession()) { server.send(401); return; }
    
    JsonDocument doc;
    JsonArray logArray = doc.to<JsonArray>();

    extern LogEntry logs[50];
    extern int totalLogs;
    
    for (int i = 0; i < totalLogs; i++) {
        // Logları en yeniden en eskiye doğru sıralamak için indeksi düzeltelim
        int idx = (logIndex - 1 - i + 50) % 50;
        if(logs[idx].message.length() == 0) continue; // Boş logları atla

        JsonObject logEntry = logArray.add<JsonObject>();
        logEntry["t"] = logs[idx].timestamp;
        logEntry["m"] = logs[idx].message;
        logEntry["l"] = logLevelToString(logs[idx].level);
        logEntry["s"] = logs[idx].source;
    }
    
    String output;
    serializeJson(doc, output);
    server.send(200, "application/json", output);
}

// Password change sayfası için token kontrolü (ama atmaz)
void handlePasswordChangeCheck() {
    String token = "";
    if (server.hasHeader("Authorization")) {
        String authHeader = server.header("Authorization");
        if (authHeader.startsWith("Bearer ")) {
            token = authHeader.substring(7);
        }
    }
    
    // Token yoksa veya geçersizse sadece uyarı döndür
    if (token.length() == 0 || settings.sessionToken.length() == 0 || token != settings.sessionToken) {
        server.send(200, "application/json", "{\"validSession\":false,\"message\":\"Oturum geçersiz ama devam edebilirsiniz\"}");
    } else {
        server.send(200, "application/json", "{\"validSession\":true}");
    }
}

void handleClearLogsAPI() {
    if (!checkSession()) { server.send(401); return; }
    clearLogs();
    server.send(200, "text/plain", "OK");
}

void setupWebRoutes() {
    
    server.on("/favicon.ico", HTTP_GET, []() { server.send(204); });
    
    // ANA SAYFALAR (Oturum kontrolü yok, JS halledecek)
    server.on("/", HTTP_GET, []() { serveStaticFile("/index.html", "text/html"); });
    server.on("/login.html", HTTP_GET, []() { serveStaticFile("/login.html", "text/html"); });
    server.on("/password_change.html", HTTP_GET, []() { serveStaticFile("/password_change.html", "text/html"); });
    
    // STATİK DOSYALAR
    server.on("/style.css", HTTP_GET, []() { serveStaticFile("/style.css", "text/css"); });
    server.on("/script.js", HTTP_GET, []() { serveStaticFile("/script.js", "application/javascript"); });
    server.on("/login.js", HTTP_GET, []() { serveStaticFile("/login.js", "application/javascript"); });

    // SPA SAYFA PARÇALARI (Oturum kontrolü GEREKLİ)
    server.on("/pages/dashboard.html", HTTP_GET, []() { if(checkSession()) serveStaticFile("/pages/dashboard.html", "text/html"); else server.send(401); });
    server.on("/pages/network.html", HTTP_GET, []() { if(checkSession()) serveStaticFile("/pages/network.html", "text/html"); else server.send(401); });
    server.on("/pages/systeminfo.html", HTTP_GET, []() { if(checkSession()) serveStaticFile("/pages/systeminfo.html", "text/html"); else server.send(401); });
    server.on("/pages/ntp.html", HTTP_GET, []() { if(checkSession()) serveStaticFile("/pages/ntp.html", "text/html"); else server.send(401); });
    server.on("/pages/baudrate.html", HTTP_GET, []() { if(checkSession()) serveStaticFile("/pages/baudrate.html", "text/html"); else server.send(401); });
    server.on("/pages/fault.html", HTTP_GET, []() { if(checkSession()) serveStaticFile("/pages/fault.html", "text/html"); else server.send(401); });
    server.on("/pages/log.html", HTTP_GET, []() { if(checkSession()) serveStaticFile("/pages/log.html", "text/html"); else server.send(401); });
    server.on("/pages/datetime.html", HTTP_GET, []() { if(checkSession()) serveStaticFile("/pages/datetime.html", "text/html"); else server.send(401); });
    server.on("/pages/account.html", HTTP_GET, []() { if(checkSession()) serveStaticFile("/pages/account.html", "text/html"); else server.send(401); });
    server.on("/pages/backup.html", HTTP_GET, []() { if(checkSession()) serveStaticFile("/pages/backup.html", "text/html"); else server.send(401); });

    // KİMLİK DOĞRULAMA
    server.on("/login", HTTP_POST, handleUserLogin);
    server.on("/logout", HTTP_GET, handleUserLogout);

    // API ENDPOINT'LERİ

    // Device Info (Auth gerekmez)
    server.on("/api/device-info", HTTP_GET, handleDeviceInfoAPI);
    
    // System Info (Auth gerekli)
    server.on("/api/system-info", HTTP_GET, handleSystemInfoAPI);

    // Network Configuration
    server.on("/api/network", HTTP_GET, handleGetNetworkAPI);
    server.on("/api/network", HTTP_POST, handlePostNetworkAPI);

    // Notifications
    server.on("/api/notifications", HTTP_GET, handleNotificationAPI);
    
    // System Reboot
    server.on("/api/system/reboot", HTTP_POST, handleSystemRebootAPI);

    server.on("/api/status", HTTP_GET, handleStatusAPI);
    server.on("/api/settings", HTTP_GET, handleGetSettingsAPI);
    server.on("/api/settings", HTTP_POST, handlePostSettingsAPI);
    server.on("/api/ntp", HTTP_GET, handleGetNtpAPI);
    server.on("/api/ntp", HTTP_POST, handlePostNtpAPI);
    server.on("/api/baudrate", HTTP_GET, handleGetBaudRateAPI);
    server.on("/api/baudrate", HTTP_POST, handlePostBaudRateAPI);
    server.on("/api/logs", HTTP_GET, handleGetLogsAPI);
    server.on("/api/logs/clear", HTTP_POST, handleClearLogsAPI);
    // DateTime API endpoints
    server.on("/api/datetime", HTTP_GET, handleGetDateTimeAPI);
    server.on("/api/datetime/fetch", HTTP_POST, handleFetchDateTimeAPI);
    server.on("/api/datetime/set", HTTP_POST, handleSetDateTimeAPI);
    server.on("/api/datetime/sync-esp32", HTTP_POST, handleSyncESP32API);
    server.on("/api/datetime/set-current", HTTP_POST, handleSetCurrentTimeAPI);
    server.on("/api/datetime/history", HTTP_GET, handleDateTimeHistoryAPI);
    server.on("/api/datetime/preview", HTTP_POST, handleDateTimePreviewAPI);
    // ✅ UART Test API'si ekle
    server.on("/api/uart/test", HTTP_GET, handleUARTTestAPI);

    // YENİ route'ları EKLE:
    server.on("/api/faults/count", HTTP_GET, handleGetFaultCountAPI);
    server.on("/api/faults/get", HTTP_POST, handleGetSpecificFaultAPI);
    server.on("/api/faults/parsed", HTTP_POST, handleParsedFaultAPI); // Güncellendi

     // ✅ Fault komutları için debug endpoint'leri
    server.on("/api/uart/send", HTTP_POST, []() {
        if (!checkSession()) {
            server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
            return;
        }
        
        String command = server.arg("command");
        if (command.length() == 0) {
            server.send(400, "application/json", "{\"error\":\"Command parameter required\"}");
            return;
        }
        
        addLog("🧪 Manuel komut gönderiliyor: " + command, INFO, "UART");
        
        String response;
        bool success = sendCustomCommand(command, response, 3000);
        
        JsonDocument doc;
        doc["command"] = command;
        doc["success"] = success;
        doc["response"] = response;
        doc["responseLength"] = response.length();
        doc["timestamp"] = getFormattedTimestamp();
        
        String output;
        serializeJson(doc, output);
        
        server.send(200, "application/json", output);
    });

    
    server.on("/api/backup/download", HTTP_GET, handleBackupDownload);
    // Yedek yükleme için doğru handler tanımı
    server.on("/api/backup/upload", HTTP_POST, 
        []() { server.send(200, "text/plain", "OK"); }, // Önce bir OK yanıtı gönderilir
        handleBackupUpload // Sonra dosya yükleme işlenir
    );
    server.on("/api/change-password", HTTP_POST, handlePasswordChangeAPI);

    // Password Change Check (soft check)
    server.on("/api/check-password-session", HTTP_GET, handlePasswordChangeCheck);
    
    // Her response'ta security headers ekle
    server.onNotFound([]() {
        addSecurityHeaders();
        addLog("404 isteği: " + server.uri(), WARN, "WEB");
        server.send(404, "application/json", "{\"error\":\"Not Found\"}");
    });
    
    server.begin();
    addLog("✅ Web sunucu başlatıldı", SUCCESS, "WEB");
}