/* SPI flash dumper - Etapa 2: dump completo
 *
 * Flash target: Spansion S25FL032P (JEDEC 01 02 15), 4 MB.
 *
 * Consola: USB nativo del ESP32-S3 (USB-Serial-JTAG, /dev/cu.usbmodemXXXX).
 *
 * Protocolo con la PC:
 *   - Bootea, imprime JEDEC ID y "READY".
 *   - Espera el byte 'D' -> dumpea los 4 MB crudos por stdout (silencio total).
 *   - Al terminar imprime "CRC32:xxxxxxxx" (CRC32 estandar, como zlib.crc32).
 *   - Vuelve a "READY" para permitir dumps repetidos sin reflashear.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"

// Pines del bus SPI hacia la flash del router (ajustar segun tu cableado)
#define FLASH_HOST      SPI2_HOST
#define PIN_NUM_MISO    13
#define PIN_NUM_MOSI    11
#define PIN_NUM_CLK     12
#define PIN_NUM_CS      10

// Velocidad conservadora para cables improvisados
#define FLASH_CLK_FREQ  (1 * 1000 * 1000)

// Comandos 25-series
#define CMD_JEDEC_ID    0x9F
#define CMD_READ        0x03

// Geometria del dump
#define FLASH_SIZE      (4 * 1024 * 1024)   // 4 MB
#define CHUNK_SIZE      4096
#define HDR_SIZE        4                   // cmd (1) + addr (3)

static const char TAG[] = "dumper";

static spi_device_handle_t flash;
static uint8_t *tx_buf;                     // header + dummies
static uint8_t *rx_buf;                     // 4 bytes descartables + datos

static void flash_init(void)
{
    esp_err_t ret;

    spi_bus_config_t buscfg = {
        .miso_io_num = PIN_NUM_MISO,
        .mosi_io_num = PIN_NUM_MOSI,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = HDR_SIZE + CHUNK_SIZE,
    };
    ret = spi_bus_initialize(FLASH_HOST, &buscfg, SPI_DMA_CH_AUTO);
    ESP_ERROR_CHECK(ret);

    // Full duplex estandar, CS por hardware. Todos los comandos se arman
    // a mano en tx_buf (ver nota en flash_read_chunk).
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = FLASH_CLK_FREQ,
        .mode = 0,
        .spics_io_num = PIN_NUM_CS,
        .queue_size = 1,
    };
    ret = spi_bus_add_device(FLASH_HOST, &devcfg, &flash);
    ESP_ERROR_CHECK(ret);

    tx_buf = heap_caps_malloc(HDR_SIZE + CHUNK_SIZE, MALLOC_CAP_DMA);
    rx_buf = heap_caps_malloc(HDR_SIZE + CHUNK_SIZE, MALLOC_CAP_DMA);
    if (!tx_buf || !rx_buf) {
        ESP_LOGE(TAG, "Sin memoria DMA para los buffers");
        abort();
    }
    // Los dummy bytes siempre son 0; solo se reescribe el header por chunk
    memset(tx_buf, 0, HDR_SIZE + CHUNK_SIZE);
}

static void read_jedec_id(uint8_t id[3])
{
    // Full duplex: mandamos 0x9F + 3 dummies. El primer byte recibido es
    // basura (la flash estaba procesando el comando), los 3 siguientes son el ID.
    spi_transaction_t t = {
        .length = 32,
        .flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA,
        .tx_data = {CMD_JEDEC_ID, 0x00, 0x00, 0x00},
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(flash, &t));
    id[0] = t.rx_data[1];
    id[1] = t.rx_data[2];
    id[2] = t.rx_data[3];
}

// Lee CHUNK_SIZE bytes desde addr; los datos quedan en rx_buf + HDR_SIZE
static void flash_read_chunk(uint32_t addr)
{
    // Header full-duplex: comando + direccion de 24 bits (big endian).
    // Durante esos 4 bytes la flash no dice nada util; despues escupe datos.
    tx_buf[0] = CMD_READ;
    tx_buf[1] = (addr >> 16) & 0xFF;
    tx_buf[2] = (addr >> 8) & 0xFF;
    tx_buf[3] = addr & 0xFF;

    // OJO: spi_device_transmit (bloqueante, cede la CPU) y NO
    // spi_device_polling_transmit (busy-wait). Con polling, el dump encadena
    // ~33ms de busy-wait por chunk sin dejar correr al IDLE0: el task
    // watchdog salta cada 5s y su mensaje de error se intercala en el
    // stream del dump, corrompiendolo.
    spi_transaction_t t = {
        .length = (HDR_SIZE + CHUNK_SIZE) * 8,
        .tx_buffer = tx_buf,
        .rx_buffer = rx_buf,
    };
    ESP_ERROR_CHECK(spi_device_transmit(flash, &t));
}

static void dump_flash(void)
{
    uint32_t crc = 0;

    // Silencio total durante el dump: solo bytes crudos. Se escriben directo
    // al driver USB-Serial-JTAG, por bloques y con espera bloqueante. Por
    // stdout el VFS escribe de a 1 byte y, si el ring buffer se llena,
    // descarta bytes sin avisar -> dump corrupto (CRC distinto).
    for (uint32_t addr = 0; addr < FLASH_SIZE; addr += CHUNK_SIZE) {
        flash_read_chunk(addr);
        crc = esp_rom_crc32_le(crc, rx_buf + HDR_SIZE, CHUNK_SIZE);
        usb_serial_jtag_write_bytes(rx_buf + HDR_SIZE, CHUNK_SIZE, portMAX_DELAY);
    }

    // Linea final en texto para el script (mismo FIFO: sale despues de los datos)
    printf("\nCRC32:%08lX\n", (unsigned long)crc);
    fflush(stdout);
}

void app_main(void)
{
    // Consola por USB-Serial-JTAG. El startup la deja en modo "simple"
    // (reads no-bloqueantes): hay que instalar el driver ANTES de
    // usb_serial_jtag_vfs_use_driver(), o el primer print entra en recursion
    // infinita (error "driver not initialized" logueado por el mismo VFS
    // roto) y paniquea.
    usb_serial_jtag_driver_config_t usj_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usj_cfg.tx_buffer_size = 8192;      // default 256 es muy chico para el dump
    esp_err_t usj_err = usb_serial_jtag_driver_install(&usj_cfg);
    if (usj_err != ESP_OK && usj_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(usj_err);
    }
    usb_serial_jtag_vfs_use_driver();   // getchar() pasa a ser bloqueante

    flash_init();

    uint8_t id[3];
    read_jedec_id(id);
    ESP_LOGI(TAG, "JEDEC ID: %02X %02X %02X", id[0], id[1], id[2]);
    if ((id[0] == 0x00 && id[1] == 0x00 && id[2] == 0x00) ||
        (id[0] == 0xFF && id[1] == 0xFF && id[2] == 0xFF)) {
        ESP_LOGW(TAG, "ID sospechoso: revisar cableado/alimentacion/SoC");
    }

    while (1) {
        printf("READY\n");
        fflush(stdout);

        // Esperar la orden 'D' del script. Con el driver USB-Serial-JTAG
        // getchar() bloquea; se descarta cualquier otro byte que llegue.
        int c;
        do {
            c = getchar();
        } while (c != 'D');

        dump_flash();
    }
}
