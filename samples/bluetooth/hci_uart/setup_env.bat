set NCS_TAG=v1.2.0
set ZEPHYR_BASE=K:\proj_1\nrf9160\code\3b\ncs\zephyr
set MY_TOOLCHAIN_DIR=C:\proj_nodic_sdk\v1.7.0\toolchain
set MY_GNUARMEMB_DIR=C:\proj_gnuarmemb_7.3.1

set PATH=%MY_GNUARMEMB_DIR%\bin;%MY_TOOLCHAIN_DIR%;%MY_TOOLCHAIN_DIR%\mingw64\bin;%MY_TOOLCHAIN_DIR%\opt\bin;%MY_TOOLCHAIN_DIR%\opt\bin\Scripts;%MY_TOOLCHAIN_DIR%\segger_embedded_studio\bin;%PATH%
set GNUARMEMB_TOOLCHAIN_PATH=%MY_GNUARMEMB_DIR%
set Python3_ROOT=%MY_TOOLCHAIN_DIR%\opt\bin
set PYTHONPATH=%MY_TOOLCHAIN_DIR%\opt\bin;%MY_TOOLCHAIN_DIR%\opt\bin\Lib;%MY_TOOLCHAIN_DIR%\opt\bin\Lib\site-packages
set tcdir=%MY_TOOLCHAIN_DIR%
set ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb

arm-none-eabi-gcc --version
west --version
cmake --version
dtc --version
ninja --version
python --version

echo to build target run "west build -b nrf52840_pca10090"
