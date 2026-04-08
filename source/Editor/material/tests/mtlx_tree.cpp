#include <gtest/gtest.h>

#include "MCore/MaterialXNodeTree.hpp"
#include "MCore/MaterialXNodeTreeWidget.h"

using namespace Ruzino;

int main()
{
    MaterialXNodeTreeDescriptor descriptor;
    MaterialXNodeTree tree(
        "resources/Materials/Examples/StandardSurface/"
        "standard_surface_marble_solid.mtlx",
        std::make_shared<MaterialXNodeTreeDescriptor>());

    std::cout << tree.nodes.size() << std::endl;
    std::cout << tree.links.size() << std::endl;
}