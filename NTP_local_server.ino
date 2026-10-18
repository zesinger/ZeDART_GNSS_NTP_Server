#include <WiFi.h>
#include <WiFiUdp.h>
#include <TinyGPSPlus.h>
#include <ESPmDNS.h>

HardwareSerial GNSS(1);
TinyGPSPlus gps;
WiFiUDP ntpUdp;

// -----------------------------------------------------------------------------
// Hardware configuration
// -----------------------------------------------------------------------------

constexpr int GNSS_RX = 18;
constexpr int GNSS_TX = 17;
constexpr int PPS_PIN = 4;
constexpr int WIFI_MODE_PIN = 5;

constexpr int TIME_LED_R = 6;
constexpr int TIME_LED_G = 7;
constexpr int TIME_LED_B = 15;

constexpr int NETWORK_LED_R = 9;
constexpr int NETWORK_LED_G = 10;
constexpr int NETWORK_LED_B = 11;

constexpr bool RGB_COMMON_ANODE = true;

// -----------------------------------------------------------------------------
// Wi-Fi configuration
// -----------------------------------------------------------------------------

constexpr char AP_SSID[] = "ZeDART-Time";
constexpr char AP_PASSWORD[] = "ZeDART2026";

// Used only when GPIO5 is connected to GND at startup.
constexpr char ROUTER_SSID[] = "MySSID";
constexpr char ROUTER_PASSWORD[] = "MyPassword";

constexpr char HOSTNAME[] = "zedart-time";
constexpr uint32_t ROUTER_CONNECT_TIMEOUT_MS = 20000;

// -----------------------------------------------------------------------------
// NTP configuration
// -----------------------------------------------------------------------------

constexpr uint16_t NTP_PORT = 123;
constexpr uint32_t NTP_UNIX_OFFSET = 2208988800UL;
constexpr size_t NTP_PACKET_SIZE = 48;

byte ntpPacket[NTP_PACKET_SIZE];

// -----------------------------------------------------------------------------
// PPS and clock configuration
// -----------------------------------------------------------------------------

constexpr uint32_t PPS_TIMEOUT_MS = 2000;
constexpr uint32_t GNSS_TIMEOUT_MS = 5000;
constexpr uint8_t PPS_LOCK_COUNT = 3;
constexpr uint32_t PPS_MIN_PERIOD_US = 990000;
constexpr uint32_t PPS_MAX_PERIOD_US = 1010000;
constexpr double PPS_FILTER_OLD_WEIGHT = 0.9;
constexpr double PPS_FILTER_NEW_WEIGHT = 0.1;
constexpr uint32_t STATUS_INTERVAL_MS = 5000;

volatile uint32_t capturedPpsMicros = 0;
volatile bool newPps = false;

uint32_t lastPpsMicros = 0;
uint32_t referencePpsMicros = 0;
uint32_t referenceUnixTime = 0;
uint32_t lastPpsSeenMillis = 0;
uint32_t lastGnssTimeMillis = 0;

double ppsPeriod = 1000000.0;

uint8_t validPpsCount = 0;

bool havePreviousPps = false;
bool clockValid = false;
bool routerMode = false;
bool ppsHasLockedOnce = false;
bool ntpActivityActive = false;

uint32_t lastGnssDataMillis = 0;
uint32_t lastNtpResponseMillis = 0;

constexpr uint32_t GNSS_DATA_TIMEOUT_MS = 2000;
constexpr uint32_t NTP_ACTIVITY_FLASH_MS = 200;

// Forward declarations used by the LED status logic.
bool isPpsLocked();
bool isGnssTimeFresh();
bool isNtpReady();

// -----------------------------------------------------------------------------
// Status LEDs
// -----------------------------------------------------------------------------

enum class LedColor
{
    Off,
    Red,
    Orange,
    Green,
    Blue,
    Purple,
    White
};

void writeLedLevel(
    int pin,
    uint8_t brightness)
{
    const uint8_t output =
        RGB_COMMON_ANODE
            ? 255 - brightness
            : brightness;

    analogWrite(pin, output);
}

void setRgb(
    int redPin,
    int greenPin,
    int bluePin,
    LedColor color)
{
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;

    switch (color)
    {
        case LedColor::Red:
            red = 255;
            break;

        case LedColor::Orange:
            red = 255;
            green = 25;
            break;

        case LedColor::Green:
            green = 255;
            break;

        case LedColor::Blue:
            blue = 255;
            break;

        case LedColor::Purple:
            red = 255;
            blue = 255;
            break;

        case LedColor::White:
            red = 255;
            green = 255;
            blue = 255;
            break;

        case LedColor::Off:
        default:
            break;
    }

    writeLedLevel(redPin, red);
    writeLedLevel(greenPin, green);
    writeLedLevel(bluePin, blue);
}

void initializeStatusLeds()
{
    pinMode(TIME_LED_R, OUTPUT);
    pinMode(TIME_LED_G, OUTPUT);
    pinMode(TIME_LED_B, OUTPUT);

    pinMode(NETWORK_LED_R, OUTPUT);
    pinMode(NETWORK_LED_G, OUTPUT);
    pinMode(NETWORK_LED_B, OUTPUT);

    setRgb(
        TIME_LED_R,
        TIME_LED_G,
        TIME_LED_B,
        LedColor::Red);

    setRgb(
        NETWORK_LED_R,
        NETWORK_LED_G,
        NETWORK_LED_B,
        LedColor::Blue);
}

bool isGnssDataPresent()
{
    return lastGnssDataMillis != 0 &&
           millis() - lastGnssDataMillis < GNSS_DATA_TIMEOUT_MS;
}

void updateStatusLeds()
{
    if (isNtpReady())
    {
        ppsHasLockedOnce = true;

        setRgb(
            TIME_LED_R,
            TIME_LED_G,
            TIME_LED_B,
            LedColor::Green);
    }
    else if (ppsHasLockedOnce &&
             clockValid &&
             isGnssTimeFresh() &&
             !isPpsLocked())
    {
        // The clock was previously synchronized, but the PPS reference was lost.
        setRgb(
            TIME_LED_R,
            TIME_LED_G,
            TIME_LED_B,
            LedColor::Purple);
    }
    else if (isGnssTimeFresh())
    {
        // GNSS time is valid, but PPS is not locked yet.
        setRgb(
            TIME_LED_R,
            TIME_LED_G,
            TIME_LED_B,
            LedColor::Orange);
    }
    else if (isGnssDataPresent() &&
             gps.satellites.isValid() &&
             gps.satellites.value() > 0)
    {
        // GNSS data is received and satellites are detected, but time is not valid yet.
        setRgb(
            TIME_LED_R,
            TIME_LED_G,
            TIME_LED_B,
            LedColor::Blue);
    }
    else
    {
        // No usable satellite reception.
        setRgb(
            TIME_LED_R,
            TIME_LED_G,
            TIME_LED_B,
            LedColor::Red);
    }

    if (ntpActivityActive &&
        millis() - lastNtpResponseMillis < NTP_ACTIVITY_FLASH_MS)
    {
        setRgb(
            NETWORK_LED_R,
            NETWORK_LED_G,
            NETWORK_LED_B,
            LedColor::White);

        return;
    }

    ntpActivityActive = false;

    bool networkReady = false;
    bool networkWaiting = false;

    if (routerMode)
    {
        networkReady = WiFi.status() == WL_CONNECTED;
        networkWaiting = !networkReady;
    }
    else
    {
        networkReady = WiFi.softAPgetStationNum() > 0;
        networkWaiting = WiFi.getMode() == WIFI_AP;
    }

    setRgb(
        NETWORK_LED_R,
        NETWORK_LED_G,
        NETWORK_LED_B,
        networkReady
            ? LedColor::Green
            : (networkWaiting ? LedColor::Blue : LedColor::Red));
}

// -----------------------------------------------------------------------------
// PPS interrupt
// -----------------------------------------------------------------------------

void IRAM_ATTR onPps()
{
    capturedPpsMicros = micros();
    newPps = true;
}

// -----------------------------------------------------------------------------
// Time conversion helpers
// -----------------------------------------------------------------------------

bool isLeapYear(int year)
{
    return (year % 4 == 0 && year % 100 != 0)
        || (year % 400 == 0);
}

uint32_t toUnixTime(
    int year,
    int month,
    int day,
    int hour,
    int minute,
    int second)
{
    static const int daysBeforeMonth[] =
    {
        0, 31, 59, 90, 120, 151,
        181, 212, 243, 273, 304, 334
    };

    uint32_t days = 0;

    for (int y = 1970; y < year; y++)
        days += isLeapYear(y) ? 366 : 365;

    days += daysBeforeMonth[month - 1];

    if (month > 2 && isLeapYear(year))
        days++;

    days += day - 1;

    return days * 86400UL
         + hour * 3600UL
         + minute * 60UL
         + second;
}

// -----------------------------------------------------------------------------
// PPS processing and clock state
// -----------------------------------------------------------------------------

void processPps()
{
    uint32_t currentPps;

    noInterrupts();

    if (!newPps)
    {
        interrupts();
        return;
    }

    currentPps = capturedPpsMicros;
    newPps = false;

    interrupts();

    lastPpsSeenMillis = millis();

    if (havePreviousPps)
    {
        const uint32_t measuredPeriod = currentPps - lastPpsMicros;

        if (measuredPeriod > PPS_MIN_PERIOD_US &&
            measuredPeriod < PPS_MAX_PERIOD_US)
        {
            ppsPeriod =
                ppsPeriod * PPS_FILTER_OLD_WEIGHT +
                measuredPeriod * PPS_FILTER_NEW_WEIGHT;

            if (validPpsCount < PPS_LOCK_COUNT)
                validPpsCount++;
        }
        else
        {
            validPpsCount = 0;
        }
    }

    lastPpsMicros = currentPps;
    havePreviousPps = true;

    if (clockValid)
    {
        referenceUnixTime++;
        referencePpsMicros = currentPps;
    }
}

void updateClockFromGnss()
{
    if (!gps.date.isValid() ||
        !gps.time.isValid() ||
        !havePreviousPps)
    {
        return;
    }

    referenceUnixTime = toUnixTime(
        gps.date.year(),
        gps.date.month(),
        gps.date.day(),
        gps.time.hour(),
        gps.time.minute(),
        gps.time.second());

    referencePpsMicros = lastPpsMicros;
    lastGnssTimeMillis = millis();
    clockValid = true;
}

bool isPpsLocked()
{
    if (!havePreviousPps)
        return false;

    if (validPpsCount < PPS_LOCK_COUNT)
        return false;

    return millis() - lastPpsSeenMillis < PPS_TIMEOUT_MS;
}

bool isGnssTimeFresh()
{
    if (!gps.date.isValid() ||
        !gps.time.isValid())
    {
        return false;
    }

    return millis() - lastGnssTimeMillis < GNSS_TIMEOUT_MS;
}

bool isNtpReady()
{
    return clockValid &&
           isPpsLocked() &&
           isGnssTimeFresh();
}

void getCurrentUtc(
    uint32_t &unixSeconds,
    uint32_t &fraction)
{
    const uint32_t nowMicros = micros();
    const uint32_t elapsedTicks = nowMicros - referencePpsMicros;
    const double elapsedSeconds = (double)elapsedTicks / ppsPeriod;
    const uint32_t wholeSeconds = (uint32_t)elapsedSeconds;
    const double fractionalSecond = elapsedSeconds - wholeSeconds;

    unixSeconds = referenceUnixTime + wholeSeconds;
    fraction = (uint32_t)(fractionalSecond * 4294967296.0);
}

// -----------------------------------------------------------------------------
// NTP server
// -----------------------------------------------------------------------------

void writeNtpTimestamp(
    byte *buffer,
    int offset,
    uint32_t unixSeconds,
    uint32_t fraction)
{
    const uint32_t ntpSeconds = unixSeconds + NTP_UNIX_OFFSET;

    buffer[offset + 0] = (ntpSeconds >> 24) & 0xFF;
    buffer[offset + 1] = (ntpSeconds >> 16) & 0xFF;
    buffer[offset + 2] = (ntpSeconds >> 8) & 0xFF;
    buffer[offset + 3] = ntpSeconds & 0xFF;

    buffer[offset + 4] = (fraction >> 24) & 0xFF;
    buffer[offset + 5] = (fraction >> 16) & 0xFF;
    buffer[offset + 6] = (fraction >> 8) & 0xFF;
    buffer[offset + 7] = fraction & 0xFF;
}

void processNtp()
{
    const int packetSize = ntpUdp.parsePacket();

    if (packetSize < NTP_PACKET_SIZE)
        return;

    const IPAddress remoteIp = ntpUdp.remoteIP();
    const uint16_t remotePort = ntpUdp.remotePort();

    byte request[NTP_PACKET_SIZE];
    ntpUdp.read(request, NTP_PACKET_SIZE);

    if (!isNtpReady())
    {
        Serial.print("NTP request ignored from ");
        Serial.print(remoteIp);
        Serial.println(" : time source not ready");
        return;
    }

    // Capture the receive timestamp as close as possible to packet reception.
    uint32_t receiveSeconds;
    uint32_t receiveFraction;

    getCurrentUtc(
        receiveSeconds,
        receiveFraction);

    memset(ntpPacket, 0, sizeof(ntpPacket));

    // LI = 0, VN = 4, Mode = 4 (server).
    ntpPacket[0] = 0b00100100;

    // Stratum 1: primary GNSS reference.
    ntpPacket[1] = 1;

    // Poll interval.
    ntpPacket[2] = 6;

    // Precision = 2^-20 s, approximately 0.95 us.
    ntpPacket[3] = (byte)-20;

    // Root delay = 0.
    ntpPacket[4] = 0;
    ntpPacket[5] = 0;
    ntpPacket[6] = 0;
    ntpPacket[7] = 0;

    // Root dispersion: approximately 1 ms.
    ntpPacket[8] = 0;
    ntpPacket[9] = 0;
    ntpPacket[10] = 0;
    ntpPacket[11] = 66;

    // Reference ID = "GPS".
    ntpPacket[12] = 'G';
    ntpPacket[13] = 'P';
    ntpPacket[14] = 'S';
    ntpPacket[15] = 0;

    // Reference timestamp: last PPS associated with referenceUnixTime.
    writeNtpTimestamp(
        ntpPacket,
        16,
        referenceUnixTime,
        0);

    // Originate timestamp: copy the client's transmit timestamp.
    memcpy(
        &ntpPacket[24],
        &request[40],
        8);

    // Receive timestamp.
    writeNtpTimestamp(
        ntpPacket,
        32,
        receiveSeconds,
        receiveFraction);

    // Transmit timestamp calculated immediately before sending the response.
    uint32_t transmitSeconds;
    uint32_t transmitFraction;

    getCurrentUtc(
        transmitSeconds,
        transmitFraction);

    writeNtpTimestamp(
        ntpPacket,
        40,
        transmitSeconds,
        transmitFraction);

    ntpUdp.beginPacket(remoteIp, remotePort);
    ntpUdp.write(ntpPacket, NTP_PACKET_SIZE);

    const int sent = ntpUdp.endPacket();

    if (sent)
    {
        lastNtpResponseMillis = millis();
        ntpActivityActive = true;

        Serial.print("NTP -> ");
        Serial.println(remoteIp);
    }
}

// -----------------------------------------------------------------------------
// Network configuration
// -----------------------------------------------------------------------------

void startAccessPoint()
{
    WiFi.mode(WIFI_AP);
    WiFi.setSleep(false);

    WiFi.softAP(
        AP_SSID,
        AP_PASSWORD);

    Serial.println();
    Serial.println("NETWORK MODE : ACCESS POINT");
    Serial.print("SSID         : ");
    Serial.println(AP_SSID);
    Serial.print("IP           : ");
    Serial.println(WiFi.softAPIP());
}

void startRouterMode()
{
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setHostname(HOSTNAME);

    Serial.println();
    Serial.println("NETWORK MODE : ROUTER");
    Serial.print("SSID         : ");
    Serial.println(ROUTER_SSID);

    WiFi.begin(
        ROUTER_SSID,
        ROUTER_PASSWORD);

    Serial.print("Connecting");

    const unsigned long start = millis();

    while (WiFi.status() != WL_CONNECTED &&
           millis() - start < ROUTER_CONNECT_TIMEOUT_MS)
    {
        delay(500);
        Serial.print(".");
    }

    Serial.println();

    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("Wi-Fi       : CONNECTION FAILED");
        return;
    }

    Serial.println("Wi-Fi       : CONNECTED");
    Serial.print("IP           : ");
    Serial.println(WiFi.localIP());

    if (!MDNS.begin(HOSTNAME))
    {
        Serial.println("mDNS         : FAILED");
        return;
    }

    MDNS.addService(
        "ntp",
        "udp",
        NTP_PORT);

    Serial.print("mDNS         : ");
    Serial.print(HOSTNAME);
    Serial.println(".local");
    Serial.println("NTP SERVICE  : _ntp._udp");
}

// -----------------------------------------------------------------------------
// Diagnostics
// -----------------------------------------------------------------------------

void printStatus()
{
    Serial.println();
    Serial.println("------------------------------");

    Serial.print("GNSS FIX    : ");
    Serial.println(
        gps.location.isValid()
        ? "YES"
        : "NO");

    Serial.print("SATELLITES  : ");

    if (gps.satellites.isValid())
        Serial.println(gps.satellites.value());
    else
        Serial.println("?");

    Serial.print("GNSS TIME   : ");
    Serial.println(
        isGnssTimeFresh()
        ? "VALID"
        : "INVALID");

    Serial.print("PPS LOCK    : ");
    Serial.println(
        isPpsLocked()
        ? "YES"
        : "NO");

    Serial.print("PPS PERIOD  : ");
    Serial.print(ppsPeriod, 2);
    Serial.println(" us");

    Serial.print("CLOCK VALID : ");
    Serial.println(
        clockValid
        ? "YES"
        : "NO");

    Serial.print("NTP READY   : ");
    Serial.println(
        isNtpReady()
        ? "YES"
        : "NO");

    Serial.print("NETWORK     : ");

    if (routerMode)
    {
        Serial.println(
            WiFi.status() == WL_CONNECTED
            ? "ROUTER CONNECTED"
            : "ROUTER DISCONNECTED");
    }
    else
    {
        Serial.print("AP - ");
        Serial.print(WiFi.softAPgetStationNum());
        Serial.println(" CLIENT(S)");
    }

    if (isNtpReady())
    {
        uint32_t seconds;
        uint32_t fraction;

        getCurrentUtc(seconds, fraction);

        Serial.print("UNIX UTC    : ");
        Serial.println(seconds);
    }
}

// -----------------------------------------------------------------------------
// Arduino entry points
// -----------------------------------------------------------------------------

void setup()
{
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("=== ZeDART GNSS NTP Server ===");

    GNSS.begin(
        9600,
        SERIAL_8N1,
        GNSS_RX,
        GNSS_TX);

    pinMode(PPS_PIN, INPUT);

    attachInterrupt(
        digitalPinToInterrupt(PPS_PIN),
        onPps,
        RISING);

    pinMode(
        WIFI_MODE_PIN,
        INPUT_PULLUP);

    initializeStatusLeds();

    routerMode =
        digitalRead(WIFI_MODE_PIN) == LOW;

    if (routerMode)
        startRouterMode();
    else
        startAccessPoint();

    ntpUdp.begin(NTP_PORT);

    Serial.print("NTP SERVER   : UDP/");
    Serial.println(NTP_PORT);
}

void loop()
{
    processPps();

    while (GNSS.available())
    {
        const char c = GNSS.read();
        lastGnssDataMillis = millis();

        if (gps.encode(c) &&
            gps.time.isUpdated())
        {
            updateClockFromGnss();
        }
    }

    processNtp();
    updateStatusLeds();

    static uint32_t lastStatus = 0;

    if (millis() - lastStatus >= STATUS_INTERVAL_MS)
    {
        lastStatus = millis();
        printStatus();
    }
}
