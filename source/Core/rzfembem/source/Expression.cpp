#include <cstring>
#include <fem_bem/Expression.hpp>

namespace Ruzino {
namespace fem_bem {
    static exprtk::parser<real> parser;

    // Create a derivative function for compound expressions using chain rule
    std::function<real(const ParameterMap<real>&)>
    create_compound_derivative_function(
        const std::function<real(const ParameterMap<real>&)>&
            compound_evaluator,
        const std::string& variable_name,
        const real& h)
    {
        return [compound_evaluator, variable_name, h](
                   const ParameterMap<real>& values) -> real {
            ParameterMap<real> values_ = values;
            real* var_value = values_.find(variable_name.c_str());
            if (!var_value) {
                return real(0);  // Variable not found
            }

            const real x_init = *var_value;

            // Create modified value maps for derivative computation
            // ParameterMap<real> values_ = values;

            // Use 2-point central difference
            *var_value = x_init + h;
            const real y_plus = compound_evaluator(values_);

            *var_value = x_init - h;
            const real y_minus = compound_evaluator(values_);

            real derivative = (y_plus - y_minus) / (real(2) * h);

            // Check for numerical issues and use adaptive step if needed
            if (std::abs(derivative) > real(1e6)) {
                real smaller_h = h * real(0.1);
                *var_value = x_init + smaller_h;
                const real y_plus_small = compound_evaluator(values_);

                *var_value = x_init - smaller_h;
                const real y_minus_small = compound_evaluator(values_);

                derivative =
                    (y_plus_small - y_minus_small) / (real(2) * smaller_h);
            }

            return derivative;
        };
    }

    Expression::Expression(const std::string& expr_str)
        : expression_string_(expr_str),
          is_compound_(false)
    {
    }

    Expression::Expression(
        const Expression& outer_expr,
        const ParameterMap<Expression>& variable_substitutions)
        : outer_expression_(std::make_unique<Expression>(outer_expr)),
          substitution_map_(
              std::make_unique<ParameterMap<Expression>>(
                  variable_substitutions)),
          is_compound_(true)
    {
        // Build compound expression string for display
        expression_string_ = outer_expr.get_string() +
                             " with "
                             "substitutions: ";
        for (int i = 0; i < substitution_map_->size(); ++i) {
            auto name = substitution_map_->get_name_at(i);
            auto expr = substitution_map_->get_value_at(i).get_string();
            expression_string_ += std::string(name) + " = " + expr + ", ";
        }
    }
    Expression::Expression(
        const Expression& outer_expr,
        std::initializer_list<std::pair<const char*, Expression>> substitutions)
        : outer_expression_(std::make_unique<Expression>(outer_expr)),
          substitution_map_(
              std::make_unique<ParameterMap<Expression>>(substitutions)),
          is_compound_(true)
    {
        // Build compound expression string for display
        expression_string_ = outer_expr.get_string() +
                             " with "
                             "substitutions: ";
        for (int i = 0; i < substitution_map_->size(); ++i) {
            auto name = substitution_map_->get_name_at(i);
            auto expr = substitution_map_->get_value_at(i).get_string();

            expression_string_ += std::string(name) + " = " + expr + ", ";
        }
    }

    Expression::Expression(const Expression& other)
        : expression_string_(other.expression_string_),
          is_parsed_(false),  // Force re-parsing with new symbol table
          is_compound_(other.is_compound_),
          outer_expression_(
              other.outer_expression_
                  ? std::make_unique<Expression>(*other.outer_expression_)
                  : nullptr),
          substitution_map_(
              other.substitution_map_
                  ? std::make_unique<ParameterMap<Expression>>(
                        *other.substitution_map_)
                  : nullptr),
          derivative_evaluator_(other.derivative_evaluator_),
          has_derivative_evaluator_(other.has_derivative_evaluator_),
          has_bound_variable_(other.has_bound_variable_)
    {
        if (has_bound_variable_) {
            variables_ = other.variables_;
        }
    }
    Expression& Expression::operator=(const Expression& other)
    {
        if (this != &other) {
            expression_string_ = other.expression_string_;
            is_parsed_ = false;  // Force re-parsing
            compiled_expression_.reset();
            symbol_table_.reset();
            is_compound_ = other.is_compound_;
            outer_expression_ =
                other.outer_expression_
                    ? std::make_unique<Expression>(*other.outer_expression_)
                    : nullptr;
            substitution_map_ =
                other.substitution_map_
                    ? std::make_unique<ParameterMap<Expression>>(
                          *other.substitution_map_)
                    : nullptr;
            derivative_evaluator_ = other.derivative_evaluator_;
            has_derivative_evaluator_ = other.has_derivative_evaluator_;
            has_bound_variable_ = other.has_bound_variable_;
            if (has_bound_variable_)
                variables_ = other.variables_;
        }
        return *this;
    }
    Expression Expression::from_string(const std::string& expr_str)
    {
        return Expression(expr_str);
    }
    Expression Expression::constant(real value)
    {
        return Expression(std::to_string(value));
    }
    Expression Expression::zero()
    {
        return Expression("0");
    }
    Expression Expression::one()
    {
        return Expression("1");
    }
    const std::string& Expression::get_string() const
    {
        return expression_string_;
    }
    bool Expression::is_string_based() const
    {
        return true;  // Regular expressions are always string-based
    }
    void Expression::bind_variables(const ParameterMap<real>& bound_values)
    {
        // Merge bound values with existing ones
        for (std::size_t i = 0; i < bound_values.size(); ++i) {
            const char* name = bound_values.get_name_at(i);
            const real& value = bound_values.get_value_at(i);
            bind_variable(name, value);
        }

        has_bound_variable_ = true;
    }

    void Expression::bind_variable(const char* var_name, real value) const
    {
        if (is_compound_) {
            for (int i = 0; i < substitution_map_->size(); ++i) {
                substitution_map_->get_value_at(i).bind_variable(
                    var_name, value);
            }
        }
        else {
            variables_.insert_or_assign(var_name, value);
        }

        has_bound_variable_ = true;
    }
    void Expression::set_variable(const char* var_name, real value) const
    {
        if (is_compound_) {
            for (int i = 0; i < substitution_map_->size(); ++i) {
                substitution_map_->get_value_at(i).set_variable(
                    var_name, value);
            }
        }
        else {
            variables_.insert_or_assign(var_name, value);
        }
    }
    bool Expression::has_bound_variables() const
    {
        return has_bound_variable_;
    }
    const ParameterMap<real>& Expression::get_bound_variables() const
    {
        return variables_;
    }
    const Expression::expression_type* Expression::get_compiled_expression()
        const
    {
        ensure_parsed();
        return compiled_expression_.get();
    }
    const Expression::symbol_table_type* Expression::get_symbol_table() const
    {
        ensure_parsed();
        return symbol_table_.get();
    }

    real Expression::evaluate_at(
        const ParameterMap<real>& variable_values) const
    {
        ++g_evaluate_calls;

        // Handle compound expressions
        if (is_compound_ && outer_expression_) {
            // Merge bound variables with provided values for substitution
            // evaluation

            for (std::size_t i = 0; i < variable_values.size(); ++i) {
                const char* name = variable_values.get_name_at(i);

                if (!substitution_map_->contains(name)) {
                    const real& value = variable_values.get_value_at(i);
                    outer_expression_->bind_variable(name, value);
                }
            }

            // Evaluate substitutions
            ParameterMap<real> outer_values = variables_;
            for (std::size_t i = 0; i < substitution_map_->size(); ++i) {
                const auto& name = substitution_map_->get_name_at(i);
                real sub_result =
                    substitution_map_->get_value_at(i).evaluate_at(
                        variable_values);
                outer_values.insert_or_assign(name, sub_result);
            }

            return outer_expression_->evaluate_at(outer_values);
        }

        if (!has_bound_variable_) {
            variables_ = variable_values;
        }
        else
            for (std::size_t i = 0; i < variable_values.size(); ++i) {
                const char* name = variable_values.get_name_at(i);
                const real& value = variable_values.get_value_at(i);
                variables_.insert_or_assign(name, value);
            }

        // Handle expressions created from DerivativeExpression
        if (has_derivative_evaluator_) {
            return derivative_evaluator_(variables_);
        }

        // Standard evaluation for non-compound expressions
        if (!is_parsed_ || !compiled_expression_) {
            parse_expression();
        }

        if (!compiled_expression_) {
            throw std::runtime_error(
                "Expression not properly parsed: " + expression_string_);
        }

        // Merge bound variables with provided values (provided values take
        // precedence)

        // ParameterMap<real> merged_values = variables_;

        // for (std::size_t i = 0; i < variable_values.size(); ++i) {
        //     const char* name = variable_values.get_name_at(i);
        //     const real& value = variable_values.get_value_at(i);
        //     merged_values.insert_or_assign(name, value);
        // }

        // Set all variables in variables_ for exprtk
        // for (std::size_t i = 0; i < merged_values.size(); ++i) {
        //    const char* name = merged_values.get_name_at(i);
        //    const real& value = merged_values.get_value_at(i);
        //    auto ptr = variables_.find(name);
        //    if (ptr)
        //        *ptr = value;
        //}

        real result = compiled_expression_->value();
        return result;
    }

    Expression Expression::operator+(const Expression& other) const
    {
        Expression add_expr("xx_ + yy_");
        return Expression(add_expr, { { "xx_", *this }, { "yy_", other } });
    }

    Expression Expression::operator-(const Expression& other) const
    {
        Expression sub_expr("xx_ - yy_");
        return Expression(sub_expr, { { "xx_", *this }, { "yy_", other } });
    }

    Expression Expression::operator*(const Expression& other) const
    {
        Expression mul_expr("xx_ * yy_");
        return Expression(mul_expr, { { "xx_", *this }, { "yy_", other } });
    }

    Expression Expression::operator/(const Expression& other) const
    {
        Expression div_expr("xx_ / yy_");
        return Expression(div_expr, { { "xx_", *this }, { "yy_", other } });
    }

    Expression Expression::operator*(real scalar) const
    {
        Expression scalar_expr(std::to_string(scalar));
        Expression mul_expr("xx_ * yy_");
        return Expression(
            mul_expr, { { "xx_", scalar_expr }, { "yy_", *this } });
    }

    Expression Expression::operator-() const
    {
        Expression neg_expr("-xx_");
        return Expression(neg_expr, { { "xx_", *this } });
    }

    DerivativeExpression Expression::derivative(
        const std::string& variable_name) const
    {
        // Use more conservative step sizes for float precision
        real h = real(1e-2);
        // if (is_compound_ && outer_expression_) {
        //     // Check if any of the substitutions are derivatives
        //     bool has_derivative_substitution = false;
        //     for (int i = 0; i < substitution_map_->size(); ++i) {
        //         const auto& pair = substitution_map_->get_value_at(i);
        //         if (pair.has_derivative_evaluator_) {
        //             has_derivative_substitution = true;
        //             break;
        //         }
        //     }

        //    // For compound expressions with derivatives, use significantly
        //    // larger step
        //    h = has_derivative_substitution ? real(5e-3) : real(1e-4);
        //}
        // else if (has_derivative_evaluator_) {
        //    // This is already a derivative, so we're computing second
        //    // derivative
        //    h = real(5e-3);
        //}
        // else {
        //    // Simple expression
        //    h = real(1e-4);
        //}

        // For compound expressions, use numerical chain rule
        if (is_compound_ && outer_expression_) {
            auto compound_evaluator = [this](const ParameterMap<real>& values) {
                return this->evaluate_at(values);
            };
            auto derivative_func = create_compound_derivative_function(
                compound_evaluator, variable_name, h);
            return DerivativeExpression(derivative_func, variable_name);
        }
        // Handle derivative expressions (derivatives of derivatives)
        else if (has_derivative_evaluator_) {
            auto derivative_func = create_compound_derivative_function(
                derivative_evaluator_, variable_name, h);
            return DerivativeExpression(derivative_func, variable_name);
        }
        else {
            auto evaluator = [this](const ParameterMap<real>& values) {
                return this->evaluate_at(values);
            };
            auto derivative_func = create_compound_derivative_function(
                evaluator, variable_name, h);
            return DerivativeExpression(derivative_func, variable_name);
        }
    }
    void Expression::ensure_parsed() const
    {
        if (!is_parsed_ || !compiled_expression_) {
            parse_expression();
        }
    }
    void Expression::parse_expression() const
    {
        symbol_table_ = std::make_unique<symbol_table_type>();
        compiled_expression_ = std::make_unique<expression_type>();

        // Enable constants and standard functions
        symbol_table_->add_constants();

        // Register specified variables only
        if (!variables_.empty()) {
            for (int i = 0; i < variables_.size(); i++) {
                auto var_name = variables_.get_name_at(i);
                auto* var_ptr = &variables_.get_value_at(i);

                symbol_table_->add_variable(
                    var_name, const_cast<real&>(*var_ptr));
            }
        }

        // Register symbol table with expression
        compiled_expression_->register_symbol_table(*symbol_table_);

        if (!parser.compile(expression_string_, *compiled_expression_)) {
            throw std::runtime_error(
                "Failed to parse expression: " + expression_string_);
        }

        is_parsed_ = true;
    }
    Expression operator*(real scalar, const Expression& expr)
    {
        return expr * scalar;
    }
    Expression make_expression(const std::string& expr_str)
    {
        return Expression::from_string(expr_str);
    }

    std::vector<DerivativeExpression> Expression::gradient(
        const std::vector<const char*>& variable_names) const
    {
        std::vector<DerivativeExpression> grad;
        grad.reserve(variable_names.size());
        for (const auto& var : variable_names) {
            grad.push_back(derivative(var));
        }
        return grad;
    }

}  // namespace fem_bem
}  // namespace Ruzino
