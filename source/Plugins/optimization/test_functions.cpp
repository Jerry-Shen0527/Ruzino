#include <Eigen/Eigen>
#include <autodiff/forward/dual.hpp>
#include <autodiff/forward/dual/eigen.hpp>
#include <autodiff/forward/real.hpp>
#include <autodiff/forward/real/eigen.hpp>
#include <autodiff/reverse/var.hpp>
#include <autodiff/reverse/var/eigen.hpp>

#include "nodes/core/def/node_def.hpp"
using namespace autodiff;

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(test_function_1)
{
    b.add_output<std::function<var(const ArrayXvar&)>>("Function");
}

NODE_EXECUTION_FUNCTION(test_function_1)
{
    auto f = [](const ArrayXvar& x) { return (x * x).sum(); };
    params.set_output<std::function<var(const ArrayXvar&)>>(
        "Function", std::move(f));
    return true;
}

NODE_DECLARATION_FUNCTION(test_function_2)
{
    b.add_output<std::function<var(const ArrayXvar&)>>("Function");
}

NODE_EXECUTION_FUNCTION(test_function_2)
{
    auto f = [](const ArrayXvar& x) { return sqrt((x * x).sum()); };
    params.set_output<std::function<var(const ArrayXvar&)>>(
        "Function", std::move(f));
    return true;
}

NODE_DECLARATION_FUNCTION(test_function_3)
{
    b.add_output<std::function<var(var)>>("Function");
}

NODE_EXECUTION_FUNCTION(test_function_3)
{
    auto f = [](var x) { return 2 * x; };
    params.set_output<std::function<var(var)>>("Function", std::move(f));
    return true;
}

NODE_DECLARATION_FUNCTION(test_function_4)
{
    b.add_output<std::function<dual(const ArrayXdual&)>>("Function");
}

NODE_EXECUTION_FUNCTION(test_function_4)
{
    auto f = [](const ArrayXdual& x) { return (x * x).sum(); };
    params.set_output<std::function<dual(const ArrayXdual&)>>(
        "Function", std::move(f));
    return true;
}

NODE_DECLARATION_FUNCTION(test_function_5)
{
    b.add_output<std::function<dual(const ArrayXdual&)>>("Function");
}

NODE_EXECUTION_FUNCTION(test_function_5)
{
    auto f = [](const ArrayXdual& x) { return sqrt((x * x).sum()); };
    params.set_output<std::function<dual(const ArrayXdual&)>>(
        "Function", std::move(f));
    return true;
}

NODE_DECLARATION_FUNCTION(test_function_6)
{
    b.add_output<std::function<dual(dual)>>("Function");
}

NODE_EXECUTION_FUNCTION(test_function_6)
{
    auto f = [](dual x) { return 2 * x; };
    params.set_output<std::function<dual(dual)>>("Function", std::move(f));
    return true;
}

NODE_DECLARATION_FUNCTION(test_function_7)
{
    b.add_output<std::function<dual2nd(const ArrayXdual2nd&)>>("Function");
}

NODE_EXECUTION_FUNCTION(test_function_7)
{
    auto f = [](const ArrayXdual2nd& x) { return (x * x).sum(); };
    params.set_output<std::function<dual2nd(const ArrayXdual2nd&)>>(
        "Function", std::move(f));
    return true;
}

NODE_DECLARATION_FUNCTION(test_function_8)
{
    b.add_output<std::function<real(const ArrayXreal&)>>("Function");
}

NODE_EXECUTION_FUNCTION(test_function_8)
{
    auto f = [](const ArrayXreal& x) { return (x * x).sum(); };
    params.set_output<std::function<real(const ArrayXreal&)>>(
        "Function", std::move(f));
    return true;
}

NODE_DECLARATION_FUNCTION(test_function_9)
{
    b.add_output<std::function<real(const ArrayXreal&)>>("Function");
}

NODE_EXECUTION_FUNCTION(test_function_9)
{
    auto f = [](const ArrayXreal& x) { return sqrt((x * x).sum()); };
    params.set_output<std::function<real(const ArrayXreal&)>>(
        "Function", std::move(f));
    return true;
}

NODE_DECLARATION_FUNCTION(test_function_10)
{
    b.add_output<std::function<real(real)>>("Function");
}

NODE_EXECUTION_FUNCTION(test_function_10)
{
    auto f = [](real x) { return 2 * x; };
    params.set_output<std::function<real(real)>>("Function", std::move(f));
    return true;
}

NODE_DECLARATION_FUNCTION(test_function_11)
{
    b.add_input<std::function<dual(const ArrayXdual&)>>("Function_in");
    b.add_output<std::function<dual(const ArrayXdual&)>>("Function_result");
}

NODE_EXECUTION_FUNCTION(test_function_11)
{
    auto f =
        params.get_input<std::function<dual(const ArrayXdual&)>>("Function_in");
    auto g = [f](const ArrayXdual& x) {
        dual y = 2 * f(x);
        return y;
    };
    params.set_output<std::function<dual(const ArrayXdual&)>>(
        "Function_result", std::move(g));
    return true;
}

NODE_DECLARATION_UI(test_function_1);
NODE_DECLARATION_UI(test_function_2);
NODE_DECLARATION_UI(test_function_3);
NODE_DECLARATION_UI(test_function_4);
NODE_DECLARATION_UI(test_function_5);
NODE_DECLARATION_UI(test_function_6);
NODE_DECLARATION_UI(test_function_7);
NODE_DECLARATION_UI(test_function_8);
NODE_DECLARATION_UI(test_function_9);
NODE_DECLARATION_UI(test_function_10);
NODE_DECLARATION_UI(test_function_11);
NODE_DEF_CLOSE_SCOPE