CXX = g++
CXXFLAGS = -std=c++17 -O3 -Wall -Wextra -Iinclude
WIN_LIBS = -lgdi32 -lcomctl32 -lcomdlg32 -lmsimg32 -lole32 -lws2_32 -lmfplat -lmfreadwrite -lmfuuid

all: smart_mixer_gui.exe hardware_sim_gui.exe smart_mixer.exe test_suite.exe

smart_mixer_gui.exe: src/gui_main.cpp include/*.hpp
	$(CXX) $(CXXFLAGS) -mwindows src/gui_main.cpp $(WIN_LIBS) -o smart_mixer_gui.exe

hardware_sim_gui.exe: src/hardware_sim_main.cpp include/*.hpp
	$(CXX) $(CXXFLAGS) -mwindows src/hardware_sim_main.cpp $(WIN_LIBS) -o hardware_sim_gui.exe

smart_mixer.exe: src/main.cpp include/*.hpp
	$(CXX) $(CXXFLAGS) src/main.cpp -o smart_mixer.exe

test_suite.exe: tests/test_main.cpp include/*.hpp
	$(CXX) $(CXXFLAGS) tests/test_main.cpp $(WIN_LIBS) -o test_suite.exe

test: test_suite.exe
	./test_suite.exe

clean:
	rm -f smart_mixer_gui.exe hardware_sim_gui.exe smart_mixer.exe test_suite.exe

