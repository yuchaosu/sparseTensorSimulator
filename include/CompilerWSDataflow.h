#ifndef __COMPILERWSDATAFLOW__H__
#define __COMPILERWSDATAFLOW__H__

#include "Compiler.h"
#include "DNNLayer.h"
#include "WSDataflowMemory.h"
#include <vector>

class CompilerWSDataflow : public Compiler {
private:
    DNNLayer* dnn_layer;
    WSDataflowMemory* memory_controller;
    
public:
    CompilerWSDataflow(DNNLayer* dnn_layer, WSDataflowMemory* memory_controller);
    void configureDataflow(std::vector<std::vector<SparseVN>> &input_tiles);
    void mapDataflowToArchitecture();
    void generatePreloadTiles();
    void generateComputeTiles();
    void scheduleTiles();
    
    // Other necessary methods...
};

#endif 