#include "tlang.h"

TC_NAMESPACE_BEGIN
namespace Tlang {

Expr &Expr::operator=(const Expr &o) {
  if (!node) {
    // Expr assignment
    node = o.node;
  } else {
    // store
    TC_ASSERT(node->type == NodeType::addr);
    auto &prog = get_current_program();
    TC_ASSERT(&prog != nullptr);
    TC_ASSERT(node->get_address().initialized());
    prog.store(o);
  }
  return *this;
}
}
TC_NAMESPACE_END
