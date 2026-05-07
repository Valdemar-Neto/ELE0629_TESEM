/** @file rtos.c
 *  @brief Implementação do componente RTOS — DataLogger ESP32
 *
 *  Contém:
 *    - Variáveis globais de sincronização (qLog, mutex_log, h_task_serial)
 *    - Task_Sensores  : acionada por esp_timer a cada 500 ms
 *    - Task_Status    : coleta estado do sistema a cada 2 s (vTaskDelay)
 *    - Task_Logger    : consumidor da fila qLog, grava em SPIFFS
 *    - Task_Serial    : acorda via ISR GPIO0, imprime Log.txt + Status.txt na UART
 *    - rtos_init()    : ponto de entrada único chamado por app_main()
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_spiffs.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/gpio.h"
#include "esp_system.h"

#include "rtos.h"
#include "dht11.h"

/* TAGS DE LOG  */
static const char *TAG_MAIN    = "RTOS_INIT";
static const char *TAG_SENSOR  = "SENSOR";
static const char *TAG_STATUS  = "STATUS";
static const char *TAG_LOGGER  = "LOGGER";
static const char *TAG_SERIAL  = "SERIAL";

/* VARIÁVEIS GLOBAIS DE SINCRONIZAÇÃO */
QueueHandle_t      qLog          = NULL;
SemaphoreHandle_t  mutex_log     = NULL;
TaskHandle_t       h_task_serial = NULL;

/* VARIÁVEIS INTERNAS */
static dht11_handle_t    s_dht11;          /* handle do driver DHT11              */
static esp_timer_handle_t s_sensor_timer;  /* esp_timer que dispara Task_Sensores */
static adc_oneshot_unit_handle_t s_adc;    /* handle do ADC oneshot               */

/* UTILITÁRIOS INTERNOS */

/**
 * @brief Constrói e enfileira uma log_entry_t na qLog.
 *        Não bloqueia — se a fila estiver cheia descarta e loga WARN.
 */
BaseType_t rtos_log_send(const log_entry_t *entry)
{
    if (qLog == NULL || entry == NULL) return pdFALSE;

    BaseType_t sent = xQueueSend(qLog, entry, 0);
    if (sent != pdTRUE) {
        /* Não chama rtos_log_send recursivamente — apenas loga no console */
        ESP_LOGW(TAG_SENSOR, "qLog cheia — entrada descartada (tag=%s)", entry->tag);
    }
    return sent;
}

/**
 * @brief Registra evento interno originado pela própria Task_Logger.
 *        Implementa o logger_internal_log() exigido pela atividade.
 */
void logger_internal_log(log_level_t level, const char *message)
{
    log_entry_t e = {
        .timestamp = (uint64_t)esp_timer_get_time(),
        .level     = level,
    };
    strncpy(e.tag,     "LOGGER",  sizeof(e.tag)     - 1);
    strncpy(e.message, message,   sizeof(e.message) - 1);
    rtos_log_send(&e);
}

/**
 * @brief Retorna string do nível para serialização no arquivo.
 */
static const char *level_str(log_level_t l)
{
    switch (l) {
        case LOG_INFO:   return "INFO";
        case LOG_WARN:   return "WARN";
        case LOG_ERROR:  return "ERROR";
        case LOG_SENSOR: return "SENSOR";
        case LOG_STATUS: return "STATUS";
        default:         return "UNKNOWN";
    }
}

/* ISR — BOTÃO GPIO0 */

/**
 * @brief ISR do botão: notifica Task_Serial via xTaskNotifyFromISR.
 *        Executa em contexto de interrupção — não bloqueia.
 */
static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (h_task_serial != NULL) {
        vTaskNotifyGiveFromISR(h_task_serial, &xHigherPriorityTaskWoken);
    }
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

/* TASK_SENSORES — callback do esp_timer  */

/**
 * @brief Callback do esp_timer disparado a cada 500 ms.
 *
 *  Executa no contexto de tarefa do esp_timer (alta prioridade).
 *  Lê DHT11 e ADC, monta log_entry_t e envia para qLog.
 *
 *  Eventos gerados:
 *    [SENSOR / INFO  ] Inicialização
 *    [SENSOR / SENSOR] Leitura válida de Temp + Umid + Vcc
 *    [SENSOR / ERROR ] Falha de leitura (timeout / CRC)
 *    [SENSOR / WARN  ] 3 falhas consecutivas
 *    [SENSOR / WARN  ] Fila qLog cheia
 *    [ADC    / WARN  ] Vcc abaixo do limiar
 */
static void sensor_timer_callback(void *arg)
{
    static int s_error_count = 0;

    dht11_data_t data;
    log_entry_t  entry = {0};

    entry.timestamp = (uint64_t)esp_timer_get_time();

    /* ---- Leitura DHT11 ---- */
    esp_err_t err = dht11_read(&s_dht11, &data);

    if (err == ESP_OK) {
        s_error_count = 0;

        /* Leitura ADC (Vcc) */
        int raw = 0;
        float vcc = 3.3f; /* fallback se ADC falhar */
        if (adc_oneshot_read(s_adc, VCC_ADC_CHANNEL, &raw) == ESP_OK) {
            /* ESP32: resolução 12 bits, referência 3.3 V */
            vcc = (raw / 4095.0f) * 3.3f;
        }

        /* Monta mensagem de leitura válida */
        snprintf(entry.message, sizeof(entry.message),
                 "Temp=%dC Umid=%d%% Vcc=%.2fV",
                 data.temperature_int,
                 data.humidity_int,
                 vcc);
        entry.level = LOG_SENSOR;
        strncpy(entry.tag, "TEMP", sizeof(entry.tag) - 1);
        rtos_log_send(&entry);

        ESP_LOGI(TAG_SENSOR, "%s", entry.message);

        /* Verifica subtensão */
        if (vcc < 3.0f) {
            log_entry_t warn = {
                .timestamp = (uint64_t)esp_timer_get_time(),
                .level     = LOG_WARN,
            };
            strncpy(warn.tag, "ADC", sizeof(warn.tag) - 1);
            snprintf(warn.message, sizeof(warn.message),
                     "Vcc=%.2fV abaixo de 3.0V — instabilidade possivel", vcc);
            rtos_log_send(&warn);
            ESP_LOGW(TAG_SENSOR, "%s", warn.message);
        }

    } else {
        /* Falha na leitura */
        s_error_count++;
        entry.level = LOG_ERROR;
        strncpy(entry.tag, "SENSOR", sizeof(entry.tag) - 1);
        snprintf(entry.message, sizeof(entry.message),
                 "dht11_read falhou: %s (falha %d)",
                 esp_err_to_name(err), s_error_count);
        rtos_log_send(&entry);
        ESP_LOGE(TAG_SENSOR, "%s", entry.message);

        /* Alerta após 3 falhas consecutivas */
        if (s_error_count >= SENSOR_MAX_ERRORS) {
            log_entry_t crit = {
                .timestamp = (uint64_t)esp_timer_get_time(),
                .level     = LOG_ERROR,
            };
            strncpy(crit.tag, "SENSOR", sizeof(crit.tag) - 1);
            snprintf(crit.message, sizeof(crit.message),
                     "%d falhas consecutivas no DHT11 — sensor sem resposta",
                     s_error_count);
            rtos_log_send(&crit);
            ESP_LOGE(TAG_SENSOR, "%s", crit.message);
            s_error_count = 0; /* reseta para não spam infinito */
        }
    }
}

/* TASK_STATUS  */

/**
 * @brief Coleta estado completo do sistema a cada 2 s e envia para qLog.
 *
 *  Campos coletados (system_status_t):
 *    - uptime_us        : esp_timer_get_time()
 *    - heap_free        : esp_get_free_heap_size()
 *    - heap_min_ever    : esp_get_minimum_free_heap_size()
 *    - task_count       : uxTaskGetNumberOfTasks()
 *    - task_list[]      : uxTaskGetSystemState() — estado + stack de cada tarefa
 *    - qlog_waiting     : uxQueueMessagesWaiting(qLog)
 *    - qlog_spaces_free : uxQueueSpacesAvailable(qLog)
 *    - reset_reason     : esp_reset_reason()
 *
 *  Eventos gerados:
 *    [STATUS / INFO  ] Inicialização da tarefa
 *    [STATUS / STATUS] Coleta válida a cada 2 s
 *    [STATUS / WARN  ] Heap livre abaixo de HEAP_WARN_THRESHOLD
 *    [STATUS / ERROR ] Stack de alguma tarefa abaixo de STACK_WARN_THRESHOLD
 *    [STATUS / ERROR ] Reset reason diferente de POWERON/SW
 *    [STATUS / WARN  ] Falha ao enviar para qLog (fila cheia)
 */
static void task_status(void *pvParameters)
{
    ESP_LOGI(TAG_STATUS, "Task_Status iniciada — coleta a cada %d ms", STATUS_PERIOD_MS);

    /* Evento de inicialização */
    log_entry_t init_evt = {
        .timestamp = (uint64_t)esp_timer_get_time(),
        .level     = LOG_INFO,
    };
    strncpy(init_evt.tag,     "STATUS",                              sizeof(init_evt.tag)     - 1);
    strncpy(init_evt.message, "Task_Status iniciada — coleta a cada 2000 ms", sizeof(init_evt.message) - 1);
    rtos_log_send(&init_evt);

    /* Detecta reset reason uma vez na inicialização */
    esp_reset_reason_t reason = esp_reset_reason();
    if (reason != ESP_RST_POWERON && reason != ESP_RST_SW) {
        log_entry_t rst = {
            .timestamp = (uint64_t)esp_timer_get_time(),
            .level     = LOG_ERROR,
        };
        strncpy(rst.tag, "STATUS", sizeof(rst.tag) - 1);
        snprintf(rst.message, sizeof(rst.message),
                 "Reset reason=%d — reinicio inesperado detectado", (int)reason);
        rtos_log_send(&rst);
        ESP_LOGE(TAG_STATUS, "%s", rst.message);
    }

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(STATUS_PERIOD_MS));

        /* ---- Preenche system_status_t ---- */
        system_status_t status = {0};

        status.uptime_us       = (uint64_t)esp_timer_get_time();
        status.heap_free       = esp_get_free_heap_size();
        status.heap_min_ever   = esp_get_minimum_free_heap_size();
        status.task_count      = uxTaskGetNumberOfTasks();
        status.qlog_waiting    = uxQueueMessagesWaiting(qLog);
        status.qlog_spaces_free= uxQueueSpacesAvailable(qLog);
        status.reset_reason    = (uint32_t)reason;

        /* Coleta estado das tarefas (até 10) */
        UBaseType_t n = status.task_count < 10 ? status.task_count : 10;
        unsigned long total_runtime;
        uxTaskGetSystemState(status.task_list, n, &total_runtime);

        /* ---- Evento de coleta válida ---- */
        log_entry_t entry = {
            .timestamp = status.uptime_us,
            .level     = LOG_STATUS,
        };
        strncpy(entry.tag, "STATUS", sizeof(entry.tag) - 1);
        snprintf(entry.message, sizeof(entry.message),
                 "Up=%llus Heap=%" PRIu32 "B MinHeap=%" PRIu32 "B Tasks=%u qLog=%u/%d",
                 (unsigned long long)(status.uptime_us / 1000000ULL),
                 status.heap_free,
                 status.heap_min_ever,
                 (unsigned int)status.task_count,
                 (unsigned int)status.qlog_waiting,
                 QLOG_LENGTH);
        rtos_log_send(&entry);
        ESP_LOGI(TAG_STATUS, "%s", entry.message);

        /* ---- Verificação de heap crítico ---- */
        if (status.heap_free < HEAP_WARN_THRESHOLD) {
            log_entry_t warn = {
                .timestamp = (uint64_t)esp_timer_get_time(),
                .level     = LOG_WARN,
            };
            strncpy(warn.tag, "STATUS", sizeof(warn.tag) - 1);
            snprintf(warn.message, sizeof(warn.message),
                     "Heap livre=%luB abaixo de %dB — risco de alocacao",
                     (unsigned long)status.heap_free, HEAP_WARN_THRESHOLD);
            rtos_log_send(&warn);
            ESP_LOGW(TAG_STATUS, "%s", warn.message);
        }

        /* ---- Verificação de stack por tarefa ---- */
        for (UBaseType_t i = 0; i < n; i++) {
            if (status.task_list[i].usStackHighWaterMark < STACK_WARN_THRESHOLD) {
                log_entry_t err_stk = {
                    .timestamp = (uint64_t)esp_timer_get_time(),
                    .level     = LOG_ERROR,
                };
                strncpy(err_stk.tag, "STATUS", sizeof(err_stk.tag) - 1);
                snprintf(err_stk.message, sizeof(err_stk.message),
                         "Task %s: highWaterMark=%" PRIu32 "w — risco de stack overflow",
                         status.task_list[i].pcTaskName,
                         status.task_list[i].usStackHighWaterMark);
                rtos_log_send(&err_stk);
                ESP_LOGE(TAG_STATUS, "%s", err_stk.message);
            }
        }
    }

    vTaskDelete(NULL);
}

/*  TASK_LOGGER  */

/**
 * @brief Consumidor da fila qLog — grava entradas em Log.txt ou Status.txt.
 *
 *  Bloqueia em xQueueReceive(qLog, portMAX_DELAY).
 *  Usa mutex_log para proteger acesso ao SPIFFS.
 *
 *  Roteamento:
 *    LOG_STATUS → Status.txt
 *    demais     → Log.txt
 *
 *  Formato de linha (Anexo B):
 *    <timestamp_us>  [LEVEL]  [TAG]  mensagem\n
 *
 *  Eventos gerados (logger_internal_log):
 *    [LOGGER / INFO  ] SPIFFS montado
 *    [LOGGER / ERROR ] Falha ao montar SPIFFS
 *    [LOGGER / INFO  ] Entrada gravada com sucesso
 *    [LOGGER / ERROR ] Falha na escrita (fwrite / errno)
 *    [LOGGER / WARN  ] SPIFFS com pouco espaço livre
 *    [LOGGER / WARN  ] Timeout na fila (não deve ocorrer com portMAX_DELAY)
 */
static void task_logger(void *pvParameters)
{
    ESP_LOGI(TAG_LOGGER, "Task_Logger iniciada");
    logger_internal_log(LOG_INFO, "Task_Logger iniciada — aguardando entradas na qLog");

    log_entry_t entry;
    char line_buf[200];

    for (;;) {
        /* Bloqueia até receber uma entrada da fila */
        if (xQueueReceive(qLog, &entry, pdMS_TO_TICKS(5000)) != pdTRUE) {
            /* Timeout (inesperado com portMAX_DELAY, mas tratado) */
            logger_internal_log(LOG_WARN, "xQueueReceive timeout 5s — fila vazia");
            continue;
        }

        /* Seleciona arquivo de destino */
        const char *path = (entry.level == LOG_STATUS)
                           ? STATUS_FILE_PATH
                           : LOG_FILE_PATH;

        /* Formata linha no padrão do Anexo B */
        int len = snprintf(line_buf, sizeof(line_buf),
                           "%llu\t[%s]\t[%s]\t%s\n",
                           (unsigned long long)entry.timestamp,
                           level_str(entry.level),
                           entry.tag,
                           entry.message);

        /* Adquire mutex antes de abrir o arquivo */
        xSemaphoreTake(mutex_log, portMAX_DELAY);

        FILE *f = fopen(path, "a");
        if (f == NULL) {
            ESP_LOGE(TAG_LOGGER, "fopen(%s) falhou — errno=%d", path, errno);
            char err_msg[80];
            snprintf(err_msg, sizeof(err_msg),
                     "fopen(%s) falhou errno=%d", path, errno);
            xSemaphoreGive(mutex_log);
            logger_internal_log(LOG_ERROR, err_msg);
            continue;
        }

        size_t written = fwrite(line_buf, 1, (size_t)len, f);
        fclose(f);

        xSemaphoreGive(mutex_log);

        if (written != (size_t)len) {
            char err_msg[80];
            snprintf(err_msg, sizeof(err_msg),
                     "fwrite falhou em %s — escreveu %zu de %d bytes",
                     path, written, len);
            logger_internal_log(LOG_ERROR, err_msg);
            ESP_LOGE(TAG_LOGGER, "%s", err_msg);
        }

        ESP_LOGD(TAG_LOGGER, "Gravado em %s: %s", path, entry.message);
    }

    vTaskDelete(NULL);
}

/* TASK_SERIAL */

/**
 * @brief Aguarda notificação da ISR do botão GPIO0 e imprime os logs na UART.
 *
 *  Usa ulTaskNotifyTake(portMAX_DELAY) para bloquear sem consumir CPU.
 *  Ao acordar, adquire mutex_log, lê Log.txt e Status.txt e imprime via printf.
 *
 *  Eventos gerados (rtos_log_send direto):
 *    [SERIAL / INFO  ] Tarefa iniciada
 *    [SERIAL / INFO  ] Botão pressionado — leitura acionada
 *    [SERIAL / ERROR ] Falha ao abrir arquivo
 *    [SERIAL / INFO  ] Log.txt / Status.txt impresso com sucesso
 *    [SERIAL / WARN  ] Timeout aguardando notificação
 */
static void task_serial(void *pvParameters)
{
    ESP_LOGI(TAG_SERIAL, "Task_Serial iniciada — aguardando botao GPIO%d", BUTTON_GPIO_PIN);

    log_entry_t evt = {
        .timestamp = (uint64_t)esp_timer_get_time(),
        .level     = LOG_INFO,
    };
    strncpy(evt.tag,     "SERIAL",                                    sizeof(evt.tag) - 1);
    strncpy(evt.message, "Task_Serial iniciada — aguardando ISR botao", sizeof(evt.message) - 1);
    rtos_log_send(&evt);

    char read_buf[256];

    for (;;) {
        /* Bloqueia sem consumir CPU até ISR acordar */
        uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30000));

        if (notified == 0) {
            /* Timeout — nenhum botão pressionado em 30 s */
            log_entry_t warn = {
                .timestamp = (uint64_t)esp_timer_get_time(),
                .level     = LOG_WARN,
            };
            strncpy(warn.tag,     "SERIAL",                              sizeof(warn.tag) - 1);
            strncpy(warn.message, "ulTaskNotifyTake timeout 30s — nenhum botao pressionado",
                    sizeof(warn.message) - 1);
            rtos_log_send(&warn);
            ESP_LOGW(TAG_SERIAL, "%s", warn.message);
            continue;
        }

        /* Botão pressionado */
        log_entry_t btn = {
            .timestamp = (uint64_t)esp_timer_get_time(),
            .level     = LOG_INFO,
        };
        strncpy(btn.tag,     "SERIAL",                          sizeof(btn.tag) - 1);
        strncpy(btn.message, "Botao pressionado — imprimindo logs na UART", sizeof(btn.message) - 1);
        rtos_log_send(&btn);
        ESP_LOGI(TAG_SERIAL, "Botao pressionado — lendo SPIFFS");

        /* Adquire mutex para leitura dos arquivos */
        xSemaphoreTake(mutex_log, portMAX_DELAY);

        /* Itera sobre Log.txt e Status.txt */
        const char *files[] = { LOG_FILE_PATH, STATUS_FILE_PATH };
        for (int fi = 0; fi < 2; fi++) {
            FILE *f = fopen(files[fi], "r");
            if (f == NULL) {
                ESP_LOGE(TAG_SERIAL, "fopen(%s) falhou", files[fi]);
                char err_msg[80];
                snprintf(err_msg, sizeof(err_msg), "fopen(%s) falhou errno=%d", files[fi], errno);
                /* Loga após liberar mutex para evitar deadlock */
                xSemaphoreGive(mutex_log);
                log_entry_t ferr = {
                    .timestamp = (uint64_t)esp_timer_get_time(),
                    .level     = LOG_ERROR,
                };
                strncpy(ferr.tag, "SERIAL", sizeof(ferr.tag) - 1);
                strncpy(ferr.message, err_msg, sizeof(ferr.message) - 1);
                rtos_log_send(&ferr);
                xSemaphoreTake(mutex_log, portMAX_DELAY);
                continue;
            }

            printf("\n========== %s ==========\n", files[fi]);
            int lines = 0;
            while (fgets(read_buf, sizeof(read_buf), f) != NULL) {
                printf("%s", read_buf);
                lines++;
            }
            fclose(f);
            printf("========== %d linhas ==========\n\n", lines);

            /* Loga sucesso */
            log_entry_t ok = {
                .timestamp = (uint64_t)esp_timer_get_time(),
                .level     = LOG_INFO,
            };
            strncpy(ok.tag, "SERIAL", sizeof(ok.tag) - 1);
            snprintf(ok.message, sizeof(ok.message),
                     "%s impresso — %d linhas na UART", files[fi], lines);
            /* Enviado após liberar mutex */
            xSemaphoreGive(mutex_log);
            rtos_log_send(&ok);
            xSemaphoreTake(mutex_log, portMAX_DELAY);
        }

        xSemaphoreGive(mutex_log);
    }

    vTaskDelete(NULL);
}

/* INICIALIZAÇÃO — SPIFFS */

static esp_err_t spiffs_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = SPIFFS_BASE_PATH,
        .partition_label        = NULL,
        .max_files              = 4,
        .format_if_mount_failed = true,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_MAIN, "SPIFFS mount falhou: %s", esp_err_to_name(err));
        return err;
    }

    size_t total = 0, used = 0;
    esp_spiffs_info(NULL, &total, &used);
    ESP_LOGI(TAG_MAIN, "SPIFFS montado — %zu KB livres de %zu KB",
             (total - used) / 1024, total / 1024);

    return ESP_OK;
}

/*  INICIALIZAÇÃO ADC */

static esp_err_t adc_init(void)
{
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id  = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t err = adc_oneshot_new_unit(&init_cfg, &s_adc);
    if (err != ESP_OK) return err;

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten    = ADC_ATTEN_DB_12,
    };
    return adc_oneshot_config_channel(s_adc, VCC_ADC_CHANNEL, &chan_cfg);
}

/* INICIALIZAÇÃO  GPIO BOTÃO  */

static esp_err_t button_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,   /* borda de descida (botão pressionado) */
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) return err;

    gpio_install_isr_service(0);
    return gpio_isr_handler_add(BUTTON_GPIO_PIN, gpio_isr_handler, NULL);
}

/* RTOS_INIT */

/**
 * @brief Inicializa todo o sistema DataLogger.
 *
 *  Ordem de inicialização:
 *    1. SPIFFS
 *    2. qLog + mutex_log
 *    3. ADC
 *    4. DHT11
 *    5. Botão GPIO (ISR)
 *    6. Tarefas FreeRTOS (Logger primeiro, depois produtoras)
 *    7. esp_timer da Task_Sensores
 */
esp_err_t rtos_init(void)
{
    esp_err_t err;

    err = spiffs_init();
    if (err != ESP_OK) return err;

    qLog = xQueueCreate(QLOG_LENGTH, sizeof(log_entry_t));
    if (qLog == NULL) {
        ESP_LOGE(TAG_MAIN, "Falha ao criar qLog");
        return ESP_ERR_NO_MEM;
    }

    mutex_log = xSemaphoreCreateMutex();
    if (mutex_log == NULL) {
        ESP_LOGE(TAG_MAIN, "Falha ao criar mutex_log");
        return ESP_ERR_NO_MEM;
    }

    err = adc_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG_MAIN, "ADC init falhou: %s", esp_err_to_name(err));
        return err;
    }

    dht11_config_t dht_cfg = { .pin = DHT11_GPIO_PIN };
    err = dht11_init(&s_dht11, &dht_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_MAIN, "DHT11 init falhou: %s", esp_err_to_name(err));
        return err;
    }

    err = button_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG_MAIN, "Botao GPIO init falhou: %s", esp_err_to_name(err));
        return err;
    }

    xTaskCreate(task_logger, "Task_Logger", 4096, NULL, 2, NULL);
    xTaskCreate(task_status, "Task_Status", 4096, NULL, 2, NULL);
    xTaskCreate(task_serial, "Task_Serial", 4096, NULL, 1, &h_task_serial);

    /* 7. esp_timer da Task_Sensores (500 ms, periódico) */
    esp_timer_create_args_t timer_args = {
        .callback        = sensor_timer_callback,
        .arg             = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "sensor_timer",
    };
    err = esp_timer_create(&timer_args, &s_sensor_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_MAIN, "esp_timer_create falhou: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_timer_start_periodic(s_sensor_timer,
                                   SENSOR_TIMER_PERIOD_MS * 1000ULL); /* µs */
    if (err != ESP_OK) {
        ESP_LOGE(TAG_MAIN, "esp_timer_start falhou: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG_MAIN, "DataLogger inicializado — sensor_timer=%d ms status=%d ms",
             SENSOR_TIMER_PERIOD_MS, STATUS_PERIOD_MS);

    /* Loga evento de sistema iniciado */
    log_entry_t boot = {
        .timestamp = (uint64_t)esp_timer_get_time(),
        .level     = LOG_INFO,
    };
    strncpy(boot.tag,     "SYS",             sizeof(boot.tag) - 1);
    strncpy(boot.message, "Sistema iniciado", sizeof(boot.message) - 1);
    rtos_log_send(&boot);

    return ESP_OK;
}