#!/system/bin/sh

#magisk 模块启动的入口

MODDIR=${0%/*} #${0} 表示当前脚本的文件名，%/* 表示从右向左查找第一个 / 字符，并删除该字符及其右侧的所有字符。因此，${0%/*} 表示删除当前脚本文件名中的文件名部分，只保留目录部分，即当前脚本所在的目录。
MODULE_ID=$(basename "$MODDIR") #basename 命令用于从路径中剥离目录和后缀，仅保留文件名. 这里对应: zygisk-sui
FLAVOR=@FLAVOR@ #占位符,在编译的过程中会被替换成 zygisk 或者 riru

# 输出当前 pid
log -p i -t "Sui" "Start post-fs-data.sh, pid: $$"

if [ "$ZYGISK_ENABLED" = false ] && [ "$FLAVOR" = "zygisk" ]; then
  log -p w -t "Sui" "Zygisk is disabled, skip zygisk-flavor script"
  exit 1
fi

if [ "$ZYGISK_ENABLED" = true ] && [ "$FLAVOR" = "riru" ]; then
  log -p w -t "Sui" "Zygisk is enabled, skip riru-flavor script"
  exit 1
fi

MAGISK_VER_CODE=$(magisk -V)
if [ "$MAGISK_VER_CODE" -ge 21000 ]; then #magisk 版本大于等于 21.0
  MAGISK_PATH="$(magisk --path)/.magisk/modules/$MODULE_ID"
else
  MAGISK_PATH=/sbin/.magisk/modules/$MODULE_ID
fi

log -p i -t "Sui" "Magisk version $MAGISK_VER_CODE"
log -p i -t "Sui" "Magisk module path $MAGISK_PATH"

enable_once="/data/adb/sui/enable_adb_root_once"
enable_forever="/data/adb/sui/enable_adb_root"
adb_root_exit=0

if [ -f $enable_once ]; then
  log -p i -t "Sui" "adb root support is enabled for this time of boot"
  rm $enable_once
  enable_adb_root=true
fi

if [ -f $enable_forever ]; then
  log -p i -t "Sui" "adb root support is enabled forever"
  enable_adb_root=true
fi

if [ "$enable_adb_root" = true ]; then
  log -p i -t "Sui" "Setup adb root support"

  # Run magiskpolicy manually if Magisk does not load sepolicy.rule
  if [ ! -e "$(magisk --path)/.magisk/mirror/sepolicy.rules/$MODULE_ID/sepolicy.rule" ]; then
    log -p e -t "Sui" "Magisk does not load sepolicy.rule..."
    log -p e -t "Sui" "Exec magiskpolicy --live --apply $MAGISK_PATH/sepolicy.rule..."
    magiskpolicy --live --apply "$MAGISK_PATH"/sepolicy.rule #指定加载策略时不重启系统，即实时加载策略。
    log -p i -t "Sui" "Apply finished"
  else
    log -p i -t "Sui" "Magisk should have loaded sepolicy.rule correctly"
  fi

  # Setup adb root support
  rm "$MODDIR/bin/adb_root"
  ln -s "$MODDIR/bin/sui" "$MODDIR/bin/adb_root"
  chmod 700 "$MODDIR/bin/adb_root"
  "$MODDIR/bin/adb_root" "$MAGISK_PATH"
  adb_root_exit=$? # $? 变量获取上一个命令的退出状态码. 通常，退出状态码为 0 表示命令执行成功，非零值表示命令执行失败。
  log -p i -t "Sui" "Exited with $adb_root_exit"
else
  log -p i -t "Sui" "adb root support is disabled" # magisk 模块安装的 sui 会这里
fi

# Setup uninstaller
rm "$MODDIR/bin/uninstall"
ln -s "$MODDIR/bin/sui" "$MODDIR/bin/uninstall" #创建软链接: uninstall -> sui

# Run Sui server
chmod 700 "$MODDIR"/bin/sui
exec "$MODDIR"/bin/sui "$MODDIR" "$adb_root_exit" # 跳转到 sui_main.cpp 中的 main 函数. 参数:[/data/adb/modules/zygisk-sui,0]