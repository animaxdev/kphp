/* Autogenerated from kphp.tl and left only used constants */
#pragma once

#define                                             TL_KPHP_LEASE_STATS 0x3013ebf4U
#define                                      TL_KPHP_PROCESS_LEASE_TASK 0xa3fdb77cU
#define                                                   TL_KPHP_READY 0x6a34cac7U
#define                                                TL_KPHP_READY_V2 0xfbdb5d27U
#define                                             TL_KPHP_START_LEASE 0x61344739U
#define                                              TL_KPHP_STOP_LEASE 0x183bf49dU
#define                                              TL_KPHP_STOP_READY 0x59d86654U

#ifdef __cplusplus
#include <cstdint>
namespace vk {
namespace tl {
namespace kphp {

namespace lease_worker_settings_fields_mask {
constexpr static uint32_t                                        php_timeout = 1U << 0U;
} // namespace lease_worker_settings_fields_mask

namespace queue_types_mode_fields_mask {
constexpr static uint32_t                                           queue_id = 1U << 0U;
} // namespace queue_types_mode_fields_mask

namespace ready_v2_fields_mask {
constexpr static uint32_t                                         is_staging = 1U << 0U;
constexpr static uint32_t                                        worker_mode = 1U << 1U;
} // namespace ready_v2_fields_mask

} // namespace kphp
} // namespace tl
} // namespace vk
#endif
