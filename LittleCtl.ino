
#include <WiFiManager.h>
#include <SoftwareSerial.h>
#include <LittleFS.h>
#include <Ticker.h>

#include "SerialProxy.h"
#include "EEPROMVars.h"
#include "ConsoleLogger.h"

#define DEFAULT_BAUD_SERIAL 115200
#define DEFAULT_BAUD_CONSOLE 115200
#define DEFAULT_RXBUFFERSIZE 1024

#define BAUD_LOGGER 115200
#define EEPROM_SIZE 32

////////////////////////////////////////////////////////////

#define MACROSTR_(k) #k
#define MACROSTR(k) MACROSTR_(k)

#define logger (Serial1)

ConsoleLogger consoleLogger(Serial, LittleFS, logger);
SerialProxy serialProxy(Serial, logger);

WiFiServer termServer(22); // Create a config server on port 22

// === EEPROM variables ===
EEPROMVar<uint32_t> SerialBaud(0);
EEPROMVar<uint16_t> SerialRxBufSz(4);
// 2bytes left
EEPROMVar<uint32_t> ConsoleBaud(8);

// *** EEPROM variables ***

Ticker timeConfigTicker;
Ticker pwrStateReader;
bool hasStartedConsoleLogger = false;
bool hasSerialConnected = false;

bool pwrState = false;
bool pwrStateChange = false;

constexpr int PIN_RSTBTN = 12; // MISO
constexpr int PIN_PWRBTN = 14; // SCK

void reConfigureTime()
{
    configTime("CST-8", "time2.cloud.tencent.com", "ntp1.aliyun.com", "ntp.ntsc.ac.cn");
}

bool initCfg()
{
    bool resetFlag = false;
    pinMode(2, OUTPUT_OPEN_DRAIN);
    delay(3000); // default Low that act the LED of module for a hint to user
    pinMode(2, INPUT_PULLUP);
    while (digitalRead(2) == LOW) {
        resetFlag = 1;
        // the user hold this button, wait for release
        delay(100);
    }
    pinMode(2, OUTPUT_OPEN_DRAIN);
    if (resetFlag) {
        // Respond to the user's operation
        for (int i = 0; i < 3; ++i) {
            digitalWrite(2, HIGH);
            delay(500);
            digitalWrite(2, LOW);
            delay(500);
        }
    }
    digitalWrite(2, HIGH);

    EEPROM.begin(EEPROM_SIZE);
    EEPROMVar<uint16_t> _magic(EEPROM_SIZE - 2);
    if (resetFlag || _magic != 0x5a5a) { // first run
        SerialBaud = DEFAULT_BAUD_SERIAL;
        SerialRxBufSz = DEFAULT_RXBUFFERSIZE;
        ConsoleBaud = DEFAULT_BAUD_CONSOLE;

        _magic = 0x5a5a;
        if (resetFlag)
            EEPROM.commitReset();
        else
            EEPROM.commit();
    }
    return resetFlag;
}

void onConnectStatusChanged(bool connected)
{
    Serial.flush(); // Send out all the data
    // disanle pins
    pinMode(13, INPUT);
    pinMode(15, INPUT);
    pinMode(3, INPUT);
    pinMode(1, INPUT);
    while (Serial.available())
        Serial.read();
    if (connected) {
        logger.print("Connected to Serial...");
        Serial.updateBaudRate(SerialBaud);
        logger.print(Serial.pins(1, 3) ? "OK\n" : "Failed\n");
        hasSerialConnected = true;
    } else {
        logger.print("Disconnected from Serial, recovering to console...");
        Serial.updateBaudRate(ConsoleBaud);
        logger.print(Serial.pins(15, 13) ? "OK\n" : "Failed\n");
        hasSerialConnected = false;
    }
    for (int i = 0; i < 10; i++)
        Serial.read(); // Throw the some first byte, it's usually garbage
}

void setup()
{
    bool resetFlag = initCfg();
    logger.begin(BAUD_LOGGER);
    logger.printf("0 Free heap: %u\n", ESP.getFreeHeap());
    if (!LittleFS.begin()) {
        logger.println("Failed to mount file system");
        delay(5000);
        ESP.restart();
    }
    logger.printf("1 Free heap: %u\n", ESP.getFreeHeap());

    WiFi.mode(WIFI_STA); // explicitly set mode, esp defaults to STA+AP
    WiFiManager wm(logger);

    if (resetFlag) // Should we reset the settings?
        wm.resetSettings();

    WiFiManagerParameter param_baud("baud", "Baudrate", MACROSTR(DEFAULT_BAUD_SERIAL), 6);
    WiFiManagerParameter param_rxbuffersize("rxbuffersize", "Serial RX buffer size", MACROSTR(DEFAULT_RXBUFFERSIZE), 4);

    WiFiManagerParameter param_console_baud("cbaud", "Console Baudrate", MACROSTR(DEFAULT_BAUD_CONSOLE), 6);

    wm.addParameter(&param_baud);
    wm.addParameter(&param_rxbuffersize);
    wm.addParameter(&param_console_baud);

    bool shouldSaveConfig = false;
    wm.setSaveParamsCallback([&]() {
        shouldSaveConfig = true;
    });

    wm.setConfigPortalTimeout(300); // 5 minutes

    bool res;
    if (!(res = wm.autoConnect())) {
        logger.println("Failed to connect! Rebooting in 5s...");
        delay(5000);
        ESP.restart();
    }
    logger.println("connected!");
    logger.print("IP address: ");
    logger.println(WiFi.localIP());

    // Register event handlers
    WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected& event) {
        logger.println("Disconnected from WiFi, rebooting in 5s...");
        if(hasStartedConsoleLogger)
            consoleLogger.end();
        Serial.end();
        termServer.stop();
        delay(3000);
        ESP.restart();
    });

    // We have connected to WiFi, now we can configure time
    reConfigureTime();

    if (shouldSaveConfig) {
        long l = atol(param_baud.getValue());
        if (l <= 0 || l > 1000000)
            logger.println("Invalid baudrate, using default");
        else
            SerialBaud = l;

        l = atol(param_console_baud.getValue());
        if (l <= 0 || l > 1000000)
            logger.println("Invalid console baudrate, using default");
        else
            ConsoleBaud = l;

        l = atoi(param_rxbuffersize.getValue());
        if (l <= 0 || l > 4096)
            logger.println("Invalid RX buffer size, using default");
        else
            SerialRxBufSz = l;

        EEPROM.commit();
    }

    logger.println("* Configured values:");
    logger.printf("SerialBaud: %u\n", (uint32_t)SerialBaud);
    logger.printf("SerialRxBufSz: %u\n", (uint16_t)SerialRxBufSz);
    logger.printf("ConsoleBaud: %u\n", (uint32_t)ConsoleBaud);

    logger.printf("2 Free heap: %u\n", ESP.getFreeHeap());
    Serial.setRxBufferSize(SerialRxBufSz);
    Serial.begin(ConsoleBaud); // default to console
    Serial.pins(15, 13);
    serialProxy.begin(onConnectStatusChanged);

    timeConfigTicker.attach(3, []() {
        struct tm tmstruct;
        time_t now;

        now = time(NULL);
        localtime_r(&now, &tmstruct);
        if (tmstruct.tm_year > (2016 - 1900)) {
            hasStartedConsoleLogger = consoleLogger.begin(30);
            if (hasStartedConsoleLogger) {
                logger.println("Console logger started");
                timeConfigTicker.detach();
                timeConfigTicker.attach(3600, reConfigureTime); // reconfigure time every hour
            } else {
                logger.println("Console logger failed to start!");
            }
        } else {
            logger.println("Time not set, retrying in 3s");
            reConfigureTime();
        }
    });

    // Power state reader
    pwrStateReader.attach_ms(200, []() {
        bool pwr = (analogRead(A0) > 550);
        if (pwr != pwrState) {
            pwrState = pwr;
            pwrStateChange = true;
        }
    });

    termServer.begin();
    termServer.setNoDelay(true);
    logger.println("Ready! Free heap: " + String(ESP.getFreeHeap()));
}

void handleTerm(char* cbuf, int csize)
{
    static WiFiClient termClient;
    static StreamString termOut;
    static bool dumpConsoleEn = false;
    static enum { CMD_NONE,
        CMD_WAIT_ACTION,
        CMD_EXEC,

        // actions
        CMD_RESET,
        CMD_SHUTDOWN,
        CMD_POWEROFF } SuperCmd
        = CMD_NONE;
    static Ticker superCmdTicker;

    // static bool logRead = false;
    static uint8_t logReadIndex = 0xff;
    static File logFile;

    static auto output = [&]() {
        if (termOut.available()) {
            int afw = termClient.availableForWrite();
            if (afw) {
                termOut.sendAvailable(termClient);
            } /*  else {
                 logger.println("Term client is congested");
             } */
        }
    };

    if (termServer.hasClient()) {
        if (!termClient) {
            termClient = termServer.accept();
            termClient.setTimeout(0);
            SuperCmd = CMD_NONE;
            logReadIndex = 0xff;
            dumpConsoleEn = false;
            logger.println("New term client connected");
            termOut.clear();
            termOut.println("=== Welcome to LittleCtl terminal ===");
        } else {
            termServer.accept().println("busy");
        }
    }

    if (termClient) {
        output();
        if (termOut.available()) // Last time output may not be complete
            return;

        if (csize && dumpConsoleEn) {
            termOut.write((const uint8_t*)cbuf, csize);
        }

        if (logFile.available()) {
            logFile.sendSize(termOut, 512);
        } else {
            if (logFile) {
                logFile.close();
                logReadIndex = 0xff;
            }
        }

        if (pwrStateChange) {
            termOut.println("Power state changed to " + String(pwrState ? "ON" : "OFF"));
            pwrStateChange = false;
        }

        int a = termClient.read();
        if (a != -1) {

            switch (a & 0xFF) {
            case '#':
                if (SuperCmd == CMD_NONE)
                    SuperCmd = CMD_WAIT_ACTION;
                else if (SuperCmd == CMD_EXEC)
                    termOut.println("Super command already in progress!");
                // else
                //     SuperCmd = CMD_NONE;
                break;
            case '$': // Confirm super command
                switch (SuperCmd) {
                case CMD_RESET:
                    termOut.println("Resetting...");
                    pinMode(PIN_RSTBTN, OUTPUT_OPEN_DRAIN);
                    digitalWrite(PIN_RSTBTN, LOW);
                    SuperCmd = CMD_EXEC;
                    superCmdTicker.once_ms(500, [&]() {
                        digitalWrite(PIN_RSTBTN, HIGH);
                        pinMode(PIN_RSTBTN, INPUT);
                        SuperCmd = CMD_NONE;
                    });
                    break;

                case CMD_SHUTDOWN:
                    termOut.println("Short press PWRBTN...");
                    pinMode(PIN_PWRBTN, OUTPUT_OPEN_DRAIN);
                    digitalWrite(PIN_PWRBTN, LOW);
                    SuperCmd = CMD_EXEC;
                    superCmdTicker.once_ms(500, [&]() {
                        digitalWrite(PIN_PWRBTN, HIGH);
                        pinMode(PIN_PWRBTN, INPUT);
                        SuperCmd = CMD_NONE;
                    });
                    break;
                case CMD_POWEROFF:
                    termOut.println("Long press PWRBTN...");
                    pinMode(PIN_PWRBTN, OUTPUT_OPEN_DRAIN);
                    digitalWrite(PIN_PWRBTN, LOW);
                    SuperCmd = CMD_EXEC;
                    superCmdTicker.once_ms(3000, [&]() {
                        digitalWrite(PIN_PWRBTN, HIGH);
                        pinMode(PIN_PWRBTN, INPUT);
                        SuperCmd = CMD_NONE;
                    });
                    break;
                }
                break;
            case 'R':
                if (SuperCmd == CMD_WAIT_ACTION)
                    SuperCmd = CMD_RESET;
                else
                    SuperCmd = CMD_NONE;
                break;
            case 'S':
                if (SuperCmd == CMD_WAIT_ACTION)
                    SuperCmd = CMD_SHUTDOWN;
                else
                    SuperCmd = CMD_NONE;
                break;
            case 'P':
                if (SuperCmd == CMD_WAIT_ACTION)
                    SuperCmd = CMD_POWEROFF;
                else
                    SuperCmd = CMD_NONE;
                break;

            case 'V':
                termOut.println("LittleCtl v1.0.0");
                break;
            case 'D':
                dumpConsoleEn = !dumpConsoleEn;
                termOut.println("Dump console: " + String(dumpConsoleEn ? "Enabled" : "Disabled"));
                break;

            case 'M':
                termOut.printf("Free heap: %u\n", ESP.getFreeHeap());
                termOut.printf("Serial baud: %u\n", (uint32_t)SerialBaud);
                termOut.printf("Serial RX buffer size: %u\n", (uint16_t)SerialRxBufSz);
                termOut.printf("Console baud: %u\n", (uint32_t)ConsoleBaud);
                termOut.printf("Power state: %s\n", pwrState ? "ON" : "OFF");
                termOut.printf("Log Service: %s\n", hasStartedConsoleLogger ? "Started" : "Not started");
                break;

            case 'L': // List all logs
            {
                Dir dir = LittleFS.openDir("/log");
                termOut.println("Name\tCreated\t\t\tLastClose\t\tSize");
                while (dir.next()) {
                    time_t t = dir.fileCreationTime();
                    tm tmstruct;
                    localtime_r(&t, &tmstruct);
                    termOut.printf("%s\t%d-%02d-%02d %02d:%02d:%02d\t", dir.fileName().c_str(), tmstruct.tm_year + 1900, tmstruct.tm_mon + 1, tmstruct.tm_mday, tmstruct.tm_hour, tmstruct.tm_min, tmstruct.tm_sec);
                    t = dir.fileTime();
                    tmstruct = tm();
                    localtime_r(&t, &tmstruct);
                    termOut.printf("%d-%02d-%02d %02d:%02d:%02d\t%uB\n", tmstruct.tm_year + 1900, tmstruct.tm_mon + 1, tmstruct.tm_mday, tmstruct.tm_hour, tmstruct.tm_min, tmstruct.tm_sec, dir.fileSize());
                }
                FSInfo fs_info;
                LittleFS.info(fs_info);
                size_t freeSpace = fs_info.totalBytes - fs_info.usedBytes;
                termOut.printf("FS Total : %u, Used: %u, Free : %u\n", fs_info.totalBytes, fs_info.usedBytes, freeSpace);
                termOut.printf("Total: %u files, MinIdx = %u, MaxIdx = %u\n", consoleLogger.getMax() - consoleLogger.getMin() + 1, consoleLogger.getMin(), consoleLogger.getMax());

            } break;
            default:
                if (a >= 0x80 && a < 0xE0) {
                    if (logReadIndex == 0xff) {
                        logReadIndex = (a - 0x80) + consoleLogger.getMin();
                        logFile = LittleFS.open("/log/" + String(logReadIndex), "r");
                        if (!logFile) {
                            termOut.println("Failed to open log file!");
                            logReadIndex = 0xff;
                        }
                    }
                }
            }

            // State machine fallback
            if (SuperCmd == CMD_WAIT_ACTION && a != 'R' && a != 'S' && a != 'P' && a != '#') {
                SuperCmd = CMD_NONE;
            }
            if ((SuperCmd == CMD_POWEROFF || SuperCmd == CMD_SHUTDOWN || SuperCmd == CMD_RESET) && a != '$' && a != 'R' && a != 'S' && a != 'P') {
                SuperCmd = CMD_NONE;
            }

            output();
        }
    }
}

void loop()
{
    int len = 0;
    char buf[512];
    // static size_t lowerFreeHeap = ESP.getFreeHeap();
    serialProxy.proxy(buf, sizeof(buf));

    if (hasStartedConsoleLogger && !hasSerialConnected) {
        len = consoleLogger.doLog(buf, sizeof(buf));
    }
    handleTerm(buf, len); // Pass this to the terminal handler, in case we need to dump it to the terminal

    // size_t freeHeap = ESP.getFreeHeap();
    // if (freeHeap < lowerFreeHeap) {
    //     lowerFreeHeap = freeHeap;
    //     logger.printf("New Lower Heap: %u\n", freeHeap);
    // }
}
