// ============================================================
//  Captura INMP441 -> servidor TCP por WiFi (PCM 16 bits)
//  Sin DumbDisplay: el ESP32 transmite audio crudo a quien se conecte.
//  - Se une a la red (hotspot del telefono): modo estacion
//  - Driver I2S nuevo (i2s_std), 16 kHz, 32 bits, filtro de mediana
//  - Al conectarse un cliente, descarta unos buffers (arranque del micro)
//  Placa: ESP32-S3 WROOM N16R8
//
//  >>> PON AQUI EL SSID Y CLAVE DEL HOTSPOT DE TU TELEFONO <<<
// ============================================================

#include <WiFi.h>
#include <driver/i2s_std.h>

const char* WIFI_SSID     = "POCOX6Pro5G";
const char* WIFI_PASSWORD = "nana1708";
const uint16_t TCP_PORT   = 8000;

// --- Pines I2S del INMP441 ---
#define I2S_WS  4    // WS  (LRCL)
#define I2S_SCK 5    // SCK (BCLK)
#define I2S_SD  6    // SD  (DOUT del micro)

#define SOUND_SAMPLE_RATE 8000

const int GAIN_SHIFT = 3;        // MENOS = mas fuerte | MAS = mas bajo
const int WARMUP_BUFFERS = 0;   // buffers a descartar al conectar un cliente

const int BUF_SAMPLES = 6000;
int32_t rawBuffer[BUF_SAMPLES];
int32_t s24[BUF_SAMPLES];
int16_t outBuffer[BUF_SAMPLES];

WiFiServer server(TCP_PORT);
i2s_chan_handle_t rx_handle;

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
  std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;   // INMP441 con L/R a GND

  i2s_channel_init_std_mode(rx_handle, &std_cfg);
  i2s_channel_enable(rx_handle);
}

void setup() {
  Serial.begin(115200);
  delay(3000);                                   // espera a que el USB enumere
  Serial.println("=== Arranque OK, el Serial funciona ===");

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
  Serial.print("Conectado. IP del ESP32: ");
  Serial.println(WiFi.localIP());
  Serial.print("Puerto: ");
  Serial.println(TCP_PORT);

  server.begin();
}

void loop() {
  WiFiClient client = server.available();
  if (!client) return;

  Serial.println("Cliente conectado, transmitiendo audio...");
  int warm = WARMUP_BUFFERS;
  int32_t prevSample = 0;

  while (client.connected()) {
    size_t bytesRead = 0;
    i2s_channel_read(rx_handle, rawBuffer, sizeof(rawBuffer), &bytesRead, portMAX_DELAY);
    int n = bytesRead / 4;

    if (warm > 0) { warm--; continue; }   // descartar buffers de arranque

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

    client.write((uint8_t*)outBuffer, n * 2);
  }

  client.stop();
  Serial.println("Cliente desconectado.");
}
