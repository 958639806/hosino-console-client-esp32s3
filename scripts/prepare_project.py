"""Stage the pinned upstream example plus the small clock overlay in an ASCII path."""
import argparse
import json
import re
import shutil
import subprocess
from pathlib import Path
import yaml


def write_if_changed(path, data):
    """Preserve unchanged timestamps; changed content gets the current mtime."""
    if path.is_file() and path.read_bytes() == data:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return True


def stage_layers(layers, project, generated=()):
    """Copy each final overlay winner once, without inheriting its old mtime."""
    excluded = {Path(name) for name in generated}
    sources = {}
    for layer in layers:
        if not layer.is_dir():
            raise FileNotFoundError(f"Missing source layer: {layer}")
        for source in layer.rglob("*"):
            relative = source.relative_to(layer)
            if source.is_dir():
                (project / relative).mkdir(parents=True, exist_ok=True)
            elif source.is_file() and relative not in excluded:
                sources[relative] = source
    for relative, source in sources.items():
        target = project / relative
        if write_if_changed(target, source.read_bytes()):
            # Keep permissions, but never copy source timestamps: an edited
            # source can predate the object compiled from the previous stage.
            shutil.copymode(source, target)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-root", default=r"C:\esp\hosino-qspi")
    parser.add_argument("--performance", action="store_true")
    parser.add_argument("--buffered", action="store_true")
    parser.add_argument("--corners", action="store_true")
    parser.add_argument("--localized", action="store_true")
    parser.add_argument("--apps", action="store_true")
    args = parser.parse_args()
    args.localized = args.localized or args.apps
    if args.localized and args.corners:
        parser.error("--localized and --corners are mutually exclusive")
    args.buffered = args.buffered or args.corners or args.localized
    performance = args.performance or args.buffered
    workspace = Path(__file__).resolve().parents[1]
    lock = json.loads((workspace / "source-lock.json").read_text(encoding="utf-8"))
    upstream = workspace / "upstream/waveshare-esp32-s3-touch-amoled-2.16"
    head = subprocess.check_output(["git", "-C", str(upstream), "rev-parse", "HEAD"], text=True).strip()
    if head != lock["upstream_commit"]:
        raise SystemExit(f"Unexpected upstream commit: {head}")
    if subprocess.check_output(["git", "-C", str(upstream), "status", "--porcelain"], text=True).strip():
        raise SystemExit("Upstream checkout has modifications; refusing to stage it")
    root = Path(args.build_root).resolve()
    if not str(root).isascii() or " " in str(root):
        raise SystemExit("ESP-IDF build path must be ASCII without spaces")
    project = root / ("project-apps" if args.apps else "project-localized" if args.localized else "project-corners" if args.corners
                      else "project-buffered" if args.buffered else "project-perf" if performance else "project")
    marker = project / ".hosino-qspi-stage"
    if project.exists() and not marker.exists():
        raise SystemExit(f"Refusing to overwrite an unmarked project: {project}")
    project.mkdir(parents=True, exist_ok=True)
    write_if_changed(marker, (lock["upstream_commit"] + "\n").encode("ascii"))
    layers = [upstream / lock["upstream_project"], workspace / "overlay"]
    if args.localized:
        layers.append(workspace / "overlay-localized")
    if args.apps:
        layers.append(workspace / "overlay-apps")
    # The generated manifest below is also a final winner. Do not temporarily
    # replace it with upstream's manifest during an otherwise unchanged stage.
    generated = ["main/idf_component.yml"]
    if args.apps:
        generated.append("CMakeLists.txt")
    stage_layers(layers, project, generated=generated)
    if args.apps:
        # project() has already processed all component directories. Setting a
        # normal variable afterwards affects only the top-level ELF target's
        # generated C stub, not the already-created component directory scopes.
        # Treat this as a final generated winner so unchanged stages keep mtime.
        cmake_source = next(layer / "CMakeLists.txt" for layer in reversed(layers)
                            if (layer / "CMakeLists.txt").is_file())
        cmake_text = cmake_source.read_text(encoding="utf-8").rstrip() + (
            "\n\n# Keep the top-level ELF stub compile command within the Windows limit.\n"
            "# Must follow project(): component directories keep their own scopes.\n"
            "if(CMAKE_HOST_WIN32 AND CMAKE_GENERATOR MATCHES \"Ninja\")\n"
            "    set(CMAKE_NINJA_FORCE_RESPONSE_FILE ON)\n"
            "endif()\n")
        write_if_changed(project / "CMakeLists.txt", cmake_text.encode("utf-8"))
        # Reuse downloaded components of the same pinned baseline. IDF still
        # verifies their component hashes and resolves only added dependencies.
        cached = root / "project-localized/managed_components"
        if cached.is_dir():
            for component in cached.iterdir():
                target = project / "managed_components" / component.name
                if component.is_dir() and not (target / ".component_hash").exists():
                    shutil.copytree(component, target, dirs_exist_ok=True)
    # Pin the two components directly involved in the clock experiment.
    manifest_text = (
        'dependencies:\n  waveshare/esp32_s3_touch_amoled_2_16:\n    version: "2.0.1"\n'
        '  espressif/esp_lcd_co5300:\n    version: "2.2.0"\n')
    if performance:
        # Adding a local app changes the solver manifest hash. Pin every registry
        # component to the already-tested baseline instead of allowing upgrades.
        baseline_lock = yaml.safe_load((workspace / "config/dependencies.lock").read_text(encoding="utf-8"))
        exact_dependencies = {name: {"version": str(info["version"])}
                              for name, info in baseline_lock["dependencies"].items()
                              if info["source"]["type"] == "service"}
        if args.apps:
            # Preserve the tested Apps dependencies too. The original display,
            # LVGL and BSP baseline above always wins over newer family locks.
            apps_lock = workspace / "config/dependencies-apps.lock"
            if apps_lock.exists():
                for name, info in yaml.safe_load(apps_lock.read_text(encoding="utf-8"))["dependencies"].items():
                    if info["source"]["type"] == "service":
                        exact_dependencies.setdefault(name, {"version": str(info["version"])})
        manifest_text = yaml.safe_dump({"dependencies": exact_dependencies}, sort_keys=True)
    write_if_changed(project / "main/idf_component.yml", manifest_text.encode("utf-8"))
    saved_lock = workspace / ("config/dependencies-apps.lock" if args.apps
                              else "config/dependencies-localized.lock" if args.localized
                              else "config/dependencies-corners.lock" if args.corners
                              else "config/dependencies-buffered.lock" if args.buffered
                              else "config/dependencies-perf.lock" if performance
                              else "config/dependencies.lock")
    if args.buffered and not saved_lock.exists():
        saved_lock = workspace / ("config/dependencies-buffered.lock" if args.corners or args.localized
                                  else "config/dependencies-perf.lock")
    if not saved_lock.exists():
        saved_lock = workspace / "config/dependencies.lock"
    if saved_lock.exists():
        lock_text = saved_lock.read_text(encoding="utf-8")
        dependency_lock = yaml.safe_load(lock_text)
        local_components = {
            "brookesia_core": "components/brookesia_core",
            "chmorgan/esp-audio-player": "components/esp_audio_player_safe/esp-audio-player",
            "espressif/avi_player": "components/avi_player_safe/avi_player",
            "espressif/gmf_io": "components/gmf_io_compat/gmf_io",
            "espressif/esp_player": "components/esp_player_compat/esp_player",
        }
        for name, relative in local_components.items():
            component = dependency_lock["dependencies"].get(name)
            if component and component["source"]["type"] == "local":
                component["source"]["path"] = str(project / relative)
        lock_text = yaml.safe_dump(dependency_lock, sort_keys=False)
        write_if_changed(project / "dependencies.lock", lock_text.encode("utf-8"))
    common = (project / "sdkconfig.defaults").read_text(encoding="utf-8")
    if args.apps:
        common += "\n" + (workspace / "overlay-apps/sdkconfig.apps.defaults").read_text(encoding="utf-8")
        # Existing sdkconfig values otherwise override newly added defaults.
        # Apply only the Apps overlay's explicit options to this family's files.
        requested = {}
        for line in (workspace / "overlay-apps/sdkconfig.apps.defaults").read_text(encoding="utf-8").splitlines():
            match = re.match(r"(?:# )?(CONFIG_\w+)(?:=| is not set)", line)
            if match:
                requested[match.group(1)] = line
        for clock in (40, 80):
            existing = root / f"sdkconfig.apps.{clock}"
            if not existing.exists():
                continue
            existing_text = existing.read_text(encoding="utf-8")
            lines = []
            seen = set()
            for line in existing_text.splitlines():
                match = re.match(r"(?:# )?(CONFIG_\w+)(?:=| is not set)", line)
                if match and match.group(1) in requested:
                    key = match.group(1)
                    if key not in seen:
                        lines.append(requested[key])
                        seen.add(key)
                    continue
                lines.append(line)
            # Keep Kconfig's ordering. Moving every override to EOF makes an
            # unchanged configuration look modified after each Kconfig run,
            # unnecessarily regenerating the entire Windows build graph.
            lines.extend(value for key, value in requested.items() if key not in seen)
            updated_text = "\n".join(lines) + "\n"
            # Kconfig writes CRLF on Windows. A newline-only difference must
            # not touch sdkconfig and trigger another full configure pass.
            if updated_text != existing_text:
                write_if_changed(existing, updated_text.encode("utf-8"))
    for mhz in (40, 80):
        other = 80 if mhz == 40 else 40
        defaults_text = (
            common + f"\nCONFIG_APP_LCD_QSPI_{mhz}MHZ=y\n"
            + f"# CONFIG_APP_LCD_QSPI_{other}MHZ is not set\n"
            + ("CONFIG_HOSINO_PERF_DEMO=y\n# CONFIG_LV_USE_PERF_MONITOR is not set\n"
               if performance else "# CONFIG_HOSINO_PERF_DEMO is not set\n")
            + ("CONFIG_HOSINO_DISPLAY_BUFFERED=y\n" if args.buffered
               else "# CONFIG_HOSINO_DISPLAY_BUFFERED is not set\n")
            + ("CONFIG_HOSINO_DISPLAY_CORNERS=y\n" if args.corners
               else "# CONFIG_HOSINO_DISPLAY_CORNERS is not set\n")
            + ("CONFIG_HOSINO_UI_ZH=y\n" if args.localized
               else "# CONFIG_HOSINO_UI_ZH is not set\n"))
        write_if_changed(project / f"sdkconfig.{mhz}.defaults", defaults_text.encode("utf-8"))
    print(project)


if __name__ == "__main__":
    main()
