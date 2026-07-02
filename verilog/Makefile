IVERILOG ?= iverilog
VVP     ?= vvp

SRC = pe.v \
      diagonal_accumulator.v \
      grid32x32.v \
      tb_grid32x32.v

all: sim

sim: grid_tb
	$(VVP) grid_tb

grid_tb: $(SRC)
	$(IVERILOG) -g2012 -Wall -Wimplicit -o $@ $(SRC)

clean:
	rm -f grid_tb

.PHONY: all sim clean
