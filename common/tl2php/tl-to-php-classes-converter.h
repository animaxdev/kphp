#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/wrappers/string_view.h"

namespace vk {
namespace tl {

struct arg;
struct combinator;
struct tl_scheme;
struct type;
struct type_expr;

class CombinatorToPhp;
struct PhpClasses;
struct PhpClassField;
struct PhpClassRepresentation;

struct PhpName;

class TlToPhpClassesConverter {
public:
  TlToPhpClassesConverter(const tl_scheme &scheme, PhpClasses &load_into);

  void register_function(const combinator &tl_function);

  void register_type(const type &tl_type,
                     std::string type_postfix,
                     CombinatorToPhp &outer_converter,
                     const type_expr &outer_type_expr,
                     bool is_builtin);

  const tl_scheme &get_sheme() const;
  const PhpClassRepresentation &get_rpc_function_interface() const;
  const PhpClassRepresentation &get_rpc_function_return_result_interface() const;

  bool need_rpc_response_type_hack(vk::string_view php_doc);

private:
  void register_builtin_classes();

  std::unique_ptr<PhpClassRepresentation> make_and_register_new_class(const PhpName &name,
                                                                      std::vector<PhpClassField> fields,
                                                                      int magic_id,
                                                                      const PhpClassRepresentation *parent = nullptr);

  std::unique_ptr<PhpClassRepresentation> make_and_register_new_interface(const PhpName &name, int magic_id, bool is_builtin);

  PhpClasses &php_classes_;
  const tl_scheme &scheme_;
  std::unordered_map<std::string, std::reference_wrapper<const type>> known_type_;
  const PhpClassRepresentation *rpc_function_interface_{nullptr};
  const PhpClassRepresentation *rpc_function_return_result_interface_{nullptr};
};

} // namespace vk
} // namespace tl
