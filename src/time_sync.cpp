// src/time_sync.cpp

#include "uart_handler.h"
#include "log_system.h"
#include <time.h>

// Global zaman değişkenleri
struct TimeData {
    bool isValid;
    String lastDate;
    String lastTime;
    unsigned long lastSync;
    int syncCount;
    int failCount; // Başarısız deneme sayısı
} timeData = {false, "", "", 0, 0, 0};

// Forward declarations
bool parseTimeResponse(const String& response);
String formatDate(const String& dateStr);
String formatTime(const String& timeStr);
void updateSystemTime();
bool isTimeSynced();

// Tarih formatla: DDMMYY -> DD.MM.20YY
String formatDate(const String& dateStr) {
    if (dateStr.length() != 6) return "Geçersiz";
    int day = dateStr.substring(0, 2).toInt();
    int month = dateStr.substring(2, 4).toInt();
    int year = 2000 + dateStr.substring(4, 6).toInt();
    if (day < 1 || day > 31 || month < 1 || month > 12) { return "Geçersiz"; }
    char buffer[12];
    sprintf(buffer, "%02d.%02d.%04d", day, month, year);
    return String(buffer);
}

// Saat formatla: HHMMSS -> HH:MM:SS
String formatTime(const String& timeStr) {
    if (timeStr.length() != 6) return "Geçersiz";
    int hour = timeStr.substring(0, 2).toInt();
    int minute = timeStr.substring(2, 4).toInt();
    int second = timeStr.substring(4, 6).toInt();
    if (hour > 23 || minute > 59 || second > 59) { return "Geçersiz"; }
    char buffer[10];
    sprintf(buffer, "%02d:%02d:%02d", hour, minute, second);
    return String(buffer);
}

// ESP32 sistem saatini güncelle
void updateSystemTime() {
    if (!timeData.isValid) return;
    int day, month, year, hour, minute, second;
    sscanf(timeData.lastDate.c_str(), "%d.%d.%d", &day, &month, &year);
    sscanf(timeData.lastTime.c_str(), "%d:%d:%d", &hour, &minute, &second);
    struct tm timeinfo;
    timeinfo.tm_year = year - 1900;
    timeinfo.tm_mon = month - 1;
    timeinfo.tm_mday = day;
    timeinfo.tm_hour = hour;
    timeinfo.tm_min = minute;
    timeinfo.tm_sec = second;
    timeinfo.tm_isdst = 0;
    time_t t = mktime(&timeinfo);
    struct timeval now = { .tv_sec = t };
    settimeofday(&now, NULL);
    addLog("Sistem saati güncellendi", INFO, "TIME");
}

// dsPIC'ten gelen zaman verisini parse et
bool parseTimeResponse(const String& response) {
    // Format 1: "DATE:DDMMYY,TIME:HHMMSS"
    if (response.indexOf("DATE:") >= 0 && response.indexOf("TIME:") >= 0) {
        int dateStart = response.indexOf("DATE:") + 5;
        int dateEnd = response.indexOf(",");
        int timeStart = response.indexOf("TIME:") + 5;
        String dateStr = response.substring(dateStart, dateEnd);
        String timeStr = response.substring(timeStart, timeStart + 6);
        if (dateStr.length() == 6 && timeStr.length() == 6) {
            timeData.lastDate = formatDate(dateStr);
            timeData.lastTime = formatTime(timeStr);
            return true;
        }
    }
    // Format 2: "DDMMYYHHMMSS" (12 karakter)
    if (response.length() == 12) {
        String dateStr = response.substring(0, 6);
        String timeStr = response.substring(6, 12);
        timeData.lastDate = formatDate(dateStr);
        timeData.lastTime = formatTime(timeStr);
        return true;
    }
    // Format 3: Checksum'lı veri "DDMMYYx" ve "HHMMSSy"
    if (response.length() == 7) {
        String dataOnly = response.substring(0, 6);
        char checksum = response.charAt(6);
        if (checksum >= 'A' && checksum <= 'Z') { // Tarih
            timeData.lastDate = formatDate(dataOnly);
            return true;
        } else if (checksum >= 'a' && checksum <= 'z') { // Saat
            timeData.lastTime = formatTime(dataOnly);
            timeData.isValid = true;
            return true;
        }
    }
    
    // Yanıt alındı ama format uymuyor
    if (response.length() > 0) {
        addLog("Geçersiz zaman formatı: " + response, WARN, "TIME");
    }
    return false;
}

// dsPIC'ten zaman isteği gönder
bool requestTimeFromDsPIC() {
    String response;
    /*if (!sendCustomCommand("GETTIME", response, 2000)) {
        timeData.failCount++;
        
        // Her 10 başarısız denemede bir log at
        if (timeData.failCount % 10 == 1) {
            addLog("❌ dsPIC'ten zaman bilgisi alınamadı (Deneme: " + String(timeData.failCount) + ")", ERROR, "TIME");
        }
        return false;
    }
    
    if (parseTimeResponse(response)) {
        timeData.lastSync = millis();
        timeData.syncCount++;
        timeData.failCount = 0; // Başarılı olunca sıfırla
        timeData.isValid = true;
        addLog("✅ Zaman senkronize edildi: " + timeData.lastDate + " " + timeData.lastTime, SUCCESS, "TIME");
        updateSystemTime();
        return true;
    }*/
    return false;
}

// Periyodik senkronizasyon kontrolü
void checkTimeSync() {
    static unsigned long lastSyncAttempt = 0;
    const unsigned long SYNC_INTERVAL = 300000;  // 5 dakika (300 saniye)
    const unsigned long RETRY_INTERVAL = 30000;  // 30 saniye (başarısız olunca)
    const unsigned long INITIAL_RETRY = 5000;    // İlk 5 deneme için 5 saniye
    
    unsigned long now = millis();
    unsigned long nextInterval;
    
    // İlk başlangıçta veya sync yoksa daha sık dene
    if (!isTimeSynced()) {
        if (timeData.failCount < 5) {
            nextInterval = INITIAL_RETRY; // İlk 5 deneme 5 saniyede bir
        } else {
            nextInterval = RETRY_INTERVAL; // Sonra 30 saniyede bir
        }
    } else {
        nextInterval = SYNC_INTERVAL; // Başarılı sync sonrası 5 dakikada bir
    }
    
    if (now - lastSyncAttempt > nextInterval) {
        lastSyncAttempt = now;
        requestTimeFromDsPIC();
    }
    
    // Senkronizasyon kaybı kontrolü (10 dakika)
    if (isTimeSynced() && (now - timeData.lastSync > 600000)) {
        timeData.isValid = false;
        addLog("⚠️ Zaman senkronizasyonu kaybedildi", WARN, "TIME");
    }
}

// API için zaman bilgilerini döndür
String getCurrentDateTime() {
    if (!isTimeSynced()) {
        // Senkronize değilse millis bazlı zaman göster
        unsigned long sec = millis() / 1000;
        unsigned long min = sec / 60;
        unsigned long hour = min / 60;
        char buffer[32];
        sprintf(buffer, "Uptime: %02lu:%02lu:%02lu", hour % 24, min % 60, sec % 60);
        return String(buffer);
    }
    return timeData.lastDate + " " + timeData.lastTime;
}

String getCurrentDate() {
    return isTimeSynced() ? timeData.lastDate : "---";
}

String getCurrentTime() {
    return isTimeSynced() ? timeData.lastTime : "---";
}

bool isTimeSynced() {
    return timeData.isValid;
}

// Zaman senkronizasyon istatistikleri
String getTimeSyncStats() {
    String stats = "Senkronizasyon Durumu: ";
    stats += isTimeSynced() ? "Aktif\n" : "Pasif\n";
    stats += "Toplam Senkronizasyon: " + String(timeData.syncCount) + "\n";
    stats += "Başarısız Deneme: " + String(timeData.failCount) + "\n";
    if (timeData.lastSync > 0) {
        unsigned long elapsed = (millis() - timeData.lastSync) / 1000;
        stats += "Son Senkronizasyon: " + String(elapsed) + " saniye önce\n";
    }
    stats += "Son Tarih: " + timeData.lastDate + "\n";
    stats += "Son Saat: " + timeData.lastTime;
    return stats;
}