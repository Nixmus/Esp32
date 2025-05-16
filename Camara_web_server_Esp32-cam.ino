/*
 * ESP32-CAM Web Server con Cámara
 * Este código configura un servidor web en el ESP32-CAM que muestra 
 * la transmisión de video de la cámara en tiempo real.
 */

#include "esp_camera.h"
#include <WiFi.h>
#include "esp_timer.h"
#include "img_converters.h"
#include "Arduino.h"
#include "soc/soc.h"           // Deshabilita el brownout detector
#include "soc/rtc_cntl_reg.h"  // Deshabilita el brownout detector
#include "driver/rtc_io.h"

// Reemplaza con tus credenciales de WiFi
const char* ssid = "WIFI_ALEJO";
const char* password = "NANANANA";

// Definir los pines de la cámara para el ESP32-CAM
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// LED Flash
#define FLASH_LED_PIN 4
bool flashState = false;

WiFiServer server(80);

void startCameraServer();

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // Deshabilitar detector de brownout
  
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

  // Configurar el pin del LED Flash como salida
  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  
  // Inicializar con alta calidad y menor tamaño de imagen
  if(psramFound()){
    config.frame_size = FRAMESIZE_VGA; // 640x480
    config.jpeg_quality = 10;
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_SVGA; // 800x600
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }

  // Inicializar cámara
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Error al inicializar la cámara: 0x%x", err);
    return;
  }

  // Configuraciones adicionales de la cámara
  sensor_t * s = esp_camera_sensor_get();
  s->set_brightness(s, 0);     // -2 to 2
  s->set_contrast(s, 0);       // -2 to 2
  s->set_saturation(s, 0);     // -2 to 2
  s->set_special_effect(s, 0); // 0 = No Effect, 1 = Negative, 2 = Grayscale, 3 = Red Tint, 4 = Green Tint, 5 = Blue Tint, 6 = Sepia
  s->set_whitebal(s, 1);       // 0 = disable, 1 = enable
  s->set_awb_gain(s, 1);       // 0 = disable, 1 = enable
  s->set_wb_mode(s, 0);        // 0 to 4 - Auto, Sunny, Cloudy, Office, Home
  s->set_exposure_ctrl(s, 1);  // 0 = disable, 1 = enable
  s->set_aec2(s, 0);           // 0 = disable, 1 = enable
  s->set_gain_ctrl(s, 1);      // 0 = disable, 1 = enable
  s->set_agc_gain(s, 0);       // 0 to 30
  s->set_gainceiling(s, (gainceiling_t)0);  // 0 to 6
  s->set_bpc(s, 0);            // 0 = disable, 1 = enable
  s->set_wpc(s, 1);            // 0 = disable, 1 = enable
  s->set_raw_gma(s, 1);        // 0 = disable, 1 = enable
  s->set_lenc(s, 1);           // 0 = disable, 1 = enable
  s->set_hmirror(s, 0);        // 0 = disable, 1 = enable
  s->set_vflip(s, 0);          // 0 = disable, 1 = enable
  s->set_dcw(s, 1);            // 0 = disable, 1 = enable
  s->set_colorbar(s, 0);       // 0 = disable, 1 = enable

  // Conectar a Wi-Fi
  WiFi.begin(ssid, password);
  WiFi.setSleep(false);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("");
  Serial.println("WiFi conectado");
  Serial.print("Dirección IP: ");
  Serial.println(WiFi.localIP());

  // Iniciar servidor web
  startCameraServer();

  Serial.println("Servidor web iniciado");
  Serial.print("Para ver la transmisión de video, ve a: http://");
  Serial.println(WiFi.localIP());
}

void loop() {
  // El código principal está en la función startCameraServer()
  delay(10000);
}

// Función que maneja la transmisión de video y las solicitudes HTTP
void startCameraServer() {
  server.begin();

  while(true) {
    WiFiClient client = server.available();
    if (client) {
      Serial.println("Nuevo cliente conectado");
      String currentLine = "";
      
      while (client.connected()) {
        if (client.available()) {
          char c = client.read();
          Serial.write(c);
          
          if (c == '\n') {
            // Si la línea actual está en blanco, tenemos dos caracteres de nueva línea seguidos.
            // Eso es el final de la solicitud HTTP del cliente, así que enviar una respuesta:
            if (currentLine.length() == 0) {
              // Encabezados HTTP siempre comienzan con un código de respuesta (e.g. HTTP/1.1 200 OK)
              // y un tipo de contenido para que el cliente sepa lo que viene, luego una línea en blanco:
              client.println("HTTP/1.1 200 OK");
              client.println("Content-type:text/html");
              client.println("Connection: close");
              client.println();
              
              // Página web HTML
              client.println("<!DOCTYPE html>");
              client.println("<html>");
              client.println("<head>");
              client.println("<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">");
              client.println("<style>");
              client.println("body { font-family: Arial; text-align: center; margin:0px auto; padding-top: 30px;}");
              client.println(".button { background-color: #4CAF50; border: none; color: white; padding: 10px 20px;");
              client.println("text-decoration: none; font-size: 18px; margin: 2px; cursor: pointer;}");
              client.println(".button2 {background-color: #555555;}");
              client.println("img { width: auto ; max-width: 100% ; height: auto ; }");
              client.println("</style>");
              client.println("</head>");
              client.println("<body>");
              client.println("<h1>ESP32-CAM Web Server</h1>");
              client.println("<img src=\"/stream\" id=\"stream\">");
              client.println("<p><button class=\"button\" onclick=\"toggleFlash()\">Toggle Flash</button></p>");
              client.println("<script>");
              client.println("function toggleFlash() {");
              client.println("  var xhr = new XMLHttpRequest();");
              client.println("  xhr.open('GET', '/flash', true);");
              client.println("  xhr.send();");
              client.println("}");
              client.println("</script>");
              client.println("</body>");
              client.println("</html>");
              
              // La respuesta HTTP termina con otra línea en blanco
              client.println();
              // Salir del bucle while
              break;
            } else {
              currentLine = "";
            }
          } else if (c != '\r') {
            // agrega el carácter a la currentLine
            currentLine += c;
          }
          
          // Comprobar si el cliente solicita la transmisión de video
          if (currentLine.endsWith("GET /stream")) {
            streamVideo(client);
          }
          
          // Comprobar si el cliente quiere controlar el flash
          if (currentLine.endsWith("GET /flash")) {
            flashState = !flashState;
            digitalWrite(FLASH_LED_PIN, flashState);
          }
        }
      }
      
      // Cerrar la conexión cuando terminamos
      client.stop();
      Serial.println("Cliente desconectado");
    }
  }
}

// Función para transmitir el video desde la cámara
void streamVideo(WiFiClient &client) {
  camera_fb_t *fb = NULL;
  
  // Encabezados HTTP para la respuesta MJPEG
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: multipart/x-mixed-replace; boundary=frame");
  client.println();
  
  while(true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Error al capturar imagen de la cámara");
      delay(1000);
      continue;
    }
    
    client.println("--frame");
    client.println("Content-Type: image/jpeg");
    client.print("Content-Length: ");
    client.println(fb->len);
    client.println();
    
    client.write(fb->buf, fb->len);
    client.println();
    
    // Liberar el buffer de la cámara
    esp_camera_fb_return(fb);
    
    // Si el cliente se desconecta, salimos del bucle
    if (!client.connected()) {
      break;
    }
    
    delay(50); // Un pequeño retraso para una transmisión más suave
  }
}
