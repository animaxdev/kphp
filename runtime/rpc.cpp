#include "runtime/rpc.h"

#include "auto/TL/constants.h"
#include "common/crc32.h"
#include "common/rpc-const.h"

#include "PHP/common-net-functions.h"
#include "runtime/drivers.h"
#include "runtime/exception.h"
#include "runtime/files.h"
#include "runtime/misc.h"
#include "runtime/net_events.h"
#include "runtime/resumable.h"
#include "runtime/string_functions.h"
#include "runtime/zlib.h"

#include "PHP/tl/tl_init.h"

static const int GZIP_PACKED = 0x3072cfa1;

static const string UNDERSCORE("_", 1);
static const string STR_ERROR("__error", 7);
static const string STR_ERROR_CODE("__error_code", 12);

static const char *last_rpc_error;

static const int *rpc_data_begin;
static const int *rpc_data;
static int rpc_data_len;
static string rpc_data_copy;
static string rpc_filename;

static const int *rpc_data_begin_backup;
static const int *rpc_data_backup;
static int rpc_data_len_backup;
static string rpc_data_copy_backup;

tl_fetch_wrapper_ptr tl_fetch_wrapper;
array<tl_storer_ptr> tl_storers_ht;

template<class T>
static inline T store_parse_number(const string &v) {
  T result = 0;
  const char *s = v.c_str();
  int sign = 1;
  if (*s == '-') {
    s++;
    sign = -1;
  }
  while ('0' <= *s && *s <= '9') {
    result = result * 10 + (*s++ - '0');
  }
  return result * sign;
}

template<class T>
static inline T store_parse_number(const var &v) {
  if (!v.is_string()) {
    if (v.is_float()) {
      return (T)v.to_float();
    }
    return (T)v.to_int();
  }
  return store_parse_number<T>(v.to_string());
}


template<class T>
static inline T store_parse_number_unsigned(const string &v) {
  T result = 0;
  const char *s = v.c_str();
  while ('0' <= *s && *s <= '9') {
    result = result * 10 + (*s++ - '0');
  }
  return result;
}

template<class T>
static inline T store_parse_number_unsigned(const var &v) {
  if (!v.is_string()) {
    if (v.is_float()) {
      return (T)v.to_float();
    }
    return (T)v.to_int();
  }
  return store_parse_number_unsigned<T>(v.to_string());
}

template<class T>
static inline T store_parse_number_hex(const string &v) {
  T result = 0;
  const char *s = v.c_str();
  while (1) {
    T next = -1;
    if ('0' <= *s && *s <= '9') {
      next = *s - '0';
    } else if ('A' <= *s && *s <= 'F') {
      next = *s - ('A' - 10);
    } else if ('a' <= *s && *s <= 'f') {
      next = *s - ('a' - 10);
    }
    if (next == (T)-1) {
      break;
    }

    result = result * 16 + next;
    s++;
  }
  return result;
}


static void rpc_parse_save_backup() {
  dl::enter_critical_section();//OK
  rpc_data_copy_backup = rpc_data_copy;
  dl::leave_critical_section();

  rpc_data_begin_backup = rpc_data_begin;
  rpc_data_backup = rpc_data;
  rpc_data_len_backup = rpc_data_len;
}

static void rpc_parse_restore_previous() {
  php_assert ((rpc_data_copy_backup.size() & 3) == 0);

  dl::enter_critical_section();//OK
  rpc_data_copy = rpc_data_copy_backup;
  rpc_data_copy_backup = UNDERSCORE;//for assert
  dl::leave_critical_section();

  rpc_data_begin = rpc_data_begin_backup;
  rpc_data = rpc_data_backup;
  rpc_data_len = rpc_data_len_backup;
}

void rpc_parse(const int *new_rpc_data, int new_rpc_data_len) {
  rpc_parse_save_backup();

  rpc_data_begin = new_rpc_data;
  rpc_data = new_rpc_data;
  rpc_data_len = new_rpc_data_len;
}

bool f$rpc_parse(const string &new_rpc_data) {
  if (new_rpc_data.size() % sizeof(int) != 0) {
    php_warning("Wrong parameter \"new_rpc_data\" of len %d passed to function rpc_parse", (int)new_rpc_data.size());
    last_rpc_error = "Result's length is not divisible by 4";
    return false;
  }

  rpc_parse_save_backup();

  dl::enter_critical_section();//OK
  rpc_data_copy = new_rpc_data;
  dl::leave_critical_section();

  rpc_data_begin = rpc_data = reinterpret_cast <const int *> (rpc_data_copy.c_str());
  rpc_data_len = rpc_data_copy.size() / sizeof(int);
  return true;
}

bool f$rpc_parse(const var &new_rpc_data) {
  if (!new_rpc_data.is_string()) {
    php_warning("Parameter 1 of function rpc_parse must be a string, %s is given", new_rpc_data.get_type_c_str());
    return false;
  }

  return f$rpc_parse(new_rpc_data.to_string());
}

bool f$rpc_parse(const OrFalse<string> &new_rpc_data) {
  return new_rpc_data.bool_value ? f$rpc_parse(new_rpc_data.val()) : f$rpc_parse(var(false));
}

int rpc_get_pos() {
  return (int)(long)(rpc_data - rpc_data_begin);
}

bool rpc_set_pos(int pos) {
  if (pos < 0 || rpc_data_begin + pos > rpc_data) {
    return false;
  }

  rpc_data_len += (int)(rpc_data - rpc_data_begin - pos);
  rpc_data = rpc_data_begin + pos;
  return true;
}


static inline void check_rpc_data_len(int len) {
  if (rpc_data_len < len) {
    THROW_EXCEPTION(f$Exception$$__construct(rpc_filename, __LINE__, string("Not enough data to fetch", 24), -1));
    return;
  }
  rpc_data_len -= len;
}

int rpc_lookup_int() {
  TRY_CALL_VOID(int, (check_rpc_data_len(1)));
  rpc_data_len++;
  return *rpc_data;
}

int f$fetch_int() {
  TRY_CALL_VOID(int, (check_rpc_data_len(1)));
  return *rpc_data++;
}

UInt f$fetch_UInt() {
  TRY_CALL_VOID(UInt, (check_rpc_data_len(1)));
  return UInt((unsigned int)(*rpc_data++));
}

Long f$fetch_Long() {
  TRY_CALL_VOID(Long, (check_rpc_data_len(2)));
  long long result = *(long long *)rpc_data;
  rpc_data += 2;

  return Long(result);
}

ULong f$fetch_ULong() {
  TRY_CALL_VOID(ULong, (check_rpc_data_len(2)));
  unsigned long long result = *(unsigned long long *)rpc_data;
  rpc_data += 2;

  return ULong(result);
}

var f$fetch_unsigned_int() {
  TRY_CALL_VOID(var, (check_rpc_data_len(1)));
  unsigned int result = *rpc_data++;

  if (result <= (unsigned int)INT_MAX) {
    return (int)result;
  }

  return f$strval(UInt(result));
}

var f$fetch_long() {
  TRY_CALL_VOID(var, (check_rpc_data_len(2)));
  long long result = *(long long *)rpc_data;
  rpc_data += 2;

  if ((long long)INT_MIN <= result && result <= (long long)INT_MAX) {
    return (int)result;
  }

  return f$strval(Long(result));
}

var f$fetch_unsigned_long() {
  TRY_CALL_VOID(var, (check_rpc_data_len(2)));
  unsigned long long result = *(unsigned long long *)rpc_data;
  rpc_data += 2;

  if (result <= (unsigned long long)INT_MAX) {
    return (int)result;
  }

  return f$strval(ULong(result));
}

string f$fetch_unsigned_int_hex() {
  TRY_CALL_VOID(string, (check_rpc_data_len(1)));
  unsigned int result = *rpc_data++;

  char buf[8], *end_buf = buf + 8;
  for (int i = 0; i < 8; i++) {
    *--end_buf = lhex_digits[result & 15];
    result >>= 4;
  }

  return string(end_buf, 8);
}

string f$fetch_unsigned_long_hex() {
  TRY_CALL_VOID(string, (check_rpc_data_len(2)));
  unsigned long long result = *(unsigned long long *)rpc_data;
  rpc_data += 2;

  char buf[16], *end_buf = buf + 16;
  for (int i = 0; i < 16; i++) {
    *--end_buf = lhex_digits[result & 15];
    result >>= 4;
  }

  return string(end_buf, 16);
}

string f$fetch_unsigned_int_str() {
  return f$strval(TRY_CALL (UInt, string, (f$fetch_UInt())));
}

string f$fetch_unsigned_long_str() {
  return f$strval(TRY_CALL (ULong, string, (f$fetch_ULong())));
}

double f$fetch_double() {
  TRY_CALL_VOID(double, (check_rpc_data_len(2)));
  double result = *(double *)rpc_data;
  rpc_data += 2;

  return result;
}

static inline const char *f$fetch_string_raw(int *string_len) {
  TRY_CALL_VOID_(check_rpc_data_len(1), return nullptr);
  const char *str = reinterpret_cast <const char *> (rpc_data);
  int result_len = (unsigned char)*str++;
  if (result_len < 254) {
    TRY_CALL_VOID_(check_rpc_data_len(result_len >> 2), return nullptr);
    rpc_data += (result_len >> 2) + 1;
  } else if (result_len == 254) {
    result_len = (unsigned char)str[0] + ((unsigned char)str[1] << 8) + ((unsigned char)str[2] << 16);
    str += 3;
    TRY_CALL_VOID_(check_rpc_data_len((result_len + 3) >> 2), return nullptr);
    rpc_data += ((result_len + 7) >> 2);
  } else {
    THROW_EXCEPTION(f$Exception$$__construct(rpc_filename, __LINE__, string("Can't fetch string, 255 found", 29), -3));
    return nullptr;
  }

  *string_len = result_len;
  return str;
}

string f$fetch_string() {
  int result_len = 0;
  const char *str = TRY_CALL(const char*, string, f$fetch_string_raw(&result_len));
  return string(str, result_len);
}

int f$fetch_string_as_int() {
  int result_len = 0;
  const char *str = TRY_CALL(const char*, int, f$fetch_string_raw(&result_len));
  return string::to_int(str, result_len);
}

var f$fetch_memcache_value() {
  int res = TRY_CALL(int, bool, f$fetch_int());
  switch (res) {
    case MEMCACHE_VALUE_STRING: {
      int value_len = 0;
      const char *value = TRY_CALL(const char*, bool, f$fetch_string_raw(&value_len));
      int flags = TRY_CALL(int, bool, f$fetch_int());
      return mc_get_value(value, value_len, flags);
    }
    case MEMCACHE_VALUE_LONG: {
      var value = TRY_CALL(var, bool, f$fetch_long());
      int flags = TRY_CALL(int, bool, f$fetch_int());

      if (flags != 0) {
        php_warning("Wrong parameter flags = %d returned in Memcache::get", flags);
      }

      return value;
    }
    case MEMCACHE_VALUE_NOT_FOUND: {
      return false;
    }
    default: {
      php_warning("Wrong memcache.Value constructor = %x", res);
      THROW_EXCEPTION(f$Exception$$__construct(rpc_filename, __LINE__, string("Wrong memcache.Value constructor", 32), -1));
      return var();
    }
  }
}

bool f$fetch_eof() {
  return rpc_data_len == 0;
}

bool f$fetch_end() {
  if (rpc_data_len) {
    THROW_EXCEPTION(f$Exception$$__construct(rpc_filename, __LINE__, string("Too much data to fetch", 22), -2));
    return false;
  }
  return true;
}


rpc_connection::rpc_connection() :
  bool_value(false),
  host_num(-1),
  port(-1),
  timeout_ms(-1),
  default_actor_id(-1),
  connect_timeout(-1),
  reconnect_timeout(-1) {
}

rpc_connection::rpc_connection(bool value) :
  bool_value(value),
  host_num(-1),
  port(-1),
  timeout_ms(-1),
  default_actor_id(-1),
  connect_timeout(-1),
  reconnect_timeout(-1) {
}

rpc_connection::rpc_connection(bool value, int host_num, int port, int timeout_ms, long long default_actor_id, int connect_timeout, int reconnect_timeout) :
  bool_value(value),
  host_num(host_num),
  port(port),
  timeout_ms(timeout_ms),
  default_actor_id(default_actor_id),
  connect_timeout(connect_timeout),
  reconnect_timeout(reconnect_timeout) {
}

rpc_connection &rpc_connection::operator=(bool value) {
  bool_value = value;
  return *this;
}


rpc_connection f$new_rpc_connection(string host_name, int port, const var &default_actor_id, double timeout, double connect_timeout, double reconnect_timeout) {
  int host_num = rpc_connect_to(host_name.c_str(), port);
  if (host_num < 0) {
    return rpc_connection();
  }

  return rpc_connection(true, host_num, port, timeout_convert_to_ms(timeout),
                        store_parse_number<long long>(default_actor_id),
                        timeout_convert_to_ms(connect_timeout), timeout_convert_to_ms(reconnect_timeout));
}

bool f$boolval(const rpc_connection &my_rpc) {
  return my_rpc.bool_value;
}

bool eq2(const rpc_connection &my_rpc, bool value) {
  return my_rpc.bool_value == value;
}

bool eq2(bool value, const rpc_connection &my_rpc) {
  return value == my_rpc.bool_value;
}

bool equals(bool value, const rpc_connection &my_rpc) {
  return equals(value, my_rpc.bool_value);
}

bool equals(const rpc_connection &my_rpc, bool value) {
  return equals(my_rpc.bool_value, value);
}


static string_buffer data_buf;
static string_buffer old_data_buf;
static const int data_buf_header_size = 2 * sizeof(long long) + 4 * sizeof(int);
static const int data_buf_header_reserved_size = sizeof(long long) + sizeof(int);

int rpc_stored;
static int rpc_pack_threshold;
static int rpc_pack_from;

void f$store_gzip_pack_threshold(int pack_threshold_bytes) {
  rpc_pack_threshold = pack_threshold_bytes;
}

void f$store_start_gzip_pack() {
  rpc_pack_from = data_buf.size();
}

void f$store_finish_gzip_pack(int threshold) {
  if (rpc_pack_from != -1 && threshold > 0) {
    int answer_size = data_buf.size() - rpc_pack_from;
    php_assert (rpc_pack_from % sizeof(int) == 0 && 0 <= rpc_pack_from && 0 <= answer_size);
    if (answer_size >= threshold) {
      const char *answer_begin = data_buf.c_str() + rpc_pack_from;
      const string_buffer *compressed = zlib_encode(answer_begin, answer_size, 6, ZLIB_ENCODE);

      if ((int)(compressed->size() + 2 * sizeof(int)) < answer_size) {
        data_buf.set_pos(rpc_pack_from);
        f$store_int(GZIP_PACKED);
        store_string(compressed->buffer(), compressed->size());
      }
    }
  }
  rpc_pack_from = -1;
}


template<class T>
inline bool store_raw(T v) {
  data_buf.append((char *)&v, sizeof(v));
  return true;
}

bool f$store_raw(const string &data) {
  int data_len = (int)data.size();
  if (data_len & 3) {
    return false;
  }
  data_buf.append(data.c_str(), data_len);
  return true;
}

bool store_header(long long cluster_id, int flags) {
  if (flags) {
    f$store_int(TL_RPC_DEST_ACTOR_FLAGS);
    store_long(cluster_id);
    f$store_int(flags);
  } else {
    f$store_int(TL_RPC_DEST_ACTOR);
    store_long(cluster_id);
  }
  return true;
}

bool f$store_header(const var &cluster_id, int flags) {
  return store_header(store_parse_number<long long>(cluster_id), flags);
}

bool store_error(int error_code, const char *error_text, int error_text_len) {
  f$rpc_clean(true);
  f$store_int(error_code);
  store_string(error_text, error_text_len);
  rpc_store(true);
  script_error();
  return true;
}

bool store_error(int error_code, const char *error_text) {
  return store_error(error_code, error_text, (int)strlen(error_text));
}

bool f$store_error(int error_code, const string &error_text) {
  return store_error(error_code, error_text.c_str(), (int)error_text.size());
}

bool f$store_int(int v) {
  return store_raw(v);
}

bool f$store_UInt(UInt v) {
  return store_raw(v.l);
}

bool f$store_Long(Long v) {
  return store_raw(v.l);
}

bool f$store_ULong(ULong v) {
  return store_raw(v.l);
}

bool store_unsigned_int(unsigned int v) {
  return store_raw(v);
}

bool store_long(long long v) {
  return store_raw(v);
}

bool store_unsigned_long(unsigned long long v) {
  return store_raw(v);
}

bool f$store_unsigned_int(const string &v) {
  return store_raw(store_parse_number_unsigned<unsigned int>(v));
}

bool f$store_long(const string &v) {
  return store_raw(store_parse_number<long long>(v));
}

bool f$store_unsigned_long(const string &v) {
  return store_raw(store_parse_number_unsigned<unsigned long long>(v));
}

bool f$store_unsigned_int_hex(const string &v) {
  return store_raw(store_parse_number_hex<unsigned int>(v));
}

bool f$store_unsigned_long_hex(const string &v) {
  return store_raw(store_parse_number_hex<unsigned long long>(v));
}

bool f$store_double(double v) {
  return store_raw(v);
}

bool store_string(const char *v, int v_len) {
  int all_len = v_len;
  if (v_len < 254) {
    data_buf << (char)(v_len);
    all_len += 1;
  } else if (v_len < (1 << 24)) {
    data_buf
      << (char)(254)
      << (char)(v_len & 255)
      << (char)((v_len >> 8) & 255)
      << (char)((v_len >> 16) & 255);
    all_len += 4;
  } else {
    php_critical_error ("trying to store too big string of length %d", v_len);
  }
  data_buf.append(v, v_len);

  while (all_len % 4 != 0) {
    data_buf << '\0';
    all_len++;
  }
  return true;
}

bool f$store_string(const string &v) {
  return store_string(v.c_str(), (int)v.size());
}

bool f$store_many(const array<var> &a) {
  int n = a.count();
  if (n == 0) {
    php_warning("store_many must take at least 1 argument");
    return false;
  }

  string pattern = a.get_value(0).to_string();
  if (n != 1 + (int)pattern.size()) {
    php_warning("Wrong number of arguments in call to store_many");
    return false;
  }

  for (int i = 1; i < n; i++) {
    switch (pattern[i - 1]) {
      case 's':
        f$store_string(a.get_value(i).to_string());
        break;
      case 'l':
        f$store_long(a.get_value(i));
        break;
      case 'd':
      case 'i':
        f$store_int(a.get_value(i).to_int());
        break;
      case 'f':
        f$store_double(a.get_value(i).to_float());
        break;
      default:
        php_warning("Wrong symbol '%c' at position %d in first argument of store_many", pattern[i - 1], i - 1);
        break;
    }
  }

  return true;
}


bool f$store_finish() {
  return rpc_store(false);
}

bool f$rpc_clean(bool is_error) {
  new_tl_mode_error_flag = false;
  data_buf.clean();
  f$store_int(-1); //reserve for TL_RPC_DEST_ACTOR
  store_long(-1); //reserve for actor_id
  f$store_int(-1); //reserve for length
  f$store_int(-1); //reserve for num
  f$store_int(-is_error); //reserve for type
  store_long(-1); //reserve for req_id

  rpc_pack_from = -1;
  return true;
}

string f$rpc_get_clean() {
  string data = string(data_buf.c_str() + data_buf_header_size, (int)(data_buf.size() - data_buf_header_size));
  f$rpc_clean();
  return data;
}

string f$rpc_get_contents() {
  return string(data_buf.c_str() + data_buf_header_size, (int)(data_buf.size() - data_buf_header_size));
}


bool rpc_store(bool is_error) {
  if (rpc_stored) {
    return false;
  }

  if (!is_error) {
    rpc_pack_from = data_buf_header_size;
    f$store_finish_gzip_pack(rpc_pack_threshold);
  }

  f$store_int(-1); // reserve for crc32
  rpc_stored = 1;
  rpc_answer(data_buf.c_str() + data_buf_header_reserved_size, (int)(data_buf.size() - data_buf_header_reserved_size));
  return true;
}


struct rpc_request {
  int resumable_id; // == 0 - default, > 0 if not finished, -1 if received an answer, -2 if received an error, -3 if answer was gotten
  union {
    event_timer *timer;
    char *answer;
    const char *error;
  };
};

static rpc_request *rpc_requests;
static int rpc_requests_size;
static long long rpc_requests_last_query_num;

static slot_id_t rpc_first_request_id;
static slot_id_t rpc_first_array_request_id;
static slot_id_t rpc_next_request_id;
static slot_id_t rpc_first_unfinished_request_id;

static rpc_request gotten_rpc_request;

static int timeout_wakeup_id = -1;

static inline rpc_request *get_rpc_request(slot_id_t request_id) {
  php_assert (rpc_first_request_id <= request_id && request_id < rpc_next_request_id);
  if (request_id < rpc_first_array_request_id) {
    return &gotten_rpc_request;
  }
  return &rpc_requests[request_id - rpc_first_array_request_id];
}

var load_rpc_request_as_var(char *storage) {
  rpc_request *data = reinterpret_cast <rpc_request *> (storage);
  var result;
  if (data->resumable_id == -2) {
    result = false;
  } else {
    php_assert (data->resumable_id == -1);

    string result_str;
    result_str.assign_raw(data->answer - 12);
    result = result_str;
  }

  data->resumable_id = -3;
  return result;
}

class rpc_resumable : public Resumable {
private:
  int request_id;
  int port;
  long long actor_id;
  double begin_time;

protected:
  bool run() {
    php_assert (dl::query_num == rpc_requests_last_query_num);
    rpc_request *request = get_rpc_request(request_id);
    php_assert (request->resumable_id < 0);
    php_assert (input_ == nullptr);

/*
    if (request->resumable_id == -1) {
      int len = *reinterpret_cast <int *>(request->answer - 12);
      fprintf (stderr, "Receive  string of len %d at %p\n", len, request->answer);
      for (int i = -12; i <= len; i++) {
        fprintf (stderr, "%d: %x(%d)\t%c\n", i, request->answer[i], request->answer[i], request->answer[i] >= 32 ? request->answer[i] : '.');
      }
    }
*/
    if (rpc_first_unfinished_request_id == request_id) {
      while (rpc_first_unfinished_request_id < rpc_next_request_id &&
             get_rpc_request(rpc_first_unfinished_request_id)->resumable_id < 0) {
        rpc_first_unfinished_request_id++;
      }
      if (rpc_first_unfinished_request_id < rpc_next_request_id) {
        int resumable_id = get_rpc_request(rpc_first_unfinished_request_id)->resumable_id;
        php_assert (resumable_id > 0);
        const Resumable *resumable = get_forked_resumable(resumable_id);
        php_assert (resumable != nullptr);
        static_cast <const rpc_resumable *>(resumable)->set_server_status_rpc();
      } else {
        ::set_server_status_rpc(0, 0, get_precise_now());
      }
    }

    request_id = -1;
    output_->save<rpc_request>(*request, load_rpc_request_as_var);
    php_assert (request->resumable_id == -2 || request->resumable_id == -1);
    request->resumable_id = -3;
    request->answer = nullptr;

    return true;
  }

  void set_server_status_rpc() const {
    ::set_server_status_rpc(port, actor_id, begin_time);
  }

public:
  rpc_resumable(int request_id, int port, long long actor_id) :
    request_id(request_id),
    port(port),
    actor_id(actor_id),
    begin_time(get_precise_now()) {
    if (rpc_first_unfinished_request_id == request_id) {
      set_server_status_rpc();
    }
  }
};

static array<double> rpc_request_need_timer;

static void process_rpc_timeout(int request_id) {
  process_rpc_error(request_id, TL_ERROR_QUERY_TIMEOUT, "Timeout in KPHP runtime");
}

static void process_rpc_timeout(event_timer *timer) {
  return process_rpc_timeout(timer->wakeup_extra);
}

int rpc_send(const rpc_connection &conn, double timeout, bool ignore_answer) {
  if (unlikely (conn.host_num < 0)) {
    php_warning("Wrong rpc_connection specified");
    return -1;
  }

  if (timeout <= 0 || timeout > MAX_TIMEOUT) {
    timeout = conn.timeout_ms * 0.001;
  }

  f$store_int(-1); // reserve for crc32
  php_assert (data_buf.size() % sizeof(int) == 0);

  int reserved = data_buf_header_reserved_size;
  if (conn.default_actor_id) {
    const char *answer_begin = data_buf.c_str() + data_buf_header_size;
    int x = *(int *)answer_begin;
    if (x != TL_RPC_DEST_ACTOR && x != TL_RPC_DEST_ACTOR_FLAGS) {
      reserved -= (int)(sizeof(int) + sizeof(long long));
      php_assert (reserved >= 0);
      *(int *)(answer_begin - sizeof(int) - sizeof(long long)) = TL_RPC_DEST_ACTOR;
      *(long long *)(answer_begin - sizeof(long long)) = conn.default_actor_id;
    }
  }

  dl::size_type request_size = (dl::size_type)(data_buf.size() - reserved);
  void *p = dl::allocate(request_size);
  memcpy(p, data_buf.c_str() + reserved, request_size);

  slot_id_t result = rpc_send_query(conn.host_num, (char *)p, (int)request_size, timeout_convert_to_ms(timeout));
  if (result <= 0) {
    return -1;
  }

  if (dl::query_num != rpc_requests_last_query_num) {
    rpc_requests_last_query_num = dl::query_num;
    rpc_requests_size = 170;
    rpc_requests = static_cast <rpc_request *> (dl::allocate(sizeof(rpc_request) * rpc_requests_size));

    rpc_first_request_id = result;
    rpc_first_array_request_id = result;
    rpc_next_request_id = result + 1;
    rpc_first_unfinished_request_id = result;
    gotten_rpc_request.resumable_id = -3;
    gotten_rpc_request.answer = nullptr;
  } else {
    php_assert (rpc_next_request_id == result);
    rpc_next_request_id++;
  }

  if (result - rpc_first_array_request_id >= rpc_requests_size) {
    php_assert (result - rpc_first_array_request_id == rpc_requests_size);
    if (rpc_first_unfinished_request_id > rpc_first_array_request_id + rpc_requests_size / 2) {
      memcpy(rpc_requests,
             rpc_requests + rpc_first_unfinished_request_id - rpc_first_array_request_id,
             sizeof(rpc_request) * (rpc_requests_size - (rpc_first_unfinished_request_id - rpc_first_array_request_id)));
      rpc_first_array_request_id = rpc_first_unfinished_request_id;
    } else {
      rpc_requests = static_cast <rpc_request *> (dl::reallocate(rpc_requests, sizeof(rpc_request) * 2 * rpc_requests_size, sizeof(rpc_request) * rpc_requests_size));
      rpc_requests_size *= 2;
    }
  }

  rpc_request *cur = get_rpc_request(result);

  cur->resumable_id = register_forked_resumable(new rpc_resumable(result, conn.port, conn.default_actor_id));
  cur->timer = nullptr;
  if (ignore_answer) {
    int resumable_id = cur->resumable_id;
    process_rpc_timeout(result);
    get_forked_storage(resumable_id)->load<rpc_request, rpc_request>();
    return resumable_id;
  } else {
    rpc_request_need_timer.set_value(result, timeout);
    return cur->resumable_id;
  }
}

void f$rpc_flush() {
  update_precise_now();
  wait_net(0);
  update_precise_now();
  for (array<double>::iterator iter = rpc_request_need_timer.begin(); iter != rpc_request_need_timer.end(); ++iter) {
    int id = iter.get_key().to_int();
    rpc_request *cur = get_rpc_request(id);
    if (cur->resumable_id > 0) {
      php_assert (cur->timer == nullptr);
      cur->timer = allocate_event_timer(iter.get_value() + get_precise_now(), timeout_wakeup_id, id);
    }
  }
  rpc_request_need_timer.clear();
}

int f$rpc_send(const rpc_connection &conn, double timeout) {
  int request_id = rpc_send(conn, timeout);
  if (request_id <= 0) {
    return 0;
  }

  f$rpc_flush();
  return request_id;
}

int f$rpc_send_noflush(const rpc_connection &conn, double timeout) {
  int request_id = rpc_send(conn, timeout);
  if (request_id <= 0) {
    return 0;
  }

  return request_id;
}


void process_rpc_answer(int request_id, char *result, int result_len __attribute__((unused))) {
  rpc_request *request = get_rpc_request(request_id);

  if (request->resumable_id < 0) {
    php_assert (result != nullptr);
    dl::deallocate(result - 12, result_len + 13);
    php_assert (request->resumable_id != -1);
    return;
  }
  int resumable_id = request->resumable_id;
  request->resumable_id = -1;

  if (request->timer) {
    remove_event_timer(request->timer);
  }

  php_assert (result != nullptr);
  request->answer = result;
//  fprintf (stderr, "answer_len = %d\n", result_len);

  php_assert (resumable_id > 0);
  resumable_run_ready(resumable_id);
}

void process_rpc_error(int request_id, int error_code __attribute__((unused)), const char *error_message) {
  rpc_request *request = get_rpc_request(request_id);

  if (request->resumable_id < 0) {
    php_assert (request->resumable_id != -1);
    return;
  }
  int resumable_id = request->resumable_id;
  request->resumable_id = -2;

  if (request->timer) {
    remove_event_timer(request->timer);
  }

  request->error = error_message;

  php_assert (resumable_id > 0);
  resumable_run_ready(resumable_id);
}


class rpc_get_resumable : public Resumable {
  typedef OrFalse<string> ReturnT;
  int resumable_id;
  double timeout;

  bool ready;
protected:
  bool run() {
    RESUMABLE_BEGIN
      ready = f$wait(resumable_id, timeout);
      TRY_WAIT(rpc_get_resumable_label_0, ready, bool);
      if (!ready) {
        last_rpc_error = last_wait_error;
        RETURN(false);
      }

      Storage *input = get_forked_storage(resumable_id);
      if (input->getter_ == nullptr) {
        last_rpc_error = "Result already was gotten";
        RETURN(false);
      }
      if (input->getter_ != load_rpc_request_as_var) {
        last_rpc_error = "Not a rpc request";
        RETURN(false);
      }

      rpc_request res = input->load<rpc_request, rpc_request>();
      php_assert (CurException.is_null());

      if (res.resumable_id == -2) {
        last_rpc_error = res.error;
        RETURN(false);
      }

      php_assert (res.resumable_id == -1);

      string result;
      result.assign_raw(res.answer - 12);
      RETURN(result);
    RESUMABLE_END
  }

public:
  rpc_get_resumable(int resumable_id, double timeout) :
    resumable_id(resumable_id),
    timeout(timeout) {
  }
};


OrFalse<string> f$rpc_get(int request_id, double timeout) {
  return start_resumable<OrFalse<string>>(new rpc_get_resumable(request_id, timeout));
}

OrFalse<string> f$rpc_get_synchronously(int request_id) {
  wait_synchronously(request_id);
  OrFalse<string> result = f$rpc_get(request_id);
  php_assert (resumable_finished);
  return result;
}

class rpc_get_and_parse_resumable : public Resumable {
  typedef bool ReturnT;
  int resumable_id;
  double timeout;

  bool ready;
protected:
  bool run() {
    RESUMABLE_BEGIN
      ready = f$wait(resumable_id, timeout);
      TRY_WAIT(rpc_get_and_parse_resumable_label_0, ready, bool);
      if (!ready) {
        last_rpc_error = last_wait_error;
        RETURN(false);
      }

      Storage *input = get_forked_storage(resumable_id);
      if (input->getter_ == nullptr) {
        last_rpc_error = "Result already was gotten";
        RETURN(false);
      }
      if (input->getter_ != load_rpc_request_as_var) {
        last_rpc_error = "Not a rpc request";
        RETURN(false);
      }

      rpc_request res = input->load<rpc_request, rpc_request>();
      php_assert (CurException.is_null());

      if (res.resumable_id == -2) {
        last_rpc_error = res.error;
        RETURN(false);
      }

      php_assert (res.resumable_id == -1);

      string result;
      result.assign_raw(res.answer - 12);
      bool parse_result = f$rpc_parse(result);
      php_assert(parse_result);

      RETURN(true);
    RESUMABLE_END
  }

public:
  rpc_get_and_parse_resumable(int resumable_id, double timeout) :
    resumable_id(resumable_id),
    timeout(timeout) {
  }
};

bool f$rpc_get_and_parse(int request_id, double timeout) {
  return start_resumable<bool>(new rpc_get_and_parse_resumable(request_id, timeout));
}


int f$query_x2(int x) {
  return query_x2(x);
}


/*
 *
 *  var wrappers
 *
 */


bool f$store_unsigned_int(const var &v) {
  return store_unsigned_int(store_parse_number_unsigned<unsigned int>(v));
}

bool f$store_long(const var &v) {
  return store_long(store_parse_number<long long>(v));
}

bool f$store_unsigned_long(const var &v) {
  return store_unsigned_long(store_parse_number_unsigned<unsigned long long>(v));
}


/*
 *
 *     RPC_TL_QUERY
 *
 */


int tl_parse_int() {
  return TRY_CALL(int, int, (f$fetch_int()));
}

long long tl_parse_long() {
  return TRY_CALL(long long, int, (f$fetch_Long().l));
}

double tl_parse_double() {
  return TRY_CALL(double, double, (f$fetch_double()));
}

string tl_parse_string() {
  return TRY_CALL(string, string, (f$fetch_string()));
}

void tl_parse_end() {
  TRY_CALL_VOID(void, (f$fetch_end()));
}

int tl_parse_save_pos() {
  return rpc_get_pos();
}

bool tl_parse_restore_pos(int pos) {
  return rpc_set_pos(pos);
}


const int NODE_TYPE_TYPE = 1;
const int NODE_TYPE_NAT_CONST = 2;
const int NODE_TYPE_VAR_TYPE = 3;
const int NODE_TYPE_VAR_NUM = 4;
const int NODE_TYPE_ARRAY = 5;

const int ID_VAR_NUM = 0x70659eff;
const int ID_VAR_TYPE = 0x2cecf817;
const int ID_INT = 0xa8509bda;
const int ID_LONG = 0x22076cba;
const int ID_DOUBLE = 0x2210c154;
const int ID_STRING = 0xb5286e24;
const int ID_VECTOR = 0x1cb5c415;
const int ID_DICTIONARY = 0x1f4c618f;
const int ID_INT_KEY_DICTIONARY = 0x07bafc42;
const int ID_LONG_KEY_DICTIONARY = 0xb424d8f1;
const int ID_MAYBE_TRUE = 0x3f9c8ef8;
const int ID_MAYBE_FALSE = 0x27930a7b;
const int ID_BOOL_FALSE = 0xbc799737;
const int ID_BOOL_TRUE = 0x997275b5;
const int TYPE_ID_BOOL = 0x250be282;

const int FLAG_OPT_VAR = (1 << 17);
const int FLAG_EXCL = (1 << 18);
const int FLAG_OPT_FIELD = (1 << 20);
const int FLAG_NOVAR = (1 << 21);
const int FLAG_DEFAULT_CONSTRUCTOR = (1 << 25);
const int FLAG_BARE = (1 << 0);
const int FLAG_NOCONS = (1 << 1);
const int FLAGS_MASK = ((1 << 16) - 1);


class tl_combinator;

class tl_tree;

class tl_type {
public:
  int id;
  string name;
  int arity;
  int flags;
  int constructors_num;
  array<tl_combinator *> constructors;
};

class arg {
public:
  string name;
  int flags;
  int var_num;
  int exist_var_num;
  int exist_var_bit;
  tl_tree *type;
};

class tl_combinator {
public:
  int id;
  string name;
  int var_count;
  int type_id;
  array<arg> args;
  tl_tree *result;

  void **IP;
  void **fetchIP;
  int IP_len;
  int fetchIP_len;
};

class tl_tree {
public:
  int flags;

  tl_tree(int flags) :
    flags(flags) {
  }

  virtual void print(int shift = 0) const = 0;

  virtual int get_type() const = 0;

  virtual bool equals(const tl_tree *other) const = 0;

  virtual tl_tree *dup() const = 0;

  virtual void destroy() = 0;

  virtual ~tl_tree() {
  }
};

class tl_tree_type : public tl_tree {
public:
  tl_type *type;
  array<tl_tree *> children;

  tl_tree_type(int flags, tl_type *type, const array_size &s) :
    tl_tree(flags),
    type(type),
    children(s) {
  }

  virtual void print(int shift = 0) const {
    fprintf(stderr, "%*sType %s(%x) at (%p)\n", shift, "", type->name.c_str(), type->id, this);
    for (array<tl_tree *>::const_iterator iter = children.begin(); iter != children.end(); ++iter) {
      iter.get_value()->print(shift + 4);
    }
  }

  virtual int get_type() const {
    return NODE_TYPE_TYPE;
  }

  virtual bool equals(const tl_tree *other_) const {
    if (other_->get_type() != NODE_TYPE_TYPE) {
      return false;
    }
    const tl_tree_type *other = static_cast <const tl_tree_type *> (other_);
    if ((flags & FLAGS_MASK) != (other->flags & FLAGS_MASK) || type->id != other->type->id) {
      return false;
    }
    for (int i = 0; i < children.count(); i++) {
      if (!children.get_value(i)->equals(other->children.get_value(i))) {
        return false;
      }
    }
    return true;
  }

  virtual tl_tree *dup() const {
    tl_tree_type *T = (tl_tree_type *)dl::allocate(sizeof(tl_tree_type));
    //fprintf (stderr, "dup type %s (%p), result = %p\n", type->name.c_str(), this, T);
    new(T) tl_tree_type(flags, type, children.size());

    for (int i = 0; i < children.count(); i++) {
      T->children.set_value(i, children.get_value(i)->dup());
    }
    return T;
  }

  virtual void destroy() {
    for (int i = 0; i < children.count(); i++) {
      if (children.get_value(i) != nullptr) {
        children.get_value(i)->destroy();
      }
    }

    this->~tl_tree_type();
    dl::deallocate(this, sizeof(*this));
  }
};

class tl_tree_nat_const : public tl_tree {
public:
  int num;

  tl_tree_nat_const(int flags, int num) :
    tl_tree(flags),
    num(num) {
  }

  virtual void print(int shift = 0) const {
    fprintf(stderr, "%*sConst %d\n", shift, "", num);
  }

  virtual int get_type() const {
    return NODE_TYPE_NAT_CONST;
  }

  virtual bool equals(const tl_tree *other) const {
    return other->get_type() == NODE_TYPE_NAT_CONST && num == static_cast <const tl_tree_nat_const *> (other)->num;
  }

  virtual tl_tree *dup() const {
    tl_tree_nat_const *T = (tl_tree_nat_const *)dl::allocate(sizeof(tl_tree_nat_const));
    //fprintf (stderr, "dup nat const %d (%p), result = %p\n", num, this, T);
    new(T) tl_tree_nat_const(flags, num);

    return T;
  }

  virtual void destroy() {
    this->~tl_tree_nat_const();
    dl::deallocate(this, sizeof(*this));
  }
};

class tl_tree_var_type : public tl_tree {
public:
  int var_num;

  tl_tree_var_type(int flags, int var_num) :
    tl_tree(flags),
    var_num(var_num) {
  }

  virtual void print(int shift = 0) const {
    fprintf(stderr, "%*sVariable type, var_num = %d.\n", shift, "", var_num);
  }

  virtual int get_type() const {
    return NODE_TYPE_VAR_TYPE;
  }

  virtual bool equals(const tl_tree *other __attribute__((unused))) const {
    php_assert (false);
    return false;
  }

  virtual tl_tree *dup() const {
    tl_tree_var_type *T = (tl_tree_var_type *)dl::allocate(sizeof(tl_tree_var_type));
    //fprintf (stderr, "dup var type (%p), result = %p\n", this, T);
    new(T) tl_tree_var_type(flags, var_num);

    return T;
  }

  virtual void destroy() {
    this->~tl_tree_var_type();
    dl::deallocate(this, sizeof(*this));
  }
};

class tl_tree_var_num : public tl_tree {
public:
  int var_num;
  int diff;

  tl_tree_var_num(int flags, int var_num, int diff) :
    tl_tree(flags),
    var_num(var_num),
    diff(diff) {
  }

  virtual void print(int shift = 0) const {
    fprintf(stderr, "%*sVariable number, var_num = %d, diff = %d.\n", shift, "", var_num, diff);
  }

  virtual int get_type() const {
    return NODE_TYPE_VAR_NUM;
  }

  virtual bool equals(const tl_tree *other __attribute__((unused))) const {
    php_assert (false);
    return false;
  }

  virtual tl_tree *dup() const {
    tl_tree_var_num *T = (tl_tree_var_num *)dl::allocate(sizeof(tl_tree_var_num));
    //fprintf (stderr, "dup var num (%p), result = %p\n", this, T);
    new(T) tl_tree_var_num(flags, var_num, diff);

    return T;
  }

  virtual void destroy() {
    this->~tl_tree_var_num();
    dl::deallocate(this, sizeof(*this));
  }
};

class tl_tree_array : public tl_tree {
public:
  tl_tree *multiplicity;
  array<arg> args;

  tl_tree_array(int flags, tl_tree *multiplicity, const array_size &s) :
    tl_tree(flags),
    multiplicity(multiplicity),
    args(s) {
  }

  tl_tree_array(int flags, tl_tree *multiplicity, const array<arg> &a) :
    tl_tree(flags),
    multiplicity(multiplicity),
    args(a) {
  }

  virtual void print(int shift = 0) const {
    fprintf(stderr, "%*sArray, number of elements = ", shift, "");
    multiplicity->print();

    fprintf(stderr, "%*s    elements:", shift, "");
    for (int i = 0; i < args.count(); i++) {
      fprintf(stderr, "%*s    name = %s, var_num = %d\n", shift, "", args.get_value(i).name.c_str(), args.get_value(i).var_num);
      args.get_value(i).type->print(shift + 4);
    }
  }

  virtual int get_type() const {
    return NODE_TYPE_ARRAY;
  }

  virtual bool equals(const tl_tree *other_) const {
    if (other_->get_type() != NODE_TYPE_ARRAY) {
      return false;
    }
    const tl_tree_array *other = static_cast <const tl_tree_array *> (other_);
    if (flags != other->flags || args.count() != other->args.count()) {
      return false;
    }
    for (int i = 0; i < args.count(); i++) {
      if (args.get_value(i).name != other->args.get_value(i).name ||
          !args.get_value(i).type->equals(other->args.get_value(i).type)) {
        return false;
      }
    }
    return true;
  }

  virtual tl_tree *dup() const {
    tl_tree_array *T = (tl_tree_array *)dl::allocate(sizeof(tl_tree_array));
    //fprintf (stderr, "dup array (%p), result = %p\n", this, T);
    new(T) tl_tree_array(flags, multiplicity->dup(), args.size());

    for (int i = 0; i < args.count(); i++) {
      T->args[i] = args.get_value(i);
      T->args[i].type = T->args[i].type->dup();
    }

    return T;
  }

  virtual void destroy() {
    multiplicity->destroy();
    for (int i = 0; i < args.count(); i++) {
      if (args[i].type != nullptr) {
        args[i].type->destroy();
      }
    }

    this->~tl_tree_array();
    dl::deallocate(this, sizeof(*this));
  }
};

const int TLS_SCHEMA_V2 = 0x3a2f9be2;
const int TLS_SCHEMA_V3 = 0xe4a8604b;
const int TLS_TYPE = 0x12eb4386;
const int TLS_COMBINATOR = 0x5c0a1ed5;
const int TLS_COMBINATOR_LEFT_BUILTIN = 0xcd211f63;
const int TLS_COMBINATOR_LEFT = 0x4c12c6d9;
const int TLS_COMBINATOR_RIGHT_V2 = 0x2c064372;
const int TLS_ARG_V2 = 0x29dfe61b;

const int TLS_EXPR_TYPE = 0xecc9da78;

const int TLS_NAT_CONST = 0xdcb49bd8;
const int TLS_NAT_VAR = 0x4e8a14f0;
const int TLS_TYPE_VAR = 0x0142ceae;
const int TLS_ARRAY = 0xd9fb20de;
const int TLS_TYPE_EXPR = 0xc1863d08;


static class TlConfig {
public:
  array<tl_type *> types;
  array<tl_type *> id_to_type;
  array<tl_type *> name_to_type;

  array<tl_combinator *> functions;
  array<tl_combinator *> id_to_function;
  array<tl_combinator *> name_to_function;

  tl_type *ReqResult;

  void **fetchIP;

  ~TlConfig() {
    // TODO may be it makes sense to destroy it more gracefully
    hard_reset_var(types);
    hard_reset_var(id_to_type);
    hard_reset_var(name_to_type);

    hard_reset_var(functions);
    hard_reset_var(id_to_function);
    hard_reset_var(name_to_function);
  }
} tl_config;


int get_constructor_by_name(const tl_type *t, const string &name) {
  for (int i = 0; i < t->constructors_num; i++) {
    if (t->constructors.get_value(i)->name == name) {
      return i;
    }
  }
  return -1;
}

int get_constructor_by_id(const tl_type *t, int id) {
  for (int i = 0; i < t->constructors_num; i++) {
    if (t->constructors.get_value(i)->id == id) {
      return i;
    }
  }
  return -1;
}

inline void tl_debug(const char *s __attribute__((unused)), int n __attribute__((unused))) {
  //fprintf(stderr, "%s\n", s);
}

const int MAX_VARS = 100000;
static tl_tree *vars_buffer[MAX_VARS];
static tl_tree **last_var_ptr;

tl_tree **get_var_space(tl_tree **vars, int n) {
  tl_tree **res = vars - n;

  php_assert (res >= vars_buffer);

  for (int i = 0; i < n; i++) {
    if (res[i] != nullptr && res + i >= last_var_ptr) {
      res[i]->destroy();
    }
    res[i] = nullptr;
  }
  if (last_var_ptr > res) {
    last_var_ptr = res;
  }

  return res;
}

void free_var_space(tl_tree **vars, int n) {
  for (int i = 0; i < n; i++) {
    if (vars[i] != nullptr) {
      vars[i]->destroy();
      vars[i] = nullptr;
    }
  }
}


static const int MAX_SIZE = 100000;
static void *Data_stack[MAX_SIZE];

static const int MAX_DEPTH = 10000;
static var var_stack[MAX_DEPTH];

var *last_arr_ptr;

void free_arr_space() {
  while (last_arr_ptr >= var_stack) {
    *last_arr_ptr-- = var();
  }
}

void clear_arr_space() {
  while (last_arr_ptr >= var_stack) {
    std::memset(reinterpret_cast<void *>(last_arr_ptr), 0x0, sizeof(var));
    last_arr_ptr--;
  }
}

typedef tl_tree *tl_tree_ptr;
typedef void *void_ptr;

typedef void *(*function_ptr)(void **IP, void **Data, var *arr, tl_tree **vars);
#define TLUNI_NEXT return TRY_CALL(void *, void_ptr, ((*(function_ptr *) IP) (IP + 1, Data, arr, vars)))
#define TLUNI_START(IP, Data, arr, vars) TRY_CALL(void *, void_ptr, ((*(function_ptr *) IP) (IP + 1, Data, arr, vars)))
#define TLUNI_OK ((void *)1l)

static const char *tl_current_function_name;
const char *new_tl_current_function_name;
bool new_tl_mode_error_flag;

tl_tree *store_function(const var &tl_object) {
  tl_debug(__FUNCTION__, -2);
  if (tl_config.fetchIP == nullptr) {
    php_warning("rpc_tl_query not supported due to missing TL scheme");
    return nullptr;
  }
  if (!tl_object.is_array()) {
    php_warning("Not an array passed to function rpc_tl_query");
    return nullptr;
  }

  //fprintf (stderr, "Before STORE\n");
  var f = tl_object.get_value(UNDERSCORE);
  if (f.is_null()) {
    f = tl_object.get_value(0);
  }

  tl_combinator *c;
  if (unlikely (f.is_int())) {
    c = tl_config.id_to_function.get_value(f.to_int());
  } else {
    c = tl_config.name_to_function.get_value(f.to_string());
  }
  if (c == nullptr) {
    php_warning("Function \"%s\" not found in rpc_tl_query", f.to_string().c_str());
    return nullptr;
  }

  tl_current_function_name = c->name.c_str();
//  fprintf (stderr, "Storing type %s\n", c->name.c_str());

  //fprintf (stderr, "Before ALLOCATE in STORE\n");
  new(var_stack) var(tl_object);

  tl_tree **vars = get_var_space(vars_buffer + MAX_VARS, c->var_count);
  tl_tree *res;
  last_arr_ptr = var_stack;
  //fprintf (stderr, "Before TLUNI_START in STORE\n");
  res = (tl_tree *)((*(function_ptr *)c->IP)(c->IP + 1, Data_stack, var_stack, vars));
  if (!CurException.is_null()) {
    res = nullptr;
    CurException = false;
  }
  //fprintf (stderr, "Before FREE in STORE\n");
  free_var_space(vars, c->var_count);
  free_arr_space();
  //fprintf (stderr, "After FREE in STORE\n");

  if (res != nullptr) {
    tl_tree_type *T = (tl_tree_type *)dl::allocate(sizeof(tl_tree_type));
    new(T) tl_tree_type(0, tl_config.ReqResult, array_size(1, 0, true));
    T->children.push_back(res);

    res = T;
  }

  //fprintf (stderr, "After STORE\n");
  return res;
}

array<var> tl_fetch_error(const string &error, int error_code) {
  array<var> result;
  result.set_value(STR_ERROR, error);
  result.set_value(STR_ERROR_CODE, error_code);
  return result;
}

array<var> tl_fetch_error(const char *error, int error_code) {
  return tl_fetch_error(string(error, strlen(error)), error_code);
}

void hexdump(const void *start, const void *end) {
  const char *ptr = (const char *)start;
  char c;
  while (ptr < (char *)end) {
    int s = (const char *)end - ptr, i;
    if (s > 16) {
      s = 16;
    }
    fprintf(stderr, "%08x", (int)(ptr - (char *)start));
    for (i = 0; i < 16; i++) {
      c = ' ';
      if (i == 8) {
        fputc(' ', stderr);
      }
      if (i < s) {
        fprintf(stderr, "%c%02x", c, (unsigned char)ptr[i]);
      } else {
        fprintf(stderr, "%c  ", c);
      }
    }
    c = ' ';
    fprintf(stderr, "%c  ", c);
    for (i = 0; i < s; i++) {
      putc((unsigned char)ptr[i] < ' ' ? '.' : ptr[i], stderr);
    }
    putc('\n', stderr);
    ptr += 16;
  }
}

array<var> fetch_function(tl_tree *T) {
  if (tl_config.fetchIP == nullptr) {
    php_warning("rpc_tl_query_result not supported due to missing TL scheme");
    php_critical_error ("unreachable");
    return tl_fetch_error("TL scheme was not loaded", TL_ERROR_UNKNOWN_FUNCTION_ID);
  }
  //fprintf (stderr, "Before FETCH\n");
  php_assert (T != nullptr);
  new(var_stack) var();

  int x = 0;

  x = rpc_lookup_int();
  if (x == RPC_REQ_ERROR && CurException.is_null()) {
    php_assert (tl_parse_int() == RPC_REQ_ERROR);
    if (CurException.is_null()) {
      tl_parse_long();
      if (CurException.is_null()) {
        int error_code = tl_parse_int();
        if (CurException.is_null()) {
          string error = tl_parse_string();
          if (CurException.is_null()) {
            T->destroy();

            return tl_fetch_error(error, error_code);
          }
        }
      }
    }
  }

  if (!CurException.is_null()) {
    T->destroy();

    array<var> result = tl_fetch_error(CurException->message, TL_ERROR_SYNTAX);
    CurException = false;
    return result;
  }

  tl_debug(__FUNCTION__, -2);

  Data_stack[0] = T;
  string fetched_type = ((tl_tree_type *)T)->type->name;
  void *res;
  last_arr_ptr = var_stack;

  tl_tree *dbg_T = T->dup();

  //fprintf (stderr, "Before TLUNI_START in FETCH\n");
  res = (tl_tree *)((*(function_ptr *)tl_config.fetchIP)(tl_config.fetchIP + 1, Data_stack + 1, var_stack, vars_buffer + MAX_VARS));
  if (!CurException.is_null()) {
    free_arr_space();
    array<var> result = tl_fetch_error(CurException->message, TL_ERROR_SYNTAX);
    var_stack[0] = var();
    CurException = false;
    return result;
  }
  //fprintf (stderr, "After TLUNI_START in FETCH\n");

  if (res == TLUNI_OK) {
    dbg_T->destroy();
    if (!f$fetch_eof()) {
      php_warning("Not all data fetched during fetch type %s", fetched_type.c_str());
      var_stack[0] = var();
      return tl_fetch_error("Not all data fetched", TL_ERROR_EXTRA_DATA);
    }
  } else {
    var_stack[0] = var();
    php_warning("incorrect result from engine during fetching type %s", fetched_type.c_str());
    fprintf(stderr, "================= Bad result from engine start ====================\n");
    dbg_T->print();
    dbg_T->destroy();
    hexdump(rpc_data_begin, rpc_data_begin + (rpc_data_copy.size() + 3) / 4);
    fprintf(stderr, "================= Bad result from engine end ====================\n");

    return tl_fetch_error("Incorrect result", TL_ERROR_SYNTAX);
  }

  var result = var_stack[0];
  var_stack[0] = var();

  if (!result.is_array()) {
    return tl_fetch_error("Result is not an array. How???", TL_ERROR_INTERNAL);
  }
  //fprintf (stderr, "After FETCH\n");
  return result.to_array();
}

static char rpc_tl_results_storage[sizeof(array<tl_tree *>)];
static array<tl_tree *> *rpc_tl_results = reinterpret_cast <array<tl_tree *> *> (rpc_tl_results_storage);
static long long rpc_tl_results_last_query_num = -1;

static char new_mode_rpc_tl_results_storage[sizeof(array<tl_func_base *>)];
static array<tl_func_base *> *new_mode_rpc_tl_results = reinterpret_cast <array<tl_func_base *> *> (new_mode_rpc_tl_results_storage);

enum tl_mode {
  SAFE_NEW_TL_MODE,
  FULL_OLD_TL_MODE,
  FULL_NEW_TL_MODE
};

tl_mode cur_tl_mode = FULL_OLD_TL_MODE;

namespace new_tl_mode {
bool try_fetch_rpc_error(array<var> &out_if_error) {
  int x = rpc_lookup_int();
  if (x == RPC_REQ_ERROR && CurException.is_null()) {
    php_assert (tl_parse_int() == RPC_REQ_ERROR);
    if (CurException.is_null()) {
      tl_parse_long();
      if (CurException.is_null()) {
        int error_code = tl_parse_int();
        if (CurException.is_null()) {
          string error = tl_parse_string();
          if (CurException.is_null()) {
            out_if_error = tl_fetch_error(error, error_code);
            return true;
          }
        }
      }
    }
  }
  if (!CurException.is_null()) {
    out_if_error = tl_fetch_error(CurException->message, TL_ERROR_SYNTAX);
    CurException = false;
    return true;
  }
  return false;
}

std::unique_ptr<tl_func_base> store_function(const var &tl_object) {
  new_tl_current_function_name = "_unknown_";
  if (!tl_object.is_array()) {
    tl_storing_error(tl_object, "Not an array passed to function rpc_tl_query");
    return nullptr;
  }
  string fun_name = tl_arr_get(tl_object, UNDERSCORE, 0).to_string();
  if (!tl_storers_ht.has_key(fun_name)) {
    tl_storing_error(tl_object, "Function \"%s\" not found in tl-scheme", fun_name.c_str());
    return nullptr;
  }
  const auto &storer_kv = tl_storers_ht.get_value(fun_name);
  new_tl_current_function_name = fun_name.c_str();    // актуально только на время процесса store, при fetch уже \0
  std::unique_ptr<tl_func_base> stored_fetcher = storer_kv(tl_object);
  new_tl_current_function_name = nullptr;
  if (new_tl_mode_error_flag) {
    return nullptr;
  }

  return stored_fetcher;
}

array<var> fetch_function(std::unique_ptr<tl_func_base> stored_fetcher) {
  php_assert(stored_fetcher != nullptr);
  array<var> new_tl_object;
  if (try_fetch_rpc_error(new_tl_object)) {
    return new_tl_object;       // тогда содержит ошибку (см. tl_fetch_error())
  }
  new_tl_object = tl_fetch_wrapper(std::move(stored_fetcher));
  if (!CurException.is_null()) {
    array<var> result = tl_fetch_error(CurException->message, TL_ERROR_SYNTAX);
    CurException = false;
    return result;
  }
  if (!f$fetch_eof()) {
    php_warning("Not all data fetched");
    return tl_fetch_error("Not all data fetched", TL_ERROR_EXTRA_DATA);
  }
  return new_tl_object;
}
}

bool f$set_tl_mode(int mode) {
  if (mode < 0 || mode > 2) {
    return false;
  }
  cur_tl_mode = static_cast<tl_mode>(mode);
  return true;
}

int rpc_tl_query_impl(const rpc_connection &c, const var &tl_object, double timeout, bool ignore_answer, bool bytes_estimating, int &bytes_sent, bool flush) {
  f$rpc_clean();
  tl_tree *result_tree = nullptr;
  std::unique_ptr<tl_func_base> stored_fetcher;

  switch (cur_tl_mode) {
    case FULL_NEW_TL_MODE:
      stored_fetcher = new_tl_mode::store_function(tl_object);
      if (stored_fetcher == nullptr) {
        return 0;
      }
      break;

    case SAFE_NEW_TL_MODE:
      result_tree = store_function(tl_object);
      if (result_tree == nullptr) {
        return 0;
      }
      old_data_buf.copy_raw_data(data_buf);
      f$rpc_clean();
      stored_fetcher = new_tl_mode::store_function(tl_object);
      if (stored_fetcher == nullptr) {
        php_warning("NEW_TL_MODE_ERROR: error_flag=1 but old is ok storing %s", tl_current_function_name);
        fprintf(stderr, "--------- NEW_TL_MODE_ERROR: error_flag=1 but old is ok storing %s\n", tl_current_function_name);
        fprintf(stderr, "Input:\n%s\n", dump_tl_array(tl_object).c_str());
        data_buf.copy_raw_data(old_data_buf);
      } else if (data_buf != old_data_buf) {
        php_warning("NEW_TL_MODE_ERROR: Buffers not equal storing %s", tl_current_function_name);
        fprintf(stderr, "--------- NEW_TL_MODE_ERROR: Buffers not equal storing %s\n", tl_current_function_name);
        fprintf(stderr, "Expected:\n"), old_data_buf.debug_print();
        fprintf(stderr, "Actual:\n"), data_buf.debug_print();
        fprintf(stderr, "Input:\n%s\n", dump_tl_array(tl_object).c_str());
        data_buf.copy_raw_data(old_data_buf);
      }
      break;

    case FULL_OLD_TL_MODE:
    default:
      result_tree = store_function(tl_object);
      if (result_tree == nullptr) {
        return 0;
      }
  }

  if (bytes_estimating) {
    bytes_sent += data_buf.size();//estimate
    if (bytes_sent >= (1 << 15) && bytes_sent > (int)data_buf.size()) {
      f$rpc_flush();
      bytes_sent = data_buf.size();
    }
  }
  int query_id = rpc_send(c, timeout, ignore_answer);
  if (query_id <= 0) {
    if (result_tree) {
      result_tree->destroy();
    }
    return 0;
  }
  if (flush) {
    f$rpc_flush();
  }
  if (ignore_answer) {
    if (result_tree) {
      result_tree->destroy();
    }
    return -1;
  }
  if (dl::query_num != rpc_tl_results_last_query_num) {
    new(rpc_tl_results_storage) array<tl_tree *>();
    rpc_tl_results_last_query_num = dl::query_num;
    new(new_mode_rpc_tl_results_storage) array<tl_func_base *>();
  }
  if (result_tree) {
    rpc_tl_results->set_value(query_id, result_tree);
  }
  if (stored_fetcher) {
    new_mode_rpc_tl_results->set_value(query_id, stored_fetcher.release());
  }
  return query_id;
}

int f$rpc_tl_query_one(const rpc_connection &c, const var &tl_object, double timeout) {
  int bytes_sent = 0;
  return rpc_tl_query_impl(c, tl_object, timeout, false, false, bytes_sent, true);
}

int f$rpc_tl_pending_queries_count() {
  if (dl::query_num != rpc_tl_results_last_query_num) {
    return 0;
  }
  return cur_tl_mode == FULL_NEW_TL_MODE ? new_mode_rpc_tl_results->count() : rpc_tl_results->count();
}

bool f$rpc_mc_parse_raw_wildcard_with_flags_to_array(const string &raw_result, array<var> &result) {
  if (raw_result.empty() || !f$rpc_parse(raw_result)) {
    return false;
  };

  int magic = TRY_CALL_ (int, f$fetch_int(), return false);
  if (magic != TL_DICTIONARY) {
    THROW_EXCEPTION(f$Exception$$__construct(rpc_filename, __LINE__, string("Strange dictionary magic", 24), -1));
    return false;
  };

  int cnt = TRY_CALL_ (int, f$fetch_int(), return false);
  if (cnt == 0) {
    return true;
  };
  result.reserve(0, cnt + f$count(result), false);

  for (int j = 0; j < cnt; ++j) {
    string key = f$fetch_string();

    if (!CurException.is_null()) {
      return false;
    }

    var value = f$fetch_memcache_value();

    if (!CurException.is_null()) {
      return false;
    }

    result.set_value(key, value);
  };

  return true;
}

array<int> f$rpc_tl_query(const rpc_connection &c, const array<var> &tl_objects, double timeout, bool ignore_answer) {
  array<int> result(tl_objects.size());
  int bytes_sent = 0;
  for (auto it = tl_objects.begin(); it != tl_objects.end(); ++it) {
    int query_id = rpc_tl_query_impl(c, it.get_value(), timeout, ignore_answer, true, bytes_sent, false);
    result.set_value(it.get_key(), query_id);
  }
  if (bytes_sent > 0) {
    f$rpc_flush();
  }

  return result;
}


class rpc_tl_query_result_one_resumable : public Resumable {
  typedef array<var> ReturnT;

  int query_id;
  tl_tree *T;
  std::unique_ptr<tl_func_base> stored_fetcher;
protected:
  bool run() {
    bool ready;

    RESUMABLE_BEGIN
      last_rpc_error = nullptr;
      ready = f$rpc_get_and_parse(query_id, -1);
      TRY_WAIT(rpc_get_and_parse_resumable_label_0, ready, bool);
      if (!ready) {
        php_assert (last_rpc_error != nullptr);
        T->destroy();
        RETURN(tl_fetch_error(last_rpc_error, TL_ERROR_UNKNOWN));
      }
      array<var> tl_object, new_tl_object;

      switch (cur_tl_mode) {
        case FULL_NEW_TL_MODE:
          new_tl_object = new_tl_mode::fetch_function(std::move(stored_fetcher));
          rpc_parse_restore_previous();
          RETURN(new_tl_object);

        case SAFE_NEW_TL_MODE:
          rpc_parse_save_backup();
          tl_object = fetch_function(T);
          if (tl_object.isset(STR_ERROR)) {
            rpc_parse_restore_previous();
            RETURN(tl_object);
          }
          if (stored_fetcher) {
            rpc_parse_restore_previous();
            rpc_parse_save_backup();
            const char *cur_f_name = stored_fetcher->get_name(); // get_name() возвращает const string literal => не будет висячего указателя
            new_tl_object = new_tl_mode::fetch_function(std::move(stored_fetcher));
            if (!equals(tl_object, new_tl_object)) {
              php_warning("NEW_TL_MODE_ERROR: fetched responses not equal %s", cur_f_name);
              fprintf(stderr, "--------- NEW_TL_MODE_ERROR: fetched responses not equal %s\n", cur_f_name);
              fprintf(stderr, "Expected:\n%s\n", dump_tl_array(tl_object).c_str());
              fprintf(stderr, "Actual:\n%s\n", dump_tl_array(new_tl_object).c_str());
            }
          } else {
            php_warning("NEW_TL_MODE_ERROR: stored_fetcher is null %s", tl_current_function_name);
            fprintf(stderr, "--------- NEW_TL_MODE_ERROR: stored_fetcher is null %s\n", tl_current_function_name);
          }
          rpc_parse_restore_previous();
          RETURN(tl_object);

        case FULL_OLD_TL_MODE:
        default:
          tl_object = fetch_function(T);
          rpc_parse_restore_previous();
          RETURN(tl_object);
      }
    RESUMABLE_END
  }

public:
  rpc_tl_query_result_one_resumable(int query_id, tl_tree *T, std::unique_ptr<tl_func_base> &&stored_fetcher) :
    query_id(query_id),
    T(T),
    stored_fetcher(std::move(stored_fetcher)) {
  }
};


array<var> f$rpc_tl_query_result_one(int query_id) {
  if (query_id <= 0) {
    resumable_finished = true;
    return tl_fetch_error("Wrong query_id", TL_ERROR_WRONG_QUERY_ID);
  }

  if (dl::query_num != rpc_tl_results_last_query_num) {
    resumable_finished = true;
    return tl_fetch_error("There was no TL queries in current script run", TL_ERROR_INTERNAL);
  }
  tl_tree *T = nullptr;
  std::unique_ptr<tl_func_base> stored_fetcher = nullptr;

  switch (cur_tl_mode) {
    case FULL_NEW_TL_MODE:
      stored_fetcher = std::unique_ptr<tl_func_base>(new_mode_rpc_tl_results->get_value(query_id));
      if (stored_fetcher == nullptr) {
        resumable_finished = true;
        return tl_fetch_error("Can't use rpc_tl_query_result for non-TL query", TL_ERROR_INTERNAL);
      }
      new_mode_rpc_tl_results->unset(query_id);
      break;

    case SAFE_NEW_TL_MODE:
      T = rpc_tl_results->get_value(query_id);
      if (T == nullptr) {
        resumable_finished = true;
        return tl_fetch_error("Can't use rpc_tl_query_result for non-TL query", TL_ERROR_INTERNAL);
      }
      rpc_tl_results->unset(query_id);
      // если по-новому не засторилось, stored_fetcher может быть null, об этом будет warning при фетче
      stored_fetcher = std::unique_ptr<tl_func_base>(new_mode_rpc_tl_results->get_value(query_id));
      new_mode_rpc_tl_results->unset(query_id);
      break;

    case FULL_OLD_TL_MODE:
    default:
      T = rpc_tl_results->get_value(query_id);
      if (T == nullptr) {
        resumable_finished = true;
        return tl_fetch_error("Can't use rpc_tl_query_result for non-TL query", TL_ERROR_INTERNAL);
      }
      rpc_tl_results->unset(query_id);
  }

  return start_resumable<array<var>>(new rpc_tl_query_result_one_resumable(query_id, T, std::move(stored_fetcher)));
}


class rpc_tl_query_result_resumable : public Resumable {
  typedef array<array<var>> ReturnT;

  const array<int> query_ids;
  array<array<var>> tl_objects_unsorted;
  int queue_id;
  int query_id;

protected:
  bool run() {
    RESUMABLE_BEGIN
      if (query_ids.count() == 1) {
        query_id = query_ids.begin().get_value();

        tl_objects_unsorted[query_id] = f$rpc_tl_query_result_one(query_id);
        TRY_WAIT(rpc_tl_query_result_resumable_label_0, tl_objects_unsorted[query_id], array<var>);
      } else {
        queue_id = wait_queue_create(query_ids);

        while (true) {
          query_id = f$wait_queue_next(queue_id, -1);
          TRY_WAIT(rpc_tl_query_result_resumable_label_1, query_id, int);
          if (query_id <= 0) {
            break;
          }
          tl_objects_unsorted[query_id] = f$rpc_tl_query_result_one(query_id);
          php_assert (resumable_finished);
        }

        unregister_wait_queue(queue_id);
      }

      array<array<var>> tl_objects(query_ids.size());
      for (array<int>::const_iterator it = query_ids.begin(); it != query_ids.end(); ++it) {
        int query_id = it.get_value();
        if (!tl_objects_unsorted.isset(query_id)) {
          if (query_id <= 0) {
            tl_objects[it.get_key()] = tl_fetch_error((static_SB.clean() << "Very wrong query_id " << query_id).str(), TL_ERROR_WRONG_QUERY_ID);
          } else {
            tl_objects[it.get_key()] = tl_fetch_error((static_SB.clean() << "No answer received or duplicate/wrong query_id "
                                                                         << query_id).str(), TL_ERROR_WRONG_QUERY_ID);
          }
        } else {
          tl_objects[it.get_key()] = tl_objects_unsorted[query_id];
        }
      }

      RETURN(tl_objects);
    RESUMABLE_END
  }

public:
  rpc_tl_query_result_resumable(const array<int> &query_ids) :
    query_ids(query_ids),
    tl_objects_unsorted(array_size(query_ids.count(), 0, false)) {
  }
};

array<array<var>> f$rpc_tl_query_result(const array<int> &query_ids) {
  return start_resumable<array<array<var>>>(new rpc_tl_query_result_resumable(query_ids));
}

array<array<var>> f$rpc_tl_query_result_synchronously(const array<int> &query_ids) {
  array<array<var>> tl_objects_unsorted(array_size(query_ids.count(), 0, false));
  if (query_ids.count() == 1) {
    f$wait_synchronously(query_ids.begin().get_value());
    tl_objects_unsorted[query_ids.begin().get_value()] = f$rpc_tl_query_result_one(query_ids.begin().get_value());
    php_assert (resumable_finished);
  } else {
    int queue_id = wait_queue_create(query_ids);

    while (true) {
      int query_id = f$wait_queue_next_synchronously(queue_id);
      if (query_id <= 0) {
        break;
      }
      tl_objects_unsorted[query_id] = f$rpc_tl_query_result_one(query_id);
      php_assert (resumable_finished);
    }

    unregister_wait_queue(queue_id);
  }

  array<array<var>> tl_objects(query_ids.size());
  for (array<int>::const_iterator it = query_ids.begin(); it != query_ids.end(); ++it) {
    int query_id = it.get_value();
    if (!tl_objects_unsorted.isset(query_id)) {
      if (query_id <= 0) {
        tl_objects[it.get_key()] = tl_fetch_error((static_SB.clean() << "Very wrong query_id " << query_id).str(), TL_ERROR_WRONG_QUERY_ID);
      } else {
        tl_objects[it.get_key()] = tl_fetch_error((static_SB.clean() << "No answer received or duplicate/wrong query_id "
                                                                     << query_id).str(), TL_ERROR_WRONG_QUERY_ID);
      }
    } else {
      tl_objects[it.get_key()] = tl_objects_unsorted[query_id];
    }
  }

  return tl_objects;
}

void *tls_push(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  *(Data++) = (void *)((tl_tree *)(*(IP++)))->dup();
  TLUNI_NEXT;
}

void *tls_arr_pop(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  *arr-- = var();
  last_arr_ptr = arr;
  TLUNI_NEXT;
}

void *tls_arr_push(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  new(++arr) var();
  last_arr_ptr = arr;
  TLUNI_NEXT;
}

void *tls_store_int(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  f$store_int((int)(long)*(IP++));
  TLUNI_NEXT;
}

void *tlcomb_skip_const_int(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  int a = TRY_CALL(int, void_ptr, tl_parse_int());
  if (a != (int)(long)*(IP++)) {
    return nullptr;
  }
  TLUNI_NEXT;
}


/**** Combinator store code {{{ ****/

/****
 *
 * Data [data] => [data] result
 *
 ****/

void *tlcomb_store_any_function(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  var v = arr->get_value(UNDERSCORE);
  if (v.is_null()) {
    v = arr->get_value(0);
    if (v.is_null()) {
      php_warning("Function name not found in unserialize(\"%s\") during store type %s", f$serialize(*arr).c_str(), tl_current_function_name);
      return nullptr;
    }
  }

  const string &name = v.to_string();
  const tl_combinator *c = tl_config.name_to_function.get_value(name);
  if (c == nullptr) {
    php_warning("Function %s not found during store type %s", name.c_str(), tl_current_function_name);
    return nullptr;
  }
  tl_tree **new_vars = get_var_space(vars, c->var_count);

  void *res = TLUNI_START (c->IP, Data, arr, new_vars);
  free_var_space(new_vars, c->var_count);
  if (res == nullptr) {
    return nullptr;
  }
  *(Data++) = res;
  TLUNI_NEXT;
}

/****
 *
 * Data [data] result => [data]
 *
 ****/

void *tlcomb_fetch_type(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  tl_tree_type *e = dynamic_cast <tl_tree_type *> ((tl_tree *)(*(--Data)));
  php_assert (e != nullptr);
  tl_type *t = e->type;
  php_assert (t != nullptr);
  bool is_bare = e->flags & FLAG_BARE;

  int l = -1;
  if (is_bare) {
    if (t->constructors_num == 1) {
      l = 0;
    }
  } else {
    int pos = tl_parse_save_pos();
    l = get_constructor_by_id(t, TRY_CALL(int, void_ptr, tl_parse_int()));
    if (l < 0 && (t->flags & FLAG_DEFAULT_CONSTRUCTOR)) {
      l = t->constructors_num - 1;
      tl_parse_restore_pos(pos);
    }
    if (l < 0) {
      e->destroy();
      return nullptr;
    }
  }
  int r;
  if (l >= 0) {
    r = l + 1;
  } else {
    l = 0;
    r = t->constructors_num;
  }

  int k = tl_parse_save_pos();
  for (int n = l; n < r; n++) {
    if (r - l > 1) {
      *Data = e->dup();
    }
    //((tl_tree *)Data[0])->print();
    tl_combinator *constructor = t->constructors.get_value(n);
    tl_tree **new_vars = get_var_space(vars, constructor->var_count);
    void *res = TLUNI_START (constructor->fetchIP, Data + 1, arr, new_vars);
    free_var_space(new_vars, constructor->var_count);
    if (res == TLUNI_OK) {
      if (!is_bare && (t->constructors_num > 1) && !(t->flags & FLAG_NOCONS)) {
        arr->set_value(UNDERSCORE, constructor->name);
      }
      if (r - l > 1) {
        e->destroy();
      }
      TLUNI_NEXT;
    }
    php_assert (tl_parse_restore_pos(k));
  }
  if (r - l > 1) {
    e->destroy();
  }
  return nullptr;
}

/****
 *
 * Data: [data] result => [data]
 * IP  :
 *
 ****/
void *tlcomb_store_bool(void **IP, void **Data, var *arr, tl_tree **vars);

void *tlcomb_store_type(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  tl_tree_type *e = (tl_tree_type *)(*(--Data));
  php_assert (e != nullptr && e->get_type() == NODE_TYPE_TYPE);
  tl_type *t = e->type;
  php_assert (t != nullptr);
//  fprintf (stderr, "%s\n", t->name.c_str());
  php_assert (t->constructors_num != 0);
  if (t->id == TYPE_ID_BOOL) {
    e->destroy();
    return tlcomb_store_bool(IP, Data, arr, vars);
  }

  int l = -1;
  if (t->constructors_num > 1) {
    var v = arr->get_value(UNDERSCORE);
    if (v.is_null()) {
      v = arr->get_value(0);
    }
    if (!v.is_null()) {
      const string &s = v.to_string();
      l = get_constructor_by_name(t, s);
      if (l < 0) {
        php_warning("Constructor %s not found during store type %s", s.c_str(), tl_current_function_name);
        e->destroy();
        return nullptr;
      }
    }
  } else {
    l = 0;
  }
  int r;
  if (l >= 0) {
    r = l + 1;
  } else {
    php_warning("### DEPRECATED TL FEATURE ###\n"
                "The constructor name must be given if type has several ones. It's going to guess the constructor in runtime!\n"
                "Type: %s\n"
                "Function: %s\n"
                "TL object:\n%s", t->name.c_str(), tl_current_function_name, dump_tl_array(*arr).c_str());
    l = 0;
    r = t->constructors_num;
  }

  int k = tl_parse_save_pos();
  for (int n = l; n < r; n++) {
    tl_combinator *constructor = t->constructors.get_value(n);
    if (!(e->flags & FLAG_BARE) && constructor->name != UNDERSCORE) {
      f$store_int(constructor->id);
    }
    if (r - l > 1) {
      *Data = e->dup();
    }
    tl_tree **new_vars = get_var_space(vars, constructor->var_count);
    void *res = TLUNI_START (constructor->IP, Data + 1, arr, new_vars);
    free_var_space(new_vars, constructor->var_count);
    if (res == TLUNI_OK) {
      if (r - l > 1) {
        e->destroy();
      }
      TLUNI_NEXT;
    }
    php_assert (tl_parse_restore_pos(k));
  }
  if (r - l > 1) {
    e->destroy();
  }
  php_warning("Apropriate constructor doesn't found in unserialize(%s) during store type %s", f$serialize(*arr).c_str(), tl_current_function_name);
  return nullptr;
}

/****
 *
 * IP  : id num
 *
 ***/
void *tlcomb_store_field(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  const string *name = (const string *)(IP++);
  int num = (int)(long)*(IP++);

  var v = arr->get_value(*name);
  if (v.is_null()) {
    v = arr->get_value(num);
    if (v.is_null()) {
      php_warning("Field \"%s\"(%d) not found during store type %s", name->c_str(), num, tl_current_function_name);
      return nullptr;
    }
  }
  new(++arr) var(v);
  last_arr_ptr = arr;
  TLUNI_NEXT;
}


/****
 *
 * IP: id num
 *
 ***/

void *tlcomb_fetch_field_end(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  const string *name = (const string *)(IP++);
  int num = (int)(long)*(IP++);

  if (arr->is_null()) {
    *arr = array<var>();
  }
  if (name->size() != 0) {
    arr[-1].set_value(*name, *arr);
  } else {
    arr[-1].set_value(num, *arr);
  }
  *arr-- = var();
  last_arr_ptr = arr;
  TLUNI_NEXT;
}

/****
 *
 * Data: [data] arity => [data]
 * IP  : newIP
 *
 ***/
void *tlcomb_store_array(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  void **newIP = (void **)*(IP++);
  if (!arr->is_array()) {
    php_warning("Array expected, unserialize (\"%s\") found during store type %s", f$serialize(*arr).c_str(), tl_current_function_name);
    return nullptr;
  }
  tl_tree_nat_const *c = (tl_tree_nat_const *)(*(--Data));
  php_assert (c != nullptr && c->get_type() == NODE_TYPE_NAT_CONST);
  int multiplicity = c->num;
  c->destroy();
  for (int i = 0; i < multiplicity; i++) {
    if (!arr->isset(i)) {
      php_warning("Field %d not found in array during store type %s", i, tl_current_function_name);
      return nullptr;
    }
    var w = arr->get_value(i);
    new(++arr) var(w);
    last_arr_ptr = arr;
    if (TLUNI_START (newIP, Data, arr, vars) != TLUNI_OK) {
      *arr-- = var();
      last_arr_ptr = arr;
      return nullptr;
    }
    *arr-- = var();
    last_arr_ptr = arr;
  }
  TLUNI_NEXT;
}

/****
 *
 * Data: [data] arity => [data]
 * IP  : newIP
 *
 ***/
void *tlcomb_fetch_array(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  void **newIP = (void **)*(IP++);
  tl_tree_nat_const *c = (tl_tree_nat_const *)(*(--Data));
  php_assert (c != nullptr && c->get_type() == NODE_TYPE_NAT_CONST);
  int multiplicity = c->num;
  c->destroy();
  *arr = array<var>();
  for (int i = 0; i < multiplicity; i++) {
    new(++arr) var();
    last_arr_ptr = arr;
    if (TLUNI_START (newIP, Data, arr, vars) != TLUNI_OK) {
      *arr-- = var();
      last_arr_ptr = arr;
      return nullptr;
    }
    arr[-1].push_back(*arr);
    *arr-- = var();
    last_arr_ptr = arr;
  }
  TLUNI_NEXT;
}

void *tlcomb_fetch_int(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  *arr = TRY_CALL(int, void_ptr, (f$fetch_int()));
  TLUNI_NEXT;
}

void *tlcomb_fetch_long(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  *arr = TRY_CALL(var, void_ptr, (f$fetch_long()));
  TLUNI_NEXT;
}

void *tlcomb_fetch_double(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  *arr = TRY_CALL(double, void_ptr, (f$fetch_double()));
  TLUNI_NEXT;
}

void *tlcomb_fetch_string(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  *arr = TRY_CALL(string, void_ptr, (f$fetch_string()));
  TLUNI_NEXT;
}

void *tlcomb_fetch_false(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  *arr = false;
  TLUNI_NEXT;
}

void *tlcomb_fetch_true(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  *arr = true;
  TLUNI_NEXT;
}

void *tlcomb_fetch_unknown_as_array_var(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  *arr = array<var>();
  TLUNI_NEXT;
}

/*****
 *
 * IP: newIP
 *
 *****/
void *tlcomb_fetch_vector(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  int multiplicity = TRY_CALL(int, void_ptr, tl_parse_int());
  void **newIP = (void **)*(IP++);

  if (multiplicity < 0) {
    THROW_EXCEPTION(f$Exception$$__construct(rpc_filename, __LINE__, string("vector size is negative"), -1));
    return nullptr;
  }

  if (multiplicity <= rpc_data_len) {
    *arr = array<var>(array_size(multiplicity, 0, true));
  } else {
    *arr = array<var>();
  }

  for (int i = 0; i < multiplicity; i++) {
    new(++arr) var();
    last_arr_ptr = arr;
    if (TLUNI_START (newIP, Data, arr, vars) != TLUNI_OK) {
      *arr-- = var();
      last_arr_ptr = arr;
      return nullptr;
    }
    arr[-1].push_back(*arr);
    *arr-- = var();
    last_arr_ptr = arr;
  }
  TLUNI_NEXT;
}

/*****
 *
 * IP: newIP
 *
 *****/
void *tlcomb_fetch_dictionary(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  int multiplicity = TRY_CALL(int, void_ptr, tl_parse_int());
  void **newIP = (void **)*(IP++);

  if (multiplicity < 0) {
    THROW_EXCEPTION(f$Exception$$__construct(rpc_filename, __LINE__, string("dictionary size is negative"), -1));
    return nullptr;
  }

  if (multiplicity <= rpc_data_len) {
    *arr = array<var>(array_size(multiplicity, 0, true));
  } else {
    *arr = array<var>();
  }

  for (int i = 0; i < multiplicity; i++) {
    new(++arr) var();
    last_arr_ptr = arr;

    string key = TRY_CALL(string, void_ptr, tl_parse_string());
    if (TLUNI_START (newIP, Data, arr, vars) != TLUNI_OK) {
      *arr-- = var();
      last_arr_ptr = arr;
      return nullptr;
    }
    arr[-1].set_value(key, *arr);
    *arr-- = var();
    last_arr_ptr = arr;
  }
  TLUNI_NEXT;
}

void *tlcomb_fetch_int_key_dictionary(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  int multiplicity = TRY_CALL(int, void_ptr, tl_parse_int());
  void **newIP = (void **)*(IP++);

  if (multiplicity < 0) {
    THROW_EXCEPTION(f$Exception$$__construct(rpc_filename, __LINE__, string("dictionary size is negative"), -1));
    return nullptr;
  }

  if (multiplicity <= rpc_data_len) {
    *arr = array<var>(array_size(multiplicity, 0, true));
  } else {
    *arr = array<var>();
  }

  for (int i = 0; i < multiplicity; i++) {
    new(++arr) var();
    last_arr_ptr = arr;

    int key = TRY_CALL(int, void_ptr, tl_parse_int());
    if (TLUNI_START (newIP, Data, arr, vars) != TLUNI_OK) {
      *arr-- = var();
      last_arr_ptr = arr;
      return nullptr;
    }
    arr[-1].set_value(key, *arr);
    *arr-- = var();
    last_arr_ptr = arr;
  }
  TLUNI_NEXT;
}

void *tlcomb_fetch_long_key_dictionary(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  int multiplicity = TRY_CALL(int, void_ptr, tl_parse_int());
  void **newIP = (void **)*(IP++);

  if (multiplicity < 0) {
    THROW_EXCEPTION(f$Exception$$__construct(rpc_filename, __LINE__, string("dictionary size is negative"), -1));
    return nullptr;
  }

  if (multiplicity <= rpc_data_len) {
    *arr = array<var>(array_size(multiplicity, 0, true));
  } else {
    *arr = array<var>();
  }

  for (int i = 0; i < multiplicity; i++) {
    new(++arr) var();
    last_arr_ptr = arr;

    long long key = TRY_CALL(long long, void_ptr, tl_parse_long());
    if (TLUNI_START (newIP, Data, arr, vars) != TLUNI_OK) {
      *arr-- = var();
      last_arr_ptr = arr;
      return nullptr;
    }
    char buf[30];
    sprintf(buf, "%lld", key);
    arr[-1].set_value(string(buf), *arr);
    *arr-- = var();
    last_arr_ptr = arr;
  }
  TLUNI_NEXT;
}

/*****
 *
 * IP: newIP
 *
 *****/
void *tlcomb_fetch_maybe(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  void **newIP = (void **)*(IP++);
  if (TLUNI_START (newIP, Data, arr, vars) != TLUNI_OK) {
    *arr-- = var();
    last_arr_ptr = arr;
    return nullptr;
  }
  TLUNI_NEXT;
}

/*****
 *
 * IP: var_num
 *
 *****/
void *tlcomb_store_var_num(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  int var_num = (int)(long)*(IP++);
  int num = f$intval(*arr);

  tl_tree_nat_const *T = (tl_tree_nat_const *)dl::allocate(sizeof(tl_tree_nat_const));
  new(T) tl_tree_nat_const(0, num);

  php_assert (vars[var_num] == nullptr);
  vars[var_num] = T;
  f$store_int(num);
  TLUNI_NEXT;
}

/*****
 *
 * IP: var_num
 *
 *****/
void *tlcomb_fetch_var_num(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  int num = TRY_CALL(int, void_ptr, tl_parse_int());
  *arr = num;
  int var_num = (int)(long)*(IP++);

  tl_tree_nat_const *T = (tl_tree_nat_const *)dl::allocate(sizeof(tl_tree_nat_const));
  new(T) tl_tree_nat_const(0, num);

  php_assert (vars[var_num] == nullptr);
  vars[var_num] = T;

  TLUNI_NEXT;
}

/*****
 *
 * IP: var_num
 *
 *****/
void *tlcomb_fetch_check_var_num(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  int num = TRY_CALL(int, void_ptr, tl_parse_int());
  int var_num = (int)(long)*(IP++);
  php_assert (vars[var_num] != nullptr && vars[var_num]->get_type() == NODE_TYPE_NAT_CONST);

  if (num != ((tl_tree_nat_const *)vars[var_num])->num) {
    return nullptr;
  }
  TLUNI_NEXT;
}

/*****
 *
 * IP: var_num flags
 *
 *****/
void *tlcomb_store_var_type(void **IP, void **Data, var *arr, tl_tree **vars) {
  php_assert ("Not supported" && 0);
  tl_debug(__FUNCTION__, -1);
  int var_num = (int)(long)*(IP++);
  int flags = (int)(long)*(IP++);
  string s = f$strval(*arr);
  tl_type *t = tl_config.name_to_type.get_value(s);
  if (t == nullptr) {
    return nullptr;
  }

  tl_tree_type *T = (tl_tree_type *)dl::allocate(sizeof(tl_tree_type));
  new(T) tl_tree_type(flags, t, array_size(0, 0, true));

  php_assert (vars[var_num] == nullptr);
  vars[var_num] = T;
  TLUNI_NEXT;
}

void *tlcomb_store_int(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  f$store_int(f$intval(*arr));
  TLUNI_NEXT;
}

void *tlcomb_store_long(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  f$store_long(*arr);
  TLUNI_NEXT;
}

void *tlcomb_store_double(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  f$store_double(f$floatval(*arr));
  TLUNI_NEXT;
}

void *tlcomb_store_string(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  f$store_string(f$strval(*arr));
  TLUNI_NEXT;
}

void *tlcomb_store_bool(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  if (arr->is_array()) {
    var v = arr->get_value(UNDERSCORE);
    if (v.is_null()) {
      v = arr->get_value(0);
    }
    if (!v.is_null()) {
      const string &s = v.to_string();
      if (s == string("boolFalse", 9)) {
        f$store_int(ID_BOOL_FALSE);
      } else if (s == string("boolTrue", 8)) {
        f$store_int(ID_BOOL_TRUE);
      } else {
        return 0;
      }
      TLUNI_NEXT;
    }
    return 0;
  } else {
    bool a = arr->to_bool();
    f$store_int(a ? ID_BOOL_TRUE : ID_BOOL_FALSE);
    TLUNI_NEXT;
  }
}

/****
 *
 * IP  : newIP
 *
 ***/
void *tlcomb_store_vector(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);

  void **newIP = (void **)*(IP++);

  if (!arr->is_array()) {
    php_warning("Vector expected, unserialize (\"%s\") found during store type %s", f$serialize(*arr).c_str(), tl_current_function_name);
    return nullptr;
  }
  int multiplicity = arr->count();
  f$store_int(multiplicity);

  for (int i = 0; i < multiplicity; i++) {
    if (!arr->isset(i)) {
      php_warning("Field %d not found in vector during store type %s", i, tl_current_function_name);
      return nullptr;
    }
    var w = arr->get_value(i);
    new(++arr) var(w);
    last_arr_ptr = arr;
    if (TLUNI_START (newIP, Data, arr, vars) != TLUNI_OK) {
      *arr-- = var();
      last_arr_ptr = arr;
      return nullptr;
    }
    *arr-- = var();
    last_arr_ptr = arr;
  }
  TLUNI_NEXT;
}

/****
 *
 * IP  : newIP
 *
 ***/
void *tlcomb_store_dictionary(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);

  void **newIP = (void **)*(IP++);

  if (!arr->is_array()) {
    php_warning("Dictionary expected, unserialize (\"%s\") found during store type %s", f$serialize(*arr).c_str(), tl_current_function_name);
    return nullptr;
  }
  int multiplicity = arr->count();
  f$store_int(multiplicity);

  const array<var> a = arr->to_array();
  for (array<var>::const_iterator p = a.begin(); p != a.end(); ++p) {
    f$store_string(f$strval(p.get_key()));

    new(++arr) var(p.get_value());
    last_arr_ptr = arr;
    if (TLUNI_START (newIP, Data, arr, vars) != TLUNI_OK) {
      *arr-- = var();
      last_arr_ptr = arr;
      return nullptr;
    }
    *arr-- = var();
    last_arr_ptr = arr;
  }
  TLUNI_NEXT;
}

void *tlcomb_store_int_key_dictionary(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);

  void **newIP = (void **)*(IP++);

  if (!arr->is_array()) {
    php_warning("Dictionary expected, unserialize (\"%s\") found during store type %s", f$serialize(*arr).c_str(), tl_current_function_name);
    return nullptr;
  }
  int multiplicity = arr->count();
  f$store_int(multiplicity);

  const array<var> a = arr->to_array();
  for (array<var>::const_iterator p = a.begin(); p != a.end(); ++p) {
    f$store_int(f$safe_intval(p.get_key()));    // todo: use f$intval

    new(++arr) var(p.get_value());
    last_arr_ptr = arr;
    if (TLUNI_START (newIP, Data, arr, vars) != TLUNI_OK) {
      *arr-- = var();
      last_arr_ptr = arr;
      return nullptr;
    }
    *arr-- = var();
    last_arr_ptr = arr;
  }
  TLUNI_NEXT;
}

void *tlcomb_store_long_key_dictionary(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);

  void **newIP = (void **)*(IP++);

  if (!arr->is_array()) {
    php_warning("Dictionary expected, unserialize (\"%s\") found during store type %s", f$serialize(*arr).c_str(), tl_current_function_name);
    return nullptr;
  }
  int multiplicity = arr->count();
  f$store_int(multiplicity);

  const array<var> a = arr->to_array();
  for (array<var>::const_iterator p = a.begin(); p != a.end(); ++p) {
    f$store_Long(f$longval(p.get_key()));       // todo: use f$store_long()

    new(++arr) var(p.get_value());
    last_arr_ptr = arr;
    if (TLUNI_START (newIP, Data, arr, vars) != TLUNI_OK) {
      *arr-- = var();
      last_arr_ptr = arr;
      return nullptr;
    }
    *arr-- = var();
    last_arr_ptr = arr;
  }
  TLUNI_NEXT;
}

/*****
 *
 * IP: var_num bit_num shift
 *
 *****/
void *tlcomb_check_bit(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  int var_num = (int)(long)*(IP++);
  int bit_num = (int)(long)*(IP++);
  int shift = (int)(long)*(IP++);

  int num = 0;
  if (vars[var_num] != nullptr) {
    tl_tree_nat_const *c = dynamic_cast <tl_tree_nat_const *> (vars[var_num]);
    php_assert (c != nullptr);

    num = c->num;
  }

//  fprintf (stderr, "Check bit %d of var %d and shift on %d. Var value = %d\n", bit_num, var_num, shift, num);

  if (!(num & (1 << bit_num))) {
    IP += shift;
  }
  TLUNI_NEXT;
}

/*****
 *
 * Data: [Data] res => [Data] childn ... child2 child1
 * IP: type
 *
 *****/
void *tluni_check_type(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  tl_tree_type *res = (tl_tree_type *)(*(--Data));
  php_assert (res->get_type() == NODE_TYPE_TYPE);

  if (res->type != *(IP++)) {
    res->destroy();
    return nullptr;
  }

  for (int i = res->children.count() - 1; i >= 0; i--) {
    *(Data++) = res->children[i];
  }

  res->~tl_tree_type();
  dl::deallocate(res, sizeof(tl_tree_type));
  TLUNI_NEXT;
}

/*****
 *
 * Data: [Data] res => [Data]
 * IP: const
 *
 *****/
void *tluni_check_nat_const(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  tl_tree_nat_const *res = (tl_tree_nat_const *)(*(--Data));
  php_assert (res->get_type() == NODE_TYPE_NAT_CONST);

  if (res->num != (long)*(IP++)) {
    res->destroy();
    return nullptr;
  }

  res->destroy();
  TLUNI_NEXT;
}

/*****
 *
 * Data: [Data] res => [Data] childn ... child2 child1 multiplicity
 * IP: array
 *
 *****/
void *tluni_check_array(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  tl_tree_array *res = (tl_tree_array *)(*(--Data));
  php_assert (res->get_type() == NODE_TYPE_ARRAY);

  if (!res->equals(static_cast <const tl_tree *> (*(IP++)))) {
    res->destroy();
    return nullptr;
  }
  for (int i = res->args.count() - 1; i >= 0; i--) {
    *(Data++) = &res->args[i];
  }
  *(Data++) = res->multiplicity;

  void *result = TLUNI_START(IP, Data, arr, vars);
  res->~tl_tree_array();
  dl::deallocate(res, sizeof(tl_tree_array));
  return result;
}

/*****
 *
 * Data [Data] child => [Data] type
 * IP arg_name
 *
 *****/
void *tluni_check_arg(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  arg *res = (arg *)(*(--Data));
  const string *name = (const string *)(IP++);
  php_assert (name != nullptr);

  if (strcmp(name->c_str(), res->name.c_str())) {
    res->type->destroy();
    return nullptr;
  }
  *(Data++) = res->type;
  TLUNI_NEXT;
}

/*****
 *
 * Data [Data] value => [Data]
 * IP var_num add_value
 *
 *****/
void *tluni_set_nat_var(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  int var_num = (int)(long)*(IP++);

  tl_tree_nat_const *c = (tl_tree_nat_const *)(*(--Data));
  php_assert (c->get_type() == NODE_TYPE_NAT_CONST);
  c->num += (int)(long)*(IP++);

  if (c->num < 0) {
    c->destroy();
    return nullptr;
  }

  php_assert (vars[var_num] == nullptr);
  vars[var_num] = c;
  TLUNI_NEXT;
}

/*****
 *
 * Data [Data] value => [Data]
 * IP var_num
 *
 *****/
void *tluni_set_type_var(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  int var_num = (int)(long)*(IP++);

  php_assert (vars[var_num] == nullptr);
  vars[var_num] = (tl_tree *)(*(--Data));
  TLUNI_NEXT;
}


/*****
 *
 * Data [Data] value => [Data]
 * IP var_num add_value
 *
 *****/
void *tluni_check_nat_var(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  int var_num = (int)(long)*(IP++);
  php_assert (vars[var_num] != nullptr);
  if (vars[var_num]->get_type() != NODE_TYPE_NAT_CONST) {
    return nullptr;
  }

  tl_tree_nat_const *v = (tl_tree_nat_const *)(vars[var_num]);
  tl_tree_nat_const *c = (tl_tree_nat_const *)(*(--Data));
  if (c->get_type() != NODE_TYPE_NAT_CONST || v->num != c->num + (int)(long)*(IP++)) {
    c->destroy();
    return nullptr;
  }
  c->destroy();
  TLUNI_NEXT;
}


/*****
 *
 * Data [Data] value => [Data]
 * IP var_num
 *
 *****/
void *tluni_check_type_var(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  tl_tree *x = (tl_tree *)(*(--Data));
  tl_tree *y = vars[(long)*(IP++)];
  php_assert (y != nullptr);
  if (!y->equals(x)) {
    x->destroy();
    return nullptr;
  }
  x->destroy();
  TLUNI_NEXT;
}


/*****
 *
 * Data [Data] multiplicity type_1 ... type_n => [Data] array
 * IP flags args_num name_n ... name_1
 *
 *****/
void *tlsub_create_array(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  int flags = (int)(long)*(IP++);
  int args_num = (int)(long)*(IP++);

  tl_tree_array *T = (tl_tree_array *)dl::allocate(sizeof(tl_tree_array));
  new(T) tl_tree_array(flags, nullptr, array_size(args_num, 0, true));

  for (int i = 0; i < args_num; i++) {
    T->args[i];//allocate vector
  }
  for (int i = args_num - 1; i >= 0; i--) {
    const string *name = (const string *)(IP++);
    T->args[i].name = *name;
    T->args[i].type = (tl_tree *)*(--Data);
  }
  T->multiplicity = (tl_tree *)*(--Data);
  *(Data++) = (void *)T;
  TLUNI_NEXT;
}


/*****
 *
 * Data [Data] type1 ... typen  => [Data] type
 * IP flags type_ptr
 *
 *****/
void *tlsub_create_type(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);

  int flags = (int)(long)*(IP++);
  tl_type *type = (tl_type *)*(IP++);
  int children_num = type->arity;

  tl_tree_type *T = (tl_tree_type *)dl::allocate(sizeof(tl_tree_type));
  new(T) tl_tree_type(flags, type, array_size(children_num, 0, true));

  for (int i = 0; i < children_num; i++) {
    T->children[i] = nullptr;//allocate vector
  }
  for (int i = children_num - 1; i >= 0; i--) {
    T->children[i] = (tl_tree *)*(--Data);
  }
  *(Data++) = T;
  TLUNI_NEXT;
}

void *tlsub_ret_ok(void **IP __attribute__((unused)), void **Data __attribute__((unused)), var *arr __attribute__((unused)), tl_tree **vars __attribute__((unused))) {
  tl_debug(__FUNCTION__, -1);
  return TLUNI_OK;
}

void *tlsub_ret(void **IP __attribute__((unused)), void **Data, var *arr __attribute__((unused)), tl_tree **vars __attribute__((unused))) {
  tl_debug(__FUNCTION__, -1);
  return *(--Data);
}

void *tlsub_push_type_var(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  *(Data++) = vars[(long)*(IP++)]->dup();
  TLUNI_NEXT;
}

void *tlsub_push_nat_var(void **IP, void **Data, var *arr, tl_tree **vars) {
  tl_debug(__FUNCTION__, -1);
  int var_num = (int)(long)*(IP++);
  php_assert (vars[var_num] != nullptr && vars[var_num]->get_type() == NODE_TYPE_NAT_CONST);
  int num = ((tl_tree_nat_const *)vars[var_num])->num + (int)(long)*(IP++);

  tl_tree_nat_const *T = (tl_tree_nat_const *)dl::allocate(sizeof(tl_tree_nat_const));
  new(T) tl_tree_nat_const(vars[var_num]->flags, num);

  *(Data++) = T;
  TLUNI_NEXT;
}

#undef TLUNI_NEXT
#undef TLUNI_START
#undef TLUNI_OK


void **IP_dup(void **IP, int len) {
  php_assert (!dl::query_num && len > 0);
  void **IP_res = (void **)dl::allocate((dl::size_type)sizeof(void *) * len);
  memcpy(IP_res, IP, sizeof(void *) * len);
  return IP_res;
}


int gen_uni(tl_tree *t, void **IP, int max_size, int *vars_int) {
  php_assert (max_size > 10);
  php_assert (t != nullptr);
  int l = 0;
  switch (t->get_type()) {
    case NODE_TYPE_TYPE: {
      tl_tree_type *t1 = dynamic_cast <tl_tree_type *> (t);
      php_assert (t1 != nullptr);
      IP[l++] = (void *)tluni_check_type;
      IP[l++] = (void *)t1->type;
      for (int i = 0; i < t1->children.count(); i++) {
        l += gen_uni(t1->children.get_value(i), IP + l, max_size - l, vars_int);
      }
      return l;
    }
    case NODE_TYPE_NAT_CONST: {
      tl_tree_nat_const *t1 = dynamic_cast <tl_tree_nat_const *> (t);
      php_assert (t1 != nullptr);
      IP[l++] = (void *)tluni_check_nat_const;
      IP[l++] = (void *)(long)t1->num;
      return l;
    }
    case NODE_TYPE_ARRAY: {
      tl_tree_array *t1 = dynamic_cast <tl_tree_array *> (t);
      php_assert (t1 != nullptr);
      IP[l++] = (void *)tluni_check_array;
      IP[l++] = (void *)t;
      l += gen_uni(t1->multiplicity, IP + l, max_size - l, vars_int);
      for (int i = 0; i < t1->args.count(); i++) {
        IP[l++] = (void *)tluni_check_arg;
        IP[l++] = *(void **)&t1->args[i].name;
        l += gen_uni(t1->args[i].type, IP + l, max_size - l, vars_int);
      }
      return l;
    }
    case NODE_TYPE_VAR_TYPE: {
      tl_tree_var_type *t1 = dynamic_cast <tl_tree_var_type *> (t);
      php_assert (t1 != nullptr);

      int var_num = t1->var_num;
      if (!vars_int[var_num]) {
        IP[l++] = (void *)tluni_set_type_var;
        IP[l++] = (void *)(long)var_num;
        vars_int[var_num] = 1;
      } else if (vars_int[var_num] == 1) {
        IP[l++] = (void *)tluni_check_type_var;
        IP[l++] = (void *)(long)var_num;
      } else {
        php_assert (0);
      }
      return l;
    }
    case NODE_TYPE_VAR_NUM: {
      tl_tree_var_num *t1 = dynamic_cast <tl_tree_var_num *> (t);
      php_assert (t1 != nullptr);

      int var_num = t1->var_num;
      if (!vars_int[var_num]) {
        IP[l++] = (void *)tluni_set_nat_var;
        IP[l++] = (void *)(long)var_num;
        IP[l++] = (void *)(long)t1->diff;
        vars_int[var_num] = 2;
      } else if (vars_int[var_num] == 2) {
        IP[l++] = (void *)tluni_check_nat_var;
        IP[l++] = (void *)(long)var_num;
        IP[l++] = (void *)(long)t1->diff;
      } else {
        php_assert (0);
      }
      return l;
    }
    default:
      php_assert (0);
  }
  return -1;
}

int gen_create(tl_tree *t, void **IP, int max_size, int *vars_int) {
  php_assert (max_size > 10);
  int l = 0;
  if (t->flags & FLAG_NOVAR) {
    IP[l++] = (void *)tls_push;
    IP[l++] = (void *)t;
    return l;
  }
  switch (t->get_type()) {
    case NODE_TYPE_TYPE: {
      tl_tree_type *t1 = (tl_tree_type *)t;
      for (int i = 0; i < t1->children.count(); i++) {
        l += gen_create(t1->children[i], IP + l, max_size - l, vars_int);
      }
      php_assert (max_size > l + 10);
      IP[l++] = (void *)tlsub_create_type;
      IP[l++] = (void *)(long)(t1->flags & FLAGS_MASK);
      IP[l++] = (void *)t1->type;
      return l;
    }
    case NODE_TYPE_ARRAY: {
      tl_tree_array *t1 = (tl_tree_array *)t;
      php_assert (t1->multiplicity != nullptr);
      l += gen_create(t1->multiplicity, IP + l, max_size - l, vars_int);

      for (int i = 0; i < t1->args.count(); i++) {
        l += gen_create(t1->args[i].type, IP + l, max_size - l, vars_int);
      }
      php_assert (max_size > l + 10 + t1->args.count());

      IP[l++] = (void *)tlsub_create_array;
      IP[l++] = (void *)(long)(t1->flags & FLAGS_MASK);
      IP[l++] = (void *)(long)t1->args.count();
      for (int i = t1->args.count() - 1; i >= 0; i--) {
        IP[l++] = *(void **)&t1->args[i].name;
      }
      return l;
    }
    case NODE_TYPE_VAR_TYPE: {
      tl_tree_var_type *t1 = (tl_tree_var_type *)t;
      IP[l++] = (void *)tlsub_push_type_var;
      IP[l++] = (void *)(long)t1->var_num;
      return l;
    }
    case NODE_TYPE_VAR_NUM: {
      tl_tree_var_num *t1 = (tl_tree_var_num *)t;
      IP[l++] = (void *)tlsub_push_nat_var;
      IP[l++] = (void *)(long)t1->var_num;
      IP[l++] = (void *)(long)t1->diff;
      return l;
    }
    default:
      php_assert (false);
      return -1;
  }
}

int gen_field(const arg &arg, void **IP, int max_size, int *vars_int, int num, bool flat);

int gen_array_store(tl_tree_array *a, void **IP, int max_size, int *vars_int) {
  php_assert (max_size > 10);
  int l = 0;
  for (int i = 0; i < a->args.count(); i++) {
    l += gen_field(a->args[i], IP + l, max_size - l, vars_int, i, a->args.count() == 1);
  }
  php_assert (max_size > l + 1);
  IP[l++] = (void *)tlsub_ret_ok;
  return l;
}

int gen_field_fetch(const arg &arg, void **IP, int max_size, int *vars_int, int num, bool flat);

int gen_array_fetch(tl_tree_array *a, void **IP, int max_size, int *vars_int) {
  php_assert (max_size > 10);
  int l = 0;
  int args_num = a->args.count();
  for (int i = 0; i < args_num; i++) {
    l += gen_field_fetch(a->args[i], IP + l, max_size - l, vars_int, i, args_num == 1);
  }
  php_assert (max_size > l + 1);
  IP[l++] = (void *)tlsub_ret_ok;
  return l;
}

int gen_constructor_store(tl_combinator &c, void **IP, int max_size);
int gen_constructor_fetch(tl_combinator &c, void **IP, int max_size);

int gen_field(const arg &arg, void **IP, int max_size, int *vars_int, int num, bool flat) {
  php_assert (max_size > 10);
  int l = 0;
  if (arg.exist_var_num >= 0) {
    IP[l++] = (void *)tlcomb_check_bit;
    IP[l++] = (void *)(long)arg.exist_var_num;
    IP[l++] = (void *)(long)arg.exist_var_bit;
    IP[l++] = nullptr;//temporary
  }
  if (!flat) {
    IP[l++] = (void *)tlcomb_store_field;
    IP[l++] = *(void **)&arg.name;
    IP[l++] = (void *)(long)num;
  }
  if (arg.var_num >= 0) {
    php_assert (arg.type->get_type() == NODE_TYPE_TYPE);
    tl_tree_type *arg_type = (tl_tree_type *)arg.type;
    int id = arg_type->type->id;
    if (id == ID_VAR_TYPE) {
      php_assert ("Not supported" && 0);
      IP[l++] = (void *)tlcomb_store_var_type;
      IP[l++] = (void *)(long)arg.var_num;
      IP[l++] = (void *)(long)(arg_type->flags & FLAGS_MASK);
    } else {
      php_assert (id == ID_VAR_NUM);
      IP[l++] = (void *)tlcomb_store_var_num;
      IP[l++] = (void *)(long)arg.var_num;
    }
  } else {
    int type = arg.type->get_type();
    if ((type == NODE_TYPE_TYPE || type == NODE_TYPE_VAR_TYPE)) {
      tl_tree_type *t1 = dynamic_cast <tl_tree_type *> (arg.type);

      if (0 && t1 != nullptr && t1->type->arity == 0 && t1->type->constructors_num == 1) {
        tl_combinator *constructor = t1->type->constructors[0];
        if (!(t1->flags & FLAG_BARE)) {
          IP[l++] = (void *)tls_store_int;
          IP[l++] = (void *)(long)constructor->id;
        }
        if (!constructor->IP_len) {
          php_assert (gen_constructor_store(*constructor, IP + l, max_size - l) > 0);
        }
        void **IP_ = constructor->IP;
        php_assert (constructor->IP_len >= 2);
        php_assert (IP_[0] == (void *)tluni_check_type);
        php_assert (IP_[1] == (void *)t1->type);
        php_assert (max_size >= l + constructor->IP_len + 10);
        memcpy(IP + l, IP_ + 2, sizeof(void *) * (constructor->IP_len - 2));

        l += constructor->IP_len - 2;
        php_assert (IP[l - 1] == (void *)tlsub_ret_ok);
        l--;
      } else {
        l += gen_create(arg.type, IP + l, max_size - l, vars_int);
        php_assert (max_size > 10 + l);
        IP[l++] = (void *)tlcomb_store_type;
      }
    } else {
      php_assert (type == NODE_TYPE_ARRAY);
      l += gen_create(((tl_tree_array *)arg.type)->multiplicity, IP + l, max_size - l, vars_int);
      php_assert (max_size > l + 10);
      IP[l++] = (void *)tlcomb_store_array;
      void *newIP[1000];
      IP[l++] = (void *)IP_dup(newIP, gen_array_store(((tl_tree_array *)arg.type), newIP, 1000, vars_int));
    }
  }
  php_assert (max_size > l + 10);
  if (!flat) {
    IP[l++] = (void *)tls_arr_pop;
  }
  if (arg.exist_var_num >= 0) {
    IP[3] = (void *)(long)(l - 4);
  }
  return l;
}

int gen_field_fetch(const arg &arg, void **IP, int max_size, int *vars_int, int num, bool flat) {
  php_assert (max_size > 30);
  int l = 0;
  if (arg.exist_var_num >= 0) {
    IP[l++] = (void *)tlcomb_check_bit;
    IP[l++] = (void *)(long)arg.exist_var_num;
    IP[l++] = (void *)(long)arg.exist_var_bit;
    IP[l++] = nullptr;//temporary
  }
  if (!flat) {
    IP[l++] = (void *)tls_arr_push;
  }
  if (arg.var_num >= 0) {
    php_assert (arg.type->get_type() == NODE_TYPE_TYPE);
    int t = ((tl_tree_type *)arg.type)->type->id;
    if (t == ID_VAR_TYPE) {
      php_assert ("Not supported yet\n" && 0);
    } else {
      php_assert (t == ID_VAR_NUM);
      if (vars_int[arg.var_num] == 0) {
        IP[l++] = (void *)tlcomb_fetch_var_num;
        IP[l++] = (void *)(long)arg.var_num;
        vars_int[arg.var_num] = 2;
      } else if (vars_int[arg.var_num] == 2) {
        IP[l++] = (void *)tlcomb_fetch_check_var_num;
        IP[l++] = (void *)(long)arg.var_num;
      } else {
        php_assert (0);
        return -1;
      }
    }
  } else {
    int t = arg.type->get_type();
    if (t == NODE_TYPE_TYPE || t == NODE_TYPE_VAR_TYPE) {
      tl_tree_type *t1 = dynamic_cast <tl_tree_type *> (arg.type);

      if (0 && t1 != nullptr && t1->type->arity == 0 && t1->type->constructors_num == 1) {
        tl_combinator *constructor = t1->type->constructors[0];
        if (!(t1->flags & FLAG_BARE)) {
          IP[l++] = (void *)tlcomb_skip_const_int;
          IP[l++] = (void *)(long)constructor->id;
        }
        if (!constructor->fetchIP_len) {
          php_assert (gen_constructor_fetch(*constructor, IP + l, max_size - l) > 0);
        }
        void **IP_ = constructor->fetchIP;
        php_assert (constructor->fetchIP_len >= 2);
        php_assert (IP_[0] == (void *)tluni_check_type);
        php_assert (IP_[1] == (void *)t1->type);
        php_assert (max_size >= l + constructor->fetchIP_len + 10);
        memcpy(IP + l, IP_ + 2, sizeof(void *) * (constructor->fetchIP_len - 2));

        l += constructor->fetchIP_len - 2;
        php_assert (IP[l - 1] == (void *)tlsub_ret_ok);
        l--;
      } else {
        l += gen_create(arg.type, IP + l, max_size - l, vars_int);
        php_assert (max_size > l + 10);
        IP[l++] = (void *)tlcomb_fetch_type;
      }
    } else {
      php_assert (t == NODE_TYPE_ARRAY);
      l += gen_create(((tl_tree_array *)arg.type)->multiplicity, IP + l, max_size - l, vars_int);
      php_assert (max_size > l + 10);
      IP[l++] = (void *)tlcomb_fetch_array;
      void *newIP[1000];
      IP[l++] = IP_dup(newIP, gen_array_fetch(((tl_tree_array *)arg.type), newIP, 1000, vars_int));
    }
  }
  php_assert (max_size > l + 10);
  if (!flat) {
    IP[l++] = (void *)tlcomb_fetch_field_end;
    IP[l++] = *(void **)&arg.name;
    IP[l++] = (void *)(long)num;
  }
  if (arg.exist_var_num >= 0) {
    IP[3] = (void *)(long)(l - 4);
  }
  return l;
}

int gen_field_excl(const arg &arg, void **IP, int max_size, int *vars_int, int num) {
  php_assert (max_size > 10);
  int l = 0;
  IP[l++] = (void *)tlcomb_store_field;
  IP[l++] = *(void **)&arg.name;
  IP[l++] = (void *)(long)num;

  php_assert (arg.var_num < 0);
  int t = arg.type->get_type();
  php_assert (t == NODE_TYPE_TYPE || t == NODE_TYPE_VAR_TYPE);
  IP[l++] = (void *)tlcomb_store_any_function;
  l += gen_uni(arg.type, IP + l, max_size - l, vars_int);
  php_assert (max_size > 1 + l);

  IP[l++] = (void *)tls_arr_pop;
  return l;
}

int gen_constructor_store(tl_combinator &c, void **IP, int max_size) {
  if (c.IP != nullptr) {
    return c.IP_len;
  }
  php_assert (max_size > 10);

  int vars_int[c.var_count];
  memset(vars_int, 0, sizeof(int) * c.var_count);
  int l = gen_uni(c.result, IP, max_size, vars_int);

  switch (c.id) {
    case ID_INT: {
      IP[l++] = (void *)tlcomb_store_int;
      break;
    }
    case ID_LONG: {
      IP[l++] = (void *)tlcomb_store_long;
      break;
    }
    case ID_STRING: {
      IP[l++] = (void *)tlcomb_store_string;
      break;
    }
    case ID_DOUBLE: {
      IP[l++] = (void *)tlcomb_store_double;
      break;
    }
    case ID_VECTOR: {
      IP[l++] = (void *)tlcomb_store_vector;
      void *tIP[4];
      tIP[0] = (void *)tlsub_push_type_var;
      tIP[1] = (void *)(long)0;
      tIP[2] = (void *)tlcomb_store_type;
      tIP[3] = (void *)tlsub_ret_ok;
      IP[l++] = (void *)IP_dup(tIP, 4);
      break;
    }
    case ID_DICTIONARY: {
      IP[l++] = (void *)tlcomb_store_dictionary;
      void *tIP[4];
      tIP[0] = (void *)tlsub_push_type_var;
      tIP[1] = (void *)(long)0;
      tIP[2] = (void *)tlcomb_store_type;
      tIP[3] = (void *)tlsub_ret_ok;
      IP[l++] = (void *)IP_dup(tIP, 4);
      break;
    }
    case ID_INT_KEY_DICTIONARY: {
      IP[l++] = (void *)tlcomb_store_int_key_dictionary;
      void *tIP[4];
      tIP[0] = (void *)tlsub_push_type_var;
      tIP[1] = (void *)(long)0;
      tIP[2] = (void *)tlcomb_store_type;
      tIP[3] = (void *)tlsub_ret_ok;
      IP[l++] = (void *)IP_dup(tIP, 4);
      break;
    }
    case ID_LONG_KEY_DICTIONARY: {
      IP[l++] = (void *)tlcomb_store_long_key_dictionary;
      void *tIP[4];
      tIP[0] = (void *)tlsub_push_type_var;
      tIP[1] = (void *)(long)0;
      tIP[2] = (void *)tlcomb_store_type;
      tIP[3] = (void *)tlsub_ret_ok;
      IP[l++] = (void *)IP_dup(tIP, 4);
      break;
    }
    default: {
      int z = 0;
      if (c.result->get_type() == NODE_TYPE_TYPE) {
        tl_type *t = ((tl_tree_type *)c.result)->type;
        if (t->constructors_num == 1) {
          for (int i = 0; i < c.args.count(); i++) {
            if (!(c.args[i].flags & FLAG_OPT_VAR)) {
              z++;
            }
          }
        }
      }
      for (int i = 0; i < c.args.count(); i++) {
        if (!(c.args[i].flags & FLAG_OPT_VAR)) {
          l += gen_field(c.args[i], IP + l, max_size - l, vars_int, i + 1, z == 1);
        }
      }
      php_assert (max_size > 10);
    }
  }

  IP[l++] = (void *)tlsub_ret_ok;
  c.IP = IP_dup(IP, l);
  c.IP_len = l;
  return l;
}

int gen_function_store(tl_combinator &c, void **IP, int max_size) {
  php_assert (max_size > 10);
  php_assert (c.IP == nullptr);
  int l = 0;
  IP[l++] = (void *)tls_store_int;
  IP[l++] = (void *)(long)c.id;

  int vars_int[c.var_count];
  memset(vars_int, 0, sizeof(int) * c.var_count);
  for (int i = 0; i < c.args.count(); i++) {
    if (!(c.args[i].flags & FLAG_OPT_VAR)) {
      if (c.args[i].flags & FLAG_EXCL) {
        l += gen_field_excl(c.args[i], IP + l, max_size - l, vars_int, i + 1);
      } else {
        l += gen_field(c.args[i], IP + l, max_size - l, vars_int, i + 1, false);
      }
    }
  }
  l += gen_create(c.result, IP + l, max_size - l, vars_int);
  php_assert (max_size > 1 + l);
  IP[l++] = (void *)tlsub_ret;
  c.IP = IP_dup(IP, l);
  c.IP_len = l;
  return l;
}

int gen_constructor_fetch(tl_combinator &c, void **IP, int max_size) {
  if (c.fetchIP) {
    return c.fetchIP_len;
  }
  php_assert (max_size > 10);

  int vars_int[c.var_count];
  memset(vars_int, 0, sizeof(int) * c.var_count);
  int l = gen_uni(c.result, IP, max_size, vars_int);

  switch (c.id) {
    case ID_INT: {
      IP[l++] = (void *)tlcomb_fetch_int;
      break;
    }
    case ID_LONG: {
      IP[l++] = (void *)tlcomb_fetch_long;
      break;
    }
    case ID_STRING: {
      IP[l++] = (void *)tlcomb_fetch_string;
      break;
    }
    case ID_DOUBLE: {
      IP[l++] = (void *)tlcomb_fetch_double;
      break;
    }
    case ID_VECTOR: {
      IP[l++] = (void *)tlcomb_fetch_vector;
      void *tIP[4];
      tIP[0] = (void *)tlsub_push_type_var;
      tIP[1] = (void *)(long)0;
      tIP[2] = (void *)tlcomb_fetch_type;
      tIP[3] = (void *)tlsub_ret_ok;
      IP[l++] = (void *)IP_dup(tIP, 4);
      break;
    }
    case ID_DICTIONARY: {
      IP[l++] = (void *)tlcomb_fetch_dictionary;
      void *tIP[4];
      tIP[0] = (void *)tlsub_push_type_var;
      tIP[1] = (void *)(long)0;
      tIP[2] = (void *)tlcomb_fetch_type;
      tIP[3] = (void *)tlsub_ret_ok;
      IP[l++] = (void *)IP_dup(tIP, 4);
      break;
    }
    case ID_INT_KEY_DICTIONARY: {
      IP[l++] = (void *)tlcomb_fetch_int_key_dictionary;
      void *tIP[4];
      tIP[0] = (void *)tlsub_push_type_var;
      tIP[1] = (void *)(long)0;
      tIP[2] = (void *)tlcomb_fetch_type;
      tIP[3] = (void *)tlsub_ret_ok;
      IP[l++] = (void *)IP_dup(tIP, 4);
      break;
    }
    case ID_LONG_KEY_DICTIONARY: {
      IP[l++] = (void *)tlcomb_fetch_long_key_dictionary;
      void *tIP[4];
      tIP[0] = (void *)tlsub_push_type_var;
      tIP[1] = (void *)(long)0;
      tIP[2] = (void *)tlcomb_fetch_type;
      tIP[3] = (void *)tlsub_ret_ok;
      IP[l++] = (void *)IP_dup(tIP, 4);
      break;
    }
    case ID_MAYBE_TRUE: {
      IP[l++] = (void *)tlcomb_fetch_maybe;
      void *tIP[4];
      tIP[0] = (void *)tlsub_push_type_var;
      tIP[1] = (void *)(long)0;
      tIP[2] = (void *)tlcomb_fetch_type;
      tIP[3] = (void *)tlsub_ret_ok;
      IP[l++] = (void *)IP_dup(tIP, 4);
      break;
    }
    case ID_MAYBE_FALSE:
    case ID_BOOL_FALSE: {
      IP[l++] = (void *)tlcomb_fetch_false;
      break;
    }
    case ID_BOOL_TRUE: {
      IP[l++] = (void *)tlcomb_fetch_true;
      break;
    }
    default: {
      IP[l++] = (void *)tlcomb_fetch_unknown_as_array_var;
      int z = 0;
      if (c.result->get_type() == NODE_TYPE_TYPE) {
        tl_type *t = ((tl_tree_type *)c.result)->type;
        if (t->constructors_num == 1) {
          for (int i = 0; i < c.args.count(); i++) {
            if (!(c.args[i].flags & FLAG_OPT_VAR)) {
              z++;
            }
          }
        }
      }
      for (int i = 0; i < c.args.count(); i++) {
        if (!(c.args[i].flags & FLAG_OPT_VAR)) {
          l += gen_field_fetch(c.args[i], IP + l, max_size - l, vars_int, i + 1, z == 1);
        }
      }
      php_assert (max_size > 10 + l);
    }
  }

  IP[l++] = (void *)tlsub_ret_ok;
  c.fetchIP = IP_dup(IP, l);
  c.fetchIP_len = l;
  return l;
}

void gen_function_fetch(void **&IP_res, void **IP, int l) {
  IP[0] = (void *)tlcomb_fetch_type;
  IP[1] = (void *)tlsub_ret_ok;
  IP_res = IP_dup(IP, l);
}


static int tl_schema_version = -1;

tl_tree *read_expr(int *var_count);
tl_tree *read_nat_expr(int *var_count);
array<arg> read_args_list(int *var_count);

tl_tree *read_num_const(int *var_count __attribute__((unused))) {
  int num = TRY_CALL_EXIT(int, "Wrong TL-scheme specified.", tl_parse_int());

  tl_tree_nat_const *T = (tl_tree_nat_const *)dl::allocate(sizeof(tl_tree_nat_const));
  new(T) tl_tree_nat_const(FLAG_NOVAR, num);
  return T;
}

tl_tree *read_num_var(int *var_count) {
  int diff = TRY_CALL_EXIT(int, "Wrong TL-scheme specified.", tl_parse_int());
  int var_num = TRY_CALL_EXIT(int, "Wrong TL-scheme specified.", tl_parse_int());

  if (var_num >= *var_count) {
    *var_count = var_num + 1;
  }

  tl_tree_var_num *T = (tl_tree_var_num *)dl::allocate(sizeof(tl_tree_var_num));
  new(T) tl_tree_var_num(0, var_num, diff);
  return T;
}

tl_tree *read_type_var(int *var_count) {
  int var_num = TRY_CALL_EXIT(int, "Wrong TL-scheme specified.", tl_parse_int());
  int flags = TRY_CALL_EXIT(int, "Wrong TL-scheme specified.", tl_parse_int());

  if (var_num >= *var_count) {
    *var_count = var_num + 1;
  }
  php_assert (!(flags & (FLAG_NOVAR | FLAG_BARE)));

  tl_tree_var_type *T = (tl_tree_var_type *)dl::allocate(sizeof(tl_tree_var_type));
  new(T) tl_tree_var_type(flags, var_num);
  return T;
}

tl_tree *read_array(int *var_count) {
  int flags = FLAG_NOVAR;
  tl_tree *multiplicity = read_nat_expr(var_count);

  tl_tree_array *T = (tl_tree_array *)dl::allocate(sizeof(tl_tree_array));
  new(T) tl_tree_array(flags, multiplicity, read_args_list(var_count));

  for (int i = 0; i < T->args.count(); i++) {
    if (!(T->args.get_value(i).flags & FLAG_NOVAR)) {
      T->flags &= ~FLAG_NOVAR;
    }
  }
  return T;
}

tl_tree *read_type(int *var_count) {
  tl_type *type = tl_config.id_to_type.get_value(TRY_CALL_EXIT(int, "Wrong TL-scheme specified.", tl_parse_int()));
  php_assert (type != nullptr);
  int flags = TRY_CALL_EXIT(int, "Wrong TL-scheme specified.", tl_parse_int()) | FLAG_NOVAR;
  php_assert (type->arity == TRY_CALL_EXIT(int, "Wrong TL-scheme specified.", tl_parse_int()));

  tl_tree_type *T = (tl_tree_type *)dl::allocate(sizeof(tl_tree_type));
  new(T) tl_tree_type(flags, type, array_size(type->arity, 0, true));

  for (int i = 0; i < type->arity; i++) {
    tl_tree *child = read_expr(var_count);

    T->children.push_back(child);
    if (!(child->flags & FLAG_NOVAR)) {
      T->flags &= ~FLAG_NOVAR;
    }
  }
  return T;
}

tl_tree *read_type_expr(int *var_count) {
  int tree_type = TRY_CALL_EXIT(int, "Wrong TL-scheme specified.", tl_parse_int());
  switch (tree_type) {
    case TLS_TYPE_VAR:
      return read_type_var(var_count);
    case TLS_TYPE_EXPR:
      return read_type(var_count);
    case TLS_ARRAY:
      return read_array(var_count);
    default:
      fprintf(stderr, "tree_type = %d\n", tree_type);
      php_assert (0);
      return nullptr;
  }
}

tl_tree *read_nat_expr(int *var_count) {
  int tree_type = TRY_CALL_EXIT(int, "Wrong TL-scheme specified.", tl_parse_int());
  switch (tree_type) {
    case TLS_NAT_CONST:
      return read_num_const(var_count);
    case TLS_NAT_VAR:
      return read_num_var(var_count);
    default:
      fprintf(stderr, "tree_type = %d\n", tree_type);
      php_assert (0);
      return nullptr;
  }
}

tl_tree *read_expr(int *var_count) {
  int tree_type = TRY_CALL_EXIT(int, "Wrong TL-scheme specified.", tl_parse_int());
  switch (tree_type) {
    case TLS_NAT_CONST:
      return read_nat_expr(var_count);
    case TLS_EXPR_TYPE:
      return read_type_expr(var_count);
    default:
      fprintf(stderr, "tree_type = %d\n", tree_type);
      php_assert (0);
      return nullptr;
  }
}

array<arg> read_args_list(int *var_count) {
  const int schema_flag_opt_field = 2 << (tl_schema_version >= 3);
  const int schema_flag_has_vars = schema_flag_opt_field ^6;

  int args_num = TRY_CALL_EXIT(int, "Wrong TL-scheme specified.", tl_parse_int());
  array<arg> args = array<arg>(array_size(args_num, 0, true));
  for (int i = 0; i < args_num; i++) {
    arg cur_arg;

    php_assert (TRY_CALL_EXIT(int, "Wrong TL-scheme specified.", tl_parse_int()) == TLS_ARG_V2);

    cur_arg.name = TRY_CALL_EXIT(string, "Wrong TL-scheme specified.", tl_parse_string());
    cur_arg.flags = TRY_CALL_EXIT(int, "Wrong TL-scheme specified.", tl_parse_int());

    if (cur_arg.flags & schema_flag_opt_field) {
      cur_arg.flags &= ~schema_flag_opt_field;
      cur_arg.flags |= FLAG_OPT_FIELD;
    }
    if (cur_arg.flags & schema_flag_has_vars) {
      cur_arg.flags &= ~schema_flag_has_vars;
      cur_arg.var_num = TRY_CALL_EXIT(int, "Wrong TL-scheme specified.", tl_parse_int());
    } else {
      cur_arg.var_num = -1;
    }

    if (cur_arg.var_num >= *var_count) {
      *var_count = cur_arg.var_num + 1;
    }
    if (cur_arg.flags & FLAG_OPT_FIELD) {
      cur_arg.exist_var_num = TRY_CALL_EXIT(int, "Wrong TL-scheme specified.", tl_parse_int());
      cur_arg.exist_var_bit = TRY_CALL_EXIT(int, "Wrong TL-scheme specified.", tl_parse_int());
    } else {
      cur_arg.exist_var_num = -1;
      cur_arg.exist_var_bit = 0;
    }
    cur_arg.type = read_type_expr(var_count);
    if (cur_arg.var_num < 0 && cur_arg.exist_var_num < 0 && (cur_arg.type->flags & FLAG_NOVAR)) {
      cur_arg.flags |= FLAG_NOVAR;
    }

    args.push_back(cur_arg);
  }
  return args;
}


tl_combinator *read_combinator() {
  php_assert (TRY_CALL_EXIT(int, "Wrong TL-scheme specified.", tl_parse_int()) == TLS_COMBINATOR);

  tl_combinator *combinator = (tl_combinator *)dl::allocate(sizeof(tl_combinator));
  combinator->id = TRY_CALL_EXIT(int, "Wrong TL-scheme specified.", tl_parse_int());
  new(&combinator->name) string(TRY_CALL_EXIT(string, "Wrong TL-scheme specified.", tl_parse_string()));
  combinator->type_id = TRY_CALL_EXIT(int, "Wrong TL-scheme specified.", tl_parse_int());
  combinator->var_count = 0;

  int left_type = TRY_CALL_EXIT(int, "Wrong TL-scheme specified.", tl_parse_int());
  if (left_type == TLS_COMBINATOR_LEFT) {
    new(&combinator->args) array<arg>(read_args_list(&combinator->var_count));
  } else {
    new(&combinator->args) array<arg>();
    php_assert (left_type == TLS_COMBINATOR_LEFT_BUILTIN);
  }

  php_assert (TRY_CALL_EXIT(int, "Wrong TL-scheme specified.", tl_parse_int()) == TLS_COMBINATOR_RIGHT_V2);
  combinator->result = read_type_expr(&combinator->var_count);

  combinator->IP = nullptr;
  combinator->fetchIP = nullptr;
  combinator->IP_len = 0;
  combinator->fetchIP_len = 0;

  return combinator;
}

tl_type *read_type() {
  php_assert (TRY_CALL_EXIT(int, "Wrong TL-scheme specified.", tl_parse_int()) == TLS_TYPE);

  tl_type *type = (tl_type *)dl::allocate(sizeof(tl_type));
  type->id = TRY_CALL_EXIT(int, "Wrong TL-scheme specified.", tl_parse_int());
  new(&type->name) string(TRY_CALL_EXIT(string, "Wrong TL-scheme specified.", tl_parse_string()));
  type->constructors_num = TRY_CALL_EXIT(int, "Wrong TL-scheme specified.", tl_parse_int());
  new(&type->constructors) array<tl_combinator *>(array_size(type->constructors_num, 0, true));
  type->flags = TRY_CALL_EXIT(int, "Wrong TL-scheme specified.", tl_parse_int());
  if (!strcmp(type->name.c_str(), "Maybe") || !strcmp(type->name.c_str(), "Bool")) {
    type->flags |= FLAG_NOCONS;
  }
  type->arity = TRY_CALL_EXIT(int, "Wrong TL-scheme specified.", tl_parse_int());
  typedef long long ll;
  TRY_CALL_EXIT(ll, "Wrong TL-scheme specified.", tl_parse_long());//unused
  return type;
}


int get_schema_version(int a) {
  if (a == TLS_SCHEMA_V3) {
    return 3;
  }
  if (a == TLS_SCHEMA_V2) {
    return 2;
  }
  return -1;
}

void renew_tl_config() {
  php_assert (!dl::query_num);
  php_assert (tl_config.fetchIP == nullptr);

  tl_schema_version = get_schema_version(TRY_CALL_EXIT(int, "Wrong TL-scheme specified.", tl_parse_int()));
  php_assert (tl_schema_version != -1);

  TRY_CALL_EXIT(int, "Wrong TL-scheme specified.", tl_parse_int()); // version
  TRY_CALL_EXIT(int, "Wrong TL-scheme specified.", tl_parse_int()); // date

  int types_n = TRY_CALL_EXIT(int, "Wrong TL-scheme specified.", tl_parse_int());
  tl_config.types = array<tl_type *>(array_size(types_n, 0, true));
  tl_config.id_to_type = array<tl_type *>(array_size(types_n, 0, false));
  tl_config.name_to_type = array<tl_type *>(array_size(0, types_n, false));

  for (int i = 0; i < types_n; i++) {
    tl_type *type = read_type();
    tl_config.types.push_back(type);
    tl_config.id_to_type.set_value(type->id, type);
    tl_config.name_to_type.set_value(type->name, type);
  }

  tl_config.ReqResult = tl_config.name_to_type.get_value(string("ReqResult", 9));
  php_assert (tl_config.ReqResult != nullptr);
  php_assert (tl_config.ReqResult->arity == 1);

  int constructors_n = TRY_CALL_EXIT(int, "Wrong TL-scheme specified.", tl_parse_int());

  for (int i = 0; i < constructors_n; i++) {
    tl_combinator *constructor = read_combinator();
    tl_config.id_to_type.get_value(constructor->type_id)->constructors.push_back(constructor);
  }

  int functions_n = TRY_CALL_EXIT(int, "Wrong TL-scheme specified.", tl_parse_int());
  tl_config.functions = array<tl_combinator *>(array_size(functions_n, 0, true));
  tl_config.id_to_function = array<tl_combinator *>(array_size(functions_n, 0, false));
  tl_config.name_to_function = array<tl_combinator *>(array_size(0, functions_n, false));

  for (int i = 0; i < functions_n; i++) {
    tl_combinator *function = read_combinator();
    tl_config.functions.push_back(function);
    tl_config.id_to_function.set_value(function->id, function);
    tl_config.name_to_function.set_value(function->name, function);
  }
  TRY_CALL_VOID_EXIT("Wrong TL-scheme specified.", tl_parse_end());

  for (int i = 0; i < types_n; i++) {
    php_assert (tl_config.types[i]->constructors.count() == tl_config.types[i]->constructors_num);
  }

  static void *IP[10000];

  gen_function_fetch(tl_config.fetchIP, IP, 10000);

  for (int i = 0; i < tl_config.types.count(); i++) {
    tl_type *cur_type = tl_config.types.get_value(i);
    for (int j = 0; j < cur_type->constructors_num; j++) {
      php_assert (gen_constructor_store(*cur_type->constructors.get_value(j), IP, 10000) > 0);
      php_assert (gen_constructor_fetch(*cur_type->constructors.get_value(j), IP, 10000) > 0);
    }
  }
  for (int i = 0; i < functions_n; i++) {
    php_assert (gen_function_store(*tl_config.functions.get_value(i), IP, 10000) > 0);
  }
}

unsigned tl_schema_crc32 = 0;

void update_tl_config(const char *data, dl::size_type data_len) {
  tl_schema_crc32 = compute_crc32(data, data_len);
  php_assert (f$rpc_parse(string(data, data_len)));
  renew_tl_config();

  rpc_parse(nullptr, 0);//remove rpc_data_copy
  rpc_parse(nullptr, 0);//remove rpc_data_backup
  free_arr_space();
}


void read_tl_config(const char *file_name) {
  OrFalse<string> config = file_file_get_contents(string(file_name, (dl::size_type)strlen(file_name)));
  php_assert (f$boolval(config));
  tl_schema_crc32 = compute_crc32(config.value.c_str(), config.value.size());
  php_assert (f$rpc_parse(config.val()));
  renew_tl_config();

  rpc_parse(nullptr, 0);//remove rpc_data_copy
  rpc_parse(nullptr, 0);//remove rpc_data_backup
  free_arr_space();
}


void global_init_rpc_lib() {
  php_assert (timeout_wakeup_id == -1);

  timeout_wakeup_id = register_wakeup_callback(&process_rpc_timeout);
}

static void reset_rpc_global_vars() {
  hard_reset_var(rpc_filename);
  hard_reset_var(rpc_data_copy);
  hard_reset_var(rpc_data_copy_backup);
  hard_reset_var(rpc_request_need_timer);
}

void init_rpc_lib() {
  php_assert (timeout_wakeup_id != -1);

  reset_rpc_global_vars();

  rpc_parse(nullptr, 0);
  // init backup
  rpc_parse(nullptr, 0);

  f$rpc_clean(false);
  rpc_stored = 0;

  rpc_pack_threshold = -1;
  rpc_pack_from = -1;
  rpc_filename = string("rpc.cpp", 7);

  last_var_ptr = vars_buffer + MAX_VARS;
}

void free_rpc_lib() {
  reset_rpc_global_vars();
  clear_arr_space();
}
