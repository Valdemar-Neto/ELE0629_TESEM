#pragma once

/** @file rtos.h
 *  @brief Componente RTOS — DataLogger ESP32
 *
 *  Define todas as estruturas de dados, tipos, variáveis globais de
 *  sincronização e protótipos das tarefas do DataLogger.
 *
 *  Arquitetura Producer/Consumer:
 *    Produtores : Task_Sensores (esp_timer 500 ms) + Task_Status (2 s)
 *    Consumidor : Task_Logger  (contínua, drena qLog → SPIFFS)
 *    Sob demanda: Task_Serial  (acorda via ISR GPIO0, imprime UART)
 *
 * @author  Valdemar Neto
 * @date    2026
 */

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_timer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 1. CONFIGURAÇÕES GERAIS (ajustáveis via Kconfig / #define) */

#define SENSOR_TIMER_PERIOD_MS   500    /* Período do esp_timer da Task_Sensores (ms) */
#define STATUS_PERIOD_MS        2000    /* Período de coleta da Task_Status (ms)       */
#define QLOG_LENGTH               20    /* Capacidade máxima da fila qLog              */
#define SPIFFS_BASE_PATH      "/spiffs" /* Ponto de montagem do SPIFFS                 */
#define LOG_FILE_PATH   "/spiffs/Log.txt"    /* Arquivo de log geral                  */
#define STATUS_FILE_PATH "/spiffs/Status.txt"/* Arquivo de status do sistema           */
#define DHT11_GPIO_PIN             4    /* Pino GPIO do DHT11                          */
#define BUTTON_GPIO_PIN            0    /* Pino GPIO do botão (ISR)                    */
#define VCC_ADC_CHANNEL            0    /* Canal ADC para leitura de Vcc               */
#define HEAP_WARN_THRESHOLD    10240    /* Heap mínimo antes de gerar WARN (bytes)     */
#define STACK_WARN_THRESHOLD     100    /* highWaterMark mínimo antes de ERROR (words) */
#define SENSOR_MAX_ERRORS          3    /* Falhas consecutivas antes de ERROR crítico  */
#define SPIFFS_WARN_FREE_KB        4    /* Espaço livre mínimo no SPIFFS antes de WARN */

/* 2. ENUMERAÇÃO DE NÍVEIS DE LOG */

/**
 * @brief Níveis de classificação de eventos para o DataLogger.
 *
 *  - LOG_INFO   : fluxo normal, inicializações, confirmações
 *  - LOG_WARN   : situação degradada mas sistema segue operando
 *  - LOG_ERROR  : falha que compromete funcionalidade
 *  - LOG_SENSOR : dado bruto coletado pelo DHT11 / ADC
 *  - LOG_STATUS : snapshot do sistema (heap, tasks, uptime)
 */
typedef enum {
    LOG_INFO   = 0,
    LOG_WARN,
    LOG_ERROR,
    LOG_SENSOR,
    LOG_STATUS,
} log_level_t;

/* 3. ESTRUTURA DE ENTRADA DE LOG  (trafega na fila qLog) */

/**
 * @brief Entrada de log — unidade de comunicação entre produtores e Task_Logger.
 *
 *  Sempre copiada por valor na fila (nunca por ponteiro) para evitar
 *  acesso a memória inválida após retorno de função.
 *
 *  Tamanho: 8 + 4 + 16 + 128 = 156 bytes por entrada.
 *  Fila de 20 entradas → ~3 KB de heap alocado pelo FreeRTOS.
 */
typedef struct {
    uint64_t     timestamp;    /**< µs desde boot — esp_timer_get_time()              */
    log_level_t  level;        /**< Nível do evento: INFO / WARN / ERROR / SENSOR / STATUS */
    char         tag[16];      /**< Origem: "SENSOR", "STATUS", "LOGGER", "SERIAL"    */
    char         message[128]; /**< Texto do evento, ex: "Temp=25C Umid=60% Vcc=3.3V" */
} log_entry_t;

/* 4. ESTRUTURA DE STATUS DO SISTEMA  (Task_Status) */

/**
 * @brief Snapshot do estado do sistema coletado pela Task_Status a cada 2 s.
 *
 *  Todos os campos são preenchidos em uma única coleta e então
 *  serializados em uma log_entry_t{LOG_STATUS} enviada para qLog.
 */
typedef struct {
    /* Tempo de operação */
    uint64_t  uptime_us;            /**< Microssegundos desde boot (esp_timer_get_time)     */

    /* Memória heap */
    uint32_t  heap_free;            /**< Heap livre atual em bytes                           */
    uint32_t  heap_min_ever;        /**< Menor heap já registrado desde o boot               */

    /* Tarefas FreeRTOS */
    UBaseType_t  task_count;        /**< Número de tarefas ativas no momento                 */
    TaskStatus_t task_list[10];     /**< Estado detalhado de até 10 tarefas                  */

    /* Uso das variáveis de log (filas) */
    UBaseType_t  qlog_waiting;      /**< Entradas aguardando na qLog no momento da coleta    */
    UBaseType_t  qlog_spaces_free;  /**< Espaços livres restantes na qLog                    */

    /* Reset reason */
    uint32_t  reset_reason;         /**< Código de reset — esp_reset_reason_t                */
} system_status_t;

/* 5. VARIÁVEIS GLOBAIS DE SINCRONIZAÇÃO
 *    (definidas em rtos.c, declaradas extern aqui) */

extern QueueHandle_t      qLog;          /**< Fila principal: produtores → Task_Logger        */
extern SemaphoreHandle_t  mutex_log;     /**< Mutex: protege acesso a Log.txt e Status.txt    */
extern TaskHandle_t       h_task_serial; /**< Handle da Task_Serial para xTaskNotifyFromISR   */

/* 6. PROTÓTIPOS — funções públicas do componente RTOS */

/**
 * @brief Inicializa todo o sistema DataLogger.
 *
 *  Deve ser chamada uma única vez em app_main().
 *  Responsabilidades:
 *    - Monta SPIFFS
 *    - Cria qLog e mutex_log
 *    - Inicializa driver DHT11
 *    - Configura ISR do botão GPIO
 *    - Cria as quatro tarefas FreeRTOS
 *    - Arma o esp_timer da Task_Sensores
 *
 * @return ESP_OK em caso de sucesso, código de erro caso contrário.
 */
esp_err_t rtos_init(void);

/**
 * @brief Enfileira uma entrada de log na qLog sem bloquear.
 *
 *  Utilitário interno usado pelas tarefas produtoras. Em caso de fila
 *  cheia, gera um evento WARN no próprio log (via logger_internal_log).
 *
 * @param entry  Ponteiro para a entrada a ser copiada na fila.
 * @return pdTRUE se enfileirado, pdFALSE se fila cheia.
 */
BaseType_t rtos_log_send(const log_entry_t *entry);

/**
 * @brief Registra evento interno da Task_Logger (logger_internal_log).
 *
 *  Equivale a chamar rtos_log_send com tag="LOGGER" e timestamp automático.
 *
 * @param level    Nível do evento.
 * @param message  Mensagem descritiva.
 */
void logger_internal_log(log_level_t level, const char *message);

#ifdef __cplusplus
}
#endif