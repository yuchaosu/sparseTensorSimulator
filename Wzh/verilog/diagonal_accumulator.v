`timescale 1ns/1ps

module diagonal_accumulator #(
    parameter integer PROD_WIDTH   = 64,
    parameter integer NUM_SOURCES  = 32,
    parameter integer ACC_WIDTH    = PROD_WIDTH + 16
) (
    input  wire                              clk,
    input  wire                              rst_n,
    input  wire                              enable,
    input  wire [NUM_SOURCES*PROD_WIDTH-1:0] value_flat,
    input  wire [NUM_SOURCES-1:0]            valid,
    output reg  [ACC_WIDTH-1:0]              accumulation,
    output reg                               accumulation_valid
);

    integer idx;
    reg signed [ACC_WIDTH-1:0] accumulation_next;
    reg                         acc_valid_next;
    wire signed [PROD_WIDTH-1:0] values   [0:NUM_SOURCES-1];

    generate
        genvar i;
        for (i = 0; i < NUM_SOURCES; i = i + 1) begin : UNPACK
            assign values[i] = value_flat[i*PROD_WIDTH +: PROD_WIDTH];
        end
    endgenerate

    always @(*) begin
        accumulation_next = accumulation;
        acc_valid_next    = 1'b0;

        if (enable) begin
            for (idx = 0; idx < NUM_SOURCES; idx = idx + 1) begin
                if (valid[idx]) begin
                    accumulation_next = accumulation_next + {{(ACC_WIDTH-PROD_WIDTH){values[idx][PROD_WIDTH-1]}}, values[idx]};
                    acc_valid_next    = 1'b1;
                end
            end
        end
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            accumulation        <= {ACC_WIDTH{1'b0}};
            accumulation_valid  <= 1'b0;
        end else begin
            accumulation        <= accumulation_next;
            accumulation_valid  <= acc_valid_next;
        end
    end

endmodule
