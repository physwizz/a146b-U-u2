#
# Makefile for grip sensors
#

ifneq (,$(filter m,$(CONFIG_SENSORS_A96T396) $(CONFIG_SENSORS_A96T396_SUB) $(CONFIG_SENSORS_A96T396_SUB2) $(CONFIG_SENSORS_A96T396_WIFI)))
	obj-m += a96t396.o
else ifneq (,$(filter y,$(CONFIG_SENSORS_A96T396) $(CONFIG_SENSORS_A96T396_SUB) $(CONFIG_SENSORS_A96T396_SUB2) $(CONFIG_SENSORS_A96T396_WIFI)))
	obj-y += a96t396.o
endif