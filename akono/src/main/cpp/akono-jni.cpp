/*
 This file is part of GNU Taler
 (C) 2019 GNUnet e.V.

 GNU Taler is free software; you can redistribute it and/or modify it under the
 terms of the GNU General Public License as published by the Free Software
 Foundation; either version 3, or (at your option) any later version.

 GNU Taler is distributed in the hope that it will be useful, but WITHOUT ANY
 WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

 You should have received a copy of the GNU General Public License along with
 GNU Taler; see the file COPYING.  If not, see <http://www.gnu.org/licenses/>
 */

#include <sys/types.h>
#include <sys/syscall.h>
#include <cstring>
#include <map>
#include <jni.h>
#include <libplatform/libplatform.h>
#include <v8.h>
#include <android/asset_manager.h>

#define NODE_WANT_INTERNALS 1 // NOLINT(cppcoreguidelines-macro-usage)

#include <node.h>

#include <uv.h>
#include <unistd.h>
#include <android/log.h>
#include <node_main_instance.h>
#include <node_binding.h>
#include <node_native_module_env.h>


void _register_akono();

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

/**
 * Mapping from module name to a string with the module's code.
 */
std::map<std::string, char *> modmap;


static int pfd[2];
static pthread_t thr;
static const char *tag = "akono-jni.cpp";


static void *thread_func(void *) {
    ssize_t rdsz;
    char buf[1024];
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

int start_logger() {
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
        m_cstr = env->GetStringUTFChars(s, nullptr);
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

static const char *main_code = "global.__akono_run = (x) => {"
                               "  0 && console.log('running code', x);"
                               "  global.eval(x);"
                               "};"
                               ""
                               "global.__akono_onMessage = (x) => {"
                               "  0 && console.log('got __akono_onMessage', x);"
                               "};";


class NativeAkonoInstance {
private:
    static bool logInitialized;
    static bool v8Initialized;
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
        _register_akono();
        loop = uv_default_loop();
        uv_async_init(loop, &async_notify, notifyCb);
        async_notify.data = this;

        if (!logInitialized) {
            start_logger();
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

        v8::Context::Scope context_scope(globalContext.Get(isolate));

        node::Environment *environment = node::CreateEnvironment(
                isolateData,
                globalContext.Get(isolate),
                nodeArgv.size(),
                &nodeArgv[0],
                nodeExecArgv.size(),
                &nodeExecArgv[0]);

        node::LoadEnvironment(environment);

        v8::Local<v8::ObjectTemplate> dataTemplate = v8::ObjectTemplate::New(isolate);
        dataTemplate->SetInternalFieldCount(1);
        v8::Local<v8::Object> dataObject = dataTemplate->NewInstance(context).ToLocalChecked();
        dataObject->SetAlignedPointerInInternalField(0, this);

        v8::Local<v8::Function> sendMessageFunction = v8::Function::New(context,
                                                                        sendMessageCallback,
                                                                        dataObject).ToLocalChecked();

        v8::Local<v8::Object> global = context->Global();

        global->Set(context, v8::String::NewFromUtf8(isolate, "__akono_sendMessage",
                                            v8::NewStringType::kNormal).ToLocalChecked(),
                    sendMessageFunction).Check();

    }

    /**
     * Process the node message loop until a break has been requested.
     *
     * @param env JNI env of the thread we're running in.
     */
    void runNode() {
        //printf("blup running node loop, tid=%llu\n", (unsigned long long) syscall(__NR_gettid));
        v8::Locker locker(isolate);
        v8::Isolate::Scope isolate_scope(isolate);
        v8::HandleScope handle_scope(isolate);
        v8::Local<v8::Context> context = globalContext.Get(isolate);
        v8::Context::Scope context_scope(context);
        this->breakRequested = false;
        while (true) {
            uv_run(uv_default_loop(), UV_RUN_ONCE);
            platform->DrainTasks(isolate);
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
        v8::Locker locker(isolate);
        v8::Isolate::Scope isolate_scope(isolate);
        v8::HandleScope handle_scope(isolate);
        v8::Local<v8::Context> context = globalContext.Get(isolate);
        v8::Context::Scope context_scope(context);
        v8::Local<v8::Object> global = context->Global();
        v8::Local<v8::Value> argv[] = {
                v8::String::NewFromUtf8(isolate, code,
                                        v8::NewStringType::kNormal).ToLocalChecked()
        };
        node::MakeCallback(isolate, global, "__akono_run", 1, argv, {0, 0});
    }

    ~NativeAkonoInstance() {
        //this->isolate->Dispose();
    }

    jstring evalJs(JNIEnv *env, jstring sourceString) {
        JStringValue jsv(env, sourceString);
        v8::Locker locker(isolate);

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

            if (!v8::Script::Compile(context, source).ToLocal(&script)) {
                return nullptr;
            }

            // Run the script to get the result.
            v8::Local<v8::Value> result;
            if (!script->Run(context).ToLocal(&result)) {
                return nullptr;
            }

            // Convert the result to an UTF8 string and print it.
            v8::String::Utf8Value utf8(isolate, result);

            return env->NewStringUTF(*utf8);
        }
    }
};


bool NativeAkonoInstance::v8Initialized = false;
bool NativeAkonoInstance::logInitialized = false;
node::MultiIsolatePlatform *NativeAkonoInstance::platform = nullptr;


void notifyCb(uv_async_t *async) {
    auto akono = (NativeAkonoInstance *) async->data;
    akono->breakRequested = true;
}

static void sendMessageCallback(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *isolate = args.GetIsolate();
    v8::Locker locker(isolate);
    if (args.Length() < 1) return;
    v8::HandleScope scope(isolate);
    v8::Local<v8::Value> arg = args[0];
    v8::String::Utf8Value value(isolate, arg);

    v8::Local<v8::Object> data = v8::Local<v8::Object>::Cast(args.Data());

    NativeAkonoInstance *myInstance = (NativeAkonoInstance *) data->GetAlignedPointerFromInternalField(0);

    JNIEnv *env = myInstance->currentJniEnv;

    if (env == nullptr) {
        mylog("FATAL: JNI env is nullptr");
        return;
    }

    jclass clazz = env->FindClass("akono/AkonoJni");

    if (clazz == nullptr) {
        mylog("FATAL: class not found");
        return;
    }

    jstring payloadStr = env->NewStringUTF(*value);

    jmethodID meth = env->GetMethodID(clazz, "internalOnNotify", "(Ljava/lang/String;)V");

    if (meth == nullptr) {
        mylog("FATAL: method not found");
        return;
    }

    env->CallVoidMethod(myInstance->currentJniThiz, meth, payloadStr);
}

static void getModuleCode(const v8::FunctionCallbackInfo<v8::Value> &args) {
    if (args.Length() < 1) return;
    v8::Isolate *isolate = args.GetIsolate();
    v8::Locker locker(isolate);
    v8::HandleScope scope(isolate);
    v8::Local<v8::Value> arg = args[0];
    v8::String::Utf8Value value(isolate, arg);

    printf("handling request for module %s\n", *value);

    std::string modName(*value);

    char *code = modmap[modName];

    if (!code) {
        printf("module not found in modmap %s\n", *value);
        return;
    }
    printf("found module in modmap %s\n", *value);
    args.GetReturnValue().Set(v8::String::NewFromUtf8(isolate, code,
                            v8::NewStringType::kNormal).ToLocalChecked());

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
Java_akono_AkonoJni_putModuleCodeNative(JNIEnv *env, jobject thiz, jstring modName, jstring modCode) {
    mylog("in putModuleCodeNative");
    JStringValue cModName(env, modName);
    JStringValue cModCode(env, modCode);
    std::string cppModName(strdup(*cModName));
    modmap[cppModName] = strdup(*cModCode);
    mylog("registered module");
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

void InitializeAkono(v8::Local<v8::Object> target,
                v8::Local<v8::Value> unused,
                v8::Local<v8::Context> context,
                void* priv) {
    NODE_SET_METHOD(target, "getModuleCode", getModuleCode);
}

NODE_MODULE_CONTEXT_AWARE_INTERNAL(akono, InitializeAkono)
