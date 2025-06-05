#include "esp_camera.h"
#include <WiFi.h>
#include "esp_timer.h"
#include "img_converters.h"
#include "Arduino.h"
#include "fb_gfx.h"
#include "soc/soc.h"             // Disable brownout problems
#include "soc/rtc_cntl_reg.h"    // Disable brownout problems
#include "esp_http_server.h"

// Configuración de red WiFi
const char* ssid = "Familia.Garzon_2.4G";
const char* password = "22547995T";

// Configuración de pines para ESP32-CAM AI-Thinker
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
#define FLASH_LED_PIN      4

// Definición para streaming
#define PART_BOUNDARY "123456789000000000000987654321"

httpd_handle_t camera_httpd = NULL;
httpd_handle_t stream_httpd = NULL;

// Página web HTML para el streaming
static const char PROGMEM INDEX_HTML[] = R"rawliteral(
<html>
  <head>
    <title>ESP32-CAM Stream</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
      body { font-family: Arial; text-align: center; margin:0px auto; padding-top: 30px;}
      table { margin-left: auto; margin-right: auto; }
      td { padding: 8 px; }
      .button { background-color: #2f4468; border: none; color: white; 
                padding: 10px 20px; text-align: center; text-decoration: none; 
                display: inline-block; font-size: 18px; margin: 6px 3px; 
                cursor: pointer; -webkit-border-radius: 10px; border-radius: 10px; }
      .button:hover { background-color: #1f2e45; }
      .slider { -webkit-appearance: none; width: 300px; height: 20px; 
                border-radius: 5px; background: #d3d3d3; outline: none; }
      .slider::-webkit-slider-thumb { -webkit-appearance: none; appearance: none; 
                                     width: 40px; height: 40px; border-radius: 50%; 
                                     background: #2f4468; cursor: pointer; }
    </style>
  </head>
  <body>
    <h1>ESP32-CAM Stream</h1>
    <img id="stream" src="">
    <table>
      <tr>
        <td><button class="button" onclick="toggleFlash()">Flash ON/OFF</button></td>
        <td><button class="button" onclick="capturePhoto()">Capturar Foto</button></td>
      </tr>
      <tr>
        <td>Resolución:</td>
        <td>
          <select id="framesize" onchange="updateConfig()">
            <option value="10">UXGA(1600x1200)</option>
            <option value="9">SXGA(1280x1024)</option>
            <option value="8">XGA(1024x768)</option>
            <option value="7">SVGA(800x600)</option>
            <option value="6" selected>VGA(640x480)</option>
            <option value="5">CIF(400x296)</option>
            <option value="4">QVGA(320x240)</option>
          </select>
        </td>
      </tr>
      <tr>
        <td>Calidad:</td>
        <td><input type="range" id="quality" min="10" max="63" value="10" class="slider" onchange="updateConfig()"></td>
      </tr>
    </table>
    
    <script>
      window.onload = function() {
        document.getElementById("stream").src = window.location.href.slice(0, -1) + ":81/stream";
      }
      
      function toggleFlash() {
        var xhr = new XMLHttpRequest();
        xhr.open('GET', "/flash", true);
        xhr.send();
      }
      
      function capturePhoto() {
        // Crear un enlace temporal para descargar la foto
        var link = document.createElement('a');
        link.href = '/capture';
        link.download = 'ESP32CAM_' + new Date().toISOString().replace(/[:.]/g, '-') + '.jpg';
        document.body.appendChild(link);
        link.click();
        document.body.removeChild(link);
      }
      
      function updateConfig() {
        var framesize = document.getElementById("framesize").value;
        var quality = document.getElementById("quality").value;
        var xhr = new XMLHttpRequest();
        xhr.open('GET', "/control?var=framesize&val=" + framesize, true);
        xhr.send();
        xhr.open('GET', "/control?var=quality&val=" + quality, true);
        xhr.send();
      }
    </script>
  </body>
</html>
)rawliteral";

// Handler para la página principal
static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, (const char *)INDEX_HTML, strlen(INDEX_HTML));
}

// Handler para capturar una foto
static esp_err_t capture_handler(httpd_req_t *req) {
  camera_fb_t * fb = NULL;
  esp_err_t res = ESP_OK;
  
  fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Error al capturar imagen");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  
  res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

// Handler para el streaming de video
static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t * fb = NULL;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t * _jpg_buf = NULL;
  char * part_buf[64];
  
  static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
  static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
  static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";
  
  httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  
  while(true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Error al obtener frame");
      res = ESP_FAIL;
    } else {
      if(fb->width > 400){
        if(fb->format != PIXFORMAT_JPEG){
          bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
          esp_camera_fb_return(fb);
          fb = NULL;
          if(!jpeg_converted){
            Serial.println("Error en conversión JPEG");
            res = ESP_FAIL;
          }
        } else {
          _jpg_buf_len = fb->len;
          _jpg_buf = fb->buf;
        }
      }
    }
    
    if(res == ESP_OK){
      size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);
      res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
    }
    if(res == ESP_OK){
      res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
    }
    if(res == ESP_OK){
      res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
    }
    if(fb){
      esp_camera_fb_return(fb);
      fb = NULL;
      _jpg_buf = NULL;
    } else if(_jpg_buf){
      free(_jpg_buf);
      _jpg_buf = NULL;
    }
    if(res != ESP_OK){
      break;
    }
  }
  return res;
}

// Handler para controlar el flash
static esp_err_t flash_handler(httpd_req_t *req) {
  static bool flash_state = false;
  flash_state = !flash_state;
  digitalWrite(FLASH_LED_PIN, flash_state ? HIGH : LOW);
  
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_send(req, flash_state ? "Flash ON" : "Flash OFF", -1);
}

// Handler para controles de cámara
static esp_err_t control_handler(httpd_req_t *req) {
  char*  buf;
  size_t buf_len;
  char variable[32] = {0,};
  char value[32] = {0,};
  
  buf_len = httpd_req_get_url_query_len(req) + 1;
  if (buf_len > 1) {
    buf = (char*)malloc(buf_len);
    if(!buf){
      httpd_resp_send_500(req);
      return ESP_FAIL;
    }
    if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
      if (httpd_query_key_value(buf, "var", variable, sizeof(variable)) == ESP_OK &&
          httpd_query_key_value(buf, "val", value, sizeof(value)) == ESP_OK) {
      } else {
        free(buf);
        httpd_resp_send_404(req);
        return ESP_FAIL;
      }
    } else {
      free(buf);
      httpd_resp_send_404(req);
      return ESP_FAIL;
    }
    free(buf);
  } else {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  
  int val = atoi(value);
  sensor_t * s = esp_camera_sensor_get();
  
  if(!strcmp(variable, "framesize")) {
    s->set_framesize(s, (framesize_t)val);
  } else if(!strcmp(variable, "quality")) {
    s->set_quality(s, val);
  }
  
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, NULL, 0);
}

void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  
  httpd_uri_t index_uri = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = index_handler,
    .user_ctx  = NULL
  };
  
  httpd_uri_t capture_uri = {
    .uri       = "/capture",
    .method    = HTTP_GET,
    .handler   = capture_handler,
    .user_ctx  = NULL
  };
  
  httpd_uri_t flash_uri = {
    .uri       = "/flash",
    .method    = HTTP_GET,
    .handler   = flash_handler,
    .user_ctx  = NULL
  };
  
  httpd_uri_t control_uri = {
    .uri       = "/control",
    .method    = HTTP_GET,
    .handler   = control_handler,
    .user_ctx  = NULL
  };
  
  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &capture_uri);
    httpd_register_uri_handler(camera_httpd, &flash_uri);
    httpd_register_uri_handler(camera_httpd, &control_uri);
  }
  
  config.server_port += 1;
  config.ctrl_port += 1;
  
  httpd_uri_t stream_uri = {
    .uri       = "/stream",
    .method    = HTTP_GET,
    .handler   = stream_handler,
    .user_ctx  = NULL
  };
  
  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
  }
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // Disable brownout detector
  
  Serial.begin(115200);
  Serial.setDebugOutput(false);
  
  // Configurar pin del flash
  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);
  
  // Configuración de la cámara
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
  
  if(psramFound()){
    config.frame_size = FRAMESIZE_VGA;
    config.jpeg_quality = 10;
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }
  
  // Inicializar cámara
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Error al inicializar cámara: 0x%x", err);
    return;
  }
  
  // Configurar sensor
  sensor_t * s = esp_camera_sensor_get();
  s->set_brightness(s, 0);     // -2 a 2
  s->set_contrast(s, 0);       // -2 a 2
  s->set_saturation(s, 0);     // -2 a 2
  s->set_special_effect(s, 0); // 0 a 6 (0-No Effect, 1-Negative, 2-Grayscale, 3-Red Tint, 4-Green Tint, 5-Blue Tint, 6-Sepia)
  s->set_whitebal(s, 1);       // 0 = disable , 1 = enable
  s->set_awb_gain(s, 1);       // 0 = disable , 1 = enable
  s->set_wb_mode(s, 0);        // 0 a 4 - if awb_gain enabled (0 - Auto, 1 - Sunny, 2 - Cloudy, 3 - Office, 4 - Home)
  s->set_exposure_ctrl(s, 1);  // 0 = disable , 1 = enable
  s->set_aec2(s, 0);           // 0 = disable , 1 = enable
  s->set_ae_level(s, 0);       // -2 a 2
  s->set_aec_value(s, 300);    // 0 a 1200
  s->set_gain_ctrl(s, 1);      // 0 = disable , 1 = enable
  s->set_agc_gain(s, 0);       // 0 a 30
  s->set_gainceiling(s, (gainceiling_t)0);  // 0 a 6
  s->set_bpc(s, 0);            // 0 = disable , 1 = enable
  s->set_wpc(s, 1);            // 0 = disable , 1 = enable
  s->set_raw_gma(s, 1);        // 0 = disable , 1 = enable
  s->set_lenc(s, 1);           // 0 = disable , 1 = enable
  s->set_hmirror(s, 0);        // 0 = disable , 1 = enable
  s->set_vflip(s, 0);          // 0 = disable , 1 = enable
  s->set_dcw(s, 1);            // 0 = disable , 1 = enable
  s->set_colorbar(s, 0);       // 0 = disable , 1 = enable
  
  // Conectar a WiFi
  WiFi.begin(ssid, password);
  Serial.print("Conectando a WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi conectado");
  
  // Iniciar servidor web
  startCameraServer();
  
  Serial.print("Cámara lista! Ve a: ");
  Serial.print("http://");
  Serial.print(WiFi.localIP());
  Serial.println(" para ver el stream");
}

void loop() {
  delay(10000);
}
