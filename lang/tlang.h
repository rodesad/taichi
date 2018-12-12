#include <taichi/common/util.h>
#include <taichi/io/io.h>
#include <dlfcn.h>
#include <set>

TC_NAMESPACE_BEGIN

namespace Tlang {

template <typename T>
using Handle = std::shared_ptr<T>;

class Expr;

struct Address {
  int64 buffer_id;
  int64 coeff_i;
  int64 coeff_imax;
  int64 coeff_const;  // offset

  // AOSOA: i / a * b
  int64 coeff_aosoa_group_size;
  int64 coeff_aosoa_stride;

  TC_IO_DEF(buffer_id,
            coeff_i,
            coeff_imax,
            coeff_const,
            coeff_aosoa_stride,
            coeff_aosoa_group_size);

  Address() {
    buffer_id = -1;
    coeff_i = 0;
    coeff_imax = 0;
    coeff_const = 0;
    coeff_aosoa_group_size = 0;
    coeff_aosoa_stride = 0;
  }

  bool initialized() {
    return buffer_id != -1;
  }

  TC_FORCE_INLINE bool same_type(Address o) {
    return buffer_id == o.buffer_id && coeff_i == o.coeff_i &&
           coeff_imax == o.coeff_imax &&
           coeff_aosoa_group_size == o.coeff_aosoa_group_size &&
           coeff_aosoa_stride == o.coeff_aosoa_stride;
  }

  TC_FORCE_INLINE bool operator==(Address o) {
    return buffer_id == o.buffer_id && coeff_i == o.coeff_i &&
           coeff_imax == o.coeff_imax && coeff_const == o.coeff_const &&
           coeff_aosoa_group_size == o.coeff_aosoa_group_size &&
           coeff_aosoa_stride == o.coeff_aosoa_group_size;
  }

  TC_FORCE_INLINE int64 offset() {
    return coeff_const;
  }

  int64 eval(int64 i, int64 n) {
    TC_ASSERT(initialized());
    if (coeff_aosoa_stride != 0) {
      return coeff_i * i + coeff_imax * n + coeff_const +
             (i / coeff_aosoa_group_size) * coeff_aosoa_stride;
    } else {
      return coeff_i * i + coeff_imax * n + coeff_const;
    }
  }
};

struct AddrNode {
  std::vector<Handle<AddrNode>> ch;
  int depth;
  Address *addr;

  int group_size;
  int repeat_factor;
  int num_variables;
  int offset;
  int buffer_id;
  int coeff_i;
  // repeat included
  int data_size;

  AddrNode(int depth, Address *addr = nullptr) : depth(depth), addr(addr) {
    num_variables = 0;
    if (addr) {
      num_variables += 1;
    }
    offset = 0;
    repeat_factor = 1;
  }

  void materialize() {
    TC_ASSERT(bool(addr == nullptr) ^ bool(ch.size() == 0));
    if (depth == 2) {  // stream, reset offset
      offset = 0;
    }
    int acc_offset = offset;
    for (auto &c : ch) {
      c->offset = acc_offset;
      c->materialize();
      num_variables += c->num_variables;
      acc_offset += c->data_size;
    }
    data_size = num_variables * repeat_factor;
    group_size = (ch.size() ? ch[0]->group_size : 1) * repeat_factor;
  }

  void set() {
    int coeff_imax = 0;
    int buffer_id = 0;
    int bundle_num_variables = -1;
    std::function<void(AddrNode *)> walk = [&](AddrNode *node) {
      if (node->addr) {
        node->addr->buffer_id = buffer_id;
        node->addr->coeff_i = node->coeff_i;
        node->addr->coeff_imax = coeff_imax;
        node->addr->coeff_aosoa_group_size = group_size;
        node->addr->coeff_const = node->offset;
        // Note: use root->data_size here
        node->addr->coeff_aosoa_stride =
            group_size * (bundle_num_variables - node->coeff_i);
      }
      for (auto c : node->ch) {
        if (c->depth == 2) {  // stream
          bundle_num_variables = c->num_variables;
        }
        if (c->depth == 1) {  // buffer
          buffer_id = c->buffer_id;
        }
        c->coeff_i = node->num_variables;
        walk(c.get());
        if (c->depth == 1) {  // buffer
          coeff_imax = 0;
        } else if (c->depth == 2) {        // stream
          coeff_imax += c->num_variables;  // stream attr update
        }
      }
    };

    walk(this);
  }

  AddrNode &group(int id = -1) {
    TC_ASSERT(depth >= 2);
    if (id == -1) {
      auto n = create(depth + 1);
      ch.push_back(n);
      return *n;
    } else {
      while ((int)ch.size() <= id) {
        auto n = create(depth + 1);
        ch.push_back(n);
      }
      return *ch[id];
    }
  }

  AddrNode &stream(int id = -1) {
    TC_ASSERT(depth == 1);
    if (id == -1) {
      auto n = create(depth + 1);
      ch.push_back(n);
      return *n;
    } else {
      while ((int)ch.size() <= id) {
        auto n = create(depth + 1);
        ch.push_back(n);
      }
      return *ch[id];
    }
  }

  AddrNode &repeat(int repeat_factor) {
    this->repeat_factor = repeat_factor;
    return *this;
  }

  AddrNode &place(Expr &expr);

  template <typename... Args>
  AddrNode &place(Expr &expr, Args &&... args) {
    return place(expr).place(std::forward<Args>(args)...);
  }

  template <typename... Args>
  static Handle<AddrNode> create(Args &&... args) {
    return std::make_shared<AddrNode>(std::forward<Args>(args)...);
  }

  void print() {
    for (int i = 0; i < depth; i++) {
      fmt::print("  ");
    }
    fmt::print("  num_variables={} data_size={} repeat={} offset={} addr={}\n",
               num_variables, data_size, repeat_factor, offset, (uint64)addr);
    for (auto c : ch) {
      c->print();
    }
  }
};

struct MemoryAllocator {
  // A tree-like structure that describes the minimal repeating unit in the
  // stream
  Handle<AddrNode> root;

  MemoryAllocator() {
    // streams are specialized groups, with discontinuous parts in memory
    root = AddrNode::create(0);
  }

  AddrNode &buffer(int id) {
    while ((int)root->ch.size() <= id) {
      root->ch.push_back(AddrNode::create(1));
      root->ch.back()->buffer_id = root->ch.size() - 1;
    }
    auto ret = root->ch[id];
    return *ret;
  }

  void materialize() {
    root->materialize();
    root->set();
  }

  void print() {
    root->print();
  }
};

class Visitor {
 public:
  enum class Order { parent_first, child_first };

  Order order;

  Visitor(Order order = Order::parent_first) : order(order) {
  }

  virtual void visit(Expr &expr) = 0;
};

// TODO: do we need polymorphism here?
class Node {
 public:
  enum class Type : int { mul, add, sub, div, load, store, combine, constant };

  Address addr;
  std::vector<Expr> ch;       // Four child max
  std::vector<Expr> members;  // for vectorized instructions
  Type type;
  std::string var_name;
  float64 value;
  bool is_vectorized;

  Node(Type type) : type(type) {
    is_vectorized = false;
  }

  Node(Type type, Expr ch0, Expr ch1);

  int member_id(const Expr &expr) const;
};

using NodeType = Node::Type;

// Reference counted...
class Expr {
 private:
  Handle<Node> node;

 public:
  Expr() {
  }

  Expr(float64 val) {
    // create a constant node
    node = std::make_shared<Node>(NodeType::constant);
    node->value = val;
  }

  Expr(Handle<Node> node) : node(node) {
  }

  template <typename... Args>
  static Expr create(Args &&... args) {
    return Expr(std::make_shared<Node>(std::forward<Args>(args)...));
  }

#define BINARY_OP(op, name)                            \
  Expr operator op(const Expr &o) const {              \
    return Expr::create(NodeType::name, node, o.node); \
  }

  BINARY_OP(*, mul);
  BINARY_OP(+, add);
  BINARY_OP(-, sub);
  BINARY_OP(/, div);
#undef BINARY_OP

  Expr store(const Expr &e) {
    if (!node) {
      node = std::make_shared<Node>(NodeType::combine);
    }
    auto n = std::make_shared<Node>(NodeType::store);
    n->ch.push_back(e.node);
    Expr store_e(n);
    node->ch.push_back(n);
    return store_e;
  }

  Expr store(const Expr &e, Address addr) {
    if (!node) {
      node = std::make_shared<Node>(NodeType::combine);
    }
    auto n = std::make_shared<Node>(NodeType::store);
    n->ch.push_back(e.node);
    n->addr = addr;
    Expr store_e(n);
    node->ch.push_back(n);
    return store_e;
  }

  Node *operator->() {
    return node.get();
  }

  const Node *operator->() const {
    return node.get();
  }

  bool operator<(const Expr &o) const {
    return node.get() < o.node.get();
  }

  operator bool() const {
    return node.get() != nullptr;
  }

  operator void *() const {
    return (void *)node.get();
  }

  bool operator==(const Expr &o) const {
    return (void *)(*this) == (void *)o;
  }

  void accept(Visitor &visitor) {
    if (visitor.order == Visitor::Order::parent_first) {
      visitor.visit(*this);
    }
    for (auto &c : this->node->ch) {
      c.accept(visitor);
    }
    if (visitor.order == Visitor::Order::child_first) {
      visitor.visit(*this);
    }
  }
};

inline bool prior_to(Expr &a, Expr &b) {
  auto address1 = a->addr;
  auto address2 = b->addr;
  return address1.same_type(address2) &&
         address1.offset() + 1 == address2.offset();
}

Node::Node(Type type, Expr ch0, Expr ch1) : Node(type) {
  ch.resize(2);
  ch[0] = ch0;
  ch[1] = ch1;
}

inline Expr placeholder() {
  auto n = std::make_shared<Node>(NodeType::load);
  return Expr(n);
}

inline Expr load(Address addr) {
  auto n = std::make_shared<Node>(NodeType::load);
  TC_ASSERT(addr.initialized());
  n->addr = addr;
  TC_ASSERT(0 <= addr.buffer_id && addr.buffer_id < 3);
  return Expr(n);
}

AddrNode &AddrNode::place(Expr &expr) {
  if (!expr) {
    expr = placeholder();
  }
  TC_ASSERT(depth >= 3);
  TC_ASSERT(this->addr == nullptr);
  ch.push_back(create(depth + 1, &expr->addr));
  return *this;
}

int Node::member_id(const Expr &expr) const {
  for (int i = 0; i < (int)members.size(); i++) {
    if (members[i] == expr) {
      return i;
    }
  }
  return -1;
}

inline int get_code_gen_id() {
  static int id = 0;
  TC_ASSERT(id < 10000);
  return id++;
}

class Vectorizer : public Visitor {
 public:
  std::map<Expr, Expr> scalar_to_vector;
  int simd_width;
  int group_size;
  int num_groups;

  Vectorizer(int simd_width)
      : Visitor(Visitor::Order::parent_first), simd_width(simd_width) {
  }

  Expr run(Expr &expr, int group_size) {
    this->group_size = group_size;
    this->num_groups = simd_width / group_size;
    TC_ASSERT(group_size * num_groups == simd_width);
    scalar_to_vector.clear();
    // expr should be a ret Op, with its children store Ops.
    // The stores are repeated by a factor of 'pack_size'
    TC_ASSERT(expr->ch.size() % group_size == 0);
    TC_ASSERT(expr->type == NodeType::combine);
    // Create the root group
    auto combined = Expr::create(NodeType::combine);
    combined->is_vectorized = true;
    // for each batch (group)
    for (int k = 0; k < (int)expr->ch.size() / group_size; k++) {
      auto root = Expr::create(NodeType::store);
      root->is_vectorized = true;
      bool has_prior_to = false, has_same = false;
      for (int i = 0; i < group_size; i++) {
        auto ch = expr->ch[k * group_size + i];
        TC_ASSERT(ch->type == NodeType::store);
        root->members.push_back(ch);  // put scalar inst into vector members
        TC_ASSERT(i < (int)root->members.size());
        if (i > k * group_size) {
          if (prior_to(root->members[i - 1], root->members[i])) {
            has_prior_to = true;
          } else if (root->members[i - 1]->addr == root->members[i]->addr) {
            has_same = true;
          } else {
            TC_P(root->members[i - 1]->addr);
            TC_P(root->members[i]->addr);
            TC_ERROR(
                "Addresses in SIMD load should be either identical or "
                "neighbouring.");
          }
        }
      }
      TC_ASSERT(!(has_prior_to && has_same));
      // TC_P(root->members.size());
      root.accept(*this);
      combined->ch.push_back(root);
    }
    // TC_P(combined->ch.size());
    return combined;
  }

  void visit(Expr &expr) override {
    // Note: expr may be replaced by an existing vectorized Expr
    if (scalar_to_vector.find(expr->members[0]) != scalar_to_vector.end()) {
      auto existing = scalar_to_vector[expr->members[0]];
      TC_ASSERT(existing->members.size() == expr->members.size());
      for (int i = 0; i < (int)existing->members.size(); i++) {
        TC_ASSERT(existing->members[i] == expr->members[i]);
      }
      expr = existing;
      return;
    }

    expr->is_vectorized = true;
    bool first = true;
    NodeType type;
    std::vector<std::vector<Expr>> vectorized_children;

    // Check for isomorphism
    for (auto member : expr->members) {
      // It must not appear to an existing vectorized expr
      TC_ASSERT(scalar_to_vector.find(member) == scalar_to_vector.end());
      if (first) {
        first = false;
        type = member->type;
        vectorized_children.resize(member->ch.size());
      } else {
        TC_ASSERT(type == member->type);
        TC_ASSERT(vectorized_children.size() == member->ch.size());
      }
      for (int i = 0; i < (int)member->ch.size(); i++) {
        vectorized_children[i].push_back(member->ch[i]);
      }
    }

    expr->is_vectorized = true;
    TC_ASSERT(expr->members.size() % group_size == 0);

    for (int i = 0; i < (int)vectorized_children.size(); i++) {
      // TC_P(i);
      auto ch = Expr::create(vectorized_children[i][0]->type);
      ch->members = vectorized_children[i];
      expr->ch.push_back(ch);
    }

    expr->addr = expr->members[0]->addr;
    if (expr->addr.coeff_aosoa_group_size == 0 ||
        expr->addr.coeff_aosoa_stride == 0) {
      expr->addr.coeff_aosoa_group_size = num_groups;
      expr->addr.coeff_aosoa_stride = 0;
    }
  }
};

class CodeGenBase : public Visitor {
 public:
  int var_count;
  std::string code;
  std::map<NodeType, std::string> binary_ops;
  std::string folder;
  std::string func_name;
  int id;
  MemoryAllocator alloc;
  using FunctionType = void (*)(float32 *, float32 *, float32 *, int);

  CodeGenBase() : Visitor(Visitor::Order::child_first) {
    code = "";
    id = get_code_gen_id();
    func_name = fmt::format("func{:06d}", id);
    binary_ops[NodeType::add] = "+";
    binary_ops[NodeType::sub] = "-";
    binary_ops[NodeType::mul] = "*";
    binary_ops[NodeType::div] = "/";
    folder = "_tlang_cache/";
    create_directories(folder);
  }

  std::string create_variable() {
    TC_ASSERT(var_count < 10000);
    return fmt::format("var_{:04d}", var_count++);
  }

  std::string get_scalar_suffix(int i) {
    return fmt::format("_{:03d}", i);
  }

  std::string get_source_fn() {
    return fmt::format("{}/tmp{:04d}.cpp", folder, id);
  }

  std::string get_library_fn() {
#if defined(TC_PLATFORM_OSX)
    // Note: use .so here will lead to wired behavior...
    return fmt::format("{}/tmp{:04d}.dylib", folder, id);
#else
    return fmt::format("{}/tmp{:04d}.so", folder, id);
#endif
  }

  template <typename... Args>
  void emit_code(std::string f, Args &&... args) {
    if (sizeof...(args)) {
      code += fmt::format(f, std::forward<Args>(args)...);
    } else {
      code += f;
    }
  }

  void write_code_to_file() {
    {
      std::ofstream of(get_source_fn());
      of << code;
    }
    auto format_ret =
        std::system(fmt::format("clang-format -i {}", get_source_fn()).c_str());
    trash(format_ret);
  }

  FunctionType load_function() {
    auto dll = dlopen(("./" + get_library_fn()).c_str(), RTLD_LAZY);
    TC_ASSERT(dll != nullptr);
    auto ret = dlsym(dll, func_name.c_str());
    TC_ASSERT(ret != nullptr);
    return (FunctionType)ret;
  }
};

class CPUCodeGen : public CodeGenBase {
 public:
  int unroll;
  int prefetch;
  enum class Mode : int { scalar, vector };
  Mode mode;
  int simd_width;
  int group_size;
  int num_groups;

 public:
  CPUCodeGen() : CodeGenBase() {
    prefetch = 0;
    unroll = 1;
    var_count = 0;
  }

  void codegen(Expr &vectorized_expr, int group_size = 1) {
    TC_ASSERT(mode == Mode::vector);
    this->group_size = group_size;
    TC_ASSERT(group_size != 0);
    // group_size = expr->ch.size();
    num_groups = simd_width / group_size;
    TC_WARN_IF(simd_width % group_size != 0, "insufficient lane usage");

    emit_code("#include <immintrin.h>\n#include <cstdio>\n");
    emit_code("using float32 = float;\n");
    emit_code("using float64 = double;\n\n");
    emit_code(
        "extern \"C\" void " + func_name +
        "(float32 * __restrict__ buffer00, float32 * __restrict__ buffer01, "
        "float32 * __restrict__ buffer02, int n) {\n");
    emit_code("#define LOOP(loop_index) {\\\n");

    // Body
    vectorized_expr.accept(*this);

    emit_code("}\n");
    emit_code("for (int i = 0, g = 0; i < n; ) {{\n", num_groups);
    for (int i = 0; i < unroll; i++) {
      emit_code("LOOP({});", i);
    }
    emit_code("i += {}; g += {};", num_groups * unroll, unroll);
    emit_code("}\n}\n");
    emit_code("#undef LOOP");
  }

  // Create vectorized IR for the root node
  // the vector width should be the final SIMD instruction width
  std::string get_vectorized_address(Address addr, int extra_offset = 0) {
    TC_ASSERT(addr.buffer_id != -1);
    auto buffer_name = fmt::format("buffer{:02d}", addr.buffer_id);
    auto stride =
        addr.coeff_i * num_groups +
        num_groups / addr.coeff_aosoa_group_size * addr.coeff_aosoa_stride;
    auto offset = addr.coeff_const;
    return fmt::format("&{}[{} * n + {} * (g + loop_index) + {} + {}]",
                       buffer_name, addr.coeff_imax, stride, offset,
                       extra_offset);
  }

  void visit(Expr &expr) override {
    TC_ASSERT(expr->is_vectorized);
    TC_ASSERT(expr->members.size() == 0 ||
              (int)expr->members.size() == group_size);
    // TC_P(expr->ch.size());
    if (expr->var_name == "")
      expr->var_name = create_variable();
    else
      return;  // visited
    if (binary_ops.find(expr->type) != binary_ops.end()) {
      auto op = binary_ops[expr->type];
      if (mode == Mode::vector) {
        emit_code("auto {} = {} {} {}; \\\n", expr->var_name,
                  expr->ch[0]->var_name, op, expr->ch[1]->var_name);
      } else if (mode == Mode::scalar) {
        for (int i = 0; i < simd_width; i++) {
          auto suf = get_scalar_suffix(i);
          emit_code("auto {} = {} {} {}; \\\n", expr->var_name + suf,
                    expr->ch[0]->var_name + suf, op,
                    expr->ch[1]->var_name + suf);
        }
      }
    } else if (expr->type == NodeType::load) {
      auto buffer_name = fmt::format("buffer{:02d}", expr->addr.buffer_id);

      if (mode == Mode::vector) {
        // TC_P(expr->members.size());
        std::vector<int> offsets;
        for (int i = 0; i + 1 < (int)expr->members.size(); i++) {
          TC_ASSERT(
              expr->members[i]->addr.same_type(expr->members[i + 1]->addr));
        }
        for (int i = 0; i < (int)expr->members.size(); i++) {
          offsets.push_back(expr->members[i]->addr.offset());
        }
        auto addr = expr->addr;
        auto i_stride = num_groups;
        // TC_P(i_stride);
        // TC_P(addr.coeff_aosoa_group_size);
        TC_ASSERT(i_stride == addr.coeff_aosoa_group_size);
        // TC_ASSERT(expr->members[0]->addr.coeff_i);
        std::string load_instr =
            simd_width == 8 ? "_mm256_load_ps" : "_mm512_load_ps";
        bool needs_shuffle = false;
        if (addr.coeff_const % simd_width != 0) {
          addr.coeff_const -= addr.coeff_const % simd_width;
          needs_shuffle = true;
        }
        if (prefetch != 0) {
          // https://stackoverflow.com/questions/46521694/what-are-mm-prefetch-locality-hints
          emit_code("if (loop_index == 0) _mm_prefetch({}, _MM_HINT_NTA); \\\n",
                    get_vectorized_address(addr, prefetch));
        }
        emit_code("auto {}_immediate = {}({}); \\\n", expr->var_name,
                  load_instr, get_vectorized_address(addr));
        auto emit_shuffle = [&](std::string imm) {
          emit_code(
              "auto {} = _mm256_shuffle_ps({}_immediate, {}_immediate, "
              "{});\\\n",
              expr->var_name, expr->var_name, expr->var_name, imm);
          needs_shuffle = false;
        };
        if (group_size == 1) {
          emit_code("auto {} = {}_immediate; \\\n", expr->var_name,
                    expr->var_name);
        } else {
          TC_ASSERT(group_size <= 4);
          // detect patterns
          int offset_const = offsets[0] % simd_width;
          int offset_inc = offsets[1] - offsets[0];
          if (group_size == 2) {
            if (offset_const == 0 && offset_inc == 1) {
              emit_code("auto {} = {}_immediate; \\\n", expr->var_name,
                        expr->var_name);
            } else if (offset_inc == 0) {
              if (offset_const == 0) {
                emit_shuffle("0xA0");
              } else if (offset_const == 1) {
                emit_shuffle("0xF5");
              } else {
                TC_NOT_IMPLEMENTED;
              }
            } else {
              TC_P(offset_const);
              TC_P(offset_inc);
              TC_NOT_IMPLEMENTED;
            }
          } else if (group_size == 4) {
            if (offset_const == 0 && offset_inc == 1) {
              emit_code("auto {} = {}_immediate;\\\n", expr->var_name,
                        expr->var_name);
            } else if (offset_inc == 0) {
              if (offset_const == 0) {
                emit_shuffle("0x00");
              } else if (offset_const == 1) {
                emit_shuffle("0x55");
              } else if (offset_const == 2) {
                emit_shuffle("0xAA");
              } else if (offset_const == 3) {
                emit_shuffle("0xFF");
              } else {
                TC_NOT_IMPLEMENTED;
              }
            } else {
              TC_P(offset_const);
              TC_P(offset_inc);
              TC_NOT_IMPLEMENTED;
            }

          } else {
            TC_NOT_IMPLEMENTED
          }
          TC_ASSERT(needs_shuffle == false);
        }

      } else {
        TC_NOT_IMPLEMENTED
        for (int i = 0; i < simd_width; i++) {
          auto suf = get_scalar_suffix(i);
          emit_code("auto {} = {}[{} * i + {} + {}];\\\n", expr->var_name + suf,
                    buffer_name, expr->addr.coeff_i, expr->addr.coeff_const, i);
        }
      }
    } else if (expr->type == NodeType::store) {
      auto buffer_name = fmt::format("buffer{:02d}", expr->addr.buffer_id);
      if (mode == Mode::vector) {
        std::string store_instr =
            // simd_width == 8 ? "_mm256_stream_ps" : "_mm512_stream_ps";
            simd_width == 8 ? "_mm256_store_ps" : "_mm512_store_ps";
        emit_code("{}({}, {}); \\\n", store_instr,
                  get_vectorized_address(expr->addr), expr->ch[0]->var_name);
      } else {
        TC_NOT_IMPLEMENTED
        for (int i = 0; i < simd_width; i++) {
          auto suf = get_scalar_suffix(i);
          emit_code("{}[{} * i + {} + {}] = {}; \\\n", buffer_name,
                    expr->addr.coeff_i, expr->addr.coeff_const, i,
                    expr->ch[0]->var_name + suf);
        }
      }
    } else if (expr->type == NodeType::combine) {
      // do nothing
    } else {
      TC_P((int)expr->type);
      TC_NOT_IMPLEMENTED;
    }
  }

  // group_size should be batch_size here...
  FunctionType compile() {
    write_code_to_file();
    auto cmd = fmt::format(
        "g++ {} -std=c++14 -shared -fPIC -O3 -march=native "
        "-D_GLIBCXX_USE_CXX11_ABI=0 -o {}",
        get_source_fn(), get_library_fn());
    auto compile_ret = std::system(cmd.c_str());
    TC_ASSERT(compile_ret == 0);
#if defined(TC_PLATFORM_LINUX)
    auto objdump_ret = system(
        fmt::format("objdump {} -d > {}.s", get_library_fn(), get_library_fn())
            .c_str());
    trash(objdump_ret);
#endif
    return load_function();
  }

  FunctionType get(Expr &e,
                   int group_size,
                   CPUCodeGen::Mode mode = CPUCodeGen::Mode::vector,
                   int simd_width = 8) {
    this->mode = mode;
    this->simd_width = simd_width;
    alloc.materialize();
    auto vectorized_expr = Vectorizer(simd_width).run(e, group_size);
    codegen(vectorized_expr, group_size);
    return compile();
  }
};

using CodeGen = CPUCodeGen;

}  // namespace Tlang

TC_NAMESPACE_END

/*
 Expr should be what the users play with.
   Simply a ref-counted pointer to nodes, with some operator overloading for
 users to program Node is the IR node, with computational graph connectivity,
 imm, op type etc.

 No double support this time.
 */
