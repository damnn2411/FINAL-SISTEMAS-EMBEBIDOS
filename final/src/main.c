#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_random.h"
 
// ── PINES ────────────────────────────────────────────────────
gpio_num_t filas[5] = {
    GPIO_NUM_25, GPIO_NUM_26, GPIO_NUM_19,
    GPIO_NUM_23, GPIO_NUM_21
};
#define FILA_VIDAS GPIO_NUM_22
 
gpio_num_t col_rojo[5] = {
    GPIO_NUM_27, GPIO_NUM_33, GPIO_NUM_32,
    GPIO_NUM_15, GPIO_NUM_16
};
gpio_num_t col_verde[5] = {
    GPIO_NUM_13, GPIO_NUM_2, GPIO_NUM_17,
    GPIO_NUM_5,  GPIO_NUM_18
};
 
#define BTN_MOVER   GPIO_NUM_12
#define BTN_INICIAR GPIO_NUM_14
 
// ── ESTADOS ──────────────────────────────────────────────────
#define ESTADO_INTRO     0
#define ESTADO_PREVIEW   1
#define ESTADO_JUEGO     2
#define ESTADO_VICTORIA  3
#define ESTADO_GAME_OVER 4
int estado = ESTADO_INTRO;
 
// ── CURSOR ───────────────────────────────────────────────────
int cursor_x = 0;
int cursor_y = 0;
 
// ── DEBOUNCE ─────────────────────────────────────────────────
#define DEBOUNCE_MS 200
TickType_t ultimo_mover   = 0;
TickType_t ultimo_iniciar = 0;
 
// ── BOTE (intro estático) ────────────────────────────────────
// o o x o o   →  0b00100
// o o x x o   →  0b00110
// o o x o o   →  0b00100
// x x x x x   →  0b11111
// o x x x o   →  0b01110
 
static const uint8_t bote[5] = {
    0b00100,
    0b00110,
    0b00100,
    0b11111,
    0b01110
};
 
// ── TABLERO ──────────────────────────────────────────────────
uint8_t tablero[5][5];
uint8_t disparos[5][5];
int vidas            = 5;
int celdas_restantes = 8;  // 3 + 3 + 2
 
// ── BARCOS ───────────────────────────────────────────────────
typedef struct { int fila; int col; } Celda;
Celda barco1[3];  // tamaño 3
Celda barco2[3];  // tamaño 3
Celda barco3[2];  // tamaño 2
 
// ── CONTADORES DE TRANSICIÓN ─────────────────────────────────
// Cada frame = 6 filas × 2000 µs = ~12 ms
// 250 frames ≈ 3 segundos
int contador_estado = 0;
#define DURACION_PREVIEW  250
#define DURACION_FIN      250
 
int blink_contador = 0;
int blink_fase     = 0;
 
// ── HELPERS ──────────────────────────────────────────────────
void apagar_todo(void)
{
    for (int i = 0; i < 5; i++) gpio_set_level(filas[i], 0);
    gpio_set_level(FILA_VIDAS, 0);
    for (int c = 0; c < 5; c++) gpio_set_level(col_rojo[c],  1);
    for (int c = 0; c < 5; c++) gpio_set_level(col_verde[c], 1);
}
 
// ── GENERACIÓN DE BARCOS ─────────────────────────────────────
/*
 * Rejection sampling: se sortean orientación y posición con
 * esp_random(). Si las celdas ya están ocupadas o se salen de
 * la grilla, se descarta y se reintenta hasta encontrar una
 * posición válida.
 *
 * Orientación 0 = vertical   → fila varía, col fija
 * Orientación 1 = horizontal → col varía,  fila fija
 */
void colocar_barco(Celda *barco, int tam)
{
    while (1) {
        int ori  = esp_random() % 2;
        int fila = (ori == 0) ? (esp_random() % (5 - tam + 1)) : (esp_random() % 5);
        int col  = (ori == 0) ? (esp_random() % 5)             : (esp_random() % (5 - tam + 1));
        int ok = 1;
        for (int i = 0; i < tam && ok; i++) {
            int f = fila + (ori == 0 ? i : 0);
            int c = col  + (ori == 1 ? i : 0);
            if (tablero[f][c]) ok = 0;
        }
        if (ok) {
            for (int i = 0; i < tam; i++) {
                int f = fila + (ori == 0 ? i : 0);
                int c = col  + (ori == 1 ? i : 0);
                tablero[f][c] = 1;
                barco[i] = (Celda){f, c};
            }
            break;
        }
    }
}
 
void generar_barcos(void)
{
    memset(tablero, 0, sizeof(tablero));
    colocar_barco(barco1, 3);
    colocar_barco(barco2, 3);
    colocar_barco(barco3, 2);
}
 
void iniciar_juego(void)
{
    generar_barcos();
    memset(disparos, 0, sizeof(disparos));
    vidas            = 5;
    celdas_restantes = 8;
    cursor_x         = 0;
    cursor_y         = 0;
    contador_estado  = 0;
}
 
void avanzar_cursor(void)
{
    int intentos = 0;
    do {
        cursor_x++;
        if (cursor_x > 4) {
            cursor_x = 0;
            cursor_y++;
            if (cursor_y > 4) cursor_y = 0;
        }
        intentos++;
    } while (disparos[cursor_y][cursor_x] != 0 && intentos < 25);
}
 
// ── DISPLAYS ─────────────────────────────────────────────────
void display_intro(void)
{
    for (int f = 0; f < 5; f++) {
        apagar_todo();
        uint8_t fila_bits = bote[f];
        for (int c = 0; c < 5; c++) {
            if (fila_bits & (1 << (4 - c)))
                gpio_set_level(col_verde[c], 0);
        }
        gpio_set_level(filas[f], 1);
        esp_rom_delay_us(2000);
        gpio_set_level(filas[f], 0);
    }
    apagar_todo();
    gpio_set_level(FILA_VIDAS, 1);
    esp_rom_delay_us(2000);
    gpio_set_level(FILA_VIDAS, 0);
}
 
// Preview: muestra la FORMA de los tres barcos, no su posición real
// o x x x o   fila 0 — barco grande 1 (3 celdas, cols 1-3)
// o x x x o   fila 1 — barco grande 2 (3 celdas, cols 1-3)
// o x x o o   fila 2 — barco chico    (2 celdas, cols 1-2)
// o o o o o   fila 3
// o o o o o   fila 4
static const uint8_t preview_ships[5] = {
    0b10101,   // fila 0 — cols 0, 2, 4
    0b10101,   // fila 1 — cols 0, 2, 4
    0b10100,   // fila 2 — cols 0, 2  (barco3 de 2 termina)
    0b00000,   // fila 3
    0b00000    // fila 4
};
 
void display_preview(void)
{
    for (int f = 0; f < 5; f++) {
        apagar_todo();
        uint8_t fila_bits = preview_ships[f];
        for (int c = 0; c < 5; c++) {
            if (fila_bits & (1 << (4 - c)))
                gpio_set_level(col_verde[c], 0);
        }
        gpio_set_level(filas[f], 1);
        esp_rom_delay_us(2000);
        gpio_set_level(filas[f], 0);
    }
    apagar_todo();
    for (int c = 0; c < 5; c++) gpio_set_level(col_rojo[c], 0);
    gpio_set_level(FILA_VIDAS, 1);
    esp_rom_delay_us(2000);
    gpio_set_level(FILA_VIDAS, 0);
}
 
void display_juego(void)
{
    for (int f = 0; f < 5; f++) {
        apagar_todo();
        for (int c = 0; c < 5; c++) {
            if (disparos[f][c] == 1) gpio_set_level(col_verde[c], 0);
            if (disparos[f][c] == 2) gpio_set_level(col_rojo[c],  0);
        }
        if (f == cursor_y && disparos[f][cursor_x] == 0) {
            gpio_set_level(col_rojo[cursor_x],  0);
            gpio_set_level(col_verde[cursor_x], 0);
        }
        gpio_set_level(filas[f], 1);
        esp_rom_delay_us(2000);
        gpio_set_level(filas[f], 0);
    }
    apagar_todo();
    for (int c = 0; c < vidas; c++) gpio_set_level(col_rojo[c], 0);
    gpio_set_level(FILA_VIDAS, 1);
    esp_rom_delay_us(2000);
    gpio_set_level(FILA_VIDAS, 0);
}
 
void display_game_over(void)
{
    for (int f = 0; f < 5; f++) {
        apagar_todo();
        for (int c = 0; c < 5; c++) {
            if      (disparos[f][c] == 1) gpio_set_level(col_verde[c], 0); // acierto previo
            else if (disparos[f][c] == 2) gpio_set_level(col_rojo[c],  0); // fallo previo
            else if (tablero[f][c])        gpio_set_level(col_verde[c], 0); // barco no encontrado
        }
        gpio_set_level(filas[f], 1);
        esp_rom_delay_us(2000);
        gpio_set_level(filas[f], 0);
    }
    apagar_todo();
    gpio_set_level(FILA_VIDAS, 1);
    esp_rom_delay_us(2000);
    gpio_set_level(FILA_VIDAS, 0);
}
 
void display_victoria(void)
{
    for (int f = 0; f < 5; f++) {
        apagar_todo();
        if (blink_fase == 0)
            for (int c = 0; c < 5; c++) gpio_set_level(col_verde[c], 0);
        gpio_set_level(filas[f], 1);
        esp_rom_delay_us(2000);
        gpio_set_level(filas[f], 0);
    }
    apagar_todo();
    if (blink_fase == 0)
        for (int c = 0; c < 5; c++) gpio_set_level(col_verde[c], 0);
    gpio_set_level(FILA_VIDAS, 1);
    esp_rom_delay_us(2000);
    gpio_set_level(FILA_VIDAS, 0);
}
 
// ── MAIN ─────────────────────────────────────────────────────
void app_main(void)
{
    for (int i = 0; i < 5; i++) {
        gpio_reset_pin(filas[i]);
        gpio_set_direction(filas[i], GPIO_MODE_OUTPUT);
        gpio_set_level(filas[i], 0);
    }
    gpio_reset_pin(FILA_VIDAS);
    gpio_set_direction(FILA_VIDAS, GPIO_MODE_OUTPUT);
    gpio_set_level(FILA_VIDAS, 0);
 
    for (int i = 0; i < 5; i++) {
        gpio_reset_pin(col_rojo[i]);
        gpio_set_direction(col_rojo[i], GPIO_MODE_OUTPUT);
        gpio_set_level(col_rojo[i], 1);
    }
    for (int i = 0; i < 5; i++) {
        gpio_reset_pin(col_verde[i]);
        gpio_set_direction(col_verde[i], GPIO_MODE_OUTPUT);
        gpio_set_level(col_verde[i], 1);
    }
 
    gpio_reset_pin(BTN_MOVER);
    gpio_set_direction(BTN_MOVER, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BTN_MOVER, GPIO_PULLUP_ONLY);
 
    gpio_reset_pin(BTN_INICIAR);
    gpio_set_direction(BTN_INICIAR, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BTN_INICIAR, GPIO_PULLUP_ONLY);
 
    while (1)
    {
        TickType_t ahora = xTaskGetTickCount();
 
        switch (estado)
        {
        case ESTADO_INTRO:
            display_intro();
            if (!gpio_get_level(BTN_INICIAR) &&
                (ahora - ultimo_iniciar) >= pdMS_TO_TICKS(DEBOUNCE_MS))
            {
                ultimo_iniciar = ahora;
                iniciar_juego();
                estado = ESTADO_PREVIEW;
            }
            break;
 
        case ESTADO_PREVIEW:
            display_preview();
            contador_estado++;
            if (contador_estado >= DURACION_PREVIEW)
                estado = ESTADO_JUEGO;
            break;
 
        case ESTADO_JUEGO:
            display_juego();
 
            if (!gpio_get_level(BTN_MOVER) &&
                (ahora - ultimo_mover) >= pdMS_TO_TICKS(DEBOUNCE_MS))
            {
                ultimo_mover = ahora;
                avanzar_cursor();
            }
 
            if (!gpio_get_level(BTN_INICIAR) &&
                (ahora - ultimo_iniciar) >= pdMS_TO_TICKS(DEBOUNCE_MS))
            {
                ultimo_iniciar = ahora;
                if (disparos[cursor_y][cursor_x] == 0) {
                    if (tablero[cursor_y][cursor_x]) {
                        disparos[cursor_y][cursor_x] = 1;  // acierto
                        vidas = 5;                          // reset vidas completo
                        celdas_restantes--;
                        if (celdas_restantes == 0) {
                            estado          = ESTADO_VICTORIA;
                            blink_contador  = 0;
                            blink_fase      = 0;
                            contador_estado = 0;
                        }
                    } else {
                        disparos[cursor_y][cursor_x] = 2;  // fallo
                        vidas--;
                        if (vidas == 0) {
                            estado          = ESTADO_GAME_OVER;
                            contador_estado = 0;
                        }
                    }
                }
            }
            break;
 
        case ESTADO_VICTORIA:
            display_victoria();
            contador_estado++;
            if (contador_estado % 40 == 0) {
                blink_fase = !blink_fase;
                blink_contador++;
            }
            if (blink_contador >= 6)
                estado = ESTADO_INTRO;
            break;
 
        case ESTADO_GAME_OVER:
            display_game_over();
            contador_estado++;
            if (contador_estado >= DURACION_FIN)
                estado = ESTADO_INTRO;
            break;
        }
    }
}