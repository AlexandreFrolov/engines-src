// Два мотора с редуктором (TT) через MX1508 + ESP32-S3-N16R8
// Управление по Wi-Fi: ESP32 создаёт точку доступа и отдаёт веб-страницу
// Arduino-ESP32 core 3.x
// Подстройка коэффициентов скорости каждого мотора с сохранением во flash
// Дальномер SRF05: при приближении к препятствию движение вперёд блокируется

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

// ---------- Wi-Fi ----------
const char* AP_SSID = "ESP32-Motors";
const char* AP_PASS = "12345678";          // не короче 8 символов
const IPAddress AP_IP(192, 168, 4, 1);

// ---------- Пины ----------
constexpr int M_A_IN1 = 15;
constexpr int M_A_IN2 = 16;
constexpr int M_B_IN1 = 17;
constexpr int M_B_IN2 = 18;

// ---------- Дальномер SRF05 ----------
constexpr int US_TRIG = 10;
constexpr int US_ECHO = 11;            // через делитель 1k/2k!
constexpr int STOP_CM = 25;            // ближе - стоп (с запасом на инерцию)
constexpr int RELEASE_CM = 30;         // гистерезис: снова разрешаем при > этого
constexpr uint32_t PING_MS = 60;       // период измерений
constexpr bool REQUIRE_SONAR = true;   // нет сигнала от датчика = ехать вперёд нельзя

// ---------- Параметры ШИМ ----------
constexpr uint32_t PWM_FREQ = 20000;  // 20 кГц
constexpr uint8_t  PWM_BITS = 8;      // 0..255
constexpr int      MIN_DUTY = 150;     // порог страгивания TT-мотора

// ---------- Безопасность ----------
// Если от телефона нет ни одного запроса дольше этого времени - моторы глушатся.
// Страница опрашивает состояние ~2 раза в секунду, так что при потере связи
// машинка остановится сама.
constexpr uint32_t FAILSAFE_MS = 2000;

Preferences prefs;
WebServer   server(80);
DNSServer   dns;

uint32_t lastClientMs = 0;

// =====================================================================
//                              МОТОР
// =====================================================================
class Motor {
public:
  Motor(int pinIn1, int pinIn2) : in1(pinIn1), in2(pinIn2) {}

  void begin() {
    ledcAttach(in1, PWM_FREQ, PWM_BITS);
    ledcAttach(in2, PWM_FREQ, PWM_BITS);
    coast();
  }

  // speed: -100..100 (%), знак = направление. Применяется сразу.
  void set(int speed) {
    target = constrain(speed, -100, 100);
    apply(target);
  }

  void coast() {                 // выбег
    target = 0;
    ledcWrite(in1, 0);
    ledcWrite(in2, 0);
    current = 0;
    braking = false;
  }

  void brake() {                 // торможение
    target = 0;
    ledcWrite(in1, 255);
    ledcWrite(in2, 255);
    current = 0;
    braking = true;
  }

  // Плавный разгон/торможение до target (шаг 1 % каждые stepDelayMs).
  // Неблокирующий: нужно вызывать update() из loop().
  void rampTo(int t) { target = constrain(t, -100, 100); }

  void update(uint32_t stepDelayMs = 15) {
    if (current == target) return;
    uint32_t now = millis();
    if (now - lastStep < stepDelayMs) return;
    lastStep = now;
    apply(current + ((target > current) ? 1 : -1));
  }

  // Коэффициент в процентах: 100 = без изменений, 110 = на 10 % быстрее
  void setTrimPct(int pct) { trim = constrain(pct, 50, 150); }
  int  trimPct() const     { return trim; }

  // Применить новый коэффициент к уже вращающемуся мотору
  void refresh() { if (current != 0) apply(current); }

  int  speed()       const { return current; }
  int  targetSpeed() const { return target; }
  bool isBraking()   const { return braking; }

private:
  void apply(int speed) {
    braking = false;
    if (speed == 0) {
      ledcWrite(in1, 0);
      ledcWrite(in2, 0);
      current = 0;
      return;
    }
    int duty = map(abs(speed), 1, 100, MIN_DUTY, 255);
    //duty = constrain(duty * trim / 100, 0, 255);   // поправка скорости
    duty = constrain(duty * trim / 100, MIN_DUTY, 255);

    if (speed > 0) {
      ledcWrite(in1, duty);
      ledcWrite(in2, 0);
    } else {
      ledcWrite(in1, 0);
      ledcWrite(in2, duty);
    }
    current = speed;
  }

  int in1, in2;
  int current = 0;
  int target  = 0;
  int trim    = 100;
  bool braking = false;
  uint32_t lastStep = 0;
};

Motor motorA(M_A_IN1, M_A_IN2);
Motor motorB(M_B_IN1, M_B_IN2);

// =====================================================================
//                    СОХРАНЕНИЕ / ЗАГРУЗКА КОЭФФИЦИЕНТОВ
// =====================================================================
bool savedFlag = false;   // показывает на странице, что коэффициенты записаны

void loadTrim() {
  prefs.begin("motors", true);                 // только чтение
  motorA.setTrimPct(prefs.getInt("trimA", 100));
  motorB.setTrimPct(prefs.getInt("trimB", 100));
  prefs.end();
}

void saveTrim() {
  prefs.begin("motors", false);
  prefs.putInt("trimA", motorA.trimPct());
  prefs.putInt("trimB", motorB.trimPct());
  prefs.end();
  savedFlag = true;
  Serial.println("Коэффициенты сохранены во flash");
}

// =====================================================================
//                                ДЕМО
// =====================================================================
// Конечный автомат вместо блокирующих delay(), чтобы веб-сервер не вис.
struct DemoStep { int a; int b; uint32_t holdMs; const char* name; };
const DemoStep DEMO[] = {
  { 80,  80, 2500, "вперёд" },
  { 80, -80, 2000, "разворот на месте" },
  {-80, -80, 2500, "назад" },
};
constexpr int DEMO_N = sizeof(DEMO) / sizeof(DEMO[0]);

bool     demoActive  = false;
int      demoIdx     = 0;
uint32_t demoT0      = 0;
bool     demoBraking = false;

void demoStart() {
  demoActive = true;
  demoIdx = 0;
  demoBraking = false;
  demoT0 = millis();
  motorA.rampTo(DEMO[0].a);
  motorB.rampTo(DEMO[0].b);
}

void demoStop() {
  demoActive = false;
}

void demoUpdate() {
  if (!demoActive) return;
  uint32_t now = millis();

  if (demoBraking) {                         // финал: торможение 0,5 с, затем выбег
    if (now - demoT0 >= 500) {
      motorA.coast(); motorB.coast();
      demoActive = false;
    }
    return;
  }
  if (now - demoT0 < DEMO[demoIdx].holdMs) return;

  demoIdx++;
  demoT0 = now;
  if (demoIdx >= DEMO_N) {
    motorA.brake(); motorB.brake();
    demoBraking = true;
  } else {
    motorA.rampTo(DEMO[demoIdx].a);
    motorB.rampTo(DEMO[demoIdx].b);
  }
}

// =====================================================================
//                            ДАЛЬНОМЕР
// =====================================================================
volatile uint32_t echoStartUs = 0;
volatile uint32_t echoWidthUs = 0;
volatile bool     echoReady   = false;

int      distCm       = -1;     // -1 = нет препятствия в зоне видимости
bool     obstacle     = false;  // препятствие ближе порога
bool     sonarOk      = false;  // датчик отвечает
uint8_t  nearCount    = 0;
uint8_t  missCount    = 0;
bool     pingPending  = false;
uint32_t lastPingMs   = 0;

void IRAM_ATTR echoISR() {
  if (digitalRead(US_ECHO)) {
    echoStartUs = micros();
  } else {
    echoWidthUs = micros() - echoStartUs;
    echoReady = true;
  }
}

void sonarBegin() {
  pinMode(US_TRIG, OUTPUT);
  digitalWrite(US_TRIG, LOW);
  pinMode(US_ECHO, INPUT);
  attachInterrupt(digitalPinToInterrupt(US_ECHO), echoISR, CHANGE);
}

void sonarProcess(int cm) {              // cm = -1: ничего нет
  distCm = cm;
  if (cm >= 0 && cm < STOP_CM) {
    if (nearCount < 255) nearCount++;
    if (nearCount >= 2) obstacle = true; // 2 подряд - фильтр от одиночных помех
  } else {
    nearCount = 0;
    if (cm < 0 || cm > RELEASE_CM) obstacle = false;
  }
}

void sonarUpdate() {
  uint32_t now = millis();

  if (echoReady) {
    noInterrupts();
    uint32_t w = echoWidthUs;
    echoReady = false;
    interrupts();
    pingPending = false;
    missCount = 0;
    sonarOk = true;
    // SRF05: при отсутствии цели эхо длится ~30 мс; 4 м ~ 23 мс
    sonarProcess(w > 23000 ? -1 : (int)(w / 58));
  }

  if (now - lastPingMs >= PING_MS) {
    lastPingMs = now;
    if (pingPending) {                   // на прошлый запуск эха не было
      if (++missCount >= 5) { sonarOk = false; sonarProcess(-1); }
    }
    digitalWrite(US_TRIG, HIGH);
    delayMicroseconds(10);
    digitalWrite(US_TRIG, LOW);
    pingPending = true;
  }
}

// Движение "вперёд" запрещено, если препятствие близко
// (или датчик не отвечает и стоит REQUIRE_SONAR)
bool forwardBlocked() {
  return obstacle || (REQUIRE_SONAR && !sonarOk);
}

// =====================================================================
//                          ВЕБ-СТРАНИЦА
// =====================================================================
static const char PAGE[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="ru"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>ESP32 Motors</title>
<style>
:root{--bg:#10151c;--card:#1a222d;--fg:#e8edf3;--mut:#8a97a8;--ac:#2f9bff;--ok:#2ecc71;--bad:#e74c3c;--warn:#f0a020}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;padding:12px;background:var(--bg);color:var(--fg);font:16px system-ui,sans-serif;max-width:480px;margin:auto}
h1{font-size:18px;margin:4px 0 10px;display:flex;justify-content:space-between;align-items:center}
#st{font-size:13px;padding:3px 10px;border-radius:12px;background:var(--ok);color:#000}
#st.bad{background:var(--bad);color:#fff}
.card{background:var(--card);border-radius:14px;padding:12px;margin-bottom:12px}
.card h2{font-size:13px;margin:0 0 8px;color:var(--mut);text-transform:uppercase;letter-spacing:.05em}
.row{display:flex;gap:8px;align-items:center}
.row>*{flex:1}
button{background:#2a3646;color:var(--fg);border:0;border-radius:10px;padding:12px 6px;font-size:16px;touch-action:manipulation}
button:active{background:var(--ac)}
button.stop{background:var(--bad)}
button.brake{background:var(--warn);color:#000}
button.ok{background:var(--ok);color:#000}
.pad{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin-top:10px}
.pad button{height:64px;font-size:26px;touch-action:none;user-select:none;-webkit-user-select:none}
input[type=range]{width:100%;height:36px;accent-color:var(--ac)}
.val{font-variant-numeric:tabular-nums;min-width:56px;text-align:right;flex:0 0 auto}
.lbl{flex:0 0 22px;font-weight:700}
.big{font-size:22px;font-variant-numeric:tabular-nums}
.mut{color:var(--mut);font-size:13px}
.trim{display:flex;align-items:center;gap:8px;margin:6px 0}
.trim button{flex:0 0 52px}
.trim .big{flex:1;text-align:center}
</style></head><body>

<h1>ESP32 Motors <span id="st">…</span></h1>

<div class="card">
  <h2>Состояние</h2>
  <div class="row">
    <div>Мотор A<div class="big" id="sa">0%</div><div class="mut" id="ma"></div></div>
    <div>Мотор B<div class="big" id="sb">0%</div><div class="mut" id="mb"></div></div>
  </div>
  <div class="mut" id="info" style="margin-top:8px"></div>
</div>

<div class="card">
  <h2>Джойстик (пока кнопка нажата)</h2>
  <div class="row"><span class="lbl" style="flex:0 0 auto">Скорость</span>
    <input type="range" id="spd" min="10" max="100" value="60">
    <span class="val" id="spdv">60%</span></div>
  <div class="pad">
    <span></span><button id="bF">▲</button><span></span>
    <button id="bL">◄</button><button class="stop" id="bS">■</button><button id="bR">►</button>
    <span></span><button id="bB">▼</button><span></span>
  </div>
</div>

<div class="card">
  <h2>Моторы по отдельности</h2>
  <div class="row"><span class="lbl">A</span><input type="range" id="ra" min="-100" max="100" value="0"><span class="val" id="rav">0%</span></div>
  <div class="row"><span class="lbl">B</span><input type="range" id="rb" min="-100" max="100" value="0"><span class="val" id="rbv">0%</span></div>
  <div class="row" style="margin-top:8px">
    <button class="stop" onclick="cmd('stop')">Стоп (выбег)</button>
    <button class="brake" onclick="cmd('brake')">Торможение</button>
  </div>
</div>

<div class="card">
  <h2>Коэффициенты скорости</h2>
  <div class="trim"><span class="lbl">A</span>
    <button onclick="trim('a',-1)">−</button><span class="big" id="ta">100%</span><button onclick="trim('a',1)">+</button></div>
  <div class="trim"><span class="lbl">B</span>
    <button onclick="trim('b',-1)">−</button><span class="big" id="tb">100%</span><button onclick="trim('b',1)">+</button></div>
  <div class="row" style="margin-top:8px">
    <button class="ok" onclick="cmd('save')">Сохранить во flash</button>
    <button onclick="cmd('reset')">Сбросить на 100%</button>
  </div>
  <div class="mut" id="saved" style="margin-top:6px"></div>
</div>

<div class="card">
  <div class="row"><button onclick="cmd('demo')">Демо</button></div>
</div>

<script>
const $ = id => document.getElementById(id);
let speed = 60;
const drag = {a:false, b:false};

async function api(path){
  try{
    const r = await fetch('/api/'+path, {cache:'no-store'});
    render(await r.json());
    $('st').textContent = 'онлайн'; $('st').className = '';
  }catch(e){
    $('st').textContent = 'нет связи'; $('st').className = 'bad';
  }
}
const cmd = c => api(c);

function modeText(sp, br){ return br ? 'торможение' : (sp>0 ? 'вперёд' : sp<0 ? 'назад' : 'стоит'); }

function render(j){
  $('sa').textContent = j.a + '%';  $('sb').textContent = j.b + '%';
  $('ma').textContent = modeText(j.a, j.brA); $('mb').textContent = modeText(j.b, j.brB);
  $('ta').textContent = j.ta + '%'; $('tb').textContent = j.tb + '%';
  $('info').textContent = (j.demo ? 'Демо выполняется. ' : '') +
      (j.sonar ? ('Дистанция: ' + (j.dist < 0 ? '> 4 м' : j.dist + ' см')) : 'Дальномер: нет сигнала') +
      (j.obs ? ' · ПРЕПЯТСТВИЕ!' : '') +
      ' · Клиентов Wi-Fi: ' + j.clients + ' · uptime ' + j.up + ' с';
  $('saved').textContent = j.saved ? 'Коэффициенты записаны во flash' : '';
  if(!drag.a){ $('ra').value = j.a; $('rav').textContent = j.a + '%'; }
  if(!drag.b){ $('rb').value = j.b; $('rbv').textContent = j.b + '%'; }
}

function trim(m, d){ api('trim?m='+m+'&d='+d); }

// --- ползунок общей скорости для джойстика
$('spd').addEventListener('input', e => { speed = +e.target.value; $('spdv').textContent = speed + '%'; });

// --- кнопки направления: едем, пока нажата
function hold(id, ka, kb){
  const el = $(id);
  el.addEventListener('pointerdown', e => {
    e.preventDefault(); el.setPointerCapture(e.pointerId);
    api('drive?a=' + ka*speed + '&b=' + kb*speed);
  });
  const stop = () => api('drive?a=0&b=0');
  el.addEventListener('pointerup', stop);
  el.addEventListener('pointercancel', stop);
}
hold('bF', 1, 1); hold('bB', -1, -1);
//hold('bL', -1, 1); hold('bR', 1, -1);
hold('bL', 1, -1); hold('bR', -1, 1);
$('bS').addEventListener('click', () => api('stop'));

// --- индивидуальные ползунки (с ограничением частоты запросов)
function slider(id, motor){
  const el = $(id); let last = 0;
  el.addEventListener('pointerdown', () => drag[motor] = true);
  el.addEventListener('input', () => {
    $(id+'v').textContent = el.value + '%';
    const now = Date.now();
    if(now - last > 80){ last = now; api(motor + '?v=' + el.value); }
  });
  el.addEventListener('change', () => { drag[motor] = false; api(motor + '?v=' + el.value); });
}
slider('ra', 'a'); slider('rb', 'b');

// --- опрос состояния (заодно служит "пульсом" для защиты от потери связи)
setInterval(() => api('state'), 400);
api('state');
</script>
</body></html>)rawliteral";

// =====================================================================
//                            API (JSON)
// =====================================================================
void touch() { lastClientMs = millis(); }

void sendState() {
  char buf[320];
  snprintf(buf, sizeof(buf),
    "{\"a\":%d,\"b\":%d,\"brA\":%s,\"brB\":%s,\"ta\":%d,\"tb\":%d,"
    "\"demo\":%s,\"saved\":%s,\"clients\":%d,\"up\":%lu,"
    "\"dist\":%d,\"obs\":%s,\"sonar\":%s}",
    motorA.speed(), motorB.speed(),
    motorA.isBraking() ? "true" : "false",
    motorB.isBraking() ? "true" : "false",
    motorA.trimPct(), motorB.trimPct(),
    demoActive ? "true" : "false",
    savedFlag ? "true" : "false",
    WiFi.softAPgetStationNum(),
    millis() / 1000UL,
    distCm,
    obstacle ? "true" : "false",
    sonarOk ? "true" : "false");
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", buf);
}

// Любая ручная команда прерывает демо
void manual() { touch(); demoStop(); }

void handleRoot() { touch(); server.send_P(200, "text/html; charset=utf-8", PAGE); }
void handleState() { touch(); sendState(); }

void handleA()     { manual(); motorA.set(server.arg("v").toInt()); sendState(); }
void handleB()     { manual(); motorB.set(server.arg("v").toInt()); sendState(); }
void handleM()     { manual(); int v = server.arg("v").toInt(); motorA.set(v); motorB.set(v); sendState(); }
//void handleDrive() { manual(); motorA.set(server.arg("a").toInt()); motorB.set(server.arg("b").toInt()); sendState(); }
void handleDrive() {
  manual();
  int a = server.arg("a").toInt();
  int b = server.arg("b").toInt();
  if (a == 0 && b == 0) {            // отпустили кнопку: стоп сразу
    motorA.coast(); motorB.coast();
  } else {                           // нажали: плавный разгон
    motorA.rampTo(a);
    motorB.rampTo(b);
  }
  sendState();
}
void handleStop()  { manual(); motorA.coast(); motorB.coast(); sendState(); }
void handleBrake() { manual(); motorA.brake(); motorB.brake(); sendState(); }

void handleTrim() {
  touch();
  Motor &m = (server.arg("m") == "b") ? motorB : motorA;
  if (server.hasArg("d")) m.setTrimPct(m.trimPct() + server.arg("d").toInt());  // шаг +-1 %
  else if (server.hasArg("v")) m.setTrimPct(server.arg("v").toInt());
  m.refresh();
  savedFlag = false;
  sendState();
}

void handleSave()  { touch(); saveTrim(); sendState(); }

void handleReset() {
  touch();
  motorA.setTrimPct(100); motorB.setTrimPct(100);
  motorA.refresh(); motorB.refresh();
  savedFlag = false;
  sendState();
}

void handleDemo()  { touch(); motorA.coast(); motorB.coast(); demoStart(); sendState(); }

// Captive portal: любой неизвестный адрес (в т.ч. проверки доступности
// интернета у Android/iOS/Windows) перенаправляем на главную страницу.
void handleNotFound() {
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
}

// =====================================================================
void setup() {
  Serial.begin(115200);
  motorA.begin();
  motorB.begin();
  sonarBegin();
  loadTrim();

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASS, 6, false, 4);   // канал 6, не скрытая, до 4 клиентов
  dns.start(53, "*", AP_IP);                    // все имена -> наш IP

  server.on("/",           handleRoot);
  server.on("/api/state",  handleState);
  server.on("/api/a",      handleA);
  server.on("/api/b",      handleB);
  server.on("/api/m",      handleM);
  server.on("/api/drive",  handleDrive);
  server.on("/api/stop",   handleStop);
  server.on("/api/brake",  handleBrake);
  server.on("/api/trim",   handleTrim);
  server.on("/api/save",   handleSave);
  server.on("/api/reset",  handleReset);
  server.on("/api/demo",   handleDemo);
  server.onNotFound(handleNotFound);
  server.begin();

  delay(300);
  Serial.printf("\nТочка доступа: %s  пароль: %s\n", AP_SSID, AP_PASS);
  Serial.printf("Страница управления: http://%s/\n", WiFi.softAPIP().toString().c_str());
  Serial.printf("Коэффициенты: A=%d%%  B=%d%%\n", motorA.trimPct(), motorB.trimPct());
}

void loop() {
  dns.processNextRequest();
  server.handleClient();

  sonarUpdate();

  // Защита от столкновения: целевое движение вперёд (сумма скоростей > 0)
  // при близком препятствии - немедленное торможение.
  // Разворот на месте (сумма = 0) и движение назад разрешены.
  if (forwardBlocked() && (motorA.targetSpeed() + motorB.targetSpeed() > 0)) {
    demoStop();
    motorA.brake();
    motorB.brake();
    Serial.println("Препятствие - моторы остановлены");
  }

  demoUpdate();
  motorA.update();
  motorB.update();

  // Защита: нет связи с телефоном - остановить моторы
  if (!demoActive &&
      (motorA.speed() != 0 || motorB.speed() != 0) &&
      millis() - lastClientMs > FAILSAFE_MS) {
    motorA.coast();
    motorB.coast();
    Serial.println("Нет связи с клиентом - моторы остановлены");
  }
}
