# usb_uac1: UAC 1.0 device layer on espressif/tinyusb, for the usb_audio component.
# The directory name is the component name (usb_uac1), as IDF resolves local components by it.
# Added to the build by usb_audio's __init__.py (add_idf_component with a local path).
set(priv_requires esp_timer heap)
if(${IDF_VERSION_MAJOR} LESS 6)
  list(APPEND priv_requires usb)  # USB PHY driver (esp_private/usb_phy.h)
endif()

idf_component_register(SRCS "usb_uac1.c"
                       INCLUDE_DIRS "include"
                       REQUIRES espressif__tinyusb
                       PRIV_REQUIRES ${priv_requires})

# TinyUSB is built with our tusb_config.h (it includes "tusb_config.h" from its include path)
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tusb_lib} PUBLIC "${COMPONENT_DIR}/tusb")
