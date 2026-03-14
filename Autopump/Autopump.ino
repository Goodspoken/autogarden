#include <Wire.h>
#include <SoftwareSerial.h>
#include "RTClib.h"
#include "LCD_1602_RUS.h"

/* ====== НАСТРОЙКИ ====== */
#define DRIVER_VERSION 1
#define LCD_ADDR 0x27


/* ===== Пины ===== */
#define ENC_CLK   A2
#define ENC_DT    A3
#define ENC_SW    7

#define BTN_P1    4
#define BTN_P2    5
#define BTN_HOME  6

#define PUMP1_PIN 10
#define PUMP2_PIN 12

#define FAN1_PIN  2     // 12В реле №1 (активно LOW)
#define FAN2_PIN  3     // 12В реле №2 (активно LOW)

#define SOIL1     A6
#define SOIL2     A7
#define LDR1      A0
#define LDR2      A1

struct PumpCfg;  static inline bool shouldAutoStart(PumpCfg& P);

/* ===== Реле логика ===== */
const uint8_t RELAY_ON        = LOW;   // для помп (5В реле)
const uint8_t RELAY_OFF       = HIGH;
const uint8_t FAN_RELAY_ON    = LOW;   // для 12В реле вентиляторов
const uint8_t FAN_RELAY_OFF   = HIGH;

/* ===== “сетевой” UART на D8/D9 (Nano<->ESP32) ===== */
SoftwareSerial net(8, 9);   // RX=D8, TX=D9 @9600

/* ===== Устройства ===== */
LCD_1602_RUS lcd(LCD_ADDR, 16, 2);
RTC_DS3231    rtc;

/* ===== Режимы полива ===== */
enum ModeType { MODE_DAILY, MODE_6H, MODE_12H, MODE_18H, MODE_24H };
static inline const char* modeLabel(ModeType m){
  switch(m){
    case MODE_DAILY: return "D";
    case MODE_6H:    return "6";
    case MODE_12H:   return "12";
    case MODE_18H:   return "18";
    case MODE_24H:   return "24";
  }
  return "?";
}
static inline uint16_t modeToHours(ModeType m){
  switch(m){
    case MODE_6H:  return 6;
    case MODE_12H: return 12;
    case MODE_18H: return 18;
    case MODE_24H: return 24;
    default:       return 0;  // DAILY
  }
}

/* ===== Структура помпы ===== */
struct PumpCfg {
  ModeType mode = MODE_6H;
  uint8_t  dailyHour = 7;
  uint8_t  dailyMin  = 0;
  uint8_t  runMin    = 1;     // 1..99 мин
  int      soilPin   = -1;    // A6/A7 или -1 (нет датчика)
  int      soilThr   = 750;   // 0..1023 (>=thr — сухо)
  bool     manualActive = false;  // помпа включена вручную
  bool     running      = false;  // сейчас работает
  unsigned long startMs = 0;      // таймер запуска
  DateTime lastWater = DateTime(2000,1,1,0,0,0);
};
PumpCfg P1, P2;

/* ===== Энкодер и кнопки ===== */
static const uint8_t  ENC_DETENT = 4;
static const unsigned ENC_MIN_US = 800;
int lastAB=0, acc=0; unsigned long lastEdgeUs=0;
bool swPrev=false, bP1was=false, bP2was=false, bHomeWas=false;
unsigned long lastBtnMs=0;

int encStep(){
  int a=digitalRead(ENC_CLK), b=digitalRead(ENC_DT), ab=(a<<1)|b;
  unsigned long now=micros();
  if(now-lastEdgeUs<ENC_MIN_US){ lastAB=ab; return 0; }
  static const int8_t t[4][4]={{0,+1,-1,0},{-1,0,0,+1},{+1,0,0,-1},{0,-1,+1,0}};
  int d=t[lastAB][ab]; lastAB=ab;
  if(d!=0){
    lastEdgeUs=now; acc += -d;
    if(acc>=ENC_DETENT){acc=0; return +1;}
    if(acc<=-ENC_DETENT){acc=0; return -1;}
  }
  return 0;
}
bool swClick(){
  bool now=(digitalRead(ENC_SW)==LOW);
  bool edge=(swPrev && !now);
  swPrev=now;
  if(edge){
    unsigned long ms=millis();
    if(ms-lastBtnMs<50) return false;
    lastBtnMs=ms; return true;
  }
  return false;
}
bool edgeHigh(bool& was, int pin){
  bool now=(digitalRead(pin)==HIGH);
  bool e=(!was && now);
  if(e){
    unsigned long ms=millis();
    if(ms-lastBtnMs<50) return false;
    lastBtnMs=ms;
  }
  was=now; return e;
}

/* ===== Помпы ===== */
static inline bool soilDry(const PumpCfg& P){
  if(P.soilPin<0) return true;
  int v=analogRead(P.soilPin);
  return v >= P.soilThr;
}
static inline void pumpStart(PumpCfg& P, uint8_t pin){
  P.running=true; P.startMs=millis(); digitalWrite(pin, RELAY_ON);
}
static inline void pumpStop(PumpCfg& P, uint8_t pin){
  P.running=false; digitalWrite(pin, RELAY_OFF);
}
static inline bool shouldAutoStart(PumpCfg& P){
  if(P.manualActive || P.running) return false;
  if(!soilDry(P)) return false;

  DateTime now = rtc.now();
  if(P.mode == MODE_DAILY){
    if(now.hour()==P.dailyHour && now.minute()==P.dailyMin){
      bool sameDay = (P.lastWater.year()==now.year() &&
                      P.lastWater.month()==now.month() &&
                      P.lastWater.day()==now.day());
      bool sameMin = (P.lastWater.hour()==now.hour() &&
                      P.lastWater.minute()==now.minute());
      if(!(sameDay && sameMin)) return true;
    }
  }else{
    TimeSpan elapsed = now - P.lastWater;
    TimeSpan need(0, modeToHours(P.mode), 0, 0);
    if(elapsed.totalseconds() >= need.totalseconds()) return true;
  }
  return false;
}

/* ===== Вентиляторы по LDR ===== */
enum FanState { FAN_OFF, FAN_WAIT_ON, FAN_ON, FAN_WAIT_OFF };
FanState fanState = FAN_OFF;

const int   LDR_ON  = 700;   // светлее — считаем «свет включен»
const int   LDR_OFF = 600;   // темнее  — «свет выключен»
const unsigned long FAN_DELAY_ON_MS  = 60UL*1000;   // 60 c до включения
const unsigned long FAN_DELAY_OFF_MS = 300UL*1000;  // 300 c до выключения

unsigned long fanTs = 0;   // таймер ожидания
int ldr1_f = 0, ldr2_f = 0;
static inline int fAvg(int prev, int raw){ return (prev*3 + raw)/4; }

static inline void fansWrite(bool on){
  digitalWrite(FAN1_PIN, on?FAN_RELAY_ON:FAN_RELAY_OFF);
  digitalWrite(FAN2_PIN, on?FAN_RELAY_ON:FAN_RELAY_OFF);
}
static inline bool lightIsOn(){
  int r1 = analogRead(LDR1);
  int r2 = analogRead(LDR2);
  ldr1_f = fAvg(ldr1_f==0? r1:ldr1_f, r1);
  ldr2_f = fAvg(ldr2_f==0? r2:ldr2_f, r2);
  int l = max(ldr1_f, ldr2_f);     // берём «хужее» (ярче)
  if(fanState==FAN_OFF || fanState==FAN_WAIT_ON)  return (l >= LDR_ON);
  else                                             return (l >= LDR_OFF);
}
static inline void fanFSM(){
  bool lit = lightIsOn();
  unsigned long ms = millis();
  switch(fanState){
    case FAN_OFF:
      if(lit){ fanState=FAN_WAIT_ON; fanTs=ms; }
      break;
    case FAN_WAIT_ON:
      if(!lit){ fanState=FAN_OFF; }
      else if(ms - fanTs >= FAN_DELAY_ON_MS){ fanState=FAN_ON; fansWrite(true); }
      break;
    case FAN_ON:
      if(!lit){ fanState=FAN_WAIT_OFF; fanTs=ms; }
      break;
    case FAN_WAIT_OFF:
      if(lit){ fanState=FAN_ON; }
      else if(ms - fanTs >= FAN_DELAY_OFF_MS){ fanState=FAN_OFF; fansWrite(false); }
      break;
  }
}

/* ===== UI ===== */
enum UiState { UI_HOME, UI_SEL_PUMP, UI_SEL_MODE, UI_SET_D_H, UI_SET_D_M, UI_SET_RUN, UI_SET_SOIL };
UiState ui=UI_HOME; uint8_t selectedPump=0, cursor=0;

static inline void printPad2(uint8_t col,uint8_t row,const char* s){
  lcd.setCursor(col,row); if(strlen(s)==1){ lcd.print(" "); } lcd.print(s);
}
static inline void drawHome(){
  DateTime now=rtc.now();
  char t[6]; snprintf(t,sizeof(t),"%02d:%02d",now.hour(),now.minute());
  lcd.clear(); lcd.setCursor(0,0); lcd.print(t);
  printPad2(0,1,  modeLabel(P1.mode));
  lcd.setCursor(7,1); lcd.print((P1.manualActive||P2.manualActive)?"R":"-");
  printPad2(14,1, modeLabel(P2.mode));
}
static inline void drawSelPump(){
  lcd.clear(); lcd.setCursor(0,0); lcd.print("Select pump");
  lcd.setCursor(0,1);
  lcd.print((selectedPump==0)?">P1  ":" P1  ");
  lcd.print((selectedPump==1)?">P2":" P2");
}
static inline void drawSelMode(){
  lcd.clear(); lcd.setCursor(0,0); lcd.print("Mode:");
  lcd.setCursor(6,0); lcd.print(modeLabel((ModeType)(cursor%5)));
  lcd.setCursor(0,1); lcd.print("OK=set");
}
static inline void drawSet(const char* cap, uint16_t v){
  lcd.clear(); lcd.setCursor(0,0); lcd.print(cap);
  lcd.setCursor(0,1); if(v<10) lcd.print("0"); lcd.print((int)v);
}

/* ===== Диагностика I2C ===== */
void scanI2C(){
  Serial.println(F("I2C scan..."));
  for(uint8_t a=1;a<127;a++){
    Wire.beginTransmission(a);
    if(Wire.endTransmission()==0){
      Serial.print(F("  found @ 0x")); if(a<16) Serial.print('0'); Serial.println(a,HEX);
    }
  }
}

/* ===== SETUP ===== */
void setup(){
  Serial.begin(9600);
  net.begin(9600);

  pinMode(PUMP1_PIN,OUTPUT); pinMode(PUMP2_PIN,OUTPUT);
  digitalWrite(PUMP1_PIN,RELAY_OFF); digitalWrite(PUMP2_PIN,RELAY_OFF);

  pinMode(FAN1_PIN,OUTPUT); pinMode(FAN2_PIN,OUTPUT);
  fansWrite(false);

  pinMode(ENC_CLK,INPUT_PULLUP);
  pinMode(ENC_DT ,INPUT_PULLUP);
  pinMode(ENC_SW ,INPUT_PULLUP);

  pinMode(BTN_P1,INPUT); pinMode(BTN_P2,INPUT); pinMode(BTN_HOME,INPUT);

  Wire.begin();
  scanI2C();

  lcd.init(); lcd.backlight(); lcd.clear();
  lcd.setCursor(0,0); lcd.print("LCD init...");
  delay(150);

  if(!rtc.begin()){
    Serial.println(F("RTC NOT FOUND"));
    lcd.setCursor(0,1); lcd.print("RTC ERR");
  } else {
    if(rtc.lostPower()) rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  P1.soilPin = SOIL1; P2.soilPin = SOIL2;

  lastAB = ((digitalRead(ENC_CLK)<<1)|digitalRead(ENC_DT));
  drawHome();
}

/* ===== LOOP ===== */
void loop(){
  unsigned long ms=millis();

  // Вентиляторы
  fanFSM();

  // Вращение/меню
  int step=encStep();
  if(step!=0){
    switch(ui){
      case UI_HOME: ui=UI_SEL_PUMP; drawSelPump(); break;
      case UI_SEL_PUMP: selectedPump=(step>0)?1:0; drawSelPump(); break;
      case UI_SEL_MODE: cursor=(cursor+(step>0?1:4))%5; drawSelMode(); break;
      case UI_SET_D_H: {PumpCfg& P=(selectedPump==0)?P1:P2; int v=P.dailyHour+(step>0?1:-1); if(v<0)v=23; if(v>23)v=0; P.dailyHour=v; drawSet("Hour",P.dailyHour);} break;
      case UI_SET_D_M: {PumpCfg& P=(selectedPump==0)?P1:P2; int v=P.dailyMin +(step>0?1:-1); if(v<0)v=59; if(v>59)v=0; P.dailyMin=v;  drawSet("Min", P.dailyMin);}  break;
      case UI_SET_RUN: {PumpCfg& P=(selectedPump==0)?P1:P2; int v=P.runMin  +(step>0?1:-1); if(v<1)v=1;  if(v>99)v=99; P.runMin=v;    drawSet("Run", P.runMin);}   break;
      case UI_SET_SOIL:{PumpCfg& P=(selectedPump==0)?P1:P2; int v=P.soilThr+(step>0?5:-5); if(v<0)v=0; if(v>1023)v=1023; P.soilThr=v; drawSet("SoilThr", P.soilThr);} break;
    }
  }

  if(swClick()){
    switch(ui){
      case UI_HOME: ui=UI_SEL_PUMP; drawSelPump(); break;
      case UI_SEL_PUMP:{
        PumpCfg& P=(selectedPump==0)?P1:P2;
        ModeType m=P.mode;
        cursor=(m==MODE_DAILY)?0:(m==MODE_6H)?1:(m==MODE_12H)?2:(m==MODE_18H)?3:4;
        ui=UI_SEL_MODE; drawSelMode();
      } break;
      case UI_SEL_MODE:{
        PumpCfg& P=(selectedPump==0)?P1:P2;
        P.mode=(ModeType)(cursor%5);
        if(P.mode==MODE_DAILY){ ui=UI_SET_D_H; drawSet("Hour",P.dailyHour); }
        else{ ui=UI_SET_RUN; drawSet("Run",P.runMin); }
      } break;
      case UI_SET_D_H:   ui=UI_SET_D_M;   drawSet("Min",    (selectedPump==0?P1:P2).dailyMin); break;
      case UI_SET_D_M:   ui=UI_SET_RUN;   drawSet("Run",    (selectedPump==0?P1:P2).runMin);   break;
      case UI_SET_RUN:   ui=UI_SET_SOIL;  drawSet("SoilThr",(selectedPump==0?P1:P2).soilThr);  break;
      case UI_SET_SOIL:  ui=UI_HOME;      drawHome(); break;
    }
  }

  // Кнопки: ручной пуск/стоп помп
  if(edgeHigh(bP1was, BTN_P1)){
    if(!P1.manualActive){ P1.manualActive=true; if(!P1.running) pumpStart(P1,PUMP1_PIN); }
    else { pumpStop(P1,PUMP1_PIN); P1.manualActive=false; P1.lastWater=rtc.now(); }
    drawHome();
  }
  if(edgeHigh(bP2was, BTN_P2)){
    if(!P2.manualActive){ P2.manualActive=true; if(!P2.running) pumpStart(P2,PUMP2_PIN); }
    else { pumpStop(P2,PUMP2_PIN); P2.manualActive=false; P2.lastWater=rtc.now(); }
    drawHome();
  }
  if(edgeHigh(bHomeWas, BTN_HOME)){ ui=UI_HOME; drawHome(); }

  // Авто-стоп по длительности
  if(P1.running && (ms-P1.startMs >= (unsigned long)P1.runMin*60000UL)){ pumpStop(P1,PUMP1_PIN); P1.lastWater=rtc.now(); }
  if(P2.running && (ms-P2.startMs >= (unsigned long)P2.runMin*60000UL)){ pumpStop(P2,PUMP2_PIN); P2.lastWater=rtc.now(); }

  // Авто-старт
  if(!P1.manualActive && !P1.running && shouldAutoStart(P1)) pumpStart(P1,PUMP1_PIN);
  if(!P2.manualActive && !P2.running && shouldAutoStart(P2)) pumpStart(P2,PUMP2_PIN);

  // Раз в секунду — статус в Serial и в “сетевой” порт (ESP32)
  static unsigned long tUp=0;
  if(ms - tUp > 1000){
    tUp = ms;
    DateTime now=rtc.now();
    bool fanOn = (fanState==FAN_ON);
    char buf[96];
    snprintf(buf,sizeof(buf),
      "%02d:%02d  P1:%s P2:%s  LDR1:%d LDR2:%d  FAN:%s",
      now.hour(), now.minute(),
      P1.running?"ON":"OFF",
      P2.running?"ON":"OFF",
      ldr1_f, ldr2_f,
      fanOn?"ON":"OFF"
    );
    Serial.println(buf);
    net.println(buf);
    if(ui==UI_HOME) drawHome();
  }
}
