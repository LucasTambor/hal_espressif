SOC_NAME := $(IDF_TARGET)

COMPONENT_SRCDIRS := .
COMPONENT_ADD_INCLUDEDIRS := include
COMPONENT_PRIV_INCLUDEDIRS := private_include
COMPONENT_ADD_LDFRAGMENTS += linker.lf

-include $(COMPONENT_PATH)/port/$(SOC_NAME)/component.mk