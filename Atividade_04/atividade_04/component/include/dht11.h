#ifndef DHT11_H
#define DHT11_H

/** @file dht11.h
 *  @brief Arquivo de cabeçalho para o driver do sensor de temperatura e umidade DHT11.
 *
 *  Este componente fornece uma API thread-safe, compatível com ESP-IDF para
 *  leitura de dados de temperatura e umidade do sensor DHT11 usando
 *  o protocolo de um único fio (single-wire) através de um pino GPIO.
 *
 * @author Valdemar Neto
 * @copyright 2026 ELE0629 - Tópicos Especiais em Sistemas Embarcados
 * @date 2026-04-16
 */

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_err.h"

#define DHT11_MIN_READ_INTERVAL_MS 2000

#ifdef __cplusplus
extern "C"
{
#endif



/** @brief Estrutura de configuração para a inicialização do DHT11.
 *
 *  Contém o número do pino GPIO ao qual a linha de dados do DHT11 está conectada.
 */
typedef struct
{
    gpio_num_t pin; 
} dht11_config_t;

/** @brief Estrutura de dados que armazena uma leitura do sensor DHT11.
 *
 *  Armazena as partes inteiras e fracionárias conforme fornecido pelo protocolo DHT11.
 *  O DHT11 retorna apenas valores inteiros; os bytes fracionários são sempre 0.
 */
typedef struct
{
    uint8_t humidity_int;    /* Parte inteira da umidade relativa (%RH) */
    uint8_t humidity_frac;   /* Parte fracionária da umidade (sempre 0 para o DHT11) */
    uint8_t temperature_int; /* Parte inteira da temperatura (°C) */
    uint8_t temperature_frac;/* Parte fracionária da temperatura (sempre 0 para o DHT11) */
} dht11_data_t;

/** @brief Estrutura de handle para controle do sensor DHT11.
 *
 *  Contém a configuração do sensor e um mutex do FreeRTOS para acesso thread-safe.
 *  Deve ser inicializado com dht11_init() antes do uso.
 */
typedef struct
{
    dht11_config_t config;  /**< Configuração para o sensor DHT11. */
    SemaphoreHandle_t mutex;/**< Mutex para acesso thread-safe ao sensor. */
} dht11_handle_t;

/** @brief Inicializa o sensor DHT11.
 *
 *  Valida argumentos, configura o pino GPIO e cria o mutex interno para acesso thread-safe.
 *
 * @param handle Ponteiro para o handle do DHT11 a ser inicializado.
 * @param config Ponteiro para a estrutura de configuração do DHT11.
 * @return ESP_OK em caso de sucesso.
 * @return ESP_ERR_INVALID_ARG se o handle ou config for NULL, ou se o GPIO for inválido.
 * @return ESP_ERR_NO_MEM se a criação do mutex falhar.
 * @return Outros códigos esp_err_t se a configuração do GPIO falhar.
 */
esp_err_t dht11_init(dht11_handle_t *handle, const dht11_config_t *config);

/** @brief Lê temperatura e umidade do sensor DHT11.
 *
 *  Executa o protocolo de comunicação single-wire do DHT11, lê 40 bits de
 *  dados, verifica o checksum e preenche a estrutura de dados de saída.
 *
 *  @note O DHT11 requer pelo menos 1 segundo entre leituras consecutivas.
 *
 * @param handle Ponteiro para um handle DHT11 inicializado.
 * @param data   Ponteiro para uma struct dht11_data_t para receber os dados do sensor.
 * @return ESP_OK em caso de sucesso.
 * @return ESP_ERR_INVALID_ARG se o handle ou data for NULL.
 * @return ESP_ERR_TIMEOUT se o sensor não responder a tempo.
 * @return ESP_ERR_INVALID_CRC se a verificação do checksum falhar.
 */
esp_err_t dht11_read(dht11_handle_t *handle, dht11_data_t *data);

/** @brief Desinicializa o sensor DHT11 e libera recursos.
 *
 *  Redefine o pino GPIO para o seu estado padrão, deleta o mutex interno
 *  e limpa o handle.
 *
 * @param handle Ponteiro para um handle DHT11 inicializado.
 * @return ESP_OK em caso de sucesso.
 * @return ESP_ERR_INVALID_ARG se o handle for NULL ou já estiver desinicializado.
 * @return Outros códigos esp_err_t se o reset do GPIO falhar.
 */
esp_err_t dht11_deinit(dht11_handle_t *handle);

#ifdef __cplusplus
}
#endif

#endif /* DHT11_H */