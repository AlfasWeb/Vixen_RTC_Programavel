#include <Adafruit_PCF8574.h>
#include <Wire.h>
#include <FastLED.h>
#include "RTClib.h"
#include "Programador.h"
#include "Commands.h"
#include <avr/wdt.h>

//====Programador=====
RTC_DS1307 rtc;

unsigned long ultimoRTC = 0;
const unsigned long intervaloRTC = 1000;
DateTime agora;

// Criar programador (construtor padrão)
Programador tarefa;
//====FIM Programador=====

#define QTDE_PCF  2      // ajuste conforme sua placa: número de PCF8574
#define QTDE_CI   1      // número de shift registers (74HC595)
#define PORTAS_RELE (QTDE_PCF * 8 + QTDE_CI * 8)

#define LED_PIN1 5
#define LED_PIN2 6
#define NUM_LEDS1 50
#define NUM_LEDS2 50

#define pinSH_CP 4
#define pinST_CP 3
#define pinDS    2
#define LED_STATUS 13

// Threshold para considerar o canal de relé como "ON" (proteção contra ruído)
// Aceitamos 0 = OFF, >= ONMIN = ON; valores intermediários serão IGNORADOS.
#define ONMIN 235

// Inverte lógica por módulo PCF se necessário (true = invertido)
bool invertPCF[QTDE_PCF] = { true, true };
bool pcfPresente[QTDE_PCF] = { false };

Adafruit_PCF8574 pcfs[QTDE_PCF];
CRGB leds1[NUM_LEDS1];
CRGB leds2[NUM_LEDS2];

// Buffer para relés (bits). Tamanho arredondado para bytes.
// layout: bytes 0..(QTDE_PCF-1) => PCF bytes (um por PCF), bytes QTDE_PCF.. => CI bytes
uint8_t bufferRele[(PORTAS_RELE + 7) / 8] = {0};

unsigned long ultimaRecepcao = 0;
const int TOTAL_CANAIS = PORTAS_RELE + (NUM_LEDS1 * 3) + (NUM_LEDS2 * 3);
bool recebendoFrame = false;
int canalAtual = 0;

// ---------- Função para escrever shift registers a partir do bufferRele ----------
void pushShiftRegistersFromBuffer() {
  // Escreve todos os bytes de CI (começando em bufferRele[QTDE_PCF])
  digitalWrite(pinST_CP, LOW);
  for (int nC = QTDE_CI - 1; nC >= 0; nC--) {
    uint8_t b = bufferRele[QTDE_PCF + nC]; // cada byte corresponde a 8 saídas do CI
    for (int nB = 7; nB >= 0; nB--) {
      digitalWrite(pinSH_CP, LOW);
      digitalWrite(pinDS, (b >> nB) & 0x01);
      digitalWrite(pinSH_CP, HIGH);
    }
  }
  digitalWrite(pinST_CP, HIGH);
}

// ---------- ciWrite compatível: atualiza bufferRele e empurra ----------
void ciWrite(byte pino, bool estado) {
  int byteIdx = QTDE_PCF + (pino / 8);
  int bitIdx  = pino % 8;
  if (estado) bufferRele[byteIdx] |= (1 << bitIdx);
  else        bufferRele[byteIdx] &= ~(1 << bitIdx);

  // Empurra todos os CIs (mantém ordem e múltiplos CIs)
  pushShiftRegistersFromBuffer();
}

// ---------- Liga todos os relés (PCF + todos os CIs) ----------
// Respeita invertPCF[] e assume 74HC595: HIGH = relé ligado
void ligarTodosRele() {
  // Atualiza buffer para todos 1
  int totalBits = PORTAS_RELE;
  for (int bit = 0; bit < totalBits; bit++) {
    int byteIdx = bit / 8;
    int bitIdx  = bit % 8;
    bufferRele[byteIdx] |= (1 << bitIdx);
  }

  // Escreve PCFs diretamente (respeitando lógica invertida)
  for (int p = 0; p < QTDE_PCF; p++) {
    for (int i = 0; i < 8; i++) {
      pcfs[p].digitalWrite(i, invertPCF[p] ? LOW : HIGH); // if invert, LOW = ligado
    }
  }

  // Empurra CIs (le o bufferRele[QTDE_PCF ..])
  pushShiftRegistersFromBuffer();
}

// ---------- Desliga todos os relés (PCF + CIs) ----------
void desligarTodosRele() {
  // Zera buffer
  int totalBytes = (PORTAS_RELE + 7) / 8;
  for (int b = 0; b < totalBytes; b++) bufferRele[b] = 0;

  // Escreve PCFs como desligado (respeitando invert)
  for (int p = 0; p < QTDE_PCF; p++) {
    for (int i = 0; i < 8; i++) {
      pcfs[p].digitalWrite(i, invertPCF[p] ? HIGH : LOW);
    }
  }

  // Empurra zero para CIs
  pushShiftRegistersFromBuffer();
}

// ---------- setRele: atualiza bit no buffer (não empurra CIs) ----------
void setRele(int canal, bool estado) {
  if (canal < 0 || canal >= PORTAS_RELE) return;
  int byteIdx = canal / 8;
  int bitIdx  = canal % 8;
  if (estado) bufferRele[byteIdx] |= (1 << bitIdx);
  else        bufferRele[byteIdx] &= ~(1 << bitIdx);
}

// ---------- atualizarRele: escreve bufferRele nos PCFs e CIs ----------
void atualizarRele() {
  for (int modulo = 0; modulo < QTDE_PCF; modulo++) {
    if (!pcfPresente[modulo]) continue;
    uint8_t byteFinal = bufferRele[modulo];
    if (invertPCF[modulo]) byteFinal = ~byteFinal;
    // método disponível em Adafruit_PCF8574:
    pcfs[modulo].digitalWriteByte(byteFinal);
  }
  pushShiftRegistersFromBuffer();
}

// ---------------- Efeitos (não chamam FastLED.show()) ----------------
// Variáveis internas dos efeitos
static uint8_t effectIndex = 0;
static unsigned long lastEffectChange = 0;
const unsigned long EFFECT_INTERVAL = 20000UL; // 20s

// Fade
static int fade_v = 0;
static int fade_dir = 1;
static unsigned long fade_lastStep = 0;

// Rainbow
static uint16_t rb_hue = 0;
static unsigned long rb_lastStep = 0;

// Twinkle
static unsigned long tw_lastUpdate = 0;

// Meteor
static unsigned long mr_lastStep = 0;
static int mr_pos = 0;
static int mr_dir = 1;
const int MR_SIZE = 10;
const int MR_SPEED = 30;

// Pulse
static unsigned long pw_lastStep = 0;
static uint8_t pw_phase = 0;

// Fire flicker
static unsigned long ff_lastStep = 0;
static byte heat1[NUM_LEDS1];
static byte heat2[NUM_LEDS2];

// Lightning (raio com fases)
static bool lt_active = false;
static int lt_phase = 0;
static unsigned long lt_nextEvent = 0;
static uint8_t lt_brightness = 0;

void effectFade_update() {
  unsigned long now = millis();
  if (now - fade_lastStep < 30) return;
  fade_lastStep = now;
  fade_v += fade_dir * 5;
  if (fade_v >= 255 || fade_v <= 0) fade_dir *= -1;
  for (int i = 0; i < NUM_LEDS1; i++) leds1[i] = CRGB(fade_v, 0, 255 - fade_v);
  for (int i = 0; i < NUM_LEDS2; i++) leds2[i] = CRGB(fade_v, 0, 255 - fade_v);
}

void effectRainbow_update() {
  unsigned long now = millis();
  if (now - rb_lastStep < 20) return;
  rb_lastStep = now;
  rb_hue += 2;
  for (int i = 0; i < NUM_LEDS1; i++) leds1[i] = CHSV(rb_hue + i * 4, 255, 255);
  for (int i = 0; i < NUM_LEDS2; i++) leds2[i] = CHSV(rb_hue + i * 4, 255, 255);
}

void effectTwinkle_update() {
  unsigned long now = millis();
  if (now - tw_lastUpdate < 150) return;
  tw_lastUpdate = now;
  for (int i = 0; i < NUM_LEDS1; i++) leds1[i] = CRGB(random8(), random8(), random8());
  for (int i = 0; i < NUM_LEDS2; i++) leds2[i] = CRGB(random8(), random8(), random8());
}

void effectPulse_update() {
  unsigned long now = millis();
  if (now - pw_lastStep < 25) return;
  pw_lastStep = now;
  pw_phase++;
  for (int i = 0; i < NUM_LEDS1; i++) {
    uint8_t wave = sin8(pw_phase + i * 4);
    leds1[i] = CHSV(128, 255, wave);
  }
  for (int i = 0; i < NUM_LEDS2; i++) {
    uint8_t wave = sin8(pw_phase + i * 5);
    leds2[i] = CHSV(32, 255, wave);
  }
}

void effectMeteor_update() {
  unsigned long now = millis();
  if (now - mr_lastStep < MR_SPEED) return;
  mr_lastStep = now;
  for (int i = 0; i < NUM_LEDS1; i++) leds1[i].fadeToBlackBy(40);
  for (int i = 0; i < NUM_LEDS2; i++) leds2[i].fadeToBlackBy(40);
  for (int i = 0; i < MR_SIZE; i++) {
    int p1 = mr_pos - i;
    if (p1 >= 0 && p1 < NUM_LEDS1) leds1[p1] = CHSV((0 + i * 8) & 255, 255, 255);
    if (p1 >= 0 && p1 < NUM_LEDS2) leds2[p1] = CHSV((160 + i * 8) & 255, 255, 255);
  }
  mr_pos += mr_dir;
  if (mr_pos >= NUM_LEDS1 - 1 || mr_pos <= 0) mr_dir = -mr_dir;
}

void runLocalEffects() {
  unsigned long now = millis();
  if (now - lastEffectChange >= EFFECT_INTERVAL) {
    effectIndex = (effectIndex + 1) % 5;
    lastEffectChange = now;
  }

  switch (effectIndex) {
    case 0: effectRainbow_update(); break;
    case 1: effectFade_update(); break;
    case 2: effectTwinkle_update(); break;
    case 3: effectPulse_update(); break;
    case 4: effectMeteor_update(); break;
  }
}

// ---------------- util ----------------
extern int __heap_start, *__brkval;
int freeMemory() {
  int v;
  return (int)&v - (__brkval == 0 ? (int)&__heap_start : (int)__brkval);
}
// ===== Setup =====
void setup() {
  // DESABILITA watchdog durante setup
  wdt_disable();

  pinMode(8, INPUT_PULLUP);   // botão Modo AT
  delay(50);

  bool modoAT = (digitalRead(8) == LOW);   // pressionado no boot?

  if (modoAT) {
    // ----------- MODO AT -----------
    Serial.begin(9600);
    Serial.println(F("=== MODO AT ==="));
    Serial.println(F("Envie comandos #...;"));
    
    // Inicializações básicas mínimas para poder gravar/programar
    Wire.begin();
    rtc.begin();

    // Carrega tarefas para permitir edição
    carregarProgramacoesEEPROM(tarefa);

    // Mantém sempre ativo (não entra em Vixen nem Standby)
    while (true) {
      wdt_reset();
      while (Serial.available()) {
        byte b = Serial.read();
        processaByteSerial(b, tarefa, rtc);
      }
    }
  }

  // ----------- MODO NORMAL (VIXEN) -----------
  Serial.begin(57600);
  Serial.println(F("Modo normal (Vixen) iniciado"));
  
  pinMode(LED_STATUS, OUTPUT);
  pinMode(pinSH_CP, OUTPUT);
  pinMode(pinST_CP, OUTPUT);
  pinMode(pinDS, OUTPUT);

  // inicia I2C
  Wire.begin();
  Wire.setClock(100000);

  // inicia PCF8574s com verificação
  for (int i = 0; i < QTDE_PCF; i++) {
      uint8_t endereco = 0x20 + i;
      Wire.beginTransmission(endereco);
      uint8_t err = Wire.endTransmission();
      if (err == 0) {
        pcfPresente[i] = true;                     // <-- agora grava no global ✔
        pcfs[i].begin(endereco, &Wire);
        for (uint8_t p = 0; p < 8; p++) {
          pcfs[i].pinMode(p, OUTPUT);
          pcfs[i].digitalWrite(p, invertPCF[i] ? HIGH : LOW);
        }
        Serial.print(F("PCF iniciado em 0x"));
        Serial.println(endereco, HEX);
      } else {
        pcfPresente[i] = false;                    // <-- global ✔
        Serial.print(F("PCF não detectado em 0x"));
        Serial.println(endereco, HEX);
      }
  }


  // limpa shift register (usa bufferRele e empurra)
  int totalBytes = (PORTAS_RELE + 7) / 8;
  for (int b = 0; b < totalBytes; b++) bufferRele[b] = 0;
  pushShiftRegistersFromBuffer();

  // LEDs
  FastLED.addLeds<WS2811, LED_PIN1, BRG>(leds1, NUM_LEDS1);
  FastLED.addLeds<WS2811, LED_PIN2, BRG>(leds2, NUM_LEDS2);
  FastLED.setBrightness(180);
  fill_solid(leds1, NUM_LEDS1, CRGB::Black);
  fill_solid(leds2, NUM_LEDS2, CRGB::Black);
  FastLED.show();

  // RTC
  rtc.begin();
  if (!rtc.isrunning()) {
    Serial.println("DS1307 não está rodando, ajustando data/hora...");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }
  agora = rtc.now();

  // Carrega programações da EEPROM (Commands.cpp)
  carregarProgramacoesEEPROM(tarefa);
  //Inicializador de efeitos
  lastEffectChange = millis();
  
  Serial.print(F("RAM livre: "));
  Serial.println(freeMemory());
  
  ultimaRecepcao = millis();   // impede standby imediato
  wdt_enable(WDTO_2S);
  Serial.println(F("🚀 Sistema iniciado. Aguardando '$'..."));
}

// ===== Loop principal =====
void loop() {
  wdt_reset(); // alimenta watchdog
  static bool standbyAtivo = false;

  // Processa bytes da Serial — pode ser Vixen ($) ou comando (terminador ';')
  while (Serial.available()) {
    byte valor = Serial.read();

    // Início de frame Vixen
    if (valor == '$') {
      recebendoFrame = true;
      canalAtual = 0;
      ultimaRecepcao = millis();
      continue;
    }

    // Se estamos em frame Vixen, processa canais
    if (recebendoFrame) {
      if (canalAtual < PORTAS_RELE) {
        // FILTRO: aceita somente 0 (OFF) ou >= ONMIN (ON). Valores intermediários são ignorados.
        if (valor == 0) {
          setRele(canalAtual, false);
        } else if (valor >= ONMIN) {
          setRele(canalAtual, true);
        } else {
          // IGNORA: não altera o bufferRele para esse canal (proteção contra sujeira)
          // (mantém o estado anterior)
        }
      }
      else if (canalAtual < PORTAS_RELE + NUM_LEDS1 * 3) {
        int idx = canalAtual - PORTAS_RELE;
        int led = idx / 3;
        int comp = idx % 3;
        if (led < NUM_LEDS1) {
          if (comp == 0) leds1[led].r = valor;
          else if (comp == 1) leds1[led].g = valor;
          else leds1[led].b = valor;
        }
      }
      else if (canalAtual < PORTAS_RELE + (NUM_LEDS1 + NUM_LEDS2) * 3) {
        int idx = canalAtual - PORTAS_RELE - NUM_LEDS1 * 3;
        int led = idx / 3;
        int comp = idx % 3;
        if (led < NUM_LEDS2) {
          if (comp == 0) leds2[led].r = valor;
          else if (comp == 1) leds2[led].g = valor;
          else leds2[led].b = valor;
        }
      }

      canalAtual++;
      ultimaRecepcao = millis();

      // fim do frame
      if (canalAtual >= TOTAL_CANAIS) {
        recebendoFrame = false;
        canalAtual = 0;
        break;
      }
      continue;
    }
  } // fim while Serial.available

  // Se entrou em frame e não recebeu bytes por timeout -> cancela frame
  if (recebendoFrame && millis() - ultimaRecepcao > 100) {
    recebendoFrame = false;
    canalAtual = 0;
  }

  // ================================
  // ENTRADA EM STANDBY (sem Vixen por >2s)
  // ================================
  unsigned long t = millis();

  if (millis() - ultimaRecepcao > 2000) {
    tarefa.atualizar(agora.dayOfTheWeek(), agora.hour(), agora.minute());
    bool ativo = tarefa.getEstadoAtual();

    if (t - ultimoRTC >= intervaloRTC) {
      ultimoRTC = t;
      agora = rtc.now();
      // Imprime hora em standby
      Serial.print(F("[STANDBY] "));
      Serial.print(agora.hour());
      Serial.print(':');
      Serial.print(agora.minute());
      Serial.print(':');
      Serial.println(agora.second());
      Serial.print("Status: ");
      Serial.println((ativo)? "Ativado":"Desativado");
    }

    if (ativo) {
      if (!standbyAtivo) {
        standbyAtivo = true;
        ligarTodosRele();
        Serial.println(F("Entrou em STANDBY: ligarTodosRele() executado"));
      }
      runLocalEffects();
      atualizarRele();
      FastLED.show();
      digitalWrite(LED_STATUS, !digitalRead(LED_STATUS));
    } else {
      // Se não está ativo, garantir que tudo permaneça desligado
      if (standbyAtivo) {
        standbyAtivo = false;
        desligarTodosRele();
        fill_solid(leds1, NUM_LEDS1, CRGB::Black);
        fill_solid(leds2, NUM_LEDS2, CRGB::Black);
        FastLED.show();
        Serial.println(F("PROGRAMACAO INATIVA: tudo desligado"));
      }
    }
    return;
  }

  // ================================
  // SAIU DO STANDBY → ATUALIZA NORMAL
  // ================================
  if (standbyAtivo) {
    standbyAtivo = false;
    // Opcional: ao sair do standby, reescrever módulos com o bufferRele atual
    atualizarRele();
  }

  // ================================
  // OPERAÇÃO NORMAL (atualiza relés/leds)
  // ================================
  static unsigned long lastUpdate = 0;
  if (!recebendoFrame && millis() - lastUpdate > 0) {
    atualizarRele();  // escreve os reles
    FastLED.show();   // ATUALIZA OS LEDS — SOMENTE AQUI!
    digitalWrite(LED_STATUS, !digitalRead(LED_STATUS));
    lastUpdate = millis();
  }
}