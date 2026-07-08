/*
 * Astra MID Reader - Fase 1 - Rev C
 * Monitora: tacômetro, velocidade, temperatura (PWM), injetor, bateria e combustível.
 * Pinos: TACHO=2 (INT0), SPEED=3 (INT1), TEMP_PWM=5, INJECTOR=8 (ICP1), BATT=A2, FUEL=A0.
 * Injetor usa Timer1 Input Capture (ICP1, pino 8) com alternância de borda por hardware.
 * Optoacoplador PC817 com pull-up: injetor ativo = pino LOW, repouso = HIGH.
 * Borda de DESCIDA = início do pulso, borda de SUBIDA = fim do pulso.
 * Período (descida a descida) e largura calculados inteiramente na ISR via Timer1.
 * Prescaler 8 → resolução 0,5µs por tick. Overflow (~32ms) tratado via contador de 32 bits.
 * TEMP_PWM lido com pulseIn a cada TEMP_INTERVAL ms para minimizar bloqueio no loop.
 * TACHO e SPEED com timeout de 2s para zerar leitura quando sem pulsos.
 * BUTTON_A=D10, BUTTON_B=D12: botões momentâneos que fecham com GND (INPUT_PULLUP).
 */

// -----------------------------------------------------------------------------
// Pinagem
// -----------------------------------------------------------------------------
#define PIN_TACHO       2
#define PIN_SPEED       3
#define PIN_TEMP_PWM    5
#define PIN_INJECTOR    8    // ICP1 — fixo no hardware do ATmega328P
#define PIN_BATT       A2
#define PIN_FUEL       A0
#define PIN_BUTTON_A   10   // Botão A — fecha com GND
#define PIN_BUTTON_B   12   // Botão B — fecha com GND

// -----------------------------------------------------------------------------
// Constantes
// -----------------------------------------------------------------------------
#define TIMEOUT_US          2000000UL
#define INJ_PULSE_MIN_US        200UL   // µs mínimo aceitável
#define INJ_PULSE_MAX_US     200000UL   // µs máximo — filtra injetor travado aberto
#define INJ_HIST                    8
#define LOOP_INTERVAL             200   // ms
#define TEMP_INTERVAL           10000UL
#define PULSEIN_TIMEOUT         30000UL
#define INJ_TIMEOUT_OVF          61UL   // ~2s em overflows do Timer1 (prescaler 8)

// -----------------------------------------------------------------------------
// Variáveis TACHO
// -----------------------------------------------------------------------------
volatile unsigned long tachoLastTime = 0;
volatile unsigned long tachoPeriod   = 0;
volatile unsigned int  tachoPulsos   = 0;

// -----------------------------------------------------------------------------
// Variáveis SPEED
// -----------------------------------------------------------------------------
volatile unsigned long speedLastTime = 0;
volatile unsigned long speedPeriod   = 0;
volatile unsigned int  speedPulsos   = 0;

// -----------------------------------------------------------------------------
// Variáveis INJECTOR (ICP1)
// -----------------------------------------------------------------------------
volatile uint32_t injOverflowCount  = 0;
volatile uint32_t injTicksFall      = 0;
volatile uint32_t injTicksPrevFall  = 0;
volatile unsigned long injPulseUs   = 0;
volatile unsigned long injPeriodUs  = 0;
volatile bool          injReady     = false;
volatile bool          injWaitingRise = false;
volatile unsigned long injHistory[INJ_HIST];
volatile uint8_t       injHistIdx   = 0;
volatile uint32_t      injLastValidOvf = 0;
float fator_consumo = 22700.0;

// -----------------------------------------------------------------------------
// Variáveis TEMP_PWM
// -----------------------------------------------------------------------------
float tempDutyCache    = -1.0;
float tempPeriodoCache =  0.0;
bool  tempValido       = false;

// -----------------------------------------------------------------------------
// ISR — TACHO (INT0, pino 2, FALLING)
// -----------------------------------------------------------------------------
void isrTacho() {
  unsigned long agora = micros();
  tachoPeriod   = agora - tachoLastTime;
  tachoLastTime = agora;
  tachoPulsos++;
}

// -----------------------------------------------------------------------------
// ISR — SPEED (INT1, pino 3, FALLING)
// -----------------------------------------------------------------------------
void isrSpeed() {
  unsigned long agora = micros();
  speedPeriod   = agora - speedLastTime;
  speedLastTime = agora;
  speedPulsos++;
}

// -----------------------------------------------------------------------------
// ISR — Timer1 Overflow
// -----------------------------------------------------------------------------
ISR(TIMER1_OVF_vect) {
  injOverflowCount++;
}

// -----------------------------------------------------------------------------
// ISR — Timer1 Input Capture
// PC817 com pull-up: repouso=HIGH, injetor ativo=LOW
//   borda de DESCIDA (ICES1=0) → início do pulso → salva ticks, alterna para subida
//   borda de SUBIDA  (ICES1=1) → fim do pulso    → calcula largura e período
// -----------------------------------------------------------------------------
ISR(TIMER1_CAPT_vect) {
  uint16_t captura  = ICR1;
  uint32_t ovfLocal = injOverflowCount;

  if ((TIFR1 & (1 << TOV1)) && captura < 0x8000) {
    ovfLocal++;
  }

  uint32_t ticksAgora = (ovfLocal << 16) | captura;

  if (!injWaitingRise) {
    injTicksPrevFall = injTicksFall;
    injTicksFall     = ticksAgora;
    injWaitingRise   = true;
    TCCR1B |= (1 << ICES1);
  } else {
    injWaitingRise = false;
    TCCR1B &= ~(1 << ICES1);

    uint32_t ticksPulso = ticksAgora - injTicksFall;
    unsigned long larguraUs = ticksPulso >> 1;

    if (larguraUs >= INJ_PULSE_MIN_US && larguraUs <= INJ_PULSE_MAX_US) {
      injPulseUs = larguraUs;

      if (injTicksPrevFall > 0) {
        uint32_t ticksPeriodo = injTicksFall - injTicksPrevFall;
        injPeriodUs = ticksPeriodo >> 1;
      }

      injHistory[injHistIdx % INJ_HIST] = larguraUs;
      injHistIdx++;
      injReady = true;
      injLastValidOvf = ovfLocal;
    }
  }
}

// -----------------------------------------------------------------------------
// Leitura TEMP_PWM
// -----------------------------------------------------------------------------
void atualizarTempPWM() {
  unsigned long high = pulseIn(PIN_TEMP_PWM, HIGH, PULSEIN_TIMEOUT);
  unsigned long low  = pulseIn(PIN_TEMP_PWM, LOW,  PULSEIN_TIMEOUT);

  if (high == 0 && low == 0) {
    tempDutyCache    = -1.0;
    tempPeriodoCache =  0.0;
    tempValido       = false;
    return;
  }

  unsigned long periodo = high + low;
  tempDutyCache    = (periodo > 0) ? (float)high / periodo * 100.0 : -1.0;
  tempPeriodoCache = (float)periodo;
  tempValido       = true;
}

// -----------------------------------------------------------------------------
// Setup
// -----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  pinMode(PIN_TACHO,    INPUT_PULLUP);
  pinMode(PIN_SPEED,    INPUT_PULLUP);
  pinMode(PIN_TEMP_PWM, INPUT_PULLUP);
  pinMode(PIN_INJECTOR, INPUT_PULLUP);
  pinMode(PIN_BUTTON_A, INPUT_PULLUP);
  pinMode(PIN_BUTTON_B, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(PIN_TACHO), isrTacho, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_SPEED), isrSpeed, FALLING);

  TCCR1A = 0;
  TCCR1B = 0;
  TCCR1B |= (1 << CS11);
  TIMSK1 |= (1 << ICIE1);
  TIMSK1 |= (1 << TOIE1);

  atualizarTempPWM();

  Serial.println("@");
  Serial.print(F("=== ASTRA MID READER - FASE 1 Rev C ==="));
  Serial.println(__FILE__);
  Serial.println(F("Aguardando sinais..."));
  Serial.println(F("--------------------------------------------------"));
  Serial.println(F("millis | BATT(V) | FUEL(V) | TACHO_P | SPEED_P | SPEED_PERIOD(us) | TEMP_DUTY(%) | INJ_PULSE_COUNT | BTN_A BTN_B"));
  Serial.println(F("--------------------------------------------------"));
}

// -----------------------------------------------------------------------------
// Loop
// -----------------------------------------------------------------------------
void loop() {
  static unsigned long ultimoLog  = 0;
  static unsigned long ultimoTemp = 0;
  unsigned long agora = millis();

  if (agora - ultimoTemp >= TEMP_INTERVAL) {
    ultimoTemp = agora;
    atualizarTempPWM();
  }

  if (agora - ultimoLog < LOOP_INTERVAL) return;
  ultimoLog = agora;

  // --- TACHO ---
  unsigned int  tachoP;
  unsigned long tachoT, tachoLast;
  noInterrupts();
    tachoP      = tachoPulsos;
    tachoT      = tachoPeriod;
    tachoLast   = tachoLastTime;
    tachoPulsos = 0;
  interrupts();
  if ((micros() - tachoLast) > TIMEOUT_US) tachoT = 0;

  // --- SPEED ---
  unsigned int  speedP;
  unsigned long speedT, speedLast;
  noInterrupts();
    speedP      = speedPulsos;
    speedT      = speedPeriod;
    speedLast   = speedLastTime;
    speedPulsos = 0;
  interrupts();
  if ((micros() - speedLast) > TIMEOUT_US) speedT = 0;

  // --- INJECTOR ---
  unsigned long injPulseLocal  = 0;
  unsigned long injPeriodLocal = 0;
  bool          injReadyLocal  = false;
  uint8_t       injCountLocal  = 0;
  unsigned long injHistLocal[INJ_HIST];
  uint32_t      injLastOvfLocal = 0;

  noInterrupts();
    injPulseLocal   = injPulseUs;
    injPeriodLocal  = injPeriodUs;
    injReadyLocal   = injReady;
    injReady        = false;
    injCountLocal   = injHistIdx;
    injHistIdx      = 0;
    for (uint8_t i = 0; i < min((uint8_t)INJ_HIST, injCountLocal); i++)
      injHistLocal[i] = injHistory[i];
    injLastOvfLocal = injLastValidOvf;
  interrupts();

  // --- BATT e FUEL ---
  float batt = analogRead(PIN_BATT) * (5.0 / 1023.0) * 5.7;
  float fuel = analogRead(PIN_FUEL) * (5.0 / 1023.0) * 5.7;

  // --- BOTÕES (0=aberto, 1=fechado) ---
  uint8_t btnA = (digitalRead(PIN_BUTTON_A) == LOW) ? 1 : 0;
  uint8_t btnB = (digitalRead(PIN_BUTTON_B) == LOW) ? 1 : 0;

  // --- LOG ---
  Serial.print(agora);
  Serial.print(F("|"));
  Serial.print(batt, 2);
  Serial.print(F("|"));
  Serial.print(fuel, 2);
  Serial.print(F("|"));
  Serial.print(tachoP);
  Serial.print(F("|"));
  Serial.print(speedP);
  Serial.print(F("|"));
  Serial.print(speedT);
  Serial.print(F("us "));
  Serial.print(speedT > 0 ? 1000000.0 / speedT : 0.0, 1);
  Serial.print(F("Hz|"));

  if (!tempValido) {
    Serial.print(F("-1.0%|"));
  } else {
    Serial.print(tempDutyCache, 1);
    Serial.print(F("%|"));
  }

  uint32_t injPulsosJanela = injCountLocal;
  Serial.print(injPulsosJanela);
  Serial.print(F("|"));
  Serial.print(btnA);
  Serial.println(btnB);
}