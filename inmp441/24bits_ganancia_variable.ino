// ============================================================
//  Captura INMP441 -> servidor TCP por WiFi (PCM 24 bits + AGC)
//  + Pulsador para iniciar cada grabacion
//  + Pantalla OLED SSD1306 (libreria Adafruit) con estados:
//      Conectando / Conectado / Grabando / Grabado
//    y la IP de forma permanente una vez conectado.
//  + Filtro de mediana (anti-clicks) + AGC dinamico por buffer
//      - Se mantiene la MISMA logica de ganancia adaptativa
//        (attack rapido / release lento, calculo por log2 del
//        pico de cada buffer) que en la version original.
//      - DIFERENCIA CLAVE: el resultado YA NO se trunca a 16
//        bits. Se conserva en el espacio de 24 bits real
//        (rango -8,388,608 .. 8,388,607) y se envia como
//        int32_t (4 bytes LE/muestra, mismo formato que la
//        version "cruda sin AGC" que ya usa la app movil).
//      - Por esto el AGC en la practica solo actua como
//        limitador de transitorios muy fuertes; para sonidos a
//        distancia normal el shift se mantendra casi siempre
//        en 0, porque la fuente (s24) y el destino ya
//        comparten la misma resolucion de 24 bits.
//  Placa: ESP32-S3 WROOM N16R8
//
//  Librerias necesarias (Library Manager):
//    - Adafruit GFX Library
//    - Adafruit SSD1306
// ============================================================

#include <WiFi.h>
#include <driver/i2s_std.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

const char* WIFI_SSID     = "POCOX6Pro5G";
const char* WIFI_PASSWORD = "nana1708";
const uint16_t TCP_PORT   = 8000;

#define I2S_WS  4
#define I2S_SCK 5
#define I2S_SD  6

#define BTN_PIN 21

#define OLED_SDA  42
#define OLED_SCL  41
#define OLED_W    128
#define OLED_H    64
#define OLED_ADDR 0x3C
Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, -1);

#define SOUND_SAMPLE_RATE 8000

const int REC_SECONDS    = 5;
const int WARMUP_BUFFERS = 0;

const int BUF_SAMPLES = 4096;
int32_t rawBuffer[BUF_SAMPLES];
int32_t s24[BUF_SAMPLES];     // valor de 24 bits, sign-extended dentro de int32_t
int32_t outBuf24[BUF_SAMPLES]; // salida post-AGC, todavia en rango de 24 bits

// --- Limites de salida: 24 bits reales, no 16 ---
const int32_t OUT24_MAX =  8388607;   //  2^23 - 1
const int32_t OUT24_MIN = -8388608;   // -2^23

// --- Parametros de AGC (misma logica que la version anterior) ---
const int   GAIN_SHIFT_MIN = 0;     // sin amplificacion, solo atenuacion
const int   GAIN_SHIFT_MAX = 8;     // maxima atenuacion permitida
const float AGC_ATTACK     = 0.3f;  // baja ganancia rapido si hay riesgo de saturar
const float AGC_RELEASE    = 0.05f; // sube ganancia despacio al volver el silencio

// Target reescalado para 24 bits (antes era 26214 ~ 0.8 * 32768 para 16 bits)
const float AGC_TARGET     = 6710886.0f;  // ~0.8 * 8388608

float currentShift = 0.0f;

WiFiServer server(TCP_PORT);
WiFiClient client;
i2s_chan_handle_t rx_handle;
String ipStr = "---";

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

int32_t med3(int32_t a, int32_t b, int32_t c) {
  int32_t t;
  if (a > b) { t = a; a = b; b = t; }
  if (b > c) { t = b; b = c; c = t; }
  if (a > b) { t = a; a = b; b = t; }
  return b;
}

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

int agcUpdate(int32_t* buf, int n) {
  int32_t pico = 0;
  for (int i = 0; i < n; i++) {
    int32_t v = buf[i];
    if (v < 0) v = -v;
    if (v > pico) pico = v;
  }

  float shiftIdeal = currentShift;
  if (pico > 0) {
    shiftIdeal = log2f((float)pico / AGC_TARGET);

    if (shiftIdeal < (float)GAIN_SHIFT_MIN) shiftIdeal = (float)GAIN_SHIFT_MIN;
    if (shiftIdeal > (float)GAIN_SHIFT_MAX) shiftIdeal = (float)GAIN_SHIFT_MAX;
  }

  float alpha = (shiftIdeal > currentShift) ? AGC_ATTACK : AGC_RELEASE;
  currentShift = currentShift + alpha * (shiftIdeal - currentShift);

  int shift = (int)(currentShift + 0.5f);
  if (shift < GAIN_SHIFT_MIN) shift = GAIN_SHIFT_MIN;
  if (shift > GAIN_SHIFT_MAX) shift = GAIN_SHIFT_MAX;

  return shift;
}

void grabar() {
  if (!(client && client.connected())) {
    oledEstado("Sin PC");
    delay(1200);
    oledEstado("Conectado");
    return;
  }

  oledEstado("Grabando");
  Serial.println("Grabando...");

  currentShift = 0.0f;

  long objetivo = (long)SOUND_SAMPLE_RATE * REC_SECONDS;
  long enviadas = 0;
  int  warm     = WARMUP_BUFFERS;
  int32_t prevSample = 0;

  while (enviadas < objetivo && client.connected()) {
    size_t bytesRead = 0;
    i2s_channel_read(rx_handle, rawBuffer, sizeof(rawBuffer), &bytesRead, portMAX_DELAY);
    int n = bytesRead / 4;

    if (warm > 0) { warm--; continue; }

    for (int i = 0; i < n; i++) s24[i] = rawBuffer[i] >> 8;

    int shift = agcUpdate(s24, n);

    for (int i = 0; i < n; i++) {
      int32_t a = (i == 0)     ? prevSample : s24[i - 1];
      int32_t b = s24[i];
      int32_t c = (i == n - 1) ? s24[i]     : s24[i + 1];
      int32_t v = med3(a, b, c) >> shift;
      if (v > OUT24_MAX) v = OUT24_MAX;
      if (v < OUT24_MIN) v = OUT24_MIN;
      outBuf24[i] = v;
    }
    if (n > 0) prevSample = s24[n - 1];

    long restan  = objetivo - enviadas;
    int  aEnviar = (n > restan) ? (int)restan : n;
    client.write((uint8_t*)outBuf24, aEnviar * 4);  // 4 bytes/muestra (int32 LE)
    enviadas += aEnviar;

    Serial.printf("  AGC shift=%d  enviadas=%ld/%ld\n", shift, enviadas, objetivo);
  }

  oledEstado("Grabado");
  Serial.println("Grabado.");
  delay(1500);
  oledEstado("Conectado");
}

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

// Comando remoto enviado por la app para iniciar grabacion sin tocar
// el dispositivo (evita el ruido del clic del pulsador en el audio).
// Protocolo simple: un solo byte 'G' (0x47) por el mismo socket TCP
// que ya se usa para enviar el audio.
const uint8_t CMD_GRABAR = 'G';

bool comandoGrabarRecibido() {
  if (!(client && client.connected())) return false;
  bool encontrado = false;
  // Vaciar el buffer de entrada; si en algun punto llego CMD_GRABAR, disparar.
  while (client.available() > 0) {
    int b = client.read();
    if (b == CMD_GRABAR) encontrado = true;
  }
  return encontrado;
}

void loop() {
  // Aceptar / mantener un cliente (app movil o PC)
  if (!client || !client.connected()) {
    WiFiClient nuevo = server.available();
    if (nuevo) {
      client = nuevo;
      Serial.println("Cliente conectado.");
      oledEstado("Conectado");
    }
  }

  // El pulsador fisico sigue funcionando como respaldo...
  bool disparar = botonPresionado();

  // ...y el comando remoto desde la app evita el ruido del clic.
  if (comandoGrabarRecibido()) disparar = true;

  if (disparar) {
    grabar();
  }
}
