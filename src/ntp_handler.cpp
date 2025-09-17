// ntp_handler.cpp
#include "ntp_handler.h"
#include "log_system.h"
#include "uart_handler.h"
#include <Preferences.h>

// Global değişkenler
NTPConfig ntpConfig;
bool ntpConfigured = false;

// IP adresini dsPIC formatına dönüştür
// Örnek: "192.168.1.2" -> ["192168", "001002"]
void formatIPForDsPIC(const String& ip, String& part1, String& part2) {
    // IP'yi parçala
    int dot1 = ip.indexOf('.');
    int dot2 = ip.indexOf('.', dot1 + 1);
    int dot3 = ip.indexOf('.', dot2 + 1);
    
    if (dot1 == -1 || dot2 == -1 || dot3 == -1) {
        part1 = "";
        part2 = "";
        return;
    }
    
    String octet1 = ip.substring(0, dot1);
    String octet2 = ip.substring(dot1 + 1, dot2);
    String octet3 = ip.substring(dot2 + 1, dot3);
    String octet4 = ip.substring(dot3 + 1);
    
    // Oktetleri integer'a çevir
    int o1 = octet1.toInt();
    int o2 = octet2.toInt();
    int o3 = octet3.toInt();
    int o4 = octet4.toInt();
    
    // İlk iki oktet'i birleştir (192168) - sprintf ile formatlama
    char buffer1[7];
    sprintf(buffer1, "%03d%03d", o1, o2);
    part1 = String(buffer1);
    
    // Son iki oktet'i birleştir (001002) - sprintf ile formatlama
    char buffer2[7];
    sprintf(buffer2, "%03d%03d", o3, o4);
    part2 = String(buffer2);
}

// NTP ayarlarını dsPIC33EP'ye gönder - YENİ FORMAT
void sendNTPConfigToBackend() {
    if (strlen(ntpConfig.ntpServer1) == 0) {
        addLog("NTP sunucu adresi boş", WARN, "NTP");
        return;
    }
    
    String response;
    bool allSuccess = true;
    
    // NTP1 için format dönüşümü
    String ntp1_part1, ntp1_part2;
    formatIPForDsPIC(String(ntpConfig.ntpServer1), ntp1_part1, ntp1_part2);
    
    if (ntp1_part1.length() == 6 && ntp1_part2.length() == 6) {
        // NTP1 ilk komut: 192168u
        String cmd1 = ntp1_part1 + "u";
        if (sendCustomCommand(cmd1, response, 1000)) {
            addLog("✅ NTP1 Part1 gönderildi: " + cmd1, SUCCESS, "NTP");
        } else {
            addLog("❌ NTP1 Part1 gönderilemedi: " + cmd1, ERROR, "NTP");
            allSuccess = false;
        }
        
        delay(50); // Komutlar arası bekleme
        
        // NTP1 ikinci komut: 001002y
        String cmd2 = ntp1_part2 + "y";
        if (sendCustomCommand(cmd2, response, 1000)) {
            addLog("✅ NTP1 Part2 gönderildi: " + cmd2, SUCCESS, "NTP");
        } else {
            addLog("❌ NTP1 Part2 gönderilemedi: " + cmd2, ERROR, "NTP");
            allSuccess = false;
        }
    } else {
        addLog("❌ NTP1 format dönüşümü başarısız", ERROR, "NTP");
        allSuccess = false;
    }
    
    // NTP2 varsa gönder
    if (strlen(ntpConfig.ntpServer2) > 0) {
        delay(50);
        
        String ntp2_part1, ntp2_part2;
        formatIPForDsPIC(String(ntpConfig.ntpServer2), ntp2_part1, ntp2_part2);
        
        if (ntp2_part1.length() == 6 && ntp2_part2.length() == 6) {
            // NTP2 ilk komut: 192169w
            String cmd3 = ntp2_part1 + "w";
            if (sendCustomCommand(cmd3, response, 1000)) {
                addLog("✅ NTP2 Part1 gönderildi: " + cmd3, SUCCESS, "NTP");
            } else {
                addLog("❌ NTP2 Part1 gönderilemedi: " + cmd3, ERROR, "NTP");
                allSuccess = false;
            }
            
            delay(50);
            
            // NTP2 ikinci komut: 001001x
            String cmd4 = ntp2_part2 + "x";
            if (sendCustomCommand(cmd4, response, 1000)) {
                addLog("✅ NTP2 Part2 gönderildi: " + cmd4, SUCCESS, "NTP");
            } else {
                addLog("❌ NTP2 Part2 gönderilemedi: " + cmd4, ERROR, "NTP");
                allSuccess = false;
            }
        } else {
            addLog("❌ NTP2 format dönüşümü başarısız", ERROR, "NTP");
            allSuccess = false;
        }
    }
    
    if (allSuccess) {
        addLog("✅ Tüm NTP ayarları başarıyla dsPIC33EP'ye gönderildi", SUCCESS, "NTP");
    } else {
        addLog("⚠️ NTP ayarları kısmen gönderildi", WARN, "NTP");
    }
}

bool loadNTPSettings() {
    Preferences preferences;
    preferences.begin("ntp-config", true);
    
    String server1 = preferences.getString("ntp_server1", "");
    if (server1.length() == 0) {
        preferences.end();
        
        // Kayıtlı ayar yoksa varsayılanları kullan
        strcpy(ntpConfig.ntpServer1, "192.168.1.1");
        strcpy(ntpConfig.ntpServer2, "8.8.8.8");
        ntpConfig.timezone = 3;
        ntpConfig.enabled = false;
        ntpConfigured = false;
        
        addLog("⚠️ Kayıtlı NTP ayarı bulunamadı, varsayılanlar yüklendi", WARN, "NTP");
        return false;
    }
    
    String server2 = preferences.getString("ntp_server2", "");
    
    if (!isValidIPOrDomain(server1) || (server2.length() > 0 && !isValidIPOrDomain(server2))) {
        preferences.end();
        return false;
    }
    
    server1.toCharArray(ntpConfig.ntpServer1, sizeof(ntpConfig.ntpServer1));
    server2.toCharArray(ntpConfig.ntpServer2, sizeof(ntpConfig.ntpServer2));
    
    ntpConfig.timezone = preferences.getInt("timezone", 3);
    ntpConfig.enabled = preferences.getBool("enabled", true);
    
    preferences.end();
    
    ntpConfigured = true;
    addLog("✅ NTP ayarları yüklendi: " + server1 + ", " + server2, SUCCESS, "NTP");
    return true;
}

bool isValidIPOrDomain(const String& address) {
    if (address.length() < 7 || address.length() > 253) return false;
    
    // IP adresi kontrolü
    IPAddress testIP;
    if (testIP.fromString(address)) {
        return true;
    }
    
    // Domain adı kontrolü (basit)
    if (address.indexOf('.') > 0 && address.indexOf(' ') == -1) {
        return true;
    }
    
    return false;
}

bool saveNTPSettings(const String& server1, const String& server2, int timezone) {
    if (!isValidIPOrDomain(server1)) {
        addLog("Geçersiz birincil NTP sunucu", ERROR, "NTP");
        return false;
    }
    
    if (server2.length() > 0 && !isValidIPOrDomain(server2)) {
        addLog("Geçersiz ikincil NTP sunucu", ERROR, "NTP");
        return false;
    }
    
    Preferences preferences;
    preferences.begin("ntp-config", false);
    
    preferences.putString("ntp_server1", server1);
    preferences.putString("ntp_server2", server2);
    preferences.putInt("timezone", timezone);
    preferences.putBool("enabled", true);
    
    preferences.end();
    
    // Global config güncelle
    server1.toCharArray(ntpConfig.ntpServer1, sizeof(ntpConfig.ntpServer1));
    server2.toCharArray(ntpConfig.ntpServer2, sizeof(ntpConfig.ntpServer2));
    ntpConfig.timezone = timezone;
    ntpConfig.enabled = true;
    ntpConfigured = true;
    
    addLog("✅ NTP ayarları kaydedildi", SUCCESS, "NTP");
    
    // dsPIC33EP'ye gönder
    sendNTPConfigToBackend();
    return true;
}

// NTP Handler başlatma
void initNTPHandler() {
    // NTP ayarları yükleme
    if (!loadNTPSettings()) {
        addLog("⚠️ Kayıtlı NTP ayarı bulunamadı", WARN, "NTP");
    }
    
    // Eğer kayıtlı ayar varsa gönder
    if (ntpConfigured && strlen(ntpConfig.ntpServer1) > 0) {
        delay(1000); // Backend'in hazır olmasını bekle
        sendNTPConfigToBackend();
    }
    
    addLog("✅ NTP Handler başlatıldı", SUCCESS, "NTP");
}

// String padding helper function - KALDIRILIYOR
// Bu fonksiyon artık kullanılmıyor, sprintf ile çözüldü

// Eski fonksiyonları inline yap (çoklu tanımlama hatası için)
ReceivedTimeData receivedTime = {.date = "", .time = "", .isValid = false, .lastUpdate = 0};

void processReceivedData() {
    // Bu fonksiyon artık time_sync.cpp tarafından yönetiliyor
}

void readBackendData() {
    // Bu fonksiyon artık time_sync.cpp tarafından yönetiliyor  
}

void parseTimeData(const String& data) {
    // Bu fonksiyon artık time_sync.cpp tarafından yönetiliyor
}

bool isTimeDataValid() {
    return false; // time_sync.cpp'deki isTimeSynced() kullanılacak
}

bool isNTPSynced() {
    return ntpConfigured;
}

void resetNTPSettings() {
    Preferences preferences;
    preferences.begin("ntp-config", false);
    preferences.clear();
    preferences.end();
    
    ntpConfigured = false;
    
    addLog("NTP ayarları sıfırlandı", INFO, "NTP");
}