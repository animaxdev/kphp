#pragma once

#include <sys/cdefs.h>

void tl_store_init_str(char *s, int size);
void tl_fetch_init_str(const char *s, int size);

#ifdef __cplusplus

#include <utility>
#include "common/tl/fetch.h"
#include "common/tl/store.h"

namespace vk {
namespace tl {

template<typename Storer>
int save_to_buffer(char *buffer, int buffer_size, const Storer &store_func) {
  vk::tl::push_guard guard;
  tl_store_init_str(buffer, buffer_size);
  store_func();
  int size = static_cast<int>(tl_bytes_stored());
  tl_store_clear();
  return size;
}

template<typename T, typename Storer = vk::tl::storer<T>>
int store_to_buffer(char *buffer, int buffer_size, T &&value, const Storer &store_func = {}) {
  return save_to_buffer(buffer, buffer_size, [&] { store_func(std::forward<T>(value)); });
}

template<typename Fetcher>
int load_from_buffer(const char *buffer, int buffer_size, const Fetcher &fetch_func) {
  tlio_push();
  tl_fetch_init_str(buffer, buffer_size);
  fetch_func();
  int size = tl_bytes_fetched();
  tlio_pop();
  return size;
}

template<typename T, typename Fetcher = vk::tl::fetcher<T>>
int fetch_from_buffer(const char *buffer, int buffer_size, T &value, const Fetcher &fetch_func = {}) {
  return load_from_buffer(buffer, buffer_size, [&] { fetch_func(value); });
}

} // namespace tl
} // namespace vk
#endif
