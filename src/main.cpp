#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <WiFiNINA.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_AHTX0.h>
#include <SDS011.h>
#include <influxdb.h>
#include <NTPClient.h>

// OLED display dimensions in pixels
constexpr int SCREEN_WIDTH = 128;
constexpr int SCREEN_HEIGHT = 64;

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
const auto display_fmt = "AQI: %d\nTemp: %d\nHum: %d%%\n\n";
int aqi_value = 0;
int8_t sds_error = 0;
float pm25_value = 0.0;
float pm10_value = 0.0;
int temperature = 0;
int humidity = 0;
char buf[128] = "";
void update_display();

// Atmo sensors
Adafruit_AHTX0 aht;
SDS011 sds011;


constexpr pin_size_t RELAY_PIN = 14;

struct Breakpoint {
    float BreakpointLow, BreakpointHigh;
    float IndexLow, IndexHigh;

    float IndexDelta() const {
        return IndexHigh - IndexLow;
    }
    float BreakpointDelta() const {
        return BreakpointHigh - BreakpointLow;
    }
};

Breakpoint breakpoints_pm25[] = {
    {0.0, 9.0, 0, 50},
    {9.1, 35.4, 51, 100},
    {35.5, 55.4, 101, 150},
    {55.5, 125.4, 151, 200},
    {125.5, 225.4, 201, 300},
    {225.5, 350, 301, 400}, // The upper bounds here are made up by me. The gov't table just says 225.5+ -> AQI 301+
};

Breakpoint breakpoints_pm10[] = {
    {0.0, 54.0, 0, 50},
    {55.0, 154.0, 51, 100},
    {155.0, 254.0, 101, 150},
    {255.0, 354.0, 151, 200},
    {355.0, 424.0, 201, 300},
    {425.0, 524.0, 301, 400}, // The upper bounds here are made up by me. The gov't table just says 225.5+ -> AQI 301+
};

int calculate_from_dust(float pm25, float pm10);
int calculate_from_pm25(float raw);
int calculate_from_pm10(float raw);
Breakpoint* find_breakpoint(Breakpoint *table, int table_size, float raw);
float calculate_from_breakpoints(
    float raw_value,
    const Breakpoint *bp
);

// Networking
constexpr char ssid[] = "WANNET";
constexpr char pass[] = "eatmithkabobs";
int wifi_status = 0;
void wifi_connect();
WiFiClient wifi;
constexpr char influx_token[] = "vXKlaAFOvUjaIUJkLaHNaQ09brXPGAe2Yz8KkolOlV_t0lrUt98RgInuuHOCycBWycnzPzVFRmzbNl0LIVm6tw==";
constexpr char influx_host[] = "klaital.com";
constexpr int influx_port = 8086;
constexpr char influx_bucket[] = "iot-telemetry";
constexpr char influx_org[] = "wannet";
Influx::InfluxDbClient influx_db_client(wifi, influx_host, influx_port, influx_org, influx_bucket, influx_token);
Influx::Point atmo_metrics;

// Time sync
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60000);

void setup() {
    Serial.begin(115200);
    // while(!Serial);

    Serial.println("Initializing I/O Pins...");
    pinMode(RELAY_PIN, OUTPUT);

    // SSD1306_SWITCHCAPVCC = generate display voltage from 3.3V internally
    if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println(F("SSD1306 allocation failed"));
        for(;;); // Don't proceed, loop forever
    }

    // Show initial display buffer contents on the screen --
    // the library initializes this with an Adafruit splash screen.
    display.display();
    delay(2000); // Pause for 2 seconds

    // initialize sensors
    pinMode(LEDR, OUTPUT);
    if (! aht.begin()) {
        Serial.println("Could not find AHT? Check wiring");
        digitalWrite(LEDR, HIGH);
        while (1) delay(10);
    }
    Serial.println("AHT10 or AHT20 found");

    sds011.setup(&Serial1);
    sds011.onData([](const float pm25Value, const float pm10Value) {
        if (pm25Value > 0 || pm10Value > 0) {
            pm10_value = pm10Value;
            pm25_value = pm25Value;
            aqi_value = calculate_from_dust(pm25Value, pm10Value);
        }
    });
    sds011.onError([](const int8_t error) {
        sds_error = error;
    });
    sds011.setWorkingPeriod(5);

    // Configure metrics clients
    atmo_metrics.set_tag("sensor", "ap1");
    atmo_metrics.set_measurement("atmo");
}

int relay_state = LOW;

void loop() {
    // Reconnect wifi as needed
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Connection lost");
        wifi_connect();
        timeClient.begin();
        timeClient.update();
    }
    // Read sensors
    sds011.loop();
    Serial.print("(");
    Serial.print(sds_error);
    Serial.print(") PM2.5: ");
    Serial.print(pm25_value);
    Serial.print(" PM1.0: ");
    Serial.print(pm10_value);

    Serial.print(" -> AQI=");
    Serial.println(aqi_value);
    atmo_metrics.set_field("pm25", pm25_value);
    atmo_metrics.set_field("pm10", pm10_value);
    atmo_metrics.set_field("aqi", aqi_value);

    sensors_event_t hum, temp;
    aht.getEvent(&hum, &temp);
    humidity = hum.relative_humidity;
    temperature = static_cast<int>((temp.temperature * 9.0 / 5.0) + 32.0);
    atmo_metrics.set_field("hum", humidity);
    atmo_metrics.set_field("temp", temperature);
    atmo_metrics.timestamp = timeClient.getEpochTime();
    const int resp = influx_db_client.send(atmo_metrics);
    Serial.print("Influx response: ");
    Serial.println(resp);
    update_display();

    delay(5000);
}

void update_display() {
  display.clearDisplay();
  // TODO: move all of this into setup()
  display.setTextSize(2);      // Normal 1:1 pixel scale
  display.setTextColor(WHITE); // Draw white text
  display.setCursor(0, 0);     // Start at top-left corner
  // display.cp437(true);         // Use full 256 char 'Code Page 437' font

  buf[0] = '\0';
  sprintf(buf, display_fmt, aqi_value, temperature, humidity);
  display.println(buf);

  display.display();
}

int calculate_from_dust(const float pm25, const float pm10) {
    const int aqi25 = calculate_from_pm25(pm25);
    const int aqi10 = calculate_from_pm10(pm10);
    if (aqi25 > aqi10) {
        return aqi25;
    }
    return aqi10;
}

int calculate_from_pm25(const float raw) {
    const Breakpoint *b = find_breakpoint(breakpoints_pm25, 6, raw);
    const float a = calculate_from_breakpoints(raw, b);
    return static_cast<int>(a);
}

int calculate_from_pm10(const float raw) {
    const Breakpoint *b = find_breakpoint(breakpoints_pm10, 6, raw);
    const float a = calculate_from_breakpoints(raw, b);
    return static_cast<int>(a);
}

Breakpoint *find_breakpoint(Breakpoint *table, const int table_size, const float raw) {
    for (int i=0; i < table_size; i++) {
        if (raw < table[i].BreakpointHigh) {
            return &table[i];
        }
    }
    // Not found
    return nullptr;
}

float calculate_from_breakpoints(const float raw_value, const Breakpoint *bp) {
    return (bp->IndexDelta() / bp->BreakpointDelta() * (raw_value - bp->BreakpointLow)) + bp->IndexLow;
}

void wifi_connect() {
    wifi_status = WL_IDLE_STATUS;
    Serial.print("Connecting to ");
    Serial.print(ssid);
    while(wifi_status!= WL_CONNECTED) {
        wifi_status = WiFi.begin(ssid, pass);
        if (wifi_status == WL_CONNECTED) {
            Serial.println("\n\rWifi connected");
        } else {
            delay(500);
            Serial.print(".");
        }
    }
}
