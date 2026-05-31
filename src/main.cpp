#include <Arduino.h>
#include <SPI.h>
#include <LovyanGFX.hpp>
#include <driver/i2s.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <nvs_flash.h>
#include <nvs.h>

// ============================================================
//  PANTALLA
// ============================================================
class LGFX : public lgfx::LGFX_Device {
    lgfx::Bus_SPI       _bus;
    lgfx::Panel_ILI9488 _panel;
    lgfx::Light_PWM     _light;
public:
    LGFX() {
        { auto cfg = _bus.config();
          cfg.spi_host   = SPI2_HOST;
          cfg.spi_mode   = 0;
          cfg.freq_write = 40000000;
          cfg.pin_sclk   = 47;
          cfg.pin_mosi   = 38;
          cfg.pin_miso   = 48;
          cfg.pin_dc     = 2;
          _bus.config(cfg);
          _panel.setBus(&_bus); }
        { auto cfg = _panel.config();
          cfg.pin_cs   = 1;
          cfg.pin_rst  = 0;
          cfg.pin_busy = -1;
          cfg.memory_width  = 320;
          cfg.memory_height = 480;
          cfg.panel_width   = 320;
          cfg.panel_height  = 480;
          cfg.invert    = false;
          cfg.rgb_order = false;
          _panel.config(cfg); }
        { auto cfg = _light.config();
          cfg.pin_bl      = 39;
          cfg.invert      = false;
          cfg.freq        = 44100;
          cfg.pwm_channel = 0;
          _light.config(cfg);
          _panel.setLight(&_light); }
        setPanel(&_panel);
    }
};

LGFX tft;

// ============================================================
//  I2S / AUDIO
// ============================================================
#define I2S_BCLK    8
#define I2S_LRCLK  16
#define I2S_DIN    18
#define SAMPLE_RATE 44100
#define I2S_PORT    I2S_NUM_0

typedef enum { SFX_NONE, SFX_SCORE, SFX_CRASH, SFX_GAMEOVER } AudioEvent;
QueueHandle_t    audioQueue;
volatile bool    musicRunning = false;
volatile bool    gameOver     = false;

SemaphoreHandle_t recordMutex;
volatile bool wifiActiu = false;

void audioInit() {
    i2s_config_t cfg = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate          = SAMPLE_RATE,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = 8,
        .dma_buf_len          = 512,
        .use_apll             = false,
        .tx_desc_auto_clear   = true,
    };
    i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
    i2s_pin_config_t pins = {
        .bck_io_num   = I2S_BCLK,
        .ws_io_num    = I2S_LRCLK,
        .data_out_num = I2S_DIN,
        .data_in_num  = I2S_PIN_NO_CHANGE,
    };
    i2s_set_pin(I2S_PORT, &pins);
    i2s_zero_dma_buffer(I2S_PORT);
    audioQueue = xQueueCreate(5, sizeof(AudioEvent));
}

void playTone(float freq, int durationMs, float volume = 0.1f) {
    const int bufSize = 256;
    int16_t buf[bufSize * 2];
    const int samples = SAMPLE_RATE * durationMs / 1000;
    int written = 0;
    while (written < samples) {
        int chunk = min(bufSize, samples - written);
        for (int i = 0; i < chunk; i++) {
            float t   = (float)(written + i) / SAMPLE_RATE;
            int16_t v = (int16_t)(sinf(2.0f * M_PI * freq * t) * 32767.0f * volume);
            buf[i*2] = v; buf[i*2+1] = v;
        }
        size_t bw;
        i2s_write(I2S_PORT, buf, chunk * 4, &bw, portMAX_DELAY);
        written += chunk;
    }
}

void playSilence(int durationMs) {
    const int bufSize = 256;
    int16_t buf[bufSize * 2] = {0};
    const int samples = SAMPLE_RATE * durationMs / 1000;
    int written = 0;
    while (written < samples) {
        int chunk = min(bufSize, samples - written);
        size_t bw;
        i2s_write(I2S_PORT, buf, chunk * 4, &bw, portMAX_DELAY);
        written += chunk;
    }
}

struct Note { float freq; int dur; };
const Note MUSIC[] = {
    {220.0f,120},{0,30},{277.2f,120},{0,30},{329.6f,120},{0,30},
    {440.0f,180},{0,30},{329.6f,120},{0,30},{277.2f,120},{0,30},
    {220.0f,120},{0,30},{0,100},
    {293.7f,120},{0,30},{369.9f,120},{0,30},{440.0f,120},{0,30},
    {587.3f,180},{0,30},{440.0f,120},{0,30},{369.9f,120},{0,30},
    {293.7f,120},{0,30},{0,100},
    {261.6f,120},{0,30},{329.6f,120},{0,30},{392.0f,120},{0,30},
    {523.3f,200},{0,30},{392.0f,120},{0,30},{329.6f,120},{0,30},
    {261.6f,120},{0,30},{0,150},
};
const int MUSIC_LEN = sizeof(MUSIC)/sizeof(Note);

void musicTask(void* param) {
    int noteIdx = 0;
    AudioEvent evt;
    while (true) {
        if (xQueueReceive(audioQueue, &evt, 0) == pdTRUE) {
            switch(evt) {
                case SFX_SCORE:
                    playTone(880, 50, 0.08f);
                    playTone(1100, 50, 0.08f);
                    break;
                case SFX_CRASH:
                    for (int f=300; f>60; f-=20) playTone((float)f, 15, 0.1f);
                    break;
                case SFX_GAMEOVER:
                    playTone(523.3f,180,0.1f); playSilence(30);
                    playTone(493.9f,180,0.1f); playSilence(30);
                    playTone(440.0f,180,0.1f); playSilence(30);
                    playTone(392.0f,400,0.12f);
                    break;
                default: break;
            }
            continue;
        }
        if (musicRunning && !gameOver) {
            Note n = MUSIC[noteIdx];
            if (n.freq > 0.0f) playTone(n.freq, n.dur, 0.07f);
            else playSilence(n.dur);
            noteIdx = (noteIdx + 1) % MUSIC_LEN;
        } else {
            playSilence(20);
        }
    }
}

void triggerScore()    { AudioEvent e=SFX_SCORE;    xQueueSend(audioQueue,&e,0); }
void triggerCrash()    { AudioEvent e=SFX_CRASH;    xQueueSend(audioQueue,&e,0); }
void triggerGameOver() { AudioEvent e=SFX_GAMEOVER; xQueueSend(audioQueue,&e,0); }

#define AP_SSID "ESPectro"
#define AP_PASS "gameloader"
#define AP_IP   "192.168.4.1"

WebServer server(80);

void wifiTask(void* param) {
    while (true) {
        if (wifiActiu) server.handleClient();
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

// ============================================================
//  PINS
// ============================================================
#define JOY_X_PIN  5
#define JOY_Y_PIN  4
#define JOY_SW_PIN 42
#define BTN_A_PIN  40
#define BTN_B_PIN  41

// ============================================================
//  DASHBOARD WEB
// ============================================================
const char PAGE_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="ca">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESPectro — Dashboard</title>
<style>
*{box-sizing:border-box;margin:0;padding:0;}
body{background:#0d0d0d;color:#eee;font-family:'Courier New',monospace;
     min-height:100vh;padding:1em;}
h1{color:#ff5000;text-align:center;font-size:1.8em;letter-spacing:4px;
   padding:0.5em 0;border-bottom:2px solid #ff5000;margin-bottom:1em;}
h1 span{color:#fff;}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:1em;max-width:900px;margin:0 auto;}
@media(max-width:600px){.grid{grid-template-columns:1fr;}}
.card{background:#1a1a1a;border:1px solid #333;border-radius:10px;padding:1.2em;}
.card h2{color:#ff5000;font-size:0.9em;letter-spacing:2px;margin-bottom:0.8em;}
.game-card{background:#1a1a1a;border:1px solid #333;border-radius:10px;
           padding:1.2em;margin-bottom:1em;}
.game-card h3{color:#ff5000;font-size:0.9em;letter-spacing:2px;margin-bottom:0.8em;}
.stat{display:flex;justify-content:space-between;align-items:center;
      padding:0.4em 0;border-bottom:1px solid #222;}
.stat:last-child{border:none;}
.stat-label{color:#888;font-size:0.85em;}
.stat-val{color:#0f0;font-weight:bold;font-size:1.1em;}
.stat-val.gold{color:#ffd700;}
.chart{margin-top:0.8em;}
.chart-title{color:#888;font-size:0.75em;margin-bottom:0.5em;}
.bars{display:flex;align-items:flex-end;gap:3px;height:80px;}
.bar-wrap{display:flex;flex-direction:column;align-items:center;flex:1;}
.bar{width:100%;background:#ff5000;border-radius:2px 2px 0 0;min-height:2px;}
.bar-val{color:#888;font-size:0.55em;margin-top:2px;}
input[type=file]{display:none;}
label.btn,button.btn{display:inline-block;padding:0.6em 1.2em;margin:0.3em 0;
  background:#ff5000;color:#fff;border:none;border-radius:6px;
  font-size:0.9em;font-family:monospace;cursor:pointer;width:100%;}
label.btn:hover,button.btn:hover{background:#cc3e00;}
#filename{color:#ff5000;margin:0.4em 0;font-size:0.85em;min-height:1.2em;}
#progress{width:100%;background:#222;border-radius:4px;height:12px;
          margin:0.5em 0;display:none;}
#bar{height:100%;width:0;background:#ff5000;border-radius:4px;transition:width 0.3s;}
#status{min-height:1.5em;font-size:0.85em;color:#ff0;}
.ok{color:#0f0!important;} .err{color:#f44!important;}
</style>
</head>
<body>
<h1><span>ESP</span>ectro — Dashboard</h1>
<div class="grid">
  <div id="games-col">
    <div id="games-container">
      <div class="game-card"><span style="color:#555">Carregant...</span></div>
    </div>
  </div>
  <div class="card">
    <h2>Carregar joc</h2>
    <label class="btn" for="file">Triar .bin</label>
    <input type="file" id="file" accept=".bin">
    <div id="filename">Cap arxiu seleccionat</div>
    <button class="btn" onclick="upload()">Pujar joc</button>
    <div id="progress"><div id="bar"></div></div>
    <div id="status"></div>
  </div>
</div>
<script>
function avg(arr){return arr.length?Math.round(arr.reduce((a,b)=>a+b,0)/arr.length):0;}
function renderAllGames(data){
  const entries=Object.entries(data);
  const container=document.getElementById('games-container');
  if(!entries.length){
    container.innerHTML='<div class="game-card"><span style="color:#555">Cap joc registrat</span></div>';
    return;
  }
  container.innerHTML=entries.map(([key,gd])=>{
    const hist=gd.history||[];
    const best=gd.best||0;
    const mitjana=avg(hist);
    const darrera=hist[0]||0;
    const last10=hist.slice(0,10).reverse();
    const maxVal=Math.max(...last10,1);
    const bars=last10.length?last10.map(v=>{
      const h=Math.round((v/maxVal)*70);
      return`<div class="bar-wrap"><div class="bar" style="height:${h}px;background:${v===best&&best>0?'#ffd700':'#ff5000'}"></div><div class="bar-val">${v}</div></div>`;
    }).join(''):'<span style="color:#555;font-size:0.8em">Sense dades</span>';
    return`<div class="game-card">
      <h3>${key.replace(/_/g,' ').toUpperCase()}</h3>
      <div class="stat"><span class="stat-label">Record</span><span class="stat-val gold">${best} pts</span></div>
      <div class="stat"><span class="stat-label">Partides</span><span class="stat-val">${hist.length}</span></div>
      <div class="stat"><span class="stat-label">Mitjana</span><span class="stat-val">${mitjana} pts</span></div>
      <div class="stat"><span class="stat-label">Darrera</span><span class="stat-val ${darrera===best&&best>0?'gold':''}">${darrera} pts</span></div>
      <div class="chart"><div class="chart-title">Ultimes partides</div>
      <div class="bars">${bars}</div></div>
    </div>`;
  }).join('');
}
function load(){
  fetch('/records').then(r=>r.json()).then(renderAllGames).catch(()=>{});
}
load();
setInterval(load,10000);
const fi=document.getElementById('file');
fi.addEventListener('change',()=>{document.getElementById('filename').textContent=fi.files[0]?.name||'Cap arxiu';});
function upload(){
  const file=fi.files[0];
  const status=document.getElementById('status');
  const bar=document.getElementById('bar');
  const prog=document.getElementById('progress');
  if(!file){status.textContent='Selecciona un arxiu';return;}
  if(!file.name.endsWith('.bin')){status.textContent='Ha de ser .bin';return;}
  const xhr=new XMLHttpRequest();
  xhr.open('POST','/update',true);
  xhr.upload.onprogress=e=>{
    if(e.lengthComputable){const pct=Math.round(e.loaded/e.total*100);prog.style.display='block';bar.style.width=pct+'%';status.textContent='Pujant... '+pct+'%';}
  };
  xhr.onload=()=>{
    if(xhr.status===200){status.textContent='Instal.lat. Reiniciant...';status.className='ok';}
    else{status.textContent='Error: '+xhr.responseText;status.className='err';}
  };
  xhr.onerror=()=>{status.textContent='Error connexio';status.className='err';};
  const fd=new FormData();fd.append('firmware',file,file.name);xhr.send(fd);
}
</script>
</body>
</html>
)rawhtml";

const int SCREEN_W = 320;
const int SCREEN_H = 480;

void showSplash() {
    tft.fillScreen(TFT_BLACK);
    const uint16_t TARONJA = tft.color565(255, 80, 0);
    tft.fillRect(0, 0, SCREEN_W, 6, TARONJA);
    tft.setTextSize(5);
    int y_titol = 140;
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(SCREEN_W/2 - tft.textWidth("ESPectro")/2, y_titol);
    tft.print("ESP");
    tft.setTextColor(TARONJA, TFT_BLACK);
    tft.print("ectro");
    tft.drawFastHLine(40, y_titol+55, SCREEN_W-80, TARONJA);
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    const char* slogan = "Consola portatil ESP32-S3";
    tft.setCursor(SCREEN_W/2 - tft.textWidth(slogan)/2, y_titol+70);
    tft.print(slogan);
    const char* autors = "Noel Medina & Bernat Figuerola - UPC 2026";
    tft.setCursor(SCREEN_W/2 - tft.textWidth(autors)/2, y_titol+86);
    tft.print(autors);
    tft.fillRect(0, SCREEN_H-6, SCREEN_W, 6, TARONJA);
    playTone(220.0f, 120, 0.15f); playSilence(30);
    playTone(277.2f, 120, 0.15f); playSilence(30);
    playTone(329.6f, 120, 0.15f);
    delay(500);
}

// ============================================================
//  RECORDS NVS
// ============================================================
#define MAX_HISTORY 20

int loadRecord(const char* key) {
    nvs_handle_t h;
    nvs_flash_init();
    int32_t r = 0;
    if (nvs_open("records", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, key, &r);
        nvs_close(h);
    }
    return (int)r;
}

String loadHistory(const char* key) {
    nvs_handle_t h;
    nvs_flash_init();
    String hist = "[]";
    String hkey = String(key) + "_h";
    if (nvs_open("records", NVS_READONLY, &h) == ESP_OK) {
        size_t len = 0;
        if (nvs_get_str(h, hkey.c_str(), nullptr, &len) == ESP_OK && len > 0) {
            char* buf = new char[len];
            nvs_get_str(h, hkey.c_str(), buf, &len);
            hist = String(buf);
            delete[] buf;
        }
        nvs_close(h);
    }
    return hist;
}

void registerGame(const char* key) {
    nvs_handle_t h;
    nvs_flash_init();
    if (nvs_open("records", NVS_READWRITE, &h) == ESP_OK) {
        char buf[256] = "";
        size_t len = sizeof(buf);
        nvs_get_str(h, "game_list", buf, &len);
        String list = String(buf);
        if (list.indexOf(key) < 0) {
            if (list.length() > 0) list += ",";
            list += key;
            nvs_set_str(h, "game_list", list.c_str());
            nvs_commit(h);
        }
        nvs_close(h);
    }
}

void saveRecord(const char* key, int score) {
    registerGame(key);
    nvs_handle_t h;
    nvs_flash_init();
    if (nvs_open("records", NVS_READWRITE, &h) == ESP_OK) {
        int32_t current = 0;
        nvs_get_i32(h, key, &current);
        if (score > current)
            nvs_set_i32(h, key, (int32_t)score);
        String hkey = String(key) + "_h";
        String hist = "[]";
        size_t len = 0;
        if (nvs_get_str(h, hkey.c_str(), nullptr, &len) == ESP_OK && len > 0) {
            char* buf = new char[len];
            nvs_get_str(h, hkey.c_str(), buf, &len);
            hist = String(buf);
            delete[] buf;
        }
        String inner = hist.substring(1, hist.length()-1);
        String nova;
        if (inner.length() == 0) {
            nova = "[" + String(score) + "]";
        } else {
            int count = 1;
            for (int i = 0; i < (int)inner.length(); i++)
                if (inner[i] == ',') count++;
            if (count >= MAX_HISTORY) {
                int lc = inner.lastIndexOf(',');
                inner = (lc >= 0) ? inner.substring(0, lc) : "";
            }
            nova = (inner.length() > 0)
                ? "[" + String(score) + "," + inner + "]"
                : "[" + String(score) + "]";
        }
        nvs_set_str(h, hkey.c_str(), nova.c_str());
        nvs_commit(h);
        nvs_close(h);
    }
}

String getAllRecords() {
    if (xSemaphoreTake(recordMutex, pdMS_TO_TICKS(100)) != pdTRUE)
        return "{}";

    nvs_handle_t h;
    nvs_flash_init();
    String json = "{";
    if (nvs_open("records", NVS_READONLY, &h) == ESP_OK) {
        char buf[256] = "";
        size_t len = sizeof(buf);
        nvs_get_str(h, "game_list", buf, &len);
        String list = String(buf);
        
        bool first = true;
        int start = 0;
        while (start < (int)list.length()) {
            int comma = list.indexOf(',', start);
            String key = (comma < 0) 
                ? list.substring(start) 
                : list.substring(start, comma);
            
            if (key.length() > 0) {
                int32_t best = 0;
                nvs_get_i32(h, key.c_str(), &best);
                String hist = loadHistory(key.c_str());
                if (!first) json += ",";
                json += "\"" + key + "\":{\"best\":" + String(best) + 
                        ",\"history\":" + hist + "}";
                first = false;
            }
            if (comma < 0) break;
            start = comma + 1;
        }
        nvs_close(h);
    }
    json += "}";
    xSemaphoreGive(recordMutex);
    return json;
}
// ============================================================
//  HANDLERS WEB
// ============================================================
void handleRoot()    { server.send_P(200, "text/html", PAGE_HTML); }
void handleRecords() { server.send(200, "application/json", getAllRecords()); }
void handleUpdate()  {
    server.send(200, "text/plain", Update.hasError() ? "FALLO" : "OK");
    delay(500);
    ESP.restart();
}
void handleUpdateUpload() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        tft.fillRect(0, 260, 320, 60, TFT_BLACK);
        tft.setTextColor(TFT_YELLOW, TFT_BLACK);
        tft.setTextSize(2);
        tft.setCursor(10, 270);
        tft.print("Rebent firmware...");
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH))
            Update.printError(Serial);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
            Update.printError(Serial);
        static size_t total = 0;
        total += upload.currentSize;
        tft.fillRect(10, 300, constrain((int)(total/10000),0,100)*3, 10, TFT_GREEN);
    } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
            tft.fillRect(0, 260, 320, 60, TFT_BLACK);
            tft.setTextColor(TFT_GREEN, TFT_BLACK);
            tft.setTextSize(2);
            tft.setCursor(10, 270); tft.print("Joc instal·lat!");
            tft.setCursor(10, 295); tft.print("Reiniciant...");
        } else { Update.printError(Serial); }
    }
}

// ============================================================
//  HANDLERS MCP
// ============================================================
void handleMcpTools() {
    String json = "{\"tools\":["
        "{\"name\":\"get_records\","
         "\"description\":\"Retorna els records i historial de puntuacions de tots els jocs de la consola ESPectro\","
         "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
        "{\"name\":\"get_status\","
         "\"description\":\"Retorna l'estat actual de la consola: uptime, memoria lliure i versio\","
         "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
        "{\"name\":\"get_system_info\","
         "\"description\":\"Retorna informacio tecnica del hardware: CPU, memoria PSRAM, frequencia i chip\","
         "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}"
        "]}";
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", json);
}

void handleMcpGetRecords() {
    String records = getAllRecords();
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json",
        "{\"content\":[{\"type\":\"text\",\"text\":" + records + "}]}");
}

void handleMcpGetStatus() {
    unsigned long uptime = millis() / 1000;
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json",
        "{\"content\":[{\"type\":\"text\",\"text\":{"
        "\"uptime_s\":" + String(uptime) + ","
        "\"free_heap_bytes\":" + String(ESP.getFreeHeap()) + ","
        "\"wifi_ssid\":\"ESPectro\","
        "\"ip\":\"192.168.4.1\","
        "\"version\":\"1.0.0\"}}]}");
}

void handleMcpGetSystemInfo() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json",
        "{\"content\":[{\"type\":\"text\",\"text\":{"
        "\"chip\":\"ESP32-S3\","
        "\"cpu_freq_mhz\":" + String(ESP.getCpuFreqMHz()) + ","
        "\"flash_size_mb\":" + String(ESP.getFlashChipSize()/1024/1024) + ","
        "\"free_heap_bytes\":" + String(ESP.getFreeHeap()) + ","
        "\"free_psram_bytes\":" + String(ESP.getFreePsram()) + ","
        "\"sdk_version\":\"" + String(ESP.getSdkVersion()) + "\"}}]}");
}

void handleMcpCall() {
    String tool = server.arg("tool");
    if      (tool == "get_records")     handleMcpGetRecords();
    else if (tool == "get_status")      handleMcpGetStatus();
    else if (tool == "get_system_info") handleMcpGetSystemInfo();
    else server.send(404, "application/json",
             "{\"error\":\"Tool no trobada: " + tool + "\"}");
}

// ============================================================
//  GAME LOADER
// ============================================================
void runGameLoader() {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setTextSize(3);
    tft.setCursor(30, 40);   tft.print("GAME LOADER");
    tft.drawFastHLine(10, 88, 300, TFT_DARKGREY);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(10, 108);  tft.print("Xarxa WiFi:");
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setCursor(10, 130);  tft.print(AP_SSID);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(10, 158);  tft.print("Contrasenya:");
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setCursor(10, 180);  tft.print(AP_PASS);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(10, 210);  tft.print("Obre al navegador:");
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setCursor(10, 232);  tft.printf("http://%s", AP_IP);
    tft.drawFastHLine(10, 256, 300, TFT_DARKGREY);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(10, 268); tft.print("RECORDS:");
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(10, 282);
    tft.printf("Road Rush: %d pts", loadRecord("road_rush"));
    tft.setTextColor(tft.color565(150,150,150), TFT_BLACK);
    tft.setCursor(10, 440);
    tft.print("Prem A per tornar al menu");

    while (true) {
        if (digitalRead(BTN_A_PIN) == LOW) {
            delay(300);
            return;
        }
        delay(20);
    }
}

// ============================================================
//  MENÚ PRINCIPAL
// ============================================================
void drawMenu(int bestScore) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(tft.color565(255,60,0), TFT_BLACK);
    tft.setTextSize(4);
    const char* t1 = "ROAD";
    const char* t2 = "RUSH";
    tft.setCursor(320/2 - tft.textWidth(t1)/2, 50);  tft.print(t1);
    tft.setCursor(320/2 - tft.textWidth(t2)/2, 100); tft.print(t2);
    tft.drawFastHLine(40, 158, 240, tft.color565(255,60,0));
    tft.setTextColor(tft.color565(255,215,0), TFT_BLACK);
    tft.setTextSize(2);
    String best = "Record: " + String(bestScore) + " pts";
    tft.setCursor(320/2 - tft.textWidth(best)/2, 175);
    tft.print(best);
    tft.drawFastHLine(40, 210, 240, tft.color565(255,60,0));
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(320/2 - tft.textWidth("Prem A per jugar")/2, 250);
    tft.print("Prem A per jugar");
    tft.setCursor(320/2 - tft.textWidth("Prem B per carregar")/2, 290);
    tft.print("Prem B per carregar");
    tft.setCursor(320/2 - tft.textWidth("un nou joc")/2, 312);
    tft.print("un nou joc");
    uint16_t verd = tft.color565(0, 220, 40);
    tft.setTextSize(1);
    tft.fillCircle(SCREEN_W/2 - 105, 360, 4, wifiActiu ? verd : tft.color565(80,80,80));
    tft.setTextColor(wifiActiu ? verd : tft.color565(120,120,120), TFT_BLACK);
    const char* w = wifiActiu ? "WiFi actiu - ESPectro / 192.168.4.1" : "WiFi inactiu";
    tft.setCursor(SCREEN_W/2 - 95, 356);
    tft.print(w);
}

// ============================================================
//  JOC DE CARRERES
// ============================================================
const int ROAD_LEFT  = 20;
const int ROAD_RIGHT = SCREEN_W - 20;
const int ROAD_W     = ROAD_RIGHT - ROAD_LEFT;
const int CENTER_X   = (ROAD_LEFT + ROAD_RIGHT) / 2;
const int CAR_W = 45, CAR_H = 45;
const int CAR_Y = SCREEN_H - CAR_H - 15;
const int SPRITE_TOP = CAR_Y - 11;
const int SPRITE_BOT = CAR_Y + CAR_H + 3;
const int SPRITE_H   = SPRITE_BOT - SPRITE_TOP;
const int MAX_OBS = 8, OBS_W = 35, OBS_H = 35;
const int DASH_H = 18, DASH_GAP = 35;
const int BASE_SPEED = 3, BASE_SPAWN_DELAY = 28, FRAME_MS = 20;

uint16_t COL_ROAD;

struct Obs { int x, y, prevY; bool active; };
Obs obs[MAX_OBS];

bool gameRunning = true;
int  score=0, bestScore=0, prevScore=-1, prevBest=-1;
int  speedFactor=1, prevSpeedFactor=-1, spawnCounter=0;
int  carX=0, roadScroll=0;
unsigned long lastFrame=0;

int getDir(int v,int center=2048,int dz=300){
    if(v<center-dz)return -1;
    if(v>center+dz)return  1;
    return 0;
}
bool hit(int ax,int ay,int aw,int ah,int bx,int by,int bw,int bh){
    return ax<bx+bw&&ax+aw>bx&&ay<by+bh&&ay+ah>by;
}
void drawRoadBg(){
    tft.fillRect(0,0,ROAD_LEFT,SCREEN_H,TFT_BLACK);
    tft.fillRect(ROAD_RIGHT,0,SCREEN_W-ROAD_RIGHT,SCREEN_H,TFT_BLACK);
    tft.fillRect(ROAD_LEFT,0,ROAD_W,SCREEN_H,COL_ROAD);
    tft.drawFastVLine(ROAD_LEFT,0,SCREEN_H,TFT_WHITE);
    tft.drawFastVLine(ROAD_LEFT+1,0,SCREEN_H,TFT_WHITE);
    tft.drawFastVLine(ROAD_RIGHT,0,SCREEN_H,TFT_WHITE);
    tft.drawFastVLine(ROAD_RIGHT-1,0,SCREEN_H,TFT_WHITE);
}
void drawDashesAll(int scroll){
    for(int y=0;y<SCREEN_H;y++){
        int phase=(y+scroll)%DASH_GAP;
        if(phase<0)phase+=DASH_GAP;
        tft.drawFastHLine(CENTER_X-4,y,8,phase<DASH_H?TFT_WHITE:COL_ROAD);
    }
}
void updateDashes(int o,int n){
    if(o==n)return;
    for(int y=0;y<SCREEN_H;y++){
        int op=(y+o)%DASH_GAP;if(op<0)op+=DASH_GAP;
        int np=(y+n)%DASH_GAP;if(np<0)np+=DASH_GAP;
        if((op<DASH_H)!=(np<DASH_H))
            tft.drawFastHLine(CENTER_X-4,y,8,(np<DASH_H)?TFT_WHITE:COL_ROAD);
    }
}
void eraseCar(int x){ tft.fillRect(x,SPRITE_TOP,CAR_W,SPRITE_H,COL_ROAD); }
void drawCar(int x){
    const uint16_t RED=tft.color565(220,30,30),BLK=tft.color565(0,0,0);
    const uint16_t YEL=tft.color565(255,220,0),GRY=tft.color565(160,160,160);
    const uint16_t DKGRY=tft.color565(50,50,50);
    tft.fillRect(x,CAR_Y,CAR_W,CAR_H,RED);
    tft.fillRect(x+8,CAR_Y-10,CAR_W-16,10,DKGRY);
    tft.fillCircle(x+10,CAR_Y+CAR_H-6,8,BLK);
    tft.fillCircle(x+CAR_W-10,CAR_Y+CAR_H-6,8,BLK);
    tft.fillCircle(x+10,CAR_Y+CAR_H-6,5,GRY);
    tft.fillCircle(x+CAR_W-10,CAR_Y+CAR_H-6,5,GRY);
    tft.fillCircle(x+CAR_W-8,CAR_Y+10,5,YEL);
    tft.fillCircle(x+8,CAR_Y+10,5,YEL);
}
void drawObs(int x,int y){
    const uint16_t BROWN=tft.color565(139,69,19),ORANGE=tft.color565(255,165,0);
    tft.fillRect(x,y,OBS_W,OBS_H,BROWN);
    tft.fillRect(x+3,y+3,OBS_W-6,OBS_H-6,ORANGE);
    tft.drawLine(x+3,y+3,x+OBS_W-4,y+OBS_H-4,TFT_BLACK);
    tft.drawLine(x+OBS_W-4,y+3,x+3,y+OBS_H-4,TFT_BLACK);
}
void drawHUD(bool force){
    tft.setTextSize(1);
    if(force||score!=prevScore){
        tft.fillRect(0,0,ROAD_LEFT-1,24,TFT_BLACK);
        tft.setTextColor(TFT_WHITE,TFT_BLACK);
        tft.setCursor(1,1);tft.print("SC");
        tft.setCursor(1,10);tft.print(score);
        prevScore=score;
    }
    if(force||bestScore!=prevBest){
        tft.fillRect(0,26,ROAD_LEFT-1,24,TFT_BLACK);
        tft.setTextColor(tft.color565(255,215,0),TFT_BLACK);
        tft.setCursor(1,27);tft.print("BS");
        tft.setCursor(1,36);tft.print(bestScore);
        prevBest=bestScore;
    }
    if(force||speedFactor!=prevSpeedFactor){
        tft.fillRect(ROAD_RIGHT+1,0,SCREEN_W-ROAD_RIGHT-1,20,TFT_BLACK);
        tft.setTextColor(TFT_WHITE,TFT_BLACK);
        tft.setCursor(ROAD_RIGHT+2,1);tft.print("SP");
        tft.setCursor(ROAD_RIGHT+2,10);tft.print(speedFactor);
        prevSpeedFactor=speedFactor;
    }
tft.setTextColor(tft.color565(0, 180, 0), TFT_BLACK);
tft.setCursor(1, 52);
tft.print("Wi");
tft.setCursor(1, 61);
tft.print("Fi");
}
void spawnObs(){
    for(int i=0;i<MAX_OBS;i++){
        if(!obs[i].active){
            obs[i].x=random(ROAD_LEFT+2,ROAD_RIGHT-OBS_W-2);
            obs[i].y=obs[i].prevY=-OBS_H;
            obs[i].active=true;
            break;
        }
    }
}
void resetGame(){
    gameOver=false; gameRunning=true;
    score=0;prevScore=-1;prevBest=-1;
    speedFactor=1;prevSpeedFactor=-1;
    spawnCounter=0;roadScroll=0;
    carX=(ROAD_LEFT+ROAD_RIGHT)/2-CAR_W/2;
    for(int i=0;i<MAX_OBS;i++) obs[i].active=false;
    tft.startWrite();
    tft.fillScreen(TFT_BLACK);
    drawRoadBg();
    drawDashesAll(0);
    tft.setTextColor(TFT_WHITE,COL_ROAD);
    tft.setTextSize(3);
    const char* msg="READY?";
    tft.setCursor(SCREEN_W/2-tft.textWidth(msg)/2,SCREEN_H/2-12);
    tft.print(msg);
    tft.endWrite();
    delay(800);
    tft.startWrite();
    tft.fillRect(ROAD_LEFT,SCREEN_H/2-20,ROAD_W,50,COL_ROAD);
    drawDashesAll(0);
    drawCar(carX);
    drawHUD(true);
    tft.endWrite();
    musicRunning=true;
}
void showGameOver(){
    musicRunning=false;
    gameOver=true;
    triggerGameOver();
    delay(800);
    tft.startWrite();
    tft.fillScreen(TFT_BLACK);
    tft.setTextSize(3);
    tft.setTextColor(TFT_RED,TFT_BLACK);
    const char* g="GAME OVER";
    tft.setCursor(SCREEN_W/2-tft.textWidth(g)/2,SCREEN_H/2-70);
    tft.print(g);
    tft.setTextSize(2);
    tft.setTextColor(TFT_WHITE,TFT_BLACK);
    String s1="SCORE: "+String(score);
    tft.setCursor(SCREEN_W/2-tft.textWidth(s1)/2,SCREEN_H/2-10);
    tft.print(s1);
    tft.setTextColor(tft.color565(255,215,0),TFT_BLACK);
    String s2="BEST:  "+String(bestScore);
    tft.setCursor(SCREEN_W/2-tft.textWidth(s2)/2,SCREEN_H/2+20);
    tft.print(s2);
    tft.setTextColor(TFT_WHITE,TFT_BLACK);
    const char* p="PREM A per tornar";
    tft.setCursor(SCREEN_W/2-tft.textWidth(p)/2,SCREEN_H/2+70);
    tft.print(p);
    tft.endWrite();
}

void runGame(){
    COL_ROAD=tft.color565(105,105,105);
    bestScore=loadRecord("road_rush");
    randomSeed(analogRead(JOY_X_PIN));
    resetGame();
    lastFrame=millis();

    while(true){
        if(!gameRunning){
            if(score>bestScore) bestScore=score;
            if (xSemaphoreTake(recordMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                saveRecord("road_rush", score);
                xSemaphoreGive(recordMutex);
            }
            tft.fillScreen(TFT_BLACK);
            triggerCrash();
            showGameOver();
            while(digitalRead(BTN_A_PIN)==HIGH) delay(20);
            delay(50);
            musicRunning=false;
            return;
        }

        unsigned long now=millis();
        if(now-lastFrame<FRAME_MS) continue;
        lastFrame=now;

        int rawX=analogRead(JOY_X_PIN);
        int rawY=analogRead(JOY_Y_PIN);
        int dirX=getDir(rawX);
        speedFactor=(getDir(rawY)==-1)
            ?constrain(map(rawY,0,1748,6,1),1,6):1;
        int scrollSpeed=BASE_SPEED*speedFactor;
        int newCarX=constrain(carX+dirX*5,ROAD_LEFT,ROAD_RIGHT-CAR_W);
        int newScroll=(roadScroll+scrollSpeed)%DASH_GAP;

        for(int i=0;i<MAX_OBS;i++){
            if(obs[i].active){obs[i].prevY=obs[i].y;obs[i].y+=scrollSpeed;}
        }
        if(++spawnCounter>=max(6,BASE_SPAWN_DELAY/speedFactor)){
            spawnCounter=0;spawnObs();
        }
        for(int i=0;i<MAX_OBS;i++){
            if(obs[i].active&&hit(newCarX,CAR_Y,CAR_W,CAR_H,
                obs[i].x,obs[i].y,OBS_W,OBS_H)){
                gameRunning=false;
                break;
            }
        }

        tft.startWrite();
        updateDashes(roadScroll,newScroll);
        roadScroll=newScroll;
        for(int i=0;i<MAX_OBS;i++){
            if(!obs[i].active)continue;
            int ox=obs[i].x,oy=obs[i].y,py=obs[i].prevY;
            if(oy>SCREEN_H){
                if(py+OBS_H>0)
                    tft.fillRect(ox,max(py,0),OBS_W,
                        min(py+OBS_H,SCREEN_H)-max(py,0),COL_ROAD);
                obs[i].active=false;score++;
                triggerScore();
                continue;
            }
            if(oy>py){
                int top=max(py,0),bot=min(oy,SCREEN_H);
                if(bot>top)tft.fillRect(ox,top,OBS_W,bot-top,COL_ROAD);
            }
            if(oy+OBS_H>0)drawObs(ox,oy);
        }
        eraseCar(carX);carX=newCarX;drawCar(carX);
        drawHUD(false);
        tft.endWrite();
    }
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
    Serial.begin(115200);
    pinMode(JOY_X_PIN,  INPUT);
    pinMode(JOY_Y_PIN,  INPUT);
    pinMode(JOY_SW_PIN, INPUT_PULLUP);
    pinMode(BTN_A_PIN,  INPUT_PULLUP);
    pinMode(BTN_B_PIN,  INPUT_PULLUP);

    tft.init();
    tft.setRotation(2);
    tft.setBrightness(255);

    recordMutex = xSemaphoreCreateMutex();
    audioInit();

    xTaskCreatePinnedToCore(musicTask, "music", 4096, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(wifiTask,  "wifi",  4096, NULL, 1, NULL, 0);

    COL_ROAD = tft.color565(105,105,105);

    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    WiFi.softAPConfig(
        IPAddress(192,168,4,1),
        IPAddress(192,168,4,1),
        IPAddress(255,255,255,0)
    );
    // Endpoints dashboard i OTA
    server.on("/",        HTTP_GET,  handleRoot);
    server.on("/records", HTTP_GET,  handleRecords);
    server.on("/update",  HTTP_POST, handleUpdate, handleUpdateUpload);
    // Endpoints MCP
    server.on("/mcp/tools",      HTTP_GET, handleMcpTools);
    server.on("/mcp/tools/call", HTTP_GET, handleMcpCall);
    server.begin();
    wifiActiu = true;

    showSplash();
}

// ============================================================
//  LOOP
// ============================================================
void loop() {
    tft.fillScreen(TFT_BLACK);
    int best = loadRecord("road_rush");
    drawMenu(best);

    while (true) {
        if (digitalRead(BTN_A_PIN) == LOW) {
            delay(50);
            runGame();
            break;
        }
        if (digitalRead(BTN_B_PIN) == LOW) {
            delay(50);
            runGameLoader();
            break;
        }
        delay(20);
    }
}