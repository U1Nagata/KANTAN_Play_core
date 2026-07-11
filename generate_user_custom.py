Import("env")
import os
import shutil

def generate_merged_firmware(source, target, env):
    project_dir = env.subst("$PROJECT_DIR")

    # MCUからチップタイプと出力ファイル名を決定
    mcu = env.BoardConfig().get("build.mcu", "esp32")
    chip = "esp32s3" if "s3" in mcu else "esp32"
    device = "CoreS3" if "s3" in mcu else "Core2"

    # サンプラー環境 (env名に "sampler" を含む) は別名で出力する
    is_sampler = "sampler" in env.subst("$PIOENV").lower()
    prefix = "KANTAN_Sampler" if is_sampler else "KANTAN_Play"
    full_name = "%s_%s_full.bin" % (prefix, device)

    # --- フルバイナリ (docs/firmware/) ---
    full_dir = os.path.join(project_dir, "docs", "firmware")
    os.makedirs(full_dir, exist_ok=True)
    full_path = os.path.join(full_dir, full_name)

    # パスに空白が含まれる環境を考慮し、各パスを引用符で囲む
    parts = " ".join(
        '%s "%s"' % (addr, env.subst(path))
        for addr, path in env.get("FLASH_EXTRA_IMAGES", [])
    )
    app = '%s "%s"' % (env.subst("$ESP32_APP_OFFSET"), str(target[0]))

    print("Generating merged firmware: %s" % full_name)
    env.Execute(env.subst(
        '"$PYTHONEXE" "$UPLOADER" --chip %s merge_bin -o "%s" %s %s'
        % (chip, full_path, parts, app)
    ))

    # --- OTAバイナリ (ota_bin/) ---
    ota_name = "%s_%s_OTA.bin" % (prefix, device)
    ota_dir = os.path.join(project_dir, "ota_bin")
    os.makedirs(ota_dir, exist_ok=True)
    ota_path = os.path.join(ota_dir, ota_name)

    app_bin = str(target[0])
    print("Copying OTA binary: %s" % ota_name)
    shutil.copy2(app_bin, ota_path)

env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", generate_merged_firmware)
