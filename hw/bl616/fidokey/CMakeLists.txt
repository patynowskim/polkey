cmake_minimum_required(VERSION 3.15)

set(CONFIG_FREERTOS 1)
set(CONFIG_CHERRYUSB 1)
set(CONFIG_CHERRYUSB_DEVICE 1)
set(CONFIG_CHERRYUSB_DEVICE_HID 1)

find_package(bouffalo_sdk REQUIRED HINTS $ENV{BL_SDK_BASE})

sdk_add_include_directories(. src)
sdk_add_compile_definitions(-DCONFIG_USB_HS)
if(DEFINED ENV{POLKEY_ALLOW_ATT_PROVISIONING})
    sdk_add_compile_definitions(-DPOLKEY_ALLOW_ATT_PROVISIONING)  # PROVISION_ATTESTATION command (off by default)
endif()

target_sources(app PRIVATE
    src/usb_descriptors.c
    src/dirtyjtag.c
    src/jtag_gpio.c
    src/ctaphid.c
    src/cbor.c
    src/ctap2.c
    src/fpga.c
    src/store.c
    src/sha256.c
    src/aes.c
    src/pin.c
    src/ctap2_mgmt.c
    src/u2f.c
    src/sha1.c
    src/gcm.c
    src/vault.c
    src/kem_cmd.c
    src/fp.c
    src/oled.c
)

sdk_set_main_file(src/main.c)

project(bl616_fidokey)
