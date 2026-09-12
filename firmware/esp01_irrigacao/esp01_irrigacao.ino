/*
  ====================================================================
  IRRIGADOR COM NÍVEL - Firmware do ESP01 (ESP8266)
  ====================================================================
  Função deste ESP01:
    - Conectar no WiFi
    - Pegar a hora certa pela internet (NTP)
    - Ler o status enviado pelo Nano (nível, temperatura, umidade,
      Nível de Transbordo, estado do relé) e publicar no Firebase
      Realtime Database, pra o site poder mostrar em tempo real
    - Ler do Firebase o comando manual (botão do site) e os até 2
      horários programados, decidir se a bomba deve ligar agora, e
      mandar esse comando pro Nano (que aplica as travas de segurança)

  Este ESP01 roda um sketch PRÓPRIO (não o firmware AT de fábrica).
  Ele precisa ser gravado separadamente do Nano - veja o README para
  o passo a passo de gravação (precisa de um adaptador USB-Serial ou
  do próprio Nano "emprestado" como programador).

  ATENÇÃO - REQUISITO DE MEMÓRIA:
  Este sketch usa HTTPS (TLS), que ocupa bastante flash. Um ESP-01
  antigo de 512KB pode não ter espaço suficiente. Use um ESP-01S de
  1MB (a versão mais comum vendida atualmente) e, na IDE do Arduino,
  em Ferramentas > Flash Size, escolha "1M (FS:64KB OTA:~470KB)" ou
  equivalente.

  Bibliotecas necessárias (Gerenciador de Bibliotecas do Arduino IDE):
    - Suporte de placas "esp8266 by ESP8266 Community" (Boards Manager)
    - "ArduinoJson" (Benoit Blanchon), versão 6.x ou 7.x

  ====================================================================
  LIGAÇÃO (resumo - veja o diagrama completo no README)
  ====================================================================
    ESP01 VCC   -> fonte 3.3V À PARTE (NÃO usar o 3.3V do Nano)
    ESP01 GND   -> mesmo GND do Nano (obrigatório para o serial funcionar)
    ESP01 CH_PD -> 3.3V (direto ou com resistor de 10k)
    ESP01 GPIO0 -> 3.3V com resistor de 10k (modo de execução normal)
    ESP01 TX    -> Nano D8 (RX do SoftwareSerial) - ligação direta
    ESP01 RX    <- Nano D9 (TX do SoftwareSerial) - ATRAVÉS de um
                   divisor de tensão 1k/2k (5V do Nano -> RX do ESP01
                   sem divisor pode danificar o ESP01)
  ====================================================================
*/

#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <SoftwareSerial.h>
#include <ArduinoJson.h>
#include <time.h>

#include "config.h"   // copie de config_exemplo.h e preencha

// ---------------- Serial com o Nano ----------------
// O ESP01 só tem os pinos TX/RX (GPIO1/GPIO3) - usamos o Serial de
// hardware dele mesmo para falar com o Nano (por isso não tem
// "Serial.println" de debug aqui: a única serial do ESP01 é essa).
#define BAUD_NANO 9600

// ---------------- Configurações de tempo ----------------
// America/Sao_Paulo = UTC-3, sem horário de verão atualmente.
const long GMT_OFFSET_SEC = -3 * 3600;
const int  DST_OFFSET_SEC = 0;
const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "a.st1.ntp.br";

// ---------------- Intervalos ----------------
const unsigned long INTERVALO_BUSCA_CONFIG_MS = 15000UL;  // lê /config.json
const unsigned long INTERVALO_ENVIO_STATUS_MS = 10000UL;  // escreve /status.json
const unsigned long MANUAL_OVERRIDE_MAX_SEC   = 2UL * 60UL; // 2 min

// ---------------- Calibração da caixa d'água (configurável pelo site) -----
// O Nano só manda a distância BRUTA (cm) lida pelo HC-SR04. É o ESP01 quem
// transforma essa distância em % e litros, usando estas 3 medidas - que
// podem ser alteradas a qualquer momento pelo card "Configuração da caixa
// d'água" no dashboard (gravadas em /config/tank no Firebase), sem precisar
// regravar nenhum dos dois firmwares. Os valores abaixo são só o ponto de
// partida (mesmas medidas informadas para esta caixa de 5L) até a primeira
// configuração salva pelo site chegar.
struct TankConfig {
  float volumeL     = 5.0;   // volume útil da caixa, em litros
  float distFundoCm = 30.0;  // distância do sensor até o FUNDO (caixa vazia)
  float distCheioCm = 5.0;   // distância do sensor até a água no nível MÁXIMO
};
TankConfig tank;

// Zona morta de segurança perto do sensor (mesmo motivo do Nano: o HC-SR04
// tem alcance mínimo de ~2cm).
const float MARGEM_CHEIO_CM = 1.0;

// ---------------- Estado local (cache) ----------------
struct Horario {
  bool enabled = false;
  int hour = 0;
  int minute = 0;
  int duration_min = 5;
};

Horario horarios[2];
String manualCommand = "auto"; // "auto" | "on" | "off"
unsigned long manualRequestedAtEpoch = 0;

bool wifiOk = false;
bool horaSincronizada = false;

unsigned long ultimaBuscaConfig = 0;
unsigned long ultimoEnvioStatus = 0;

// Últimos dados recebidos do Nano (ultDistancia é a distância BRUTA em cm;
// ultTransbordoPct é o Nível de Transbordo do HW-038, já em %)
float ultDistancia = -1, ultTemp = -99, ultUmid = -99, ultTransbordoPct = -1;
bool ultRele = false, ultBloqueado = false;
bool dadosNanoValidos = false;

String bufferNano = "";

// ---------------- WiFi ----------------
void conectarWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_SENHA);

  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < 20000UL) {
    delay(250);
  }
  wifiOk = (WiFi.status() == WL_CONNECTED);
}

void garantirWifi() {
  if (WiFi.status() != WL_CONNECTED) {
    wifiOk = false;
    conectarWifi();
  } else {
    wifiOk = true;
  }
}

void sincronizarHora() {
  configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2);
  struct tm timeinfo;
  // getLocalTime tenta por alguns segundos e retorna false se não conseguir
  horaSincronizada = getLocalTime(&timeinfo, 8000);
}

// ---------------- HTTP helpers (Firebase REST) ----------------
String montarUrl(const String &caminho) {
  String url = String(FIREBASE_DATABASE_URL) + caminho + ".json";
  if (strlen(FIREBASE_AUTH_TOKEN) > 0) {
    url += "?auth=";
    url += FIREBASE_AUTH_TOKEN;
  }
  return url;
}

// Faz GET e devolve o corpo da resposta (String vazia em caso de erro)
String firebaseGet(const String &caminho) {
  if (!wifiOk) return "";

  WiFiClientSecure client;
  client.setInsecure(); // simplicidade: não valida o certificado do Firebase
  HTTPClient http;

  String url = montarUrl(caminho);
  String resposta = "";

  if (http.begin(client, url)) {
    int codigo = http.GET();
    if (codigo == 200) {
      resposta = http.getString();
    }
    http.end();
  }
  return resposta;
}

bool firebasePut(const String &caminho, const String &jsonBody) {
  if (!wifiOk) return false;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  String url = montarUrl(caminho);
  bool ok = false;

  if (http.begin(client, url)) {
    http.addHeader("Content-Type", "application/json");
    int codigo = http.PUT(jsonBody);
    ok = (codigo == 200);
    http.end();
  }
  return ok;
}

// ---------------- Busca /config.json (schedule + manual) ----------------
void buscarConfiguracao() {
  String json = firebaseGet("/config");
  if (json.length() == 0 || json == "null") return;

  StaticJsonDocument<512> doc;
  DeserializationError erro = deserializeJson(doc, json);
  if (erro) return;

  if (!doc["manual"].isNull()) {
    manualCommand = doc["manual"]["command"] | "auto";
    manualRequestedAtEpoch = doc["manual"]["requestedAt"] | 0UL;
  }

  if (!doc["schedule"].isNull()) {
    JsonObject sched = doc["schedule"];
    for (int i = 0; i < 2; i++) {
      String chave = String(i + 1);
      if (!sched[chave].isNull()) {
        horarios[i].enabled = sched[chave]["enabled"] | false;
        horarios[i].hour = sched[chave]["hour"] | 0;
        horarios[i].minute = sched[chave]["minute"] | 0;
        horarios[i].duration_min = sched[chave]["duration_min"] | 5;
      }
    }
  }

  if (!doc["tank"].isNull()) {
    float volumeL     = doc["tank"]["volume_l"]      | tank.volumeL;
    float distFundoCm = doc["tank"]["dist_fundo_cm"] | tank.distFundoCm;
    float distCheioCm = doc["tank"]["dist_cheio_cm"] | tank.distCheioCm;

    // Validação básica: só aplica se fizer sentido físico (evita que um
    // valor incompleto/errado no site derrube o cálculo do nível).
    bool valido = (volumeL > 0) &&
                  (distFundoCm > 0) && (distCheioCm >= 0) &&
                  (distFundoCm - distCheioCm) > 1.0;

    if (valido) {
      tank.volumeL = volumeL;
      tank.distFundoCm = distFundoCm;
      tank.distCheioCm = distCheioCm;
    }
  }
}

// ---------------- Calcula nível % a partir da distância bruta do Nano -----
// Usa as medidas de "tank" (configuráveis pelo dashboard), do mesmo jeito
// que o Nano fazia localmente antes com valores fixos.
float calcularNivelPercentual(float distanciaCm) {
  if (distanciaCm < 0) return -1.0; // erro de leitura do sensor

  float alturaUtilCm = tank.distFundoCm - tank.distCheioCm;
  if (alturaUtilCm <= 0) return -1.0; // configuração inválida

  if (distanciaCm <= (tank.distCheioCm - MARGEM_CHEIO_CM)) return 100.0;
  if (distanciaCm >= tank.distFundoCm) return 0.0;

  float nivel = (tank.distFundoCm - distanciaCm) / alturaUtilCm * 100.0;
  if (nivel < 0) nivel = 0;
  if (nivel > 100) nivel = 100;
  return nivel;
}

// Se o override manual expirou, volta pro modo automático (e avisa o Firebase)
void checarExpiracaoManual(unsigned long agoraEpoch) {
  if (manualCommand == "auto") return;
  if (manualRequestedAtEpoch == 0) return;

  if (agoraEpoch >= manualRequestedAtEpoch &&
      (agoraEpoch - manualRequestedAtEpoch) > MANUAL_OVERRIDE_MAX_SEC) {
    manualCommand = "auto";
    firebasePut("/config/manual/command", "\"auto\"");
  }
}

// ---------------- Decide se a bomba deve ligar agora ----------------
bool decidirLigarBomba() {
  if (manualCommand == "on") return true;
  if (manualCommand == "off") return false;

  // modo "auto": olha os horários programados
  if (!horaSincronizada) return false; // sem hora certa, não arrisca ligar por horário

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 1000)) return false;

  int agoraMin = timeinfo.tm_hour * 60 + timeinfo.tm_min;

  for (int i = 0; i < 2; i++) {
    if (!horarios[i].enabled) continue;
    int inicioMin = horarios[i].hour * 60 + horarios[i].minute;
    int fimMin = inicioMin + horarios[i].duration_min;

    if (fimMin <= 1440) {
      if (agoraMin >= inicioMin && agoraMin < fimMin) return true;
    } else {
      // janela cruza a meia-noite (ex.: começa 23:58 e dura 5 min)
      if (agoraMin >= inicioMin || agoraMin < (fimMin - 1440)) return true;
    }
  }
  return false;
}

// ---------------- Comunicação com o Nano ----------------
void enviarComandoNano(bool ligar) {
  Serial.print("C:");
  Serial.print(ligar ? "1" : "0");
  Serial.print("\n");
}

void processarLinhaNano(String linha) {
  linha.trim();
  if (!linha.startsWith("D:")) return;

  // D:<distancia_bruta_cm>,<temp>,<umid>,<transbordo_pct>,<rele>,<bloqueado>
  linha.remove(0, 2);
  float valores[6];
  int idx = 0;
  int inicio = 0;
  for (int i = 0; i <= linha.length() && idx < 6; i++) {
    if (i == linha.length() || linha.charAt(i) == ',') {
      valores[idx++] = linha.substring(inicio, i).toFloat();
      inicio = i + 1;
    }
  }
  if (idx < 6) return;

  ultDistancia = valores[0];
  ultTemp = valores[1];
  ultUmid = valores[2];
  ultTransbordoPct = valores[3];
  ultRele = (valores[4] != 0);
  ultBloqueado = (valores[5] != 0);
  dadosNanoValidos = true;
}

void lerSerialNano() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      processarLinhaNano(bufferNano);
      bufferNano = "";
    } else if (c != '\r') {
      bufferNano += c;
      if (bufferNano.length() > 60) bufferNano = "";
    }
  }
}

// ---------------- Publica status no Firebase ----------------
void enviarStatus(bool bombaComandada, unsigned long agoraEpoch) {
  if (!dadosNanoValidos) return;

  // Recalcula sempre com o "tank" mais recente (pode ter chegado uma
  // configuração nova do site desde a última leitura do Nano).
  float nivelPct = calcularNivelPercentual(ultDistancia);
  float volumeLitros = (nivelPct >= 0) ? (nivelPct / 100.0 * tank.volumeL) : -1;

  String json = "{";
  json += "\"level_pct\":" + String(nivelPct, 1) + ",";
  json += "\"volume_l\":" + String(volumeLitros, 2) + ",";
  json += "\"distance_cm\":" + String(ultDistancia, 1) + ",";
  json += "\"temp_c\":" + String(ultTemp, 1) + ",";
  json += "\"humidity_pct\":" + String(ultUmid, 1) + ",";
  json += "\"overflow_pct\":" + String(ultTransbordoPct, 1) + ",";
  json += "\"pump_on\":" + String(ultRele ? "true" : "false") + ",";
  json += "\"pump_commanded\":" + String(bombaComandada ? "true" : "false") + ",";
  json += "\"blocked_overflow\":" + String(ultBloqueado ? "true" : "false") + ",";
  json += "\"mode\":\"" + manualCommand + "\",";
  json += "\"wifi_ok\":" + String(wifiOk ? "true" : "false") + ",";
  json += "\"last_update\":" + String(agoraEpoch);
  json += "}";

  firebasePut("/status", json);
}

void setup() {
  Serial.begin(BAUD_NANO); // essa é a serial que fala com o Nano

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH); // LED do ESP01 é invertido (LOW = aceso)

  conectarWifi();
  if (wifiOk) sincronizarHora();
}

void loop() {
  lerSerialNano();
  garantirWifi();

  if (wifiOk && !horaSincronizada) sincronizarHora();

  unsigned long agora = millis();
  unsigned long agoraEpoch = time(nullptr);

  if (wifiOk && (agora - ultimaBuscaConfig >= INTERVALO_BUSCA_CONFIG_MS)) {
    ultimaBuscaConfig = agora;
    buscarConfiguracao();
    checarExpiracaoManual(agoraEpoch);
  }

  // Se não tem WiFi, manda desligar por segurança (não arrisca comando "cego")
  bool ligar = wifiOk ? decidirLigarBomba() : false;
  enviarComandoNano(ligar);

  if (agora - ultimoEnvioStatus >= INTERVALO_ENVIO_STATUS_MS) {
    ultimoEnvioStatus = agora;
    enviarStatus(ligar, agoraEpoch);
  }

  digitalWrite(LED_BUILTIN, wifiOk ? LOW : HIGH); // aceso = WiFi ok

  delay(200);
}
