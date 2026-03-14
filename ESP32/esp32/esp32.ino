#include <WiFi.h>
#include <WebServer.h>

/*** Wi-Fi ***/
const char* SSID = "WickedTomato420";
const char* PASS = "c0d3n4mE@Wickedt0mat00";

/*** UART2 (Nano) ***/
static const int RX2_PIN = 16;    // GPIO16 = RX2
static const int TX2_PIN = 17;    // GPIO17 = TX2
static const uint32_t UART_BAUD = 9600;
HardwareSerial Nano(2);

/*** Веб-сервер ***/
WebServer server(80);

/*** Кольцевой буфер последних строк (без String) ***/
#define RING_N     80
#define LINE_MAX   128
char ring[RING_N][LINE_MAX];
uint8_t head = 0;

inline void addLineC(const char* s){
  strncpy(ring[head], s, LINE_MAX-1);
  ring[head][LINE_MAX-1] = '\0';
  head = (head + 1) % RING_N;
}

/*** Страница UI (очень лёгкая) ***/
const char INDEX_HTML[] PROGMEM =
"<!doctype html><meta charset=utf-8>"
"<style>body{font:14px monospace;margin:16px}#log{white-space:pre-wrap}</style>"
"<h3>Nano status</h3><pre id=log>loading…</pre>"
"<form onsubmit='send(event)'><input id=cmd placeholder='CMD to Nano'><button>Send</button></form>"
"<script>"
"async function tick(){try{let r=await fetch('/status');document.getElementById('log').textContent=await r.text();}catch(e){}}"
"function send(ev){ev.preventDefault();let v=document.getElementById('cmd').value.trim();if(!v)return;"
"fetch('/cmd?c='+encodeURIComponent(v)).then(()=>document.getElementById('cmd').value='');}"
"tick();setInterval(tick,2000);"
"</script>";

/*** Хэндлеры ***/
void handleRoot(){
  server.send_P(200, "text/html", INDEX_HTML);
}
void handleStatus(){
  // формируем текст без динамики
  String out; out.reserve(RING_N * 32);
  for(uint8_t i=0;i<RING_N;i++){
    uint8_t idx = (head + i) % RING_N;
    if(ring[idx][0]){ out += ring[idx]; out += '\n'; }
  }
  server.sendHeader("Cache-Control","no-store");
  server.send(200, "text/plain; charset=utf-8", out);
}
void handleCmd(){
  if(server.hasArg("c")){
    String c = server.arg("c"); c.trim();
    if(c.length()){ Nano.println(c); server.send(200,"text/plain","SENT\n"); return; }
  }
  server.send(400,"text/plain","no cmd\n");
}

/*** Чтение UART неблокирующе (сбор строки в char[]) ***/
void pollUart(){
  static char line[LINE_MAX];
  static uint16_t n = 0;
  static uint32_t last = 0;

  while(Nano.available()){
    int ch = Nano.read();
    if(ch < 0) break;

    if(ch == '\r') continue;
    if(ch == '\n'){
      line[n] = '\0';
      if(n){ addLineC(line); Serial.println(line); }
      n = 0;
      last = millis();
      continue;
    }

    if(n < LINE_MAX-1){
      line[n++] = (char)ch;
    }else{
      // переполнение — закрываем строку, сбрасываем
      line[LINE_MAX-1] = '\0';
      addLineC(line);
      n = 0;
    }
    last = millis();
  }

  // таймаут «рваной» строки (если \n так и не пришёл)
  if(n && millis() - last > 50){
    line[n] = '\0';
    addLineC(line);
    Serial.println(line);
    n = 0;
  }
}

void setup(){
  // очистим буфер
  for(uint8_t i=0;i<RING_N;i++) ring[i][0] = '\0';

  Serial.begin(115200);

  // UART2: уровни! Nano TX (5V) -> через делитель -> GPIO16
  Nano.begin(UART_BAUD, SERIAL_8N1, RX2_PIN, TX2_PIN);

  // Wi-Fi STA (DHCP). Для постоянного IP лучше сделать DHCP-резервацию на роутере.
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.softAPdisconnect(true);
  WiFi.begin(SSID, PASS);

  Serial.print("Connecting");
  uint32_t t0 = millis();
  while(WiFi.status() != WL_CONNECTED){
    delay(300); Serial.print(".");
    if(millis()-t0 > 20000){ Serial.println("\nWiFi FAIL"); break; }
  }
  if(WiFi.status()==WL_CONNECTED){
    Serial.print("\nIP: "); Serial.println(WiFi.localIP());
  }

  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/cmd", handleCmd);
  server.begin();
}

void loop(){
  server.handleClient();
  pollUart();
  delay(1); // даём время Wi-Fi стеку
}
