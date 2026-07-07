idf_component_register(
    SRCS
        "machine.c" "machine_ftms.c" "machine_ifit.c"
        "ftms_parse.c" "ifit_parse.c" "rsc_encode.c"
        "garmin_rsc.c" "ftms_devlist.c"
        "ctrl_dispatch.c" "serial_ctrl.c" "nus_ctrl.c"
        "ctrl_svc.c" "ctrl_frames.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        bt nvs_flash
        esp_driver_uart
        esp_driver_usb_serial_jtag
        vfs
)
