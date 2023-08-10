/*
 * This file is part of Sui.
 *
 * Sui is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Sui is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Sui.  If not, see <https://www.gnu.org/licenses/>.
 *
 * Copyright (c) 2021 Sui Contributors
 */

#include <cstdlib>
#include <cstring>
#include <logging.h>
#include <unistd.h>
#include <sched.h>
#include <app_process.h>
#include <sys/stat.h>

/*
 * argv[1]: path of the module, such as /data/adb/modules/zygisk-sui
 */
static int sui_main(int argc, char **argv) {
    LOGI("Sui starter begin argv: [%s, %s], pid: %d", argv[1], argv[2], getpid());

    if (daemon(false, false) != 0) {//转换为守护进程. 第一个参数: 表示不更改当前工作目录. 第二个参数: 不关闭任何文件描述符
        PLOGE("daemon");
        return EXIT_FAILURE;
    }

    wait_for_zygote();

    // 检查 /data/adb/sui 目录是否存在, 不存在则创建
    if (access("/data/adb/sui", F_OK) != 0) {
        mkdir("/data/adb/sui", 0600);
    }
    chmod("/data/adb/sui", 0600);
    chown("/data/adb/sui", 0, 0);//修改文件所有者为 root

    auto root_path = argv[1];

    //设置 dex_path 为 /data/adb/sui/sui.dex
    char dex_path[PATH_MAX]{0};
    strcpy(dex_path, root_path);
    strcat(dex_path, "/sui.dex");

    app_process(dex_path, root_path, "rikka.sui.server.Starter", "sui");

    return EXIT_SUCCESS;
}
