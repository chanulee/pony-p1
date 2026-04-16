Import("env")

# TFT_eSPI configuration via build flags (no User_Setup.h needed)
env.Append(BUILD_FLAGS=[
    "-DUSER_SETUP_LOADED=1",
    "-DST7735_DRIVER=1",
    "-DTFT_WIDTH=128",
    "-DTFT_HEIGHT=160",
    "-DST7735_GREENTAB160x128",
    "-DTFT_CS=14",
    "-DTFT_DC=7",
    "-DTFT_RST=11",
    "-DTFT_MOSI=13",
    "-DTFT_SCLK=12",
    "-DSPI_FREQUENCY=27000000",
    "-DLOAD_GLCD=1",
    "-DLOAD_FONT2=1",
    "-DLOAD_FONT4=1",
    "-DLOAD_GFXFF=1",
])
