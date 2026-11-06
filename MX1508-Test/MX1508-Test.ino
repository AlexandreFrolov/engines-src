// Два мотора с редуктором (TT) через MX1508 + ESP32-S3-N16R8
// Arduino-ESP32 core 3.x
// Есть подстройка коэффициентов скорости каждого мотора с сохранением во flash

#include <Arduino.h>
#include <Preferences.h>

// ---------- Пины ----------
constexpr int M_A_IN1 = 15;
constexpr int M_A_IN2 = 16;
constexpr int M_B_IN1 = 17;
constexpr int M_B_IN2 = 18;

// ---------- Параметры ШИМ ----------
constexpr uint32_t PWM_FREQ = 20000;  // 20 кГц
constexpr uint8_t  PWM_BITS = 8;      // 0..255
constexpr int      MIN_DUTY = 70;     // порог страгивания TT-мотора

Preferences prefs;

class Motor {
public:
  Motor(int pinIn1, int pinIn2) : in1(pinIn1), in2(pinIn2) {}

  void begin() {
    ledcAttach(in1, PWM_FREQ, PWM_BITS);
    ledcAttach(in2, PWM_FREQ, PWM_BITS);
    coast();
  }

  // speed: -100..100 (%), знак = направление
  void set(int speed) {
    speed = constrain(speed, -100, 100);
    if (speed == 0) { coast(); return; }

    int duty = map(abs(speed), 1, 100, MIN_DUTY, 255);
    duty = constrain(duty * trim / 100, 0, 255);   // поправка скорости

    if (speed > 0) {
      ledcWrite(in1, duty);
      ledcWrite(in2, 0);
    } else {
      ledcWrite(in1, 0);
      ledcWrite(in2, duty);
    }
    current = speed;
  }

  void coast() {                 // выбег
    ledcWrite(in1, 0);
    ledcWrite(in2, 0);
    current = 0;
  }

  void brake() {                 // торможение
    ledcWrite(in1, 255);
    ledcWrite(in2, 255);
    current = 0;
  }

  void rampTo(int target, int stepDelayMs = 15) {
    target = constrain(target, -100, 100);
    while (current != target) {
      current += (target > current) ? 1 : -1;
      set(current);
      delay(stepDelayMs);
    }
  }

  // Коэффициент в процентах: 100 = без изменений, 110 = на 10 % быстрее
  void setTrimPct(int pct) { trim = constrain(pct, 50, 150); }
  int  trimPct() const     { return trim; }

  // Применить новый коэффициент к уже вращающемуся мотору
  void refresh() { if (current != 0) set(current); }

  int speed() const { return current; }

private:
  int in1, in2;
  int current = 0;
  int trim = 100;
};

Motor motorA(M_A_IN1, M_A_IN2);
Motor motorB(M_B_IN1, M_B_IN2);

// ---------- Сохранение / загрузка коэффициентов ----------
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
  Serial.println("Коэффициенты сохранены во flash");
}

void printStatus() {
  Serial.printf("Скорость: A=%d%%  B=%d%%  |  Коэффициенты: A=%d%%  B=%d%%\n",
                motorA.speed(), motorB.speed(),
                motorA.trimPct(), motorB.trimPct());
}

void printHelp() {
  Serial.println(F(
    "\nКоманды (конец строки: Newline):\n"
    "  a N      мотор A, скорость -100..100\n"
    "  b N      мотор B, скорость -100..100\n"
    "  m N      оба мотора, скорость -100..100\n"
    "  s        стоп (выбег)\n"
    "  x        торможение\n"
    "  ta N     коэффициент A в %, 50..150 (ta + / ta - : шаг 1 %)\n"
    "  tb N     коэффициент B в % (tb + / tb - : шаг 1 %)\n"
    "  w        сохранить коэффициенты во flash\n"
    "  r        сбросить коэффициенты на 100 %\n"
    "  p        показать состояние\n"
    "  d        демо\n"
    "  h        эта справка\n"));
}

void demo() {
  Serial.println("Демо: вперёд");
  motorA.rampTo(80); motorB.rampTo(80);
  delay(2000);

  Serial.println("Демо: разворот на месте");
  motorA.rampTo(80); motorB.rampTo(-80);
  delay(1500);

  Serial.println("Демо: назад");
  motorA.rampTo(-80); motorB.rampTo(-80);
  delay(2000);

  motorA.brake(); motorB.brake();
  delay(500);
  motorA.coast(); motorB.coast();
}

// ---------- Разбор команд ----------
void adjustTrim(Motor &m, const String &arg) {
  if (arg == "+")            m.setTrimPct(m.trimPct() + 1);
  else if (arg == "-")       m.setTrimPct(m.trimPct() - 1);
  else if (arg.length() > 0) m.setTrimPct(arg.toInt());
  m.refresh();
}

void handleSerial() {
  if (!Serial.available()) return;

  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.isEmpty()) return;

  int sp = line.indexOf(' ');
  String cmd = (sp < 0) ? line : line.substring(0, sp);
  String arg = (sp < 0) ? ""  : line.substring(sp + 1);
  cmd.toLowerCase();
  arg.trim();
  int val = arg.toInt();

  if      (cmd == "a")  motorA.set(val);
  else if (cmd == "b")  motorB.set(val);
  else if (cmd == "m")  { motorA.set(val); motorB.set(val); }
  else if (cmd == "s")  { motorA.coast(); motorB.coast(); }
  else if (cmd == "x")  { motorA.brake(); motorB.brake(); }
  else if (cmd == "ta") adjustTrim(motorA, arg);
  else if (cmd == "tb") adjustTrim(motorB, arg);
  else if (cmd == "w")  saveTrim();
  else if (cmd == "r")  { motorA.setTrimPct(100); motorB.setTrimPct(100);
                          motorA.refresh(); motorB.refresh(); }
  else if (cmd == "d")  demo();
  else if (cmd == "h")  { printHelp(); return; }
  else if (cmd != "p")  { Serial.println("Неизвестная команда, h = справка"); return; }

  printStatus();
}

void setup() {
  Serial.begin(115200);
  motorA.begin();
  motorB.begin();
  loadTrim();
  delay(500);
  Serial.println("MX1508 + ESP32-S3 готов");
  printHelp();
  printStatus();
}

void loop() {
  handleSerial();
}