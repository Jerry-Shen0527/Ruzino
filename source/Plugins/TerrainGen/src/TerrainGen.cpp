#include "TerrainGen/TerrainGeneration.h"
#include "TerrainGen/api.h"

// TerrainGen Plugin Entry Point

namespace TerrainGen {

// Version information
TERRAINGEN_API const char* GetVersion()
{
    return "1.0.0";
}

TERRAINGEN_API const char* GetPluginName()
{
    return "TerrainGen";
}

}  // namespace TerrainGen
