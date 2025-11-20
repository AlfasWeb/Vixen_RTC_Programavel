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

      // Efeito RGB (igual ao original)
      static uint8_t hue = 0;
      static unsigned long lastStandby = 0;
      if (millis() - lastStandby > 30) {
        hue++;
        fill_solid(leds1, NUM_LEDS1, CHSV(hue, 255, 200));
        fill_solid(leds2, NUM_LEDS2, CHSV(hue + 64, 255, 200));
        FastLED.show();
        lastStandby = millis();
      }
    }
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
    // Escreve bufferRele nos expansores / shift registers
    atualizarRele();
    FastLED.show();
    digitalWrite(LED_STATUS, !digitalRead(LED_STATUS));
    lastUpdate = millis();
  }
}