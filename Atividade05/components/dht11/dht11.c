#include <string.h>
#include "dht11.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "rom/ets_sys.h"
#include "freertos/task.h"

static const char *TAG = "dht11";

#define DHT11_TIMEOUT_US        100
#define DHT11_START_LOW_MS      20
#define DHT11_DATA_BITS         40

/**
 * @brief Aguarda uma mudança de nível no GPIO, até DHT11_TIMEOUT_US microssegundos.
 *
 * Monitora o pino GPIO em um loop fechado. Retorna o número de microssegundos
 * decorridos, ou -1 em caso de timeout.
 *
 * @param pin    Número do pino GPIO a ser monitorado.
 * @param level  Nível lógico alvo (0 ou 1) pelo qual esperar.
 * @return Microssegundos decorridos (>=0), ou -1 em caso de timeout.
 */
static int dht11_wait_level(gpio_num_t pin, int level)
{
    int elapsed = 0;

    while (gpio_get_level(pin) != level)
    {
        if (elapsed >= DHT11_TIMEOUT_US)
        {
            return -1;
        }
        esp_rom_delay_us(1);
        elapsed++;
    }

    return elapsed;
}

esp_err_t dht11_init(dht11_handle_t *handle, const dht11_config_t *config)
{
    ESP_LOGI(TAG, "Inicializando o sensor DHT11");

    if (!handle || !config)
    {
        ESP_LOGE(TAG, "Argumento inválido: handle ou config é NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (!GPIO_IS_VALID_GPIO(config->pin))
    {
        ESP_LOGE(TAG, "Pino GPIO inválido: %d", config->pin);
        return ESP_ERR_INVALID_ARG;
    }

    handle->config = *config;

    handle->mutex = xSemaphoreCreateMutex();
    if (handle->mutex == NULL)
    {
        ESP_LOGE(TAG, "Falha ao criar o mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Configura o pino como saída open-drain, inicialmente em nível alto (estado inativo). */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << config->pin),
        .mode         = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Falha na configuração do GPIO: %s", esp_err_to_name(err));
        vSemaphoreDelete(handle->mutex);
        handle->mutex = NULL;
        return err;
    }

    /* Define a linha em nível alto e aguarda o sensor estabilizar. */
    err = gpio_set_level(config->pin, 1);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Falha ao definir o nível do GPIO: %s", esp_err_to_name(err));
        vSemaphoreDelete(handle->mutex);
        handle->mutex = NULL;
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG, "DHT11 inicializado no GPIO %d", config->pin);
    return ESP_OK;
}

/* Defina como 1 para testar sem o sensor físico */
#define DHT11_MOCK_ENABLE 0

esp_err_t dht11_read(dht11_handle_t *handle, dht11_data_t *data)
{
#if DHT11_MOCK_ENABLE
    if (data) {
        data->humidity_int = 60;
        data->humidity_frac = 0;
        data->temperature_int = 25;
        data->temperature_frac = 0;
        ESP_LOGW(TAG, "MODO SIMULAÇÃO ATIVO: Retornando valores fixos (25°C, 60%%RH)");
        return ESP_OK;
    }
#endif

    if (!handle || handle->mutex == NULL)
    {
        ESP_LOGE(TAG, "Handle inválido");
        return ESP_ERR_INVALID_ARG;
    }

    if (!data)
    {
        ESP_LOGE(TAG, "Ponteiro de dados de saída é NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(handle->mutex, pdMS_TO_TICKS(2000)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Timeout ao adquirir o mutex");
        return ESP_ERR_TIMEOUT;
    }

    gpio_num_t pin = handle->config.pin;
    uint8_t raw_bits[DHT11_DATA_BITS] = {0};
    esp_err_t err = ESP_OK;

    /* host envia o sinal*/
    gpio_set_direction(pin, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(pin, 0);
    vTaskDelay(pdMS_TO_TICKS(DHT11_START_LOW_MS));
    gpio_set_level(pin, 1);
    esp_rom_delay_us(30);

    /* seta o sinal para entradda */
    gpio_set_direction(pin, GPIO_MODE_INPUT);

    /* clock do sensor  */
    if (dht11_wait_level(pin, 0) < 0)
    {
        ESP_LOGE(TAG, "O sensor não puxou o nível baixo para ACK");
        err = ESP_ERR_TIMEOUT;
        goto cleanup;
    }

    if (dht11_wait_level(pin, 1) < 0)
    {
        ESP_LOGE(TAG, "O sensor não puxou o nível alto para ACK");
        err = ESP_ERR_TIMEOUT;
        goto cleanup;
    }

    /* agurdada o fim do high */
    if (dht11_wait_level(pin, 0) < 0)
    {
        ESP_LOGE(TAG, "Timeout na fase alta do ACK");
        err = ESP_ERR_TIMEOUT;
        goto cleanup;
    }

    
    for (int i = 0; i < DHT11_DATA_BITS; i++)
    {
        /* Espera o nível alto do bit */
        if (dht11_wait_level(pin, 1) < 0)
        {
            ESP_LOGE(TAG, "Timeout esperando pelo nível alto do bit %d", i);
            err = ESP_ERR_TIMEOUT;
            goto cleanup;
        }

        /* Mede a duração do nível alto */
        int high_us = dht11_wait_level(pin, 0);
        if (high_us < 0)
        {
            ESP_LOGE(TAG, "Timeout medindo o bit %d", i);
            err = ESP_ERR_TIMEOUT;
            goto cleanup;
        }

        raw_bits[i] = (high_us > 40) ? 1 : 0;
    }


    uint8_t bytes[5] = {0};
    for (int i = 0; i < DHT11_DATA_BITS; i++)
    {
        bytes[i / 8] = (uint8_t)((bytes[i / 8] << 1) | raw_bits[i]);
    }


    uint8_t checksum = (uint8_t)(bytes[0] + bytes[1] + bytes[2] + bytes[3]);
    if (checksum != bytes[4])
    {
        ESP_LOGE(TAG, "Erro de checksum: calculado 0x%02X, recebido 0x%02X",
                 checksum, bytes[4]);
        err = ESP_ERR_INVALID_CRC;
        goto cleanup;
    }


    data->humidity_int    = bytes[0];
    data->humidity_frac   = bytes[1];
    data->temperature_int = bytes[2];
    data->temperature_frac= bytes[3];

    ESP_LOGD(TAG, "Leitura OK — Umidade: %d.%d %%RH  Temperatura: %d.%d °C",
             data->humidity_int, data->humidity_frac,
             data->temperature_int, data->temperature_frac);

cleanup:
    /* Retorna a linha para o estado inativo em nível alto. */
    gpio_set_direction(pin, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(pin, 1);

    xSemaphoreGive(handle->mutex);
    return err;
}

esp_err_t dht11_deinit(dht11_handle_t *handle)
{
    if (!handle || handle->mutex == NULL)
    {
        ESP_LOGE(TAG, "Handle inválido no deinit");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Desinicializando DHT11 no GPIO %d", handle->config.pin);

    if (xSemaphoreTake(handle->mutex, portMAX_DELAY) != pdTRUE)
    {
        ESP_LOGW(TAG, "Falha ao adquirir o mutex durante o deinit");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = gpio_reset_pin(handle->config.pin);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Falha ao resetar o pino GPIO: %s", esp_err_to_name(err));
        xSemaphoreGive(handle->mutex);
        return err;
    }

    xSemaphoreGive(handle->mutex);
    vSemaphoreDelete(handle->mutex);
    handle->mutex = NULL;

    ESP_LOGI(TAG, "DHT11 desinicializado");
    return ESP_OK;
}