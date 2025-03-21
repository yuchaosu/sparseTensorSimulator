#include "CompilerWSDataflow.h"
#include "WSDataflowMemory.h"
#include "Tile.h"

CompilerWSDataflow::CompilerWSDataflow(DNNLayer* dnn_layer, WSDataflowMemory* memory_controller) {
    this->dnn_layer = dnn_layer;
    this->memory_controller = memory_controller;
}

void CompilerWSDataflow::configureDataflow(std::vector<std::vector<SparseVN>> &input_tiles) {
    // Configure how data flows through your PE grid
    // ...
}

void CompilerWSDataflow::mapDataflowToArchitecture() {
    // Create mapping from your dataflow to hardware components
    // Determine which PE calculates which operations
    // ...
}

void CompilerWSDataflow::generatePreloadTiles() {
    // Generate tiles for preload phase
    // ...
}

void CompilerWSDataflow::generateComputeTiles() {
    // Generate tiles for compute phase
    // ...
}

void CompilerWSDataflow::scheduleTiles() {
    // Schedule which tiles go to which PEs and when
    // ...
}