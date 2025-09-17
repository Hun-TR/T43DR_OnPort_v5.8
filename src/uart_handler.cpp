#include "uart_handler.h"
#include "log_system.h"
#include "settings.h"
#include <Preferences.h>

// UART Pin tanımlamaları
#define UART_RX_PIN 4   // IO4 - RX2
#define UART_TX_PIN 14  // IO14 - TX2
#define UART_PORT   Serial2
#define UART_TIMEOUT 3000
#define MAX_RESPONSE_LENGTH 256

// Global değişkenler
static unsigned long lastUARTActivity = 0;
static int uartErrorCount = 0;
bool uartHealthy = true;
String lastResponse = "";
UARTStatistics uartStats = {0, 0, 0, 0, 0, 100.0};

// Buffer temizleme
void clearUARTBuffer() {
    delay(50);
    while (UART_PORT.available()) {
        UART_PORT.read();
        delay(1);
    }
}

// UART istatistiklerini güncelle
void updateUARTStats(bool success) {
    if (success) {
        unsigned long total = uartStats.totalFramesSent + uartStats.totalFramesReceived;
        unsigned long errors = uartStats.frameErrors + uartStats.checksumErrors + uartStats.timeoutErrors;
        if (total > 0) {
            uartStats.successRate = ((float)(total - errors) / (float)total) * 100.0;
        }
    } else {
        uartStats.frameErrors++;
        unsigned long total = uartStats.totalFramesSent + uartStats.totalFramesReceived;
        unsigned long errors = uartStats.frameErrors + uartStats.checksumErrors + uartStats.timeoutErrors;
        if (total > 0) {
            uartStats.successRate = ((float)(total - errors) / (float)total) * 100.0;
        }
    }
}

// UART reset
void resetUART() {
    addLog("🔄 UART reset ediliyor...", WARN, "UART");
    
    UART_PORT.end();
    delay(200);
    
    pinMode(UART_RX_PIN, INPUT);
    pinMode(UART_TX_PIN, OUTPUT);
    digitalWrite(UART_TX_PIN, HIGH);
    
    UART_PORT.begin(250000, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
    delay(200);
    
    clearUARTBuffer();
    
    lastUARTActivity = millis();
    uartErrorCount = 0;
    uartHealthy = true;
    
    addLog("✅ UART reset tamamlandı", SUCCESS, "UART");
    delay(500);
}

// UART başlatma
void initUART() {
    addLog("🚀 UART başlatılıyor...", INFO, "UART");
    
    pinMode(UART_RX_PIN, INPUT);
    pinMode(UART_TX_PIN, OUTPUT);
    
    UART_PORT.begin(250000, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
    
    delay(100);
    clearUARTBuffer();
    
    lastUARTActivity = millis();
    uartErrorCount = 0;
    uartHealthy = true;
    
    addLog("✅ UART başlatıldı - TX2: IO" + String(UART_TX_PIN) + 
           ", RX2: IO" + String(UART_RX_PIN) + 
           ", Baud: 250000", SUCCESS, "UART");
    
    testUARTConnection();
}

// UART bağlantı testi
bool testUARTConnection() {
    addLog("🧪 UART bağlantısı test ediliyor...", INFO, "UART");
    
    if (UART_PORT.available()) {
        String response = "";
        while (UART_PORT.available() && response.length() < 50) {
            char c = UART_PORT.read();
            if (c >= 32 && c <= 126) {
                response += c;
            }
        }
        
        if (response.length() > 0) {
            addLog("✅ UART'da mevcut veri: '" + response + "'", SUCCESS, "UART");
            uartHealthy = true;
            lastUARTActivity = millis();
            return true;
        }
    }
    
    if (UART_PORT) {
        addLog("✅ UART portu aktif", SUCCESS, "UART");
        uartHealthy = true;
        return true;
    } else {
        addLog("❌ UART portu kapalı", ERROR, "UART");
        uartHealthy = false;
        return false;
    }
}

// Güvenli UART okuma
String safeReadUARTResponse(unsigned long timeout) {
    String response = "";
    unsigned long startTime = millis();
    bool dataStarted = false;
    
    while (millis() - startTime < timeout) {
        if (UART_PORT.available()) {
            char c = UART_PORT.read();
            lastUARTActivity = millis();
            uartHealthy = true;
            dataStarted = true;
            
            if (c == '\n' || c == '\r') {
                if (response.length() > 0) {
                    uartStats.totalFramesReceived++;
                    return response;
                }
            } else if (c >= 32 && c <= 126) {
                response += c;
                if (response.length() >= MAX_RESPONSE_LENGTH - 1) {
                    uartStats.totalFramesReceived++;
                    return response;
                }
            }
        } else if (dataStarted) {
            delay(10);
            if (!UART_PORT.available()) {
                if (response.length() > 0) {
                    uartStats.totalFramesReceived++;
                    return response;
                }
            }
        }
        delay(1);
    }
    
    if (response.length() == 0) {
        uartStats.timeoutErrors++;
    }
    
    return response;
}

// Özel komut gönderme
bool sendCustomCommand(const String& command, String& response, unsigned long timeout) {
    if (command.length() == 0 || command.length() > 100) {
        return false;
    }
    
    if (!uartHealthy) {
        resetUART();
    }
    
    clearUARTBuffer();
    
    UART_PORT.print(command);
    UART_PORT.flush();
    
    uartStats.totalFramesSent++;
    
    response = safeReadUARTResponse(timeout == 0 ? UART_TIMEOUT : timeout);
    
    bool success = response.length() > 0;
    updateUARTStats(success);
    
    if (!success) {
        uartErrorCount++;
    }
    
    return success;
}

// BaudRate değiştirme
bool changeBaudRate(long baudRate) {
    addLog("ESP32 UART hızı sabit 250000'de kalıyor, sadece dsPIC'e kod gönderiliyor", INFO, "UART");
    return sendBaudRateCommand(baudRate);
}

// BaudRate komutunu gönder
bool sendBaudRateCommand(long baudRate) {
    String command = "";
    
    switch(baudRate) {
        case 9600:   command = "0Br";   break;
        case 19200:  command = "1Br";  break;
        case 38400:  command = "2Br";  break;
        case 57600:  command = "3Br";  break;
        case 115200: command = "4Br"; break;
        default:
            addLog("Geçersiz baudrate: " + String(baudRate), ERROR, "UART");
            return false;
    }
    
    clearUARTBuffer();
    
    UART_PORT.print(command);
    UART_PORT.flush();
    
    uartStats.totalFramesSent++;
    
    addLog("dsPIC33EP'ye baudrate kodu gönderildi: " + command, INFO, "UART");
    
    String response = safeReadUARTResponse(2000);
    
    if (response == "ACK" || response.indexOf("OK") >= 0) {
        addLog("✅ Baudrate kodu dsPIC33EP tarafından alındı", SUCCESS, "UART");
        updateUARTStats(true);
        return true;
    } else if (response.length() > 0) {
        addLog("dsPIC33EP yanıtı: " + response, WARN, "UART");
        updateUARTStats(true);
        return true;
    } else {
        addLog("❌ dsPIC33EP'den yanıt alınamadı", ERROR, "UART");
        updateUARTStats(false);
        return false;
    }
}

// ============ YENİ ARIZA SORGULAMA FONKSİYONLARI ============

// Toplam arıza sayısını al (AN komutu)
int getTotalFaultCount() {
    clearUARTBuffer();
    
    UART_PORT.print("AN");
    UART_PORT.flush();
    
    uartStats.totalFramesSent++;
    
    addLog("📊 Arıza sayısı sorgulanıyor (AN komutu)", DEBUG, "UART");
    
    String response = safeReadUARTResponse(2000);
    
    if (response.length() >= 2 && response.charAt(0) == 'A') {
        addLog("📥 Gelen yanıt: " + response, DEBUG, "UART");
        
        // A'dan sonrasını sayıya çevir
        String numberStr = response.substring(1);  
        int count = numberStr.toInt();
        
        // 50 - 1 = 49 mantığı
        int actualFaultCount = count - 1;
        
        if (actualFaultCount >= 0) {
            addLog("✅ Toplam arıza sayısı: " + String(actualFaultCount), SUCCESS, "UART");
            updateUARTStats(true);
            return actualFaultCount;
        }
    }
    
    addLog("❌ Arıza sayısı alınamadı veya geçersiz format: " + response, ERROR, "UART");
    updateUARTStats(false);
    return 0;
}

// Belirli bir arıza adresini sorgula
bool requestSpecificFault(int faultNumber) {
    clearUARTBuffer();
    
    // Komutu formatla: 00001v, 00002v, ... formatında
    char command[10];
    sprintf(command, "%05dv", faultNumber);
    
    UART_PORT.print(command);
    UART_PORT.flush();
    
    uartStats.totalFramesSent++;
    
    addLog("🔍 Arıza komutu gönderildi: " + String(command), DEBUG, "UART");
    
    lastResponse = safeReadUARTResponse(3000);
    
    if (lastResponse.length() > 0 && lastResponse != "E") {
        String preview = lastResponse.length() > 50 ? 
                        lastResponse.substring(0, 50) + "..." : 
                        lastResponse;
        addLog("✅ Arıza " + String(faultNumber) + " yanıtı: " + preview, DEBUG, "UART");
        updateUARTStats(true);
        return true;
    } else {
        addLog("❌ Arıza " + String(faultNumber) + " için yanıt alınamadı", ERROR, "UART");
        updateUARTStats(false);
        return false;
    }
}

// İlk arıza kaydını al (geriye uyumluluk için)
bool requestFirstFault() {
    return requestSpecificFault(1);
}

// Sonraki arıza kaydını al (DEPRECATED - kullanmayın)
bool requestNextFault() {
    addLog("⚠️ requestNextFault() artık kullanılmıyor. requestSpecificFault() kullanın.", WARN, "UART");
    return false;
}

// Son yanıtı al
String getLastFaultResponse() {
    return lastResponse;
}

// Test komutu gönder
bool sendTestCommand(const String& testCmd) {
    clearUARTBuffer();
    
    UART_PORT.print(testCmd);
    UART_PORT.flush();
    
    addLog("🧪 Test komutu gönderildi: " + testCmd, DEBUG, "UART");
    
    String response = safeReadUARTResponse(3000);
    
    if (response.length() > 0) {
        addLog("📡 Test yanıtı: " + response, DEBUG, "UART");
        return true;
    } else {
        addLog("❌ Test komutu için yanıt yok", WARN, "UART");
        return false;
    }
}

// UART sağlık kontrolü
void checkUARTHealth() {
    static unsigned long lastHealthCheck = 0;
    
    if (millis() - lastHealthCheck < 30000) {
        return;
    }
    lastHealthCheck = millis();
    
    // 5 dakika sessizlik kontrolü
    if (millis() - lastUARTActivity > 300000) {
        if (uartHealthy) {
            addLog("⚠️ UART 5 dakikadır sessiz", WARN, "UART");
            uartHealthy = false;
        }
    }
    
    // Çok fazla hata varsa reset
    if (uartErrorCount > 5) {
        addLog("🔄 Çok fazla UART hatası (" + String(uartErrorCount) + "), reset yapılıyor...", WARN, "UART");
        resetUART();
    }
    
    // Periyodik test
    if (!uartHealthy) {
        addLog("🩺 UART sağlık testi yapılıyor...", INFO, "UART");
        testUARTConnection();
    }
}

// UART durumunu al
String getUARTStatus() {
    String status = "UART Durumu:\n";
    status += "Sağlık: " + String(uartHealthy ? "✅ İyi" : "❌ Kötü") + "\n";
    status += "Son Aktivite: " + String((millis() - lastUARTActivity) / 1000) + " saniye önce\n";
    status += "Hata Sayısı: " + String(uartErrorCount) + "\n";
    status += "Başarı Oranı: " + String(uartStats.successRate, 1) + "%\n";
    status += "Gönderilen: " + String(uartStats.totalFramesSent) + "\n";
    status += "Alınan: " + String(uartStats.totalFramesReceived) + "\n";
    status += "Timeout: " + String(uartStats.timeoutErrors);
    return status;
}