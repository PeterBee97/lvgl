LVGL_PATH ?= ${shell pwd}/lvgl

CSRCS += $(shell find $(LVGL_PATH)/src -type f -name '*.c')
CSRCS += $(shell find $(LVGL_PATH)/demos -type f -name '*.c')
CSRCS += $(shell find $(LVGL_PATH)/examples -type f -name '*.c')
CFLAGS += "-I$(LVGL_PATH)"
CFLAGS += -I/home/peter/ambiq/vendor/ambiq/chips/apollo4x/apollo_sdks/AmbiqSuite_R4_3_2_Lite/AmbiqSuite/third_party/ThinkSi/NemaGFX_SDK/include/tsi/NemaGFX
CFLAGS += -I/home/peter/ambiq/vendor/ambiq/chips/apollo4x/apollo_sdks/AmbiqSuite_R4_3_2_Lite/AmbiqSuite/third_party/ThinkSi/config/apollo4l_nemagfx
