`timescale 1ns/1ps

module pe #(
    parameter integer DATA_WIDTH = 32,
    parameter integer PROD_WIDTH = 2 * DATA_WIDTH
) (
    input  wire                     clk,
    input  wire                     rst_n,
    input  wire                     enable,
    input  wire [DATA_WIDTH-1:0]    a_in,
    input  wire [DATA_WIDTH-1:0]    b_in,
    input  wire                     a_valid,
    input  wire                     b_valid,
    output reg  [DATA_WIDTH-1:0]    a_out,
    output reg  [DATA_WIDTH-1:0]    b_out,
    output reg                      a_out_valid,
    output reg                      b_out_valid,
    output reg  [PROD_WIDTH-1:0]    product_out,
    output reg                      product_valid
);

    wire signed [DATA_WIDTH-1:0] signed_a = a_in;
    wire signed [DATA_WIDTH-1:0] signed_b = b_in;
    wire signed [PROD_WIDTH-1:0] signed_product = signed_a * signed_b;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            a_out        <= {DATA_WIDTH{1'b0}};
            b_out        <= {DATA_WIDTH{1'b0}};
            a_out_valid  <= 1'b0;
            b_out_valid  <= 1'b0;
            product_out  <= {PROD_WIDTH{1'b0}};
            product_valid<= 1'b0;
        end else begin
            if (enable) begin
                a_out       <= a_in;
                b_out       <= b_in;
                a_out_valid <= a_valid;
                b_out_valid <= b_valid;

                if (a_valid && b_valid) begin
                    product_out   <= signed_product;
                    product_valid <= 1'b1;
                end else begin
                    product_valid <= 1'b0;
                end
            end else begin
                a_out_valid   <= 1'b0;
                b_out_valid   <= 1'b0;
                product_valid <= 1'b0;
            end
        end
    end

endmodule
