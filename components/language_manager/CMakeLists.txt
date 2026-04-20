idf_component_register(
    SRCS "language_manager.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash   # if your implementation uses nvs_flash; otherwise remove it
)
