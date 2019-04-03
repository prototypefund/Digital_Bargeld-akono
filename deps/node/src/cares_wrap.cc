// Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

#define CARES_STATICLIB
#include "ares.h"
#include "async_wrap-inl.h"
#include "env-inl.h"
#include "node.h"
#include "req_wrap-inl.h"
#include "util-inl.h"
#include "uv.h"

#include <cerrno>
#include <cstring>
#include <vector>
#include <unordered_set>

#ifdef __POSIX__
# include <netdb.h>
#endif  // __POSIX__

#if defined(__ANDROID__) || \
    defined(__MINGW32__) || \
    defined(__OpenBSD__) || \
    defined(_MSC_VER)

# include <nameser.h>
#else
# include <arpa/nameser.h>
#endif

#if defined(__OpenBSD__)
# define AI_V4MAPPED 0
#endif

namespace node {
namespace cares_wrap {

using v8::Array;
using v8::Context;
using v8::EscapableHandleScope;
using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::HandleScope;
using v8::Int32;
using v8::Integer;
using v8::Local;
using v8::Null;
using v8::Object;
using v8::String;
using v8::Value;

namespace {

Mutex ares_library_mutex;

inline uint16_t cares_get_16bit(const unsigned char* p) {
  return static_cast<uint32_t>(p[0] << 8U) | (static_cast<uint32_t>(p[1]));
}

inline uint32_t cares_get_32bit(const unsigned char* p) {
  return static_cast<uint32_t>(p[0] << 24U) |
         static_cast<uint32_t>(p[1] << 16U) |
         static_cast<uint32_t>(p[2] << 8U) |
         static_cast<uint32_t>(p[3]);
}

const int ns_t_cname_or_a = -1;

#define DNS_ESETSRVPENDING -1000
inline const char* ToErrorCodeString(int status) {
  switch (status) {
#define V(code) case ARES_##code: return #code;
    V(EADDRGETNETWORKPARAMS)
    V(EBADFAMILY)
    V(EBADFLAGS)
    V(EBADHINTS)
    V(EBADNAME)
    V(EBADQUERY)
    V(EBADRESP)
    V(EBADSTR)
    V(ECANCELLED)
    V(ECONNREFUSED)
    V(EDESTRUCTION)
    V(EFILE)
    V(EFORMERR)
    V(ELOADIPHLPAPI)
    V(ENODATA)
    V(ENOMEM)
    V(ENONAME)
    V(ENOTFOUND)
    V(ENOTIMP)
    V(ENOTINITIALIZED)
    V(EOF)
    V(EREFUSED)
    V(ESERVFAIL)
    V(ETIMEOUT)
#undef V
  }

  return "UNKNOWN_ARES_ERROR";
}

class ChannelWrap;

struct node_ares_task : public MemoryRetainer {
  ChannelWrap* channel;
  ares_socket_t sock;
  uv_poll_t poll_watcher;

  inline void MemoryInfo(MemoryTracker* tracker) const override;
  SET_MEMORY_INFO_NAME(node_ares_task)
  SET_SELF_SIZE(node_ares_task)
};

struct TaskHash {
  size_t operator()(node_ares_task* a) const {
    return std::hash<ares_socket_t>()(a->sock);
  }
};

struct TaskEqual {
  inline bool operator()(node_ares_task* a, node_ares_task* b) const {
    return a->sock == b->sock;
  }
};

using node_ares_task_list =
    std::unordered_set<node_ares_task*, TaskHash, TaskEqual>;

class ChannelWrap : public AsyncWrap {
 public:
  ChannelWrap(Environment* env, Local<Object> object);
  ~ChannelWrap() override;

  static void New(const FunctionCallbackInfo<Value>& args);

  void Setup();
  void EnsureServers();
  void StartTimer();
  void CloseTimer();

  void ModifyActivityQueryCount(int count);

  inline uv_timer_t* timer_handle() { return timer_handle_; }
  inline ares_channel cares_channel() { return channel_; }
  inline void set_query_last_ok(bool ok) { query_last_ok_ = ok; }
  inline void set_is_servers_default(bool is_default) {
    is_servers_default_ = is_default;
  }
  inline int active_query_count() { return active_query_count_; }
  inline node_ares_task_list* task_list() { return &task_list_; }

  void MemoryInfo(MemoryTracker* tracker) const override {
    if (timer_handle_ != nullptr)
      tracker->TrackField("timer_handle", *timer_handle_);
    tracker->TrackField("task_list", task_list_, "node_ares_task_list");
  }

  SET_MEMORY_INFO_NAME(ChannelWrap)
  SET_SELF_SIZE(ChannelWrap)

  static void AresTimeout(uv_timer_t* handle);

 private:
  uv_timer_t* timer_handle_;
  ares_channel channel_;
  bool query_last_ok_;
  bool is_servers_default_;
  bool library_inited_;
  int active_query_count_;
  node_ares_task_list task_list_;
};

ChannelWrap::ChannelWrap(Environment* env,
                         Local<Object> object)
  : AsyncWrap(env, object, PROVIDER_DNSCHANNEL),
    timer_handle_(nullptr),
    channel_(nullptr),
    query_last_ok_(true),
    is_servers_default_(true),
    library_inited_(false),
    active_query_count_(0) {
  MakeWeak();

  Setup();
}

void ChannelWrap::New(const FunctionCallbackInfo<Value>& args) {
  CHECK(args.IsConstructCall());
  CHECK_EQ(args.Length(), 0);

  Environment* env = Environment::GetCurrent(args);
  new ChannelWrap(env, args.This());
}

class GetAddrInfoReqWrap : public ReqWrap<uv_getaddrinfo_t> {
 public:
  GetAddrInfoReqWrap(Environment* env,
                     Local<Object> req_wrap_obj,
                     bool verbatim);

  SET_NO_MEMORY_INFO()
  SET_MEMORY_INFO_NAME(GetAddrInfoReqWrap)
  SET_SELF_SIZE(GetAddrInfoReqWrap)

  bool verbatim() const { return verbatim_; }

 private:
  const bool verbatim_;
};

GetAddrInfoReqWrap::GetAddrInfoReqWrap(Environment* env,
                                       Local<Object> req_wrap_obj,
                                       bool verbatim)
    : ReqWrap(env, req_wrap_obj, AsyncWrap::PROVIDER_GETADDRINFOREQWRAP)
    , verbatim_(verbatim) {
}


class GetNameInfoReqWrap : public ReqWrap<uv_getnameinfo_t> {
 public:
  GetNameInfoReqWrap(Environment* env, Local<Object> req_wrap_obj);

  SET_NO_MEMORY_INFO()
  SET_MEMORY_INFO_NAME(GetNameInfoReqWrap)
  SET_SELF_SIZE(GetNameInfoReqWrap)
};

GetNameInfoReqWrap::GetNameInfoReqWrap(Environment* env,
                                       Local<Object> req_wrap_obj)
    : ReqWrap(env, req_wrap_obj, AsyncWrap::PROVIDER_GETNAMEINFOREQWRAP) {
}


/* This is called once per second by loop->timer. It is used to constantly */
/* call back into c-ares for possibly processing timeouts. */
void ChannelWrap::AresTimeout(uv_timer_t* handle) {
  ChannelWrap* channel = static_cast<ChannelWrap*>(handle->data);
  CHECK_EQ(channel->timer_handle(), handle);
  CHECK_EQ(false, channel->task_list()->empty());
  ares_process_fd(channel->cares_channel(), ARES_SOCKET_BAD, ARES_SOCKET_BAD);
}


void ares_poll_cb(uv_poll_t* watcher, int status, int events) {
  node_ares_task* task = ContainerOf(&node_ares_task::poll_watcher, watcher);
  ChannelWrap* channel = task->channel;

  /* Reset the idle timer */
  uv_timer_again(channel->timer_handle());

  if (status < 0) {
    /* An error happened. Just pretend that the socket is both readable and */
    /* writable. */
    ares_process_fd(channel->cares_channel(), task->sock, task->sock);
    return;
  }

  /* Process DNS responses */
  ares_process_fd(channel->cares_channel(),
                  events & UV_READABLE ? task->sock : ARES_SOCKET_BAD,
                  events & UV_WRITABLE ? task->sock : ARES_SOCKET_BAD);
}


void ares_poll_close_cb(uv_poll_t* watcher) {
  node_ares_task* task = ContainerOf(&node_ares_task::poll_watcher, watcher);
  delete task;
}

void node_ares_task::MemoryInfo(MemoryTracker* tracker) const {
  tracker->TrackField("channel", channel);
}

/* Allocates and returns a new node_ares_task */
node_ares_task* ares_task_create(ChannelWrap* channel, ares_socket_t sock) {
  auto task = new node_ares_task();

  if (task == nullptr) {
    /* Out of memory. */
    return nullptr;
  }

  task->channel = channel;
  task->sock = sock;

  if (uv_poll_init_socket(channel->env()->event_loop(),
                          &task->poll_watcher, sock) < 0) {
    /* This should never happen. */
    free(task);
    return nullptr;
  }

  return task;
}


/* Callback from ares when socket operation is started */
void ares_sockstate_cb(void* data,
                       ares_socket_t sock,
                       int read,
                       int write) {
  ChannelWrap* channel = static_cast<ChannelWrap*>(data);
  node_ares_task* task;

  node_ares_task lookup_task;
  lookup_task.sock = sock;
  auto it = channel->task_list()->find(&lookup_task);

  task = (it == channel->task_list()->end()) ? nullptr : *it;

  if (read || write) {
    if (!task) {
      /* New socket */
      channel->StartTimer();

      task = ares_task_create(channel, sock);
      if (task == nullptr) {
        /* This should never happen unless we're out of memory or something */
        /* is seriously wrong. The socket won't be polled, but the query will */
        /* eventually time out. */
        return;
      }

      channel->task_list()->insert(task);
    }

    /* This should never fail. If it fails anyway, the query will eventually */
    /* time out. */
    uv_poll_start(&task->poll_watcher,
                  (read ? UV_READABLE : 0) | (write ? UV_WRITABLE : 0),
                  ares_poll_cb);

  } else {
    /* read == 0 and write == 0 this is c-ares's way of notifying us that */
    /* the socket is now closed. We must free the data associated with */
    /* socket. */
    CHECK(task &&
          "When an ares socket is closed we should have a handle for it");

    channel->task_list()->erase(it);
    channel->env()->CloseHandle(&task->poll_watcher, ares_poll_close_cb);

    if (channel->task_list()->empty()) {
      channel->CloseTimer();
    }
  }
}


Local<Array> HostentToNames(Environment* env,
                            struct hostent* host,
                            Local<Array> append_to = Local<Array>()) {
  EscapableHandleScope scope(env->isolate());
  auto context = env->context();
  bool append = !append_to.IsEmpty();
  Local<Array> names = append ? append_to : Array::New(env->isolate());
  size_t offset = names->Length();

  for (uint32_t i = 0; host->h_aliases[i] != nullptr; ++i) {
    Local<String> address = OneByteString(env->isolate(), host->h_aliases[i]);
    names->Set(context, i + offset, address).FromJust();
  }

  return append ? names : scope.Escape(names);
}

void safe_free_hostent(struct hostent* host) {
  int idx;

  if (host->h_addr_list != nullptr) {
    idx = 0;
    while (host->h_addr_list[idx]) {
      free(host->h_addr_list[idx++]);
    }
    free(host->h_addr_list);
    host->h_addr_list = nullptr;
  }

  if (host->h_aliases != nullptr) {
    idx = 0;
    while (host->h_aliases[idx]) {
      free(host->h_aliases[idx++]);
    }
    free(host->h_aliases);
    host->h_aliases = nullptr;
  }

  free(host->h_name);
  free(host);
}

void cares_wrap_hostent_cpy(struct hostent* dest, const struct hostent* src) {
  dest->h_addr_list = nullptr;
  dest->h_addrtype = 0;
  dest->h_aliases = nullptr;
  dest->h_length = 0;
  dest->h_name = nullptr;

  /* copy `h_name` */
  size_t name_size = strlen(src->h_name) + 1;
  dest->h_name = node::Malloc<char>(name_size);
  memcpy(dest->h_name, src->h_name, name_size);

  /* copy `h_aliases` */
  size_t alias_count;
  for (alias_count = 0;
      src->h_aliases[alias_count] != nullptr;
      alias_count++) {
  }

  dest->h_aliases = node::Malloc<char*>(alias_count + 1);
  for (size_t i = 0; i < alias_count; i++) {
    const size_t cur_alias_size = strlen(src->h_aliases[i]) + 1;
    dest->h_aliases[i] = node::Malloc(cur_alias_size);
    memcpy(dest->h_aliases[i], src->h_aliases[i], cur_alias_size);
  }
  dest->h_aliases[alias_count] = nullptr;

  /* copy `h_addr_list` */
  size_t list_count;
  for (list_count = 0;
      src->h_addr_list[list_count] != nullptr;
      list_count++) {
  }

  dest->h_addr_list = node::Malloc<char*>(list_count + 1);
  for (size_t i = 0; i < list_count; i++) {
    dest->h_addr_list[i] = node::Malloc(src->h_length);
    memcpy(dest->h_addr_list[i], src->h_addr_list[i], src->h_length);
  }
  dest->h_addr_list[list_count] = nullptr;

  /* work after work */
  dest->h_length = src->h_length;
  dest->h_addrtype = src->h_addrtype;
}

class QueryWrap;

void ChannelWrap::Setup() {
  struct ares_options options;
  memset(&options, 0, sizeof(options));
  options.flags = ARES_FLAG_NOCHECKRESP;
  options.sock_state_cb = ares_sockstate_cb;
  options.sock_state_cb_data = this;

  int r;
  if (!library_inited_) {
    Mutex::ScopedLock lock(ares_library_mutex);
    // Multiple calls to ares_library_init() increase a reference counter,
    // so this is a no-op except for the first call to it.
    r = ares_library_init(ARES_LIB_INIT_ALL);
    if (r != ARES_SUCCESS)
      return env()->ThrowError(ToErrorCodeString(r));
  }

  /* We do the call to ares_init_option for caller. */
  r = ares_init_options(&channel_,
                        &options,
                        ARES_OPT_FLAGS | ARES_OPT_SOCK_STATE_CB);

  if (r != ARES_SUCCESS) {
    Mutex::ScopedLock lock(ares_library_mutex);
    ares_library_cleanup();
    return env()->ThrowError(ToErrorCodeString(r));
  }

  library_inited_ = true;
}

void ChannelWrap::StartTimer() {
  if (timer_handle_ == nullptr) {
    timer_handle_ = new uv_timer_t();
    timer_handle_->data = static_cast<void*>(this);
    uv_timer_init(env()->event_loop(), timer_handle_);
  } else if (uv_is_active(reinterpret_cast<uv_handle_t*>(timer_handle_))) {
    return;
  }
  uv_timer_start(timer_handle_, AresTimeout, 1000, 1000);
}

void ChannelWrap::CloseTimer() {
  if (timer_handle_ == nullptr)
    return;

  env()->CloseHandle(timer_handle_, [](uv_timer_t* handle) { delete handle; });
  timer_handle_ = nullptr;
}

ChannelWrap::~ChannelWrap() {
  ares_destroy(channel_);

  if (library_inited_) {
    Mutex::ScopedLock lock(ares_library_mutex);
    // This decreases the reference counter increased by ares_library_init().
    ares_library_cleanup();
  }

  CloseTimer();
}


void ChannelWrap::ModifyActivityQueryCount(int count) {
  active_query_count_ += count;
  CHECK_GE(active_query_count_, 0);
}


/**
 * This function is to check whether current servers are fallback servers
 * when cares initialized.
 *
 * The fallback servers of cares is [ "127.0.0.1" ] with no user additional
 * setting.
 */
void ChannelWrap::EnsureServers() {
  /* if last query is OK or servers are set by user self, do not check */
  if (query_last_ok_ || !is_servers_default_) {
    return;
  }

  ares_addr_port_node* servers = nullptr;

  ares_get_servers_ports(channel_, &servers);

  /* if no server or multi-servers, ignore */
  if (servers == nullptr) return;
  if (servers->next != nullptr) {
    ares_free_data(servers);
    is_servers_default_ = false;
    return;
  }

  /* if the only server is not 127.0.0.1, ignore */
  if (servers[0].family != AF_INET ||
      servers[0].addr.addr4.s_addr != htonl(INADDR_LOOPBACK) ||
      servers[0].tcp_port != 0 ||
      servers[0].udp_port != 0) {
    ares_free_data(servers);
    is_servers_default_ = false;
    return;
  }

  ares_free_data(servers);
  servers = nullptr;

  /* destroy channel and reset channel */
  ares_destroy(channel_);

  CloseTimer();
  Setup();
}


class QueryWrap : public AsyncWrap {
 public:
  QueryWrap(ChannelWrap* channel, Local<Object> req_wrap_obj, const char* name)
      : AsyncWrap(channel->env(), req_wrap_obj, AsyncWrap::PROVIDER_QUERYWRAP),
        channel_(channel),
        trace_name_(name) {
    // Make sure the channel object stays alive during the query lifetime.
    req_wrap_obj->Set(env()->context(),
                      env()->channel_string(),
                      channel->object()).FromJust();
  }

  ~QueryWrap() override {
    CHECK_EQ(false, persistent().IsEmpty());

    // Let Callback() know that this object no longer exists.
    if (callback_ptr_ != nullptr)
      *callback_ptr_ = nullptr;
  }

  // Subclasses should implement the appropriate Send method.
  virtual int Send(const char* name) {
    UNREACHABLE();
    return 0;
  }

  virtual int Send(const char* name, int family) {
    UNREACHABLE();
    return 0;
  }

 protected:
  void AresQuery(const char* name,
                 int dnsclass,
                 int type) {
    channel_->EnsureServers();
    TRACE_EVENT_NESTABLE_ASYNC_BEGIN1(
      TRACING_CATEGORY_NODE2(dns, native), trace_name_, this,
      "name", TRACE_STR_COPY(name));
    ares_query(channel_->cares_channel(), name, dnsclass, type, Callback,
               MakeCallbackPointer());
  }

  struct ResponseData {
    int status;
    bool is_host;
    DeleteFnPtr<hostent, safe_free_hostent> host;
    MallocedBuffer<unsigned char> buf;
  };

  void AfterResponse() {
    CHECK(response_data_);

    const int status = response_data_->status;

    if (status != ARES_SUCCESS) {
      ParseError(status);
    } else if (!response_data_->is_host) {
      Parse(response_data_->buf.data, response_data_->buf.size);
    } else {
      Parse(response_data_->host.get());
    }

    delete this;
  }

  void* MakeCallbackPointer() {
    CHECK_NULL(callback_ptr_);
    callback_ptr_ = new QueryWrap*(this);
    return callback_ptr_;
  }

  static QueryWrap* FromCallbackPointer(void* arg) {
    std::unique_ptr<QueryWrap*> wrap_ptr { static_cast<QueryWrap**>(arg) };
    QueryWrap* wrap = *wrap_ptr.get();
    if (wrap == nullptr) return nullptr;
    wrap->callback_ptr_ = nullptr;
    return wrap;
  }

  static void Callback(void* arg, int status, int timeouts,
                       unsigned char* answer_buf, int answer_len) {
    QueryWrap* wrap = FromCallbackPointer(arg);
    if (wrap == nullptr) return;

    unsigned char* buf_copy = nullptr;
    if (status == ARES_SUCCESS) {
      buf_copy = node::Malloc<unsigned char>(answer_len);
      memcpy(buf_copy, answer_buf, answer_len);
    }

    wrap->response_data_.reset(new ResponseData());
    ResponseData* data = wrap->response_data_.get();
    data->status = status;
    data->is_host = false;
    data->buf = MallocedBuffer<unsigned char>(buf_copy, answer_len);

    wrap->QueueResponseCallback(status);
  }

  static void Callback(void* arg, int status, int timeouts,
                       struct hostent* host) {
    QueryWrap* wrap = FromCallbackPointer(arg);
    if (wrap == nullptr) return;

    struct hostent* host_copy = nullptr;
    if (status == ARES_SUCCESS) {
      host_copy = node::Malloc<hostent>(1);
      cares_wrap_hostent_cpy(host_copy, host);
    }

    wrap->response_data_.reset(new ResponseData());
    ResponseData* data = wrap->response_data_.get();
    data->status = status;
    data->host.reset(host_copy);
    data->is_host = true;

    wrap->QueueResponseCallback(status);
  }

  void QueueResponseCallback(int status) {
    env()->SetImmediate([](Environment*, void* data) {
      static_cast<QueryWrap*>(data)->AfterResponse();
    }, this, object());

    channel_->set_query_last_ok(status != ARES_ECONNREFUSED);
    channel_->ModifyActivityQueryCount(-1);
  }

  void CallOnComplete(Local<Value> answer,
                      Local<Value> extra = Local<Value>()) {
    HandleScope handle_scope(env()->isolate());
    Context::Scope context_scope(env()->context());
    Local<Value> argv[] = {
      Integer::New(env()->isolate(), 0),
      answer,
      extra
    };
    const int argc = arraysize(argv) - extra.IsEmpty();
    TRACE_EVENT_NESTABLE_ASYNC_END0(
        TRACING_CATEGORY_NODE2(dns, native), trace_name_, this);

    MakeCallback(env()->oncomplete_string(), argc, argv);
  }

  void ParseError(int status) {
    CHECK_NE(status, ARES_SUCCESS);
    HandleScope handle_scope(env()->isolate());
    Context::Scope context_scope(env()->context());
    const char* code = ToErrorCodeString(status);
    Local<Value> arg = OneByteString(env()->isolate(), code);
    TRACE_EVENT_NESTABLE_ASYNC_END1(
        TRACING_CATEGORY_NODE2(dns, native), trace_name_, this,
        "error", status);
    MakeCallback(env()->oncomplete_string(), 1, &arg);
  }

  // Subclasses should implement the appropriate Parse method.
  virtual void Parse(unsigned char* buf, int len) {
    UNREACHABLE();
  }

  virtual void Parse(struct hostent* host) {
    UNREACHABLE();
  }

  ChannelWrap* channel_;

 private:
  std::unique_ptr<ResponseData> response_data_;
  const char* trace_name_;
  // Pointer to pointer to 'this' that can be reset from the destructor,
  // in order to let Callback() know that 'this' no longer exists.
  QueryWrap** callback_ptr_ = nullptr;
};


template <typename T>
Local<Array> AddrTTLToArray(Environment* env,
                            const T* addrttls,
                            size_t naddrttls) {
  auto isolate = env->isolate();
  EscapableHandleScope escapable_handle_scope(isolate);
  auto context = env->context();

  Local<Array> ttls = Array::New(isolate, naddrttls);
  for (size_t i = 0; i < naddrttls; i++) {
    auto value = Integer::New(isolate, addrttls[i].ttl);
    ttls->Set(context, i, value).FromJust();
  }

  return escapable_handle_scope.Escape(ttls);
}


int ParseGeneralReply(Environment* env,
                      const unsigned char* buf,
                      int len,
                      int* type,
                      Local<Array> ret,
                      void* addrttls = nullptr,
                      int* naddrttls = nullptr) {
  HandleScope handle_scope(env->isolate());
  auto context = env->context();
  hostent* host;

  int status;
  switch (*type) {
    case ns_t_a:
    case ns_t_cname:
    case ns_t_cname_or_a:
      status = ares_parse_a_reply(buf,
                                  len,
                                  &host,
                                  static_cast<ares_addrttl*>(addrttls),
                                  naddrttls);
      break;
    case ns_t_aaaa:
      status = ares_parse_aaaa_reply(buf,
                                     len,
                                     &host,
                                     static_cast<ares_addr6ttl*>(addrttls),
                                     naddrttls);
      break;
    case ns_t_ns:
      status = ares_parse_ns_reply(buf, len, &host);
      break;
    case ns_t_ptr:
      status = ares_parse_ptr_reply(buf, len, nullptr, 0, AF_INET, &host);
      break;
    default:
      CHECK(0 && "Bad NS type");
      break;
  }

  if (status != ARES_SUCCESS)
    return status;

  /* If it's `CNAME`, return the CNAME value;
   * And if it's `CNAME_OR_A` and it has value in `h_name` and `h_aliases[0]`,
   * we consider it's a CNAME record, otherwise we consider it's an A record. */
  if ((*type == ns_t_cname_or_a && host->h_name && host->h_aliases[0]) ||
      *type == ns_t_cname) {
    // A cname lookup always returns a single record but we follow the
    // common API here.
    *type = ns_t_cname;
    ret->Set(context,
             ret->Length(),
             OneByteString(env->isolate(), host->h_name)).FromJust();
    ares_free_hostent(host);
    return ARES_SUCCESS;
  }

  if (*type == ns_t_cname_or_a)
    *type = ns_t_a;

  if (*type == ns_t_ns) {
    HostentToNames(env, host, ret);
  } else if (*type == ns_t_ptr) {
    uint32_t offset = ret->Length();
    for (uint32_t i = 0; host->h_aliases[i] != nullptr; i++) {
      auto alias = OneByteString(env->isolate(), host->h_aliases[i]);
      ret->Set(context, i + offset, alias).FromJust();
    }
  } else {
    uint32_t offset = ret->Length();
    char ip[INET6_ADDRSTRLEN];
    for (uint32_t i = 0; host->h_addr_list[i] != nullptr; ++i) {
      uv_inet_ntop(host->h_addrtype, host->h_addr_list[i], ip, sizeof(ip));
      auto address = OneByteString(env->isolate(), ip);
      ret->Set(context, i + offset, address).FromJust();
    }
  }

  ares_free_hostent(host);

  return ARES_SUCCESS;
}


int ParseMxReply(Environment* env,
                 const unsigned char* buf,
                 int len,
                 Local<Array> ret,
                 bool need_type = false) {
  HandleScope handle_scope(env->isolate());
  auto context = env->context();

  struct ares_mx_reply* mx_start;
  int status = ares_parse_mx_reply(buf, len, &mx_start);
  if (status != ARES_SUCCESS) {
    return status;
  }

  uint32_t offset = ret->Length();
  ares_mx_reply* current = mx_start;
  for (uint32_t i = 0; current != nullptr; ++i, current = current->next) {
    Local<Object> mx_record = Object::New(env->isolate());
    mx_record->Set(context,
                   env->exchange_string(),
                   OneByteString(env->isolate(), current->host)).FromJust();
    mx_record->Set(context,
                   env->priority_string(),
                   Integer::New(env->isolate(), current->priority)).FromJust();
    if (need_type)
      mx_record->Set(context,
                     env->type_string(),
                     env->dns_mx_string()).FromJust();

    ret->Set(context, i + offset, mx_record).FromJust();
  }

  ares_free_data(mx_start);
  return ARES_SUCCESS;
}

int ParseTxtReply(Environment* env,
                  const unsigned char* buf,
                  int len,
                  Local<Array> ret,
                  bool need_type = false) {
  HandleScope handle_scope(env->isolate());
  auto context = env->context();

  struct ares_txt_ext* txt_out;

  int status = ares_parse_txt_reply_ext(buf, len, &txt_out);
  if (status != ARES_SUCCESS) {
    return status;
  }

  Local<Array> txt_chunk;

  struct ares_txt_ext* current = txt_out;
  uint32_t i = 0, j;
  uint32_t offset = ret->Length();
  for (j = 0; current != nullptr; current = current->next) {
    Local<String> txt = OneByteString(env->isolate(), current->txt);

    // New record found - write out the current chunk
    if (current->record_start) {
      if (!txt_chunk.IsEmpty()) {
        if (need_type) {
          Local<Object> elem = Object::New(env->isolate());
          elem->Set(context, env->entries_string(), txt_chunk).FromJust();
          elem->Set(context,
                    env->type_string(),
                    env->dns_txt_string()).FromJust();
          ret->Set(context, offset + i++, elem).FromJust();
        } else {
          ret->Set(context, offset + i++, txt_chunk).FromJust();
        }
      }

      txt_chunk = Array::New(env->isolate());
      j = 0;
    }

    txt_chunk->Set(context, j++, txt).FromJust();
  }

  // Push last chunk if it isn't empty
  if (!txt_chunk.IsEmpty()) {
    if (need_type) {
      Local<Object> elem = Object::New(env->isolate());
      elem->Set(context, env->entries_string(), txt_chunk).FromJust();
      elem->Set(context,
                env->type_string(),
                env->dns_txt_string()).FromJust();
      ret->Set(context, offset + i, elem).FromJust();
    } else {
      ret->Set(context, offset + i, txt_chunk).FromJust();
    }
  }

  ares_free_data(txt_out);
  return ARES_SUCCESS;
}


int ParseSrvReply(Environment* env,
                  const unsigned char* buf,
                  int len,
                  Local<Array> ret,
                  bool need_type = false) {
  HandleScope handle_scope(env->isolate());
  auto context = env->context();

  struct ares_srv_reply* srv_start;
  int status = ares_parse_srv_reply(buf, len, &srv_start);
  if (status != ARES_SUCCESS) {
    return status;
  }

  ares_srv_reply* current = srv_start;
  int offset = ret->Length();
  for (uint32_t i = 0; current != nullptr; ++i, current = current->next) {
    Local<Object> srv_record = Object::New(env->isolate());
    srv_record->Set(context,
                    env->name_string(),
                    OneByteString(env->isolate(), current->host)).FromJust();
    srv_record->Set(context,
                    env->port_string(),
                    Integer::New(env->isolate(), current->port)).FromJust();
    srv_record->Set(context,
                    env->priority_string(),
                    Integer::New(env->isolate(), current->priority)).FromJust();
    srv_record->Set(context,
                    env->weight_string(),
                    Integer::New(env->isolate(), current->weight)).FromJust();
    if (need_type)
      srv_record->Set(context,
                      env->type_string(),
                      env->dns_srv_string()).FromJust();

    ret->Set(context, i + offset, srv_record).FromJust();
  }

  ares_free_data(srv_start);
  return ARES_SUCCESS;
}


int ParseNaptrReply(Environment* env,
                    const unsigned char* buf,
                    int len,
                    Local<Array> ret,
                    bool need_type = false) {
  HandleScope handle_scope(env->isolate());
  auto context = env->context();

  ares_naptr_reply* naptr_start;
  int status = ares_parse_naptr_reply(buf, len, &naptr_start);

  if (status != ARES_SUCCESS) {
    return status;
  }

  ares_naptr_reply* current = naptr_start;
  int offset = ret->Length();
  for (uint32_t i = 0; current != nullptr; ++i, current = current->next) {
    Local<Object> naptr_record = Object::New(env->isolate());
    naptr_record->Set(context,
                      env->flags_string(),
                      OneByteString(env->isolate(), current->flags)).FromJust();
    naptr_record->Set(context,
                      env->service_string(),
                      OneByteString(env->isolate(),
                                    current->service)).FromJust();
    naptr_record->Set(context,
                      env->regexp_string(),
                      OneByteString(env->isolate(),
                                    current->regexp)).FromJust();
    naptr_record->Set(context,
                      env->replacement_string(),
                      OneByteString(env->isolate(),
                                    current->replacement)).FromJust();
    naptr_record->Set(context,
                      env->order_string(),
                      Integer::New(env->isolate(), current->order)).FromJust();
    naptr_record->Set(context,
                      env->preference_string(),
                      Integer::New(env->isolate(),
                                   current->preference)).FromJust();
    if (need_type)
      naptr_record->Set(context,
                        env->type_string(),
                        env->dns_naptr_string()).FromJust();

    ret->Set(context, i + offset, naptr_record).FromJust();
  }

  ares_free_data(naptr_start);
  return ARES_SUCCESS;
}


int ParseSoaReply(Environment* env,
                  unsigned char* buf,
                  int len,
                  Local<Object>* ret) {
  EscapableHandleScope handle_scope(env->isolate());
  auto context = env->context();

  // Manage memory using standardard smart pointer std::unique_tr
  struct AresDeleter {
    void operator()(char* ptr) const noexcept { ares_free_string(ptr); }
  };
  using ares_unique_ptr = std::unique_ptr<char[], AresDeleter>;

  // Can't use ares_parse_soa_reply() here which can only parse single record
  const unsigned int ancount = cares_get_16bit(buf + 6);
  unsigned char* ptr = buf + NS_HFIXEDSZ;
  char* name_temp;
  long temp_len;  // NOLINT(runtime/int)
  int status = ares_expand_name(ptr, buf, len, &name_temp, &temp_len);
  const ares_unique_ptr name(name_temp);
  if (status != ARES_SUCCESS) {
    // returns EBADRESP in case of invalid input
    return status == ARES_EBADNAME ? ARES_EBADRESP : status;
  }

  if (ptr + temp_len + NS_QFIXEDSZ > buf + len) {
    return ARES_EBADRESP;
  }
  ptr += temp_len + NS_QFIXEDSZ;

  for (unsigned int i = 0; i < ancount; i++) {
    char* rr_name_temp;
    long rr_temp_len;  // NOLINT(runtime/int)
    int status2 = ares_expand_name(ptr, buf, len, &rr_name_temp, &rr_temp_len);
    const ares_unique_ptr rr_name(rr_name_temp);

    if (status2 != ARES_SUCCESS)
      return status2 == ARES_EBADNAME ? ARES_EBADRESP : status2;

    ptr += rr_temp_len;
    if (ptr + NS_RRFIXEDSZ > buf + len) {
      return ARES_EBADRESP;
    }

    const int rr_type = cares_get_16bit(ptr);
    const int rr_len = cares_get_16bit(ptr + 8);
    ptr += NS_RRFIXEDSZ;

    // only need SOA
    if (rr_type == ns_t_soa) {
      char* nsname_temp;
      long nsname_temp_len;  // NOLINT(runtime/int)

      int status3 = ares_expand_name(ptr, buf, len,
                                     &nsname_temp,
                                     &nsname_temp_len);
      const ares_unique_ptr nsname(nsname_temp);
      if (status3 != ARES_SUCCESS) {
        return status3 == ARES_EBADNAME ? ARES_EBADRESP : status3;
      }
      ptr += nsname_temp_len;

      char* hostmaster_temp;
      long hostmaster_temp_len;  // NOLINT(runtime/int)
      int status4 = ares_expand_name(ptr, buf, len,
                                     &hostmaster_temp,
                                     &hostmaster_temp_len);
      const ares_unique_ptr hostmaster(hostmaster_temp);
      if (status4 != ARES_SUCCESS) {
        return status4 == ARES_EBADNAME ? ARES_EBADRESP : status4;
      }
      ptr += hostmaster_temp_len;

      if (ptr + 5 * 4 > buf + len) {
        return ARES_EBADRESP;
      }

      const unsigned int serial = cares_get_32bit(ptr + 0 * 4);
      const unsigned int refresh = cares_get_32bit(ptr + 1 * 4);
      const unsigned int retry = cares_get_32bit(ptr + 2 * 4);
      const unsigned int expire = cares_get_32bit(ptr + 3 * 4);
      const unsigned int minttl = cares_get_32bit(ptr + 4 * 4);

      Local<Object> soa_record = Object::New(env->isolate());
      soa_record->Set(context,
                      env->nsname_string(),
                      OneByteString(env->isolate(), nsname.get())).FromJust();
      soa_record->Set(context,
                      env->hostmaster_string(),
                      OneByteString(env->isolate(),
                                    hostmaster.get())).FromJust();
      soa_record->Set(context,
                      env->serial_string(),
                      Integer::New(env->isolate(), serial)).FromJust();
      soa_record->Set(context,
                      env->refresh_string(),
                      Integer::New(env->isolate(), refresh)).FromJust();
      soa_record->Set(context,
                      env->retry_string(),
                      Integer::New(env->isolate(), retry)).FromJust();
      soa_record->Set(context,
                      env->expire_string(),
                      Integer::New(env->isolate(), expire)).FromJust();
      soa_record->Set(context,
                      env->minttl_string(),
                      Integer::New(env->isolate(), minttl)).FromJust();
      soa_record->Set(context,
                      env->type_string(),
                      env->dns_soa_string()).FromJust();


      *ret = handle_scope.Escape(soa_record);
      break;
    }

    ptr += rr_len;
  }

  return ARES_SUCCESS;
}


class QueryAnyWrap: public QueryWrap {
 public:
  QueryAnyWrap(ChannelWrap* channel, Local<Object> req_wrap_obj)
    : QueryWrap(channel, req_wrap_obj, "resolveAny") {
  }

  int Send(const char* name) override {
    AresQuery(name, ns_c_in, ns_t_any);
    return 0;
  }

  SET_NO_MEMORY_INFO()
  SET_MEMORY_INFO_NAME(QueryAnyWrap)
  SET_SELF_SIZE(QueryAnyWrap)

 protected:
  void Parse(unsigned char* buf, int len) override {
    HandleScope handle_scope(env()->isolate());
    auto context = env()->context();
    Context::Scope context_scope(context);

    Local<Array> ret = Array::New(env()->isolate());
    int type, status, old_count;

    /* Parse A records or CNAME records */
    ares_addrttl addrttls[256];
    int naddrttls = arraysize(addrttls);

    type = ns_t_cname_or_a;
    status = ParseGeneralReply(env(),
                               buf,
                               len,
                               &type,
                               ret,
                               addrttls,
                               &naddrttls);
    uint32_t a_count = ret->Length();
    if (status != ARES_SUCCESS && status != ARES_ENODATA) {
      ParseError(status);
      return;
    }

    if (type == ns_t_a) {
      CHECK_EQ(static_cast<uint32_t>(naddrttls), a_count);
      for (uint32_t i = 0; i < a_count; i++) {
        Local<Object> obj = Object::New(env()->isolate());
        obj->Set(context,
                 env()->address_string(),
                 ret->Get(context, i).ToLocalChecked()).FromJust();
        obj->Set(context,
                 env()->ttl_string(),
                 Integer::New(env()->isolate(), addrttls[i].ttl)).FromJust();
        obj->Set(context,
                 env()->type_string(),
                 env()->dns_a_string()).FromJust();
        ret->Set(context, i, obj).FromJust();
      }
    } else {
      for (uint32_t i = 0; i < a_count; i++) {
        Local<Object> obj = Object::New(env()->isolate());
        obj->Set(context,
                 env()->value_string(),
                 ret->Get(context, i).ToLocalChecked()).FromJust();
        obj->Set(context,
                 env()->type_string(),
                 env()->dns_cname_string()).FromJust();
        ret->Set(context, i, obj).FromJust();
      }
    }

    /* Parse AAAA records */
    ares_addr6ttl addr6ttls[256];
    int naddr6ttls = arraysize(addr6ttls);

    type = ns_t_aaaa;
    status = ParseGeneralReply(env(),
                               buf,
                               len,
                               &type,
                               ret,
                               addr6ttls,
                               &naddr6ttls);
    uint32_t aaaa_count = ret->Length() - a_count;
    if (status != ARES_SUCCESS && status != ARES_ENODATA) {
      ParseError(status);
      return;
    }

    CHECK_EQ(aaaa_count, static_cast<uint32_t>(naddr6ttls));
    CHECK_EQ(ret->Length(), a_count + aaaa_count);
    for (uint32_t i = a_count; i < ret->Length(); i++) {
      Local<Object> obj = Object::New(env()->isolate());
      obj->Set(context,
               env()->address_string(),
               ret->Get(context, i).ToLocalChecked()).FromJust();
      obj->Set(context,
               env()->ttl_string(),
               Integer::New(env()->isolate(), addr6ttls[i - a_count].ttl))
          .FromJust();
      obj->Set(context,
               env()->type_string(),
               env()->dns_aaaa_string()).FromJust();
      ret->Set(context, i, obj).FromJust();
    }

    /* Parse MX records */
    status = ParseMxReply(env(), buf, len, ret, true);
    if (status != ARES_SUCCESS && status != ARES_ENODATA) {
      ParseError(status);
      return;
    }

    /* Parse NS records */
    type = ns_t_ns;
    old_count = ret->Length();
    status = ParseGeneralReply(env(), buf, len, &type, ret);
    if (status != ARES_SUCCESS && status != ARES_ENODATA) {
      ParseError(status);
      return;
    }
    for (uint32_t i = old_count; i < ret->Length(); i++) {
      Local<Object> obj = Object::New(env()->isolate());
      obj->Set(context,
               env()->value_string(),
               ret->Get(context, i).ToLocalChecked()).FromJust();
      obj->Set(context,
               env()->type_string(),
               env()->dns_ns_string()).FromJust();
      ret->Set(context, i, obj).FromJust();
    }

    /* Parse TXT records */
    status = ParseTxtReply(env(), buf, len, ret, true);
    if (status != ARES_SUCCESS && status != ARES_ENODATA) {
      ParseError(status);
      return;
    }

    /* Parse SRV records */
    status = ParseSrvReply(env(), buf, len, ret, true);
    if (status != ARES_SUCCESS && status != ARES_ENODATA) {
      return;
    }

    /* Parse PTR records */
    type = ns_t_ptr;
    old_count = ret->Length();
    status = ParseGeneralReply(env(), buf, len, &type, ret);
    for (uint32_t i = old_count; i < ret->Length(); i++) {
      Local<Object> obj = Object::New(env()->isolate());
      obj->Set(context,
               env()->value_string(),
               ret->Get(context, i).ToLocalChecked()).FromJust();
      obj->Set(context,
               env()->type_string(),
               env()->dns_ptr_string()).FromJust();
      ret->Set(context, i, obj).FromJust();
    }

    /* Parse NAPTR records */
    status = ParseNaptrReply(env(), buf, len, ret, true);
    if (status != ARES_SUCCESS && status != ARES_ENODATA) {
      ParseError(status);
      return;
    }

    /* Parse SOA records */
    Local<Object> soa_record = Local<Object>();
    status = ParseSoaReply(env(), buf, len, &soa_record);
    if (status != ARES_SUCCESS && status != ARES_ENODATA) {
      ParseError(status);
      return;
    }
    if (!soa_record.IsEmpty())
      ret->Set(context, ret->Length(), soa_record).FromJust();

    CallOnComplete(ret);
  }
};


class QueryAWrap: public QueryWrap {
 public:
  QueryAWrap(ChannelWrap* channel, Local<Object> req_wrap_obj)
      : QueryWrap(channel, req_wrap_obj, "resolve4") {
  }

  int Send(const char* name) override {
    AresQuery(name, ns_c_in, ns_t_a);
    return 0;
  }

  SET_NO_MEMORY_INFO()
  SET_MEMORY_INFO_NAME(QueryAWrap)
  SET_SELF_SIZE(QueryAWrap)

 protected:
  void Parse(unsigned char* buf, int len) override {
    HandleScope handle_scope(env()->isolate());
    Context::Scope context_scope(env()->context());

    ares_addrttl addrttls[256];
    int naddrttls = arraysize(addrttls), status;
    Local<Array> ret = Array::New(env()->isolate());

    int type = ns_t_a;
    status = ParseGeneralReply(env(),
                               buf,
                               len,
                               &type,
                               ret,
                               addrttls,
                               &naddrttls);
    if (status != ARES_SUCCESS) {
      ParseError(status);
      return;
    }

    Local<Array> ttls = AddrTTLToArray<ares_addrttl>(env(),
                                                     addrttls,
                                                     naddrttls);

    CallOnComplete(ret, ttls);
  }
};


class QueryAaaaWrap: public QueryWrap {
 public:
  QueryAaaaWrap(ChannelWrap* channel, Local<Object> req_wrap_obj)
      : QueryWrap(channel, req_wrap_obj, "resolve6") {
  }

  int Send(const char* name) override {
    AresQuery(name, ns_c_in, ns_t_aaaa);
    return 0;
  }

  SET_NO_MEMORY_INFO()
  SET_MEMORY_INFO_NAME(QueryAaaaWrap)
  SET_SELF_SIZE(QueryAaaaWrap)

 protected:
  void Parse(unsigned char* buf, int len) override {
    HandleScope handle_scope(env()->isolate());
    Context::Scope context_scope(env()->context());

    ares_addr6ttl addrttls[256];
    int naddrttls = arraysize(addrttls), status;
    Local<Array> ret = Array::New(env()->isolate());

    int type = ns_t_aaaa;
    status = ParseGeneralReply(env(),
                               buf,
                               len,
                               &type,
                               ret,
                               addrttls,
                               &naddrttls);
    if (status != ARES_SUCCESS) {
      ParseError(status);
      return;
    }

    Local<Array> ttls = AddrTTLToArray<ares_addr6ttl>(env(),
                                                      addrttls,
                                                      naddrttls);

    CallOnComplete(ret, ttls);
  }
};


class QueryCnameWrap: public QueryWrap {
 public:
  QueryCnameWrap(ChannelWrap* channel, Local<Object> req_wrap_obj)
      : QueryWrap(channel, req_wrap_obj, "resolveCname") {
  }

  int Send(const char* name) override {
    AresQuery(name, ns_c_in, ns_t_cname);
    return 0;
  }

  SET_NO_MEMORY_INFO()
  SET_MEMORY_INFO_NAME(QueryCnameWrap)
  SET_SELF_SIZE(QueryCnameWrap)

 protected:
  void Parse(unsigned char* buf, int len) override {
    HandleScope handle_scope(env()->isolate());
    Context::Scope context_scope(env()->context());

    Local<Array> ret = Array::New(env()->isolate());
    int type = ns_t_cname;
    int status = ParseGeneralReply(env(), buf, len, &type, ret);
    if (status != ARES_SUCCESS) {
      ParseError(status);
      return;
    }

    this->CallOnComplete(ret);
  }
};


class QueryMxWrap: public QueryWrap {
 public:
  QueryMxWrap(ChannelWrap* channel, Local<Object> req_wrap_obj)
      : QueryWrap(channel, req_wrap_obj, "resolveMx") {
  }

  int Send(const char* name) override {
    AresQuery(name, ns_c_in, ns_t_mx);
    return 0;
  }

  SET_NO_MEMORY_INFO()
  SET_MEMORY_INFO_NAME(QueryMxWrap)
  SET_SELF_SIZE(QueryMxWrap)

 protected:
  void Parse(unsigned char* buf, int len) override {
    HandleScope handle_scope(env()->isolate());
    Context::Scope context_scope(env()->context());

    Local<Array> mx_records = Array::New(env()->isolate());
    int status = ParseMxReply(env(), buf, len, mx_records);

    if (status != ARES_SUCCESS) {
      ParseError(status);
      return;
    }

    this->CallOnComplete(mx_records);
  }
};


class QueryNsWrap: public QueryWrap {
 public:
  QueryNsWrap(ChannelWrap* channel, Local<Object> req_wrap_obj)
      : QueryWrap(channel, req_wrap_obj, "resolveNs") {
  }

  int Send(const char* name) override {
    AresQuery(name, ns_c_in, ns_t_ns);
    return 0;
  }

  SET_NO_MEMORY_INFO()
  SET_MEMORY_INFO_NAME(QueryNsWrap)
  SET_SELF_SIZE(QueryNsWrap)

 protected:
  void Parse(unsigned char* buf, int len) override {
    HandleScope handle_scope(env()->isolate());
    Context::Scope context_scope(env()->context());

    int type = ns_t_ns;
    Local<Array> names = Array::New(env()->isolate());
    int status = ParseGeneralReply(env(), buf, len, &type, names);
    if (status != ARES_SUCCESS) {
      ParseError(status);
      return;
    }

    this->CallOnComplete(names);
  }
};


class QueryTxtWrap: public QueryWrap {
 public:
  QueryTxtWrap(ChannelWrap* channel, Local<Object> req_wrap_obj)
      : QueryWrap(channel, req_wrap_obj, "resolveTxt") {
  }

  int Send(const char* name) override {
    AresQuery(name, ns_c_in, ns_t_txt);
    return 0;
  }

  SET_NO_MEMORY_INFO()
  SET_MEMORY_INFO_NAME(QueryTxtWrap)
  SET_SELF_SIZE(QueryTxtWrap)

 protected:
  void Parse(unsigned char* buf, int len) override {
    HandleScope handle_scope(env()->isolate());
    Context::Scope context_scope(env()->context());

    Local<Array> txt_records = Array::New(env()->isolate());
    int status = ParseTxtReply(env(), buf, len, txt_records);
    if (status != ARES_SUCCESS) {
      ParseError(status);
      return;
    }

    this->CallOnComplete(txt_records);
  }
};


class QuerySrvWrap: public QueryWrap {
 public:
  explicit QuerySrvWrap(ChannelWrap* channel, Local<Object> req_wrap_obj)
      : QueryWrap(channel, req_wrap_obj, "resolveSrv") {
  }

  int Send(const char* name) override {
    AresQuery(name, ns_c_in, ns_t_srv);
    return 0;
  }

  SET_NO_MEMORY_INFO()
  SET_MEMORY_INFO_NAME(QuerySrvWrap)
  SET_SELF_SIZE(QuerySrvWrap)

 protected:
  void Parse(unsigned char* buf, int len) override {
    HandleScope handle_scope(env()->isolate());
    Context::Scope context_scope(env()->context());

    Local<Array> srv_records = Array::New(env()->isolate());
    int status = ParseSrvReply(env(), buf, len, srv_records);
    if (status != ARES_SUCCESS) {
      ParseError(status);
      return;
    }

    this->CallOnComplete(srv_records);
  }
};

class QueryPtrWrap: public QueryWrap {
 public:
  explicit QueryPtrWrap(ChannelWrap* channel, Local<Object> req_wrap_obj)
      : QueryWrap(channel, req_wrap_obj, "resolvePtr") {
  }

  int Send(const char* name) override {
    AresQuery(name, ns_c_in, ns_t_ptr);
    return 0;
  }

  SET_NO_MEMORY_INFO()
  SET_MEMORY_INFO_NAME(QueryPtrWrap)
  SET_SELF_SIZE(QueryPtrWrap)

 protected:
  void Parse(unsigned char* buf, int len) override {
    HandleScope handle_scope(env()->isolate());
    Context::Scope context_scope(env()->context());

    int type = ns_t_ptr;
    Local<Array> aliases = Array::New(env()->isolate());

    int status = ParseGeneralReply(env(), buf, len, &type, aliases);
    if (status != ARES_SUCCESS) {
      ParseError(status);
      return;
    }

    this->CallOnComplete(aliases);
  }
};

class QueryNaptrWrap: public QueryWrap {
 public:
  explicit QueryNaptrWrap(ChannelWrap* channel, Local<Object> req_wrap_obj)
      : QueryWrap(channel, req_wrap_obj, "resolveNaptr") {
  }

  int Send(const char* name) override {
    AresQuery(name, ns_c_in, ns_t_naptr);
    return 0;
  }

  SET_NO_MEMORY_INFO()
  SET_MEMORY_INFO_NAME(QueryNaptrWrap)
  SET_SELF_SIZE(QueryNaptrWrap)

 protected:
  void Parse(unsigned char* buf, int len) override {
    HandleScope handle_scope(env()->isolate());
    Context::Scope context_scope(env()->context());

    Local<Array> naptr_records = Array::New(env()->isolate());
    int status = ParseNaptrReply(env(), buf, len, naptr_records);
    if (status != ARES_SUCCESS) {
      ParseError(status);
      return;
    }

    this->CallOnComplete(naptr_records);
  }
};


class QuerySoaWrap: public QueryWrap {
 public:
  QuerySoaWrap(ChannelWrap* channel, Local<Object> req_wrap_obj)
      : QueryWrap(channel, req_wrap_obj, "resolveSoa") {
  }

  int Send(const char* name) override {
    AresQuery(name, ns_c_in, ns_t_soa);
    return 0;
  }

  SET_NO_MEMORY_INFO()
  SET_MEMORY_INFO_NAME(QuerySoaWrap)
  SET_SELF_SIZE(QuerySoaWrap)

 protected:
  void Parse(unsigned char* buf, int len) override {
    HandleScope handle_scope(env()->isolate());
    auto context = env()->context();
    Context::Scope context_scope(context);

    ares_soa_reply* soa_out;
    int status = ares_parse_soa_reply(buf, len, &soa_out);

    if (status != ARES_SUCCESS) {
      ParseError(status);
      return;
    }

    Local<Object> soa_record = Object::New(env()->isolate());

    soa_record->Set(context,
                    env()->nsname_string(),
                    OneByteString(env()->isolate(),
                                  soa_out->nsname)).FromJust();
    soa_record->Set(context,
                    env()->hostmaster_string(),
                    OneByteString(env()->isolate(),
                                  soa_out->hostmaster)).FromJust();
    soa_record->Set(context,
                    env()->serial_string(),
                    Integer::New(env()->isolate(), soa_out->serial)).FromJust();
    soa_record->Set(context,
                    env()->refresh_string(),
                    Integer::New(env()->isolate(),
                                 soa_out->refresh)).FromJust();
    soa_record->Set(context,
                    env()->retry_string(),
                    Integer::New(env()->isolate(), soa_out->retry)).FromJust();
    soa_record->Set(context,
                    env()->expire_string(),
                    Integer::New(env()->isolate(), soa_out->expire)).FromJust();
    soa_record->Set(context,
                    env()->minttl_string(),
                    Integer::New(env()->isolate(), soa_out->minttl)).FromJust();

    ares_free_data(soa_out);

    this->CallOnComplete(soa_record);
  }
};


class GetHostByAddrWrap: public QueryWrap {
 public:
  explicit GetHostByAddrWrap(ChannelWrap* channel, Local<Object> req_wrap_obj)
      : QueryWrap(channel, req_wrap_obj, "reverse") {
  }

  int Send(const char* name) override {
    int length, family;
    char address_buffer[sizeof(struct in6_addr)];

    if (uv_inet_pton(AF_INET, name, &address_buffer) == 0) {
      length = sizeof(struct in_addr);
      family = AF_INET;
    } else if (uv_inet_pton(AF_INET6, name, &address_buffer) == 0) {
      length = sizeof(struct in6_addr);
      family = AF_INET6;
    } else {
      return UV_EINVAL;  // So errnoException() reports a proper error.
    }

    TRACE_EVENT_NESTABLE_ASYNC_BEGIN2(
        TRACING_CATEGORY_NODE2(dns, native), "reverse", this,
        "name", TRACE_STR_COPY(name),
        "family", family == AF_INET ? "ipv4" : "ipv6");

    ares_gethostbyaddr(channel_->cares_channel(),
                       address_buffer,
                       length,
                       family,
                       Callback,
                       MakeCallbackPointer());
    return 0;
  }

  SET_NO_MEMORY_INFO()
  SET_MEMORY_INFO_NAME(GetHostByAddrWrap)
  SET_SELF_SIZE(GetHostByAddrWrap)

 protected:
  void Parse(struct hostent* host) override {
    HandleScope handle_scope(env()->isolate());
    Context::Scope context_scope(env()->context());
    this->CallOnComplete(HostentToNames(env(), host));
  }
};


template <class Wrap>
static void Query(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  ChannelWrap* channel;
  ASSIGN_OR_RETURN_UNWRAP(&channel, args.Holder());

  CHECK_EQ(false, args.IsConstructCall());
  CHECK(args[0]->IsObject());
  CHECK(args[1]->IsString());

  Local<Object> req_wrap_obj = args[0].As<Object>();
  Local<String> string = args[1].As<String>();
  auto wrap = std::make_unique<Wrap>(channel, req_wrap_obj);

  node::Utf8Value name(env->isolate(), string);
  channel->ModifyActivityQueryCount(1);
  int err = wrap->Send(*name);
  if (err) {
    channel->ModifyActivityQueryCount(-1);
  } else {
    // Release ownership of the pointer allowing the ownership to be transferred
    USE(wrap.release());
  }

  args.GetReturnValue().Set(err);
}


void AfterGetAddrInfo(uv_getaddrinfo_t* req, int status, struct addrinfo* res) {
  std::unique_ptr<GetAddrInfoReqWrap> req_wrap {
      static_cast<GetAddrInfoReqWrap*>(req->data)};
  Environment* env = req_wrap->env();

  HandleScope handle_scope(env->isolate());
  Context::Scope context_scope(env->context());

  Local<Value> argv[] = {
    Integer::New(env->isolate(), status),
    Null(env->isolate())
  };

  uint64_t n = 0;
  const bool verbatim = req_wrap->verbatim();

  if (status == 0) {
    Local<Array> results = Array::New(env->isolate());

    auto add = [&] (bool want_ipv4, bool want_ipv6) {
      for (auto p = res; p != nullptr; p = p->ai_next) {
        CHECK_EQ(p->ai_socktype, SOCK_STREAM);

        const char* addr;
        if (want_ipv4 && p->ai_family == AF_INET) {
          addr = reinterpret_cast<char*>(
              &(reinterpret_cast<struct sockaddr_in*>(p->ai_addr)->sin_addr));
        } else if (want_ipv6 && p->ai_family == AF_INET6) {
          addr = reinterpret_cast<char*>(
              &(reinterpret_cast<struct sockaddr_in6*>(p->ai_addr)->sin6_addr));
        } else {
          continue;
        }

        char ip[INET6_ADDRSTRLEN];
        if (uv_inet_ntop(p->ai_family, addr, ip, sizeof(ip)))
          continue;

        Local<String> s = OneByteString(env->isolate(), ip);
        results->Set(env->context(), n, s).FromJust();
        n++;
      }
    };

    add(true, verbatim);
    if (verbatim == false)
      add(false, true);

    // No responses were found to return
    if (n == 0) {
      argv[0] = Integer::New(env->isolate(), UV_EAI_NODATA);
    }

    argv[1] = results;
  }

  uv_freeaddrinfo(res);

  TRACE_EVENT_NESTABLE_ASYNC_END2(
      TRACING_CATEGORY_NODE2(dns, native), "lookup", req_wrap.get(),
      "count", n, "verbatim", verbatim);

  // Make the callback into JavaScript
  req_wrap->MakeCallback(env->oncomplete_string(), arraysize(argv), argv);
}


void AfterGetNameInfo(uv_getnameinfo_t* req,
                      int status,
                      const char* hostname,
                      const char* service) {
  std::unique_ptr<GetNameInfoReqWrap> req_wrap {
      static_cast<GetNameInfoReqWrap*>(req->data)};
  Environment* env = req_wrap->env();

  HandleScope handle_scope(env->isolate());
  Context::Scope context_scope(env->context());

  Local<Value> argv[] = {
    Integer::New(env->isolate(), status),
    Null(env->isolate()),
    Null(env->isolate())
  };

  if (status == 0) {
    // Success
    Local<String> js_hostname = OneByteString(env->isolate(), hostname);
    Local<String> js_service = OneByteString(env->isolate(), service);
    argv[1] = js_hostname;
    argv[2] = js_service;
  }

  TRACE_EVENT_NESTABLE_ASYNC_END2(
      TRACING_CATEGORY_NODE2(dns, native), "lookupService", req_wrap.get(),
      "hostname", TRACE_STR_COPY(hostname),
      "service", TRACE_STR_COPY(service));

  // Make the callback into JavaScript
  req_wrap->MakeCallback(env->oncomplete_string(), arraysize(argv), argv);
}

using ParseIPResult =
    decltype(static_cast<ares_addr_port_node*>(nullptr)->addr);

int ParseIP(const char* ip, ParseIPResult* result = nullptr) {
  ParseIPResult tmp;
  if (result == nullptr) result = &tmp;
  if (0 == uv_inet_pton(AF_INET, ip, result)) return 4;
  if (0 == uv_inet_pton(AF_INET6, ip, result)) return 6;
  return 0;
}

void CanonicalizeIP(const FunctionCallbackInfo<Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  node::Utf8Value ip(isolate, args[0]);

  ParseIPResult result;
  const int rc = ParseIP(*ip, &result);
  if (rc == 0) return;

  char canonical_ip[INET6_ADDRSTRLEN];
  const int af = (rc == 4 ? AF_INET : AF_INET6);
  CHECK_EQ(0, uv_inet_ntop(af, &result, canonical_ip, sizeof(canonical_ip)));
  v8::Local<String> val = String::NewFromUtf8(isolate, canonical_ip,
      v8::NewStringType::kNormal).ToLocalChecked();
  args.GetReturnValue().Set(val);
}

void GetAddrInfo(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  CHECK(args[0]->IsObject());
  CHECK(args[1]->IsString());
  CHECK(args[2]->IsInt32());
  CHECK(args[4]->IsBoolean());
  Local<Object> req_wrap_obj = args[0].As<Object>();
  node::Utf8Value hostname(env->isolate(), args[1]);

  int32_t flags = 0;
  if (args[3]->IsInt32()) {
    flags = args[3].As<Int32>()->Value();
  }

  int family;

  switch (args[2].As<Int32>()->Value()) {
    case 0:
      family = AF_UNSPEC;
      break;
    case 4:
      family = AF_INET;
      break;
    case 6:
      family = AF_INET6;
      break;
    default:
      CHECK(0 && "bad address family");
  }

  auto req_wrap = std::make_unique<GetAddrInfoReqWrap>(env,
                                                       req_wrap_obj,
                                                       args[4]->IsTrue());

  struct addrinfo hints;
  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_family = family;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = flags;

  TRACE_EVENT_NESTABLE_ASYNC_BEGIN2(
      TRACING_CATEGORY_NODE2(dns, native), "lookup", req_wrap.get(),
      "hostname", TRACE_STR_COPY(*hostname),
      "family",
      family == AF_INET ? "ipv4" : family == AF_INET6 ? "ipv6" : "unspec");

  int err = req_wrap->Dispatch(uv_getaddrinfo,
                               AfterGetAddrInfo,
                               *hostname,
                               nullptr,
                               &hints);
  if (err == 0)
    // Release ownership of the pointer allowing the ownership to be transferred
    USE(req_wrap.release());

  args.GetReturnValue().Set(err);
}


void GetNameInfo(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  CHECK(args[0]->IsObject());
  CHECK(args[1]->IsString());
  CHECK(args[2]->IsUint32());
  Local<Object> req_wrap_obj = args[0].As<Object>();
  node::Utf8Value ip(env->isolate(), args[1]);
  const unsigned port = args[2]->Uint32Value(env->context()).FromJust();
  struct sockaddr_storage addr;

  CHECK(uv_ip4_addr(*ip, port, reinterpret_cast<sockaddr_in*>(&addr)) == 0 ||
        uv_ip6_addr(*ip, port, reinterpret_cast<sockaddr_in6*>(&addr)) == 0);

  auto req_wrap = std::make_unique<GetNameInfoReqWrap>(env, req_wrap_obj);

  TRACE_EVENT_NESTABLE_ASYNC_BEGIN2(
      TRACING_CATEGORY_NODE2(dns, native), "lookupService", req_wrap.get(),
      "ip", TRACE_STR_COPY(*ip), "port", port);

  int err = req_wrap->Dispatch(uv_getnameinfo,
                               AfterGetNameInfo,
                               reinterpret_cast<struct sockaddr*>(&addr),
                               NI_NAMEREQD);
  if (err == 0)
    // Release ownership of the pointer allowing the ownership to be transferred
    USE(req_wrap.release());

  args.GetReturnValue().Set(err);
}


void GetServers(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  ChannelWrap* channel;
  ASSIGN_OR_RETURN_UNWRAP(&channel, args.Holder());

  Local<Array> server_array = Array::New(env->isolate());

  ares_addr_port_node* servers;

  int r = ares_get_servers_ports(channel->cares_channel(), &servers);
  CHECK_EQ(r, ARES_SUCCESS);

  ares_addr_port_node* cur = servers;

  for (uint32_t i = 0; cur != nullptr; ++i, cur = cur->next) {
    char ip[INET6_ADDRSTRLEN];

    const void* caddr = static_cast<const void*>(&cur->addr);
    int err = uv_inet_ntop(cur->family, caddr, ip, sizeof(ip));
    CHECK_EQ(err, 0);

    Local<Array> ret = Array::New(env->isolate(), 2);
    ret->Set(env->context(), 0, OneByteString(env->isolate(), ip)).FromJust();
    ret->Set(env->context(),
             1,
             Integer::New(env->isolate(), cur->udp_port)).FromJust();

    server_array->Set(env->context(), i, ret).FromJust();
  }

  ares_free_data(servers);

  args.GetReturnValue().Set(server_array);
}


void SetServers(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  ChannelWrap* channel;
  ASSIGN_OR_RETURN_UNWRAP(&channel, args.Holder());

  if (channel->active_query_count()) {
    return args.GetReturnValue().Set(DNS_ESETSRVPENDING);
  }

  CHECK(args[0]->IsArray());

  Local<Array> arr = Local<Array>::Cast(args[0]);

  uint32_t len = arr->Length();

  if (len == 0) {
    int rv = ares_set_servers(channel->cares_channel(), nullptr);
    return args.GetReturnValue().Set(rv);
  }

  std::vector<ares_addr_port_node> servers(len);
  ares_addr_port_node* last = nullptr;

  int err;

  for (uint32_t i = 0; i < len; i++) {
    CHECK(arr->Get(env->context(), i).ToLocalChecked()->IsArray());

    Local<Array> elm =
        Local<Array>::Cast(arr->Get(env->context(), i).ToLocalChecked());

    CHECK(elm->Get(env->context(),
                   0).ToLocalChecked()->Int32Value(env->context()).FromJust());
    CHECK(elm->Get(env->context(), 1).ToLocalChecked()->IsString());
    CHECK(elm->Get(env->context(),
                   2).ToLocalChecked()->Int32Value(env->context()).FromJust());

    int fam = elm->Get(env->context(), 0)
        .ToLocalChecked()->Int32Value(env->context()).FromJust();
    node::Utf8Value ip(env->isolate(),
                       elm->Get(env->context(), 1).ToLocalChecked());
    int port = elm->Get(env->context(), 2)
        .ToLocalChecked()->Int32Value(env->context()).FromJust();

    ares_addr_port_node* cur = &servers[i];

    cur->tcp_port = cur->udp_port = port;
    switch (fam) {
      case 4:
        cur->family = AF_INET;
        err = uv_inet_pton(AF_INET, *ip, &cur->addr);
        break;
      case 6:
        cur->family = AF_INET6;
        err = uv_inet_pton(AF_INET6, *ip, &cur->addr);
        break;
      default:
        CHECK(0 && "Bad address family.");
    }

    if (err)
      break;

    cur->next = nullptr;

    if (last != nullptr)
      last->next = cur;

    last = cur;
  }

  if (err == 0)
    err = ares_set_servers_ports(channel->cares_channel(), &servers[0]);
  else
    err = ARES_EBADSTR;

  if (err == ARES_SUCCESS)
    channel->set_is_servers_default(false);

  args.GetReturnValue().Set(err);
}

void Cancel(const FunctionCallbackInfo<Value>& args) {
  ChannelWrap* channel;
  ASSIGN_OR_RETURN_UNWRAP(&channel, args.Holder());

  TRACE_EVENT_INSTANT0(TRACING_CATEGORY_NODE2(dns, native),
      "cancel", TRACE_EVENT_SCOPE_THREAD);

  ares_cancel(channel->cares_channel());
}

const char EMSG_ESETSRVPENDING[] = "There are pending queries.";
void StrError(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  int code = args[0]->Int32Value(env->context()).FromJust();
  const char* errmsg = (code == DNS_ESETSRVPENDING) ?
    EMSG_ESETSRVPENDING :
    ares_strerror(code);
  args.GetReturnValue().Set(OneByteString(env->isolate(), errmsg));
}


void Initialize(Local<Object> target,
                Local<Value> unused,
                Local<Context> context,
                void* priv) {
  Environment* env = Environment::GetCurrent(context);

  env->SetMethod(target, "getaddrinfo", GetAddrInfo);
  env->SetMethod(target, "getnameinfo", GetNameInfo);
  env->SetMethodNoSideEffect(target, "canonicalizeIP", CanonicalizeIP);

  env->SetMethod(target, "strerror", StrError);

  target->Set(env->context(), FIXED_ONE_BYTE_STRING(env->isolate(), "AF_INET"),
              Integer::New(env->isolate(), AF_INET)).FromJust();
  target->Set(env->context(), FIXED_ONE_BYTE_STRING(env->isolate(), "AF_INET6"),
              Integer::New(env->isolate(), AF_INET6)).FromJust();
  target->Set(env->context(), FIXED_ONE_BYTE_STRING(env->isolate(),
                                                    "AF_UNSPEC"),
              Integer::New(env->isolate(), AF_UNSPEC)).FromJust();
  target->Set(env->context(), FIXED_ONE_BYTE_STRING(env->isolate(),
                                                    "AI_ADDRCONFIG"),
              Integer::New(env->isolate(), AI_ADDRCONFIG)).FromJust();
  target->Set(env->context(), FIXED_ONE_BYTE_STRING(env->isolate(),
                                                    "AI_V4MAPPED"),
              Integer::New(env->isolate(), AI_V4MAPPED)).FromJust();

  Local<FunctionTemplate> aiw =
      BaseObject::MakeLazilyInitializedJSTemplate(env);
  aiw->Inherit(AsyncWrap::GetConstructorTemplate(env));
  Local<String> addrInfoWrapString =
      FIXED_ONE_BYTE_STRING(env->isolate(), "GetAddrInfoReqWrap");
  aiw->SetClassName(addrInfoWrapString);
  target->Set(env->context(),
              addrInfoWrapString,
              aiw->GetFunction(context).ToLocalChecked()).FromJust();

  Local<FunctionTemplate> niw =
      BaseObject::MakeLazilyInitializedJSTemplate(env);
  niw->Inherit(AsyncWrap::GetConstructorTemplate(env));
  Local<String> nameInfoWrapString =
      FIXED_ONE_BYTE_STRING(env->isolate(), "GetNameInfoReqWrap");
  niw->SetClassName(nameInfoWrapString);
  target->Set(env->context(),
              nameInfoWrapString,
              niw->GetFunction(context).ToLocalChecked()).FromJust();

  Local<FunctionTemplate> qrw =
      BaseObject::MakeLazilyInitializedJSTemplate(env);
  qrw->Inherit(AsyncWrap::GetConstructorTemplate(env));
  Local<String> queryWrapString =
      FIXED_ONE_BYTE_STRING(env->isolate(), "QueryReqWrap");
  qrw->SetClassName(queryWrapString);
  target->Set(env->context(),
              queryWrapString,
              qrw->GetFunction(context).ToLocalChecked()).FromJust();

  Local<FunctionTemplate> channel_wrap =
      env->NewFunctionTemplate(ChannelWrap::New);
  channel_wrap->InstanceTemplate()->SetInternalFieldCount(1);
  channel_wrap->Inherit(AsyncWrap::GetConstructorTemplate(env));

  env->SetProtoMethod(channel_wrap, "queryAny", Query<QueryAnyWrap>);
  env->SetProtoMethod(channel_wrap, "queryA", Query<QueryAWrap>);
  env->SetProtoMethod(channel_wrap, "queryAaaa", Query<QueryAaaaWrap>);
  env->SetProtoMethod(channel_wrap, "queryCname", Query<QueryCnameWrap>);
  env->SetProtoMethod(channel_wrap, "queryMx", Query<QueryMxWrap>);
  env->SetProtoMethod(channel_wrap, "queryNs", Query<QueryNsWrap>);
  env->SetProtoMethod(channel_wrap, "queryTxt", Query<QueryTxtWrap>);
  env->SetProtoMethod(channel_wrap, "querySrv", Query<QuerySrvWrap>);
  env->SetProtoMethod(channel_wrap, "queryPtr", Query<QueryPtrWrap>);
  env->SetProtoMethod(channel_wrap, "queryNaptr", Query<QueryNaptrWrap>);
  env->SetProtoMethod(channel_wrap, "querySoa", Query<QuerySoaWrap>);
  env->SetProtoMethod(channel_wrap, "getHostByAddr", Query<GetHostByAddrWrap>);

  env->SetProtoMethodNoSideEffect(channel_wrap, "getServers", GetServers);
  env->SetProtoMethod(channel_wrap, "setServers", SetServers);
  env->SetProtoMethod(channel_wrap, "cancel", Cancel);

  Local<String> channelWrapString =
      FIXED_ONE_BYTE_STRING(env->isolate(), "ChannelWrap");
  channel_wrap->SetClassName(channelWrapString);
  target->Set(env->context(), channelWrapString,
              channel_wrap->GetFunction(context).ToLocalChecked()).FromJust();
}

}  // anonymous namespace
}  // namespace cares_wrap
}  // namespace node

NODE_MODULE_CONTEXT_AWARE_INTERNAL(cares_wrap, node::cares_wrap::Initialize)
