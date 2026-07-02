`timescale 1ns/1ps

module grid32x32 #(
    parameter integer DATA_WIDTH    = 32,
    parameter integer NUM_ROWS      = 32,
    parameter integer NUM_COLS      = 32,
    parameter integer PROD_WIDTH    = 2 * DATA_WIDTH,
    parameter integer ACC_WIDTH     = PROD_WIDTH + 16,
    parameter integer NUM_PES       = NUM_ROWS * NUM_COLS,
    parameter integer NUM_DIAGS     = NUM_ROWS + NUM_COLS - 1,
    parameter integer MAX_DIAG_LEN  = (NUM_ROWS < NUM_COLS) ? NUM_ROWS : NUM_COLS
) (
    input  wire                             clk,
    input  wire                             rst_n,
    input  wire                             enable,
    input  wire [NUM_COLS*DATA_WIDTH-1:0]   north_data,
    input  wire [NUM_COLS-1:0]              north_valid,
    input  wire [NUM_ROWS*DATA_WIDTH-1:0]   west_data,
    input  wire [NUM_ROWS-1:0]              west_valid,
    output wire [NUM_COLS*DATA_WIDTH-1:0]   south_data,
    output wire [NUM_COLS-1:0]              south_valid,
    output wire [NUM_ROWS*DATA_WIDTH-1:0]   east_data,
    output wire [NUM_ROWS-1:0]              east_valid,
    output wire [NUM_DIAGS*ACC_WIDTH-1:0]   diag_accumulation,
    output wire [NUM_DIAGS-1:0]             diag_accumulation_valid
);

    function integer pe_index;
        input integer row;
        input integer col;
        begin
            pe_index = row * NUM_COLS + col;
        end
    endfunction

    function integer diag_row_from_slot;
        input integer diag;
        input integer slot;
        integer row_start;
        begin
            if (diag < NUM_COLS) begin
                row_start = 0;
            end else begin
                row_start = diag - NUM_COLS + 1;
            end
            diag_row_from_slot = row_start + slot;
        end
    endfunction

    function integer diag_col_from_slot;
        input integer diag;
        input integer slot;
        integer col_start;
        begin
            if (diag < NUM_COLS) begin
                col_start = diag;
            end else begin
                col_start = NUM_COLS - 1;
            end
            diag_col_from_slot = col_start - slot;
        end
    endfunction

    wire [DATA_WIDTH-1:0] a_down_data   [0:NUM_PES-1];
    wire                  a_down_valid  [0:NUM_PES-1];
    wire [DATA_WIDTH-1:0] b_right_data  [0:NUM_PES-1];
    wire                  b_right_valid [0:NUM_PES-1];

    wire [PROD_WIDTH-1:0] pe_product    [0:NUM_PES-1];
    wire                  pe_product_v  [0:NUM_PES-1];

    wire [ACC_WIDTH-1:0]  diag_accum    [0:NUM_DIAGS-1];
    wire                  diag_valid    [0:NUM_DIAGS-1];

    genvar row, col;
    generate
        for (row = 0; row < NUM_ROWS; row = row + 1) begin : ROW_GEN
            for (col = 0; col < NUM_COLS; col = col + 1) begin : COL_GEN
                localparam integer PE_IDX = row * NUM_COLS + col;

                wire [DATA_WIDTH-1:0] a_in_wire;
                wire                  a_valid_wire;
                wire [DATA_WIDTH-1:0] b_in_wire;
                wire                  b_valid_wire;

                if (row == 0) begin : TOP_ROW
                    assign a_in_wire    = north_data[col*DATA_WIDTH +: DATA_WIDTH];
                    assign a_valid_wire = north_valid[col];
                end else begin : INNER_ROW
                    assign a_in_wire    = a_down_data[pe_index(row-1, col)];
                    assign a_valid_wire = a_down_valid[pe_index(row-1, col)];
                end

                if (col == 0) begin : LEFT_COL
                    assign b_in_wire    = west_data[row*DATA_WIDTH +: DATA_WIDTH];
                    assign b_valid_wire = west_valid[row];
                end else begin : INNER_COL
                    assign b_in_wire    = b_right_data[pe_index(row, col-1)];
                    assign b_valid_wire = b_right_valid[pe_index(row, col-1)];
                end

                pe #(
                    .DATA_WIDTH( DATA_WIDTH ),
                    .PROD_WIDTH( PROD_WIDTH )
                ) u_pe (
                    .clk           (clk),
                    .rst_n         (rst_n),
                    .enable        (enable),
                    .a_in          (a_in_wire),
                    .b_in          (b_in_wire),
                    .a_valid       (a_valid_wire),
                    .b_valid       (b_valid_wire),
                    .a_out         (a_down_data[PE_IDX]),
                    .b_out         (b_right_data[PE_IDX]),
                    .a_out_valid   (a_down_valid[PE_IDX]),
                    .b_out_valid   (b_right_valid[PE_IDX]),
                    .product_out   (pe_product[PE_IDX]),
                    .product_valid (pe_product_v[PE_IDX])
                );
            end
        end
    endgenerate

    genvar south_idx;
    generate
        for (south_idx = 0; south_idx < NUM_COLS; south_idx = south_idx + 1) begin : SOUTH_OUT
            localparam integer PE_IDX_S = (NUM_ROWS-1) * NUM_COLS + south_idx;
            assign south_data[south_idx*DATA_WIDTH +: DATA_WIDTH] = a_down_data[PE_IDX_S];
            assign south_valid[south_idx] = a_down_valid[PE_IDX_S];
        end
    endgenerate

    genvar east_idx;
    generate
        for (east_idx = 0; east_idx < NUM_ROWS; east_idx = east_idx + 1) begin : EAST_OUT
            localparam integer PE_IDX_E = east_idx * NUM_COLS + (NUM_COLS-1);
            assign east_data[east_idx*DATA_WIDTH +: DATA_WIDTH] = b_right_data[PE_IDX_E];
            assign east_valid[east_idx] = b_right_valid[PE_IDX_E];
        end
    endgenerate

    genvar diag, slot;
    generate
        for (diag = 0; diag < NUM_DIAGS; diag = diag + 1) begin : DIAG_GEN
            wire [MAX_DIAG_LEN*PROD_WIDTH-1:0] diag_value_bus;
            wire [MAX_DIAG_LEN-1:0]            diag_valid_bus;

            for (slot = 0; slot < MAX_DIAG_LEN; slot = slot + 1) begin : SLOT_GEN
                localparam integer ROW_ID = diag_row_from_slot(diag, slot);
                localparam integer COL_ID = diag_col_from_slot(diag, slot);
                if ((ROW_ID >= 0) && (ROW_ID < NUM_ROWS) && (COL_ID >= 0) && (COL_ID < NUM_COLS)) begin : CONNECTED
                    localparam integer PE_SLOT = pe_index(ROW_ID, COL_ID);
                    assign diag_value_bus[slot*PROD_WIDTH +: PROD_WIDTH] = pe_product[PE_SLOT];
                    assign diag_valid_bus[slot]                         = pe_product_v[PE_SLOT];
                end else begin : UNUSED
                    assign diag_value_bus[slot*PROD_WIDTH +: PROD_WIDTH] = {PROD_WIDTH{1'b0}};
                    assign diag_valid_bus[slot]                         = 1'b0;
                end
            end

            diagonal_accumulator #(
                .PROD_WIDTH       (PROD_WIDTH),
                .NUM_SOURCES      (MAX_DIAG_LEN),
                .ACC_WIDTH        (ACC_WIDTH)
            ) u_diag_accum (
                .clk              (clk),
                .rst_n            (rst_n),
                .enable           (enable),
                .value_flat       (diag_value_bus),
                .valid            (diag_valid_bus),
                .accumulation     (diag_accum[diag]),
                .accumulation_valid (diag_valid[diag])
            );
        end
    endgenerate

    genvar diag_idx;
    generate
        for (diag_idx = 0; diag_idx < NUM_DIAGS; diag_idx = diag_idx + 1) begin : DIAG_EXPORT
            assign diag_accumulation[diag_idx*ACC_WIDTH +: ACC_WIDTH] = diag_accum[diag_idx];
            assign diag_accumulation_valid[diag_idx] = diag_valid[diag_idx];
        end
    endgenerate

endmodule
