CXX = g++
CXXFLAGS = -std=c++17 -O3 -Iinclude/ -Iexternal/
# Uncomment for debugging
#DEBUGFLAGS = -O0 -g

# Executables: the DIAMOND accelerator driver + the baseline-comparison tool.
BINARIES = diamond accel_compare

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
INCLUDES = $(wildcard include/*.h)
OBJSDIR = objs
OUTDIR = outputs
COMMON_OBJS = $(patsubst src/%, $(OBJSDIR)/%, $(patsubst %.cpp,%.o,$(COMMON_SOURCES)))

.PHONY: all clean $(BINARIES)

all: $(BINARIES)

# diamond: the cycle-accurate DIAMOND accelerator (offset-space DIA convolution on the
# S*S PE mesh, convOnGrid), DRAM modeled in-loop by Ramulator 2.1 (SOTA HBM4).
diamond: $(COMMON_OBJS) $(RAM_OBJ)
	@mkdir -p $(OUTDIR)
	$(CXX) $(CXXFLAGS) $(DEBUGFLAGS) -o $(OUTDIR)/$@ $(COMMON_OBJS) $(RAM_OBJ) main/diamond.cpp $(RAMLINK)

# conv_breakdown: STANDALONE per-part cycle breakdown of the offset-space convolution
# (compute vs cbalance NoC/reduce gather vs fill/drain), to show how much of the gather
# routing OVERLAPS compute. Cycle domain only -> no Ramulator/HBM, links against PE alone.
conv_breakdown: $(COMMON_OBJS)
	@mkdir -p $(OUTDIR)
	$(CXX) $(CXXFLAGS) $(DEBUGFLAGS) -o $(OUTDIR)/$@ $(COMMON_OBJS) main/conv_breakdown.cpp

# accel_compare: DIAMOND vs TPU vs Trapezoid baselines on the shared PE mesh + same HBM.
accel_compare: $(COMMON_OBJS) $(RAM_OBJ)
	@mkdir -p $(OUTDIR)
	$(CXX) $(CXXFLAGS) $(DEBUGFLAGS) -o $(OUTDIR)/$@ $(COMMON_OBJS) $(RAM_OBJ) main/accel_compare.cpp $(RAMLINK)

# Shared src/ objects
$(OBJSDIR)/%.o: src/%.cpp $(INCLUDES)
	@mkdir -p $(OBJSDIR)
	$(CXX) $(CXXFLAGS) $(DEBUGFLAGS) -c $< -o $@

# Ramulator adapter: C++20 + Ramulator headers, config path baked in
$(RAM_OBJ): $(RAM_SRC) include/RamulatorHBM.h
	@mkdir -p $(OBJSDIR)
	$(CXX) $(RAMFLAGS) $(DEBUGFLAGS) -DRAMULATOR_HBM_CONFIG='"$(RAM_CONFIG)"' -c $< -o $@

clean:
	rm -rf $(OBJSDIR) $(OUTDIR)
