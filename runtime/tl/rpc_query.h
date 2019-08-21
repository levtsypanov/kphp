#pragma once
#include "runtime/kphp_core.h"
#include "runtime/refcountable_php_classes.h"
#include "runtime/unique_object.h"

class RpcRequestResult;

struct RpcQuery : refcountable_php_classes<RpcQuery> {
  unique_object<RpcRequestResult> result_fetcher;
  string tl_function_name;
  int query_id{0};
};

class RpcPendingQueries {
public:
  void save(const class_instance<RpcQuery> &query);
  class_instance<RpcQuery> withdraw(int query_id);

  void hard_reset();

  static RpcPendingQueries &get() {
    static RpcPendingQueries queries;
    return queries;
  }

  int count() const { return queries_.count(); }

private:
  RpcPendingQueries() = default;

  array<class_instance<RpcQuery>> queries_;
};

class CurrentProcessingQuery {
public:
  static CurrentProcessingQuery &get() {
    static CurrentProcessingQuery context;
    return context;
  }

  void reset();
  void set_current_tl_function(const string &tl_function_name);
  void set_current_tl_function(const class_instance<RpcQuery> &current_query);
  void raise_fetching_error(const char *format, ...) __attribute__ ((format (printf, 2, 3)));
  void raise_storing_error(const char *format, ...) __attribute__ ((format (printf, 2, 3)));

private:
  string current_tl_function_name_;
};