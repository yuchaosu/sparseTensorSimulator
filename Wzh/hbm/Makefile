# Makefile for Standalone HBM Controller

CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -g
LDFLAGS = 

# Source files
SOURCES = AcceleratorHBM.cpp main.cpp
OBJECTS = $(SOURCES:.cpp=.o)
HEADERS = AcceleratorHBM.h

# Target executable
TARGET = hbm_example

# Default target
all: $(TARGET)

# Link
$(TARGET): $(OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $^
	@echo "Build complete: $(TARGET)"
	@echo "Run with: ./$(TARGET)"

# Compile
%.o: %.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Clean
clean:
	rm -f $(OBJECTS) $(TARGET)
	@echo "Clean complete"

# Run
run: $(TARGET)
	./$(TARGET)

# Debug build
debug: CXXFLAGS += -DDEBUG -O0
debug: clean all

# Optimized build
release: CXXFLAGS += -O3 -DNDEBUG
release: clean all

# Help
help:
	@echo "Available targets:"
	@echo "  all     - Build the project (default)"
	@echo "  clean   - Remove build artifacts"
	@echo "  run     - Build and run the example"
	@echo "  debug   - Build with debug symbols"
	@echo "  release - Build optimized release version"
	@echo ""
	@echo "Configuration:"
	@echo "  - 256 GB/s bandwidth"
	@echo "  - 8 channels with 128-bit interface each"
	@echo "  - 1 GHz clock (2 Gbps DDR)"
	@echo "  - HBM2 timing characteristics"

.PHONY: all clean run debug release help