idf_component_register(SRCS "spi_dumper_main.c"
                       PRIV_REQUIRES esp_driver_spi esp_driver_usb_serial_jtag vfs
                       INCLUDE_DIRS ".")
