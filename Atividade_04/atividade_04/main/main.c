#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "dht11.h"
#include "sdkconfig.h"




#define DHT11_PIN           4
#define READ_INTERVAL_MS    DHT11_MIN_READ_INTERVAL_MS

static const char *TAG = "main";

static void dht11_task(void *pvParameters);

void app_main(void)
{
    ESP_LOGI(TAG, "Exemplo DHT11 iniciando");
    ESP_LOGW(TAG, "MODO DE TESTE SEM HARDWARE ATIVADO");

    /* Inicializa o componente do sensor. */
    static dht11_handle_t sensor;

    dht11_config_t config = {
        .pin = DHT11_PIN,
    };

    esp_err_t err = dht11_init(&sensor, &config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "dht11_init falhou: %s", esp_err_to_name(err));
        return;
    }

    /* Cria uma tarefa dedicada para as leituras do sensor. */
    xTaskCreate(dht11_task,"dht11_task", 2048, &sensor, 5, NULL);

    ESP_LOGI(TAG, "Tarefa DHT11 criada — lendo a cada %d ms", READ_INTERVAL_MS);
}


/**
 * @brief Tarefa do FreeRTOS que lê periodicamente o sensor DHT11 e registra
 *        os resultados. O handle é passado como parâmetro da tarefa.
 *
 * @param pvParameters Ponteiro para um dht11_handle_t inicializado.
 */
static void dht11_task(void *pvParameters)
{
    dht11_handle_t *sensor = (dht11_handle_t *)pvParameters;
    dht11_data_t data;

    for (;;)
    {
        esp_err_t err = dht11_read(sensor, &data);

        if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "Temperatura: %d.%d °C | Umidade: %d.%d %%RH",
                     data.temperature_int, data.temperature_frac,
                     data.humidity_int,    data.humidity_frac);
        }
        else
        {
            ESP_LOGW(TAG, "Falha na leitura: %s", esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(READ_INTERVAL_MS));
    }

    vTaskDelete(NULL);
}
