#include <Arduino.h>
#include <DHTesp.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <HTTPClient.h>

/*** ===== WIFI + THINGSPEAK ===== ***/
#define USE_WIFI 1
const char* WIFI_SSID = "Retro";
const char* WIFI_PASS = "Retro_Maker";
const char* TS_API_KEY = "4IAN7QWMMBM42Y1P";
static const uint32_t TS_MIN_INTERVAL_MS = 20000; // >=15s

/*** ===== PINS ===== ***/
// ⚠ Real UV on ADC1 when Wi-Fi is ON (GPIO34 recommended)
static const int PIN_DHT      = 4;     // DHT11
static const int PIN_UV_REAL  = 34;    // real UV (ADC1)
static const int PIN_POT_UV   = 35;    // SIM UV pot
static const int PIN_POT_TEMP = 32;    // SIM TEMP pot
static const int PIN_R = 25, PIN_G = 26, PIN_B = 27;   // RGB
static const int PIN_BUZZER = 23;      // buzzer via transistor
static const int PIN_SERVO  = 5;       // servo
static const int BTN_SIM    = 13;      // SIM toggle to 3V3

/*** ===== OPTIONS & THRESHOLDS ===== ***/
static const bool  COMMON_ANODE     = false;
static const bool  BUZZ_ACTIVE_HIGH = true;
static const bool  PASSIVE_BUZZER   = false;

static const float UVI_TRIG_TH      = 8.0f;
static const float UVI_CLEAR_TH     = 6.0f;
static const float TEMP_BEEP_TH     = 35.0f;
static const float TEMP_BEEP_CLR    = 34.0f;
static const int   TEMP_BEEP_COUNT  = 5;
static const int   TEMP_BEEP_ON_MS  = 320;
static const int   TEMP_BEEP_OFF_MS = 120;

// Print once per second regardless of DHT period:
static const uint32_t PRINT_MS = 1000;

/*** ===== TIMING ===== ***/
static const uint32_t DHT_MS = 2500;
static const uint32_t UV_MS  = 300;
static const uint32_t DEBOUNCE_MS = 25;

/*** ===== UV mV→UVI maps ===== ***/
static const float uv_mv_zero_real  = 120.0f;
static const float uv_mv_uvi11_real = 2800.0f;

// Manual SIM calibration via serial: 'z' set zero, 'x' set max (maps to UVI 0..11)
float sim_uv_zero_mv = 0.0f;
float sim_uv_max_mv  = 3300.0f;

/*** ===== RISK (NOAA-like) ===== ***/
enum Risk { R_NORMAL=0, R_CAUTION=1, R_WARNING=2, R_DANGER=3 };
static inline Risk riskFromUV(float uvi){
  if(uvi>=11) return R_DANGER;
  if(uvi>=6)  return R_WARNING;
  if(uvi>=3)  return R_CAUTION;
  return R_NORMAL;
}
static inline Risk riskFromHI(float hi){
  if(hi>=54) return R_DANGER;
  if(hi>=41) return R_WARNING;
  if(hi>=32) return R_CAUTION;
  return R_NORMAL;
}
static inline const char* riskText(Risk r){
  switch(r){case R_NORMAL:return"NORMAL";case R_CAUTION:return"CAUTION";
            case R_WARNING:return"WARNING";default:return"DANGER";}
}

/*** ===== GLOBALS ===== ***/
DHTesp dht;
Servo  servo;
bool   servoAttached=false;
int    currentDeg=0, prevDegBeforeUV=0;
bool   uvEventActive=false, highTempActive=false, simEnabled=false;
uint32_t lastUV=0, lastDHT=0, lastPrint=0;

float T=NAN, RH=NAN, HI=NAN, UVI=0, UVmV=0;
float uv_mv_ema=NAN, tp_mv_ema=NAN;

/*** ===== RGB PWM ===== ***/
static const int CH_R=0, CH_G=1, CH_B=2;
static inline void setRGB(uint8_t r,uint8_t g,uint8_t b){
  if(COMMON_ANODE){ r=255-r; g=255-g; b=255-b; }
  ledcWrite(CH_R,r); ledcWrite(CH_G,g); ledcWrite(CH_B,b);
}

/*** ===== BUZZER ===== ***/
static const int BUZZ_CH = 3;
static const int BUZZ_TONE_HZ = 2500;
inline void buzzOn(){  if(PASSIVE_BUZZER) ledcWriteTone(BUZZ_CH, BUZZ_TONE_HZ);
                       else digitalWrite(PIN_BUZZER, BUZZ_ACTIVE_HIGH?HIGH:LOW); }
inline void buzzOff(){ if(PASSIVE_BUZZER) ledcWriteTone(BUZZ_CH, 0);
                       else digitalWrite(PIN_BUZZER, BUZZ_ACTIVE_HIGH?LOW:HIGH); }
void beepBurst(int count=TEMP_BEEP_COUNT, int onMs=TEMP_BEEP_ON_MS, int offMs=TEMP_BEEP_OFF_MS){
  for(int i=0;i<count;i++){ buzzOn(); delay(onMs); buzzOff(); delay(offMs); }
}

/*** ===== SERVO ===== ***/
void servoAttachOnce(){ if(!servoAttached){ servo.setPeriodHertz(50); servo.attach(PIN_SERVO,500,2400); servoAttached=true; } }
void servoDetachQuiet(){ if(servoAttached){ servo.detach(); servoAttached=false; } }
int clampi(int v,int a,int b){ return v<a?a:(v>b?b:v); }
void softMoveTo(int target){
  target = clampi(target, 0, 180);
  if(target==currentDeg) return;
  servoAttachOnce();
  int step=(target>currentDeg)?3:-3;
  for(int a=currentDeg; a!=target; a+=step){ servo.write(a); delay(12); }
  servo.write(target); currentDeg=target;
  delay(120); servoDetachQuiet();
}

/*** ===== ANALOG HELPERS ===== ***/
float ema(float x, float &y, float alpha){ if(isnan(y)) y=x; else y += alpha*(x-y); return y; }
float avgMilliVolts(int pin, int N=16){
  analogSetPinAttenuation(pin, ADC_11db);
  uint32_t s=0; for(int i=0;i<N;i++){ s+=analogReadMilliVolts(pin); delayMicroseconds(150); }
  return (float)s/N;
}
float uviFromReal(float mv){
  float span = max(50.0f, uv_mv_uvi11_real - uv_mv_zero_real);
  float u = (mv - uv_mv_zero_real) * (11.0f / span);
  if(u<0)u=0; if(u>12.5f)u=12.5f; return u;
}
float uviFromSim(float mv){
  float z=sim_uv_zero_mv, M=sim_uv_max_mv;
  if(M < z + 100.0f) M = z + 100.0f;
  if(mv<z) mv=z; if(mv>M) mv=M;
  float u=(mv - z) * (11.0f / (M - z));
  if(u<0)u=0; if(u>12.5f)u=12.5f; return u;
}

/*** ===== BUTTON (debounced) ===== ***/
struct Debounced{ int pin; bool stable=false, raw=false; uint32_t t=0; Debounced(int p):pin(p){} };
Debounced bSim(BTN_SIM);
bool updateBtn(Debounced& b){
  bool r=digitalRead(b.pin); uint32_t n=millis();
  if(r!=b.raw){ b.raw=r; b.t=n; }
  if(n-b.t>=DEBOUNCE_MS && b.stable!=b.raw){ b.stable=b.raw; return true; }
  return false;
}
bool pressed(const Debounced& b){ return b.stable; }

/*** ===== WIFI / TS ===== ***/
#if USE_WIFI
static uint32_t lastWifiTry=0, lastTsPush=0;
void wifiConnect(){
  if(WiFi.status()==WL_CONNECTED) return;
  Serial.print("WiFi: connecting to "); Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t start=millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-start<12000){ delay(250); }
  if(WiFi.status()==WL_CONNECTED){
    Serial.print("WiFi: connected, IP=");
    Serial.println(WiFi.localIP());
  }else{
    Serial.println("WiFi: connect timeout");
  }
}
bool thingSpeakUpdate(float t, float h, float hi, float uvi, float uvmv, int riskNum, int servoDeg, bool simOn){
  if(WiFi.status()!=WL_CONNECTED) return false;
  String url = String("http://api.thingspeak.com/update?api_key=") + TS_API_KEY;
  auto addField=[&](int idx, float v){ if(!isnan(v)) url += "&field"+String(idx)+"="+String(v,2); };
  addField(1,t); addField(2,h); addField(3,hi); addField(4,uvi); addField(5,uvmv);
  url += "&field6="+String(riskNum);
  url += "&field7="+String(servoDeg);
  url += "&field8="+String(simOn?1:0);

  HTTPClient http;
  http.begin(url);
  int code = http.GET();
  String body = (code>0) ? http.getString() : "";
  http.end();
  long entryId = body.toInt();
  if(code>0 && entryId>0){
    Serial.print("TS: ok id="); Serial.println(entryId);
    return true;
  }else{
    Serial.print("TS: fail code="); Serial.print(code); Serial.print(" body='"); Serial.print(body); Serial.println("'");
    return false;
  }
}
#endif

/*** ===== RESET ===== ***/
void resetState(){
  uvEventActive=false; highTempActive=false;
  uv_mv_ema=NAN; tp_mv_ema=NAN;
  T=RH=HI=NAN; UVI=0; UVmV=0;
  lastUV=lastDHT=0; lastPrint=0;
  currentDeg=0; prevDegBeforeUV=0;
  servoDetachQuiet(); setRGB(0,0,0);
  for(int i=0;i<16;i++){ analogReadMilliVolts(PIN_POT_UV); analogReadMilliVolts(PIN_POT_TEMP); delay(5); }
}

/*** ===== SETUP ===== ***/
void setup(){
  Serial.begin(115200); delay(300);
  ledcSetup(CH_R,5000,8); ledcSetup(CH_G,5000,8); ledcSetup(CH_B,5000,8);
  ledcAttachPin(PIN_R,CH_R); ledcAttachPin(PIN_G,CH_G); ledcAttachPin(PIN_B,CH_B);
  setRGB(0,0,0);
  pinMode(PIN_BUZZER, OUTPUT);
  if(PASSIVE_BUZZER){ ledcAttachPin(PIN_BUZZER, BUZZ_CH); ledcWriteTone(BUZZ_CH, 0); }
  else{ digitalWrite(PIN_BUZZER, BUZZ_ACTIVE_HIGH?LOW:HIGH); }
  dht.setup(PIN_DHT, DHTesp::DHT11);
  analogSetPinAttenuation(PIN_UV_REAL,  ADC_11db);
  analogSetPinAttenuation(PIN_POT_UV,   ADC_11db);
  analogSetPinAttenuation(PIN_POT_TEMP, ADC_11db);
  pinMode(BTN_SIM, INPUT_PULLDOWN);
  servoDetachQuiet();
  resetState();
  Serial.println("SIM OFF=real | SIM ON=pots. Minimal status every 1s.");
  #if USE_WIFI
    wifiConnect();
  #endif
}

void loop(){
  // Serial commands for SIM UV calibration
  if (Serial.available()){
    char c = Serial.read();
    if (c=='z'||c=='Z'){ sim_uv_zero_mv = UVmV; Serial.println("SIM UV zero set."); }
    if (c=='x'||c=='X'){ sim_uv_max_mv  = UVmV; Serial.println("SIM UV max set."); }
    if (c=='c'||c=='C'){ sim_uv_zero_mv = 0.0f; sim_uv_max_mv = 3300.0f; Serial.println("SIM UV cal cleared."); }
  }

  // SIM toggle (debounced)
  if (updateBtn(bSim) && pressed(bSim)){
    simEnabled = !simEnabled;
    if (uvEventActive){ softMoveTo(prevDegBeforeUV); uvEventActive = false; }
    uv_mv_ema = NAN; tp_mv_ema = NAN;   // fresh filters per mode
  }

  uint32_t now = millis();

  // ---- UV read ----
  if (now - lastUV >= UV_MS){
    lastUV = now;

    if (simEnabled){
      float mv_raw = avgMilliVolts(PIN_POT_UV, 24);
      float mv     = ema(mv_raw, uv_mv_ema, 0.25f);
      UVmV = mv; UVI = uviFromSim(mv);
    } else {
      float mv = avgMilliVolts(PIN_UV_REAL, 16);
      UVmV = mv; UVI = uviFromReal(mv);
    }

    // one-shot UV servo logic
    if (!uvEventActive && UVI >= UVI_TRIG_TH){
      prevDegBeforeUV = currentDeg;
      softMoveTo(180);
      uvEventActive = true;
    } else if (uvEventActive && UVI <= UVI_CLEAR_TH){
      softMoveTo(prevDegBeforeUV);
      uvEventActive = false;
    }
  }

  // ---- DHT / SIM TEMP read ----
  if (now - lastDHT >= DHT_MS){
    lastDHT = now;

    if (simEnabled){
      float mv_raw = avgMilliVolts(PIN_POT_TEMP, 24);
      float mv     = ema(mv_raw, tp_mv_ema, 0.25f);
      T  = 15.0f + (mv/3300.0f) * (45.0f - 15.0f);
      RH = 55.0f;
      // approximate heat index
      float Tf = T*9/5 + 32;
      float HI_f = -42.379 + 2.04901523*Tf + 10.14333127*RH
                 - 0.22475541*Tf*RH - 0.00683783*Tf*Tf
                 - 0.05481717*RH*RH + 0.00122874*Tf*Tf*RH
                 + 0.00085282*Tf*RH*RH - 0.00000199*Tf*Tf*RH*RH;
      HI = (HI_f - 32)*5/9;
    }else{
      TempAndHumidity th = dht.getTempAndHumidity();
      if (dht.getStatus()==DHTesp::ERROR_NONE && !isnan(th.temperature) && !isnan(th.humidity)){
        T = th.temperature; RH = th.humidity;
        HI = dht.computeHeatIndex(T, RH, false);
      }else{
        T = RH = HI = NAN;
      }
    }

    // 5-beep burst on upward crossing of 35°C
    if (!isnan(T)){
      if (!highTempActive && T >= TEMP_BEEP_TH){ beepBurst(); highTempActive = true; }
      else if (highTempActive && T <= TEMP_BEEP_CLR){ highTempActive = false; }
    }

    #if USE_WIFI
      // Wi-Fi reconnect + ThingSpeak push
      if (WiFi.status()!=WL_CONNECTED){
        static uint32_t lastWifiTry=0;
        if (now - lastWifiTry > 5000){ lastWifiTry = now; wifiConnect(); }
      }
      static uint32_t lastTsPush=0;
      if (WiFi.status()==WL_CONNECTED && now - lastTsPush >= TS_MIN_INTERVAL_MS){
        lastTsPush = now;
        Risk r_uv = riskFromUV(UVI);
        Risk r_hi = isnan(HI)?R_NORMAL:riskFromHI(HI);
        Risk r = (Risk)max((int)r_uv,(int)r_hi);
        thingSpeakUpdate(T, RH, HI, UVI, UVmV, (int)r, currentDeg, simEnabled);
      }
    #endif
  }

  // ---- RGB by risk (kept fresh) ----
  Risk r_uv = riskFromUV(UVI);
  Risk r_hi = isnan(HI)?R_NORMAL:riskFromHI(HI);
  Risk r = (Risk)max((int)r_uv,(int)r_hi);
  switch (r){
    case R_NORMAL:  setRGB(0,180,0);   break;
    case R_CAUTION: setRGB(200,130,0); break;
    case R_WARNING: setRGB(200,0,0);   break;
    case R_DANGER:  setRGB(220,0,180); break;
  }

  // ---- 1 s status line ----
  static uint32_t lastPrint=0;
  if (now - lastPrint >= 1000){
    lastPrint = now;
    Serial.printf("SIM=%s | Risk=%s | T=%s RH=%s HI=%s | UV=%.0f mV (UVI=%.2f) | Servo=%d%s\n",
      simEnabled?"ON":"OFF", riskText(r),
      isnan(T)?"N/A":String(T,1).c_str(),
      isnan(RH)?"N/A":String(RH,1).c_str(),
      isnan(HI)?"N/A":String(HI,1).c_str(),
      UVmV, UVI, currentDeg, uvEventActive?" (HOLD)":"");
  }
}