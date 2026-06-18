// ============================================================
//  Captura INMP441 -> servidor TCP por WiFi (PCM 16 bits)
//  + Pulsador para iniciar cada grabacion
//  + Pantalla OLED SSD1306 (libreria Adafruit) con estados:
//      Conectando / Conectado / Grabando / Grabado
//    y la IP de forma permanente una vez conectado.
//  Placa: ESP32-S3 WROOM N16R8
//
//  Librerias necesarias (Library Manager):
//    - Adafruit GFX Library
//    - Adafruit SSD1306
//
// ============================================================

#include <WiFi.h>
#include <driver/i2s_std.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

const char* WIFI_SSID     = "POCOX6Pro5G";
const char* WIFI_PASSWORD = "nana1708";
const uint16_t TCP_PORT   = 8000;

// --- Pines I2S del INMP441 ---
#define I2S_WS  4    // WS  (LRCL)
#define I2S_SCK 5    // SCK (BCLK)
#define I2S_SD  6    // SD  (DOUT del micro)

// --- Pulsador (a GND, usa pull-up interno) ---
#define BTN_PIN 21

// --- OLED SSD1306 (I2C) ---
#define OLED_SDA  42
#define OLED_SCL  41
#define OLED_W    128
#define OLED_H    64
#define OLED_ADDR 0x3C        // si no enciende, prueba 0x3D
Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, -1);

#define SOUND_SAMPLE_RATE 8000

const int REC_SECONDS    = 5;   
const int GAIN_SHIFT     = 0;  
const int WARMUP_BUFFERS = 0;   

const int BUF_SAMPLES = 600;
int32_t rawBuffer[BUF_SAMPLES];
int32_t s24[BUF_SAMPLES];
int16_t outBuffer[BUF_SAMPLES];

WiFiServer server(TCP_PORT);
WiFiClient client;                 
i2s_chan_handle_t rx_handle;
String ipStr = "---";

// ---------------- OLED ----------------
void oledConectando() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("SSID: "); display.println(WIFI_SSID);
  display.setTextSize(2);
  display.setCursor(0, 26);
  display.println("Conectando");
  display.setTextSize(1);
  display.setCursor(0, 54);
  display.println("WiFi...");
  display.display();
}

void oledEstado(const char* estado) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("SSID: "); display.println(WIFI_SSID);
  display.drawFastHLine(0, 10, OLED_W, SSD1306_WHITE);
  display.setCursor(0, 16);
  display.print("IP: ");  display.println(ipStr);
  display.setCursor(0, 28);
  display.print("Puerto: "); display.println(TCP_PORT);
  display.setTextSize(2);
  display.setCursor(0, 44);
  display.println(estado);
  display.display();
}

// ---------------- Filtro de mediana ----------------
int32_t med3(int32_t a, int32_t b, int32_t c) {
  int32_t t;
  if (a > b) { t = a; a = b; b = t; }
  if (b > c) { t = b; b = c; c = t; }
  if (a > b) { t = a; a = b; b = t; }
  return b;
}

// ---------------- I2S ----------------
void i2s_init() {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  i2s_new_channel(&chan_cfg, NULL, &rx_handle);

  i2s_std_config_t std_cfg = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SOUND_SAMPLE_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = (gpio_num_t)I2S_SCK,
      .ws   = (gpio_num_t)I2S_WS,
      .dout = I2S_GPIO_UNUSED,
      .din  = (gpio_num_t)I2S_SD,
      .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    },
  };
  std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;  

  i2s_channel_init_std_mode(rx_handle, &std_cfg);
  i2s_channel_enable(rx_handle);
}

// ---------------- Pulsador (con antirrebote) ----------------
bool botonPresionado() {
  static bool estadoPrev = HIGH;
  static unsigned long ultimoCambio = 0;
  bool lectura = digitalRead(BTN_PIN);
  if (lectura != estadoPrev && (millis() - ultimoCambio) > 50) {
    ultimoCambio = millis();
    estadoPrev = lectura;
    if (lectura == LOW) return true;   
  }
  return false;
}

// ---------------- Grabacion ----------------
void grabar() {
  if (!(client && client.connected())) {
    oledEstado("Sin PC");           
    delay(1200);
    oledEstado("Conectado");
    return;
  }

  oledEstado("Grabando");
  Serial.println("Grabando...");

  long objetivo  = (long)SOUND_SAMPLE_RATE * REC_SECONDS;  
  long enviadas  = 0;
  int  warm      = WARMUP_BUFFERS;
  int32_t prevSample = 0;

  while (enviadas < objetivo && client.connected()) {
    size_t bytesRead = 0;
    i2s_channel_read(rx_handle, rawBuffer, sizeof(rawBuffer), &bytesRead, portMAX_DELAY);
    int n = bytesRead / 4;

    if (warm > 0) { warm--; continue; }   

    for (int i = 0; i < n; i++) s24[i] = rawBuffer[i] >> 8;

    for (int i = 0; i < n; i++) {
      int32_t a = (i == 0)     ? prevSample : s24[i - 1];
      int32_t b = s24[i];
      int32_t c = (i == n - 1) ? s24[i]     : s24[i + 1];
      int32_t v = med3(a, b, c) >> GAIN_SHIFT;
      if (v > 32767)  v = 32767;
      else if (v < -32768) v = -32768;
      outBuffer[i] = (int16_t)v;
    }
    if (n > 0) prevSample = s24[n - 1];

    
    long restan   = objetivo - enviadas;
    int  aEnviar  = (n > restan) ? (int)restan : n;
    client.write((uint8_t*)outBuffer, aEnviar * 2);
    enviadas += aEnviar;
  }

  oledEstado("Grabado");
  Serial.println("Grabado.");
  delay(1500);
  oledEstado("Conectado");
}

// ---------------- Setup ----------------
void setup() {
  Serial.begin(115200);
  delay(3000);
  Serial.println("=== Arranque OK, el Serial funciona ===");

  pinMode(BTN_PIN, INPUT_PULLUP);

  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("No se encontro la OLED (revisa direccion/cableado).");
  }
  oledConectando();

  Serial.println("Inicializando I2S...");
  i2s_init();
  Serial.println("I2S listo.");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Conectando a WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();

  ipStr = WiFi.localIP().toString();
  Serial.print("Conectado. IP del ESP32: "); Serial.println(ipStr);
  Serial.print("Puerto: "); Serial.println(TCP_PORT);

  server.begin();
  oledEstado("Conectado");  
}

// ---------------- Loop ----------------
void loop() {

  if (!client || !client.connected()) {
    WiFiClient nuevo = server.available();
    if (nuevo) {
      client = nuevo;
      Serial.println("Cliente PC conectado.");
      oledEstado("Conectado");
    }
  }

  // el pulsador inicia una grabacion de REC_SECONDS
  if (botonPresionado()) {
    grabar();
  }
}
