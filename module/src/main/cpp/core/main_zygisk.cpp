#include <zygisk.hpp>
#include <cstring>
#include <cerrno>
#include <logging.h>
#include <cstdio>
#include <sys/socket.h>
#include <bit>
#include <unistd.h>
#include <config.h>
#include <dex_file.h>
#include <memory.h>
#include <sys/mman.h>
#include <misc.h>
#include <nativehelper/scoped_utf_chars.h>
#include <fcntl.h>
#include <cinttypes>
#include <socket.h>
#include <sys/system_properties.h>
#include "system_server.h"
#include "main.h"
#include "settings_process.h"
#include "manager_process.h"

inline constexpr auto kProcessNameMax = 256;

enum Identity : int {

    IGNORE = 0,
    SYSTEM_SERVER = 1,
    SYSTEM_UI = 2,
    SETTINGS = 3,
};

class ZygiskModule : public zygisk::ModuleBase {

// 当模块被加载时调用，保存zygisk::Api和JNIEnv对象。
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        api_ = api;
        env_ = env;
    }

    // 程序执行前调用
    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        char process_name[kProcessNameMax]{0};
        char app_data_dir[PATH_MAX]{0};

        // 如果有可用的 nice name，将其复制到进程名称缓冲区中。
        if (args->nice_name) {
            // ScopedUtfChars 提供对 Java 字符串的 UTF 字符的只读访问的智能指针
            ScopedUtfChars niceName{env_, args->nice_name};
            strcpy(process_name, niceName.c_str());
        }

        //调试模式则将 app_data_dir 复制到应用程序数据目录缓冲区中。
#ifdef DEBUG
        if (args->app_data_dir) {
            ScopedUtfChars appDataDir{env_, args->app_data_dir};
            strcpy(app_data_dir, appDataDir.c_str());
        }
#endif
        LOGD("preAppSpecialize: %s %s", process_name, app_data_dir);

        // 使用给定的 UID 和进程名称初始化 zygote 函数
        InitCompanion(false, args->uid, process_name);

        // 如果身份是普通 App, 那么设置 zygisk 的 DLCLOSE_MODULE_LIBRARY 选项
        if (whoami == Identity::IGNORE) {
            api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
        }

        // 卸载 apex adbd,因为后面要加载自己修改之后的 /apex/com.android.adbd/lib64/libsui_adbd_preload.so
        UmountApexAdbd();
    }

    // 程序执行之后调用
    void postAppSpecialize(const zygisk::AppSpecializeArgs *args) override {
        LOGD("postAppSpecialize");

        if (whoami == Identity::IGNORE) {
            return;
        }

        char app_data_dir[PATH_MAX]{0};

        if (args->app_data_dir) {
            ScopedUtfChars appDataDir{env_, args->app_data_dir};
            strcpy(app_data_dir, appDataDir.c_str());
        }

        if (whoami == Identity::SETTINGS) {
            Settings::main(env_, app_data_dir, dex);
        } else if (whoami == Identity::SYSTEM_UI) {
            Manager::main(env_, app_data_dir, dex);
        }
    }

    // 系统服务执行前调用
    void preServerSpecialize(zygisk::ServerSpecializeArgs *args) override {
        LOGD("preServerSpecialize");

        InitCompanion(true, args->uid);
    }

    // 系统服务执行后调用
    void postServerSpecialize(const zygisk::ServerSpecializeArgs *args) override {
        LOGD("postServerSpecialize");

        if (__system_property_find("ro.vendor.product.ztename")) {
            auto *process = env_->FindClass("android/os/Process");
            auto *set_argv0 = env_->GetStaticMethodID(
                    process, "setArgV0", "(Ljava/lang/String;)V");
            env_->CallStaticVoidMethod(process, set_argv0, env_->NewStringUTF("system_server"));
        }

        SystemServer::main(env_, dex);
    }

private:
    zygisk::Api *api_{};
    JNIEnv *env_{};

    Identity whoami = Identity::IGNORE;
    Dex *dex = nullptr;

    /**
     * 这个 socket 是 zygisk 维护的. 建立链接后,具体的互操作是通过
     * REGISTER_ZYGISK_COMPANION 绑定的. 在这里对应的就是 @see #CompanionEntry
     *
     * 因为要访问 /data/adb 下面的 sui.dex 文件，所以需要 root 权限,
     * 并且这个文件需要共享, 所以在 zygote 进程中运行比较合适. 参见
     * #PrepareCompanion
     *
     * @param is_system_server 是否正在初始化系统服务器。
     * @param uid 应用程序的用户 ID。
     * @param process_name 应用程序的进程名称。
     */
    void InitCompanion(bool is_system_server, int uid, const char *process_name = nullptr) {
        auto companion = api_->connectCompanion();
        if (companion == -1) {
            LOGE("Zygote: failed to connect to companion");
            return;
        }

        // 如果是系统服务器，则向 companion 程序发送一个标志: 1。
        if (is_system_server) {
            write_int(companion, 1);
            whoami = Identity::SYSTEM_SERVER;
        } else {
            // 否则，向 companion 程序发送一个标志: 0，UID 和进程名称。
            write_int(companion, 0);
            write_int(companion, uid);
            write_full(companion, process_name, kProcessNameMax);
            whoami = static_cast<Identity>(read_int(companion));
        }

        if (whoami != Identity::IGNORE) {
            auto fd = recv_fd(companion);
            auto size = (size_t) read_int(companion);

            // 记录身份和 Dex 文件的文件描述符和大小。
            if (whoami == Identity::SETTINGS) {
                LOGI("Zygote: in Settings");
            } else if (whoami == Identity::SYSTEM_UI) {
                LOGI("Zygote: in SystemUi");
            } else {
                LOGI("Zygote: in SystemServer");
            }

            LOGI("Zygote: dex fd is %d, size is %" PRIdPTR, fd, size);
            // 使用文件描述符和大小创建 Dex 对象。
            dex = new Dex(fd, size);
            close(fd);
        }

        // 关闭与 companion 程序的连接。
        close(companion);
    }
};

static int dex_mem_fd = -1;
static size_t dex_size = 0;
static uid_t manager_uid = -1, settings_uid = -1;
static char manager_process[kProcessNameMax], settings_process[kProcessNameMax];

// 从指定的文件中读取应用程序的信息
// package：应用程序的包名
static void ReadApplicationInfo(const char *package, uid_t &uid, char *process) {
    char buf[PATH_MAX];
    // 构造文件路径: /data/adb/modules/zygisk-sui/[com.android.systemui|com.android.settings]
    snprintf(buf, PATH_MAX, "/data/adb/modules/%s/%s", ZYGISK_MODULE_ID, package);
    // 打开文件并映射到内存中
    auto file = Buffer(buf);
    auto bytes = file.data();
    //bytes: 10155\ncom.android.systemui
    auto size = file.size();
    for (int i = 0; i < size; ++i) {
        if (bytes[i] == '\n') {
            memset(process, 0, 256); // 清空 manager_process 或 settings_process 的数据
            memcpy(process, bytes + i + 1, size - i - 1);//设置 manager_process 或 settings_process 为进程名字

            bytes[i] = 0;//也清空了后面的数据
            uid = atoi((char *)bytes);//用于将字符串转换为整数,并赋值给 manager_uid 或 settings_uid(进程 id)
            break;
        }
    }
}

// 加载 sui.dex 文件, 并读取 settings 和 systemui 的 uid\进程名称
static bool PrepareCompanion() {
    bool result = false;

    auto path = "/data/adb/modules/" ZYGISK_MODULE_ID "/" DEX_NAME; // sui.dex
    int fd = open(path, O_RDONLY);
    ssize_t size;

    if (fd == -1) {
        PLOGE("open %s", path);
        goto cleanup;
    }

    size = lseek(fd, 0, SEEK_END);
    if (size == -1) {
        PLOGE("lseek %s", path);
        goto cleanup;
    }
    lseek(fd, 0, SEEK_SET);

    LOGD("Companion: dex size is %" PRIdPTR, size);

    // 创建共享内存
    dex_mem_fd = CreateSharedMem("sui.dex", size);
    if (dex_mem_fd >= 0) {
        // 将 dex 文件映射到共享内存中
        auto addr = (uint8_t *) mmap(nullptr, size, PROT_WRITE, MAP_SHARED, dex_mem_fd, 0);
        if (addr != MAP_FAILED) {
            read_full(fd, addr, size); // 将 dex 数据写入共享内存
            dex_size = size;
            munmap(addr, size); // 解除共享内存的映射关系
        }
        SetSharedMemProt(dex_mem_fd, PROT_READ); // 设置共享内存的权限
    }

    LOGI("Companion: dex fd is %d", dex_mem_fd);

    ReadApplicationInfo(MANAGER_APPLICATION_ID, manager_uid, manager_process);
    ReadApplicationInfo(SETTINGS_APPLICATION_ID, settings_uid, settings_process);

    LOGI("Companion: SystemUI %d %s", manager_uid, manager_process);
    LOGI("Companion: Settings %d %s", settings_uid, settings_process);

    result = true;

    cleanup:
    if (fd != -1) close(fd);

    return result;
}

static void CompanionEntry(int socket) {
    LOGI("Companion: start");
    static auto prepare = PrepareCompanion();

    char process_name[kProcessNameMax]{0};
    Identity whoami;

    // 判断是否为系统进程
    int is_system_server = read_int(socket) == 1;
    if (is_system_server != 0) {
        whoami = Identity::SYSTEM_SERVER;
    } else {
        // 读取uid和进程名称
        int uid = read_int(socket);
        read_full(socket, process_name, kProcessNameMax);

        if (uid == manager_uid && strcmp(process_name, manager_process) == 0) { // 判断是否为系统UI进程
            whoami = Identity::SYSTEM_UI;
        } else if (uid == settings_uid && strcmp(process_name, settings_process) == 0) { // 判断是否为系统设置进程
            whoami = Identity::SETTINGS;
        } else { // 其他进程( App 进程)
            whoami = Identity::IGNORE;
        }

        // 向对方程序发送身份标志。
        write_int(socket, whoami);
    }

    // 发送 dex 文件的文件描述符和大小。
    if (whoami != Identity::IGNORE) {
        send_fd(socket, dex_mem_fd);
        write_int(socket, dex_size);
    }

    close(socket);
}

REGISTER_ZYGISK_MODULE(ZygiskModule)

/**
 * 将 CompanionEntry 函数注册到 zygisk 的 companion
 * 中，以便应用程序可以与其通信。 CompanionEntry 中的代码是在 zygote
 * 的进程中运行的,并且可以共享资源, 但是只能在 preAppSpecialize 里面调用。
 * 可以参考 Zygisk.hpp 中 preAppSpecialize 和 connectCompanion 的注释
 */
REGISTER_ZYGISK_COMPANION(CompanionEntry)
