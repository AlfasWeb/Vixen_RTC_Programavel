#ifndef COMMANDS_H
#define COMMANDS_H

#include <Arduino.h>
#include "Programador.h"
#include "RTClib.h"

// EEPROM helpers
void salvarProgramacoesEEPROM(Programador &prog);
void carregarProgramacoesEEPROM(Programador &prog);

// Processamento de comandos
void processaComando(String cmd, Programador &prog, RTC_DS1307 &rtc);
void processaByteSerial(byte b, Programador &prog, RTC_DS1307 &rtc);

#endif
