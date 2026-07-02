CXX = g++
CXXFLAGS = -std=c++17 -O3 -Iinclude/ -Iexternal/
# Uncomment for debugging
#DEBUGFLAGS = -O0 -g
#DEBUGFLAGS = -DDEBUG_MEM_OUTPUT -DDEBUG_MSWITCH_FUNC

# Executable names (without paths)
BINARIES = matrixMulti matrixMultiBlock matrixMultiBlockDiagonal matrixMultiHam HBMHamiltonian HBMHamiltonianScheduled HBMHamiltonianPrefetch

# Ramulator 2.1 in-loop DRAM model (SOTA HBM4). The adapter TU must be built as
# C++20 and linked against libramulator.so; everything else stays C++17.
RAMULATOR_DIR = external/ramulator2
RAM_SRC = src/RamulatorHBM.cpp
RAM_OBJ = $(OBJSDIR)/RamulatorHBM.o
RAMFLAGS = -std=c++20 -O3 -I$(RAMULATOR_DIR)/src
RAM_CONFIG = $(abspath config/hbm4_sota.yaml)
RAMLINK = -L$(RAMULATOR_DIR) -lramulator -Wl,-rpath,$(abspath $(RAMULATOR_DIR))

# All shared .cpp sources in src/ (the Ramulator adapter is built separately as C++20)
COMMON_SOURCES = $(filter-out $(RAM_SRC),$(wildcard src/*.cpp))

# All headers
INCLUDES = $(wildcard include/*.h)

# Object directory
OBJSDIR = objs

# Output directory for executables
OUTDIR = outputs

# Common objects
COMMON_OBJS = $(patsubst src/%, $(OBJSDIR)/%, $(patsubst %.cpp,%.o,$(COMMON_SOURCES)))

.PHONY: all clean $(BINARIES)

# Default target: build everything
all: $(BINARIES)

# matrixMulti: link main source
matrixMulti: $(COMMON_OBJS)
	@mkdir -p $(OUTDIR)
	$(CXX) $(CXXFLAGS) $(DEBUGFLAGS) -o $(OUTDIR)/$@ $(COMMON_OBJS) matrixMulti.cpp

# matrixMultiBlock: link main source
matrixMultiBlock: $(COMMON_OBJS)
	@mkdir -p $(OUTDIR)
	$(CXX) $(CXXFLAGS) $(DEBUGFLAGS) -o $(OUTDIR)/$@ $(COMMON_OBJS) matrixMultiBlock.cpp

matrixMultiBlockDiagonal: $(COMMON_OBJS)
	@mkdir -p $(OUTDIR)
	$(CXX) $(CXXFLAGS) $(DEBUGFLAGS) -o $(OUTDIR)/$@ $(COMMON_OBJS) main/matrixMultiBlockDiagonal.cpp

# matrixMultiHam: link main source
matrixMultiHam: $(COMMON_OBJS)
	@mkdir -p $(OUTDIR)
	$(CXX) $(CXXFLAGS) $(DEBUGFLAGS) -o $(OUTDIR)/$@ $(COMMON_OBJS) main/matrixMultiplyHam.cpp

# HBMHamiltonian: DRAM modeled by Ramulator 2.1 (SOTA HBM4), linked in-loop
HBMHamiltonian: $(COMMON_OBJS) $(RAM_OBJ)
	@mkdir -p $(OUTDIR)
	$(CXX) $(CXXFLAGS) $(DEBUGFLAGS) -o $(OUTDIR)/$@ $(COMMON_OBJS) $(RAM_OBJ) main/HBMHamiltonian.cpp $(RAMLINK)

# HBMHamiltonianScheduled: link main source with scheduling heuristic
HBMHamiltonianScheduled: $(COMMON_OBJS)
	@mkdir -p $(OUTDIR)
	$(CXX) $(CXXFLAGS) $(DEBUGFLAGS) -o $(OUTDIR)/$@ $(COMMON_OBJS) main/HBMHamiltonianScheduled.cpp

# HBMHamiltonianPrefetch: link new prefetch-enabled main
HBMHamiltonianPrefetch: $(COMMON_OBJS)
	@mkdir -p $(OUTDIR)
	$(CXX) $(CXXFLAGS) $(DEBUGFLAGS) -o $(OUTDIR)/$@ $(COMMON_OBJS) main/HBMHamiltonianPrefetch.cpp

# Rule to build .o files from src/
$(OBJSDIR)/%.o: src/%.cpp $(INCLUDES)
	@mkdir -p $(OBJSDIR)
	$(CXX) $(CXXFLAGS) $(DEBUGFLAGS) -c $< -o $@

# Ramulator adapter: C++20 + Ramulator headers, config path baked in
$(RAM_OBJ): $(RAM_SRC) include/RamulatorHBM.h
	@mkdir -p $(OBJSDIR)
	$(CXX) $(RAMFLAGS) $(DEBUGFLAGS) -DRAMULATOR_HBM_CONFIG='"$(RAM_CONFIG)"' -c $< -o $@

# Clean rule
clean:
	rm -rf $(OBJSDIR) $(OUTDIR)
