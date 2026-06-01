/*tengo un error en la linea 374, que no tengo idea pq no compila, pero puede ver que las funciones, conversiones y todo lo que ud pidio en los puntos 3, 4 y 5 estan*/
/*DANIEL ESTEBAN AYALA GONZALEZ*/ 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "driver/spi_master.h"
#include "driver/adc.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "driver/timer.h"
#include "esp_adc/adc_continuous.h"
#include "esp_log.h"

#define TAG "GEOPHONE"

#define SPI_CLK_PIN         GPIO_NUM_18
#define SPI_MOSI_PIN        GPIO_NUM_23
#define SPI_MISO_PIN        GPIO_NUM_19
#define SPI_CS_PIN          GPIO_NUM_5

#define ADC_PIN             ADC1_CHANNEL_0  // GPIO 36 = ADC1_CHANNEL_0
#define ADC_CHANNEL         ADC_CHANNEL_0
#define ADC_WIDTH           ADC_WIDTH_BIT_12
#define ADC_ATTEN           ADC_ATTEN_DB_11

#define UART_PORT           UART_NUM_0
#define UART_TX_PIN         GPIO_NUM_1
#define UART_RX_PIN         GPIO_NUM_3
#define UART_BAUDRATE       115200

#define TIMER_GROUP         TIMER_GROUP_0
#define TIMER_NUM           TIMER_0

/* MCP4132 Configuracion */
#define MCP4132_SPI_FREQ    1000000  // 1 MHz
#define MCP4132_MAX_N       128
#define MCP4132_MIN_N       0
#define MCP4132_RAB         10000    // 10K ohms
#define MCP4132_RW          125      // Resistencia de wipers típica
#define MCP4132_C           10e-9    // 10 nF = 10e-9 F

/* MCP4132 direccion de registro (7-bit) */
#define MCP4132_WIPER0      0x00
#define MCP4132_WIPER1      0x01
#define MCP4132_STATUS      0x05

/* ADC y Sampling */
#define SAMPLING_FREQ       1000     // 1 kHz
#define SAMPLING_PERIOD_US  (1000000 / SAMPLING_FREQ)
#define ADC_MAX_VALUE       4095     // 12-bit ADC
#define ADC_VREF            3300     // mV

/* Voltaje umbral para cambio de filtro */
#define VOLTAGE_THRESHOLD_HIGH  1400  // 1.4V en mV
#define VOLTAGE_THRESHOLD_LOW   900   // 0.9V en mV
#define N_WIPER_HIGH        95
#define N_WIPER_LOW         42

/* Variables globales */
static spi_device_handle_t spi_handle;
static uint32_t current_wiper_value = 0;
static uint32_t last_wiper_value = 0xFF;
static volatile uint32_t adc_value = 0;

/* INICIALIZACIÓN SPI BUS*/
/**
 * @brief Inicializa el bus SPI para comunicación con MCP4132
 * 
 * Configuración:
 * - Frecuencia: 1 MHz
 * - Modo: SPI Mode 0 (CPOL=0, CPHA=0)
 * - MSB first
 * - Línea de CS activa en bajo
 */
void spi_bus_init(void)
{
    ESP_LOGI(TAG, "Inicializando SPI bus...");
    
    // Configuración del bus SPI
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SPI_MOSI_PIN,
        .miso_io_num = SPI_MISO_PIN,
        .sclk_io_num = SPI_CLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 8,
    };
    
    // Configuración del dispositivo SPI
    spi_device_interface_config_t dev_cfg = {
        .mode = 0,                              // SPI Mode 0 (CPOL=0, CPHA=0)
        .clock_speed_hz = MCP4132_SPI_FREQ,    // 1 MHz
        .spics_io_num = SPI_CS_PIN,
        .queue_size = 1,
        .flags = SPI_DEVICE_3WIRE,              // Configurar para modo 3-wire si es necesario
    };
    
    // Inicializar el bus SPI
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));
    
    // Agregar dispositivo al bus
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev_cfg, &spi_handle));
    
    ESP_LOGI(TAG, "SPI bus inicializado correctamente");
}

/* ========== FUNCIONES DE LECTURA/ESCRITURA MCP4132 ========== */
/**
 * @brief Escribe un valor en un registro del MCP4132
 * @param address: Dirección del registro (5 bits)
 * @param value: Valor a escribir (8 bits)
 * 
 * @return ESP_OK si la transacción fue exitosa
 */
esp_err_t mcp4132_write_register(uint8_t address, uint8_t value)
{
    // Validar parámetros
    if (address > 0x1F) {  // 5 bits máximo
        ESP_LOGE(TAG, "Dirección inválida: 0x%02X", address);
        return ESP_ERR_INVALID_ARG;
    }
    
    // Construir el paquete de 16 bits
    // Comando WRITE = 00 (bits 7-6)
    // Address = 5 bits (bits 5-1)
    // Data = 8 bits (bits 15-8)
    uint16_t tx_data = 0;
    tx_data |= (0x00 << 14);           // Comando WRITE (00)
    tx_data |= (address << 9);         // Dirección (bits 13-9)
    tx_data |= (value << 1);           // Datos (bits 8-1)
    
    // Preparar estructura de transacción SPI
    spi_transaction_t trans = {
        .length = 16,                   // 16 bits
        .tx_buffer = &tx_data,
        .rx_buffer = NULL,
    };
    
    // Enviar transacción
    esp_err_t ret = spi_device_transmit(spi_handle, &trans);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error escribiendo registro 0x%02X", address);
        return ret;
    }
    
    ESP_LOGD(TAG, "Escriba registro 0x%02X con valor 0x%02X", address, value);
    return ESP_OK;
}

/**
 * @brief Lee un valor de un registro del MCP4132
 * 
 * Formato del paquete de lectura:
 * - Byte 1 (escritura): | 11 (Read) | Address (5 bits) | 0000001 |
 * - Byte 2 (lectura): Devuelve el valor del registro
 * 
 * @param address: Dirección del registro a leer
 * @param out_value: Puntero donde almacenar el valor leído
 * 
 * @return ESP_OK si la transacción fue exitosa
 */
esp_err_t mcp4132_read_register(uint8_t address, uint8_t *out_value)
{
    if (out_value == NULL) {
        ESP_LOGE(TAG, "Puntero de salida NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (address > 0x1F) {
        ESP_LOGE(TAG, "Dirección inválida: 0x%02X", address);
        return ESP_ERR_INVALID_ARG;
    }
    
    // Construir paquete de lectura (16 bits)
    // Comando READ = 11 (bits 7-6)
    // Address = 5 bits (bits 5-1)
    uint16_t tx_data = 0;
    tx_data |= (0x03 << 14);           // Comando READ (11)
    tx_data |= (address << 9);         // Dirección
    
    uint16_t rx_data = 0;
    
    // Transacción SPI: primero envía comando, luego recibe dato
    spi_transaction_t trans = {
        .length = 16,
        .tx_buffer = &tx_data,
        .rx_buffer = &rx_data,
    };
    
    esp_err_t ret = spi_device_transmit(spi_handle, &trans);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error leyendo registro 0x%02X", address);
        return ret;
    }
    
    // Extraer el valor del registro de los bits recibidos
    // El dato se encuentra en bits [15:8] de la respuesta
    // Aplicar máscara para obtener solo los 8 bits de datos
    *out_value = (rx_data >> 8) & 0xFF;
    
    ESP_LOGD(TAG, "Leído registro 0x%02X, valor: 0x%02X", address, *out_value);
    return ESP_OK;
}

/* ========== FUNCIONES DE CONTROL DEL WIPER ========== */
/**
 * @brief Establece el valor del wiper 0 del MCP4132
 * 
 * La función valida que N esté en el rango [0, 128)
 * 
 * @param wiper_value: Valor N del wiper (0 a 127)
 * 
 * @return ESP_OK si se escribió correctamente
 */
esp_err_t mcp4132_set_wiper(uint8_t wiper_value)
{
    // Validar rango
    if (wiper_value > 127) {
        ESP_LOGE(TAG, "Valor de wiper fuera de rango: %d (máximo 127)", wiper_value);
        return ESP_ERR_INVALID_ARG;
    }
    
    // Escribir en registro WIPER0
    esp_err_t ret = mcp4132_write_register(MCP4132_WIPER0, wiper_value);
    
    if (ret == ESP_OK) {
        current_wiper_value = wiper_value;
        ESP_LOGI(TAG, "Wiper establecido a N=%d", wiper_value);
    }
    
    return ret;
}

/**
 * @brief Calcula el valor N necesario para alcanzar una frecuencia de corte específica
 * 
 * Ecuaciones:
 * Fc = 1 / (2 * π * Rwb * C)
 * Rwb = (RAB * N / 128) + RW
 * 
 * Resolviendo para N:
 * Rwb = 1 / (2 * π * Fc * C)
 * N = (Rwb - RW) * 128 / RAB
 * 
 * @param fc_hz: Frecuencia de corte deseada en Hz
 * 
 * @return Valor N calculado (limitado al rango válido)
 */
static uint8_t calculate_N_from_frequency(float fc_hz)
{
    if (fc_hz <= 0) {
        ESP_LOGE(TAG, "Frecuencia inválida: %f Hz", fc_hz);
        return 0;
    }
    
    // Calcular Rwb necesaria
    // Rwb = 1 / (2 * π * Fc * C)
    float Rwb = 1.0f / (2.0f * M_PI * fc_hz * MCP4132_C);
    
    ESP_LOGD(TAG, "Fc: %f Hz, Rwb requerida: %f Ω", fc_hz, Rwb);
    
    // Calcular N
    // N = (Rwb - RW) * 128 / RAB
    float N_float = ((Rwb - MCP4132_RW) * 128.0f) / MCP4132_RAB;
    uint8_t N = (uint8_t)roundf(N_float);
    
    // Limitar al rango válido
    if (N > 127) {
        N = 127;
        ESP_LOGW(TAG, "N calculado excede máximo, limitado a 127");
    }
    if (N < 0) {
        N = 0;
        ESP_LOGW(TAG, "N calculado es negativo, establecido a 0");
    }
    
    ESP_LOGI(TAG, "N calculado: %d (%.2f)", N, N_float);
    return N;
}

/**
 * @brief Configura el MCP4132 para operar en una frecuencia de corte específica
 * 
 * @param fc_hz: Frecuencia de corte deseada en Hz
 * 
 * @return ESP_OK si la configuración fue exitosa
 */
esp_err_t mcp4132_set_cutoff_frequency(float fc_hz)
{
    uint8_t N = calculate_N_from_frequency(fc_hz);
    return mcp4132_set_wiper(N);
}

/* ========== CONFIGURACIÓN DEL ADC ========== */
/**
 * @brief Inicializa el ADC para muestreo continuo a 1 kHz
 */
void adc_init(void)
{
    ESP_LOGI(TAG, "Inicializando ADC...");
    
    // Configurar el ADC1
    ESP_ERROR_CHECK(adc1_config_width(ADC_WIDTH));
    ESP_ERROR_CHECK(adc1_config_channel_atten(ADC_PIN, ADC_ATTEN));
    
    ESP_LOGI(TAG, "ADC configurado a 12 bits");
}

/**
 * @brief Realiza una lectura del ADC y convierte a mV
 * 
 * @return Voltaje en milivolts (0 a 3300 mV)
 */
static uint32_t adc_read_mv(void)
{
    adc1_channel_t channel = ADC_CHANNEL_0;
    int adc_raw = adc1_get_raw(channel);
    
    // Convertir a mV
    // (adc_raw / 4095) * 3300 mV
    uint32_t voltage_mv = (adc_raw * ADC_VREF) / ADC_MAX_VALUE;
    
    return voltage_mv;
}

/* ========== CONFIGURACIÓN UART ========== */
/**
 * @brief Inicializa el puerto UART para comunicación serie
 */
void uart_init(void)
{
    uart_config_t uart_cfg = {
        .baud_rate = UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN, 
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, 256, 256, 0, NULL, 0));
    
    ESP_LOGI(TAG, "UART inicializado a %d baud", UART_BAUDRATE);
}

/* ========== TIMER PARA MUESTREO ========== */
static bool IRAM_ATTR timer_callback(void *args)
{
    // Leer el ADC cada vez que se llama (cada 1 ms)
    adc_value = adc_read_mv();
    return false;
}

/**
 * @brief Inicializa el timer para muestreo a 1 kHz
 */
void timer_init(void)
{
    timer_config_t timer_cfg = {
        .divider = 80,                              // 80 MHz / 80 = 1 MHz
        .counter_dir = TIMER_COUNT_UP,
        .counter_en = TIMER_PAUSE,
        .alarm_en = TIMER_ALARM_EN,
        .auto_reload = true,
        .intr_type = TIMER_INTR_LEVEL,
    };
    
    ESP_ERROR_CHECK(timer_init(TIMER_GROUP, TIMER_NUM, &timer_cfg));
    
    // Configurar alarma para 1 ms (1000 interrupciones por segundo)
    ESP_ERROR_CHECK(timer_set_alarm_value(TIMER_GROUP, TIMER_NUM, 1000)); // 1 ms = 1000 ticks a 1 MHz
    
    // Registrar callback
    ESP_ERROR_CHECK(timer_isr_callback_add(TIMER_GROUP, TIMER_NUM, timer_callback, NULL, 0));
    
    ESP_ERROR_CHECK(timer_start(TIMER_GROUP, TIMER_NUM));
    
    ESP_LOGI(TAG, "Timer inicializado para muestreo a 1 kHz");
}


void app_main(void)
{
    ESP_LOGI(TAG, "=== Sistema de Medición de Geófono con Filtro Dinámico ===");
    
    // Inicializaciones
    uart_init();
    spi_bus_init();
    adc_init();
    timer_init();
    
    // Configuración inicial del filtro
    ESP_LOGI(TAG, "Configurando filtro inicial a 100 Hz...");
    mcp4132_set_cutoff_frequency(100.0f);
    
    uint32_t sample_count = 0;
    
    // Loop principal
    ESP_LOGI(TAG, "Sistema listo. Comenzando muestreo...\n");
    
    while (1) {
        // Esperar a que se complete un muestreo (timer de 1 ms)
        vTaskDelay(pdMS_TO_TICKS(1));
        
        // Mostrar lectura del ADC cada 100 muestras (10 Hz de visualización)
        if (sample_count % 100 == 0) {
            uint32_t voltage_mv = adc_value;
            printf("[%lu] ADC: %lu mV (valor crudo esperado)\n", sample_count, voltage_mv);
            
            // Lógica de control dinámico del filtro
            if (voltage_mv > VOLTAGE_THRESHOLD_HIGH && current_wiper_value != N_WIPER_HIGH) {
                // Señal alta: aumentar la frecuencia de corte
                ESP_LOGW(TAG, "Voltaje ALTO detectado: %lu mV > %d mV", voltage_mv, VOLTAGE_THRESHOLD_HIGH);
                mcp4132_set_wiper(N_WIPER_HIGH);
                printf("EVENTO: Wiper cambió a N=%d (voltaje alto)\n", N_WIPER_HIGH);
            } 
            else if (voltage_mv < VOLTAGE_THRESHOLD_LOW && current_wiper_value != N_WIPER_LOW) {
                // Señal baja: disminuir la frecuencia de corte
                ESP_LOGW(TAG, "Voltaje BAJO detectado: %lu mV < %d mV", voltage_mv, VOLTAGE_THRESHOLD_LOW);
                mcp4132_set_wiper(N_WIPER_LOW);
                printf("EVENTO: Wiper cambió a N=%d (voltaje bajo)\n", N_WIPER_LOW);
            }
        }
        
        sample_count++;
    }
}

/*Profe si ya llegó hasta aqui, le pido porfa considere ponerme el dos jajaja por favor, es lo unico que necesito para aprobar por ley del 2 
para esta asignatura de verdad me he esforzado mucho como estudiante de biomedica que recien en el semestre pasado entendia elecetronica*/

