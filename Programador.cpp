#include "Programador.h"

Programador::Programador() {
  estadoAtual = false;
  // inicializa como slots vazios
  for (int i = 0; i < MAX_PROGRAMACOES; i++) {
    for (int d = 0; d < 7; d++) progs[i].dias[d] = false;
    progs[i].hIni = progs[i].mIni = progs[i].hFim = progs[i].mFim = 0;
    progs[i].ativo = false;
  }
}

void Programador::setProgramacao(int idx, const Programacao &p) {
  if (idx < 0 || idx >= MAX_PROGRAMACOES) return;
  progs[idx] = p;
  // when setting a program, keep the provided ativo value (Commands uses ativo=true on create)
}

Programacao Programador::getProgramacao(int idx) {
  Programacao empty;
  if (idx < 0 || idx >= MAX_PROGRAMACOES) {
    // return empty
    for (int d = 0; d < 7; d++) empty.dias[d] = false;
    empty.hIni = empty.mIni = empty.hFim = empty.mFim = 0;
    empty.ativo = false;
    return empty;
  }
  return progs[idx];
}

void Programador::ativar(int idx) {
  if (idx < 0 || idx >= MAX_PROGRAMACOES) return;
  progs[idx].ativo = true;
}

void Programador::desativar(int idx) {
  if (idx < 0 || idx >= MAX_PROGRAMACOES) return;
  progs[idx].ativo = false;
}

void Programador::remover(int idx) {
  if (idx < 0 || idx >= MAX_PROGRAMACOES) return;
  for (int d = 0; d < 7; d++) progs[idx].dias[d] = false;
  progs[idx].hIni = progs[idx].mIni = progs[idx].hFim = progs[idx].mFim = 0;
  progs[idx].ativo = false;
}

// Helper: verifica se um slot está "vazio" (sem dados)
static bool slotVazio(const Programacao &p) {
  if (p.hIni != 0) return false;
  if (p.mIni != 0) return false;
  if (p.hFim != 0) return false;
  if (p.mFim != 0) return false;
  for (int d = 0; d < 7; d++) if (p.dias[d]) return false;
  return true;
}

// Helper: verifica se p está ativo no dia/hora atual
static bool progAtivaAgora(const Programacao &p, int dow, int hora, int minuto) {
  if (slotVazio(p)) return false;
  if (!p.ativo) return false;
  if (!p.dias[dow]) return false;

  int agora = hora * 60 + minuto;
  int ini = p.hIni * 60 + p.mIni;
  int fim = p.hFim * 60 + p.mFim;

  // caso início <= fim (mesmo dia)
  if (ini <= fim) {
    return (agora >= ini && agora <= fim);
  } else {
    // caso a programação rode passando meia-noite (ex: 22:00 -> 02:00)
    return (agora >= ini || agora <= fim);
  }
}

bool Programador::atualizar(int diaSemana, int hora, int minuto) {
  bool novoEstado = false;
  for (int i = 0; i < MAX_PROGRAMACOES; i++) {
    if (progAtivaAgora(progs[i], diaSemana, hora, minuto)) {
      novoEstado = true;
      break;
    }
  }
  bool mudou = (novoEstado != estadoAtual);
  estadoAtual = novoEstado;
  return mudou;
}

bool Programador::getEstadoAtual() const {
  return estadoAtual;
}