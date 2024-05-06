#include <Arduino.h>
#include "ATEM_tally_light.hpp"

// FIRMWARE VERSION !!!
//

float firmware_version = 1.03;

//
//
#define VERSION "dev"
#define FASTLED_ALLOW_INTERRUPTS 0
#define DISPLAY_NAME "Tally Light"

// Include libraries:
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>

#include <EEPROM.h>
#include <ATEMmin.h>
#include <TallyServer.h>
#include <FastLED.h>
#include <iostream>
#include <string>

#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>

// Define LED colors
#define LED_OFF 0
#define LED_RED 1
#define LED_GREEN 2
#define LED_BLUE 3
#define LED_YELLOW 4
#define LED_PINK 5
#define LED_WHITE 6
#define LED_ORANGE 7

// Map "old" LED colors to CRGB colors
CRGB color_led[8] = {CRGB::Black, CRGB::Red, CRGB::Lime, CRGB::Blue, CRGB::Yellow, CRGB::Fuchsia, CRGB::White, CRGB::Orange};

// Define states
#define STATE_STARTING 0
#define STATE_CONNECTING_TO_WIFI 1
#define STATE_CONNECTING_TO_SWITCHER 2
#define STATE_RUNNING 3

// Define modes of operation
#define MODE_NORMAL 1
#define MODE_PREVIEW_STAY_ON 2
#define MODE_PROGRAM_ONLY 3
#define MODE_ON_AIR 4

#define TALLY_FLAG_OFF 0
#define TALLY_FLAG_PROGRAM 1
#define TALLY_FLAG_PREVIEW 2

// Define Neopixel status-LED options
#define NEOPIXEL_STATUS_FIRST 1
#define NEOPIXEL_STATUS_LAST 2
#define NEOPIXEL_STATUS_NONE 3

// FastLED
#define TALLY_DATA_PIN 13 // D7

int tempBrightness;
int updateColor = 0;
int numTallyLEDs;
int numStatusLEDs;
CRGB *leds;
CRGB *tallyLEDs;
CRGB *statusLED;
bool neopixelsUpdated = false;

// Initialize global variables
ESP8266WebServer server(80);
ATEMmin atemSwitcher;
TallyServer tallyServer;
ImprovWiFi improv(&Serial);

uint8_t state = STATE_STARTING;

// Define struct for holding tally settings (mostly to simplify EEPROM read and write, in order to persist settings)
struct Settings
{
    char tallyName[32] = "";
    uint8_t tallyNo;
    uint8_t tallyModeLED1;
    uint8_t tallyModeLED2;
    bool staticIP;
    IPAddress tallyIP;
    IPAddress tallySubnetMask;
    IPAddress tallyGateway;
    bool whichSwicher; // false -> swicher1, true -> switcher 2
    IPAddress switcherIP1;
    IPAddress switcherIP2;
    uint16_t neopixelsAmount;
    uint8_t neopixelStatusLEDOption;
    uint8_t neopixelBrightness; // 0-100%
    uint8_t ledBrightness;
    char updateURL[32] = "";
    int updateURLPort;
    char requestURLs[112] = "";
    bool colorTerminal = false;
};

Settings settings;

bool firstRun = true;

int bytesAvailable = false;
uint8_t readByte;
String readString;

void update_started()
{
    Serial.println("CALLBACK:  HTTP update process started");
}

void update_finished()
{
    Serial.println("CALLBACK:  HTTP update process finished");
}

void update_progress(int cur, int total)
{
    Serial.printf("CALLBACK:  HTTP update process at %d of %d bytes...\n", cur, total);
    if (updateColor % 5 == 0)
        setSTRIP(LED_GREEN);
    else
        setSTRIP(LED_OFF);
    FastLED.show();
    updateColor++;
}

void update_error(int err)
{
    Serial.printf("CALLBACK:  HTTP update fatal error code %d\n", err);
}

float getRemoteFirmwareVersion()
{
    WiFiClient client;
    HTTPClient http;

    String fullURL = settings.updateURL;
    fullURL += ":";
    fullURL += String(settings.updateURLPort);
    fullURL += "/tallyLight/firmware/version";

    if (http.begin(client, fullURL))
    {
        int httpCode = http.GET();
        if (httpCode == HTTP_CODE_OK)
        {
            String version = http.getString();
            return version.toFloat();
        }
        else
        {
            return 0; // server online
        }
    }
    return 0; // Return empty string if unable to fetch version
}

void removePrefix(char *url)
{
    if (strncmp(url, "http://", 7) == 0)
    {
        memmove(url, url + 7, strlen(url) - 6);
    }
    else if (strncmp(url, "https://", 8) == 0)
    {
        memmove(url, url + 8, strlen(url) - 7);
    }
}

void updateSoftware()
{

    WiFiClient client;

    float remoteVersion = getRemoteFirmwareVersion();

    if (remoteVersion != 0 && remoteVersion > firmware_version)
    {
        Serial.print("New firmware available: ");
        Serial.print(firmware_version);
        Serial.print(" -> ");
        Serial.print(remoteVersion);
        Serial.println(". Starting update...");

        // Add optional callback notifiers
        ESPhttpUpdate.onStart(update_started);
        ESPhttpUpdate.onEnd(update_finished);
        ESPhttpUpdate.onProgress(update_progress);
        ESPhttpUpdate.onError(update_error);

        ESPhttpUpdate.rebootOnUpdate(false); // remove automatic update

        char *shortURL = settings.updateURL;
        removePrefix(shortURL);
        Serial.println(shortURL);

        // Specify the server IP, port, and firmware path for update
        t_httpUpdate_return ret = ESPhttpUpdate.update(client, String(shortURL), uint16_t(settings.updateURLPort), String("/tallyLight/firmware/firmware.bin"));

        switch (ret)
        {
        case HTTP_UPDATE_FAILED:
            Serial.printf("HTTP_UPDATE_FAILED Error (%d): %s\n", ESPhttpUpdate.getLastError(), ESPhttpUpdate.getLastErrorString().c_str());
            Serial.println(F("Retry in 10secs!"));
            delay(10000); // Wait 10secs before retrying
            break;

        case HTTP_UPDATE_NO_UPDATES:
            Serial.println("HTTP_UPDATE_NO_UPDATES");
            break;

        case HTTP_UPDATE_OK:
            Serial.println("HTTP_UPDATE_OK");
            delay(1000); // Wait a second and restart
            ESP.restart();
            break;
        }
    }
    else if (remoteVersion == 0)
    {
        Serial.println("Server is OFFLINE!");
    }
    else
    {
        Serial.println("Firmware is up to date. No update needed.");
    }
}

void onImprovWiFiErrorCb(ImprovTypes::Error err)
{
}

void onImprovWiFiConnectedCb(const char *ssid, const char *password)
{
}

// Perform initial setup on power on
void setup()
{
    // Start Serial
    if (settings.colorTerminal)
        Serial.println("\u001b[37m");
    Serial.begin(115200);
    // for (int z = 0; z < 20; z++)
    //     Serial.println();
    Serial.print("\033[2J\033[H");
    Serial.println("########################");
    Serial.println("Serial started");
    Serial.println();

    // Read settings from EEPROM. WIFI settings are stored separately by the ESP
    EEPROM.begin(sizeof(settings)); // Needed on ESP8266 module, as EEPROM lib works a bit differently than on a regular Arduino
    EEPROM.get(0, settings);

    // Initialize LED strip
    if (0 < settings.neopixelsAmount && settings.neopixelsAmount <= 1000)
    {
        leds = new CRGB[settings.neopixelsAmount];
        FastLED.addLeds<NEOPIXEL, TALLY_DATA_PIN>(leds, settings.neopixelsAmount);

        if (settings.neopixelStatusLEDOption != NEOPIXEL_STATUS_NONE)
        {
            numStatusLEDs = 1;
            numTallyLEDs = settings.neopixelsAmount - numStatusLEDs;
            if (settings.neopixelStatusLEDOption == NEOPIXEL_STATUS_FIRST)
            {
                statusLED = leds;
                tallyLEDs = leds + numStatusLEDs;
            }
            else
            { // if last or other value
                statusLED = leds + numTallyLEDs;
                tallyLEDs = leds;
            }
        }
        else
        {
            numTallyLEDs = settings.neopixelsAmount;
            numStatusLEDs = 0;
            tallyLEDs = leds;
        }
    }
    else
    {
        settings.neopixelsAmount = 0;
        numTallyLEDs = 0;
        numStatusLEDs = 0;
    }

    tempBrightness = round(settings.neopixelBrightness * 255 / 100);
    FastLED.setBrightness(tempBrightness);
    setSTRIP(LED_OFF);
    setStatusLED(LED_BLUE);
    FastLED.show();

    Serial.println(settings.tallyName);

    IPAddress primaryDNS(1, 1, 1, 1);   // optional
    IPAddress secondaryDNS(8, 8, 4, 4); // optional
    if (settings.staticIP && settings.tallyIP != IPADDR_NONE)
    {
        WiFi.config(settings.tallyIP, settings.tallyGateway, settings.tallySubnetMask, primaryDNS, secondaryDNS);
    }
    else
    {
        settings.staticIP = false;
    }
    // Put WiFi into station mode and make it connect to saved network
    WiFi.mode(WIFI_STA);
    WiFi.hostname(settings.tallyName);
    Serial.println("------------------------");
    Serial.println("Press r to restart.");
    WiFi.setAutoReconnect(true);
    WiFi.begin();
    Serial.println("------------------------");
    Serial.print("Connecting to WiFi:  ");
    Serial.println(getSSID());

    // Initialize and begin HTTP server for handeling the web interface
    server.on("/", handleRoot);
    server.on("/save", handleSave);
    server.onNotFound(handleNotFound);
    server.begin();

    tallyServer.begin();

    improv.setDeviceInfo(CHIP_FAMILY, DISPLAY_NAME, VERSION, "Tally Light", "");
    improv.onImprovError(onImprovWiFiErrorCb);
    improv.onImprovConnected(onImprovWiFiConnectedCb);

    // Wait for result from first attempt to connect - This makes sure it only activates the softAP if it was unable to connect,
    // and not just because it hasn't had the time to do so yet. It's blocking, so don't use it inside loop()
    unsigned long start = millis();
    while ((!WiFi.status() || WiFi.status() >= WL_DISCONNECTED) && (millis() - start) < 10000LU)
    {
        bytesAvailable = Serial.available();
        if (bytesAvailable > 0)
        {
            readByte = Serial.read();
            improv.handleByte(readByte);
        }
    }

    // Set state to connecting before entering loop
    changeState(STATE_CONNECTING_TO_WIFI);

    Serial.flush();
}

/* Save settings to FLASH and restart ESP
(not working with WiFi settings!!!)
*/
void updateSettings()
{
    EEPROM.put(0, settings);
    EEPROM.commit();

    // Delay to let data be saved, and the response to be sent properly to the client
    server.close(); // Close server to flush and ensure the response gets to the client
    delay(100);

    // Change into STA mode to disable softAP
    WiFi.mode(WIFI_STA);
    delay(100); // Give it time to switch over to STA mode (this is important on the ESP32 at least)

    // Delay to apply settings before restart
    delay(100);
    ESP.restart();
}

bool correctCMD;

void loop()
{
    bytesAvailable = Serial.available();
    if (bytesAvailable > 0)
    {
        // readByte = Serial.read();
        Serial.flush();
        readString = Serial.readStringUntil('\n');
        readString.trim();
        if (settings.colorTerminal)
            Serial.println("\u001b[33m" + readString + "\u001b[37m");
        else
            Serial.println(readString);
        correctCMD = false;
        // improv.handleByte(readByte);
    }

    if (bytesAvailable && (readString == "h" || readString == "help"))
    {
        correctCMD = true;
        if (settings.colorTerminal)
        {
            Serial.println();
            Serial.println("All of the commands: ");
            Serial.println();
            Serial.println("* '\u001b[32mr\u001b[37m'/'\u001b[32mrestart\u001b[37m' - restart ESP");
            Serial.println("* '\u001b[32mcls\u001b[37m'/'\u001b[32mclear\u001b[37m' - clear terminal (not all terminal compatible)");
            Serial.println("* '\u001b[32mcolor\u001b[37m' - turn on/off colored terminal");
            Serial.println("* '\u001b[32mipconfig\u001b[37m' - simple IP info");
            Serial.println("* '\u001b[32mip a\u001b[37m'/'\u001b[32mipconfig /all\u001b[37m' - advanced IP info");
            Serial.println("* '\u001b[32mip\u001b[37m'/'\u001b[32mip set\u001b[37m' - change IP addresses");
            Serial.println("* '\u001b[32mwifi set\u001b[37m'/'\u001b[32mwifi\u001b[37m' - change WiFi SSID and password for ESP");
            Serial.println("* '\u001b[32mtally\u001b[37m' - change Tally number (no. of camera)");
            Serial.println("* '\u001b[32mls switcher\u001b[37m'/'\u001b[32mlss\u001b[37m' - show IP addresses of switches");
            Serial.println("* '\u001b[32mswitcher set ip\u001b[37m' - change switcher IP address");
            Serial.println("* '\u001b[32mswitcher set active\u001b[37m' - change switcher IP address");
        }
        else
        {
            Serial.println();
            Serial.println("All of the commands: ");
            Serial.println();
            Serial.println("* 'r'/'restart' - restart ESP");
            Serial.println("* 'cls'/'clear' - clear terminal (not all terminal compatible)");
            Serial.println("* 'color' - turn on/off colored terminal");
            Serial.println("* 'ipconfig' - simple IP info");
            Serial.println("* 'ip a'/'ipconfig /all' - advanced IP info");
            Serial.println("* 'ip'/'ip set' - change IP addresses");
            Serial.println("* 'wifi set'/'wifi' - change WiFi SSID and password for ESP");
            Serial.println("* 'tally' - change Tally number (no. of camera)");
            Serial.println("* 'ls switcher'/'lss' - show IP addresses of switches");
            Serial.println("* 'switcher set ip' - change switcher IP address");
            Serial.println("* 'switcher set active' - change switcher IP address");
        }
    }

    if (bytesAvailable && (readString == "r" || readString == "restart"))
    {
        correctCMD = true;
        Serial.println("Restarting ...");
        ESP.restart();
    }

    if (bytesAvailable && (readString == "cls" || readString == "clear"))
    {
        correctCMD = true;
        if (settings.colorTerminal)
            Serial.print("\033[2J\033[H");
        else
            Serial.println("Serial Clear is not supported on your Serial Terminal");
    }

    if (bytesAvailable && (readString == "tally" || readString == "tallynumber"))
    {
        correctCMD = true;
        Serial.print("Write Tally Number [1-40]: ");
        while (!Serial.available())
        {
            // waiting for serial data
        }
        String nrStr = Serial.readStringUntil('\n');
        nrStr.trim();
        int nr = nrStr.toInt();
        if (settings.colorTerminal)
            Serial.println("\u001b[33m" + nrStr + "\u001b[37m");
        else
            Serial.println(nr);
        if (nr > 0 && nr < 41)
        {
            settings.tallyNo = nr - 1;
            Serial.println("Tally number saved successfully!");
            delay(500);
            updateSettings();
        }
        else
        {
            if (settings.colorTerminal)
                Serial.println("\u001b[31mInvalid Tally Number!\u001b[37m");
            else
                Serial.println("Invalid Tally Number!");
        }
    }

    if (bytesAvailable && (readString == "switcher set ip" || readString == "switcher ip set"))
    {
        correctCMD = true;
        Serial.print("Which switcher you want to configure? [1-2]: ");
        while (!Serial.available())
        {
            // waiting for serial data
        }
        String nrStr = Serial.readStringUntil('\n');
        nrStr.trim();
        int nr = nrStr.toInt();
        if (settings.colorTerminal)
            Serial.println("\u001b[33m" + nrStr + "\u001b[37m");
        else
            Serial.println(nr);
        if (nr > 0 && nr < 3)
        {
            if (nr == 1)
                Serial.print("Switcher 1 IP: ");
            else
                Serial.print("Switcher 2 IP: ");
            while (!Serial.available())
            {
                // waiting for serial data
            }
            String ipStr = Serial.readStringUntil('\n');
            ipStr.trim();
            IPAddress ip;
            if (settings.colorTerminal)
                Serial.println("\u001b[33m" + ipStr + "\u001b[37m");
            else
                Serial.println(ipStr);
            if (ip.fromString(ipStr))
            {
                if (nr == 1)
                    settings.switcherIP1 = ip;
                else
                    settings.switcherIP2 = ip;

                Serial.println("Changed settings successfully");
                delay(500);
                updateSettings();
            }
            else
            {
                if (settings.colorTerminal)
                    Serial.println("\u001b[31mInvalid IP!\u001b[37m");
                else
                    Serial.println("Invalid IP!");
            }
        }
        else
        {
            if (settings.colorTerminal)
                Serial.println("\u001b[31mInvalid Switcher Number!\u001b[37m");
            else
                Serial.println("Invalid Switcher Number!");
        }
    }

    if (bytesAvailable && (readString == "switcher set active" || readString == "switcher active set"))
    {
        correctCMD = true;
        Serial.print("Which switcher you want to set as active one? [1-2]: ");
        while (!Serial.available())
        {
            // waiting for serial data
        }
        String nrStr = Serial.readStringUntil('\n');
        nrStr.trim();
        int nr = nrStr.toInt();
        if (settings.colorTerminal)
            Serial.println("\u001b[33m" + nrStr + "\u001b[37m");
        else
            Serial.println(nr);
        if (nr > 0 && nr < 3)
        {
            if (nr == 1)
                settings.whichSwicher = false;
            else
                settings.whichSwicher = true;
            Serial.flush();
            Serial.println(" Changed switcher to " + nr);
            delay(500);
            updateSettings();
        }
        else
        {
            if (settings.colorTerminal)
                Serial.println("\u001b[31mInvalid Switcher Number!\u001b[37m");
            else
                Serial.println("Invalid Switcher Number!");
        }
    }

    if (bytesAvailable && (readString == "lss"))
    {
        correctCMD = true;
        Serial.println((String) "Switcher 1 IP: " + settings.switcherIP1[0] + "." + settings.switcherIP1[1] + "." + settings.switcherIP1[2] + "." + settings.switcherIP1[3]);
        Serial.println((String) "Switcher 2 IP: " + settings.switcherIP2[0] + "." + settings.switcherIP2[1] + "." + settings.switcherIP2[2] + "." + settings.switcherIP2[3]);
    }

    if (bytesAvailable && (readString == "color"))
    {
        correctCMD = true;
        Serial.println("Do you want to have color terminal enabled? [yes/no]");
        while (!Serial.available())
        {
            // waiting for serial data
        }
        String answer = Serial.readStringUntil('\n');
        answer.trim();
        answer.toLowerCase();
        if (answer == "yes" || answer == "y")
        {
            Serial.println("\u001b[32mColor terminal enabled!\u001b[37m");
            settings.colorTerminal = true;
        }
        else
        {
            Serial.println("Color terminal disabled!");
            settings.colorTerminal = false;
        }
        delay(500);

        EEPROM.put(0, settings);
        EEPROM.commit();
    }

    if (bytesAvailable && (readString == "ipconfig"))
    {
        correctCMD = true;
        Serial.println("IP:                  " + WiFi.localIP().toString());
        Serial.println("Subnet Mask:         " + WiFi.subnetMask().toString());
        Serial.println("Gateway IP:          " + WiFi.gatewayIP().toString());
        // Serial.println("DNS:                 " + WiFi.dnsIP().toString());
    }

    if (bytesAvailable && (readString == "ip a" || readString == "ipconfig /all"))
    {
        correctCMD = true;
        Serial.println("IP:                  " + WiFi.localIP().toString());
        Serial.println("Subnet Mask:         " + WiFi.subnetMask().toString());
        Serial.println("Gateway IP:          " + WiFi.gatewayIP().toString());
        Serial.println("DNS:                 " + WiFi.dnsIP().toString());
        Serial.println("MAC:                 " + WiFi.macAddress());
    }

    if (bytesAvailable && (readString == "ip" || readString == "ip set"))
    {
        correctCMD = true;
        Serial.print("Configure automaticly? (DHCP) [Yes/No]: ");
        while (!Serial.available())
        {
            // Waiting for serial to be available
        }
        String answer = Serial.readStringUntil('\n');
        answer.trim();
        answer.toLowerCase();
        if (settings.colorTerminal)
            Serial.println("\u001b[33m" + answer + "\u001b[37m");
        else
            Serial.println(answer);
        if (answer == "yes" || answer == "y" || answer == "tak" || answer == "t")
        {
            settings.staticIP = false;
            delay(500);
            updateSettings();
        }

        Serial.flush();

        Serial.print("Write new IP address: ");

        while (!Serial.available())
        {
            // Waiting for serial to be available
        }
        // trimming off whitespaces
        String ipString = Serial.readStringUntil('\n');
        ipString.trim();
        IPAddress ip = IPAddress();
        if (settings.colorTerminal)
            Serial.println("\u001b[33m" + ipString + "\u001b[37m");
        else
            Serial.println(ipString);

        // checking if we have a valid IP address
        if (!ip.fromString(ipString))
        {
            if (settings.colorTerminal)
                Serial.println("\u001b[31mInvalid IP address.\u001b[37m");
            else
                Serial.println("Invalid IP address.");
        }
        else
        {
            // setting new ip address
            settings.tallyIP = ip;

            // MASK
            Serial.print("Write new mask: ");

            while (!Serial.available())
            {
                // Waiting for serial to be available
            }
            // trimming off whitespaces
            String maskString = Serial.readStringUntil('\n');
            maskString.trim();
            IPAddress mask = IPAddress();
            if (settings.colorTerminal)
                Serial.println("\u001b[33m" + maskString + "\u001b[37m");
            else
                Serial.println(maskString);

            // checking if we have a valid mask
            if (!mask.fromString(maskString))
            {
                if (settings.colorTerminal)
                    Serial.println("\u001b[31mInvalid mask.\u001b[37m");
                else
                    Serial.println("Invalid mask.");
            }
            else
            {
                // setting new mask
                settings.tallySubnetMask = mask;

                // GATEWAY
                Serial.print("Write new gateway: ");

                while (!Serial.available())
                { // Waiting for serial to be available
                } // trimming off whitespaces
                String gatewayString = Serial.readStringUntil('\n');
                gatewayString.trim();
                IPAddress gateway = IPAddress();
                if (settings.colorTerminal)
                    Serial.println("\u001b[33m" + gatewayString + "\u001b[37m");
                else
                    Serial.println(gatewayString);
                // checking if we have a valid gateway
                if (!gateway.fromString(gatewayString))
                {
                    if (settings.colorTerminal)
                        Serial.println("\u001b[31mInvalid gateway.\u001b[37m");
                    else
                        Serial.println("Invalid gateway.");
                }
                else
                { // setting new gateway
                    settings.tallyGateway = gateway;

                    settings.staticIP = true;

                    if (settings.colorTerminal)
                        Serial.println("\u001b[32mNew settings applied.\u001b[37m");
                    else
                        Serial.println("New settings applied.");
                    delay(500);
                    updateSettings();
                }
            }
        }
    }

    if (bytesAvailable && (readString == "wifi set" || readString == "wifi"))
    {
        correctCMD = true;
        String ssid = "", pwd = "";
        Serial.print("Write new SSID: ");
        while (!Serial.available())
        {
            // Waiting for serial to be available
        }
        // trimming off whitespaces
        String ssidString = Serial.readStringUntil('\n');
        ssidString.trim();
        if (settings.colorTerminal)
            Serial.println("\u001b[33m" + ssidString + "\u001b[37m");
        else
            Serial.println(ssidString);
        // checking if we have a valid SSID
        if (!ssidString.length() > 0)
        {
            if (settings.colorTerminal)
                Serial.println("\u001b[31mSSID must be longer\u001b[37m");
            else
                Serial.println("SSID must be longer");
        }
        else
        {
            // setting new SSID
            ssid = ssidString;

            // PASSWORD
            Serial.print("Write new password: ");
            while (!Serial.available())
            {
                // Waiting for serial to be available
            }
            // trimming off whitespaces
            String pwdString = Serial.readStringUntil('\n');
            pwdString.trim();
            if (settings.colorTerminal)
                Serial.println("\u001b[33m" + pwdString + "\u001b[37m");
            else
                Serial.println(pwdString);
            // checking if we have a valid pwd
            if (!pwdString.length() > 0)
            {
                if (settings.colorTerminal)
                    Serial.println("\u001b[31mPassword must be longer\u001b[37m");
                else
                    Serial.println("Password must be longer");
            }
            else
            {
                // setting new SSID
                pwd = pwdString;

                // SAVING settings

                EEPROM.put(0, settings);
                EEPROM.commit();

                // Delay to let data be saved, and the response to be sent properly to the client
                server.close(); // Close server to flush and ensure the response gets to the client
                delay(100);

                // Change into STA mode to disable softAP
                WiFi.mode(WIFI_STA);
                delay(100); // Give it time to switch over to STA mode (this is important on the ESP32 at least)

                if (ssid && pwd)
                {
                    WiFi.persistent(true); // Needed by ESP8266
                    // Pass in 'false' as 5th (connect) argument so we don't waste time trying to connect, just save the new SSID/PSK
                    // 3rd argument is channel - '0' is default. 4th argument is BSSID - 'NULL' is default.
                    WiFi.begin(ssid.c_str(), pwd.c_str(), 0, NULL, false);
                }

                // Delay to apply settings before restart
                delay(100);
                ESP.restart();
            }
        }
    }

    if (bytesAvailable && correctCMD == false)
    {
        // incorrect command
        if (settings.colorTerminal)
        {
            Serial.println("\u001b[31mCommand '\u001b[33m" + readString + "\u001b[31m' is not supported!");
            Serial.println("Press 'h' or 'help' for more information\u001b[37m");
        }
        else
        {
            Serial.println("Command '" + readString + "' is not supported!");
            Serial.println("Press 'h' or 'help' for more information");
        }
    }

    /* DONT TOUCH*/
    if (bytesAvailable)
    {
        if (settings.colorTerminal)
            Serial.print("\u001b[32mroot\u001b[34m:$ \u001b[37m");
        else
        {
            Serial.print("root:$ ");
        }
    }

    switch (state)
    {
    case STATE_CONNECTING_TO_WIFI:
        if (WiFi.status() == WL_CONNECTED)
        {
            WiFi.mode(WIFI_STA); // Disable softAP if connection is successful
            Serial.println("------------------------");
            // Serial.println("Connected to WiFi:   " + getSSID());
            Serial.println("IP:                  " + WiFi.localIP().toString());
            Serial.println("Subnet Mask:         " + WiFi.subnetMask().toString());
            Serial.println("Gateway IP:          " + WiFi.gatewayIP().toString());
            Serial.println("DNS:                 " + WiFi.dnsIP().toString());
            Serial.println("------------------------");
            Serial.print(F("Current firmware version: "));
            Serial.println(firmware_version);
            updateSoftware();

            changeState(STATE_CONNECTING_TO_SWITCHER);
        }
        else if (firstRun)
        {
            firstRun = false;
            Serial.println("Unable to connect. Serving \"Tally Light setup\" WiFi for configuration, while still trying to connect...");
            WiFi.softAP((String)DISPLAY_NAME + " setup");
            WiFi.mode(WIFI_AP_STA); // Enable softAP to access web interface in case of no WiFi
            setStatusLED(LED_WHITE);
        }
        break;
    case STATE_CONNECTING_TO_SWITCHER:
        // Initialize a connection to the switcher:
        if (firstRun)
        {
            if (!settings.whichSwicher)
            {
                atemSwitcher.begin(settings.switcherIP1);
            }
            else
            {
                atemSwitcher.begin(settings.switcherIP2);
            }
            // atemSwitcher.serialOutput(0xff); //Makes Atem library print debug info
            Serial.println("------------------------");
            Serial.println("Connecting to switcher...");
            if (!settings.whichSwicher)
            {
                Serial.println((String) "Switcher IP:         " + settings.switcherIP1[0] + "." + settings.switcherIP1[1] + "." + settings.switcherIP1[2] + "." + settings.switcherIP1[3]);
            }
            else
            {
                Serial.println((String) "Switcher IP:         " + settings.switcherIP2[0] + "." + settings.switcherIP2[1] + "." + settings.switcherIP2[2] + "." + settings.switcherIP2[3]);
            }
            firstRun = false;

            if (settings.colorTerminal)
                Serial.print("\u001b[32mroot\u001b[34m:$ \u001b[37m");
            else
            {
                Serial.print("root:$ ");
            }
        }
        atemSwitcher.runLoop();
        if (atemSwitcher.isConnected())
        {
            changeState(STATE_RUNNING);
            Serial.println("Connected to switcher");
            changeState(STATE_RUNNING);
        }
        break;

    case STATE_RUNNING:
        // Handle data exchange and connection to swithcher
        atemSwitcher.runLoop();

        int tallySources = atemSwitcher.getTallyByIndexSources();
        tallyServer.setTallySources(tallySources);
        for (int i = 0; i < tallySources; i++)
        {
            tallyServer.setTallyFlag(i, atemSwitcher.getTallyByIndexTallyFlags(i));
        }

        // Switch state if ATEM connection is lost...
        if (!atemSwitcher.isConnected())
        { // will return false if the connection was lost
            Serial.println("------------------------");
            Serial.println("Connection to Switcher lost...");
            changeState(STATE_CONNECTING_TO_SWITCHER);

            // Reset tally server's tally flags, so clients turn off their lights.
            tallyServer.resetTallyFlags();
        }

        // Handle Tally Server
        tallyServer.runLoop();

        // Set LED and Neopixel colors accordingly
        int color = getLedColor(settings.tallyModeLED1, settings.tallyNo);
        setSTRIP(color);
        break;
    }

    // Switch state if WiFi connection is lost...
    if (WiFi.status() != WL_CONNECTED && state != STATE_CONNECTING_TO_WIFI)
    {
        Serial.println("------------------------");
        Serial.println("WiFi connection lost...");
        changeState(STATE_CONNECTING_TO_WIFI);

        // Force atem library to reset connection, in order for status to read correctly on website.
        if (!settings.whichSwicher)
        {
            atemSwitcher.begin(settings.switcherIP1);
        }
        else
        {
            atemSwitcher.begin(settings.switcherIP2);
        }
        atemSwitcher.connect();

        // Reset tally server's tally flags, They won't get the message, but it'll be reset for when the connectoin is back.
        tallyServer.resetTallyFlags();
    }

    // Show strip only on updates
    if (neopixelsUpdated)
    {
        FastLED.show();
        neopixelsUpdated = false;
    }

    // Handle web interface
    server.handleClient();
}

// Handle the change of states in the program
void changeState(uint8_t stateToChangeTo)
{
    firstRun = true;
    switch (stateToChangeTo)
    {
    case STATE_CONNECTING_TO_WIFI:
        state = STATE_CONNECTING_TO_WIFI;
        setStatusLED(LED_BLUE);
        setSTRIP(LED_OFF);
        break;
    case STATE_CONNECTING_TO_SWITCHER:
        state = STATE_CONNECTING_TO_SWITCHER;
        setStatusLED(LED_PINK);
        setSTRIP(LED_OFF);
        break;
    case STATE_RUNNING:
        state = STATE_RUNNING;
        setStatusLED(LED_ORANGE);
        break;
    }
}

// Set the color of a LED using the given pins
void setLED(uint8_t color, int pinRed, int pinGreen, int pinBlue)
{

    uint8_t ledBrightness = settings.ledBrightness;
    void (*writeFunc)(uint8_t, uint8_t);
    if (ledBrightness >= 0xff)
    {
        writeFunc = &digitalWrite;
        ledBrightness = 1;
    }
    else
    {
        writeFunc = &analogWriteWrapper;
    }

    switch (color)
    {
    case LED_OFF:
        digitalWrite(pinRed, 0);
        digitalWrite(pinGreen, 0);
        digitalWrite(pinBlue, 0);
        break;
    case LED_RED:
        writeFunc(pinRed, ledBrightness);
        digitalWrite(pinGreen, 0);
        digitalWrite(pinBlue, 0);
        break;
    case LED_GREEN:
        digitalWrite(pinRed, 0);
        writeFunc(pinGreen, ledBrightness);
        digitalWrite(pinBlue, 0);
        break;
    case LED_BLUE:
        digitalWrite(pinRed, 0);
        digitalWrite(pinGreen, 0);
        writeFunc(pinBlue, ledBrightness);
        break;
    case LED_YELLOW:
        writeFunc(pinRed, ledBrightness);
        writeFunc(pinGreen, ledBrightness);
        digitalWrite(pinBlue, 0);
        break;
    case LED_PINK:
        writeFunc(pinRed, ledBrightness);
        digitalWrite(pinGreen, 0);
        writeFunc(pinBlue, ledBrightness);
        break;
    case LED_WHITE:
        writeFunc(pinRed, ledBrightness);
        writeFunc(pinGreen, ledBrightness);
        writeFunc(pinBlue, ledBrightness);
        break;
    }
}

void analogWriteWrapper(uint8_t pin, uint8_t value)
{
    analogWrite(pin, value);
}

// Set the color of the LED strip, except for the status LED
void setSTRIP(uint8_t color)
{
    if (numTallyLEDs > 0 && tallyLEDs[0] != color_led[color])
    {
        for (int i = 0; i < numTallyLEDs; i++)
        {
            tallyLEDs[i] = color_led[color];
        }
        neopixelsUpdated = true;
    }
}

// Set the single status LED (last LED)
void setStatusLED(uint8_t color)
{
    if (numStatusLEDs > 0 && statusLED[0] != color_led[color])
    {
        for (int i = 0; i < numStatusLEDs; i++)
        {
            statusLED[i] = color_led[color];
            if (color == LED_ORANGE)
            {
                statusLED[i].fadeToBlackBy(230);
            }
            else
            {
                statusLED[i].fadeToBlackBy(0);
            }
        }
        neopixelsUpdated = true;
    }
}

int getTallyState(uint16_t tallyNo)
{
    if (tallyNo >= atemSwitcher.getTallyByIndexSources())
    { // out of range
        return TALLY_FLAG_OFF;
    }

    uint8_t tallyFlag = atemSwitcher.getTallyByIndexTallyFlags(tallyNo);
    // Serial.println(tallyFlag);
    if (tallyFlag & TALLY_FLAG_PROGRAM)
    {
        return TALLY_FLAG_PROGRAM;
    }
    else if (tallyFlag & TALLY_FLAG_PREVIEW)
    {
        return TALLY_FLAG_PREVIEW;
    }
    else
    {
        return TALLY_FLAG_OFF;
    }
}

int getLedColor(int tallyMode, int tallyNo)
{
    if (tallyMode == MODE_ON_AIR)
    {
        if (atemSwitcher.getStreamStreaming())
        {
            return LED_RED;
        }
        return LED_OFF;
    }

    int tallyState = getTallyState(tallyNo);

    if (tallyState == TALLY_FLAG_PROGRAM)
    { // if tally live
        // Serial.println("On live");
        return LED_RED;
    }
    else if ((tallyState == TALLY_FLAG_PREVIEW      // if tally preview
              || tallyMode == MODE_PREVIEW_STAY_ON) // or preview stay on
             && tallyMode != MODE_PROGRAM_ONLY)
    { // and not program only
        // Serial.println("On live");
        return LED_GREEN;
    }
    else
    { // if tally is neither
        // Serial.println("OFF");
        return LED_OFF;
    }
}

// Serve setup web page to client, by sending HTML with the correct variables
void handleRoot()
{
    String html = "<!DOCTYPE html><html><head><link rel=\"icon\" type=\"image/x-icon\" href=\"https://avatars.githubusercontent.com/u/104673265?v=4\"><meta charset=\"UTF-8\"><meta name=\"viewport\"content=\"width=device-width,initial-scale=1.0\"><title>Tally Light</title></head>";
    html += "<style>.switch {position: relative;display: inline-block;width: 40px;height: 20px; margin:0 15px}/* Hide default HTML checkbox */.switch input {opacity: 0;width: 0;height: 0;}/* The slider */.slider {position: absolute;cursor: pointer;top: 0;left: 0;right: 0;bottom: 0;background-color: #07b50c;-webkit-transition: .3s;transition: .3s;}.slider:before {position: absolute;content: \"\";height: 15px;width: 15px;left: 2.5px;bottom: 2.5px;background-color: white;-webkit-transition: .3s;transition: .3s;}input:checked + .slider:before {-webkit-transform: translateX(20px);-ms-transform: translateX(20px);transform: translateX(20px);}/* Rounded sliders */.slider.round {border-radius: 34px;}.slider.round:before {border-radius: 50%;}#staticIP {accent-color: #07b50c;}.s777777 h1,.s777777 h2 {color: #07b50c;}body {display: flex;align-items: center;justify-content: center;width: 100vw;overflow-x: hidden;font-family: \"Arial\", sans-serif;background-color: #242424;color: #fff;table {width: 80%;max-width: 1200px;background-color: #3b3b3b;padding: 20px;margin: 20px;border-radius: 10px;box-shadow: 0 0 10px rgba(0, 0, 0, 0.5);border-radius: 12px;overflow: hidden;border-spacing: 0;padding: 5px 45px;box-sizing: border-box;}tr.s777777 {background-color: transparent;color: #07b50c !important;}tr.cccccc {background-color: transparent;} tr.cccccc p {font-size: 16px;}input[type=\"checkbox\"] {width: 17.5px;aspect-ratio: 1;cursor: pointer;}td {cursor: default;user-select: none;}input {border-radius: 6px;cursor: text;}select {border-radius: 6px;cursor: pointer;}td.fr input {position:relative; left:135px;background-color: #07b50c !important; -webkit-appearance: none; accent-color: #07b50c !important;color: white;padding: 7px 17px;cursor: pointer;}* {line-height: 1.2;}@media screen and (max-width: 730px) {body {width: 100vw;margin: 0;padding: 10px;}table {width: 100%;padding: 0 10px;margin: 0;}}</style>";
    html += "<script>function changeAdvancedOptions(state) { var elements = document.querySelectorAll(\".advanced\"); var button = document.querySelector(\".advButton\"); if (state == false) { elements.forEach(function (element) { element.style.display = \"table-row\"; }); button.innerHTML = \"Ukryj zazwansowane ustawienia\"; advancedOptions = true;} else { elements.forEach(function (element) { element.style.display = \"none\"; }); button.innerHTML = \"Pokaż zazwansowane ustawienia\"; advancedOptions = false;} } let advancedOptions = false;function sendChangeSwichRequest() { if (document.querySelector(\"#switcherHidden\").disabled) { fetchSwitchState(true); alert(\"Wysłano polecenie zmiany miksera video na mikser 2\"); } else { fetchSwitchState(false); alert(\"Wysłano polecenie zmiany miksera video na mikser 1\"); } }";
    html += "function fetchSwitchState(state) { const urlslong = \"";
    html += settings.requestURLs;
    html += "\"; const urls = urlslong.split(\",\"); const switcherValue = state; urls.forEach((ip) => { const requestOptions = { method: \"POST\", headers: { \"Content-Type\": \"application/x-www-form-urlencoded\" }, body: `switcher=${switcherValue}`, }; let fullurl = \"http://\" + ip + \"/save\"; console.log(fullurl+\" , \"+state); fetch(fullurl, requestOptions) .then((response) => { if (!response.ok) { throw new Error(`Błąd HTTP! Kod: ${response.status}`); } return response.text(); }) .then((data) => { console.log(`Odpowiedź z ${ip}: ${data}`); }) .catch((error) => { console.error(`Błąd: ${error.message}`); }); }); } function validateIP() {if (document.querySelector(\"#staticIPHidden\").disabled) { function validateNetwork(ip1Array, ip2Array, maskArray) { const ip1ArrayAsArray = Array.from(ip1Array); const ip2ArrayAsArray = Array.from(ip2Array); const maskArrayAsArray = Array.from(maskArray); if (maskArrayAsArray.some((octet) => octet < 0 || octet > 255)) { console.log(\"Błędna maska sieci.\"); alert(\"Błędna maska sieci.\\n \" + maskArrayAsArray); return 0; } if ( ip1ArrayAsArray.some((octet) => octet < 0 || octet > 255) || ip2ArrayAsArray.some((octet) => octet < 0 || octet > 255) ) { console.log(\"Błędny format adresu IP.\"); alert(\"Błędny format adresu IP lub maski.\\n\" + ip1ArrayAsArray + \" oraz \" + ip2ArrayAsArray); return 0; } for (let i = 0; i < 4; i++) { if ((ip1ArrayAsArray[i] & maskArrayAsArray[i]) !== (ip2ArrayAsArray[i] & maskArrayAsArray[i])) { console.log(\"Adresy nie należą do tej samej sieci.\"); alert(\"Adresy nie należą do tej samej sieci.\\n \" + ip1ArrayAsArray + \" oraz \" + ip2ArrayAsArray); return 0; } } console.log(\"wszystko działa\"); return 1; } var ipaddres = []; document.querySelectorAll(\".tip\").forEach(function (element) { ipaddres.push(element.value); }); var mask = []; document.querySelectorAll(\".tm\").forEach(function (element) { mask.push(element.value); }); var gateway = []; document.querySelectorAll(\".tg\").forEach(function (element) { gateway.push(element.value); }); var mixer1 = []; document.querySelectorAll(\".mip1\").forEach(function (element) { mixer1.push(element.value); }); var mixer2 = []; document.querySelectorAll(\".mip2\").forEach(function (element) { mixer2.push(element.value); }); if (!validateNetwork(ipaddres, gateway, mask)) { console.log(\"cos nie działa\"); } else if (!validateNetwork(ipaddres, mixer1, mask)) { console.log(\"cos nie działa\"); } else if (!validateNetwork(ipaddres, mixer2, mask)) {  console.log(\"cos nie działa\"); } }} function toggleSwitcherChange(){var enabled = document.getElementById(\"switcher\").checked;document.getElementById(\"switcherHidden\").disabled = enabled;}function switchIpField(e){console.log(\"switch\");console.log(e);var target=e.srcElement||e.target;var maxLength=parseInt(target.attributes[\"maxlength\"].value,10);var myLength=target.value.length;if(myLength>=maxLength){var next=target.nextElementSibling;if(next!=null){if(next.className.includes(\"IP\")){next.focus();}}}else if(myLength==0){var previous=target.previousElementSibling;if(previous!=null){if(previous.className.includes(\"IP\")){previous.focus();}}}}function ipFieldFocus(e){console.log(\"focus\");console.log(e);var target=e.srcElement||e.target;target.select();}function load(){var containers=document.getElementsByClassName(\"IP\");for(var i=0;i<containers.length;i++){var container=containers[i];container.oninput=switchIpField;container.onfocus=ipFieldFocus;}containers=document.getElementsByClassName(\"tIP\");for(var i=0;i<containers.length;i++){var container=containers[i];container.oninput=switchIpField;container.onfocus=ipFieldFocus;}toggleStaticIPFields();}function toggleStaticIPFields(){var enabled=document.getElementById(\"staticIP\").checked;document.getElementById(\"staticIPHidden\").disabled=enabled;var staticIpFields=document.getElementsByClassName('tIP');for(var i=0;i<staticIpFields.length;i++){staticIpFields[i].disabled=!enabled;}}</script><style>a{color:#0F79E0}</style><body style=\"font-family:Verdana;white-space:nowrap;\"onload=\"load()\"><table cellpadding=\"2\"style=\"width:100%\"><tr class=\"s777777\"style=\"color:#ffffff;font-size:.8em;\"><td colspan=\"3\"><h1>&nbsp;" +
            (String)DISPLAY_NAME +
            "</h1><h2>&nbsp;Status:</h2></td></tr><tr><td>Status połączenia:</td><td colspan=\"2\" style=\"width:75%\">";
    switch (WiFi.status())
    {
    case WL_CONNECTED:
        html += "Połączono do sieci";
        break;
    case WL_NO_SSID_AVAIL:
        html += "Sieć nie znaleziona";
        break;
    case WL_CONNECT_FAILED:
        html += "Nie poprawne hasło";
        break;
    case WL_IDLE_STATUS:
        html += "Zmiana stanu...";
        break;
    case WL_DISCONNECTED:
        html += "Tryb stacji niedostępny i nie wiem co to znaczy";
        break;
    case WL_CONNECTION_LOST:
        html += "Utracono połączenie WiFi";
        break;
    default:
        html += "Timeout";
        break;
    }

    html += "</td></tr><tr><td>Nazwa sieci (SSID):</td><td colspan=\"2\">";
    html += getSSID();
    html += "</td></tr><tr><td><br></td></tr><tr><td>Siła sygnału:</td><td colspan=\"2\">";
    html += WiFi.RSSI();
    html += " dBm</td></tr>";
    html += "<tr><td>Statyczny adres IP:</td><td colspan=\"2\">";
    html += settings.staticIP == true ? "Tak" : "Nie";
    html += "</td></tr><tr><td> Adres IP:</td><td colspan=\"2\">";
    html += WiFi.localIP().toString();
    html += "</td></tr><tr><td>Maska sieciowa: </td><td colspan=\"2\">";
    html += WiFi.subnetMask().toString();
    html += "</td></tr><tr><td>Brama domyślna: </td><td colspan=\"2\">";
    html += WiFi.gatewayIP().toString();
    html += "</td></tr><tr><td><br></td></tr>";
    html += "<tr><td>Status połączenia z ATEM:</td><td colspan=\"2\">";
    if (atemSwitcher.isRejected())
        html += "Połączenie odrzucone - brak wolnego slotu";
    else if (atemSwitcher.isConnected())
        html += "Połączono"; // - Wating for initialization";
    else if (WiFi.status() == WL_CONNECTED)
        html += "Rozłączono - brak odpowiedzi od ATEM";
    else
        html += "Rozłączono - oczekiwanie na sieć";
    html += "</td></tr><tr><td>Adres IP ATEM:</td><td colspan=\"2\">";
    if (!settings.whichSwicher)
    {
        html += (String)settings.switcherIP1[0] + '.' + settings.switcherIP1[1] + '.' + settings.switcherIP1[2] + '.' + settings.switcherIP1[3];
    }
    else
    {
        html += (String)settings.switcherIP2[0] + '.' + settings.switcherIP2[1] + '.' + settings.switcherIP2[2] + '.' + settings.switcherIP2[3];
    }

    html += "</td></tr><tr><td><br></td></tr>";
    html += "<tr class=\"s777777\"style=\"color:#ffffff;font-size:.8em;\"><td colspan=\"3\"><h2>&nbsp;Ustawienia:</h2></td></tr><form action=\"/save\"method=\"post\"><tr><td>Nazwa urządzenia: </td><td><input type=\"text\"size=\"34\"maxlength=\"30\"name=\"tName\"value=\"";
    html += WiFi.hostname();
    html += "\"required/></td></tr><tr><td>Numer kamery: </td><td><input type=\"number\"size=\"5\"min=\"1\"max=\"41\"name=\"tNo\"value=\"";
    html += (settings.tallyNo + 1);
    html += "\"required/></td></tr><tr style=\"display:none;\"><td>Tally Light mode (LED 1):&nbsp;</td><td><select name=\"tModeLED1\"><option value=\"";
    html += (String)MODE_NORMAL + "\"";
    if (settings.tallyModeLED1 == MODE_NORMAL)
        html += "selected";
    html += ">Normal</option><option value=\"";
    html += (String)MODE_PREVIEW_STAY_ON + "\"";
    if (settings.tallyModeLED1 == MODE_PREVIEW_STAY_ON)
        html += "selected";
    html += ">Preview stay on</option><option value=\"";
    html += (String)MODE_PROGRAM_ONLY + "\"";
    if (settings.tallyModeLED1 == MODE_PROGRAM_ONLY)
        html += "selected";
    html += ">Program only</option><option value=\"";
    html += (String)MODE_ON_AIR + "\"";
    if (settings.tallyModeLED1 == MODE_ON_AIR)
        html += "selected";
    html += ">On Air</option></select></td></tr><tr style=\"display:none;\"><td>Tally Light mode (LED 2):</td><td><select name=\"tModeLED2\"><option value=\"";
    html += (String)MODE_NORMAL + "\"";
    if (settings.tallyModeLED2 == MODE_NORMAL)
        html += "selected";
    html += ">Normal</option><option value=\"";
    html += (String)MODE_PREVIEW_STAY_ON + "\"";
    if (settings.tallyModeLED2 == MODE_PREVIEW_STAY_ON)
        html += "selected";
    html += ">Preview stay on</option><option value=\"";
    html += (String)MODE_PROGRAM_ONLY + "\"";
    if (settings.tallyModeLED2 == MODE_PROGRAM_ONLY)
        html += "selected";
    html += ">Program only</option><option value=\"";
    html += (String)MODE_ON_AIR + "\"";
    if (settings.tallyModeLED2 == MODE_ON_AIR)
        html += "selected";
    html += ">On Air</option></select></td></tr><tr style=\"display:none;\"><td> Jasność diód: </td><td><input type=\"number\"size=\"5\"min=\"0\"max=\"100\"name=\"ledBright\"value=\"";
    html += settings.ledBrightness;
    html += "\"required/></td></tr><tr style=\"display:none;\" class=\"advanced\"><td>Ilość ledów:</td><td><input type=\"number\"size=\"5\"min=\"0\"max=\"1000\"name=\"neoPxAmount\"value=\"";
    html += settings.neopixelsAmount;
    html += "\"required/></td></tr><tr><td>Dioda statusu: </td><td><select name=\"neoPxStatus\"><option value=\"";
    html += (String)NEOPIXEL_STATUS_FIRST + "\"";
    if (settings.neopixelStatusLEDOption == NEOPIXEL_STATUS_FIRST)
        html += "selected";
    html += ">Pierwsza</option><option value=\"";
    html += (String)NEOPIXEL_STATUS_LAST + "\"";
    if (settings.neopixelStatusLEDOption == NEOPIXEL_STATUS_LAST)
        html += "selected";
    html += ">Ostatnia</option><option value=\"";
    html += (String)NEOPIXEL_STATUS_NONE + "\"";
    if (settings.neopixelStatusLEDOption == NEOPIXEL_STATUS_NONE)
        html += "selected";
    html += ">Żadna</option></select></td></tr><tr><td> Jasność ledów: </td><td><input type=\"number\"size=\"5\"min=\"0\"max=\"100\"name=\"neoPxBright\"value=\"";
    html += settings.neopixelBrightness;
    html += "\"required/>%</td></tr><tr><td><br></td></tr><tr><td>Nazwa sieci (SSID): </td><td><input type =\"text\"size=\"34\"maxlength=\"30\"name=\"ssid\"value=\"";
    html += getSSID();
    html += "\"required/></td></tr><tr><td>Hasło do sieci: </td><td><input type=\"password\"size=\"34\"maxlength=\"30\"name=\"pwd\"pattern=\"^$|.{8,32}\"value=\"";
    if (WiFi.isConnected()) // As a minimum security meassure, to only send the wifi password if it's currently connected to the given network.
        html += WiFi.psk();
    html += "\"onmouseenter='this.type=\"text\"'onmouseleave='this.type=\"password\"'/></td><td></td></tr><tr><td><br></td></tr><tr><td>Użyj statycznego adresu IP: </td><td><input type=\"hidden\"id=\"staticIPHidden\"name=\"staticIP\"value=\"false\"/><input id=\"staticIP\"type=\"checkbox\"name=\"staticIP\"value=\"true\"onchange=\"toggleStaticIPFields()\"";
    if (settings.staticIP)
        html += "checked";
    html += "/></td></tr><tr><td> Adres IP: </td><td><input class=\"tIP tip\"type=\"text\"size=\"3\"maxlength=\"3\"name=\"tIP1\"pattern=\"\\d{0,3}\"value=\"";
    html += settings.tallyIP[0];
    html += "\"required/>. <input class=\"tIP tip\"type=\"text\"size=\"3\"maxlength=\"3\"name=\"tIP2\"pattern=\"\\d{0,3}\"value=\"";
    html += settings.tallyIP[1];
    html += "\"required/>. <input class=\"tIP tip\"type=\"text\"size=\"3\"maxlength=\"3\"name=\"tIP3\"pattern=\"\\d{0,3}\"value=\"";
    html += settings.tallyIP[2];
    html += "\"required/>. <input class=\"tIP tip\"type=\"text\"size=\"3\"maxlength=\"3\"name=\"tIP4\"pattern=\"\\d{0,3}\"value=\"";
    html += settings.tallyIP[3];
    html += "\"required/></td></tr><tr><td>Maska sieciowa: </td><td><input class=\"tIP tm\"type=\"text\"size=\"3\"maxlength=\"3\"name=\"mask1\"pattern=\"\\d{0,3}\"value=\"";
    html += settings.tallySubnetMask[0];
    html += "\"required/>. <input class=\"tIP tm\"type=\"text\"size=\"3\"maxlength=\"3\"name=\"mask2\"pattern=\"\\d{0,3}\"value=\"";
    html += settings.tallySubnetMask[1];
    html += "\"required/>. <input class=\"tIP tm\"type=\"text\"size=\"3\"maxlength=\"3\"name=\"mask3\"pattern=\"\\d{0,3}\"value=\"";
    html += settings.tallySubnetMask[2];
    html += "\"required/>. <input class=\"tIP tm\"type=\"text\"size=\"3\"maxlength=\"3\"name=\"mask4\"pattern=\"\\d{0,3}\"value=\"";
    html += settings.tallySubnetMask[3];
    html += "\"required/></td></tr><tr><td>Brama domyślna: </td><td><input class=\"tIP tg\"type=\"text\"size=\"3\"maxlength=\"3\"name=\"gate1\"pattern=\"\\d{0,3}\"value=\"";
    html += settings.tallyGateway[0];
    html += "\"required/>. <input class=\"tIP tg\"type=\"text\"size=\"3\"maxlength=\"3\"name=\"gate2\"pattern=\"\\d{0,3}\"value=\"";
    html += settings.tallyGateway[1];
    html += "\"required/>. <input class=\"tIP tg\"type=\"text\"size=\"3\"maxlength=\"3\"name=\"gate3\"pattern=\"\\d{0,3}\"value=\"";
    html += settings.tallyGateway[2];
    html += "\"required/>. <input class=\"tIP tg\"type=\"text\"size=\"3\"maxlength=\"3\"name=\"gate4\"pattern=\"\\d{0,3}\"value=\"";
    html += settings.tallyGateway[3];
    html += "\"required/></td></tr>";
    html += "<tr><td><br></td></tr>";
    // switcher checkbox
    html += "<tr><td><span>Mikser 1</span><span><label class=\"switch\"><input type=\"hidden\" id=\"switcherHidden\" name=\"switcher\" value=\"false\"";
    if (settings.whichSwicher)
        html += "disabled";
    html += "><input type=\"checkbox\" id=\"switcher\" name=\"switcher\" ";
    if (settings.whichSwicher)
        html += "checked";
    html += " value=\"true\" onchange=\"toggleSwitcherChange()\"><span class=\"slider round\"></span></label></span><span>Mikser 2</span></td><td><button type=\"button\" onclick=\"sendChangeSwichRequest()\" style=\"border-radius:6px;background-color: #07b50c !important;-webkit-appearance: none; accent-color: #07b50c !important;color: white;padding: 5px 10px;cursor: pointer;\">Wyślij do wszystkich</button></td></tr>";
    html += "<tr style=\"display:none;\" class=\"advanced\"><td>IP urządzeń do automatycznej zmiany <br>miksera (oddzielone przecinkami)</td><td><input type=\"text\" size=\"34\" maxlength=\"112\" name=\"requestURLs\" value=\"";
    html += settings.requestURLs;
    html += "\" required></td></tr>";
    html += "<tr><td>Adres IP miksera 1: </td><td><input class=\"IP mip1\"type=\"text\"size=\"3\"maxlength=\"3\"name=\"aIP11\"pattern=\"\\d{0,3}\"value=\""; // aIP[swicher number][octet]
    html += settings.switcherIP1[0];
    html += "\"required/>. <input class=\"IP mip1\"type=\"text\"size=\"3\"maxlength=\"3\"name=\"aIP12\"pattern=\"\\d{0,3}\"value=\"";
    html += settings.switcherIP1[1];
    html += "\"required/>. <input class=\"IP mip1\"type=\"text\"size=\"3\"maxlength=\"3\"name=\"aIP13\"pattern=\"\\d{0,3}\"value=\"";
    html += settings.switcherIP1[2];
    html += "\"required/>. <input class=\"IP mip1\"type=\"text\"size=\"3\"maxlength=\"3\"name=\"aIP14\"pattern=\"\\d{0,3}\"value=\"";
    html += settings.switcherIP1[3];
    html += "\"required/></tr>";
    html += "<tr><td>Adres IP miksera 2: </td><td><input class=\"IP mip2\"type=\"text\"size=\"3\"maxlength=\"3\"name=\"aIP21\"pattern=\"\\d{0,3}\"value=\"";
    html += settings.switcherIP2[0];
    html += "\"required/>. <input class=\"IP mip2\"type=\"text\"size=\"3\"maxlength=\"3\"name=\"aIP22\"pattern=\"\\d{0,3}\"value=\"";
    html += settings.switcherIP2[1];
    html += "\"required/>. <input class=\"IP mip2\"type=\"text\"size=\"3\"maxlength=\"3\"name=\"aIP23\"pattern=\"\\d{0,3}\"value=\"";
    html += settings.switcherIP2[2];
    html += "\"required/>. <input class=\"IP mip2\"type=\"text\"size=\"3\"maxlength=\"3\"name=\"aIP24\"pattern=\"\\d{0,3}\"value=\"";
    html += settings.switcherIP2[3];
    html += "\"required/></tr>";
    html += "<tr style=\"display:none;\" class=\"advanced\"><td>Adres URL do serwera aktualizacji</td><td><input type=\"text\" size=\"34\" maxlength=\"30\" name=\"updateURL\" value=\"";
    html += settings.updateURL;
    html += "\" required></td></tr>";
    html += "<tr style=\"display:none;\" class=\"advanced\"><td>Port serwera aktualizacji</td><td><input type=\"number\" size=\"5\" min=\"1\" max=\"65536\" name=\"updateURLPort\" value=\"";
    html += settings.updateURLPort;
    html += "\" required></td></tr>";
    html += "<tr><td><br></td></tr><tr><td><button class=\"advButton\" type=\"button\" onclick=\"changeAdvancedOptions(advancedOptions)\" style=\"border-radius:6px;background-color: #07b50c !important;-webkit-appearance: none; accent-color: #07b50c !important;color: white;padding: 5px 10px;cursor: pointer;\"> Pokaż zaawansowane ustawienia </button></td></td><td class=\"fr\"><input type=\"submit\"value=\"Zapisz zmiany\" onmouseover=\"validateIP()\"/></td></tr></form><tr class=\"cccccc\"style=\"font-size: .8em;\"><td colspan=\"3\"><p>&nbsp;Stworzone przez <a href=\"https://github.com/Dodo765\" target=\"_blank\">Dominik Kawalec</a></p><p>&nbsp;Napisane w oparciu o bibliotekę <a href=\"https://github.com/kasperskaarhoj/SKAARHOJ-Open-Engineering/tree/master/ArduinoLibs\" target=\"_blank\">SKAARHOJ</a></p></td></tr></table></body></html>";
    server.send(200, "text/html", html);
}

// Save new settings from client in EEPROM and restart the ESP8266 module
void handleSave()
{
    if (server.method() != HTTP_POST)
    {
        server.send(405, "text/html", "<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><meta name=\"viewport\"content=\"width=device-width, initial-scale=1.0\"><title>Tally Light</title><style>#staticIP {accent-color: #07b50c;}.s777777 h1,.s777777 h2 {color: #07b50c;}.fr{float: right}body {display: flex;align-items: center;justify-content: center;width: 100vw;overflow-x: hidden;font-family: \"Arial\", sans-serif;background-color: #242424;color: #fff;table {width: 80%;max-width: 1200px;background-color: #3b3b3b;padding: 20px;margin: 20px;border-radius: 10px;box-shadow: 0 0 10px rgba(0, 0, 0, 0.5);border-radius: 12px;overflow: hidden;border-spacing: 0;padding: 5px 45px;box-sizing: border-box;}tr.s777777 {background-color: transparent;color: #07b50c !important;}tr.cccccc {background-color: transparent;} tr.cccccc p {font-size: 16px;}input[type=\"checkbox\"] {width: 17.5px;aspect-ratio: 1;cursor: pointer;}td {cursor: default;user-select: none;}input {border-radius: 6px;cursor: text;}select {border-radius: 6px;cursor: pointer;}td.fr input {background-color: #07b50c !important; -webkit-appearance: none; accent-color: #07b50c !important;color: white;padding: 7px 17px;cursor: pointer;}* {line-height: 1.2;}@media screen and (max-width: 730px) {body {width: 100vw;margin: 0;padding: 10px;}table {width: 100%;padding: 0 10px;margin: 0;}}</style></head><body style=\"font-family:Verdana;\"><table class=\"s777777\"border=\"0\"width=\"100%\"cellpadding=\"1\"style=\"color:#ffffff;font-size:.8em;\"><tr><td><h1>&nbsp;Tally Light</h1></td></tr><tr><td><h2 style=\"color: white\">Żądanie bez zmiany ustawień nie jest możliwe</td></tr></table></body></html>");
    }
    else
    {
        String ssid;
        String pwd;
        bool change = false;
        for (uint8_t i = 0; i < server.args(); i++)
        {
            change = true;
            String var = server.argName(i);
            String val = server.arg(i);

            if (var == "tName")
            {
                val.toCharArray(settings.tallyName, (uint8_t)32);
            }
            else if (var == "tModeLED1")
            {
                settings.tallyModeLED1 = val.toInt();
            }
            else if (var == "tModeLED2")
            {
                settings.tallyModeLED2 = val.toInt();
            }
            else if (var == "ledBright")
            {
                settings.ledBrightness = val.toInt();
            }
            else if (var == "neoPxAmount")
            {
                settings.neopixelsAmount = val.toInt();
            }
            else if (var == "neoPxStatus")
            {
                settings.neopixelStatusLEDOption = val.toInt();
            }
            else if (var == "neoPxBright")
            {
                settings.neopixelBrightness = val.toInt();
            }
            else if (var == "tNo")
            {
                settings.tallyNo = val.toInt() - 1;
            }
            else if (var == "ssid")
            {
                ssid = String(val);
            }
            else if (var == "pwd")
            {
                pwd = String(val);
            }
            else if (var == "staticIP")
            {
                settings.staticIP = (val == "true");
            }
            else if (var == "tIP1")
            {
                settings.tallyIP[0] = val.toInt();
            }
            else if (var == "tIP2")
            {
                settings.tallyIP[1] = val.toInt();
            }
            else if (var == "tIP3")
            {
                settings.tallyIP[2] = val.toInt();
            }
            else if (var == "tIP4")
            {
                settings.tallyIP[3] = val.toInt();
            }
            else if (var == "mask1")
            {
                settings.tallySubnetMask[0] = val.toInt();
            }
            else if (var == "mask2")
            {
                settings.tallySubnetMask[1] = val.toInt();
            }
            else if (var == "mask3")
            {
                settings.tallySubnetMask[2] = val.toInt();
            }
            else if (var == "mask4")
            {
                settings.tallySubnetMask[3] = val.toInt();
            }
            else if (var == "gate1")
            {
                settings.tallyGateway[0] = val.toInt();
            }
            else if (var == "gate2")
            {
                settings.tallyGateway[1] = val.toInt();
            }
            else if (var == "gate3")
            {
                settings.tallyGateway[2] = val.toInt();
            }
            else if (var == "gate4")
            {
                settings.tallyGateway[3] = val.toInt();
            }
            else if (var == "aIP11")
            {
                settings.switcherIP1[0] = val.toInt();
            }
            else if (var == "aIP12")
            {
                settings.switcherIP1[1] = val.toInt();
            }
            else if (var == "aIP13")
            {
                settings.switcherIP1[2] = val.toInt();
            }
            else if (var == "aIP14")
            {
                settings.switcherIP1[3] = val.toInt();
            }
            else if (var == "aIP21")
            {
                settings.switcherIP2[0] = val.toInt();
            }
            else if (var == "aIP22")
            {
                settings.switcherIP2[1] = val.toInt();
            }
            else if (var == "aIP23")
            {
                settings.switcherIP2[2] = val.toInt();
            }
            else if (var == "aIP24")
            {
                settings.switcherIP2[3] = val.toInt();
            }
            else if (var == "switcher")
            {
                settings.whichSwicher = (val == "true");
            }
            else if (var == "updateURL")
            {
                val.toCharArray(settings.updateURL, (uint8_t)32);
            }
            else if (var == "updateURLPort")
            {
                settings.updateURLPort = val.toInt();
            }
            else if (var == "requestURLs")
            {
                val.toCharArray(settings.requestURLs, (uint8_t)112);
            }
        }

        if (change)
        {
            EEPROM.put(0, settings);
            EEPROM.commit();

            server.send(200, "text/html", (String) "<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><meta name=\"viewport\"content=\"width=device-width, initial-scale=1.0\"><title>Tally Light</title><style>#staticIP {accent-color: #07b50c;}.s777777 h1,.s777777 h2 {color: #07b50c;}.fr{float: right}body {display: flex;align-items: center;justify-content: center;width: 100vw;overflow-x: hidden;font-family: \"Arial\", sans-serif;background-color: #242424;color: #fff;table {width: 80%;max-width: 1200px;background-color: #3b3b3b;padding: 20px;margin: 20px;border-radius: 10px;box-shadow: 0 0 10px rgba(0, 0, 0, 0.5);border-radius: 12px;overflow: hidden;border-spacing: 0;padding: 5px 45px;box-sizing: border-box;}tr.s777777 {background-color: transparent;color: #07b50c !important;}tr.cccccc {background-color: transparent;} tr.cccccc p {font-size: 16px;}input[type=\"checkbox\"] {width: 17.5px;aspect-ratio: 1;cursor: pointer;}td {cursor: default;user-select: none;}input {border-radius: 6px;cursor: text;}select {border-radius: 6px;cursor: pointer;}td.fr input {background-color: #07b50c !important; -webkit-appearance: none; accent-color: #07b50c !important;color: white;padding: 7px 17px;cursor: pointer;}* {line-height: 1.2;}@media screen and (max-width: 730px) {body {width: 100vw;margin: 0;padding: 10px;}table {width: 100%;padding: 0 10px;margin: 0;}}</style></head><body><table class=\"s777777\"border=\"0\"width=\"100%\"cellpadding=\"1\"style=\"font-family:Verdana;color:#ffffff;font-size:.8em;\"><tr><td><h1>&nbsp;Tally Light</h1></td></tr><tr><td><h2 style=\"color: white\">Ustawienia zapisane pomyślnie!</td></tr></table></body></html>");

            // Delay to let data be saved, and the response to be sent properly to the client
            server.close(); // Close server to flush and ensure the response gets to the client
            delay(100);

            // Change into STA mode to disable softAP
            WiFi.mode(WIFI_STA);
            delay(100); // Give it time to switch over to STA mode (this is important on the ESP32 at least)

            if (ssid && pwd)
            {
                WiFi.persistent(true); // Needed by ESP8266
                // Pass in 'false' as 5th (connect) argument so we don't waste time trying to connect, just save the new SSID/PSK
                // 3rd argument is channel - '0' is default. 4th argument is BSSID - 'NULL' is default.
                WiFi.begin(ssid.c_str(), pwd.c_str(), 0, NULL, false);
            }

            // Delay to apply settings before restart
            delay(100);
            ESP.restart();
        }
    }
}

// Send 404 to client in case of invalid webpage being requested.
void handleNotFound()
{
    server.send(404, "text/html", "<!DOCTYPE html><html><head><meta charset=\"ASCII\"><meta name=\"viewport\"content=\"width=device-width, initial-scale=1.0\"><title>" + (String)DISPLAY_NAME + " setup</title></head><body style=\"font-family:Verdana;\"><table bgcolor=\"#777777\"border=\"0\"width=\"100%\"cellpadding=\"1\"style=\"color:#ffffff;font-size:.8em;\"><tr><td><h1>&nbsp Tally Light setup</h1></td></tr></table><br>404 - Page not found</body></html>");
}

String getSSID()
{
    return WiFi.SSID();
}