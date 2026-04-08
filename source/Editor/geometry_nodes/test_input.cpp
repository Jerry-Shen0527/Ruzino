#include <Eigen/Dense>

#include "geom_node_base.h"
#include "nodes/core/def/node_def.hpp"

NODE_DEF_OPEN_SCOPE

// Declaration function: Sets up the input and output for the node
NODE_DECLARATION_FUNCTION(test_input)
{
    // Define the input as a vector of numbers
    b.add_input<std::vector<glm::vec3>>("Input");
    b.add_output<Eigen::MatrixXd>("Output");
}

// Execution function: Processes the input and produces the output
NODE_EXECUTION_FUNCTION(test_input)
{
    auto input = params.get_input<std::vector<glm::vec3>>("Input");
    size_t numRows = input.size();       // Number of rows (N)
    Eigen::MatrixXd output(numRows, 3);  // Create an N x 3 matrix

    // Populate the Eigen matrix with data from std::vector<glm::vec3>
    for (size_t i = 0; i < numRows; ++i) {
        const glm::vec3& vec = input[i];
        output(i, 0) = static_cast<double>(vec[0]);  // X component
        output(i, 1) = static_cast<double>(vec[1]);  // Y component
        output(i, 2) = static_cast<double>(vec[2]);  // Z component
    }

    return true;
}

// UI function: Describes the node's interface in the editor
NODE_DECLARATION_UI(test_input);

NODE_DEF_CLOSE_SCOPE