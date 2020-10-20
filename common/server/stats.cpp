#include "common/server/stats.h"

#include <algorithm>

#include "common/server/engine-settings.h"
#include "common/tl/constants/engine.h"
#include "common/tl/fetch.h"
#include "common/tl/parse.h"
#include "common/wrappers/optional.h"

int (*tl_stat_function)(const vk::optional<std::vector<std::string>> &sorted_filter_keys);

char *engine_default_prepare_stats_with_tag_mask(stats_type type, int *len, const char *statsd_prefix, unsigned int tag_mask) {
  static char buf[1 << 20];
  stats_t stats;
  stats.type = type;
  stats.statsd_prefix = statsd_prefix;

  sb_init(&stats.sb, buf, 1 << 20);
  prepare_common_stats_with_tag_mask(&stats, tag_mask);

  if (len) {
    *len = stats.sb.pos;
  }
  return buf;
}

char *engine_default_prepare_stats(stats_type type, int *len, const char *statsd_prefix) {
  return engine_default_prepare_stats_with_tag_mask(type, len, statsd_prefix, STATS_TAG_MASK_FULL);
}

void engine_default_tl_stat_function(const vk::optional<std::vector<std::string>> &sorted_filter_keys) {
  tl_store_stats(engine_default_prepare_stats(STATS_TYPE_TL, NULL, NULL), 0, sorted_filter_keys);
}

const char *engine_default_char_stats() {
  return engine_default_prepare_stats(STATS_TYPE_TL, NULL, NULL);
}

__attribute__((constructor))
static void register_char_stats() {
  engine_settings_handlers.char_stats = engine_default_char_stats;
}



