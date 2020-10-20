#pragma once

#include <memory>

namespace vk {
template<typename T, void (*Deleter)(T *)>
class deleter_wrapper {
public:
  void operator()(T *x) {
    Deleter(x);
  }
};

template<typename T, void (*Deleter)(T *)>
using unique_ptr_with_delete_function = std::unique_ptr<T, deleter_wrapper<T, Deleter>>;

class free_wrapper {
public:
  void operator()(void *x) {
    free(x);
  }
};

template <class T>
using unique_ptr_with_free = std::unique_ptr<T, free_wrapper>;

}
