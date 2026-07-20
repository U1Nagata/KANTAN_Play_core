Import("env")
import os
import shutil
import json


def read_version(header_path, macro_prefix):
    """Read three integer version macros from a firmware version header."""
    values = {}
    with open(header_path, "r", encoding="utf-8") as header:
        for line in header:
            parts = line.strip().split()
            if len(parts) == 3 and parts[0] == "#define" and parts[1].startswith(macro_prefix):
                values[parts[1]] = parts[2]
    required = [
        macro_prefix + "MAJOR",
        macro_prefix + "MINOR",
        macro_prefix + "PATCH",
    ]
    if not all(key in values for key in required):
        raise RuntimeError("Could not read version macros from %s" % header_path)
    return "%s.%s.%s" % tuple(values[key] for key in required)


def update_web_manifest(project_dir, is_sampler):
    """Keep the ESP Web Tools manifest version aligned with the firmware."""
    if is_sampler:
        header = os.path.join(project_dir, "main", "sampler", "sampler_version.hpp")
        version = read_version(header, "SAMPLER_VERSION_") + "-beta"
        manifest_name = "manifest_sampler.json"
    else:
        header = os.path.join(project_dir, "main", "version_define.hpp")
        version = read_version(header, "APP_VERSION_")
        manifest_name = "manifest.json"

    manifest_path = os.path.join(project_dir, "docs", manifest_name)
    with open(manifest_path, "r", encoding="utf-8") as manifest_file:
        manifest = json.load(manifest_file)
    if manifest.get("version") == version:
        return
    manifest["version"] = version
    with open(manifest_path, "w", encoding="utf-8", newline="\n") as manifest_file:
        json.dump(manifest, manifest_file, ensure_ascii=False, indent=2)
        manifest_file.write("\n")
    print("Updated USB installer manifest: %s" % version)

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

    update_web_manifest(project_dir, is_sampler)

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

    # OTA更新はカタログと同じGitHub Pages配信元から取得する。raw.githubusercontent
    # への別TLS接続・リダイレクトを避け、低帯域の本体でも安定して更新できるようにする。
    docs_ota_path = os.path.join(full_dir, ota_name)
    print("Copying OTA binary for web: %s" % ota_name)
    shutil.copy2(app_bin, docs_ota_path)

env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", generate_merged_firmware)
