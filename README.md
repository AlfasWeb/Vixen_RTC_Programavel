# Vixen RTC Programavel

# Projeto: Controlador de Iluminação com PCF8574, 74HC595, WS2811 e Vixen

Este documento descreve detalhadamente o funcionamento, arquitetura, comunicação e lógica do firmware desenvolvido para o controlador de iluminação utilizado com o software **Vixen Lights**, integrando **relés**, **PCF8574**, **74HC595**, **fitas/cordões WS2811** e um **RTC DS1307** com programação interna.

---

## 📌 Visão Geral do Sistema

O controlador recebe dados via **Serial** em alta velocidade, interpreta frames enviados pelo Vixen (iniciados por `$`) e distribui os valores para:

* **2 módulos PCF8574** → 16 relés
* **1 CI 74HC595** → 8 relés adicionais
* **2 saídas WS2811** → 100 LEDs (50 em cada saída)
* **Programação interna por horário** → usando RTC DS1307
* **Modo Standby** quando o Vixen não envia dados

Total de canais:

* **24 canais de relé (1-24)**
* **300 canais de LED (25-324)**
* TOTAL = **324 canais**

---

## 🧱 Hardware Utilizado

### 🔹 Expansores de porta

| Componente | Quantidade | Função                 |
| ---------- | ---------- | ---------------------- |
| PCF8574    | 2          | 16 relés (canais 1–16) |
| 74HC595    | 1          | 8 relés (canais 17–24) |

### 🔹 LEDs WS2811

* Porta **LED_PIN1 = 5** → 50 LEDs (canais 25-174)
* Porta **LED_PIN2 = 6** → 50 LEDs (canais 175-324)
* Protocolo configurado como **BRG** (não RGB)

### 🔹 RTC DS1307

* Mantém hora e executa programação automatizada

---

## 🧭 Mapeamento de Canais

### 🔹 Relés

| Intervalo | Módulo  | Descrição   |
| --------- | ------- | ----------- |
| **1–8**   | PCF1    | Relés 1-8   |
| **9–16**  | PCF2    | Relés 9-16  |
| **17–24** | 74HC595 | Relés 17-24 |

### 🔹 LEDs

Como WS2811 usa 3 canais por LED:

| Intervalo   | Porta    | LEDs        |
| ----------- | -------- | ----------- |
| **25–174**  | LED_PIN1 | LEDs 1–50   |
| **175–324** | LED_PIN2 | LEDs 51–100 |

---

## 🚦 Lógica de Comunicação com o Vixen

* Cada frame enviado começa com **`$`**
* Em seguida o Vixen envia **324 bytes**, um por canal
* Relés interpretam valor alto como **ligado** apenas se **valor > 235** (ONMIN)
* LEDs recebem valores **RAW (0-255)** diretamente nos componentes R/G/B

---

## 🔄 Funcionamento Geral do Firmware

### 1️⃣ Recepção de frame do Vixen

* Ao receber `$`, entra em modo de captura de frame
* Cada byte é atribuído ao seu canal correspondente
* Ao receber todos os 324 canais:

  * Atualiza todos os relés
  * Atualiza LEDs via FastLED
  * Grava estado no buffer

### 2️⃣ Timeout de frame

Se o Vixen não enviar dados por **100 ms**, o frame é cancelado.
Se passar **2 segundos sem Vixen**, entra no **Modo Standby**.

---

## 🌙 Modo Standby (sem Vixen)

Ativado após **2 segundos** sem comunicação.

### Comportamento do Standby:

* **Todos os relés são ligados**
* LEDs entram em efeito suave baseado em **CHSV(hue)** com brilho reduzido
* A programação por horário é executada (objeto `Programador`)

Quando o Vixen volta a enviar dados, o Standby é desativado automaticamente.

---

## 🕒 Programação por Horário

Gerenciada pelo objeto:

```
Programador tarefa;
```

Comandos são recebidos via Serial e armazenados na EEPROM.
O RTC faz ticking a cada 1 segundo.
Se a tarefa ativa indicar horário válido → mantém relés ligados.

---

## 🔧 Lógica dos Relés

### PCF8574

* Os módulos são invertidos com:

```
bool invertPCF[] = { true, true };
```

* `true` = LOW liga o relé

### 74HC595

* Relé LIGADO = **HIGH**

### Buffer unificado (`bufferRele[]`)

O Vixen escreve neste buffer, depois o firmware distribui:

* Primeiro para **PCF8574**
* Depois para **74HC595**

---

## 💡 LEDs WS2811

Configuração:

```
FastLED.addLeds<WS2811, LED_PIN1, BRG>(...);
FastLED.addLeds<WS2811, LED_PIN2, BRG>(...);
```

Importante: o formato é **BRG**, conforme seu hardware.

Cada LED usa 3 bytes na ordem:

1. Vermelho (R)
2. Verde (G)
3. Azul (B)

---

## 🚧 Proteção contra interferência / travamentos

O firmware **não usa** `noInterrupts()` para o FastLED, evitando bloqueios de I²C e problemas nos PCF.

---

## 📂 Estrutura dos Arquivos

```
main.ino
Programador.cpp / Programador.h
Commands.cpp / Commands.h
README.md
```

---

## 🧪 Testes sugeridos

1. Teste de relés manual
2. Teste de canais pelo Vixen
3. Teste de Standby com relés ligados
4. Teste de programação por horário

---

## 📞 Suporte e Ajustes

Se quiser, posso gerar também:

* Diagrama do fluxo de comunicação
* Mapa visual dos canais
* Planilha pronta para importar no Vixen
* Diagrama elétrico

Basta pedir! 😊
