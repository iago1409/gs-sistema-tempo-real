#include <WiFi.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

// -------- CONFIGURAÇÃO DE REDE ----------
const char* ssid     = "Wokwi-GUEST";  // Rede do simulador
const char* password = "";             // Sem senha no Wokwi

// Timeout de conexão (robustez)
const uint32_t WIFI_CONNECT_TIMEOUT_MS   = 15000; // 15s no setup
const uint32_t WIFI_RECONNECT_TIMEOUT_MS = 10000; // 10s nas reconexões

// -------- ESTRUTURAS DE DADOS ----------
struct WifiInfo {
  char ssid[33];  // SSID máx. 32 chars + '\0'
};

// Fila para comunicação entre tarefas
QueueHandle_t wifiQueue;

// Semáforo para proteger a lista de redes seguras
SemaphoreHandle_t listaMutex;

// Lista de redes seguras (precisa de pelo menos 5)
const char* redesSeguras[] = {
  "Wokwi-GUEST",
  "RedeEmpresa",
  "RedeCasa",
  "LabFIAP",
  "MinhaRedeSegura"
};
const int numRedesSeguras = sizeof(redesSeguras) / sizeof(redesSeguras[0]);

// -------- PROTÓTIPOS DAS TAREFAS ----------
void taskMonitorWiFi(void* param);
void taskVerificaSeguranca(void* param);
void taskHeartbeat(void* param);

// -------- PROTÓTIPOS DE FUNÇÕES DE ROBUSTEZ ----------
bool conectarWiFiComTimeout(uint32_t timeoutMs);

// =========================================
// FUNÇÃO DE ROBUSTEZ: CONEXÃO COM TIMEOUT
// =========================================
bool conectarWiFiComTimeout(uint32_t timeoutMs) {
  WiFi.begin(ssid, password);
  Serial.print("[WiFi] Tentando conectar em ");
  Serial.print(ssid);

  uint32_t inicio = millis();

  while (WiFi.status() != WL_CONNECTED &&
         (millis() - inicio) < timeoutMs) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Conectado com sucesso!");
    Serial.print("[WiFi] SSID atual: ");
    Serial.println(WiFi.SSID());
    return true;
  } else {
    Serial.println("\n[WiFi] ERRO: Timeout ao tentar conectar.");
    return false;
  }
}

// =========================================
// SETUP
// =========================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n[SETUP] Iniciando GS2 - Monitor de Redes Wi-Fi Seguras");

  WiFi.mode(WIFI_STA);

  // Usa a função de robustez com timeout
  bool conectado = conectarWiFiComTimeout(WIFI_CONNECT_TIMEOUT_MS);
  if (!conectado) {
    Serial.println("[SETUP] Aviso: sistema será iniciado mesmo sem Wi-Fi,");
    Serial.println("[SETUP] a tarefa de monitoramento tentará recuperar a conexão.");
  }

  // Criação da fila (até 5 mensagens do tipo WifiInfo)
  wifiQueue = xQueueCreate(5, sizeof(WifiInfo));
  if (wifiQueue == NULL) {
    Serial.println("[ERRO] Falha ao criar fila wifiQueue!");
    while (true) {}
  }

  // Criação do semáforo (mutex) para a lista de redes seguras
  listaMutex = xSemaphoreCreateMutex();
  if (listaMutex == NULL) {
    Serial.println("[ERRO] Falha ao criar mutex listaMutex!");
    while (true) {}
  }

  // Criação das tarefas FreeRTOS
  xTaskCreate(
    taskMonitorWiFi,       // Função
    "MonitorWiFi",         // Nome
    4096,                  // Stack
    NULL,                  // Param
    2,                     // Prioridade
    NULL                   // Handle
  );

  xTaskCreate(
    taskVerificaSeguranca,
    "SegurancaWiFi",
    4096,
    NULL,
    3,   // prioridade maior (tarefa mais crítica)
    NULL
  );

  xTaskCreate(
    taskHeartbeat,
    "Heartbeat",
    2048,
    NULL,
    1,   // prioridade menor
    NULL
  );

  Serial.println("[SETUP] Tarefas FreeRTOS criadas. Sistema iniciado.");
}

// loop fica vazio, tudo roda nas tarefas
void loop() {
  // Não usamos o loop() no modelo com FreeRTOS
  vTaskDelay(pdMS_TO_TICKS(1000));
}

// =========================================
// TAREFA 1 - MONITORAR Wi-Fi E ENVIAR SSID PARA A FILA
// =========================================
void taskMonitorWiFi(void* param) {
  (void)param;

  WifiInfo info;

  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      String atual = WiFi.SSID();
      atual.toCharArray(info.ssid, sizeof(info.ssid));

      // Envia SSID para a fila
      xQueueSend(wifiQueue, &info, portMAX_DELAY);

      Serial.print("[MonitorWiFi] SSID enviado para fila: ");
      Serial.println(info.ssid);
    } else {
      Serial.println("[MonitorWiFi] Wi-Fi desconectado! Tentando recuperar conexao...");

      // Estratégia de recuperação com timeout
      bool reconectou = conectarWiFiComTimeout(WIFI_RECONNECT_TIMEOUT_MS);
      if (!reconectou) {
        Serial.println("[MonitorWiFi] Falha na reconexao. Nova tentativa em alguns segundos.");
      }
    }

    // Verifica a cada 3 segundos
    vTaskDelay(pdMS_TO_TICKS(3000));
  }
}

// =========================================
// TAREFA 2 - VERIFICAR SE A REDE É SEGURA
// =========================================
void taskVerificaSeguranca(void* param) {
  (void)param;

  WifiInfo recebido;

  for (;;) {
    // Espera até receber um SSID na fila
    if (xQueueReceive(wifiQueue, &recebido, portMAX_DELAY) == pdTRUE) {

      bool segura = false;

      // Protege o acesso à lista de redes seguras com o mutex
      xSemaphoreTake(listaMutex, portMAX_DELAY);
      for (int i = 0; i < numRedesSeguras; i++) {
        if (strcmp(recebido.ssid, redesSeguras[i]) == 0) {
          segura = true;
          break;
        }
      }
      xSemaphoreGive(listaMutex);

      if (segura) {
        Serial.print("[SegurancaWiFi] [OK] Rede segura: ");
        Serial.println(recebido.ssid);
      } else {
        Serial.print("[SegurancaWiFi] [ALERTA] Rede NAO segura: ");
        Serial.println(recebido.ssid);
      }
    }
  }
}

// =========================================
// TAREFA 3 - HEARTBEAT / MONITOR DO SISTEMA
// =========================================
void taskHeartbeat(void* param) {
  (void)param;

  for (;;) {
    Serial.println("[Heartbeat] Sistema rodando normalmente...");
    vTaskDelay(pdMS_TO_TICKS(5000));  // a cada 5 segundos
  }
}
