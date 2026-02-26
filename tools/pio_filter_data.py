from pathlib import Path
import shutil

Import("env")
from SCons.Script import COMMAND_LINE_TARGETS


def _prepare_filtered_data_dir(source, target, env):
    project_dir = Path(env.subst("$PROJECT_DIR"))
    data_dir = Path(env.subst("$PROJECT_DATA_DIR"))
    filtered_dir = project_dir / ".pio" / "data_filtered"

    if filtered_dir.exists():
        shutil.rmtree(filtered_dir)

    # Keep FS payload identical to data/ so required boot/runtime assets are present.
    shutil.copytree(data_dir, filtered_dir)
    env.Replace(PROJECT_DATA_DIR=str(filtered_dir))


if "buildfs" in COMMAND_LINE_TARGETS or "uploadfs" in COMMAND_LINE_TARGETS:
    _prepare_filtered_data_dir(None, None, env)
