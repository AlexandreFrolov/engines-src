// Управление двумя моторами с редуктором (TT, "жёлтые") через MX1508
// Плата: ESP32-S3-DevKitC-1-N16R8, Arduino-ESP32 core 3.x

#include <Arduino.h>

// ---------- Пины ----------
constexpr int M_A_IN1 = 15;
constexpr int M_A_IN2 = 16;
constexpr int M_B_IN1 = 17;
constexpr int M_B_IN2 = 18;

// ---------- Параметры ШИМ ----------
constexpr uint32_t PWM_FREQ = 20000;  // 20 кГц, чтобы не пищало
constexpr uint8_t  PWM_BITS = 8;      // 0..255
constexpr int      MIN_DUTY = 70;     // ниже этого значения TT-мотор обычно не трогается

class Motor {
public:
  Motor(int pinIn1, int pinIn2) : in1(pinIn1), in2(pinIn2) {}

  void begin() {
    ledcAttach(in1, PWM_FREQ, PWM_BITS);
    ledcAttach(in2, PWM_FREQ, PWM_BITS);
    coast();
  }

  // speed: -100..100 (%), знак задаёт направление
  void set(int speed) {
    speed = constrain(speed, -100, 100);
    if (speed == 0) { coast(); return; }

    int duty = map(abs(speed), 1, 100, MIN_DUTY, 255);
    if (speed > 0) {          // вперёд: ШИМ на IN1, IN2 = 0
      ledcWrite(in1, duty);
      ledcWrite(in2, 0);
    } else {                  // назад: IN1 = 0, ШИМ на IN2
      ledcWrite(in1, 0);
      ledcWrite(in2, duty);
    }
    current = speed;
  }

  // Свободный выбег: оба входа в 0
  void coast() {
    ledcWrite(in1, 0);
    ledcWrite(in2, 0);
    current = 0;
  }

  // Электрическое торможение: оба входа в HIGH
  void brake() {
    ledcWrite(in1, 255);
    ledcWrite(in2, 255);
    current = 0;
  }

  // Плавный разгон/торможение до целевой скорости
  void rampTo(int target, int stepDelayMs = 15) {
    target = constrain(target, -100, 100);
    while (current != target) {
      current += (target > current) ? 1 : -1;
      set(current);
      delay(stepDelayMs);
    }
  }

  int speed() const { return current; }

private:
  int in1, in2;
  int current = 0;
};

Motor motorA(M_A_IN1, M_A_IN2);
Motor motorB(M_B_IN1, M_B_IN2);

// ---------- Управление через Serial ----------
// Команды (с новой строки):
//   a 60    мотор A вперёд на 60 %
//   b -40   мотор B назад на 40 %
//   m 50    оба вперёд на 50 %
//   s       стоп (выбег)
//   x       торможение
//   d       демо-последовательность
void handleSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.isEmpty()) return;

  char cmd = line.charAt(0);
  int val = line.length() > 1 ? line.substring(1).toInt() : 0;

  switch (cmd) {
    case 'a': motorA.set(val); break;
    case 'b': motorB.set(val); break;
    case 'm': motorA.set(val); motorB.set(val); break;
    case 's': motorA.coast(); motorB.coast(); break;
    case 'x': motorA.brake(); motorB.brake(); break;
    case 'd': demo(); break;
    default:  Serial.println("Команды: a N, b N, m N, s, x, d  (N = -100..100)"); return;
  }
  Serial.printf("A=%d%%  B=%d%%\n", motorA.speed(), motorB.speed());
}

void demo() {
  Serial.println("Демо: вперёд");
  motorA.rampTo(70); motorB.rampTo(70);
  delay(2000);

  Serial.println("Демо: разворот на месте");
  motorA.rampTo(60); motorB.rampTo(-60);
  delay(1500);

  Serial.println("Демо: назад");
  motorA.rampTo(-70); motorB.rampTo(-70);
  delay(2000);

  Serial.println("Демо: торможение");
  motorA.brake(); motorB.brake();
  delay(500);
  motorA.coast(); motorB.coast();
}

void setup() {
  Serial.begin(115200);
  motorA.begin();
  motorB.begin();
  delay(500);
  Serial.println("MX1508 + ESP32-S3 готов. Команды: a N, b N, m N, s, x, d");
}

void loop() {
  handleSerial();
}