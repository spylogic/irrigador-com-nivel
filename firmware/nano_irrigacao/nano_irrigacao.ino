/*
  ====================================================================
  IRRIGADOR COM NÍVEL - Firmware do ARDUINO NANO
  ====================================================================
  Função deste Nano:
    - Ler o nível da caixa d'água (HC-SR04)
    - Ler temperatura e umidade (DHT11)
    - Ler o Nível de Transbordo (HW-038), como indicador percentual
      (0-100%) de quanto o reservatório de destino/transbordo está cheio
    - Acionar o relé da bomba (HW-482), com travas de segurança
    - Conversar com o ESP01 por serial (SoftwareSerial), que é quem
      fala com a internet/Firebase e decide QUANDO ligar a bomba
      (botão do site ou horário programado)

  O Nano NUNCA decide "por que" ligar a bomba - isso é do ESP01/site.
  O Nano decide "se pode" ligar (trava de tempo máximo e trava de
  transbordo), o que é essencial para não estragar a bomba e não
  alagar nada caso a internet/ESP01 trave.

  Sobre o HW-038 (Nível de Transbordo): este sensor não é mais usado como
  trava de "bomba a seco". Ele virou um indicador de transbordo - ao
  chegar em 50% o dashboard mostra o alarme "Rega Completa", e ao chegar
  em 75% a bomba é desligada automaticamente por segurança (só liga de
  novo depois de um comando explícito de "desligar" vindo do site).

  Bibliotecas necessárias (Gerenciador de Bibliotecas do Arduino IDE):
    - "DHT sensor library" (Adafruit)
    - "Adafruit Unified Sensor" (dependência da acima)
  (SoftwareSerial já vem com o Arduino IDE, não precisa instalar)

  ====================================================================
  MAPA DE PINOS DO NANO
  ====================================================================
    D2  -> DHT11 DATA
    D3  -> HC-SR04 TRIG
    D4  -> HC-SR04 ECHO
    A0  -> HW-038 (pino "S" / saída analógica) - indicador de Nível de Transbordo
    D5  -> HW-482 IN (sinal do relé)
    D8  -> SoftwareSerial RX  <- ESP01 TX (ligação direta)
    D9  -> SoftwareSerial TX  -> divisor de tensão (1k+2k) -> ESP01 RX
    5V / GND -> alimentação do DHT11, HC-SR04, HW-482 e HW-038

  IMPORTANTE: o ESP01 é alimentado por uma fonte 3.3V À PARTE (não pelo
  Nano), mas o GND do ESP01 tem que ser o MESMO GND do Nano.
  Veja o diagrama de ligação (wiring_diagram.svg) e o README para os
  detalhes completos, incluindo a parte de segurança elétrica da bomba
  AC 127/220V.
  ====================================================================
*/

#include <SoftwareSerial.h>
#include <DHT.h>

// ---------------- Pinos ----------------
#define PIN_DHT        2
#define PIN_TRIG       3
#define PIN_ECHO       4
#define PIN_AGUA_AO    A0
#define PIN_RELE       5
#define PIN_SS_RX      8   // <- ESP01 TX
#define PIN_SS_TX      9   // -> divisor -> ESP01 RX

#define DHTTYPE DHT11

// ---------------- Calibração da caixa d'água ----------------
// Medidas informadas: altura útil = 25 cm, sensor montado 5 cm acima
// do nível máximo de água (olhando pra baixo).
//
// IMPORTANTE: estas constantes são usadas SÓ para o cálculo de nível
// mostrado aqui no Monitor Serial (debug local). O nível/volume que
// aparece no DASHBOARD é calculado pelo ESP01, com as medidas que você
// configura direto no site (card "Configuração da caixa d'água") - isso
// permite reaproveitar este mesmo firmware em qualquer caixa d'água sem
// precisar regravar o Nano. O Nano só manda a DISTÂNCIA BRUTA (cm) lida
// pelo sensor; quem transforma isso em % e litros é o ESP01.
const float SENSOR_ATE_NIVEL_MAX_CM = 5.0;   // distância do sensor até a água quando a caixa está CHEIA
const float ALTURA_UTIL_CM          = 25.0;  // altura útil (nível máximo até o fundo)
const float SENSOR_ATE_VAZIA_CM     = SENSOR_ATE_NIVEL_MAX_CM + ALTURA_UTIL_CM; // distância quando VAZIA (30 cm)
const float VOLUME_TOTAL_LITROS     = 5.0;   // volume da caixa (aprox., assume seção uniforme)

// Zona morta de segurança perto do sensor (o HC-SR04 tem alcance mínimo
// de ~2 cm; como o nível cheio já fica a 5 cm, deixamos uma margem).
const float MARGEM_CHEIO_CM = 1.0;

// ---------------- Calibração do Nível de Transbordo (HW-038) ----------------
// O HW-038 é um sensor resistivo: conforme a água sobe e cobre mais
// trilhas, a leitura analógica no pino "S" varia de forma gradual - por
// isso dá pra usar como indicador de PERCENTUAL, não só um sim/não.
// Para calibrar: abra o Monitor Serial, anote a leitura bruta (0-1023)
// com o sensor seco e depois totalmente submerso na altura máxima que
// você quer monitorar, e ajuste os dois valores abaixo.
const int LEITURA_SECA_TRANSBORDO  = 200;  // leitura aproximada com o sensor seco
const int LEITURA_CHEIA_TRANSBORDO = 800;  // leitura aproximada com o sensor totalmente molhado

// A partir de que % o dashboard mostra o alarme "Rega Completa"
const float TRANSBORDO_LIMIAR_ALARME_PCT = 50.0;

// A partir de que % a bomba é desligada automaticamente por segurança
// (evita transbordamento). Só liga de novo depois de um comando explícito
// de "desligar" vindo do ESP01 (mesma lógica da trava de tempo máximo).
const float TRANSBORDO_LIMIAR_DESLIGA_PCT = 75.0;

// ---------------- Relé / bomba: travas de segurança ----------------
// Alguns módulos de relé de 1 canal (como o HW-482) acionam em NÍVEL
// ALTO no IN, outros (bem comuns) acionam em NÍVEL BAIXO (invertido).
// Teste com a bomba/carga DESLIGADA da rede primeiro: se o relé ligar
// "trocado" (ativa quando devia desativar), troque HIGH<->LOW abaixo.
#define RELE_NIVEL_ATIVO   HIGH   // <-- ajuste aqui se o seu módulo for invertido

// Tempo máximo contínuo com a bomba ligada, MESMO que o ESP01 mande
// ligar continuamente. Protege contra travamento de software / Firebase
// fora do ar com comando "ligado" preso.
const unsigned long TEMPO_MAX_LIGADO_MS = 2UL * 60UL * 1000UL; // 2 minutos

// Se o Nano ficar esse tempo sem receber NENHUM comando válido do ESP01,
// desliga a bomba por segurança (rede caiu, ESP01 travou, cabo solto, etc).
const unsigned long TIMEOUT_COMUNICACAO_MS = 60UL * 1000UL; // 60 segundos

// Intervalo de leitura/relato dos sensores
const unsigned long INTERVALO_LEITURA_MS = 5000UL;

// ---------------- Objetos ----------------
DHT dht(PIN_DHT, DHTTYPE);
SoftwareSerial espSerial(PIN_SS_RX, PIN_SS_TX); // RX, TX

// ---------------- Estado ----------------
bool releLigado = false;
bool comandoDesejado = false;     // último comando recebido do ESP01 (0/1)
unsigned long releLigadoDesde = 0;
unsigned long ultimoComandoRecebidoEm = 0;
unsigned long ultimaLeituraEm = 0;
bool precisaTransicaoParaRearmar = false; // exige C:0 depois de um corte de segurança

String bufferSerial = "";

void setup() {
  Serial.begin(9600);        // Monitor Serial (USB) - debug
  espSerial.begin(9600);     // Serial por software até o ESP01

  // Define o pino do relé como DESLIGADO antes de virar OUTPUT,
  // pra não dar um pulso de "ligado" durante o boot/reset.
  digitalWrite(PIN_RELE, RELE_NIVEL_ATIVO == HIGH ? LOW : HIGH);
  pinMode(PIN_RELE, OUTPUT);
  digitalWrite(PIN_RELE, RELE_NIVEL_ATIVO == HIGH ? LOW : HIGH);

  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  digitalWrite(PIN_TRIG, LOW);

  dht.begin();

  Serial.println(F("Nano irrigador iniciado."));
}

// ---------------- Leitura HC-SR04 (com filtro de mediana) ----------------
float lerDistanciaCm() {
  const int N = 5;
  float leituras[N];
  int validas = 0;

  for (int i = 0; i < N; i++) {
    digitalWrite(PIN_TRIG, LOW);
    delayMicroseconds(3);
    digitalWrite(PIN_TRIG, HIGH);
    delayMicroseconds(10);
    digitalWrite(PIN_TRIG, LOW);

    unsigned long duracao = pulseIn(PIN_ECHO, HIGH, 30000UL); // timeout 30ms (~5m)
    if (duracao > 0) {
      float distancia = duracao * 0.0343 / 2.0; // cm
      leituras[validas++] = distancia;
    }
    delay(30);
  }

  if (validas == 0) return -1.0; // sensor não respondeu

  // ordena (bubble sort simples, N é pequeno) e pega a mediana
  for (int i = 0; i < validas - 1; i++) {
    for (int j = 0; j < validas - i - 1; j++) {
      if (leituras[j] > leituras[j + 1]) {
        float tmp = leituras[j];
        leituras[j] = leituras[j + 1];
        leituras[j + 1] = tmp;
      }
    }
  }
  return leituras[validas / 2];
}

float calcularNivelPercentual(float distanciaCm) {
  if (distanciaCm < 0) return -1.0; // erro de leitura

  if (distanciaCm <= (SENSOR_ATE_NIVEL_MAX_CM - MARGEM_CHEIO_CM)) return 100.0;
  if (distanciaCm >= SENSOR_ATE_VAZIA_CM) return 0.0;

  float nivel = (SENSOR_ATE_VAZIA_CM - distanciaCm) / ALTURA_UTIL_CM * 100.0;
  if (nivel < 0) nivel = 0;
  if (nivel > 100) nivel = 100;
  return nivel;
}

// ---------------- Nível de Transbordo (HW-038), em % ----------------
float calcularTransbordoPercentual(int leitura) {
  int faixa = LEITURA_CHEIA_TRANSBORDO - LEITURA_SECA_TRANSBORDO;
  if (faixa <= 0) return 0;
  float pct = (float)(leitura - LEITURA_SECA_TRANSBORDO) / faixa * 100.0;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

// ---------------- Processa uma linha recebida do ESP01 ----------------
// Formato esperado: "C:0" ou "C:1"
void processarLinhaEsp(String linha) {
  linha.trim();
  if (linha.length() < 3) return;
  if (linha.charAt(0) != 'C' || linha.charAt(1) != ':') return;

  char c = linha.charAt(2);
  if (c != '0' && c != '1') return;

  comandoDesejado = (c == '1');
  ultimoComandoRecebidoEm = millis();

  if (comandoDesejado == false) {
    precisaTransicaoParaRearmar = false; // recebeu um "desligar" explícito, pode rearmar depois
  }
}

void lerSerialEsp() {
  while (espSerial.available()) {
    char c = espSerial.read();
    if (c == '\n') {
      processarLinhaEsp(bufferSerial);
      bufferSerial = "";
    } else if (c != '\r') {
      bufferSerial += c;
      if (bufferSerial.length() > 40) bufferSerial = ""; // proteção contra lixo/overflow
    }
  }
}

// ---------------- Aplica a lógica de segurança e aciona o relé ----------------
void atualizarRele(float transbordoPct) {
  unsigned long agora = millis();
  bool estavaLigada = releLigado; // captura ANTES de qualquer mudança nesta chamada

  bool semComandoRecente =
      (ultimoComandoRecebidoEm == 0) ||
      (agora - ultimoComandoRecebidoEm > TIMEOUT_COMUNICACAO_MS);

  bool bloqueadoPorTransbordo = (transbordoPct >= TRANSBORDO_LIMIAR_DESLIGA_PCT);

  bool podeLigar = comandoDesejado && !semComandoRecente && !precisaTransicaoParaRearmar && !bloqueadoPorTransbordo;

  if (podeLigar && !releLigado) {
    // Liga a bomba
    releLigado = true;
    releLigadoDesde = agora;
    digitalWrite(PIN_RELE, RELE_NIVEL_ATIVO);
  } else if (!podeLigar && releLigado) {
    // Desliga a bomba
    releLigado = false;
    digitalWrite(PIN_RELE, RELE_NIVEL_ATIVO == HIGH ? LOW : HIGH);
  }

  // Trava de tempo máximo contínuo ligado
  if (releLigado && (agora - releLigadoDesde > TEMPO_MAX_LIGADO_MS)) {
    releLigado = false;
    digitalWrite(PIN_RELE, RELE_NIVEL_ATIVO == HIGH ? LOW : HIGH);
    precisaTransicaoParaRearmar = true; // só liga de novo depois de um C:0 explícito
    Serial.println(F("[SEGURANCA] Tempo maximo ligado atingido - bomba desligada."));
  }

  // Trava de Nível de Transbordo: se a bomba estava ligada e o nível
  // cruzou o limiar agora, exige um comando explícito de "desligar" do
  // ESP01 antes de religar (evita ligar/desligar repetidamente perto do
  // limiar - "chattering" do relé).
  if (estavaLigada && bloqueadoPorTransbordo) {
    precisaTransicaoParaRearmar = true;
    Serial.println(F("[SEGURANCA] Nivel de transbordo atingido - bomba desligada."));
  }

  // Sem comunicação com o ESP01 -> desliga por segurança
  if (releLigado && semComandoRecente) {
    releLigado = false;
    digitalWrite(PIN_RELE, RELE_NIVEL_ATIVO == HIGH ? LOW : HIGH);
    Serial.println(F("[SEGURANCA] Sem comunicacao com ESP01 - bomba desligada."));
  }
}

void loop() {
  lerSerialEsp();

  unsigned long agora = millis();
  if (agora - ultimaLeituraEm >= INTERVALO_LEITURA_MS) {
    ultimaLeituraEm = agora;

    float distancia = lerDistanciaCm();
    float nivelPct = calcularNivelPercentual(distancia);

    float temperatura = dht.readTemperature();
    float umidade = dht.readHumidity();
    bool dhtOk = !(isnan(temperatura) || isnan(umidade));

    int leituraTransbordo = analogRead(PIN_AGUA_AO);
    float transbordoPct = calcularTransbordoPercentual(leituraTransbordo);

    atualizarRele(transbordoPct);

    bool bloqueadoPorTransbordo = comandoDesejado &&
        (transbordoPct >= TRANSBORDO_LIMIAR_DESLIGA_PCT || precisaTransicaoParaRearmar);

    // Monta e envia a linha de status pro ESP01:
    // D:<distancia_bruta_cm>,<temp>,<umid>,<transbordo_pct>,<rele 0/1>,<bloqueado 0/1>
    // A distância bruta (sem transformar em %) é o que permite o ESP01
    // calcular o nível usando as medidas configuradas no dashboard.
    espSerial.print("D:");
    espSerial.print(distancia >= 0 ? distancia : -1, 1);
    espSerial.print(",");
    espSerial.print(dhtOk ? temperatura : -99, 1);
    espSerial.print(",");
    espSerial.print(dhtOk ? umidade : -99, 1);
    espSerial.print(",");
    espSerial.print(transbordoPct, 1);
    espSerial.print(",");
    espSerial.print(releLigado ? 1 : 0);
    espSerial.print(",");
    espSerial.print(bloqueadoPorTransbordo ? 1 : 0);
    espSerial.print("\n");

    // Debug via USB (nivel local, só para conferência - o dashboard usa o
    // cálculo do ESP01 com as medidas configuradas no site)
    Serial.print(F("Distancia: ")); Serial.print(distancia);
    Serial.print(F("cm  Nivel(local): ")); Serial.print(nivelPct);
    Serial.print(F("%  Transbordo: ")); Serial.print(transbordoPct);
    Serial.print(F("%  Temp: ")); Serial.print(temperatura);
    Serial.print(F("  Umid: ")); Serial.print(umidade);
    Serial.print(F("  Rele: ")); Serial.print(releLigado);
    Serial.print(F("  Bloqueado: ")); Serial.println(bloqueadoPorTransbordo);
  }
}
