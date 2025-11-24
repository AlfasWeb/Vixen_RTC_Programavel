#include "Commands.h"
#include <EEPROM.h>
#include <stdint.h>

// EEPROM layout:
// addr 0 = magic (0x42)
// from addr 1: for each slot (0..MAX_PROGRAMACOES-1):
//   bytes 0..6 = dias[0..6] (1 byte each)
//   byte 7 = ativo (0/1)
//   byte 8 = hIni
//   byte 9 = mIni
//   byte10 = hFim
//   byte11 = mFim
// total per slot = 12 bytes

#define EEPROM_MAGIC        0x42
#define EEPROM_ADDR_MAGIC   0
#define EEPROM_ADDR_DATA    1
#define SLOT_BYTES          12

// Buffer de parsing: preferimos um buffer char para reduzir uso de String
#define CMD_BUF_MAX 80
static char cmdBuf[CMD_BUF_MAX];
static uint8_t cmdPos = 0;
int cmdLen = 0;

// forward
bool parseProgramacao(const String &dataIn, Programacao &p);
void salvarProgramacoesEEPROM(Programador &prog);
void carregarProgramacoesEEPROM(Programador &prog);
static void imprimirProgramacaoSlot(int idx, const Programacao &p, bool ativoAgora, RTC_DS1307 &rtc);
static bool estaAtivaAgoraLocal(const Programacao &p, int dow, int hour, int minute);

bool charValido(char c) {
  if (c >= '0' && c <= '9') return true;      // números
  if (c >= 'A' && c <= 'Z') return true;      // letras maiúsculas
  if (c >= 'a' && c <= 'z') return true;      // letras minúsculas
  if (c == ',' || c == ';' || c == '#') return true;  // vírgula, ponto e vírgula, hashtag
  return false;                               // descarta o resto
}
// =========================
// Processamento de bytes
// =========================

// Chamada a cada byte recebido quando não estamos em frame Vixen
void processaByteSerial(byte b, Programador &prog, RTC_DS1307 &rtc) {
  if (!charValido((char)b)) {
    return; // ignora caractere proibido
  }
  if (b == ';') {
    cmdBuf[cmdLen] = '\0';   // finaliza a string C
    processaComando(cmdBuf, prog, rtc);
    cmdLen = 0;              // limpa para próximo comando
    cmdBuf[0] = '\0';
    return;
  }
  
  if (cmdLen < CMD_BUF_MAX - 1) {
    cmdBuf[cmdLen++] = (char)b;
  }
}
// =========================
// Parsing / comandos
// =========================

// parse string "1111111,HH,MM,HH,MM" into Programacao (does NOT set ativo)
bool parseProgramacao(const char *dataIn, Programacao &p) {
    // Copia para buffer local pois vamos editar
    char s[40];
    strncpy(s, dataIn, sizeof(s));
    s[sizeof(s)-1] = '\0';

    // 1) Pegar dias: 7 chars antes da primeira vírgula
    char *comma = strchr(s, ',');
    if (!comma) return false;

    *comma = '\0'; // separa dias do resto

    if (strlen(s) != 7) return false;

    for (int i = 0; i < 7; i++) {
        p.dias[i] = (s[i] == '1');
    }

    // 2) Pegar os horários: HH,MM,HH,MM

    char *resto = comma + 1;

    int valores[4];
    char *token = strtok(resto, ",");

    for (int i = 0; i < 4; i++) {
        if (!token) return false;
        valores[i] = atoi(token);
        token = strtok(NULL, ",");
    }

    // validar
    if (valores[0] < 0 || valores[0] > 23) return false;
    if (valores[1] < 0 || valores[1] > 59) return false;
    if (valores[2] < 0 || valores[2] > 23) return false;
    if (valores[3] < 0 || valores[3] > 59) return false;

    p.hIni = valores[0];
    p.mIni = valores[1];
    p.hFim = valores[2];
    p.mFim = valores[3];

    return true;
}

void salvarProgramacoesEEPROM(Programador &prog) {
  EEPROM.write(EEPROM_ADDR_MAGIC, EEPROM_MAGIC);
  int addr = EEPROM_ADDR_DATA;
  for (int i = 0; i < MAX_PROGRAMACOES; i++) {
    Programacao p = prog.getProgramacao(i);
    for (int d = 0; d < 7; d++) EEPROM.write(addr++, p.dias[d] ? 1 : 0);
    EEPROM.write(addr++, p.ativo ? 1 : 0);
    EEPROM.write(addr++, p.hIni);
    EEPROM.write(addr++, p.mIni);
    EEPROM.write(addr++, p.hFim);
    EEPROM.write(addr++, p.mFim);
  }
  Serial.println("EEPROM: gravação concluída");
}

void carregarProgramacoesEEPROM(Programador &prog) {
  if (EEPROM.read(EEPROM_ADDR_MAGIC) != EEPROM_MAGIC) {
    Serial.println("EEPROM: magic nao encontrado, ignorando load");
    return;
  }
  int addr = EEPROM_ADDR_DATA;
  for (int i = 0; i < MAX_PROGRAMACOES; i++) {
    Programacao p;
    for (int d = 0; d < 7; d++) p.dias[d] = (EEPROM.read(addr++) != 0);
    p.ativo = (EEPROM.read(addr++) != 0);
    p.hIni = EEPROM.read(addr++);
    p.mIni = EEPROM.read(addr++);
    p.hFim = EEPROM.read(addr++);
    p.mFim = EEPROM.read(addr++);
    prog.setProgramacao(i, p);
  }
  Serial.println("EEPROM: carregado");
}

static void imprimirProgramacaoSlot(int idx, const Programacao &p, bool ativoAgora, RTC_DS1307 &rtc) {
  Serial.print("PG");
  Serial.print(idx + 1);
  Serial.print(": ");
  if (p.hIni == 0 && p.mIni == 0 && p.hFim == 0 && p.mFim == 0) {
    Serial.println("Vazio");
    return;
  }
  Serial.print(p.ativo ? "(ATIVADO) " : "(DESATIVADO) ");
  Serial.print("Dias: ");
  const char* names[7] = {"Dom","Seg","Ter","Qua","Qui","Sex","Sab"};
  for (int d = 0; d < 7; d++) {
    if (p.dias[d]) {
      Serial.print(names[d]);
      Serial.print(" ");
    }
  }
  Serial.print(" | ");
  Serial.print(p.hIni); Serial.print(":"); if (p.mIni < 10) Serial.print('0'); Serial.print(p.mIni);
  Serial.print(" -> ");
  Serial.print(p.hFim); Serial.print(":"); if (p.mFim < 10) Serial.print('0'); Serial.print(p.mFim);
  Serial.print(" | Agora: ");
  Serial.println(ativoAgora ? "ON" : "OFF");
}

static bool estaAtivaAgoraLocal(const Programacao &p, int dow, int hour, int minute) {
  if (!p.ativo) return false;
  if (!p.dias[dow]) return false;
  int agora = hour * 60 + minute;
  int ini = p.hIni * 60 + p.mIni;
  int fim = p.hFim * 60 + p.mFim;
  if (ini <= fim) return (agora >= ini && agora <= fim);
  return (agora >= ini || agora <= fim);
}

// processa o comando já limpo (pode começar com '#' ou não)
void processaComando(const char *cmdRaw, Programador &prog, RTC_DS1307 &rtc) {
    // Copia para buffer local editável
    char cmd[CMD_BUF_MAX];
    strncpy(cmd, cmdRaw, CMD_BUF_MAX);
    cmd[CMD_BUF_MAX - 1] = '\0';

    // ---------------------------------------------------------
    // Remove \r \n
    // ---------------------------------------------------------
    for (int i = 0; cmd[i]; i++) {
        if (cmd[i] == '\n' || cmd[i] == '\r')
            cmd[i] = '\0';
    }

    // Remove espaços no começo
    while (cmd[0] == ' ') memmove(cmd, cmd + 1, strlen(cmd));

    // Remove '#'
    if (cmd[0] == '#')
        memmove(cmd, cmd + 1, strlen(cmd));

    // Agora cmd contém apenas:
    // "st"
    // "pg1,1111111,18,00,23,00"
    // "rm1"
    // etc.

    // ---------------------------------------------------------
    // Comando ST
    // ---------------------------------------------------------
    if (strncmp(cmd, "st", 2) == 0) {
        DateTime now = rtc.now();
        Serial.println("=== STATUS DAS PROGRAMACOES ===");
        for (int i = 0; i < MAX_PROGRAMACOES; i++) {
            Programacao p = prog.getProgramacao(i);
            bool vazio = (p.hIni == 0 && p.mIni == 0 && p.hFim == 0 && p.mFim == 0);

            Serial.print("Slot ");
            Serial.print(i + 1);
            Serial.print(": ");

            if (vazio) {
                Serial.println("Vazio");
                continue;
            }

            bool ativoAgora = (
                p.ativo &&
                p.dias[now.dayOfTheWeek()] &&
                ((now.hour() * 60 + now.minute()) >= (p.hIni * 60 + p.mIni)) &&
                ((now.hour() * 60 + now.minute()) <= (p.hFim * 60 + p.mFim))
            );

            Serial.print(p.ativo ? "(ATIVADO) " : "(DESATIVADO) ");
            Serial.print("Dias: ");

            const char* names[7] = {
                "Dom","Seg","Ter","Qua","Qui","Sex","Sab"
            };

            for (int d = 0; d < 7; d++) {
                if (p.dias[d]) {
                    Serial.print(names[d]);
                    Serial.print(" ");
                }
            }

            Serial.print(" | ");
            Serial.print(p.hIni); Serial.print(":");
            if (p.mIni < 10) Serial.print('0');
            Serial.print(p.mIni);

            Serial.print(" -> ");

            Serial.print(p.hFim); Serial.print(":");
            if (p.mFim < 10) Serial.print('0');
            Serial.print(p.mFim);

            Serial.print(" | Agora: ");
            Serial.println(ativoAgora ? "ON" : "OFF");
        }
        Serial.println("=== FIM DO STATUS ===");
        return;
    }

    // ---------------------------------------------------------
    // Comando PGx,dados...
    // ---------------------------------------------------------
    // ---------------------------------------------------------
// Comando PGx,dados...
// Ex:  pg1,1111111,18,30,23,59
// ---------------------------------------------------------
  if (strncmp(cmd, "pg", 2) == 0) {

      // Pega o índice após "pg"
      // Ex: "pg1" → idx = 1-1 = 0
      int idx = cmd[2] - '0' - 1;

      if (idx < 0 || idx >= MAX_PROGRAMACOES) {
          Serial.println("ERR: idx fora");
          return;
      }

      // Procura a vírgula que separa pgX dos dados
      char *dados = strchr(cmd, ',');
      if (!dados) {
          Serial.println("ERR: formato pg");
          return;
      }

      dados++;     // pula a vírgula — agora aponta para "1111111,18,30,23,59"

      Programacao p;

      // Agora parseProgramacao recebe um char* e não String
      if (!parseProgramacao(dados, p)) {
          Serial.println("ERR: pg formato invalido");
          return;
      }

      p.ativo = true;
      prog.setProgramacao(idx, p);
      salvarProgramacoesEEPROM(prog);

      Serial.print("OK: PG ");
      Serial.print(idx + 1);
      Serial.println(" salva e ativada");
      return;
  }


    // ---------------------------------------------------------
    // RMx — Remover programação
    // ---------------------------------------------------------
    if (strncmp(cmd, "rm", 2) == 0) {
        int idx = atoi(cmd + 2) - 1;
        if (idx < 0 || idx >= MAX_PROGRAMACOES) {
            Serial.println("ERR: idx fora");
            return;
        }
        prog.remover(idx);
        salvarProgramacoesEEPROM(prog);
        Serial.print("OK: PG ");
        Serial.print(idx + 1);
        Serial.println(" removida");
        return;
    }
    // ---------------------------------------------------------
    // RMx — Remover programação
    // ---------------------------------------------------------
    if (strncmp(cmd, "up", 2) == 0) {
        rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
        Serial.print("OK: Relogio atualizado ");
        return;
    }
    // ---------------------------------------------------------
    // HR — Ajustar relógio
    // Formato: hrHH,MM,DD,MM,YYYY
    // ---------------------------------------------------------
    if (strncmp(cmd, "hr", 2) == 0) {

        char *dados = cmd + 2;

        int hh = atoi(strtok(dados, ","));
        int mm = atoi(strtok(NULL, ","));
        int dd = atoi(strtok(NULL, ","));
        int mo = atoi(strtok(NULL, ","));
        int yy = atoi(strtok(NULL, ","));

        rtc.adjust(DateTime(yy, mo, dd, hh, mm, 0));

        Serial.println("OK: RTC ajustado");
        return;
    }

    // ---------------------------------------------------------
    // Comando não reconhecido
    // ---------------------------------------------------------
    Serial.print("ERR comando desconhecido: ");
    Serial.println(cmd);
}