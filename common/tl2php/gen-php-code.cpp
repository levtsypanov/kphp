// Compiler for PHP (aka KPHP)
// Copyright (c) 2020 LLC «V Kontakte»
// Distributed under the GPL v3 License, see LICENSE.notice.txt

#include "common/tl2php/gen-php-code.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <exception>
#include <fstream>
#include <ftw.h>
#include <iterator>
#include <map>
#include <sstream>
#include <sys/stat.h>
#include <tuple>
#include <unordered_set>

#include "common/algorithms/find.h"
#include "common/tl2php/gen-php-common.h"
#include "common/tl2php/gen-php-tests.h"
#include "common/tl2php/php-classes.h"
#include "common/tl2php/tl-hints.h"
#include "common/tlo-parsing/tl-objects.h"
#include "common/wrappers/iterator_range.h"
#include "common/wrappers/mkdir_recursive.h"

namespace vk {
namespace tl {
namespace {

struct DescriptionComment {
  const PhpClassRepresentation &repr;
  const TlHints &hints;

  friend std::ostream &operator<<(std::ostream &os, const DescriptionComment &self) {
    os << "/**" << std::endl
       << " * AUTOGENERATED, DO NOT EDIT! If you want to modify it, check tl schema." << std::endl
       << " *" << std::endl
       << " * This autogenerated code represents tl class for typed RPC API." << std::endl;

    if (const TlHint *hint = self.hints.get_hint_for_combinator(self.repr.tl_name)) {
      os << " *" << std::endl
         << " * " << self.repr.tl_name << "#" << hint->magic;
      for (const auto &arg_str : hint->args) {
        os << std::endl
           << " *   " << arg_str;
      }
      if (!hint->args.empty()) {
        os << std::endl
           << " *  ";
      }
      os << " = " << hint->result << ";" << std::endl;
    }
    return os << " */" << SkipLine{};
  }
};

struct BuiltinOpen {
  const PhpClassRepresentation &repr;

  friend std::ostream &operator<<(std::ostream &os, const BuiltinOpen &self) {
    if (self.repr.is_builtin) {
      os << "#ifndef KPHP" << SkipLine{};
    }
    return os;
  }
};

struct BuiltinClose {
  const PhpClassRepresentation &repr;

  friend std::ostream &operator<<(std::ostream &os, const BuiltinClose &self) {
    if (self.repr.is_builtin) {
      os << std::endl << "#endif" << std::endl;
    }
    return os;
  }
};

struct RpcResponseChecker {
  explicit RpcResponseChecker(const PhpClassRepresentation &repr) {
    is_rpc_response = repr.php_class_name == PhpClasses::rpc_response_type();
    is_rpc_response_ok = repr.php_class_name == PhpClasses::rpc_response_ok();
    is_rpc_response_header = repr.php_class_name == PhpClasses::rpc_response_header();
    is_rpc_response_error = repr.php_class_name == PhpClasses::rpc_response_error();
  }

  bool is_any() const {
    return is_rpc_response || is_rpc_response_ok || is_rpc_response_header || is_rpc_response_error;
  }

  bool is_rpc_response{false};
  bool is_rpc_response_ok{false};
  bool is_rpc_response_header{false};
  bool is_rpc_response_error{false};
};

bool need_use_common_namespace(const PhpClassRepresentation &class_repr) {
  if (class_repr.parent) {
    return true;
  }
  if (class_repr.is_interface && !class_repr.is_builtin) {
    return true;
  }
  for (const auto &field : class_repr.class_fields) {
    if (field.use_other_type) {
      return true;
    }
  }
  return RpcResponseChecker{class_repr}.is_any();
}

struct FileClassHeader {
  const PhpClassRepresentation &repr;

  friend std::ostream &operator<<(std::ostream &os, const FileClassHeader &self) {
    os << "namespace " << PhpClasses::tl_parent_namespace() << "\\" << self.repr.php_class_namespace << ";" << SkipLine{};
    if (need_use_common_namespace(self.repr)) {
      os << "use " << PhpClasses::tl_full_namespace() << ";" << SkipLine{};
    }
    return os;
  }
};

struct FieldDeclaration {
  const PhpVariable &variable;
  bool allow_default;
  bool add_type_hint;

  friend std::ostream &operator<<(std::ostream &os, const FieldDeclaration &self) {
    if (!self.allow_default && self.variable.type == php_field_type::t_class && self.add_type_hint) {
      os << self.variable.php_doc_type << " ";
    }
    os << "$" << self.variable.name;
    if (self.allow_default) {
      os << " = " << DefaultValue{self.variable};
    }
    return os;
  }
};

struct FieldBitMaskName {
  const PhpClassField &field;

  friend std::ostream &operator<<(std::ostream &os, const FieldBitMaskName &self) {
    assert(!self.field.field_mask_name.empty() && self.field.field_mask_bit >= 0);
    os << "BIT_";
    for (char c : self.field.field_name) {
      os << static_cast<char>(toupper(c));
    }
    return os << "_" << self.field.field_mask_bit;    // example: BIT_LEGACY_ID_1
  }
};

struct ClassFieldBitMask {
  const PhpClassField &field;

  friend std::ostream &operator<<(std::ostream &os, const ClassFieldBitMask &self) {
    assert(!self.field.field_mask_name.empty() && self.field.field_mask_bit >= 0);
    return
      os << "  /** Field mask for $" << self.field.field_name << " field" << " */" << std::endl
         << "  const " << FieldBitMaskName{self.field} << " = (1 << " << self.field.field_mask_bit << ");" << SkipLine{};
  }
};

struct PhpDocTypeName {
  const PhpVariable &variable;

  friend std::ostream &operator<<(std::ostream &os, const PhpDocTypeName &self) {
    os << self.variable.php_doc_type;
    if (self.variable.under_field_mask &&
        is_or_null_possible(self.variable.type) &&
        self.variable.type != php_field_type::t_bool_true) {
      os << "|null";
    }
    return os;
  }
};

struct ClassFieldDefinition {
  const PhpVariable variable;

  friend std::ostream &operator<<(std::ostream &os, const ClassFieldDefinition &self) {
    os << "  /** @var " << PhpDocTypeName{self.variable} << " */" << std::endl
       << "  public " << FieldDeclaration{self.variable, true, false} << ";";
    return os << SkipLine{};
  }
};

struct PhpDocParam {
  const PhpVariable &variable;

  friend std::ostream &operator<<(std::ostream &os, const PhpDocParam &self) {
    return os << "   * @param " << PhpDocTypeName{self.variable}
              << " $" << self.variable.name << std::endl;
  }
};

struct FunctionDeclarationPhpdoc {
  const std::vector<PhpVariable> &params;
  vk::string_view return_value;
  bool has_kphp_inline;

  friend std::ostream &operator<<(std::ostream &os, const FunctionDeclarationPhpdoc &self) {
    os << "  /**" << std::endl;
    if (self.has_kphp_inline) {
      os << "   * @kphp-inline" << std::endl;
      if (!self.params.empty() || !self.return_value.empty()) {
        os << "   *" << std::endl;
      }
    }

    for (const auto &param : self.params) {
      os << PhpDocParam{param};
    }
    if (!self.return_value.empty()) {
      os << "   * @return " << self.return_value << std::endl;
    }
    return os << "   */" << std::endl;
  }
};

enum FunctionFlags {
  no_flags = 0,
  with_default_params = 1,
  static_method = 2,
  has_kphp_inline = 4,
  add_type_hint = 8
};

struct FunctionDeclaration {
  vk::string_view name;
  std::vector<PhpVariable> params;
  vk::string_view return_value;
  int flags;

  friend std::ostream &operator<<(std::ostream &os, const FunctionDeclaration &self) {
    const bool allow_default = self.flags & with_default_params;
    const bool kphp_inline = self.flags & has_kphp_inline;
    os << FunctionDeclarationPhpdoc{self.params, self.return_value, kphp_inline};

    const bool param_type_hint = self.flags & add_type_hint;
    os << "  public " << ((self.flags & static_method) ? "static " : "") << "function " << self.name << "(";
    for (auto arg_it = self.params.begin(); arg_it != self.params.end();) {
      os << FieldDeclaration{*arg_it, allow_default, param_type_hint};
      if (++arg_it != self.params.end()) {
        os << ", ";
      }
    }
    return os << ")";
  }
};

struct InitFieldInConstructor {
  const PhpClassField &field;

  friend std::ostream &operator<<(std::ostream &os, const InitFieldInConstructor &self) {
    return os << "    $this->" << self.field.field_name << " = $" << self.field.field_name << ";" << std::endl;
  }
};

struct TypeConstructorsConstant {
  const std::vector<std::unique_ptr<const PhpClassRepresentation>> &constructors;

  friend std::ostream &operator<<(std::ostream &os, const TypeConstructorsConstant &self) {
    if (self.constructors.empty()) {
      return os;
    }

    os << "  /** Allows kphp implicitly load all available constructors */" << std::endl
       << "  const CONSTRUCTORS = [" << std::endl;
    for (auto it = self.constructors.begin(); it != self.constructors.end();) {
      os << "    " << ClassNameWithNamespace{**it} << "::class";
      if (++it != self.constructors.end()) {
        os << ",";
      }
      os << std::endl;
    }
    return os << "  ];" << SkipLine{};
  }
};

struct FunctionResultPrivateStatic {
  const PhpClassRepresentation &function_args;
  const PhpClassRepresentation &function_result;

  friend std::ostream &operator<<(std::ostream &os, const FunctionResultPrivateStatic &self) {
    if (self.function_args.is_builtin) {
      return os;
    }
    return os << "  /** Allows kphp implicitly load function result class */" << std::endl
              << "  private const RESULT = " << ClassNameWithNamespace{self.function_result} << "::class;" << SkipLine{};
  }
};

struct ClassConstructor {
  const PhpClassRepresentation &class_repr;

  friend std::ostream &operator<<(std::ostream &os, const ClassConstructor &self) {
    if (self.class_repr.is_interface) {
      return os;
    }

    auto args_range = vk::make_filter_iterator_range(
      [](const PhpClassField &field) {
        return field.field_mask_name.empty() || field.field_mask_bit < 0;
      },
      self.class_repr.class_fields.begin(),
      self.class_repr.class_fields.end());

    const int ctr_flags = with_default_params | (args_range.empty() ? has_kphp_inline : 0);
    os << FunctionDeclaration{"__construct", {args_range.begin(), args_range.end()}, "", ctr_flags} << " {" << std::endl;
    for (const auto &arg : args_range) {
      os << InitFieldInConstructor{arg};
    }
    return os << "  }" << SkipLine{};
  }
};

struct FunctionReturnValueMethod {
  const PhpClassRepresentation &result_repr;

  friend std::ostream &operator<<(std::ostream &os, const FunctionReturnValueMethod &self) {
    if (self.result_repr.is_interface) {
      return os;
    }
    assert(self.result_repr.class_fields.size() == 1);
    assert(self.result_repr.class_fields.front().field_name == "value");
    const PhpVariable param{"function_return_result", php_field_type::t_class, PhpClasses::rpc_function_return_result_with_tl_namespace()};
    const auto &ret_type = self.result_repr.class_fields.front().php_doc_type;
    return
      os << FunctionDeclaration{"functionReturnValue", {param}, ret_type, static_method} << " {" << std::endl
         << "    if ($" << param.name << " instanceof " << self.result_repr.php_class_name << ") {" << std::endl
         << "      return $" << param.name << "->value;" << std::endl
         << "    }" << std::endl
         << "    warning('Unexpected result type in functionReturnValue: ' . ($" << param.name << " ? get_class($" << param.name << ") : 'null'));" << std::endl
         << "    return (new " << self.result_repr.php_class_name << "())->value;" << std::endl
         << "  }" << SkipLine{};
  }
};

struct FunctionGetTLFunctionName {
  const PhpClassRepresentation &class_repr;

  friend std::ostream &operator<<(std::ostream &os, const FunctionGetTLFunctionName &self) {
    os << FunctionDeclaration{"getTLFunctionName", {}, "string", has_kphp_inline};
    if (self.class_repr.is_interface) {
      return os << ";" << std::endl;
    }

    return os << " {" << std::endl
              << "    return '" << self.class_repr.tl_name << "';" << std::endl
              << "  }" << SkipLine{};
  }
};

struct FunctionCreateRpcServerResponse {
  const TlFunctionPhpRepresentation &class_repr;

  friend std::ostream &operator<<(std::ostream &os, const FunctionCreateRpcServerResponse &self) {
    if (!self.class_repr.is_kphp_rpc_server_function) {
      return os;
    }

    std::stringstream ss;
    ss << ClassNameWithNamespace{*self.class_repr.function_result};
    const std::string function_result_type = ss.str();

    assert(self.class_repr.function_result->class_fields.size() == 1);
    assert(self.class_repr.function_result->class_fields.front().field_name == "value");
    PhpVariable value_param{self.class_repr.function_result->class_fields.front()};
    os << FunctionDeclaration{"createRpcServerResponse", {value_param}, function_result_type, static_method};
    return os << " {" << std::endl
              << "    $response = new " << function_result_type << "();" << std::endl
              << "    $response->value = $value;" << std::endl
              << "    return $response;" << std::endl
              << "  }" << SkipLine{};
  }
};

struct ResultMethod {
  const PhpClassRepresentation &result_repr;

  friend std::ostream &operator<<(std::ostream &os, const ResultMethod &self) {
    if (self.result_repr.is_interface) {
      return os;
    }
    assert(self.result_repr.class_fields.size() == 1);
    assert(self.result_repr.class_fields.front().field_name == "value");
    const PhpVariable param{"response", php_field_type::t_class, PhpClasses::rpc_response_type_with_tl_namespace()};
    const auto &ret_type = self.result_repr.class_fields.front().php_doc_type;
    return os << FunctionDeclaration{"result", {param}, ret_type, static_method | has_kphp_inline | add_type_hint} << " {" << std::endl
              << "    return self::functionReturnValue($" << param.name << "->getResult());" << std::endl
              << "  }" << SkipLine{};
  }
};

struct CalcFieldMaskMethods {
  const PhpClassRepresentation &repr;

  friend std::ostream &operator<<(std::ostream &os, const CalcFieldMaskMethods &self) {
    std::map<vk::string_view, std::vector<std::reference_wrapper<const PhpClassField>>> fields;
    for (const auto &field : self.repr.class_fields) {
      if (!field.field_mask_name.empty()) {
        fields[field.field_mask_name].emplace_back(field);
      }
    }

    if (fields.empty()) {
      return os;
    }

    std::unordered_set<std::string> used_names;
    for (const auto &f : fields) {
      std::map<int, std::vector<std::reference_wrapper<const PhpClassField>>> field_mask_bit_to_class_field;
      // do not use unordered_map, because it may invalidate refs
      std::map<vk::string_view, PhpVariable> indeterminable_fields_map;
      std::vector<PhpVariable> indeterminable_field_flags;
      for (const auto &field : f.second) {
        field_mask_bit_to_class_field[field.get().field_mask_bit].emplace_back(field.get());
        if (vk::any_of_equal(field.get().field_value_type, php_field_type::t_maybe)) {
          PhpVariable has_variable{"has_" + field.get().field_name, php_field_type::t_bool, "bool"};
          indeterminable_fields_map.emplace(field.get().field_name, has_variable);
          indeterminable_field_flags.emplace_back(std::move(has_variable));
        }
      }

      std::string functionName = "calculate";
      std::string functionNameLower = functionName;
      bool nextUpper = true;
      for (char c: f.first) {
        if (nextUpper) {
          c = static_cast<char>(std::toupper(c));
        }
        nextUpper = c == '_';
        if (!nextUpper) {
          functionName.push_back(c);
          functionNameLower.push_back(static_cast<char>(std::tolower(c)));
        }
      }
      if (!used_names.emplace(std::move(functionNameLower)).second) {
        throw std::runtime_error{
          "Error on processing '" + self.repr.php_class_name + "." + f.first + "' : got collision after field mask name transformation"};
      }

      os << FunctionDeclaration{functionName, indeterminable_field_flags, "int", no_flags} << " {" << std::endl
         << "    $mask = 0;" << SkipLine{};

      for (const auto &fields : field_mask_bit_to_class_field) {
        os << "    if (";
        std::stringstream masks;
        if (fields.second.size() > 1) {
          masks << '(';
        }
        for (auto field_it = fields.second.begin(); field_it != fields.second.end();) {
          auto indeterminable_field_it = indeterminable_fields_map.find(field_it->get().field_name);
          if (indeterminable_field_it != indeterminable_fields_map.end()) {
            os << "$" << indeterminable_field_it->second.name;
          } else {
            os << "$this->" << field_it->get().field_name;
            if (vk::none_of_equal(field_it->get().field_value_type, php_field_type::t_class, php_field_type::t_bool_true)) {
              os << " !== null";
            }
          }

          masks << "self::" << FieldBitMaskName{field_it->get()};

          if (++field_it != fields.second.end()) {
            os << " && ";
            masks << " | ";
          }
        }
        if (fields.second.size() > 1) {
          masks << ')';
        }
        os << ") {" << std::endl
           << "      $mask |= " << masks.rdbuf() << ";" << std::endl
           << "    }" << SkipLine{};
      }

      os << "    return $mask;" << std::endl
         << "  }" << SkipLine{};
    }
    return os;
  }
};

struct GetterMethod : protected RpcResponseChecker {
  explicit GetterMethod(const PhpClassRepresentation &r) :
    RpcResponseChecker(r),
    repr(r) {
  }

  const PhpClassRepresentation &repr;

  virtual const char *func_name() const = 0;
  virtual const char *ret_type() const = 0;
  virtual std::string ret_value() const = 0;
  virtual ~GetterMethod() = default;

  friend std::ostream &operator<<(std::ostream &os, const GetterMethod &self) {
    if (!self.is_any()) {
      return os;
    }

    os << FunctionDeclaration{self.func_name(), {}, self.ret_type(), no_flags};
    if (self.is_rpc_response) {
      assert(self.repr.is_interface);
      os << ";";
    } else {
      os << " {" << std::endl
         << "    return " << self.ret_value() << ";" << std::endl
         << "  }";
    }
    return os << SkipLine{};
  }
};

struct GetResultMethod final : GetterMethod {
  using GetterMethod::GetterMethod;

  const char *func_name() const final { return "getResult"; }
  const char *ret_type() const final {
    return is_rpc_response_error ? "null" : PhpClasses::rpc_function_return_result_with_tl_namespace();
  }
  std::string ret_value() const final {
    if (is_rpc_response_error) {
      return "null";
    }
    const PhpClassField *result_field = nullptr;
    for (const auto &field: repr.class_fields) {
      if (field.php_doc_type == PhpClasses::rpc_function_return_result_with_tl_namespace()) {
        assert(!result_field);
        result_field = &field;
      }
    }
    assert(result_field);
    return "$this->" + result_field->field_name;
  }
};

struct GetHeaderMethod final : GetterMethod {
  using GetterMethod::GetterMethod;

  const char *func_name() const final { return "getHeader"; }
  const char *ret_type() const final {
    return (is_rpc_response || is_rpc_response_header) ? PhpClasses::rpc_response_header_with_tl_namespace() : "null";
  }
  std::string ret_value() const final { return is_rpc_response_header ? "$this" : "null"; }
};

struct IsErrorMethod final : GetterMethod {
  using GetterMethod::GetterMethod;

  const char *func_name() const final { return "isError"; }
  const char *ret_type() const final { return "bool"; }
  std::string ret_value() const final { return is_rpc_response_error ? "true" : "false"; }
};

struct GetErrorMethod : GetterMethod {
  using GetterMethod::GetterMethod;

  const char *func_name() const final { return "getError"; }
  const char *ret_type() const final {
    return is_rpc_response || is_rpc_response_error ? PhpClasses::rpc_response_error_with_tl_namespace() : "null";
  }
  std::string ret_value() const final { return is_rpc_response_error ? "$this" : "null"; }
};

template<class ...Members>
struct ClassDefinition {
  const PhpClassRepresentation &class_repr;
  std::tuple<Members...> members;

  template<size_t Index = 0>
  std::enable_if_t<Index != sizeof...(Members)> write_member(std::ostream &os) const {
    os << std::get<Index>(members);
    write_member<Index + 1>(os);
  }

  template<size_t Index = 0>
  std::enable_if_t<Index == sizeof...(Members)> write_member(std::ostream &) const {
  }

  friend std::ostream &operator<<(std::ostream &os, const ClassDefinition<Members...> &self) {
    os << "/**" << std::endl
       << " * @kphp-tl-class" << std::endl
       << " * @kphp-infer" << std::endl
       << " */" << std::endl
       << (self.class_repr.is_interface ? "interface " : "class ") << self.class_repr.php_class_name;
    if (self.class_repr.parent) {
      os << " implements " << ClassNameWithNamespace{*self.class_repr.parent};
    }
    os << " {" << SkipLine{};


    if (self.class_repr.php_class_name == "rpcResponseHeader") {
      os << "  private static $_enable_new_tl_long = true; // toggle for switching to int64_t TL long, will be deleted" << SkipLine{};
    }

    for (const auto &field : self.class_repr.class_fields) {    // биты филд масок
      if (!field.field_mask_name.empty()) {
        os << ClassFieldBitMask{field};
      }
    }
    for (const auto &field : self.class_repr.class_fields) {    // поля
      os << ClassFieldDefinition{field};
    }

    self.write_member(os);

    return os << "}" << std::endl;
  }
};

template<class ...Members>
ClassDefinition<Members...> make_class(const PhpClassRepresentation &class_repr, Members &&...members) {
  return ClassDefinition<Members...>{class_repr, std::make_tuple(std::forward<Members>(members)...)};
}

std::string prepare_class_dir(const std::string &root_dir,
                              const PhpClassRepresentation &class_repr,
                              const char *class_type) {
  std::string dir = root_dir + '/' + class_repr.php_class_namespace;
  std::replace(dir.begin(), dir.end(), '\\', '/');
  if (!mkdir_recursive(dir.c_str(), 0777)) {
    throw std::runtime_error{"Can't create dir '" + dir + "' for RPC " + class_type + " PHP classes: " + strerror(errno)};
  }
  return dir;
}

size_t gen_rpc_function_classes(const std::string &out_dir, const PhpClasses &classes, const TlHints &hints) {
  for (const auto &php_repr: classes.functions) {
    const auto &repr = php_repr.second;
    assert(repr.function_args->php_class_namespace == php_repr.second.function_result->php_class_namespace);

    if (!is_php_code_gen_allowed(repr)) {
      continue;
    }

    auto dir = prepare_class_dir(out_dir, *repr.function_args, "functions");
    assert(repr.function_args->is_builtin == repr.function_result->is_builtin);

    std::ofstream{dir + "/" + repr.function_args->php_class_name + ".php"}
      << PhpTag{}
      << DescriptionComment{*repr.function_args, hints}
      << BuiltinOpen{*repr.function_args}
      << FileClassHeader{*repr.function_args}
      << make_class(*repr.function_args,
                    FunctionResultPrivateStatic{*repr.function_args, *repr.function_result},
                    ClassConstructor{*repr.function_args},
                    CalcFieldMaskMethods{*repr.function_args},
                    FunctionReturnValueMethod{*repr.function_result},
                    ResultMethod{*repr.function_result},
                    FunctionCreateRpcServerResponse{repr},
                    FunctionGetTLFunctionName{*repr.function_args})
      << std::endl
      << make_class(*repr.function_result)
      << BuiltinClose{*repr.function_args};
  }
  return classes.functions.size() * 2;
}

void gen_rpc_type_class(const std::string &dir, const TlHints &hints, const PhpClassRepresentation &class_repr,
                        const std::vector<std::unique_ptr<const PhpClassRepresentation>> &type_constructors = {}) {
  assert(!class_repr.is_interface || class_repr.class_fields.empty());
  std::ofstream{dir + "/" + class_repr.php_class_name + ".php"}
    << PhpTag{}
    << DescriptionComment{class_repr, hints}
    << BuiltinOpen{class_repr}
    << FileClassHeader{class_repr}
    << make_class(class_repr,
                  TypeConstructorsConstant{type_constructors},
                  ClassConstructor{class_repr},
                  GetResultMethod{class_repr},
                  GetHeaderMethod{class_repr},
                  IsErrorMethod{class_repr},
                  GetErrorMethod{class_repr},
                  CalcFieldMaskMethods{class_repr})
    << BuiltinClose{class_repr};
}

size_t gen_rpc_type_classes(const std::string &out_dir, const PhpClasses &classes, const TlHints &hints) {
  size_t classes_generated = 0;
  for (const auto &php_repr: classes.types) {
    const auto &type_repr = *php_repr.second.type_representation;
    assert(!type_repr.parent);

    if (type_repr.is_interface) {
      assert(type_repr.class_fields.empty());
      assert(!php_repr.second.constructors.empty());
    } else {
      assert(php_repr.second.constructors.empty());
    }

    if (!is_php_code_gen_allowed(php_repr.second)) {
      classes_generated += php_repr.second.constructors.size() + 1;
      continue;
    }

    std::string dir = prepare_class_dir(out_dir, type_repr, "types");
    gen_rpc_type_class(dir, hints, type_repr, php_repr.second.constructors);
    ++classes_generated;

    for (const auto &php_repr_ctr : php_repr.second.constructors) {
      assert(!php_repr_ctr->is_interface);

      dir = prepare_class_dir(out_dir, *php_repr_ctr, "types");
      gen_rpc_type_class(dir, hints, *php_repr_ctr);
      ++classes_generated;
    }
  }
  return classes_generated;
}

void create_out_dir(const std::string &dir, bool forcibly_overwrite_dir) {
  if (!mkdir(dir.c_str(), 0777)) {
    return;
  }
  if (errno == EEXIST && forcibly_overwrite_dir) {
    auto unlink_cb = [](const char *fpath, const struct stat *, int, struct FTW *) {
      return remove(fpath);
    };
    if (nftw(dir.c_str(), unlink_cb, 64, FTW_DEPTH | FTW_PHYS) == -1) {
      throw std::runtime_error{"Can't remove root dir '" + dir + "' for PHP classes: " + strerror(errno)};
    }
    create_out_dir(dir, false);
    return;
  }
  throw std::runtime_error{"Can't create root dir '" + dir + "' for PHP classes: " + strerror(errno)};
}

} // namespace

size_t gen_php_code(const tl_scheme &scheme,
                    const TlHints &hints,
                    const std::string &out_php_dir,
                    bool forcibly_overwrite_dir,
                    bool generate_tests,
                    bool generate_tl_internals) {
  struct stat out_php_dir_stat;
  if (forcibly_overwrite_dir || stat(out_php_dir.c_str(), &out_php_dir_stat) || !S_ISDIR(out_php_dir_stat.st_mode)) {
    create_out_dir(out_php_dir, forcibly_overwrite_dir);
  }
  std::string root_tl_out_dir = out_php_dir + "/" + PhpClasses::tl_parent_namespace();
  create_out_dir(root_tl_out_dir, forcibly_overwrite_dir);

  PhpClasses php_classes;
  php_classes.load_from(scheme, generate_tl_internals);

  const std::size_t functions_generated = gen_rpc_function_classes(root_tl_out_dir, php_classes, hints);
  const std::size_t types_generated = gen_rpc_type_classes(root_tl_out_dir, php_classes, hints);
  const size_t total_classes = functions_generated + types_generated;
  assert(total_classes == php_classes.all_classes.size());

  if (generate_tests) {
    std::string tests_out_dir = out_php_dir + "/";
    gen_php_tests(tests_out_dir, php_classes);
  }

  return total_classes;
}

} // namespace tl
} // namespace vk
