CXX = g++
CXXFLAGS = -std=c++17 -O3 -Iinclude/ -Iexternal/
# Uncomment for debugging
#DEBUGFLAGS = -O0 -g
#DEBUGFLAGS = -DDEBUG_MEM_OUTPUT -DDEBUG_MSWITCH_FUNC

# Executable names (without paths)
BINARIES = matrixMulti matrixMultiBlock matrixMultiBlockDiagonal matrixMultiHam HBMHamiltonian HBMHamiltonianScheduled HBMHamiltonianPrefetch

# All shared .cpp sources in src/
COMMON_SOURCES = $(wildcard src/*.cpp)

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
	$(CXX) $(CXXFLAGS) $(DEBUGFLAGS) -o $(OUTDIR)/$@ $(COMMON_OBJS) matrixMultiBlockDiagonal.cpp

# matrixMultiHam: link main source
matrixMultiHam: $(COMMON_OBJS)
	@mkdir -p $(OUTDIR)
	$(CXX) $(CXXFLAGS) $(DEBUGFLAGS) -o $(OUTDIR)/$@ $(COMMON_OBJS) main/matrixMultiplyHam.cpp

# HBMHamiltonian: link main source
HBMHamiltonian: $(COMMON_OBJS)
	@mkdir -p $(OUTDIR)
	$(CXX) $(CXXFLAGS) $(DEBUGFLAGS) -o $(OUTDIR)/$@ $(COMMON_OBJS) main/HBMHamiltonian.cpp

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

# Clean rule
clean:
	rm -rf $(OBJSDIR) $(OUTDIR)
