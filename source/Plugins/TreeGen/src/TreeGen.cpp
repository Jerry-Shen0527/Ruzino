#include "TreeGen/TreeGrowth.h"
#include "TreeGen/api.h"

// TreeGen Plugin Entry Point
// This file provides a placeholder to ensure the library builds correctly

namespace TreeGen {

// Version information - export this to force lib generation
TREEGEN_API const char* GetVersion()
{
    return "1.0.0";
}

TREEGEN_API const char* GetPluginName()
{
    return "TreeGen";
}

}  // namespace TreeGen
