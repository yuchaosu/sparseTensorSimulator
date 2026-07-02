`timescale 1ns/1ps

module tb_grid32x32;

    localparam integer DATA_WIDTH = 32;
    localparam integer NUM_ROWS   = 32;
    localparam integer NUM_COLS   = 32;
    localparam integer PROD_WIDTH = 2 * DATA_WIDTH;
    localparam integer ACC_WIDTH  = PROD_WIDTH + 16;
    localparam integer NUM_DIAGS  = NUM_ROWS + NUM_COLS - 1;

    reg clk;
    reg rst_n;
    reg enable;

    reg [NUM_COLS*DATA_WIDTH-1:0] north_data;
    reg [NUM_COLS-1:0]            north_valid;
    reg [NUM_ROWS*DATA_WIDTH-1:0] west_data;
    reg [NUM_ROWS-1:0]            west_valid;

    wire [NUM_COLS*DATA_WIDTH-1:0] south_data;
    wire [NUM_COLS-1:0]            south_valid;
    wire [NUM_ROWS*DATA_WIDTH-1:0] east_data;
    wire [NUM_ROWS-1:0]            east_valid;
    wire [NUM_DIAGS*ACC_WIDTH-1:0] diag_accumulation;
    wire [NUM_DIAGS-1:0]           diag_accumulation_valid;

    grid32x32 #(
        .DATA_WIDTH (DATA_WIDTH),
        .NUM_ROWS   (NUM_ROWS),
        .NUM_COLS   (NUM_COLS)
    ) dut (
        .clk                     (clk),
        .rst_n                   (rst_n),
        .enable                  (enable),
        .north_data              (north_data),
        .north_valid             (north_valid),
        .west_data               (west_data),
        .west_valid              (west_valid),
        .south_data              (south_data),
        .south_valid             (south_valid),
        .east_data               (east_data),
        .east_valid              (east_valid),
        .diag_accumulation       (diag_accumulation),
        .diag_accumulation_valid (diag_accumulation_valid)
    );

    initial begin
        clk = 1'b0;
        forever #5 clk = ~clk;
    end

    integer cycle;
    integer idx;
    integer sample_cycle;
    reg [ACC_WIDTH-1:0] diag_snapshot [0:NUM_DIAGS-1];

    initial begin
        rst_n       = 1'b0;
        enable      = 1'b0;
        north_data  = {NUM_COLS*DATA_WIDTH{1'b0}};
        west_data   = {NUM_ROWS*DATA_WIDTH{1'b0}};
        north_valid = {NUM_COLS{1'b0}};
        west_valid  = {NUM_ROWS{1'b0}};

        repeat (5) @(posedge clk);
        rst_n  = 1'b1;
        enable = 1'b1;

        for (cycle = 0; cycle < 24; cycle = cycle + 1) begin
            for (idx = 0; idx < NUM_COLS; idx = idx + 1) begin
                north_data[idx*DATA_WIDTH +: DATA_WIDTH] = cycle + idx;
            end
            for (idx = 0; idx < NUM_ROWS; idx = idx + 1) begin
                west_data[idx*DATA_WIDTH +: DATA_WIDTH] = (cycle * 2) + idx;
            end
            north_valid = {NUM_COLS{1'b1}};
            west_valid  = {NUM_ROWS{1'b1}};
            @(posedge clk);
        end

        north_valid = {NUM_COLS{1'b0}};
        west_valid  = {NUM_ROWS{1'b0}};
        north_data  = {NUM_COLS*DATA_WIDTH{1'b0}};
        west_data   = {NUM_ROWS*DATA_WIDTH{1'b0}};

        repeat (64) @(posedge clk);

        for (sample_cycle = 0; sample_cycle < NUM_DIAGS; sample_cycle = sample_cycle + 1) begin
            diag_snapshot[sample_cycle] = diag_accumulation[sample_cycle*ACC_WIDTH +: ACC_WIDTH];
        end

        $display("==== Sampled diagonal accumulations ====");
        $display("Diag 0   : %0d (valid=%0d)", diag_snapshot[0], diag_accumulation_valid[0]);
        $display("Diag 31  : %0d (valid=%0d)", diag_snapshot[31], diag_accumulation_valid[31]);
        $display("Diag 62  : %0d (valid=%0d)", diag_snapshot[NUM_DIAGS-1], diag_accumulation_valid[NUM_DIAGS-1]);
        $display("South lane 0 valid=%0d data=%0d", south_valid[0], south_data[DATA_WIDTH-1:0]);
        $display("East lane  0 valid=%0d data=%0d", east_valid[0], east_data[DATA_WIDTH-1:0]);

        $finish;
    end

endmodule
