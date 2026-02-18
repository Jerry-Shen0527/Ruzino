#pragma once
#include <exprtk/exprtk.hpp>
#include <memory>
#include <string>
#include <vector>

#include "api.h"
#include "parameter_map.hpp"
#include "pxr/base/gf/vec2d.h"
#include "pxr/base/gf/vec3d.h"

namespace Ruzino {
namespace fem_bem {
    using real = float;

    // Forward declarations
    class DerivativeExpression;

    class RZFEMBEM_API Expression {
       public:
        using expression_type = exprtk::expression<real>;
        using symbol_table_type = exprtk::symbol_table<real>;
        using parser_type = exprtk::parser<real>;

        // Constructors
        Expression() = default;
        explicit Expression(const std::string& expr_str);

        // Compound expression constructor
        Expression(
            const Expression& outer_expr,
            const ParameterMap<Expression>& variable_substitutions);

        Expression(
            const Expression& outer_expr,
            std::initializer_list<std::pair<const char*, Expression>>
                substitutions);
        Expression(const Expression& other);

        Expression& operator=(const Expression& other);

        // Move constructor and assignment
        Expression(Expression&& other) noexcept = default;
        Expression& operator=(Expression&& other) noexcept = default;

        // Virtual destructor for inheritance
        virtual ~Expression() = default;

        // Factory methods
        static Expression from_string(const std::string& expr_str);
        static Expression constant(real value);
        static Expression zero();
        static Expression one();

        // Basic properties
        const std::string& get_string() const;

        // Method to check if this is a string-based expression
        virtual bool is_string_based() const;

        real evaluate_at(const ParameterMap<real>& variable_values) const;

        // Derivative methods
        DerivativeExpression derivative(const std::string& variable_name) const;

        std::vector<DerivativeExpression> gradient(
            const std::vector<const char*>& variable_names) const;

        // Closure methods - bind specific variables to values
        void bind_variables(const ParameterMap<real>& bound_values);
        void bind_variable(const char* var_name, real value) const;
        void set_variable(const char* var_name, real value) const;

        // Check if expression has bound variables (is a closure)
        bool has_bound_variables() const;

        const ParameterMap<real>& get_bound_variables() const;

        // Access to underlying exprtk objects (for advanced use)
        const expression_type* get_compiled_expression() const;

        const symbol_table_type* get_symbol_table() const;

        // Arithmetic operations
        Expression operator+(const Expression& other) const;
        Expression operator-(const Expression& other) const;
        Expression operator*(const Expression& other) const;
        Expression operator/(const Expression& other) const;
        Expression operator*(real scalar) const;
        Expression operator-() const;

       protected:
        // Protected members for derived classes to access
        std::string expression_string_;

        mutable bool has_bound_variable_ = false;

        // Parsed expression components
        mutable std::unique_ptr<symbol_table_type> symbol_table_;
        mutable std::unique_ptr<expression_type> compiled_expression_;
        mutable bool is_parsed_ = false;

        // Unified variable storage - serves both as temp storage for exprtk and
        // bound variables
        mutable ParameterMap<real> variables_;

        // Compound expression support
        bool is_compound_ = false;
        std::unique_ptr<Expression> outer_expression_;
        // std::vector<std::pair<const char*, Expression>> substitution_map_;
        mutable std::unique_ptr<ParameterMap<Expression>> substitution_map_;

        // Support for DerivativeExpression conversion
        std::function<real(const ParameterMap<real>&)> derivative_evaluator_;
        bool has_derivative_evaluator_ = false;

        void ensure_parsed() const;

       private:
        void parse_expression() const;

        template<typename MappingFunc>
        friend real integrate_over_simplex(
            const Expression& expr,
            const std::vector<const char*>& barycentric_names,
            const MappingFunc& mapping,
            std::size_t intervals);

        template<typename MappingFunc>
        friend real integrate_simplex_with_mapping(
            const Expression& expr,
            MappingFunc mapping,
            const std::vector<const char*>& barycentric_names,
            std::size_t intervals);
    };

    RZFEMBEM_API Expression operator*(real scalar, const Expression& expr);
    RZFEMBEM_API Expression make_expression(const std::string& expr_str);

    // Numerical derivative class that inherits from Expression
    class DerivativeExpression : public Expression {
       private:
        std::string variable_name_;

       public:
        DerivativeExpression(
            std::function<real(const ParameterMap<real>&)> func,
            const std::string& var_name)
            : Expression(),
              variable_name_(var_name)
        {
            // Set up the base class with the derivative evaluator
            this->derivative_evaluator_ = std::move(func);
            this->has_derivative_evaluator_ = true;
            this->expression_string_ =
                "";  // Derivatives don't have string representation
            this->is_compound_ = false;
        }

        // Get the variable name for this derivative
        const std::string& get_variable_name() const
        {
            return variable_name_;
        }

        // Override is_string_based to return false for derivatives
        bool is_string_based() const override
        {
            return false;
        }

        // Note: Integration methods are inherited from Expression base class
    };

    // Direct numerical integration for expressions
    inline real integrate_expression_numerically(
        const Expression& expr,
        const std::vector<const char*>& barycentric_names,
        std::size_t intervals)
    {
        if (barycentric_names.empty())
            return real(0);

        const std::size_t dim = barycentric_names.size();
        real total_integral = real(0);

        // Create a ParameterMap for evaluation - reuse the same object
        ParameterMap<real> values;

        // For 1D case (line segment)
        if (dim == 1) {
            const real h = real(1) / intervals;

            for (std::size_t i = 0; i < intervals; ++i) {
                const real u1 = i * h;
                const real u2 = (i + 1) * h;
                const real u_mid = (u1 + u2) / real(2);

                // Simpson's rule over [u1, u2]
                values.clear();
                values.insert_unchecked(barycentric_names[0], u1);
                const real y1 = expr.evaluate_at(values);

                values.clear();
                values.insert_unchecked(barycentric_names[0], u_mid);
                const real y_mid = expr.evaluate_at(values);

                values.clear();
                values.insert_unchecked(barycentric_names[0], u2);
                const real y2 = expr.evaluate_at(values);

                total_integral += h * (y1 + real(4) * y_mid + y2) / real(6);
            }
        }
        // For 2D case (triangle)
        else if (dim == 2) {
            const real h = real(1) / intervals;

            for (std::size_t i = 0; i <= intervals; ++i) {
                for (std::size_t j = 0; j <= intervals - i; ++j) {
                    const real u1 = i * h;
                    const real u2 = j * h;

                    if (u1 + u2 <= real(1)) {
                        real weight = real(1);
                        // Corner and edge corrections for trapezoidal rule
                        int boundary_count = 0;
                        if (i == 0)
                            boundary_count++;
                        if (j == 0)
                            boundary_count++;
                        if (i + j == intervals)
                            boundary_count++;

                        if (boundary_count == 1)
                            weight = real(0.5);
                        else if (boundary_count >= 2)
                            weight = real(1.0f / 6);

                        values.clear();
                        values.insert_unchecked(barycentric_names[0], u1);
                        values.insert_unchecked(barycentric_names[1], u2);

                        total_integral += weight * expr.evaluate_at(values) *
                                          h * h * real(2.0);
                    }
                }
            }
        }
        // For 3D case (tetrahedron)
        else if (dim == 3) {
            const real h = real(1) / intervals;

            for (std::size_t i = 0; i <= intervals; ++i) {
                for (std::size_t j = 0; j <= intervals - i; ++j) {
                    for (std::size_t k = 0; k <= intervals - i - j; ++k) {
                        const real u1 = i * h;
                        const real u2 = j * h;
                        const real u3 = k * h;

                        if (u1 + u2 + u3 <= real(1)) {
                            real weight = real(1);
                            // Boundary correction
                            int boundary_count = 0;
                            if (i == 0)
                                boundary_count++;
                            if (j == 0)
                                boundary_count++;
                            if (k == 0)
                                boundary_count++;
                            if (i + j + k == intervals)
                                boundary_count++;

                            if (boundary_count > 0)
                                weight = real(1) / real(1 << boundary_count);

                            values.clear();
                            values.insert_unchecked(barycentric_names[0], u1);
                            values.insert_unchecked(barycentric_names[1], u2);
                            values.insert_unchecked(barycentric_names[2], u3);

                            total_integral += weight *
                                              expr.evaluate_at(values) * h * h *
                                              h * real(6);
                        }
                    }
                }
            }
        }

        return total_integral;
    }
    // Integration methods
    template<typename MappingExpr>
    real integrate_over_simplex(
        const Expression& expr,
        const std::vector<const char*>& barycentric_names,
        const MappingExpr& mapping_expr,
        std::size_t intervals)
    {
        // If mapping is provided, compose it with the expression
        if constexpr (!std::is_same_v<MappingExpr, std::nullptr_t>) {
            Expression final_expr = expr;
            final_expr =
                compose_with_mapping(expr, mapping_expr, barycentric_names);
            return integrate_expression_numerically(
                final_expr, barycentric_names, intervals);
        }
        for (const auto& barycentric_name : barycentric_names) {
            expr.set_variable(barycentric_name, real(0));
        }
        return integrate_expression_numerically(
            expr, barycentric_names, intervals);
    }

    // Helper function to compose expression with mapping
    template<typename MappingExpr>
    Expression compose_with_mapping(
        const Expression& expr,
        const MappingExpr& mapping_expr,
        const std::vector<const char*>& barycentric_names)
    {
        if constexpr (std::is_same_v<MappingExpr, std::nullptr_t>) {
            return expr;
        }
        else if constexpr (std::is_same_v<
                               MappingExpr,
                               ParameterMap<Expression>>) {
            // Handle Expression-based mapping
            return create_mapped_expression_with_coord_mapping(
                expr, mapping_expr, barycentric_names);
        }
        else {
            throw std::runtime_error(
                "Mapping has to be either nullptr or ParameterMap<Expression>");
        }
    }

    // Create coordinate mapping expressions that bind world vertex coordinates
    RZFEMBEM_API ParameterMap<Expression> create_coordinate_mapping(
        const std::vector<const char*>& barycentric_names,
        const std::vector<pxr::GfVec2d>& world_vertices);

    RZFEMBEM_API ParameterMap<Expression> create_coordinate_mapping(
        const std::vector<const char*>& barycentric_names,
        const std::vector<pxr::GfVec3d>& world_vertices);

    // Helper to create mapped expression using coordinate mapping
    RZFEMBEM_API Expression create_mapped_expression_with_coord_mapping(
        const Expression& expr,
        const ParameterMap<Expression>& coord_mapping,
        const std::vector<const char*>& barycentric_names);

    // Create mapping expression from barycentric to physical coordinates
    // (legacy)
    Expression create_mapping_expression(
        const std::vector<const char*>& barycentric_names,
        const std::vector<pxr::GfVec2d>& world_vertices);

    Expression create_mapping_expression(
        const std::vector<const char*>& barycentric_names,
        const std::vector<pxr::GfVec3d>& world_vertices);

}  // namespace fem_bem
}  // namespace Ruzino
