CXX = g++
CXXFLAGS = -std=c++17 -O3 -Wall -Wextra -I.

ARM_CXX = arm-none-eabi-g++
ARM_FLAGS = -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard -std=c++17 -O3 -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti -I.

.PHONY: test arm clean

test: test_mcu_firmware.exe
	.\test_mcu_firmware.exe

test_mcu_firmware.exe: test_firmware.cpp main_mcu.cpp stm32_mixer_app.hpp embedded_dsp.hpp hw_hal.h packet_protocol.h
	$(CXX) $(CXXFLAGS) test_firmware.cpp main_mcu.cpp -o test_mcu_firmware.exe

# STM32H743VIT6 (Cortex-M7 FPU) 오브젝트 빌드 (arm-none-eabi-g++ 설치 환경용)
arm: main_mcu.o

main_mcu.o: main_mcu.cpp stm32_mixer_app.hpp embedded_dsp.hpp hw_hal.h packet_protocol.h
	$(ARM_CXX) $(ARM_FLAGS) -c main_mcu.cpp -o main_mcu.o

clean:
	del /Q test_mcu_firmware.exe main_mcu.o 2>nul || rm -f test_mcu_firmware.exe main_mcu.o

