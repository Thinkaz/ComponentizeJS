#include "embedding.h"
#include "debugger.h"
#include "builtins/web/performance.h"

namespace builtins::web::console {

class Console : public BuiltinNoConstructor<Console> {
private:
public:
  static constexpr const char *class_name = "Console";
  enum LogType {
    Log,
    Info,
    Debug,
    Warn,
    Error,
  };
  enum Slots { Count };
  static const JSFunctionSpec methods[];
  static const JSPropertySpec properties[];
};

void builtin_impl_console_log(Console::LogType log_ty, const char *msg) {
  if (log_ty == Console::LogType::Log || log_ty == Console::LogType::Info) {
    fprintf(stdout, "%s\n", msg);
    fflush(stdout);
  } else {
    fprintf(stderr, "%s\n", msg);
    fflush(stderr);
  }
}

} // namespace builtins::web::console

namespace {

using componentize::embedding::Runtime;

bool call_then_handler(JSContext *cx, JS::HandleObject receiver,
                       JS::HandleValue extra, JS::CallArgs args) {
  LOG("(call) call then handler");
  Runtime.engine->decr_event_loop_interest();
  return true;
}

bool call_catch_handler(JSContext *cx, JS::HandleObject receiver,
                        JS::HandleValue extra, JS::CallArgs args) {
  LOG("(call) call catch handler");
  Runtime.engine->decr_event_loop_interest();
  Runtime.engine->dump_error(args.get(0), stderr);
  return false;
}

} // namespace

extern "C" {
using componentize::embedding::cabi_free;
using componentize::embedding::ComponentizeRuntime;
using componentize::embedding::CoreVal;
using componentize::embedding::ReportAndClearException;
using componentize::embedding::Runtime;

// These functions are used both internally and also exported for use directly
// by the splicer codegen
__attribute__((noinline, export_name("coreabi_from_bigint64"))) int64_t
from_bigint64(JS::MutableHandleValue handle) {
  JS::BigInt *arg0 = handle.toBigInt();
  uint64_t arg0_uint64;
  if (!JS::detail::BigIntIsUint64(arg0, &arg0_uint64)) {
    Runtime.engine->abort(
        "Internal bindgen error in coreabi_from_bigint64 validation");
  }
  return arg0_uint64;
}

__attribute__((noinline, export_name("coreabi_to_bigint64"))) JS::BigInt *
to_bigint64(JSContext *cx, int64_t val) {
  return JS::detail::BigIntFromUint64(cx, val);
}

/*
 * These 4 "sample" functions are deconstructed after compilation and fully
 * removed. The prime number separates the get from the set in this
 * deconstruction. The generated code is then used to build a template for
 * constructing the generic binding functions from it. By always keeping these
 * samples around we can ensure this approach is resiliant to some degree of
 * compiled output changes, or at least throw a vaguely useful error when that
 * is no longer the case.
 */
__attribute__((export_name("coreabi_sample_i32"))) bool
CoreAbiSampleI32(JSContext *cx, unsigned argc, JS::Value *vp) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
  int32_t arg0 = static_cast<int32_t>(args[0].toInt32());
  args.rval().setInt32(arg0 * 32771);
  return true;
}

__attribute__((export_name("coreabi_sample_i64"))) bool
CoreAbiSampleI64(JSContext *cx, unsigned argc, JS::Value *vp) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
  int64_t arg1 = from_bigint64(args[1]);
  args.rval().setBigInt(to_bigint64(cx, arg1 * 32771));
  return true;
}

__attribute__((export_name("coreabi_sample_f32"))) bool
CoreAbiSampleF32(JSContext *cx, unsigned argc, JS::Value *vp) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
  float arg2 = static_cast<float>(args[2].toDouble());
  args.rval().setDouble(arg2 * 32771);
  return true;
}

__attribute__((export_name("coreabi_sample_f64"))) bool
CoreAbiSampleF64(JSContext *cx, unsigned argc, JS::Value *vp) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
  double arg3 = args[3].toDouble();
  args.rval().setDouble(arg3 * 32771);
  return true;
}

// Allocation functions for the splicer
__attribute__((optnone, export_name("coreabi_get_import"))) JSFunction *
coreabi_get_import(int32_t idx, int32_t argcnt, const char *name) {
  return JS_NewFunction(Runtime.cx, CoreAbiSampleI32, argcnt, 0, name);
}

__attribute__((export_name("cabi_realloc_adapter"))) void *
cabi_realloc_adapter(void *ptr, size_t orig_size, size_t org_align,
                     size_t new_size) {
  return JS_realloc(Runtime.cx, ptr, orig_size, new_size);
}

// This MUST override the StarlingMonkey core cabi_realloc export
//
// NOTE: You *should* avoid external host calls during realloc
// (ex. using the LOG macro to log a message), as this is a condition
// under which the component may be marked to prevent leaving (doing a new host call).
//
// see: https://github.com/bytecodealliance/wasmtime/blob/aec935f2e746d71934c8a131be15bbbb4392138c/crates/wasmtime/src/runtime/component/func/host.rs#L741
__attribute__((export_name("cabi_realloc"))) void *
cabi_realloc(void *ptr, size_t orig_size, size_t org_align, size_t new_size) {
  void *ret = JS_realloc(Runtime.cx, ptr, orig_size, new_size);
  // track all allocations during a function "call" for freeing
  Runtime.free_list.push_back(ret);
  if (!ret) {
    Runtime.engine->abort("(cabi_realloc) Unable to realloc");
  }
  return ret;
}

// ---------------------------------------------------------------------------
// No-snapshot lazy initialization
//
// When ComponentizeJS runs with noSnapshot=true, Wizer is skipped entirely.
// Instead, the JS source + initializer + env metadata are embedded as a data
// segment at a page-aligned boundary in linear memory.  The first call to
// any export discovers this data, boots the SpiderMonkey engine, evaluates
// the JS, and populates Runtime.fns – exactly what componentize_initialize()
// does during the normal Wizer flow, but at runtime.
// ---------------------------------------------------------------------------

#define EMBEDDED_SOURCE_MAGIC 0x4352534A // "JSRC" (little-endian)

struct EmbeddedSourceData {
  uint32_t env_len;
  uint32_t init_len;
  uint32_t source_len;
  const char *env_data;
  const char *init_js;
  const char *source_js;
};

static bool find_embedded_source(EmbeddedSourceData *out) {
  uint32_t mem_pages = __builtin_wasm_memory_size(0);
  // Scan page boundaries from top downward for the magic header.
  for (int32_t page = mem_pages - 1; page >= 1; page--) {
    uint32_t addr = (uint32_t)page * 65536;
    const uint32_t *header = (const uint32_t *)addr;
    if (header[0] != EMBEDDED_SOURCE_MAGIC)
      continue;

    out->env_len = header[2];
    out->init_len = header[3];
    out->source_len = header[4];
    const char *body = (const char *)(addr + 20); // 5 x uint32
    out->env_data = body;
    out->init_js = body + out->env_len;
    out->source_js = out->init_js + out->init_len;
    return true;
  }
  return false;
}

static void parse_and_set_env(const char *data, uint32_t len) {
  const char *end = data + len;
  const char *p = data;
  while (p < end && *p) {
    const char *eq = (const char *)memchr(p, '=', end - p);
    if (!eq)
      break;
    size_t key_len = eq - p;
    char key[256];
    if (key_len >= sizeof(key))
      break;
    memcpy(key, p, key_len);
    key[key_len] = '\0';

    const char *val = eq + 1;
    const char *val_end = (const char *)memchr(val, '\0', end - val);
    if (!val_end)
      break;

    setenv(key, val, 1);
    p = val_end + 1;
  }
}

// Parse a comma-separated type string (e.g. "i32,i64,f32") into a CoreVal vector.
// Leading '*' sets the paramptr/retptr flag on the caller side.
static bool parse_core_types(const char *tys, std::vector<CoreVal> &out,
                             bool *has_star) {
  int j = 0;
  if (tys[0] == '*') {
    *has_star = true;
    j++;
  }
  while (tys[j] != '\0') {
    if (strncmp(&tys[j], "i32", 3) == 0) {
      out.push_back(CoreVal::I32);
      j += 3;
    } else if (strncmp(&tys[j], "i64", 3) == 0) {
      out.push_back(CoreVal::I64);
      j += 3;
    } else if (strncmp(&tys[j], "f32", 3) == 0) {
      out.push_back(CoreVal::F32);
      j += 3;
    } else if (strncmp(&tys[j], "f64", 3) == 0) {
      out.push_back(CoreVal::F64);
      j += 3;
    } else {
      return false;
    }
    if (tys[j] == ',')
      j++;
  }
  return true;
}

// Resolve a single CoreVal from a type string (for return types).
static bool parse_single_core_type(const char *ty, std::optional<CoreVal> &out) {
  if (strncmp(ty, "i32", 3) == 0)      out.emplace(CoreVal::I32);
  else if (strncmp(ty, "i64", 3) == 0) out.emplace(CoreVal::I64);
  else if (strncmp(ty, "f32", 3) == 0) out.emplace(CoreVal::F32);
  else if (strncmp(ty, "f64", 3) == 0) out.emplace(CoreVal::F64);
  else return false;
  return true;
}

// Parse EXPORT{i}_* env vars, bind the JS function, and populate a CoreFn.
// Used by both the Wizer and no-snapshot initialization paths.
static bool populate_core_fn(ComponentizeRuntime::CoreFn *fn, size_t idx) {
  char env_name[100];

  // Bind the JS function from the init global
  sprintf(&env_name[0], "EXPORT%zu_NAME", idx);
  RootedObject init_global(Runtime.cx, Runtime.engine->init_script_global());
  RootedValue function_binding(Runtime.cx);
  if (!JS_GetProperty(Runtime.cx, init_global, getenv(env_name),
                      &function_binding)) {
    return false;
  }
  fn->func.init(Runtime.cx, function_binding);

  // Parse argument types
  sprintf(&env_name[0], "EXPORT%zu_ARGS", idx);
  bool has_star = false;
  if (!parse_core_types(getenv(env_name), fn->args, &has_star))
    return false;
  fn->paramptr = has_star;

  // Parse return type
  sprintf(&env_name[0], "EXPORT%zu_RET", idx);
  char *ret_tys = getenv(env_name);
  int j = 0;
  if (ret_tys[0] != '\0') {
    if (ret_tys[0] == '*') {
      fn->retptr = true;
      j++;
    }
    if (!parse_single_core_type(&ret_tys[j], fn->ret))
      return false;
  }

  sprintf(&env_name[0], "EXPORT%zu_RETSIZE", idx);
  fn->retsize = atoi(getenv(env_name));
  return true;
}

// Wire up exports: set $source_mod, call bindExports(), populate CoreFn vector.
// Shared by both the Wizer and no-snapshot initialization paths.
static bool wire_up_exports() {
  JSAutoRealm ar(Runtime.cx, Runtime.engine->init_script_global());

  RootedValue nsVal(Runtime.cx, Runtime.engine->script_value());
  RootedObject init_global(Runtime.cx, Runtime.engine->init_script_global());

  if (!JS_SetProperty(Runtime.cx, init_global, "$source_mod", nsVal))
    return false;

  RootedString source_name_str(
      Runtime.cx,
      JS_NewStringCopyZ(Runtime.cx, Runtime.source_name.c_str()));
  MOZ_RELEASE_ASSERT(source_name_str);
  RootedValue source_name_val(Runtime.cx, JS::StringValue(source_name_str));
  HandleValueArray bind_args(source_name_val);
  RootedValue rval(Runtime.cx);
  if (!JS_CallFunctionName(Runtime.cx, init_global, "bindExports",
                           bind_args, &rval))
    return false;

  uint32_t export_cnt = atoi(getenv("EXPORT_CNT"));
  for (size_t i = 0; i < export_cnt; i++) {
    ComponentizeRuntime::CoreFn *fn = &Runtime.fns.emplace_back();
    if (!populate_core_fn(fn, i))
      return false;
  }
  return true;
}

static void lazy_runtime_init() {
  // Already initialized (either by Wizer or by a previous lazy init).
  if (Runtime.engine != nullptr)
    return;

  // Look for embedded source data in memory.
  EmbeddedSourceData data;
  if (!find_embedded_source(&data))
    return;

  // Copy the embedded data to heap before Engine construction, which may
  // call memory.grow and (in theory) allow sbrk to overwrite the region.
  char *env_copy = (char *)malloc(data.env_len + 1);
  memcpy(env_copy, data.env_data, data.env_len);
  env_copy[data.env_len] = '\0';

  char *init_copy = (char *)malloc(data.init_len + 1);
  memcpy(init_copy, data.init_js, data.init_len);
  init_copy[data.init_len] = '\0';

  char *source_copy = (char *)malloc(data.source_len + 1);
  memcpy(source_copy, data.source_js, data.source_len);
  source_copy[data.source_len] = '\0';

  // 1. Populate the process environment so that install() (called as a
  //    builtin during Engine construction) can read IMPORT_CNT etc.
  parse_and_set_env(env_copy, data.env_len);
  free(env_copy);

  // 2. Construct the Engine WITHOUT scripts.  We evaluate the initializer
  //    and content script manually so that defineBuiltinModule() calls
  //    (from the initializer) are in place before the content script runs.
  auto config = std::make_unique<api::EngineConfig>();
  config->content_script_path.reset();
  new api::Engine(std::move(config));
  // Runtime.engine / Runtime.cx are now set by install().

  // 3. Evaluate the initializer script (sets up defineBuiltinModule,
  //    bindExports, etc.) via indirect eval in the init_script_global realm.
  {
    JSAutoRealm ar(Runtime.cx, Runtime.engine->init_script_global());
    RootedObject init_global(Runtime.cx,
                             Runtime.engine->init_script_global());

    JSString *init_str =
        JS_NewStringCopyN(Runtime.cx, init_copy, data.init_len);
    if (!init_str) {
      Runtime.engine->abort("(no-snapshot) failed to create init string");
    }
    RootedValue init_val(Runtime.cx, JS::StringValue(init_str));
    HandleValueArray eval_args(init_val);
    RootedValue eval_result(Runtime.cx);
    if (!JS_CallFunctionName(Runtime.cx, init_global, "eval", eval_args,
                             &eval_result)) {
      ReportAndClearException(Runtime.cx);
      Runtime.engine->abort("(no-snapshot) failed to evaluate initializer");
    }
  }
  free(init_copy);

  // 4. Evaluate the user's content script.
  //    Embedded mode: source lives in the data segment.
  //    External mode: loaded from WASI filesystem (default "extension.js").
  if (data.source_len > 0) {
    // Embedded mode: source is in the data segment.
    // SourceOwnership::Borrowed means SpiderMonkey compiles but doesn't
    // take ownership.  We free source_copy after eval_toplevel returns.
    JS::SourceText<mozilla::Utf8Unit> src;
    if (!src.init(Runtime.cx, source_copy, data.source_len,
                  JS::SourceOwnership::Borrowed)) {
      Runtime.engine->abort("(no-snapshot) failed to init content source");
    }
    RootedValue result(Runtime.cx);
    if (!Runtime.engine->eval_toplevel(src, Runtime.source_name, &result)) {
      ReportAndClearException(Runtime.cx);
      Runtime.engine->abort("(no-snapshot) failed to evaluate content script");
    }
    free(source_copy);
  } else {
    // External mode: load user JS from WASI filesystem.
    free(source_copy);
    const char *ext_path = getenv("EXTENSION_PATH");
    if (!ext_path || ext_path[0] == '\0') {
      ext_path = "extension.js";
    }
    RootedValue result(Runtime.cx);
    if (!Runtime.engine->eval_toplevel(std::string_view(ext_path), &result)) {
      ReportAndClearException(Runtime.cx);
      Runtime.engine->abort("(no-snapshot) failed to load external script");
    }
  }

  // 5. Read debug/feature flags from env.
  {
    const char *dbg = getenv("DEBUG");
    if (dbg && dbg[0] != '\0' && atoi(dbg))
      Runtime.debug = true;

    const char *fc = getenv("FEATURE_CLOCKS");
    if (fc && atoi(fc))
      Runtime.clocks = true;
  }

  // 6. Wire up exports (shared with Wizer path).
  if (!wire_up_exports()) {
    ReportAndClearException(Runtime.cx);
    Runtime.engine->abort("(no-snapshot) failed to wire up exports");
  }
}

__attribute__((export_name("call"))) uint32_t call(uint32_t fn_idx,
                                                   void *argptr) {
  lazy_runtime_init();
  if (Runtime.first_call) {
    content_debugger::maybe_init_debugger(Runtime.engine, true);
    js::ResetMathRandomSeed(Runtime.cx);
    Runtime.first_call = false;
    if (Runtime.clocks) {
      builtins::web::performance::Performance::timeOrigin.emplace(
          std::chrono::high_resolution_clock::now());
    }
  }
  if (Runtime.cur_fn_idx != -1) {
    Runtime.engine->abort("(call) unexpected call state, post_call was not called after last call");
  }
  Runtime.cur_fn_idx = fn_idx;
  ComponentizeRuntime::CoreFn *fn = &Runtime.fns[fn_idx];
  if (Runtime.debug) {
    fprintf(stderr, "(call) Function [%d] - ", fn_idx);
    fprintf(stderr, "(");
    if (fn->paramptr) {
      fprintf(stderr, "*");
    }
    bool first = true;
    for (int i = 0; i < fn->args.size(); i++) {
      if (first) {
        first = false;
      } else {
        fprintf(stderr, ", ");
      }
      fprintf(stderr, "%s", core_ty_str(fn->args[i]));
    }
    fprintf(stderr, ")");
    if (fn->ret.has_value()) {
      fprintf(stderr, " -> ");
      if (fn->retptr) {
        fprintf(stderr, "*");
      }
      fprintf(stderr, "%s", core_ty_str(fn->ret.value()));
    }
    fprintf(stderr, "\n");
  }

  JSAutoRealm ar(Runtime.cx, Runtime.engine->global());

  JS::RootedVector<JS::Value> args(Runtime.cx);
  if (!args.resize(fn->args.size() + (fn->retptr ? 1 : 0))) {
    Runtime.engine->abort("(call) unable to allocate memory for array resize");
  }

  LOG("(call) setting args");
  int argcnt = 0;
  if (fn->paramptr) {
    args[0].setInt32((uint32_t)argptr);
    argcnt = 1;
  } else if (fn->args.size() > 0) {
    uint32_t *curptr = static_cast<uint32_t *>(argptr);
    argcnt = fn->args.size();
    for (int i = 0; i < argcnt; i++) {
      switch (fn->args[i]) {
      case CoreVal::I32:
        args[i].setInt32(*curptr);
        curptr += 1;
        break;
      case CoreVal::I64:
        args[i].setBigInt(
            JS::detail::BigIntFromUint64(Runtime.cx, *(uint64_t *)(curptr)));
        curptr += 2;
        break;
      case CoreVal::F32:
        args[i].setNumber(*((float *)curptr));
        curptr += 1;
        break;
      case CoreVal::F64:
        args[i].setNumber(*((double *)curptr));
        curptr += 2;
        break;
      }
    }
  }

  void *retptr = nullptr;
  if (fn->retptr) {
    LOG("(call) setting retptr at arg %d\n", argcnt);
    retptr = cabi_realloc(nullptr, 0, 4, fn->retsize);
    args[argcnt].setInt32((uint32_t)retptr);
  }

  LOG("(call) JS lowering call");
  Runtime.engine->incr_event_loop_interest();
  JS::RootedValue r(Runtime.cx);
  if (!JS_CallFunctionValue(Runtime.cx, nullptr, fn->func, args, &r)) {
    LOG("(call) runtime JS Error");
    ReportAndClearException(Runtime.cx);
    abort();
  }

  // all calls are async functions returning promises
  LOG("(call) getting promise return");
  JS::RootedObject promise(Runtime.cx, &r.toObject());
  if (!promise) {
    // caught Result<> errors won't bubble here, so these are just critical
    // errors (same for promise rejections)
    Runtime.engine->abort("(call) unable to obtain call promise");
  }

  RootedObject empty_receiver(Runtime.cx, JS_NewPlainObject(Runtime.cx));
  JS::RootedObject call_then_handler_obj(
      Runtime.cx,
      create_internal_method<call_then_handler>(Runtime.cx, empty_receiver));
  JS::RootedObject call_catch_handler_obj(
      Runtime.cx,
      create_internal_method<call_catch_handler>(Runtime.cx, empty_receiver));
  if (!call_then_handler_obj || !call_catch_handler_obj) {
    Runtime.engine->abort("(call) unable to obtain call promise");
  }

  LOG("(call) adding promise reactions");
  if (!JS::AddPromiseReactions(Runtime.cx, promise, call_then_handler_obj,
                               call_catch_handler_obj)) {
    LOG("(call) unable to add promise reactions");
    ReportAndClearException(Runtime.cx);
    abort();
  }

  LOG("(call) driving event loop to promise completion");
  if (!Runtime.engine->run_event_loop()) {
    Runtime.engine->abort("(call) event loop error");
  }

  LOG("(call) retrieving promise result");
  auto promise_state = JS::GetPromiseState(promise);
  if (promise_state != JS::PromiseState::Fulfilled) {
    if (promise_state == JS::PromiseState::Pending) {
      LOG("(call) Unexpected promise state pending");
    } else {
      LOG("(call) Unexpected promise state rejected");
    }
    abort();
  }

  RootedValue ret(Runtime.cx, JS::GetPromiseResult(promise));

  // Handle singular returns
  if (!fn->retptr && fn->ret.has_value()) {
    LOG("(call) singular return");
    retptr = cabi_realloc(0, 0, 4, fn->retsize);
    switch (fn->ret.value()) {
    case CoreVal::I32:
      *((uint32_t *)retptr) = ret.toInt32();
      break;
    case CoreVal::I64:
      if (!JS::detail::BigIntIsUint64(ret.toBigInt(), (uint64_t *)retptr)) {
        abort();
      }
      break;
    case CoreVal::F32:
      *((float *)retptr) = ret.isInt32() ? static_cast<float>(ret.toInt32())
                                         : static_cast<float>(ret.toDouble());
      break;
    case CoreVal::F64:
      *((double *)retptr) =
          ret.isInt32() ? static_cast<double>(ret.toInt32()) : ret.toDouble();
      break;
    }
  }

  LOG("(call) end");

  // we always return a retptr (even if null)
  // the wrapper will drop it if not needed
  return (uint32_t)retptr;
}

__attribute__((export_name("post_call"))) void post_call(uint32_t fn_idx) {
  LOG("(post_call) Function [%d]", fn_idx);
  if (Runtime.cur_fn_idx != fn_idx) {
    LOG("(post_call) Unexpected call state, post_call must only be called "
        "immediately after call");
    abort();
  }
  Runtime.cur_fn_idx = -1;
  for (void *ptr : Runtime.free_list) {
    cabi_free(ptr);
  }
  Runtime.free_list.clear();
  RootedValue result(Runtime.cx);
  LOG("(post_call) end");
}

__attribute__((export_name("check_init"))) ComponentizeRuntime::InitError
check_init() {
  JSAutoRealm ar(Runtime.cx, Runtime.engine->global());
  JS::RootedValue exc(Runtime.cx);
  if (JS_GetPendingException(Runtime.cx, &exc)) {
    ReportAndClearException(Runtime.cx);
  }
  return Runtime.init_err;
}

__attribute__((export_name("componentize.wizer"))) void
componentize_initialize() {
  uint32_t is_debug = atoi(getenv("DEBUG"));
  if (is_debug)
    Runtime.debug = true;

  uint32_t feature_clocks = atoi(getenv("FEATURE_CLOCKS"));
  if (feature_clocks)
    Runtime.clocks = true;

  __wizer_initialize();

  LOG("(wizer) retrieve and generate the export bindings");
  if (!wire_up_exports()) {
    Runtime.init_err = ComponentizeRuntime::InitError::FnList;
    return;
  }
}
}

namespace componentize::embedding {
static bool ReallocFn(JSContext *cx, unsigned argc, JS::Value *vp) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
  void *old_ptr = (void *)args[0].toInt32();
  size_t old_len = args[1].toInt32();
  size_t align = args[2].toInt32();
  size_t new_len = args[3].toInt32();
  void *ptr = cabi_realloc(old_ptr, old_len, align, new_len);
  args.rval().setInt32((uint32_t)ptr);
  return true;
}

void cabi_free(void *ptr) {
  LOG("(cabi_free) %d", (uint32_t)ptr);
  JS_free(Runtime.cx, ptr);
}

const char *core_ty_str(CoreVal ty) {
  switch (ty) {
  case CoreVal::I32:
    return "i32";
  case CoreVal::I64:
    return "i64";
  case CoreVal::F32:
    return "f32";
  case CoreVal::F64:
    return "f64";
  }
}

// Note requires an AutoRealm
bool ReportAndClearException(JSContext *cx) {
  JS::ExceptionStack stack(cx);
  if (!JS::StealPendingExceptionStack(cx, &stack)) {
    LOG("(err) Uncatchable exception thrown");
    return false;
  }

  JS::ErrorReportBuilder report(cx);
  if (!report.init(cx, stack, JS::ErrorReportBuilder::WithSideEffects)) {
    LOG("(err) Couldn't build error report");
    return false;
  }

  JS::PrintError(stderr, report, false);
  return true;
}

void *LAST_SBRK = nullptr;
JS::PersistentRootedObject AB;
static bool GetMemBuffer(JSContext *cx, unsigned argc, JS::Value *vp) {
  if (sbrk(0) != LAST_SBRK) {
    LAST_SBRK = sbrk(0);
#ifdef DEBUG
    void *base = (void *)64;
#else
    void *base = 0;
#endif
    JS::RootedObject mem_buffer(cx, JS::NewArrayBufferWithUserOwnedContents(
                                        cx, (size_t)LAST_SBRK, base));
    AB.set(mem_buffer);
  }
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
  args.rval().setObject(*AB);
  return true;
}

bool install(api::Engine *engine) {
  Runtime.engine = engine;
  Runtime.cx = engine->cx();
  AB.init(engine->cx());

  char env_name[100];

  Runtime.source_name = std::string(getenv("SOURCE_NAME"));

  // -- Wire up the imports  --
  uint32_t import_cnt = atoi(getenv("IMPORT_CNT"));

  JS::RootedObject import_bindings(
      Runtime.cx, JS::NewArrayObject(Runtime.cx, 2 + import_cnt));

  LOG("(wizer) create the memory buffer JS object");
  JS::RootedObject mem(Runtime.cx, JS_NewPlainObject(Runtime.cx));
  if (!JS_DefineProperty(Runtime.cx, mem, "buffer", GetMemBuffer, nullptr,
                         JSPROP_ENUMERATE)) {
    return false;
  }

  JS_SetElement(Runtime.cx, import_bindings, 0, mem);

  LOG("(wizer) create the realloc JS function");
  JSFunction *realloc_fn =
      JS_NewFunction(Runtime.cx, ReallocFn, 0, 0, "realloc");
  if (!realloc_fn) {
    return false;
  }
  JS::RootedObject function_obj(Runtime.cx, JS_GetFunctionObject(realloc_fn));
  JS_SetElement(Runtime.cx, import_bindings, 1, function_obj);

  LOG("(wizer) create the %d import JS functions", import_cnt);
  for (size_t i = 0; i < import_cnt; i++) {
    sprintf(&env_name[0], "IMPORT%zu_NAME", i);
    const char *name = getenv(env_name);
    sprintf(&env_name[0], "IMPORT%zu_ARGCNT", i);
    uint32_t argcnt = atoi(getenv(env_name));

    JSFunction *import_fn = coreabi_get_import(i, argcnt, name);
    if (!import_fn) {
      return false;
    }
    JS::RootedObject function_obj(Runtime.cx, JS_GetFunctionObject(import_fn));
    JS_SetElement(Runtime.cx, import_bindings, 2 + i, function_obj);
  }

  LOG("(wizer) setting the binding global");
  if (!JS_DefineProperty(engine->cx(), engine->init_script_global(), "$bindings",
                         import_bindings, 0)) {
    return false;
  }

  LOG("(wizer) complete");

  return true;
}

} // namespace componentize::embedding
