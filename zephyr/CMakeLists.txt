if(CONFIG_TINYUSB)
	add_library(TINYUSB INTERFACE)

	target_include_directories(TINYUSB INTERFACE
		${CMAKE_CURRENT_SOURCE_DIR}
		${ZEPHYR_TINYUSB_MODULE_DIR}/src/class/video
		${ZEPHYR_TINYUSB_MODULE_DIR}/src/common
		${ZEPHYR_TINYUSB_MODULE_DIR}/src/device
		${ZEPHYR_TINYUSB_MODULE_DIR}/src/osal
		${ZEPHYR_TINYUSB_MODULE_DIR}/src
		${ZEPHYR_TINYUSB_MODULE_DIR}/zephyr
	)

	# target_compile_definitions(TINYUSB INTERFACE
	# SOC_FAMILY_RENESAS
	# )
	set(incldue_dirs
		${ZEPHYR_TINYUSB_MODULE_DIR}/src/class/video
		${ZEPHYR_TINYUSB_MODULE_DIR}/src/common
		${ZEPHYR_TINYUSB_MODULE_DIR}/src/device
		${ZEPHYR_TINYUSB_MODULE_DIR}/src/osal
		${ZEPHYR_TINYUSB_MODULE_DIR}/src
		${ZEPHYR_TINYUSB_MODULE_DIR}/zephyr
	)

	zephyr_library()
	zephyr_library_sources(
		${ZEPHYR_TINYUSB_MODULE_DIR}/src/class/video/video_device.c
		# ${ZEPHYR_TINYUSB_MODULE_DIR}/demo/video_static_mjpeg_template.c
		${ZEPHYR_TINYUSB_MODULE_DIR}/zephyr/init.c
		${ZEPHYR_TINYUSB_MODULE_DIR}/src/common/tusb_fifo.c
		${ZEPHYR_TINYUSB_MODULE_DIR}/src/device/usbd_control.c
		${ZEPHYR_TINYUSB_MODULE_DIR}/src/device/usbd.c
		${ZEPHYR_TINYUSB_MODULE_DIR}/src/tusb.c
	)

	zephyr_library_sources_ifdef(CONFIG_TINYUSB_RENESAS
		${ZEPHYR_TINYUSB_MODULE_DIR}/src/portable/renesas/zephyr_ra8/dcd.c
	)

	zephyr_include_directories(${incldue_dirs})


	zephyr_library_link_libraries(TINYUSB)
	target_link_libraries(TINYUSB INTERFACE zephyr_interface)
endif()
