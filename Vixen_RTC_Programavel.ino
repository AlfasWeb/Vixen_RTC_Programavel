#include <Adafruit_PCF8574.h>
#include <Wire.h>
#include <FastLED.h>
#include "RTClib.h"
#include "Programador.h"
#include "Commands.h"

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
  // PCFs: bytes 0..(QTDE_PCF-1) — cada bit mapeia pino 0..7
  for (int j = 0; j < QTDE_PCF * 8; j++) {
    int modulo = j / 8;
    int pino   = j % 8;
    bool estado = (bufferRele[modulo] >> pino) & 0x01;
    // Respeita invertPCF: se invertido, o pino físico recebe !estado
    pcfs[modulo].digitalWrite(pino, invertPCF[modulo] ? !estado : estado);
  }

  // CIs: já guardados em bufferRele[QTDE_PCF .. QTDE_PCF+QTDE_CI-1]
  pushShiftRegistersFromBuffer();
}

// ----------------------------------------------------
// Variáveis internas dos efeitos
// ----------------------------------------------------
  static uint8_t effectIndex = 0;
  static unsigned long lastEffectChange = 0;
  const unsigned long EFFECT_INTERVAL = 20000; //a cada 20 segundos ele troca o efeito

  // ==== Variáveis do efeito Fade ====
  static int fade_v = 0;
  static int fade_dir = 1;
  static int fade_relayIndex = 0;
  static unsigned long fade_lastStep = 0;

  // ==== Variáveis do Rainbow ====
  static uint16_t rb_hue = 0;
  static int rb_step = 0;
  static int rb_dir = 1;
  static unsigned long rb_lastStep = 0;

  // ==== Variáveis Twinkle ====
  static unsigned long tw_lastUpdate = 0;
  static unsigned long tw_relayTimers[32] = {0};
  static bool tw_relayStates[32] = {false};
  static unsigned long tw_nextUpdate[32] = {0};

  // ==== Variáveis Meteor Rain ====
static unsigned long mr_lastStep = 0;
static int mr_pos = 0;
static int mr_dir = 1;  // 1 = vai para frente, -1 = volta
const int MR_SIZE = 10; // comprimento do meteoro
const int MR_SPEED = 30;

// ==== Variáveis Pulse Waves ====
static unsigned long pw_lastStep = 0;
static uint8_t pw_phase = 0;

// ==== Variáveis Fire Flicker ====
static unsigned long ff_lastStep = 0;
static byte heat1[NUM_LEDS1];
static byte heat2[NUM_LEDS2];

// ==== Variáveis Lightning ====
static unsigned long lt_nextFlash = 0;
static bool lt_isFlashing = false;
static unsigned long lt_flashEndTime = 0;

void effectLightning() {
    unsigned long now = millis();

    // Se não está piscando, espera até o próximo raio
    if (!lt_isFlashing) {
        if (now > lt_nextFlash) {
            lt_isFlashing = true;
            lt_flashEndTime = now + random(50, 150);  // duração do flash
        } else {
            // Mantém tudo apagado
            fill_solid(leds1, NUM_LEDS1, CRGB::Black);
            fill_solid(leds2, NUM_LEDS2, CRGB::Black);
            return;
        }
    }

    // Está piscando (clarão)
    if (now < lt_flashEndTime) {
        CRGB flashColor = CRGB(255, 255, 255);

        for (int i = 0; i < NUM_LEDS1; i++)
            leds1[i] = flashColor;

        for (int i = 0; i < NUM_LEDS2; i++)
            leds2[i] = flashColor;

    } else {
        // Flash terminou
        lt_isFlashing = false;
        lt_nextFlash = now + random(500, 3000); // tempo até próximo raio (0.5–3s)

        // apaga após o flash
        fill_solid(leds1, NUM_LEDS1, CRGB::Black);
        fill_solid(leds2, NUM_LEDS2, CRGB::Black);
    }
}

void effectFire() {
    unsigned long now = millis();
    if (now - ff_lastStep < 40) return;  
    ff_lastStep = now;

    // ---- Tira 1 ----
    for (int i = 0; i < NUM_LEDS1; i++) {
        heat1[i] = qsub8(heat1[i], random8(0, 20));   // perde calor
        heat1[i] = qadd8(heat1[i], random8(0, 30));   // ganha calor
        leds1[i] = HeatColor(heat1[i]);               // converte calor → cor
    }

    // ---- Tira 2 ----
    for (int i = 0; i < NUM_LEDS2; i++) {
        heat2[i] = qsub8(heat2[i], random8(0, 20));
        heat2[i] = qadd8(heat2[i], random8(0, 30));
        leds2[i] = HeatColor(heat2[i]);
    }
}

void effectPulse() {
    unsigned long now = millis();
    if (now - pw_lastStep < 25) return;
    pw_lastStep = now;

    pw_phase++;

    for (int i = 0; i < NUM_LEDS1; i++) {
        uint8_t wave = sin8(pw_phase + i * 4);
        leds1[i] = CHSV(128, 255, wave); // Azul-esverdeado pulsando
    }

    for (int i = 0; i < NUM_LEDS2; i++) {
        uint8_t wave = sin8(pw_phase + i * 5);
        leds2[i] = CHSV(32, 255, wave); // Laranja pulsando
    }
}

void effectMeteor() {
    unsigned long now = millis();
    if (now - mr_lastStep < MR_SPEED) return;
    mr_lastStep = now;

    // Apaga lentamente — rastro
    for (int i = 0; i < NUM_LEDS1; i++) leds1[i].fadeToBlackBy(40);
    for (int i = 0; i < NUM_LEDS2; i++) leds2[i].fadeToBlackBy(40);

    // Desenha meteoro brilhante
    for (int i = 0; i < MR_SIZE; i++) {
        int p1 = mr_pos - i;
        int p2 = mr_pos - i;
        if (p1 >= 0 && p1 < NUM_LEDS1)
            leds1[p1] = CHSV(0 + i * 8, 255, 255);

        if (p2 >= 0 && p2 < NUM_LEDS2)
            leds2[p2] = CHSV(160 + i * 8, 255, 255);
    }

    // Atualiza posição
    mr_pos += mr_dir;

    // Bate nas bordas
    if (mr_pos >= NUM_LEDS1 - 1 || mr_pos <= 0)
        mr_dir *= -1;
}

  void effectFade() {
    unsigned long now = millis();
    if (now - fade_lastStep >= 30) {
        fade_lastStep = now;

        fade_v += fade_dir * 5;
        if (fade_v >= 255 || fade_v <= 0) fade_dir *= -1;

        for (int i = 0; i < NUM_LEDS1; i++) leds1[i] = CRGB(fade_v, 0, 255 - fade_v);
        for (int i = 0; i < NUM_LEDS2; i++) leds2[i] = CRGB(fade_v, 0, 255 - fade_v);
    }
}

void effectRainbow() {
    unsigned long now = millis();
    if (now - rb_lastStep >= 20) {
        rb_lastStep = now;

        rb_hue += 2;

        for (int i = 0; i < NUM_LEDS1; i++)
            leds1[i] = CHSV(rb_hue + i * 4, 255, 255);

        for (int i = 0; i < NUM_LEDS2; i++)
            leds2[i] = CHSV(rb_hue + i * 4, 255, 255);
    }
}

void effectTwinkle() {
  unsigned long now = millis();
  if (now - tw_lastUpdate >= 150) {
      tw_lastUpdate = now;

      for (int i = 0; i < NUM_LEDS1; i++)
          leds1[i] = CRGB(random(255), random(255), random(255));

      for (int i = 0; i < NUM_LEDS2; i++)
          leds2[i] = CRGB(random(255), random(255), random(255));
  }
}

void runLocalEffects() {
  unsigned long now = millis();

  if (now - lastEffectChange >= EFFECT_INTERVAL) {
      effectIndex = (effectIndex + 1) % 7;
      lastEffectChange = now;
  }

  switch (effectIndex) {
      case 0: effectRainbow(); break;
      case 1: effectFade(); break;
      case 2: effectTwinkle(); break;
      case 3: effectPulse(); break;
      case 4: effectMeteor(); break;
      case 5: effectFire(); break;
      case 6: effectLightning(); break;
  }
}


// ===== Setup =====
void setup() {
  Serial.begin(57600);
  pinMode(LED_STATUS, OUTPUT);
  pinMode(pinSH_CP, OUTPUT);
  pinMode(pinST_CP, OUTPUT);
  pinMode(pinDS, OUTPUT);

  // inicia PCF8574s
  for (int i = 0; i < QTDE_PCF; i++) {
    uint8_t endereco = 0x20 + i;
    pcfs[i].begin(endereco, &Wire);
    for (uint8_t p = 0; p < 8; p++) {
      pcfs[i].pinMode(p, OUTPUT);
      // inicializa com nível de repouso (leva invertPCF em conta)
      pcfs[i].digitalWrite(p, invertPCF[i] ? HIGH : LOW);
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

  ultimaRecepcao = millis();   // impede standby imediato
  Serial.println("🚀 Sistema iniciado. Aguardando '$'...");
}

// ===== Loop principal =====
void loop() {
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

    // Se não estamos em frame Vixen → bytes são comandos (terminador ';')
    // Processa comando (Commands.cpp)
    processaByteSerial(valor, tarefa, rtc);
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
    if (t - ultimoRTC >= intervaloRTC) {
      ultimoRTC = t;
      agora = rtc.now();
    }

    // tarefa.atualizar devolve se mudou o estado global (true = ativo)
    bool ativo = tarefa.atualizar(agora.dayOfTheWeek(), agora.hour(), agora.minute());

    if (ativo) {
      if (!standbyAtivo) {
        standbyAtivo = true;

        // Liga TODOS os relés (atualiza buffer e módulos físicos)
        ligarTodosRele();
      }
    }
    runLocalEffects();
    // ATUALIZAR LEDS E RELES MESMO EM STANDBY
    atualizarRele();
    FastLED.show();
    digitalWrite(LED_STATUS, !digitalRead(LED_STATUS));
    return; // volta cedo como no original (impede atualização normal)
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
