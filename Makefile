CXX      := clang++
CXXFLAGS := -std=c++17 -g -Wall -Wextra -fno-omit-frame-pointer
FRAMEWORKS := -framework CoreFoundation -framework AppKit
TARGET   := vst3_debugger
SRCS     := vst3_debugger.cpp host_window.mm

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) $(FRAMEWORKS) -o $@ $(SRCS)
	@echo "Build complete: ./$(TARGET)"

clean:
	rm -f $(TARGET)
