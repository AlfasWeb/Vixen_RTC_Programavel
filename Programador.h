#ifndef PROGRAMADOR_H
#define PROGRAMADOR_H

#include <Arduino.h>

#define MAX_PROGRAMACOES 10

struct Programacao {
  bool dias[7];    // Dom..Sab
  uint8_t hIni;
  uint8_t mIni;
  uint8_t hFim;
  uint8_t mFim;
  bool ativo;      // true = ativada, false = desativada (Modelo B)
};

class Programador {
public:
  Programador();

  // leitura/escrita de slot
  void setProgramacao(int idx, const Programacao &p);
  Programacao getProgramacao(int idx);

  // ativar / desativar sem apagar dados
  void ativar(int idx);
  void desativar(int idx);

  // remover (apagar dados)
  void remover(int idx);

  // rotina chamada periodicamente para atualizar estado
  // retorna true se o estado global (on/off) mudou
  bool atualizar(int diaSemana, int hora, int minuto);

  // retorna estado atual (ON/OFF) calculado
  bool getEstadoAtual() const;

private:
  Programacao progs[MAX_PROGRAMACOES];
  bool estadoAtual;
};

#endif