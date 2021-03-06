// Compiler for PHP (aka KPHP)
// Copyright (c) 2020 LLC «V Kontakte»
// Distributed under the GPL v3 License, see LICENSE.notice.txt

#pragma once

#include <string>
#include <vector>
#include <sys/cdefs.h>

#include "net/net-memcache-server.h"

#include "common/stats/provider.h"
#include "common/wrappers/optional.h"

char *engine_default_prepare_stats_with_tag_mask(stats_type type, int *len, const char *statsd_prefix, unsigned int tag_mask);
char *engine_default_prepare_stats(stats_type type, int *len, const char *statsd_prefix);
const char *engine_default_char_stats();

void engine_default_tl_stat_function(const vk::optional<std::vector<std::string>> &sorted_filter_keys);
extern int (*tl_stat_function)(const vk::optional<std::vector<std::string>> &sorted_filter_keys);
