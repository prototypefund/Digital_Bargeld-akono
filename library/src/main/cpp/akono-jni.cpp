#include <string.h>
#include <jni.h>
#include <libplatform/libplatform.h>
#include <v8.h>
#define NODE_WANT_INTERNALS 1
#include <node.h>
#include <uv.h>
#include <unistd.h>
#include <android/log.h>



static int pfd[2];
static pthread_t thr;
static const char *tag = "myapp";


static void *thread_func(void*)
{
    ssize_t rdsz;
    char buf[128];
    while((rdsz = read(pfd[0], buf, sizeof buf - 1)) > 0) {
        if(buf[rdsz - 1] == '\n') --rdsz;
        buf[rdsz] = 0;  /* add null-terminator */
        __android_log_write(ANDROID_LOG_DEBUG, tag, buf);
    }
    return 0;
}

static void mylog(const char *msg)
{
    __android_log_write(ANDROID_LOG_DEBUG, tag, msg);
}

int start_logger(const char *app_name)
{
    tag = app_name;

    /* make stdout line-buffered and stderr unbuffered */
    setvbuf(stdout, 0, _IOLBF, 0);
    setvbuf(stderr, 0, _IONBF, 0);

    /* create the pipe and redirect stdout and stderr */
    pipe(pfd);
    dup2(pfd[1], 1);
    dup2(pfd[1], 2);

    /* spawn the logging thread */
    if(pthread_create(&thr, 0, thread_func, 0) == -1)
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
    JNIEnv* m_env;
public:
    JStringValue(JNIEnv* env, jstring s) : m_env(env), m_jstr(s) {
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

    NativeAkonoInstance() : globalContext() {

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
            InitNode(std::vector<const char *>{"node", "--trace-events-enabled"});
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


        globalContext.Reset(isolate, v8::Context::New(isolate));

        // Arguments for node itself
        std::vector<const char*> nodeArgv{"node", "-e", "console.log('hello world');"};
        // Arguments for the script run by node
        std::vector<const char*> nodeExecArgv{};

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

        mylog("finished environment");

        //v8::Isolate::CreateParams create_params;
        //create_params.array_buffer_allocator = v8::ArrayBuffer::Allocator::NewDefaultAllocator();
        //this->isolate = v8::Isolate::New(create_params);
//
//        node::IsolateData *isolateData = node::CreateIsolateData()
//        node::Environment *environment = node::CreateEnvironment(isolateData);
    }

    ~NativeAkonoInstance() {
        //this->isolate->Dispose();
    }

    jstring evalJs(JNIEnv* env, jstring sourceString) {
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


extern "C" JNIEXPORT jobject JNICALL
Java_akono_AkonoJni_initNative(JNIEnv* env, jobject thiz)
{
    NativeAkonoInstance *myInstance = new NativeAkonoInstance();
    return env->NewDirectByteBuffer(myInstance, 0);
}


extern "C" JNIEXPORT void JNICALL
Java_akono_AkonoJni_destroyNative(JNIEnv* env, jobject thiz, jobject buf)
{
    NativeAkonoInstance *myInstance = (NativeAkonoInstance *) env->GetDirectBufferAddress(buf);
    delete myInstance;
}


extern "C" JNIEXPORT jstring JNICALL
Java_akono_AkonoJni_evalJs(JNIEnv* env, jobject thiz, jstring sourceStr, jobject buf)
{
    NativeAkonoInstance *myInstance = (NativeAkonoInstance *) env->GetDirectBufferAddress(buf);
    return myInstance->evalJs(env, sourceStr);
}
