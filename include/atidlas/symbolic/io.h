#ifndef ATIDLAS_SCHEDULER_IO_H
#define ATIDLAS_SCHEDULER_IO_H

#include <iostream>
#include "atidlas/symbolic/expression.h"

namespace atidlas
{

std::string to_string(array_expression_node_subtype const & f);
std::string to_string(lhs_rhs_element const & e);
std::ostream & operator<<(std::ostream & os, array_expression::node const & s_node);
std::string to_string(atidlas::array_expression const & s);

}

#endif

