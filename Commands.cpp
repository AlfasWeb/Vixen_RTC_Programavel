#include "Commands.h"
#include <EEPROM.h>

// EEPROM layout:
// addr 0 = magic (0x42)
// from addr 1: for each slot (0..9):
//   bytes 0..6 = dias[0..6] (1 byte each)
//   byte 7 = ativo (0/1)
//   byte 8 = hIni
//   byte 9 = mIni
//   byte10 = hFim
//   byte11 = mFim
// total per slot = 12 bytes
// total = 1 + 10*12 = 121 bytes (fits in Arduino UNO EEPROM)

#define EEPROM_MAGIC        0x42
#define EEPROM_ADDR_MAGIC   0
#define EEPROM_ADDR_DATA    1
#define SLOT_BYTES          12

static String cmdBuf = "";

// parse string "1111111,HH,MM,HH,MM" into Programacao (does NOT set ativo)
bool parseProgramacao(const String &dataIn, Programacao &p) {
  String s = dataIn;
  s.trim();

  int pos = s.indexOf(',');
  if (pos < 0) return false;
  String diasStr = s.substring(0, pos);
  if (diasStr.length() != 7) return false;
  for (int i = 0; i < 7; i++) p.dias[i] = (diasStr.charAt(i) == '1');

  // rest: HH,MM,HH,MM
  s = s.substring(pos + 1);
  int parts[4];
  for (int i = 0; i < 4; i++) {
    int ppos = s.indexOf(',');
    if (ppos == -1 && i < 3) return false;
    String f = (ppos == -1) ? s : s.substring(0, ppos);
    parts[i] = f.toInt();
    if (ppos == -1) s = "";
    else s = s.substring(ppos + 1);
  }
  p.hIni = (uint8_t)constrain(parts[0], 0, 23);
  p.mIni = (uint8_t)constrain(parts[1], 0, 59);
  p.hFim = (uint8_t)constrain(parts[2], 0, 23);
  p.mFim = (uint8_t)constrain(parts[3], 0, 59);
  // ativo is set by caller
  return true;
}

void salvarProgramacoesEEPROM(Programador &prog) {
  EEPROM.write(EEPROM_ADDR_MAGIC, EEPROM_MAGIC);
  int addr = EEPROM_ADDR_DATA;
  for (int i = 0; i < MAX_PROGRAMACOES; i++) {
    Programacao p = prog.getProgramacao(i);
    // dias
    for (int d = 0; d < 7; d++) EEPROM.write(addr++, p.dias[d] ? 1 : 0);
    // ativo
    EEPROM.write(addr++, p.ativo ? 1 : 0);
    // times
    EEPROM.write(addr++, p.hIni);
    EEPROM.write(addr++, p.mIni);
    EEPROM.write(addr++, p.hFim);
    EEPROM.write(addr++, p.mFim);
  }
  Serial.println("EEPROM: gravação concluída");
}

void carregarProgramacoesEEPROM(Programador &prog) {
  if (EEPROM.read(EEPROM_ADDR_MAGIC) != EEPROM_MAGIC) {
    // EEPROM vazia — inicializa zeros (opcional)
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

// helper para imprimir programacao
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

// calcula se p está ativo agora (sem precisar de Programador.verificarAtivo)
static bool estaAtivaAgoraLocal(const Programacao &p, int dow, int hour, int minute) {
  if (!p.ativo) return false;
  if (!p.dias[dow]) return false;
  int agora = hour * 60 + minute;
  int ini = p.hIni * 60 + p.mIni;
  int fim = p.hFim * 60 + p.mFim;
  if (ini <= fim) {
    return (agora >= ini && agora <= fim);
  } else {
    // cross-midnight
    return (agora >= ini || agora <= fim);
  }
}

// processa o comando já limpo (sem '#', sem ';' final)
void processaComando(String cmdRaw, Programador &prog, RTC_DS1307 &rtc) {
  String cmd = cmdRaw;
  cmd.trim();
  if (cmd.length() == 0) return;

  // garante que cmd não começa com '#'
  if (cmd.charAt(0) == '#') cmd = cmd.substring(1);

  // comando #pgX,...
  if (cmd.startsWith("pg")) {
    int sep = cmd.indexOf(',');
    if (sep < 0) { Serial.println("ERR: formato #pg"); return; }
    int idx = cmd.substring(2, sep).toInt() - 1;
    if (idx < 0 || idx >= MAX_PROGRAMACOES) { Serial.println("ERR: idx fora"); return; }
    String dados = cmd.substring(sep + 1);
    Programacao p;
    if (!parseProgramacao(dados, p)) { Serial.println("ERR: pg formato invalido"); return; }
    p.ativo = true; // ativar ao criar/editar
    prog.setProgramacao(idx, p);
    salvarProgramacoesEEPROM(prog);
    Serial.print("OK: PG ");
    Serial.print(idx + 1);
    Serial.println(" salva e ativada");
    return;
  }

  // #rmX
  if (cmd.startsWith("rm")) {
    int idx = cmd.substring(2).toInt() - 1;
    if (idx < 0 || idx >= MAX_PROGRAMACOES) { Serial.println("ERR: idx fora"); return; }
    prog.remover(idx);
    salvarProgramacoesEEPROM(prog);
    Serial.print("OK: PG ");
    Serial.print(idx + 1);
    Serial.println(" removida");
    return;
  }

  // #dtX -> desativar (mantem dados)
  if (cmd.startsWith("dt")) {
    int idx = cmd.substring(2).toInt() - 1;
    if (idx < 0 || idx >= MAX_PROGRAMACOES) { Serial.println("ERR: idx fora"); return; }
    prog.desativar(idx);
    salvarProgramacoesEEPROM(prog);
    Serial.print("OK: PG ");
    Serial.print(idx + 1);
    Serial.println(" desativada");
    return;
  }

  // #atX -> ativar (mantem dados)
  if (cmd.startsWith("at")) {
    int idx = cmd.substring(2).toInt() - 1;
    if (idx < 0 || idx >= MAX_PROGRAMACOES) { Serial.println("ERR: idx fora"); return; }
    prog.ativar(idx);
    salvarProgramacoesEEPROM(prog);
    Serial.print("OK: PG ");
    Serial.print(idx + 1);
    Serial.println(" ativada");
    return;
  }

  // #hrHH,MM,DD,MM,YYYY
  if (cmd.startsWith("hr")) {
    String dados = cmd.substring(2);
    // split by ','
    int parts[5];
    for (int i = 0; i < 5; i++) {
      int p = dados.indexOf(',');
      if (p == -1 && i < 4) { Serial.println("ERR: formato #hr"); return; }
      String part = (p == -1) ? dados : dados.substring(0, p);
      parts[i] = part.toInt();
      if (p == -1) dados = "";
      else dados = dados.substring(p + 1);
    }
    int hh = parts[0], mm = parts[1], dd = parts[2], mo = parts[3], yy = parts[4];
    rtc.adjust(DateTime(yy, mo, dd, hh, mm, 0));
    Serial.println("OK: RTC ajustado");
    return;
  }

  // #st -> status geral (modelo B)
  if (cmd.startsWith("st")) {
    DateTime now = rtc.now();
    Serial.println("=== STATUS DAS PROGRAMACOES ===");
    for (int i = 0; i < MAX_PROGRAMACOES; i++) {
      Programacao p = prog.getProgramacao(i);
      bool vazio = (p.hIni == 0 && p.mIni == 0 && p.hFim == 0 && p.mFim == 0);
      Serial.print("Slot "); Serial.print(i + 1); Serial.print(": ");
      if (vazio) {
        Serial.println("Vazio");
        continue;
      }
      bool ativoAgora = estaAtivaAgoraLocal(p, now.dayOfTheWeek(), now.hour(), now.minute());
      imprimirProgramacaoSlot(i, p, ativoAgora, rtc);
    }
    Serial.println("=== FIM DO STATUS ===");
    return;
  }

  Serial.println("ERR: comando desconhecido");
}

// processa um byte vindo do Serial quando fora de frame Vixen
void processaByteSerial(byte b, Programador &prog, RTC_DS1307 &rtc) {
  if (b == ';') {
    // processa comando completo em cmdBuf
    processaComando(cmdBuf, prog, rtc);
    cmdBuf = "";
  } else {
    cmdBuf += (char)b;
  }
}
