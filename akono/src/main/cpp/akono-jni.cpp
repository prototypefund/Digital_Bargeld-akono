#include <string.h>
#include <jni.h>
#include <libplatform/libplatform.h>
#include <v8.h>

#define NODE_WANT_INTERNALS 1

#include <node.h>

#include <uv.h>
#include <unistd.h>
#include <android/log.h>
#include <node_main_instance.h>
#include <node_native_module_env.h>



// Provide stubs so that libnode.so links properly
namespace node {
    namespace native_module {
        const bool has_code_cache = false;

        void NativeModuleEnv::InitializeCodeCache() {}
    }

    v8::StartupData *NodeMainInstance::GetEmbeddedSnapshotBlob() {
        return nullptr;
    }

    const std::vector<size_t> *NodeMainInstance::GetIsolateDataIndexes() {
        return nullptr;
    }
}


static int pfd[2];
static pthread_t thr;
static const char *tag = "myapp";


static void *thread_func(void *) {
    ssize_t rdsz;
    char buf[128];
    while ((rdsz = read(pfd[0], buf, sizeof buf - 1)) > 0) {
        if (buf[rdsz - 1] == '\n') --rdsz;
        buf[rdsz] = 0;  /* add null-terminator */
        __android_log_write(ANDROID_LOG_DEBUG, tag, buf);
    }
    return 0;
}

static void mylog(const char *msg) {
    __android_log_write(ANDROID_LOG_DEBUG, tag, msg);
}

int start_logger(const char *app_name) {
    tag = app_name;

    /* make stdout line-buffered and stderr unbuffered */
    setvbuf(stdout, 0, _IOLBF, 0);
    setvbuf(stderr, 0, _IONBF, 0);

    /* create the pipe and redirect stdout and stderr */
    pipe(pfd);
    dup2(pfd[1], 1);
    dup2(pfd[1], 2);

    /* spawn the logging thread */
    if (pthread_create(&thr, 0, thread_func, 0) == -1)
        return -1;
    pthread_detach(thr);
    return 0;
}


/**
 * Helper class to manage conversion from a JNI string to a C string.
 */
class JStringValue {
private:
    jstring m_jstr;
    const char *m_cstr;
    JNIEnv *m_env;
public:
    JStringValue(JNIEnv *env, jstring s) : m_env(env), m_jstr(s) {
        m_cstr = env->GetStringUTFChars(s, NULL);
    }

    ~JStringValue() {
        m_env->ReleaseStringUTFChars(m_jstr, m_cstr);
    }

    const char *operator*() {
        return m_cstr;
    }
};


/**
 * Slightly more sane wrapper around node::Init
 */
static void InitNode(std::vector<const char *> argv) {
    int ret_exec_argc = 0;
    int ret_argc = argv.size();
    const char **ret_exec_argv = nullptr;

    node::Init(&ret_argc, &argv[0], &ret_exec_argc, &ret_exec_argv);
}

// Forward declarations
void notifyCb(uv_async_t *async);

static void sendMessageCallback(const v8::FunctionCallbackInfo<v8::Value> &args);
static void loadModuleCallback(const v8::FunctionCallbackInfo<v8::Value> &args);
static void getDataCallback(const v8::FunctionCallbackInfo<v8::Value> &args);

static const char *main_code = "global.__akono_run = (x) => {"
                               "  console.log('running code', x);"
                               "  global.eval(x);"
                               "};"
                               ""
                               "global.__akono_onMessage = (x) => {"
                               "  console.log('got __akono_onMessage', x);"
                               "};"
                               ""
                               "mod = require('module');"
                               "mod._saved_findPath = mod._findPath;"
                               "mod._akonoMods = {};"
                               "mod._findPath = (request, paths, isMain) => {"
                               "  console.log('in _findPath');"
                               "  const res = mod._saved_findPath(request, paths, isMain);"
                               "  if (res !== false) return res;"
                               "  const args = JSON.stringify({ request, paths});"
                               "  const loadResult = JSON.parse(global.__akono_loadModule(args));"
                               "  console.log('got loadModule result', loadResult);"
                               "  if (!loadResult) return false;"
                               "  mod._akonoMods[loadResult.path] = loadResult;"
                               "  console.log('returning path', loadResult.path);"
                               "  return loadResult.path;"
                               "};"
                               ""
                               "function stripBOM(content) {"
                               "  if (content.charCodeAt(0) === 0xFEFF) {"
                               "    content = content.slice(1);"
                               "  }"
                               "  return content;"
                               "}"
                               ""
                               "mod._saved_js_extension = mod._extensions[\".js\"];"
                               "mod._extensions[\".js\"] = (module, filename) => {"
                               "  console.log('handling js extension', [module, filename]);"
                               "  if (mod._akonoMods.hasOwnProperty(filename)) {"
                               "    const akmod = mod._akonoMods[filename];"
                               "    console.log('found mod', akmod);"
                               "    const content = akmod.content;"
                               "    return module._compile(stripBOM(content), filename);"
                               "  }"
                               "  console.log('falling back');"
                               "  return mod._saved_js_extension(module, filename);"
                               "};";


class NativeAkonoInstance {
private:
    static bool logInitialized;
    static bool v8Initialized;
    //static std::unique_ptr<v8::Platform> platform;
    static node::MultiIsolatePlatform *platform;
public:
    v8::Isolate *isolate;
    node::Environment *environment;
    v8::Persistent<v8::Context> globalContext;
    uv_async_t async_notify;
    uv_loop_t *loop;
    bool breakRequested = false;
    JNIEnv *currentJniEnv = nullptr;
    jobject currentJniThiz = nullptr;

    NativeAkonoInstance() : globalContext() {
        loop = uv_default_loop();
        uv_async_init(loop, &async_notify, notifyCb);
        async_notify.data = this;

        if (!logInitialized) {
            start_logger("myapp");
            logInitialized = true;
        }

        if (!v8Initialized) {
            //platform = v8::platform::NewDefaultPlatform();
            //v8::V8::InitializePlatform(platform.get());
            //v8::V8::Initialize();

            // Here, only the arguments used to initialize the global node/v8 platform
            // are relevant, the others are skipped.
            InitNode(std::vector<const char *>{"node", "-e", main_code});
            platform = node::InitializeV8Platform(10);
            v8::V8::Initialize();

            v8Initialized = true;
        }

        node::ArrayBufferAllocator *allocator = node::CreateArrayBufferAllocator();
        this->isolate = node::NewIsolate(allocator, uv_default_loop());


        v8::Isolate::Scope isolate_scope(isolate);
        v8::HandleScope handle_scope(isolate);

        node::IsolateData *isolateData = node::CreateIsolateData(
                this->isolate,
                uv_default_loop(),
                platform,
                allocator);


        globalContext.Reset(isolate, node::NewContext(isolate));

        v8::Local<v8::Context> context = globalContext.Get(isolate);

        // Arguments for node itself
        std::vector<const char *> nodeArgv{"node", "-e", "console.log('hello world');"};
        // Arguments for the script run by node
        std::vector<const char *> nodeExecArgv{};

        mylog("entering global scopt");

        v8::Context::Scope context_scope(globalContext.Get(isolate));

        mylog("creating environment");

        node::Environment *environment = node::CreateEnvironment(
                isolateData,
                globalContext.Get(isolate),
                nodeArgv.size(),
                &nodeArgv[0],
                nodeExecArgv.size(),
                &nodeExecArgv[0]);


        mylog("loading environment");

        node::LoadEnvironment(environment);

        mylog("finished loading environment");

        v8::Local<v8::ObjectTemplate> dataTemplate = v8::ObjectTemplate::New(isolate);
        dataTemplate->SetInternalFieldCount(1);
        v8::Local<v8::Object> dataObject = dataTemplate->NewInstance(context).ToLocalChecked();
        dataObject->SetAlignedPointerInInternalField(0, this);

        v8::Local<v8::Function> sendMessageFunction = v8::Function::New(context,
                                                                        sendMessageCallback,
                                                                        dataObject).ToLocalChecked();

        v8::Local<v8::Function> loadModuleFunction = v8::Function::New(context,
                                                                        loadModuleCallback,
                                                                        dataObject).ToLocalChecked();

        v8::Local<v8::Function> getDataFunction = v8::Function::New(context,
                                                                       getDataCallback,
                                                                       dataObject).ToLocalChecked();

        v8::Local<v8::Object> global = context->Global();

        global->Set(v8::String::NewFromUtf8(isolate, "__akono_sendMessage",
                                            v8::NewStringType::kNormal).ToLocalChecked(),
                    sendMessageFunction);

        global->Set(v8::String::NewFromUtf8(isolate, "__akono_loadModule",
                                            v8::NewStringType::kNormal).ToLocalChecked(),
                    loadModuleFunction);

        // Get data synchronously (!) from the embedder
        global->Set(v8::String::NewFromUtf8(isolate, "__akono_getData",
                                            v8::NewStringType::kNormal).ToLocalChecked(),
                    getDataFunction);

    }

    /**
     * Process the node message loop until a break has been requested.
     *
     * @param env JNI env of the thread we're running in.
     */
    void runNode() {
        this->breakRequested = false;
        while (1) {
            uv_run(uv_default_loop(), UV_RUN_ONCE);
            if (this->breakRequested)
                break;
        }
    }

    /**
     * Inject code into the running node instance.
     *
     * Must not be called from a different thread.
     */
    void makeCallback(const char *code) {
        mylog("in makeCallback");
        v8::Isolate::Scope isolate_scope(isolate);
        v8::HandleScope handle_scope(isolate);
        v8::Local<v8::Context> context = globalContext.Get(isolate);
        v8::Context::Scope context_scope(context);
        v8::Local<v8::Object> global = context->Global();
        v8::Local<v8::Value> argv[] = {
                v8::String::NewFromUtf8(isolate, code,
                                        v8::NewStringType::kNormal).ToLocalChecked()
        };
        mylog("calling node::MakeCallback");
        node::MakeCallback(isolate, global, "__akono_run", 1, argv, {0, 0});
    }

    ~NativeAkonoInstance() {
        //this->isolate->Dispose();
    }

    jstring evalJs(JNIEnv *env, jstring sourceString) {
        mylog("begin evalJs");

        JStringValue jsv(env, sourceString);

        v8::Isolate::Scope isolate_scope(isolate);

        // Create a stack-allocated handle scope.
        v8::HandleScope handle_scope(isolate);

        // Create a new context.
        //v8::Local<v8::Context> context = v8::Context::New(isolate);

        v8::Local<v8::Context> context = globalContext.Get(isolate);

        // Enter the context for compiling and running the hello world script.
        v8::Context::Scope context_scope(context);

        {
            // Create a string containing the JavaScript source code.
            v8::Local<v8::String> source =
                    v8::String::NewFromUtf8(isolate, *jsv,
                                            v8::NewStringType::kNormal)
                            .ToLocalChecked();

            // Compile the source code.
            v8::Local<v8::Script> script;

            mylog("about to compile script");

            if (!v8::Script::Compile(context, source).ToLocal(&script)) {
                return nullptr;
            }

            mylog("about to run script");

            // Run the script to get the result.
            v8::Local<v8::Value> result;
            if (!script->Run(context).ToLocal(&result)) {
                mylog("running script failed");
                return nullptr;
            }

            mylog("converting script result value");

            // Convert the result to an UTF8 string and print it.
            v8::String::Utf8Value utf8(isolate, result);

            mylog("about to return value");

            return env->NewStringUTF(*utf8);
        }
    }
};


bool NativeAkonoInstance::v8Initialized = false;
bool NativeAkonoInstance::logInitialized = false;
node::MultiIsolatePlatform *NativeAkonoInstance::platform = nullptr;


void notifyCb(uv_async_t *async) {
    NativeAkonoInstance *akono = (NativeAkonoInstance *) async->data;
    mylog("async notifyCb called!");
    akono->breakRequested = true;
}

static void sendMessageCallback(const v8::FunctionCallbackInfo<v8::Value> &args) {
    if (args.Length() < 1) return;
    v8::Isolate *isolate = args.GetIsolate();
    v8::HandleScope scope(isolate);
    v8::Local<v8::Value> arg = args[0];
    v8::String::Utf8Value value(isolate, arg);
    mylog("sendMessageCallback called, yay!");

    v8::Local<v8::Object> data = v8::Local<v8::Object>::Cast(args.Data());

    mylog("getting instance");
    NativeAkonoInstance *myInstance = (NativeAkonoInstance *) data->GetAlignedPointerFromInternalField(0);

    JNIEnv *env = myInstance->currentJniEnv;

    if (env == nullptr) {
        mylog("FATAL: JNI env is nullptr");
        return;
    }

    mylog("finding class");
    jclass clazz = env->FindClass("akono/AkonoJni");

    if (clazz == nullptr) {
        mylog("FATAL: class not found");
        return;
    }

    mylog("creating strings");
    jstring jstr1 = env->NewStringUTF("message");
    jstring jstr2 = env->NewStringUTF(*value);

    mylog("getting method");

    jmethodID meth = env->GetMethodID(clazz, "internalOnNotify", "(Ljava/lang/String;Ljava/lang/String;)V");

    if (meth == nullptr) {
        mylog("FATAL: method not found");
        return;
    }

    mylog("calling method");

    env->CallVoidMethod(myInstance->currentJniThiz, meth, jstr1, jstr2);
}


static void loadModuleCallback(const v8::FunctionCallbackInfo<v8::Value> &args) {
    if (args.Length() < 1) return;
    v8::Isolate *isolate = args.GetIsolate();
    v8::HandleScope scope(isolate);
    v8::Local<v8::Value> arg = args[0];
    v8::String::Utf8Value value(isolate, arg);
    mylog("sendMessageCallback called, yay!");

    v8::Local<v8::Object> data = v8::Local<v8::Object>::Cast(args.Data());

    mylog("getting instance");
    NativeAkonoInstance *myInstance = (NativeAkonoInstance *) data->GetAlignedPointerFromInternalField(0);

    JNIEnv *env = myInstance->currentJniEnv;

    if (env == nullptr) {
        mylog("FATAL: JNI env is nullptr");
        return;
    }

    mylog("finding class");
    jclass clazz = env->FindClass("akono/AkonoJni");

    if (clazz == nullptr) {
        mylog("FATAL: class not found");
        return;
    }

    mylog("creating strings");
    jstring jstr1 = env->NewStringUTF(*value);

    mylog("getting method");

    jmethodID meth = env->GetMethodID(clazz, "internalOnModuleLoad", "(Ljava/lang/String;)Ljava/lang/String;");

    if (meth == nullptr) {
        mylog("FATAL: method not found");
        return;
    }

    mylog("calling method");

    jstring jresult = (jstring) env->CallObjectMethod(myInstance->currentJniThiz, meth, jstr1);

    JStringValue resultStringValue(env, jresult);

    printf("before creating string, res %s\n", *resultStringValue);

    // Create a string containing the JavaScript source code.
    v8::Local<v8::String> rs =
            v8::String::NewFromUtf8(isolate, *resultStringValue,
                                    v8::NewStringType::kNormal)
                    .ToLocalChecked();

    args.GetReturnValue().Set(rs);
}


static void getDataCallback(const v8::FunctionCallbackInfo<v8::Value> &args) {
    if (args.Length() < 1) return;
    v8::Isolate *isolate = args.GetIsolate();
    v8::HandleScope scope(isolate);
    v8::Local<v8::Value> arg = args[0];
    v8::String::Utf8Value value(isolate, arg);
    mylog("getDataCallback called");

    v8::Local<v8::Object> data = v8::Local<v8::Object>::Cast(args.Data());
    mylog("getting instance");
    NativeAkonoInstance *myInstance = (NativeAkonoInstance *) data->GetAlignedPointerFromInternalField(0);

    JNIEnv *env = myInstance->currentJniEnv;
    if (env == nullptr) {
        mylog("FATAL: JNI env is nullptr");
        return;
    }

    mylog("finding class");
    jclass clazz = env->FindClass("akono/AkonoJni");

    if (clazz == nullptr) {
        mylog("FATAL: class not found");
        return;
    }

    mylog("creating strings");
    jstring jstr1 = env->NewStringUTF(*value);

    mylog("getting method");

    jmethodID meth = env->GetMethodID(clazz, "internalOnGetData", "(Ljava/lang/String;)Ljava/lang/String;");

    if (meth == nullptr) {
        mylog("FATAL: method not found");
        return;
    }

    mylog("calling method");

    jstring jresult = (jstring) env->CallObjectMethod(myInstance->currentJniThiz, meth, jstr1);

    JStringValue resultStringValue(env, jresult);

    printf("before creating string, res %s\n", *resultStringValue);

    // Create a string containing the JavaScript source code.
    v8::Local<v8::String> rs =
            v8::String::NewFromUtf8(isolate, *resultStringValue,
                                    v8::NewStringType::kNormal)
                    .ToLocalChecked();

    args.GetReturnValue().Set(rs);
}


extern "C" JNIEXPORT jobject JNICALL
Java_akono_AkonoJni_initNative(JNIEnv *env, jobject thiz) {
    NativeAkonoInstance *myInstance = new NativeAkonoInstance();
    return env->NewDirectByteBuffer(myInstance, 0);
}


extern "C" JNIEXPORT void JNICALL
Java_akono_AkonoJni_destroyNative(JNIEnv *env, jobject thiz, jobject buf) {
    NativeAkonoInstance *myInstance = (NativeAkonoInstance *) env->GetDirectBufferAddress(buf);
    delete myInstance;
}


extern "C" JNIEXPORT jstring JNICALL
Java_akono_AkonoJni_evalJs(JNIEnv *env, jobject thiz, jstring sourceStr, jobject buf) {
    NativeAkonoInstance *myInstance = (NativeAkonoInstance *) env->GetDirectBufferAddress(buf);
    return myInstance->evalJs(env, sourceStr);
}

extern "C" JNIEXPORT void JNICALL
Java_akono_AkonoJni_notifyNative(JNIEnv *env, jobject thiz, jobject buf) {
    NativeAkonoInstance *myInstance = (NativeAkonoInstance *) env->GetDirectBufferAddress(buf);
    uv_async_send(&myInstance->async_notify);
}

extern "C" JNIEXPORT void JNICALL
Java_akono_AkonoJni_runNode(JNIEnv *env, jobject thiz, jobject buf) {
    NativeAkonoInstance *myInstance = (NativeAkonoInstance *) env->GetDirectBufferAddress(buf);
    myInstance->currentJniEnv = env;
    myInstance->currentJniThiz = thiz;
    myInstance->runNode();
}

extern "C" JNIEXPORT void JNICALL
Java_akono_AkonoJni_makeCallbackNative(JNIEnv *env, jobject thiz, jstring sourceStr, jobject buf) {
    JStringValue jsv(env, sourceStr);
    NativeAkonoInstance *myInstance = (NativeAkonoInstance *) env->GetDirectBufferAddress(buf);
    myInstance->currentJniEnv = env;
    myInstance->currentJniThiz = thiz;
    return myInstance->makeCallback(*jsv);
}
